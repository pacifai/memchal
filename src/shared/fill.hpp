#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
#include "shared/chacha20.hpp"
#include "shared/challenge.hpp"

namespace mc {

// Fills the buffer with ChaCha20 keystream, split across the machine's cores.
static inline void fill_chacha(uint64_t* buf, uint64_t num_words, const uint8_t key[32]) {
    const size_t nbytes = (size_t)num_words * 8;
    const size_t blocks = (nbytes + 63) / 64;
    unsigned threads = std::min(std::max(std::thread::hardware_concurrency(), 1u), 64u);
    if (blocks < (size_t)threads * 1024) threads = 1;
    const size_t per = (blocks + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) {
        const size_t b0 = (size_t)i * per;
        if (b0 >= blocks) break;
        const size_t off = b0 * 64;
        const size_t len = std::min(nbytes - off, per * 64);
        pool.emplace_back([=] { chacha20_fill((uint8_t*)buf + off, len, key, b0); });
    }
    for (auto& t : pool) t.join();
}

static inline void fill_splitmix64(uint64_t* buf, uint64_t num_words, uint64_t seed) {
    uint64_t state = seed;
    for (uint64_t i = 0; i < num_words; ++i) buf[i] = splitmix64(state);
}

} // namespace mc
