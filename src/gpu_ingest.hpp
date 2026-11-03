// gpu_ingest.hpp — moves the buffer onto the card for the GPU provers. Host
// only: never include it from a .cu file.
#pragma once
#include "gpu_answer.hpp"
#include "server.hpp"

namespace mc {

// Copies `bytes` from src onto the device through pinned staging, so an upload
// never needs a host copy of the whole buffer.
static inline bool ingest(gpu::Prover& p, Source& src, size_t bytes, char* err, size_t errlen) {
    size_t cap = 0;
    unsigned char* stage = gpu::staging(p, &cap);
    size_t left = bytes;
    while (left) {
        size_t chunk = left < cap ? left : cap;
        if (!src_read(src, stage, chunk)) {
            snprintf(err, errlen, "short data (%zu bytes outstanding)", left);
            return false;
        }
        if (!gpu::push(p, chunk, err, errlen)) return false;
        left -= chunk;
    }
    return true;
}

} // namespace mc
