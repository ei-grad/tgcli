#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tgcli::net {

// Sets FD_CLOEXEC on an fd, closing it and returning -1 on failure. Used to
// emulate SOCK_CLOEXEC/accept4 where the platform lacks them (macOS).
inline int set_cloexec(int fd) {
    if (fd < 0) {
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// socket(2) with the close-on-exec flag set atomically where supported.
inline int socket_cloexec(int domain, int type, int protocol) {
#ifdef SOCK_CLOEXEC
    return ::socket(domain, type | SOCK_CLOEXEC, protocol);
#else
    return set_cloexec(::socket(domain, type, protocol));
#endif
}

// accept(2) with the close-on-exec flag set atomically where supported.
inline int accept_cloexec(int listen_fd) {
#if defined(__linux__)
    return ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    return set_cloexec(::accept(listen_fd, nullptr, nullptr));
#endif
}

// pipe(2) with close-on-exec on both ends. Returns false on failure.
inline bool pipe_cloexec(int& read_fd, int& write_fd) {
    int fds[2] = {-1, -1}; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        return false;
    }
#else
    if (::pipe(fds) != 0) {
        return false;
    }
    // set_cloexec closes the fd it failed on; close the sibling ourselves.
    if (set_cloexec(fds[0]) < 0) {
        ::close(fds[1]);
        return false;
    }
    if (set_cloexec(fds[1]) < 0) {
        ::close(fds[0]);
        return false;
    }
#endif
    read_fd = fds[0];
    write_fd = fds[1];
    return true;
}

} // namespace tgcli::net
