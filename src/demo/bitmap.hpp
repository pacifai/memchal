#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mc {
namespace demo {

// The low bit of each of `count` flags as a base64 bitmap, least significant
// bit first within each byte.
static inline std::string base64_bitmap(const uint8_t* flags, size_t count) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t nbytes = (count + 7) / 8;
    auto byte_at = [&](size_t b) {
        uint8_t v = 0;
        for (size_t i = b * 8; i < b * 8 + 8 && i < count; ++i)
            if (flags[i] & 1u) v |= (uint8_t)(1u << (i & 7));
        return (uint32_t)v;
    };
    std::string out;
    out.reserve((nbytes + 2) / 3 * 4);
    for (size_t i = 0; i < nbytes; i += 3) {
        uint32_t v = byte_at(i) << 16;
        if (i + 1 < nbytes) v |= byte_at(i + 1) << 8;
        if (i + 2 < nbytes) v |= byte_at(i + 2);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < nbytes ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < nbytes ? kAlphabet[v & 63] : '=';
    }
    return out;
}

} // namespace demo
} // namespace mc
