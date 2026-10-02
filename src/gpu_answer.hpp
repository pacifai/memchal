// gpu_answer.hpp — host-callable surface of the CUDA answer path.
//
// Deliberately free of CUDA types so it can be included by plain g++ code. The
// implementation lives in gpu_answer.cu, which includes challenge.hpp and
// cuda_runtime.h and nothing else — no POSIX, no sockets. That separation is
// the reason common.hpp was split: the server half of the prover needs sockets,
// the kernel half must not see them.
//
// Every call that can fail takes an (err, errlen) pair and writes a human
// readable reason, so the server can log why it is refusing to run.
#pragma once
#include <cstdint>
#include <cstddef>

namespace mc {
namespace gpu {

// How the challenge buffer is backed. Only Device is honest.
//
//   Device     — cudaMalloc. Resident in HBM, not pageable, not migratable.
//   Managed    — cudaMallocManaged. The UVM adversary: pages migrate between
//                host and device on fault, so a prover can "hold" more than the
//                card has and still answer correctly. The GPU analog of the
//                mmapdisk cheat, and the reason "did you cudaMalloc?" cannot be
//                settled from outside.
//   HostMapped — cudaHostAlloc(cudaHostAllocMapped). The buffer sits in pinned
//                host DRAM and every probe crosses PCIe.
//   Split      — part of the buffer in device memory and the rest in pinned
//                host memory, in a fixed ratio. This is the partial-residency
//                adversary, and it is the one that sets the detector's
//                RESOLUTION: uvm and zerocopy answer "can you catch a prover
//                holding none of it", this one answers "how little can a prover
//                offload before you stop seeing it".
enum class Alloc { Device, Managed, HostMapped, Split };

const char* alloc_name(Alloc a);

struct Prover {
    uint64_t* d_buf      = nullptr; // device-visible words (the resident part
                                    // when alloc == Split)
    uint64_t* d_off      = nullptr; // Split: device pointer to the offloaded
                                    // tail, which lives in pinned host memory
    void*     h_off      = nullptr; // Split: the host allocation to free
    uint64_t  split_words = 0;      // Split: words [0, split) are on the card
    uint64_t  num_words  = 0;       // buffer size the verifier believes in
    uint64_t  held_words = 0;       // words actually allocated (< num_words == fake)
    bool      fabricate  = false;   // fabricate anything past held_words
    Alloc     alloc      = Alloc::Device;

    uint64_t* d_lane     = nullptr; // scratch: one hash per lane
    uint64_t* d_lane2    = nullptr; // second buffer: a tree level cannot reduce
                                    // in place, because thread i writes v[i]
                                    // while thread i/2 is still reading it
    uint64_t* h_lane     = nullptr; // pinned host copy of the above
    uint32_t  lane_cap   = 0;

    void*     h_mapped   = nullptr; // HostMapped: the host allocation to free
    unsigned char* h_stage = nullptr; // pinned upload staging
    size_t    stage_cap  = 0;
    uint64_t  cursor     = 0;         // bytes pushed to the device so far

    void*     ev_start   = nullptr;   // cudaEvent_t, opaque here
    void*     ev_stop    = nullptr;

    // prefetch cheat: answer in three passes instead of one. See answer().
    bool      prefetch   = false;
    uint64_t* d_pf_idx   = nullptr;   // the probe addresses the dry run listed
    uint64_t* d_pf_val   = nullptr;   // the words fetched for them in one batch
    uint32_t  pf_cap     = 0;

    // swap cheat: a pinned host copy of the buffer, so the card copy can be
    // freed between challenges and restored when one arrives.
    void*     h_swap     = nullptr;
    bool      swapped_out = false;
};

// The prefetch cheat. Turns on a three-pass answer: a dry run that lists the
// probe addresses, one parallel batch that fetches those words, then the real
// answer reading from the batch. Meant for a HostMapped buffer, where each
// serial probe is a PCIe round trip and batching them is the whole gain. The
// batch matches only when the addresses can be listed without reading the
// data, which is what the dependent chain takes away; on a mismatch the answer
// falls back to a direct read, so it stays correct either way.
void enable_prefetch(Prover& p);

// The swap cheat. Keeps a pinned host copy of the buffer, filled during the
// upload, so swap_out() can free the card copy and swap_in() can restore it.
// Call before the upload. Between the two calls the HBM is free for anything
// else, which is the point of the cheat; answer() refuses while swapped out.
bool enable_swap(Prover& p, char* err, size_t errlen);
bool swap_out(Prover& p, char* err, size_t errlen);
bool swap_in(Prover& p, double* ms, char* err, size_t errlen);

// Pick a device and report what it is. Returns false if there is no usable GPU.
bool select_device(int ordinal, char* name, size_t name_len, char* err, size_t errlen);

// Allocate the challenge buffer and the per-lane scratch.
//
// held_fraction < 1.0 allocates only that fraction of the buffer and turns on
// fabrication for the rest — the `fake` adversary, which saves real memory and
// is caught by correctness rather than timing.
// Two independent knobs, easy to confuse:
//   held_fraction  < 1.0  — the `fake` cheat. Allocate only this much and
//                           fabricate the rest. Saves memory, answers wrong.
//   offload_fraction > 0  — the `partial` cheat (Alloc::Split). Hold all the
//                           data, but keep this fraction off the card. Answers
//                           correctly, and slowly in proportion.
bool create(Prover& p, uint64_t num_words, Alloc alloc, uint32_t lane_cap,
            double held_fraction, double offload_fraction, char* err, size_t errlen);

void destroy(Prover& p);

// Pinned staging for the upload. The server recv()s straight into this, then
// calls push() to move it onto the device, so a multi-GB buffer never needs a
// second host copy.
unsigned char* staging(Prover& p, size_t* cap);
bool push(Prover& p, size_t bytes, char* err, size_t errlen);

// Fill the buffer on-device from the splitmix64 generator instead of receiving
// it. Used by the k=0 transport floor and by local self-tests, never as part of
// an honest protocol run.
bool fill_splitmix64(Prover& p, uint64_t seed, char* err, size_t errlen);

// Answer one challenge. The lane kernel runs on the device; the fold across
// lanes is a strict sequential chain in lane order, so it happens on the host
// using the same functions the CPU reference uses.
//
// The three timings are reported separately because they are different physics:
// kernel_ms is the memory-bound work, copy_ms is PCIe, fold_ms is host ALU.
// Lumping them together is how a bandwidth number gets flattered.
// `tree` selects the parallel Merkle fold over the sequential chain. The chain
// runs on the host (the lane hashes come back over PCIe); the tree runs on the
// device in log2(lanes) passes and only the root comes back. They are different
// protocols with different answers — the caller must pass whatever the verifier
// asked for on the wire.
bool answer(Prover& p, uint64_t nonce, uint32_t k, uint32_t lanes, bool chained,
            bool tree, uint64_t* out_hash, double* kernel_ms, double* copy_ms,
            double* fold_ms, char* err, size_t errlen);

// The cudaMalloc analog of prover.cpp's VmLck check (ADR 0002). Reports what
// the driver says backs the pointer. A managed or host pointer means the buffer
// is NOT unconditionally resident in HBM.
//
// Like VmLck this is self-reported — it stops the prover fooling itself, not a
// remote verifier. Proving it to a verifier is what the timing detector is for.
bool residency(const Prover& p, const char** backing, bool* is_device);

// cudaMemGetInfo, for the demo's HBM occupancy readout.
void mem_info(size_t* free_bytes, size_t* total_bytes);

// Hold `bytes` of device memory and do nothing with it, shrinking the HBM left
// for the buffer. This is the GPU analog of the CPU demo's container RAM
// budget: it is what forces a managed (UVM) buffer to actually page instead of
// quietly migrating everything to the card and looking honest. Pass 0 to
// release.
bool hog(size_t bytes, char* err, size_t errlen);
void unhog();

// Achieved read bandwidth in GB/s for a challenge that probed k words of 8
// bytes in kernel_ms. This is the honest-side number the GPU port hinges on.
double probe_gbps(uint32_t k, double kernel_ms);

// Fraction of the buffer this prover actually holds in device memory, as it
// understands itself. 1.0 for an honest cudaMalloc, 0.0 for host-mapped, the
// split ratio for a partial cheat, the held fraction for a fake one. Managed
// memory reports 1.0 because the driver will not say how much has migrated —
// which is the whole reason that cheat is hard, and the page says so.
double resident_fraction(const Prover& p);

} // namespace gpu
} // namespace mc
