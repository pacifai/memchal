#pragma once
#include <cstdio>

namespace mc {

static inline void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

} // namespace mc
