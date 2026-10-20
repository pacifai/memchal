// segments.hpp — a challenge buffer made of separate pieces of memory.
//
// The challenge math (challenge.hpp) reads word `idx` of one flat buffer. An
// inference engine holds a model's weights as many separate tensors, each its
// own allocation, and the reason to challenge the weights is to read them where
// the engine keeps them rather than in a copy made for the occasion. A Segments
// table lets the frozen math do that unchanged: the buffer is *defined* as the
// pieces laid end to end in a fixed order, and SegLoad turns a word index into
// a piece and an offset. The answer over the pieces is therefore the answer
// over their concatenation, word for word (tests/segments_test.cpp pins this).
//
// The lookup is a directory, not a search. The buffer is cut into chunks of
// 2^shift words, and dir[c] names the piece holding the chunk's first word.
// When every piece is at least one chunk long, a chunk crosses at most one
// boundary, so a lookup is one directory read and at most one step forward.
// Shorter pieces cost a few more steps; the answer is the same either way.
//
// Plain integers and pointers, no allocation, no POSIX, so it compiles for
// host and device like challenge.hpp. Who owns the arrays, and which side of
// the bus they live on, is the caller's business.
#pragma once
#include <cstdint>
#include "challenge.hpp"

namespace mc {

struct Segments {
    const uint64_t* const* base;  // base[s]: the first word of piece s
    const uint64_t* start;        // start[s]: buffer index of piece s's first word;
                                  // start[count] is the buffer's length in words
    const uint32_t* dir;          // dir[c]: the piece holding word (c << shift)
    uint32_t count;
    uint32_t shift;
};

// The loader answer_lane_t() calls. Indices come from map_index(), so they are
// always below start[count], and the walk below never runs off the table.
struct SegLoad {
    Segments t;
    MC_HOST_DEVICE uint64_t operator()(uint64_t idx) const {
        uint32_t s = t.dir[idx >> t.shift];
        while (idx >= t.start[s + 1]) ++s;
        return t.base[s][idx - t.start[s]];
    }
};

// The directory's chunk size, as a power of two: the largest chunk no longer
// than the shortest non-empty piece, so a lookup takes at most one step, then
// coarser until the directory has at most `max_dir` entries. The total must be
// at least one word.
static inline uint32_t segments_shift(const uint64_t* words, uint32_t count, uint64_t max_dir) {
    uint64_t total = 0, shortest = ~0ULL;
    for (uint32_t s = 0; s < count; ++s) {
        total += words[s];
        if (words[s] && words[s] < shortest) shortest = words[s];
    }
    if (max_dir == 0) max_dir = 1;
    uint32_t shift = 0;
    while (shift < 63 && (2ULL << shift) <= shortest) ++shift;
    while (shift < 63 && ((total - 1) >> shift) + 1 > max_dir) ++shift;
    return shift;
}

static inline uint64_t segments_dir_len(uint64_t total_words, uint32_t shift) {
    return ((total_words - 1) >> shift) + 1;
}

// Fill start[0..count] and dir[0..segments_dir_len) for pieces of the given
// lengths, in the order given. The caller sizes both arrays.
static inline void segments_fill(const uint64_t* words, uint32_t count, uint32_t shift,
                                 uint64_t* start, uint32_t* dir) {
    start[0] = 0;
    for (uint32_t s = 0; s < count; ++s) start[s + 1] = start[s] + words[s];
    uint64_t n = segments_dir_len(start[count], shift);
    uint32_t s = 0;
    for (uint64_t c = 0; c < n; ++c) {
        uint64_t w = c << shift;
        while (w >= start[s + 1]) ++s;
        dir[c] = s;
    }
}

} // namespace mc
