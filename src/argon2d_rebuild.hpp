// argon2d_rebuild.hpp — rebuild one block of a finished Argon2d matrix from a
// partial copy of it. This is the partial-recompute adversary's engine
// (ADR 0007): the cheat keeps a fraction of the final blocks, and when a probe
// lands on a missing one, it walks Argon2d's dependency graph and recomputes
// whatever that block needs.
//
// Host only. It calls the frozen primitives in argon2d.hpp and changes none of
// them. tests/rebuild_argon2d.cpp checks every rebuilt block against a full
// build_buffer() of the same recipe.
//
// What a block depends on. Write v(b, P) for block b's value after pass P
// writes it. Computing v(b, P) for b = (lane, col) needs three earlier values:
//
//   prev  — (lane, col-1) at pass P, or for col 0 the lane's last block at
//           pass P-1. Its first word picks the reference, so the reference is
//           unknown until prev exists: this is Argon2d's data-dependent
//           addressing, and the reason the rebuild cascades.
//   ref   — the block index_alpha() picks, at pass P if pass P already rewrote
//           it, else at pass P-1.
//   old   — v(b, P-1), because Argon2 v1.3 XORs each pass into the last. Pass 0
//           has none.
//
// The cheat keeps only final values, v(b, t-1). So for t >= 2, every missing
// block needs its own pass t-2 value, which nobody kept, and that value's
// ancestry is the earlier passes of the whole matrix. With t = 1 the cascade
// stops at the nearest kept blocks. The two are expected to behave very
// differently, and that difference is what the sweep measures.
//
// Recomputed values are memoized for one challenge, then dropped: keeping them
// would be holding the memory the cheat set out to save. The memo is still
// transient memory the cheat needs while it answers, so it is reported.
//
// A budget caps the fill_block() calls per challenge. A real cheat is capped
// too, by the verifier's stopwatch, and a probe that needs thousands of
// rebuilds is caught whether or not it finishes. When the budget runs out the
// rebuild gives up and the caller fills in a wrong block, so the answer is
// wrong as well as late.
#pragma once
#include "argon2d.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

namespace mc {

// The fixed facts about a recipe's matrix that every rebuild needs.
struct MatrixShape {
    uint32_t lanes = 0, lane_len = 0, seg_len = 0, passes = 0, blocks = 0;
    uint8_t  h0[a2::kPrehashLen] = {};
};

static inline bool matrix_shape(const Recipe& r, MatrixShape& s) {
    if (r.p == 0 || r.t == 0 || r.m_kib < 8 * r.p) return false;
    s.lanes    = r.p;
    s.passes   = r.t;
    s.blocks   = a2::argon2d_blocks(r.m_kib, r.p);
    s.lane_len = s.blocks / r.p;
    s.seg_len  = s.lane_len / a2::kSyncPoints;
    if (s.seg_len < 2) return false;
    // The same parameter binding build_buffer() uses, so H0 matches.
    a2::Params prm;
    prm.pwd = r.key;          prm.pwdlen  = sizeof(r.key);
    prm.salt = r.salt;        prm.saltlen = sizeof(r.salt);
    prm.ad = r.workload_tag;  prm.adlen   = sizeof(r.workload_tag);
    prm.m_kib = r.m_kib; prm.t = r.t; prm.p = r.p;
    a2::initial_hash(s.h0, prm, 32);   // build_buffer() hashes with taglen 32
    return true;
}

// The blocks the cheat kept, as final-pass values. slot[b] is the position of
// block b in `words` (kBlockWords words each), or -1 when it was discarded.
struct KeptBlocks {
    const int32_t*  slot  = nullptr;
    const uint64_t* words = nullptr;
    const uint64_t* get(uint32_t b) const {
        const int32_t s = slot[b];
        return s < 0 ? nullptr : words + (size_t)s * a2::kBlockWords;
    }
};

class Rebuilder {
public:
    struct Stats {
        uint64_t fills    = 0;   // fill_block() calls, the unit of rebuild work
        uint64_t seeds    = 0;   // pass-0 seed blocks rehashed from H0
        uint64_t gave_up  = 0;   // requests abandoned at the budget
        uint64_t memo_hi  = 0;   // most blocks memoized at once, for the memory bill
    };

    Rebuilder(const MatrixShape& s, KeptBlocks kept, uint64_t budget)
        : s_(s), kept_(kept), budget_(budget) {}

    // Forget everything recomputed for the previous challenge.
    void begin_challenge() {
        memo_.clear();
        arena_.clear();
        spent_ = 0;
    }

    // Writes the final value of block b to out. Returns false if the budget ran
    // out first; out is then unspecified.
    bool rebuild(uint32_t b, uint64_t* out) {
        const uint64_t* v = value(b, s_.passes - 1);
        if (!v) { ++stats_.gave_up; return false; }
        memcpy(out, v, a2::kBlockBytes);
        return true;
    }

    const Stats& stats() const { return stats_; }

private:
    using Block = std::array<uint64_t, a2::kBlockWords>;

    static uint64_t key(uint32_t b, uint32_t pass) { return ((uint64_t)b << 8) | pass; }

    const uint64_t* remembered(uint32_t b, uint32_t pass) const {
        auto it = memo_.find(key(b, pass));
        return it == memo_.end() ? nullptr : arena_[it->second].data();
    }

    uint64_t* remember(uint32_t b, uint32_t pass) {
        memo_.emplace(key(b, pass), (uint32_t)arena_.size());
        arena_.emplace_back();
        if (arena_.size() > stats_.memo_hi) stats_.memo_hi = arena_.size();
        return arena_.back().data();
    }

    // A value that needs no fill_block(): kept, memoized, or a pass-0 seed.
    const uint64_t* known(uint32_t b, uint32_t pass) {
        if (pass == s_.passes - 1) {
            if (const uint64_t* k = kept_.get(b)) return k;
        }
        if (const uint64_t* m = remembered(b, pass)) return m;
        const uint32_t lane = b / s_.lane_len, col = b % s_.lane_len;
        if (pass == 0 && col < 2) {
            // fill_first_blocks(), for one block.
            uint8_t seed[a2::kPrehashSeed], bytes[a2::kBlockBytes];
            memcpy(seed, s_.h0, a2::kPrehashLen);
            a2::store32(seed + a2::kPrehashLen, col);
            a2::store32(seed + a2::kPrehashLen + 4, lane);
            a2::blake2b_long(bytes, a2::kBlockBytes, seed, a2::kPrehashSeed);
            uint64_t* dst = remember(b, pass);
            for (size_t i = 0; i < a2::kBlockWords; ++i) dst[i] = a2::load64(bytes + i * 8);
            ++stats_.seeds;
            return dst;
        }
        return nullptr;
    }

    struct Need { uint32_t b, pass; };

    // The value of block b after `pass`, rebuilt depth-first on an explicit
    // stack: a t >= 2 cascade can run hundreds of thousands of blocks deep.
    // Each frame is re-examined until all three inputs exist, then filled.
    const uint64_t* value(uint32_t b0, uint32_t pass0) {
        if (const uint64_t* v = known(b0, pass0)) return v;
        std::vector<Need> stack{{b0, pass0}};
        while (!stack.empty()) {
            const Need n = stack.back();
            if (known(n.b, n.pass)) { stack.pop_back(); continue; }

            const uint32_t lane = n.b / s_.lane_len, col = n.b % s_.lane_len;
            const uint32_t slice = col / s_.seg_len, index = col % s_.seg_len;

            // prev, mirroring fill_segment()'s wrap at column 0.
            const Need prev = (col == 0)
                ? Need{lane * s_.lane_len + s_.lane_len - 1, n.pass - 1}
                : Need{n.b - 1, n.pass};
            const uint64_t* pv = known(prev.b, prev.pass);
            if (!pv) { stack.push_back(prev); continue; }

            // ref, exactly as fill_segment() derives it from prev's first word.
            const uint64_t pseudo = pv[0];
            uint32_t ref_lane = (uint32_t)(pseudo >> 32) % s_.lanes;
            if (n.pass == 0 && slice == 0) ref_lane = lane;
            const uint32_t ref_col = a2::index_alpha(n.pass, slice, index, s_.seg_len,
                                                     s_.lane_len,
                                                     (uint32_t)(pseudo & 0xFFFFFFFFULL),
                                                     ref_lane == lane);
            // Pass n.pass has already rewritten the ref if it sits in an
            // earlier slice, or earlier in this lane's own segment. Otherwise
            // the ref still holds its value from the pass before.
            const uint32_t ref_slice = ref_col / s_.seg_len;
            const bool rewritten = ref_slice < slice ||
                                   (ref_slice == slice && ref_lane == lane && ref_col < col);
            const Need ref{ref_lane * s_.lane_len + ref_col,
                           rewritten ? n.pass : n.pass - 1};
            const uint64_t* rv = known(ref.b, ref.pass);
            if (!rv) { stack.push_back(ref); continue; }

            const uint64_t* ov = nullptr;
            if (n.pass > 0) {
                ov = known(n.b, n.pass - 1);
                if (!ov) { stack.push_back(Need{n.b, n.pass - 1}); continue; }
            }

            if (spent_ >= budget_) return nullptr;
            ++spent_;
            ++stats_.fills;
            // The arena is a deque, so remembering a block never moves pv, rv,
            // or ov out from under the fill.
            uint64_t* dst = remember(n.b, n.pass);
            if (ov) memcpy(dst, ov, a2::kBlockBytes);
            a2::fill_block(pv, rv, dst, n.pass != 0);
            stack.pop_back();
        }
        return known(b0, pass0);
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
