#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <vector>
#include <argon2.h>
#include "shared/argon2d.hpp"

using namespace mc;

static int g_failed = 0, g_ran = 0;

static void hex(const char* label, const uint8_t* p, size_t n) {
    printf("  %s ", label);
    for (size_t i = 0; i < n; ++i) printf("%02x", p[i]);
    printf("\n");
}

// Compares our Argon2d tag with libargon2's for one set of inputs.
static void compare(uint32_t memory_kib, uint32_t passes, uint32_t parallelism, uint32_t taglen,
                    const std::vector<uint8_t>& pwd,
                    const std::vector<uint8_t>& salt,
                    const std::vector<uint8_t>& secret,
                    const std::vector<uint8_t>& ad) {
    ++g_ran;
    std::vector<uint8_t> ours(taglen), theirs(taglen);

    a2::Params prm;
    prm.pwd    = pwd.data();    prm.pwdlen    = (uint32_t)pwd.size();
    prm.salt   = salt.data();   prm.saltlen   = (uint32_t)salt.size();
    prm.secret = secret.empty() ? nullptr : secret.data();
    prm.secretlen = (uint32_t)secret.size();
    prm.ad     = ad.empty() ? nullptr : ad.data();
    prm.adlen  = (uint32_t)ad.size();
    prm.memory_kib = memory_kib; prm.passes = passes; prm.parallelism = parallelism;

    const uint32_t blocks = a2::argon2d_blocks(memory_kib, parallelism);
    uint64_t* mem = (uint64_t*)malloc((size_t)blocks * a2::kBlockBytes);
    if (!mem) { printf("allocation failed\n"); ++g_failed; return; }
    const bool ok = a2::argon2d_raw(mem, prm, ours.data(), taglen);
    free(mem);
    if (!ok) {
        printf("FAIL m=%u t=%u p=%u taglen=%u: our implementation rejected valid parameters\n",
               memory_kib, passes, parallelism, taglen);
        ++g_failed; return;
    }

    argon2_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = theirs.data();          ctx.outlen = taglen;
    ctx.pwd = (uint8_t*)pwd.data();   ctx.pwdlen = (uint32_t)pwd.size();
    ctx.salt = (uint8_t*)salt.data(); ctx.saltlen = (uint32_t)salt.size();
    ctx.secret = secret.empty() ? nullptr : (uint8_t*)secret.data();
    ctx.secretlen = (uint32_t)secret.size();
    ctx.ad = ad.empty() ? nullptr : (uint8_t*)ad.data();
    ctx.adlen = (uint32_t)ad.size();
    ctx.t_cost = passes; ctx.m_cost = memory_kib; ctx.lanes = parallelism; ctx.threads = parallelism;
    ctx.version = ARGON2_VERSION_13;
    ctx.flags = ARGON2_DEFAULT_FLAGS;

    const int rc = argon2d_ctx(&ctx);
    if (rc != ARGON2_OK) {
        printf("FAIL m=%u t=%u p=%u taglen=%u: libargon2 said %s\n",
               memory_kib, passes, parallelism, taglen, argon2_error_message(rc));
        ++g_failed; return;
    }

    if (ours != theirs) {
        printf("FAIL m=%u t=%u p=%u taglen=%u pwd=%zu salt=%zu secret=%zu ad=%zu\n",
               memory_kib, passes, parallelism, taglen, pwd.size(), salt.size(), secret.size(), ad.size());
        hex("ours  ", ours.data(), taglen);
        hex("theirs", theirs.data(), taglen);
        ++g_failed;
    }
}

static std::vector<uint8_t> rand_bytes(std::mt19937& rng, size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(rng() & 0xFF);
    return v;
}

int main(int argc, char** argv) {
    const int cases = argc > 1 ? atoi(argv[1]) : 200;
    std::mt19937 rng(0xA2D0);

    printf("=== Differential test: shared/argon2d.hpp vs libargon2 ===\n");

    compare(8,  1, 1, 4,   rand_bytes(rng, 1),  rand_bytes(rng, 8),  {}, {});
    compare(32, 3, 4, 32,  rand_bytes(rng, 32), rand_bytes(rng, 16), rand_bytes(rng, 8), rand_bytes(rng, 12));
    compare(64, 2, 2, 128, rand_bytes(rng, 16), rand_bytes(rng, 16), {}, {});

    for (int i = 0; i < cases; ++i) {
        const uint32_t parallelism = 1 + rng() % 4;
        const uint32_t memory_kib  = 8 * parallelism + rng() % 500;
        const uint32_t passes      = 1 + rng() % 4;
        const uint32_t taglen      = 4 + rng() % 125;
        compare(memory_kib, passes, parallelism, taglen,
                rand_bytes(rng, 1 + rng() % 64),
                rand_bytes(rng, 8 + rng() % 25),
                rand_bytes(rng, rng() % 33),
                rand_bytes(rng, rng() % 33));
    }

    if (g_failed) {
        fprintf(stderr, "\n%d of %d cases FAILED\n",
                g_failed, g_ran);
        return 1;
    }
    printf("All %d cases agree with libargon2\n", g_ran);
    return 0;
}
