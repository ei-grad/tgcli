#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
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

// On macOS there is no MSG_NOSIGNAL; SO_NOSIGPIPE on the socket is the
// equivalent, making writes to a closed peer fail with EPIPE instead of
// raising SIGPIPE. No-op elsewhere (Linux sends with MSG_NOSIGNAL).
inline int set_nosigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    if (fd >= 0) {
        const int one = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
            ::close(fd);
            return -1;
        }
    }
#endif
    return fd;
}

// socket(2) with the close-on-exec flag set atomically where supported.
inline int socket_cloexec(int domain, int type, int protocol) {
#ifdef SOCK_CLOEXEC
    return set_nosigpipe(::socket(domain, type | SOCK_CLOEXEC, protocol));
#else
    return set_nosigpipe(set_cloexec(::socket(domain, type, protocol)));
#endif
}

// accept(2) with the close-on-exec flag set atomically where supported.
inline int accept_cloexec(int listen_fd) {
#if defined(__linux__)
    return ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    return set_nosigpipe(set_cloexec(::accept(listen_fd, nullptr, nullptr)));
#endif
}

// Returns the process at the connected unix-stream peer. This binds an
// incompatible main-protocol connection to the owner of the bootstrap lock.
inline bool peer_pid(int fd, pid_t& pid, std::string& error) {
#if defined(__APPLE__) && defined(LOCAL_PEERPID)
    socklen_t length = sizeof(pid);
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &length) != 0 || length != sizeof(pid)) {
        error = std::string("cannot inspect unix peer pid: ") + std::strerror(errno);
        return false;
    }
#elif defined(__linux__)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
        length != sizeof(credentials)) {
        error = std::string("cannot inspect unix peer pid: ") + std::strerror(errno);
        return false;
    }
    pid = credentials.pid;
#else
    (void)fd;
    (void)pid;
    error = "unix peer pid inspection is unsupported on this platform";
    return false;
#endif
    if (pid < 1) {
        error = "unix peer reported an invalid pid";
        return false;
    }
    return true;
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
