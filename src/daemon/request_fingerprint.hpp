#pragma once

#include "daemon/message_summary.hpp"
#include "proto/operation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tgcli::daemon {

struct ResolverPrincipal;

enum class FingerprintParseMode { Plain, MarkdownV2, Html };

struct FingerprintScheduleOnline {
    bool operator==(const FingerprintScheduleOnline&) const = default;
};

struct FingerprintScheduleAt {
    std::int32_t send_date = 0;

    bool operator==(const FingerprintScheduleAt&) const = default;
};

using FingerprintSchedule = std::variant<FingerprintScheduleOnline, FingerprintScheduleAt>;

struct SendFingerprintPayload {
    std::string chat_selector;
    std::string text;
    FingerprintParseMode parse_mode = FingerprintParseMode::Plain;
    std::optional<std::int64_t> reply_to;
    std::optional<TopicRef> requested_topic;
    bool silent = false;
    std::optional<FingerprintSchedule> schedule;
};

struct MsgEditFingerprintPayload {
    std::string chat_selector;
    std::int64_t message_id = 0;
    std::string text;
};

struct MsgDeleteFingerprintPayload {
    std::string chat_selector;
    std::vector<std::int64_t> message_ids;
    bool for_all = false;
};

struct MsgForwardFingerprintPayload {
    std::string from_selector;
    std::string to_selector;
    std::vector<std::int64_t> message_ids;
    bool drop_author = false;
};

struct MsgReactFingerprintPayload {
    std::string chat_selector;
    std::int64_t message_id = 0;
    std::string reaction;
    bool remove = false;
    bool big = false;
};

struct MsgPinFingerprintPayload {
    std::string chat_selector;
    std::int64_t message_id = 0;
};

struct MsgUnpinFingerprintPayload {
    std::string chat_selector;
    std::int64_t message_id = 0;
};

struct ChatMarkReadFingerprintPayload {
    std::string chat_selector;
};

struct ChatMuteFingerprintPayload {
    std::string chat_selector;
    std::int32_t duration_seconds = 0;
};

struct ChatUnmuteFingerprintPayload {
    std::string chat_selector;
    std::int32_t duration_seconds = 0;
};

struct ChatPinFingerprintPayload {
    std::string chat_selector;
};

struct ChatUnpinFingerprintPayload {
    std::string chat_selector;
};

struct ChatArchiveFingerprintPayload {
    std::string chat_selector;
};

struct ChatUnarchiveFingerprintPayload {
    std::string chat_selector;
};

struct ChatJoinUsernameFingerprint {
    std::string username;
};

struct ChatJoinInviteFingerprint {
    std::string invite_link_sha256;
};

using ChatJoinFingerprintTarget =
    std::variant<ChatJoinUsernameFingerprint, ChatJoinInviteFingerprint>;

struct ChatJoinFingerprintPayload {
    ChatJoinFingerprintTarget target;
};

struct ChatLeaveFingerprintPayload {
    std::string chat_selector;
};

struct SavedAttachFingerprintPayload {
    std::int64_t message_id = 0;
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
    std::string caption;
};

using FingerprintPayload = std::variant<
    SendFingerprintPayload, MsgEditFingerprintPayload, MsgDeleteFingerprintPayload,
    MsgForwardFingerprintPayload, MsgReactFingerprintPayload, MsgPinFingerprintPayload,
    MsgUnpinFingerprintPayload, ChatMarkReadFingerprintPayload, ChatMuteFingerprintPayload,
    ChatUnmuteFingerprintPayload, ChatPinFingerprintPayload, ChatUnpinFingerprintPayload,
    ChatArchiveFingerprintPayload, ChatUnarchiveFingerprintPayload, ChatJoinFingerprintPayload,
    ChatLeaveFingerprintPayload, SavedAttachFingerprintPayload>;

enum class FingerprintError { InvalidAccount, InvalidPrincipal, InvalidPayload, CanonicalJson };

using RequestFingerprintResult = std::variant<std::string, FingerprintError>;

[[nodiscard]] proto::M3Operation fingerprint_operation(const FingerprintPayload& payload);
[[nodiscard]] std::optional<std::string> canonical_write_selector(std::string_view selector);
[[nodiscard]] std::string idempotency_key_hash(std::string_view raw_key);
[[nodiscard]] std::string invite_link_hash(std::string_view raw_invite);
[[nodiscard]] RequestFingerprintResult request_fingerprint(std::string_view account,
                                                           const ResolverPrincipal& principal,
                                                           const FingerprintPayload& payload);

} // namespace tgcli::daemon
