#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mc {
namespace demo {

// Writes the low bit of each of `count` flags as a base64 bitmap, least
// significant bit first within each byte.
static inline void base64_bitmap(FILE* f, const uint8_t* flags, size_t count) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t nbytes = (count + 7) / 8;
    auto byte_at = [&](size_t b) {
        uint8_t v = 0;
        for (size_t i = b * 8; i < b * 8 + 8 && i < count; ++i)
            if (flags[i] & 1u) v |= (uint8_t)(1u << (i & 7));
        return (uint32_t)v;
    };
    for (size_t i = 0; i < nbytes; i += 3) {
        uint32_t v = byte_at(i) << 16;
        if (i + 1 < nbytes) v |= byte_at(i + 1) << 8;
        if (i + 2 < nbytes) v |= byte_at(i + 2);
        fputc(kAlphabet[(v >> 18) & 63], f);
        fputc(kAlphabet[(v >> 12) & 63], f);
        fputc(i + 1 < nbytes ? kAlphabet[(v >> 6) & 63] : '=', f);
        fputc(i + 2 < nbytes ? kAlphabet[v & 63] : '=', f);
    }
}

} // namespace demo
} // namespace mc
