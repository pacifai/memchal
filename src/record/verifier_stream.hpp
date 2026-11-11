#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include "record/bitmap.hpp"
#include "record/event.hpp"

namespace mc {
namespace record {

// A loader that marks the page of every word it reads.
struct PageRecorder {
    const uint64_t* buf;
    uint8_t*        pages;
    uint64_t        page_words;
    uint64_t operator()(uint64_t idx) const {
        pages[idx / page_words] = 1;
        return buf[idx];
    }
};

// The verifier's event stream for a session file: one '@'-prefixed JSON line
// per event, with a bitmap of the probed pages every `map_every` challenges.
class VerifierStream {
public:
    bool open(const char* path, uint32_t map_every, uint64_t num_words) {
        f_ = !strcmp(path, "-") ? stdout : fopen(path, "w");
        if (!f_) return false;
        setvbuf(f_, nullptr, _IOLBF, 0);
        long page_bytes = sysconf(_SC_PAGESIZE);
        page_bytes_ = page_bytes > 0 ? page_bytes : 4096;
        page_words_ = (uint64_t)page_bytes_ / 8;
        num_pages_ = (num_words + page_words_ - 1) / page_words_;
        map_every_ = map_every;
        if (map_every_) pages_.assign(num_pages_, 0);
        return true;
    }

    bool active() const { return f_ != nullptr; }
    bool wants_map(uint64_t seq) const { return f_ && map_every_ && seq % map_every_ == 0; }

    // A loader over buf that records the pages it reads, after clearing the map.
    PageRecorder recorder(const uint64_t* buf) {
        std::fill(pages_.begin(), pages_.end(), (uint8_t)0);
        return PageRecorder{buf, pages_.data(), page_words_};
    }

    void run(const char* label, double mib, uint32_t samples, uint32_t lanes, uint32_t challenges,
             uint32_t warmup, bool locked, const char* gen, double gen_s, double upload_s) {
        Event(f_, "run")
            .str("label", label)
            .num("mb", mib, "%.6g")
            .uint("samples", samples)
            .uint("lanes", lanes)
            .uint("challenges", challenges)
            .uint("warmup", warmup)
            .uint("pages", num_pages_)
            .integer("page_bytes", page_bytes_)
            .boolean("locked", locked)
            .str("gen", gen)
            .num("gen_s", gen_s)
            .num("upload_s", upload_s)
            .uint("map_every", map_every_);
    }

    void challenge(uint64_t seq, const char* phase, double us, uint64_t nonce, uint64_t hash,
                   uint64_t expect, bool ok, bool checked, bool with_map) {
        Event e(f_, "c");
        e.uint("i", seq)
            .str("ph", phase)
            .num("us", us, "%.3f")
            .hex("nonce", nonce)
            .hex("hash", hash)
            .hex("exp", expect)
            .boolean("ok", ok)
            .boolean("checked", checked);
        if (with_map) e.str("map", base64_bitmap(pages_.data(), pages_.size()).c_str());
    }

    void end(size_t challenges, uint64_t errors, uint64_t checked, double p50_us, double p99_us,
             double min_us, double max_us, double mean_us) {
        Event(f_, "end")
            .uint("challenges", challenges)
            .uint("errors", errors)
            .uint("checked", checked)
            .num("p50", p50_us, "%.3f")
            .num("p99", p99_us, "%.3f")
            .num("min", min_us, "%.3f")
            .num("max", max_us, "%.3f")
            .num("mean", mean_us, "%.3f");
    }

    void close() {
        if (f_ && f_ != stdout) fclose(f_);
        f_ = nullptr;
    }

private:
    FILE*                f_          = nullptr;
    uint32_t             map_every_  = 0;
    long                 page_bytes_ = 4096;
    uint64_t             page_words_ = 512;
    uint64_t             num_pages_  = 0;
    std::vector<uint8_t> pages_;
};

} // namespace record
} // namespace mc
