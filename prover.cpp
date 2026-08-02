// prover.cpp — receives a large random buffer, pins it in RAM, and answers
// memory challenges. The design keeps the answer path GPU-portable: a flat
// word buffer plus the pure functions in common.hpp.
//
// Memory pinning: we mmap an anonymous region, prefault every page, and mlock()
// it so the kernel may never swap it out. On a box without CAP_IPC_LOCK / a high
// `ulimit -l`, mlock() fails and we continue unlocked (still correct, just no
// anti-swap guarantee) after printing why.
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

using namespace mc;

static long g_page = 4096;

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

    if (mlock(p, bytes) == 0) {
        *locked = 1;
    } else {
        fprintf(stderr, "[prover] mlock failed (%s); continuing UNLOCKED. "
                        "Raise `ulimit -l` or grant CAP_IPC_LOCK for anti-swap.\n",
                strerror(errno));
    }
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
    printf("[prover] buffer resident; memory %s.\n", locked ? "LOCKED (mlock ok)" : "NOT locked");

    Ready ready{ kMagic, (uint32_t)locked, 0 };
    if (!send_all(cfd, &ready, sizeof(ready))) { munmap(raw, bytes); return; }

    // Challenge loop: answer as fast as possible.
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        Response resp{ answer_challenge(buf, num_words, ch.nonce, ch.k, ch.lanes) };
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
    if (argc > 1) port = (uint16_t)atoi(argv[1]);

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
