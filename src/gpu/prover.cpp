#include <cstring>
#include "record/gpu_trace.hpp"
#include "gpu/serve.hpp"
#include "shared/cli.hpp"

using namespace mc;

static const char kUsage[] =
    "usage: gpu/prover [--flag value]...\n"
    "  --port N           listen port (default %u)\n"
    "  --device N         CUDA device (default 0)\n"
    "  --lane-cap N       most lanes a challenge may use; wider ones end the session\n"
    "                     (default 131072)\n"
    "  --trace N          print a timing event every N challenges\n"
    "  --allow-unlocked   run even if the buffer is not in device memory\n";

static bool     g_allow_unlocked = false;
static int      g_device         = 0;
static uint32_t g_lane_cap       = 1u << 17;
static uint32_t g_trace_every    = 0;

static void serve(int cfd) {
    Setup s;
    char err[512] = {0};
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[gpu] refusing: %s\n", err); return; }
    printf("[gpu] %s %.2f MiB (%llu words)\n", s.build ? "building" : "receiving",
           s.bytes / 1048576.0, (unsigned long long)s.num_words);

    gpu::Prover p;
    if (!gpu::create(p, s.num_words, g_lane_cap, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return;
    }
    const gpu::Residency where = gpu::residency(p);
    if (!where.on_device) {
        if (!g_allow_unlocked) {
            fprintf(stderr, "[gpu] refusing: buffer is not device-resident (%s); "
                            "pass --allow-unlocked to run anyway\n", where.backing);
            gpu::destroy(p);
            return;
        }
        fprintf(stderr, "[gpu] WARNING: running on non-device memory; reporting locked=false\n");
    }
    gpu::serve_buffer(cfd, s, p, "gpu", {"proverinfo", "honest", where, 1.0, where.on_device},
                      g_trace_every);
    gpu::destroy(p);
    gpu::report_hbm("gpu", "after release");
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
    gpu::report_hbm("gpu", "at start");
    record::print_gpu_info(g_device, name);
    return serve_forever("gpu", port, serve);
}
