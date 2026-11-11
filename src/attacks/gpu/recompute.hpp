#pragma once
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <random>
#include <sched.h>
#include <thread>
#include <vector>
#include "attacks/gpu/adversary.hpp"
#include "attacks/argon2d_rebuild.hpp"
#include "shared/server.hpp"
#include "shared/thread_pool.hpp"

namespace mc {

static_assert(gpu::kRecomputeBlockWords == a2::kBlockWords,
              "the CUDA loader and Argon2d disagree on the block size");

struct RecomputeOptions {
    double   keep      = 0.99;
    uint64_t keep_seed = 1;
    uint64_t budget    = 1u << 16;
    uint32_t threads   = 0;
};

// The host side of the recompute cheat: the kept blocks and one Rebuilder per
// worker thread.
struct RecomputeHost {
    MatrixShape shape;
    std::vector<int32_t>  slot;
    std::vector<uint64_t> kept;
    std::vector<std::unique_ptr<Rebuilder>> rebuilders;
    std::unique_ptr<ThreadPool> pool;

    uint32_t n_kept() const { return (uint32_t)(kept.size() / a2::kBlockWords); }

    uint64_t total(uint64_t Rebuilder::Stats::*field) const {
        uint64_t t = 0;
        for (const auto& r : rebuilders) t += r->stats().*field;
        return t;
    }
};

static inline uint32_t available_cores() {
    cpu_set_t set;
    CPU_ZERO(&set);
    uint32_t n = sched_getaffinity(0, sizeof set, &set) == 0 ? (uint32_t)CPU_COUNT(&set)
                                                             : std::thread::hardware_concurrency();
    return n ? n : 1;
}

// Keeps a random opt.keep of the blocks of the built matrix and sets up the
// rebuild workers.
static inline bool recompute_setup(RecomputeHost& h, const Setup& s, const uint64_t* built,
                                   const RecomputeOptions& opt, char* err, size_t errlen) {
    if (!matrix_shape(s.recipe, h.shape)) {
        snprintf(err, errlen, "recipe has no valid Argon2d shape");
        return false;
    }
    std::mt19937_64 rng(opt.keep_seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    h.slot.assign(h.shape.blocks, -1);
    uint32_t n_kept = 0;
    for (uint32_t b = 0; b < h.shape.blocks; ++b)
        if (u(rng) < opt.keep) h.slot[b] = (int32_t)n_kept++;
    h.kept.resize((size_t)n_kept * a2::kBlockWords);
    for (uint32_t b = 0; b < h.shape.blocks; ++b)
        if (h.slot[b] >= 0)
            memcpy(h.kept.data() + (size_t)h.slot[b] * a2::kBlockWords,
                   built + (size_t)b * a2::kBlockWords, a2::kBlockBytes);

    h.pool.reset(new ThreadPool(opt.threads ? opt.threads : available_cores()));
    for (uint32_t w = 0; w < h.pool->size(); ++w)
        h.rebuilders.emplace_back(new Rebuilder(h.shape, KeptBlocks{h.slot.data(), h.kept.data()},
                                                opt.budget));
    return true;
}

// A gpu::RebuildFn over a RecomputeHost. A block that runs out of budget is
// returned as zeros. Returns false if a rebuild throws.
static inline bool rebuild_blocks(void* ctx, const uint32_t* ids, uint32_t n, uint64_t* out) {
    RecomputeHost& h = *(RecomputeHost*)ctx;
    try {
        h.pool->run(n, [&](uint32_t w, uint32_t i) {
            uint64_t* dst = out + (size_t)i * a2::kBlockWords;
            if (!h.rebuilders[w]->rebuild(ids[i], dst)) memset(dst, 0, a2::kBlockBytes);
        });
    } catch (const std::exception& e) {
        fprintf(stderr, "[cheat] rebuild failed: %s\n", e.what());
        return false;
    }
    return true;
}

} // namespace mc
