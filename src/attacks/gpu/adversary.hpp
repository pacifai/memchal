#pragma once
#include "gpu/answer.hpp"

namespace mc {
namespace gpu {

enum class Cheat { Fake, Managed, Partial, Recompute };

static constexpr uint32_t kRecomputeBlockWords = 128;

struct RecomputeState {
    int32_t*  d_slot       = nullptr;  // >= 0: kept index; -1: missing; <= -2: rebuilt index -2 - slot
    uint32_t  n_blocks     = 0;
    uint64_t* d_scratch    = nullptr;
    uint32_t* d_scratch_id = nullptr;
    uint32_t  scratch_cap  = 0;
    uint32_t  scratch_used = 0;
    uint32_t* d_need       = nullptr;
    uint32_t* d_nneed      = nullptr;
    uint8_t*  d_done       = nullptr;
    uint32_t* h_need       = nullptr;
    uint64_t* h_rebuilt    = nullptr;
};

struct Adversary {
    Cheat     cheat     = Cheat::Fake;
    uint64_t  num_words = 0;
    uint64_t* d_buf     = nullptr;    // words [0, dev_words)
    uint64_t  dev_words = 0;
    uint64_t* d_host    = nullptr;    // words [dev_words, num_words), in host memory
    void*     h_host    = nullptr;
    Lanes     lanes;
    Staging   stage;
    RecomputeState rc;
};

// Holds held_fraction of the buffer on the card and invents the rest.
bool create_fake(Adversary& a, uint64_t num_words, double held_fraction, uint32_t lane_cap,
                 char* err, size_t errlen);
// Holds the buffer in managed memory, which pages between host and card.
bool create_managed(Adversary& a, uint64_t num_words, uint32_t lane_cap, char* err, size_t errlen);
// Holds offload_fraction of the buffer in pinned host memory and the rest on the card.
bool create_partial(Adversary& a, uint64_t num_words, double offload_fraction, uint32_t lane_cap,
                    char* err, size_t errlen);

// Holds the kept Argon2d blocks on the card and rebuilds the rest on demand.
//   slot: for each of n_blocks blocks, its index in kept_words, or -1 if discarded.
bool create_recompute(Adversary& a, uint32_t n_blocks, const int32_t* slot,
                      const uint64_t* kept_words, uint32_t n_kept, uint32_t lane_cap,
                      char* err, size_t errlen);

void destroy(Adversary& a);

// Copies the next `bytes` from a.stage.h into the buffer, dropping the words
// a fake buffer does not hold.
bool push(Adversary& a, size_t bytes, char* err, size_t errlen);

bool answer(Adversary& a, uint64_t nonce, uint32_t samples, uint32_t lanes, bool chained, bool tree,
            uint64_t* out_hash, Timing* t, char* err, size_t errlen);

// Rebuilds blocks[0, n) into out, kRecomputeBlockWords words each. Returning
// false aborts the answer.
using RebuildFn = bool (*)(void* ctx, const uint32_t* blocks, uint32_t n, uint64_t* out);

struct RecomputeStats {
    uint32_t rounds     = 0;
    uint32_t rebuilt    = 0;
    double   rebuild_ms = 0;
};

// Answers with a recompute buffer. Each round runs the unfinished lanes until
// they finish or reach a missing block, then rebuilds the missing blocks on the
// host. kernel_ms covers every round, rebuilds included.
bool answer_recompute(Adversary& a, uint64_t nonce, uint32_t samples, uint32_t lanes, bool chained,
                      bool tree, RebuildFn rebuild, void* ctx, uint64_t* out_hash, Timing* t,
                      RecomputeStats* st, char* err, size_t errlen);

// Marks the blocks rebuilt for the last challenge missing again.
bool recompute_forget(Adversary& a, char* err, size_t errlen);

const char* residency(const Adversary& a, bool* is_device);

// The fraction of the buffer allocated in device memory. Managed reports 1.0.
double resident_fraction(const Adversary& a);

// Allocates `bytes` of device memory and leaves it unused.
bool reserve_hbm(size_t bytes, char* err, size_t errlen);
void release_hbm();

} // namespace gpu
} // namespace mc
