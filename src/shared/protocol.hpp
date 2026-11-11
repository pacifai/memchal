#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "shared/argon2d.hpp"
#include "shared/challenge.hpp"

namespace mc {

constexpr uint64_t kMagic       = 0x4D454D4348414C31ULL;
constexpr uint16_t kDefaultPort = 9099;
constexpr uint32_t kWireVersion = 2u;

constexpr uint32_t kChallengeTerminate = 1u;
constexpr uint32_t kChallengeChained   = 2u;
constexpr uint32_t kChallengeTreeFold  = 4u;
constexpr uint32_t kHelloBuild         = 4u;

constexpr uint64_t kMaxBufferBytes = 1ULL << 40;
constexpr uint32_t kMaxLanes       = 1u << 24;
constexpr uint32_t kMaxSamples     = 1u << 24;
constexpr uint32_t kMaxBuildPasses = 255;
constexpr uint32_t kMaxBuildLanes  = 256;

// Seconds a prover waits for the verifier's next message before ending the session.
constexpr int kIdleTimeoutS = 60;

#pragma pack(push, 1)
struct Hello     { uint64_t magic; uint64_t buffer_bytes; uint32_t version; uint32_t flags; };
struct Ready     { uint64_t magic; uint32_t locked; uint32_t pad; };
struct Challenge { uint64_t nonce; uint32_t samples; uint32_t lanes; uint32_t flags; uint32_t pad; };
struct Response  { uint64_t hash; };
struct RecipeMsg {
    uint8_t  key[32];
    uint8_t  salt[16];
    uint32_t memory_kib;
    uint32_t passes;
    uint32_t parallelism;
    uint8_t  workload_tag[32];
};
#pragma pack(pop)
static_assert(sizeof(RecipeMsg) == 92, "RecipeMsg is a fixed 92-byte wire layout");

static inline Recipe recipe_from_msg(const RecipeMsg& m) {
    Recipe r;
    memcpy(r.key, m.key, sizeof r.key);
    memcpy(r.salt, m.salt, sizeof r.salt);
    memcpy(r.workload_tag, m.workload_tag, sizeof r.workload_tag);
    r.memory_kib = m.memory_kib; r.passes = m.passes; r.parallelism = m.parallelism;
    return r;
}

static inline RecipeMsg msg_from_recipe(const Recipe& r) {
    RecipeMsg m{};
    memcpy(m.key, r.key, sizeof m.key);
    memcpy(m.salt, r.salt, sizeof m.salt);
    memcpy(m.workload_tag, r.workload_tag, sizeof m.workload_tag);
    m.memory_kib = r.memory_kib; m.passes = r.passes; m.parallelism = r.parallelism;
    return m;
}

static inline bool hello_ok(const Hello& h, char* err, size_t n) {
    if (h.magic != kMagic) { snprintf(err, n, "bad magic"); return false; }
    if (h.version < 1 || h.version > kWireVersion) {
        snprintf(err, n, "unsupported wire version %u (want 1 to %u)", h.version, kWireVersion);
        return false;
    }
    if (h.buffer_bytes < 8 || h.buffer_bytes > kMaxBufferBytes) {
        snprintf(err, n, "buffer size %llu is outside 8 bytes to %llu GiB",
                 (unsigned long long)h.buffer_bytes, (unsigned long long)(kMaxBufferBytes >> 30));
        return false;
    }
    if (h.flags & ~kHelloBuild) { snprintf(err, n, "unknown hello flags 0x%x", h.flags); return false; }
    return true;
}

static inline bool recipe_ok(const Recipe& r, char* err, size_t n) {
    if (r.passes < 1 || r.passes > kMaxBuildPasses) {
        snprintf(err, n, "recipe passes %u is outside 1 to %u", r.passes, kMaxBuildPasses);
        return false;
    }
    if (r.parallelism < 1 || r.parallelism > kMaxBuildLanes) {
        snprintf(err, n, "recipe parallelism %u is outside 1 to %u", r.parallelism, kMaxBuildLanes);
        return false;
    }
    return true;
}

static inline bool challenge_ok(const Challenge& c, char* err, size_t n) {
    if (c.lanes < 1 || c.lanes > kMaxLanes) {
        snprintf(err, n, "challenge lanes %u is outside 1 to %u", c.lanes, kMaxLanes);
        return false;
    }
    if (c.samples > kMaxSamples) {
        snprintf(err, n, "challenge samples %u is above %u", c.samples, kMaxSamples);
        return false;
    }
    if (c.flags & ~(kChallengeTerminate | kChallengeChained | kChallengeTreeFold)) {
        snprintf(err, n, "unknown challenge flags 0x%x", c.flags);
        return false;
    }
    return true;
}

static inline bool is_chained(const Challenge& ch) { return (ch.flags & kChallengeChained) != 0; }
static inline bool is_tree(const Challenge& ch)    { return (ch.flags & kChallengeTreeFold) != 0; }

// Folds lane hashes into the challenge's answer. The tree fold overwrites `v`.
static inline uint64_t fold_lanes(const Challenge& ch, uint64_t* v) {
    return is_tree(ch) ? tree_fold(v, ch.lanes, ch.nonce, ch.samples)
                       : sequential_fold(v, ch.lanes, ch.nonce, ch.samples);
}

// The answer to `ch`, reading words through `load`.
//   scratch: space for the tree fold's lane hashes; grown as needed.
template <typename Load>
static inline uint64_t answer(Load&& load, uint64_t num_words, const Challenge& ch,
                              std::vector<uint64_t>& scratch) {
    if (!is_tree(ch))
        return answer_sequential(load, num_words, ch.nonce, ch.samples, ch.lanes, is_chained(ch));
    if (scratch.size() < ch.lanes) scratch.resize(ch.lanes);
    return answer_tree(load, num_words, ch.nonce, ch.samples, ch.lanes, is_chained(ch),
                       scratch.data());
}

} // namespace mc
