// gpu_resident.hpp — serve challenges over a buffer a GPU prover already holds.
//
// Two callers attach a gpu::Prover to pieces of device memory and hand each
// connection to serve_resident():
//
//   - prover_cuda --weights: the weights in memchal's own allocations, one per
//     tensor, read from the checkpoint the manifest names. The baseline, and a
//     way to test the attached path without an inference engine.
//   - libmemchal_weights.so: the weights where an inference engine keeps them
//     (weights_server.cpp, loaded by the vLLM plugin).
//
// The connection takes a Hello flagged kHelloResident whose size matches what
// is held, then challenges until the verifier stops. It never exits the
// process: inside an engine, a refusal must cost a connection, not the engine.
//
// Host only: sockets from net.hpp, the device behind gpu_answer.hpp.
#pragma once
#include "net.hpp"
#include "gpu_answer.hpp"
#include <cstdio>

namespace mc {

struct ResidentOpts {
    bool allow_unlocked = false;   // serve even if a piece isn't device memory
    uint32_t trace_every = 0;      // one trace line every N challenges (0 = off)
    FILE* trace = nullptr;         // where trace lines go (stdout if null)
    const char* tag = "[gpu]";
};

// The number of challenges served, or -1 if the connection was refused.
static inline long long serve_resident(int cfd, gpu::Prover& p, const ResidentOpts& o) {
    set_low_latency(cfd);
    FILE* tr = o.trace ? o.trace : stdout;

    Hello hello{};
    if (!recv_all(cfd, &hello, sizeof(hello)) || hello.magic != kMagic) {
        fprintf(stderr, "%s no hello\n", o.tag);
        return -1;
    }
    uint64_t bytes = p.num_words * 8;
    if (!(hello.flags & kHelloResident) || hello.buffer_bytes != bytes) {
        fprintf(stderr, "%s refusing: this prover serves only the %llu bytes it holds, with no upload "
                        "(asked for %llu bytes, %s)\n", o.tag, (unsigned long long)bytes,
                (unsigned long long)hello.buffer_bytes,
                (hello.flags & kHelloResident) ? "no upload" : "with an upload");
        return -1;
    }

    const char* backing = "unknown";
    bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    if (!is_device) {
        if (!o.allow_unlocked) {
            fprintf(stderr, "%s refusing: %s\n", o.tag, backing);
            return -1;
        }
        fprintf(stderr, "%s WARNING: %s — reporting locked=false\n", o.tag, backing);
    }
    Ready ready{ kMagic, (uint32_t)(is_device ? 1 : 0), 0 };
    if (!send_all(cfd, &ready, sizeof(ready))) return 0;

    char err[512] = {0};
    long long served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        bool chained = (ch.flags & kFlagChained) != 0;
        bool tree    = (ch.flags & kFlagTreeFold) != 0;

        uint64_t hash = 0;
        double kms = 0, cms = 0, fms = 0;
        if (!gpu::answer(p, ch.nonce, ch.k, ch.lanes, chained, tree,
                         &hash, &kms, &cms, &fms, err, sizeof err)) {
            fprintf(stderr, "%s answer failed: %s\n", o.tag, err);
            break;
        }
        Response resp{ hash };
        if (!send_all(cfd, &resp, sizeof(resp))) break;
        ++served;

        // After the response, outside the verifier's timed window. The same
        // fields prover_cuda traces, so the same tools read both.
        if (o.trace_every && (served % o.trace_every) == 0) {
            size_t hf = 0, ht = 0;
            gpu::mem_info(&hf, &ht);
            fprintf(tr, "@{\"t\":\"g\",\"served\":%lld,\"k\":%u,\"lanes\":%u,"
                        "\"chained\":%s,\"tree\":%s,\"kernel_ms\":%.4f,\"copy_ms\":%.4f,"
                        "\"fold_ms\":%.4f,\"probe_gbps\":%.3f,\"hbm_used\":%llu,"
                        "\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s,\"source\":\"weights\"}\n",
                    served, ch.k, ch.lanes, chained ? "true" : "false", tree ? "true" : "false",
                    kms, cms, fms, gpu::probe_gbps(ch.k, kms),
                    (unsigned long long)(ht - hf), (unsigned long long)ht,
                    gpu::resident_fraction(p), is_device ? "true" : "false");
            fflush(tr);
        }
    }
    return served;
}

} // namespace mc
