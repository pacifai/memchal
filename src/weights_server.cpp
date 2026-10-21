// weights_server.cpp — libmemchal_weights.so: serve challenges over memory an
// inference engine owns.
//
// The vLLM plugin (vllm_plugin/) loads this library with ctypes in the process
// that holds the model, passes the device pointer and length of every weight
// tensor, and calls mc_weights_start(). From then on a thread of ours listens
// for a verifier and answers each challenge by reading the tensors where the
// engine keeps them, on a CUDA stream of our own (gpu::attach with own_stream).
// Nothing is copied and nothing is uploaded.
//
// The engine's CUDA context is the process's primary context, which the CUDA
// runtime shares with every library in the process, so the engine's device
// pointers are valid here once the thread selects the same device.
//
// Plain C entry points, so ctypes needs no headers. Every call that can fail
// writes a readable reason into (err, errlen).
#include "gpu_resident.hpp"
#include "gpu_answer.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace mc;

namespace {

gpu::Prover g_prover;
std::thread g_thread;
std::atomic<bool> g_running{false};
int g_listen = -1;
int g_device = 0;
ResidentOpts g_opts;
FILE* g_trace = nullptr;

int listen_on(uint16_t port, char* err, size_t errlen) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errlen, "socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        snprintf(err, errlen, "cannot listen on port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void serve_forever() {
    char name[256] = {0}, err[512] = {0};
    if (!gpu::select_device(g_device, name, sizeof name, err, sizeof err)) {
        fprintf(stderr, "[memchal] cannot select device %d: %s\n", g_device, err);
        return;
    }
    while (g_running.load()) {
        sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(g_listen, (sockaddr*)&ca, &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;                       // the listener was shut down
        }
        fprintf(stderr, "[memchal] verifier %s connected\n", inet_ntoa(ca.sin_addr));
        long long served = serve_resident(cfd, g_prover, g_opts);
        if (served >= 0)
            fprintf(stderr, "[memchal] served %lld challenges from the engine's weights\n", served);
        close(cfd);
    }
}

} // namespace

extern "C" {

// Attach to `count` tensors (device pointers and lengths in 64-bit words, in
// buffer order) on `device`, listen on `port`, and serve from a background
// thread. Returns 0, or -1 with the reason in err. The attach and the bind
// happen before this returns, so their failures are reported here, not lost
// in the thread.
int mc_weights_start(int device, const uint64_t* const* bases, const uint64_t* words,
                     uint32_t count, uint16_t port, uint32_t lane_cap, int allow_unlocked,
                     uint32_t trace_every, const char* trace_path, char* err, size_t errlen) {
    if (g_running.load()) { snprintf(err, errlen, "already serving"); return -1; }
    char name[256] = {0};
    if (!gpu::select_device(device, name, sizeof name, err, errlen)) return -1;
    if (!gpu::attach(g_prover, bases, words, count, lane_cap, /*own_stream=*/true, err, errlen)) return -1;

    g_listen = listen_on(port, err, errlen);
    if (g_listen < 0) { gpu::destroy(g_prover); return -1; }

    g_trace = nullptr;
    if (trace_path && *trace_path) {
        g_trace = fopen(trace_path, "a");
        if (!g_trace) {
            snprintf(err, errlen, "cannot open trace file %s: %s", trace_path, strerror(errno));
            close(g_listen); g_listen = -1; gpu::destroy(g_prover);
            return -1;
        }
    }
    g_opts = ResidentOpts{};
    g_opts.allow_unlocked = allow_unlocked != 0;
    g_opts.trace_every = trace_every;
    g_opts.trace = g_trace;
    g_opts.tag = "[memchal]";
    g_device = device;
    g_running = true;
    g_thread = std::thread(serve_forever);
    return 0;
}

// What the driver says backs the attached tensors, and whether every one is
// device memory (1) or not (0). -1 if nothing is attached.
int mc_weights_backing(char* out, size_t n) {
    const char* backing = "nothing attached";
    bool is_device = false;
    bool ok = gpu::residency(g_prover, &backing, &is_device);
    if (out && n) snprintf(out, n, "%s", backing);
    return (g_prover.alloc == gpu::Alloc::External && ok) ? (is_device ? 1 : 0) : -1;
}

// Stop listening and wait for the serving thread. A verifier still connected
// keeps it until that verifier disconnects.
void mc_weights_stop(void) {
    if (!g_running.exchange(false)) return;
    if (g_listen >= 0) { shutdown(g_listen, SHUT_RDWR); close(g_listen); g_listen = -1; }
    if (g_thread.joinable()) g_thread.join();
    gpu::destroy(g_prover);
    if (g_trace) { fclose(g_trace); g_trace = nullptr; }
}

} // extern "C"
