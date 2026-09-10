// tests/known_answer_argon2d.cpp — pins src/argon2d.hpp to published vectors.
//
//   Vector A — Blake2b, RFC 7693 Appendix A
//   Vector B — Argon2d, RFC 9106 section 5.1
//   Vector C — memchal's build_buffer() wrapper, which no RFC covers
//
//   g++ -O3 -std=c++17 -I/work/src -o tests/known_answer_argon2d \
//       tests/known_answer_argon2d.cpp
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "argon2d.hpp"

using namespace mc;

static int g_failed = 0;

static void report(const char* name, const uint8_t* got, const uint8_t* want,
                   size_t n) {
    const bool ok = memcmp(got, want, n) == 0;
    printf("%-28s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        ++g_failed;
        printf("  got : "); for (size_t i = 0; i < n; ++i) printf("%02x", got[i]);
        printf("\n  want: "); for (size_t i = 0; i < n; ++i) printf("%02x", want[i]);
        printf("\n");
    }
}

static void check(const char* name, bool ok) {
    printf("%-28s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failed;
}

// Vector A — Blake2b-512("abc"), RFC 7693 Appendix A.
static void vector_blake2b() {
    static const uint8_t want[64] = {
        0xba,0x80,0xa5,0x3f,0x98,0x1c,0x4d,0x0d,0x6a,0x27,0x97,0xb6,0x9f,0x12,0xf6,0xe9,
        0x4c,0x21,0x2f,0x14,0x68,0x5a,0xc4,0xb7,0x4b,0x12,0xbb,0x6f,0xdb,0xff,0xa2,0xd1,
        0x7d,0x87,0xc5,0x39,0x2a,0xab,0x79,0x2d,0xc2,0x52,0xd5,0xde,0x45,0x33,0xcc,0x95,
        0x18,0xd3,0x8a,0xa8,0xdb,0xf1,0x92,0x5a,0xb9,0x23,0x86,0xed,0xd4,0x00,0x99,0x23
    };
    uint8_t got[64];
    a2::blake2b(got, 64, "abc", 3);
    report("Vector A blake2b-512(abc)", got, want, 64);
}

// Vector B — Argon2d, RFC 9106 section 5.1: v0x13, m=32 KiB, t=3, p=4,
// taglen=32, password 32x01, salt 16x02, secret 8x03, associated data 12x04.
static void vector_argon2d_rfc() {
    static const uint8_t want[32] = {
        0x51,0x2b,0x39,0x1b,0x6f,0x11,0x62,0x97,0x53,0x71,0xd3,0x09,0x19,0x73,0x42,0x94,
        0xf8,0x68,0xe3,0xbe,0x39,0x84,0xf3,0xc1,0xa1,0x3a,0x4d,0xb9,0xfa,0xbe,0x4a,0xcb
    };
    uint8_t pwd[32], salt[16], secret[8], ad[12];
    memset(pwd, 0x01, sizeof pwd);
    memset(salt, 0x02, sizeof salt);
    memset(secret, 0x03, sizeof secret);
    memset(ad, 0x04, sizeof ad);

    a2::Params prm;
    prm.pwd = pwd;       prm.pwdlen    = sizeof pwd;
    prm.salt = salt;     prm.saltlen   = sizeof salt;
    prm.secret = secret; prm.secretlen = sizeof secret;
    prm.ad = ad;         prm.adlen     = sizeof ad;
    prm.m_kib = 32; prm.t = 3; prm.p = 4;

    const uint32_t blocks = a2::argon2d_blocks(prm.m_kib, prm.p);
    uint64_t* mem = (uint64_t*)malloc((size_t)blocks * a2::kBlockBytes);
    if (!mem) { printf("Vector B: allocation failed\n"); ++g_failed; return; }

    uint8_t got[32];
    if (!a2::argon2d_raw(mem, prm, got, 32)) {
        printf("Vector B: argon2d_raw rejected valid parameters\n");
        ++g_failed; free(mem); return;
    }
    report("Vector B argon2d RFC 9106", got, want, 32);
    free(mem);
}

// Vector C — build_buffer(), the matrix rather than the tag. No RFC covers it.
static void vector_build_buffer() {
    Recipe r;
    memset(r.key, 0xA5, sizeof r.key);
    memset(r.salt, 0x5A, sizeof r.salt);
    r.m_kib = 1024;   // 1 MiB
    r.t = 2;
    r.p = 1;

    const uint64_t words = build_buffer_words(r);
    check("Vector C sizing (1 MiB, p=1)", words == 1024ull * a2::kBlockWords);

    uint64_t* a = (uint64_t*)malloc(words * 8);
    uint64_t* b = (uint64_t*)malloc(words * 8);
    if (!a || !b) { printf("Vector C: allocation failed\n"); ++g_failed; free(a); free(b); return; }

    check("Vector C build succeeds", build_buffer(a, words, r));
    check("Vector C build is deterministic",
          build_buffer(b, words, r) && memcmp(a, b, words * 8) == 0);

    check("Vector C rejects wrong size", !build_buffer(a, words - 1, r));

    Recipe r2 = r;
    r2.key[0] ^= 0x01;
    check("Vector C key changes buffer",
          build_buffer(b, words, r2) && memcmp(a, b, words * 8) != 0);

    Recipe r3 = r;
    r3.workload_tag[0] ^= 0x01;
    check("Vector C workload tag binds",
          build_buffer(b, words, r3) && memcmp(a, b, words * 8) != 0);

    // 1000 KiB with p=4 rounds to 992.
    Recipe r4 = r; r4.m_kib = 1000; r4.p = 4;
    check("Vector C rounds m down to 4*p",
          build_buffer_words(r4) == 992ull * a2::kBlockWords);

    free(a); free(b);
}

int main() {
    printf("=== Known-answer test: buffer generator (Argon2d) ===\n");
    vector_blake2b();
    vector_argon2d_rfc();
    vector_build_buffer();
    if (g_failed) {
        fprintf(stderr, "\n%d vector(s) FAILED — the generator changed.\n", g_failed);
        return 1;
    }
    printf("\nAll generator vectors PASS\n");
    return 0;
}
