// pinned.hpp — host memory locked into RAM, for the CPU provers (ADR 0002).
#pragma once
#include "net.hpp"
#include <sys/mman.h>
#include <sys/resource.h>

namespace mc {

struct Pinned {
    uint64_t* words  = nullptr;
    size_t    bytes  = 0;
    bool      locked = false;
};

// Maps `bytes`, touches every page, and locks it. `locked` is what the kernel
// reports in VmLck, not what mlock() returned; where there is no /proc to ask,
// it falls back to mlock()'s return value.
static inline bool pin(Pinned& m, size_t bytes) {
    m = Pinned{};
    struct rlimit rl{ RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return false; }
    madvise(p, bytes, MADV_WILLNEED);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    volatile char* vp = (volatile char*)p;
    for (size_t off = 0; off < bytes; off += page) vp[off] = 0;
    const bool mlock_ok = mlock(p, bytes) == 0;
    const size_t lb = locked_bytes();
    m.words  = (uint64_t*)p;
    m.bytes  = bytes;
    m.locked = lb != (size_t)-1 ? lb >= bytes : mlock_ok;
    return true;
}

static inline void unpin(Pinned& m) {
    if (!m.words) return;
    munlock(m.words, m.bytes);
    munmap(m.words, m.bytes);
    m = Pinned{};
}

} // namespace mc
