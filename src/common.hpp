// common.hpp — shared protocol, sampling, and hashing for the memory-challenge PoC.
//
// The functions answer_lane() / answer_challenge() are the *only* place the
// challenge is defined. Both the prover (to respond) and the verifier (to check)
// call the exact same code, so there is a single source of truth.
//
// Everything in the hot path is plain C: raw pointers, fixed-width ints, no STL,
// no exceptions, no 128-bit-only tricks that don't exist on a GPU. See the
// "GPU PORT" notes below each function.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "chacha20.hpp"
#include "argon2d.hpp"

namespace mc {

constexpr uint64_t kMagic       = 0x4D454D4348414C31ULL; // "MEMCHAL1"
constexpr uint16_t kDefaultPort = 9099;
constexpr uint32_t kFlagTerminate = 1u;   // Challenge.flags
constexpr uint32_t kFlagChained   = 2u;   // Challenge.flags
constexpr uint32_t kFlagBuild     = 4u;   // Hello.flags: a Recipe follows, not bytes

// Wire version. 1 = the verifier uploads the buffer. 2 = it may instead send a
// Recipe and both sides build (ADR 0007). A version-1 Hello never carries
// kFlagBuild, so a version-2 prover still serves a version-1 verifier.
constexpr uint32_t kWireVersion = 2u;

// Minimum effective chain depth (k / lanes) required when --chain dependent.
// At depth D, an attacker can pipeline across lanes and achieve ~1/D of the
// latency-hardness compared to a fully serial chain.  Below this floor the
// dependent access mode offers negligible defense.
constexpr uint32_t kMinChainDepth = 16u;

// Fixed-layout wire messages. Native byte order: this PoC assumes prover and
// verifier run on the same-endian machines (true for loopback / x86 / ARM LE).
#pragma pack(push, 1)
struct Hello     { uint64_t magic; uint64_t buffer_bytes; uint32_t version; uint32_t flags; };
struct Ready     { uint64_t magic; uint32_t locked; uint32_t pad; };
struct Challenge { uint64_t nonce; uint32_t k; uint32_t lanes; uint32_t flags; uint32_t pad; };
struct Response  { uint64_t hash; };
// Sent immediately after a Hello carrying kFlagBuild, in place of the buffer.
struct RecipeMsg { uint8_t key[32]; uint8_t salt[16];
                   uint32_t m_kib; uint32_t t; uint32_t p;
                   uint8_t workload_tag[32]; };
#pragma pack(pop)

static inline Recipe recipe_from_msg(const RecipeMsg& m) {
    Recipe r;
    memcpy(r.key, m.key, sizeof r.key);
    memcpy(r.salt, m.salt, sizeof r.salt);
    memcpy(r.workload_tag, m.workload_tag, sizeof r.workload_tag);
    r.m_kib = m.m_kib; r.t = m.t; r.p = m.p;
    return r;
}
static inline RecipeMsg msg_from_recipe(const Recipe& r) {
    RecipeMsg m{};
    memcpy(m.key, r.key, sizeof m.key);
    memcpy(m.salt, r.salt, sizeof m.salt);
    memcpy(m.workload_tag, r.workload_tag, sizeof m.workload_tag);
    m.m_kib = r.m_kib; m.t = r.t; m.p = r.p;
    return m;
}


// ---------------------------------------------------------------------------
// Hashing / sampling core
// ---------------------------------------------------------------------------

// splitmix64 finalizer constants.
static constexpr uint64_t kSM64_C1  = 0xBF58476D1CE4E5B9ULL;
static constexpr uint64_t kSM64_C2  = 0x94D049BB133111EBULL;
static constexpr uint64_t kSM64_GOLDEN = 0x9E3779B97F4A7C15ULL;

// The 3-step mixing half of splitmix64 (no increment).  Called by splitmix64
// and also by the recompute-adversary helpers below.
static inline uint64_t splitmix64_finalize(uint64_t z) {
    z = (z ^ (z >> 30)) * kSM64_C1;
    z = (z ^ (z >> 27)) * kSM64_C2;
    return z ^ (z >> 31);
}

// splitmix64: fast, well-distributed PRNG step. GPU PORT: identical.
// Behavior is bit-identical to the original one-liner.
static inline uint64_t splitmix64(uint64_t& s) {
    s += kSM64_GOLDEN;
    return splitmix64_finalize(s);
}

// O(1) random-access: returns the word that fill_random would place at buf[idx]
// when started with the given seed.  Equivalent to advancing the state by
// (idx+1) increments from seed, then finalizing.
static inline uint64_t splitmix64_word(uint64_t seed, uint64_t idx) {
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
static inline uint64_t inv_mul(uint64_t a) {
    uint64_t inv = a; // initial approx: a*a == 1 mod 8 when a is odd
    for (int i = 0; i < 5; ++i) inv *= 2 - a * inv;
    return inv;
}

// Recover x from y = x ^ (x >> s) for right-shifts s in [1,63].
// Iterative fix-point: applying x = y ^ (x >> s) ceil(64/s) times
// converges to the unique solution because each pass propagates the
// correct bits one window of s further down.
static inline uint64_t unxorshift_r(uint64_t y, int s) {
    uint64_t x = y;
    int iters = (63 + s) / s;   // ceil(64/s)
    for (int i = 0; i < iters; ++i)
        x = y ^ (x >> s);
    return x;
}

// Invert the splitmix64 finalizer.
static inline uint64_t splitmix64_unfinalize(uint64_t w) {
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
static inline uint64_t splitmix64_recover_seed(uint64_t word0) {
    return splitmix64_unfinalize(word0) - kSM64_GOLDEN;
}

// ---------------------------------------------------------------------------
// Generator selection
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
    // (host is little-endian for the purposes of this PoC; see comment in
    // common.hpp near kMagic: "same-endian machines").
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

// mix64: MurmurHash3 finalizer, a strong 64-bit avalanche. GPU PORT: identical.
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

// Uniform index in [0, n) from a 64-bit random (Lemire multiply-shift, no modulo
// bias). GPU PORT: replace the 128-bit multiply with `__umul64hi(r, n)`.
static inline uint64_t map_index(uint64_t r, uint64_t n) {
    return (uint64_t)(((unsigned __int128)r * (unsigned __int128)n) >> 64);
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
// GPU PORT: this becomes the per-thread body of a CUDA kernel; the loader is a
// direct global-memory read from a cudaMalloc buffer. Each thread owns one lane.
//
// When chained==false (default): the EXACT current independent code path — do
// not change a single operation, so existing known-answer vectors remain
// bit-identical.
// When chained==true: pointer-chasing loop where each index depends on the
// previously-read word, preventing any pre-enumeration of offsets.
template <typename Load>
static inline uint64_t answer_lane_t(Load&& load, uint64_t num_words,
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

// Full challenge answer: partition k samples across `lanes` and fold the lane
// results together. Deterministic given (nonce, k, lanes) and the data.
//
// GPU PORT: launch `lanes` threads computing answer_lane_t() into a scratch
// array, then reduce. The fold below is the sequential CPU reference.
//
// chained==false (default): independent path, bit-identical to original.
// chained==true: each lane uses the data-dependent (pointer-chasing) path.
template <typename Load>
static inline uint64_t answer_challenge_t(Load&& load, uint64_t num_words,
                                          uint64_t nonce, uint32_t k, uint32_t lanes,
                                          bool chained = false) {
    if (lanes == 0) lanes = 1;
    uint32_t per = k / lanes;
    uint32_t rem = k % lanes;
    uint64_t combined = mix64(nonce ^ ((uint64_t)k << 20) ^ (uint64_t)lanes);
    for (uint32_t l = 0; l < lanes; ++l) {
        uint32_t samples = per + (l < rem ? 1u : 0u);
        uint64_t h = answer_lane_t(load, num_words, nonce, l, samples, chained);
        combined = mix64(combined ^ mix64(h + 0x9E3779B97F4A7C15ULL * (uint64_t)(l + 1)));
    }
    return combined;
}

// Raw-buffer wrappers — the honest prover and the verifier use these.
// chained defaults to false so all existing callers and the known-answer test
// are unchanged.
static inline uint64_t answer_lane(const uint64_t* buf, uint64_t num_words,
                                   uint64_t nonce, uint32_t lane, uint32_t samples,
                                   bool chained = false) {
    return answer_lane_t([buf](uint64_t idx){ return buf[idx]; },
                         num_words, nonce, lane, samples, chained);
}
static inline uint64_t answer_challenge(const uint64_t* buf, uint64_t num_words,
                                        uint64_t nonce, uint32_t k, uint32_t lanes,
                                        bool chained = false) {
    return answer_challenge_t([buf](uint64_t idx){ return buf[idx]; },
                              num_words, nonce, k, lanes, chained);
}

// ---------------------------------------------------------------------------
// Blocking socket helpers
// ---------------------------------------------------------------------------
static inline bool send_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}
static inline bool recv_all(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        ssize_t k = ::recv(fd, c, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}
// Where a prover's buffer bytes come from. Every mode ingests the buffer through
// src_read(), so the send-buffer and build-buffer paths differ in one place
// rather than in each mode's setup.
//
// Build mode materializes the whole matrix up front, because Argon2 cannot
// produce part of it: a prover that intends to spill to disk still needs
// buffer-sized RAM for the duration of the build.
struct Source {
    int             fd    = -1;       // socket, when receiving
    const uint64_t* built = nullptr;  // whole built buffer, when building
    size_t          off   = 0;        // read cursor into `built`
};
static inline bool src_read(Source& s, void* dst, size_t n) {
    if (!s.built) return recv_all(s.fd, dst, n);
    memcpy(dst, (const uint8_t*)s.built + s.off, n);
    s.off += n;
    return true;
}

static inline void set_low_latency(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // disable Nagle
}

// write() all n bytes to a (file) descriptor.
static inline bool write_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::write(fd, c, n);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}

// Deterministic buffer fill (verifier generates; also handy for tests).
// Delegates to fill_buffer(SPLITMIX64) — behavior is bit-identical to the
// original implementation.
static inline void fill_random(uint64_t* buf, uint64_t num_words, uint64_t seed) {
    fill_buffer(buf, num_words, seed, Gen::SPLITMIX64);
}

// Bytes this process currently has locked (wired), as reported by the OS, so we
// can VERIFY mlock took effect instead of trusting its return value. Returns
// (size_t)-1 if it cannot be determined (e.g. no /proc, as on macOS). On Linux
// it reads VmLck from /proc/self/status.
static inline size_t locked_bytes() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return (size_t)-1;
    char line[256]; unsigned long kb = 0; bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmLck: %lu kB", &kb) == 1) { found = true; break; }
    }
    fclose(f);
    return found ? (size_t)kb * 1024 : (size_t)-1;
}

// Bytes this process has actually fetched from the block layer, per the OS
// (/proc/self/io read_bytes). Lets us check whether "disk" reads are genuinely
// cold (read_bytes grows) or page-cache hits (read_bytes ~ 0 after one sweep).
// Returns (size_t)-1 if unavailable (e.g. macOS).
static inline size_t io_read_bytes() {
    FILE* f = fopen("/proc/self/io", "r");
    if (!f) return (size_t)-1;
    char line[256]; unsigned long long b = 0; bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "read_bytes: %llu", &b) == 1) { found = true; break; }
    }
    fclose(f);
    return found ? (size_t)b : (size_t)-1;
}

// Major page faults this process has taken (faults that had to go to storage),
// from field 12 of /proc/self/stat. For the mmap adversary this is the direct
// counter of "probes that could NOT be served from the page cache": with the
// file cached it stays ~0; under memory pressure it grows with every probe.
// Returns (size_t)-1 if unavailable.
static inline size_t major_faults() {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return (size_t)-1;
    char buf[1024];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!got) return (size_t)-1;
    buf[got] = '\0';
    // comm (field 2) may contain spaces/parens; parse after the LAST ')'.
    char* p = strrchr(buf, ')');
    if (!p) return (size_t)-1;
    ++p;
    // Fields after comm: state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8)
    // flags(9) minflt(10) cminflt(11) majflt(12) -> the 10th token from here.
    unsigned long long v = 0;
    int field = 2; // we are positioned just after field 2
    char* tok = strtok(p, " ");
    while (tok) {
        ++field;
        if (field == 12) { v = strtoull(tok, nullptr, 10); return (size_t)v; }
        tok = strtok(nullptr, " ");
    }
    return (size_t)-1;
}

} // namespace mc
