#include <cstring>
#include "demo/gpu_trace.hpp"
#include "gpu/adversary.hpp"
#include "gpu/recompute.hpp"
#include "gpu/upload.hpp"
#include "shared/cli.hpp"
#include "shared/server.hpp"

using namespace mc;

enum Mode { FAKE, UVM, PARTIAL, RECOMPUTE };
static const char* const kModeNames[] = { "fake", "uvm", "partial", "recompute", nullptr };

static const char kUsage[] =
    "usage: gpu/cheating_prover [--flag value]...\n"
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

// Answers challenges with the recompute cheat and prints its rebuild work.
static void answer_recompute(int cfd, gpu::Adversary& p, RecomputeHost& h) {
    char err[512] = {0};
    uint64_t rebuilt_total = 0, rounds_total = 0, gave_up_challenges = 0;
    double rebuild_ms_total = 0, kernel_ms_total = 0;
    gpu::Timing t;
    gpu::RecomputeStats st;
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer_recompute(p, ch.nonce, ch.samples, ch.lanes, is_chained(ch), is_tree(ch),
                                      rebuild_blocks, &h, hash, &t, &st, err, sizeof err))
                return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            const uint64_t fills   = h.total(&Rebuilder::Stats::fills);
            const uint64_t gave_up = h.total(&Rebuilder::Stats::gave_up);
            printf("@{\"t\":\"rc\",\"served\":%llu,\"samples\":%u,\"lanes\":%u,\"rounds\":%u,"
                   "\"rebuilt\":%u,\"fills\":%llu,\"gave_up\":%llu,\"rebuild_ms\":%.4f,"
                   "\"kernel_ms\":%.4f,\"copy_ms\":%.4f,\"fold_ms\":%.4f}\n",
                   (unsigned long long)n, ch.samples, ch.lanes, st.rounds, st.rebuilt,
                   (unsigned long long)(fills - h.fills_before),
                   (unsigned long long)(gave_up - h.gave_up_before),
                   st.rebuild_ms, t.kernel_ms, t.copy_ms, t.fold_ms);
            if (gave_up > h.gave_up_before) ++gave_up_challenges;
            h.fills_before = fills;
            h.gave_up_before = gave_up;
            rebuilt_total += st.rebuilt;
            rounds_total += st.rounds;
            rebuild_ms_total += st.rebuild_ms;
            kernel_ms_total += t.kernel_ms;
            for (auto& r : h.rebuilders) r->begin_challenge();
            if (gpu::recompute_forget(p, err, sizeof err)) return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        });
    const double n = served ? (double)served : 1.0;
    printf("[cheat] served %llu challenges; %.1f blocks rebuilt, %.1f rounds, "
           "%.3f ms rebuilding per challenge; %llu challenge(s) gave up\n",
           (unsigned long long)served, rebuilt_total / n, rounds_total / n,
           rebuild_ms_total / n, (unsigned long long)gave_up_challenges);
    printf("@{\"t\":\"rc_summary\",\"served\":%llu,\"rebuilt_per_ch\":%.3f,"
           "\"rounds_per_ch\":%.3f,\"rebuild_ms_per_ch\":%.4f,\"kernel_ms_per_ch\":%.4f,"
           "\"fills\":%llu,\"seeds\":%llu,\"gave_up\":%llu,\"gave_up_challenges\":%llu,"
           "\"memo_peak_mib\":%.2f}\n",
           (unsigned long long)served, rebuilt_total / n, rounds_total / n,
           rebuild_ms_total / n, kernel_ms_total / n,
           (unsigned long long)h.total(&Rebuilder::Stats::fills),
           (unsigned long long)h.total(&Rebuilder::Stats::seeds),
           (unsigned long long)h.total(&Rebuilder::Stats::gave_up),
           (unsigned long long)gave_up_challenges,
           h.total(&Rebuilder::Stats::memo_peak) * a2::kBlockBytes / 1048576.0);
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
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    printf("[cheat] built in %.3f s; kept %u of %u blocks (%.2f%%) on the card, %u rebuild "
           "threads, budget %llu fills per thread per challenge\n",
           src.build_s, h.n_kept(), h.shape.blocks, 100.0 * h.n_kept() / h.shape.blocks,
           h.pool->size(), (unsigned long long)g_recompute.budget);
    printf("@{\"t\":\"cheatinfo\",\"mode\":\"recompute\",\"keep\":%.6f,"
           "\"kept_blocks\":%u,\"blocks\":%u,\"build_passes\":%u,\"build_parallelism\":%u,"
           "\"threads\":%u,\"budget\":%llu,\"hbm_used\":%llu,\"hbm_total\":%llu,"
           "\"build\":true,\"build_s\":%.4f}\n",
           g_recompute.keep, h.n_kept(), h.shape.blocks, s.recipe.passes, s.recipe.parallelism,
           h.pool->size(), (unsigned long long)g_recompute.budget,
           (unsigned long long)(total_b - free_b), (unsigned long long)total_b, src.build_s);
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
// prover does.
static void serve_upload(int cfd, const Setup& s) {
    char err[512] = {0};
    gpu::Adversary p;
    if (!create_for_mode(p, s.num_words, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return;
    }
    Source src{cfd};
    bool ok = !s.build || build_on_host(src, s, err, sizeof err);
    if (ok && s.build) printf("[cheat] built on the host in %.3f s; copying to the card\n", src.build_s);
    ok = ok && upload(p, src, s.bytes, err, sizeof err);
    free_built(src);
    if (!ok) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        gpu::destroy(p);
        return;
    }

    bool is_device = false;
    const char* backing = gpu::residency(p, &is_device);
    const double resident = gpu::resident_fraction(p);
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    printf("[cheat] backing = %s (claiming locked=true regardless)\n", backing);
    printf("[cheat] hbm used=%.2f GiB of %.2f GiB\n",
           (double)(total_b - free_b) / 1073741824.0, (double)total_b / 1073741824.0);
    demo::print_buffer_info("cheatinfo", kModeNames[g_mode], backing, is_device, resident,
                            s.build, src.build_s);
    if (!send_ready(cfd, true)) { gpu::destroy(p); return; }

    gpu::Timing t;
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer(p, ch.nonce, ch.samples, ch.lanes, is_chained(ch), is_tree(ch),
                            hash, &t, err, sizeof err))
                return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            if (g_trace_every && n % g_trace_every == 0)
                demo::print_timing(kModeNames[g_mode], ch, n, t, resident, is_device);
            return true;
        });
    printf("[cheat] served %llu challenges; releasing\n", (unsigned long long)served);
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
    demo::print_gpu_info(g_device, name);
    return serve_forever("cheat", port, serve);
}
