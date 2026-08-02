// verifier.cpp — generates a large random buffer, uploads it to the prover,
// then issues nonce challenges and times each round trip. Because it holds its
// own copy, it recomputes every answer locally and checks correctness.
//
// The latency distribution is the actual result: an honest prover with the data
// resident in fast memory shows a low, tight profile; a prover that dropped the
// data, recomputes it, or let it swap to disk shows misses / a fat tail / errors.
#include "common.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <random>

using namespace mc;
using Clock = std::chrono::steady_clock;

static int connect_to(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad host %s\n", host); exit(1); }
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("connect"); exit(1); }
    set_low_latency(fd);
    return fd;
}

static double pctile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p / 100.0 * (double)(sorted.size() - 1);
    size_t i = (size_t)idx;
    return sorted[i]; // nearest-rank (good enough for a profile)
}

struct Args {
    const char* host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    double mb = 256.0;         // buffer size in MiB
    uint32_t k = 65536;        // samples per challenge
    uint32_t lanes = 64;       // parallel lanes (GPU-thread analog)
    uint32_t n = 2000;         // measured challenges
    uint32_t warmup = 200;     // unmeasured warmup challenges
    int sweep = 1;             // run a K sweep at the end
    const char* csv = "latency.csv";
    uint64_t seed = 0xABCDEF0123456789ULL;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc - 1; i += 2) {
        const char* k = argv[i]; const char* v = argv[i + 1];
        if      (!strcmp(k, "--host"))   a.host = v;
        else if (!strcmp(k, "--port"))   a.port = (uint16_t)atoi(v);
        else if (!strcmp(k, "--mb"))     a.mb = atof(v);
        else if (!strcmp(k, "--k"))      a.k = (uint32_t)strtoul(v, nullptr, 10);
        else if (!strcmp(k, "--lanes"))  a.lanes = (uint32_t)strtoul(v, nullptr, 10);
        else if (!strcmp(k, "--n"))      a.n = (uint32_t)strtoul(v, nullptr, 10);
        else if (!strcmp(k, "--warmup")) a.warmup = (uint32_t)strtoul(v, nullptr, 10);
        else if (!strcmp(k, "--sweep"))  a.sweep = atoi(v);
        else if (!strcmp(k, "--csv"))    a.csv = v;
        else if (!strcmp(k, "--seed"))   a.seed = strtoull(v, nullptr, 0);
    }
    return a;
}

// Run `count` challenges at fixed k, return latencies (ns) and #errors.
static void run_batch(int fd, const uint64_t* buf, uint64_t num_words,
                      uint32_t k, uint32_t lanes, uint32_t count,
                      uint64_t nonce_base, std::vector<double>* out_ns, uint64_t* errors) {
    for (uint32_t i = 0; i < count; ++i) {
        Challenge ch{}; ch.nonce = nonce_base + i; ch.k = k; ch.lanes = lanes;
        uint64_t expect = answer_challenge(buf, num_words, ch.nonce, ch.k, ch.lanes);

        auto t0 = Clock::now();
        if (!send_all(fd, &ch, sizeof(ch))) { fprintf(stderr, "send fail\n"); return; }
        Response r{};
        if (!recv_all(fd, &r, sizeof(r))) { fprintf(stderr, "recv fail\n"); return; }
        auto t1 = Clock::now();

        if (r.hash != expect) (*errors)++;
        if (out_ns) out_ns->push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);

    size_t bytes = (size_t)llround(a.mb * 1048576.0);
    bytes = (bytes + 7) & ~size_t(7);
    uint64_t num_words = bytes / 8;

    printf("[verifier] buffer=%.1f MiB k=%u lanes=%u n=%u warmup=%u host=%s:%u\n",
           bytes / 1048576.0, a.k, a.lanes, a.n, a.warmup, a.host, a.port);

    // Generate our reference copy.
    uint64_t* buf = (uint64_t*)malloc(bytes);
    if (!buf) { fprintf(stderr, "malloc failed\n"); return 1; }
    fill_random(buf, num_words, a.seed);

    int fd = connect_to(a.host, a.port);

    // Upload.
    Hello hello{ kMagic, (uint64_t)bytes, 1, 0 };
    if (!send_all(fd, &hello, sizeof(hello))) { fprintf(stderr, "hello send fail\n"); return 1; }
    auto up0 = Clock::now();
    if (!send_all(fd, buf, bytes)) { fprintf(stderr, "data send fail\n"); return 1; }
    auto up1 = Clock::now();
    double up_s = std::chrono::duration<double>(up1 - up0).count();
    printf("[verifier] uploaded in %.3f s (%.2f GiB/s)\n", up_s, bytes / 1073741824.0 / up_s);

    Ready ready{};
    if (!recv_all(fd, &ready, sizeof(ready)) || ready.magic != kMagic) {
        fprintf(stderr, "no/invalid ready\n"); return 1;
    }
    printf("[verifier] prover memory %s\n", ready.locked ? "LOCKED" : "not locked");

    std::mt19937_64 rng(a.seed ^ 0x5555u);
    uint64_t base = rng();

    // Warmup (fault in pages, warm caches/branch predictors) — not measured.
    uint64_t werr = 0;
    run_batch(fd, buf, num_words, a.k, a.lanes, a.warmup, base, nullptr, &werr);
    base += a.warmup;

    // Measured run.
    std::vector<double> lat; lat.reserve(a.n);
    uint64_t errors = 0;
    run_batch(fd, buf, num_words, a.k, a.lanes, a.n, base, &lat, &errors);
    base += a.n;

    // Stats.
    std::vector<double> s = lat; std::sort(s.begin(), s.end());
    double sum = 0, sq = 0; for (double x : lat) { sum += x; sq += x * x; }
    double mean = lat.empty() ? 0 : sum / lat.size();
    double var  = lat.empty() ? 0 : sq / lat.size() - mean * mean;
    double sd   = var > 0 ? std::sqrt(var) : 0;

    printf("\n=== latency profile (%zu samples, %llu errors) ===\n",
           lat.size(), (unsigned long long)errors);
    printf("  min    %10.2f us\n", pctile(s, 0)   / 1000.0);
    printf("  p50    %10.2f us\n", pctile(s, 50)  / 1000.0);
    printf("  p90    %10.2f us\n", pctile(s, 90)  / 1000.0);
    printf("  p99    %10.2f us\n", pctile(s, 99)  / 1000.0);
    printf("  p99.9  %10.2f us\n", pctile(s, 99.9)/ 1000.0);
    printf("  max    %10.2f us\n", pctile(s, 100) / 1000.0);
    printf("  mean   %10.2f us   stddev %8.2f us\n", mean / 1000.0, sd / 1000.0);
    if (mean > 0)
        printf("  throughput: %.2f probes/us (%u probes/challenge)\n", a.k / (mean / 1000.0), a.k);

    // Write per-challenge CSV for plotting.
    if (FILE* f = fopen(a.csv, "w")) {
        fprintf(f, "index,latency_us\n");
        for (size_t i = 0; i < lat.size(); ++i) fprintf(f, "%zu,%.4f\n", i, lat[i] / 1000.0);
        fclose(f);
        printf("[verifier] wrote %s\n", a.csv);
    }

    // Optional sweep: median latency vs samples-per-challenge. Shows the
    // memory-bound scaling — the core evidence that answers require real probes.
    if (a.sweep) {
        printf("\n=== K sweep (median latency vs samples) ===\n");
        printf("   %10s  %12s  %14s\n", "k", "median_us", "probes/us");
        uint32_t ks[] = { 1024, 4096, 16384, 65536, 262144, 1048576 };
        for (uint32_t kk : ks) {
            std::vector<double> sl; uint64_t e = 0;
            run_batch(fd, buf, num_words, kk, a.lanes, 300, base, &sl, &e);
            base += 300;
            std::sort(sl.begin(), sl.end());
            double med = pctile(sl, 50);
            printf("   %10u  %12.2f  %14.2f\n", kk, med / 1000.0, kk / (med / 1000.0));
        }
    }

    // Terminate the prover session cleanly.
    Challenge stop{}; stop.flags = kFlagTerminate;
    send_all(fd, &stop, sizeof(stop));
    close(fd);
    free(buf);
    return 0;
}
