#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "shared/argon2d.hpp"

namespace mc {

// A byte range of one checkpoint file.
struct Piece {
    uint32_t    file   = 0;
    uint64_t    offset = 0;
    uint64_t    bytes  = 0;
    std::string tensor;
};

// The model weights at the start of the buffer, as scripts/weights_manifest.py
// lists them:
//
//   memchal-weights 1
//   file 0 model-00001-of-00004.safetensors
//   piece 0 <offset> <bytes> model.layers.0.mlp.down_proj.weight
//   ...
//   total <bytes>
//
// The weights are the pieces laid end to end. Relative paths resolve against
// the manifest's folder. Both sides compare the digest, a Blake2b hash of the
// manifest's bytes, before any challenge.
struct Manifest {
    std::vector<std::string> files;
    std::vector<Piece>       pieces;
    uint64_t                 bytes = 0;
    uint8_t                  digest[32] = {};
};

static constexpr char kManifestHeader[] = "memchal-weights 1";

namespace detail {

static inline bool read_file(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char chunk[1 << 16];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) out.append(chunk, n);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static inline bool parse_line(const char* line, const std::string& dir, Manifest& m,
                              bool& have_total) {
    unsigned idx = 0;
    unsigned long long off = 0, bytes = 0;
    int used = 0;
    if (sscanf(line, "file %u %n", &idx, &used) == 1 && used > 0) {
        if (idx != m.files.size()) return false;
        const std::string p(line + used);
        m.files.push_back(p[0] == '/' ? p : dir + "/" + p);
        return true;
    }
    if (sscanf(line, "piece %u %llu %llu %n", &idx, &off, &bytes, &used) == 3 && used > 0) {
        if (idx >= m.files.size() || off % 8 || bytes % 8 || bytes == 0) return false;
        m.pieces.push_back({ idx, off, bytes, std::string(line + used) });
        return true;
    }
    if (sscanf(line, "total %llu", &bytes) == 1) {
        m.bytes = bytes;
        have_total = true;
        return true;
    }
    return false;
}

} // namespace detail

static inline bool read_manifest(const char* path, Manifest& m, char* err, size_t n) {
    m = Manifest{};
    std::string text;
    if (!detail::read_file(path, text)) {
        snprintf(err, n, "cannot read manifest %s: %s", path, strerror(errno));
        return false;
    }
    a2::blake2b(m.digest, sizeof m.digest, text.data(), text.size());

    std::string dir(path);
    const size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);

    bool header = false, have_total = false;
    size_t at = 0;
    for (int lineno = 1; at < text.size(); ++lineno) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const bool ok = header ? detail::parse_line(line.c_str(), dir, m, have_total)
                               : (header = line == kManifestHeader);
        if (!ok) { snprintf(err, n, "%s:%d: cannot parse '%s'", path, lineno, line.c_str()); return false; }
    }
    uint64_t sum = 0;
    for (const Piece& p : m.pieces) sum += p.bytes;
    if (!header || !have_total || m.pieces.empty() || sum != m.bytes) {
        snprintf(err, n, "%s: incomplete manifest, or pieces that don't add up to its total", path);
        return false;
    }
    return true;
}

// Reads the weights a manifest lists, in order, from the checkpoint files.
class WeightsReader {
public:
    WeightsReader() = default;
    WeightsReader(const WeightsReader&) = delete;
    WeightsReader& operator=(const WeightsReader&) = delete;
    ~WeightsReader() { close_all(); }

    bool open(const Manifest& m, char* err, size_t n) {
        close_all();
        m_ = &m;
        for (const std::string& path : m.files) {
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) { snprintf(err, n, "cannot open %s: %s", path.c_str(), strerror(errno)); return false; }
            fds_.push_back(fd);
        }
        return true;
    }

    uint64_t left() const { return m_ ? m_->bytes - read_ : 0; }

    bool read(void* dst, size_t bytes) {
        unsigned char* out = (unsigned char*)dst;
        while (bytes) {
            if (piece_ >= m_->pieces.size()) return false;
            const Piece& p = m_->pieces[piece_];
            const size_t take = (size_t)std::min<uint64_t>(bytes, p.bytes - in_piece_);
            if (pread(fds_[p.file], out, take, (off_t)(p.offset + in_piece_)) != (ssize_t)take)
                return false;
            out += take; bytes -= take; read_ += take; in_piece_ += take;
            if (in_piece_ == p.bytes) { ++piece_; in_piece_ = 0; }
        }
        return true;
    }

private:
    void close_all() {
        for (int fd : fds_) ::close(fd);
        fds_.clear();
        m_ = nullptr;
        piece_ = 0; in_piece_ = 0; read_ = 0;
    }

    const Manifest*  m_        = nullptr;
    std::vector<int> fds_;
    size_t           piece_    = 0;
    uint64_t         in_piece_ = 0;
    uint64_t         read_     = 0;
};

} // namespace mc
