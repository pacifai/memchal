#pragma once
// The ChaCha20 stream cipher from RFC 8439 (https://www.rfc-editor.org/rfc/rfc8439).
// The verifier fills the default buffer with ChaCha20 keystream under a fresh
// random key, so the prover can't predict or compress the contents, and draws
// its challenge nonces from it.
//
// The code lives in the repo so the verifier and the provers build with no
// dependencies; only the keystream is needed, not encryption.
// tests/shared/challenge_kat.cpp checks a block against RFC 8439's test vector.
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mc {

static inline uint32_t cc20_rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

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

// One 64-byte keystream block (RFC 8439 section 2.3).
static inline void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                                  uint32_t counter, uint8_t out[64]) {
    uint32_t s[16];
    s[ 0] = 0x61707865UL;
    s[ 1] = 0x3320646eUL;
    s[ 2] = 0x79622d32UL;
    s[ 3] = 0x6b206574UL;
    s[ 4] = cc20_u8tou32le(key +  0);
    s[ 5] = cc20_u8tou32le(key +  4);
    s[ 6] = cc20_u8tou32le(key +  8);
    s[ 7] = cc20_u8tou32le(key + 12);
    s[ 8] = cc20_u8tou32le(key + 16);
    s[ 9] = cc20_u8tou32le(key + 20);
    s[10] = cc20_u8tou32le(key + 24);
    s[11] = cc20_u8tou32le(key + 28);
    s[12] = counter;
    s[13] = cc20_u8tou32le(nonce + 0);
    s[14] = cc20_u8tou32le(nonce + 4);
    s[15] = cc20_u8tou32le(nonce + 8);

    uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = s[i];

    for (int round = 0; round < 10; ++round) {
        cc20_qr(w[ 0], w[ 4], w[ 8], w[12]);
        cc20_qr(w[ 1], w[ 5], w[ 9], w[13]);
        cc20_qr(w[ 2], w[ 6], w[10], w[14]);
        cc20_qr(w[ 3], w[ 7], w[11], w[15]);
        cc20_qr(w[ 0], w[ 5], w[10], w[15]);
        cc20_qr(w[ 1], w[ 6], w[11], w[12]);
        cc20_qr(w[ 2], w[ 7], w[ 8], w[13]);
        cc20_qr(w[ 3], w[ 4], w[ 9], w[14]);
    }

    for (int i = 0; i < 16; ++i)
        cc20_u32tou8le(out + i * 4, w[i] + s[i]);
}

// Writes `nbytes` of keystream starting at block `start_block`. The block
// counter is 64 bits wide: its high half takes the first four nonce bytes.
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
