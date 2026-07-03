#include "proto/frame_io.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace tgcli::proto {

namespace {

// A frame is one command's worth of JSON; anything beyond this is a protocol
// violation, not data.
constexpr std::size_t kMaxLineBytes = std::size_t{16} * 1024 * 1024;

} // namespace

std::optional<std::string> FrameReader::read_line(std::string& error) {
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
        const ssize_t n = ::read(fd_, chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("read: ") + std::strerror(errno);
            return std::nullopt;
        }
        if (n == 0) {
            eof_ = true;
            continue;
        }
        buffer_.append(chunk.data(), static_cast<std::size_t>(n));
    }
}

bool write_frame(int fd, const Frame& frame, std::string& error) {
    std::string line = serialize(frame);
    line.push_back('\n');
    std::size_t written = 0;
    while (written < line.size()) {
        const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("write: ") + std::strerror(errno);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace tgcli::proto
