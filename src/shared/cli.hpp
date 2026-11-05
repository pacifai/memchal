#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc {

// Command-line parsing that exits with the usage text on any error.
//   usage: a printf format with one %u, the default port.
struct Cli {
    const char* tag;
    const char* usage;
    unsigned default_port;

    [[noreturn]] void help() const {
        printf(usage, default_port);
        exit(0);
    }

    [[noreturn]] void error(const char* fmt, const char* a, const char* b = "") const {
        fprintf(stderr, "[%s] ", tag);
        fprintf(stderr, fmt, a, b);
        fprintf(stderr, "\n\n");
        fprintf(stderr, usage, default_port);
        exit(2);
    }

    // The value after the flag at argv[*i]. Advances *i past it.
    const char* value(int argc, char** argv, int* i) const {
        if (*i + 1 >= argc) error("%s: missing value", argv[*i]);
        return argv[++*i];
    }

    uint64_t uint(const char* flag, const char* v) const {
        char* end = nullptr;
        errno = 0;
        unsigned long long x = strtoull(v, &end, 0);
        if (errno || end == v || *end || v[0] == '-') error("%s: not a number: %s", flag, v);
        return x;
    }

    uint32_t u32(const char* flag, const char* v) const {
        uint64_t x = uint(flag, v);
        if (x > UINT32_MAX) error("%s: out of range: %s", flag, v);
        return (uint32_t)x;
    }

    uint16_t port(const char* flag, const char* v) const {
        uint64_t x = uint(flag, v);
        if (x == 0 || x > 65535) error("%s: out of range: %s", flag, v);
        return (uint16_t)x;
    }

    double number(const char* flag, const char* v, double lo, double hi) const {
        char* end = nullptr;
        double x = strtod(v, &end);
        if (end == v || *end || !(x >= lo && x <= hi)) error("%s: out of range: %s", flag, v);
        return x;
    }

    // The index of v in `names`, a nullptr-terminated list.
    int choice(const char* flag, const char* v, const char* const* names) const {
        for (int i = 0; names[i]; ++i)
            if (!strcmp(v, names[i])) return i;
        error("%s: unknown value: %s", flag, v);
    }
};

} // namespace mc
