#pragma once
#include <cstdint>
#include <cstdio>
#include "record/event.hpp"
#include "gpu/answer.hpp"
#include "shared/protocol.hpp"

namespace mc {
namespace record {

struct HbmUse {
    uint64_t used  = 0;
    uint64_t total = 0;
};

static inline HbmUse hbm_use() {
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    return HbmUse{total_b - free_b, total_b};
}

static inline void print_gpu_info(int device, const char* name) {
    size_t free_b = 0, total_b = 0;
    gpu::mem_info(&free_b, &total_b);
    Event(stdout, "gpuinfo")
        .integer("device", device)
        .str("name", name)
        .uint("hbm_free", free_b)
        .uint("hbm_total", total_b);
}

// Describes the prover's buffer once it holds it.
//   event: "proverinfo" or "cheatinfo".
static inline void print_buffer_info(const char* event, const char* mode, const char* backing,
                                     bool on_device, double resident_frac, bool build,
                                     double build_s) {
    const HbmUse hbm = hbm_use();
    Event(stdout, event)
        .str("mode", mode)
        .str("backing", backing)
        .boolean("device_backed", on_device)
        .num("resident_frac", resident_frac)
        .uint("hbm_used", hbm.used)
        .uint("hbm_total", hbm.total)
        .boolean("build", build)
        .num("build_s", build_s);
}

static inline void print_timing(const char* mode, const Challenge& ch, uint64_t served,
                                const gpu::Timing& t, double resident_frac, bool on_device) {
    const HbmUse hbm = hbm_use();
    Event(stdout, "g")
        .str("mode", mode)
        .uint("served", served)
        .uint("samples", ch.samples)
        .uint("lanes", ch.lanes)
        .boolean("chained", is_chained(ch))
        .boolean("tree", is_tree(ch))
        .num("kernel_ms", t.kernel_ms)
        .num("copy_ms", t.copy_ms)
        .num("fold_ms", t.fold_ms)
        .num("probe_gbps", gpu::probe_gbps(ch.samples, t.kernel_ms), "%.3f")
        .uint("hbm_used", hbm.used)
        .uint("hbm_total", hbm.total)
        .num("resident_frac", resident_frac)
        .boolean("device", on_device);
}

} // namespace record
} // namespace mc
