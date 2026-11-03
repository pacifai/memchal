// cheating_prover.cpp — CPU provers that do not hold the buffer in RAM, one
// per --mode. See the README for what each one tests and what catches it.
#include "challenge.hpp"
#include "cli.hpp"
#include "pinned.hpp"
#include "server.hpp"
#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace mc;

static const char kUsage[] =
    "usage: cheating_prover [--flag value]...\n"
    "  --port N            listen port (default %u)\n"
    "  --mode MODE         honest, disk, partial, fake, mmapdisk, recompute,\n"
    "                      prefetch, or precompute (default honest)\n"
    "  --frac F            partial and fake: fraction held in RAM (default 0.5)\n"
    "  --datafile PATH     where disk-backed modes spill the buffer\n"
    "  --trace N           print a residency event every N challenges\n"
    "  --allow-unlocked    honest: run even if the buffer cannot be locked\n";

enum Mode { HONEST, DISK, PARTIAL, FAKE, MMAP_DISK, RECOMPUTE, PREFETCH, PRECOMPUTE };
static const char* const kModeNames[] = {
    "honest", "disk", "partial", "fake", "mmapdisk", "recompute", "prefetch", "precompute", nullptr };

struct Options {
    Mode        mode           = HONEST;
    double      frac           = 0.5;
    const char* datafile       = "/tmp/memchal_cheat.bin";
    uint32_t    trace_every    = 0;
    bool        allow_unlocked = false;
};

static Options g_opt;
static size_t  g_page   = 4096;
static uint64_t g_preads = 0;   // pread() calls, for report_io()
static uint64_t g_probes = 0;   // probes requested across all challenges

static inline uint64_t disk_word(int fd, uint64_t idx) {
    uint64_t w = 0;
    ++g_preads;
    ssize_t got = pread(fd, &w, sizeof(w), (off_t)(idx * 8));
    if (got != (ssize_t)sizeof(w)) {
        fprintf(stderr, "[cheat:disk] pread error at idx=%llu: got=%zd errno=%d\n",
                (unsigned long long)idx, got, errno);
        assert(got == (ssize_t)sizeof(w) && "pread short read");
    }
    return w;
}

// ---------------------------------------------------------------------------
// Live trace for demo/bridge.py
// ---------------------------------------------------------------------------
// Every `every` challenges, prints one '@'-prefixed JSON line with this
// prover's counters and, for a mapped buffer, a mincore() snapshot of which of
// its pages are resident.
struct Trace {
    uint32_t every = 0;
    const char* mode = "";
    const char* reader = "";               // "ram", "pread", "mmap", ...
    const unsigned char* map = nullptr;    // region mincore() inspects, if any
    size_t bytes = 0;                      // size of that region
    size_t total_bytes = 0;                // size of the buffer the verifier sent
    std::vector<unsigned char> vec;
};

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Packs the low bit of each byte into a bitstream and prints it as base64.
static void b64_low_bits(const unsigned char* v, size_t count) {
    uint32_t acc = 0; int have = 0;
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
    const size_t pages = tr.bytes / g_page;
    size_t resident = 0;
    bool have_res = false;
    if (tr.map && pages) {
        tr.vec.resize(pages);
        if (mincore((void*)tr.map, tr.bytes, tr.vec.data()) == 0) {
            have_res = true;
            for (size_t i = 0; i < pages; ++i) resident += (tr.vec[i] & 1u);
        }
    }
    size_t rb = io_read_bytes(), mf = major_faults();
    printf("@{\"t\":\"p\",\"served\":%llu,\"mode\":\"%s\",\"reader\":\"%s\","
           "\"pages\":%zu,\"pages_total\":%zu,\"read_bytes\":%lld,\"majflt\":%lld,"
           "\"probes\":%llu,\"res_pages\":%zu",
           (unsigned long long)served, tr.mode, tr.reader, pages,
           tr.total_bytes / g_page,
           rb == (size_t)-1 ? -1LL : (long long)rb,
           mf == (size_t)-1 ? -1LL : (long long)mf,
           (unsigned long long)g_probes, resident);
    if (have_res) {
        printf(",\"res\":\"");
        b64_low_bits(tr.vec.data(), pages);
        putchar('"');
    }
    puts("}");
}

// Prints how many bytes the block layer fetched and how many major faults the
// process took while answering. The experiment scripts parse this line.
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
    size_t uncached_floor = probes * 4096;
    if (io_delta < uncached_floor / 10) {
        printf("[cheat:%s] io: => probes were served from the PAGE CACHE (%.1f%% of the "
               "uncached floor)\n", name, 100.0 * (double)io_delta / (double)uncached_floor);
    } else {
        printf("[cheat:%s] io: => probes went to STORAGE (%.1f%% of the uncached floor)\n",
               name, 100.0 * (double)io_delta / (double)uncached_floor);
    }
}

// The challenge loop every mode shares: counts probes, prints the trace after
// each response, and reports how many challenges it served.
template <typename Answer>
static void run(int cfd, Trace& tr, Answer&& answer) {
    const uint64_t served = answer_challenges(cfd, "cheat",
        [&](const Challenge& ch, uint64_t* hash) {
            g_probes += ch.k;
            *hash = answer(ch);
            return true;
        },
        [&](const Challenge&, uint64_t n) {
            if (tr.every && n % tr.every == 0) emit_trace(tr, n);
            return true;
        });
    if (tr.every) emit_trace(tr, served);
    printf("[cheat:%s] served %llu challenges\n", tr.mode, (unsigned long long)served);
}

// Answers through `load`, the way every mode but prefetch and precompute does.
template <typename Load>
static void run_with(int cfd, const Setup& s, Trace& tr, Load&& load) {
    std::vector<uint64_t> scratch;
    run(cfd, tr, [&](const Challenge& ch) { return answer_on_host(load, s.num_words, ch, scratch); });
}

// Writes the buffer to `path`, copying the first `prefix_bytes` into `prefix`
// on the way, and returns the file descriptor. It flushes and drops the page
// cache every 2 MiB: otherwise dirty pages pile up faster than writeback
// drains them, and a prover whose memory budget is smaller than the buffer is
// killed before it can answer.
static int spill(Source& src, size_t bytes, const char* path,
                 uint64_t* prefix = nullptr, size_t prefix_bytes = 0) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open datafile"); return -1; }
    static const size_t kChunk = 1u << 20;
    std::vector<char> chunk(kChunk);
    size_t off = 0, unflushed = 0;
    while (off < bytes) {
        size_t want = std::min(bytes - off, kChunk);
        if (!src_read(src, chunk.data(), want) || !write_all(fd, chunk.data(), want)) {
            fprintf(stderr, "[cheat] short data / file write failed\n");
            close(fd); unlink(path); return -1;
        }
        if (off < prefix_bytes)
            memcpy((char*)prefix + off, chunk.data(), std::min(prefix_bytes - off, want));
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

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

// The whole buffer in locked RAM, the same as prover.cpp.
static void serve_honest(int cfd, const Setup& s) {
    Pinned buf;
    if (!pin(buf, s.bytes)) return;
    if (s.build ? !build_buffer(buf.words, s.num_words, s.recipe) : !recv_all(cfd, buf.words, s.bytes)) {
        fprintf(stderr, "[cheat:honest] %s\n", s.build ? "build failed" : "short data");
        unpin(buf); return;
    }
    printf("[cheat:honest] resident and %s\n", buf.locked ? "locked" : "unlocked");
    if (!buf.locked && !g_opt.allow_unlocked) {
        fprintf(stderr, "[cheat:honest] FATAL: memory not locked (need CAP_IPC_LOCK / `ulimit -l`, "
                        "or pass --allow-unlocked)\n");
        unpin(buf); exit(1);
    }
    if (send_ready(cfd, buf.locked)) {
        Trace tr{g_opt.trace_every, "honest", "ram", (const unsigned char*)buf.words, s.bytes, s.bytes, {}};
        run_with(cfd, s, tr, BufLoad{buf.words});
    }
    unpin(buf);
}

// Keeps one word, recovers the splitmix64 seed from it, and regenerates every
// probe. Right against --gen splitmix64, wrong against anything else (ADR 0004).
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
    Trace tr{g_opt.trace_every, "recompute", "regenerate", nullptr, 0, s.bytes, {}};
    run_with(cfd, s, tr, [seed](uint64_t idx) { return splitmix64_word(seed, idx); });
}

// disk reads every probe with pread(). partial keeps the first --frac of the
// buffer in RAM and reads the rest with pread(): right, and slow on the probes
// that miss. fake keeps the same prefix and invents the rest: fast, and wrong.
static void serve_prefix(int cfd, const Setup& s, Source& src) {
    const Mode mode = g_opt.mode;
    const uint64_t held = mode == DISK ? 0 : (uint64_t)(g_opt.frac * (double)s.num_words);
    Pinned ram;
    if (held && !pin(ram, held * 8)) return;
    int fd = spill(src, s.bytes, g_opt.datafile, ram.words, held * 8);
    free_built(src);
    if (fd < 0) { unpin(ram); return; }

    if (send_ready(cfd, ram.locked)) {
        const char* name = kModeNames[mode];
        printf("[cheat:%s] %.0f%% in RAM, %s\n", name, 100.0 * held / s.num_words,
               mode == FAKE ? "the rest invented" : "the rest read from disk");
        const size_t io0 = io_read_bytes(), mf0 = major_faults();
        Trace tr{g_opt.trace_every, name, mode == FAKE ? "fabricate" : mode == DISK ? "pread" : "partial",
                 (const unsigned char*)ram.words, held * 8, s.bytes, {}};
        const uint64_t* words = ram.words;
        if (mode == FAKE) {
            run_with(cfd, s, tr, [words, held](uint64_t idx) {
                return idx < held ? words[idx] : mix64(idx * 0x9E3779B97F4A7C15ULL + 1);
            });
        } else {
            run_with(cfd, s, tr, [words, held, fd](uint64_t idx) {
                return idx < held ? words[idx] : disk_word(fd, idx);
            });
            report_io(name, g_preads, io0, io_read_bytes(), mf0, major_faults());
        }
    }
    unpin(ram);
    close(fd); unlink(g_opt.datafile);
}

// Reads the spilled file through mmap(), so a probe costs a page fault rather
// than a syscall, and costs nothing once the page cache holds the file
// (ADR 0003). report_io() says which of the two happened.
static void serve_mmapdisk(int cfd, const Setup& s, Source& src) {
    int fd = spill(src, s.bytes, g_opt.datafile);
    free_built(src);
    if (fd < 0) return;
    const uint64_t* map = map_file(fd, s.bytes);
    if (map && send_ready(cfd, false)) {
        const size_t io0 = io_read_bytes(), mf0 = major_faults();
        Trace tr{g_opt.trace_every, "mmapdisk", "mmap", (const unsigned char*)map, s.bytes, s.bytes, {}};
        run_with(cfd, s, tr, BufLoad{map});
        report_io("mmapdisk", g_probes, io0, io_read_bytes(), mf0, major_faults());
    }
    if (map) munmap((void*)map, s.bytes);
    close(fd); unlink(g_opt.datafile);
}

// mmapdisk, plus a dry run of each challenge that records its probe offsets so
// it can ask the kernel for those pages before answering. The offsets are
// right only when they do not depend on the data, so --chain dependent defeats it.
static void serve_prefetch(int cfd, const Setup& s, Source& src) {
    int fd = spill(src, s.bytes, g_opt.datafile);
    free_built(src);
    if (fd < 0) return;
    const uint64_t* map = map_file(fd, s.bytes);
    if (map && send_ready(cfd, false)) {
        const size_t io0 = io_read_bytes(), mf0 = major_faults();
        Trace tr{g_opt.trace_every, "prefetch", "mmap", (const unsigned char*)map, s.bytes, s.bytes, {}};
        std::vector<uint64_t> scratch;
        std::vector<char*> pages;
        run(cfd, tr, [&](const Challenge& ch) {
            pages.clear();
            answer_on_host([&](uint64_t idx) -> uint64_t {
                pages.push_back((char*)((uintptr_t)(map + idx) & ~(uintptr_t)(g_page - 1)));
                return 0;
            }, s.num_words, ch, scratch);
            std::sort(pages.begin(), pages.end());
            pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
            for (char* pg : pages) madvise(pg, g_page, MADV_WILLNEED);
            return answer_on_host(BufLoad{map}, s.num_words, ch, scratch);
        });
        report_io("prefetch", g_probes, io0, io_read_bytes(), mf0, major_faults());
    }
    if (map) munmap((void*)map, s.bytes);
    close(fd); unlink(g_opt.datafile);
}

// mmapdisk, plus a background thread that guesses the next nonces from the
// step between the last two and answers them ahead of time. Sequential nonces
// make every guess right; random ones make every guess wrong.
struct Precompute {
    std::mutex mtx;
    std::unordered_map<uint64_t, uint64_t> cache;   // nonce -> answer
    Challenge last{}, prev{};
    uint64_t seen = 0;
    bool done = false;
};

static constexpr uint64_t kLookahead = 64;

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
        if (step) {
            for (uint64_t j = 1; j <= kLookahead; ++j) {
                Challenge ch = base;
                ch.nonce = base.nonce + step * j;
                {
                    std::lock_guard<std::mutex> lk(st->mtx);
                    if (st->done) return;
                    if (st->cache.count(ch.nonce)) continue;
                }
                const uint64_t ans = answer_on_host(BufLoad{map}, num_words, ch, scratch);
                std::lock_guard<std::mutex> lk(st->mtx);
                if (st->done) return;
                if (st->last.k == ch.k && st->last.lanes == ch.lanes && st->last.flags == ch.flags)
                    st->cache[ch.nonce] = ans;
            }
        }
        struct timespec ts{0, step ? 100000 : 1000000};
        nanosleep(&ts, nullptr);
    }
}

static void serve_precompute(int cfd, const Setup& s, Source& src) {
    int fd = spill(src, s.bytes, g_opt.datafile);
    free_built(src);
    if (fd < 0) return;
    const uint64_t* map = map_file(fd, s.bytes);
    if (map && send_ready(cfd, false)) {
        Precompute st;
        std::thread worker(precompute_worker, &st, map, s.num_words);
        uint64_t hits = 0, answered = 0;
        std::vector<uint64_t> scratch;
        Trace tr{g_opt.trace_every, "precompute", "mmap", (const unsigned char*)map, s.bytes, s.bytes, {}};
        run(cfd, tr, [&](const Challenge& ch) {
            ++answered;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.prev = st.last;
                st.last = ch;
                ++st.seen;
                auto it = st.cache.find(ch.nonce);
                if (it != st.cache.end()) { ++hits; return it->second; }
            }
            const uint64_t ans = answer_on_host(BufLoad{map}, s.num_words, ch, scratch);
            std::lock_guard<std::mutex> lk(st.mtx);
            st.cache[ch.nonce] = ans;
            return ans;
        });
        { std::lock_guard<std::mutex> lk(st.mtx); st.done = true; }
        worker.join();
        printf("[cheat:precompute] cache hits=%llu of %llu (%.1f%%)\n",
               (unsigned long long)hits, (unsigned long long)answered,
               answered ? 100.0 * (double)hits / (double)answered : 0.0);
    }
    if (map) munmap((void*)map, s.bytes);
    close(fd); unlink(g_opt.datafile);
}

static void serve(int cfd) {
    Setup s;
    char err[256];
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[cheat] refusing: %s\n", err); return; }
    if (g_opt.mode == HONEST) { serve_honest(cfd, s); return; }

    Source src{cfd};
    if (s.build && !build_on_host(src, s, err, sizeof err)) { fprintf(stderr, "[cheat] %s\n", err); return; }
    switch (g_opt.mode) {
        case RECOMPUTE:  serve_recompute(cfd, s, src); break;
        case DISK:
        case PARTIAL:
        case FAKE:       serve_prefix(cfd, s, src); break;
        case MMAP_DISK:  serve_mmapdisk(cfd, s, src); break;
        case PREFETCH:   serve_prefetch(cfd, s, src); break;
        case PRECOMPUTE: serve_precompute(cfd, s, src); break;
        case HONEST:     break;
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
        if      (!strcmp(a, "--help") || !strcmp(a, "-h")) cli.help();
        else if (!strcmp(a, "--allow-unlocked")) g_opt.allow_unlocked = true;
        else if (!strcmp(a, "--port"))     port = cli.port(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--mode"))     g_opt.mode = (Mode)cli.choice(a, cli.value(argc, argv, &i), kModeNames);
        else if (!strcmp(a, "--frac"))     g_opt.frac = cli.number(a, cli.value(argc, argv, &i), 0.0, 1.0);
        else if (!strcmp(a, "--datafile")) g_opt.datafile = cli.value(argc, argv, &i);
        else if (!strcmp(a, "--trace"))    g_opt.trace_every = cli.u32(a, cli.value(argc, argv, &i));
        else cli.error("unknown flag: %s", a);
    }
    printf("[cheat] mode=%s frac=%.2f\n", kModeNames[g_opt.mode], g_opt.frac);
    return serve_forever("cheat", port, serve);
}
