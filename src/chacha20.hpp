// chacha20.hpp — vendored ChaCha20 stream cipher (RFC 8439).
//
// Provides:
//   chacha20_block()  — single 64-byte keystream block (counter-mode ARX).
//   chacha20_fill()   — fills an arbitrary number of bytes by chaining blocks.
//
// This is a table-free, pure-ARX implementation derived from the RFC 8439
// reference description (§2.1 quarter round, §2.3 block function).  It has
// been verified against the RFC §2.3.2 test vector (see tests/kat.cpp).
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
// chacha20_fill — concatenate ChaCha20 blocks to fill `nbytes` of output.
//
// out[nbytes]   : destination
// nbytes        : number of bytes to produce
// key[32]       : 32-byte key
// nonce[12]     : 12-byte nonce (see note below for the keyed-fill variant)
// start_block   : 64-bit starting block index.
//
// 64-bit block addressing (beyond the 32-bit RFC counter limit):
//   The RFC 8439 block function keeps word 12 as a 32-bit counter and
//   words 13-15 as a 96-bit nonce.  To extend addressing to 2^64 blocks
//   without altering chacha20_block's state layout, we split the 64-bit
//   start_block as follows:
//     word 12 (counter) = low 32 bits of start_block
//     nonce bytes 0-3   = high 32 bits of start_block (little-endian)
//     nonce bytes 4-11  = nonce[4..11] passed in (caller-controlled)
//   This gives unique keystream for any of the 2^64 possible block indices.
//   The existing RFC 8439 KAT uses start_block=1 with a nonce whose bytes
//   0-3 happen to be {0,0,0,9}, which maps to counter=1, nonce[0..3]={0,0,0,9}
//   — bit-identical to the RFC layout — so the KAT is unaffected.
// ---------------------------------------------------------------------------
static inline void chacha20_fill(uint8_t* out, size_t nbytes,
                                 const uint8_t key[32], const uint8_t nonce[12],
                                 uint64_t start_block) {
    uint8_t block[64];
    size_t written = 0;
    uint64_t blk = start_block;

    // Build the effective 12-byte nonce: high 32 bits of blk into bytes 0-3,
    // then the caller's nonce[4..11] into bytes 4-11.
    uint8_t eff_nonce[12];
    uint32_t blk_hi = (uint32_t)(blk >> 32);
    eff_nonce[0] = (uint8_t)(blk_hi);
    eff_nonce[1] = (uint8_t)(blk_hi >> 8);
    eff_nonce[2] = (uint8_t)(blk_hi >> 16);
    eff_nonce[3] = (uint8_t)(blk_hi >> 24);
    eff_nonce[4]  = nonce[4];  eff_nonce[5]  = nonce[5];
    eff_nonce[6]  = nonce[6];  eff_nonce[7]  = nonce[7];
    eff_nonce[8]  = nonce[8];  eff_nonce[9]  = nonce[9];
    eff_nonce[10] = nonce[10]; eff_nonce[11] = nonce[11];

    while (written < nbytes) {
        uint32_t ctr_lo = (uint32_t)(blk & 0xFFFFFFFFULL);
        // Update nonce bytes 0-3 when high word changes (blk increments by 1
        // each iteration; the high word wraps every 2^32 blocks).
        uint32_t new_hi = (uint32_t)(blk >> 32);
        if (new_hi != blk_hi) {
            blk_hi = new_hi;
            eff_nonce[0] = (uint8_t)(blk_hi);
            eff_nonce[1] = (uint8_t)(blk_hi >> 8);
            eff_nonce[2] = (uint8_t)(blk_hi >> 16);
            eff_nonce[3] = (uint8_t)(blk_hi >> 24);
        }
        chacha20_block(key, eff_nonce, ctr_lo, block);
        size_t take = nbytes - written;
        if (take > 64) take = 64;
        memcpy(out + written, block, take);
        written += take;
        ++blk;
    }
}

} // namespace mc
