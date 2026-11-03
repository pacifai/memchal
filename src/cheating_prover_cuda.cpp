// cheating_prover_cuda.cpp — the GPU adversaries. Each claims locked=true,
// because a verifier has no way to check the claim; the timing detector has to
// catch them.
//
//   fake      — holds --frac of the buffer on the card and invents the rest.
//   uvm       — cudaMallocManaged. Pages migrate between host and card on fault.
//   partial   — keeps --offload of the buffer in pinned host memory, read over
//               PCIe. At 1.0 the whole buffer is in host memory.
//   recompute — build mode only. Keeps --keep of the Argon2d blocks on the
//               card and rebuilds the rest on the host when a probe lands on
//               one (ADR 0007).
//
// --leave-free reserves device memory until that much is free, so a managed
// buffer larger than what is left has to page.
#include "challenge.hpp"
#include "cli.hpp"
#include "gpu_adversary.hpp"
#include "gpu_server.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <sched.h>
#include <thread>
#include <vector>
#include "argon2d_rebuild.hpp"

using namespace mc;

static_assert(gpu::kRecomputeBlockWords == a2::kBlockWords,
              "the CUDA loader and Argon2d disagree on the block size");

enum Mode { FAKE, UVM, PARTIAL, RECOMPUTE };
static const char* const kModeNames[] = { "fake", "uvm", "partial", "recompute", nullptr };

static const char kUsage[] =
    "usage: cheating_prover_cuda [--flag value]...\n"
    "  --port N              listen port (default %u)\n"
    "  --mode MODE           fake, uvm, partial, or recompute (default uvm)\n"
    "  --frac F              fake: fraction held (default 0.5)\n"
    "  --offload F           partial: fraction kept in host memory (default 0.1)\n"
    "  --leave-free MIB      reserve device memory until this much is free\n"
    "  --device N --lane-cap N --trace N   as for prover_cuda\n"
    "  --keep F              recompute: fraction of blocks kept (default 0.99)\n"
    "  --rebuild-budget N    recompute: block fills per thread per challenge (default 65536)\n"
    "  --rebuild-threads N   recompute: rebuild threads (default: every available core)\n"
    "  --keep-seed N         recompute: which blocks to keep (default 1)\n";

static Mode     g_mode        = UVM;
static double   g_frac        = 0.5;
static double   g_offload     = 0.1;
static long long g_leave_free_mib = -1;
static int      g_device      = 0;
static uint32_t g_lane_cap    = 1u << 17;
static uint32_t g_trace_every = 0;
static double   g_keep        = 0.99;
static uint64_t g_budget      = 1u << 16;
static uint32_t g_threads     = 0;
static uint64_t g_keep_seed   = 1;

// Workers that live across rounds. run() hands items to whichever worker is
// free, with the calling thread as worker 0, and returns when all are done.
class Pool {
public:
    explicit Pool(uint32_t workers) {
        for (uint32_t w = 1; w < workers; ++w) threads_.emplace_back([this, w] { loop(w); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> g(m_); stop_ = true; }
        start_.notify_all();
        for (auto& t : threads_) t.join();
    }
    uint32_t size() const { return (uint32_t)threads_.size() + 1; }

    void run(uint32_t items, const std::function<void(uint32_t, uint32_t)>& job) {
        if (items == 0) return;
        if (threads_.empty() || items == 1) {
            for (uint32_t i = 0; i < items; ++i) job(0, i);
            return;
        }
        {
            std::lock_guard<std::mutex> g(m_);
            job_ = &job; items_ = items; next_ = 0;
            busy_ = (uint32_t)threads_.size();
            ++generation_;
        }
        start_.notify_all();
        work(0);
        std::unique_lock<std::mutex> g(m_);
        done_.wait(g, [this] { return busy_ == 0; });
        job_ = nullptr;
    }

private:
    void work(uint32_t w) {
        for (uint32_t i; (i = next_.fetch_add(1)) < items_;) (*job_)(w, i);
    }
    void loop(uint32_t w) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> g(m_);
                start_.wait(g, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            work(w);
            std::lock_guard<std::mutex> g(m_);
            if (--busy_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable start_, done_;
    const std::function<void(uint32_t, uint32_t)>* job_ = nullptr;
    std::atomic<uint32_t> next_{0};
    uint32_t items_ = 0, busy_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

// The recompute cheat's state between challenges: a host copy of the kept
// blocks, and one Rebuilder with its own memo per worker.
struct RecomputeCtx {
    MatrixShape shape;
    std::vector<int32_t>  slot;
    std::vector<uint64_t> kept;
    std::vector<std::unique_ptr<Rebuilder>> rb;
    std::unique_ptr<Pool> pool;
    uint64_t fills_before = 0, gave_up_before = 0;   // totals at the challenge's start
};

static bool rebuild_blocks(void* vctx, const uint32_t* ids, uint32_t n, uint64_t* out) {
    RecomputeCtx& c = *(RecomputeCtx*)vctx;
    c.pool->run(n, [&](uint32_t w, uint32_t i) {
        uint64_t* dst = out + (size_t)i * a2::kBlockWords;
        if (!c.rb[w]->rebuild(ids[i], dst)) memset(dst, 0, a2::kBlockBytes);
    });
    return true;
}

static uint64_t ctx_total(const RecomputeCtx& c, uint64_t Rebuilder::Stats::*field) {
    uint64_t t = 0;
    for (const auto& r : c.rb) t += r->stats().*field;
    return t;
}

// Picks the kept blocks out of a freshly built matrix and builds the rebuild
// state. Afterwards the full matrix can be freed.
static bool recompute_setup(RecomputeCtx& c, const Setup& s, const uint64_t* built,
                            char* err, size_t errlen) {
    if (!matrix_shape(s.recipe, c.shape)) {
        snprintf(err, errlen, "recipe has no valid Argon2d shape");
        return false;
    }
    std::mt19937_64 rng(g_keep_seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    c.slot.assign(c.shape.blocks, -1);
    uint32_t n_kept = 0;
    for (uint32_t b = 0; b < c.shape.blocks; ++b)
        if (u(rng) < g_keep) c.slot[b] = (int32_t)n_kept++;
    c.kept.resize((size_t)n_kept * a2::kBlockWords);
    for (uint32_t b = 0; b < c.shape.blocks; ++b)
        if (c.slot[b] >= 0)
            memcpy(c.kept.data() + (size_t)c.slot[b] * a2::kBlockWords,
                   built + (size_t)b * a2::kBlockWords, a2::kBlockBytes);

    uint32_t threads = g_threads;
    if (threads == 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        threads = sched_getaffinity(0, sizeof set, &set) == 0 ? (uint32_t)CPU_COUNT(&set)
                                                              : std::thread::hardware_concurrency();
    }
    if (threads == 0) threads = 1;
    c.pool.reset(new Pool(threads));
    for (uint32_t w = 0; w < c.pool->size(); ++w)
        c.rb.emplace_back(new Rebuilder(c.shape, KeptBlocks{c.slot.data(), c.kept.data()},
                                        g_budget));
    return true;
}

// The recompute cheat's challenge loop. Prints one '@' line per challenge,
// after the response is sent, and a summary when the verifier hangs up.
static void serve_recompute(int cfd, gpu::Adversary& p, RecomputeCtx& rc) {
    char err[512] = {0};
    uint64_t rebuilt_total = 0, rounds_total = 0, gave_up_challenges = 0;
    double rebuild_ms_total = 0, kernel_ms_total = 0;
    gpu::Timing t;
    gpu::RecomputeStats st;
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer_recompute(p, ch.nonce, ch.k, ch.lanes, is_chained(ch), is_tree(ch),
                                      rebuild_blocks, &rc, hash, &t, &st, err, sizeof err))
                return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            const uint64_t fills   = ctx_total(rc, &Rebuilder::Stats::fills);
            const uint64_t gave_up = ctx_total(rc, &Rebuilder::Stats::gave_up);
            printf("@{\"t\":\"rc\",\"served\":%llu,\"k\":%u,\"lanes\":%u,\"rounds\":%u,"
                   "\"rebuilt\":%u,\"fills\":%llu,\"gave_up\":%llu,\"rebuild_ms\":%.4f,"
                   "\"kernel_ms\":%.4f,\"copy_ms\":%.4f,\"fold_ms\":%.4f}\n",
                   (unsigned long long)n, ch.k, ch.lanes, st.rounds, st.rebuilt,
                   (unsigned long long)(fills - rc.fills_before),
                   (unsigned long long)(gave_up - rc.gave_up_before),
                   st.rebuild_ms, t.kernel_ms, t.copy_ms, t.fold_ms);
            if (gave_up > rc.gave_up_before) ++gave_up_challenges;
            rc.fills_before = fills;
            rc.gave_up_before = gave_up;
            rebuilt_total += st.rebuilt;
            rounds_total += st.rounds;
            rebuild_ms_total += st.rebuild_ms;
            kernel_ms_total += t.kernel_ms;
            for (auto& r : rc.rb) r->begin_challenge();
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
           (unsigned long long)ctx_total(rc, &Rebuilder::Stats::fills),
           (unsigned long long)ctx_total(rc, &Rebuilder::Stats::seeds),
           (unsigned long long)ctx_total(rc, &Rebuilder::Stats::gave_up),
           (unsigned long long)gave_up_challenges,
           ctx_total(rc, &Rebuilder::Stats::memo_hi) * a2::kBlockBytes / 1048576.0);
}

// Builds the matrix, keeps --keep of its blocks on the card, and answers.
static void serve_recompute_mode(int cfd, const Setup& s) {
    char err[512] = {0};
    Source src{cfd};
    RecomputeCtx rc;
    if (!build_on_host(src, s, err, sizeof err) || !recompute_setup(rc, s, src.built, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        free_built(src);
        return;
    }
    free_built(src);
    gpu::Adversary p;
    const uint32_t n_kept = (uint32_t)(rc.kept.size() / a2::kBlockWords);
    if (!gpu::create_recompute(p, rc.shape.blocks, rc.slot.data(), rc.kept.data(), n_kept,
                               g_lane_cap, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return;
    }
    size_t hf = 0, ht = 0;
    gpu::mem_info(&hf, &ht);
    printf("[cheat] built in %.3f s (m=%u KiB t=%u p=%u); kept %u of %u blocks (%.2f%%) "
           "on the card, %u rebuild threads, budget %llu fills per thread per challenge\n",
           src.build_s, s.recipe.m_kib, s.recipe.t, s.recipe.p, n_kept, rc.shape.blocks,
           100.0 * n_kept / rc.shape.blocks, rc.pool->size(), (unsigned long long)g_budget);
    printf("@{\"t\":\"cheatinfo\",\"mode\":\"recompute\",\"keep\":%.6f,"
           "\"kept_blocks\":%u,\"blocks\":%u,\"build_t\":%u,\"build_p\":%u,"
           "\"threads\":%u,\"budget\":%llu,\"hbm_used\":%llu,\"hbm_total\":%llu,"
           "\"build\":true,\"build_s\":%.4f}\n",
           g_keep, n_kept, rc.shape.blocks, s.recipe.t, s.recipe.p, rc.pool->size(),
           (unsigned long long)g_budget, (unsigned long long)(ht - hf),
           (unsigned long long)ht, src.build_s);
    if (send_ready(cfd, true)) serve_recompute(cfd, p, rc);
    gpu::destroy(p);
}

static bool create_for_mode(gpu::Adversary& p, uint64_t num_words, char* err, size_t errlen) {
    switch (g_mode) {
        case FAKE:    return gpu::create_fake(p, num_words, g_frac, g_lane_cap, err, errlen);
        case UVM:     return gpu::create_managed(p, num_words, g_lane_cap, err, errlen);
        case PARTIAL: return gpu::create_partial(p, num_words, g_offload, g_lane_cap, err, errlen);
        case RECOMPUTE: break;
    }
    snprintf(err, errlen, "recompute has its own setup");
    return false;
}

// fake, uvm, and partial: fills the buffer the way the honest prover does.
static void serve_upload_mode(int cfd, const Setup& s) {
    char err[512] = {0};
    gpu::Adversary p;
    if (!create_for_mode(p, s.num_words, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return;
    }
    Source src{cfd};
    bool ok = !s.build || build_on_host(src, s, err, sizeof err);
    if (ok && s.build)
        printf("[cheat] built on the host in %.3f s (m=%u KiB t=%u p=%u); copying to the card\n",
               src.build_s, s.recipe.m_kib, s.recipe.t, s.recipe.p);
    ok = ok && ingest(p, src, s.bytes, err, sizeof err);
    free_built(src);
    if (!ok) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        gpu::destroy(p);
        return;
    }

    bool is_device = false;
    const char* backing = gpu::residency(p, &is_device);
    const double resident = gpu::resident_fraction(p);
    size_t hf = 0, ht = 0;
    gpu::mem_info(&hf, &ht);
    printf("[cheat] backing = %s (claiming locked=true regardless)\n", backing);
    printf("[cheat] hbm used=%.2f GiB of %.2f GiB\n",
           (double)(ht - hf) / 1073741824.0, (double)ht / 1073741824.0);
    printf("@{\"t\":\"cheatinfo\",\"mode\":");
    json_str(stdout, kModeNames[g_mode]);
    printf(",\"backing\":");
    json_str(stdout, backing);
    printf(",\"device_backed\":%s,\"resident_frac\":%.4f,\"hbm_used\":%llu,\"hbm_total\":%llu,"
           "\"build\":%s,\"build_s\":%.4f}\n",
           is_device ? "true" : "false", resident,
           (unsigned long long)(ht - hf), (unsigned long long)ht,
           s.build ? "true" : "false", src.build_s);

    if (!send_ready(cfd, true)) { gpu::destroy(p); return; }

    gpu::Timing t;
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer(p, ch.nonce, ch.k, ch.lanes, is_chained(ch), is_tree(ch),
                            hash, &t, err, sizeof err))
                return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            if (g_trace_every && n % g_trace_every == 0)
                print_trace(kModeNames[g_mode], ch, n, t, resident, is_device);
            return true;
        });
    printf("[cheat] served %llu challenges; releasing\n", (unsigned long long)served);
    gpu::destroy(p);
}

static void serve(int cfd) {
    Setup s;
    char err[512] = {0};
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[cheat] refusing: %s\n", err); return; }
    if (g_mode == RECOMPUTE && !s.build) {
        fprintf(stderr, "[cheat] recompute needs a build-mode verifier (--gen argon2d): "
                        "an uploaded buffer comes with no recipe to rebuild from\n");
        return;
    }
    printf("[cheat] mode=%s %s %.2f MiB (%llu words)\n", kModeNames[g_mode],
           s.build ? "building" : "receiving", s.bytes / 1048576.0, (unsigned long long)s.num_words);

    if (g_leave_free_mib >= 0) {
        size_t free_b = 0, total_b = 0;
        gpu::mem_info(&free_b, &total_b);
        const size_t free_mib = free_b / 1048576ull, leave = (size_t)g_leave_free_mib;
        const size_t reserve_mib = free_mib > leave ? free_mib - leave : 0;
        printf("[cheat] %zu MiB free on the card; reserving %zu MiB to leave %zu MiB free\n",
               free_mib, reserve_mib, free_mib - reserve_mib);
        if (!gpu::reserve_hbm(reserve_mib * 1048576ull, err, sizeof err)) {
            fprintf(stderr, "[cheat] FATAL: could not reserve %zu MiB: %s\n", reserve_mib, err);
            return;
        }
    }
    if (g_mode == RECOMPUTE) serve_recompute_mode(cfd, s);
    else                     serve_upload_mode(cfd, s);
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
        else if (!strcmp(a, "--keep"))            g_keep = cli.number(a, v, 0.0, 1.0);
        else if (!strcmp(a, "--rebuild-budget"))  g_budget = cli.uint(a, v);
        else if (!strcmp(a, "--rebuild-threads")) g_threads = cli.u32(a, v);
        else if (!strcmp(a, "--keep-seed"))       g_keep_seed = cli.uint(a, v);
        else cli.error("unknown flag: %s", a);
    }

    char name[256] = {0}, err[512] = {0};
    if (!gpu::select_device(g_device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return 1;
    }
    printf("[cheat] device %d: %s; mode=%s\n", g_device, name, kModeNames[g_mode]);
    print_gpuinfo(g_device, name);
    return serve_forever("cheat", port, serve);
}
