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
    Profile(const std::vector<double>& latencies_ns, uint64_t errors, uint64_t checked)
        : errors(errors), checked(checked) {
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
    double   mean_us   = 0;
    double   stddev_us = 0;

private:
    std::vector<double> sorted_us_;
};

static inline void print_profile(const Profile& pr, uint32_t samples) {
    printf("\n=== latency profile (%zu challenges, %llu errors in %llu checked) ===\n",
           pr.count(), (unsigned long long)pr.errors, (unsigned long long)pr.checked);
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
    fclose(f);
    return true;
}

static inline bool write_json(const char* path, const Args& a, const Profile& pr, bool locked,
                              size_t bytes, double upload_s) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"schema\": \"memchal.verifier.v2\",\n  \"label\": ");
    json_str(f, a.label);
    fprintf(f, ",\n  \"env\": {\"page_size\": %ld, \"online_cpus\": %ld, \"clock\": \"steady_clock\"},\n",
            sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
    fprintf(f, "  \"params\": {\"mb\": %.6g, \"samples\": %u, \"lanes\": %u, \"challenges\": %u, "
               "\"warmup\": %u, \"seed\": \"0x%llx\", \"host\": \"%s\", \"port\": %u, "
               "\"gen\": \"%s\", \"chain\": \"%s\", \"fold\": \"%s\", \"nonce\": \"%s\", "
               "\"gap_us\": %u, \"gap_dist\": \"%s\"",
            a.mib, a.samples, a.lanes, a.challenges, a.warmup, (unsigned long long)a.seed, a.host,
            a.port, generator_name(a.gen), a.chained ? "dependent" : "independent",
            a.tree ? "tree" : "sequential", a.random_nonces ? "random" : "sequential", a.gap_us,
            a.gap_exponential ? "exp" : "fixed");
    if (a.chained) fprintf(f, ", \"chain_depth\": %u", a.lanes ? a.samples / a.lanes : a.samples);
    fprintf(f, "},\n  \"prover\": {\"locked\": %s},\n", locked ? "true" : "false");
    fprintf(f, "  \"upload\": {\"bytes\": %zu, \"seconds\": %.6f, \"gib_per_s\": %.4f},\n",
            bytes, upload_s, upload_s > 0 ? bytes / 1073741824.0 / upload_s : 0.0);
    fprintf(f, "  \"result\": {\n    \"challenges\": %zu,\n    \"errors\": %llu,\n"
               "    \"mean_us\": %.4f,\n    \"stddev_us\": %.4f,\n"
               "    \"throughput_probes_per_us\": %.4f,\n    \"percentiles_us\": {",
            pr.count(), (unsigned long long)pr.errors, pr.mean_us, pr.stddev_us,
            pr.mean_us > 0 ? a.samples / pr.mean_us : 0.0);
    bool first = true;
    for (const Percentile& q : kPercentiles) {
        fprintf(f, "%s\n      \"%s\": {\"value\": %.4f, \"tail_n\": %zu, \"reliable\": %s}",
                first ? "" : ",", q.name, pr.at(q.p), pr.tail(q.p), pr.reliable(q.p) ? "true" : "false");
        first = false;
    }
    fprintf(f, "\n    }\n  }\n}\n");
    fclose(f);
    return true;
}

} // namespace mc
