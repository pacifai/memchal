#pragma once
#include <cstddef>
#include <cstdint>

namespace mc {
namespace gpu {

struct Lanes {
    uint64_t* d_hash   = nullptr;
    uint64_t* d_level  = nullptr;
    uint64_t* h_hash   = nullptr;
    uint32_t  cap      = 0;
    void*     ev_start = nullptr;
    void*     ev_stop  = nullptr;
};

// Pinned host memory that push() copies the buffer from.
struct Staging {
    unsigned char* h      = nullptr;
    size_t         cap    = 0;
    uint64_t       cursor = 0;
};

struct Timing {
    double kernel_ms = 0;
    double copy_ms   = 0;
    double fold_ms   = 0;
};

// What memory backs a buffer. on_device is true only for cudaMalloc memory,
// the one kind a kernel reads without crossing PCIe or migrating pages.
struct Residency {
    const char* backing   = "unknown";
    bool        on_device = false;
};

struct Prover {
    uint64_t* d_buf     = nullptr;
    uint64_t  num_words = 0;
    Lanes     lanes;
    Staging   stage;
};

// Selects a device and writes a description of it to name.
bool select_device(int ordinal, char* name, size_t name_len, char* err, size_t errlen);
void mem_info(size_t* free_bytes, size_t* total_bytes);

// Probed bytes per second, counting 8 bytes per probe.
double probe_gbps(uint32_t samples, double kernel_ms);

bool create(Prover& p, uint64_t num_words, uint32_t lane_cap, char* err, size_t errlen);
void destroy(Prover& p);

// Copies the next `bytes` from p.stage.h to the device.
bool push(Prover& p, size_t bytes, char* err, size_t errlen);

// Fills the buffer on the device with fill_splitmix64()'s words.
bool fill_splitmix64(Prover& p, uint64_t seed, char* err, size_t errlen);

bool answer(Prover& p, uint64_t nonce, uint32_t samples, uint32_t lanes, bool chained, bool tree,
            uint64_t* out_hash, Timing* t, char* err, size_t errlen);

// The memory backing the buffer, as CUDA reports it.
Residency residency(const Prover& p);

} // namespace gpu
} // namespace mc
