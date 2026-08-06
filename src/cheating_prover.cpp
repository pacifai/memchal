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
static void challenge_loop(int cfd, Load&& load, uint64_t num_words, const char* name) {
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        g_probes_total += ch.k;
        Response r{ answer_challenge_t(load, num_words, ch.nonce, ch.k, ch.lanes) };
        if (!send_all(cfd, &r, sizeof(r))) break;
        ++served;
    }
    printf("[cheat:%s] served %llu challenges\n", name, (unsigned long long)served);
}

static void serve(int cfd, Mode mode, double frac, const char* datafile) {
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
        challenge_loop(cfd, [buf](uint64_t idx){ return buf[idx]; }, num_words, "honest");
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
            // Flush and drop what we've written every 8 MiB. Without this the
            // dirty page cache grows faster than writeback drains it, which
            // OOM-kills the process whenever its memory budget is smaller than
            // the buffer — the exact regime this mode exists to measure.
            unflushed += want;
            if (unflushed >= (8u << 20) || off == bytes) {
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
        challenge_loop(cfd, [map](uint64_t idx){ return map[idx]; }, num_words, mname);
        report_io(mname, g_probes_total, io0, io_read_bytes(), mf0, major_faults());

        munmap(map, bytes); close(fd); unlink(datafile);
        return;
    } else if (mode == DISK) {
        mname = "disk";
        printf("[cheat:disk] 0%% resident; every probe hits disk\n");

        size_t io0 = io_read_bytes(), mf0 = major_faults();
        challenge_loop(cfd, [fd](uint64_t idx){
            return disk_word(fd, idx);
        }, num_words, mname);
        report_io(mname, g_preads, io0, io_read_bytes(), mf0, major_faults());
    } else if (mode == PARTIAL) {
        mname = "partial";
        printf("[cheat:partial] %.0f%% resident, rest on disk (correct, bimodal)\n", frac * 100.0);

        size_t io0 = io_read_bytes(), mf0 = major_faults();
        challenge_loop(cfd, [ram, resident_words, fd](uint64_t idx){
            if (idx < resident_words) return ram[idx];
            return disk_word(fd, idx);
        }, num_words, mname);
        report_io(mname, g_preads, io0, io_read_bytes(), mf0, major_faults());
    } else { // FAKE
        mname = "fake";
        printf("[cheat:fake] %.0f%% resident, misses fabricated (fast, WRONG)\n", frac * 100.0);
        challenge_loop(cfd, [ram, resident_words](uint64_t idx){
            if (idx < resident_words) return ram[idx];
            return mix64(idx * 0x9E3779B97F4A7C15ULL + 1); // plausible-looking garbage
        }, num_words, mname);
    }

    if (ram) { munlock(ram, resident_words * 8); munmap(ram, resident_words * 8); }
    close(fd); unlink(datafile);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    long ps = sysconf(_SC_PAGESIZE); if (ps > 0) g_page = ps;

    uint16_t port = kDefaultPort;
    Mode mode = HONEST; double frac = 0.5;
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
    }

    int lfd = make_listener(port);
    const char* names[] = { "honest", "disk", "partial", "fake", "mmapdisk" };
    printf("[cheat] listening on 0.0.0.0:%u  mode=%s frac=%.2f\n", port, names[mode], frac);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        serve(cfd, mode, frac, datafile);
        close(cfd);
    }
    close(lfd);
    return 0;
}
