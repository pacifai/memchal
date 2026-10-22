// tests/known_answer_gpu.cpp — the GPU port's correctness gate.
//
// This is the test the port stands or falls on. It does NOT compare the GPU
// against a freshly computed CPU answer and call that agreement — that would
// only prove the two implementations drifted together. It compares the GPU
// against the SAME SIX CONSTANTS that tests/known_answer.cpp pins, which were
// computed before any CUDA existed and are protected by ADR 0001.
//
// If these pass, the CUDA kernel computes the frozen challenge math exactly:
// __umul64hi agrees with the 128-bit multiply, the per-lane sample split is the
// same, and the cross-lane fold ran in lane order rather than in whatever order
// the blocks happened to finish.
//
// Three things are checked, in order of how much they would hurt to get wrong:
//
//   1. The pinned vectors, with the buffer UPLOADED — the real protocol path.
//   2. The pinned vectors, with the buffer generated ON-DEVICE by k_fill —
//      which also proves k_fill is bit-identical to fill_random().
//   3. A wide case (many blocks, lanes past the initial scratch capacity)
//      cross-checked against the CPU reference, to catch a reduction that only
//      breaks once there is more than one block in flight.
//
// COMPILE: needs a GPU at run time; see the `prover_cuda` target in the Makefile.
// EXIT CODE: 0 all pass, 1 otherwise.
#include "challenge.hpp"
#include "gpu_answer.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The pinned constants, copied from tests/known_answer.cpp. Duplicated on
// purpose: if someone edits one file's expectations, the other still objects.
static const uint64_t kBytes    = 1ULL * 1024 * 1024;
static const uint64_t kNumWords = kBytes / 8;               // 131 072
static const uint64_t kSeed     = 0xABCDEF0123456789ULL;

struct Vec { uint64_t nonce; uint32_t k; uint32_t lanes; };
static const Vec kVecs[3] = {
    { 0x0000000000000001ULL, 1,     1  },
    { 0x1234567890ABCDEFULL, 4096,  32 },
    { 0xFFFFFFFFFFFFFFFFULL, 65536, 64 },
};
static const uint64_t kExpected[3] = {
    0xDB349A095B054DC1ULL,
    0xCA0AC4E4D3BAE406ULL,
    0x404EF3CD1DDBD68CULL,
};
static const uint64_t kExpectedChained[3] = {
    0x9223B322843E6ED1ULL,
    0xB48B8C33BFD2C50EULL,
    0x6889B0F93AE541F3ULL,
};

static int run_vectors(mc::gpu::Prover& p, const char* how) {
    int failed = 0;
    char err[512] = {0};

    for (int chained = 0; chained <= 1; ++chained) {
        const uint64_t* expect = chained ? kExpectedChained : kExpected;
        for (int i = 0; i < 3; ++i) {
            uint64_t got = 0;
            double kms = 0, cms = 0, fms = 0;
            if (!mc::gpu::answer(p, kVecs[i].nonce, kVecs[i].k, kVecs[i].lanes,
                                 chained != 0, false, &got, &kms, &cms, &fms,
                                 err, sizeof err)) {
                fprintf(stderr, "GPU KAT (%s) vector %d%s: ERROR %s\n",
                        how, i + 1, chained ? " chained" : "", err);
                ++failed;
                continue;
            }
            bool pass = (got == expect[i]);
            if (!pass) ++failed;
            printf("GPU KAT (%s) %svector %d: %s  (got=0x%016llX  expected=0x%016llX)\n",
                   how, chained ? "chained " : "", i + 1,
                   pass ? "PASS" : "FAIL",
                   (unsigned long long)got, (unsigned long long)expect[i]);
        }
    }
    return failed;
}

// A wide case: enough lanes to need many blocks and to outgrow the scratch
// buffer the prover was created with, so a fold that depends on completion
// order shows up here even if the small vectors passed.
static int run_wide(mc::gpu::Prover& p, const uint64_t* host_buf, uint64_t num_words) {
    struct WVec { uint64_t nonce; uint32_t k; uint32_t lanes; bool chained; };
    static const WVec wv[4] = {
        { 0xA5A5A5A5A5A5A5A5ULL, 1u << 17, 1024,  false },
        { 0xA5A5A5A5A5A5A5A5ULL, 1u << 17, 1024,  true  },
        { 0x0123456789ABCDEFULL, 1u << 18, 16384, false },
        { 0x0123456789ABCDEFULL, 1u << 18, 16384, true  },
    };

    int failed = 0;
    char err[512] = {0};
    for (int i = 0; i < 4; ++i) {
        uint64_t want = mc::answer_challenge(host_buf, num_words, wv[i].nonce,
                                             wv[i].k, wv[i].lanes, wv[i].chained);
        uint64_t got = 0;
        double kms = 0, cms = 0, fms = 0;
        if (!mc::gpu::answer(p, wv[i].nonce, wv[i].k, wv[i].lanes, wv[i].chained,
                             false, &got, &kms, &cms, &fms, err, sizeof err)) {
            fprintf(stderr, "GPU KAT wide %d: ERROR %s\n", i + 1, err);
            ++failed;
            continue;
        }
        bool pass = (got == want);
        if (!pass) ++failed;
        printf("GPU KAT wide k=%u lanes=%u%s: %s  (gpu=0x%016llX  cpu=0x%016llX) "
               "kernel=%.3f ms fold=%.3f ms probe=%.2f GB/s\n",
               wv[i].k, wv[i].lanes, wv[i].chained ? " chained" : "",
               pass ? "PASS" : "FAIL",
               (unsigned long long)got, (unsigned long long)want,
               kms, fms, mc::gpu::probe_gbps(wv[i].k, kms));
    }
    return failed;
}

int main(int argc, char** argv) {
    int device = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--device") && i + 1 < argc) device = atoi(argv[++i]);

    char name[256] = {0}, err[512] = {0};
    if (!mc::gpu::select_device(device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "GPU KAT: no usable device — %s\n", err);
        return 1;
    }
    printf("GPU KAT: device %d: %s\n\n", device, name);

    std::vector<uint64_t> host(kNumWords);
    mc::fill_random(host.data(), kNumWords, kSeed);

    int failed = 0;

    // --- 1. pinned vectors over an UPLOADED buffer (the protocol path) -------
    {
        mc::gpu::Prover p;
        // Start with a small lane cap on purpose so run_wide has to grow it.
        if (!mc::gpu::create(p, kNumWords, mc::gpu::Alloc::Device, 64, 1.0, 0.0, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: create failed — %s\n", err);
            return 1;
        }
        const char* backing = "?"; bool is_dev = false;
        mc::gpu::residency(p, &backing, &is_dev);
        printf("GPU KAT: backing = %s\n", backing);
        if (!is_dev) { fprintf(stderr, "GPU KAT: buffer is not device-resident\n"); ++failed; }

        size_t cap = 0;
        unsigned char* stage = mc::gpu::staging(p, &cap);
        const unsigned char* src = (const unsigned char*)host.data();
        size_t left = kBytes;
        while (left) {
            size_t chunk = left < cap ? left : cap;
            memcpy(stage, src, chunk);
            if (!mc::gpu::push(p, chunk, err, sizeof err)) {
                fprintf(stderr, "GPU KAT: push failed — %s\n", err);
                return 1;
            }
            src += chunk; left -= chunk;
        }
        printf("\n=== GPU KAT: pinned vectors, uploaded buffer ===\n");
        failed += run_vectors(p, "uploaded");

        printf("\n=== GPU KAT: wide cases vs CPU reference ===\n");
        failed += run_wide(p, host.data(), kNumWords);
        mc::gpu::destroy(p);
    }

    // --- 2. pinned vectors over an ON-DEVICE generated buffer ---------------
    // Also proves k_fill is bit-identical to fill_random(): same constants.
    {
        mc::gpu::Prover p;
        if (!mc::gpu::create(p, kNumWords, mc::gpu::Alloc::Device, 64, 1.0, 0.0, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: create failed — %s\n", err);
            return 1;
        }
        if (!mc::gpu::fill_splitmix64(p, kSeed, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: fill failed — %s\n", err);
            return 1;
        }
        printf("\n=== GPU KAT: pinned vectors, buffer generated on-device ===\n");
        failed += run_vectors(p, "on-device fill");
        mc::gpu::destroy(p);
    }

    printf("\n");
    if (failed == 0) {
        printf("GPU KAT: all vectors PASS — the CUDA kernel computes the frozen math\n");
        return 0;
    }
    fprintf(stderr, "GPU KAT: %d check(s) FAILED\n", failed);
    return 1;
}
