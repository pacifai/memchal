#include <chrono>
#include <cstring>
#include <vector>
#include "cpu/pinned.hpp"
#include "demo/cpu_trace.hpp"
#include "shared/cli.hpp"
#include "shared/server.hpp"

using namespace mc;

static const char kUsage[] =
    "usage: prover [--flag value]...\n"
    "  --port N           listen port (default %u)\n"
    "  --trace N          print a residency event every N challenges\n"
    "  --allow-unlocked   run even if the buffer cannot be locked in RAM\n";

static bool     g_allow_unlocked = false;
static uint32_t g_trace_every    = 0;

static void serve(int cfd) {
    Setup s;
    char err[256];
    if (!read_setup(cfd, s, err, sizeof err)) { fprintf(stderr, "[prover] refusing: %s\n", err); return; }
    printf("[prover] %s %.2f MiB (%llu words)\n", s.build ? "building" : "receiving",
           s.bytes / 1048576.0, (unsigned long long)s.num_words);

    Pinned buf;
    if (!pin(buf, s.bytes)) return;
    mlockall(MCL_CURRENT | MCL_FUTURE);

    if (s.build) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!build_buffer(buf.words, s.num_words, s.recipe)) {
            fprintf(stderr, "[prover] build failed\n");
            unpin(buf); return;
        }
        printf("[prover] built in %.3f s\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    } else if (!recv_all(cfd, buf.words, s.bytes)) {
        fprintf(stderr, "[prover] short data\n"); unpin(buf); return;
    }
    printf("[prover] buffer resident; memory %s\n", buf.locked ? "locked" : "NOT locked");

    if (!buf.locked) {
        if (!g_allow_unlocked) {
            fprintf(stderr, "[prover] FATAL: memory not locked (need CAP_IPC_LOCK / `ulimit -l`, "
                            "or pass --allow-unlocked)\n");
            exit(1);
        }
        fprintf(stderr, "[prover] WARNING: running unlocked; reporting locked=false\n");
    }
    if (!send_ready(cfd, buf.locked)) { unpin(buf); return; }

    demo::ResidencyTrace trace(g_trace_every, "honest", "ram", buf.words, s.bytes, s.bytes);
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
        else if (!strcmp(a, "--port"))  port = cli.port(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--trace")) g_trace_every = cli.u32(a, cli.value(argc, argv, &i));
        else cli.error("unknown flag: %s", a);
    }
    return serve_forever("prover", port, serve);
}
