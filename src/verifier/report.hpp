#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <vector>
#include "shared/json.hpp"
#include "verifier/args.hpp"

namespace mc {

// A percentile whose tail holds fewer measurements than this is reported as
// unreliable.
static constexpr size_t kMinTail = 10;

struct Percentile { const char* name; double p; };
static const Percentile kPercentiles[] = {
    {"min", 0}, {"p50", 50}, {"p90", 90}, {"p99", 99}, {"p99.9", 99.9}, {"max", 100}};

// Summary statistics of the measured latencies, in microseconds.
class Profile {
public:
    Profile(const std::vector<double>& latencies_ns, uint64_t errors, uint64_t checked,
            uint64_t warmup_errors)
        : errors(errors), checked(checked), warmup_errors(warmup_errors) {
        for (double ns : latencies_ns) sorted_us_.push_back(ns / 1000.0);
        std::sort(sorted_us_.begin(), sorted_us_.end());
        double sum = 0, sq = 0;
        for (double x : sorted_us_) { sum += x; sq += x * x; }
        const size_t n = sorted_us_.size();
        mean_us = n ? sum / n : 0;
        const double var = n ? sq / n - mean_us * mean_us : 0;
        stddev_us = var > 0 ? std::sqrt(var) : 0;
    }

    size_t count() const { return sorted_us_.size(); }

    double at(double p) const {
        if (sorted_us_.empty()) return 0.0;
        return sorted_us_[(size_t)(p / 100.0 * (double)(sorted_us_.size() - 1))];
    }

    // Measurements at or beyond percentile p.
    size_t tail(double p) const {
        const double t = (1.0 - p / 100.0) * (double)count();
        return t < 0 ? 0 : (size_t)(t + 0.5);
    }

    bool reliable(double p) const { return p <= 0 || p >= 100 || tail(p) >= kMinTail; }

    uint64_t errors;
    uint64_t checked;
    uint64_t warmup_errors;
    double   mean_us   = 0;
    double   stddev_us = 0;

private:
    std::vector<double> sorted_us_;
};

static inline void print_profile(const Profile& pr, uint32_t samples) {
    printf("\n=== latency profile (%zu challenges; %llu errors in %llu checked; %llu warmup errors) ===\n",
           pr.count(), (unsigned long long)pr.errors, (unsigned long long)pr.checked,
           (unsigned long long)pr.warmup_errors);
    for (const Percentile& q : kPercentiles)
        printf("  %-6s %10.2f us   [tail n=%zu]%s\n", q.name, pr.at(q.p), pr.tail(q.p),
               pr.reliable(q.p) ? "" : "  UNRELIABLE: n too small for this percentile");
    printf("  %-6s %10.2f us   stddev %8.2f us\n", "mean", pr.mean_us, pr.stddev_us);
    if (pr.mean_us > 0)
        printf("  throughput: %.2f probes/us (%u probes/challenge)\n", samples / pr.mean_us, samples);
}

static inline bool write_csv(const char* path, const std::vector<double>& latencies_ns) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "index,latency_us\n");
    for (size_t i = 0; i < latencies_ns.size(); ++i) fprintf(f, "%zu,%.4f\n", i, latencies_ns[i] / 1000.0);
    return fclose(f) == 0;
}

static inline bool write_json(const char* path, const Args& a, const Profile& pr, bool locked,
                              size_t bytes, double upload_s) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    JsonWriter j(f, true);
    j.begin_object()
        .str("schema", "memchal.verifier.v1")
        .str("label", a.label);
    j.begin_object("env")
        .integer("page_size", sysconf(_SC_PAGESIZE))
        .integer("online_cpus", sysconf(_SC_NPROCESSORS_ONLN))
        .str("clock", "steady_clock")
        .end_object();
    j.begin_object("params")
        .num("mb", a.mib, "%.6g")
        .uint("samples", a.samples)
        .uint("lanes", a.lanes)
        .uint("challenges", a.challenges)
        .uint("warmup", a.warmup)
        .uint("verify_every", a.verify_every)
        .hex("seed", a.seed)
        .str("host", a.host)
        .uint("port", a.port)
        .str("gen", generator_name(a.gen))
        .str("chain", a.chained ? "dependent" : "independent")
        .str("fold", a.tree ? "tree" : "sequential")
        .str("nonce", a.random_nonces ? "random" : "sequential")
        .uint("gap_us", a.gap_us);
    if (a.chained) j.uint("chain_depth", a.samples / a.lanes);
    j.end_object();
    j.begin_object("prover").boolean("locked", locked).end_object();
    if (!a.build())
        j.begin_object("upload")
            .uint("bytes", bytes)
            .num("seconds", upload_s, "%.6f")
            .num("gib_per_s", upload_s > 0 ? bytes / 1073741824.0 / upload_s : 0.0)
            .end_object();
    j.begin_object("result")
        .uint("challenges", pr.count())
        .uint("checked", pr.checked)
        .uint("errors", pr.errors)
        .uint("warmup_errors", pr.warmup_errors)
        .num("mean_us", pr.mean_us)
        .num("stddev_us", pr.stddev_us)
        .num("throughput_probes_per_us", pr.mean_us > 0 ? a.samples / pr.mean_us : 0.0);
    j.begin_object("percentiles_us");
    for (const Percentile& q : kPercentiles)
        j.begin_object(q.name)
            .num("value", pr.at(q.p))
            .uint("tail_n", pr.tail(q.p))
            .boolean("reliable", pr.reliable(q.p))
            .end_object();
    j.end_object().end_object().end_object();
    return fclose(f) == 0;
}

} // namespace mc
