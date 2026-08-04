#include "cli/control_stop.hpp"

#include "common/net_compat.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace tgcli::cli::detail {

namespace {

bool retry_before_deadline(proto::IoDeadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return false;
    }
    std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds(10)));
    return std::chrono::steady_clock::now() < deadline;
}

} // namespace

ControlConnectOutcome
connect_verified_control_endpoint(const std::string& control_socket_path,
                                  const paths::SocketIdentity& frozen_identity, uid_t uid, int& fd,
                                  std::string& error) {
    fd = net::socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = "cannot create daemon control socket: " + std::string(std::strerror(errno));
        return ControlConnectOutcome::Failed;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error =
            "cannot make daemon control socket non-blocking: " + std::string(std::strerror(errno));
        ::close(fd);
        fd = -1;
        return ControlConnectOutcome::Failed;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (control_socket_path.size() >= sizeof(address.sun_path)) {
        error = "control socket path too long: " + control_socket_path;
        ::close(fd);
        fd = -1;
        return ControlConnectOutcome::Failed;
    }
    std::strncpy(address.sun_path, control_socket_path.c_str(), sizeof(address.sun_path) - 1);
    while (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int connect_error = errno;
        ::close(fd);
        fd = -1;
        if (connect_error == ENOENT || connect_error == ECONNREFUSED) {
            return ControlConnectOutcome::AlreadyGone;
        }
        error = "cannot connect daemon control socket " + control_socket_path + ": " +
                std::strerror(connect_error);
        return ControlConnectOutcome::Failed;
    }

    bool changed = false;
    if (!paths::socket_endpoint_changed(control_socket_path, uid, frozen_identity, changed,
                                        error)) {
        ::close(fd);
        fd = -1;
        return ControlConnectOutcome::Failed;
    }
    if (changed) {
        error = "daemon control endpoint changed while connecting";
        ::close(fd);
        fd = -1;
        return ControlConnectOutcome::Failed;
    }
    return ControlConnectOutcome::Connected;
}

ControlStopOutcome send_connected_control_stop(int fd, std::string_view control_token,
                                               proto::IoDeadline deadline,
                                               const ControlRetryObserver& retry_observer,
                                               std::string& error) {
    for (;;) {
        int send_flags = 0;
#if defined(MSG_NOSIGNAL)
        send_flags |= MSG_NOSIGNAL;
#endif
#if defined(MSG_DONTWAIT)
        send_flags |= MSG_DONTWAIT;
#endif
        const ssize_t count = ::send(fd, control_token.data(), control_token.size(), send_flags);
        if (count == static_cast<ssize_t>(control_token.size())) {
            return ControlStopOutcome::Sent;
        }
        if (count >= 0) {
            error = "short daemon control datagram";
            return ControlStopOutcome::Failed;
        }
        const int send_error = errno;
        if (send_error == EINTR) {
            continue;
        }
        if (send_error == ENOENT || send_error == ECONNREFUSED) {
            return ControlStopOutcome::AlreadyGone;
        }
        if (send_error == EAGAIN || send_error == EWOULDBLOCK || send_error == ENOBUFS) {
            if (retry_observer) {
                retry_observer();
            }
            if (!retry_before_deadline(deadline)) {
                error = "timed out sending daemon control datagram";
                return ControlStopOutcome::Failed;
            }
            continue;
        }
        error = "cannot send daemon control datagram: " + std::string(std::strerror(send_error));
        return ControlStopOutcome::Failed;
    }
}

ControlStopOutcome send_verified_control_stop(const std::string& control_socket_path,
                                              const paths::SocketIdentity& frozen_identity,
                                              uid_t uid, std::string_view control_token,
                                              proto::IoDeadline deadline, std::string& error) {
    int fd = -1;
    const auto connected =
        connect_verified_control_endpoint(control_socket_path, frozen_identity, uid, fd, error);
    if (connected == ControlConnectOutcome::AlreadyGone) {
        return ControlStopOutcome::AlreadyGone;
    }
    if (connected == ControlConnectOutcome::Failed) {
        return ControlStopOutcome::Failed;
    }
    const auto outcome =
        send_connected_control_stop(fd, control_token, deadline, ControlRetryObserver{}, error);
    ::close(fd);
    return outcome;
}

} // namespace tgcli::cli::detail
