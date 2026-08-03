#include "proto/frame_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace tgcli::proto {

namespace {

// A frame is one command's worth of JSON; anything beyond this is a protocol
// violation, not data.
constexpr std::size_t kMaxLineBytes = std::size_t{16} * 1024 * 1024;

bool wait_for_io(int fd, short events, const IoDeadline& deadline, std::string_view operation,
                 std::string& error) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "timed out " + std::string(operation);
            return false;
        }
        const auto remaining = deadline - now;
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (timeout < remaining) {
            timeout += std::chrono::milliseconds(1);
        }
        const auto bounded = std::min<std::chrono::milliseconds::rep>(
            timeout.count(), std::numeric_limits<int>::max());
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(bounded));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            error = "poll while " + std::string(operation) + ": " + std::strerror(errno);
            return false;
        }
        if (result == 0) {
            error = "timed out " + std::string(operation);
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            error = "invalid descriptor while " + std::string(operation);
            return false;
        }
        return true;
    }
}

bool write_line(int fd, std::string line, const IoDeadline* deadline, std::string& error) {
    line.push_back('\n');
    std::size_t written = 0;
    while (written < line.size()) {
        if (deadline != nullptr && !wait_for_io(fd, POLLOUT, *deadline, "writing frame", error)) {
            return false;
        }
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
#if defined(MSG_DONTWAIT)
        if (deadline != nullptr) {
            flags |= MSG_DONTWAIT;
        }
#endif
        const ssize_t count = ::send(fd, line.data() + written, line.size() - written, flags);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && deadline != nullptr && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count < 0) {
            error = std::string("write: ") + std::strerror(errno);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

enum class ReadChunkStatus { Data, Retry, Eof, Failed };

ReadChunkStatus read_chunk(int fd, const IoDeadline* deadline, std::array<char, 65536>& chunk,
                           std::size_t& count, std::string& error) {
    if (deadline != nullptr && !wait_for_io(fd, POLLIN, *deadline, "reading frame", error)) {
        return ReadChunkStatus::Failed;
    }
    const ssize_t received =
        ::recv(fd, chunk.data(), chunk.size(), deadline != nullptr ? MSG_DONTWAIT : 0);
    if (received < 0 && errno == EINTR) {
        return ReadChunkStatus::Retry;
    }
    if (received < 0 && deadline != nullptr && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return ReadChunkStatus::Retry;
    }
    if (received < 0) {
        error = std::string("read: ") + std::strerror(errno);
        return ReadChunkStatus::Failed;
    }
    if (received == 0) {
        return ReadChunkStatus::Eof;
    }
    count = static_cast<std::size_t>(received);
    return ReadChunkStatus::Data;
}

} // namespace

std::optional<std::string> FrameReader::read_line(std::string& error) {
    return read_line_impl(nullptr, error);
}

std::optional<std::string> FrameReader::read_line_until(IoDeadline deadline, std::string& error) {
    return read_line_impl(&deadline, error);
}

std::optional<std::string> FrameReader::read_line_impl(const IoDeadline* deadline,
                                                       std::string& error) {
    error.clear();
    while (true) {
        if (const auto pos = buffer_.find('\n'); pos != std::string::npos) {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            return line;
        }
        if (eof_) {
            if (!buffer_.empty()) {
                error = "connection closed mid-frame";
            }
            return std::nullopt;
        }
        if (buffer_.size() > kMaxLineBytes) {
            error = "frame exceeds " + std::to_string(kMaxLineBytes) + " bytes";
            return std::nullopt;
        }
        std::array<char, 65536> chunk{};
        std::size_t count = 0;
        const auto status = read_chunk(fd_, deadline, chunk, count, error);
        if (status == ReadChunkStatus::Failed) {
            return std::nullopt;
        }
        if (status == ReadChunkStatus::Retry) {
            continue;
        }
        if (status == ReadChunkStatus::Eof) {
            eof_ = true;
            continue;
        }
        buffer_.append(chunk.data(), count);
    }
}

bool write_frame(int fd, const Frame& frame, std::string& error) {
    return write_line(fd, serialize(frame), nullptr, error);
}

bool write_frame_until(int fd, const Frame& frame, IoDeadline deadline, std::string& error) {
    return write_line(fd, serialize(frame), &deadline, error);
}

} // namespace tgcli::proto
