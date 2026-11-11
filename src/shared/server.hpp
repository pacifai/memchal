#pragma once
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include "shared/protocol.hpp"
#include "shared/socket.hpp"

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

struct Setup {
    bool     build     = false;
    Recipe   recipe{};
    size_t   bytes     = 0;
    uint64_t num_words = 0;
};

// Reads the verifier's hello, and the recipe that follows it in build mode.
static inline bool read_setup(int cfd, Setup& s, char* err, size_t n) {
    Hello h{};
    if (!recv_all(cfd, &h, sizeof h)) { snprintf(err, n, "no hello"); return false; }
    if (!hello_ok(h, err, n)) return false;
    s.build = (h.flags & kHelloBuild) != 0;
    s.bytes = ((size_t)h.buffer_bytes + 7) & ~size_t(7);
    s.num_words = s.bytes / 8;
    if (!s.build) return true;

    RecipeMsg m{};
    if (!recv_all(cfd, &m, sizeof m)) { snprintf(err, n, "no recipe after a build-mode hello"); return false; }
    s.recipe = recipe_from_msg(m);
    if (!recipe_ok(s.recipe, err, n)) return false;
    const uint64_t want = build_buffer_bytes(s.recipe);
    if (want != h.buffer_bytes) {
        snprintf(err, n, "recipe builds %llu bytes but the hello announced %llu",
                 (unsigned long long)want, (unsigned long long)h.buffer_bytes);
        return false;
    }
    return true;
}

// Where a prover reads the buffer from: the socket, or a host copy built from
// the recipe.
struct Source {
    int       fd      = -1;
    uint64_t* built   = nullptr;
    size_t    offset  = 0;
    double    build_s = 0.0;
};

static inline void free_built(Source& src) {
    free(src.built);
    src.built = nullptr;
}

static inline bool build_on_host(Source& src, const Setup& s, char* err, size_t n) {
    src.built = (uint64_t*)malloc(s.bytes);
    if (!src.built) {
        snprintf(err, n, "cannot allocate %.1f MiB of host memory to build in", s.bytes / 1048576.0);
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!build_buffer(src.built, s.num_words, s.recipe)) {
        snprintf(err, n, "build failed");
        free_built(src);
        return false;
    }
    src.build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

static inline bool src_read(Source& s, void* dst, size_t n) {
    if (!s.built) return recv_all(s.fd, dst, n);
    memcpy(dst, (const uint8_t*)s.built + s.offset, n);
    s.offset += n;
    return true;
}

static inline bool src_skip(Source& s, size_t n) {
    if (s.built) { s.offset += n; return true; }
    char chunk[1 << 16];
    while (n) {
        size_t take = n < sizeof chunk ? n : sizeof chunk;
        if (!recv_all(s.fd, chunk, take)) return false;
        n -= take;
    }
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
