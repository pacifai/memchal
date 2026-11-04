#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace mc {
namespace a2 {

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

static constexpr size_t kB2bOutBytes   = 64;
static constexpr size_t kB2bBlockBytes = 128;

static const uint64_t kB2bIV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

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
    uint64_t t[2];
    uint64_t f[2];
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

static inline void blake2b_init(blake2b_state* S, size_t outlen) {
    memset(S, 0, sizeof(*S));
    for (int i = 0; i < 8; ++i) S->h[i] = kB2bIV[i];
    S->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;
    S->outlen = outlen;
}

static inline void blake2b_update(blake2b_state* S, const void* in, size_t inlen) {
    if (inlen == 0) return;
    const uint8_t* p = (const uint8_t*)in;
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
    S->f[0] = (uint64_t)-1;
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

// H', Argon2's variable-length hash (RFC 9106 section 3.3).
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

static constexpr size_t   kBlockBytes  = 1024;
static constexpr size_t   kBlockWords  = kBlockBytes / 8;
static constexpr uint32_t kSyncPoints  = 4;
static constexpr uint32_t kVersion     = 0x13;
static constexpr uint32_t kTypeArgon2d = 0;
static constexpr size_t   kPrehashLen  = 64;
static constexpr size_t   kPrehashSeed = kPrehashLen + 8;

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

static const int kRowOffsets[16] = {
    0, 1, 16, 17, 32, 33, 48, 49, 64, 65, 80, 81, 96, 97, 112, 113
};

// Argon2's compression function G (RFC 9106 section 3.4.2). Writes G(prev, ref)
// to next, XORed into next's old value when with_xor is set.
static inline void fill_block(const uint64_t* prev, const uint64_t* ref,
                              uint64_t* next, bool with_xor) {
    uint64_t R[kBlockWords], tmp[kBlockWords];
    for (size_t i = 0; i < kBlockWords; ++i) R[i] = prev[i] ^ ref[i];
    memcpy(tmp, R, sizeof(R));
    if (with_xor)
        for (size_t i = 0; i < kBlockWords; ++i) tmp[i] ^= next[i];

    for (int i = 0; i < 8; ++i) a2_round(R + 16 * i);

    for (int i = 0; i < 8; ++i) {
        uint64_t t[16];
        for (int j = 0; j < 16; ++j) t[j] = R[2 * i + kRowOffsets[j]];
        a2_round(t);
        for (int j = 0; j < 16; ++j) R[2 * i + kRowOffsets[j]] = t[j];
    }

    for (size_t i = 0; i < kBlockWords; ++i) next[i] = tmp[i] ^ R[i];
}

// The column of the reference block (RFC 9106 section 3.4.1.2).
//   index: position within the current segment.
static inline uint32_t index_alpha(uint32_t pass, uint32_t slice, uint32_t index,
                                   uint32_t seg_len, uint32_t lane_len,
                                   uint32_t pseudo_rand, bool same_lane) {
    uint32_t ref_area;
    if (pass == 0) {
        if (slice == 0) {
            ref_area = index - 1;
        } else if (same_lane) {
            ref_area = slice * seg_len + index - 1;
        } else {
            ref_area = slice * seg_len - (index == 0 ? 1 : 0);
        }
    } else {
        if (same_lane) ref_area = lane_len - seg_len + index - 1;
        else           ref_area = lane_len - seg_len - (index == 0 ? 1 : 0);
    }

    uint64_t rel = pseudo_rand;
    rel = (rel * rel) >> 32;
    rel = (uint64_t)ref_area - 1 - (((uint64_t)ref_area * rel) >> 32);

    uint32_t start = 0;
    if (pass != 0)
        start = (slice == kSyncPoints - 1) ? 0 : (slice + 1) * seg_len;

    return (uint32_t)((start + rel) % lane_len);
}

// Blocks in the matrix: memory_kib rounded down to a multiple of 4 * parallelism.
static inline uint32_t argon2d_blocks(uint32_t memory_kib, uint32_t parallelism) {
    if (parallelism == 0) return 0;
    const uint32_t unit = kSyncPoints * parallelism;
    return (memory_kib / unit) * unit;
}

struct Params {
    const uint8_t* pwd    = nullptr; uint32_t pwdlen    = 0;
    const uint8_t* salt   = nullptr; uint32_t saltlen   = 0;
    const uint8_t* secret = nullptr; uint32_t secretlen = 0;
    const uint8_t* ad     = nullptr; uint32_t adlen     = 0;
    uint32_t memory_kib  = 0;
    uint32_t passes      = 0;
    uint32_t parallelism = 0;
};

// H0, the pre-hash over every parameter and input (RFC 9106 section 3.2).
static inline void initial_hash(uint8_t out[kPrehashLen], const Params& prm,
                                uint32_t taglen) {
    blake2b_state S;
    blake2b_init(&S, kPrehashLen);
    uint8_t w[4];
    store32(w, prm.parallelism); blake2b_update(&S, w, 4);
    store32(w, taglen);          blake2b_update(&S, w, 4);
    store32(w, prm.memory_kib);  blake2b_update(&S, w, 4);
    store32(w, prm.passes);      blake2b_update(&S, w, 4);
    store32(w, kVersion);        blake2b_update(&S, w, 4);
    store32(w, kTypeArgon2d);    blake2b_update(&S, w, 4);
    store32(w, prm.pwdlen);      blake2b_update(&S, w, 4);
    if (prm.pwdlen)    blake2b_update(&S, prm.pwd, prm.pwdlen);
    store32(w, prm.saltlen);     blake2b_update(&S, w, 4);
    if (prm.saltlen)   blake2b_update(&S, prm.salt, prm.saltlen);
    store32(w, prm.secretlen);   blake2b_update(&S, w, 4);
    if (prm.secretlen) blake2b_update(&S, prm.secret, prm.secretlen);
    store32(w, prm.adlen);       blake2b_update(&S, w, 4);
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

// Fills one lane's segment of one slice.
static inline void fill_segment(uint64_t* mem, uint32_t pass, uint32_t slice,
                                uint32_t lane, uint32_t lanes,
                                uint32_t lane_len, uint32_t seg_len) {
    const uint32_t start = (pass == 0 && slice == 0) ? 2u : 0u;
    uint32_t curr = lane * lane_len + slice * seg_len + start;
    uint32_t prev = (curr % lane_len == 0) ? curr + lane_len - 1 : curr - 1;

    for (uint32_t i = start; i < seg_len; ++i, ++curr, ++prev) {
        if (curr % lane_len == 1) prev = curr - 1;

        const uint64_t pseudo = mem[(size_t)prev * kBlockWords];
        uint32_t ref_lane = (uint32_t)(pseudo >> 32) % lanes;
        if (pass == 0 && slice == 0) ref_lane = lane;

        const uint32_t ref_index =
            index_alpha(pass, slice, i, seg_len, lane_len,
                        (uint32_t)(pseudo & 0xFFFFFFFFULL), ref_lane == lane);

        fill_block(mem + (size_t)prev * kBlockWords,
                   mem + (size_t)(ref_lane * lane_len + ref_index) * kBlockWords,
                   mem + (size_t)curr * kBlockWords,
                   pass != 0);
    }
}

// Fills every lane's segment of one slice on `workers` threads.
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
    } catch (...) {}
    run(0);
    for (uint32_t w = started; w < workers; ++w) run(w);
    for (std::thread& th : pool) th.join();
}

// The Argon2 tag of a finished matrix.
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

// Runs Argon2d into `mem`, which must hold argon2d_blocks() blocks, and
// writes the tag when `tag` is set.
static inline bool argon2d_raw(uint64_t* mem, const Params& prm,
                               uint8_t* tag, uint32_t taglen) {
    if (!mem || prm.parallelism == 0 || prm.passes == 0) return false;
    if (tag && (taglen < 4 || taglen > 0xFFFFFFF0u)) return false;
    if (prm.memory_kib < 8 * prm.parallelism) return false;

    const uint32_t m_prime  = argon2d_blocks(prm.memory_kib, prm.parallelism);
    const uint32_t lane_len = m_prime / prm.parallelism;
    const uint32_t seg_len  = lane_len / kSyncPoints;
    if (seg_len < 2) return false;

    uint8_t h0[kPrehashLen];
    initial_hash(h0, prm, tag ? taglen : 32);
    fill_first_blocks(mem, h0, prm.parallelism, lane_len);

    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const uint32_t workers = prm.parallelism < hw ? prm.parallelism : hw;

    for (uint32_t pass = 0; pass < prm.passes; ++pass)
        for (uint32_t slice = 0; slice < kSyncPoints; ++slice)
            fill_slice(mem, pass, slice, prm.parallelism, lane_len, seg_len, workers);

    if (tag) finalize_tag(mem, prm.parallelism, lane_len, tag, taglen);
    return true;
}

} // namespace a2

// The parameters the verifier sends in place of the buffer.
struct Recipe {
    uint8_t  key[32]          = {};
    uint8_t  salt[16]         = {};
    uint32_t memory_kib       = 0;
    uint32_t passes           = 2;
    uint32_t parallelism      = 1;
    uint8_t  workload_tag[32] = {};
};

static inline a2::Params argon2_params(const Recipe& r) {
    a2::Params prm;
    prm.pwd         = r.key;          prm.pwdlen  = sizeof(r.key);
    prm.salt        = r.salt;         prm.saltlen = sizeof(r.salt);
    prm.ad          = r.workload_tag; prm.adlen   = sizeof(r.workload_tag);
    prm.memory_kib  = r.memory_kib;
    prm.passes      = r.passes;
    prm.parallelism = r.parallelism;
    return prm;
}

static inline uint64_t build_buffer_words(const Recipe& r) {
    return (uint64_t)a2::argon2d_blocks(r.memory_kib, r.parallelism) * a2::kBlockWords;
}
static inline uint64_t build_buffer_bytes(const Recipe& r) {
    return build_buffer_words(r) * 8;
}

// Builds the buffer described by `r` in place.
//   tag_out: when set, receives the 32-byte Argon2 tag.
static inline bool build_buffer(uint64_t* buf, uint64_t num_words, const Recipe& r,
                                uint8_t tag_out[32] = nullptr) {
    if (num_words != build_buffer_words(r) || num_words == 0) return false;
    return a2::argon2d_raw(buf, argon2_params(r), tag_out, tag_out ? 32u : 0u);
}

} // namespace mc
