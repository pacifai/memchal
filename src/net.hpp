// net.hpp — the wire protocol and the host-only plumbing around it.
//
// Split out of common.hpp so that challenge.hpp (the math) can be included from
// a CUDA translation unit. Everything here is host-only by nature: POSIX
// sockets, the build-buffer recipe and the Argon2d generator behind it
// (argon2d.hpp, which uses std::thread), and /proc readers that report what the
// OS thinks this process is doing. None of it has a device analog.
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
#include "argon2d.hpp"   // Recipe and build_buffer(); host-only (std::thread)

namespace mc {

constexpr uint64_t kMagic       = 0x4D454D4348414C31ULL; // "MEMCHAL1"
constexpr uint16_t kDefaultPort = 9099;
// Challenge.flags and Hello.flags are separate bit spaces: kFlagTreeFold and
// kFlagBuild share the value 4 but never appear in the same field.
constexpr uint32_t kFlagTerminate = 1u;   // Challenge.flags
constexpr uint32_t kFlagChained   = 2u;   // Challenge.flags
// Challenge.flags: fold the per-lane hashes as a binary tree instead of a sequential chain.
// A different fold means a different answer, so this is a protocol change and
// carries its own known-answer vectors. See tree_fold() in challenge.hpp.
constexpr uint32_t kFlagTreeFold  = 4u;   // Challenge.flags
constexpr uint32_t kFlagBuild     = 4u;   // Hello.flags: a Recipe follows, not bytes

// Wire version. 1 = the verifier uploads the buffer. 2 = it may instead send a
// Recipe and both sides build (ADR 0007). A version-1 Hello never carries
// kFlagBuild, so a version-2 prover still serves a version-1 verifier.
constexpr uint32_t kWireVersion = 2u;

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
// Sent immediately after a Hello carrying kFlagBuild, in place of the buffer.
struct RecipeMsg { uint8_t key[32]; uint8_t salt[16];
                   uint32_t m_kib; uint32_t t; uint32_t p;
                   uint8_t workload_tag[32]; };
#pragma pack(pop)
static_assert(sizeof(RecipeMsg) == 92, "RecipeMsg is a fixed 92-byte wire layout");

static inline Recipe recipe_from_msg(const RecipeMsg& m) {
    Recipe r;
    memcpy(r.key, m.key, sizeof r.key);
    memcpy(r.salt, m.salt, sizeof r.salt);
    memcpy(r.workload_tag, m.workload_tag, sizeof r.workload_tag);
    r.m_kib = m.m_kib; r.t = m.t; r.p = m.p;
    return r;
}
static inline RecipeMsg msg_from_recipe(const Recipe& r) {
    RecipeMsg m{};
    memcpy(m.key, r.key, sizeof m.key);
    memcpy(m.salt, r.salt, sizeof m.salt);
    memcpy(m.workload_tag, r.workload_tag, sizeof m.workload_tag);
    m.m_kib = r.m_kib; m.t = r.t; m.p = r.p;
    return m;
}

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
// Where a prover's buffer bytes come from. Every mode ingests the buffer through
// src_read(), so the send-buffer and build-buffer paths differ in one place
// rather than in each mode's setup.
//
// Build mode materializes the whole matrix up front, because Argon2 cannot
// produce part of it: a prover that intends to spill to disk still needs
// buffer-sized RAM for the duration of the build.
struct Source {
    int             fd    = -1;       // socket, when receiving
    const uint64_t* built = nullptr;  // whole built buffer, when building
    size_t          off   = 0;        // read cursor into `built`
};
static inline bool src_read(Source& s, void* dst, size_t n) {
    if (!s.built) return recv_all(s.fd, dst, n);
    memcpy(dst, (const uint8_t*)s.built + s.off, n);
    s.off += n;
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
