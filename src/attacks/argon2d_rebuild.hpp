#pragma once
#include "shared/argon2d.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

namespace mc {

struct MatrixShape {
    uint32_t lanes = 0, lane_len = 0, seg_len = 0, passes = 0, blocks = 0;
    uint8_t  h0[a2::kPrehashLen] = {};
};

static inline bool matrix_shape(const Recipe& r, MatrixShape& s) {
    if (r.parallelism == 0 || r.passes == 0 || r.memory_kib < 8 * r.parallelism) return false;
    s.lanes    = r.parallelism;
    s.passes   = r.passes;
    s.blocks   = a2::argon2d_blocks(r.memory_kib, r.parallelism);
    s.lane_len = s.blocks / r.parallelism;
    s.seg_len  = s.lane_len / a2::kSyncPoints;
    if (s.seg_len < 2) return false;
    a2::initial_hash(s.h0, argon2_params(r), 32);
    return true;
}

// Final-pass values of the blocks a prover kept. slot[block] is the block's
// index in `words`, or -1 if it was discarded.
struct KeptBlocks {
    const int32_t*  slot  = nullptr;
    const uint64_t* words = nullptr;
    const uint64_t* get(uint32_t block) const {
        const int32_t s = slot[block];
        return s < 0 ? nullptr : words + (size_t)s * a2::kBlockWords;
    }
};

// Rebuilds discarded blocks of an Argon2d matrix from the kept ones, following
// each block's dependencies back to blocks it can read. Rebuilt values are
// remembered until begin_challenge(), and each challenge may spend at most
// `budget` block fills. The GPU recompute attack uses it to keep only part of a
// build-mode buffer and rebuild each missing block a challenge probes, which
// measures how much the Argon2d dependencies cost a prover that drops blocks.
class Rebuilder {
public:
    struct Stats {
        uint64_t fills     = 0;
        uint64_t seeds     = 0;
        uint64_t gave_up   = 0;
        uint64_t memo_peak = 0;
    };

    Rebuilder(const MatrixShape& s, KeptBlocks kept, uint64_t budget)
        : s_(s), kept_(kept), budget_(budget) {}

    void begin_challenge() {
        memo_.clear();
        arena_.clear();
        spent_ = 0;
    }

    // Writes the final value of `block` to out. Returns false if the budget
    // runs out first.
    bool rebuild(uint32_t block, uint64_t* out) {
        const uint64_t* v = value(block, s_.passes - 1);
        if (!v) { ++stats_.gave_up; return false; }
        memcpy(out, v, a2::kBlockBytes);
        return true;
    }

    const Stats& stats() const { return stats_; }

private:
    using Block = std::array<uint64_t, a2::kBlockWords>;

    static uint64_t key(uint32_t block, uint32_t pass) { return ((uint64_t)block << 8) | pass; }

    const uint64_t* remembered(uint32_t block, uint32_t pass) const {
        auto it = memo_.find(key(block, pass));
        return it == memo_.end() ? nullptr : arena_[it->second].data();
    }

    uint64_t* remember(uint32_t block, uint32_t pass) {
        memo_.emplace(key(block, pass), (uint32_t)arena_.size());
        arena_.emplace_back();
        if (arena_.size() > stats_.memo_peak) stats_.memo_peak = arena_.size();
        return arena_.back().data();
    }

    // The value of a block after a pass if it is kept, remembered, or a seed block.
    const uint64_t* known(uint32_t block, uint32_t pass) {
        if (pass == s_.passes - 1) {
            if (const uint64_t* k = kept_.get(block)) return k;
        }
        if (const uint64_t* m = remembered(block, pass)) return m;
        const uint32_t lane = block / s_.lane_len, col = block % s_.lane_len;
        if (pass == 0 && col < 2) {
            uint8_t seed[a2::kPrehashSeed], bytes[a2::kBlockBytes];
            memcpy(seed, s_.h0, a2::kPrehashLen);
            a2::store32(seed + a2::kPrehashLen, col);
            a2::store32(seed + a2::kPrehashLen + 4, lane);
            a2::blake2b_long(bytes, a2::kBlockBytes, seed, a2::kPrehashSeed);
            uint64_t* dst = remember(block, pass);
            for (size_t i = 0; i < a2::kBlockWords; ++i) dst[i] = a2::load64(bytes + i * 8);
            ++stats_.seeds;
            return dst;
        }
        return nullptr;
    }

    // A block as it stands after a given pass.
    struct BlockAtPass { uint32_t block, pass; };

    // The value of a block after a pass, rebuilding whatever it depends on.
    const uint64_t* value(uint32_t block0, uint32_t pass0) {
        if (const uint64_t* v = known(block0, pass0)) return v;
        std::vector<BlockAtPass> stack{{block0, pass0}};
        while (!stack.empty()) {
            const BlockAtPass n = stack.back();
            if (known(n.block, n.pass)) { stack.pop_back(); continue; }

            const uint32_t lane = n.block / s_.lane_len, col = n.block % s_.lane_len;
            const uint32_t slice = col / s_.seg_len, index = col % s_.seg_len;

            const BlockAtPass prev = (col == 0)
                ? BlockAtPass{lane * s_.lane_len + s_.lane_len - 1, n.pass - 1}
                : BlockAtPass{n.block - 1, n.pass};
            const uint64_t* pv = known(prev.block, prev.pass);
            if (!pv) { stack.push_back(prev); continue; }

            const uint64_t pseudo = pv[0];
            uint32_t ref_lane = (uint32_t)(pseudo >> 32) % s_.lanes;
            if (n.pass == 0 && slice == 0) ref_lane = lane;
            const uint32_t ref_col = a2::index_alpha(n.pass, slice, index, s_.seg_len,
                                                     s_.lane_len,
                                                     (uint32_t)(pseudo & 0xFFFFFFFFULL),
                                                     ref_lane == lane);
            const uint32_t ref_slice = ref_col / s_.seg_len;
            const bool rewritten = ref_slice < slice ||
                                   (ref_slice == slice && ref_lane == lane && ref_col < col);
            const BlockAtPass ref{ref_lane * s_.lane_len + ref_col,
                                  rewritten ? n.pass : n.pass - 1};
            const uint64_t* rv = known(ref.block, ref.pass);
            if (!rv) { stack.push_back(ref); continue; }

            const uint64_t* ov = nullptr;
            if (n.pass > 0) {
                ov = known(n.block, n.pass - 1);
                if (!ov) { stack.push_back(BlockAtPass{n.block, n.pass - 1}); continue; }
            }

            if (spent_ >= budget_) return nullptr;
            ++spent_;
            ++stats_.fills;
            uint64_t* dst = remember(n.block, n.pass);
            if (ov) memcpy(dst, ov, a2::kBlockBytes);
            a2::fill_block(pv, rv, dst, n.pass != 0);
            stack.pop_back();
        }
        return known(block0, pass0);
    }

    MatrixShape s_;
    KeptBlocks  kept_;
    uint64_t    budget_;
    uint64_t    spent_ = 0;
    Stats       stats_;
    std::unordered_map<uint64_t, uint32_t> memo_;
    std::deque<Block> arena_;
};

} // namespace mc
