// prover.cpp — the honest CPU prover. It holds the buffer in locked RAM and
// refuses to run if the lock did not take (ADR 0002).
#include "challenge.hpp"
#include "cli.hpp"
#include "pinned.hpp"
#include "server.hpp"
#include <chrono>
#include <vector>

using namespace mc;

static const char kUsage[] =
    "usage: prover [--port N] [--allow-unlocked]\n"
    "  --port N           listen port (default %u)\n"
    "  --allow-unlocked   run even if the buffer cannot be locked in RAM\n";

static bool g_allow_unlocked = false;

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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
        const double t0 = now_s();
        if (!build_buffer(buf.words, s.num_words, s.recipe)) {
            fprintf(stderr, "[prover] build failed (m=%u t=%u p=%u)\n", s.recipe.m_kib, s.recipe.t, s.recipe.p);
            unpin(buf); return;
        }
        printf("[prover] built in %.3f s (m=%u KiB t=%u p=%u)\n",
               now_s() - t0, s.recipe.m_kib, s.recipe.t, s.recipe.p);
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

    std::vector<uint64_t> scratch;
    const uint64_t served = answer_challenges(cfd, "prover",
        [&](const Challenge& ch, uint64_t* hash) {
            *hash = answer_on_host(BufLoad{buf.words}, s.num_words, ch, scratch);
            return true;
        },
        [](const Challenge&, uint64_t) { return true; });
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
        else if (!strcmp(a, "--port"))           port = cli.port(a, cli.value(argc, argv, &i));
        else if (!strcmp(a, "--allow-unlocked")) g_allow_unlocked = true;
        else cli.error("unknown flag: %s", a);
    }
    return serve_forever("prover", port, serve);
}
