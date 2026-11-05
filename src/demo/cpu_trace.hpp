#pragma once
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include "cpu/proc_stats.hpp"
#include "demo/bitmap.hpp"
#include "shared/protocol.hpp"

namespace mc {
namespace demo {

// A CPU prover's event stream for the browser demo: every `every` challenges,
// one '@'-prefixed JSON line with its I/O counters and, for a mapped region,
// which of its pages are resident.
class ResidencyTrace {
public:
    // mode:   the prover mode, as the demo names it.
    // reader: how the prover reads a word ("ram", "pread", "mmap", ...).
    // region: the memory whose residency to report, or nullptr.
    ResidencyTrace(uint32_t every, const char* mode, const char* reader, const void* region,
                   size_t region_bytes, size_t buffer_bytes)
        : every_(every), mode_(mode), reader_(reader), region_((const unsigned char*)region),
          region_bytes_(region_bytes), buffer_bytes_(buffer_bytes) {
        long page = sysconf(_SC_PAGESIZE);
        page_ = page > 0 ? (size_t)page : 4096;
    }

    void after(const Challenge& ch, uint64_t served) {
        probes_ += ch.samples;
        if (every_ && served % every_ == 0) emit(served);
    }

    void finish(uint64_t served) {
        if (every_) emit(served);
    }

private:
    void emit(uint64_t served) {
        const size_t pages = region_bytes_ / page_;
        size_t resident = 0;
        bool have_map = false;
        if (region_ && pages) {
            vec_.resize(pages);
            if (mincore((void*)region_, region_bytes_, vec_.data()) == 0) {
                have_map = true;
                for (size_t i = 0; i < pages; ++i) resident += vec_[i] & 1u;
            }
        }
        const size_t rb = io_read_bytes(), mf = major_faults();
        printf("@{\"t\":\"p\",\"served\":%llu,\"mode\":\"%s\",\"reader\":\"%s\","
               "\"pages\":%zu,\"pages_total\":%zu,\"read_bytes\":%lld,\"majflt\":%lld,"
               "\"probes\":%llu,\"res_pages\":%zu",
               (unsigned long long)served, mode_, reader_, pages, buffer_bytes_ / page_,
               rb == kUnavailable ? -1LL : (long long)rb, mf == kUnavailable ? -1LL : (long long)mf,
               (unsigned long long)probes_, resident);
        if (have_map) {
            fputs(",\"res\":\"", stdout);
            base64_bitmap(stdout, vec_.data(), pages);
            putchar('"');
        }
        puts("}");
    }

    uint32_t                   every_;
    const char*                mode_;
    const char*                reader_;
    const unsigned char*       region_;
    size_t                     region_bytes_;
    size_t                     buffer_bytes_;
    size_t                     page_   = 4096;
    uint64_t                   probes_ = 0;
    std::vector<unsigned char> vec_;
};

} // namespace demo
} // namespace mc
