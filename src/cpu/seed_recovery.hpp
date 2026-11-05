#pragma once
#include "shared/challenge.hpp"

namespace mc {

// The inverse of an odd number modulo 2^64.
static inline uint64_t inverse_mod64(uint64_t a) {
    uint64_t inv = a;
    for (int i = 0; i < 5; ++i) inv *= 2 - a * inv;
    return inv;
}

// Recovers x from y = x ^ (x >> shift).
static inline uint64_t undo_xorshift_right(uint64_t y, int shift) {
    uint64_t x = y;
    for (int i = 0; i < (63 + shift) / shift; ++i) x = y ^ (x >> shift);
    return x;
}

static inline uint64_t splitmix64_unfinalize(uint64_t w) {
    uint64_t z = undo_xorshift_right(w, 31);
    z = undo_xorshift_right(z * inverse_mod64(kSplitmixMul2), 27);
    return undo_xorshift_right(z * inverse_mod64(kSplitmixMul1), 30);
}

// The seed of a fill_splitmix64() buffer, from its first word.
static inline uint64_t splitmix64_recover_seed(uint64_t word0) {
    return splitmix64_unfinalize(word0) - kGoldenGamma;
}

} // namespace mc
