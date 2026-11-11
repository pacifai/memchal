#include <algorithm>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "cpu/pinned.hpp"
#include "cpu/proc_stats.hpp"
#include "attacks/cpu/seed_recovery.hpp"
#include "record/cpu_trace.hpp"
#include "shared/cli.hpp"
#include "shared/server.hpp"
#include "shared/thread_pool.hpp"

using namespace mc;

static const char kUsage[] =
    "usage: cheating_prover [--flag value]...\n"
    "  --port N            listen port (default %u)\n"
    "  --mode MODE         disk, partial, fake, mmapdisk, recompute, prefetch,\n"
    "                      or precompute (default disk)\n"
    "  --frac F            partial and fake: fraction held in RAM (default 0.5)\n"
    "  --datafile PATH     where disk-backed modes write the buffer\n"
    "  --trace N           print a residency event every N challenges\n";

enum Mode { DISK, PARTIAL, FAKE, MMAP_DISK, RECOMPUTE, PREFETCH, PRECOMPUTE };
static const char* const kModeNames[] = {
    "disk", "partial", "fake", "mmapdisk", "recompute", "prefetch", "precompute", nullptr };

static Mode        g_mode        = DISK;
static double      g_frac        = 0.5;
static const char* g_datafile    = "/tmp/memchal_cheat.bin";
static uint32_t    g_trace_every = 0;
static size_t      g_page        = 4096;

static bool write_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::write(fd, c, n);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}

static uint64_t disk_word(int fd, uint64_t idx, uint64_t* preads) {
    uint64_t w = 0;
    ++*preads;
    ssize_t got = pread(fd, &w, sizeof(w), (off_t)(idx * 8));
    if (got != (ssize_t)sizeof(w)) {
        fprintf(stderr, "[cheat:disk] pread error at idx=%llu: got=%zd errno=%d\n",
                (unsigned long long)idx, got, errno);
        assert(false && "pread short read");
    }
    return w;
}

// Prints how many bytes the process read from storage and how many major
// faults it took while answering, and whether the probes hit the page cache.
static void report_io(const char* name, uint64_t probes, size_t io0, size_t io1, size_t mf0, size_t mf1) {
    const bool have_io = io0 != kUnavailable && io1 != kUnavailable;
    const bool have_mf = mf0 != kUnavailable && mf1 != kUnavailable;
    const size_t io_delta = have_io ? io1 - io0 : 0;

    printf("[cheat:%s] io: probes=%llu read_bytes_delta=", name, (unsigned long long)probes);
    if (have_io) printf("%zu", io_delta); else printf("unavailable");
    printf(" majflt_delta=");
    if (have_mf) printf("%zu\n", mf1 - mf0); else printf("unavailable\n");

    if (!have_io || probes == 0) return;
    const size_t uncached_floor = probes * 4096;
    const double pct = 100.0 * (double)io_delta / (double)uncached_floor;
    if (io_delta < uncached_floor / 10)
        printf("[cheat:%s] io: => probes were served from the PAGE CACHE (%.1f%% of the "
               "uncached floor)\n", name, pct);
    else
        printf("[cheat:%s] io: => probes went to STORAGE (%.1f%% of the uncached floor)\n", name, pct);
}

// Answers challenges with `answer(ch)` and reports how many it served.
template <typename Answer>
static void run(int cfd, record::ResidencyTrace& trace, Answer&& answer) {
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            *hash = answer(ch);
            return true;
        },
        [&](const Challenge& ch, uint64_t n) { trace.after(ch, n); return true; });
    trace.finish(served);
    printf("[cheat:%s] served %llu challenges\n", kModeNames[g_mode], (unsigned long long)served);
}

template <typename Load>
static void run_with(int cfd, const Setup& s, record::ResidencyTrace& trace, Load&& load) {
    std::vector<uint64_t> scratch;
    run(cfd, trace, [&](const Challenge& ch) { return answer(load, s.num_words, ch, scratch); });
}

// Writes the buffer to `path` and returns the open file. Keeps the page cache
// from holding what it wrote.
static int spill(Source& src, size_t bytes, const char* path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open datafile"); return -1; }
    static const size_t kChunk = 1u << 20;
    std::vector<char> chunk(kChunk);
    size_t off = 0, unflushed = 0;
    while (off < bytes) {
        const size_t want = std::min(bytes - off, kChunk);
        if (!src_read(src, chunk.data(), want) || !write_all(fd, chunk.data(), want)) {
            fprintf(stderr, "[cheat] short data / file write failed\n");
            close(fd); unlink(path); return -1;
        }
        off += want;
        unflushed += want;
        if (unflushed >= (2u << 20) || off == bytes) {
            const size_t from = off - unflushed;
            sync_file_range(fd, (off_t)from, (off_t)unflushed,
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                            SYNC_FILE_RANGE_WAIT_AFTER);
            posix_fadvise(fd, (off_t)from, (off_t)unflushed, POSIX_FADV_DONTNEED);
            unflushed = 0;
        }
    }
    fsync(fd);
    posix_fadvise(fd, 0, bytes, POSIX_FADV_RANDOM);
    posix_fadvise(fd, 0, bytes, POSIX_FADV_DONTNEED);
    return fd;
}

static uint64_t* map_file(int fd, size_t bytes) {
    void* m = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { perror("mmap datafile"); return nullptr; }
    madvise(m, bytes, MADV_RANDOM);
    return (uint64_t*)m;
}

// Pins `bytes` of RAM and copies the start of the file into it.
static bool pin_from_file(Pinned& ram, int fd, size_t bytes) {
    void* file = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) return false;
    const bool ok = pin(ram, bytes);
    if (ok) memcpy(ram.words, file, bytes);
    munmap(file, bytes);
    return ok;
}

// Keeps one word, recovers the splitmix64 seed from it, and regenerates every
// probed word.
static void serve_recompute(int cfd, const Setup& s, Source& src) {
    uint64_t word0 = 0;
    if (!src_read(src, &word0, 8) || !src_skip(src, s.bytes - 8)) {
        fprintf(stderr, "[cheat:recompute] short data\n"); return;
    }
    free_built(src);
    const uint64_t seed = splitmix64_recover_seed(word0);
    printf("[cheat:recompute] holding no data; recovered seed=0x%016llx from buf[0]=0x%016llx\n",
           (unsigned long long)seed, (unsigned long long)word0);
    if (!send_ready(cfd, false)) return;
    record::ResidencyTrace trace(g_trace_every, "recompute", "regenerate", nullptr, 0, s.bytes);
    run_with(cfd, s, trace, [seed](uint64_t idx) { return splitmix64_word(seed, idx); });
}

// disk: reads every probe from the file. partial: keeps the first --frac of the
// buffer in RAM and reads the rest from the file. fake: keeps the same prefix
// and invents the rest.
static void serve_prefix(int cfd, const Setup& s, Source& src) {
    const uint64_t in_ram = g_mode == DISK ? 0 : (uint64_t)(g_frac * (double)s.num_words);
    int fd = spill(src, s.bytes, g_datafile);
    free_built(src);
    if (fd < 0) return;
    // In build mode the whole buffer is built in RAM first. Freeing it before reading
    // the first --frac back from the file keeps peak memory at one buffer, so partial
    // and fake run under the same memory limit as the honest prover.
    Pinned ram;
    if (in_ram && !pin_from_file(ram, fd, in_ram * 8)) {
        fprintf(stderr, "[cheat] could not load the first %.0f%% into RAM\n", 100.0 * g_frac);
        unpin(ram);
        close(fd); unlink(g_datafile);
        return;
    }

    if (send_ready(cfd, ram.locked)) {
        const char* name = kModeNames[g_mode];
        printf("[cheat:%s] %.0f%% in RAM, %s\n", name, 100.0 * in_ram / s.num_words,
               g_mode == FAKE ? "the rest invented" : "the rest read from disk");
        const size_t io0 = io_read_bytes(), mf0 = major_faults();
        record::ResidencyTrace trace(g_trace_every, name,
                                   g_mode == FAKE ? "fabricate" : g_mode == DISK ? "pread" : "partial",
                                   ram.words, in_ram * 8, s.bytes);
        const uint64_t* words = ram.words;
        if (g_mode == FAKE) {
            run_with(cfd, s, trace, [words, in_ram](uint64_t idx) {
                return idx < in_ram ? words[idx] : mix64(idx * kGoldenGamma + 1);
            });
        } else {
            uint64_t preads = 0;
            run_with(cfd, s, trace, [words, in_ram, fd, &preads](uint64_t idx) {
                return idx < in_ram ? words[idx] : disk_word(fd, idx, &preads);
            });
            report_io(name, preads, io0, io_read_bytes(), mf0, major_faults());
        }
    }
    unpin(ram);
    close(fd); unlink(g_datafile);
}

// Reads the file through mmap(), so each probe is a page fault or a page-cache hit.
static void serve_mmapdisk(int cfd, const Setup& s, Source& src) {
    int fd = spill(src, s.bytes, g_datafile);
    free_built(src);
    if (fd < 0) return;
    const uint64_t* map = map_file(fd, s.bytes);
    if (map && send_ready(cfd, false)) {
        const size_t io0 = io_read_bytes(), mf0 = major_faults();
        record::ResidencyTrace trace(g_trace_every, "mmapdisk", "mmap", map, s.bytes, s.bytes);
        run_with(cfd, s, trace, BufferLoad{map});
        report_io("mmapdisk", trace.probes(), io0, io_read_bytes(), mf0, major_faults());
    }
    if (map) munmap((void*)map, s.bytes);
    close(fd); unlink(g_datafile);
}

static constexpr uint32_t kPrefetchThreads = 64;

// Asks the kernel to read every page an independent-chain challenge probes. The
// probe offsets don't depend on the data, so a dry run that returns 0 finds them all.
static void prefetch_pages(const uint64_t* map, uint64_t num_words, const Challenge& ch,
                           std::vector<uint64_t>& scratch, std::vector<char*>& pages) {
    pages.clear();
    answer([&](uint64_t idx) -> uint64_t {
        pages.push_back((char*)((uintptr_t)(map + idx) & ~(uintptr_t)(g_page - 1)));
        return 0;
    }, num_words, ch, scratch);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (char* pg : pages) madvise(pg, g_page, MADV_WILLNEED);
}

// mmapdisk, with the page faults overlapped. Lanes run on separate threads, so a
// dependent chain still has one fault in flight per lane. An independent chain
// also gets every page requested before answering.
static void serve_prefetch(int cfd, const Setup& s, Source& src) {
    int fd = spill(src, s.bytes, g_datafile);
    free_built(src);
    if (fd < 0) return;
    const uint64_t* map = map_file(fd, s.bytes);
    if (map && send_ready(cfd, false)) {
        const size_t io0 = io_read_bytes(), mf0 = major_faults();
        record::ResidencyTrace trace(g_trace_every, "prefetch", "mmap", map, s.bytes, s.bytes);
        ThreadPool pool(kPrefetchThreads);
        std::vector<uint64_t> scratch, hashes;
        std::vector<char*> pages;
        run(cfd, trace, [&](const Challenge& ch) {
            if (!is_chained(ch)) prefetch_pages(map, s.num_words, ch, scratch, pages);
            hashes.resize(ch.lanes);
            pool.run(ch.lanes, [&](uint32_t, uint32_t l) {
                hashes[l] = answer_lane(BufferLoad{map}, s.num_words, ch.nonce, l,
                                        samples_in_lane(ch.samples, ch.lanes, l), is_chained(ch));
            });
            return fold_lanes(ch, hashes.data());
        });
        report_io("prefetch", trace.probes(), io0, io_read_bytes(), mf0, major_faults());
    }
    if (map) munmap((void*)map, s.bytes);
    close(fd); unlink(g_datafile);
}

struct Precompute {
    std::mutex mtx;
    std::unordered_map<uint64_t, uint64_t> answers;
    Challenge last{}, prev{};
    uint64_t seen = 0;
    bool done = false;
};

static constexpr uint64_t kLookahead = 64;

// Guesses the next nonces from the step between the last two and answers them
// ahead of time.
static void precompute_worker(Precompute* st, const uint64_t* map, uint64_t num_words) {
    std::vector<uint64_t> scratch;
    for (;;) {
        Challenge base{};
        uint64_t step = 0;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (st->done) return;
            if (st->seen >= 2) { base = st->last; step = st->last.nonce - st->prev.nonce; }
        }
        for (uint64_t j = 1; step && j <= kLookahead; ++j) {
            Challenge ch = base;
            ch.nonce = base.nonce + step * j;
            {
                std::lock_guard<std::mutex> lk(st->mtx);
                if (st->done) return;
                if (st->answers.count(ch.nonce)) continue;
            }
            const uint64_t ans = answer(BufferLoad{map}, num_words, ch, scratch);
            std::lock_guard<std::mutex> lk(st->mtx);
            if (st->done) return;
            if (st->last.samples == ch.samples && st->last.lanes == ch.lanes && st->last.flags == ch.flags)
                st->answers[ch.nonce] = ans;
        }
        struct timespec ts{0, step ? 100000 : 1000000};
        nanosleep(&ts, nullptr);
    }
}

// mmapdisk, plus a thread that answers predicted challenges in advance.
static void serve_precompute(int cfd, const Setup& s, Source& src) {
    int fd = spill(src, s.bytes, g_datafile);
    free_built(src);
    if (fd < 0) return;
    const uint64_t* map = map_file(fd, s.bytes);
    if (map && send_ready(cfd, false)) {
        Precompute st;
        std::thread worker(precompute_worker, &st, map, s.num_words);
        uint64_t hits = 0, answered = 0;
        std::vector<uint64_t> scratch;
        record::ResidencyTrace trace(g_trace_every, "precompute", "mmap", map, s.bytes, s.bytes);
        run(cfd, trace, [&](const Challenge& ch) {
            ++answered;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.prev = st.last;
                st.last = ch;
                ++st.seen;
                auto it = st.answers.find(ch.nonce);
                if (it != st.answers.end()) { ++hits; return it->second; }
            }
            const uint64_t ans = answer(BufferLoad{map}, s.num_words, ch, scratch);
            std::lock_guard<std::mutex> lk(st.mtx);
            st.answers[ch.nonce] = ans;
            return ans;
        });
        { std::lock_guard<std::mutex> lk(st.mtx); st.done = true; }
        worker.join();
        printf("[cheat:precompute] cache hits=%llu of %llu (%.1f%%)\n",
               (unsigned long long)hits, (unsigned long long)answered,
               answered ? 100.0 * (double)hits / (double)answered : 0.0);
    }
    if (map) munmap((void*)map, s.bytes);
    close(fd); unlink(g_datafile);
}

static void serve(int cfd) {
    Setup s;
    char err[256];
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[cheat] refusing: %s\n", err); return; }
    Source src{cfd};
    if (s.build && !build_on_host(src, s, err, sizeof err)) { fprintf(stderr, "[cheat] %s\n", err); return; }
    switch (g_mode) {
        case RECOMPUTE:  serve_recompute(cfd, s, src); break;
        case DISK:
        case PARTIAL:
        case FAKE:       serve_prefix(cfd, s, src); break;
        case MMAP_DISK:  serve_mmapdisk(cfd, s, src); break;
        case PREFETCH:   serve_prefetch(cfd, s, src); break;
        case PRECOMPUTE: serve_precompute(cfd, s, src); break;
    }
    free_built(src);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    long ps = sysconf(_SC_PAGESIZE); if (ps > 0) g_page = (size_t)ps;
    const Cli cli{"cheat", kUsage, kDefaultPort};
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) cli.help();
        const char* v = cli.value(argc, argv, &i);
        if      (!strcmp(a, "--port"))     port = cli.port(a, v);
        else if (!strcmp(a, "--mode"))     g_mode = (Mode)cli.choice(a, v, kModeNames);
        else if (!strcmp(a, "--frac"))     g_frac = cli.number(a, v, 0.0, 1.0);
        else if (!strcmp(a, "--datafile")) g_datafile = v;
        else if (!strcmp(a, "--trace"))    g_trace_every = cli.u32(a, v);
        else cli.error("unknown flag: %s", a);
    }
    printf("[cheat] mode=%s frac=%.2f\n", kModeNames[g_mode], g_frac);
    return serve_forever("cheat", port, serve);
}
