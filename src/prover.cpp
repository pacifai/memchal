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
#include "common.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/resource.h>
#include <chrono>

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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

    if (hello.version > kWireVersion) {
        fprintf(stderr, "[prover] unsupported wire version %u (max %u)\n",
                hello.version, kWireVersion);
        return;
    }
    const bool build = (hello.flags & kFlagBuild) != 0;

    size_t bytes = (size_t)hello.buffer_bytes;
    bytes = (bytes + 7) & ~size_t(7);            // round to whole 64-bit words
    uint64_t num_words = bytes / 8;

    RecipeMsg rmsg{};
    if (build && !recv_all(cfd, &rmsg, sizeof(rmsg))) {
        fprintf(stderr, "[prover] no recipe\n"); return;
    }
    printf("[prover] %s %.2f MiB (%llu words)...\n",
           build ? "building" : "receiving", bytes / 1048576.0,
           (unsigned long long)num_words);

    int locked = 0;
    void* raw = alloc_pinned(bytes, &locked);
    if (!raw) return;
    uint64_t* buf = (uint64_t*)raw;

    // Build into the locked region, so the matrix is pinned as it is written and
    // never lands in swap between construction and the first challenge.
    if (build) {
        const Recipe r = recipe_from_msg(rmsg);
        if (build_buffer_words(r) != num_words) {
            fprintf(stderr, "[prover] recipe yields %llu words, hello says %llu\n",
                    (unsigned long long)build_buffer_words(r),
                    (unsigned long long)num_words);
            munmap(raw, bytes); return;
        }
        const double t0 = now_s();
        if (!build_buffer(buf, num_words, r)) {
            fprintf(stderr, "[prover] build failed (m=%u t=%u p=%u)\n", r.m_kib, r.t, r.p);
            munmap(raw, bytes); return;
        }
        printf("[prover] built in %.3f s (m=%u KiB t=%u p=%u)\n",
               now_s() - t0, r.m_kib, r.t, r.p);
    } else if (!recv_all(cfd, buf, bytes)) {
        fprintf(stderr, "[prover] short data\n"); munmap(raw, bytes); return;
    }
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
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        bool chained = (ch.flags & kFlagChained) != 0;
        Response resp{ answer_challenge(buf, num_words, ch.nonce, ch.k, ch.lanes, chained) };
        if (!send_all(cfd, &resp, sizeof(resp))) break;
        ++served;
    }
    printf("[prover] served %llu challenges; releasing buffer.\n", (unsigned long long)served);
    munlock(raw, bytes);
    munmap(raw, bytes);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffered logs
    long ps = sysconf(_SC_PAGESIZE); if (ps > 0) g_page = ps;
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--allow-unlocked")) {
            g_allow_unlocked = true;
        } else {
            int p = atoi(argv[i]);
            if (p > 0) port = (uint16_t)p;
        }
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
