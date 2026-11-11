#pragma once
#include <chrono>
#include <cmath>
#include <cstdlib>
#include "shared/fill.hpp"
#include "shared/protocol.hpp"
#include "shared/random.hpp"
#include "verifier/args.hpp"

namespace mc {

// The verifier's copy of the buffer it hands the prover.
struct Reference {
    uint64_t* words     = nullptr;
    uint64_t  num_words = 0;
    size_t    bytes     = 0;
    Recipe    recipe;
    double    gen_s     = 0.0;
};

// Sizes and fills the reference buffer as `a` asks. Returns false with a
// message in err on bad parameters or allocation failure.
static inline bool make_reference(const Args& a, Reference& ref, char* err, size_t errlen) {
    ref.bytes = ((size_t)llround(a.mib * 1048576.0) + 7) & ~size_t(7);
    if (a.build()) {
        ref.recipe.memory_kib  = (uint32_t)llround(a.mib * 1024.0);
        ref.recipe.passes      = a.build_passes;
        ref.recipe.parallelism = a.build_parallelism;
        if (!recipe_ok(ref.recipe, err, errlen)) return false;
        if (build_buffer_words(ref.recipe) == 0) {
            snprintf(err, errlen, "%u KiB is too small for parallelism %u",
                     ref.recipe.memory_kib, ref.recipe.parallelism);
            return false;
        }
        ref.bytes = (size_t)build_buffer_bytes(ref.recipe);
        random_bytes(ref.recipe.key, sizeof ref.recipe.key);
        random_bytes(ref.recipe.salt, sizeof ref.recipe.salt);
    }
    ref.num_words = ref.bytes / 8;
    ref.words = (uint64_t*)malloc(ref.bytes);
    if (!ref.words) { snprintf(err, errlen, "cannot allocate %zu bytes", ref.bytes); return false; }

    const auto t0 = std::chrono::steady_clock::now();
    switch (a.gen) {
        case Generator::Argon2d:
            if (!build_buffer(ref.words, ref.num_words, ref.recipe)) {
                snprintf(err, errlen, "build failed");
                return false;
            }
            break;
        case Generator::ChaCha: {
            uint8_t key[32];
            random_bytes(key, sizeof key);
            fill_chacha(ref.words, ref.num_words, key);
            break;
        }
        case Generator::Splitmix64:
            fill_splitmix64(ref.words, ref.num_words, a.seed);
            break;
    }
    ref.gen_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace mc
