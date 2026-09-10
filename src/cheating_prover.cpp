// cheating_prover.cpp — a prover that does NOT honestly hold the data resident;
// demonstrates what each cheat does to the latency profile / correctness.
//
// Modes (--mode). The first group is caught by the two built-in signals (a wrong
// answer, or a slow one). The last three are shortcuts that beat both signals until
// the matching verifier defense is turned on.
//
//   honest     full buffer pinned in RAM (baseline; same as prover.cpp)
//   disk       buffer lives only on disk; every probe is a pread() -> slow tail
//   partial    keep a fraction --frac in RAM, rest on disk; probes into the
//              evicted part do pread() -> CORRECT but BIMODAL latency (fast hits +
//              slow misses). Tail grows as --frac shrinks. A pure timing detector.
//   fake       keep a fraction --frac in RAM; for misses, fabricate a value ->
//              fast but WRONG -> verification errors. A pure correctness detector.
//   mmapdisk   data on disk (page-cache DONTNEED'd), read via a mmap'd view — NO
//              per-probe pread syscall. Answers at ~honest speed *when it has the
//              RAM to cache the file* (the page cache is then holding the data, so
//              it really is resident); with a memory budget smaller than the buffer
//              it claims, probes fault through to storage and it is ~1000x honest.
//              See docs/investigations/mmap-needs-ram/.
//   recompute  hold nothing: recover the buffer's splitmix64 seed from one word and
//              regenerate every probe in O(1). Correct AND fast against the default
//              generator. Defeated by `--gen chacha` (seed unrecoverable -> wrong
//              data -> correctness catches it). See docs/investigations/ + ADR 0004.
//   prefetch   data on disk via mmap; enumerate the independent challenge's offsets
//              up front and MADV_WILLNEED-batch the pages, dodging serial faults.
//              Real only under memory pressure. Defeated by `--chain dependent`
//              (offsets depend on prior reads -> can't be enumerated ahead).
//   precompute exploit predictable nonces: a background thread infers coming nonces
//              and precomputes their answers from disk between challenges, serving
//              instantly. Defeated by `--nonce random` (nothing to precompute).
//
// All modes reuse answer_challenge_t() from common.hpp, so a correct answer is
// byte-identical to the verifier's local recomputation — only latency, or (for
// fake / a defended recompute) correctness, exposes the cheat.
#include "common.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unordered_map>
#include <mutex>
#include <thread>

using namespace mc;

enum Mode { HONEST, DISK, PARTIAL, FAKE, MMAP_DISK, RECOMPUTE, PREFETCH, PRECOMPUTE };

static long g_page = 4096;
static bool g_allow_unlocked = false;  // --allow-unlocked: skip honest-mode lock refusal

// Count of pread() calls made in disk_word(); instrumentation only.
static uint64_t g_preads = 0;

// Total probes requested across all challenges served (sum of ch.k). Accumulated
// once per challenge, never in the probe loop, so it costs nothing measurable.
static uint64_t g_probes_total = 0;

static void* alloc_pinned(size_t bytes, int* locked) {
    *locked = 0;
    struct rlimit rl{ RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return nullptr; }
    volatile char* vp = (volatile char*)p;
    for (size_t off = 0; off < bytes; off += (size_t)g_page) vp[off] = 0;
    int mlock_rc = mlock(p, bytes);
    // VERIFY the lock rather than trusting mlock's return value.
    size_t lb = locked_bytes();
    bool verified;
    if (lb != (size_t)-1) {
        verified = (lb >= bytes);
    } else {
        verified = (mlock_rc == 0);
    }
    *locked = verified ? 1 : 0;
    return p;
}

static int make_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 8) < 0) { perror("listen"); exit(1); }
    return fd;
}

// Read one 64-bit word from the file at word-index idx (0 on failure).
static inline uint64_t disk_word(int fd, uint64_t idx) {
    uint64_t w = 0;
    ++g_preads;
    ssize_t got = pread(fd, &w, sizeof(w), (off_t)(idx * 8));
    if (got != (ssize_t)sizeof(w)) {
        // A partial or failed read silently folds in 0, producing a wrong hash.
        // Make this loud so real I/O errors are not masked as plausible answers.
        fprintf(stderr, "[cheat:disk] pread error at idx=%llu: got=%zd errno=%d\n",
                (unsigned long long)idx, got, errno);
        assert(got == (ssize_t)sizeof(w) && "pread short read — real I/O error");
    }
    return w;
}

// ---------------------------------------------------------------------------
// Live trace for the browser demo (demo/bridge.py) — off unless --trace is given
// ---------------------------------------------------------------------------
// Every `every` challenges, emit one '@'-prefixed JSON line with this prover's
// own counters and, for a mapped mode, a mincore() snapshot of which pages of the
// buffer are actually resident right now. That snapshot is the honest answer to
// "is it holding the data?" — it is the kernel's view, not our claim.
//
// COST: emitted after the response has been sent, so it is outside the verifier's
// timed window; a mincore() over a 64 MiB mapping does cost ~0.1 ms, which delays
// the NEXT challenge. That is why the cadence is coarse and why the headline
// numbers come from the terminal experiments, which never enable this.
struct Trace {
    uint32_t every = 0;             // 0 = disabled
    const char* mode = "";
    const char* reader = "";        // "ram" | "pread" | "mmap"
    const unsigned char* map = nullptr;  // region to inspect, if any (for mincore)
    size_t bytes = 0;               // size of that region (may be a prefix, e.g. partial)
    size_t total_bytes = 0;         // size of the buffer the verifier asked us to hold
    unsigned char* vec = nullptr;   // mincore output, one byte per page
    size_t vec_len = 0;
};

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Pack the low bit of each byte into a bitstream, then base64 it to stdout.
static void b64_low_bits(const unsigned char* v, size_t count) {
    uint32_t acc = 0; int have = 0;   // bits pending, MSB-first within the sextets
    size_t nbytes = (count + 7) / 8;
    for (size_t b = 0; b < nbytes; ++b) {
        unsigned char byte = 0;
        for (size_t i = b * 8; i < (b + 1) * 8 && i < count; ++i)
            if (v[i] & 1u) byte |= (unsigned char)(1u << (i & 7));
        acc = (acc << 8) | byte; have += 8;
        while (have >= 6) { putchar(kB64[(acc >> (have - 6)) & 63]); have -= 6; }
    }
    if (have) { putchar(kB64[(acc << (6 - have)) & 63]); }
    for (int pad = (int)(nbytes % 3); pad && pad < 3; ++pad) putchar('=');
}

static void emit_trace(Trace& tr, uint64_t served) {
    size_t pages = tr.bytes / (size_t)g_page;
    size_t resident = 0;
    bool have_res = false;
    if (tr.map && pages) {
        if (tr.vec_len != pages) {
            free(tr.vec);
            tr.vec = (unsigned char*)calloc(pages, 1);
            tr.vec_len = tr.vec ? pages : 0;
        }
        if (tr.vec && mincore((void*)tr.map, tr.bytes, tr.vec) == 0) {
            have_res = true;
            for (size_t i = 0; i < pages; ++i) resident += (tr.vec[i] & 1u);
        }
    }
    size_t rb = io_read_bytes(), mf = major_faults();
    // `pages` is the region we can actually inspect; `pages_total` is the buffer the
    // verifier asked for. A pread reader maps nothing, so pages=0 — it holds no
    // pages at all, which is exactly the claim the page map should show.
    printf("@{\"t\":\"p\",\"served\":%llu,\"mode\":\"%s\",\"reader\":\"%s\","
           "\"pages\":%zu,\"pages_total\":%zu,\"read_bytes\":%lld,\"majflt\":%lld,"
           "\"probes\":%llu,\"res_pages\":%zu",
           (unsigned long long)served, tr.mode, tr.reader, pages,
           tr.total_bytes / (size_t)g_page,
           rb == (size_t)-1 ? -1LL : (long long)rb,
           mf == (size_t)-1 ? -1LL : (long long)mf,
           (unsigned long long)g_probes_total, resident);
    if (have_res) {
        printf(",\"res\":\"");
        b64_low_bits(tr.vec, pages);
        putchar('"');
    }
    puts("}");
}

// Print the per-run I/O instrumentation for a disk-backed mode: bytes actually
// fetched from the block layer and major faults taken (probes that could not be
// served from the page cache). The line is stable and greppable — the experiment
// scripts parse it. `probes` is the number of probes that went to the file.
static void report_io(const char* name, uint64_t probes,
                      size_t io0, size_t io1, size_t mf0, size_t mf1) {
    bool have_io = (io0 != (size_t)-1 && io1 != (size_t)-1);
    bool have_mf = (mf0 != (size_t)-1 && mf1 != (size_t)-1);
    size_t io_delta = have_io ? io1 - io0 : 0;
    size_t mf_delta = have_mf ? mf1 - mf0 : 0;

    printf("[cheat:%s] io: probes=%llu read_bytes_delta=", name, (unsigned long long)probes);
    if (have_io) printf("%zu", io_delta); else printf("unavailable");
    printf(" majflt_delta=");
    if (have_mf) printf("%zu\n", mf_delta); else printf("unavailable\n");

    if (!have_io || probes == 0) return;
    // One uncached probe costs at least one 4 KiB page from storage. Far less
    // than that means the probes were served from RAM.
    size_t uncached_floor = probes * 4096;
    if (io_delta < uncached_floor / 10) {
        printf("[cheat:%s] io: => probes were served from the PAGE CACHE (%.1f%% of the "
               "uncached floor); any latency here is syscall/page-cache overhead, "
               "NOT storage distance\n", name, 100.0 * (double)io_delta / (double)uncached_floor);
    } else {
        printf("[cheat:%s] io: => probes genuinely went to STORAGE (%.1f%% of the "
               "uncached floor); the page cache could not hold this file\n",
               name, 100.0 * (double)io_delta / (double)uncached_floor);
    }
}

// Run the challenge loop with a given word loader (no std::function, so the
// loader is inlined and doesn't distort the timing we're trying to expose).
template <typename Load>
static void challenge_loop(int cfd, Load&& load, uint64_t num_words, const char* name,
                           Trace* tr = nullptr) {
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        g_probes_total += ch.k;
        bool chained = (ch.flags & kFlagChained) != 0;
        Response r{ answer_challenge_t(load, num_words, ch.nonce, ch.k, ch.lanes, chained) };
        if (!send_all(cfd, &r, sizeof(r))) break;
        ++served;
        // Strictly after the response: never inside the verifier's timed window.
        if (tr && tr->every && (served % tr->every) == 0) emit_trace(*tr, served);
    }
    if (tr && tr->every) emit_trace(*tr, served);
    printf("[cheat:%s] served %llu challenges\n", name, (unsigned long long)served);
}

// ---------------------------------------------------------------------------
// PRECOMPUTE mode — the "third escape hatch" attack:
//
// Strategy:
//   • Keep the data on disk (mmap + MADV_RANDOM), hold nothing pinned.
//   • Serve loop (main thread): recv challenge, look up nonce in a cache.
//     Hit -> respond instantly. Miss -> compute from mmap (slow), then cache.
//   • Background precompute thread: once 2+ challenges seen, infer the step
//     (nonce_{i+1} - nonce_i), and precompute answers for the next K predicted
//     nonces into the cache, using the last-seen k/lanes/flags.
//
// Under sequential nonces + gap: the background thread stays ahead of the
// serve loop -> all cache hits -> latency ≈ transport floor with data on disk.
// Under random nonces: predictions always miss -> serve loop computes from
// disk on every challenge -> caught by the timing detector.
// ---------------------------------------------------------------------------

// Shared state between the serve loop and the background precompute thread.
struct PrecomputeState {
    std::mutex mtx;

    // Cache: nonce -> answer.
    std::unordered_map<uint64_t, uint64_t> cache;

    // Last two nonces seen (for step inference) and their challenge params.
    uint64_t last_nonce  = 0;
    uint64_t prev_nonce  = 0;
    uint32_t last_k      = 0;
    uint32_t last_lanes  = 0;
    uint32_t last_flags  = 0;
    uint64_t seen        = 0;   // number of challenges received so far

    bool done = false;          // set by the serve loop when the session ends
};

static constexpr uint64_t kPrecomputeLookahead = 64;  // nonces to precompute ahead

// Background thread: infer step, precompute the next kPrecomputeLookahead nonces.
static void precompute_worker(PrecomputeState* st, uint64_t* mmap_buf, uint64_t num_words) {
    for (;;) {
        // Snapshot shared state under lock.
        uint64_t step = 0, base_nonce = 0;
        uint32_t k = 0, lanes = 0, flags = 0;
        uint64_t seen = 0;
        {
            std::unique_lock<std::mutex> lk(st->mtx);
            if (st->done) return;
            seen = st->seen;
            if (seen < 2) {
                // Not enough samples yet; spin briefly then retry.
                lk.unlock();
                // Busy-wait is fine here: we only enter this path for the very
                // first ~2 challenges. After that we loop in the precompute body.
                struct timespec ts{0, 1000000}; // 1 ms
                nanosleep(&ts, nullptr);
                continue;
            }
            step  = st->last_nonce - st->prev_nonce;
            base_nonce = st->last_nonce;
            k     = st->last_k;
            lanes = st->last_lanes;
            flags = st->last_flags;
        }

        // Precompute the next kPrecomputeLookahead predicted nonces.
        bool chained = (flags & kFlagChained) != 0;
        for (uint64_t j = 1; j <= kPrecomputeLookahead; ++j) {
            uint64_t predicted_nonce = base_nonce + step * j;

            // Check if already cached (avoid redundant computation).
            {
                std::unique_lock<std::mutex> lk(st->mtx);
                if (st->done) return;
                if (st->cache.count(predicted_nonce)) continue;
            }

            // Compute from mmap (may fault, but we have offline time during gap_us).
            uint64_t ans = answer_challenge_t(
                [mmap_buf](uint64_t idx) -> uint64_t { return mmap_buf[idx]; },
                num_words, predicted_nonce, k, lanes, chained);

            {
                std::unique_lock<std::mutex> lk(st->mtx);
                if (st->done) return;
                // Only insert if the params haven't changed under us.
                if (st->last_k == k && st->last_lanes == lanes && st->last_flags == flags)
                    st->cache[predicted_nonce] = ans;
            }
        }

        // After precomputing, re-check if done; otherwise loop to stay ahead.
        {
            std::unique_lock<std::mutex> lk(st->mtx);
            if (st->done) return;
            // Update base_nonce snapshot so next iteration looks further ahead.
            // (The serve loop will have advanced last_nonce in the meantime.)
        }
        // Small yield so the serve loop can acquire the mutex.
        struct timespec ts{0, 100000}; // 0.1 ms
        nanosleep(&ts, nullptr);
    }
}

static void serve_precompute(int cfd, const char* datafile, uint32_t trace_every) {
    set_low_latency(cfd);

    Hello hello{};
    if (!recv_all(cfd, &hello, sizeof(hello)) || hello.magic != kMagic) {
        fprintf(stderr, "[cheat:precompute] bad hello\n"); return;
    }
    size_t bytes = ((size_t)hello.buffer_bytes + 7) & ~size_t(7);
    uint64_t num_words = bytes / 8;

    // Build mode: materialize the matrix, then spill it. See the note in serve().
    uint64_t* scratch = nullptr;
    Source src; src.fd = cfd;
    if (hello.flags & kFlagBuild) {
        RecipeMsg rmsg{};
        if (!recv_all(cfd, &rmsg, sizeof rmsg)) {
            fprintf(stderr, "[cheat:precompute] no recipe\n"); return;
        }
        const Recipe r = recipe_from_msg(rmsg);
        if (build_buffer_words(r) != num_words) {
            fprintf(stderr, "[cheat:precompute] recipe size disagrees with hello\n"); return;
        }
        scratch = (uint64_t*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (scratch == MAP_FAILED || !build_buffer(scratch, num_words, r)) {
            fprintf(stderr, "[cheat:precompute] cannot build %.1f MiB\n", bytes / 1048576.0);
            if (scratch != MAP_FAILED) munmap(scratch, bytes);
            return;
        }
        src.built = scratch;
    }

    // Stream buffer to disk (never hold a full anonymous copy in RAM).
    int fd = open(datafile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("[precompute] open datafile"); if (scratch) munmap(scratch, bytes); return; }

    {
        static const size_t kChunk = 1u << 20; // 1 MiB
        char* chunk = (char*)malloc(kChunk);
        if (!chunk) { fprintf(stderr, "[cheat:precompute] chunk alloc failed\n"); close(fd); return; }
        size_t off = 0, unflushed = 0;
        bool ok = true;
        while (off < bytes) {
            size_t want = bytes - off < kChunk ? bytes - off : kChunk;
            if (!src_read(src, chunk, want)) { ok = false; break; }
            if (!write_all(fd, chunk, want)) { ok = false; break; }
            off += want;
            unflushed += want;
            if (unflushed >= (2u << 20) || off == bytes) {
                size_t from = off - unflushed;
                sync_file_range(fd, (off_t)from, (off_t)unflushed,
                                SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                                SYNC_FILE_RANGE_WAIT_AFTER);
                posix_fadvise(fd, (off_t)from, (off_t)unflushed, POSIX_FADV_DONTNEED);
                unflushed = 0;
            }
        }
        free(chunk);
        if (scratch) { munmap(scratch, bytes); scratch = nullptr; src.built = nullptr; }
        if (!ok) {
            fprintf(stderr, "[cheat:precompute] short data / file write failed\n");
            close(fd); return;
        }
    }
    fsync(fd);
    posix_fadvise(fd, 0, bytes, POSIX_FADV_RANDOM);
    posix_fadvise(fd, 0, bytes, POSIX_FADV_DONTNEED);

    // mmap the file: no pinned RAM, reads fault on demand.
    uint64_t* mmap_buf = (uint64_t*)mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mmap_buf == MAP_FAILED) {
        perror("[precompute] mmap"); close(fd); return;
    }
    madvise(mmap_buf, bytes, MADV_RANDOM);

    // Send Ready (not locked).
    Ready rd{ kMagic, 0, 0 };
    send_all(cfd, &rd, sizeof(rd));

    printf("[cheat:precompute] 0%% pinned; data on disk via mmap; background precompute thread started\n");

    PrecomputeState st;
    std::thread worker(precompute_worker, &st, mmap_buf, num_words);

    uint64_t served = 0, cache_hits = 0;
    // precompute is mmap-backed: mincore() over the mmap shows which pages the
    // background thread has actually staged into the page cache.
    Trace tr_pc{trace_every, "precompute", "mmap", (const unsigned char*)mmap_buf, bytes, bytes, nullptr, 0};
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;

        uint64_t ans = 0;
        bool hit = false;

        {
            std::unique_lock<std::mutex> lk(st.mtx);
            auto it = st.cache.find(ch.nonce);
            if (it != st.cache.end()) {
                ans = it->second;
                hit = true;
            }
            // Update shared state for the background thread to track.
            st.prev_nonce  = st.last_nonce;
            st.last_nonce  = ch.nonce;
            st.last_k      = ch.k;
            st.last_lanes  = ch.lanes;
            st.last_flags  = ch.flags;
            st.seen++;
        }

        if (!hit) {
            // Cache miss: compute from mmap now (slow path).
            bool chained = (ch.flags & kFlagChained) != 0;
            ans = answer_challenge_t(
                [mmap_buf](uint64_t idx) -> uint64_t { return mmap_buf[idx]; },
                num_words, ch.nonce, ch.k, ch.lanes, chained);
            // Insert into cache for potential reuse.
            std::unique_lock<std::mutex> lk(st.mtx);
            st.cache[ch.nonce] = ans;
        } else {
            ++cache_hits;
        }

        Response r{ ans };
        if (!send_all(cfd, &r, sizeof(r))) break;
        ++served;
        // Strictly after the response: never inside the verifier's timed window.
        if (tr_pc.every && (served % tr_pc.every) == 0) emit_trace(tr_pc, served);
    }
    if (tr_pc.every) emit_trace(tr_pc, served);
    free(tr_pc.vec);

    // Signal the background thread to stop and join it.
    {
        std::unique_lock<std::mutex> lk(st.mtx);
        st.done = true;
    }
    worker.join();

    printf("[cheat:precompute] served %llu challenges, cache hits=%llu (%.1f%%)\n",
           (unsigned long long)served,
           (unsigned long long)cache_hits,
           served > 0 ? 100.0 * (double)cache_hits / (double)served : 0.0);

    munmap(mmap_buf, bytes);
    close(fd);
    unlink(datafile);
}

static void serve(int cfd, Mode mode, double frac, const char* datafile,
                  uint32_t trace_every) {
    set_low_latency(cfd);

    Hello hello{};
    if (!recv_all(cfd, &hello, sizeof(hello)) || hello.magic != kMagic) {
        fprintf(stderr, "[cheat] bad hello\n"); return;
    }
    size_t bytes = ((size_t)hello.buffer_bytes + 7) & ~size_t(7);
    uint64_t num_words = bytes / 8;

    const bool build = (hello.flags & kFlagBuild) != 0;
    Recipe recipe;
    if (build) {
        RecipeMsg rmsg{};
        if (!recv_all(cfd, &rmsg, sizeof rmsg)) { fprintf(stderr, "[cheat] no recipe\n"); return; }
        recipe = recipe_from_msg(rmsg);
        if (build_buffer_words(recipe) != num_words) {
            fprintf(stderr, "[cheat] recipe size disagrees with hello\n"); return;
        }
    }

    int locked = 0;
    const char* mname = "honest";

    if (mode == HONEST) {
        int lk = 0; uint64_t* buf = (uint64_t*)alloc_pinned(bytes, &lk);
        if (!buf) return;
        if (build) {
            if (!build_buffer(buf, num_words, recipe)) {
                fprintf(stderr, "[cheat] build failed\n"); munmap(buf, bytes); return;
            }
        } else if (!recv_all(cfd, buf, bytes)) {
            fprintf(stderr, "[cheat] short data\n"); munmap(buf, bytes); return;
        }
        locked = lk;
        printf("[cheat:honest] resident+%s\n", lk ? "LOCKED" : "unlocked");
        if (!lk && !g_allow_unlocked) {
            fprintf(stderr, "[cheat:honest] FATAL: memory not locked (need CAP_IPC_LOCK / `ulimit -l`, or pass --allow-unlocked)\n");
            munmap(buf, bytes); exit(1);
        }
        Ready rd{ kMagic, (uint32_t)locked, 0 }; send_all(cfd, &rd, sizeof(rd));
        Trace tr{trace_every, "honest", "ram", (const unsigned char*)buf, bytes, bytes, nullptr, 0};
        challenge_loop(cfd, [buf](uint64_t idx){ return buf[idx]; }, num_words, "honest",
                       trace_every ? &tr : nullptr);
        free(tr.vec);
        munlock(buf, bytes); munmap(buf, bytes);
        return;
    }

    // Build mode has to materialize the whole matrix before anything can be
    // spilled: Argon2 cannot produce a slice of it on demand. So a prover whose
    // RAM budget is smaller than the buffer cannot even set up under --gen
    // argon2d — the build-buffer design forces every prover to hold buffer-sized
    // memory at least once. It may free it immediately afterward, which is why
    // this raises the floor rather than proving residency.
    uint64_t* scratch = nullptr;
    Source src; src.fd = cfd;
    if (build) {
        scratch = (uint64_t*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (scratch == MAP_FAILED) {
            fprintf(stderr, "[cheat] cannot map %.1f MiB to build in: %s\n"
                            "        --gen argon2d needs buffer-sized RAM at build time\n",
                    bytes / 1048576.0, strerror(errno));
            return;
        }
        if (!build_buffer(scratch, num_words, recipe)) {
            fprintf(stderr, "[cheat] build failed\n"); munmap(scratch, bytes); return;
        }
        src.built = scratch;
    }

    if (mode == RECOMPUTE) {
        // ADVERSARY: recompute attack against a splitmix64-generated buffer.
        //
        // Strategy: receive buf[0] from the incoming stream, recover the seed via
        // splitmix64_recover_seed(), then FREE the entire buffer and answer every
        // challenge using the O(1) splitmix64_word() formula.
        //
        // Against a splitmix64-generated buffer this reproduces every word exactly
        // → 0 verification errors, near-zero memory.
        // Against a chacha20-generated buffer the "recovered" seed is bogus →
        // wrong words → verification errors (correctness detector catches it).
        //
        // Receive just the first word, then drain-and-discard the rest of the stream.
        uint64_t word0 = 0;
        if (!src_read(src, &word0, 8)) { fprintf(stderr, "[cheat:recompute] short data\n"); return; }
        uint64_t recovered_seed = splitmix64_recover_seed(word0);

        // Drain the rest of the incoming buffer (we don't store it).
        {
            static const size_t kDrain = 1u << 20;
            char* chunk = (char*)malloc(kDrain);
            if (!chunk) { fprintf(stderr, "[cheat:recompute] alloc failed\n"); return; }
            size_t remaining = bytes - 8;
            while (remaining > 0) {
                size_t want = remaining < kDrain ? remaining : kDrain;
                if (!src_read(src, chunk, want)) { free(chunk); return; }
                remaining -= want;
            }
            free(chunk);
        }

        if (scratch) { munmap(scratch, bytes); scratch = nullptr; src.built = nullptr; }
        printf("[cheat:recompute] holding ~0 data; recovered seed=0x%016llx from buf[0]=0x%016llx\n",
               (unsigned long long)recovered_seed, (unsigned long long)word0);

        Ready rd{ kMagic, 0, 0 };
        send_all(cfd, &rd, sizeof(rd));

        // recompute holds NO mapping — set map=nullptr and bytes=0 so emit_trace
        // emits pages=0, no residency bitmap: every probe was regenerated, not read.
        Trace tr{trace_every, "recompute", "regenerate", nullptr, 0, bytes, nullptr, 0};
        challenge_loop(cfd,
            [recovered_seed](uint64_t idx){ return splitmix64_word(recovered_seed, idx); },
            num_words, "recompute", trace_every ? &tr : nullptr);
        free(tr.vec);
        return;
    }

    // Disk-backed modes stream the buffer to a file in chunks. On the upload path
    // that means never holding a full-size anonymous copy, which is what lets the
    // cheat run in a budget smaller than the buffer it claims to hold — the
    // regime where the mmap trick can no longer hide behind the page cache.
    // Under --gen argon2d the scratch above has already broken that: the matrix
    // must exist whole before any of it can be spilled.
    int fd = open(datafile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open datafile"); if (scratch) munmap(scratch, bytes); return; }

    uint64_t resident_words = 0;
    uint64_t* ram = nullptr;
    if (mode == PARTIAL || mode == FAKE) {
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        resident_words = (uint64_t)(frac * (double)num_words);
        if (resident_words) {
            int lk = 0; ram = (uint64_t*)alloc_pinned(resident_words * 8, &lk);
            if (ram) locked = lk; else resident_words = 0;
        }
    }

    {
        static const size_t kChunk = 1u << 20; // 1 MiB
        char* chunk = (char*)malloc(kChunk);
        if (!chunk) { fprintf(stderr, "[cheat] chunk alloc failed\n"); close(fd); return; }
        size_t off = 0, unflushed = 0;
        bool ok = true;
        while (off < bytes) {
            size_t want = bytes - off < kChunk ? bytes - off : kChunk;
            if (!src_read(src, chunk, want)) { ok = false; break; }
            if (!write_all(fd, chunk, want)) { ok = false; break; }
            // PARTIAL/FAKE keep a resident prefix: copy that part as it streams by.
            size_t prefix = resident_words * 8;
            if (ram && off < prefix) {
                size_t n = prefix - off < want ? prefix - off : want;
                memcpy((char*)ram + off, chunk, n);
            }
            off += want;
            // Flush and drop what we've written every 2 MiB. Without this the
            // dirty page cache grows faster than writeback drains it, which
            // OOM-kills the process whenever its memory budget is smaller than
            // the buffer — the exact regime this mode exists to measure. The
            // window is deliberately small: it bounds the dirty high-water mark to
            // ~2 MiB, so a 32 MiB budget survives even when the host is busy
            // enough to slow writeback (at 8 MiB it was marginal and died).
            unflushed += want;
            if (unflushed >= (2u << 20) || off == bytes) {
                size_t from = off - unflushed;
                sync_file_range(fd, (off_t)from, (off_t)unflushed,
                                SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                                SYNC_FILE_RANGE_WAIT_AFTER);
                posix_fadvise(fd, (off_t)from, (off_t)unflushed, POSIX_FADV_DONTNEED);
                unflushed = 0;
            }
        }
        free(chunk);
        if (scratch) { munmap(scratch, bytes); scratch = nullptr; src.built = nullptr; }
        if (!ok) { fprintf(stderr, "[cheat] short data / file write failed\n"); close(fd); return; }
    }
    fsync(fd);
    posix_fadvise(fd, 0, bytes, POSIX_FADV_RANDOM);   // no readahead
    posix_fadvise(fd, 0, bytes, POSIX_FADV_DONTNEED); // try to evict page cache

    Ready rd{ kMagic, (uint32_t)locked, 0 };
    send_all(cfd, &rd, sizeof(rd));

    if (mode == PREFETCH) {
        // ADVERSARY (timing attack): data on disk, accessed via mmap+MADV_RANDOM.
        // For each challenge, enumerate the probe indices up-front (dry run with a
        // zero-returning loader), then issue MADV_WILLNEED on those pages
        // asynchronously before reading for real.
        //
        // With --chain independent: the indices are data-independent, so the dry
        // run yields the *true* indices -> prefetch hits the right pages -> fewer
        // serialized faults -> faster than cold serial reads under memory pressure.
        //
        // With --chain dependent: the dry-run indices are computed from zeros
        // (wrong) -> prefetch hits wrong pages -> the real read still faults
        // serially -> slower (defense wins). Correctness is always 0 errors.
        mname = "prefetch";
        uint64_t* map = (uint64_t*)mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { perror("mmap file (prefetch)"); close(fd); return; }
        madvise(map, bytes, MADV_RANDOM); // no readahead; we control it manually
        printf("[cheat:prefetch] 0%% pinned; enumerate indices then MADV_WILLNEED before answering\n");

        size_t io0 = io_read_bytes(), mf0 = major_faults();

        // Reserve a scratch buffer for recorded indices (worst-case k per challenge).
        // We'll use the verifier's k as the upper bound — reuse across challenges.
        uint64_t served = 0;
        Trace tr_pf{trace_every, mname, "mmap", (const unsigned char*)map, bytes, bytes, nullptr, 0};
        for (;;) {
            Challenge ch{};
            if (!recv_all(cfd, &ch, sizeof(ch))) break;
            if (ch.flags & kFlagTerminate) break;
            g_probes_total += ch.k;
            bool chained = (ch.flags & kFlagChained) != 0;

            // --- Phase 1: dry run — enumerate indices (data-independent values if
            //     !chained, zero-derived values if chained — intentionally wrong). ---
            std::vector<uint64_t> recorded;
            recorded.reserve(ch.k);
            answer_challenge_t(
                [&](uint64_t idx) -> uint64_t { recorded.push_back(idx); return (uint64_t)0; },
                num_words, ch.nonce, ch.k, ch.lanes, chained);

            // --- Phase 2: prefetch — issue MADV_WILLNEED on the pages containing
            //     the recorded indices (deduplicated). This is async: the kernel
            //     reads them into the page cache in the background. ---
            {
                // Collect unique page base addresses.
                std::vector<void*> pages_to_advise;
                pages_to_advise.reserve(recorded.size());
                for (uint64_t idx : recorded) {
                    char* addr = (char*)map + idx * 8;
                    // Align down to page boundary.
                    char* page_addr = (char*)((uintptr_t)addr & ~(uintptr_t)(g_page - 1));
                    pages_to_advise.push_back(page_addr);
                }
                // Sort and deduplicate.
                std::sort(pages_to_advise.begin(), pages_to_advise.end());
                pages_to_advise.erase(
                    std::unique(pages_to_advise.begin(), pages_to_advise.end()),
                    pages_to_advise.end());
                // Advise each unique page.
                for (void* pg : pages_to_advise) {
                    madvise(pg, (size_t)g_page, MADV_WILLNEED);
                }
            }

            // --- Phase 3: answer for real with the actual mmap loader. ---
            uint64_t ans = answer_challenge_t(
                [map](uint64_t idx) -> uint64_t { return map[idx]; },
                num_words, ch.nonce, ch.k, ch.lanes, chained);

            Response r{ ans };
            if (!send_all(cfd, &r, sizeof(r))) break;
            ++served;
            // Strictly after the response: never inside the verifier's timed window.
            if (tr_pf.every && (served % tr_pf.every) == 0) emit_trace(tr_pf, served);
        }
        if (tr_pf.every) emit_trace(tr_pf, served);

        report_io(mname, g_probes_total, io0, io_read_bytes(), mf0, major_faults());
        printf("[cheat:%s] served %llu challenges\n", mname, (unsigned long long)served);
        free(tr_pf.vec);
        munmap(map, bytes); close(fd); unlink(datafile);
        return;
    } else if (mode == MMAP_DISK) {
        // ADVERSARY: data on disk (page-cache DONTNEED'd), but read via a
        // memory-mapped view -> NO per-probe syscall. Falsification test: if
        // this answers at ~honest speed, the "disk" timing signal was syscall
        // overhead, not storage distance. It does — but only while the page cache
        // can hold the file, which is itself residency. The report_io() line below
        // says which regime the run was actually in, so the latency is never read
        // without the evidence next to it.
        mname = "mmapdisk";
        uint64_t* map = (uint64_t*)mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { perror("mmap file"); close(fd); return; }
        madvise(map, bytes, MADV_RANDOM); // no readahead; fault on demand
        printf("[cheat:mmapdisk] 0%% pinned; probes read a mmap'd file view (no pread syscall)\n");

        size_t io0 = io_read_bytes(), mf0 = major_faults();
        Trace tr{trace_every, mname, "mmap", (const unsigned char*)map, bytes, bytes, nullptr, 0};
        challenge_loop(cfd, [map](uint64_t idx){ return map[idx]; }, num_words, mname,
                       trace_every ? &tr : nullptr);
        free(tr.vec);
        report_io(mname, g_probes_total, io0, io_read_bytes(), mf0, major_faults());

        munmap(map, bytes); close(fd); unlink(datafile);
        return;
    } else if (mode == DISK) {
        mname = "disk";
        printf("[cheat:disk] 0%% resident; every probe hits disk\n");

        size_t io0 = io_read_bytes(), mf0 = major_faults();
        Trace tr{trace_every, mname, "pread", nullptr, 0, bytes, nullptr, 0};
        challenge_loop(cfd, [fd](uint64_t idx){
            return disk_word(fd, idx);
        }, num_words, mname, trace_every ? &tr : nullptr);
        free(tr.vec);
        report_io(mname, g_preads, io0, io_read_bytes(), mf0, major_faults());
    } else if (mode == PARTIAL) {
        mname = "partial";
        printf("[cheat:partial] %.0f%% resident, rest on disk (correct, bimodal)\n", frac * 100.0);

        size_t io0 = io_read_bytes(), mf0 = major_faults();
        Trace tr{trace_every, mname, "partial", (const unsigned char*)ram,
                 resident_words * 8, bytes, nullptr, 0};
        challenge_loop(cfd, [ram, resident_words, fd](uint64_t idx){
            if (idx < resident_words) return ram[idx];
            return disk_word(fd, idx);
        }, num_words, mname, trace_every ? &tr : nullptr);
        free(tr.vec);
        report_io(mname, g_preads, io0, io_read_bytes(), mf0, major_faults());
    } else { // FAKE
        mname = "fake";
        printf("[cheat:fake] %.0f%% resident, misses fabricated (fast, WRONG)\n", frac * 100.0);
        Trace tr{trace_every, mname, "fabricate", (const unsigned char*)ram,
                 resident_words * 8, bytes, nullptr, 0};
        challenge_loop(cfd, [ram, resident_words](uint64_t idx){
            if (idx < resident_words) return ram[idx];
            return mix64(idx * 0x9E3779B97F4A7C15ULL + 1); // plausible-looking garbage
        }, num_words, mname, trace_every ? &tr : nullptr);
        free(tr.vec);
    }

    if (ram) { munlock(ram, resident_words * 8); munmap(ram, resident_words * 8); }
    close(fd); unlink(datafile);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    long ps = sysconf(_SC_PAGESIZE); if (ps > 0) g_page = ps;

    uint16_t port = kDefaultPort;
    Mode mode = HONEST; double frac = 0.5; uint32_t trace_every = 0;
    const char* datafile = "/tmp/memchal_cheat.bin";
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--port")  && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode")  && i + 1 < argc) {
            const char* m = argv[++i];
            if      (!strcmp(m, "honest"))      mode = HONEST;
            else if (!strcmp(m, "disk"))        mode = DISK;
            else if (!strcmp(m, "partial"))     mode = PARTIAL;
            else if (!strcmp(m, "fake"))        mode = FAKE;
            else if (!strcmp(m, "mmapdisk"))    mode = MMAP_DISK;
            else if (!strcmp(m, "recompute"))   mode = RECOMPUTE;
            else if (!strcmp(m, "prefetch"))    mode = PREFETCH;
            else if (!strcmp(m, "precompute"))  mode = PRECOMPUTE;
        }
        else if (!strcmp(argv[i], "--frac") && i + 1 < argc) frac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--datafile") && i + 1 < argc) datafile = argv[++i];
        // Live trace for the browser demo: emit a counters+residency event every N
        // challenges. Off by default — the experiments must stay uninstrumented.
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_every = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--allow-unlocked")) g_allow_unlocked = true;
    }

    int lfd = make_listener(port);
    const char* names[] = { "honest", "disk", "partial", "fake", "mmapdisk", "recompute", "prefetch", "precompute" };
    printf("[cheat] listening on 0.0.0.0:%u  mode=%s frac=%.2f\n", port, names[mode], frac);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        if (mode == PRECOMPUTE) {
            serve_precompute(cfd, datafile, trace_every);
        } else {
            serve(cfd, mode, frac, datafile, trace_every);
        }
        close(cfd);
    }
    close(lfd);
    return 0;
}
