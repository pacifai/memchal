// tests/known_answer.cpp — Known-Answer Test for the memory-challenge math.
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
//   g++ -O3 -std=c++17 -I/work/src -o tests/known_answer tests/known_answer.cpp
//
// EXIT CODE
//   0 — all vectors pass
//   1 — at least one vector failed

#include "common.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// ---------------------------------------------------------------------------
// Vector group 1: original fill_random / answer_challenge vectors (must not change)
// ---------------------------------------------------------------------------

// Independent path (chained=false) — same 3 vectors, unchanged.
static int kat_challenge() {
    static const uint64_t kBytes    = 1ULL * 1024 * 1024;
    static const uint64_t kNumWords = kBytes / 8;   // 131 072
    static const uint64_t kSeed     = 0xABCDEF0123456789ULL;

    uint64_t* buf = (uint64_t*)malloc(kBytes);
    if (!buf) { fprintf(stderr, "[kat] malloc failed\n"); return 1; }
    mc::fill_random(buf, kNumWords, kSeed);

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
        printf("challenge vector %d: %s  (got=0x%016llX  expected=0x%016llX)\n",
               i + 1,
               pass ? "PASS" : "FAIL",
               (unsigned long long)got,
               (unsigned long long)kExpected[i]);
    }
    free(buf);

    if (failed == 0) {
        printf("challenge math: all 3 vectors PASS\n");
    } else {
        fprintf(stderr, "challenge math: %d/3 vectors FAILED — challenge math has changed!\n", failed);
    }
    return failed;
}

// ---------------------------------------------------------------------------
// Vector group 1b: chained (data-dependent) path — same buffer and vectors, different
// expected values because idx_{i+1} depends on w_i = load(idx_i).
//
// Values computed once from the current chained loop implementation and
// pinned here.  A change to the chained math breaks this test.
//
// Dependent vector 1: nonce=0x0000000000000001, k=1,     lanes=1
// Dependent vector 2: nonce=0x1234567890ABCDEF, k=4096,  lanes=32
// Dependent vector 3: nonce=0xFFFFFFFFFFFFFFFF, k=65536, lanes=64
// ---------------------------------------------------------------------------
static const uint64_t kExpectedChained[3] = {
    0x9223B322843E6ED1ULL,   // dependent vector 1
    0xB48B8C33BFD2C50EULL,   // dependent vector 2
    0x6889B0F93AE541F3ULL,   // dependent vector 3
};

static int kat_challenge_chained() {
    static const uint64_t kBytes    = 1ULL * 1024 * 1024;
    static const uint64_t kNumWords = kBytes / 8;   // 131 072
    static const uint64_t kSeed     = 0xABCDEF0123456789ULL;

    uint64_t* buf = (uint64_t*)malloc(kBytes);
    if (!buf) { fprintf(stderr, "[kat] malloc failed\n"); return 1; }
    mc::fill_random(buf, kNumWords, kSeed);

    struct Vec { uint64_t nonce; uint32_t k; uint32_t lanes; };
    static const Vec vecs[3] = {
        { 0x0000000000000001ULL, 1,     1  },
        { 0x1234567890ABCDEFULL, 4096,  32 },
        { 0xFFFFFFFFFFFFFFFFULL, 65536, 64 },
    };

    int failed = 0;
    for (int i = 0; i < 3; ++i) {
        uint64_t got = mc::answer_challenge(buf, kNumWords,
                                            vecs[i].nonce, vecs[i].k, vecs[i].lanes,
                                            /*chained=*/true);
        bool pass = (got == kExpectedChained[i]);
        if (!pass) ++failed;
        printf("chained challenge vector %d: %s  (got=0x%016llX  expected=0x%016llX)\n",
               i + 1,
               pass ? "PASS" : "FAIL",
               (unsigned long long)got,
               (unsigned long long)kExpectedChained[i]);
    }
    free(buf);

    if (failed == 0) {
        printf("chained challenge math: all 3 vectors PASS\n");
    } else {
        fprintf(stderr, "chained challenge math: %d/3 vectors FAILED — chained math has changed!\n", failed);
    }
    return failed;
}

// ---------------------------------------------------------------------------
// Vector group 2: ChaCha20 RFC 8439 §2.3.2 test vector
//
// key   = 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
//         10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
// nonce = 00 00 00 09 00 00 00 4a 00 00 00 00
// counter = 1
// Expected keystream first 64 bytes (RFC 8439 §2.3.2):
//   10 f1 e7 e4 d1 3b 59 15 50 0f dd 1f a3 20 71 c4
//   c7 d1 f4 c7 33 c0 68 03 04 22 aa 9a c3 d4 6c 4e
//   d2 82 64 46 07 9f aa 09 14 c2 d7 05 d9 8b 02 a2
//   b5 12 9c d1 de 16 4e b9 cb d0 83 e8 a2 50 3c 4e
// ---------------------------------------------------------------------------
static int kat_chacha20() {
    static const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    };
    static const uint8_t nonce[12] = {
        0x00,0x00,0x00,0x09,
        0x00,0x00,0x00,0x4a,
        0x00,0x00,0x00,0x00,
    };
    static const uint8_t expected[64] = {
        0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,
        0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7,0x33,0xc0,0x68,0x03,
        0x04,0x22,0xaa,0x9a,0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46,0x07,0x9f,0xaa,0x09,
        0x14,0xc2,0xd7,0x05,0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1,0xde,0x16,0x4e,0xb9,
        0xcb,0xd0,0x83,0xe8,0xa2,0x50,0x3c,0x4e,
    };

    uint8_t out[64];
    mc::chacha20_block(key, nonce, 1, out);

    // Assert the first 16 bytes (minimum required); we check all 64 for strictness.
    bool pass = (memcmp(out, expected, 64) == 0);
    printf("chacha20 RFC 8439 §2.3.2: %s", pass ? "PASS" : "FAIL");
    if (!pass) {
        printf("\n  expected: ");
        for (int i = 0; i < 16; ++i) printf("%02x ", expected[i]);
        printf("...\n  got:      ");
        for (int i = 0; i < 16; ++i) printf("%02x ", out[i]);
    }
    printf("\n");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Vector group 3: splitmix64 recompute round-trip
//
// Fill a buffer with a fixed seed using fill_random().
// Assert splitmix64_recover_seed(buf[0]) == seed.
// Assert splitmix64_word(seed, idx) == buf[idx] for several idx values.
// ---------------------------------------------------------------------------
static int kat_recompute() {
    static const uint64_t kSeed     = 0xDEAD600DC0FFEEULL;
    static const uint64_t kNumWords = 1024;

    uint64_t buf[kNumWords];
    mc::fill_random(buf, kNumWords, kSeed);

    int failed = 0;

    // Recover seed from buf[0].
    uint64_t recovered = mc::splitmix64_recover_seed(buf[0]);
    bool seed_ok = (recovered == kSeed);
    if (!seed_ok) ++failed;
    printf("recompute seed recovery: %s  (recovered=0x%016llX  expected=0x%016llX)\n",
           seed_ok ? "PASS" : "FAIL",
           (unsigned long long)recovered, (unsigned long long)kSeed);

    // Check O(1) indexed access for several positions.
    const uint64_t check_idx[] = { 0, 1, 7, 63, 255, 512, 1023 };
    for (uint64_t idx : check_idx) {
        uint64_t word = mc::splitmix64_word(kSeed, idx);
        bool ok = (word == buf[idx]);
        if (!ok) ++failed;
        printf("recompute word[%4llu]: %s  (got=0x%016llX  buf=0x%016llX)\n",
               (unsigned long long)idx, ok ? "PASS" : "FAIL",
               (unsigned long long)word, (unsigned long long)buf[idx]);
    }

    if (failed == 0)
        printf("recompute: all checks PASS\n");
    else
        fprintf(stderr, "recompute: %d check(s) FAILED\n", failed);
    return failed;
}

// ---------------------------------------------------------------------------
// Vector group 4: fill_buffer_chacha with a fixed 32-byte key
//
// Pins the byte→word ordering and key handling in fill_buffer_chacha() so that
// a silent change to the fill or the chacha20_fill 64-bit addressing logic
// causes this test to fail.
//
// Key: bytes 0..31 (0x00, 0x01, ..., 0x1f)
// Nonce: all-zero 12 bytes, starting block index: 0
// Expected: first 4 uint64_t words of the output buffer (LE interpretation
//   of the raw ChaCha20 keystream bytes), computed once and hardcoded here.
// ---------------------------------------------------------------------------
static int kat_fill_chacha() {
    static const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    };

    // Expected: ChaCha20 block(key, nonce=0, counter=0) interpreted as
    // 8 little-endian uint64_t words.
    static const uint64_t kExpectedFill[4] = {
        0x6A19C5D97D2BFD39ULL,   // buf[0]
        0x494ADCB87703BD8DULL,   // buf[1]
        0xCC6ADEBC6FD8358AULL,   // buf[2]
        0x9224EAD84C7DCCB2ULL,   // buf[3]
    };

    uint64_t buf[4] = {};
    mc::fill_buffer_chacha(buf, 4, key);

    int failed = 0;
    for (int i = 0; i < 4; ++i) {
        bool pass = (buf[i] == kExpectedFill[i]);
        if (!pass) ++failed;
        printf("fill_buffer_chacha word[%d]: %s  (got=0x%016llX  expected=0x%016llX)\n",
               i, pass ? "PASS" : "FAIL",
               (unsigned long long)buf[i],
               (unsigned long long)kExpectedFill[i]);
    }

    if (failed == 0)
        printf("fill_buffer_chacha: all 4 words PASS\n");
    else
        fprintf(stderr, "fill_buffer_chacha: %d word(s) FAILED — fill or byte-order changed!\n", failed);
    return failed;
}

int main() {
    int total_failed = 0;

    printf("=== Known-answer test: challenge math (original vectors) ===\n");
    total_failed += kat_challenge();

    printf("\n=== Known-answer test: challenge math (chained / data-dependent path) ===\n");
    total_failed += kat_challenge_chained();

    printf("\n=== Known-answer test: ChaCha20 RFC 8439 §2.3.2 ===\n");
    total_failed += kat_chacha20();

    printf("\n=== Known-answer test: splitmix64 recompute round-trip ===\n");
    total_failed += kat_recompute();

    printf("\n=== Known-answer test: fill_buffer_chacha (keyed fill, fixed key) ===\n");
    total_failed += kat_fill_chacha();

    printf("\n");
    if (total_failed == 0) {
        printf("All vectors PASS\n");
        return 0;
    } else {
        fprintf(stderr, "%d vector(s) FAILED\n", total_failed);
        return 1;
    }
}
