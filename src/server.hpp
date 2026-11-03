// server.hpp — the parts every prover shares: listening, the opening exchange,
// reading the buffer in, and the challenge loop. Host only.
#pragma once
#include "challenge.hpp"
#include "net.hpp"
#include <chrono>
#include <vector>

namespace mc {

static inline int listen_on(uint16_t port) {
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

// Serves one verifier at a time until accept() fails.
template <typename Serve>
static inline int serve_forever(const char* tag, uint16_t port, Serve&& serve) {
    int lfd = listen_on(port);
    printf("[%s] listening on 0.0.0.0:%u\n", tag, port);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        printf("[%s] client %s connected\n", tag, inet_ntoa(ca.sin_addr));
        set_low_latency(cfd);
        serve(cfd);
        close(cfd);
    }
    close(lfd);
    return 1;
}

// What the verifier asked for: a buffer size, and in build mode the recipe.
struct Setup {
    bool     build     = false;
    Recipe   recipe{};
    size_t   bytes     = 0;
    uint64_t num_words = 0;
};

static inline bool read_setup(int cfd, Setup& s, char* err, size_t n) {
    Hello h{};
    if (!recv_all(cfd, &h, sizeof h)) { snprintf(err, n, "no hello"); return false; }
    if (!hello_ok(h, err, n)) return false;
    s.build = (h.flags & kFlagBuild) != 0;
    s.bytes = ((size_t)h.buffer_bytes + 7) & ~size_t(7);
    s.num_words = s.bytes / 8;
    if (!s.build) return true;

    RecipeMsg m{};
    if (!recv_all(cfd, &m, sizeof m)) { snprintf(err, n, "no recipe after a build-mode hello"); return false; }
    s.recipe = recipe_from_msg(m);
    if (!recipe_ok(s.recipe, err, n)) return false;
    const uint64_t want = build_buffer_bytes(s.recipe);
    if (want != h.buffer_bytes) {
        snprintf(err, n, "recipe builds %llu bytes (m=%u KiB t=%u p=%u) but the hello announced %llu",
                 (unsigned long long)want, s.recipe.m_kib, s.recipe.t, s.recipe.p,
                 (unsigned long long)h.buffer_bytes);
        return false;
    }
    return true;
}

// Where a prover reads the buffer's bytes from, in order: the socket, or a
// copy that build_on_host() made.
struct Source {
    int       fd      = -1;
    uint64_t* built   = nullptr;
    size_t    off     = 0;
    double    build_s = 0.0;
};

static inline void free_built(Source& src) {
    free(src.built);
    src.built = nullptr;
}

// Builds the whole buffer from the recipe into host memory, so src_read()
// returns it in place of the socket. Argon2d cannot build part of a buffer, so
// even a prover that keeps none of it holds all of it here.
static inline bool build_on_host(Source& src, const Setup& s, char* err, size_t n) {
    src.built = (uint64_t*)malloc(s.bytes);
    if (!src.built) {
        snprintf(err, n, "cannot allocate %.1f MiB of host memory to build in", s.bytes / 1048576.0);
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!build_buffer(src.built, s.num_words, s.recipe)) {
        snprintf(err, n, "build failed (m=%u KiB t=%u p=%u)", s.recipe.m_kib, s.recipe.t, s.recipe.p);
        free_built(src);
        return false;
    }
    src.build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

static inline bool src_read(Source& s, void* dst, size_t n) {
    if (!s.built) return recv_all(s.fd, dst, n);
    memcpy(dst, (const uint8_t*)s.built + s.off, n);
    s.off += n;
    return true;
}

static inline bool src_skip(Source& s, size_t n) {
    if (s.built) { s.off += n; return true; }
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

static inline bool is_chained(const Challenge& ch) { return (ch.flags & kFlagChained) != 0; }
static inline bool is_tree(const Challenge& ch)    { return (ch.flags & kFlagTreeFold) != 0; }

// Answers challenges until the verifier terminates or hangs up, and returns
// how many were answered. answer(ch, &hash) and after(ch, served) return false
// to stop. after() runs once the response is sent, outside the verifier's timer.
template <typename Answer, typename After>
static inline uint64_t answer_challenges(int cfd, const char* tag, Answer&& answer, After&& after) {
    char err[256];
    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof ch) || (ch.flags & kFlagTerminate)) break;
        if (!challenge_ok(ch, err, sizeof err)) { fprintf(stderr, "[%s] refusing: %s\n", tag, err); break; }
        Response r{};
        if (!answer(ch, &r.hash)) break;
        if (!send_all(cfd, &r, sizeof r)) break;
        if (!after(ch, ++served)) break;
    }
    return served;
}

// Answers ch on the host, reading words through `load`. scratch holds the lane
// hashes for the tree fold; it lives with the caller so that the timed path
// allocates only when the lane count grows.
template <typename Load>
static inline uint64_t answer_on_host(Load&& load, uint64_t num_words, const Challenge& ch,
                                      std::vector<uint64_t>& scratch) {
    if (!is_tree(ch)) return answer_challenge_t(load, num_words, ch.nonce, ch.k, ch.lanes, is_chained(ch));
    const uint32_t lanes = ch.lanes ? ch.lanes : 1u;
    if (scratch.size() < lanes) scratch.resize(lanes);
    return answer_challenge_tree_t(load, num_words, ch.nonce, ch.k, lanes, is_chained(ch),
                                   scratch.data());
}

static inline void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

} // namespace mc
