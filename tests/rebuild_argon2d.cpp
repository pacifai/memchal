// tests/rebuild_argon2d.cpp — the partial-recompute engine rebuilds the same
// bytes build_buffer() produces.
//
// For each recipe shape, build the full matrix, keep a random fraction of its
// blocks, rebuild every discarded block from the kept ones, and compare. Shapes
// cover one and several passes (t >= 2 is where the rebuild has to reach back
// into earlier passes) and one and several build lanes (where references cross
// lanes). A last case checks that a tight budget gives up rather than returning
// a wrong block as if it were right.
//
//   Build: make tests/rebuild_argon2d
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "argon2d_rebuild.hpp"

using namespace mc;

static int g_failed = 0;

static void check(const char* name, bool ok) {
    printf("%-44s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failed;
}

static Recipe recipe(uint32_t m_kib, uint32_t t, uint32_t p, uint32_t seed) {
    Recipe r;
    std::mt19937 rng(seed);
    for (auto& b : r.key)  b = (uint8_t)rng();
    for (auto& b : r.salt) b = (uint8_t)rng();
    r.m_kib = m_kib; r.t = t; r.p = p;
    return r;
}

// Keeps fraction `keep` of the blocks, the way the cheat does, and returns the
// slot table plus the compacted kept words.
static void keep_fraction(const std::vector<uint64_t>& full, uint32_t blocks, double keep,
                          uint32_t seed, std::vector<int32_t>& slot,
                          std::vector<uint64_t>& kept) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    slot.assign(blocks, -1);
    kept.clear();
    for (uint32_t b = 0; b < blocks; ++b) {
        if (u(rng) >= keep) continue;
        slot[b] = (int32_t)(kept.size() / a2::kBlockWords);
        kept.insert(kept.end(), full.begin() + (size_t)b * a2::kBlockWords,
                    full.begin() + (size_t)(b + 1) * a2::kBlockWords);
    }
}

static void case_rebuild(uint32_t m_kib, uint32_t t, uint32_t p, double keep) {
    const Recipe r = recipe(m_kib, t, p, m_kib * 131 + t * 7 + p);
    std::vector<uint64_t> full(build_buffer_words(r));
    if (!build_buffer(full.data(), full.size(), r)) { check("build_buffer", false); return; }

    MatrixShape s;
    if (!matrix_shape(r, s)) { check("matrix_shape", false); return; }

    std::vector<int32_t> slot;
    std::vector<uint64_t> kept;
    keep_fraction(full, s.blocks, keep, 99, slot, kept);

    // The budget is large enough that no block in these shapes gives up.
    Rebuilder rb(s, KeptBlocks{slot.data(), kept.data()}, 1ull << 40);
    uint64_t out[a2::kBlockWords];
    uint32_t missing = 0, wrong = 0;
    for (uint32_t b = 0; b < s.blocks; ++b) {
        if (slot[b] >= 0) continue;
        ++missing;
        // A fresh memo per block, so every rebuild walks its whole ancestry.
        rb.begin_challenge();
        if (!rb.rebuild(b, out) ||
            memcmp(out, full.data() + (size_t)b * a2::kBlockWords, a2::kBlockBytes) != 0)
            ++wrong;
    }
    char name[96];
    snprintf(name, sizeof name, "m=%u t=%u p=%u keep=%.2f (%u rebuilt)",
             m_kib, t, p, keep, missing);
    check(name, missing > 0 && wrong == 0);
}

// One challenge's worth of budget runs out on a t=2 rebuild, which needs most
// of pass 0. The engine must report that, not hand back a block.
static void case_budget() {
    const Recipe r = recipe(1024, 2, 1, 5);
    std::vector<uint64_t> full(build_buffer_words(r));
    build_buffer(full.data(), full.size(), r);
    MatrixShape s;
    matrix_shape(r, s);
    std::vector<int32_t> slot;
    std::vector<uint64_t> kept;
    keep_fraction(full, s.blocks, 0.9, 7, slot, kept);

    uint32_t b = s.blocks - 1;
    while (slot[b] >= 0) --b;   // the last missing block has the deepest ancestry
    Rebuilder rb(s, KeptBlocks{slot.data(), kept.data()}, 16);
    rb.begin_challenge();
    uint64_t out[a2::kBlockWords];
    check("tight budget gives up", !rb.rebuild(b, out) && rb.stats().gave_up == 1);
}

int main() {
    for (uint32_t t : {1u, 2u, 3u})
        for (uint32_t p : {1u, 4u})
            for (double keep : {0.0, 0.5, 0.9})
                case_rebuild(256, t, p, keep);
    case_rebuild(1024, 2, 2, 0.99);
    case_budget();
    if (g_failed) { printf("%d case(s) FAILED\n", g_failed); return 1; }
    printf("all rebuild cases passed\n");
    return 0;
}
