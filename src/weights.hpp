// weights.hpp — a buffer the prover already holds, described by a manifest.
//
// The usual protocol uploads a random buffer. This is the other case: the
// buffer is a model's weights, which an inference engine already holds for its
// own use, so nothing is uploaded and both sides must agree on what the buffer
// is. The manifest says so. scripts/weights_manifest.py writes it from a
// checkpoint; it lists the files, then the pieces of the buffer in order:
//
//   memchal-weights 1
//   file 0 model-00001-of-00002.safetensors
//   piece 0 <offset> <bytes> model.layers.0.self_attn.qkv_proj.weight
//   piece 0 <offset> <bytes> model.layers.0.self_attn.qkv_proj.weight
//   ...
//   total <bytes>
//
// A piece is a byte range of one file. Consecutive pieces with the same name
// form one segment: a tensor as the engine holds it, which may be several
// checkpoint tensors stacked (vLLM stacks q, k, and v into one). The buffer is
// every piece laid end to end. Relative file paths resolve against the
// manifest's directory.
//
// Host only: files, mmap, the standard library. The device side sees nothing
// but the Segments table in segments.hpp.
#pragma once
#include "segments.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mc {

// Owns the arrays a Segments table points at, on the host.
struct SegmentTable {
    std::vector<const uint64_t*> base;
    std::vector<uint64_t> start;
    std::vector<uint32_t> dir;
    uint32_t shift = 0;

    // `max_dir` bounds the directory: 2^20 entries is 4 MiB, ample for any
    // model, and only a buffer of tiny pieces ever reaches it.
    bool build(const std::vector<const uint64_t*>& bases, const std::vector<uint64_t>& words,
               uint64_t max_dir = 1u << 20) {
        if (bases.empty() || bases.size() != words.size()) return false;
        uint64_t total = 0;
        for (uint64_t w : words) total += w;
        if (total == 0) return false;
        base = bases;
        uint32_t n = (uint32_t)words.size();
        shift = segments_shift(words.data(), n, max_dir);
        start.assign(n + 1, 0);
        dir.assign(segments_dir_len(total, shift), 0);
        segments_fill(words.data(), n, shift, start.data(), dir.data());
        return true;
    }
    Segments view() const {
        return Segments{ base.data(), start.data(), dir.data(), (uint32_t)base.size(), shift };
    }
    uint64_t words() const { return start.empty() ? 0 : start.back(); }
};

struct Piece {
    uint32_t file = 0;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    std::string segment;
};

struct Manifest {
    std::vector<std::string> files;   // resolved paths
    std::vector<Piece> pieces;
    uint64_t total_bytes = 0;
};

// Segments as runs of consecutive pieces sharing a name: [first, end) indices.
struct SegmentRun { size_t first, end; uint64_t bytes; };

static inline std::vector<SegmentRun> segment_runs(const Manifest& m) {
    std::vector<SegmentRun> runs;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        if (runs.empty() || m.pieces[i].segment != m.pieces[runs.back().first].segment)
            runs.push_back({ i, i, 0 });
        runs.back().end = i + 1;
        runs.back().bytes += m.pieces[i].bytes;
    }
    return runs;
}

static inline bool read_manifest(const char* path, Manifest* out, char* err, size_t errlen) {
    FILE* f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "cannot open manifest %s: %s", path, strerror(errno)); return false; }
    std::string dir(path);
    size_t slash = dir.rfind('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);

    Manifest m;
    bool header = false, have_total = false;
    uint64_t sum = 0;
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        ++lineno;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len || line[0] == '#') continue;
        if (!header) {
            if (strcmp(line, "memchal-weights 1") != 0) {
                snprintf(err, errlen, "%s: not a memchal-weights 1 manifest", path);
                fclose(f); return false;
            }
            header = true;
            continue;
        }
        unsigned idx = 0; unsigned long long off = 0, bytes = 0; int used = 0;
        if (sscanf(line, "file %u %n", &idx, &used) == 1 && used > 0) {
            if (idx != m.files.size()) {
                snprintf(err, errlen, "%s:%d: files must be numbered 0, 1, ... in order", path, lineno);
                fclose(f); return false;
            }
            std::string p(line + used);
            m.files.push_back(p[0] == '/' ? p : dir + "/" + p);
        } else if (sscanf(line, "piece %u %llu %llu %n", &idx, &off, &bytes, &used) == 3 && used > 0) {
            if (idx >= m.files.size()) {
                snprintf(err, errlen, "%s:%d: piece names file %u before it is listed", path, lineno, idx);
                fclose(f); return false;
            }
            if (off % 8 || bytes % 8 || bytes == 0) {
                snprintf(err, errlen, "%s:%d: pieces must be whole 8-byte words at 8-byte offsets", path, lineno);
                fclose(f); return false;
            }
            m.pieces.push_back({ idx, off, bytes, std::string(line + used) });
            sum += bytes;
        } else if (sscanf(line, "total %llu", &bytes) == 1) {
            m.total_bytes = bytes;
            have_total = true;
        } else {
            snprintf(err, errlen, "%s:%d: cannot parse '%s'", path, lineno, line);
            fclose(f); return false;
        }
    }
    fclose(f);
    if (!header || !have_total || m.pieces.empty()) {
        snprintf(err, errlen, "%s: incomplete manifest", path); return false;
    }
    if (sum != m.total_bytes) {
        snprintf(err, errlen, "%s: pieces add up to %llu bytes, total says %llu", path,
                 (unsigned long long)sum, (unsigned long long)m.total_bytes);
        return false;
    }
    *out = m;
    return true;
}

// Read-only maps of every file a manifest names. The verifier's reference
// copy: it reads only the words a challenge probes, so mapping costs nothing
// up front and the checkpoint is never copied.
struct MappedFiles {
    std::vector<void*> base;
    std::vector<size_t> size;
    ~MappedFiles() {
        for (size_t i = 0; i < base.size(); ++i)
            if (base[i] && base[i] != MAP_FAILED) munmap(base[i], size[i]);
    }
};

static inline bool map_files(const Manifest& m, MappedFiles* out, char* err, size_t errlen) {
    for (const std::string& path : m.files) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) { snprintf(err, errlen, "cannot open %s: %s", path.c_str(), strerror(errno)); return false; }
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            snprintf(err, errlen, "cannot size %s", path.c_str()); close(fd); return false;
        }
        void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { snprintf(err, errlen, "cannot map %s: %s", path.c_str(), strerror(errno)); return false; }
        out->base.push_back(p);
        out->size.push_back((size_t)st.st_size);
    }
    for (const Piece& pc : m.pieces) {
        if (pc.offset + pc.bytes > out->size[pc.file]) {
            snprintf(err, errlen, "piece of %s runs past the end of %s", pc.segment.c_str(),
                     m.files[pc.file].c_str());
            return false;
        }
    }
    return true;
}

// A table over the pieces where they sit in the mapped files.
static inline bool table_over_files(const Manifest& m, const MappedFiles& f, SegmentTable* t) {
    std::vector<const uint64_t*> bases;
    std::vector<uint64_t> words;
    for (const Piece& pc : m.pieces) {
        bases.push_back((const uint64_t*)((const unsigned char*)f.base[pc.file] + pc.offset));
        words.push_back(pc.bytes / 8);
    }
    return t->build(bases, words);
}

} // namespace mc
