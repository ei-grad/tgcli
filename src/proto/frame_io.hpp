#pragma once

#include "proto/frame.hpp"

#include <optional>
#include <string>

namespace tgcli::proto {

// Buffered line reader over a stream fd. Not thread-safe; one reader per
// connection side.
class FrameReader {
  public:
    explicit FrameReader(int fd) : fd_(fd) {}

    // Next complete line (without the '\n'). Empty optional on EOF (error
    // stays empty) or on a read error / oversized line (error says why).
    std::optional<std::string> read_line(std::string& error);

  private:
    int fd_;
    std::string buffer_;
    bool eof_ = false;
};

// Serializes and writes one frame plus '\n', handling partial writes and
// EINTR. Callers serialize concurrent writers with their own lock. Returns
// false on a write error.
bool write_frame(int fd, const Frame& frame, std::string& error);

} // namespace tgcli::proto
