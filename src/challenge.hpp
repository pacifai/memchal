// challenge.hpp — the challenge math, and nothing else.
//
// This header is the single source of truth for what a challenge *is*. It is
// split out of common.hpp so it can be included from a CUDA translation unit:
// it pulls in no POSIX headers, no sockets, and no /proc readers. Everything
// here is plain C over fixed-width integers — no STL, no exceptions, no
// allocation — so the same code compiles for host and device.
//
// FROZEN. The math below is pinned by tests/known_answer.cpp (ADR 0001). A
// change that moves any vector invalidates every published number. The split
// that created this file deliberately changed no operation: mulhi64() below is
// the one addition, and on the host it expands to exactly the 128-bit multiply
// that map_index() used before.
//
// Sockets, the wire structs, and the /proc readers live in net.hpp. common.hpp
// includes both, so existing callers are unaffected.
#pragma once
#include <cstdint>
#include <cstddef>
#include "chacha20.hpp"

// Execution-space marker. Under nvcc a function tagged with this compiles for
// both host and device; under a host-only compiler it vanishes. Tag only the
// pure-integer math — anything that allocates, reads a file, or touches a
// socket stays host-only.
#if defined(__CUDACC__)
  #define MC_HOST_DEVICE __host__ __device__
#else
  #define MC_HOST_DEVICE
#endif

namespace mc {

// ---------------------------------------------------------------------------
// Hashing / sampling core
// ---------------------------------------------------------------------------

// splitmix64 finalizer constants.
static constexpr uint64_t kSM64_C1  = 0xBF58476D1CE4E5B9ULL;
static constexpr uint64_t kSM64_C2  = 0x94D049BB133111EBULL;
static constexpr uint64_t kSM64_GOLDEN = 0x9E3779B97F4A7C15ULL;

// The 3-step mixing half of splitmix64 (no increment).  Called by splitmix64
// and also by the recompute-adversary helpers below.
MC_HOST_DEVICE static inline uint64_t splitmix64_finalize(uint64_t z) {
    z = (z ^ (z >> 30)) * kSM64_C1;
    z = (z ^ (z >> 27)) * kSM64_C2;
    return z ^ (z >> 31);
}

// splitmix64: fast, well-distributed PRNG step. GPU PORT: identical.
// Behavior is bit-identical to the original one-liner.
MC_HOST_DEVICE static inline uint64_t splitmix64(uint64_t& s) {
    s += kSM64_GOLDEN;
    return splitmix64_finalize(s);
}

// O(1) random-access: returns the word that fill_random would place at buf[idx]
// when started with the given seed.  Equivalent to advancing the state by
// (idx+1) increments from seed, then finalizing.
MC_HOST_DEVICE static inline uint64_t splitmix64_word(uint64_t seed, uint64_t idx) {
    return splitmix64_finalize(seed + (idx + 1) * kSM64_GOLDEN);
}

// ---------------------------------------------------------------------------
// splitmix64 seed recovery from a single output word.
//
// The finalizer f(z) is a bijection on uint64_t; its inverse lets us recover
// z (= seed + GOLDEN) from any output word w = f(z), then seed = z - GOLDEN.
// Used by the recompute adversary to reconstruct the seed from buf[0].
// ---------------------------------------------------------------------------

// Newton-inversion: given a uint64_t a (odd), find inv such that a*inv == 1
// (mod 2^64).  Five Hensel-lifting steps suffice for 64 bits.
MC_HOST_DEVICE static inline uint64_t inv_mul(uint64_t a) {
    uint64_t inv = a; // initial approx: a*a == 1 mod 8 when a is odd
    for (int i = 0; i < 5; ++i) inv *= 2 - a * inv;
    return inv;
}

// Recover x from y = x ^ (x >> s) for right-shifts s in [1,63].
// Iterative fix-point: applying x = y ^ (x >> s) ceil(64/s) times
// converges to the unique solution because each pass propagates the
// correct bits one window of s further down.
MC_HOST_DEVICE static inline uint64_t unxorshift_r(uint64_t y, int s) {
    uint64_t x = y;
    int iters = (63 + s) / s;   // ceil(64/s)
    for (int i = 0; i < iters; ++i)
        x = y ^ (x >> s);
    return x;
}

// Invert the splitmix64 finalizer.
MC_HOST_DEVICE static inline uint64_t splitmix64_unfinalize(uint64_t w) {
    // Reverse step 3: w = z ^ (z >> 31)
    uint64_t z = unxorshift_r(w, 31);
    // Reverse step 2: z = (prev ^ (prev >> 27)) * C2  =>  prev = z * inv(C2)
    z = z * inv_mul(kSM64_C2);
    // Reverse step 1 unmixing: z = prev ^ (prev >> 27)
    z = unxorshift_r(z, 27);
    // Reverse step 1 multiply
    z = z * inv_mul(kSM64_C1);
    // Reverse step 0: z = prev ^ (prev >> 30)
    z = unxorshift_r(z, 30);
    return z;
}

// Recover the seed from buf[0] = splitmix64_finalize(seed + GOLDEN).
// After recovery: splitmix64_word(seed, idx) == buf[idx] for all idx.
MC_HOST_DEVICE static inline uint64_t splitmix64_recover_seed(uint64_t word0) {
    return splitmix64_unfinalize(word0) - kSM64_GOLDEN;
}

// ---------------------------------------------------------------------------
// Generator selection
//
// HOST ONLY: the generators allocate nothing but do run ChaCha20 over a whole
// buffer, and only the verifier and the cheating prover call them. Keeping them
// off the device keeps chacha20.hpp out of device code.
// ---------------------------------------------------------------------------
enum class Gen { SPLITMIX64, CHACHA20 };

// Fill a uint64_t buffer using the selected generator.
//
// SPLITMIX64 (default): produces exactly what fill_random() produced —
//   buf[i] = splitmix64_finalize(s + (i+1)*GOLDEN), where s = (seed ? seed : DEFAULT).
//
// CHACHA20: derive a 32-byte key from the 64-bit seed by running splitmix64
//   four times (words 0..3 of the sequence, little-endian bytes), use a
//   zero nonce and counter0=0.  The resulting keystream bytes are assembled
//   into uint64_t words little-endian (buf[i] = LE64 of bytes [8i..8i+7]).
//   The seed cannot be recovered from the output, so the recompute adversary
//   produces wrong words against a ChaCha20-generated buffer.
static inline void fill_buffer(uint64_t* buf, uint64_t num_words,
                                uint64_t seed, Gen gen) {
    if (gen == Gen::SPLITMIX64) {
        uint64_t s = seed ? seed : 0xDEADBEEFCAFEF00DULL;
        for (uint64_t i = 0; i < num_words; ++i) buf[i] = splitmix64(s);
        return;
    }
    // CHACHA20: seed -> key via 4 splitmix64 words
    uint64_t ks = seed ? seed : 0xDEADBEEFCAFEF00DULL;
    uint8_t key[32];
    for (int i = 0; i < 4; ++i) {
        uint64_t w = splitmix64(ks);
        // Little-endian store
        for (int b = 0; b < 8; ++b)
            key[i * 8 + b] = (uint8_t)(w >> (b * 8));
    }
    static const uint8_t zero_nonce[12] = {};
    // Fill raw bytes, then the uint64_t words are already in the right order
    // (host is little-endian for the purposes of this PoC; see the comment near
    // kMagic in net.hpp: "same-endian machines").
    chacha20_fill((uint8_t*)buf, num_words * 8, key, zero_nonce, 0);
}

// Fill a uint64_t buffer using a caller-supplied full 32-byte ChaCha20 key.
//
// This is the hardened entry point used by the verifier's --gen chacha path.
// The key must be 32 bytes of entropy from std::random_device (not derived
// from the 64-bit seed), so the buffer capacity is unconditionally 256 bits.
//
// A zero 12-byte nonce and block index 0 are used; the buffer is treated as
// a stream of little-endian uint64_t words (consistent with the host-LE
// assumption documented near kMagic).
static inline void fill_buffer_chacha(uint64_t* buf, uint64_t num_words,
                                      const uint8_t key[32]) {
    static const uint8_t zero_nonce[12] = {};
    chacha20_fill((uint8_t*)buf, num_words * 8, key, zero_nonce, 0);
}

// Deterministic buffer fill (verifier generates; also handy for tests).
// Delegates to fill_buffer(SPLITMIX64) — behavior is bit-identical to the
// original implementation.
static inline void fill_random(uint64_t* buf, uint64_t num_words, uint64_t seed) {
    fill_buffer(buf, num_words, seed, Gen::SPLITMIX64);
}

// mix64: MurmurHash3 finalizer, a strong 64-bit avalanche. GPU PORT: identical.
MC_HOST_DEVICE static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

// High 64 bits of the 128-bit product a*b.
//
// This is the ONE operation in the challenge math with no portable spelling.
// On the host it is the `unsigned __int128` expression map_index() has always
// used; on the device it is the __umul64hi intrinsic. The two were checked
// bit-identical across 2x10^7 random pairs plus all 64 boundary combinations of
// {0, 1, 2^32-1, 2^32, 2^63, 2^64-1}, so this is a spelling change, not a math
// change — and the known-answer vectors pin that.
//
// The guard is __CUDA_ARCH__, not __CUDACC__: inside a .cu file nvcc also runs
// a host pass, where __umul64hi does not exist.
MC_HOST_DEVICE static inline uint64_t mulhi64(uint64_t a, uint64_t b) {
#if defined(__CUDA_ARCH__)
    return __umul64hi(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
#endif
}

// Uniform index in [0, n) from a 64-bit random (Lemire multiply-shift, no modulo
// bias).
MC_HOST_DEVICE static inline uint64_t map_index(uint64_t r, uint64_t n) {
    return mulhi64(r, n);
}

// One "lane" = exactly the work a single GPU thread performs: draw `samples`
// pseudo-random offsets across the whole buffer and fold the probed words into a
// chained accumulator. Chaining means every probed word (and its order) affects
// the result, so you cannot answer without actually reading the data.
//
// The word source is abstracted as `load(idx) -> uint64_t`. The honest prover
// and the verifier bind it to a raw RAM buffer (see wrappers below); a dishonest
// prover can bind it to disk, a partial cache, etc. — the challenge math is
// byte-identical either way, so a *correct* answer verifies regardless of where
// the bytes came from. That's the point: only the latency (or, for fabricated
// misses, the correctness) betrays the cheat.
//
// GPU PORT: this is the per-thread body of the CUDA kernel; the loader is a
// __device__ functor reading a cudaMalloc'd buffer. Each thread owns one lane.
// Pass a functor rather than a lambda so the header needs no --extended-lambda.
//
// When chained==false (default): the EXACT current independent code path — do
// not change a single operation, so existing known-answer vectors remain
// bit-identical.
// When chained==true: pointer-chasing loop where each index depends on the
// previously-read word, preventing any pre-enumeration of offsets.
template <typename Load>
MC_HOST_DEVICE static inline uint64_t answer_lane_t(Load&& load, uint64_t num_words,
                                     uint64_t nonce, uint32_t lane, uint32_t samples,
                                     bool chained = false) {
    if (!chained) {
        // INDEPENDENT path — bit-identical to original; do not edit these lines.
        uint64_t s   = mix64(nonce ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(lane + 1)));
        uint64_t acc = mix64(nonce + 0x100000001B3ULL * (uint64_t)(lane + 1));
        for (uint32_t i = 0; i < samples; ++i) {
            uint64_t r   = splitmix64(s);
            uint64_t idx = map_index(r, num_words);
            uint64_t w   = load(idx);                // <-- the probe (RAM/disk/…)
            acc = mix64(acc ^ (w + 0x9E3779B97F4A7C15ULL * (uint64_t)i) ^ idx);
        }
        return acc;
    } else {
        // DEPENDENT (chained) path — pointer-chasing: idx_{i+1} depends on
        // w_i = load(idx_i), so offsets cannot be enumerated without reading.
        uint64_t acc = mix64(nonce ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(lane + 1)));
        for (uint32_t i = 0; i < samples; ++i) {
            uint64_t idx = map_index(acc, num_words);   // index from running acc
            uint64_t w   = load(idx);                   // <-- the probe
            acc = mix64(acc ^ w ^ (0x9E3779B97F4A7C15ULL * (uint64_t)i) ^ idx);
        }
        return acc;
    }
}

// How many samples lane `l` owns, given k spread over `lanes` lanes. The GPU
// kernel needs this per thread; the sequential fold below needs the same rule.
// Extracted so the two cannot drift apart.
MC_HOST_DEVICE static inline uint32_t lane_samples(uint32_t k, uint32_t lanes, uint32_t l) {
    return k / lanes + (l < (k % lanes) ? 1u : 0u);
}

// The per-lane contribution to the final fold. The fold is a strict sequential
// chain — combined_{l+1} = mix64(combined_l ^ this) — so it is NOT associative
// and a tree reduction would produce a different answer. The GPU kernel writes
// each lane's hash to an array and folds them in lane order afterwards.
MC_HOST_DEVICE static inline uint64_t lane_fold_term(uint64_t lane_hash, uint32_t l) {
    return mix64(lane_hash + 0x9E3779B97F4A7C15ULL * (uint64_t)(l + 1));
}

// The fold's starting value, before any lane is mixed in.
MC_HOST_DEVICE static inline uint64_t fold_seed(uint64_t nonce, uint32_t k, uint32_t lanes) {
    return mix64(nonce ^ ((uint64_t)k << 20) ^ (uint64_t)lanes);
}

// Full challenge answer: partition k samples across `lanes` and fold the lane
// results together. Deterministic given (nonce, k, lanes) and the data.
//
// GPU PORT: launch `lanes` threads computing answer_lane_t() into a scratch
// array, then fold with fold_seed/lane_fold_term IN LANE ORDER. The loop below
// is the sequential CPU reference and defines that order.
//
// chained==false (default): independent path, bit-identical to original.
// chained==true: each lane uses the data-dependent (pointer-chasing) path.
template <typename Load>
MC_HOST_DEVICE static inline uint64_t answer_challenge_t(Load&& load, uint64_t num_words,
                                          uint64_t nonce, uint32_t k, uint32_t lanes,
                                          bool chained = false) {
    if (lanes == 0) lanes = 1;
    uint64_t combined = fold_seed(nonce, k, lanes);
    for (uint32_t l = 0; l < lanes; ++l) {
        uint32_t samples = lane_samples(k, lanes, l);
        uint64_t h = answer_lane_t(load, num_words, nonce, l, samples, chained);
        combined = mix64(combined ^ lane_fold_term(h, l));
    }
    return combined;
}

// ---------------------------------------------------------------------------
// Tree fold — the parallel alternative to the sequential cross-lane chain.
//
// WHY THIS EXISTS. answer_challenge_t() folds lane hashes in a strict sequence:
// combined = mix64(combined ^ term_l), lane after lane. That is not associative,
// so it cannot be parallelised, and on a GPU it has to happen after the kernel
// on one thread or on the host. Measured on an A40 at 16384 lanes, that fold
// cost ~312 us against the kernel's ~86 us — over half the response, growing
// with lanes while the kernel did not.
//
// A binary tree fixes it: every level is independent, so the whole fold is
// log2(lanes) parallel passes. The root is a Merkle root over the per-lane
// hashes, which is also the shape the wire revision needs in order to open a
// random subset of lanes and stop verification costing as much as proving.
// Opening is not implemented here — this change buys back the fold cost and
// leaves the response 64 bits wide.
//
// THIS IS A DELIBERATE BREAK OF THE FROZEN MATH. A tree gives a different answer
// than a chain, by construction. The sequential path above is untouched and its
// vectors still pass; the tree path carries its own vectors in
// tests/known_answer.cpp, and the wire signals which one is in use
// (kFlagTreeFold in net.hpp). Nothing silently changes.
// ---------------------------------------------------------------------------

// Combine two sibling nodes. Deliberately NOT commutative: if swapping siblings
// left the root alone, a prover could permute lanes after seeing the challenge
// and still answer. The two inputs go through different operations for that
// reason, not for extra mixing.
MC_HOST_DEVICE static inline uint64_t tree_combine(uint64_t left, uint64_t right) {
    return mix64(mix64(left + kSM64_GOLDEN) ^ (right * 0xC2B2AE3D27D4EB4FULL));
}

// Filler for a missing right-hand sibling when a level holds an odd count.
// Derived from its position, so it cannot collide with a real lane hash and a
// prover cannot substitute one for the other.
MC_HOST_DEVICE static inline uint64_t tree_pad(uint32_t level, uint32_t index) {
    return mix64(0xA5A5A5A5A5A5A5A5ULL ^ ((uint64_t)level * kSM64_GOLDEN)
                 ^ (uint64_t)index);
}

// Fold `lanes` raw lane hashes, in place, into one root.
//
// `v` holds the per-lane results on entry and is destroyed. The caller owns it,
// because this header does not allocate — the verifier uses a vector, the GPU
// prover uses the device array the kernel already wrote.
//
// Level 0 applies lane_fold_term(), the same per-lane contribution the
// sequential fold uses, so a lane's identity is bound in exactly once either
// way. Levels above it are pure tree_combine().
MC_HOST_DEVICE static inline uint64_t tree_fold(uint64_t* v, uint32_t lanes,
                                                uint64_t nonce, uint32_t k) {
    if (lanes == 0) return fold_seed(nonce, k, 1);
    for (uint32_t l = 0; l < lanes; ++l) v[l] = lane_fold_term(v[l], l);

    uint32_t n = lanes, level = 0;
    while (n > 1) {
        uint32_t m = (n + 1) / 2;
        for (uint32_t i = 0; i < m; ++i) {
            uint64_t a = v[2 * i];
            uint64_t b = (2 * i + 1 < n) ? v[2 * i + 1] : tree_pad(level, 2 * i + 1);
            v[i] = tree_combine(a, b);
        }
        n = m; ++level;
    }
    return mix64(v[0] ^ fold_seed(nonce, k, lanes));
}

// Compute every lane's hash into `out` (which must hold `lanes` entries). This
// is the part a GPU does in parallel; splitting it out means the CPU reference
// and the CUDA kernel share one definition of a lane rather than two.
template <typename Load>
static inline void answer_lanes_t(Load&& load, uint64_t num_words, uint64_t nonce,
                                  uint32_t k, uint32_t lanes, bool chained,
                                  uint64_t* out) {
    if (lanes == 0) return;
    for (uint32_t l = 0; l < lanes; ++l)
        out[l] = answer_lane_t(load, num_words, nonce, l,
                               lane_samples(k, lanes, l), chained);
}

// A loader over a plain word buffer.
//
// This is a struct rather than a lambda on purpose. A C++17 lambda's operator()
// is implicitly constexpr and host-only, so every time nvcc instantiated
// answer_lane_t() for the device it warned that a __host__ __device__ function
// was calling a constexpr __host__ one — several screens of it per build. An
// explicit MC_HOST_DEVICE operator() says what was true all along: reading a
// word out of a flat buffer is valid on either side, and which side a given
// pointer belongs to is the caller's business.
struct BufLoad {
    const uint64_t* buf;
    MC_HOST_DEVICE uint64_t operator()(uint64_t idx) const { return buf[idx]; }
};

// Raw-buffer wrappers — the honest prover and the verifier use these.
// chained defaults to false so all existing callers and the known-answer test
// are unchanged.
static inline uint64_t answer_lane(const uint64_t* buf, uint64_t num_words,
                                   uint64_t nonce, uint32_t lane, uint32_t samples,
                                   bool chained = false) {
    return answer_lane_t(BufLoad{buf}, num_words, nonce, lane, samples, chained);
}
static inline uint64_t answer_challenge(const uint64_t* buf, uint64_t num_words,
                                        uint64_t nonce, uint32_t k, uint32_t lanes,
                                        bool chained = false) {
    return answer_challenge_t(BufLoad{buf}, num_words, nonce, k, lanes, chained);
}

// The tree-fold answer over a raw buffer. `scratch` must hold `lanes` entries
// and is destroyed; the caller owns it because this header does not allocate.
static inline uint64_t answer_challenge_tree(const uint64_t* buf, uint64_t num_words,
                                             uint64_t nonce, uint32_t k, uint32_t lanes,
                                             bool chained, uint64_t* scratch) {
    if (lanes == 0) lanes = 1;
    answer_lanes_t(BufLoad{buf}, num_words, nonce, k, lanes, chained, scratch);
    return tree_fold(scratch, lanes, nonce, k);
}

} // namespace mc
