// chacha20.hpp — vendored ChaCha20 stream cipher (RFC 8439).
//
// Provides:
//   chacha20_block()  — single 64-byte keystream block (counter-mode ARX).
//   chacha20_fill()   — any number of keystream bytes, with a 64-bit block counter.
//
// This is a table-free, pure-ARX implementation derived from the RFC 8439
// reference description (§2.1 quarter round, §2.3 block function).  It has
// been verified against the RFC §2.3.2 test vector (see tests/known_answer.cpp).
//
// No external dependencies; header-only.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mc {

// ---------------------------------------------------------------------------
// ChaCha20 internals
// ---------------------------------------------------------------------------

static inline uint32_t cc20_rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

// Quarter-round (RFC 8439 §2.1).
static inline void cc20_qr(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = cc20_rotl32(d, 16);
    c += d; b ^= c; b = cc20_rotl32(b, 12);
    a += b; d ^= a; d = cc20_rotl32(d,  8);
    c += d; b ^= c; b = cc20_rotl32(b,  7);
}

static inline uint32_t cc20_u8tou32le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void cc20_u32tou8le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >>  8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// ---------------------------------------------------------------------------
// chacha20_block — RFC 8439 §2.3
//
// key[32]    : 32-byte (256-bit) secret key
// nonce[12]  : 12-byte (96-bit) nonce
// counter    : 32-bit block counter (first block is typically 0 or 1)
// out[64]    : 64 bytes of keystream
// ---------------------------------------------------------------------------
static inline void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                                  uint32_t counter, uint8_t out[64]) {
    // Constants: "expand 32-byte k" (RFC 8439 §2.3)
    uint32_t s[16];
    s[ 0] = 0x61707865UL;
    s[ 1] = 0x3320646eUL;
    s[ 2] = 0x79622d32UL;
    s[ 3] = 0x6b206574UL;
    // Key words (little-endian)
    s[ 4] = cc20_u8tou32le(key +  0);
    s[ 5] = cc20_u8tou32le(key +  4);
    s[ 6] = cc20_u8tou32le(key +  8);
    s[ 7] = cc20_u8tou32le(key + 12);
    s[ 8] = cc20_u8tou32le(key + 16);
    s[ 9] = cc20_u8tou32le(key + 20);
    s[10] = cc20_u8tou32le(key + 24);
    s[11] = cc20_u8tou32le(key + 28);
    // Counter
    s[12] = counter;
    // Nonce words (little-endian)
    s[13] = cc20_u8tou32le(nonce + 0);
    s[14] = cc20_u8tou32le(nonce + 4);
    s[15] = cc20_u8tou32le(nonce + 8);

    // Working copy
    uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = s[i];

    // 20 rounds = 10 column rounds + 10 diagonal rounds
    for (int round = 0; round < 10; ++round) {
        // Column rounds
        cc20_qr(w[ 0], w[ 4], w[ 8], w[12]);
        cc20_qr(w[ 1], w[ 5], w[ 9], w[13]);
        cc20_qr(w[ 2], w[ 6], w[10], w[14]);
        cc20_qr(w[ 3], w[ 7], w[11], w[15]);
        // Diagonal rounds
        cc20_qr(w[ 0], w[ 5], w[10], w[15]);
        cc20_qr(w[ 1], w[ 6], w[11], w[12]);
        cc20_qr(w[ 2], w[ 7], w[ 8], w[13]);
        cc20_qr(w[ 3], w[ 4], w[ 9], w[14]);
    }

    // Add initial state and serialize (little-endian)
    for (int i = 0; i < 16; ++i)
        cc20_u32tou8le(out + i * 4, w[i] + s[i]);
}

// ---------------------------------------------------------------------------
// chacha20_fill — `nbytes` of keystream under `key`, starting at block
// `start_block`.
//
// RFC 8439's 32-bit block counter covers only 256 GiB, so this uses the
// original ChaCha20 layout instead: a 64-bit block counter in state words
// 12-13 and a zero 64-bit nonce in words 14-15. It reuses chacha20_block() by
// passing the counter's high half as the first four nonce bytes.
// ---------------------------------------------------------------------------
static inline void chacha20_fill(uint8_t* out, size_t nbytes,
                                 const uint8_t key[32], uint64_t start_block) {
    uint8_t block[64];
    uint8_t nonce[12] = {};
    size_t written = 0;
    for (uint64_t blk = start_block; written < nbytes; ++blk) {
        uint32_t hi = (uint32_t)(blk >> 32);
        for (int b = 0; b < 4; ++b) nonce[b] = (uint8_t)(hi >> (8 * b));
        chacha20_block(key, nonce, (uint32_t)blk, block);
        size_t take = nbytes - written;
        if (take > 64) take = 64;
        memcpy(out + written, block, take);
        written += take;
    }
}

} // namespace mc
