#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "attacks/weights_codec.hpp"

using namespace mc;

static uint16_t bf16(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return (uint16_t)(u >> 16);
}

static std::vector<uint64_t> weight_like(uint64_t words, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> normal(0.0f, 0.02f);
    std::vector<uint64_t> out(words);
    for (uint64_t& w : out)
        for (uint32_t j = 0; j < kCodecValuesPerWord; ++j) w |= (uint64_t)bf16(normal(rng)) << (16 * j);
    return out;
}

static std::vector<uint64_t> random_words(uint64_t words, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> out(words);
    for (uint64_t& w : out) w = rng();
    return out;
}

// Appends in uneven pieces, then checks every word and returns the size ratio.
static bool round_trip(const char* name, const std::vector<uint64_t>& in, double* ratio) {
    CodecWriter w;
    w.reserve(in.size());
    for (uint64_t at = 0, step = 1; at < in.size(); at += step, step = step * 3 % 1000 + 1)
        w.append(in.data() + at, std::min<uint64_t>(step, in.size() - at));
    if (w.words() != in.size()) {
        printf("FAIL %s: words() is %llu before finish(), want %llu\n", name, (unsigned long long)w.words(),
               (unsigned long long)in.size());
        return false;
    }
    w.finish();
    const CodecView v = w.view();
    for (uint64_t i = 0; i < in.size(); ++i) {
        if (v(i) != in[i]) {
            printf("FAIL %s: word %llu decodes to %016llx, want %016llx\n", name, (unsigned long long)i,
                   (unsigned long long)v(i), (unsigned long long)in[i]);
            return false;
        }
    }
    *ratio = (double)w.compressed_bytes() / (double)(in.size() * 8);
    printf("ok   %s: %llu words, %.1f%% of the original size\n", name, (unsigned long long)in.size(),
           100.0 * *ratio);
    return true;
}

int main() {
    constexpr uint64_t kWords = kCodecBlockWords * 4096 + 37;
    constexpr double kMaxWeightRatio = 0.85;
    double weights = 0, random = 0;
    bool ok = round_trip("bf16 weights", weight_like(kWords, 1), &weights) &&
              round_trip("random words", random_words(kWords, 2), &random) &&
              round_trip("one short block", weight_like(5, 3), &random);
    if (ok && weights > kMaxWeightRatio) {
        printf("FAIL bf16 weights compress to %.1f%%, above %.0f%%\n", 100 * weights, 100 * kMaxWeightRatio);
        ok = false;
    }
    return ok ? 0 : 1;
}
