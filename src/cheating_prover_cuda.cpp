// cheating_prover_cuda.cpp — the GPU adversaries.
//
// Mirrors cheating_prover.cpp's role on the CPU side: every attack is a --mode,
// so a defense is tested rather than asserted. Run the attack, watch what the
// detector does, and report it either way.
//
// Unlike the honest prover, this one ALWAYS claims locked=true. That is not an
// oversight, it is the threat model: nothing stops a prover saying it called
// cudaMalloc when it called cudaMallocManaged, and no verifier can check from
// outside. "Did you cudaMalloc?" is unanswerable remotely, which is precisely
// why the timing detector has to carry the weight.
//
//   honest    — cudaMalloc. The baseline the others are measured against, so
//               the same binary can produce it under the same conditions.
//   fake      — holds --frac of the buffer and invents the rest. Fast and
//               wrong: the correctness detector, not the timing one.
//   uvm       — cudaMallocManaged. Pages migrate host<->device on fault, so the
//               prover can "hold" more than the card has. The direct analog of
//               the CPU mmapdisk cheat, and #1 in the GPU adversary list.
//   zerocopy  — cudaHostAlloc(mapped). The buffer sits in pinned host DRAM and
//               every probe crosses PCIe.
//   partial   — --offload f keeps fraction f of the buffer in host memory and
//               the rest on the card. This is the one that sets the detector's
//               RESOLUTION. uvm and zerocopy ask "can you catch a prover that
//               holds none of it"; this asks "how little can a prover offload
//               before you stop seeing it", which is the number a decision rule
//               needs and the thing the first GPU session could not measure.
//
// A note on uvm, because it is easy to misread the result. With a buffer that
// fits in free HBM, UVM migrates everything to the card once and then answers
// at honest speed — it really is resident, so that is a true negative, not a
// failure of the detector. It is the same shape as the CPU finding that the
// mmap cheat looks honest while the page cache holds its file. To see it
// collapse you have to take the memory away: --hog <MiB> holds device memory so
// the managed buffer cannot fit, which is the GPU analog of the CPU demo's
// container RAM budget.
//
// --leave-free <MiB> is the same lever stated the way the experiment is: reserve
// whatever it takes to leave that much free. The prover measures free memory
// itself, at the moment it reserves, because nothing outside the process can
// know it — the CUDA context and allocator already hold a few hundred MiB that
// cudaMemGetInfo's total still counts. A caller that computes a reserve from the
// card's total asks for memory that does not exist and the reserve fails.
#include "common.hpp"
#include "gpu_answer.hpp"
#include "gpu_ingest.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mc;

enum Mode { HONEST, FAKE, UVM, ZEROCOPY, PARTIAL };
static const char* kModeNames[] = { "honest", "fake", "uvm", "zerocopy", "partial" };

static Mode     g_mode        = UVM;
static double   g_frac        = 0.5;    // fake: fraction actually held
static double   g_offload     = 0.1;    // partial: fraction kept off the card
static size_t   g_hog_mib     = 0;      // device memory to occupy
static long long g_leave_free_mib = -1;  // if >= 0, reserve all but this much
static int      g_device      = 0;
static uint32_t g_lane_cap    = 1u << 17;
static uint32_t g_trace_every = 0;

static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static int make_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 8) < 0) { perror("listen"); exit(1); }
    return fd;
}

static gpu::Alloc alloc_for(Mode m) {
    switch (m) {
        case UVM:      return gpu::Alloc::Managed;
        case ZEROCOPY: return gpu::Alloc::HostMapped;
        case PARTIAL:  return gpu::Alloc::Split;
        default:       return gpu::Alloc::Device;   // honest and fake
    }
}

static void serve(int cfd) {
    set_low_latency(cfd);

    Hello hello{};
    if (!recv_all(cfd, &hello, sizeof(hello))) { fprintf(stderr, "[cheat] no hello\n"); return; }
    if (hello.magic != kMagic) { fprintf(stderr, "[cheat] bad magic\n"); return; }
    if (hello.version > kWireVersion) {
        fprintf(stderr, "[cheat] unsupported wire version %u (max %u)\n",
                hello.version, kWireVersion);
        return;
    }
    const bool build = (hello.flags & kFlagBuild) != 0;

    size_t bytes = (size_t)hello.buffer_bytes;
    bytes = (bytes + 7) & ~size_t(7);
    uint64_t num_words = bytes / 8;

    char err[512] = {0};
    if (g_leave_free_mib >= 0) {
        size_t free_b = 0, total_b = 0;
        gpu::mem_info(&free_b, &total_b);
        size_t free_mib = free_b / 1048576ull;
        size_t leave = (size_t)g_leave_free_mib;
        g_hog_mib = free_mib > leave ? free_mib - leave : 0;
        printf("[cheat] %zu MiB free on the card; reserving %zu MiB to leave %zu MiB free\n",
               free_mib, g_hog_mib, free_mib - g_hog_mib);
    }
    if (g_hog_mib) {
        if (!gpu::hog(g_hog_mib * 1048576ull, err, sizeof err)) {
            fprintf(stderr, "[cheat] FATAL: could not reserve %zu MiB: %s\n", g_hog_mib, err);
            return;
        }
        printf("[cheat] reserving %zu MiB of device memory\n", g_hog_mib);
    }

    double held = (g_mode == FAKE) ? g_frac : 1.0;
    printf("[cheat] mode=%s %s %.2f MiB (%llu words), holding %.0f%%",
           kModeNames[g_mode], build ? "building" : "receiving", bytes / 1048576.0,
           (unsigned long long)num_words, held * 100.0);
    if (g_mode == PARTIAL) printf(", offloading %.1f%% off the card", g_offload * 100.0);
    printf("\n");

    gpu::Prover p;
    double offload = (g_mode == PARTIAL) ? g_offload : 0.0;
    if (!gpu::create(p, num_words, alloc_for(g_mode), g_lane_cap, held, offload,
                     err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        gpu::unhog();
        return;
    }

    // Build mode: build on the host, then copy to the card (see gpu_ingest.hpp).
    // Every mode then ingests through the same push() loop, so fake still
    // discards its tail and uvm still prefetches. uvm and zerocopy also hold
    // their buffer in host memory, so during the copy they need two full host
    // copies at once.
    Source src; src.fd = cfd;
    HostBuild hb;
    if (build) {
        if (!host_build_from_wire(cfd, hello, hb, err, sizeof err)) {
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            gpu::destroy(p); gpu::unhog(); return;
        }
        printf("[cheat] built on the host in %.3f s (m=%u KiB t=%u p=%u); copying to the card\n",
               hb.build_s, hb.recipe.m_kib, hb.recipe.t, hb.recipe.p);
        src.built = hb.buf;
    }
    bool ok = ingest(p, src, bytes, err, sizeof err);
    host_build_free(hb);
    if (!ok) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        gpu::destroy(p); gpu::unhog(); return;
    }

    const char* backing = "?";
    bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    size_t hf = 0, ht = 0;
    gpu::mem_info(&hf, &ht);
    printf("[cheat] backing = %s (claiming locked=true regardless)\n", backing);
    printf("[cheat] hbm used=%.2f GiB of %.2f GiB\n",
           (double)(ht - hf) / 1073741824.0, (double)ht / 1073741824.0);

    printf("@{\"t\":\"cheatinfo\",\"mode\":");
    json_str(stdout, kModeNames[g_mode]);
    printf(",\"backing\":");
    json_str(stdout, backing);
    printf(",\"device_backed\":%s,\"held_frac\":%.4f,\"hog_mib\":%llu,"
           "\"offload\":%.4f,\"hbm_used\":%llu,\"hbm_total\":%llu,"
           "\"build\":%s,\"build_s\":%.4f}\n",
           is_device ? "true" : "false", held, offload,
           (unsigned long long)g_hog_mib,
           (unsigned long long)(ht - hf), (unsigned long long)ht,
           build ? "true" : "false", hb.build_s);

    // The lie: claim a pinned baseline whatever the driver actually says.
    Ready ready{ kMagic, 1u, 0 };
    if (!send_all(cfd, &ready, sizeof(ready))) { gpu::destroy(p); gpu::unhog(); return; }

    uint64_t served = 0;
    for (;;) {
        Challenge ch{};
        if (!recv_all(cfd, &ch, sizeof(ch))) break;
        if (ch.flags & kFlagTerminate) break;
        bool chained = (ch.flags & kFlagChained) != 0;
        bool tree    = (ch.flags & kFlagTreeFold) != 0;

        uint64_t hash = 0;
        double kms = 0, cms = 0, fms = 0;
        if (!gpu::answer(p, ch.nonce, ch.k, ch.lanes, chained, tree,
                         &hash, &kms, &cms, &fms, err, sizeof err)) {
            fprintf(stderr, "[cheat] FATAL: %s\n", err);
            break;
        }
        Response resp{ hash };
        if (!send_all(cfd, &resp, sizeof(resp))) break;
        ++served;

        if (g_trace_every && (served % g_trace_every) == 0) {
            gpu::mem_info(&hf, &ht);
            printf("@{\"t\":\"g\",\"served\":%llu,\"k\":%u,\"lanes\":%u,"
                   "\"chained\":%s,\"tree\":%s,\"kernel_ms\":%.4f,\"copy_ms\":%.4f,"
                   "\"fold_ms\":%.4f,\"probe_gbps\":%.3f,\"hbm_used\":%llu,"
                   "\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s,\"mode\":",
                   (unsigned long long)served, ch.k, ch.lanes,
                   chained ? "true" : "false", tree ? "true" : "false", kms, cms, fms,
                   gpu::probe_gbps(ch.k, kms),
                   (unsigned long long)(ht - hf), (unsigned long long)ht, gpu::resident_fraction(p),
                   is_device ? "true" : "false");
            json_str(stdout, kModeNames[g_mode]);
            printf("}\n");
        }
    }
    printf("[cheat] served %llu challenges; releasing.\n", (unsigned long long)served);
    gpu::destroy(p);
    gpu::unhog();
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--mode") && i + 1 < argc) {
            const char* m = argv[++i];
            if      (!strcmp(m, "honest"))   g_mode = HONEST;
            else if (!strcmp(m, "fake"))     g_mode = FAKE;
            else if (!strcmp(m, "uvm"))      g_mode = UVM;
            else if (!strcmp(m, "zerocopy")) g_mode = ZEROCOPY;
            else if (!strcmp(m, "partial"))  g_mode = PARTIAL;
            else { fprintf(stderr, "unknown mode %s\n", m); return 1; }
        }
        else if (!strcmp(a, "--frac")  && i + 1 < argc) g_frac = atof(argv[++i]);
        else if (!strcmp(a, "--offload") && i + 1 < argc) g_offload = atof(argv[++i]);
        else if (!strcmp(a, "--hog")   && i + 1 < argc) g_hog_mib = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--leave-free") && i + 1 < argc) g_leave_free_mib = strtoll(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--device") && i + 1 < argc) g_device = atoi(argv[++i]);
        else if (!strcmp(a, "--lane-cap") && i + 1 < argc) g_lane_cap = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--trace") && i + 1 < argc) g_trace_every = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--port")  && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else { int v = atoi(a); if (v > 0) port = (uint16_t)v; }
    }
    if (g_frac < 0.0) g_frac = 0.0;
    if (g_frac > 1.0) g_frac = 1.0;
    if (g_offload < 0.0) g_offload = 0.0;
    if (g_offload > 1.0) g_offload = 1.0;

    char name[256] = {0}, err[512] = {0};
    if (!gpu::select_device(g_device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "[cheat] FATAL: %s\n", err);
        return 1;
    }
    printf("[cheat] device %d: %s\n", g_device, name);
    printf("@{\"t\":\"gpuinfo\",\"device\":%d,\"name\":", g_device);
    json_str(stdout, name);
    printf("}\n");

    int lfd = make_listener(port);
    printf("[cheat] listening on 0.0.0.0:%u mode=%s\n", port, kModeNames[g_mode]);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        printf("[cheat] client %s connected\n", inet_ntoa(ca.sin_addr));
        serve(cfd);
        close(cfd);
    }
    close(lfd);
    return 0;
}
