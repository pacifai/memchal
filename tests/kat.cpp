// tests/kat.cpp — Known-Answer Test for the memory-challenge math.
//
// PURPOSE
// -------
// This program pins the exact output of mc::fill_random() and
// mc::answer_challenge() for three fixed (nonce, k, lanes) vectors against a
// 1 MiB buffer seeded with 0xABCDEF0123456789.
//
// WHY THIS MATTERS
// ----------------
// The challenge math in common.hpp is the single source of truth shared by both
// the prover and the verifier.  If these constants ever change, it means the
// math changed — and every previously-published latency number and correctness
// result is invalid, because it was produced against a different hash function.
//
// ARCHITECTURE INDEPENDENCE
// -------------------------
// All operations are pure 64-bit unsigned integer arithmetic (splitmix64,
// mix64, Lemire multiply-shift, XOR/ADD).  There is no floating point and no
// byte-order-dependent memory interpretation: fill_random() writes words via
// a uint64_t pointer and answer_challenge() reads them back the same way.
// Therefore the computed values are identical on any architecture (x86-64,
// ARM64, big-endian POWER, etc.) and in any container.  These three constants
// were computed on both macOS/arm64 and Linux/arm64 and matched exactly.
//
// COMPILE
//   g++ -O3 -std=c++17 -I<repo-root> -o tests/kat tests/kat.cpp
//
// EXIT CODE
//   0 — all vectors pass
//   1 — at least one vector failed

#include "common.hpp"
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Hardcoded expected values — computed by running the fill+challenge math on a
// 1 MiB buffer (131 072 words) seeded with 0xABCDEF0123456789.
//
// Vector 1: nonce=0x0000000000000001, k=1,     lanes=1
// Vector 2: nonce=0x1234567890ABCDEF, k=4096,  lanes=32
// Vector 3: nonce=0xFFFFFFFFFFFFFFFF, k=65536, lanes=64
// ---------------------------------------------------------------------------
static const uint64_t kExpected[3] = {
    0xDB349A095B054DC1ULL,   // vector 1
    0xCA0AC4E4D3BAE406ULL,   // vector 2
    0x404EF3CD1DDBD68CULL,   // vector 3
};

int main() {
    // Allocate a 1 MiB buffer and fill it deterministically.
    static const uint64_t kBytes    = 1ULL * 1024 * 1024;
    static const uint64_t kNumWords = kBytes / 8;   // 131 072
    static const uint64_t kSeed     = 0xABCDEF0123456789ULL;

    uint64_t* buf = (uint64_t*)malloc(kBytes);
    if (!buf) {
        fprintf(stderr, "[kat] malloc failed\n");
        return 1;
    }
    mc::fill_random(buf, kNumWords, kSeed);

    // Test vectors: (nonce, k, lanes).
    struct Vec { uint64_t nonce; uint32_t k; uint32_t lanes; };
    static const Vec vecs[3] = {
        { 0x0000000000000001ULL, 1,     1  },
        { 0x1234567890ABCDEFULL, 4096,  32 },
        { 0xFFFFFFFFFFFFFFFFULL, 65536, 64 },
    };

    int failed = 0;
    for (int i = 0; i < 3; ++i) {
        uint64_t got = mc::answer_challenge(buf, kNumWords,
                                            vecs[i].nonce, vecs[i].k, vecs[i].lanes);
        bool pass = (got == kExpected[i]);
        if (!pass) ++failed;
        printf("KAT vector %d: %s  (got=0x%016llX  expected=0x%016llX)\n",
               i + 1,
               pass ? "PASS" : "FAIL",
               (unsigned long long)got,
               (unsigned long long)kExpected[i]);
    }

    free(buf);

    if (failed == 0) {
        printf("KAT: all %d vectors PASS\n", 3);
        return 0;
    } else {
        fprintf(stderr, "KAT: %d/%d vectors FAILED — challenge math has changed!\n", failed, 3);
        return 1;
    }
}
