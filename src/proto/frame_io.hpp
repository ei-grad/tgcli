#pragma once

#include "proto/frame.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace tgcli::proto {

using IoDeadline = std::chrono::steady_clock::time_point;

// Buffered line reader over a stream fd. Not thread-safe; one reader per
// connection side.
class FrameReader {
  public:
    explicit FrameReader(int fd) : fd_(fd) {}

    // Next complete line (without the '\n'). Empty optional on EOF (error
    // stays empty) or on a read error / oversized line (error says why).
    std::optional<std::string> read_line(std::string& error);

    // Deadline-bounded counterpart used during daemon bootstrap/restart.
    std::optional<std::string> read_line_until(IoDeadline deadline, std::string& error);

  private:
    std::optional<std::string> read_line_impl(const IoDeadline* deadline, std::string& error);

    int fd_;
    std::string buffer_;
    bool eof_ = false;
};

// Serializes and writes one frame plus '\n', handling partial writes and
// EINTR. Callers serialize concurrent writers with their own lock. Returns
// false on a write error.
bool write_frame(int fd, const Frame& frame, std::string& error);

// Deadline-bounded counterpart used during daemon bootstrap/restart.
bool write_frame_until(int fd, const Frame& frame, IoDeadline deadline, std::string& error);

} // namespace tgcli::proto
