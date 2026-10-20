// segments_test.cpp — a Segments table reads the same buffer as a flat copy.
//
// The weights challenge defines its buffer as separate pieces laid end to end
// and reads them through SegLoad. That is sound only if every index lands on
// the word the flat concatenation holds there. This checks it twice: index by
// index, exhaustively, over tables of awkward shapes, and then as whole
// challenges — both chain modes, both folds — against the flat answer the
// known-answer test already pins.
#include "segments.hpp"
#include "weights.hpp"
#include <cstdio>
#include <vector>

using namespace mc;

static int g_fail = 0;
static void check(bool ok, const char* what, size_t shape) {
    if (!ok) { ++g_fail; fprintf(stderr, "FAIL: %s (shape %zu)\n", what, shape); }
}

static void run_shape(size_t id, const std::vector<uint64_t>& lens, uint64_t max_dir) {
    uint64_t total = 0;
    for (uint64_t l : lens) total += l;
    std::vector<uint64_t> flat(total);
    for (uint64_t i = 0; i < total; ++i) flat[i] = splitmix64_word(0x5EED + id, i);

    // Copy each piece into its own allocation, so nothing about the answer can
    // lean on the pieces sitting next to each other.
    std::vector<std::vector<uint64_t>> pieces;
    std::vector<const uint64_t*> bases;
    uint64_t at = 0;
    for (uint64_t l : lens) {
        pieces.emplace_back(flat.begin() + (long)at, flat.begin() + (long)(at + l));
        at += l;
    }
    for (auto& p : pieces) bases.push_back(p.empty() ? nullptr : p.data());

    SegmentTable t;
    check(t.build(bases, lens, max_dir), "build", id);
    check(t.words() == total, "length", id);
    check(t.dir.size() <= max_dir, "directory within its cap", id);
    SegLoad load{t.view()};

    for (uint64_t i = 0; i < total; ++i)
        if (load(i) != flat[i]) { check(false, "every index reads the flat word", id); break; }

    const uint64_t nonces[] = { 0, 1, 0xDEADBEEFCAFEF00DULL };
    const uint32_t ks[] = { 64, 1000 };
    const uint32_t lane_counts[] = { 1, 4, 16 };
    std::vector<uint64_t> s1(16), s2(16);
    for (uint64_t nonce : nonces)
        for (uint32_t k : ks)
            for (uint32_t lanes : lane_counts)
                for (int chained = 0; chained < 2; ++chained) {
                    check(answer_challenge_t(load, total, nonce, k, lanes, chained) ==
                              answer_challenge(flat.data(), total, nonce, k, lanes, chained),
                          "sequential-fold answer", id);
                    answer_lanes_t(load, total, nonce, k, lanes, chained, s1.data());
                    check(tree_fold(s1.data(), lanes, nonce, k) ==
                              answer_challenge_tree(flat.data(), total, nonce, k, lanes, chained, s2.data()),
                          "tree-fold answer", id);
                }
}

int main() {
    const uint64_t roomy = 1u << 20;
    std::vector<std::pair<std::vector<uint64_t>, uint64_t>> shapes = {
        { {1}, roomy },                               // one word
        { {1, 1, 1}, roomy },                         // all minimal
        { {5, 1, 7}, roomy },                         // a one-word piece mid-buffer
        { {64, 64}, roomy },                          // power-of-two pieces
        { {100, 3, 1000, 1, 1, 50}, roomy },          // mixed, two adjacent tiny ones
        { {100, 0, 3, 0, 1000}, roomy },              // empty pieces are skipped
        { {4096, 4096, 4096, 1, 4096, 4096}, roomy }, // weights-like, plus a stray word
        { {100, 3, 1000, 1, 1, 50}, 4 },              // directory capped: lookups walk
        { {4096, 4096, 4096, 4096}, 2 },              // capped below the piece count
    };
    uint64_t r = 0x1234;
    std::vector<uint64_t> random_lens;
    for (int i = 0; i < 40; ++i) random_lens.push_back(1 + splitmix64(r) % 300);
    shapes.push_back({ random_lens, roomy });
    shapes.push_back({ random_lens, 8 });

    for (size_t i = 0; i < shapes.size(); ++i) run_shape(i, shapes[i].first, shapes[i].second);
    if (g_fail) { fprintf(stderr, "segments_test: %d failures\n", g_fail); return 1; }
    printf("segments_test: %zu shapes, every index and every answer matches the flat buffer\n",
           shapes.size());
    return 0;
}
