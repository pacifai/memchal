// argon2d.hpp — Argon2d (RFC 9106), vendored. Argon2d only, version 0x13.
//
// build_buffer() hands back the filled block matrix, not the tag: memchal uses
// the matrix as the challenge buffer. Rationale and cost parameters in ADR 0007.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace mc {
namespace a2 {

// Argon2 and Blake2b are defined over little-endian bytes regardless of host.
static inline uint64_t load64(const uint8_t* p) {
    return (uint64_t)p[0]        | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static inline void store64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void store32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint64_t rotr64(uint64_t w, unsigned c) {
    return (w >> c) | (w << (64 - c));
}

// ---- Blake2b (RFC 7693) ----

static constexpr size_t kB2bOutBytes   = 64;
static constexpr size_t kB2bBlockBytes = 128;

static const uint64_t kB2bIV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

// Message-word schedule. Rounds 10 and 11 repeat rounds 0 and 1.
static const uint8_t kB2bSigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

struct blake2b_state {
    uint64_t h[8];
    uint64_t t[2];       // message byte counter (128-bit)
    uint64_t f[2];       // finalization flags
    uint8_t  buf[kB2bBlockBytes];
    size_t   buflen;
    size_t   outlen;
};

static inline void b2b_g(uint64_t* v, const uint64_t* m, const uint8_t* s,
                         int i, int a, int b, int c, int d) {
    v[a] = v[a] + v[b] + m[s[2 * i]];
    v[d] = rotr64(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];
    v[b] = rotr64(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + m[s[2 * i + 1]];
    v[d] = rotr64(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = rotr64(v[b] ^ v[c], 63);
}

static inline void blake2b_compress(blake2b_state* S, const uint8_t block[128]) {
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; ++i) m[i] = load64(block + i * 8);
    for (int i = 0; i < 8; ++i)  v[i] = S->h[i];
    for (int i = 0; i < 8; ++i)  v[i + 8] = kB2bIV[i];
    v[12] ^= S->t[0];
    v[13] ^= S->t[1];
    v[14] ^= S->f[0];
    v[15] ^= S->f[1];
    for (int r = 0; r < 12; ++r) {
        const uint8_t* s = kB2bSigma[r];
        b2b_g(v, m, s, 0, 0, 4,  8, 12);
        b2b_g(v, m, s, 1, 1, 5,  9, 13);
        b2b_g(v, m, s, 2, 2, 6, 10, 14);
        b2b_g(v, m, s, 3, 3, 7, 11, 15);
        b2b_g(v, m, s, 4, 0, 5, 10, 15);
        b2b_g(v, m, s, 5, 1, 6, 11, 12);
        b2b_g(v, m, s, 6, 2, 7,  8, 13);
        b2b_g(v, m, s, 7, 3, 4,  9, 14);
    }
    for (int i = 0; i < 8; ++i) S->h[i] ^= v[i] ^ v[i + 8];
}

static inline void b2b_inc(blake2b_state* S, uint64_t n) {
    S->t[0] += n;
    if (S->t[0] < n) ++S->t[1];
}

// Unkeyed: Argon2 passes its secret inside H0's message, not as a Blake2b key.
static inline void blake2b_init(blake2b_state* S, size_t outlen) {
    memset(S, 0, sizeof(*S));
    for (int i = 0; i < 8; ++i) S->h[i] = kB2bIV[i];
    // Parameter block word 0: digest_length | key_length<<8 | fanout<<16 | depth<<24
    S->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;
    S->outlen = outlen;
}

static inline void blake2b_update(blake2b_state* S, const void* in, size_t inlen) {
    if (inlen == 0) return;
    const uint8_t* p = (const uint8_t*)in;
    // '>' not '>=': finalization needs the last block still uncompressed.
    if (S->buflen + inlen > kB2bBlockBytes) {
        size_t fill = kB2bBlockBytes - S->buflen;
        memcpy(S->buf + S->buflen, p, fill);
        b2b_inc(S, kB2bBlockBytes);
        blake2b_compress(S, S->buf);
        S->buflen = 0;
        p += fill; inlen -= fill;
        while (inlen > kB2bBlockBytes) {
            b2b_inc(S, kB2bBlockBytes);
            blake2b_compress(S, p);
            p += kB2bBlockBytes; inlen -= kB2bBlockBytes;
        }
    }
    memcpy(S->buf + S->buflen, p, inlen);
    S->buflen += inlen;
}

static inline void blake2b_final(blake2b_state* S, uint8_t* out, size_t outlen) {
    uint8_t full[kB2bOutBytes];
    b2b_inc(S, (uint64_t)S->buflen);
    S->f[0] = (uint64_t)-1;                                  // last block
    memset(S->buf + S->buflen, 0, kB2bBlockBytes - S->buflen);
    blake2b_compress(S, S->buf);
    for (int i = 0; i < 8; ++i) store64(full + i * 8, S->h[i]);
    memcpy(out, full, outlen);
}

static inline void blake2b(uint8_t* out, size_t outlen,
                           const void* in, size_t inlen) {
    blake2b_state S;
    blake2b_init(&S, outlen);
    blake2b_update(&S, in, inlen);
    blake2b_final(&S, out, outlen);
}

// H' — Argon2's variable-length hash (RFC 9106 section 3.3).
static inline void blake2b_long(uint8_t* out, size_t outlen,
                                const void* in, size_t inlen) {
    uint8_t olen_le[4];
    store32(olen_le, (uint32_t)outlen);

    if (outlen <= kB2bOutBytes) {
        blake2b_state S;
        blake2b_init(&S, outlen);
        blake2b_update(&S, olen_le, 4);
        blake2b_update(&S, in, inlen);
        blake2b_final(&S, out, outlen);
        return;
    }

    uint8_t cur[kB2bOutBytes], prev[kB2bOutBytes];
    blake2b_state S;
    blake2b_init(&S, kB2bOutBytes);
    blake2b_update(&S, olen_le, 4);
    blake2b_update(&S, in, inlen);
    blake2b_final(&S, cur, kB2bOutBytes);

    memcpy(out, cur, kB2bOutBytes / 2);
    out += kB2bOutBytes / 2;
    size_t left = outlen - kB2bOutBytes / 2;

    while (left > kB2bOutBytes) {
        memcpy(prev, cur, kB2bOutBytes);
        blake2b(cur, kB2bOutBytes, prev, kB2bOutBytes);
        memcpy(out, cur, kB2bOutBytes / 2);
        out += kB2bOutBytes / 2;
        left -= kB2bOutBytes / 2;
    }
    memcpy(prev, cur, kB2bOutBytes);
    blake2b(out, left, prev, kB2bOutBytes);
}

// ---- Argon2 core ----

static constexpr size_t   kBlockBytes = 1024;
static constexpr size_t   kBlockWords = kBlockBytes / 8;   // 128 uint64 words
static constexpr uint32_t kSyncPoints = 4;                 // slices per pass
static constexpr uint32_t kVersion    = 0x13;              // Argon2 v1.3
static constexpr uint32_t kTypeArgon2d = 0;
static constexpr size_t   kPrehashLen  = 64;               // H0
static constexpr size_t   kPrehashSeed = kPrehashLen + 8;  // H0 || LE32(i) || LE32(lane)

// fBlaMka (RFC 9106 section 3.4.1): add with a 32x32->64 multiply folded in.
static inline uint64_t fBlaMka(uint64_t x, uint64_t y) {
    const uint64_t xy = (uint64_t)(uint32_t)x * (uint64_t)(uint32_t)y;
    return x + y + 2 * xy;
}

static inline void a2_g(uint64_t& a, uint64_t& b, uint64_t& c, uint64_t& d) {
    a = fBlaMka(a, b);  d = rotr64(d ^ a, 32);
    c = fBlaMka(c, d);  b = rotr64(b ^ c, 24);
    a = fBlaMka(a, b);  d = rotr64(d ^ a, 16);
    c = fBlaMka(c, d);  b = rotr64(b ^ c, 63);
}

static inline void a2_round(uint64_t* v) {
    a2_g(v[0], v[4], v[ 8], v[12]);
    a2_g(v[1], v[5], v[ 9], v[13]);
    a2_g(v[2], v[6], v[10], v[14]);
    a2_g(v[3], v[7], v[11], v[15]);
    a2_g(v[0], v[5], v[10], v[15]);
    a2_g(v[1], v[6], v[11], v[12]);
    a2_g(v[2], v[7], v[ 8], v[13]);
    a2_g(v[3], v[4], v[ 9], v[14]);
}

// Row offsets for the second pass: word 2*i of each 16-word row.
static const int kRowOffsets[16] = {
    0, 1, 16, 17, 32, 33, 48, 49, 64, 65, 80, 81, 96, 97, 112, 113
};

// G(prev, ref) -> next (RFC 9106 section 3.4.2):
//   R = prev XOR ref;  next = R XOR P(rows of P(columns of R))
// with_xor accumulates into next's old value, as v1.3 requires after pass 0.
static inline void fill_block(const uint64_t* prev, const uint64_t* ref,
                              uint64_t* next, bool with_xor) {
    uint64_t R[kBlockWords], tmp[kBlockWords];
    for (size_t i = 0; i < kBlockWords; ++i) R[i] = prev[i] ^ ref[i];
    memcpy(tmp, R, sizeof(R));
    if (with_xor)
        for (size_t i = 0; i < kBlockWords; ++i) tmp[i] ^= next[i];

    // Columns: contiguous.
    for (int i = 0; i < 8; ++i) a2_round(R + 16 * i);

    // Rows: strided — gather, round, scatter.
    for (int i = 0; i < 8; ++i) {
        uint64_t t[16];
        for (int j = 0; j < 16; ++j) t[j] = R[2 * i + kRowOffsets[j]];
        a2_round(t);
        for (int j = 0; j < 16; ++j) R[2 * i + kRowOffsets[j]] = t[j];
    }

    for (size_t i = 0; i < kBlockWords; ++i) next[i] = tmp[i] ^ R[i];
}

// Reference-block selection (RFC 9106 section 3.4.1.2).
// `index` is the position within the current segment, not the column.
static inline uint32_t index_alpha(uint32_t pass, uint32_t slice, uint32_t index,
                                   uint32_t seg_len, uint32_t lane_len,
                                   uint32_t pseudo_rand, bool same_lane) {
    uint32_t ref_area;
    if (pass == 0) {
        if (slice == 0) {
            ref_area = index - 1;                       // everything earlier in the segment
        } else if (same_lane) {
            ref_area = slice * seg_len + index - 1;     // finished slices plus this lane's work
        } else {
            // Another lane: finished slices only, minus one at a segment boundary.
            ref_area = slice * seg_len - (index == 0 ? 1 : 0);
        }
    } else {
        // Later passes see the whole lane except the segment being overwritten.
        if (same_lane) ref_area = lane_len - seg_len + index - 1;
        else           ref_area = lane_len - seg_len - (index == 0 ? 1 : 0);
    }

    // Map pseudo_rand into [0, ref_area) with a quadratic bias toward recent
    // blocks (RFC 9106: x = J1^2 >> 32, y = (area * x) >> 32, z = area-1-y).
    uint64_t rel = pseudo_rand;
    rel = (rel * rel) >> 32;
    rel = (uint64_t)ref_area - 1 - (((uint64_t)ref_area * rel) >> 32);

    uint32_t start = 0;
    if (pass != 0)
        start = (slice == kSyncPoints - 1) ? 0 : (slice + 1) * seg_len;

    return (uint32_t)((start + rel) % lane_len);
}

// ---- The matrix fill ----

// Argon2 rounds m down to a multiple of 4*p. Size allocations from this, not m_kib.
static inline uint32_t argon2d_blocks(uint32_t m_kib, uint32_t p) {
    if (p == 0) return 0;
    const uint32_t unit = kSyncPoints * p;
    return (m_kib / unit) * unit;
}

struct Params {
    const uint8_t* pwd    = nullptr; uint32_t pwdlen    = 0;
    const uint8_t* salt   = nullptr; uint32_t saltlen   = 0;
    const uint8_t* secret = nullptr; uint32_t secretlen = 0;
    const uint8_t* ad     = nullptr; uint32_t adlen     = 0;
    uint32_t m_kib = 0;   // requested memory in KiB (== 1 KiB blocks)
    uint32_t t     = 0;   // passes
    uint32_t p     = 0;   // lanes
};

// H0 — the 64-byte pre-hash binding every parameter and input (RFC 9106 3.2).
static inline void initial_hash(uint8_t out[kPrehashLen], const Params& prm,
                                uint32_t taglen) {
    blake2b_state S;
    blake2b_init(&S, kPrehashLen);
    uint8_t w[4];
    store32(w, prm.p);          blake2b_update(&S, w, 4);
    store32(w, taglen);         blake2b_update(&S, w, 4);
    store32(w, prm.m_kib);      blake2b_update(&S, w, 4);
    store32(w, prm.t);          blake2b_update(&S, w, 4);
    store32(w, kVersion);       blake2b_update(&S, w, 4);
    store32(w, kTypeArgon2d);   blake2b_update(&S, w, 4);
    store32(w, prm.pwdlen);     blake2b_update(&S, w, 4);
    if (prm.pwdlen)    blake2b_update(&S, prm.pwd, prm.pwdlen);
    store32(w, prm.saltlen);    blake2b_update(&S, w, 4);
    if (prm.saltlen)   blake2b_update(&S, prm.salt, prm.saltlen);
    store32(w, prm.secretlen);  blake2b_update(&S, w, 4);
    if (prm.secretlen) blake2b_update(&S, prm.secret, prm.secretlen);
    store32(w, prm.adlen);      blake2b_update(&S, w, 4);
    if (prm.adlen)     blake2b_update(&S, prm.ad, prm.adlen);
    blake2b_final(&S, out, kPrehashLen);
}

static inline void fill_first_blocks(uint64_t* mem, const uint8_t h0[kPrehashLen],
                                     uint32_t lanes, uint32_t lane_len) {
    uint8_t seed[kPrehashSeed];
    uint8_t block[kBlockBytes];
    memcpy(seed, h0, kPrehashLen);
    for (uint32_t l = 0; l < lanes; ++l) {
        store32(seed + kPrehashLen + 4, l);
        for (uint32_t idx = 0; idx < 2; ++idx) {
            store32(seed + kPrehashLen, idx);
            blake2b_long(block, kBlockBytes, seed, kPrehashSeed);
            uint64_t* dst = mem + (size_t)(l * lane_len + idx) * kBlockWords;
            for (size_t i = 0; i < kBlockWords; ++i) dst[i] = load64(block + i * 8);
        }
    }
}

// Fills one segment: one lane's share of one slice. Within a slice, a lane
// writes only its own segment and reads other lanes only in slices that are
// already finished, so the p segments of a slice can fill in any order or at
// once. fill_slice() runs them on threads; the output is the same either way.
static inline void fill_segment(uint64_t* mem, uint32_t pass, uint32_t slice,
                                uint32_t lane, uint32_t lanes,
                                uint32_t lane_len, uint32_t seg_len) {
    const uint32_t start = (pass == 0 && slice == 0) ? 2u : 0u;
    uint32_t curr = lane * lane_len + slice * seg_len + start;
    // The first block of a lane refers back to that lane's last block.
    uint32_t prev = (curr % lane_len == 0) ? curr + lane_len - 1 : curr - 1;

    for (uint32_t i = start; i < seg_len; ++i, ++curr, ++prev) {
        if (curr % lane_len == 1) prev = curr - 1;      // undo the wrap above

        // Argon2d: the addressing words come from the data itself.
        const uint64_t pseudo = mem[(size_t)prev * kBlockWords];
        uint32_t ref_lane = (uint32_t)(pseudo >> 32) % lanes;
        if (pass == 0 && slice == 0) ref_lane = lane;   // no other lane exists yet

        const uint32_t ref_index =
            index_alpha(pass, slice, i, seg_len, lane_len,
                        (uint32_t)(pseudo & 0xFFFFFFFFULL), ref_lane == lane);

        fill_block(mem + (size_t)prev * kBlockWords,
                   mem + (size_t)(ref_lane * lane_len + ref_index) * kBlockWords,
                   mem + (size_t)curr * kBlockWords,
                   pass != 0);
    }
}

// Fills every lane's segment of one slice on `workers` threads, worker w taking
// lanes w, w + workers, ... The calling thread is worker 0. Returning joins all
// of them, which is the slice boundary Argon2 synchronizes on. If the OS
// refuses a thread, the calling thread fills that worker's lanes itself.
static inline void fill_slice(uint64_t* mem, uint32_t pass, uint32_t slice,
                              uint32_t lanes, uint32_t lane_len, uint32_t seg_len,
                              uint32_t workers) {
    auto run = [=](uint32_t w) {
        for (uint32_t lane = w; lane < lanes; lane += workers)
            fill_segment(mem, pass, slice, lane, lanes, lane_len, seg_len);
    };
    if (workers <= 1) { run(0); return; }

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    uint32_t started = 1;
    try {
        for (; started < workers; ++started) pool.emplace_back(run, started);
    } catch (...) {
        // Fall through: the workers that did not start run below.
    }
    run(0);
    for (uint32_t w = started; w < workers; ++w) run(w);
    for (std::thread& th : pool) th.join();
}

// XOR the last column across lanes, then squeeze with H'. Used only by tests.
static inline void finalize_tag(const uint64_t* mem, uint32_t lanes,
                                uint32_t lane_len, uint8_t* tag, uint32_t taglen) {
    uint64_t acc[kBlockWords];
    memcpy(acc, mem + (size_t)(lane_len - 1) * kBlockWords, sizeof(acc));
    for (uint32_t l = 1; l < lanes; ++l) {
        const uint64_t* b = mem + (size_t)(l * lane_len + lane_len - 1) * kBlockWords;
        for (size_t i = 0; i < kBlockWords; ++i) acc[i] ^= b[i];
    }
    uint8_t bytes[kBlockBytes];
    for (size_t i = 0; i < kBlockWords; ++i) store64(bytes + i * 8, acc[i]);
    blake2b_long(tag, taglen, bytes, kBlockBytes);
}

// `mem` must hold argon2d_blocks(m_kib, p) * kBlockWords words and is left
// holding the finished matrix. `tag` may be null.
static inline bool argon2d_raw(uint64_t* mem, const Params& prm,
                               uint8_t* tag, uint32_t taglen) {
    if (!mem || prm.p == 0 || prm.t == 0) return false;
    if (tag && (taglen < 4 || taglen > 0xFFFFFFF0u)) return false;
    if (prm.m_kib < 8 * prm.p) return false;              // RFC 9106 minimum

    const uint32_t m_prime  = argon2d_blocks(prm.m_kib, prm.p);
    const uint32_t lane_len = m_prime / prm.p;
    const uint32_t seg_len  = lane_len / kSyncPoints;
    if (seg_len < 2) return false;                        // blocks 0,1 are seeds

    uint8_t h0[kPrehashLen];
    initial_hash(h0, prm, tag ? taglen : 32);
    fill_first_blocks(mem, h0, prm.p, lane_len);

    // Up to one thread per lane, never more than the machine has cores. The
    // thread count changes only speed: p, not the thread count, defines the
    // matrix.
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const uint32_t workers = prm.p < hw ? prm.p : hw;

    for (uint32_t pass = 0; pass < prm.t; ++pass)
        for (uint32_t slice = 0; slice < kSyncPoints; ++slice)
            fill_slice(mem, pass, slice, prm.p, lane_len, seg_len, workers);

    if (tag) finalize_tag(mem, prm.p, lane_len, tag, taglen);
    return true;
}

} // namespace a2

// ---- memchal interface ----

// What the verifier ships in place of the buffer itself.
struct Recipe {
    uint8_t  key[32]  = {};   // 256-bit secret (Argon2's "password"), from random_device
    uint8_t  salt[16] = {};   // per-run salt, likewise random
    uint32_t m_kib    = 0;    // requested buffer size in KiB (== 1 KiB blocks)
    uint32_t t        = 2;    // passes; >= 2 makes a block's ancestry the whole matrix
    uint32_t p        = 1;    // BUILD lanes — not the challenge's `lanes`, a different knob
    // Reserved for weight binding (ADR 0007); passed to Argon2 as associated data.
    uint8_t  workload_tag[32] = {};
};

// Can be less than m_kib * 128 because of the 4*p rounding. Size allocations here.
static inline uint64_t build_buffer_words(const Recipe& r) {
    return (uint64_t)a2::argon2d_blocks(r.m_kib, r.p) * a2::kBlockWords;
}
static inline uint64_t build_buffer_bytes(const Recipe& r) {
    return build_buffer_words(r) * 8;
}

// Fills `buf` in place with exactly build_buffer_words(r) words; the matrix is
// the buffer, so there is no copy. tag_out is for tests.
static inline bool build_buffer(uint64_t* buf, uint64_t num_words, const Recipe& r,
                                uint8_t tag_out[32] = nullptr) {
    if (num_words != build_buffer_words(r) || num_words == 0) return false;
    a2::Params prm;
    prm.pwd    = r.key;          prm.pwdlen  = sizeof(r.key);
    prm.salt   = r.salt;         prm.saltlen = sizeof(r.salt);
    prm.ad     = r.workload_tag; prm.adlen   = sizeof(r.workload_tag);
    prm.m_kib  = r.m_kib;
    prm.t      = r.t;
    prm.p      = r.p;
    return a2::argon2d_raw(buf, prm, tag_out, tag_out ? 32u : 0u);
}

} // namespace mc
