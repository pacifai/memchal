#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc {

constexpr size_t kUnavailable = (size_t)-1;

// Bytes of this process's memory the kernel has locked, from /proc/self/status.
static inline size_t locked_bytes() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return kUnavailable;
    char line[256]; unsigned long kb = 0; bool found = false;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmLck: %lu kB", &kb) == 1) { found = true; break; }
    fclose(f);
    return found ? (size_t)kb * 1024 : kUnavailable;
}

// Bytes this process has read from block storage, from /proc/self/io.
static inline size_t io_read_bytes() {
    FILE* f = fopen("/proc/self/io", "r");
    if (!f) return kUnavailable;
    char line[256]; unsigned long long b = 0; bool found = false;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "read_bytes: %llu", &b) == 1) { found = true; break; }
    fclose(f);
    return found ? (size_t)b : kUnavailable;
}

// Major page faults this process has taken, from /proc/self/stat.
static inline size_t major_faults() {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return kUnavailable;
    char buf[1024];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!got) return kUnavailable;
    buf[got] = '\0';
    char* p = strrchr(buf, ')');
    if (!p) return kUnavailable;
    int field = 2;
    for (char* tok = strtok(p + 1, " "); tok; tok = strtok(nullptr, " "))
        if (++field == 12) return (size_t)strtoull(tok, nullptr, 10);
    return kUnavailable;
}

} // namespace mc
