// prover_cuda.cpp — the honest GPU prover.
//
// The CPU counterpart (prover.cpp) mmaps a region, prefaults it, mlocks it, and
// then refuses to run unless /proc says the lock took (ADR 0002). This is the
// same shape with the GPU's equivalents:
//
//   mmap + mlock          ->  cudaMalloc. Device memory is not pageable.
//   VmLck in /proc        ->  cudaPointerGetAttributes reporting the pointer is
//                             cudaMemoryTypeDevice rather than Managed or Host.
//   refuse if not pinned  ->  refuse if the driver does not call it device
//                             memory; --allow-unlocked overrides and stamps
//                             locked=false into every response.
//
// Both checks are self-reported — VmLck always was. What they buy is that the
// prover cannot fool itself into publishing an unpinned baseline. Proving
// residency to someone else is the timing detector's job, not this check's.
//
// Sockets live here; the kernel lives in gpu_answer.cu. Nothing in this file
// includes CUDA headers, and nothing in that file includes POSIX ones.
#include "common.hpp"
#include "gpu_answer.hpp"
#include "gpu_ingest.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mc;

static bool     g_allow_unlocked = false;
static int      g_device         = 0;
static uint32_t g_lane_cap       = 1u << 17;   // 131072: the GPU design point
static uint32_t g_trace_every    = 0;      // emit an event every N challenges (0 = off)

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

// Minimal JSON string escaping for the one field that comes from the driver.
static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20)  fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void report_hbm(const char* when) {
    size_t f = 0, t = 0;
    gpu::mem_info(&f, &t);
    if (!t) return;
    printf("[gpu] hbm %s: used=%.2f GiB free=%.2f GiB total=%.2f GiB\n",
           when, (double)(t - f) / 1073741824.0, (double)f / 1073741824.0,
           (double)t / 1073741824.0);
}

static void serve(int cfd) {
    set_low_latency(cfd);

    Hello hello{};
    if (!recv_all(cfd, &hello, sizeof(hello))) { fprintf(stderr, "[gpu] no hello\n"); return; }
    if (hello.magic != kMagic) { fprintf(stderr, "[gpu] bad magic\n"); return; }
    if (hello.version > kWireVersion) {
        fprintf(stderr, "[gpu] unsupported wire version %u (max %u)\n",
                hello.version, kWireVersion);
        return;
    }
    const bool build = (hello.flags & kFlagBuild) != 0;

    size_t bytes = (size_t)hello.buffer_bytes;
    bytes = (bytes + 7) & ~size_t(7);            // round to whole 64-bit words
    uint64_t num_words = bytes / 8;
    printf("[gpu] %s %.2f MiB (%llu words)...\n",
           build ? "building" : "receiving", bytes / 1048576.0, (unsigned long long)num_words);

    char err[512] = {0};
    gpu::Prover p;
    if (!gpu::create(p, num_words, gpu::Alloc::Device, g_lane_cap, 1.0, 0.0, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return;
    }

    // Build mode: build on the host, then copy to the card (see gpu_ingest.hpp).
    // The device buffer is allocated first so a card that cannot hold it fails
    // before a long build rather than after.
    Source src; src.fd = cfd;
    HostBuild hb;
    if (build) {
        if (!host_build_from_wire(cfd, hello, hb, err, sizeof err)) {
            fprintf(stderr, "[gpu] FATAL: %s\n", err);
            gpu::destroy(p);
            return;
        }
        printf("[gpu] built on the host in %.3f s (m=%u KiB t=%u p=%u); copying to the card\n",
               hb.build_s, hb.recipe.m_kib, hb.recipe.t, hb.recipe.p);
        src.built = hb.buf;
    }

    // Stream the bytes through pinned staging, so an upload never needs a full
    // host copy (a build needs exactly one, freed as soon as it is on the card).
    bool ok = ingest(p, src, bytes, err, sizeof err);
    host_build_free(hb);
    if (!ok) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        gpu::destroy(p);
        return;
    }

    const char* backing = "unknown";
    bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    printf("[gpu] buffer %s; backing = %s\n", build ? "built and copied" : "uploaded",
           backing);
    report_hbm("after upload");
    {
        size_t hf = 0, ht = 0;
        gpu::mem_info(&hf, &ht);
        printf("@{\"t\":\"proverinfo\",\"backing\":");
        json_str(stdout, backing);
        printf(",\"device_backed\":%s,\"build\":%s,\"build_s\":%.4f,"
               "\"hbm_used\":%llu,\"hbm_total\":%llu}\n",
               is_device ? "true" : "false", build ? "true" : "false", hb.build_s,
               (unsigned long long)(ht - hf), (unsigned long long)ht);
    }

    // The ADR 0002 analog: an honest baseline on migratable or host-mapped
    // memory is meaningless, so refuse by default.
    if (!is_device) {
        if (!g_allow_unlocked) {
            fprintf(stderr, "[gpu] FATAL: buffer is not device-resident (%s); "
                            "pass --allow-unlocked to run anyway\n", backing);
            gpu::destroy(p);
            exit(1);
        }
        fprintf(stderr, "[gpu] WARNING: running on non-device memory — baseline is "
                        "not pinned; reporting locked=false\n");
    }

    Ready ready{ kMagic, (uint32_t)(is_device ? 1 : 0), 0 };
    if (!send_all(cfd, &ready, sizeof(ready))) { gpu::destroy(p); return; }

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
            fprintf(stderr, "[gpu] FATAL: %s\n", err);
            break;
        }

        Response resp{ hash };
        if (!send_all(cfd, &resp, sizeof(resp))) break;
        ++served;

        // One parseable line per challenge. The three costs are kept apart on
        // purpose: kernel is the memory-bound work, copy is PCIe, fold is host
        // ALU. Summing them would be the easiest way to misreport bandwidth.
        if (g_trace_every && (served % g_trace_every) == 0) {
            size_t hf = 0, ht = 0;
            gpu::mem_info(&hf, &ht);
            printf("@{\"t\":\"g\",\"served\":%llu,\"k\":%u,\"lanes\":%u,"
                   "\"chained\":%s,\"tree\":%s,\"kernel_ms\":%.4f,\"copy_ms\":%.4f,"
                   "\"fold_ms\":%.4f,\"probe_gbps\":%.3f,\"hbm_used\":%llu,"
                   "\"hbm_total\":%llu,\"resident_frac\":%.4f,\"device\":%s}\n",
                   (unsigned long long)served, ch.k, ch.lanes,
                   chained ? "true" : "false", tree ? "true" : "false", kms, cms, fms,
                   gpu::probe_gbps(ch.k, kms),
                   (unsigned long long)(ht - hf), (unsigned long long)ht, gpu::resident_fraction(p),
                   is_device ? "true" : "false");
        }
    }
    printf("[gpu] served %llu challenges; releasing buffer.\n", (unsigned long long)served);
    gpu::destroy(p);
    report_hbm("after release");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffered logs
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--allow-unlocked")) {
            g_allow_unlocked = true;
        } else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            g_trace_every = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            g_device = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--lane-cap") && i + 1 < argc) {
            g_lane_cap = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else {
            int v = atoi(argv[i]);
            if (v > 0) port = (uint16_t)v;
        }
    }

    char name[256] = {0}, err[512] = {0};
    if (!gpu::select_device(g_device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return 1;
    }
    printf("[gpu] device %d: %s\n", g_device, name);
    report_hbm("at start");
    {
        size_t hf = 0, ht = 0;
        gpu::mem_info(&hf, &ht);
        printf("@{\"t\":\"gpuinfo\",\"device\":%d,\"name\":", g_device);
        json_str(stdout, name);
        printf(",\"hbm_free\":%llu,\"hbm_total\":%llu}\n",
               (unsigned long long)hf, (unsigned long long)ht);
    }

    int lfd = make_listener(port);
    printf("[gpu] listening on 0.0.0.0:%u (lane cap %u)\n", port, g_lane_cap);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        printf("[gpu] client %s connected\n", inet_ntoa(ca.sin_addr));
        serve(cfd);
        close(cfd);
    }
    close(lfd);
    return 0;
}
