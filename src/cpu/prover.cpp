#include <chrono>
#include <cstring>
#include <vector>
#include "cpu/pinned.hpp"
#include "record/cpu_trace.hpp"
#include "shared/cli.hpp"
#include "shared/server.hpp"

using namespace mc;

static const char kUsage[] =
    "usage: prover [--flag value]...\n"
    "  --port N           listen port (default %u)\n"
    "  --trace N          print a residency event every N challenges\n"
    "  --weights PATH     hold the model weights this manifest lists\n"
    "  --allow-unlocked   run even if the buffer cannot be locked in RAM\n";

static bool            g_allow_unlocked = false;
static uint32_t        g_trace_every    = 0;
static const Manifest* g_weights        = nullptr;

// Reads the weights into the start of the buffer, then builds or receives the
// filler after them.
static bool fill(int cfd, const Setup& s, uint64_t* words) {
    char err[512] = "short read";
    WeightsReader weights;
    if (s.weights && (!weights.open(*s.weights, err, sizeof err) || !weights.read(words, s.weight_bytes))) {
        fprintf(stderr, "[prover] cannot read the weights: %s\n", err);
        return false;
    }
    uint64_t* filler = words + s.weight_bytes / 8;
    if (!s.build) {
        if (recv_all(cfd, filler, s.filler_bytes())) return true;
        fprintf(stderr, "[prover] short data\n");
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!build_buffer(filler, s.filler_words(), s.recipe)) {
        fprintf(stderr, "[prover] build failed\n");
        return false;
    }
    printf("[prover] built in %.3f s\n",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

static void serve(int cfd) {
    Setup s;
    char err[512];
    if (!read_setup(cfd, g_weights, s, err, sizeof err)) {
        fprintf(stderr, "[prover] refusing: %s\n", err);
        return;
    }
    printf("[prover] %.2f MiB (%llu words): %.2f MiB of weights, then %s the filler\n",
           s.bytes / 1048576.0, (unsigned long long)s.num_words, s.weight_bytes / 1048576.0,
           s.build ? "building" : "receiving");

    Pinned buf;
    if (!pin(buf, s.bytes)) return;
    mlockall(MCL_CURRENT);
    if (!buf.locked) {
        if (!g_allow_unlocked) {
            fprintf(stderr, "[prover] refusing: memory not locked (need CAP_IPC_LOCK / `ulimit -l`, "
                            "or pass --allow-unlocked)\n");
            unpin(buf);
            return;
        }
        fprintf(stderr, "[prover] WARNING: running unlocked; reporting locked=false\n");
    }
    if (!fill(cfd, s, buf.words)) { unpin(buf); return; }
    printf("[prover] buffer resident; memory %s\n", buf.locked ? "locked" : "NOT locked");
    if (!send_ready(cfd, buf.locked)) { unpin(buf); return; }

    record::ResidencyTrace trace(g_trace_every, "honest", "ram", buf.words, s.bytes, s.bytes);
    std::vector<uint64_t> scratch;
    const uint64_t served = answer_challenges(cfd, "prover",
        [&](const Challenge& ch, uint64_t* hash) {
            *hash = answer(BufferLoad{buf.words}, s.num_words, ch, scratch);
            return true;
        },
        [&](const Challenge& ch, uint64_t n) { trace.after(ch, n); return true; });
    trace.finish(served);
    printf("[prover] served %llu challenges; releasing buffer\n", (unsigned long long)served);
    unpin(buf);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const Cli cli{"prover", kUsage, kDefaultPort};
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (!strcmp(a, "--help") || !strcmp(a, "-h")) cli.help();
        else if (!strcmp(a, "--allow-unlocked")) g_allow_unlocked = true;
        else if (!strcmp(a, "--port"))    port = cli.port(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--trace"))   g_trace_every = cli.u32(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--weights")) g_weights = held_weights(cli.value(argc, argv, &i), "prover");
        else cli.error("unknown flag: %s", a);
    }
    return serve_forever("prover", port, serve);
}
