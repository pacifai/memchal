#pragma once
#include <cstdint>

// Known answers for a 1 MiB buffer filled by fill_splitmix64() with kVectorSeed.
// The last two vectors give some lanes one probe more than others, and the last
// has an odd lane count, so the tree fold pads a level.
namespace mc::kat {

static const uint64_t kVectorSeed = 0xABCDEF0123456789ULL;

struct Vector { uint64_t nonce; uint32_t samples; uint32_t lanes; };
static const Vector kVectors[] = {
    { 0x0000000000000001ULL, 1,     1  },
    { 0x1234567890ABCDEFULL, 4096,  32 },
    { 0xFFFFFFFFFFFFFFFFULL, 65536, 64 },
    { 0x0F1E2D3C4B5A6978ULL, 4097,  32 },
    { 0x8877665544332211ULL, 1000,  7  },
};
static const int kVectorCount = sizeof kVectors / sizeof kVectors[0];

static const uint64_t kSequential[kVectorCount] = {
    0xDB349A095B054DC1ULL, 0xCA0AC4E4D3BAE406ULL, 0x404EF3CD1DDBD68CULL,
    0xDCBB4016013347C5ULL, 0xE150C4FF50A9734FULL };
static const uint64_t kSequentialChained[kVectorCount] = {
    0x9223B322843E6ED1ULL, 0xB48B8C33BFD2C50EULL, 0x6889B0F93AE541F3ULL,
    0xC01CE1E3D51B0927ULL, 0xE94CE9393CEE39CEULL };
static const uint64_t kTree[kVectorCount] = {
    0xDB349A095B054DC1ULL, 0x3D7231F696115A72ULL, 0x75D36F59F81165F0ULL,
    0x21362B5105834C10ULL, 0x1E2160A066398376ULL };
static const uint64_t kTreeChained[kVectorCount] = {
    0x9223B322843E6ED1ULL, 0x6CB30B91F44FF207ULL, 0x86106229AE2DDC5BULL,
    0x2FF747B43C5F25EFULL, 0x419A4DDA1B6B9DDFULL };

static inline const uint64_t* expected(bool tree, bool chained) {
    return tree ? (chained ? kTreeChained : kTree) : (chained ? kSequentialChained : kSequential);
}

} // namespace mc::kat
