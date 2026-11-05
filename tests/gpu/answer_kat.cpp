#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gpu/answer.hpp"
#include "gpu/upload.hpp"
#include "shared/challenge.hpp"
#include "shared/fill.hpp"

static const uint64_t kBytes    = 1ULL * 1024 * 1024;
static const uint64_t kNumWords = kBytes / 8;
static const uint64_t kSeed     = 0xABCDEF0123456789ULL;
static const uint32_t kLaneCap  = 16384;

struct Vec { uint64_t nonce; uint32_t samples; uint32_t lanes; };
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
static const uint64_t kExpectedTree[3] = {
    0xDB349A095B054DC1ULL,
    0x3D7231F696115A72ULL,
    0x75D36F59F81165F0ULL,
};
static const uint64_t kExpectedTreeChained[3] = {
    0x9223B322843E6ED1ULL,
    0x6CB30B91F44FF207ULL,
    0x86106229AE2DDC5BULL,
};

static int run_vectors(mc::gpu::Prover& p, const char* how) {
    int failed = 0;
    char err[512] = {0};

    for (int tree = 0; tree <= 1; ++tree)
    for (int chained = 0; chained <= 1; ++chained) {
        const uint64_t* expect = tree ? (chained ? kExpectedTreeChained : kExpectedTree)
                                      : (chained ? kExpectedChained : kExpected);
        const char* fold = tree ? "tree " : "";
        for (int i = 0; i < 3; ++i) {
            uint64_t got = 0;
            mc::gpu::Timing t;
            if (!mc::gpu::answer(p, kVecs[i].nonce, kVecs[i].samples, kVecs[i].lanes,
                                 chained != 0, tree != 0, &got, &t, err, sizeof err)) {
                fprintf(stderr, "GPU KAT (%s) %s%svector %d: ERROR %s\n",
                        how, fold, chained ? "chained " : "", i + 1, err);
                ++failed;
                continue;
            }
            bool pass = (got == expect[i]);
            if (!pass) ++failed;
            printf("GPU KAT (%s) %s%svector %d: %s  (got=0x%016llX  expected=0x%016llX)\n",
                   how, fold, chained ? "chained " : "", i + 1,
                   pass ? "PASS" : "FAIL",
                   (unsigned long long)got, (unsigned long long)expect[i]);
        }
    }
    return failed;
}

// Compares the GPU with the CPU on lane counts that span many thread blocks.
static int run_wide(mc::gpu::Prover& p, const uint64_t* host_buf, uint64_t num_words) {
    struct WVec { uint64_t nonce; uint32_t samples; uint32_t lanes; bool chained; };
    static const WVec wv[4] = {
        { 0xA5A5A5A5A5A5A5A5ULL, 1u << 17, 1024,  false },
        { 0xA5A5A5A5A5A5A5A5ULL, 1u << 17, 1024,  true  },
        { 0x0123456789ABCDEFULL, 1u << 18, 16384, false },
        { 0x0123456789ABCDEFULL, 1u << 18, 16384, true  },
    };

    int failed = 0;
    char err[512] = {0};
    std::vector<uint64_t> scratch;
    for (int tree = 0; tree <= 1; ++tree)
    for (int i = 0; i < 4; ++i) {
        uint64_t want;
        if (tree) {
            scratch.resize(wv[i].lanes);
            want = mc::answer_tree(mc::BufferLoad{host_buf}, num_words, wv[i].nonce, wv[i].samples,
                                   wv[i].lanes, wv[i].chained, scratch.data());
        } else {
            want = mc::answer_sequential(mc::BufferLoad{host_buf}, num_words, wv[i].nonce,
                                         wv[i].samples, wv[i].lanes, wv[i].chained);
        }
        uint64_t got = 0;
        mc::gpu::Timing t;
        if (!mc::gpu::answer(p, wv[i].nonce, wv[i].samples, wv[i].lanes, wv[i].chained,
                             tree != 0, &got, &t, err, sizeof err)) {
            fprintf(stderr, "GPU KAT wide %d%s: ERROR %s\n", i + 1, tree ? " tree" : "", err);
            ++failed;
            continue;
        }
        bool pass = (got == want);
        if (!pass) ++failed;
        printf("GPU KAT wide samples=%u lanes=%u%s%s: %s  (gpu=0x%016llX  cpu=0x%016llX) "
               "kernel=%.3f ms fold=%.3f ms probe=%.2f GB/s\n",
               wv[i].samples, wv[i].lanes, wv[i].chained ? " chained" : "", tree ? " tree" : "",
               pass ? "PASS" : "FAIL",
               (unsigned long long)got, (unsigned long long)want,
               t.kernel_ms, t.fold_ms, mc::gpu::probe_gbps(wv[i].samples, t.kernel_ms));
    }
    return failed;
}

int main(int argc, char** argv) {
    int device = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--device") && i + 1 < argc) device = atoi(argv[++i]);

    char name[256] = {0}, err[512] = {0};
    if (!mc::gpu::select_device(device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "GPU KAT: no usable device: %s\n", err);
        return 1;
    }
    printf("GPU KAT: device %d: %s\n\n", device, name);

    std::vector<uint64_t> host(kNumWords);
    mc::fill_splitmix64(host.data(), kNumWords, kSeed);

    int failed = 0;

    {
        mc::gpu::Prover p;
        if (!mc::gpu::create(p, kNumWords, kLaneCap, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: create failed: %s\n", err);
            return 1;
        }
        bool is_dev = false;
        const char* backing = mc::gpu::residency(p, &is_dev);
        printf("GPU KAT: backing = %s\n", backing);
        if (!is_dev) { fprintf(stderr, "GPU KAT: buffer is not device-resident\n"); ++failed; }

        mc::Source src;
        src.built = host.data();
        if (!mc::upload(p, src, kBytes, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: upload failed: %s\n", err);
            return 1;
        }
        printf("\n=== GPU KAT: pinned vectors, uploaded buffer ===\n");
        failed += run_vectors(p, "uploaded");

        printf("\n=== GPU KAT: wide cases vs CPU reference ===\n");
        failed += run_wide(p, host.data(), kNumWords);
        mc::gpu::destroy(p);
    }

    {
        mc::gpu::Prover p;
        if (!mc::gpu::create(p, kNumWords, kLaneCap, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: create failed: %s\n", err);
            return 1;
        }
        if (!mc::gpu::fill_splitmix64(p, kSeed, err, sizeof err)) {
            fprintf(stderr, "GPU KAT: fill failed: %s\n", err);
            return 1;
        }
        printf("\n=== GPU KAT: pinned vectors, buffer generated on-device ===\n");
        failed += run_vectors(p, "on-device fill");
        mc::gpu::destroy(p);
    }

    printf("\n");
    if (failed == 0) {
        printf("GPU KAT: all vectors PASS\n");
        return 0;
    }
    fprintf(stderr, "GPU KAT: %d check(s) FAILED\n", failed);
    return 1;
}
