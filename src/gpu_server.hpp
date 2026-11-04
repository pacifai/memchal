// gpu_server.hpp — the host side both GPU provers share. Host only: never
// include it from a .cu file.
#pragma once
#include "gpu_answer.hpp"
#include "server.hpp"

namespace mc {

// Copies `bytes` from src onto the card through the prover's pinned staging,
// so an upload needs no host copy of the whole buffer.
template <typename P>
static inline bool ingest(P& p, Source& src, size_t bytes, char* err, size_t errlen) {
    size_t left = bytes;
    while (left) {
        size_t chunk = left < p.stage.cap ? left : p.stage.cap;
        if (!src_read(src, p.stage.h, chunk)) {
            snprintf(err, errlen, "short data (%zu bytes outstanding)", left);
            return false;
        }
        if (!push(p, chunk, err, errlen)) return false;
        left -= chunk;
    }
    return true;
}

static inline void print_gpuinfo(int device, const char* name) {
    size_t f = 0, t = 0;
    gpu::mem_info(&f, &t);
    printf("@{\"t\":\"gpuinfo\",\"device\":%d,\"name\":", device);
    json_str(stdout, name);
    printf(",\"hbm_free\":%llu,\"hbm_total\":%llu}\n", (unsigned long long)f, (unsigned long long)t);
}

static inline void print_trace(const char* mode, const Challenge& ch, uint64_t served,
                               const gpu::Timing& t, double resident_frac, bool is_device) {
    size_t f = 0, total = 0;
    gpu::mem_info(&f, &total);
    printf("@{\"t\":\"g\",\"mode\":");
    json_str(stdout, mode);
    printf(",\"served\":%llu,\"k\":%u,\"lanes\":%u,\"chained\":%s,\"tree\":%s,"
           "\"kernel_ms\":%.4f,\"copy_ms\":%.4f,\"fold_ms\":%.4f,\"probe_gbps\":%.3f,"
           "\"hbm_used\":%llu,\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s}\n",
           (unsigned long long)served, ch.k, ch.lanes,
           is_chained(ch) ? "true" : "false", is_tree(ch) ? "true" : "false",
           t.kernel_ms, t.copy_ms, t.fold_ms, gpu::probe_gbps(ch.k, t.kernel_ms),
           (unsigned long long)(total - f), (unsigned long long)total,
           resident_frac, is_device ? "true" : "false");
}

} // namespace mc
