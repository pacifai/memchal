#pragma once
#include "gpu/answer.hpp"
#include "shared/challenge.hpp"
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>

namespace mc {
namespace gpu {

constexpr int kThreadsPerBlock = 256;

static inline unsigned int blocks_for(uint64_t n) {
    return (unsigned int)((n + kThreadsPerBlock - 1) / kThreadsPerBlock);
}

static inline void fail(char* err, size_t n, const char* what, cudaError_t e) {
    if (err && n) snprintf(err, n, "%s: %s", what, cudaGetErrorString(e));
}

static inline void failmsg(char* err, size_t n, const char* msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static inline bool launched(const char* what, char* err, size_t n) {
    cudaError_t e = cudaGetLastError();
    if (e != cudaSuccess) { fail(err, n, what, e); return false; }
    return true;
}

static inline double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool lanes_create(Lanes& l, uint32_t cap, char* err, size_t errlen);
void lanes_destroy(Lanes& l);
// False, with a message in err, when a challenge has more lanes than l was created for.
bool lanes_fit(const Lanes& l, uint32_t lanes, char* err, size_t errlen);
bool staging_create(Staging& s, char* err, size_t errlen);
void staging_destroy(Staging& s);

// Folds the lane hashes in l.d_hash into the answer.
bool fold(Lanes& l, uint64_t nonce, uint32_t samples, uint32_t lanes, bool tree,
          uint64_t* out_hash, Timing* t, char* err, size_t errlen);

// Describes the memory backing ptr. is_device is true only for cudaMalloc memory.
const char* describe_pointer(const void* ptr, bool* is_device);

namespace {

struct LoadDirect {
    const uint64_t* __restrict__ buf;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const { return buf[idx]; }
};

template <typename Load>
__global__ void k_lane(Load load, uint64_t num_words, uint64_t nonce,
                       uint32_t samples, uint32_t lanes, bool chained, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes) return;
    out[l] = answer_lane(load, num_words, nonce, l, samples_in_lane(samples, lanes, l), chained);
}

// Answers one challenge with one thread per lane, reading words through `load`.
template <typename Load>
bool answer_with(Lanes& l, Load load, uint64_t num_words, uint64_t nonce, uint32_t samples,
                 uint32_t lanes, bool chained, bool tree, uint64_t* out_hash, Timing* t,
                 char* err, size_t errlen) {
    if (!lanes_fit(l, lanes, err, errlen)) return false;
    cudaEvent_t start = (cudaEvent_t)l.ev_start, stop = (cudaEvent_t)l.ev_stop;
    cudaError_t e = cudaEventRecord(start);
    if (e != cudaSuccess) { fail(err, errlen, "cudaEventRecord(start)", e); return false; }
    k_lane<<<blocks_for(lanes), kThreadsPerBlock>>>(load, num_words, nonce, samples, lanes,
                                                    chained, l.d_hash);
    if (!launched("k_lane launch", err, errlen)) return false;
    if ((e = cudaEventRecord(stop)) != cudaSuccess) {
        fail(err, errlen, "cudaEventRecord(stop)", e); return false;
    }
    if ((e = cudaEventSynchronize(stop)) != cudaSuccess) { fail(err, errlen, "k_lane", e); return false; }
    float ms = 0.0f;
    if ((e = cudaEventElapsedTime(&ms, start, stop)) != cudaSuccess) {
        fail(err, errlen, "cudaEventElapsedTime", e); return false;
    }
    t->kernel_ms = ms;
    return fold(l, nonce, samples, lanes, tree, out_hash, t, err, errlen);
}

} // namespace

} // namespace gpu
} // namespace mc
