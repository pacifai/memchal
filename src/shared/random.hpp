#pragma once
#include <cstddef>
#include <cstdint>
#include <random>

namespace mc {

static inline void random_bytes(uint8_t* out, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = rd();
        for (size_t b = 0; b < 4 && i + b < n; ++b) out[i + b] = (uint8_t)(w >> (8 * b));
    }
}

} // namespace mc
