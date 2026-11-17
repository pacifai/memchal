#pragma once
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "shared/fill.hpp"
#include "shared/protocol.hpp"
#include "shared/random.hpp"
#include "shared/weights.hpp"
#include "verifier/args.hpp"

namespace mc {

// The verifier's copy of the buffer: the model weights, if any, then the filler.
struct Reference {
    uint64_t* words        = nullptr;
    uint64_t  num_words    = 0;
    size_t    bytes        = 0;
    Manifest  weights;
    size_t    weight_bytes = 0;
    Recipe    recipe;
    double    gen_s        = 0.0;

    bool      has_weights() const { return weight_bytes > 0; }
    uint64_t* filler() const { return words + weight_bytes / 8; }
    size_t    filler_bytes() const { return bytes - weight_bytes; }
};

namespace detail {

// Sizes the filler, and in build mode sets the recipe, tied to the weights.
static inline bool size_filler(const Args& a, Reference& ref, size_t* filler_bytes,
                               char* err, size_t errlen) {
    *filler_bytes = ((size_t)llround(a.mib * 1048576.0) + 7) & ~size_t(7);
    if (!a.build()) return true;
    ref.recipe.memory_kib  = (uint32_t)llround(a.mib * 1024.0);
    ref.recipe.passes      = a.build_passes;
    ref.recipe.parallelism = a.build_parallelism;
    if (!recipe_ok(ref.recipe, err, errlen)) return false;
    if (build_buffer_words(ref.recipe) == 0) {
        snprintf(err, errlen, "%u KiB is too small for parallelism %u",
                 ref.recipe.memory_kib, ref.recipe.parallelism);
        return false;
    }
    *filler_bytes = (size_t)build_buffer_bytes(ref.recipe);
    random_bytes(ref.recipe.key, sizeof ref.recipe.key);
    random_bytes(ref.recipe.salt, sizeof ref.recipe.salt);
    if (ref.has_weights())
        memcpy(ref.recipe.workload_tag, ref.weights.digest, sizeof ref.recipe.workload_tag);
    return true;
}

static inline bool fill_filler(const Args& a, Reference& ref, char* err, size_t errlen) {
    const uint64_t words = ref.filler_bytes() / 8;
    if (!words) return true;
    switch (a.gen) {
        case Generator::Argon2d:
            if (build_buffer(ref.filler(), words, ref.recipe)) return true;
            snprintf(err, errlen, "build failed");
            return false;
        case Generator::ChaCha: {
            uint8_t key[32];
            random_bytes(key, sizeof key);
            fill_chacha(ref.filler(), words, key);
            return true;
        }
        case Generator::Splitmix64:
            fill_splitmix64(ref.filler(), words, a.seed);
            return true;
    }
    return false;
}

} // namespace detail

// Builds the reference buffer as `a` asks. Returns false with a message in err
// on bad parameters, unreadable weights, or allocation failure.
static inline bool make_reference(const Args& a, Reference& ref, char* err, size_t errlen) {
    if (a.weights) {
        if (!read_manifest(a.weights, ref.weights, err, errlen)) return false;
        ref.weight_bytes = ref.weights.bytes;
    }
    if (a.build() && a.mib <= 0) { snprintf(err, errlen, "--gen argon2d builds filler: set --mb above 0"); return false; }
    size_t filler_bytes = 0;
    if (a.mib > 0 && !detail::size_filler(a, ref, &filler_bytes, err, errlen)) return false;
    ref.bytes = ref.weight_bytes + filler_bytes;
    if (ref.bytes == 0) { snprintf(err, errlen, "the buffer is empty: set --mb, --weights, or both"); return false; }
    ref.num_words = ref.bytes / 8;
    ref.words = (uint64_t*)malloc(ref.bytes);
    if (!ref.words) { snprintf(err, errlen, "cannot allocate %zu bytes", ref.bytes); return false; }

    const auto t0 = std::chrono::steady_clock::now();
    WeightsReader reader;
    if (ref.has_weights() && (!reader.open(ref.weights, err, errlen) ||
                              !reader.read(ref.words, ref.weight_bytes))) {
        snprintf(err, errlen, "cannot read the weights %s lists", a.weights);
        return false;
    }
    if (!detail::fill_filler(a, ref, err, errlen)) return false;
    ref.gen_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace mc
