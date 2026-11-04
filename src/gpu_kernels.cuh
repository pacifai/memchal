// gpu_kernels.cuh — what gpu_answer.cu and gpu_adversary.cu share: error
// reporting, the lane kernel, and the timed answer around it.
#pragma once
#include "challenge.hpp"
#include "gpu_answer.hpp"
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

// A bad launch configuration shows up only in cudaGetLastError().
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
bool lanes_grow(Lanes& l, uint32_t lanes, char* err, size_t errlen);
bool staging_create(Staging& s, char* err, size_t errlen);
void staging_destroy(Staging& s);

// Folds the lane hashes in l.d_hash into the answer.
bool fold(Lanes& l, uint64_t nonce, uint32_t k, uint32_t lanes, bool tree,
          uint64_t* out_hash, Timing* t, char* err, size_t errlen);

// What the driver says backs ptr. is_device is true only for cudaMalloc memory.
const char* describe_pointer(const void* ptr, bool* is_device);

namespace {

struct LoadDirect {
    const uint64_t* __restrict__ buf;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const { return buf[idx]; }
};

template <typename Load>
__global__ void k_lane(Load load, uint64_t num_words, uint64_t nonce,
                       uint32_t k, uint32_t lanes, bool chained, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes) return;
    out[l] = answer_lane_t(load, num_words, nonce, l, lane_samples(k, lanes, l), chained);
}

// Runs one thread per lane over `load`, times the kernel with the lane events,
// and folds.
template <typename Load>
bool answer_with(Lanes& l, Load load, uint64_t num_words, uint64_t nonce, uint32_t k,
                 uint32_t lanes, bool chained, bool tree, uint64_t* out_hash, Timing* t,
                 char* err, size_t errlen) {
    if (lanes == 0) lanes = 1;
    if (!lanes_grow(l, lanes, err, errlen)) return false;
    cudaEvent_t start = (cudaEvent_t)l.ev_start, stop = (cudaEvent_t)l.ev_stop;
    cudaError_t e = cudaEventRecord(start);
    if (e != cudaSuccess) { fail(err, errlen, "cudaEventRecord(start)", e); return false; }
    k_lane<<<blocks_for(lanes), kThreadsPerBlock>>>(load, num_words, nonce, k, lanes, chained,
                                                    l.d_hash);
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
    return fold(l, nonce, k, lanes, tree, out_hash, t, err, errlen);
}

} // namespace

} // namespace gpu
} // namespace mc
