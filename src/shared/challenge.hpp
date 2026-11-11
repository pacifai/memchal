#pragma once
#include <cstddef>
#include <cstdint>

#if defined(__CUDACC__)
  #define MC_HOST_DEVICE __host__ __device__
#else
  #define MC_HOST_DEVICE
#endif

namespace mc {

// SplitMix64: Steele, Lea, and Flood, "Fast Splittable Pseudorandom Number
// Generators" (OOPSLA 2014); reference code at https://prng.di.unimi.it/splitmix64.c.
// The finalizer's shifts and multipliers are Stafford's Mix13:
// https://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html
static constexpr uint64_t kSplitmixMul1 = 0xBF58476D1CE4E5B9ULL;
static constexpr uint64_t kSplitmixMul2 = 0x94D049BB133111EBULL;
static constexpr uint64_t kGoldenGamma  = 0x9E3779B97F4A7C15ULL;

// MurmurHash3's fmix64:
// https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
static constexpr uint64_t kMurmurMul1 = 0xFF51AFD7ED558CCDULL;
static constexpr uint64_t kMurmurMul2 = 0xC4CEB9FE1A85EC53ULL;

// The 64-bit FNV prime: http://www.isthe.com/chongo/tech/comp/fnv/
static constexpr uint64_t kFnvPrime = 0x100000001B3ULL;

// xxHash's PRIME64_2: https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md
static constexpr uint64_t kXxhPrime2 = 0xC2B2AE3D27D4EB4FULL;

static constexpr uint64_t kTreePadTag = 0xA5A5A5A5A5A5A5A5ULL;

MC_HOST_DEVICE static inline uint64_t splitmix64_finalize(uint64_t z) {
    z = (z ^ (z >> 30)) * kSplitmixMul1;
    z = (z ^ (z >> 27)) * kSplitmixMul2;
    return z ^ (z >> 31);
}

MC_HOST_DEVICE static inline uint64_t splitmix64(uint64_t& state) {
    state += kGoldenGamma;
    return splitmix64_finalize(state);
}

// The word fill_splitmix64() writes at buf[idx].
MC_HOST_DEVICE static inline uint64_t splitmix64_word(uint64_t seed, uint64_t idx) {
    return splitmix64_finalize(seed + (idx + 1) * kGoldenGamma);
}

MC_HOST_DEVICE static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= kMurmurMul1;
    x ^= x >> 33; x *= kMurmurMul2;
    x ^= x >> 33;
    return x;
}

// High 64 bits of the 128-bit product a * b.
MC_HOST_DEVICE static inline uint64_t mulhi64(uint64_t a, uint64_t b) {
#if defined(__CUDA_ARCH__)
    return __umul64hi(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
#endif
}

// Maps a 64-bit random value to an index in [0, num_words).
MC_HOST_DEVICE static inline uint64_t map_index(uint64_t r, uint64_t num_words) {
    return mulhi64(r, num_words);
}

// Hashes `count` probes of the buffer for one lane. `load(idx)` returns the
// word at idx.
//   chained: each probe's index depends on the word the previous probe read.
template <typename Load>
MC_HOST_DEVICE static inline uint64_t answer_lane(Load&& load, uint64_t num_words, uint64_t nonce,
                                                  uint32_t lane, uint32_t count, bool chained) {
    if (!chained) {
        uint64_t state = mix64(nonce ^ (kGoldenGamma * (uint64_t)(lane + 1)));
        uint64_t acc   = mix64(nonce + kFnvPrime * (uint64_t)(lane + 1));
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t idx = map_index(splitmix64(state), num_words);
            uint64_t w   = load(idx);
            acc = mix64(acc ^ (w + kGoldenGamma * (uint64_t)i) ^ idx);
        }
        return acc;
    }
    uint64_t acc = mix64(nonce ^ (kGoldenGamma * (uint64_t)(lane + 1)));
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t idx = map_index(acc, num_words);
        uint64_t w   = load(idx);
        acc = mix64(acc ^ w ^ (kGoldenGamma * (uint64_t)i) ^ idx);
    }
    return acc;
}

// How many of `samples` probes lane `lane` draws.
MC_HOST_DEVICE static inline uint32_t samples_in_lane(uint32_t samples, uint32_t lanes, uint32_t lane) {
    return samples / lanes + (lane < (samples % lanes) ? 1u : 0u);
}

MC_HOST_DEVICE static inline uint64_t lane_fold_term(uint64_t lane_hash, uint32_t lane) {
    return mix64(lane_hash + kGoldenGamma * (uint64_t)(lane + 1));
}

MC_HOST_DEVICE static inline uint64_t fold_seed(uint64_t nonce, uint32_t samples, uint32_t lanes) {
    return mix64(nonce ^ ((uint64_t)samples << 20) ^ (uint64_t)lanes);
}

// Folds the lane hashes in `v` in lane order.
MC_HOST_DEVICE static inline uint64_t sequential_fold(const uint64_t* v, uint32_t lanes,
                                                      uint64_t nonce, uint32_t samples) {
    uint64_t combined = fold_seed(nonce, samples, lanes);
    for (uint32_t l = 0; l < lanes; ++l) combined = mix64(combined ^ lane_fold_term(v[l], l));
    return combined;
}

// The answer with the sequential fold: lane hashes chained in lane order.
template <typename Load>
MC_HOST_DEVICE static inline uint64_t answer_sequential(Load&& load, uint64_t num_words,
                                                        uint64_t nonce, uint32_t samples,
                                                        uint32_t lanes, bool chained) {
    uint64_t combined = fold_seed(nonce, samples, lanes);
    for (uint32_t l = 0; l < lanes; ++l) {
        uint64_t h = answer_lane(load, num_words, nonce, l, samples_in_lane(samples, lanes, l),
                                 chained);
        combined = mix64(combined ^ lane_fold_term(h, l));
    }
    return combined;
}

// Combines two sibling nodes of the tree fold. Swapping the siblings changes
// the result.
MC_HOST_DEVICE static inline uint64_t tree_combine(uint64_t left, uint64_t right) {
    return mix64(mix64(left + kGoldenGamma) ^ (right * kXxhPrime2));
}

// The right-hand sibling of the last node on a level with an odd count.
MC_HOST_DEVICE static inline uint64_t tree_pad(uint32_t level, uint32_t index) {
    return mix64(kTreePadTag ^ ((uint64_t)level * kGoldenGamma) ^ (uint64_t)index);
}

// Folds the lane hashes in `v` into a binary tree's root. Overwrites `v`.
MC_HOST_DEVICE static inline uint64_t tree_fold(uint64_t* v, uint32_t lanes, uint64_t nonce,
                                                uint32_t samples) {
    for (uint32_t l = 0; l < lanes; ++l) v[l] = lane_fold_term(v[l], l);
    for (uint32_t n = lanes, level = 0; n > 1; n = (n + 1) / 2, ++level) {
        for (uint32_t i = 0; i < (n + 1) / 2; ++i) {
            uint64_t a = v[2 * i];
            uint64_t b = (2 * i + 1 < n) ? v[2 * i + 1] : tree_pad(level, 2 * i + 1);
            v[i] = tree_combine(a, b);
        }
    }
    return mix64(v[0] ^ fold_seed(nonce, samples, lanes));
}

// Writes each lane's hash to out[0, lanes).
template <typename Load>
static inline void answer_lanes(Load&& load, uint64_t num_words, uint64_t nonce, uint32_t samples,
                                uint32_t lanes, bool chained, uint64_t* out) {
    for (uint32_t l = 0; l < lanes; ++l)
        out[l] = answer_lane(load, num_words, nonce, l, samples_in_lane(samples, lanes, l), chained);
}

// The answer with the tree fold.
//   scratch: space for `lanes` values, supplied by the caller; overwritten.
template <typename Load>
static inline uint64_t answer_tree(Load&& load, uint64_t num_words, uint64_t nonce,
                                   uint32_t samples, uint32_t lanes, bool chained,
                                   uint64_t* scratch) {
    answer_lanes(load, num_words, nonce, samples, lanes, chained, scratch);
    return tree_fold(scratch, lanes, nonce, samples);
}

struct BufferLoad {
    const uint64_t* buf;
    MC_HOST_DEVICE uint64_t operator()(uint64_t idx) const { return buf[idx]; }
};

} // namespace mc
