#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "shared/cli.hpp"
#include "shared/protocol.hpp"

namespace mc {

enum class Generator { ChaCha, Splitmix64, Argon2d };

struct Args {
    const char* host              = "127.0.0.1";
    uint16_t    port              = kDefaultPort;
    double      mib               = 256.0;
    uint32_t    samples           = 65536;
    uint32_t    lanes             = 64;
    uint32_t    challenges        = 2000;
    uint32_t    warmup            = 200;
    Generator   gen               = Generator::ChaCha;
    uint32_t    build_passes      = 2;
    uint32_t    build_parallelism = 1;
    bool        chained           = true;
    bool        tree              = false;
    bool        random_nonces     = true;
    uint32_t    verify_every      = 1;
    uint32_t    gap_us            = 0;
    bool        gap_exponential   = false;
    uint64_t    seed              = 0xABCDEF0123456789ULL;
    const char* csv               = "latency.csv";
    const char* json              = nullptr;
    const char* label             = "";
    const char* stream            = nullptr;
    uint32_t    probe_map_every   = 0;

    bool build() const { return gen == Generator::Argon2d; }
};

static const char kUsage[] =
    "usage: verifier [--flag value]...\n"
    "  --host ADDR --port N          prover address (default 127.0.0.1:%u)\n"
    "  --mb MIB                      buffer size\n"
    "  --samples N --lanes N         probes per challenge, and parallel lanes\n"
    "  --challenges N --warmup N     measured and unmeasured challenges\n"
    "  --gen chacha|splitmix64|argon2d   buffer generator (default chacha)\n"
    "  --build-passes N --build-parallelism N   Argon2d parameters (with --gen argon2d)\n"
    "  --chain dependent|independent     offset chaining (default dependent)\n"
    "  --fold sequential|tree        cross-lane fold (default sequential)\n"
    "  --nonce random|sequential     nonce source (default random)\n"
    "  --verify-every N              check a random 1 in N answers (default 1, all)\n"
    "  --gap-us N --gap-dist fixed|exp   pause between challenges\n"
    "  --csv PATH --json PATH --label TEXT --seed N\n"
    "  --stream PATH|- --probe-map N live events for the demo\n";

static Args parse_args(int argc, char** argv) {
    static const Cli cli{"verifier", kUsage, kDefaultPort};
    Args a;
    for (int i = 1; i < argc; ++i) {
        const char* flag = argv[i];
        if (!strcmp(flag, "--help") || !strcmp(flag, "-h")) cli.help();
        const char* v = cli.value(argc, argv, &i);
        if      (!strcmp(flag, "--host"))       a.host = v;
        else if (!strcmp(flag, "--port"))       a.port = cli.port(flag, v);
        else if (!strcmp(flag, "--mb"))         a.mib = cli.number(flag, v, 1e-9, 1 << 20);
        else if (!strcmp(flag, "--samples"))    a.samples = cli.u32(flag, v);
        else if (!strcmp(flag, "--lanes"))      a.lanes = cli.u32(flag, v);
        else if (!strcmp(flag, "--challenges")) a.challenges = cli.u32(flag, v);
        else if (!strcmp(flag, "--warmup"))     a.warmup = cli.u32(flag, v);
        else if (!strcmp(flag, "--csv"))        a.csv = v;
        else if (!strcmp(flag, "--json"))       a.json = v;
        else if (!strcmp(flag, "--label"))      a.label = v;
        else if (!strcmp(flag, "--seed"))       a.seed = cli.uint(flag, v);
        else if (!strcmp(flag, "--gen")) {
            static const char* const names[] = {"chacha", "splitmix64", "argon2d", nullptr};
            a.gen = (Generator)cli.choice(flag, v, names);
        }
        else if (!strcmp(flag, "--build-passes"))      a.build_passes = cli.u32(flag, v);
        else if (!strcmp(flag, "--build-parallelism")) a.build_parallelism = cli.u32(flag, v);
        else if (!strcmp(flag, "--stream"))            a.stream = v;
        else if (!strcmp(flag, "--probe-map"))         a.probe_map_every = cli.u32(flag, v);
        else if (!strcmp(flag, "--chain")) {
            static const char* const names[] = {"dependent", "independent", nullptr};
            a.chained = cli.choice(flag, v, names) == 0;
        }
        else if (!strcmp(flag, "--fold")) {
            static const char* const names[] = {"sequential", "tree", nullptr};
            a.tree = cli.choice(flag, v, names) == 1;
        }
        else if (!strcmp(flag, "--nonce")) {
            static const char* const names[] = {"random", "sequential", nullptr};
            a.random_nonces = cli.choice(flag, v, names) == 0;
        }
        else if (!strcmp(flag, "--verify-every")) {
            a.verify_every = cli.u32(flag, v);
            if (a.verify_every == 0) a.verify_every = 1;
        }
        else if (!strcmp(flag, "--gap-us")) a.gap_us = cli.u32(flag, v);
        else if (!strcmp(flag, "--gap-dist")) {
            static const char* const names[] = {"fixed", "exp", nullptr};
            a.gap_exponential = cli.choice(flag, v, names) == 1;
        }
        else cli.error("unknown flag: %s", flag);
    }
    return a;
}

static inline const char* generator_name(Generator g) {
    switch (g) {
        case Generator::ChaCha:     return "chacha20";
        case Generator::Splitmix64: return "splitmix64";
        case Generator::Argon2d:    return "argon2d";
    }
    return "";
}

} // namespace mc
