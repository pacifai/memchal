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

namespace mc {

constexpr uint64_t kMagic       = 0x4D454D4348414C31ULL; // "MEMCHAL1"
constexpr uint16_t kDefaultPort = 9099;
constexpr uint32_t kFlagTerminate = 1u;

// Fixed-layout wire messages. Native byte order: this PoC assumes prover and
// verifier run on the same-endian machines (true for loopback / x86 / ARM LE).
#pragma pack(push, 1)
struct Hello     { uint64_t magic; uint64_t buffer_bytes; uint32_t version; uint32_t flags; };
struct Ready     { uint64_t magic; uint32_t locked; uint32_t pad; };
struct Challenge { uint64_t nonce; uint32_t k; uint32_t lanes; uint32_t flags; uint32_t pad; };
struct Response  { uint64_t hash; };
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Hashing / sampling core
// ---------------------------------------------------------------------------

// splitmix64: fast, well-distributed PRNG step. GPU PORT: identical.
static inline uint64_t splitmix64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
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
template <typename Load>
static inline uint64_t answer_lane_t(Load&& load, uint64_t num_words,
                                     uint64_t nonce, uint32_t lane, uint32_t samples) {
    uint64_t s   = mix64(nonce ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(lane + 1)));
    uint64_t acc = mix64(nonce + 0x100000001B3ULL * (uint64_t)(lane + 1));
    for (uint32_t i = 0; i < samples; ++i) {
        uint64_t r   = splitmix64(s);
        uint64_t idx = map_index(r, num_words);
        uint64_t w   = load(idx);                // <-- the probe (RAM/disk/…)
        acc = mix64(acc ^ (w + 0x9E3779B97F4A7C15ULL * (uint64_t)i) ^ idx);
    }
    return acc;
}

// Full challenge answer: partition k samples across `lanes` and fold the lane
// results together. Deterministic given (nonce, k, lanes) and the data.
//
// GPU PORT: launch `lanes` threads computing answer_lane_t() into a scratch
// array, then reduce. The fold below is the sequential CPU reference.
template <typename Load>
static inline uint64_t answer_challenge_t(Load&& load, uint64_t num_words,
                                          uint64_t nonce, uint32_t k, uint32_t lanes) {
    if (lanes == 0) lanes = 1;
    uint32_t per = k / lanes;
    uint32_t rem = k % lanes;
    uint64_t combined = mix64(nonce ^ ((uint64_t)k << 20) ^ (uint64_t)lanes);
    for (uint32_t l = 0; l < lanes; ++l) {
        uint32_t samples = per + (l < rem ? 1u : 0u);
        uint64_t h = answer_lane_t(load, num_words, nonce, l, samples);
        combined = mix64(combined ^ mix64(h + 0x9E3779B97F4A7C15ULL * (uint64_t)(l + 1)));
    }
    return combined;
}

// Raw-buffer wrappers — the honest prover and the verifier use these.
static inline uint64_t answer_lane(const uint64_t* buf, uint64_t num_words,
                                   uint64_t nonce, uint32_t lane, uint32_t samples) {
    return answer_lane_t([buf](uint64_t idx){ return buf[idx]; },
                         num_words, nonce, lane, samples);
}
static inline uint64_t answer_challenge(const uint64_t* buf, uint64_t num_words,
                                        uint64_t nonce, uint32_t k, uint32_t lanes) {
    return answer_challenge_t([buf](uint64_t idx){ return buf[idx]; },
                              num_words, nonce, k, lanes);
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
static inline void fill_random(uint64_t* buf, uint64_t num_words, uint64_t seed) {
    uint64_t s = seed ? seed : 0xDEADBEEFCAFEF00DULL;
    for (uint64_t i = 0; i < num_words; ++i) buf[i] = splitmix64(s);
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
