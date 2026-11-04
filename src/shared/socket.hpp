#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mc {

static inline bool send_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}

static inline bool recv_all(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        ssize_t k = ::recv(fd, c, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return false; }
        c += k; n -= (size_t)k;
    }
    return true;
}

static inline void set_no_delay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static inline int connect_to(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad host %s\n", host); exit(1); }
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("connect"); exit(1); }
    set_no_delay(fd);
    return fd;
}

static inline int listen_on(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 8) < 0) { perror("listen"); exit(1); }
    return fd;
}

} // namespace mc
