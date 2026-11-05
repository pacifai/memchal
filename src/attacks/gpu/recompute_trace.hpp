#pragma once
#include <cstdint>
#include <cstdio>
#include "attacks/gpu/recompute.hpp"
#include "demo/event.hpp"
#include "demo/gpu_trace.hpp"

namespace mc {

static inline void print_recompute_info(const RecomputeHost& h, const RecomputeOptions& opt,
                                        const Setup& s, double build_s) {
    const demo::HbmUse hbm = demo::hbm_use();
    demo::Event(stdout, "cheatinfo")
        .str("mode", "recompute")
        .num("keep", opt.keep, "%.6f")
        .uint("kept_blocks", h.n_kept())
        .uint("blocks", h.shape.blocks)
        .uint("build_passes", s.recipe.passes)
        .uint("build_parallelism", s.recipe.parallelism)
        .uint("threads", h.pool->size())
        .uint("budget", opt.budget)
        .uint("hbm_used", hbm.used)
        .uint("hbm_total", hbm.total)
        .boolean("build", true)
        .num("build_s", build_s);
}

// The recompute attack's rebuild work: an "rc" event every `every` challenges,
// and totals when the session ends.
class RecomputeTrace {
public:
    explicit RecomputeTrace(uint32_t every) : every_(every) {}

    void after(const Challenge& ch, uint64_t served, const gpu::RecomputeStats& st,
               const gpu::Timing& t, const RecomputeHost& h) {
        const uint64_t fills   = h.total(&Rebuilder::Stats::fills);
        const uint64_t gave_up = h.total(&Rebuilder::Stats::gave_up);
        if (every_ && served % every_ == 0)
            demo::Event(stdout, "rc")
                .uint("served", served)
                .uint("samples", ch.samples)
                .uint("lanes", ch.lanes)
                .uint("rounds", st.rounds)
                .uint("rebuilt", st.rebuilt)
                .uint("fills", fills - fills_)
                .uint("gave_up", gave_up - gave_up_)
                .num("rebuild_ms", st.rebuild_ms)
                .num("kernel_ms", t.kernel_ms)
                .num("copy_ms", t.copy_ms)
                .num("fold_ms", t.fold_ms);
        if (gave_up > gave_up_) ++gave_up_challenges_;
        fills_   = fills;
        gave_up_ = gave_up;
        rebuilt_ += st.rebuilt;
        rounds_ += st.rounds;
        rebuild_ms_ += st.rebuild_ms;
        kernel_ms_ += t.kernel_ms;
    }

    void finish(uint64_t served, const RecomputeHost& h) const {
        const double n = served ? (double)served : 1.0;
        printf("[cheat] served %llu challenges; %.1f blocks rebuilt, %.1f rounds, "
               "%.3f ms rebuilding per challenge; %llu challenge(s) gave up\n",
               (unsigned long long)served, rebuilt_ / n, rounds_ / n, rebuild_ms_ / n,
               (unsigned long long)gave_up_challenges_);
        if (!every_) return;
        demo::Event(stdout, "rc_summary")
            .uint("served", served)
            .num("rebuilt_per_ch", rebuilt_ / n, "%.3f")
            .num("rounds_per_ch", rounds_ / n, "%.3f")
            .num("rebuild_ms_per_ch", rebuild_ms_ / n)
            .num("kernel_ms_per_ch", kernel_ms_ / n)
            .uint("fills", h.total(&Rebuilder::Stats::fills))
            .uint("seeds", h.total(&Rebuilder::Stats::seeds))
            .uint("gave_up", h.total(&Rebuilder::Stats::gave_up))
            .uint("gave_up_challenges", gave_up_challenges_)
            .num("memo_peak_mib",
                 h.total(&Rebuilder::Stats::memo_peak) * a2::kBlockBytes / 1048576.0, "%.2f");
    }

private:
    uint32_t every_;
    uint64_t fills_ = 0, gave_up_ = 0, gave_up_challenges_ = 0;
    uint64_t rebuilt_ = 0, rounds_ = 0;
    double   rebuild_ms_ = 0, kernel_ms_ = 0;
};

} // namespace mc
