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
constexpr std::size_t kMaxLineBytes = kMaximumSerializedFrameBytes;

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

bool write_line(int fd, std::string line, const IoDeadline* deadline, std::string& error,
                const secure::WipeObserver& wipe_observer) {
    const secure::StringWiper line_wiper(line, wipe_observer, "write_line");
    constexpr char newline = '\n';
    const auto total_size = line.size() + 1;
    std::size_t written = 0;
    while (written < total_size) {
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
        const bool writing_newline = written == line.size();
        const char* bytes = writing_newline ? &newline : line.data() + written;
        const auto remaining = writing_newline ? std::size_t{1} : line.size() - written;
        const ssize_t count = ::send(fd, bytes, remaining, flags);
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

FrameReader::~FrameReader() {
    secure::wipe(buffer_, wipe_observer_, "frame_reader_buffer");
}

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
            if (pos > kMaxLineBytes) {
                error = "frame exceeds " + std::to_string(kMaxLineBytes) + " bytes";
                secure::wipe(buffer_, wipe_observer_, "frame_reader_error");
                return std::nullopt;
            }
            std::string line = buffer_.substr(0, pos);
            std::string remainder = buffer_.substr(pos + 1);
            secure::wipe(buffer_, wipe_observer_, "frame_reader_buffer");
            buffer_ = std::move(remainder);
            return line;
        }
        if (eof_) {
            if (!buffer_.empty()) {
                error = "connection closed mid-frame";
                secure::wipe(buffer_, wipe_observer_, "frame_reader_error");
            }
            return std::nullopt;
        }
        if (buffer_.size() > kMaxLineBytes) {
            error = "frame exceeds " + std::to_string(kMaxLineBytes) + " bytes";
            secure::wipe(buffer_, wipe_observer_, "frame_reader_error");
            return std::nullopt;
        }
        std::array<char, 65536> chunk{};
        std::size_t count = 0;
        const auto status = read_chunk(fd_, deadline, chunk, count, error);
        if (status == ReadChunkStatus::Failed) {
            secure::wipe(buffer_, wipe_observer_, "frame_reader_error");
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
        secure::wipe(chunk.data(), count, wipe_observer_, "frame_reader_chunk");
    }
}

bool write_frame(int fd, const Frame& frame, std::string& error,
                 const secure::WipeObserver& wipe_observer) {
    auto serialized = serialize_bounded(frame, error, wipe_observer);
    return serialized && write_line(fd, std::move(*serialized), nullptr, error, wipe_observer);
}

bool write_frame_until(int fd, const Frame& frame, IoDeadline deadline, std::string& error,
                       const secure::WipeObserver& wipe_observer) {
    auto serialized = serialize_bounded(frame, error, wipe_observer);
    return serialized && write_line(fd, std::move(*serialized), &deadline, error, wipe_observer);
}

} // namespace tgcli::proto
