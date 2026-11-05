#include "gpu/kernels.cuh"

namespace mc {
namespace gpu {

namespace {

constexpr size_t kStagingBytes = 8u << 20;

__global__ void k_tree_leaves(uint64_t* v, uint32_t lanes) {
    uint32_t l = blockIdx.x * blockDim.x + threadIdx.x;
    if (l < lanes) v[l] = lane_fold_term(v[l], l);
}

__global__ void k_tree_level(const uint64_t* __restrict__ src, uint64_t* __restrict__ dst,
                             uint32_t n, uint32_t level) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= (n + 1) / 2) return;
    uint64_t a = src[2 * i];
    uint64_t b = (2 * i + 1 < n) ? src[2 * i + 1] : tree_pad(level, 2 * i + 1);
    dst[i] = tree_combine(a, b);
}

__global__ void k_fill(uint64_t* buf, uint64_t n, uint64_t seed) {
    uint64_t i      = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    uint64_t stride = (uint64_t)blockDim.x * gridDim.x;
    for (; i < n; i += stride) buf[i] = splitmix64_word(seed, i);
}

void free_hashes(Lanes& l) {
    if (l.d_hash)  cudaFree(l.d_hash);
    if (l.d_level) cudaFree(l.d_level);
    if (l.h_hash)  cudaFreeHost(l.h_hash);
    l.d_hash = l.d_level = l.h_hash = nullptr;
    l.cap = 0;
}

bool alloc_hashes(Lanes& l, uint32_t n, char* err, size_t errlen) {
    cudaError_t e;
    if ((e = cudaMalloc((void**)&l.d_hash, (size_t)n * 8)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(lane hashes)", e); return false;
    }
    if ((e = cudaMalloc((void**)&l.d_level, (size_t)n * 8)) != cudaSuccess) {
        fail(err, errlen, "cudaMalloc(tree level)", e); return false;
    }
    if ((e = cudaHostAlloc((void**)&l.h_hash, (size_t)n * 8, cudaHostAllocDefault)) != cudaSuccess) {
        fail(err, errlen, "cudaHostAlloc(lane hashes)", e); return false;
    }
    l.cap = n;
    return true;
}

} // namespace

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

void mem_info(size_t* free_bytes, size_t* total_bytes) {
    size_t f = 0, t = 0;
    if (cudaMemGetInfo(&f, &t) != cudaSuccess) f = t = 0;
    if (free_bytes)  *free_bytes = f;
    if (total_bytes) *total_bytes = t;
}

double probe_gbps(uint32_t samples, double kernel_ms) {
    if (kernel_ms <= 0.0) return 0.0;
    return ((double)samples * 8.0) / (kernel_ms / 1000.0) / 1e9;
}

bool lanes_create(Lanes& l, uint32_t cap, char* err, size_t errlen) {
    if (!alloc_hashes(l, cap, err, errlen)) return false;
    cudaEvent_t ev = nullptr;
    cudaError_t e = cudaEventCreate(&ev);
    if (e != cudaSuccess) { fail(err, errlen, "cudaEventCreate", e); return false; }
    l.ev_start = ev;
    if ((e = cudaEventCreate(&ev)) != cudaSuccess) { fail(err, errlen, "cudaEventCreate", e); return false; }
    l.ev_stop = ev;
    return true;
}

void lanes_destroy(Lanes& l) {
    free_hashes(l);
    if (l.ev_start) cudaEventDestroy((cudaEvent_t)l.ev_start);
    if (l.ev_stop)  cudaEventDestroy((cudaEvent_t)l.ev_stop);
    l = Lanes{};
}

bool lanes_fit(const Lanes& l, uint32_t lanes, char* err, size_t errlen) {
    if (lanes <= l.cap) return true;
    if (err && errlen) snprintf(err, errlen, "challenge has %u lanes, above the lane cap of %u", lanes, l.cap);
    return false;
}

bool staging_create(Staging& s, char* err, size_t errlen) {
    cudaError_t e = cudaHostAlloc((void**)&s.h, kStagingBytes, cudaHostAllocDefault);
    if (e != cudaSuccess) { fail(err, errlen, "cudaHostAlloc(upload staging)", e); return false; }
    s.cap = kStagingBytes;
    s.cursor = 0;
    return true;
}

void staging_destroy(Staging& s) {
    if (s.h) cudaFreeHost(s.h);
    s = Staging{};
}

bool fold(Lanes& l, uint64_t nonce, uint32_t samples, uint32_t lanes, bool tree,
          uint64_t* out_hash, Timing* t, char* err, size_t errlen) {
    cudaError_t e;
    if (tree) {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t* src = l.d_hash;
        uint64_t* dst = l.d_level;
        k_tree_leaves<<<blocks_for(lanes), kThreadsPerBlock>>>(src, lanes);
        if (!launched("k_tree_leaves launch", err, errlen)) return false;
        for (uint32_t n = lanes, level = 0; n > 1; n = (n + 1) / 2, ++level) {
            k_tree_level<<<blocks_for((n + 1) / 2), kThreadsPerBlock>>>(src, dst, n, level);
            if (!launched("k_tree_level launch", err, errlen)) return false;
            uint64_t* swap = src; src = dst; dst = swap;
        }
        if ((e = cudaDeviceSynchronize()) != cudaSuccess) { fail(err, errlen, "tree fold", e); return false; }
        t->fold_ms = ms_since(t0);

        auto t1 = std::chrono::steady_clock::now();
        uint64_t root = 0;
        e = cudaMemcpy(&root, src, sizeof root, cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(root)", e); return false; }
        t->copy_ms = ms_since(t1);
        *out_hash = mix64(root ^ fold_seed(nonce, samples, lanes));
        return true;
    }

    auto t0 = std::chrono::steady_clock::now();
    e = cudaMemcpy(l.h_hash, l.d_hash, (size_t)lanes * 8, cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(lane hashes)", e); return false; }
    t->copy_ms = ms_since(t0);

    auto t1 = std::chrono::steady_clock::now();
    uint64_t combined = fold_seed(nonce, samples, lanes);
    for (uint32_t i = 0; i < lanes; ++i)
        combined = mix64(combined ^ lane_fold_term(l.h_hash[i], i));
    t->fold_ms = ms_since(t1);
    *out_hash = combined;
    return true;
}

const char* describe_pointer(const void* ptr, bool* is_device) {
    *is_device = false;
    cudaPointerAttributes a{};
    if (!ptr || cudaPointerGetAttributes(&a, ptr) != cudaSuccess) return "unknown";
    switch (a.type) {
        case cudaMemoryTypeDevice:
            *is_device = true;
            return "device — cudaMalloc, not pageable";
        case cudaMemoryTypeManaged: return "managed — cudaMallocManaged, pages migrate on fault";
        case cudaMemoryTypeHost:    return "host — pinned and mapped, every probe crosses PCIe";
        default:                    return "unregistered";
    }
}

void destroy(Prover& p) {
    if (p.d_buf) cudaFree(p.d_buf);
    lanes_destroy(p.lanes);
    staging_destroy(p.stage);
    p = Prover{};
}

bool create(Prover& p, uint64_t num_words, uint32_t lane_cap, char* err, size_t errlen) {
    destroy(p);
    p.num_words = num_words;
    cudaError_t e = cudaMalloc((void**)&p.d_buf, (size_t)(num_words ? num_words : 1) * 8);
    if (e != cudaSuccess) fail(err, errlen, "cudaMalloc(buffer)", e);
    else if (lanes_create(p.lanes, lane_cap, err, errlen) && staging_create(p.stage, err, errlen))
        return true;
    destroy(p);
    return false;
}

bool push(Prover& p, size_t bytes, char* err, size_t errlen) {
    if (bytes > p.stage.cap) { failmsg(err, errlen, "push larger than the staging buffer"); return false; }
    if (p.stage.cursor + bytes > p.num_words * 8) {
        failmsg(err, errlen, "push past the end of the buffer"); return false;
    }
    cudaError_t e = cudaMemcpy((unsigned char*)p.d_buf + p.stage.cursor, p.stage.h, bytes,
                               cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { fail(err, errlen, "cudaMemcpy(upload)", e); return false; }
    p.stage.cursor += bytes;
    return true;
}

bool fill_splitmix64(Prover& p, uint64_t seed, char* err, size_t errlen) {
    if (!p.d_buf) { failmsg(err, errlen, "no buffer to fill"); return false; }
    k_fill<<<1024, kThreadsPerBlock>>>(p.d_buf, p.num_words, seed);
    if (!launched("k_fill launch", err, errlen)) return false;
    cudaError_t e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { fail(err, errlen, "k_fill", e); return false; }
    p.stage.cursor = p.num_words * 8;
    return true;
}

bool answer(Prover& p, uint64_t nonce, uint32_t samples, uint32_t lanes, bool chained, bool tree,
            uint64_t* out_hash, Timing* t, char* err, size_t errlen) {
    return answer_with(p.lanes, LoadDirect{p.d_buf}, p.num_words, nonce, samples, lanes, chained, tree,
                       out_hash, t, err, errlen);
}

const char* residency(const Prover& p, bool* is_device) {
    return describe_pointer(p.d_buf, is_device);
}

} // namespace gpu
} // namespace mc
