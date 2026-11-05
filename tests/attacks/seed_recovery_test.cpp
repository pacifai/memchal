#include <cstdio>
#include "attacks/cpu/seed_recovery.hpp"
#include "shared/fill.hpp"

using namespace mc;

int main() {
    const uint64_t seed = 0xDEAD600DC0FFEEULL;
    uint64_t buf[1];
    fill_splitmix64(buf, 1, seed);
    const uint64_t recovered = splitmix64_recover_seed(buf[0]);
    const bool pass = recovered == seed;
    printf("seed recovery: %s  (recovered=0x%016llX  expected=0x%016llX)\n", pass ? "PASS" : "FAIL",
           (unsigned long long)recovered, (unsigned long long)seed);
    return pass ? 0 : 1;
}
