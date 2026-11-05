#pragma once
#include "gpu/answer.hpp"
#include "shared/server.hpp"

namespace mc {

// Copies `bytes` from src to the card through the prover's pinned staging buffer.
template <typename P>
static inline bool upload(P& p, Source& src, size_t bytes, char* err, size_t errlen) {
    size_t left = bytes;
    while (left) {
        const size_t chunk = left < p.stage.cap ? left : p.stage.cap;
        if (!src_read(src, p.stage.h, chunk)) {
            snprintf(err, errlen, "short data (%zu bytes outstanding)", left);
            return false;
        }
        if (!push(p, chunk, err, errlen)) return false;
        left -= chunk;
    }
    return true;
}

} // namespace mc
