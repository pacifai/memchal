#include <cstdio>
#include <vector>
#include "shared/challenge.hpp"
#include "shared/fill.hpp"

using namespace mc;

// Shows what the dependent chain defends against. A prefetching prover runs
// each challenge once with a load that returns 0, to learn which offsets to read
// ahead. With an independent chain that dry run finds every offset; with a
// dependent chain it finds only each lane's first one.

static const uint64_t kNumWords = (1u << 20) / 8;

// The fraction of probes whose offset a dry run predicts.
static double predicted(const uint64_t* buf, uint32_t samples, uint32_t lanes, bool chained) {
    const uint64_t nonce = 0x0DDBA11CAFEF00DULL;
    std::vector<uint64_t> real, guess;
    answer_sequential([&](uint64_t idx) { real.push_back(idx); return buf[idx]; },
                      kNumWords, nonce, samples, lanes, chained);
    answer_sequential([&](uint64_t idx) { guess.push_back(idx); return uint64_t(0); },
                      kNumWords, nonce, samples, lanes, chained);
    size_t hits = 0;
    for (size_t i = 0; i < real.size(); ++i) hits += real[i] == guess[i];
    return (double)hits / (double)real.size();
}

int main() {
    std::vector<uint64_t> buf(kNumWords);
    fill_splitmix64(buf.data(), kNumWords, 0xABCDEF0123456789ULL);
    const uint32_t samples = 4096, lanes = 32;
    const double open = predicted(buf.data(), samples, lanes, false);
    const double chained = predicted(buf.data(), samples, lanes, true);
    const double first_only = (double)lanes / samples;
    printf("independent chain: dry run predicts %.1f%% of offsets\n", 100 * open);
    printf("dependent chain:   dry run predicts %.1f%% of offsets (each lane's first is %.1f%%)\n",
           100 * chained, 100 * first_only);
    const bool ok = open == 1.0 && chained < 2 * first_only;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
