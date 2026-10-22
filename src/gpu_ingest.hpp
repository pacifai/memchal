// gpu_ingest.hpp — how the GPU provers get the buffer onto the card, for both
// wire protocols. Host only: it includes net.hpp (sockets, Argon2d), so never
// include it from a .cu file.
//
// Upload mode (wire version 1, or 2 without kFlagBuild): the verifier streams
// the bytes and the prover recv()s them chunk by chunk into pinned staging.
//
// Build mode (kFlagBuild): a 92-byte RecipeMsg follows the Hello. The prover
// builds the Argon2d matrix ON THE HOST CPU into a temporary allocation, then
// feeds it through the same staging/push loop the upload uses, then frees the
// host copy. This is the intended first step, not the end state: it needs one
// full host copy of the buffer for the length of the build and the copy, and it
// leaves the GPU idle while the CPU builds. Building on the GPU itself is
// future work (see ADR 0007's point on GPU parallelism).
//
// Either way, every byte reaches the device through gpu::push(), which already
// handles every allocation kind (Device, Managed with its prefetch, HostMapped,
// Split, and fake's discarded tail). So there is one ingestion loop.
#pragma once
#include "common.hpp"
#include "gpu_answer.hpp"
#include <chrono>
#include <cstdlib>

namespace mc {

// A buffer built on the host from a recipe. Free it with host_build_free().
struct HostBuild {
    Recipe    recipe{};
    uint64_t* buf     = nullptr;
    size_t    bytes   = 0;
    double    build_s = 0.0;
};

static inline void host_build_free(HostBuild& hb) {
    free(hb.buf);
    hb.buf = nullptr;
}

// Reads the RecipeMsg that follows a build-mode Hello, checks it against the
// size the Hello announced, and builds the buffer into host memory. On failure,
// writes the reason to err and holds no memory.
static inline bool host_build_from_wire(int cfd, const Hello& hello, HostBuild& hb,
                                        char* err, size_t errlen) {
    RecipeMsg rmsg{};
    if (!recv_all(cfd, &rmsg, sizeof rmsg)) {
        snprintf(err, errlen, "no recipe after a build-mode hello");
        return false;
    }
    hb.recipe = recipe_from_msg(rmsg);
    const uint64_t want = build_buffer_bytes(hb.recipe);
    if (hello.buffer_bytes != want) {
        snprintf(err, errlen, "refusing recipe: it builds %llu bytes (m=%u KiB t=%u p=%u) "
                              "but the hello announced %llu",
                 (unsigned long long)want, hb.recipe.m_kib, hb.recipe.t, hb.recipe.p,
                 (unsigned long long)hello.buffer_bytes);
        return false;
    }
    hb.bytes = (size_t)want;
    hb.buf = (uint64_t*)malloc(hb.bytes ? hb.bytes : 1);
    if (!hb.buf) {
        snprintf(err, errlen, "cannot allocate %.1f MiB of host memory to build in",
                 hb.bytes / 1048576.0);
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!build_buffer(hb.buf, hb.bytes / 8, hb.recipe)) {
        snprintf(err, errlen, "build failed (m=%u KiB t=%u p=%u)",
                 hb.recipe.m_kib, hb.recipe.t, hb.recipe.p);
        host_build_free(hb);
        return false;
    }
    hb.build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// Moves `bytes` from src (the socket, or a built host buffer) onto the device
// through pinned staging. On failure, writes the reason to err.
static inline bool ingest(gpu::Prover& p, Source& src, size_t bytes,
                          char* err, size_t errlen) {
    size_t cap = 0;
    unsigned char* stage = gpu::staging(p, &cap);
    size_t left = bytes;
    while (left) {
        size_t chunk = left < cap ? left : cap;
        if (!src_read(src, stage, chunk)) {
            snprintf(err, errlen, "short data (%zu bytes outstanding)", left);
            return false;
        }
        if (!gpu::push(p, chunk, err, errlen)) return false;
        left -= chunk;
    }
    return true;
}

} // namespace mc
