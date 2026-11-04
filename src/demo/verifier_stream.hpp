#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include "demo/bitmap.hpp"
#include "shared/json.hpp"

namespace mc {
namespace demo {

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

// The verifier's event stream for the browser demo: one '@'-prefixed JSON line
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
        fprintf(f_, "@{\"t\":\"run\",\"label\":");
        json_str(f_, label);
        fprintf(f_, ",\"mb\":%.6g,\"samples\":%u,\"lanes\":%u,\"challenges\":%u,\"warmup\":%u,"
                    "\"pages\":%llu,\"page_bytes\":%ld,\"locked\":%s,\"gen\":\"%s\","
                    "\"gen_s\":%.4f,\"upload_s\":%.4f,\"map_every\":%u}\n",
                mib, samples, lanes, challenges, warmup, (unsigned long long)num_pages_,
                page_bytes_, locked ? "true" : "false", gen, gen_s, upload_s, map_every_);
    }

    void challenge(uint64_t seq, const char* phase, double us, uint64_t nonce, uint64_t hash,
                   uint64_t expect, bool ok, bool checked, bool with_map) {
        fprintf(f_, "@{\"t\":\"c\",\"i\":%llu,\"ph\":\"%s\",\"us\":%.3f,"
                    "\"nonce\":\"0x%016llx\",\"hash\":\"0x%016llx\",\"exp\":\"0x%016llx\","
                    "\"ok\":%s,\"checked\":%s",
                (unsigned long long)seq, phase, us, (unsigned long long)nonce,
                (unsigned long long)hash, (unsigned long long)expect,
                ok ? "true" : "false", checked ? "true" : "false");
        if (with_map) {
            fputs(",\"map\":\"", f_);
            base64_bitmap(f_, pages_.data(), pages_.size());
            fputc('"', f_);
        }
        fputs("}\n", f_);
    }

    void end(size_t challenges, uint64_t errors, uint64_t checked, double p50_us, double p99_us,
             double min_us, double max_us, double mean_us) {
        fprintf(f_, "@{\"t\":\"end\",\"challenges\":%zu,\"errors\":%llu,\"checked\":%llu,"
                    "\"p50\":%.3f,\"p99\":%.3f,\"min\":%.3f,\"max\":%.3f,\"mean\":%.3f}\n",
                challenges, (unsigned long long)errors, (unsigned long long)checked,
                p50_us, p99_us, min_us, max_us, mean_us);
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

} // namespace demo
} // namespace mc
