// gpu_answer.hpp — the honest GPU prover: a cudaMalloc buffer and the answer
// path, one thread per lane. The header uses no CUDA types, so g++ callers can
// include it. gpu_answer.cu implements it.
//
// Every call that can fail writes the reason to (err, errlen).
#pragma once
#include <cstddef>
#include <cstdint>

namespace mc {
namespace gpu {

// Per-lane scratch and the events that time the lane kernel.
struct Lanes {
    uint64_t* d_hash   = nullptr;   // one hash per lane
    uint64_t* d_hash2  = nullptr;   // the tree fold's other level
    uint64_t* h_hash   = nullptr;   // pinned copy for the sequential fold
    uint32_t  cap      = 0;
    void*     ev_start = nullptr;   // cudaEvent_t
    void*     ev_stop  = nullptr;
};

// Pinned host memory the server receives the buffer into before push().
struct Staging {
    unsigned char* h      = nullptr;
    size_t         cap    = 0;
    uint64_t       cursor = 0;      // bytes pushed so far
};

struct Timing {
    double kernel_ms = 0;   // the lane kernel
    double copy_ms   = 0;   // device to host: the lane hashes, or the tree's root
    double fold_ms   = 0;   // the fold across lanes
};

struct Prover {
    uint64_t* d_buf     = nullptr;
    uint64_t  num_words = 0;
    Lanes     lanes;
    Staging   stage;
};

// Picks a device and describes it in name.
bool select_device(int ordinal, char* name, size_t name_len, char* err, size_t errlen);
void mem_info(size_t* free_bytes, size_t* total_bytes);

// Useful bytes per second for k probes of 8 bytes. An 8-byte probe moves at
// least a 32-byte sector, so DRAM traffic is several times higher.
double probe_gbps(uint32_t k, double kernel_ms);

bool create(Prover& p, uint64_t num_words, uint32_t lane_cap, char* err, size_t errlen);
void destroy(Prover& p);

// Copies the next `bytes` from p.stage.h to the device.
bool push(Prover& p, size_t bytes, char* err, size_t errlen);

// Fills the buffer on the device with fill_random()'s words. The GPU
// known-answer test uses it.
bool fill_splitmix64(Prover& p, uint64_t seed, char* err, size_t errlen);

// Answers one challenge. `tree` picks the fold, and has to match the verifier's.
bool answer(Prover& p, uint64_t nonce, uint32_t k, uint32_t lanes, bool chained, bool tree,
            uint64_t* out_hash, Timing* t, char* err, size_t errlen);

// What the driver says backs the buffer (ADR 0008). is_device is true only for
// cudaMalloc memory.
const char* residency(const Prover& p, bool* is_device);

} // namespace gpu
} // namespace mc
