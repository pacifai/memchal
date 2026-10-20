// net.hpp — the wire protocol and the host-only plumbing around it.
//
// Split out of common.hpp so that challenge.hpp (the math) can be included from
// a CUDA translation unit. Everything here is host-only by nature: POSIX
// sockets, and /proc readers that report what the OS thinks this process is
// doing. None of it has a device analog.
//
// The challenge math lives in challenge.hpp. common.hpp includes both.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

namespace mc {

constexpr uint64_t kMagic       = 0x4D454D4348414C31ULL; // "MEMCHAL1"
constexpr uint16_t kDefaultPort = 9099;
constexpr uint32_t kFlagTerminate = 1u;
constexpr uint32_t kFlagChained   = 2u;
// Fold the per-lane hashes as a binary tree instead of a sequential chain.
// A different fold means a different answer, so this is a protocol change and
// carries its own known-answer vectors. See tree_fold() in challenge.hpp.
constexpr uint32_t kFlagTreeFold  = 4u;

// Hello.flags: the prover already holds the buffer — a model's weights, say,
// which it keeps for its own work — so no upload follows the Hello. The
// prover answers Ready only if what it holds is buffer_bytes long, and closes
// the connection otherwise. What the buffer *is* both sides take from the same
// manifest (weights.hpp); a prover holding other bytes of the right length
// gives wrong answers, which correctness catches. Older provers ignore the
// flag and wait for an upload that never comes.
constexpr uint32_t kHelloResident = 1u;

// Minimum effective chain depth (k / lanes) required when --chain dependent.
// At depth D, an attacker can pipeline across lanes and achieve ~1/D of the
// latency-hardness compared to a fully serial chain.  Below this floor the
// dependent access mode offers negligible defense.
constexpr uint32_t kMinChainDepth = 16u;

// Fixed-layout wire messages. Native byte order: this PoC assumes prover and
// verifier run on the same-endian machines (true for loopback / x86 / ARM LE).
#pragma pack(push, 1)
struct Hello     { uint64_t magic; uint64_t buffer_bytes; uint32_t version; uint32_t flags; };
struct Ready     { uint64_t magic; uint32_t locked; uint32_t pad; };
struct Challenge { uint64_t nonce; uint32_t k; uint32_t lanes; uint32_t flags; uint32_t pad; };
struct Response  { uint64_t hash; };
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Blocking socket helpers
// ---------------------------------------------------------------------------
static inline bool send_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}
static inline bool recv_all(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        ssize_t k = ::recv(fd, c, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}
static inline void set_low_latency(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // disable Nagle
}

// write() all n bytes to a (file) descriptor.
static inline bool write_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::write(fd, c, n);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}

// Bytes this process currently has locked (wired), as reported by the OS, so we
// can VERIFY mlock took effect instead of trusting its return value. Returns
// (size_t)-1 if it cannot be determined (e.g. no /proc, as on macOS). On Linux
// it reads VmLck from /proc/self/status.
static inline size_t locked_bytes() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return (size_t)-1;
    char line[256]; unsigned long kb = 0; bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmLck: %lu kB", &kb) == 1) { found = true; break; }
    }
    fclose(f);
    return found ? (size_t)kb * 1024 : (size_t)-1;
}

// Bytes this process has actually fetched from the block layer, per the OS
// (/proc/self/io read_bytes). Lets us check whether "disk" reads are genuinely
// cold (read_bytes grows) or page-cache hits (read_bytes ~ 0 after one sweep).
// Returns (size_t)-1 if unavailable (e.g. macOS).
static inline size_t io_read_bytes() {
    FILE* f = fopen("/proc/self/io", "r");
    if (!f) return (size_t)-1;
    char line[256]; unsigned long long b = 0; bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "read_bytes: %llu", &b) == 1) { found = true; break; }
    }
    fclose(f);
    return found ? (size_t)b : (size_t)-1;
}

// Major page faults this process has taken (faults that had to go to storage),
// from field 12 of /proc/self/stat. For the mmap adversary this is the direct
// counter of "probes that could NOT be served from the page cache": with the
// file cached it stays ~0; under memory pressure it grows with every probe.
// Returns (size_t)-1 if unavailable.
static inline size_t major_faults() {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return (size_t)-1;
    char buf[1024];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!got) return (size_t)-1;
    buf[got] = '\0';
    // comm (field 2) may contain spaces/parens; parse after the LAST ')'.
    char* p = strrchr(buf, ')');
    if (!p) return (size_t)-1;
    ++p;
    // Fields after comm: state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8)
    // flags(9) minflt(10) cminflt(11) majflt(12) -> the 10th token from here.
    unsigned long long v = 0;
    int field = 2; // we are positioned just after field 2
    char* tok = strtok(p, " ");
    while (tok) {
        ++field;
        if (field == 12) { v = strtoull(tok, nullptr, 10); return (size_t)v; }
        tok = strtok(nullptr, " ");
    }
    return (size_t)-1;
}

} // namespace mc
