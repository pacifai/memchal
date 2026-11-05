#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "shared/chacha20.hpp"
#include "shared/challenge.hpp"
#include "shared/fill.hpp"

using namespace mc;

static const uint64_t kNumWords = (1u << 20) / 8;
static const uint64_t kSeed     = 0xABCDEF0123456789ULL;

struct Vector { uint64_t nonce; uint32_t samples; uint32_t lanes; };
static const Vector kVectors[3] = {
    { 0x0000000000000001ULL, 1,     1  },
    { 0x1234567890ABCDEFULL, 4096,  32 },
    { 0xFFFFFFFFFFFFFFFFULL, 65536, 64 },
};

static const uint64_t kSequential[3]        = { 0xDB349A095B054DC1ULL, 0xCA0AC4E4D3BAE406ULL, 0x404EF3CD1DDBD68CULL };
static const uint64_t kSequentialChained[3] = { 0x9223B322843E6ED1ULL, 0xB48B8C33BFD2C50EULL, 0x6889B0F93AE541F3ULL };
static const uint64_t kTree[3]              = { 0xDB349A095B054DC1ULL, 0x3D7231F696115A72ULL, 0x75D36F59F81165F0ULL };
static const uint64_t kTreeChained[3]       = { 0x9223B322843E6ED1ULL, 0x6CB30B91F44FF207ULL, 0x86106229AE2DDC5BULL };

static int check(const char* name, uint64_t got, uint64_t want) {
    const bool pass = got == want;
    printf("%-36s %s  (got=0x%016llX  expected=0x%016llX)\n", name, pass ? "PASS" : "FAIL",
           (unsigned long long)got, (unsigned long long)want);
    return pass ? 0 : 1;
}

static int check_true(const char* name, bool ok) {
    printf("%-36s %s\n", name, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int answer_vectors(const uint64_t* buf) {
    int failed = 0;
    char name[64];
    for (int chained = 0; chained <= 1; ++chained) {
        for (int i = 0; i < 3; ++i) {
            const Vector& v = kVectors[i];
            snprintf(name, sizeof name, "sequential%s vector %d", chained ? " chained" : "", i + 1);
            failed += check(name, answer_sequential(BufferLoad{buf}, kNumWords, v.nonce, v.samples,
                                                    v.lanes, chained),
                            (chained ? kSequentialChained : kSequential)[i]);
            std::vector<uint64_t> scratch(v.lanes);
            snprintf(name, sizeof name, "tree%s vector %d", chained ? " chained" : "", i + 1);
            failed += check(name, answer_tree(BufferLoad{buf}, kNumWords, v.nonce, v.samples, v.lanes,
                                              chained, scratch.data()),
                            (chained ? kTreeChained : kTree)[i]);
        }
    }
    return failed;
}

static int tree_properties(const uint64_t* buf) {
    int failed = 0;
    for (int i = 1; i < 3; ++i) {
        const Vector& v = kVectors[i];
        std::vector<uint64_t> scratch(v.lanes);
        const uint64_t tree = answer_tree(BufferLoad{buf}, kNumWords, v.nonce, v.samples, v.lanes,
                                          false, scratch.data());
        const uint64_t seq = answer_sequential(BufferLoad{buf}, kNumWords, v.nonce, v.samples,
                                               v.lanes, false);
        failed += check_true("tree differs from sequential", tree != seq);
    }

    const uint32_t lanes = 64, samples = 65536;
    const uint64_t nonce = 0x1234ULL;
    std::vector<uint64_t> a(lanes), b(lanes);
    answer_lanes(BufferLoad{buf}, kNumWords, nonce, samples, lanes, false, a.data());
    b = a;
    std::swap(b[0], b[1]);
    failed += check_true("tree sibling order matters",
                         tree_fold(a.data(), lanes, nonce, samples) !=
                         tree_fold(b.data(), lanes, nonce, samples));

    for (uint32_t odd : { 3u, 5u, 7u, 17u, 100u }) {
        std::vector<uint64_t> s1(odd), s2(odd);
        char name[64];
        snprintf(name, sizeof name, "tree odd lanes=%u deterministic", odd);
        failed += check_true(name,
            answer_tree(BufferLoad{buf}, kNumWords, 0xDEADBEEFULL, odd * 16, odd, true, s1.data()) ==
            answer_tree(BufferLoad{buf}, kNumWords, 0xDEADBEEFULL, odd * 16, odd, true, s2.data()));
    }
    return failed;
}

// RFC 8439 section 2.3.2.
static int chacha20_rfc_vector() {
    uint8_t key[32], out[64];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
    static const uint8_t nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    static const uint8_t expected[64] = {
        0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15, 0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7,0x33,0xc0,0x68,0x03, 0x04,0x22,0xaa,0x9a,0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46,0x07,0x9f,0xaa,0x09, 0x14,0xc2,0xd7,0x05,0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1,0xde,0x16,0x4e,0xb9, 0xcb,0xd0,0x83,0xe8,0xa2,0x50,0x3c,0x4e,
    };
    chacha20_block(key, nonce, 1, out);
    return check_true("chacha20 RFC 8439 block", memcmp(out, expected, 64) == 0);
}

static int chacha_fill() {
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
    static const uint64_t expected[4] = {
        0x6A19C5D97D2BFD39ULL, 0x494ADCB87703BD8DULL, 0xCC6ADEBC6FD8358AULL, 0x9224EAD84C7DCCB2ULL };
    uint64_t buf[4] = {};
    fill_chacha(buf, 4, key);
    int failed = 0;
    char name[64];
    for (int i = 0; i < 4; ++i) {
        snprintf(name, sizeof name, "fill_chacha word %d", i);
        failed += check(name, buf[i], expected[i]);
    }
    return failed;
}

static int splitmix64_random_access() {
    const uint64_t seed = 0xDEAD600DC0FFEEULL;
    uint64_t buf[1024];
    fill_splitmix64(buf, 1024, seed);
    int failed = 0;
    char name[64];
    for (uint64_t idx : { 0, 1, 7, 63, 255, 512, 1023 }) {
        snprintf(name, sizeof name, "splitmix64_word %llu", (unsigned long long)idx);
        failed += check(name, splitmix64_word(seed, idx), buf[idx]);
    }
    return failed;
}

int main() {
    std::vector<uint64_t> buf(kNumWords);
    fill_splitmix64(buf.data(), kNumWords, kSeed);
    const int failed = answer_vectors(buf.data()) + tree_properties(buf.data()) +
                       chacha20_rfc_vector() + chacha_fill() + splitmix64_random_access();
    if (failed) { fprintf(stderr, "%d check(s) FAILED\n", failed); return 1; }
    printf("All vectors PASS\n");
    return 0;
}
