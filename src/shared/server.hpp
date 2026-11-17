#pragma once
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <vector>
#include "shared/protocol.hpp"
#include "shared/socket.hpp"
#include "shared/weights.hpp"

namespace mc {

// Accepts one verifier at a time and hands its socket to `serve`. A client that
// closes before sending anything, such as a port check, is dropped silently, and
// one that goes quiet for kIdleTimeoutS is dropped so it can't hold the prover.
// Returns only if accept() fails.
template <typename Serve>
static inline int serve_forever(const char* tag, uint16_t port, Serve&& serve) {
    int lfd = listen_on(port);
    printf("[%s] listening on 0.0.0.0:%u\n", tag, port);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        const timeval idle{kIdleTimeoutS, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
        char first;
        if (recv(cfd, &first, 1, MSG_PEEK) <= 0) { close(cfd); continue; }
        printf("[%s] client %s connected\n", tag, inet_ntoa(ca.sin_addr));
        set_no_delay(cfd);
        serve(cfd);
        close(cfd);
    }
    close(lfd);
    return 1;
}

// The weights a prover started with --weights PATH holds, or null when path is
// null. Exits if the manifest can't be read.
static inline const Manifest* held_weights(const char* path, const char* tag) {
    if (!path) return nullptr;
    static Manifest m;
    char err[512];
    if (!read_manifest(path, m, err, sizeof err)) {
        fprintf(stderr, "[%s] FATAL: %s\n", tag, err);
        exit(1);
    }
    printf("[%s] weights: %.2f MiB in %zu pieces, from %s\n", tag, m.bytes / 1048576.0,
           m.pieces.size(), path);
    return &m;
}

struct Setup {
    bool            build        = false;
    Recipe          recipe{};
    size_t          bytes        = 0;
    uint64_t        num_words    = 0;
    const Manifest* weights      = nullptr;
    uint64_t        weight_bytes = 0;

    size_t   filler_bytes() const { return bytes - (size_t)weight_bytes; }
    uint64_t filler_words() const { return filler_bytes() / 8; }
};

// Checks that `held`, the weights this prover was started with, are the ones
// the verifier named.
static inline bool weights_match(const WeightsMsg& w, const Manifest* held, uint64_t buffer_bytes,
                                 char* err, size_t n) {
    if (!held) { snprintf(err, n, "the verifier named model weights, and this prover holds none"); return false; }
    if (memcmp(w.digest, held->digest, sizeof w.digest) != 0) {
        snprintf(err, n, "the verifier's weights manifest differs from this prover's");
        return false;
    }
    if (w.bytes != held->bytes || w.bytes > buffer_bytes) {
        snprintf(err, n, "the verifier named %llu bytes of weights in a %llu-byte buffer; this "
                         "prover holds %llu", (unsigned long long)w.bytes,
                 (unsigned long long)buffer_bytes, (unsigned long long)held->bytes);
        return false;
    }
    return true;
}

// Reads the verifier's hello, the weights message and the recipe that may
// follow it, and checks them against the weights this prover holds, if any.
static inline bool read_setup(int cfd, const Manifest* held, Setup& s, char* err, size_t n) {
    Hello h{};
    if (!recv_all(cfd, &h, sizeof h)) { snprintf(err, n, "no hello"); return false; }
    if (!hello_ok(h, err, n)) return false;
    s.build = (h.flags & kHelloBuild) != 0;
    s.bytes = ((size_t)h.buffer_bytes + 7) & ~size_t(7);
    s.num_words = s.bytes / 8;
    if (h.flags & kHelloWeights) {
        WeightsMsg w{};
        if (!recv_all(cfd, &w, sizeof w)) { snprintf(err, n, "no weights message"); return false; }
        if (!weights_match(w, held, h.buffer_bytes, err, n)) return false;
        s.weights = held;
        s.weight_bytes = w.bytes;
    }
    if (!s.build) return true;

    RecipeMsg m{};
    if (!recv_all(cfd, &m, sizeof m)) { snprintf(err, n, "no recipe after a build-mode hello"); return false; }
    s.recipe = recipe_from_msg(m);
    if (!recipe_ok(s.recipe, err, n)) return false;
    const uint64_t want = build_buffer_bytes(s.recipe);
    if (want != s.filler_bytes()) {
        snprintf(err, n, "recipe builds %llu bytes but the hello leaves %llu for filler",
                 (unsigned long long)want, (unsigned long long)s.filler_bytes());
        return false;
    }
    return true;
}

// Where a prover reads the buffer from: the weights it holds, then the filler
// from the socket or from a host copy built from the recipe.
struct Source {
    int           fd      = -1;
    uint64_t*     built   = nullptr;
    size_t        offset  = 0;
    double        build_s = 0.0;
    WeightsReader weights{};
};

static inline void free_built(Source& src) {
    free(src.built);
    src.built = nullptr;
}

// Builds the filler on the host. The caller frees it with free_built().
static inline bool build_on_host(Source& src, const Setup& s, char* err, size_t n) {
    src.built = (uint64_t*)malloc(s.filler_bytes());
    if (!src.built) {
        snprintf(err, n, "cannot allocate %.1f MiB of host memory to build in",
                 s.filler_bytes() / 1048576.0);
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!build_buffer(src.built, s.filler_words(), s.recipe)) {
        snprintf(err, n, "build failed");
        free_built(src);
        return false;
    }
    src.build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// Opens the weights and, in build mode, builds the filler.
static inline bool open_source(Source& src, const Setup& s, char* err, size_t n) {
    if (s.weights && !src.weights.open(*s.weights, err, n)) return false;
    return !s.build || build_on_host(src, s, err, n);
}

static inline bool src_read(Source& s, void* dst, size_t n) {
    unsigned char* out = (unsigned char*)dst;
    const size_t from_weights = (size_t)std::min<uint64_t>(n, s.weights.left());
    if (from_weights && !s.weights.read(out, from_weights)) return false;
    out += from_weights;
    n -= from_weights;
    if (!n) return true;
    if (!s.built) return recv_all(s.fd, out, n);
    memcpy(out, (const uint8_t*)s.built + s.offset, n);
    s.offset += n;
    return true;
}

static inline bool src_skip(Source& s, size_t n) {
    std::vector<char> chunk(1 << 16);
    while (n && (s.weights.left() || !s.built)) {
        const size_t take = std::min(n, chunk.size());
        if (!src_read(s, chunk.data(), take)) return false;
        n -= take;
    }
    s.offset += n;
    return true;
}

static inline bool send_ready(int cfd, bool locked) {
    Ready r{ kMagic, locked ? 1u : 0u, 0 };
    return send_all(cfd, &r, sizeof r);
}

// Answers challenges until the verifier terminates or hangs up, and returns
// how many were answered.
//   answer: (challenge, &hash) -> false to stop.
//   after:  (challenge, served) -> false to stop; runs after the response is sent.
template <typename Answer, typename After>
static inline uint64_t answer_challenges(int cfd, const char* tag, Answer&& answer, After&& after) {
    char err[256];
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof ch) || (ch.flags & kChallengeTerminate)) break;
        if (!challenge_ok(ch, err, sizeof err)) { fprintf(stderr, "[%s] refusing: %s\n", tag, err); break; }
        Response r{};
        if (!answer(ch, &r.hash)) break;
        if (!send_all(cfd, &r, sizeof r)) break;
        if (!after(ch, ++served)) break;
    }
    return served;
}

} // namespace mc
