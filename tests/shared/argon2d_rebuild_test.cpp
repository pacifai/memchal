#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "shared/argon2d_rebuild.hpp"

using namespace mc;

static int g_failed = 0;

static void check(const char* name, bool ok) {
    printf("%-44s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failed;
}

static Recipe recipe(uint32_t memory_kib, uint32_t passes, uint32_t parallelism, uint32_t seed) {
    Recipe r;
    std::mt19937 rng(seed);
    for (auto& b : r.key)  b = (uint8_t)rng();
    for (auto& b : r.salt) b = (uint8_t)rng();
    r.memory_kib = memory_kib; r.passes = passes; r.parallelism = parallelism;
    return r;
}

// Keeps a random `keep` fraction of the blocks.
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

// Checks that every discarded block rebuilds to its built value.
static void case_rebuild(uint32_t memory_kib, uint32_t passes, uint32_t parallelism, double keep) {
    const Recipe r = recipe(memory_kib, passes, parallelism, memory_kib * 131 + passes * 7 + parallelism);
    std::vector<uint64_t> full(build_buffer_words(r));
    if (!build_buffer(full.data(), full.size(), r)) { check("build_buffer", false); return; }

    MatrixShape s;
    if (!matrix_shape(r, s)) { check("matrix_shape", false); return; }

    std::vector<int32_t> slot;
    std::vector<uint64_t> kept;
    keep_fraction(full, s.blocks, keep, 99, slot, kept);

    Rebuilder rb(s, KeptBlocks{slot.data(), kept.data()}, 1ull << 40);
    uint64_t out[a2::kBlockWords];
    uint32_t missing = 0, wrong = 0;
    for (uint32_t b = 0; b < s.blocks; ++b) {
        if (slot[b] >= 0) continue;
        ++missing;
        rb.begin_challenge();
        if (!rb.rebuild(b, out) ||
            memcmp(out, full.data() + (size_t)b * a2::kBlockWords, a2::kBlockBytes) != 0)
            ++wrong;
    }
    char name[96];
    snprintf(name, sizeof name, "%u KiB passes=%u par=%u keep=%.2f (%u rebuilt)",
             memory_kib, passes, parallelism, keep, missing);
    check(name, missing > 0 && wrong == 0);
}

// Checks that a rebuild reports running out of budget.
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
    while (slot[b] >= 0) --b;
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
