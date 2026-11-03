// prover_cuda.cpp — the honest GPU prover. It holds the buffer in cudaMalloc
// memory and refuses to run if the driver reports any other backing, the GPU
// counterpart of prover.cpp's lock check (ADR 0002).
//
// The kernel lives in gpu_answer.cu. This file has no CUDA headers, and that
// one has no POSIX ones.
#include "challenge.hpp"
#include "cli.hpp"
#include "gpu_answer.hpp"
#include "gpu_ingest.hpp"
#include "server.hpp"

using namespace mc;

static const char kUsage[] =
    "usage: prover_cuda [--flag value]...\n"
    "  --port N           listen port (default %u)\n"
    "  --device N         CUDA device (default 0)\n"
    "  --lane-cap N       most lanes a challenge may use (default 131072)\n"
    "  --trace N          print a timing event every N challenges\n"
    "  --allow-unlocked   run even if the buffer is not in device memory\n";

static bool     g_allow_unlocked = false;
static int      g_device         = 0;
static uint32_t g_lane_cap       = 1u << 17;
static uint32_t g_trace_every    = 0;

static void report_hbm(const char* when) {
    size_t f = 0, t = 0;
    gpu::mem_info(&f, &t);
    if (!t) return;
    printf("[gpu] hbm %s: used=%.2f GiB free=%.2f GiB total=%.2f GiB\n",
           when, (double)(t - f) / 1073741824.0, (double)f / 1073741824.0,
           (double)t / 1073741824.0);
}

static void serve(int cfd) {
    Setup s;
    char err[512] = {0};
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[gpu] refusing: %s\n", err); return; }
    printf("[gpu] %s %.2f MiB (%llu words)\n", s.build ? "building" : "receiving",
           s.bytes / 1048576.0, (unsigned long long)s.num_words);

    // The device buffer comes first, so a card that cannot hold it fails
    // before a long build.
    gpu::Prover p;
    if (!gpu::create(p, s.num_words, gpu::Alloc::Device, g_lane_cap, 1.0, 0.0, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return;
    }
    Source src{cfd};
    bool ok = !s.build || build_on_host(src, s, err, sizeof err);
    if (ok && s.build)
        printf("[gpu] built on the host in %.3f s (m=%u KiB t=%u p=%u); copying to the card\n",
               src.build_s, s.recipe.m_kib, s.recipe.t, s.recipe.p);
    ok = ok && ingest(p, src, s.bytes, err, sizeof err);
    free_built(src);
    if (!ok) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        gpu::destroy(p);
        return;
    }

    const char* backing = "unknown";
    bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    printf("[gpu] buffer %s; backing = %s\n", s.build ? "built and copied" : "uploaded", backing);
    report_hbm("after upload");
    {
        size_t hf = 0, ht = 0;
        gpu::mem_info(&hf, &ht);
        printf("@{\"t\":\"proverinfo\",\"backing\":");
        json_str(stdout, backing);
        printf(",\"device_backed\":%s,\"build\":%s,\"build_s\":%.4f,"
               "\"hbm_used\":%llu,\"hbm_total\":%llu}\n",
               is_device ? "true" : "false", s.build ? "true" : "false", src.build_s,
               (unsigned long long)(ht - hf), (unsigned long long)ht);
    }

    if (!is_device) {
        if (!g_allow_unlocked) {
            fprintf(stderr, "[gpu] FATAL: buffer is not device-resident (%s); "
                            "pass --allow-unlocked to run anyway\n", backing);
            gpu::destroy(p);
            exit(1);
        }
        fprintf(stderr, "[gpu] WARNING: running on non-device memory; reporting locked=false\n");
    }
    if (!send_ready(cfd, is_device)) { gpu::destroy(p); return; }

    double kms = 0, cms = 0, fms = 0;
    const uint64_t served = answer_challenges(cfd, "gpu",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer(p, ch.nonce, ch.k, ch.lanes, is_chained(ch), is_tree(ch),
                            hash, &kms, &cms, &fms, err, sizeof err))
                return true;
            fprintf(stderr, "[gpu] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            if (!g_trace_every || n % g_trace_every) return true;
            size_t hf = 0, ht = 0;
            gpu::mem_info(&hf, &ht);
            printf("@{\"t\":\"g\",\"served\":%llu,\"k\":%u,\"lanes\":%u,"
                   "\"chained\":%s,\"tree\":%s,\"kernel_ms\":%.4f,\"copy_ms\":%.4f,"
                   "\"fold_ms\":%.4f,\"probe_gbps\":%.3f,\"hbm_used\":%llu,"
                   "\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s}\n",
                   (unsigned long long)n, ch.k, ch.lanes,
                   is_chained(ch) ? "true" : "false", is_tree(ch) ? "true" : "false",
                   kms, cms, fms, gpu::probe_gbps(ch.k, kms),
                   (unsigned long long)(ht - hf), (unsigned long long)ht,
                   gpu::resident_fraction(p), is_device ? "true" : "false");
            return true;
        });
    printf("[gpu] served %llu challenges; releasing buffer\n", (unsigned long long)served);
    gpu::destroy(p);
    report_hbm("after release");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const Cli cli{"gpu", kUsage, kDefaultPort};
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (!strcmp(a, "--help") || !strcmp(a, "-h")) cli.help();
        else if (!strcmp(a, "--allow-unlocked")) g_allow_unlocked = true;
        else if (!strcmp(a, "--port"))     port = cli.port(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--device"))   g_device = (int)cli.u32(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--lane-cap")) g_lane_cap = cli.u32(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--trace"))    g_trace_every = cli.u32(a, cli.value(argc, argv, &i));
        else cli.error("unknown flag: %s", a);
    }

    char name[256] = {0}, err[512] = {0};
    if (!gpu::select_device(g_device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return 1;
    }
    printf("[gpu] device %d: %s; lane cap %u\n", g_device, name, g_lane_cap);
    report_hbm("at start");
    {
        size_t hf = 0, ht = 0;
        gpu::mem_info(&hf, &ht);
        printf("@{\"t\":\"gpuinfo\",\"device\":%d,\"name\":", g_device);
        json_str(stdout, name);
        printf(",\"hbm_free\":%llu,\"hbm_total\":%llu}\n",
               (unsigned long long)hf, (unsigned long long)ht);
    }
    return serve_forever("gpu", port, serve);
}
