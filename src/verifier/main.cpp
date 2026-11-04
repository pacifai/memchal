#include <cstdio>
#include <cstdlib>
#include <vector>
#include "demo/verifier_stream.hpp"
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
    if (a.lanes > kMaxLanes) {
        fprintf(stderr, "[verifier] ERROR: --lanes %u is above the provers' limit of %u\n",
                a.lanes, kMaxLanes);
        return 2;
    }
    const uint32_t chain_depth = a.lanes ? a.samples / a.lanes : a.samples;
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
           "gen=%s chain=%s fold=%s nonce=%s gap_us=%u gap_dist=%s verify_every=%u\n",
           ref.bytes / 1048576.0, a.samples, a.lanes, a.challenges, a.warmup, a.host, a.port,
           generator_name(a.gen), a.chained ? "dependent" : "independent",
           a.tree ? "tree" : "sequential", a.random_nonces ? "random" : "sequential", a.gap_us,
           a.gap_exponential ? "exp" : "fixed", a.verify_every);
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

    demo::VerifierStream stream;
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
    uint64_t warmup_errors = 0, errors = 0;
    if (!session.run(a.warmup, "w", nullptr, &warmup_errors) ||
        !session.run(a.challenges, "r", &latencies, &errors)) {
        fprintf(stderr, "[verifier] ERROR: connection lost after %zu of %u measured challenges; "
                        "no results written\n", latencies.size(), a.challenges);
        return 1;
    }

    const Profile profile(latencies, errors, session.checked());
    print_profile(profile, a.samples);
    if (write_csv(a.csv, latencies)) printf("[verifier] wrote %s\n", a.csv);
    if (a.json) {
        if (write_json(a.json, a, profile, ready.locked, ref.bytes, upload_s))
            printf("[verifier] wrote %s\n", a.json);
        else
            fprintf(stderr, "[verifier] WARNING: could not write JSON to %s\n", a.json);
    }
    if (stream.active()) {
        stream.end(profile.count(), errors, session.checked(), profile.at(50), profile.at(99),
                   profile.at(0), profile.at(100), profile.mean_us);
        stream.close();
    }

    session.terminate();
    close(fd);
    free(ref.words);
    return 0;
}
