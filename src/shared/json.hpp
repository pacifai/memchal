#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace mc {

// Writes JSON to a file as a sequence of calls. Indents nested values when
// `pretty` is set and writes one line otherwise. Every call that takes a key
// adds an object member; pass nullptr for an array element.
class JsonWriter {
public:
    JsonWriter(FILE* f, bool pretty) : f_(f), pretty_(pretty) {}

    JsonWriter& begin_object(const char* key = nullptr) { return open(key, '{'); }
    JsonWriter& end_object() { return close('}'); }
    JsonWriter& begin_array(const char* key = nullptr) { return open(key, '['); }
    JsonWriter& end_array() { return close(']'); }

    JsonWriter& str(const char* key, const char* v) {
        member(key);
        quoted(v);
        return *this;
    }

    JsonWriter& uint(const char* key, uint64_t v) {
        member(key);
        fprintf(f_, "%llu", (unsigned long long)v);
        return *this;
    }

    JsonWriter& integer(const char* key, int64_t v) {
        member(key);
        fprintf(f_, "%lld", (long long)v);
        return *this;
    }

    //   fmt: a printf format for one double.
    JsonWriter& num(const char* key, double v, const char* fmt = "%.4f") {
        member(key);
        fprintf(f_, fmt, v);
        return *this;
    }

    JsonWriter& boolean(const char* key, bool v) {
        member(key);
        fputs(v ? "true" : "false", f_);
        return *this;
    }

    // A 64-bit value as a "0x"-prefixed hex string.
    JsonWriter& hex(const char* key, uint64_t v) {
        member(key);
        fprintf(f_, "\"0x%016llx\"", (unsigned long long)v);
        return *this;
    }

private:
    JsonWriter& open(const char* key, char c) {
        member(key);
        fputc(c, f_);
        empty_.push_back(true);
        return *this;
    }

    JsonWriter& close(char c) {
        const bool was_empty = empty_.back();
        empty_.pop_back();
        if (!was_empty) newline();
        fputc(c, f_);
        if (empty_.empty() && pretty_) fputc('\n', f_);
        return *this;
    }

    void member(const char* key) {
        if (!empty_.empty()) {
            if (!empty_.back()) fputc(',', f_);
            empty_.back() = false;
            newline();
        }
        if (!key) return;
        quoted(key);
        fputs(pretty_ ? ": " : ":", f_);
    }

    void newline() {
        if (!pretty_) return;
        fputc('\n', f_);
        for (size_t i = 0; i < empty_.size(); ++i) fputs("  ", f_);
    }

    void quoted(const char* s) {
        fputc('"', f_);
        for (; s && *s; ++s) {
            if (*s == '"' || *s == '\\') { fputc('\\', f_); fputc(*s, f_); }
            else if ((unsigned char)*s < 0x20) fprintf(f_, "\\u%04x", *s);
            else fputc(*s, f_);
        }
        fputc('"', f_);
    }

    FILE*             f_;
    bool              pretty_;
    std::vector<bool> empty_;
};

} // namespace mc
