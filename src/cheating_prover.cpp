// cheating_prover.cpp — a prover that does NOT honestly hold the data resident;
// demonstrates what each cheat does to the latency profile / correctness.
//
// Modes (--mode):
//   honest   full buffer pinned in RAM (baseline; same as prover.cpp)
//   disk     buffer lives only on disk; every probe is a pread() -> slow tail
//   partial  keep a fraction --frac in RAM, rest on disk; probes into the
//            evicted part do pread() -> CORRECT but BIMODAL latency (fast hits +
//            slow misses). Tail grows as --frac shrinks. A pure timing detector.
//   fake     keep a fraction --frac in RAM; for misses, fabricate a value ->
//            fast but WRONG -> verification errors. A pure correctness detector.
//   mmapdisk data on disk (page-cache DONTNEED'd), read via a mmap'd view — NO
//            per-probe pread syscall. Answers at ~honest speed *when it has the
//            RAM to cache the file* (the page cache is then holding the data, so
//            it really is resident); with a memory budget smaller than the buffer
//            it claims, probes fault through to storage and it is ~1000x honest.
//            See scripts/experiments/mmap_needs_ram.sh.
//
// All modes reuse answer_challenge_t() from common.hpp, so a correct answer is
// byte-identical to the verifier's local recomputation — only latency (disk/
// partial) or correctness (fake) exposes the cheat.
#include "common.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>

using namespace mc;

enum Mode { HONEST, DISK, PARTIAL, FAKE, MMAP_DISK };

static long g_page = 4096;

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
    (void)got; // best-effort; a failed probe just folds in 0
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
        Response r{ answer_challenge_t(load, num_words, ch.nonce, ch.k, ch.lanes) };
        if (!send_all(cfd, &r, sizeof(r))) break;
        ++served;
        // Strictly after the response: never inside the verifier's timed window.
        if (tr && tr->every && (served % tr->every) == 0) emit_trace(*tr, served);
    }
    if (tr && tr->every) emit_trace(*tr, served);
    printf("[cheat:%s] served %llu challenges\n", name, (unsigned long long)served);
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

    int locked = 0;
    const char* mname = "honest";

    if (mode == HONEST) {
        int lk = 0; uint64_t* buf = (uint64_t*)alloc_pinned(bytes, &lk);
        if (!buf) return;
        if (!recv_all(cfd, buf, bytes)) { fprintf(stderr, "[cheat] short data\n"); munmap(buf, bytes); return; }
        locked = lk;
        printf("[cheat:honest] resident+%s\n", lk ? "LOCKED" : "unlocked");
        Ready rd{ kMagic, (uint32_t)locked, 0 }; send_all(cfd, &rd, sizeof(rd));
        Trace tr{trace_every, "honest", "ram", (const unsigned char*)buf, bytes, bytes, nullptr, 0};
        challenge_loop(cfd, [buf](uint64_t idx){ return buf[idx]; }, num_words, "honest",
                       trace_every ? &tr : nullptr);
        free(tr.vec);
        munlock(buf, bytes); munmap(buf, bytes);
        return;
    }

    // Disk-backed modes: stream the buffer straight from the socket to a file in
    // chunks, so this process NEVER holds a full-size anonymous copy. That is
    // what a real adversary would do, and it is what lets the cheater run in a
    // memory budget smaller than the buffer it claims to hold (the regime where
    // the mmap trick can no longer hide behind the page cache).
    int fd = open(datafile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open datafile"); return; }

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
            if (!recv_all(cfd, chunk, want)) { ok = false; break; }
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
        if (!ok) { fprintf(stderr, "[cheat] short data / file write failed\n"); close(fd); return; }
    }
    fsync(fd);
    posix_fadvise(fd, 0, bytes, POSIX_FADV_RANDOM);   // no readahead
    posix_fadvise(fd, 0, bytes, POSIX_FADV_DONTNEED); // try to evict page cache

    Ready rd{ kMagic, (uint32_t)locked, 0 };
    send_all(cfd, &rd, sizeof(rd));

    if (mode == MMAP_DISK) {
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
            if      (!strcmp(m, "honest"))   mode = HONEST;
            else if (!strcmp(m, "disk"))     mode = DISK;
            else if (!strcmp(m, "partial"))  mode = PARTIAL;
            else if (!strcmp(m, "fake"))     mode = FAKE;
            else if (!strcmp(m, "mmapdisk")) mode = MMAP_DISK;
        }
        else if (!strcmp(argv[i], "--frac") && i + 1 < argc) frac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--datafile") && i + 1 < argc) datafile = argv[++i];
        // Live trace for the browser demo: emit a counters+residency event every N
        // challenges. Off by default — the experiments must stay uninstrumented.
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_every = (uint32_t)strtoul(argv[++i], nullptr, 10);
    }

    int lfd = make_listener(port);
    const char* names[] = { "honest", "disk", "partial", "fake", "mmapdisk" };
    printf("[cheat] listening on 0.0.0.0:%u  mode=%s frac=%.2f\n", port, names[mode], frac);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        serve(cfd, mode, frac, datafile, trace_every);
        close(cfd);
    }
    close(lfd);
    return 0;
}
