// gpu_answer.cu — the CUDA answer path.
//
// Includes challenge.hpp and the CUDA runtime, and nothing else. No POSIX, no
// sockets: that is the whole reason common.hpp was split. The server half of
// the GPU prover is ordinary C++ in prover_cuda.cpp and links against this.
//
// What runs where, and why:
//
//   - One thread per lane. answer_lane_t() is the thread body, unchanged from
//     the CPU reference — it was written as a template over a loader precisely
//     so this would be possible.
//   - The fold across lanes runs on the HOST. It is a strict sequential chain
//     (combined = mix64(combined ^ term_l), lane order), so it is not
//     associative and a warp-shuffle or tree reduction would produce a
//     different hash. Folding on the host reuses the same fold_seed() and
//     lane_fold_term() the CPU verifier calls, so there is exactly one
//     definition of the order.
//
// The fold's cost is reported separately from the kernel's, because they are
// different physics and adding them together is how a bandwidth claim gets
// quietly flattered.
#include "gpu_answer.hpp"
#include "challenge.hpp"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace mc {
namespace gpu {

// ---------------------------------------------------------------------------
// error plumbing
// ---------------------------------------------------------------------------
static void fail(char* err, size_t n, const char* what, cudaError_t e) {
    if (err && n) snprintf(err, n, "%s: %s", what, cudaGetErrorString(e));
}
static void failmsg(char* err, size_t n, const char* msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

const char* alloc_name(Alloc a) {
    switch (a) {
        case Alloc::Device:     return "device";
        case Alloc::Managed:    return "managed";
        case Alloc::HostMapped: return "hostmapped";
        case Alloc::Split:      return "split";
        case Alloc::Recompute:  return "recompute";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// loaders — the __device__ equivalent of the CPU prover's lambda
// ---------------------------------------------------------------------------

// Honest, UVM, and host-mapped all read the same way. What differs is only what
// the pointer is backed by, which is exactly the point: the challenge math
// cannot tell, so only timing can.
struct LoadDirect {
    const uint64_t* __restrict__ buf;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const {
        return buf[idx];
    }
};

// The `fake` adversary: holds `held` words and invents the rest. It saves real
// memory, answers at honest speed, and is wrong on every challenge that probes
// past what it holds — the correctness detector, not the timing one.
struct LoadFake {
    const uint64_t* __restrict__ buf;
    uint64_t held;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const {
        if (idx < held) return buf[idx];
        return mc::splitmix64_word(0x9E3779B97F4A7C15ULL, idx); // deterministically wrong
    }
};

// The partial-residency cheat: most of the buffer on the card, a tail of it in
// host memory reached over PCIe. The branch costs some warp divergence, which
// is the adversary's problem, not the detector's.
struct LoadSplit {
    const uint64_t* __restrict__ dev;
    const uint64_t* __restrict__ off;
    uint64_t split;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const {
        return idx < split ? dev[idx] : off[idx - split];
    }
};

// The prefetch cheat's dry run: return zero for every probe and note where the
// probe went. Each thread builds its own, so the cursor is per lane. With
// independent chains the addresses do not depend on the data, so the list is
// exact; with dependent chains only the first address per lane is.
struct LoadRecord {
    uint64_t* rec;
    uint32_t  j;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) {
        rec[j++] = idx;
        return 0;
    }
};

// The prefetch cheat's real pass: take the fetched word when the dry run
// listed this address, read the buffer directly when it did not.
struct LoadStaged {
    const uint64_t* __restrict__ idx;
    const uint64_t* __restrict__ val;
    const uint64_t* __restrict__ buf;
    uint32_t j;
    __device__ __forceinline__ uint64_t operator()(uint64_t i) {
        uint64_t w = (idx[j] == i) ? val[j] : buf[i];
        ++j;
        return w;
    }
};

// The recompute cheat. Reads a kept block or one rebuilt this challenge; on a
// missing block it asks for it and stops reading, returning zeros so the rest
// of the lane runs to completion as garbage. The kernel throws that hash away
// and the lane runs again once the block is on the card.
struct LoadRecompute {
    const uint64_t* __restrict__ kept;
    const uint64_t* __restrict__ scratch;
    const int32_t*  __restrict__ slot;
    uint32_t* need;
    uint32_t* nneed;
    bool missed;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) {
        if (missed) return 0;
        const uint32_t b = (uint32_t)(idx / kRecomputeBlockWords);
        const uint64_t w = idx % kRecomputeBlockWords;
        const int32_t s = slot[b];
        if (s >= 0)  return kept[(uint64_t)s * kRecomputeBlockWords + w];
        if (s <= -2) return scratch[(uint64_t)(-2 - s) * kRecomputeBlockWords + w];
        missed = true;
        need[atomicAdd(nneed, 1u)] = b;   // at most one per lane per round
        return 0;
    }
};

// Where lane l's probes start in a flat array of all k, matching
// lane_samples(): the first k % lanes lanes take one extra.
__device__ __forceinline__ uint32_t lane_offset(uint32_t k, uint32_t lanes, uint32_t l) {
    uint32_t q = k / lanes, r = k % lanes;
    return l * q + (l < r ? l : r);
}

// ---------------------------------------------------------------------------
// kernels
// ---------------------------------------------------------------------------
template <typename Load>
__global__ void k_lane(Load load, uint64_t num_words, uint64_t nonce,
                       uint32_t k, uint32_t lanes, bool chained, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes) return;
    out[l] = mc::answer_lane_t(load, num_words, nonce, l,
                               mc::lane_samples(k, lanes, l), chained);
}

__global__ void k_lane_recompute(LoadRecompute load, uint64_t num_words, uint64_t nonce,
                                 uint32_t k, uint32_t lanes, bool chained,
                                 uint8_t* done, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes || done[l]) return;
    LoadRecompute ld = load;
    ld.missed = false;
    uint64_t h = mc::answer_lane_t(ld, num_words, nonce, l,
                                   mc::lane_samples(k, lanes, l), chained);
    if (!ld.missed) { out[l] = h; done[l] = 1; }
}

// Point each newly rebuilt block's slot at its scratch entry, from `first` on.
__global__ void k_point_slots(int32_t* slot, const uint32_t* ids, uint32_t n, uint32_t first) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) slot[ids[i]] = -2 - (int32_t)(first + i);
}

__global__ void k_clear_slots(int32_t* slot, const uint32_t* ids, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) slot[ids[i]] = -1;
}

__global__ void k_pf_record(uint64_t num_words, uint64_t nonce, uint32_t k,
                            uint32_t lanes, bool chained, uint64_t* rec, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes) return;
    LoadRecord ld{rec + lane_offset(k, lanes, l), 0};
    out[l] = mc::answer_lane_t(ld, num_words, nonce, l, mc::lane_samples(k, lanes, l), chained);
}

// One thread per listed address, all independent, so the reads overlap
// instead of waiting on each other the way a lane's probes do.
__global__ void k_pf_gather(const uint64_t* __restrict__ buf, const uint64_t* __restrict__ idx,
                            uint64_t* __restrict__ val, uint32_t k) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < k) val[i] = buf[idx[i]];
}

__global__ void k_pf_answer(const uint64_t* buf, const uint64_t* idx, const uint64_t* val,
                            uint64_t num_words, uint64_t nonce, uint32_t k, uint32_t lanes,
                            bool chained, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes) return;
    uint32_t o = lane_offset(k, lanes, l);
    LoadStaged ld{idx + o, val + o, buf, 0};
    out[l] = mc::answer_lane_t(ld, num_words, nonce, l, mc::lane_samples(k, lanes, l), chained);
}

// Level 0 of the tree: turn each raw lane hash into its leaf contribution.
__global__ void k_tree_leaves(uint64_t* v, uint32_t lanes) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l < lanes) v[l] = mc::lane_fold_term(v[l], l);
}

// One level of the tree. Reads `src`, writes `dst` — not in place, because
// thread i writes index i while thread i/2 still needs to read index 2*(i/2),
// which is i when i is even. Padding for a missing right sibling comes from
// tree_pad(), matching the CPU reference exactly.
__global__ void k_tree_level(const uint64_t* __restrict__ src, uint64_t* __restrict__ dst,
                             uint32_t n, uint32_t level) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t m = (n + 1) / 2;
    if (i >= m) return;
    uint64_t a = src[2 * i];
    uint64_t b = (2 * i + 1 < n) ? src[2 * i + 1] : mc::tree_pad(level, 2 * i + 1);
    dst[i] = mc::tree_combine(a, b);
}

// Generate the buffer on-device. splitmix64_word(seed, i) is by construction
// the word fill_random() puts at buf[i], so this is bit-identical to the CPU
// generator and lets a self-test run without an upload.
__global__ void k_fill(uint64_t* buf, uint64_t n, uint64_t seed) {
    uint64_t i      = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    uint64_t stride = (uint64_t)blockDim.x * gridDim.x;
    for (; i < n; i += stride) buf[i] = mc::splitmix64_word(seed, i);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
bool select_device(int ordinal, char* name, size_t name_len, char* err, size_t errlen) {
    int count = 0;
    cudaError_t e = cudaGetDeviceCount(&count);
    if (e != cudaSuccess) { fail(err, errlen, "cudaGetDeviceCount", e); return false; }
    if (count <= 0)       { failmsg(err, errlen, "no CUDA device visible"); return false; }
    if (ordinal < 0 || ordinal >= count) {
        if (err && errlen) snprintf(err, errlen, "device %d out of range (%d visible)", ordinal, count);
        return false;
    }
    if ((e = cudaSetDevice(ordinal)) != cudaSuccess) { fail(err, errlen, "cudaSetDevice", e); return false; }

    cudaDeviceProp prop{};
    if ((e = cudaGetDeviceProperties(&prop, ordinal)) != cudaSuccess) {
        fail(err, errlen, "cudaGetDeviceProperties", e); return false;
    }
    if (name && name_len) {
        snprintf(name, name_len, "%s (sm_%d%d, %.1f GiB HBM, %d SMs, L2 %.0f MiB)",
                 prop.name, prop.major, prop.minor,
                 (double)prop.totalGlobalMem / 1073741824.0,
                 prop.multiProcessorCount,
                 (double)prop.l2CacheSize / 1048576.0);
    }
    return true;
}

void destroy(Prover& p) {
    if (p.d_slot)       cudaFree(p.d_slot);
    if (p.d_scratch)    cudaFree(p.d_scratch);
    if (p.d_scratch_id) cudaFree(p.d_scratch_id);
    if (p.d_need)       cudaFree(p.d_need);
    if (p.d_nneed)      cudaFree(p.d_nneed);
    if (p.d_done)       cudaFree(p.d_done);
    if (p.h_need)       cudaFreeHost(p.h_need);
    if (p.h_rebuilt)    cudaFreeHost(p.h_rebuilt);
    if (p.d_pf_idx) cudaFree(p.d_pf_idx);
    if (p.d_pf_val) cudaFree(p.d_pf_val);
    if (p.h_swap)   cudaFreeHost(p.h_swap);
    if (p.d_lane)   cudaFree(p.d_lane);
    if (p.d_lane2)  cudaFree(p.d_lane2);
    if (p.h_lane)   cudaFreeHost(p.h_lane);
    if (p.h_stage)  cudaFreeHost(p.h_stage);
    if (p.h_off)    cudaFreeHost(p.h_off);
    if (p.alloc == Alloc::Split) {
        if (p.d_buf) cudaFree(p.d_buf);    // the resident half is a real cudaMalloc
    } else if (p.h_mapped) {
        cudaFreeHost(p.h_mapped);          // frees the mapped host allocation
    } else if (p.d_buf) {
        cudaFree(p.d_buf);                 // Device and Managed both free this way
    }
    if (p.ev_start) cudaEventDestroy((cudaEvent_t)p.ev_start);
    if (p.ev_stop)  cudaEventDestroy((cudaEvent_t)p.ev_stop);
    p = Prover{};
}

// The per-lane buffers, upload staging, and timing events every kind of
// prover needs.
static bool alloc_scratch(Prover& p, uint32_t lane_cap, char* err, size_t errlen) {
    cudaError_t e;
    if ((e = cudaMalloc((void**)&p.d_lane, (size_t)lane_cap * 8)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(lane scratch)", e); return false;
    }
    if ((e = cudaMalloc((void**)&p.d_lane2, (size_t)lane_cap * 8)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(tree scratch)", e); return false;
    }
    if ((e = cudaHostAlloc((void**)&p.h_lane, (size_t)lane_cap * 8, cudaHostAllocDefault)) != cudaSuccess) {
        fail(err, errlen, "cudaHostAlloc(lane staging)", e); return false;
    }
    p.lane_cap = lane_cap;

    p.stage_cap = 8u << 20; // 8 MiB of pinned staging for the upload
    if ((e = cudaHostAlloc((void**)&p.h_stage, p.stage_cap, cudaHostAllocDefault)) != cudaSuccess) {
        fail(err, errlen, "cudaHostAlloc(upload staging)", e); return false;
    }

    cudaEvent_t a = nullptr, b = nullptr;
    if ((e = cudaEventCreate(&a)) != cudaSuccess) { fail(err, errlen, "cudaEventCreate", e); return false; }
    if ((e = cudaEventCreate(&b)) != cudaSuccess) { fail(err, errlen, "cudaEventCreate", e); return false; }
    p.ev_start = a; p.ev_stop = b;
    p.cursor = 0;
    return true;
}

bool create(Prover& p, uint64_t num_words, Alloc alloc, uint32_t lane_cap,
            double held_fraction, double offload_fraction, char* err, size_t errlen) {
    destroy(p);
    if (lane_cap == 0) lane_cap = 1;
    p.num_words = num_words;
    p.alloc     = alloc;

    if (held_fraction >= 1.0) {
        p.held_words = num_words;
        p.fabricate  = false;
    } else {
        if (held_fraction < 0.0) held_fraction = 0.0;
        p.held_words = (uint64_t)((double)num_words * held_fraction);
        p.fabricate  = true;
    }
    // Always allocate at least one word so the pointer is valid to query.
    size_t bytes = (size_t)(p.held_words ? p.held_words : 1) * 8;

    if (alloc == Alloc::Split) {
        if (offload_fraction < 0.0) offload_fraction = 0.0;
        if (offload_fraction > 1.0) offload_fraction = 1.0;
        uint64_t off_words = (uint64_t)((double)p.held_words * offload_fraction);
        p.split_words = p.held_words - off_words;
    } else {
        p.split_words = p.held_words;
    }

    cudaError_t e = cudaSuccess;
    switch (alloc) {
        case Alloc::Device:
            e = cudaMalloc((void**)&p.d_buf, bytes);
            if (e != cudaSuccess) { fail(err, errlen, "cudaMalloc(buffer)", e); return false; }
            break;
        case Alloc::Managed:
            // Deliberately NOT prefetched to the device: letting pages migrate
            // on fault is the adversary's whole mechanism.
            e = cudaMallocManaged((void**)&p.d_buf, bytes);
            if (e != cudaSuccess) { fail(err, errlen, "cudaMallocManaged(buffer)", e); return false; }
            break;
        case Alloc::HostMapped:
            e = cudaHostAlloc(&p.h_mapped, bytes, cudaHostAllocMapped);
            if (e != cudaSuccess) { fail(err, errlen, "cudaHostAlloc(mapped)", e); return false; }
            e = cudaHostGetDevicePointer((void**)&p.d_buf, p.h_mapped, 0);
            if (e != cudaSuccess) { fail(err, errlen, "cudaHostGetDevicePointer", e); return false; }
            break;
        case Alloc::Recompute:
            failmsg(err, errlen, "a recompute buffer is made by create_recompute()");
            return false;
        case Alloc::Split: {
            size_t dev_bytes = (size_t)p.split_words * 8;
            size_t off_bytes = bytes - dev_bytes;
            if (dev_bytes) {
                e = cudaMalloc((void**)&p.d_buf, dev_bytes);
                if (e != cudaSuccess) { fail(err, errlen, "cudaMalloc(resident part)", e); return false; }
            }
            if (off_bytes) {
                e = cudaHostAlloc(&p.h_off, off_bytes, cudaHostAllocMapped);
                if (e != cudaSuccess) { fail(err, errlen, "cudaHostAlloc(offloaded part)", e); return false; }
                e = cudaHostGetDevicePointer((void**)&p.d_off, p.h_off, 0);
                if (e != cudaSuccess) { fail(err, errlen, "cudaHostGetDevicePointer(offloaded)", e); return false; }
            }
            break;
        }
    }

    return alloc_scratch(p, lane_cap, err, errlen);
}

bool create_recompute(Prover& p, uint32_t n_blocks, const int32_t* slot,
                      const uint64_t* kept_words, uint32_t n_kept, uint32_t lane_cap,
                      char* err, size_t errlen) {
    destroy(p);
    if (lane_cap == 0) lane_cap = 1;
    p.alloc      = Alloc::Recompute;
    p.num_words  = (uint64_t)n_blocks * kRecomputeBlockWords;
    p.held_words = p.num_words;     // it can produce every word, just not quickly
    p.n_blocks   = n_blocks;
    p.n_kept     = n_kept;

    const size_t kept_bytes = (size_t)(n_kept ? n_kept : 1) * kRecomputeBlockWords * 8;
    cudaError_t e = cudaMalloc((void**)&p.d_buf, kept_bytes);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMalloc(kept blocks)", e); return false; }
    if (n_kept) {
        e = cudaMemcpy(p.d_buf, kept_words, (size_t)n_kept * kRecomputeBlockWords * 8,
                       cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(kept blocks)", e); return false; }
    }
    if ((e = cudaMalloc((void**)&p.d_slot, (size_t)n_blocks * 4)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(slot table)", e); return false;
    }
    e = cudaMemcpy(p.d_slot, slot, (size_t)n_blocks * 4, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(slot table)", e); return false; }
    if ((e = cudaMalloc((void**)&p.d_nneed, 4)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(need count)", e); return false;
    }
    return alloc_scratch(p, lane_cap, err, errlen);
}

// ---------------------------------------------------------------------------
// upload
// ---------------------------------------------------------------------------
unsigned char* staging(Prover& p, size_t* cap) {
    if (cap) *cap = p.stage_cap;
    return p.h_stage;
}

bool push(Prover& p, size_t bytes, char* err, size_t errlen) {
    if (bytes == 0) return true;
    if (bytes > p.stage_cap) { failmsg(err, errlen, "push larger than staging buffer"); return false; }
    size_t held_bytes = (size_t)p.held_words * 8;
    if (p.cursor >= held_bytes) { p.cursor += bytes; return true; } // fake: discard the tail
    size_t room = held_bytes - (size_t)p.cursor;
    size_t take = bytes < room ? bytes : room;
    if (p.h_swap) memcpy((unsigned char*)p.h_swap + p.cursor, p.h_stage, take);

    // A Split buffer has a seam: bytes before split_words land on the card,
    // bytes after it land in host memory, and one chunk can cross the seam.
    size_t seam = (size_t)p.split_words * 8;
    size_t done = 0;
    while (done < take) {
        size_t at = (size_t)p.cursor + done;
        size_t left = take - done;
        cudaError_t e;
        if (p.alloc == Alloc::Split && at >= seam) {
            size_t n = left;
            memcpy((unsigned char*)p.h_off + (at - seam), p.h_stage + done, n);
            done += n;
            continue;
        }
        size_t n = left;
        if (p.alloc == Alloc::Split && at + n > seam) n = seam - at;
        if (p.alloc == Alloc::Managed) {
            // Write managed memory from the host; the part that fits on the
            // card is prefetched there once the upload completes, below.
            // cudaMemcpy H2D here instead forces every page onto the card as it
            // arrives, and with HBM reserved below the buffer size each chunk
            // evicts an earlier one: a 4 GiB upload with 2.4 GiB free took 53 s
            // against 3.5 s honest.
            memcpy((unsigned char*)p.d_buf + at, p.h_stage + done, n);
            done += n;
            continue;
        }
        e = cudaMemcpy((unsigned char*)p.d_buf + at, p.h_stage + done, n,
                       cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(H2D upload)", e); return false; }
        done += n;
    }
    size_t before = (size_t)p.cursor;
    p.cursor += bytes;

    // Once a managed buffer is fully written, place as much of it on the card
    // as free HBM allows. Left alone, host-written pages did not settle on the
    // card even when they all fit: a 4 GiB buffer with 8 GiB free answered at
    // 7x honest, which would make a genuinely resident buffer look like a
    // caught cheat. Prefetching restores that case to honest speed and leaves a
    // starved buffer with exactly what fits resident and the rest in host
    // memory, to migrate on demand — the state the challenges measure. The
    // margin keeps room for the lane buffers and kernel launches.
    if (p.alloc == Alloc::Managed && before < held_bytes && (size_t)p.cursor >= held_bytes) {
        size_t free_b = 0, total_b = 0;
        cudaMemGetInfo(&free_b, &total_b);
        const size_t margin = (size_t)512 << 20;
        size_t fit = free_b > margin ? free_b - margin : 0;
        size_t n = held_bytes < fit ? held_bytes : fit;
        if (n) {
            int dev = 0;
            cudaGetDevice(&dev);
            cudaError_t e = cudaMemPrefetchAsync(p.d_buf, n, dev, 0);
            if (e == cudaSuccess) e = cudaDeviceSynchronize();
            if (e != cudaSuccess) { fail(err, errlen, "cudaMemPrefetchAsync(managed)", e); return false; }
        }
    }
    return true;
}

bool fill_splitmix64(Prover& p, uint64_t seed, char* err, size_t errlen) {
    if (!p.d_buf || p.held_words == 0) { failmsg(err, errlen, "no buffer to fill"); return false; }
    if (p.alloc == Alloc::Split) {
        failmsg(err, errlen, "on-device fill does not cover a Split buffer's host half");
        return false;
    }
    int tpb = 256;
    unsigned int blocks = 1024;
    k_fill<<<blocks, tpb>>>(p.d_buf, p.held_words, seed);
    cudaError_t e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { fail(err, errlen, "k_fill", e); return false; }
    p.cursor = (uint64_t)p.held_words * 8;
    return true;
}

// ---------------------------------------------------------------------------
// answer
// ---------------------------------------------------------------------------
static bool grow_lanes(Prover& p, uint32_t lanes, char* err, size_t errlen) {
    if (lanes <= p.lane_cap) return true;
    if (p.d_lane)  cudaFree(p.d_lane);
    if (p.d_lane2) cudaFree(p.d_lane2);
    if (p.h_lane)  cudaFreeHost(p.h_lane);
    p.d_lane = nullptr; p.d_lane2 = nullptr; p.h_lane = nullptr; p.lane_cap = 0;
    cudaError_t e;
    if ((e = cudaMalloc((void**)&p.d_lane, (size_t)lanes * 8)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(lane scratch grow)", e); return false;
    }
    if ((e = cudaMalloc((void**)&p.d_lane2, (size_t)lanes * 8)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(tree scratch grow)", e); return false;
    }
    if ((e = cudaHostAlloc((void**)&p.h_lane, (size_t)lanes * 8, cudaHostAllocDefault)) != cudaSuccess) {
        fail(err, errlen, "cudaHostAlloc(lane staging grow)", e); return false;
    }
    p.lane_cap = lanes;
    return true;
}

static bool fold(Prover& p, uint64_t nonce, uint32_t k, uint32_t lanes, bool tree,
                 uint64_t* out_hash, double* copy_ms, double* fold_ms,
                 char* err, size_t errlen);

bool answer(Prover& p, uint64_t nonce, uint32_t k, uint32_t lanes, bool chained,
            bool tree, uint64_t* out_hash, double* kernel_ms, double* copy_ms,
            double* fold_ms, char* err, size_t errlen) {
    if (lanes == 0) lanes = 1;
    if (p.alloc == Alloc::Recompute) { failmsg(err, errlen, "a recompute buffer answers through answer_recompute()"); return false; }
    if (p.swapped_out) { failmsg(err, errlen, "answer() while the buffer is swapped out"); return false; }
    if (!grow_lanes(p, lanes, err, errlen)) return false;
    if (p.prefetch && k > p.pf_cap) {
        if (p.d_pf_idx) cudaFree(p.d_pf_idx);
        if (p.d_pf_val) cudaFree(p.d_pf_val);
        p.d_pf_idx = p.d_pf_val = nullptr; p.pf_cap = 0;
        cudaError_t e;
        if ((e = cudaMalloc((void**)&p.d_pf_idx, (size_t)k * 8)) != cudaSuccess) {
            fail(err, errlen, "cudaMalloc(prefetch addresses)", e); return false;
        }
        if ((e = cudaMalloc((void**)&p.d_pf_val, (size_t)k * 8)) != cudaSuccess) {
            fail(err, errlen, "cudaMalloc(prefetch words)", e); return false;
        }
        p.pf_cap = k;
    }

    const int tpb = 256;
    unsigned int blocks = (unsigned int)(((uint64_t)lanes + tpb - 1) / tpb);

    cudaEventRecord((cudaEvent_t)p.ev_start);
    if (p.prefetch) {
        // All three passes count as kernel time: together they are what the
        // cheat does instead of the one honest pass.
        unsigned int kb = (unsigned int)(((uint64_t)k + tpb - 1) / tpb);
        k_pf_record<<<blocks, tpb>>>(p.num_words, nonce, k, lanes, chained, p.d_pf_idx, p.d_lane);
        k_pf_gather<<<kb, tpb>>>(p.d_buf, p.d_pf_idx, p.d_pf_val, k);
        k_pf_answer<<<blocks, tpb>>>(p.d_buf, p.d_pf_idx, p.d_pf_val, p.num_words, nonce, k,
                                     lanes, chained, p.d_lane);
    } else if (p.fabricate) {
        k_lane<<<blocks, tpb>>>(LoadFake{p.d_buf, p.held_words},
                                p.num_words, nonce, k, lanes, chained, p.d_lane);
    } else if (p.alloc == Alloc::Split) {
        k_lane<<<blocks, tpb>>>(LoadSplit{p.d_buf, p.d_off, p.split_words},
                                p.num_words, nonce, k, lanes, chained, p.d_lane);
    } else {
        k_lane<<<blocks, tpb>>>(LoadDirect{p.d_buf},
                                p.num_words, nonce, k, lanes, chained, p.d_lane);
    }
    cudaEventRecord((cudaEvent_t)p.ev_stop);

    cudaError_t e = cudaEventSynchronize((cudaEvent_t)p.ev_stop);
    if (e != cudaSuccess) { fail(err, errlen, "k_lane", e); return false; }
    if ((e = cudaGetLastError()) != cudaSuccess) { fail(err, errlen, "k_lane launch", e); return false; }

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, (cudaEvent_t)p.ev_start, (cudaEvent_t)p.ev_stop);
    if (kernel_ms) *kernel_ms = (double)ms;

    return fold(p, nonce, k, lanes, tree, out_hash, copy_ms, fold_ms, err, errlen);
}

// Folds the lane hashes the kernel left in d_lane into the answer.
static bool fold(Prover& p, uint64_t nonce, uint32_t k, uint32_t lanes, bool tree,
                 uint64_t* out_hash, double* copy_ms, double* fold_ms,
                 char* err, size_t errlen) {
    const int tpb = 256;
    unsigned int blocks = (unsigned int)(((uint64_t)lanes + tpb - 1) / tpb);
    cudaError_t e;

    if (tree) {
        // The whole fold stays on the device: log2(lanes) passes, then eight
        // bytes come back instead of one hash per lane. This is what removes
        // the term that dominated the response at high lane counts.
        auto t2 = std::chrono::steady_clock::now();
        uint64_t* src = p.d_lane;
        uint64_t* dst = p.d_lane2;

        k_tree_leaves<<<blocks, tpb>>>(src, lanes);
        uint32_t n = lanes, level = 0;
        while (n > 1) {
            uint32_t m = (n + 1) / 2;
            unsigned int lb = (unsigned int)(((uint64_t)m + tpb - 1) / tpb);
            k_tree_level<<<lb, tpb>>>(src, dst, n, level);
            uint64_t* t = src; src = dst; dst = t;
            n = m; ++level;
        }
        if ((e = cudaDeviceSynchronize()) != cudaSuccess) {
            fail(err, errlen, "tree fold", e); return false;
        }
        auto t3 = std::chrono::steady_clock::now();
        if (fold_ms) *fold_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

        auto t0 = std::chrono::steady_clock::now();
        uint64_t root = 0;
        e = cudaMemcpy(&root, src, sizeof(root), cudaMemcpyDeviceToHost);
        auto t1 = std::chrono::steady_clock::now();
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(root D2H)", e); return false; }
        if (copy_ms) *copy_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (out_hash) *out_hash = mc::mix64(root ^ mc::fold_seed(nonce, k, lanes));
        return true;
    }

    auto t0 = std::chrono::steady_clock::now();
    e = cudaMemcpy(p.h_lane, p.d_lane, (size_t)lanes * 8, cudaMemcpyDeviceToHost);
    auto t1 = std::chrono::steady_clock::now();
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(lane hashes D2H)", e); return false; }
    if (copy_ms) *copy_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // The sequential fold, in lane order, through the same functions the CPU
    // reference uses. Changing this loop changes every known-answer vector.
    auto t2 = std::chrono::steady_clock::now();
    uint64_t combined = mc::fold_seed(nonce, k, lanes);
    for (uint32_t l = 0; l < lanes; ++l)
        combined = mc::mix64(combined ^ mc::lane_fold_term(p.h_lane[l], l));
    auto t3 = std::chrono::steady_clock::now();
    if (fold_ms) *fold_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    if (out_hash) *out_hash = combined;
    return true;
}

// ---------------------------------------------------------------------------
// recompute
// ---------------------------------------------------------------------------

// Room for `lanes` paused lanes per round: the device's request list, the
// done flags, and the host's pinned copies.
static bool grow_recompute_lanes(Prover& p, uint32_t lanes, char* err, size_t errlen) {
    if (lanes <= p.rebuilt_cap && p.d_need) return true;
    if (p.d_need)    cudaFree(p.d_need);
    if (p.d_done)    cudaFree(p.d_done);
    if (p.h_need)    cudaFreeHost(p.h_need);
    if (p.h_rebuilt) cudaFreeHost(p.h_rebuilt);
    p.d_need = nullptr; p.d_done = nullptr; p.h_need = nullptr; p.h_rebuilt = nullptr;
    p.rebuilt_cap = 0;
    cudaError_t e;
    if ((e = cudaMalloc((void**)&p.d_need, (size_t)lanes * 4)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(need list)", e); return false;
    }
    if ((e = cudaMalloc((void**)&p.d_done, (size_t)lanes)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(done flags)", e); return false;
    }
    if ((e = cudaHostAlloc((void**)&p.h_need, (size_t)lanes * 4, cudaHostAllocDefault)) != cudaSuccess) {
        fail(err, errlen, "cudaHostAlloc(need list)", e); return false;
    }
    e = cudaHostAlloc((void**)&p.h_rebuilt, (size_t)lanes * kRecomputeBlockWords * 8,
                      cudaHostAllocDefault);
    if (e != cudaSuccess) { fail(err, errlen, "cudaHostAlloc(rebuilt blocks)", e); return false; }
    p.rebuilt_cap = lanes;
    return true;
}

// Room for `need` rebuilt blocks this challenge, keeping the ones already there.
static bool grow_scratch(Prover& p, uint32_t need, char* err, size_t errlen) {
    if (need <= p.scratch_cap) return true;
    uint32_t cap = p.scratch_cap ? p.scratch_cap : 1024;
    while (cap < need) cap *= 2;
    uint64_t* blk = nullptr;
    uint32_t* ids = nullptr;
    cudaError_t e = cudaMalloc((void**)&blk, (size_t)cap * kRecomputeBlockWords * 8);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMalloc(rebuild scratch)", e); return false; }
    if ((e = cudaMalloc((void**)&ids, (size_t)cap * 4)) != cudaSuccess) {
        cudaFree(blk); fail(err, errlen, "cudaMalloc(rebuild scratch ids)", e); return false;
    }
    if (p.scratch_used) {
        cudaMemcpy(blk, p.d_scratch, (size_t)p.scratch_used * kRecomputeBlockWords * 8,
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(ids, p.d_scratch_id, (size_t)p.scratch_used * 4, cudaMemcpyDeviceToDevice);
    }
    if (p.d_scratch)    cudaFree(p.d_scratch);
    if (p.d_scratch_id) cudaFree(p.d_scratch_id);
    p.d_scratch = blk; p.d_scratch_id = ids; p.scratch_cap = cap;
    return true;
}

bool answer_recompute(Prover& p, uint64_t nonce, uint32_t k, uint32_t lanes, bool chained,
                      bool tree, RebuildFn rebuild, void* ctx, uint64_t* out_hash,
                      double* kernel_ms, double* copy_ms, double* fold_ms,
                      RecomputeStats* st, char* err, size_t errlen) {
    if (lanes == 0) lanes = 1;
    if (p.alloc != Alloc::Recompute) { failmsg(err, errlen, "not a recompute buffer"); return false; }
    if (!grow_lanes(p, lanes, err, errlen)) return false;
    if (!grow_recompute_lanes(p, lanes, err, errlen)) return false;
    RecomputeStats s;

    const int tpb = 256;
    unsigned int blocks = (unsigned int)(((uint64_t)lanes + tpb - 1) / tpb);
    auto t0 = std::chrono::steady_clock::now();
    cudaError_t e = cudaMemset(p.d_done, 0, lanes);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemset(done flags)", e); return false; }

    for (;;) {
        if ((e = cudaMemset(p.d_nneed, 0, 4)) != cudaSuccess) {
            fail(err, errlen, "cudaMemset(need count)", e); return false;
        }
        LoadRecompute ld{p.d_buf, p.d_scratch, p.d_slot, p.d_need, p.d_nneed, false};
        k_lane_recompute<<<blocks, tpb>>>(ld, p.num_words, nonce, k, lanes, chained,
                                          p.d_done, p.d_lane);
        ++s.rounds;
        uint32_t nneed = 0;
        if ((e = cudaMemcpy(&nneed, p.d_nneed, 4, cudaMemcpyDeviceToHost)) != cudaSuccess) {
            fail(err, errlen, "k_lane_recompute", e); return false;
        }
        if (nneed == 0) break;

        e = cudaMemcpy(p.h_need, p.d_need, (size_t)nneed * 4, cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(need list)", e); return false; }
        // Lanes that paused on the same block need it rebuilt once.
        std::sort(p.h_need, p.h_need + nneed);
        const uint32_t n = (uint32_t)(std::unique(p.h_need, p.h_need + nneed) - p.h_need);

        auto r0 = std::chrono::steady_clock::now();
        if (!rebuild(ctx, p.h_need, n, p.h_rebuilt)) {
            failmsg(err, errlen, "rebuild callback failed"); return false;
        }
        s.rebuild_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - r0).count();

        if (!grow_scratch(p, p.scratch_used + n, err, errlen)) return false;
        e = cudaMemcpy(p.d_scratch + (size_t)p.scratch_used * kRecomputeBlockWords, p.h_rebuilt,
                       (size_t)n * kRecomputeBlockWords * 8, cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(rebuilt blocks)", e); return false; }
        e = cudaMemcpy(p.d_scratch_id + p.scratch_used, p.h_need, (size_t)n * 4,
                       cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(rebuilt ids)", e); return false; }
        unsigned int nb = (n + tpb - 1) / tpb;
        k_point_slots<<<nb, tpb>>>(p.d_slot, p.d_scratch_id + p.scratch_used, n, p.scratch_used);
        p.scratch_used += n;
        s.rebuilt += n;
    }
    if ((e = cudaGetLastError()) != cudaSuccess) { fail(err, errlen, "k_lane_recompute launch", e); return false; }
    if (kernel_ms)
        *kernel_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    if (st) *st = s;
    return fold(p, nonce, k, lanes, tree, out_hash, copy_ms, fold_ms, err, errlen);
}

bool recompute_forget(Prover& p, char* err, size_t errlen) {
    if (p.alloc != Alloc::Recompute || p.scratch_used == 0) return true;
    const int tpb = 256;
    unsigned int nb = (p.scratch_used + tpb - 1) / tpb;
    k_clear_slots<<<nb, tpb>>>(p.d_slot, p.d_scratch_id, p.scratch_used);
    p.scratch_used = 0;
    cudaError_t e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { fail(err, errlen, "k_clear_slots", e); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// prefetch and swap
// ---------------------------------------------------------------------------
void enable_prefetch(Prover& p) { p.prefetch = true; }

bool enable_swap(Prover& p, char* err, size_t errlen) {
    if (p.alloc != Alloc::Device || p.fabricate) {
        failmsg(err, errlen, "swap needs a full cudaMalloc buffer"); return false;
    }
    cudaError_t e = cudaHostAlloc(&p.h_swap, (size_t)p.held_words * 8, cudaHostAllocDefault);
    if (e != cudaSuccess) { p.h_swap = nullptr; fail(err, errlen, "cudaHostAlloc(swap copy)", e); return false; }
    return true;
}

// Free the card copy outright, so cudaMemGetInfo and nvidia-smi both show the
// HBM coming back. Zeroing it would leave the memory allocated and prove less.
bool swap_out(Prover& p, char* err, size_t errlen) {
    if (!p.h_swap || p.swapped_out) return true;
    cudaError_t e = cudaFree(p.d_buf);
    if (e != cudaSuccess) { fail(err, errlen, "cudaFree(swap out)", e); return false; }
    p.d_buf = nullptr;
    p.swapped_out = true;
    return true;
}

bool swap_in(Prover& p, double* ms, char* err, size_t errlen) {
    if (ms) *ms = 0.0;
    if (!p.h_swap || !p.swapped_out) return true;
    auto t0 = std::chrono::steady_clock::now();
    size_t bytes = (size_t)p.held_words * 8;
    cudaError_t e = cudaMalloc((void**)&p.d_buf, bytes);
    if (e != cudaSuccess) { p.d_buf = nullptr; fail(err, errlen, "cudaMalloc(swap in)", e); return false; }
    e = cudaMemcpy(p.d_buf, p.h_swap, bytes, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(swap in)", e); return false; }
    p.swapped_out = false;
    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// ---------------------------------------------------------------------------
// introspection
// ---------------------------------------------------------------------------
bool residency(const Prover& p, const char** backing, bool* is_device) {
    if (backing)   *backing = "unallocated";
    if (is_device) *is_device = false;
    if (!p.d_buf) return false;

    if (p.alloc == Alloc::Split) {
        if (backing) *backing = "split — part on the card, the rest in host memory over PCIe";
        return true;                       // is_device stays false: not all of it is
    }
    if (p.alloc == Alloc::Recompute) {
        if (backing) *backing = "recompute — some blocks on the card, the rest rebuilt on demand";
        return true;                       // is_device stays false: not all of it is
    }
    cudaPointerAttributes a{};
    if (cudaPointerGetAttributes(&a, p.d_buf) != cudaSuccess) {
        if (backing) *backing = "unknown (driver would not say)";
        return false;
    }
    switch (a.type) {
        case cudaMemoryTypeDevice:
            if (backing) *backing = "device — cudaMalloc, not pageable";
            if (is_device) *is_device = true;
            break;
        case cudaMemoryTypeManaged:
            if (backing) *backing = "managed — cudaMallocManaged, pages migrate on fault";
            break;
        case cudaMemoryTypeHost:
            if (backing) *backing = "host — pinned and mapped, every probe crosses PCIe";
            break;
        default:
            if (backing) *backing = "unregistered";
            break;
    }
    return true;
}

// A deliberate memory hog. Nothing reads it; its only job is to occupy HBM so
// the adversary under test has to page. Without it a managed buffer smaller
// than the card migrates fully to the device and is indistinguishable from
// honest — which is a true result, and exactly the GPU echo of the CPU finding
// that mmapdisk looks honest while the page cache can hold its file.
static void* g_hog = nullptr;

bool hog(size_t bytes, char* err, size_t errlen) {
    unhog();
    if (bytes == 0) return true;
    cudaError_t e = cudaMalloc(&g_hog, bytes);
    if (e != cudaSuccess) {
        g_hog = nullptr;
        fail(err, errlen, "cudaMalloc(hog)", e);
        return false;
    }
    return true;
}

void unhog() {
    if (g_hog) { cudaFree(g_hog); g_hog = nullptr; }
}

void mem_info(size_t* free_bytes, size_t* total_bytes) {
    size_t f = 0, t = 0;
    cudaMemGetInfo(&f, &t);
    if (free_bytes)  *free_bytes = f;
    if (total_bytes) *total_bytes = t;
}

// Useful bytes per second: k probes of 8 bytes each. This is deliberately NOT
// the DRAM traffic — an 8-byte probe drags at least a 32-byte sector, so the
// real traffic is several times higher and the gap between the two is the
// argument for widening the probe. Reporting useful bytes keeps that visible
// instead of hiding it in a flattering number.
double resident_fraction(const Prover& p) {
    if (!p.num_words) return 0.0;
    switch (p.alloc) {
        case Alloc::HostMapped: return 0.0;
        case Alloc::Split:      return (double)p.split_words / (double)p.num_words;
        case Alloc::Managed:    return 1.0;   // unknowable from here, by design
        case Alloc::Recompute:  return p.n_blocks ? (double)p.n_kept / (double)p.n_blocks : 0.0;
        case Alloc::Device:     break;
    }
    return (double)p.held_words / (double)p.num_words;   // < 1 only for `fake`
}

double probe_gbps(uint32_t k, double kernel_ms) {
    if (kernel_ms <= 0.0) return 0.0;
    return ((double)k * 8.0) / (kernel_ms / 1000.0) / 1e9;
}

} // namespace gpu
} // namespace mc
