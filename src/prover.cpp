// prover.cpp — receives a large random buffer, pins it in RAM, and answers
// memory challenges. The design keeps the answer path GPU-portable: a flat
// word buffer plus the pure functions in common.hpp.
//
// Memory pinning: we mmap an anonymous region, prefault every page, and mlock()
// it so the kernel may never swap it out. We do not trust mlock()'s return value
// — we verify the lock actually took by reading VmLck from /proc/self/status. If
// the buffer is not genuinely pinned the prover REFUSES to run (an unpinned
// honest baseline is meaningless); --allow-unlocked overrides and stamps the
// result locked=false. Needs CAP_IPC_LOCK / a high `ulimit -l` (run.sh grants both).
//
// GPU note: the on-GPU analog of mlock is simply cudaMalloc — VRAM is not
// swappable. For host staging use cudaHostAlloc(cudaHostAllocDefault) pinned
// memory so H2D copies are fast and non-pageable.
//
// --weights MANIFEST makes this the CPU stand-in for an inference engine that
// already holds a model's weights. At start it reads every segment the
// manifest lists into its own locked allocation — separate pieces of memory,
// the way an engine keeps each tensor — and answers a Hello flagged
// kHelloResident from those, through a Segments table, with no upload.
#include "common.hpp"
#include "weights.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>

using namespace mc;

static long g_page = 4096;
static bool g_allow_unlocked = false;

// Allocate `bytes`, prefault, and try to lock it resident. Sets *locked.
static void* alloc_pinned(size_t bytes, int* locked) {
    *locked = 0;
    struct rlimit rl{ RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl); // best effort; needs privilege to raise

    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return nullptr; }

    madvise(p, bytes, MADV_WILLNEED);
    // Prefault one byte per page so no lazy page-in happens during a challenge.
    volatile char* vp = (volatile char*)p;
    for (size_t off = 0; off < bytes; off += (size_t)g_page) vp[off] = 0;

    int mlock_rc = mlock(p, bytes);
    if (mlock_rc != 0) {
        fprintf(stderr, "[prover] mlock failed (%s)\n", strerror(errno));
    }

    // VERIFY the lock rather than trusting mlock's return value.
    size_t lb = locked_bytes();
    bool verified;
    if (lb != (size_t)-1) {
        // Linux: /proc/self/status VmLck is authoritative.
        verified = (lb >= bytes);
        if (verified) {
            printf("[prover] memory LOCKED and VERIFIED (VmLck=%.1f MiB >= required %.1f MiB)\n",
                   (double)lb / 1048576.0, (double)bytes / 1048576.0);
        } else {
            printf("[prover] mlock did NOT take effect (VmLck=%.1f MiB < required %.1f MiB)\n",
                   (double)lb / 1048576.0, (double)bytes / 1048576.0);
        }
    } else {
        // macOS / no /proc: fall back to trusting the mlock return value.
        verified = (mlock_rc == 0);
        printf("[prover] mlock rc ok but lock UNVERIFIED (no /proc; trusting return value)\n");
    }
    *locked = verified ? 1 : 0;

    mlockall(MCL_CURRENT | MCL_FUTURE); // best effort, also pins stacks/future maps
    return p;
}

// The weights this prover holds, when started with --weights. They stay
// mapped and locked for the life of the process, like an engine's weights.
struct Resident {
    SegmentTable table;
    uint64_t bytes = 0;
    int locked = 0;
};
static Resident g_res;
static bool g_have_res = false;

static void* map_prefaulted(size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return nullptr; }
    madvise(p, bytes, MADV_WILLNEED);
    volatile char* vp = (volatile char*)p;
    for (size_t off = 0; off < bytes; off += (size_t)g_page) vp[off] = 0;
    return p;
}

// Read each segment into its own allocation, lock them all, and verify the
// lock once over the total, as alloc_pinned() does for one buffer.
static bool load_resident(const char* manifest_path) {
    char err[512] = {0};
    Manifest m;
    if (!read_manifest(manifest_path, &m, err, sizeof err)) { fprintf(stderr, "[prover] %s\n", err); return false; }

    struct rlimit rl{ RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);

    std::vector<int> fds;
    for (const std::string& f : m.files) {
        int fd = open(f.c_str(), O_RDONLY);
        if (fd < 0) { fprintf(stderr, "[prover] cannot open %s: %s\n", f.c_str(), strerror(errno)); return false; }
        fds.push_back(fd);
    }
    std::vector<const uint64_t*> bases;
    std::vector<uint64_t> words;
    bool ok = true;
    for (const SegmentRun& r : segment_runs(m)) {
        void* p = map_prefaulted(r.bytes);
        if (!p) { ok = false; break; }
        size_t at = 0;
        for (size_t i = r.first; ok && i < r.end; ++i) {
            const Piece& pc = m.pieces[i];
            ssize_t got = pread(fds[pc.file], (char*)p + at, pc.bytes, (off_t)pc.offset);
            if (got != (ssize_t)pc.bytes) {
                fprintf(stderr, "[prover] short read of %s from %s\n", pc.segment.c_str(), m.files[pc.file].c_str());
                ok = false;
            }
            at += pc.bytes;
        }
        if (mlock(p, r.bytes) != 0)
            fprintf(stderr, "[prover] mlock failed (%s)\n", strerror(errno));
        bases.push_back((const uint64_t*)p);
        words.push_back(r.bytes / 8);
        g_res.bytes += r.bytes;
    }
    for (int fd : fds) close(fd);
    if (!ok) return false;
    if (!g_res.table.build(bases, words)) { fprintf(stderr, "[prover] cannot build the segment table\n"); return false; }

    size_t lb = locked_bytes();
    g_res.locked = (lb != (size_t)-1 && lb >= g_res.bytes) ? 1 : 0;
    printf("[prover] holding %zu segments, %.2f MiB, from %s; memory %s\n",
           bases.size(), g_res.bytes / 1048576.0, manifest_path,
           g_res.locked ? "LOCKED (verified)" : "NOT locked");
    mlockall(MCL_CURRENT | MCL_FUTURE);
    return true;
}

// The challenge loop, over whichever loader holds the buffer. Scratch for the
// tree fold lives outside the loop — allocating inside it would put malloc in
// the timed path, which is the one thing being measured.
template <typename Load>
static uint64_t answer_loop(int cfd, const Load& load, uint64_t num_words) {
    std::vector<uint64_t> scratch;
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        bool chained = (ch.flags & kFlagChained) != 0;
        bool tree    = (ch.flags & kFlagTreeFold) != 0;
        uint64_t hash;
        if (tree) {
            uint32_t nl = ch.lanes ? ch.lanes : 1u;
            if (scratch.size() < nl) scratch.resize(nl);
            answer_lanes_t(load, num_words, ch.nonce, ch.k, nl, chained, scratch.data());
            hash = tree_fold(scratch.data(), nl, ch.nonce, ch.k);
        } else {
            hash = answer_challenge_t(load, num_words, ch.nonce, ch.k, ch.lanes, chained);
        }
        Response resp{ hash };
        if (!send_all(cfd, &resp, sizeof(resp))) break;
        ++served;
    }
    return served;
}

static int make_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 8) < 0) { perror("listen"); exit(1); }
    return fd;
}

static void serve(int cfd) {
    set_low_latency(cfd);

    Hello hello{};
    if (!recv_all(cfd, &hello, sizeof(hello))) { fprintf(stderr, "[prover] no hello\n"); return; }
    if (hello.magic != kMagic) { fprintf(stderr, "[prover] bad magic\n"); return; }

    if (hello.flags & kHelloResident) {
        // No upload: answer from the weights loaded at start, if they are the
        // size the verifier expects. Closing the connection is the refusal.
        if (!g_have_res || hello.buffer_bytes != g_res.bytes) {
            fprintf(stderr, "[prover] refusing: asked to hold %llu bytes, holding %llu\n",
                    (unsigned long long)hello.buffer_bytes,
                    (unsigned long long)(g_have_res ? g_res.bytes : 0));
            return;
        }
        if (!g_res.locked && !g_allow_unlocked) {
            fprintf(stderr, "[prover] FATAL: weights not locked (need CAP_IPC_LOCK / `ulimit -l`, or pass --allow-unlocked)\n");
            exit(1);
        }
        Ready ready{ kMagic, (uint32_t)g_res.locked, 0 };
        if (!send_all(cfd, &ready, sizeof(ready))) return;
        uint64_t served = answer_loop(cfd, SegLoad{g_res.table.view()}, g_res.table.words());
        printf("[prover] served %llu challenges from the weights it holds.\n", (unsigned long long)served);
        return;
    }

    size_t bytes = (size_t)hello.buffer_bytes;
    bytes = (bytes + 7) & ~size_t(7);            // round to whole 64-bit words
    uint64_t num_words = bytes / 8;
    printf("[prover] receiving %.2f MiB (%llu words)...\n",
           bytes / 1048576.0, (unsigned long long)num_words);

    int locked = 0;
    void* raw = alloc_pinned(bytes, &locked);
    if (!raw) return;
    uint64_t* buf = (uint64_t*)raw;

    if (!recv_all(cfd, buf, bytes)) { fprintf(stderr, "[prover] short data\n"); munmap(raw, bytes); return; }
    printf("[prover] buffer resident; memory %s.\n", locked ? "LOCKED (verified)" : "NOT locked");

    // An unpinned buffer makes the honest baseline meaningless (pages can swap
    // out), so refuse by default; --allow-unlocked runs anyway and reports locked=false.
    if (!locked) {
        if (!g_allow_unlocked) {
            fprintf(stderr, "[prover] FATAL: memory not locked (need CAP_IPC_LOCK / `ulimit -l`, or pass --allow-unlocked)\n");
            exit(1);
        }
        fprintf(stderr, "[prover] WARNING: running UNLOCKED — baseline not pinned; reporting locked=false\n");
    }

    Ready ready{ kMagic, (uint32_t)locked, 0 };
    if (!send_all(cfd, &ready, sizeof(ready))) { munmap(raw, bytes); return; }

    // Challenge loop: answer as fast as possible.
    uint64_t served = answer_loop(cfd, BufLoad{buf}, num_words);
    printf("[prover] served %llu challenges; releasing buffer.\n", (unsigned long long)served);
    munlock(raw, bytes);
    munmap(raw, bytes);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffered logs
    long ps = sysconf(_SC_PAGESIZE); if (ps > 0) g_page = ps;
    uint16_t port = kDefaultPort;
    const char* weights = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--allow-unlocked")) {
            g_allow_unlocked = true;
        } else if (!strcmp(argv[i], "--weights") && i + 1 < argc) {
            weights = argv[++i];
        } else {
            int p = atoi(argv[i]);
            if (p > 0) port = (uint16_t)p;
        }
    }

    if (weights) {
        if (!load_resident(weights)) return 1;
        g_have_res = true;
    }

    int lfd = make_listener(port);
    printf("[prover] listening on 0.0.0.0:%u (page=%ld)\n", port, g_page);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        printf("[prover] client %s connected\n", inet_ntoa(ca.sin_addr));
        serve(cfd);
        close(cfd);
    }
    close(lfd);
    return 0;
}
