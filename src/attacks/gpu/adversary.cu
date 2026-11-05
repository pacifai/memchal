#include "attacks/gpu/adversary.hpp"
#include "gpu/kernels.cuh"
#include <algorithm>
#include <cstring>

namespace mc {
namespace gpu {

namespace {

struct LoadFake {
    const uint64_t* __restrict__ buf;
    uint64_t held;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const {
        return idx < held ? buf[idx] : splitmix64_word(kGoldenGamma, idx);
    }
};

struct LoadPartial {
    const uint64_t* __restrict__ dev;
    const uint64_t* __restrict__ host;
    uint64_t split;
    __device__ __forceinline__ uint64_t operator()(uint64_t idx) const {
        return idx < split ? dev[idx] : host[idx - split];
    }
};

// Reads a kept or rebuilt block. On a missing block, records it in `need` and
// returns zeros for the rest of the lane.
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
        need[atomicAdd(nneed, 1u)] = b;
        return 0;
    }
};

__global__ void k_lane_recompute(LoadRecompute load, uint64_t num_words, uint64_t nonce,
                                 uint32_t samples, uint32_t lanes, bool chained,
                                 uint8_t* done, uint64_t* out) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l >= lanes || done[l]) return;
    LoadRecompute ld = load;
    ld.missed = false;
    uint64_t h = answer_lane(ld, num_words, nonce, l, samples_in_lane(samples, lanes, l), chained);
    if (!ld.missed) { out[l] = h; done[l] = 1; }
}

__global__ void k_point_slots(int32_t* slot, const uint32_t* ids, uint32_t n, uint32_t first) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) slot[ids[i]] = -2 - (int32_t)(first + i);
}

__global__ void k_clear_slots(int32_t* slot, const uint32_t* ids, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) slot[ids[i]] = -1;
}

constexpr size_t kBlockBytes = (size_t)kRecomputeBlockWords * 8;
constexpr size_t kManagedMargin = (size_t)512 << 20;

void* g_reserved = nullptr;

size_t words_bytes(uint64_t words) { return (size_t)(words ? words : 1) * 8; }

bool failed(Adversary& a, char* err, size_t errlen, const char* what, cudaError_t e) {
    fail(err, errlen, what, e);
    destroy(a);
    return false;
}

bool finish_create(Adversary& a, uint32_t lane_cap, char* err, size_t errlen) {
    if (lanes_create(a.lanes, lane_cap, err, errlen) && staging_create(a.stage, err, errlen))
        return true;
    destroy(a);
    return false;
}

// Moves as much of a managed buffer onto the card as free memory allows.
bool prefetch_managed(Adversary& a, char* err, size_t errlen) {
    size_t free_b = 0, total_b = 0;
    cudaError_t e = cudaMemGetInfo(&free_b, &total_b);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemGetInfo", e); return false; }
    size_t fit = free_b > kManagedMargin ? free_b - kManagedMargin : 0;
    size_t n = std::min(a.num_words * 8, fit);
    if (!n) return true;
    int dev = 0;
    if ((e = cudaGetDevice(&dev)) != cudaSuccess) { fail(err, errlen, "cudaGetDevice", e); return false; }
    e = cudaMemPrefetchAsync(a.d_buf, n, dev, 0);
    if (e == cudaSuccess) e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemPrefetchAsync(managed)", e); return false; }
    return true;
}

bool alloc_need(RecomputeState& r, uint32_t lanes, char* err, size_t errlen) {
    cudaError_t e;
    if ((e = cudaMalloc((void**)&r.d_need, (size_t)lanes * 4)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(need list)", e); return false;
    }
    if ((e = cudaMalloc((void**)&r.d_done, (size_t)lanes)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(done flags)", e); return false;
    }
    if ((e = cudaHostAlloc((void**)&r.h_need, (size_t)lanes * 4, cudaHostAllocDefault)) != cudaSuccess) {
        fail(err, errlen, "cudaHostAlloc(need list)", e); return false;
    }
    e = cudaHostAlloc((void**)&r.h_rebuilt, (size_t)lanes * kBlockBytes, cudaHostAllocDefault);
    if (e != cudaSuccess) { fail(err, errlen, "cudaHostAlloc(rebuilt blocks)", e); return false; }
    return true;
}

bool grow_scratch(RecomputeState& r, uint32_t need, char* err, size_t errlen) {
    if (need <= r.scratch_cap) return true;
    uint32_t cap = r.scratch_cap ? r.scratch_cap : 1024;
    while (cap < need) cap *= 2;
    uint64_t* blk = nullptr;
    uint32_t* ids = nullptr;
    cudaError_t e = cudaMalloc((void**)&blk, (size_t)cap * kBlockBytes);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMalloc(rebuild scratch)", e); return false; }
    if ((e = cudaMalloc((void**)&ids, (size_t)cap * 4)) != cudaSuccess) {
        cudaFree(blk); fail(err, errlen, "cudaMalloc(rebuild scratch ids)", e); return false;
    }
    if (r.scratch_used) {
        e = cudaMemcpy(blk, r.d_scratch, (size_t)r.scratch_used * kBlockBytes, cudaMemcpyDeviceToDevice);
        if (e == cudaSuccess)
            e = cudaMemcpy(ids, r.d_scratch_id, (size_t)r.scratch_used * 4, cudaMemcpyDeviceToDevice);
        if (e != cudaSuccess) {
            cudaFree(blk); cudaFree(ids);
            fail(err, errlen, "cudaMemcpy(rebuild scratch)", e); return false;
        }
    }
    if (r.d_scratch)    cudaFree(r.d_scratch);
    if (r.d_scratch_id) cudaFree(r.d_scratch_id);
    r.d_scratch = blk; r.d_scratch_id = ids; r.scratch_cap = cap;
    return true;
}

} // namespace

void destroy(Adversary& a) {
    RecomputeState& r = a.rc;
    if (r.d_slot)       cudaFree(r.d_slot);
    if (r.d_scratch)    cudaFree(r.d_scratch);
    if (r.d_scratch_id) cudaFree(r.d_scratch_id);
    if (r.d_need)       cudaFree(r.d_need);
    if (r.d_nneed)      cudaFree(r.d_nneed);
    if (r.d_done)       cudaFree(r.d_done);
    if (r.h_need)       cudaFreeHost(r.h_need);
    if (r.h_rebuilt)    cudaFreeHost(r.h_rebuilt);
    if (a.d_buf)  cudaFree(a.d_buf);
    if (a.h_host) cudaFreeHost(a.h_host);
    lanes_destroy(a.lanes);
    staging_destroy(a.stage);
    a = Adversary{};
}

bool create_fake(Adversary& a, uint64_t num_words, double held_fraction, uint32_t lane_cap,
                 char* err, size_t errlen) {
    destroy(a);
    a.cheat = Cheat::Fake;
    a.num_words = num_words;
    a.dev_words = (uint64_t)((double)num_words * std::min(std::max(held_fraction, 0.0), 1.0));
    cudaError_t e = cudaMalloc((void**)&a.d_buf, words_bytes(a.dev_words));
    if (e != cudaSuccess) return failed(a, err, errlen, "cudaMalloc(held part)", e);
    return finish_create(a, lane_cap, err, errlen);
}

bool create_managed(Adversary& a, uint64_t num_words, uint32_t lane_cap, char* err, size_t errlen) {
    destroy(a);
    a.cheat = Cheat::Managed;
    a.num_words = a.dev_words = num_words;
    cudaError_t e = cudaMallocManaged((void**)&a.d_buf, words_bytes(num_words));
    if (e != cudaSuccess) return failed(a, err, errlen, "cudaMallocManaged(buffer)", e);
    return finish_create(a, lane_cap, err, errlen);
}

bool create_partial(Adversary& a, uint64_t num_words, double offload_fraction, uint32_t lane_cap,
                    char* err, size_t errlen) {
    destroy(a);
    a.cheat = Cheat::Partial;
    a.num_words = num_words;
    const double off = std::min(std::max(offload_fraction, 0.0), 1.0);
    a.dev_words = num_words - (uint64_t)((double)num_words * off);
    cudaError_t e;
    if (a.dev_words && (e = cudaMalloc((void**)&a.d_buf, a.dev_words * 8)) != cudaSuccess)
        return failed(a, err, errlen, "cudaMalloc(resident part)", e);
    if (a.dev_words < num_words) {
        e = cudaHostAlloc(&a.h_host, (num_words - a.dev_words) * 8, cudaHostAllocMapped);
        if (e != cudaSuccess) return failed(a, err, errlen, "cudaHostAlloc(offloaded part)", e);
        e = cudaHostGetDevicePointer((void**)&a.d_host, a.h_host, 0);
        if (e != cudaSuccess) return failed(a, err, errlen, "cudaHostGetDevicePointer", e);
    }
    return finish_create(a, lane_cap, err, errlen);
}

bool create_recompute(Adversary& a, uint32_t n_blocks, const int32_t* slot,
                      const uint64_t* kept_words, uint32_t n_kept, uint32_t lane_cap,
                      char* err, size_t errlen) {
    destroy(a);
    a.cheat = Cheat::Recompute;
    a.num_words = (uint64_t)n_blocks * kRecomputeBlockWords;
    a.dev_words = (uint64_t)n_kept * kRecomputeBlockWords;
    a.rc.n_blocks = n_blocks;
    cudaError_t e = cudaMalloc((void**)&a.d_buf, words_bytes(a.dev_words));
    if (e != cudaSuccess) return failed(a, err, errlen, "cudaMalloc(kept blocks)", e);
    if (n_kept && (e = cudaMemcpy(a.d_buf, kept_words, a.dev_words * 8,
                                  cudaMemcpyHostToDevice)) != cudaSuccess)
        return failed(a, err, errlen, "cudaMemcpy(kept blocks)", e);
    if ((e = cudaMalloc((void**)&a.rc.d_slot, (size_t)n_blocks * 4)) != cudaSuccess)
        return failed(a, err, errlen, "cudaMalloc(slot table)", e);
    if ((e = cudaMemcpy(a.rc.d_slot, slot, (size_t)n_blocks * 4, cudaMemcpyHostToDevice)) != cudaSuccess)
        return failed(a, err, errlen, "cudaMemcpy(slot table)", e);
    if ((e = cudaMalloc((void**)&a.rc.d_nneed, 4)) != cudaSuccess)
        return failed(a, err, errlen, "cudaMalloc(need count)", e);
    if (!alloc_need(a.rc, lane_cap, err, errlen)) { destroy(a); return false; }
    return finish_create(a, lane_cap, err, errlen);
}

bool push(Adversary& a, size_t bytes, char* err, size_t errlen) {
    if (a.cheat == Cheat::Recompute) {
        failmsg(err, errlen, "create_recompute() places a recompute buffer"); return false;
    }
    if (bytes > a.stage.cap) { failmsg(err, errlen, "push larger than the staging buffer"); return false; }
    const size_t at = a.stage.cursor, seam = a.dev_words * 8, end = a.num_words * 8;
    if (at + bytes > end) { failmsg(err, errlen, "push past the end of the buffer"); return false; }

    const size_t dev = at < seam ? std::min(bytes, seam - at) : 0;
    unsigned char* dst = (unsigned char*)a.d_buf + at;
    if (dev && a.cheat == Cheat::Managed) {
        memcpy(dst, a.stage.h, dev);
    } else if (dev) {
        cudaError_t e = cudaMemcpy(dst, a.stage.h, dev, cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(upload)", e); return false; }
    }
    if (a.cheat == Cheat::Partial && bytes > dev)
        memcpy((unsigned char*)a.h_host + (at + dev - seam), a.stage.h + dev, bytes - dev);
    a.stage.cursor += bytes;

    if (a.cheat == Cheat::Managed && a.stage.cursor == end) return prefetch_managed(a, err, errlen);
    return true;
}

bool answer(Adversary& a, uint64_t nonce, uint32_t samples, uint32_t lanes, bool chained, bool tree,
            uint64_t* out_hash, Timing* t, char* err, size_t errlen) {
    switch (a.cheat) {
        case Cheat::Fake:
            return answer_with(a.lanes, LoadFake{a.d_buf, a.dev_words}, a.num_words, nonce, samples,
                               lanes, chained, tree, out_hash, t, err, errlen);
        case Cheat::Managed:
            return answer_with(a.lanes, LoadDirect{a.d_buf}, a.num_words, nonce, samples, lanes,
                               chained, tree, out_hash, t, err, errlen);
        case Cheat::Partial:
            return answer_with(a.lanes, LoadPartial{a.d_buf, a.d_host, a.dev_words}, a.num_words,
                               nonce, samples, lanes, chained, tree, out_hash, t, err, errlen);
        case Cheat::Recompute:
            break;
    }
    failmsg(err, errlen, "a recompute buffer answers through answer_recompute()");
    return false;
}

bool answer_recompute(Adversary& a, uint64_t nonce, uint32_t samples, uint32_t lanes, bool chained,
                      bool tree, RebuildFn rebuild, void* ctx, uint64_t* out_hash, Timing* t,
                      RecomputeStats* st, char* err, size_t errlen) {
    if (a.cheat != Cheat::Recompute) { failmsg(err, errlen, "not a recompute buffer"); return false; }
    RecomputeState& r = a.rc;
    if (!lanes_fit(a.lanes, lanes, err, errlen)) return false;
    *st = RecomputeStats{};

    auto t0 = std::chrono::steady_clock::now();
    cudaError_t e = cudaMemset(r.d_done, 0, lanes);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemset(done flags)", e); return false; }
    for (;;) {
        if ((e = cudaMemset(r.d_nneed, 0, 4)) != cudaSuccess) {
            fail(err, errlen, "cudaMemset(need count)", e); return false;
        }
        LoadRecompute ld{a.d_buf, r.d_scratch, r.d_slot, r.d_need, r.d_nneed, false};
        k_lane_recompute<<<blocks_for(lanes), kThreadsPerBlock>>>(ld, a.num_words, nonce, samples, lanes,
                                                                  chained, r.d_done, a.lanes.d_hash);
        if (!launched("k_lane_recompute launch", err, errlen)) return false;
        ++st->rounds;
        uint32_t nneed = 0;
        if ((e = cudaMemcpy(&nneed, r.d_nneed, 4, cudaMemcpyDeviceToHost)) != cudaSuccess) {
            fail(err, errlen, "k_lane_recompute", e); return false;
        }
        if (nneed == 0) break;

        e = cudaMemcpy(r.h_need, r.d_need, (size_t)nneed * 4, cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(need list)", e); return false; }
        std::sort(r.h_need, r.h_need + nneed);
        const uint32_t n = (uint32_t)(std::unique(r.h_need, r.h_need + nneed) - r.h_need);

        auto r0 = std::chrono::steady_clock::now();
        if (!rebuild(ctx, r.h_need, n, r.h_rebuilt)) {
            failmsg(err, errlen, "rebuild callback failed"); return false;
        }
        st->rebuild_ms += ms_since(r0);

        if (!grow_scratch(r, r.scratch_used + n, err, errlen)) return false;
        e = cudaMemcpy(r.d_scratch + (size_t)r.scratch_used * kRecomputeBlockWords, r.h_rebuilt,
                       (size_t)n * kBlockBytes, cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(rebuilt blocks)", e); return false; }
        e = cudaMemcpy(r.d_scratch_id + r.scratch_used, r.h_need, (size_t)n * 4, cudaMemcpyHostToDevice);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(rebuilt ids)", e); return false; }
        k_point_slots<<<blocks_for(n), kThreadsPerBlock>>>(r.d_slot, r.d_scratch_id + r.scratch_used,
                                                           n, r.scratch_used);
        if (!launched("k_point_slots launch", err, errlen)) return false;
        r.scratch_used += n;
        st->rebuilt += n;
    }
    t->kernel_ms = ms_since(t0);
    return fold(a.lanes, nonce, samples, lanes, tree, out_hash, t, err, errlen);
}

bool recompute_forget(Adversary& a, char* err, size_t errlen) {
    RecomputeState& r = a.rc;
    if (a.cheat != Cheat::Recompute || r.scratch_used == 0) return true;
    k_clear_slots<<<blocks_for(r.scratch_used), kThreadsPerBlock>>>(r.d_slot, r.d_scratch_id,
                                                                    r.scratch_used);
    if (!launched("k_clear_slots launch", err, errlen)) return false;
    r.scratch_used = 0;
    cudaError_t e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { fail(err, errlen, "k_clear_slots", e); return false; }
    return true;
}

const char* residency(const Adversary& a, bool* is_device) {
    *is_device = false;
    if (a.cheat == Cheat::Recompute)
        return "recompute — some blocks on the card, the rest rebuilt on demand";
    if (a.d_buf && a.h_host) return "split — part on the card, the rest in host memory over PCIe";
    return describe_pointer(a.d_buf ? (const void*)a.d_buf : a.h_host, is_device);
}

double resident_fraction(const Adversary& a) {
    return a.num_words ? (double)a.dev_words / (double)a.num_words : 0.0;
}

bool reserve_hbm(size_t bytes, char* err, size_t errlen) {
    release_hbm();
    if (bytes == 0) return true;
    cudaError_t e = cudaMalloc(&g_reserved, bytes);
    if (e != cudaSuccess) {
        g_reserved = nullptr;
        fail(err, errlen, "cudaMalloc(reserve)", e);
        return false;
    }
    return true;
}

void release_hbm() {
    if (g_reserved) { cudaFree(g_reserved); g_reserved = nullptr; }
}

} // namespace gpu
} // namespace mc
