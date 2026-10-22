// bench_build.cpp — build cost of the memory-hard buffer generator.
//
// Reports microseconds per 1 KiB block: the unit of work a prover pays when it
// discarded a block and has to rebuild it inside the verifier's timing budget.
// Absolute times are session-local (ADR 0003); compare within one run only.
//
//   ./bench_build --sweep --json results/build_cost.json
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include <random>
#include <sys/mman.h>
#include "argon2d.hpp"

using namespace mc;
using Clock = std::chrono::steady_clock;

static double secs_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct Row {
    double   mb;
    uint32_t t, p;
    uint32_t blocks;
    double   build_s;
    double   us_per_block;
    double   mib_per_s;
};

// Isolated fill_block() with hot operands: a floor on the per-block rebuild cost.
static double measure_rebuild_us() {
    const int kIters = 200000;
    std::vector<uint64_t> prev(a2::kBlockWords), ref(a2::kBlockWords), next(a2::kBlockWords);
    std::mt19937_64 rng(12345);
    for (size_t i = 0; i < a2::kBlockWords; ++i) { prev[i] = rng(); ref[i] = rng(); next[i] = rng(); }

    // next feeds back in so the compiler cannot hoist the call out of the loop.
    for (int i = 0; i < 1000; ++i) a2::fill_block(prev.data(), ref.data(), next.data(), true);
    auto t0 = Clock::now();
    for (int i = 0; i < kIters; ++i) a2::fill_block(prev.data(), ref.data(), next.data(), true);
    const double s = secs_since(t0);
    // Consume the result so the loop cannot be discarded.
    volatile uint64_t sink = next[0]; (void)sink;
    return s * 1e6 / kIters;
}

static bool run_one(double mb, uint32_t t, uint32_t p, Row* out) {
    Recipe r;
    std::random_device rd;
    for (int i = 0; i < 8; ++i) {
        uint32_t w = rd();
        for (int b = 0; b < 4; ++b) r.key[i * 4 + b] = (uint8_t)(w >> (8 * b));
    }
    for (int i = 0; i < 4; ++i) {
        uint32_t w = rd();
        for (int b = 0; b < 4; ++b) r.salt[i * 4 + b] = (uint8_t)(w >> (8 * b));
    }
    r.m_kib = (uint32_t)(mb * 1024.0);
    r.t = t;
    r.p = p;

    const uint64_t words = build_buffer_words(r);
    if (words == 0) { fprintf(stderr, "[bench] m=%u KiB too small for p=%u\n", r.m_kib, p); return false; }
    const size_t bytes = (size_t)words * 8;

    // Prefault so the build measures the generator, not first-touch page faults.
    void* raw = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("[bench] mmap"); return false; }
    memset(raw, 0, bytes);

    auto t0 = Clock::now();
    const bool ok = build_buffer((uint64_t*)raw, words, r);
    const double build_s = secs_since(t0);
    munmap(raw, bytes);
    if (!ok) { fprintf(stderr, "[bench] build rejected mb=%.1f t=%u p=%u\n", mb, t, p); return false; }

    out->mb           = (double)bytes / 1048576.0;
    out->t            = t;
    out->p            = p;
    out->blocks       = a2::argon2d_blocks(r.m_kib, p);
    out->build_s      = build_s;
    out->us_per_block = build_s * 1e6 / ((double)out->blocks * t);
    out->mib_per_s    = out->mb / build_s;
    return true;
}

int main(int argc, char** argv) {
    double mb = 0; uint32_t t = 2, p = 1;
    bool sweep = false; const char* json = nullptr;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--sweep")) sweep = true;
        else if (!strcmp(argv[i], "--mb")   && i + 1 < argc) mb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--t")    && i + 1 < argc) t  = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--p")    && i + 1 < argc) p  = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else { fprintf(stderr, "usage: bench_build [--mb N] [--t N] [--p N] [--sweep] [--json path]\n"); return 2; }
    }
    if (mb <= 0 && !sweep) sweep = true;

    printf("[bench] Argon2d buffer generator — build cost\n");
    printf("[bench] absolute microseconds are THIS-SESSION-ONLY; compare within this run only\n\n");

    const double rebuild_us = measure_rebuild_us();

    std::vector<Row> rows;
    if (sweep) {
        const double sizes[] = { 16, 64, 256 };
        const uint32_t passes[] = { 1, 2, 4 };
        for (double s : sizes) { Row r; if (run_one(s, 2, p, &r)) rows.push_back(r); }
        for (uint32_t tt : passes) { Row r; if (run_one(64, tt, p, &r)) rows.push_back(r); }
        for (uint32_t pp : {1u, 2u, 4u}) { Row r; if (run_one(64, 2, pp, &r)) rows.push_back(r); }
    } else {
        Row r; if (run_one(mb, t, p, &r)) rows.push_back(r);
    }
    if (rows.empty()) return 1;

    printf("%8s %4s %4s %9s %10s %12s %11s\n",
           "MiB", "t", "p", "blocks", "build_s", "MiB/s", "us/block");
    for (const Row& r : rows)
        printf("%8.1f %4u %4u %9u %10.3f %12.1f %11.3f\n",
               r.mb, r.t, r.p, r.blocks, r.build_s, r.mib_per_s, r.us_per_block);

    printf("\n[bench] isolated fill_block (hot cache): %.3f us per 1 KiB block\n", rebuild_us);
    printf("[bench] that is a FLOOR on one cascade step; a real cascade also pays cache misses.\n");
    printf("[bench] blocks rebuildable inside a budget of:\n");
    for (double budget_us : { 100.0, 1000.0, 10000.0 })
        printf("           %7.0f us  ->  %8.0f blocks\n", budget_us, budget_us / rebuild_us);
    printf("[bench] a k=4096 challenge probes at most 4096 distinct blocks, so a cheat\n");
    printf("        holding fraction f faces roughly 4096*(1-f) misses per challenge,\n");
    printf("        each dragging its own missing ancestors behind it.\n");

    if (json) {
        FILE* f = fopen(json, "w");
        if (!f) { perror("[bench] open json"); return 1; }
        fprintf(f, "{\n  \"generator\": \"argon2d\",\n");
        fprintf(f, "  \"note\": \"absolute times are session-local; compare within one run only\",\n");
        fprintf(f, "  \"rebuild_us_per_block_hot\": %.6f,\n", rebuild_us);
        fprintf(f, "  \"rows\": [\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            fprintf(f, "    {\"mb\": %.3f, \"t\": %u, \"p\": %u, \"blocks\": %u, "
                       "\"build_s\": %.6f, \"mib_per_s\": %.3f, \"us_per_block\": %.6f}%s\n",
                    r.mb, r.t, r.p, r.blocks, r.build_s, r.mib_per_s, r.us_per_block,
                    i + 1 < rows.size() ? "," : "");
        }
        fprintf(f, "  ]\n}\n");
        fclose(f);
        printf("\n[bench] wrote %s\n", json);
    }
    return 0;
}
