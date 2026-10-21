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
// --weights MANIFEST holds a model's weights instead: every segment the
// manifest lists in its own cudaMalloc, the way an inference engine holds each
// tensor, attached through a Segments table (gpu::attach) and served with no
// upload (gpu_resident.hpp). It is the baseline for the engine's own prover,
// and the attached path with no engine in the way.
//
// Sockets live here; the kernel lives in gpu_answer.cu. Nothing in this file
// includes CUDA headers, and nothing in that file includes POSIX ones.
#include "common.hpp"
#include "gpu_answer.hpp"
#include "gpu_resident.hpp"
#include "weights.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace mc;

static bool     g_allow_unlocked = false;
static int      g_device         = 0;
static uint32_t g_lane_cap       = 1u << 17;   // 131072: the GPU design point
static uint32_t g_trace_every    = 0;      // emit an event every N challenges (0 = off)

// Read every segment into its own device allocation, then attach to them.
static bool load_weights(const char* manifest_path, gpu::Prover& p, std::vector<void*>* allocs) {
    char err[512] = {0};
    Manifest m;
    if (!read_manifest(manifest_path, &m, err, sizeof err)) { fprintf(stderr, "[gpu] %s\n", err); return false; }
    std::vector<int> fds;
    for (const std::string& f : m.files) {
        int fd = open(f.c_str(), O_RDONLY);
        if (fd < 0) { fprintf(stderr, "[gpu] cannot open %s: %s\n", f.c_str(), strerror(errno)); return false; }
        fds.push_back(fd);
    }
    std::vector<const uint64_t*> bases;
    std::vector<uint64_t> words;
    std::vector<unsigned char> host;
    bool ok = true;
    for (const SegmentRun& r : segment_runs(m)) {
        void* d = nullptr;
        if (!gpu::device_alloc(&d, r.bytes, err, sizeof err)) { fprintf(stderr, "[gpu] %s\n", err); ok = false; break; }
        allocs->push_back(d);
        host.resize(r.bytes);
        size_t at = 0;
        for (size_t i = r.first; ok && i < r.end; ++i) {
            const Piece& pc = m.pieces[i];
            if (pread(fds[pc.file], host.data() + at, pc.bytes, (off_t)pc.offset) != (ssize_t)pc.bytes) {
                fprintf(stderr, "[gpu] short read of %s\n", pc.segment.c_str());
                ok = false;
            }
            at += pc.bytes;
        }
        if (ok && !gpu::device_copy_in(d, host.data(), r.bytes, err, sizeof err)) {
            fprintf(stderr, "[gpu] %s\n", err); ok = false;
        }
        if (!ok) break;
        bases.push_back((const uint64_t*)d);
        words.push_back(r.bytes / 8);
    }
    for (int fd : fds) close(fd);
    if (!ok) return false;
    if (!gpu::attach(p, bases.data(), words.data(), (uint32_t)bases.size(), g_lane_cap, false, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return false;
    }
    const char* backing = "?"; bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    printf("[gpu] holding %zu segments, %.2f GiB, from %s; backing = %s\n", bases.size(),
           (double)m.total_bytes / 1073741824.0, manifest_path, backing);
    return true;
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

    size_t bytes = (size_t)hello.buffer_bytes;
    bytes = (bytes + 7) & ~size_t(7);            // round to whole 64-bit words
    uint64_t num_words = bytes / 8;
    printf("[gpu] receiving %.2f MiB (%llu words)...\n",
           bytes / 1048576.0, (unsigned long long)num_words);

    char err[512] = {0};
    gpu::Prover p;
    if (!gpu::create(p, num_words, gpu::Alloc::Device, g_lane_cap, 1.0, 0.0, err, sizeof err)) {
        fprintf(stderr, "[gpu] FATAL: %s\n", err);
        return;
    }

    // Stream the upload through pinned staging so a multi-GB buffer never needs
    // a second full copy in host memory.
    size_t cap = 0;
    unsigned char* stage = gpu::staging(p, &cap);
    size_t left = bytes;
    while (left) {
        size_t chunk = left < cap ? left : cap;
        if (!recv_all(cfd, stage, chunk)) {
            fprintf(stderr, "[gpu] short data (%zu bytes outstanding)\n", left);
            gpu::destroy(p);
            return;
        }
        if (!gpu::push(p, chunk, err, sizeof err)) {
            fprintf(stderr, "[gpu] FATAL: %s\n", err);
            gpu::destroy(p);
            return;
        }
        left -= chunk;
    }

    const char* backing = "unknown";
    bool is_device = false;
    gpu::residency(p, &backing, &is_device);
    printf("[gpu] buffer uploaded; backing = %s\n", backing);
    report_hbm("after upload");

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
    const char* weights = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--weights") && i + 1 < argc) {
            weights = argv[++i];
        } else if (!strcmp(argv[i], "--allow-unlocked")) {
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

    gpu::Prover held;
    std::vector<void*> allocs;
    if (weights) {
        if (!load_weights(weights, held, &allocs)) return 1;
        report_hbm("after loading the weights");
    }
    ResidentOpts ropts;
    ropts.allow_unlocked = g_allow_unlocked;
    ropts.trace_every = g_trace_every;

    int lfd = make_listener(port);
    printf("[gpu] listening on 0.0.0.0:%u (lane cap %u)\n", port, g_lane_cap);
    for (;;) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        printf("[gpu] client %s connected\n", inet_ntoa(ca.sin_addr));
        if (weights) {
            long long served = serve_resident(cfd, held, ropts);
            if (served >= 0) printf("[gpu] served %lld challenges from the weights it holds\n", served);
        } else {
            serve(cfd);
        }
        close(cfd);
    }
    gpu::destroy(held);
    for (void* d : allocs) gpu::device_free(d);
    close(lfd);
    return 0;
}
