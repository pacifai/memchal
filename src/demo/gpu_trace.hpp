#pragma once
#include <cstdint>
#include <cstdio>
#include "gpu/answer.hpp"
#include "shared/json.hpp"
#include "shared/protocol.hpp"

namespace mc {
namespace demo {

static inline void print_gpu_info(int device, const char* name) {
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    printf("@{\"t\":\"gpuinfo\",\"device\":%d,\"name\":", device);
    json_str(stdout, name);
    printf(",\"hbm_free\":%llu,\"hbm_total\":%llu}\n",
           (unsigned long long)free_b, (unsigned long long)total_b);
}

// Describes the prover's buffer once it holds it.
//   event: "proverinfo" or "cheatinfo".
static inline void print_buffer_info(const char* event, const char* mode, const char* backing,
                                     bool is_device, double resident_frac, bool build,
                                     double build_s) {
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    printf("@{\"t\":\"%s\",\"mode\":", event);
    json_str(stdout, mode);
    printf(",\"backing\":");
    json_str(stdout, backing);
    printf(",\"device_backed\":%s,\"resident_frac\":%.4f,\"hbm_used\":%llu,\"hbm_total\":%llu,"
           "\"build\":%s,\"build_s\":%.4f}\n",
           is_device ? "true" : "false", resident_frac,
           (unsigned long long)(total_b - free_b), (unsigned long long)total_b,
           build ? "true" : "false", build_s);
}

static inline void print_timing(const char* mode, const Challenge& ch, uint64_t served,
                                const gpu::Timing& t, double resident_frac, bool is_device) {
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    printf("@{\"t\":\"g\",\"mode\":");
    json_str(stdout, mode);
    printf(",\"served\":%llu,\"samples\":%u,\"lanes\":%u,\"chained\":%s,\"tree\":%s,"
           "\"kernel_ms\":%.4f,\"copy_ms\":%.4f,\"fold_ms\":%.4f,\"probe_gbps\":%.3f,"
           "\"hbm_used\":%llu,\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s}\n",
           (unsigned long long)served, ch.samples, ch.lanes,
           is_chained(ch) ? "true" : "false", is_tree(ch) ? "true" : "false",
           t.kernel_ms, t.copy_ms, t.fold_ms, gpu::probe_gbps(ch.samples, t.kernel_ms),
           (unsigned long long)(total_b - free_b), (unsigned long long)total_b,
           resident_frac, is_device ? "true" : "false");
}

} // namespace demo
} // namespace mc
