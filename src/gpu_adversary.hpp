// gpu_adversary.hpp — the GPU adversaries' buffers. Each one answers through
// the same lane kernel as the honest prover and differs only in where its
// words live. gpu_adversary.cu implements it.
#pragma once
#include "gpu_answer.hpp"

namespace mc {
namespace gpu {

enum class Cheat {
    Fake,       // holds a fraction of the buffer on the card and invents the rest
    Managed,    // cudaMallocManaged: pages migrate between host and card on fault
    Partial,    // holds every word, with a fraction in pinned host memory
    Recompute,  // keeps some Argon2d blocks on the card and rebuilds the rest on demand
};

// Words per Argon2d block. gpu_adversary.cu cannot include argon2d.hpp, so the
// caller checks this against a2::kBlockWords.
static constexpr uint32_t kRecomputeBlockWords = 128;

struct RecomputeState {
    int32_t*  d_slot       = nullptr;  // per block: >= 0 kept, -1 missing, <= -2 scratch entry -2 - slot
    uint32_t  n_blocks     = 0;
    uint64_t* d_scratch    = nullptr;  // blocks rebuilt for the current challenge
    uint32_t* d_scratch_id = nullptr;  // which block each scratch entry holds
    uint32_t  scratch_cap  = 0;        // in blocks
    uint32_t  scratch_used = 0;
    uint32_t* d_need       = nullptr;  // blocks the paused lanes asked for
    uint32_t* d_nneed      = nullptr;
    uint8_t*  d_done       = nullptr;  // per lane: finished this challenge
    uint32_t* h_need       = nullptr;
    uint64_t* h_rebuilt    = nullptr;
    uint32_t  need_cap     = 0;        // in lanes
};

struct Adversary {
    Cheat     cheat     = Cheat::Fake;
    uint64_t  num_words = 0;          // the buffer size the verifier believes in
    uint64_t* d_buf     = nullptr;    // the words on the card: [0, dev_words)
    uint64_t  dev_words = 0;
    uint64_t* d_host    = nullptr;    // Partial: device view of words [dev_words, num_words)
    void*     h_host    = nullptr;
    Lanes     lanes;
    Staging   stage;
    RecomputeState rc;
};

bool create_fake(Adversary& a, uint64_t num_words, double held_fraction, uint32_t lane_cap,
                 char* err, size_t errlen);
bool create_managed(Adversary& a, uint64_t num_words, uint32_t lane_cap, char* err, size_t errlen);
bool create_partial(Adversary& a, uint64_t num_words, double offload_fraction, uint32_t lane_cap,
                    char* err, size_t errlen);

// slot has n_blocks entries: each block's index in kept_words, or -1 if the
// cheat discarded it. kept_words holds n_kept built blocks, and only these go
// to the card.
bool create_recompute(Adversary& a, uint32_t n_blocks, const int32_t* slot,
                      const uint64_t* kept_words, uint32_t n_kept, uint32_t lane_cap,
                      char* err, size_t errlen);

void destroy(Adversary& a);

// Copies the next `bytes` from a.stage.h into the buffer. Fake drops the words
// it does not hold.
bool push(Adversary& a, size_t bytes, char* err, size_t errlen);

bool answer(Adversary& a, uint64_t nonce, uint32_t k, uint32_t lanes, bool chained, bool tree,
            uint64_t* out_hash, Timing* t, char* err, size_t errlen);

// Rebuilds blocks[0..n) into out, kRecomputeBlockWords words each. Returning
// false aborts the answer.
using RebuildFn = bool (*)(void* ctx, const uint32_t* blocks, uint32_t n, uint64_t* out);

struct RecomputeStats {
    uint32_t rounds     = 0;   // kernel launches
    uint32_t rebuilt    = 0;   // distinct blocks rebuilt
    double   rebuild_ms = 0;   // host time inside RebuildFn
};

// Answers with the recompute cheat. Each round runs every unfinished lane until
// it finishes or reaches a missing block. The host rebuilds the blocks the
// paused lanes asked for, and the next round runs those lanes again from the
// start. kernel_ms covers every round, rebuilds included.
bool answer_recompute(Adversary& a, uint64_t nonce, uint32_t k, uint32_t lanes, bool chained,
                      bool tree, RebuildFn rebuild, void* ctx, uint64_t* out_hash, Timing* t,
                      RecomputeStats* st, char* err, size_t errlen);

// Marks the blocks rebuilt for the last challenge missing again. Call it after
// the response is sent.
bool recompute_forget(Adversary& a, char* err, size_t errlen);

const char* residency(const Adversary& a, bool* is_device);

// The fraction of the buffer held in device memory. Managed reports 1.0
// because the driver does not say how much has migrated.
double resident_fraction(const Adversary& a);

// Holds `bytes` of device memory unused, so a managed buffer has to page.
bool reserve_hbm(size_t bytes, char* err, size_t errlen);
void release_hbm();

} // namespace gpu
} // namespace mc
