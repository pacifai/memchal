#include <cstdio>
#include <cstdlib>
#include <vector>
#include "record/verifier_stream.hpp"
#include "shared/socket.hpp"
#include "verifier/args.hpp"
#include "verifier/reference.hpp"
#include "verifier/report.hpp"
#include "verifier/session.hpp"

using namespace mc;

// The smallest samples-per-lane the verifier accepts with chained probes.
static constexpr uint32_t kMinChainDepth = 16;

int main(int argc, char** argv) {
    const Args a = parse_args(argc, argv);
    if (a.lanes < 1 || a.lanes > kMaxLanes) {
        fprintf(stderr, "[verifier] ERROR: --lanes %u is outside 1 to %u\n", a.lanes, kMaxLanes);
        return 2;
    }
    if (a.samples > kMaxSamples) {
        fprintf(stderr, "[verifier] ERROR: --samples %u is above %u\n", a.samples, kMaxSamples);
        return 2;
    }
    // The longest pause is kMaxGapFactor times --gap-us. Keeping it under the prover's
    // idle timeout stops a pause from ending the session.
    const uint32_t max_gap_us = (uint32_t)(kIdleTimeoutS * 1e6 / kMaxGapFactor);
    if (a.gap_us > max_gap_us) {
        fprintf(stderr, "[verifier] ERROR: --gap-us %u is above %u\n", a.gap_us, max_gap_us);
        return 2;
    }
    const uint32_t chain_depth = a.samples / a.lanes;
    if (a.chained && chain_depth < kMinChainDepth) {
        fprintf(stderr, "[verifier] ERROR: samples per lane is %u, below the minimum of %u for "
                        "--chain dependent; raise --samples, lower --lanes, or pass "
                        "--chain independent\n", chain_depth, kMinChainDepth);
        return 1;
    }

    char err[256];
    Reference ref;
    if (!make_reference(a, ref, err, sizeof err)) {
        fprintf(stderr, "[verifier] ERROR: %s\n", err);
        return 1;
    }
    printf("[verifier] buffer=%.1f MiB samples=%u lanes=%u challenges=%u warmup=%u host=%s:%u "
           "gen=%s chain=%s fold=%s nonce=%s gap_us=%u verify_every=%u\n",
           ref.bytes / 1048576.0, a.samples, a.lanes, a.challenges, a.warmup, a.host, a.port,
           generator_name(a.gen), a.chained ? "dependent" : "independent",
           a.tree ? "tree" : "sequential", a.random_nonces ? "random" : "sequential", a.gap_us,
           a.verify_every);
    printf("[verifier] generated in %.3f s (%.2f GiB/s)\n", ref.gen_s,
           ref.bytes / 1073741824.0 / ref.gen_s);

    const int fd = connect_to(a.host, a.port);
    Ready ready{};
    double upload_s = 0;
    if (!hand_over(fd, ref, a.build(), &ready, &upload_s, err, sizeof err)) {
        fprintf(stderr, "[verifier] ERROR: %s\n", err);
        return 1;
    }
    if (a.build())
        printf("[verifier] sent a %zu-byte recipe in place of %.1f MiB\n",
               sizeof(RecipeMsg), ref.bytes / 1048576.0);
    else
        printf("[verifier] uploaded in %.3f s (%.2f GiB/s)\n", upload_s,
               ref.bytes / 1073741824.0 / upload_s);
    printf("[verifier] prover memory %s\n", ready.locked ? "LOCKED" : "not locked");

    record::VerifierStream stream;
    if (a.stream) {
        if (stream.open(a.stream, a.probe_map_every, ref.num_words))
            stream.run(a.label, a.mib, a.samples, a.lanes, a.challenges, a.warmup, ready.locked,
                       generator_name(a.gen), ref.gen_s, upload_s);
        else
            fprintf(stderr, "[verifier] WARNING: could not open stream %s\n", a.stream);
    }

    Session session(fd, a, ref, stream.active() ? &stream : nullptr);
    std::vector<double> latencies;
    latencies.reserve(a.challenges);
    Tally warmup, measured;
    if (!session.run(a.warmup, "w", nullptr, warmup) ||
        !session.run(a.challenges, "r", &latencies, measured)) {
        fprintf(stderr, "[verifier] ERROR: connection lost after %zu of %u measured challenges; "
                        "no results written\n", latencies.size(), a.challenges);
        return 1;
    }

    session.terminate();
    close(fd);
    free(ref.words);

    const Profile profile(latencies, measured.errors, measured.checked, warmup.errors);
    print_profile(profile, a.samples);
    int rc = 0;
    if (write_csv(a.csv, latencies)) {
        printf("[verifier] wrote %s\n", a.csv);
    } else {
        fprintf(stderr, "[verifier] ERROR: could not write CSV to %s\n", a.csv);
        rc = 1;
    }
    if (a.json) {
        if (write_json(a.json, a, profile, ready.locked, ref.bytes, upload_s)) {
            printf("[verifier] wrote %s\n", a.json);
        } else {
            fprintf(stderr, "[verifier] ERROR: could not write JSON to %s\n", a.json);
            rc = 1;
        }
    }
    if (stream.active()) {
        stream.end(profile.count(), profile.errors, profile.checked, profile.at(50), profile.at(99),
                   profile.at(0), profile.at(100), profile.mean_us);
        stream.close();
    }
    return rc;
}
