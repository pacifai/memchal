#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gpu/answer.hpp"
#include "gpu/upload.hpp"
#include "shared/challenge.hpp"
#include "shared/fill.hpp"
#include "../shared/answer_vectors.hpp"

using namespace mc::kat;

static const uint64_t kBytes    = 1ULL * 1024 * 1024;
static const uint64_t kNumWords = kBytes / 8;
static const uint32_t kLaneCap  = 16384;

static int run_vectors(mc::gpu::Prover& p, const char* how) {
    int failed = 0;
    char err[512] = {0};

    for (int tree = 0; tree <= 1; ++tree)
    for (int chained = 0; chained <= 1; ++chained) {
        const uint64_t* expect = expected(tree, chained);
        const char* fold = tree ? "tree " : "";
        for (int i = 0; i < kVectorCount; ++i) {
            uint64_t got = 0;
            mc::gpu::Timing t;
            if (!mc::gpu::answer(p, kVectors[i].nonce, kVectors[i].samples, kVectors[i].lanes,
                                 chained != 0, tree != 0, &got, &t, err, sizeof err)) {
                fprintf(stderr, "GPU known-answer test (%s) %s%svector %d: ERROR %s\n",
                        how, fold, chained ? "chained " : "", i + 1, err);
                ++failed;
                continue;
            }
            bool pass = (got == expect[i]);
            if (!pass) ++failed;
            printf("GPU known-answer test (%s) %s%svector %d: %s  (got=0x%016llX  expected=0x%016llX)\n",
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
            fprintf(stderr, "GPU known-answer test wide %d%s: ERROR %s\n", i + 1, tree ? " tree" : "", err);
            ++failed;
            continue;
        }
        bool pass = (got == want);
        if (!pass) ++failed;
        printf("GPU known-answer test wide samples=%u lanes=%u%s%s: %s  (gpu=0x%016llX  cpu=0x%016llX) "
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
        fprintf(stderr, "GPU known-answer test: no usable device: %s\n", err);
        return 1;
    }
    printf("GPU known-answer test: device %d: %s\n\n", device, name);

    std::vector<uint64_t> host(kNumWords);
    mc::fill_splitmix64(host.data(), kNumWords, kVectorSeed);

    int failed = 0;

    {
        mc::gpu::Prover p;
        if (!mc::gpu::create(p, kNumWords, kLaneCap, err, sizeof err)) {
            fprintf(stderr, "GPU known-answer test: create failed: %s\n", err);
            return 1;
        }
        const mc::gpu::Residency where = mc::gpu::residency(p);
        printf("GPU known-answer test: backing = %s\n", where.backing);
        if (!where.on_device) { fprintf(stderr, "GPU known-answer test: buffer is not device-resident\n"); ++failed; }

        mc::Source src;
        src.built = host.data();
        if (!mc::upload(p, src, kBytes, err, sizeof err)) {
            fprintf(stderr, "GPU known-answer test: upload failed: %s\n", err);
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
            fprintf(stderr, "GPU known-answer test: create failed: %s\n", err);
            return 1;
        }
        if (!mc::gpu::fill_splitmix64(p, kVectorSeed, err, sizeof err)) {
            fprintf(stderr, "GPU known-answer test: fill failed: %s\n", err);
            return 1;
        }
        printf("\n=== GPU KAT: pinned vectors, buffer generated on-device ===\n");
        failed += run_vectors(p, "on-device fill");
        mc::gpu::destroy(p);
    }

    printf("\n");
    if (failed == 0) {
        printf("GPU known-answer test: all vectors PASS\n");
        return 0;
    }
    fprintf(stderr, "GPU known-answer test: %d check(s) FAILED\n", failed);
    return 1;
}
