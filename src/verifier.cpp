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

// How many samples lie in the tail at or beyond percentile p, i.e. how many
// observations actually support the estimate. p99.9 from n=300 has ~0 tail
// samples, so it is essentially one draw and must not be reported as a number.
static size_t tail_count(double p, size_t n) {
    double t = (1.0 - p / 100.0) * (double)n;
    return t < 0 ? 0 : (size_t)(t + 0.5);
}
// A percentile is trustworthy only if at least this many samples fall in its
// tail. Below it, we print the value but flag it UNRELIABLE (and mark it so in
// JSON) rather than letting a single sample masquerade as a percentile.
static constexpr size_t kMinTail = 10;

enum class NonceMode { SEQUENTIAL, RANDOM };

struct Args {
    const char* host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    double mb = 256.0;         // buffer size in MiB
    uint32_t k = 65536;        // samples per challenge
    uint32_t lanes = 64;       // parallel lanes (GPU-thread analog)
    uint32_t n = 2000;         // measured challenges
    uint32_t warmup = 200;     // unmeasured warmup challenges
    const char* csv = "latency.csv";
    const char* json = nullptr;  // machine-readable summary (scripts consume this)
    const char* label = "";      // free-form environment/run label, copied into JSON
    uint64_t seed = 0xABCDEF0123456789ULL;
    // The three defenses default ON (secure by default). The benchmark opt-out is
    // to name the weak setting explicitly: --gen splitmix64 / --chain independent /
    // --nonce sequential. See ADR 0004 (Safety) and the README adversary table.
    Gen gen = Gen::CHACHA20;    // buffer generator: chacha20 (default) or splitmix64
    // Live streaming for the browser demo (demo/bridge.py). Off unless asked for:
    // the experiment scripts must keep measuring an uninstrumented loop.
    const char* stream = nullptr;   // "-" for stdout, else a path
    uint32_t probe_map_every = 0;   // emit the probed-page bitmap every N challenges
    bool chained = true;            // --chain independent -> off; dependent is default
    bool tree = false;              // --fold tree -> parallel Merkle fold (different answers)
    uint32_t verify_every = 1;      // check 1 in N answers; 1 = check all
    NonceMode nonce_mode = NonceMode::RANDOM; // --nonce sequential|random (random default)
    uint32_t gap_us = 0;            // --gap-us N: sleep N microseconds between challenges
    bool gap_random = false;        // --gap-dist exp: draw each gap from an exponential
                                    // with mean gap_us instead of sleeping exactly gap_us
};

// The swap defense. A fixed gap tells the prover when the next challenge will
// arrive, so it can lend its memory out in between and load the buffer back
// just in time. Exponential gaps are memoryless: however long the prover has
// waited, the next challenge is as likely to come now as later, so no moment
// is safe to be off the card. Draws come from a random_device-seeded generator
// the prover never sees.
static bool g_gap_random = false;

static void gap_sleep(uint32_t gap_us) {
    if (gap_us == 0) return;
    if (!g_gap_random) { usleep(gap_us); return; }
    static std::mt19937_64 rng{std::random_device{}()};
    std::exponential_distribution<double> d(1.0 / (double)gap_us);
    double us = d(rng);
    if (us > 20.0 * gap_us) us = 20.0 * gap_us;   // bound the rare very long draw
    usleep((useconds_t)us);
}

// ---------------------------------------------------------------------------
// Live event stream (browser demo only)
// ---------------------------------------------------------------------------
// Events are one JSON object per line, prefixed '@' so they can share stdout
// with the human-readable output without either needing to know about the other.
//
// COST: every write happens AFTER the round trip has been timed and before the
// next challenge is sent, so it is outside all measurement windows. It still
// spaces the challenges out (a few us of gap), which is why the terminal
// experiments — not this stream — produce the headline numbers.
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_bits(FILE* f, const uint8_t* flags, size_t count) {
    // Pack one bit per flag (LSB-first within each byte), then base64 it.
    size_t nbytes = (count + 7) / 8;
    std::vector<uint8_t> bits(nbytes, 0);
    for (size_t i = 0; i < count; ++i)
        if (flags[i]) bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    for (size_t i = 0; i < nbytes; i += 3) {
        uint32_t v = (uint32_t)bits[i] << 16;
        if (i + 1 < nbytes) v |= (uint32_t)bits[i + 1] << 8;
        if (i + 2 < nbytes) v |= (uint32_t)bits[i + 2];
        fputc(kB64[(v >> 18) & 63], f);
        fputc(kB64[(v >> 12) & 63], f);
        fputc(i + 1 < nbytes ? kB64[(v >> 6) & 63] : '=', f);
        fputc(i + 2 < nbytes ? kB64[v & 63] : '=', f);
    }
}

struct Stream {
    FILE* f = nullptr;
    uint32_t map_every = 0;
    uint64_t page_words = 512;        // 4096-byte page / 8-byte word
    std::vector<uint8_t> pages;       // scratch: probed-page flags for one challenge
    bool wants_map(uint64_t idx) const { return f && map_every && (idx % map_every) == 0; }
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
        else if (!strcmp(k, "--csv"))    a.csv = v;
        else if (!strcmp(k, "--json"))   a.json = v;
        else if (!strcmp(k, "--label"))  a.label = v;
        else if (!strcmp(k, "--seed"))   a.seed = strtoull(v, nullptr, 0);
        else if (!strcmp(k, "--gen")) {
            if      (!strcmp(v, "chacha20") || !strcmp(v, "chacha")) a.gen = Gen::CHACHA20;
            else                                                      a.gen = Gen::SPLITMIX64;
        }
        else if (!strcmp(k, "--stream")) a.stream = v;
        else if (!strcmp(k, "--probe-map")) a.probe_map_every = (uint32_t)strtoul(v, nullptr, 10);
        else if (!strcmp(k, "--chain")) {
            a.chained = (!strcmp(v, "dependent"));
        }
        else if (!strcmp(k, "--verify-every")) {
            a.verify_every = (uint32_t)strtoul(v, nullptr, 10);
            if (a.verify_every == 0) a.verify_every = 1;
        }
        else if (!strcmp(k, "--fold")) {
            // The tree fold is a different protocol with different answers, so
            // it is opt-in and travels on the wire. Default stays sequential.
            a.tree = (!strcmp(v, "tree"));
        }
        else if (!strcmp(k, "--nonce")) {
            if (!strcmp(v, "random")) a.nonce_mode = NonceMode::RANDOM;
            else                     a.nonce_mode = NonceMode::SEQUENTIAL;
        }
        else if (!strcmp(k, "--gap-us")) {
            a.gap_us = (uint32_t)strtoul(v, nullptr, 10);
        }
        else if (!strcmp(k, "--gap-dist")) {
            a.gap_random = !strcmp(v, "exp");
        }
    }
    return a;
}

// Run `count` challenges at fixed k, return latencies (ns) and #errors.
// `st` (optional) streams one event per challenge for the browser demo; `seq` is
// the running challenge counter across warmup+run so map sampling is stable.
// `chained` selects the data-dependent index path (see common.hpp; default on).
// `nonce_mode` selects sequential or random/CSPRNG nonces (random is the default).
// `gap_us` sleeps that many microseconds after recording each challenge latency
// (simulates a periodic verifier; outside the timed window). With --gap-dist exp
// it is the mean of an exponential draw instead; see gap_sleep().
static void run_batch(int fd, const uint64_t* buf, uint64_t num_words,
                      uint32_t k, uint32_t lanes, uint32_t count,
                      uint64_t nonce_base, std::vector<double>* out_ns, uint64_t* errors,
                      Stream* st = nullptr, const char* phase = "r", uint64_t* seq = nullptr,
                      bool chained = false,
                      bool tree = false,
                      uint32_t verify_every = 1,
                      uint64_t* checked = nullptr,
                      NonceMode nonce_mode = NonceMode::SEQUENTIAL,
                      uint32_t gap_us = 0,
                      uint8_t* cc20_key = nullptr,   // 32-byte key for random nonces
                      uint32_t* cc20_ctr = nullptr)  // counter for random nonces
{
    for (uint32_t i = 0; i < count; ++i) {
        Challenge ch{}; ch.k = k; ch.lanes = lanes;
        if (chained) ch.flags |= kFlagChained;
        if (tree)    ch.flags |= kFlagTreeFold;

        // Nonce selection: sequential (original) or CSPRNG (unpredictable).
        if (nonce_mode == NonceMode::RANDOM && cc20_key && cc20_ctr) {
            // Draw 8 bytes of ChaCha20 keystream as the nonce.
            static const uint8_t zero_nonce12[12] = {};
            uint8_t block[64];
            // Each 64-byte block gives 8 nonces; advance counter every 8 challenges.
            uint32_t block_idx = *cc20_ctr / 8;
            uint32_t word_in_block = *cc20_ctr % 8;
            chacha20_block(cc20_key, zero_nonce12, block_idx, block);
            uint64_t nonce_val = 0;
            for (int b = 0; b < 8; ++b)
                nonce_val |= ((uint64_t)block[word_in_block * 8 + b]) << (b * 8);
            ch.nonce = nonce_val;
            (*cc20_ctr)++;
        } else {
            ch.nonce = nonce_base + i;
        }
        uint64_t idx = seq ? (*seq)++ : i;
        bool map = st && st->wants_map(idx);

        // Checking an answer means redoing every probe on the CPU, which at
        // large k costs hundreds of times what the prover spent producing it.
        // Sampling keeps the timing series dense while the check stays
        // affordable. It weakens the correctness detector in proportion: a
        // prover wrong on a fraction f of challenges is caught with probability
        // 1-(1-f)^(checked). Against `fake`, which is wrong on every challenge,
        // one check is enough. The real fix is opening a subset of lanes from
        // the Merkle root, which is still owed.
        bool do_check = (verify_every <= 1) || (i % verify_every == 0);
        if (map) do_check = true;   // a probe map is only meaningful if computed

        uint64_t expect = 0;
        if (!do_check) {
            // fall through: nothing computed, nothing compared
        } else if (map) {
            // Same loader, plus a note of which page each probe landed on. The
            // index sequence comes from answer_challenge_t itself, so the map is
            // exactly what the prover was asked for — not a reconstruction.
            std::fill(st->pages.begin(), st->pages.end(), (uint8_t)0);
            uint8_t* pg = st->pages.data();
            uint64_t pw = st->page_words;
            auto probe = [buf, pg, pw](uint64_t j) {
                pg[j / pw] = 1;
                return buf[j];
            };
            if (tree) {
                uint32_t nl = ch.lanes ? ch.lanes : 1u;
                std::vector<uint64_t> scratch(nl);
                answer_lanes_t(probe, num_words, ch.nonce, ch.k, nl, chained, scratch.data());
                expect = tree_fold(scratch.data(), nl, ch.nonce, ch.k);
            } else {
                expect = answer_challenge_t(probe, num_words, ch.nonce, ch.k, ch.lanes, chained);
            }
        } else if (tree) {
            uint32_t nl = ch.lanes ? ch.lanes : 1u;
            std::vector<uint64_t> scratch(nl);
            expect = answer_challenge_tree(buf, num_words, ch.nonce, ch.k, nl, chained,
                                           scratch.data());
        } else {
            expect = answer_challenge(buf, num_words, ch.nonce, ch.k, ch.lanes, chained);
        }

        auto t0 = Clock::now();
        if (!send_all(fd, &ch, sizeof(ch))) { fprintf(stderr, "send fail\n"); return; }
        Response r{};
        if (!recv_all(fd, &r, sizeof(r))) { fprintf(stderr, "recv fail\n"); return; }
        auto t1 = Clock::now();

        bool ok = do_check ? (r.hash == expect) : true;
        if (do_check) {
            if (checked) (*checked)++;
            if (!ok) (*errors)++;
        }
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        if (out_ns) out_ns->push_back(ns);

        if (st && st->f) {   // strictly after the timed window
            fprintf(st->f, "@{\"t\":\"c\",\"i\":%llu,\"ph\":\"%s\",\"us\":%.3f,"
                           "\"nonce\":\"0x%016llx\",\"hash\":\"0x%016llx\",\"exp\":\"0x%016llx\","
                           "\"ok\":%s,\"checked\":%s",
                    (unsigned long long)idx, phase, ns / 1000.0,
                    (unsigned long long)ch.nonce, (unsigned long long)r.hash,
                    (unsigned long long)expect, ok ? "true" : "false",
                    do_check ? "true" : "false");
            if (map) {
                fprintf(st->f, ",\"map\":\"");
                b64_bits(st->f, st->pages.data(), st->pages.size());
                fputc('"', st->f);
            }
            fputs("}\n", st->f);
        }

        // Gap: sleep AFTER recording latency, BEFORE the next challenge.
        // Simulates a periodic verifier that gives an attacker offline time.
        // This is strictly outside the timed window (measured above).
        gap_sleep(gap_us);
    }
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);

    size_t bytes = (size_t)llround(a.mb * 1048576.0);
    bytes = (bytes + 7) & ~size_t(7);
    uint64_t num_words = bytes / 8;

    const char* gen_name   = (a.gen == Gen::CHACHA20) ? "chacha20" : "splitmix64";
    const char* chain_name = a.chained ? "dependent" : "independent";
    const char* nonce_name = (a.nonce_mode == NonceMode::RANDOM) ? "random" : "sequential";

    // Chain-depth floor: when using the dependent (data-dependent) access path,
    // the effective latency-hardness equals the chain depth = k / lanes, because
    // each lane's first index is derived from a predictable seed and an attacker
    // can pipeline across lanes.  Enforce a minimum of kMinChainDepth to ensure
    // the dependent mode provides meaningful defense.
    uint32_t chain_depth = (a.lanes > 0) ? (a.k / a.lanes) : a.k;
    if (a.chained && chain_depth < kMinChainDepth) {
        fprintf(stderr,
                "[verifier] ERROR: chain depth k/lanes = %u < %u; "
                "dependent access gives ~1/%u latency-hardness — "
                "raise k, lower lanes, or (benchmark only) pass --chain independent\n",
                chain_depth, kMinChainDepth, chain_depth);
        return 1;
    }

    g_gap_random = a.gap_random;
    const char* gap_dist = a.gap_random ? "exp" : "fixed";
    printf("[verifier] buffer=%.1f MiB k=%u lanes=%u n=%u warmup=%u host=%s:%u gen=%s chain=%s fold=%s nonce=%s gap_us=%u gap_dist=%s verify_every=%u\n",
           bytes / 1048576.0, a.k, a.lanes, a.n, a.warmup, a.host, a.port, gen_name, chain_name, a.tree ? "tree" : "sequential",
           nonce_name, a.gap_us, gap_dist, a.verify_every);

    // For random nonce mode: derive a 32-byte CSPRNG key from std::random_device.
    // The key is seeded fresh per run so the prover cannot predict future nonces
    // by observing past ones.
    uint8_t cc20_key[32] = {};
    uint32_t cc20_ctr = 0;
    if (a.nonce_mode == NonceMode::RANDOM) {
        std::random_device rd;
        for (int b = 0; b < 8; ++b) {
            uint32_t w = rd();
            cc20_key[b * 4 + 0] = (uint8_t)(w);
            cc20_key[b * 4 + 1] = (uint8_t)(w >> 8);
            cc20_key[b * 4 + 2] = (uint8_t)(w >> 16);
            cc20_key[b * 4 + 3] = (uint8_t)(w >> 24);
        }
    }

    // For --gen chacha: generate a full random 256-bit key from std::random_device.
    // This makes the capacity claim unconditional (not stretched from a 64-bit seed).
    // The verifier holds its reference copy and re-fills from the same key when
    // checking answers; no reproducibility is needed (no seed-sharing with prover).
    uint8_t chacha_buf_key[32] = {};
    if (a.gen == Gen::CHACHA20) {
        std::random_device rd;
        for (int b = 0; b < 8; ++b) {
            uint32_t w = rd();
            chacha_buf_key[b * 4 + 0] = (uint8_t)(w);
            chacha_buf_key[b * 4 + 1] = (uint8_t)(w >> 8);
            chacha_buf_key[b * 4 + 2] = (uint8_t)(w >> 16);
            chacha_buf_key[b * 4 + 3] = (uint8_t)(w >> 24);
        }
    }

    // Generate our reference copy.
    uint64_t* buf = (uint64_t*)malloc(bytes);
    if (!buf) { fprintf(stderr, "malloc failed\n"); return 1; }
    if (a.gen == Gen::CHACHA20) {
        fill_buffer_chacha(buf, num_words, chacha_buf_key);
    } else {
        fill_buffer(buf, num_words, a.seed, a.gen);
    }

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

    // Optional live event stream for the browser demo.
    Stream stream;
    long page_bytes = sysconf(_SC_PAGESIZE); if (page_bytes <= 0) page_bytes = 4096;
    if (a.stream) {
        stream.f = (!strcmp(a.stream, "-")) ? stdout : fopen(a.stream, "w");
        if (!stream.f) {
            fprintf(stderr, "[verifier] WARNING: could not open stream %s\n", a.stream);
        } else {
            setvbuf(stream.f, nullptr, _IOLBF, 0);   // one write per event, no more
            stream.map_every = a.probe_map_every;
            stream.page_words = (uint64_t)page_bytes / 8;
            uint64_t num_pages = (num_words + stream.page_words - 1) / stream.page_words;
            if (stream.map_every) stream.pages.assign(num_pages, 0);
            fprintf(stream.f,
                    "@{\"t\":\"run\",\"label\":\"%s\",\"mb\":%.6g,\"k\":%u,\"lanes\":%u,\"n\":%u,"
                    "\"warmup\":%u,\"pages\":%llu,\"page_bytes\":%ld,\"locked\":%s,"
                    "\"upload_s\":%.4f,\"map_every\":%u}\n",
                    a.label, a.mb, a.k, a.lanes, a.n, a.warmup,
                    (unsigned long long)num_pages, page_bytes,
                    ready.locked ? "true" : "false", up_s, a.probe_map_every);
        }
    }
    uint64_t seq = 0;
    uint64_t checked = 0;

    // Warmup (fault in pages, warm caches/branch predictors) — not measured.
    uint64_t werr = 0;
    run_batch(fd, buf, num_words, a.k, a.lanes, a.warmup, base, nullptr, &werr,
              stream.f ? &stream : nullptr, "w", &seq, a.chained, a.tree, a.verify_every, &checked,
              a.nonce_mode, a.gap_us,
              (a.nonce_mode == NonceMode::RANDOM) ? cc20_key : nullptr, &cc20_ctr);
    base += a.warmup;

    // Measured run.
    std::vector<double> lat; lat.reserve(a.n);
    uint64_t errors = 0;
    run_batch(fd, buf, num_words, a.k, a.lanes, a.n, base, &lat, &errors,
              stream.f ? &stream : nullptr, "r", &seq, a.chained, a.tree, a.verify_every, &checked,
              a.nonce_mode, a.gap_us,
              (a.nonce_mode == NonceMode::RANDOM) ? cc20_key : nullptr, &cc20_ctr);

    // Stats.
    std::vector<double> s = lat; std::sort(s.begin(), s.end());
    double sum = 0, sq = 0; for (double x : lat) { sum += x; sq += x * x; }
    double mean = lat.empty() ? 0 : sum / lat.size();
    double var  = lat.empty() ? 0 : sq / lat.size() - mean * mean;
    double sd   = var > 0 ? std::sqrt(var) : 0;

    // Percentiles to report. Each carries the number of samples in its tail so
    // the reader can see how much evidence stands behind it.
    struct Pctl { const char* name; double p; };
    const Pctl pctls[] = { {"min",0}, {"p50",50}, {"p90",90}, {"p99",99},
                           {"p99.9",99.9}, {"max",100} };
    size_t n = lat.size();

    printf("\n=== latency profile (%zu samples, %llu errors in %llu checked) ===\n",
           n, (unsigned long long)errors, (unsigned long long)checked);
    for (const Pctl& q : pctls) {
        size_t tail = tail_count(q.p, n);
        // For min/max "tail" is degenerate; only flag the high inner percentiles.
        bool inner = (q.p > 0 && q.p < 100);
        bool weak  = inner && tail < kMinTail;
        printf("  %-6s %10.2f us   [tail n=%zu]%s\n", q.name, pctile(s, q.p) / 1000.0,
               tail, weak ? "  UNRELIABLE: n too small for this percentile" : "");
    }
    printf("  %-6s %10.2f us   stddev %8.2f us\n", "mean", mean / 1000.0, sd / 1000.0);
    if (mean > 0)
        printf("  throughput: %.2f probes/us (%u probes/challenge)\n", a.k / (mean / 1000.0), a.k);

    // Write per-challenge CSV for plotting.
    if (FILE* f = fopen(a.csv, "w")) {
        fprintf(f, "index,latency_us\n");
        for (size_t i = 0; i < lat.size(); ++i) fprintf(f, "%zu,%.4f\n", i, lat[i] / 1000.0);
        fclose(f);
        printf("[verifier] wrote %s\n", a.csv);
    }

    // Machine-readable summary. Scripts consume THIS, never the printf text, so a
    // formatting change can't silently turn a result into an empty string.
    if (a.json) {
        if (FILE* f = fopen(a.json, "w")) {
            long page = sysconf(_SC_PAGESIZE);
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            fprintf(f, "{\n");
            fprintf(f, "  \"schema\": \"memchal.verifier.v1\",\n");
            fprintf(f, "  \"label\": \"%s\",\n", a.label);
            fprintf(f, "  \"env\": {\"page_size\": %ld, \"online_cpus\": %ld, \"clock\": \"steady_clock\"},\n",
                    page, ncpu);
            fprintf(f, "  \"params\": {\"mb\": %.6g, \"k\": %u, \"lanes\": %u, \"n\": %u, "
                       "\"warmup\": %u, \"seed\": \"0x%llx\", \"host\": \"%s\", \"port\": %u, "
                       "\"gen\": \"%s\", \"chain\": \"%s\", \"nonce\": \"%s\", \"gap_us\": %u, "
                       "\"gap_dist\": \"%s\"",
                    a.mb, a.k, a.lanes, a.n, a.warmup, (unsigned long long)a.seed, a.host, a.port,
                    gen_name, chain_name, nonce_name, a.gap_us, gap_dist);
            if (a.chained) {
                // Stamp effective chain depth so readers know the latency-hardness factor.
                fprintf(f, ", \"chain_depth\": %u", chain_depth);
            }
            fprintf(f, "},\n");
            fprintf(f, "  \"prover\": {\"locked\": %s},\n", ready.locked ? "true" : "false");
            fprintf(f, "  \"upload\": {\"bytes\": %zu, \"seconds\": %.6f, \"gib_per_s\": %.4f},\n",
                    bytes, up_s, up_s > 0 ? bytes / 1073741824.0 / up_s : 0.0);
            fprintf(f, "  \"result\": {\n");
            fprintf(f, "    \"samples\": %zu,\n    \"errors\": %llu,\n",
                    n, (unsigned long long)errors);
            fprintf(f, "    \"mean_us\": %.4f,\n    \"stddev_us\": %.4f,\n",
                    mean / 1000.0, sd / 1000.0);
            fprintf(f, "    \"throughput_probes_per_us\": %.4f,\n",
                    mean > 0 ? a.k / (mean / 1000.0) : 0.0);
            fprintf(f, "    \"percentiles_us\": {");
            bool first = true;
            for (const Pctl& q : pctls) {
                size_t tail = tail_count(q.p, n);
                bool inner = (q.p > 0 && q.p < 100);
                bool reliable = !inner || tail >= kMinTail;
                fprintf(f, "%s\n      \"%s\": {\"value\": %.4f, \"tail_n\": %zu, \"reliable\": %s}",
                        first ? "" : ",", q.name, pctile(s, q.p) / 1000.0, tail,
                        reliable ? "true" : "false");
                first = false;
            }
            fprintf(f, "\n    }\n  }\n}\n");
            fclose(f);
            printf("[verifier] wrote %s\n", a.json);
        } else {
            fprintf(stderr, "[verifier] WARNING: could not write JSON to %s\n", a.json);
        }
    }

    if (stream.f) {
        fprintf(stream.f, "@{\"t\":\"end\",\"samples\":%zu,\"errors\":%llu,"
                          "\"checked\":%llu,\"p50\":%.3f,"
                          "\"p99\":%.3f,\"min\":%.3f,\"max\":%.3f,\"mean\":%.3f}\n",
                n, (unsigned long long)errors, (unsigned long long)checked,
                pctile(s, 50) / 1000.0, pctile(s, 99) / 1000.0,
                pctile(s, 0) / 1000.0, pctile(s, 100) / 1000.0, mean / 1000.0);
        if (stream.f != stdout) fclose(stream.f);
    }

    // Terminate the prover session cleanly.
    Challenge stop{}; stop.flags = kFlagTerminate;
    send_all(fd, &stop, sizeof(stop));
    close(fd);
    free(buf);
    return 0;
}
