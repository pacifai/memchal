#pragma once
#include <cstdio>
#include "record/gpu_trace.hpp"
#include "gpu/answer.hpp"
#include "gpu/upload.hpp"
#include "shared/server.hpp"

namespace mc {
namespace gpu {

// What a prover tells the verifier and the session file about the buffer it holds.
struct Claim {
    const char* event;
    const char* mode;
    Residency   where;
    double      resident_frac;
    bool        locked;
};

static inline void report_hbm(const char* tag, const char* when) {
    size_t free_b = 0, total_b = 0;
    mem_info(&free_b, &total_b);
    if (!total_b) return;
    printf("[%s] hbm %s: used=%.2f GiB free=%.2f GiB total=%.2f GiB\n", tag, when,
           (double)(total_b - free_b) / 1073741824.0, (double)free_b / 1073741824.0,
           (double)total_b / 1073741824.0);
}

// Loads the buffer onto the card and answers challenges until the verifier
// stops. The honest prover and every cheat that receives the buffer run this one
// function, so a latency difference between them comes from how `p` reads
// memory and never from the code around the answer.
template <typename P>
static inline void serve_buffer(int cfd, const Setup& s, P& p, const char* tag, const Claim& c,
                                uint32_t trace_every) {
    char err[512] = {0};
    Source src{cfd};
    bool ok = !s.build || build_on_host(src, s, err, sizeof err);
    if (ok && s.build) printf("[%s] built on the host in %.3f s; copying to the card\n", tag, src.build_s);
    ok = ok && upload(p, src, s.bytes, err, sizeof err);
    free_built(src);
    if (!ok) { fprintf(stderr, "[%s] FATAL: %s\n", tag, err); return; }

    printf("[%s] buffer %s; backing = %s\n", tag, s.build ? "built and copied" : "uploaded",
           c.where.backing);
    report_hbm(tag, "after upload");
    record::print_buffer_info(c.event, c.mode, c.where.backing, c.where.on_device, c.resident_frac,
                            s.build, src.build_s);
    if (!send_ready(cfd, c.locked)) return;

    Timing t;
    const uint64_t served = answer_challenges(cfd, tag,
        [&](const Challenge& ch, uint64_t* hash) {
            if (answer(p, ch.nonce, ch.samples, ch.lanes, is_chained(ch), is_tree(ch), hash, &t,
                       err, sizeof err))
                return true;
            fprintf(stderr, "[%s] FATAL: %s\n", tag, err);
            return false;
        },
        [&](const Challenge& ch, uint64_t n) {
            if (trace_every && n % trace_every == 0)
                record::print_timing(c.mode, ch, n, t, c.resident_frac, c.where.on_device);
            return true;
        });
    printf("[%s] served %llu challenges\n", tag, (unsigned long long)served);
}

} // namespace gpu
} // namespace mc
