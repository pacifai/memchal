// cheating_prover_cuda.cpp — the GPU adversaries.
//
// Mirrors cheating_prover.cpp's role on the CPU side: every attack is a --mode,
// so a defense is tested rather than asserted. Run the attack, watch what the
// detector does, and report it either way.
//
// Unlike the honest prover, this one ALWAYS claims locked=true. That is not an
// oversight, it is the threat model: nothing stops a prover saying it called
// cudaMalloc when it called cudaMallocManaged, and no verifier can check from
// outside. "Did you cudaMalloc?" is unanswerable remotely, which is precisely
// why the timing detector has to carry the weight.
//
//   honest    — cudaMalloc. The baseline the others are measured against, so
//               the same binary can produce it under the same conditions.
//   fake      — holds --frac of the buffer and invents the rest. Fast and
//               wrong: the correctness detector, not the timing one.
//   uvm       — cudaMallocManaged. Pages migrate host<->device on fault, so the
//               prover can "hold" more than the card has. The direct analog of
//               the CPU mmapdisk cheat, and #1 in the GPU adversary list.
//   zerocopy  — cudaHostAlloc(mapped). The buffer sits in pinned host DRAM and
//               every probe crosses PCIe.
//   partial   — --offload f keeps fraction f of the buffer in host memory and
//               the rest on the card. This is the one that sets the detector's
//               RESOLUTION. uvm and zerocopy ask "can you catch a prover that
//               holds none of it"; this asks "how little can a prover offload
//               before you stop seeing it", which is the number a decision rule
//               needs and the thing the first GPU session could not measure.
//   recompute — the partial-recompute cheat (ADR 0007), build mode only. Builds
//               the Argon2d buffer, keeps a random --keep fraction of its 1 KiB
//               blocks on the card, and discards the rest. A probe that lands on
//               a discarded block pauses its lane while the host rebuilds the
//               block from the recipe, recursing through whatever that block
//               depends on (argon2d_rebuild.hpp). Correct, and slow by however
//               much rebuilding costs: this is the attack the build-buffer
//               design introduces, and the one its memory-hardness has to beat.
//
// Two assumptions favor the recompute cheat, so its measured cost is a lower
// bound. The host rebuild reads kept blocks from a host copy rather than from
// the card, so it never waits on PCIe for an ancestor; HBM is what this cheat
// saves, and host RAM is not counted against it. And it keeps a random
// fraction, where a real one would study the dependency graph and keep the
// blocks that are costliest to rebuild.
//
// A note on uvm, because it is easy to misread the result. With a buffer that
// fits in free HBM, UVM migrates everything to the card once and then answers
// at honest speed — it really is resident, so that is a true negative, not a
// failure of the detector. It is the same shape as the CPU finding that the
// mmap cheat looks honest while the page cache holds its file. To see it
// collapse you have to take the memory away: --hog <MiB> holds device memory so
// the managed buffer cannot fit, which is the GPU analog of the CPU demo's
// container RAM budget.
//
// --leave-free <MiB> is the same lever stated the way the experiment is: reserve
// whatever it takes to leave that much free. The prover measures free memory
// itself, at the moment it reserves, because nothing outside the process can
// know it — the CUDA context and allocator already hold a few hundred MiB that
// cudaMemGetInfo's total still counts. A caller that computes a reserve from the
// card's total asks for memory that does not exist and the reserve fails.
#include "challenge.hpp"
#include "cli.hpp"
#include "gpu_answer.hpp"
#include "gpu_ingest.hpp"
#include "server.hpp"
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

enum Mode { HONEST, FAKE, UVM, ZEROCOPY, PARTIAL, RECOMPUTE };
static const char* const kModeNames[] = {
    "honest", "fake", "uvm", "zerocopy", "partial", "recompute", nullptr };

static const char kUsage[] =
    "usage: cheating_prover_cuda [--flag value]...\n"
    "  --port N              listen port (default %u)\n"
    "  --mode MODE           honest, fake, uvm, zerocopy, partial, or recompute (default uvm)\n"
    "  --frac F              fake: fraction held (default 0.5)\n"
    "  --offload F           partial: fraction kept in host memory (default 0.1)\n"
    "  --hog MIB             device memory to hold unused\n"
    "  --leave-free MIB      hold device memory until this much is free\n"
    "  --device N --lane-cap N --trace N   as for prover_cuda\n"
    "  --keep F              recompute: fraction of blocks kept (default 0.99)\n"
    "  --rebuild-budget N    recompute: block fills per thread per challenge (default 65536)\n"
    "  --rebuild-threads N   recompute: rebuild threads (default: every available core)\n"
    "  --keep-seed N         recompute: which blocks to keep (default 1)\n";

static Mode     g_mode        = UVM;
static double   g_frac        = 0.5;    // fake: fraction actually held
static double   g_offload     = 0.1;    // partial: fraction kept off the card
static size_t   g_hog_mib     = 0;      // device memory to occupy
static long long g_leave_free_mib = -1;  // if >= 0, reserve all but this much
static int      g_device      = 0;
static uint32_t g_lane_cap    = 1u << 17;
static uint32_t g_trace_every = 0;
static double   g_keep        = 0.99;   // recompute: fraction of blocks kept
static uint64_t g_budget      = 1u << 16; // recompute: fill_block() calls per thread per challenge
static uint32_t g_threads     = 0;      // recompute: rebuild threads, 0 = every core
static uint64_t g_keep_seed   = 1;      // recompute: which blocks to keep

// Workers that stay alive across rounds, so a round of rebuilds does not pay
// for thread creation. run() hands out items one at a time to whichever worker
// is free, the calling thread included as worker 0, and returns when all are
// done.
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

// What the recompute cheat keeps between challenges: the kept blocks in host
// memory for the rebuild to read, and one Rebuilder per worker, each with its
// own memo. Separate memos can rebuild the same ancestor twice; sharing one
// would need a lock on every lookup.
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
        // Out of budget: hand back zeros. The answer will be wrong, which is
        // what a cheat that ran out of time would send.
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

    // Default to the CPUs this process may run on, which in a container can be
    // far fewer than hardware_concurrency() reports.
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

static gpu::Alloc alloc_for(Mode m) {
    switch (m) {
        case UVM:      return gpu::Alloc::Managed;
        case ZEROCOPY: return gpu::Alloc::HostMapped;
        case PARTIAL:  return gpu::Alloc::Split;
        default:       return gpu::Alloc::Device;   // honest and fake
    }
}

// The recompute cheat's challenge loop. Prints one '@' line per challenge,
// after the response is sent, and a summary when the verifier hangs up.
static void serve_recompute(int cfd, gpu::Prover& p, RecomputeCtx& rc) {
    char err[512] = {0};
    uint64_t rebuilt_total = 0, rounds_total = 0, gave_up_challenges = 0;
    double rebuild_ms_total = 0, kernel_ms_total = 0;
    double kms = 0, cms = 0, fms = 0;
    gpu::RecomputeStats st;
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            st = gpu::RecomputeStats{};
            if (gpu::answer_recompute(p, ch.nonce, ch.k, ch.lanes, is_chained(ch), is_tree(ch),
                                      rebuild_blocks, &rc, hash, &kms, &cms, &fms, &st,
                                      err, sizeof err))
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
                   st.rebuild_ms, kms, cms, fms);
            if (gave_up > rc.gave_up_before) ++gave_up_challenges;
            rc.fills_before = fills;
            rc.gave_up_before = gave_up;
            rebuilt_total += st.rebuilt;
            rounds_total += st.rounds;
            rebuild_ms_total += st.rebuild_ms;
            kernel_ms_total += kms;
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
    Source src;
    RecomputeCtx rc;
    if (!open_source(src, cfd, s, err, sizeof err) || !recompute_setup(rc, s, src.built, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        free_built(src);
        return;
    }
    free_built(src);
    gpu::Prover p;
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

// Every other mode: one allocation kind from gpu_answer.cu, filled the same
// way the honest prover fills its buffer.
static void serve_alloc_mode(int cfd, const Setup& s) {
    char err[512] = {0};
    const double held = (g_mode == FAKE) ? g_frac : 1.0;
    const double offload = (g_mode == PARTIAL) ? g_offload : 0.0;
    gpu::Prover p;
    if (!gpu::create(p, s.num_words, alloc_for(g_mode), g_lane_cap, held, offload, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return;
    }
    Source src;
    bool ok = open_source(src, cfd, s, err, sizeof err);
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

    const char* backing = "?";
    bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    size_t hf = 0, ht = 0;
    gpu::mem_info(&hf, &ht);
    printf("[cheat] backing = %s (claiming locked=true regardless)\n", backing);
    printf("[cheat] hbm used=%.2f GiB of %.2f GiB\n",
           (double)(ht - hf) / 1073741824.0, (double)ht / 1073741824.0);
    printf("@{\"t\":\"cheatinfo\",\"mode\":");
    json_str(stdout, kModeNames[g_mode]);
    printf(",\"backing\":");
    json_str(stdout, backing);
    printf(",\"device_backed\":%s,\"held_frac\":%.4f,\"hog_mib\":%llu,"
           "\"offload\":%.4f,\"hbm_used\":%llu,\"hbm_total\":%llu,"
           "\"build\":%s,\"build_s\":%.4f}\n",
           is_device ? "true" : "false", held,
           (unsigned long long)g_hog_mib, offload,
           (unsigned long long)(ht - hf), (unsigned long long)ht,
           s.build ? "true" : "false", src.build_s);

    if (!send_ready(cfd, true)) { gpu::destroy(p); return; }

    double kms = 0, cms = 0, fms = 0;
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            if (gpu::answer(p, ch.nonce, ch.k, ch.lanes, is_chained(ch), is_tree(ch),
                            hash, &kms, &cms, &fms, err, sizeof err))
                return true;
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            if (!g_trace_every || n % g_trace_every) return true;
            gpu::mem_info(&hf, &ht);
            printf("@{\"t\":\"g\",\"served\":%llu,\"k\":%u,\"lanes\":%u,"
                   "\"chained\":%s,\"tree\":%s,\"kernel_ms\":%.4f,\"copy_ms\":%.4f,"
                   "\"fold_ms\":%.4f,\"probe_gbps\":%.3f,\"hbm_used\":%llu,"
                   "\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s,\"mode\":",
                   (unsigned long long)n, ch.k, ch.lanes,
                   is_chained(ch) ? "true" : "false", is_tree(ch) ? "true" : "false",
                   kms, cms, fms, gpu::probe_gbps(ch.k, kms),
                   (unsigned long long)(ht - hf), (unsigned long long)ht,
                   gpu::resident_fraction(p), is_device ? "true" : "false");
            json_str(stdout, kModeNames[g_mode]);
            printf("}\n");
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
        size_t free_mib = free_b / 1048576ull;
        size_t leave = (size_t)g_leave_free_mib;
        g_hog_mib = free_mib > leave ? free_mib - leave : 0;
        printf("[cheat] %zu MiB free on the card; reserving %zu MiB to leave %zu MiB free\n",
               free_mib, g_hog_mib, free_mib - g_hog_mib);
    }
    if (g_hog_mib) {
        if (!gpu::hog(g_hog_mib * 1048576ull, err, sizeof err)) {
            fprintf(stderr, "[cheat] FATAL: could not reserve %zu MiB: %s\n", g_hog_mib, err);
            return;
        }
        printf("[cheat] reserving %zu MiB of device memory\n", g_hog_mib);
    }
    if (g_mode == RECOMPUTE) serve_recompute_mode(cfd, s);
    else                     serve_alloc_mode(cfd, s);
    gpu::unhog();
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
        else if (!strcmp(a, "--hog"))             g_hog_mib = cli.uint(a, v);
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
    printf("@{\"t\":\"gpuinfo\",\"device\":%d,\"name\":", g_device);
    json_str(stdout, name);
    printf("}\n");
    return serve_forever("cheat", port, serve);
}
