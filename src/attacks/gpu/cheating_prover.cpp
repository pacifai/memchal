#include <cstring>
#include "record/gpu_trace.hpp"
#include "attacks/gpu/adversary.hpp"
#include "attacks/gpu/recompute.hpp"
#include "attacks/gpu/recompute_trace.hpp"
#include "gpu/serve.hpp"
#include "shared/cli.hpp"

using namespace mc;

enum Mode { FAKE, UVM, PARTIAL, RECOMPUTE };
static const char* const kModeNames[] = { "fake", "uvm", "partial", "recompute", nullptr };

static const char kUsage[] =
    "usage: attacks/gpu/cheating_prover [--flag value]...\n"
    "  --port N              listen port (default %u)\n"
    "  --mode MODE           fake, uvm, partial, or recompute (default uvm)\n"
    "  --frac F              fake: fraction held (default 0.5)\n"
    "  --offload F           partial: fraction kept in host memory (default 0.1)\n"
    "  --leave-free MIB      reserve device memory until this much is free\n"
    "  --device N --lane-cap N --trace N   as for gpu/prover\n"
    "  --keep F              recompute: fraction of blocks kept (default 0.99)\n"
    "  --rebuild-budget N    recompute: block fills per thread per challenge (default 65536)\n"
    "  --rebuild-threads N   recompute: rebuild threads (default: every available core)\n"
    "  --keep-seed N         recompute: which blocks to keep (default 1)\n";

static Mode             g_mode           = UVM;
static double           g_frac           = 0.5;
static double           g_offload        = 0.1;
static long long        g_leave_free_mib = -1;
static int              g_device         = 0;
static uint32_t         g_lane_cap       = 1u << 17;
static uint32_t         g_trace_every    = 0;
static RecomputeOptions g_recompute;

// Answers challenges with the recompute cheat.
static void answer_recompute(int cfd, gpu::Adversary& p, RecomputeHost& h) {
    char err[512] = {0};
    gpu::Timing t;
    gpu::RecomputeStats st;
    RecomputeTrace trace(g_trace_every);
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer_recompute(p, ch.nonce, ch.samples, ch.lanes, is_chained(ch), is_tree(ch),
                                      rebuild_blocks, &h, hash, &t, &st, err, sizeof err))
                return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            trace.after(ch, n, st, t, h);
            for (auto& r : h.rebuilders) r->begin_challenge();
            if (gpu::recompute_forget(p, err, sizeof err)) return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        });
    trace.finish(served, h);
}

static void serve_recompute(int cfd, const Setup& s) {
    char err[512] = {0};
    Source src{cfd};
    RecomputeHost h;
    if (!build_on_host(src, s, err, sizeof err) ||
        !recompute_setup(h, s, src.built, g_recompute, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        free_built(src);
        return;
    }
    free_built(src);
    gpu::Adversary p;
    if (!gpu::create_recompute(p, h.shape.blocks, h.slot.data(), h.kept.data(), h.n_kept(),
                               g_lane_cap, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return;
    }
    printf("[cheat] built in %.3f s; kept %u of %u blocks (%.2f%%) on the card, %u rebuild "
           "threads, budget %llu fills per thread per challenge\n",
           src.build_s, h.n_kept(), h.shape.blocks, 100.0 * h.n_kept() / h.shape.blocks,
           h.pool->size(), (unsigned long long)g_recompute.budget);
    print_recompute_info(h, g_recompute, s, src.build_s);
    if (send_ready(cfd, true)) answer_recompute(cfd, p, h);
    gpu::destroy(p);
}

static bool create_for_mode(gpu::Adversary& p, uint64_t num_words, char* err, size_t errlen) {
    switch (g_mode) {
        case FAKE:      return gpu::create_fake(p, num_words, g_frac, g_lane_cap, err, errlen);
        case UVM:       return gpu::create_managed(p, num_words, g_lane_cap, err, errlen);
        case PARTIAL:   return gpu::create_partial(p, num_words, g_offload, g_lane_cap, err, errlen);
        case RECOMPUTE: break;
    }
    snprintf(err, errlen, "recompute has its own setup");
    return false;
}

// Serves fake, uvm, and partial, which receive the buffer the way the honest
// prover does and claim it is locked in device memory.
static void serve_upload(int cfd, const Setup& s) {
    char err[512] = {0};
    gpu::Adversary p;
    if (!create_for_mode(p, s.num_words, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return;
    }
    const gpu::Residency where = gpu::residency(p);
    printf("[cheat] backing = %s (claiming locked=true regardless)\n", where.backing);
    gpu::serve_buffer(cfd, s, p, "cheat",
                      {"cheatinfo", kModeNames[g_mode], where, gpu::resident_fraction(p), true},
                      g_trace_every);
    gpu::destroy(p);
}

// Reserves device memory until only --leave-free MiB remain free.
static bool leave_free() {
    if (g_leave_free_mib < 0) return true;
    char err[512] = {0};
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    const size_t free_mib = free_b / 1048576ull, leave = (size_t)g_leave_free_mib;
    const size_t reserve_mib = free_mib > leave ? free_mib - leave : 0;
    printf("[cheat] %zu MiB free on the card; reserving %zu MiB to leave %zu MiB free\n",
           free_mib, reserve_mib, free_mib - reserve_mib);
    if (gpu::reserve_hbm(reserve_mib * 1048576ull, err, sizeof err)) return true;
    fprintf(stderr, "[cheat] FATAL: could not reserve %zu MiB: %s\n", reserve_mib, err);
    return false;
}

static void serve(int cfd) {
    Setup s;
    char err[512] = {0};
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[cheat] refusing: %s\n", err); return; }
    if (g_mode == RECOMPUTE && !s.build) {
        fprintf(stderr, "[cheat] recompute needs a build-mode verifier (--gen argon2d)\n");
        return;
    }
    printf("[cheat] mode=%s %s %.2f MiB (%llu words)\n", kModeNames[g_mode],
           s.build ? "building" : "receiving", s.bytes / 1048576.0, (unsigned long long)s.num_words);
    if (!leave_free()) return;
    if (g_mode == RECOMPUTE) serve_recompute(cfd, s);
    else                     serve_upload(cfd, s);
    gpu::release_hbm();
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const Cli cli{"cheat", kUsage, kDefaultPort};
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) cli.help();
        const char* v = cli.value(argc, argv, &i);
        if      (!strcmp(a, "--port"))            port = cli.port(a, v);
        else if (!strcmp(a, "--mode"))            g_mode = (Mode)cli.choice(a, v, kModeNames);
        else if (!strcmp(a, "--frac"))            g_frac = cli.number(a, v, 0.0, 1.0);
        else if (!strcmp(a, "--offload"))         g_offload = cli.number(a, v, 0.0, 1.0);
        else if (!strcmp(a, "--leave-free"))      g_leave_free_mib = (long long)cli.uint(a, v);
        else if (!strcmp(a, "--device"))          g_device = (int)cli.u32(a, v);
        else if (!strcmp(a, "--lane-cap"))        g_lane_cap = cli.u32(a, v);
        else if (!strcmp(a, "--trace"))           g_trace_every = cli.u32(a, v);
        else if (!strcmp(a, "--keep"))            g_recompute.keep = cli.number(a, v, 0.0, 1.0);
        else if (!strcmp(a, "--rebuild-budget"))  g_recompute.budget = cli.uint(a, v);
        else if (!strcmp(a, "--rebuild-threads")) g_recompute.threads = cli.u32(a, v);
        else if (!strcmp(a, "--keep-seed"))       g_recompute.keep_seed = cli.uint(a, v);
        else cli.error("unknown flag: %s", a);
    }

    char name[256] = {0}, err[512] = {0};
    if (!gpu::select_device(g_device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return 1;
    }
    printf("[cheat] device %d: %s; mode=%s\n", g_device, name, kModeNames[g_mode]);
    record::print_gpu_info(g_device, name);
    return serve_forever("cheat", port, serve);
}
