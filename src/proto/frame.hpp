#pragma once

#include "common/deadline.hpp"
#include "common/frame_budget.hpp"
#include "common/secure_wipe.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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

struct RequestSourceAccess;
struct RequestFacts;

inline constexpr int kProtocolVersion = 3;
inline constexpr std::uint64_t kMaximumSerializedFrameBytes =
    frame_budget::kMaximumSerializedFrameBytes;
inline constexpr std::uint64_t kMaximumRequestSourceBytes = kMaximumSerializedFrameBytes;
constexpr std::size_t maximum_result_payload_bytes(std::uint64_t request_id) noexcept {
    return frame_budget::maximum_result_payload_bytes(request_id);
}

// The client folds --allow-write and TGCLI_ALLOW_WRITE into this field; the
// daemon cannot see the invoking shell's environment (DESIGN.md §6/§10).
enum class WriteAuthority { Unset, Grant, Deny };

enum class ChallengeKind {
    ApiId,
    ApiHash,
    DatabaseKey,
    PhoneNumber,
    AuthenticationCode,
    EmailAddress,
    EmailCode,
    Password,
    BotToken,
    RegistrationTerms,
    RegistrationFirstName,
    RegistrationLastName,
    DestructiveConfirmation,
};

std::string_view challenge_kind_name(ChallengeKind kind);
std::optional<ChallengeKind> parse_challenge_kind(std::string_view name);
bool challenge_kind_is_secret(ChallengeKind kind);
bool challenge_kind_expects_boolean(ChallengeKind kind);

using tgcli::deadline_expired;
using tgcli::DeadlineDefault;
using tgcli::event_precedes_deadline;
using tgcli::request_deadline;
using tgcli::RequestDeadline;

// Validates the exact closed payload shapes from DESIGN.md §10.
bool validate_challenge_payload(const nlohmann::json& payload, std::string& error);
bool validate_answer_payload(const nlohmann::json& payload, std::string& error);

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
    std::optional<std::string> idempotency_key;
};

struct Request {
    explicit Request(std::string account_value, secure::WipeObserver wipe_observer = {});
    ~Request();
    Request(const Request& other) = default;
    Request& operator=(const Request& other);
    Request(Request&& other) noexcept;
    Request& operator=(Request&& other) noexcept;

    [[nodiscard]] std::uint64_t source_bytes() const noexcept {
        return source_bytes_;
    }

    [[nodiscard]] const secure::WipeObserver& wipe_observer() const noexcept {
        return wipe_observer_;
    }

    std::uint64_t id = 0; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    std::string account;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
    std::vector<std::string> command;
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
    nlohmann::json args = nlohmann::json::object();
    RequestContext context; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)

  private:
    std::uint64_t source_bytes_ = 0;
    std::shared_ptr<const RequestFacts> admitted_facts_;
    secure::WipeObserver wipe_observer_;

    friend std::optional<Request> admit_request_source(const Request& request, std::string& error);
    friend struct RequestSourceAccess;
};

// Terminal success frame for a request.
struct Result {
    std::uint64_t id = 0;
    nlohmann::json data;
};

class RawResult final {
  public:
    RawResult(std::uint64_t id_value, std::string canonical,
              secure::WipeObserver wipe_observer = {});
    ~RawResult() = default;
    RawResult(const RawResult&) = delete;
    RawResult& operator=(const RawResult&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    RawResult(RawResult&&) = default;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    RawResult& operator=(RawResult&&) = default;

    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] std::string_view canonical() const noexcept;

  private:
    std::uint64_t id_ = 0;
    secure::SensitiveString canonical_;
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
    Answer();
    Answer(std::uint64_t id_value, nlohmann::json&& answer_value,
           secure::WipeObserver wipe_observer_value = {});
    ~Answer();
    Answer(const Answer&) = delete;
    Answer& operator=(const Answer&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    Answer(Answer&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    Answer& operator=(Answer&& other);
    [[nodiscard]] const secure::WipeObserver& wipe_observer() const;

    std::uint64_t id = 0;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    nlohmann::json answer; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)

  private:
    secure::WipeObserver wipe_observer_;
};

using Frame =
    std::variant<Hello, Request, Result, RawResult, Item, Progress, Error, Challenge, Answer>;

// Canonically samples an in-process Request using the same compact JSON bytes
// admitted by the socket reader. The returned copy owns the immutable sample.
std::optional<Request> admit_request_source(const Request& request, std::string& error);

// Single-line JSON without a trailing newline; the transport appends '\n'.
std::string serialize(const Frame& frame, const secure::WipeObserver& wipe_observer = {});

// Serializes one complete compact frame and rejects it before transport when
// its bytes excluding LF exceed the shared protocol ceiling.
std::optional<std::string> serialize_bounded(const Frame& frame, std::string& error,
                                             const secure::WipeObserver& wipe_observer = {});

// Parses one line. Returns std::nullopt and sets `error` on malformed input:
// invalid JSON, unknown/missing type, missing or mistyped required fields.
std::optional<Frame> parse(std::string line, std::string& error,
                           const secure::WipeObserver& wipe_observer = {},
                           std::optional<Answer>* invalid_answer = nullptr);

// Recovers only an answer frame's request id and raw payload after strict
// parsing failed. The server uses this to return the specified
// PROTOCOL_ANSWER_INVALID terminal instead of treating a malformed answer as
// an unrelated connection error.
std::optional<Answer> parse_answer_candidate(std::string line,
                                             const secure::WipeObserver& wipe_observer = {});

} // namespace tgcli::proto
