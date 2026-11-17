#pragma once
#include <cstdint>
#include <vector>
#include "shared/challenge.hpp"

namespace mc {

// Lossless compression of bf16 weights that still reads any word directly, so
// each probe costs a few extra memory reads and no sequential decoding.
//
// A bf16 value is a sign bit, an 8-bit exponent, and a 7-bit mantissa. Weights
// in one block use a narrow range of exponents, so each block stores its
// lowest exponent once and every value's exponent as a `width`-bit offset from
// it. The sign and mantissa stay as one byte per value. Words that aren't bf16
// still decode correctly; they just compress less.
static constexpr uint32_t kCodecBlockWords  = 64;
static constexpr uint32_t kCodecValuesPerWord = 4;
static constexpr uint32_t kCodecBlockValues = kCodecBlockWords * kCodecValuesPerWord;

// Block header: bit offset into the exponent stream, lowest exponent, width.
MC_HOST_DEVICE static inline uint64_t codec_header(uint64_t bit, uint32_t base, uint32_t width) {
    return (bit << 16) | ((uint64_t)base << 8) | width;
}

struct CodecView {
    const uint64_t* head;
    const uint32_t* bytes;
    const uint64_t* exps;

    MC_HOST_DEVICE uint64_t operator()(uint64_t idx) const {
        const uint64_t h = head[idx / kCodecBlockWords];
        const uint32_t width = (uint32_t)(h & 0xFF), base = (uint32_t)((h >> 8) & 0xFF);
        const uint64_t bit = (h >> 16) + (idx % kCodecBlockWords) * kCodecValuesPerWord * width;
        const uint64_t at = bit >> 6, shift = bit & 63;
        uint64_t packed = exps[at] >> shift;
        if (shift + kCodecValuesPerWord * width > 64) packed |= exps[at + 1] << (64 - shift);
        const uint32_t sm = bytes[idx];
        const uint64_t mask = (1ull << width) - 1;
        uint64_t word = 0;
        for (uint32_t j = 0; j < kCodecValuesPerWord; ++j) {
            const uint64_t e = base + ((packed >> (j * width)) & mask);
            const uint64_t b = (sm >> (8 * j)) & 0xFF;
            word |= (((b & 0x80) << 8) | (e << 7) | (b & 0x7F)) << (16 * j);
        }
        return word;
    }
};

// Compresses words appended in order. Call finish() after the last one.
class CodecWriter {
public:
    void reserve(uint64_t words) {
        head_.reserve(words / kCodecBlockWords + 1);
        bytes_.reserve(words);
    }

    void append(const uint64_t* w, uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) {
            pending_.push_back(w[i]);
            if (pending_.size() == kCodecBlockWords) flush();
        }
    }

    void finish() {
        if (!pending_.empty()) flush();
        exps_.push_back(0);
    }

    uint64_t words() const { return bytes_.size(); }
    uint64_t compressed_bytes() const { return head_.size() * 8 + bytes_.size() * 4 + exps_.size() * 8; }
    const std::vector<uint64_t>& head() const { return head_; }
    const std::vector<uint32_t>& bytes() const { return bytes_; }
    const std::vector<uint64_t>& exps() const { return exps_; }

    CodecView view() const { return CodecView{head_.data(), bytes_.data(), exps_.data()}; }

private:
    static uint32_t exponent(uint64_t word, uint32_t j) { return (uint32_t)(word >> (16 * j + 7)) & 0xFF; }

    void put_bits(uint64_t v, uint32_t n) {
        if (!n) return;
        const uint32_t off = (uint32_t)(bit_ % 64);
        if (off == 0) exps_.push_back(0);
        exps_.back() |= v << off;
        if (off + n > 64) exps_.push_back(v >> (64 - off));
        bit_ += n;
    }

    void flush() {
        uint32_t lo = 0xFF, hi = 0;
        for (uint64_t w : pending_)
            for (uint32_t j = 0; j < kCodecValuesPerWord; ++j) {
                const uint32_t e = exponent(w, j);
                lo = e < lo ? e : lo;
                hi = e > hi ? e : hi;
            }
        uint32_t width = 0;
        while ((1u << width) <= hi - lo) ++width;
        head_.push_back(codec_header(bit_, lo, width));
        for (uint64_t w : pending_) {
            uint32_t sm = 0;
            uint64_t offsets = 0;
            for (uint32_t j = 0; j < kCodecValuesPerWord; ++j) {
                const uint32_t v = (uint32_t)(w >> (16 * j)) & 0xFFFF;
                sm |= (((v >> 8) & 0x80) | (v & 0x7F)) << (8 * j);
                offsets |= (uint64_t)(exponent(w, j) - lo) << (j * width);
            }
            put_bits(offsets, kCodecValuesPerWord * width);
            bytes_.push_back(sm);
        }
        pending_.clear();
    }

    std::vector<uint64_t> pending_, head_, exps_;
    std::vector<uint32_t> bytes_;
    uint64_t              bit_ = 0;
};

} // namespace mc
