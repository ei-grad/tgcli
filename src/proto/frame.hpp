#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

// JSONL frame protocol between the CLI client and the per-account daemon
// (DESIGN.md §10). Frames are exchanged only between a client and daemon of
// the same binary+protocol version (the hello handshake enforces this), so
// the wire encoding is internal — the frame *kinds* and their semantics are
// the spec-bound part.
namespace tgcli::proto {

inline constexpr int kProtocolVersion = 1;

// The client folds --allow-write and TGCLI_ALLOW_WRITE into this field; the
// daemon cannot see the invoking shell's environment (DESIGN.md §6/§10).
enum class WriteAuthority { Unset, Grant, Deny };

// First frame in each direction after connect.
struct Hello {
    std::string binary_version;
    int protocol_version = 0;
};

struct RequestContext {
    bool tty = false;
    bool json = false;
    bool yes = false;
    bool dry_run = false;
    std::optional<double> timeout_seconds;
    std::string cwd;
    std::optional<std::string> media_dir;
    WriteAuthority write_authority = WriteAuthority::Unset;
};

struct Request {
    std::uint64_t id = 0;
    std::vector<std::string> command;
    nlohmann::json args = nlohmann::json::object();
    RequestContext context;
};

// Terminal success frame for a request.
struct Result {
    std::uint64_t id = 0;
    nlohmann::json data;
};

// One element of a streamed response (NDJSON on the client's stdout).
struct Item {
    std::uint64_t id = 0;
    nlohmann::json data;
};

// Progress report; client renders to stderr, never stdout.
struct Progress {
    std::uint64_t id = 0;
    nlohmann::json data;
};

// Terminal failure frame; exit_code follows the DESIGN.md §5 table.
struct Error {
    std::uint64_t id = 0;
    std::string code;
    std::string message;
    nlohmann::json details = nlohmann::json::object();
    int exit_code = 1;
};

// Daemon asks the client to prompt on its TTY (login secrets, destructive
// confirmations); the client responds with an Answer carrying the same id.
struct Challenge {
    std::uint64_t id = 0;
    nlohmann::json challenge;
};

struct Answer {
    std::uint64_t id = 0;
    nlohmann::json answer;
};

using Frame = std::variant<Hello, Request, Result, Item, Progress, Error, Challenge, Answer>;

// Single-line JSON without a trailing newline; the transport appends '\n'.
std::string serialize(const Frame& frame);

// Parses one line. Returns std::nullopt and sets `error` on malformed input:
// invalid JSON, unknown/missing type, missing or mistyped required fields.
std::optional<Frame> parse(std::string_view line, std::string& error);

} // namespace tgcli::proto
