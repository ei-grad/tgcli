#include "daemon/account_audit.hpp"

#include "common/daemon_lock.hpp"
#include "common/paths.hpp"
#include "common/utf8.hpp"
#include "proto/destructive_plan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace tgcli::daemon {

namespace {

using json = nlohmann::json;
using namespace account_audit_limits;

constexpr std::array<std::pair<AccountAuditOperation, std::string_view>, 18> kOperations{{
    {AccountAuditOperation::Send, "send"},
    {AccountAuditOperation::MsgEdit, "msg_edit"},
    {AccountAuditOperation::MsgDelete, "msg_delete"},
    {AccountAuditOperation::MsgForward, "msg_forward"},
    {AccountAuditOperation::MsgReact, "msg_react"},
    {AccountAuditOperation::MsgPin, "msg_pin"},
    {AccountAuditOperation::MsgUnpin, "msg_unpin"},
    {AccountAuditOperation::ChatMarkRead, "chat_mark_read"},
    {AccountAuditOperation::ChatMute, "chat_mute"},
    {AccountAuditOperation::ChatUnmute, "chat_unmute"},
    {AccountAuditOperation::ChatPin, "chat_pin"},
    {AccountAuditOperation::ChatUnpin, "chat_unpin"},
    {AccountAuditOperation::ChatArchive, "chat_archive"},
    {AccountAuditOperation::ChatUnarchive, "chat_unarchive"},
    {AccountAuditOperation::ChatJoin, "chat_join"},
    {AccountAuditOperation::ChatLeave, "chat_leave"},
    {AccountAuditOperation::SavedAttach, "saved_attach"},
    {AccountAuditOperation::SessionTerminate, "session_terminate"},
}};

constexpr std::array<std::pair<AccountAuditStage, std::string_view>, 6> kStages{{
    {AccountAuditStage::IdempotencyPending, "idempotency_pending"},
    {AccountAuditStage::SpoolReady, "spool_ready"},
    {AccountAuditStage::DispatchStarted, "dispatch_started"},
    {AccountAuditStage::TemporaryIdsObserved, "temporary_ids_observed"},
    {AccountAuditStage::ForwardProgress, "forward_progress"},
    {AccountAuditStage::MutationConfirmed, "mutation_confirmed"},
}};

constexpr std::array<std::pair<AccountAuditDurabilityReason, std::string_view>, 17>
    kDurabilityReasons{{
        {AccountAuditDurabilityReason::PathInvalid, "path_invalid"},
        {AccountAuditDurabilityReason::WrongOwner, "wrong_owner"},
        {AccountAuditDurabilityReason::WrongType, "wrong_type"},
        {AccountAuditDurabilityReason::WrongMode, "wrong_mode"},
        {AccountAuditDurabilityReason::WrongLinkCount, "wrong_link_count"},
        {AccountAuditDurabilityReason::TooLarge, "too_large"},
        {AccountAuditDurabilityReason::CapacityExhausted, "capacity_exhausted"},
        {AccountAuditDurabilityReason::OpenFailed, "open_failed"},
        {AccountAuditDurabilityReason::LockFailed, "lock_failed"},
        {AccountAuditDurabilityReason::ReadFailed, "read_failed"},
        {AccountAuditDurabilityReason::WriteFailed, "write_failed"},
        {AccountAuditDurabilityReason::SyncFailed, "sync_failed"},
        {AccountAuditDurabilityReason::RenameFailed, "rename_failed"},
        {AccountAuditDurabilityReason::DirectorySyncFailed, "directory_sync_failed"},
        {AccountAuditDurabilityReason::ParseError, "parse_error"},
        {AccountAuditDurabilityReason::SchemaError, "schema_error"},
        {AccountAuditDurabilityReason::Contradiction, "contradiction"},
    }};

class Descriptor final {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                ::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const {
        return value_;
    }

  private:
    int value_;
};

bool exact_fields(const json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object() || value.size() != names.size()) {
        return false;
    }
    return std::all_of(names.begin(), names.end(), [&value](std::string_view name) {
        return value.contains(std::string(name));
    });
}

std::optional<std::size_t> utf8_scalar_count(std::string_view value) {
    if (!common::valid_utf8(value)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(), [](char character) {
        return (static_cast<unsigned char>(character) & 0xc0U) != 0x80U;
    }));
}

bool valid_string(const json& value, std::uint64_t maximum = kRequestSourceBytes) {
    return value.is_string() && value.get_ref<const std::string&>().size() <= maximum &&
           common::valid_utf8(value.get_ref<const std::string&>());
}

bool valid_hex(std::string_view value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool valid_hash(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return text.starts_with("sha256:") && valid_hex(std::string_view(text).substr(7), 64);
}

bool valid_timestamp(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return false;
    }
    const auto digits = [&text](std::size_t begin, std::size_t count) {
        return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(begin),
                           text.begin() + static_cast<std::ptrdiff_t>(begin + count),
                           [](char character) { return character >= '0' && character <= '9'; });
    };
    if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2) || !digits(11, 2) || !digits(14, 2) ||
        !digits(17, 2) || text.substr(0, 4) == "0000") {
        return false;
    }
    const auto number = [&text](std::size_t begin) {
        return (text[begin] - '0') * 10 + (text[begin + 1] - '0');
    };
    const auto date =
        std::chrono::year_month_day{std::chrono::year{std::stoi(text.substr(0, 4))},
                                    std::chrono::month{static_cast<unsigned>(number(5))},
                                    std::chrono::day{static_cast<unsigned>(number(8))}};
    return date.ok() && number(11) <= 23 && number(14) <= 59 && number(17) <= 60;
}

bool valid_int53(const json& value, bool positive = false) {
    constexpr std::int64_t maximum = 9'007'199'254'740'991LL;
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    return number >= (positive ? 1 : -maximum) && number <= maximum;
}

bool valid_int53_array(const json& value, std::size_t minimum = 1, std::size_t maximum = 100) {
    return value.is_array() && value.size() >= minimum && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(),
                       [](const json& item) { return valid_int53(item); });
}

bool valid_positive_int53_array(const json& value, std::size_t minimum = 1,
                                std::size_t maximum = 100) {
    return value.is_array() && value.size() >= minimum && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(),
                       [](const json& item) { return valid_int53(item, true); });
}

bool valid_session_id(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text == "-0" || text.front() == '+' ||
        (text.size() > 1 && text.front() == '0') ||
        (text.size() > 2 && text[0] == '-' && text[1] == '0')) {
        return false;
    }
    std::int64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool valid_chat_identity(const json& value) {
    if (!exact_fields(value, {"id", "title", "type", "is_bot", "usernames"}) ||
        !valid_int53(value["id"]) || !valid_string(value["title"], kChatTitleBytes) ||
        !value["type"].is_string() || !value["is_bot"].is_boolean() ||
        !value["usernames"].is_array() || value["usernames"].size() > kChatUsernameCount) {
        return false;
    }
    const auto type = value["type"].get_ref<const std::string&>();
    if (type != "private" && type != "basic_group" && type != "supergroup" && type != "channel") {
        return false;
    }
    if (value["id"] == 0 || (type != "private" && value["is_bot"] == true)) {
        return false;
    }
    return std::all_of(value["usernames"].begin(), value["usernames"].end(), [](const json& item) {
        if (!item.is_string()) {
            return false;
        }
        const auto& username = item.get_ref<const std::string&>();
        return !username.empty() && username.size() <= kChatUsernameBytes &&
               std::all_of(username.begin(), username.end(), [](char character) {
                   return (character >= 'A' && character <= 'Z') ||
                          (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
               });
    });
}

bool valid_topic(const json& value, std::string_view kind, bool int32_only) {
    if (value.is_null()) {
        return true;
    }
    if (!exact_fields(value, {"kind", "id"}) || value["kind"] != kind ||
        !valid_int53(value["id"], true)) {
        return false;
    }
    return !int32_only ||
           value["id"].get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max();
}

bool valid_schedule(const json& value) {
    if (value.is_null()) {
        return true;
    }
    if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string()) {
        return false;
    }
    if (value["kind"] == "online") {
        return exact_fields(value, {"kind"});
    }
    return value["kind"] == "at" && exact_fields(value, {"kind", "send_date"}) &&
           value["send_date"].is_number_integer() && value["send_date"].get<std::int64_t>() > 0 &&
           value["send_date"].get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max();
}

bool valid_file_snapshot(const json& value) {
    return exact_fields(value, {"path", "name", "size", "sha256", "device", "inode", "mtime_ns",
                                "ctime_ns"}) &&
           valid_string(value["path"]) && valid_string(value["name"], 255) &&
           !value["name"].get_ref<const std::string&>().empty() &&
           value["name"].get_ref<const std::string&>().find('/') == std::string::npos &&
           value["size"].is_number_unsigned() && valid_hash(value["sha256"]) &&
           value["device"].is_number_unsigned() && value["inode"].is_number_unsigned() &&
           value["mtime_ns"].is_number_integer() && value["ctime_ns"].is_number_integer();
}

bool valid_arguments(AccountAuditOperation operation, const json& value) {
    switch (operation) {
    case AccountAuditOperation::Send:
        return exact_fields(value, {"chat", "text", "parse_mode", "reply_to", "topic", "silent",
                                    "schedule"}) &&
               valid_string(value["chat"]) && valid_string(value["text"]) &&
               (value["parse_mode"] == "plain" || value["parse_mode"] == "markdown_v2" ||
                value["parse_mode"] == "html") &&
               (value["reply_to"].is_null() || valid_int53(value["reply_to"], true)) &&
               valid_topic(value["topic"], "forum", true) && value["silent"].is_boolean() &&
               valid_schedule(value["schedule"]);
    case AccountAuditOperation::MsgEdit:
        return exact_fields(value, {"chat", "message_id", "text"}) && valid_string(value["chat"]) &&
               valid_int53(value["message_id"], true) && valid_string(value["text"]);
    case AccountAuditOperation::MsgDelete:
        return exact_fields(value, {"chat", "message_ids", "for_all"}) &&
               valid_string(value["chat"]) && valid_positive_int53_array(value["message_ids"]) &&
               value["for_all"].is_boolean();
    case AccountAuditOperation::MsgForward:
        return exact_fields(value, {"from", "to", "message_ids", "drop_author"}) &&
               valid_string(value["from"]) && valid_string(value["to"]) &&
               valid_positive_int53_array(value["message_ids"]) &&
               value["drop_author"].is_boolean();
    case AccountAuditOperation::MsgReact:
        return exact_fields(value, {"chat", "message_id", "reaction", "remove", "big"}) &&
               valid_string(value["chat"]) && valid_int53(value["message_id"], true) &&
               valid_string(value["reaction"], 64) && value["remove"].is_boolean() &&
               value["big"].is_boolean();
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return exact_fields(value, {"chat", "message_id"}) && valid_string(value["chat"]) &&
               valid_int53(value["message_id"], true);
    case AccountAuditOperation::ChatMarkRead:
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
    case AccountAuditOperation::ChatLeave:
        return exact_fields(value, {"chat"}) && valid_string(value["chat"]);
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return exact_fields(value, {"chat", "duration_seconds"}) && valid_string(value["chat"]) &&
               value["duration_seconds"].is_number_integer() &&
               value["duration_seconds"].get<std::int64_t>() >=
                   std::numeric_limits<std::int32_t>::min() &&
               value["duration_seconds"].get<std::int64_t>() <=
                   std::numeric_limits<std::int32_t>::max();
    case AccountAuditOperation::ChatJoin:
        if (exact_fields(value, {"source", "username"})) {
            return value["source"] == "username" && valid_string(value["username"]);
        }
        return exact_fields(value, {"source", "invite_link_sha256"}) &&
               value["source"] == "invite_link" && valid_hash(value["invite_link_sha256"]);
    case AccountAuditOperation::SavedAttach:
        return exact_fields(value, {"message_id", "path", "caption"}) &&
               valid_int53(value["message_id"], true) && valid_string(value["path"]) &&
               valid_string(value["caption"]);
    case AccountAuditOperation::SessionTerminate:
        return exact_fields(value, {"session_id"}) && valid_session_id(value["session_id"]);
    }
    return false;
}

std::set<std::string_view> expected_tdlib_functions(AccountAuditOperation operation) {
    switch (operation) {
    case AccountAuditOperation::Send:
    case AccountAuditOperation::SavedAttach:
        return {"sendMessage"};
    case AccountAuditOperation::MsgEdit:
        return {"editMessageText"};
    case AccountAuditOperation::MsgDelete:
        return {"deleteMessages"};
    case AccountAuditOperation::MsgForward:
        return {"forwardMessages"};
    case AccountAuditOperation::MsgReact:
        return {"addMessageReaction", "removeMessageReaction"};
    case AccountAuditOperation::MsgPin:
        return {"pinChatMessage"};
    case AccountAuditOperation::MsgUnpin:
        return {"unpinChatMessage"};
    case AccountAuditOperation::ChatMarkRead:
        return {"viewMessages", ""};
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return {"setChatNotificationSettings"};
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
        return {"toggleChatIsPinned"};
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
        return {"addChatToList"};
    case AccountAuditOperation::ChatJoin:
        return {"joinChat", "joinChatByInviteLink"};
    case AccountAuditOperation::ChatLeave:
        return {"leaveChat"};
    case AccountAuditOperation::SessionTerminate:
        return {"terminateSession"};
    }
    return {};
}

bool valid_session_target(const json& value) {
    if (!exact_fields(value, {"id", "is_current", "is_password_pending", "is_unconfirmed",
                              "device_type", "application_name", "application_version",
                              "device_model", "platform", "system_version", "last_active_date"}) ||
        !valid_session_id(value["id"]) || value["is_current"] != false ||
        !value["is_password_pending"].is_boolean() || !value["is_unconfirmed"].is_boolean() ||
        !value["device_type"].is_string() ||
        !valid_string(value["application_name"], kSessionStringBytes) ||
        !valid_string(value["application_version"], kSessionStringBytes) ||
        !valid_string(value["device_model"], kSessionStringBytes) ||
        !valid_string(value["platform"], kSessionStringBytes) ||
        !valid_string(value["system_version"], kSessionStringBytes)) {
        return false;
    }
    constexpr std::array<std::string_view, 17> device_types{
        "android", "apple", "brave",  "chrome", "edge",    "firefox", "ipad",    "iphone", "linux",
        "mac",     "opera", "safari", "ubuntu", "unknown", "vivaldi", "windows", "xbox"};
    if (std::find(device_types.begin(), device_types.end(),
                  value["device_type"].get_ref<const std::string&>()) == device_types.end()) {
        return false;
    }
    return value["last_active_date"].is_null() || valid_timestamp(value["last_active_date"]);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed per-operation plan oneOf.
bool valid_plan(AccountAuditOperation operation, const json& value, std::string_view account) {
    if (!value.is_object() || !value.contains("operation") ||
        value["operation"] != account_audit_operation_name(operation) ||
        !value.contains("account") || value["account"] != account ||
        !value.contains("tdlib_request")) {
        return false;
    }
    const auto functions = expected_tdlib_functions(operation);
    if (value["tdlib_request"].is_null()) {
        if (!functions.contains("")) {
            return false;
        }
    } else if (!value["tdlib_request"].is_string() ||
               !functions.contains(value["tdlib_request"].get_ref<const std::string&>())) {
        return false;
    }
    const auto chat = [&value](std::string_view field) {
        return valid_chat_identity(value.at(field));
    };
    switch (operation) {
    case AccountAuditOperation::Send:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "text",
                                    "parse_mode", "reply_to", "requested_topic", "effective_topic",
                                    "silent", "schedule", "observed_server_unix_time"}) &&
               chat("chat") && valid_string(value["text"]) &&
               (value["parse_mode"] == "plain" || value["parse_mode"] == "markdown_v2" ||
                value["parse_mode"] == "html") &&
               (value["reply_to"].is_null() || valid_int53(value["reply_to"], true)) &&
               valid_topic(value["requested_topic"], "forum", true) &&
               valid_topic(value["effective_topic"], "forum", true) &&
               value["silent"].is_boolean() && valid_schedule(value["schedule"]) &&
               (value["observed_server_unix_time"].is_null() ||
                value["observed_server_unix_time"].is_number_integer());
    case AccountAuditOperation::MsgEdit:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "text"}) &&
               chat("chat") && valid_int53(value["message_id"], true) &&
               valid_string(value["text"]);
    case AccountAuditOperation::MsgDelete:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_ids",
                                    "requested_for_all", "effective_for_all"}) &&
               chat("chat") && valid_positive_int53_array(value["message_ids"]) &&
               value["requested_for_all"].is_boolean() && value["effective_for_all"].is_boolean();
    case AccountAuditOperation::MsgForward:
        return exact_fields(value, {"operation", "account", "tdlib_request", "from", "to",
                                    "message_ids", "drop_author"}) &&
               chat("from") && chat("to") && valid_positive_int53_array(value["message_ids"]) &&
               value["drop_author"].is_boolean();
    case AccountAuditOperation::MsgReact:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "reaction", "remove", "big"}) &&
               chat("chat") && valid_int53(value["message_id"], true) &&
               valid_string(value["reaction"], 64) && value["remove"].is_boolean() &&
               value["big"].is_boolean();
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "pinned"}) &&
               chat("chat") && valid_int53(value["message_id"], true) &&
               value["pinned"].is_boolean();
    case AccountAuditOperation::ChatMarkRead:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "last_message_id"}) &&
               chat("chat") &&
               (value["last_message_id"].is_null() || valid_int53(value["last_message_id"], true));
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "muted",
                                    "duration_seconds"}) &&
               chat("chat") && value["muted"].is_boolean() &&
               value["duration_seconds"].is_number_integer() &&
               value["duration_seconds"].get<std::int64_t>() >=
                   std::numeric_limits<std::int32_t>::min() &&
               value["duration_seconds"].get<std::int64_t>() <=
                   std::numeric_limits<std::int32_t>::max();
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "chat_list",
                                    "pinned"}) &&
               chat("chat") && (value["chat_list"] == "main" || value["chat_list"] == "archive") &&
               value["pinned"].is_boolean();
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "archived"}) &&
               chat("chat") && value["archived"].is_boolean();
    case AccountAuditOperation::ChatJoin:
        return exact_fields(value, {"operation", "account", "tdlib_request", "source", "chat",
                                    "invite_link_sha256"}) &&
               (value["source"] == "username" || value["source"] == "invite_link") &&
               (value["chat"].is_null() || valid_chat_identity(value["chat"])) &&
               (value["invite_link_sha256"].is_null() || valid_hash(value["invite_link_sha256"]));
    case AccountAuditOperation::ChatLeave:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat"}) &&
               chat("chat");
    case AccountAuditOperation::SavedAttach:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "effective_topic", "caption", "file"}) &&
               chat("chat") && valid_int53(value["message_id"], true) &&
               valid_topic(value["effective_topic"], "saved", false) &&
               valid_string(value["caption"]) && valid_file_snapshot(value["file"]);
    case AccountAuditOperation::SessionTerminate:
        return exact_fields(value, {"operation", "account", "tdlib_request", "session"}) &&
               valid_session_target(value["session"]);
    }
    return false;
}

bool arguments_match_plan(AccountAuditOperation operation, const json& arguments,
                          const json& plan) {
    switch (operation) {
    case AccountAuditOperation::Send:
        return arguments["text"] == plan["text"] && arguments["parse_mode"] == plan["parse_mode"] &&
               arguments["reply_to"] == plan["reply_to"] &&
               arguments["topic"] == plan["requested_topic"] &&
               arguments["silent"] == plan["silent"] && arguments["schedule"] == plan["schedule"];
    case AccountAuditOperation::MsgEdit:
        return arguments["message_id"] == plan["message_id"] && arguments["text"] == plan["text"];
    case AccountAuditOperation::MsgDelete:
        return arguments["message_ids"] == plan["message_ids"] &&
               arguments["for_all"] == plan["requested_for_all"];
    case AccountAuditOperation::MsgForward:
        return arguments["message_ids"] == plan["message_ids"] &&
               arguments["drop_author"] == plan["drop_author"];
    case AccountAuditOperation::MsgReact:
        return arguments["message_id"] == plan["message_id"] &&
               arguments["reaction"] == plan["reaction"] && arguments["remove"] == plan["remove"] &&
               arguments["big"] == plan["big"];
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return arguments["message_id"] == plan["message_id"] &&
               plan["pinned"] == (operation == AccountAuditOperation::MsgPin);
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return arguments["duration_seconds"] == plan["duration_seconds"] &&
               plan["muted"] == (operation == AccountAuditOperation::ChatMute);
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
        return plan["pinned"] == (operation == AccountAuditOperation::ChatPin);
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
        return plan["archived"] == (operation == AccountAuditOperation::ChatArchive);
    case AccountAuditOperation::ChatJoin:
        return arguments["source"] == plan["source"] &&
               (arguments["source"] == "username"
                    ? plan["tdlib_request"] == "joinChat" && plan["chat"].is_object() &&
                          plan["invite_link_sha256"].is_null()
                    : plan["tdlib_request"] == "joinChatByInviteLink" &&
                          plan["invite_link_sha256"] == arguments["invite_link_sha256"]);
    case AccountAuditOperation::SavedAttach:
        return arguments["message_id"] == plan["message_id"] &&
               arguments["caption"] == plan["caption"];
    case AccountAuditOperation::SessionTerminate:
        return arguments["session_id"] == plan["session"]["id"];
    case AccountAuditOperation::ChatMarkRead:
    case AccountAuditOperation::ChatLeave:
        return true;
    }
    return false;
}

bool valid_message_topic(const json& value) {
    if (value.is_null()) {
        return true;
    }
    if (!exact_fields(value, {"kind", "id"}) || !value["kind"].is_string()) {
        return false;
    }
    const auto& kind = value["kind"].get_ref<const std::string&>();
    if (kind != "forum" && kind != "thread" && kind != "direct" && kind != "saved") {
        return false;
    }
    return valid_int53(value["id"], true) &&
           (kind != "forum" ||
            value["id"].get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max());
}

bool valid_message_write_result(const json& value) {
    if (!exact_fields(value, {"id", "chat_id", "date", "sender", "is_outgoing", "topic", "type",
                              "text", "scheduled"}) ||
        !valid_int53(value["id"], true) || !valid_int53(value["chat_id"]) ||
        value["chat_id"] == 0 || !value["is_outgoing"].is_boolean() ||
        !valid_message_topic(value["topic"]) || !value["type"].is_string() ||
        !valid_string(value["text"], kMessageTextBytes) || !value["scheduled"].is_boolean()) {
        return false;
    }
    const auto& type = value["type"].get_ref<const std::string&>();
    if (type != "text" && type != "photo" && type != "video" && type != "doc" && type != "voice" &&
        type != "other") {
        return false;
    }
    const auto scalars = utf8_scalar_count(value["text"].get_ref<const std::string&>());
    if (!scalars || *scalars > kMessageTextScalars ||
        (value["date"].is_null() != value["scheduled"].get<bool>())) {
        return false;
    }
    if (!value["date"].is_null() && !valid_timestamp(value["date"])) {
        return false;
    }
    if (!value["sender"].is_object() || !exact_fields(value["sender"], {"type", "id"}) ||
        !value["sender"]["type"].is_string()) {
        return false;
    }
    if (value["sender"]["type"] == "user") {
        return valid_int53(value["sender"]["id"], true);
    }
    return value["sender"]["type"] == "chat" && valid_int53(value["sender"]["id"]) &&
           value["sender"]["id"] != 0;
}

bool valid_forward_item(const json& value);

bool valid_result_data(AccountAuditOperation operation, const json& value) {
    switch (operation) {
    case AccountAuditOperation::Send:
    case AccountAuditOperation::MsgEdit:
    case AccountAuditOperation::SavedAttach:
        return valid_message_write_result(value);
    case AccountAuditOperation::MsgDelete:
        return exact_fields(value, {"chat_id", "message_ids", "for_all", "deleted"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               valid_positive_int53_array(value["message_ids"]) && value["for_all"].is_boolean() &&
               value["deleted"] == true;
    case AccountAuditOperation::MsgForward:
        return exact_fields(value, {"from_chat_id", "to_chat_id", "items"}) &&
               valid_int53(value["from_chat_id"]) && value["from_chat_id"] != 0 &&
               valid_int53(value["to_chat_id"]) && value["to_chat_id"] != 0 &&
               value["items"].is_array() && !value["items"].empty() &&
               value["items"].size() <= kForwardItemCount &&
               std::all_of(value["items"].begin(), value["items"].end(), [](const json& item) {
                   return item.is_object() && item.value("status", std::string{}) == "sent" &&
                          valid_forward_item(item);
               });
    case AccountAuditOperation::MsgReact:
        return exact_fields(value, {"chat_id", "message_id", "reaction", "removed", "big"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               valid_int53(value["message_id"], true) && valid_string(value["reaction"]) &&
               value["removed"].is_boolean() && value["big"].is_boolean();
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return exact_fields(value, {"chat_id", "message_id", "pinned"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               valid_int53(value["message_id"], true) && value["pinned"].is_boolean();
    case AccountAuditOperation::ChatMarkRead:
        return exact_fields(value, {"chat_id", "last_read_message_id", "marked_read"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               (value["last_read_message_id"].is_null() ||
                valid_int53(value["last_read_message_id"], true)) &&
               value["marked_read"] == true;
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return exact_fields(value, {"chat_id", "muted", "duration_seconds"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               value["muted"].is_boolean() && value["duration_seconds"].is_number_integer() &&
               value["duration_seconds"].get<std::int64_t>() >=
                   std::numeric_limits<std::int32_t>::min() &&
               value["duration_seconds"].get<std::int64_t>() <=
                   std::numeric_limits<std::int32_t>::max();
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
        return exact_fields(value, {"chat_id", "chat_list", "pinned"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               (value["chat_list"] == "main" || value["chat_list"] == "archive") &&
               value["pinned"].is_boolean();
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
        return exact_fields(value, {"chat_id", "archived"}) && valid_int53(value["chat_id"]) &&
               value["chat_id"] != 0 && value["archived"].is_boolean();
    case AccountAuditOperation::ChatJoin:
        if (exact_fields(value, {"status", "chat_id"}) && value["status"] == "joined") {
            return valid_int53(value["chat_id"]) && value["chat_id"] != 0;
        }
        return exact_fields(value, {"status", "chat_id"}) && value["status"] == "request_sent" &&
               (value["chat_id"].is_null() ||
                (valid_int53(value["chat_id"]) && value["chat_id"] != 0));
    case AccountAuditOperation::ChatLeave:
        return exact_fields(value, {"chat_id", "left"}) && valid_int53(value["chat_id"]) &&
               value["chat_id"] != 0 && value["left"] == true;
    case AccountAuditOperation::SessionTerminate:
        return exact_fields(value, {"session_id", "terminated"}) &&
               valid_session_id(value["session_id"]) && value["terminated"] == true;
    }
    return false;
}

bool valid_terminal(AccountAuditOperation operation, const json& value) {
    if (exact_fields(value, {"kind", "data"})) {
        return value["kind"] == "result" && valid_result_data(operation, value["data"]);
    }
    return exact_fields(value, {"kind", "code", "message", "details", "exit_code"}) &&
           value["kind"] == "error" && valid_string(value["code"], 128) &&
           valid_string(value["message"]) && value["details"].is_object() &&
           value["exit_code"].is_number_integer();
}

std::uint64_t terminal_byte_ceiling(AccountAuditOperation operation) {
    if (operation == AccountAuditOperation::MsgForward) {
        return kForwardTerminalBytes;
    }
    if (operation == AccountAuditOperation::Send || operation == AccountAuditOperation::MsgEdit ||
        operation == AccountAuditOperation::SavedAttach) {
        return kSingleMessageTerminalBytes;
    }
    return kOtherTerminalBytes;
}

bool valid_forward_item(const json& value) {
    if (!value.is_object() || !value.contains("source_id") ||
        !valid_int53(value["source_id"], true) || !value.contains("status") ||
        !value["status"].is_string()) {
        return false;
    }
    if (value["status"] == "pending") {
        return exact_fields(value, {"source_id", "status", "temporary_message_id"}) &&
               valid_int53(value["temporary_message_id"]);
    }
    if (value["status"] == "sent") {
        return exact_fields(value, {"source_id", "status", "message"}) &&
               valid_message_write_result(value["message"]);
    }
    if (value["status"] != "failed" ||
        !exact_fields(value,
                      {"source_id", "status", "failure_reason", "tdlib_code", "retry_after"}) ||
        !value["failure_reason"].is_string()) {
        return false;
    }
    const auto& reason = value["failure_reason"].get_ref<const std::string&>();
    if (reason != "upstream_null" && reason != "tdlib_error" &&
        reason != "deleted_before_confirmation") {
        return false;
    }
    const bool code_valid =
        value["tdlib_code"].is_null() || value["tdlib_code"].is_number_integer();
    const bool retry_valid =
        value["retry_after"].is_null() ||
        (value["retry_after"].is_number_integer() && value["retry_after"].get<std::int64_t>() >= 0);
    if (!code_valid || !retry_valid) {
        return false;
    }
    if (reason != "tdlib_error") {
        return value["tdlib_code"].is_null() && value["retry_after"].is_null();
    }
    if (value["tdlib_code"].is_null()) {
        return false;
    }
    return value["tdlib_code"] == 429 ? !value["retry_after"].is_null()
                                      : value["retry_after"].is_null();
}

bool valid_checkpoint_data(AccountAuditOperation operation, AccountAuditStage stage,
                           const json& value) {
    switch (stage) {
    case AccountAuditStage::IdempotencyPending:
        return operation != AccountAuditOperation::SessionTerminate &&
               exact_fields(value, {"key_hash", "request_fingerprint", "expires_at",
                                    "reserved_terminal_bytes"}) &&
               valid_hash(value["key_hash"]) && valid_hash(value["request_fingerprint"]) &&
               value["expires_at"].is_number_unsigned() &&
               value["reserved_terminal_bytes"].is_number_unsigned();
    case AccountAuditStage::SpoolReady:
        return operation == AccountAuditOperation::SavedAttach &&
               exact_fields(value, {"file", "relative_path"}) &&
               valid_file_snapshot(value["file"]) && valid_string(value["relative_path"], 4'096);
    case AccountAuditStage::DispatchStarted:
        return exact_fields(value, {"tdlib_function", "dispatch_token", "client_generation"}) &&
               value["tdlib_function"].is_string() &&
               !value["tdlib_function"].get_ref<const std::string&>().empty() &&
               expected_tdlib_functions(operation).contains(
                   value["tdlib_function"].get_ref<const std::string&>()) &&
               value["dispatch_token"].is_string() &&
               valid_hex(value["dispatch_token"].get_ref<const std::string&>(), 32) &&
               value["client_generation"].is_number_unsigned();
    case AccountAuditStage::TemporaryIdsObserved:
        return (operation == AccountAuditOperation::Send ||
                operation == AccountAuditOperation::SavedAttach ||
                operation == AccountAuditOperation::MsgForward) &&
               exact_fields(value, {"temporary_message_ids"}) &&
               valid_int53_array(value["temporary_message_ids"]);
    case AccountAuditStage::ForwardProgress:
        return operation == AccountAuditOperation::MsgForward && exact_fields(value, {"items"}) &&
               value["items"].is_array() && !value["items"].empty() &&
               value["items"].size() <= kForwardItemCount &&
               std::all_of(value["items"].begin(), value["items"].end(), valid_forward_item);
    case AccountAuditStage::MutationConfirmed:
        return exact_fields(value, {"terminal"}) && valid_terminal(operation, value["terminal"]);
    }
    return false;
}

bool destructive_operation(AccountAuditOperation operation) {
    return operation == AccountAuditOperation::MsgDelete ||
           operation == AccountAuditOperation::ChatLeave ||
           operation == AccountAuditOperation::SessionTerminate;
}

bool valid_common_identity(const json& value) {
    return value.contains("invocation_id") && value["invocation_id"].is_string() &&
           valid_hex(value["invocation_id"].get_ref<const std::string&>(), 32) &&
           value.contains("timestamp") && valid_timestamp(value["timestamp"]) &&
           value.contains("account") && value["account"].is_string() &&
           paths::valid_account_name(value["account"].get_ref<const std::string&>()) &&
           value.contains("command") && value["command"].is_string() &&
           parse_account_audit_operation(value["command"].get_ref<const std::string&>())
               .has_value();
}

bool validate_intent_impl(const json& document, std::string& error) {
    if (!exact_fields(document,
                      {"schema_version", "phase", "invocation_id", "timestamp", "account",
                       "command", "arguments", "plan", "request_fingerprint", "config_snapshot",
                       "authority_source", "confirmation_source", "idempotency_key_hash"}) ||
        document["schema_version"] != 2 || document["phase"] != "intent" ||
        !valid_common_identity(document)) {
        error = "invalid v2 intent envelope";
        return false;
    }
    const auto parsed_operation =
        parse_account_audit_operation(document["command"].get_ref<const std::string&>());
    if (!parsed_operation) {
        error = "invalid v2 intent operation";
        return false;
    }
    const auto operation = *parsed_operation;
    const bool confirmation_valid =
        destructive_operation(operation)
            ? (document["confirmation_source"] == "yes" || document["confirmation_source"] == "tty")
            : document["confirmation_source"].is_null();
    const bool key_valid =
        document["idempotency_key_hash"].is_null() || valid_hash(document["idempotency_key_hash"]);
    if (!valid_arguments(operation, document["arguments"]) ||
        !valid_plan(operation, document["plan"],
                    document["account"].get_ref<const std::string&>()) ||
        !arguments_match_plan(operation, document["arguments"], document["plan"]) ||
        !valid_hash(document["request_fingerprint"]) || !document["config_snapshot"].is_string() ||
        !proto::valid_config_snapshot_identity(
            document["config_snapshot"].get_ref<const std::string&>(), false) ||
        (document["authority_source"] != "request" && document["authority_source"] != "config") ||
        !confirmation_valid || !key_valid ||
        (operation == AccountAuditOperation::SessionTerminate &&
         !document["idempotency_key_hash"].is_null())) {
        error = "invalid v2 intent fields";
        return false;
    }
    if (serialize_account_audit_record(document).size() > kIntentJsonBytes) {
        error = "v2 intent exceeds its byte ceiling";
        return false;
    }
    error.clear();
    return true;
}

bool validate_checkpoint_impl(const json& document, std::string& error) {
    if (!exact_fields(document, {"schema_version", "phase", "invocation_id", "timestamp", "account",
                                 "command", "checkpoint_sequence", "stage", "data"}) ||
        document["schema_version"] != 2 || document["phase"] != "checkpoint" ||
        !valid_common_identity(document) || !document["checkpoint_sequence"].is_number_unsigned() ||
        document["checkpoint_sequence"].get<std::uint64_t>() == 0 ||
        document["checkpoint_sequence"].get<std::uint64_t>() >
            std::numeric_limits<std::uint32_t>::max() ||
        !document["stage"].is_string()) {
        error = "invalid v2 checkpoint envelope";
        return false;
    }
    const auto parsed_operation =
        parse_account_audit_operation(document["command"].get_ref<const std::string&>());
    if (!parsed_operation) {
        error = "invalid v2 checkpoint operation";
        return false;
    }
    const auto operation = *parsed_operation;
    const auto stage = parse_account_audit_stage(document["stage"].get_ref<const std::string&>());
    if (!stage || !valid_checkpoint_data(operation, *stage, document["data"])) {
        error = "invalid v2 checkpoint data";
        return false;
    }
    if (*stage == AccountAuditStage::IdempotencyPending &&
        (document["data"]["expires_at"].get<std::uint64_t>() > 253'402'300'799ULL ||
         document["data"]["reserved_terminal_bytes"].get<std::uint64_t>() >
             std::numeric_limits<std::uint32_t>::max())) {
        error = "v2 idempotency checkpoint integer is out of range";
        return false;
    }
    if (*stage == AccountAuditStage::SpoolReady) {
        const auto expected = "spool/" + document["invocation_id"].get<std::string>() + "/" +
                              document["data"]["file"]["name"].get<std::string>();
        if (document["data"]["relative_path"] != expected) {
            error = "v2 spool checkpoint path is not canonical";
            return false;
        }
    }
    if (*stage == AccountAuditStage::MutationConfirmed &&
        serialize_account_audit_record(document["data"]["terminal"]).size() >
            terminal_byte_ceiling(operation)) {
        error = "v2 mutation terminal exceeds its operation ceiling";
        return false;
    }
    const auto maximum = *stage == AccountAuditStage::ForwardProgress ||
                                 *stage == AccountAuditStage::MutationConfirmed
                             ? kVectorJsonBytes
                             : kNonVectorJsonBytes;
    if (serialize_account_audit_record(document).size() > maximum) {
        error = "v2 checkpoint exceeds its byte ceiling";
        return false;
    }
    error.clear();
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed outcome/stage relations.
bool validate_outcome_impl(const json& document, std::string& error) {
    if (!exact_fields(document,
                      {"schema_version", "phase", "invocation_id", "timestamp", "account",
                       "command", "success", "mutation_state", "completed_stages", "terminal"}) ||
        document["schema_version"] != 2 || document["phase"] != "outcome" ||
        !valid_common_identity(document) || !document["success"].is_boolean() ||
        !document["mutation_state"].is_string() || !document["completed_stages"].is_array() ||
        !valid_terminal(
            parse_account_audit_operation(document["command"].get<std::string>()).value(),
            document["terminal"])) {
        error = "invalid v2 outcome envelope";
        return false;
    }
    const auto& mutation = document["mutation_state"].get_ref<const std::string&>();
    if (mutation != "none" && mutation != "possible" && mutation != "confirmed") {
        error = "invalid v2 outcome mutation state";
        return false;
    }
    std::set<AccountAuditStage> unique;
    std::vector<AccountAuditStage> completed;
    for (const auto& item : document["completed_stages"]) {
        if (!item.is_string()) {
            error = "invalid v2 outcome stage";
            return false;
        }
        const auto stage = parse_account_audit_stage(item.get_ref<const std::string&>());
        if (!stage || !unique.emplace(*stage).second) {
            error = "invalid v2 outcome stage";
            return false;
        }
        completed.push_back(*stage);
    }
    const auto operation =
        parse_account_audit_operation(document["command"].get_ref<const std::string&>()).value();
    const auto is_prefix = [&completed](const std::vector<AccountAuditStage>& complete) {
        return completed.size() <= complete.size() &&
               std::equal(completed.begin(), completed.end(), complete.begin());
    };
    bool legal_prefix = completed.empty();
    for (const bool keyed : {false, true}) {
        for (const bool temporary : {false, true}) {
            for (const bool progress : {false, true}) {
                for (const bool mutation_proof : {false, true}) {
                    std::vector<AccountAuditStage> complete;
                    if (keyed && operation != AccountAuditOperation::SessionTerminate) {
                        complete.push_back(AccountAuditStage::IdempotencyPending);
                    }
                    if (operation == AccountAuditOperation::SavedAttach) {
                        complete.push_back(AccountAuditStage::SpoolReady);
                    }
                    complete.push_back(AccountAuditStage::DispatchStarted);
                    if (temporary && (operation == AccountAuditOperation::Send ||
                                      operation == AccountAuditOperation::SavedAttach ||
                                      operation == AccountAuditOperation::MsgForward)) {
                        complete.push_back(AccountAuditStage::TemporaryIdsObserved);
                    }
                    if (progress && operation == AccountAuditOperation::MsgForward) {
                        complete.push_back(AccountAuditStage::ForwardProgress);
                    }
                    if (mutation_proof) {
                        complete.push_back(AccountAuditStage::MutationConfirmed);
                    }
                    legal_prefix = legal_prefix || is_prefix(complete);
                }
            }
        }
    }
    const bool has_dispatch = unique.contains(AccountAuditStage::DispatchStarted);
    const bool success = document["success"].get<bool>();
    if (!legal_prefix || (document["terminal"]["kind"] == "result") != success ||
        (success &&
         (mutation != "confirmed" || !unique.contains(AccountAuditStage::MutationConfirmed))) ||
        ((mutation == "possible" || mutation == "confirmed") && !has_dispatch) ||
        serialize_account_audit_record(document["terminal"]).size() >
            terminal_byte_ceiling(operation) ||
        serialize_account_audit_record(document).size() > kVectorJsonBytes) {
        error = "invalid v2 outcome terminal";
        return false;
    }
    error.clear();
    return true;
}

std::vector<AccountAuditStage> allowed_order(AccountAuditOperation operation) {
    if (operation == AccountAuditOperation::SavedAttach) {
        return {AccountAuditStage::IdempotencyPending, AccountAuditStage::SpoolReady,
                AccountAuditStage::DispatchStarted, AccountAuditStage::TemporaryIdsObserved,
                AccountAuditStage::MutationConfirmed};
    }
    if (operation == AccountAuditOperation::MsgForward) {
        return {AccountAuditStage::IdempotencyPending, AccountAuditStage::DispatchStarted,
                AccountAuditStage::TemporaryIdsObserved, AccountAuditStage::ForwardProgress,
                AccountAuditStage::MutationConfirmed};
    }
    if (operation == AccountAuditOperation::Send) {
        return {AccountAuditStage::IdempotencyPending, AccountAuditStage::DispatchStarted,
                AccountAuditStage::TemporaryIdsObserved, AccountAuditStage::MutationConfirmed};
    }
    if (operation == AccountAuditOperation::SessionTerminate) {
        return {AccountAuditStage::DispatchStarted, AccountAuditStage::MutationConfirmed};
    }
    return {AccountAuditStage::IdempotencyPending, AccountAuditStage::DispatchStarted,
            AccountAuditStage::MutationConfirmed};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact six-stage automaton.
bool transition_history(AccountAuditOperation operation,
                        const std::vector<AccountAuditCheckpointInput>& history,
                        std::string& error) {
    const auto order = allowed_order(operation);
    std::uint32_t previous_sequence = 0;
    std::optional<std::size_t> previous_position;
    std::optional<json> previous_forward;
    std::size_t progress_count = 0;
    bool saw_idempotency = false;
    bool saw_spool = false;
    bool saw_dispatch = false;
    bool saw_temporary_ids = false;
    bool saw_mutation = false;
    for (const auto& checkpoint : history) {
        if (checkpoint.operation != operation ||
            checkpoint.checkpoint_sequence <= previous_sequence ||
            !valid_checkpoint_data(operation, checkpoint.stage, checkpoint.data)) {
            error = "invalid v2 checkpoint history identity or sequence";
            return false;
        }
        previous_sequence = checkpoint.checkpoint_sequence;
        const auto found = std::find(order.begin(), order.end(), checkpoint.stage);
        if (found == order.end()) {
            error = "illegal v2 checkpoint stage";
            return false;
        }
        const auto position = static_cast<std::size_t>(std::distance(order.begin(), found));
        if (previous_position && position < *previous_position) {
            error = "v2 checkpoint stage regression";
            return false;
        }
        if (previous_position && position == *previous_position &&
            checkpoint.stage != AccountAuditStage::ForwardProgress) {
            error = "repeated non-forward checkpoint stage";
            return false;
        }
        if (checkpoint.stage == AccountAuditStage::IdempotencyPending &&
            operation == AccountAuditOperation::SessionTerminate) {
            error = "session terminate cannot use idempotency";
            return false;
        }
        switch (checkpoint.stage) {
        case AccountAuditStage::IdempotencyPending:
            if (previous_position || saw_idempotency) {
                error = "idempotency checkpoint is not first";
                return false;
            }
            saw_idempotency = true;
            break;
        case AccountAuditStage::SpoolReady:
            if (operation != AccountAuditOperation::SavedAttach || saw_spool || saw_dispatch ||
                (previous_position && !saw_idempotency)) {
                error = "spool checkpoint has an illegal predecessor";
                return false;
            }
            saw_spool = true;
            break;
        case AccountAuditStage::DispatchStarted:
            if (saw_dispatch || saw_mutation ||
                (operation == AccountAuditOperation::SavedAttach && !saw_spool) ||
                (previous_position && !saw_idempotency && !saw_spool)) {
                error = "dispatch checkpoint has an illegal predecessor";
                return false;
            }
            saw_dispatch = true;
            break;
        case AccountAuditStage::TemporaryIdsObserved:
            if (!saw_dispatch || saw_temporary_ids || saw_mutation) {
                error = "temporary ids checkpoint has an illegal predecessor";
                return false;
            }
            saw_temporary_ids = true;
            break;
        case AccountAuditStage::ForwardProgress:
            if (!saw_dispatch || saw_mutation) {
                error = "forward progress has an illegal predecessor";
                return false;
            }
            break;
        case AccountAuditStage::MutationConfirmed:
            if (!saw_dispatch || saw_mutation) {
                error = "mutation proof has an illegal predecessor";
                return false;
            }
            saw_mutation = true;
            break;
        }
        if (checkpoint.stage == AccountAuditStage::ForwardProgress) {
            if (++progress_count > kMaximumForwardProgressRecords) {
                error = "too many forward progress records";
                return false;
            }
            const auto& current = checkpoint.data["items"];
            if (previous_forward) {
                if (current.size() != previous_forward->size()) {
                    error = "forward vector size changed";
                    return false;
                }
                bool advanced = false;
                for (std::size_t index = 0; index < current.size(); ++index) {
                    const auto& before = previous_forward->at(index);
                    const auto& after = current.at(index);
                    if (before["source_id"] != after["source_id"] ||
                        (before["status"] != "pending" && before != after)) {
                        error = "forward vector regressed or changed terminal item";
                        return false;
                    }
                    if (before["status"] == "pending" && after["status"] == "pending" &&
                        before != after) {
                        error = "forward pending item changed without becoming terminal";
                        return false;
                    }
                    advanced = advanced || before != after;
                }
                if (!advanced) {
                    error = "forward progress did not advance";
                    return false;
                }
            }
            previous_forward = current;
        }
        previous_position = position;
    }
    error.clear();
    return true;
}

bool write_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool valid_directory_metadata(int fd, uid_t uid, AccountAuditFailure& failure) {
    struct stat metadata {};
    if (::fstat(fd, &metadata) != 0) {
        failure.reason = AccountAuditDurabilityReason::OpenFailed;
        return false;
    }
    if (!S_ISDIR(metadata.st_mode)) {
        failure.reason = AccountAuditDurabilityReason::WrongType;
        return false;
    }
    if (metadata.st_uid != uid) {
        failure.reason = AccountAuditDurabilityReason::WrongOwner;
        return false;
    }
    if ((metadata.st_mode & 07777) != 0700) {
        failure.reason = AccountAuditDurabilityReason::WrongMode;
        return false;
    }
    return true;
}

bool valid_file_metadata(const struct stat& metadata, uid_t uid, AccountAuditFailure& failure) {
    if (!S_ISREG(metadata.st_mode)) {
        failure.reason = AccountAuditDurabilityReason::WrongType;
        return false;
    }
    if (metadata.st_uid != uid) {
        failure.reason = AccountAuditDurabilityReason::WrongOwner;
        return false;
    }
    if ((metadata.st_mode & 07777) != 0600) {
        failure.reason = AccountAuditDurabilityReason::WrongMode;
        return false;
    }
    if (metadata.st_nlink != 1) {
        failure.reason = AccountAuditDurabilityReason::WrongLinkCount;
        return false;
    }
    return true;
}

bool injected(const std::shared_ptr<const testing::AccountAuditHooks>& hooks,
              AccountAuditFault fault) {
    return hooks && hooks->should_fail && hooks->should_fail(fault);
}

bool exclusive_rename(int directory, const char* source, const char* destination) {
#if defined(__linux__)
    return ::syscall(SYS_renameat2, directory, source, directory, destination, RENAME_NOREPLACE) ==
           0;
#elif defined(__APPLE__)
    return ::renameatx_np(directory, source, directory, destination, RENAME_EXCL) == 0;
#else
#error Unsupported platform for exclusive account-audit rotation
#endif
}

struct SegmentIdentity {
    std::string name;
    bool present = false;
    std::uint64_t inode = 0;
};

std::optional<std::vector<SegmentIdentity>> inspect_segments(int directory, uid_t uid,
                                                             AccountAuditFailure& failure) {
    std::vector<SegmentIdentity> result;
    std::set<std::uint64_t> inodes;
    for (const auto* name :
         {"audit.log", "audit.log.1", "audit.log.2", "audit.log.3", "audit.log.4"}) {
        struct stat metadata {};
        if (::fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                result.push_back({name, false, 0});
                continue;
            }
            failure.reason = AccountAuditDurabilityReason::OpenFailed;
            return std::nullopt;
        }
        if (!valid_file_metadata(metadata, uid, failure) || metadata.st_size < 0 ||
            static_cast<std::uint64_t>(metadata.st_size) > kMaximumSegmentBytes) {
            if (metadata.st_size < 0 ||
                static_cast<std::uint64_t>(metadata.st_size) > kMaximumSegmentBytes) {
                failure.reason = AccountAuditDurabilityReason::PathInvalid;
            }
            return std::nullopt;
        }
        const auto inode = static_cast<std::uint64_t>(metadata.st_ino);
        if (!inodes.emplace(inode).second) {
            failure.reason = AccountAuditDurabilityReason::PathInvalid;
            return std::nullopt;
        }
        result.push_back({name, true, inode});
    }
    return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): bounded streaming JSONL validation.
bool segment_contains_pin_relation(int directory, std::string_view name, uid_t uid,
                                   const AccountAuditPin& pin, AccountAuditFailure& failure) {
    const Descriptor file(
        ::openat(directory, std::string(name).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat metadata {};
    if (file.get() < 0 || ::fstat(file.get(), &metadata) != 0) {
        failure.reason = AccountAuditDurabilityReason::OpenFailed;
        return false;
    }
    if (!valid_file_metadata(metadata, uid, failure) ||
        static_cast<std::uint64_t>(metadata.st_ino) != pin.audit_generation) {
        failure.reason = AccountAuditDurabilityReason::Contradiction;
        return false;
    }
    std::string line;
    std::array<char, kIoChunkBytes> chunk{};
    for (;;) {
        const auto count = ::read(file.get(), chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            failure.reason = AccountAuditDurabilityReason::ReadFailed;
            return false;
        }
        if (count == 0) {
            break;
        }
        for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
            if (chunk.at(index) != '\n') {
                if (line.size() == kIntentJsonBytes) {
                    failure.reason = AccountAuditDurabilityReason::PathInvalid;
                    return false;
                }
                line.push_back(chunk.at(index));
                continue;
            }
            auto document = json::parse(line, nullptr, false);
            if (!document.is_discarded() && document.is_object() &&
                document.value("schema_version", 0) == 2 &&
                document.value("phase", std::string{}) == "intent" &&
                document.value("invocation_id", std::string{}) == pin.invocation_id &&
                document.value("request_fingerprint", std::string{}) == pin.request_fingerprint &&
                document.value("command", std::string{}) ==
                    account_audit_operation_name(pin.operation)) {
                return true;
            }
            line.clear();
        }
    }
    if (!line.empty()) {
        failure.reason = AccountAuditDurabilityReason::Contradiction;
        return false;
    }
    failure.reason = AccountAuditDurabilityReason::Contradiction;
    return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact hole-first crash automaton.
bool rotate_for_intent(int directory, uid_t uid, std::uint64_t incoming,
                       const AccountAuditPinSource& pins,
                       const std::shared_ptr<const testing::AccountAuditHooks>& hooks,
                       AccountAuditFailure& failure) {
    if (const auto* unavailable = std::get_if<UnavailableAccountAuditPins>(&pins)) {
        failure.reason = unavailable->reason;
        return false;
    }
    auto segments = inspect_segments(directory, uid, failure);
    if (!segments) {
        return false;
    }
    auto& active = segments->at(0);
    if (!active.present) {
        return true;
    }
    struct stat active_metadata {};
    if (::fstatat(directory, "audit.log", &active_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        active_metadata.st_size < 0) {
        failure.reason = AccountAuditDurabilityReason::OpenFailed;
        return false;
    }
    const auto threshold = hooks ? hooks->rotation_bytes : kRotationBytes;
    const auto active_size = static_cast<std::uint64_t>(active_metadata.st_size);
    if (active_size == 0 || (incoming <= threshold && active_size <= threshold - incoming)) {
        return true;
    }

    std::size_t hole = 0;
    for (std::size_t index = 1; index < segments->size(); ++index) {
        if (!segments->at(index).present) {
            hole = index;
            break;
        }
    }
    if (hole == 0) {
        if (!std::holds_alternative<KnownAccountAuditPins>(pins)) {
            failure.reason = AccountAuditDurabilityReason::CapacityExhausted;
            return false;
        }
        std::set<std::uint64_t> pinned_inodes;
        for (const auto& pin : std::get<KnownAccountAuditPins>(pins).pins) {
            auto found = std::find_if(segments->begin(), segments->end(), [&pin](const auto& item) {
                return item.present && item.inode == pin.audit_generation;
            });
            if (found == segments->end() ||
                !segment_contains_pin_relation(directory, found->name, uid, pin, failure)) {
                failure.reason = AccountAuditDurabilityReason::Contradiction;
                return false;
            }
            pinned_inodes.insert(pin.audit_generation);
        }
        for (std::size_t index = segments->size() - 1; index > 0; --index) {
            if (!pinned_inodes.contains(segments->at(index).inode)) {
                hole = index;
                break;
            }
        }
        if (hole == 0) {
            failure.reason = AccountAuditDurabilityReason::CapacityExhausted;
            return false;
        }
        if (injected(hooks, AccountAuditFault::Unlink) ||
            ::unlinkat(directory, segments->at(hole).name.c_str(), 0) != 0) {
            failure.reason = AccountAuditDurabilityReason::RenameFailed;
            return false;
        }
        if (hooks && hooks->after_rotation_step) {
            hooks->after_rotation_step("unlink");
        }
    }

    for (std::size_t index = hole; index > 1; --index) {
        const auto source = "audit.log." + std::to_string(index - 1);
        const auto destination = "audit.log." + std::to_string(index);
        if (injected(hooks, AccountAuditFault::Rename) ||
            !exclusive_rename(directory, source.c_str(), destination.c_str())) {
            failure.reason = AccountAuditDurabilityReason::RenameFailed;
            return false;
        }
        if (hooks && hooks->after_rotation_step) {
            hooks->after_rotation_step(destination);
        }
    }
    if (injected(hooks, AccountAuditFault::Rename) ||
        !exclusive_rename(directory, "audit.log", "audit.log.1")) {
        failure.reason = AccountAuditDurabilityReason::RenameFailed;
        return false;
    }
    if (hooks && hooks->after_rotation_step) {
        hooks->after_rotation_step("audit.log.1");
    }
    if (injected(hooks, AccountAuditFault::DirectorySync) || ::fsync(directory) != 0) {
        failure.reason = AccountAuditDurabilityReason::DirectorySyncFailed;
        return false;
    }
    return true;
}

json incomplete_terminal(std::string_view account, std::string_view path,
                         AccountAuditMutationState mutation,
                         const std::vector<AccountAuditStage>& completed) {
    json stages = json::array();
    for (const auto stage : completed) {
        stages.push_back(account_audit_stage_name(stage));
    }
    return {{"kind", "error"},
            {"code", "AUDIT_INCOMPLETE"},
            {"message", "a prior audited invocation did not reach a terminal proof"},
            {"details",
             {{"account", account},
              {"path", path},
              {"mutation_state", account_audit_mutation_state_name(mutation)},
              {"completed_stages", std::move(stages)}}},
            {"exit_code", 1}};
}

bool forward_vector_complete(const json& items);

std::optional<json> derive_forward_terminal(const AccountAuditOpenGroup& group,
                                            std::string& error) {
    const auto found = std::find_if(
        group.checkpoints.rbegin(), group.checkpoints.rend(),
        [](const json& checkpoint) { return checkpoint["stage"] == "forward_progress"; });
    if (found == group.checkpoints.rend() || !forward_vector_complete((*found)["data"]["items"])) {
        error = "terminal forward vector is unavailable";
        return std::nullopt;
    }
    const auto& items = (*found)["data"]["items"];
    const auto from = group.intent["plan"]["from"]["id"];
    const auto to = group.intent["plan"]["to"]["id"];
    const auto sent = std::count_if(items.begin(), items.end(),
                                    [](const json& item) { return item["status"] == "sent"; });
    if (sent == static_cast<std::ptrdiff_t>(items.size())) {
        return json{{"kind", "result"},
                    {"data", {{"from_chat_id", from}, {"to_chat_id", to}, {"items", items}}}};
    }
    if (sent > 0) {
        return json{{"kind", "error"},
                    {"code", "FORWARD_PARTIAL"},
                    {"message", "some messages could not be forwarded"},
                    {"details",
                     {{"operation", "msg_forward"},
                      {"from_chat_id", from},
                      {"to_chat_id", to},
                      {"items", items}}},
                    {"exit_code", 1}};
    }
    const bool all_rate_limited = std::all_of(items.begin(), items.end(), [](const json& item) {
        return item["failure_reason"] == "tdlib_error" && item["tdlib_code"] == 429 &&
               !item["retry_after"].is_null();
    });
    if (all_rate_limited) {
        std::int64_t retry_after = 0;
        for (const auto& item : items) {
            retry_after = std::max(retry_after, item["retry_after"].get<std::int64_t>());
        }
        return json{{"kind", "error"},
                    {"code", "RATE_LIMITED"},
                    {"message", "Telegram rate limit exceeded"},
                    {"details",
                     {{"operation", "msg_forward"},
                      {"tdlib_code", 429},
                      {"retry_after", retry_after},
                      {"items", items}}},
                    {"exit_code", 5}};
    }
    return json{{"kind", "error"},
                {"code", "FORWARD_FAILED"},
                {"message", "messages could not be forwarded"},
                {"details",
                 {{"operation", "msg_forward"},
                  {"from_chat_id", from},
                  {"to_chat_id", to},
                  {"items", items}}},
                {"exit_code", 1}};
}

struct ScanState {
    std::optional<AccountAuditOpenGroup> open;
    bool positive_v2 = false;
};

constexpr std::array<const char*, 5> kSegmentNames{"audit.log.4", "audit.log.3", "audit.log.2",
                                                   "audit.log.1", "audit.log"};

std::optional<bool>
// NOLINTNEXTLINE(readability-function-cognitive-complexity): bounded five-segment rescan.
prior_invocation_seen(int directory, uid_t uid, std::size_t current_segment,
                      std::uint64_t current_record_offset, std::string_view invocation,
                      const std::shared_ptr<const testing::AccountAuditHooks>& hooks,
                      AccountAuditFailure& failure) {
    std::array<char, kIoChunkBytes> chunk{};
    for (std::size_t segment = 0; segment <= current_segment; ++segment) {
        struct stat named_metadata {};
        if (::fstatat(directory, kSegmentNames.at(segment), &named_metadata, AT_SYMLINK_NOFOLLOW) !=
            0) {
            if (errno == ENOENT) {
                continue;
            }
            failure = {AccountAuditDurabilityReason::OpenFailed, {}};
            return std::nullopt;
        }
        if (!valid_file_metadata(named_metadata, uid, failure)) {
            return std::nullopt;
        }
        if (named_metadata.st_size < 0 ||
            static_cast<std::uint64_t>(named_metadata.st_size) > kMaximumSegmentBytes) {
            failure.reason = AccountAuditDurabilityReason::PathInvalid;
            return std::nullopt;
        }
        const Descriptor file(
            ::openat(directory, kSegmentNames.at(segment), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat descriptor_metadata {};
        if (file.get() < 0 || ::fstat(file.get(), &descriptor_metadata) != 0 ||
            descriptor_metadata.st_dev != named_metadata.st_dev ||
            descriptor_metadata.st_ino != named_metadata.st_ino ||
            descriptor_metadata.st_size != named_metadata.st_size) {
            failure = {AccountAuditDurabilityReason::OpenFailed, {}};
            return std::nullopt;
        }
        const auto limit = segment == current_segment
                               ? current_record_offset
                               : static_cast<std::uint64_t>(descriptor_metadata.st_size);
        std::string line;
        line.reserve(static_cast<std::size_t>(std::min(limit, kIoChunkBytes)));
        std::uint64_t offset = 0;
        while (offset < limit) {
            if (injected(hooks, AccountAuditFault::Read)) {
                failure = {AccountAuditDurabilityReason::ReadFailed, {}};
                return std::nullopt;
            }
            const auto count = ::pread(
                file.get(), chunk.data(),
                static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), limit - offset)),
                static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                failure = {AccountAuditDurabilityReason::ReadFailed, {}};
                return std::nullopt;
            }
            offset += static_cast<std::uint64_t>(count);
            for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
                if (chunk.at(index) != '\n') {
                    if (line.size() == kIntentJsonBytes) {
                        failure = {AccountAuditDurabilityReason::PathInvalid, {}};
                        return std::nullopt;
                    }
                    line.push_back(chunk.at(index));
                    continue;
                }
                const auto document = json::parse(line, nullptr, false);
                if (document.is_discarded()) {
                    failure = {AccountAuditDurabilityReason::ParseError, {}};
                    return std::nullopt;
                }
                if (document.is_object() && document.contains("invocation_id") &&
                    document["invocation_id"].is_string() &&
                    document["invocation_id"].get_ref<const std::string&>() == invocation) {
                    return true;
                }
                line.clear();
            }
        }
        if (!line.empty()) {
            failure = {AccountAuditDurabilityReason::ParseError, {}};
            return std::nullopt;
        }
        struct stat final_metadata {};
        if (::fstat(file.get(), &final_metadata) != 0 ||
            final_metadata.st_dev != descriptor_metadata.st_dev ||
            final_metadata.st_ino != descriptor_metadata.st_ino ||
            final_metadata.st_size != descriptor_metadata.st_size) {
            failure = {AccountAuditDurabilityReason::ReadFailed, {}};
            return std::nullopt;
        }
    }
    return false;
}

bool forward_vector_complete(const json& items) {
    return std::none_of(items.begin(), items.end(),
                        [](const json& item) { return item["status"] == "pending"; });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): strict v2 group automaton.
bool consume_v2(const json& document, ScanState& state, std::string& error) {
    const auto invocation = document.value("invocation_id", std::string{});
    const auto phase = document.value("phase", std::string{});
    if (phase == "intent") {
        if (state.open || !validate_intent_impl(document, error)) {
            error = error.empty() ? "v2 intent contradicts chronology" : error;
            return false;
        }
        AccountAuditOpenGroup group;
        group.intent = document;
        group.keyed = !document["idempotency_key_hash"].is_null();
        state.open = std::move(group);
        return true;
    }
    if (!state.open || state.open->intent.value("invocation_id", std::string{}) != invocation ||
        state.open->intent.value("command", std::string{}) !=
            document.value("command", std::string{}) ||
        state.open->intent.value("account", std::string{}) !=
            document.value("account", std::string{})) {
        error = "v2 record has no matching open group";
        return false;
    }
    if (phase == "checkpoint") {
        if (!validate_checkpoint_impl(document, error)) {
            return false;
        }
        AccountAuditCheckpointInput checkpoint;
        checkpoint.identity = {invocation, document.value("timestamp", std::string{})};
        checkpoint.account = document.value("account", std::string{});
        const auto operation =
            parse_account_audit_operation(document.value("command", std::string{}));
        const auto stage = parse_account_audit_stage(document.value("stage", std::string{}));
        if (!operation || !stage) {
            error = "validated v2 checkpoint lost its enum identity";
            return false;
        }
        checkpoint.operation = *operation;
        checkpoint.checkpoint_sequence = document["checkpoint_sequence"].get<std::uint32_t>();
        checkpoint.stage = *stage;
        checkpoint.data = document["data"];
        if (checkpoint.stage == AccountAuditStage::IdempotencyPending) {
            if (state.open->intent["idempotency_key_hash"].is_null() ||
                checkpoint.data["key_hash"] != state.open->intent["idempotency_key_hash"] ||
                checkpoint.data["request_fingerprint"] !=
                    state.open->intent["request_fingerprint"]) {
                error = "idempotency checkpoint contradicts the intent";
                return false;
            }
        }
        if (checkpoint.stage == AccountAuditStage::SpoolReady &&
            checkpoint.data["file"] != state.open->intent["plan"]["file"]) {
            error = "spool checkpoint file contradicts the intent";
            return false;
        }
        if (checkpoint.stage == AccountAuditStage::TemporaryIdsObserved) {
            const auto expected_count = checkpoint.operation == AccountAuditOperation::MsgForward
                                            ? state.open->intent["arguments"]["message_ids"].size()
                                            : std::size_t{1};
            if (checkpoint.data["temporary_message_ids"].size() != expected_count) {
                error = "temporary id vector does not match the intent";
                return false;
            }
        }
        if (checkpoint.stage == AccountAuditStage::ForwardProgress) {
            const auto& expected = state.open->intent["arguments"]["message_ids"];
            const auto& items = checkpoint.data["items"];
            if (items.size() != expected.size()) {
                error = "forward vector does not match the intent";
                return false;
            }
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (items.at(index)["source_id"] != expected.at(index)) {
                    error = "forward source order does not match the intent";
                    return false;
                }
            }
        }
        std::vector<AccountAuditCheckpointInput> history;
        history.reserve(state.open->checkpoints.size() + 1);
        for (const auto& prior : state.open->checkpoints) {
            const auto prior_operation =
                parse_account_audit_operation(prior.value("command", std::string{}));
            const auto prior_stage = parse_account_audit_stage(prior.value("stage", std::string{}));
            if (!prior_operation || !prior_stage) {
                error = "validated v2 history lost its enum identity";
                return false;
            }
            history.push_back({{prior.value("invocation_id", std::string{}),
                                prior.value("timestamp", std::string{})},
                               prior.value("account", std::string{}),
                               *prior_operation,
                               prior["checkpoint_sequence"].get<std::uint32_t>(),
                               *prior_stage,
                               prior["data"]});
        }
        history.push_back(checkpoint);
        if (!transition_history(checkpoint.operation, history, error)) {
            return false;
        }
        state.open->checkpoints.push_back(document);
        if (checkpoint.stage != AccountAuditStage::ForwardProgress) {
            state.open->completed_stages.push_back(checkpoint.stage);
        } else {
            if (std::find(state.open->completed_stages.begin(), state.open->completed_stages.end(),
                          checkpoint.stage) == state.open->completed_stages.end()) {
                state.open->completed_stages.push_back(checkpoint.stage);
            }
            state.open->any_forward_sent =
                state.open->any_forward_sent ||
                std::any_of(checkpoint.data["items"].begin(), checkpoint.data["items"].end(),
                            [](const json& item) { return item["status"] == "sent"; });
            state.open->forward_complete = forward_vector_complete(checkpoint.data["items"]);
        }
        state.open->has_spool =
            state.open->has_spool || checkpoint.stage == AccountAuditStage::SpoolReady;
        state.open->dispatch_started =
            state.open->dispatch_started || checkpoint.stage == AccountAuditStage::DispatchStarted;
        state.open->mutation_confirmed = state.open->mutation_confirmed ||
                                         checkpoint.stage == AccountAuditStage::MutationConfirmed;
        return true;
    }
    if (phase != "outcome" || !validate_outcome_impl(document, error)) {
        error = error.empty() ? "invalid v2 outcome phase" : error;
        return false;
    }
    json completed = json::array();
    for (const auto stage : state.open->completed_stages) {
        completed.push_back(account_audit_stage_name(stage));
    }
    if (document["completed_stages"] != completed) {
        error = "v2 outcome stages do not match the durable prefix";
        return false;
    }
    AccountAuditMutationState expected_mutation = AccountAuditMutationState::None;
    if (state.open->mutation_confirmed || state.open->any_forward_sent) {
        expected_mutation = AccountAuditMutationState::Confirmed;
    } else if (state.open->dispatch_started) {
        expected_mutation = AccountAuditMutationState::Possible;
        if (state.open->forward_complete) {
            const auto& items = state.open->checkpoints.back()["data"]["items"];
            const bool deletion_ambiguity =
                std::any_of(items.begin(), items.end(), [](const json& item) {
                    return item["status"] == "failed" &&
                           item["failure_reason"] == "deleted_before_confirmation";
                });
            expected_mutation = deletion_ambiguity ? AccountAuditMutationState::Possible
                                                   : AccountAuditMutationState::None;
        }
    }
    if (document["mutation_state"] != account_audit_mutation_state_name(expected_mutation)) {
        error = "v2 outcome mutation state contradicts the durable prefix";
        return false;
    }
    if (state.open->mutation_confirmed) {
        const auto found = std::find_if(
            state.open->checkpoints.rbegin(), state.open->checkpoints.rend(),
            [](const json& checkpoint) { return checkpoint["stage"] == "mutation_confirmed"; });
        if (found == state.open->checkpoints.rend() ||
            document["terminal"] != (*found)["data"]["terminal"]) {
            error = "v2 outcome terminal contradicts the mutation proof";
            return false;
        }
    }
    state.open.reset();
    return true;
}

AccountAuditInspection contradiction(const ScanState& state, std::string_view account,
                                     std::string_view path, std::string detail) {
    AccountAuditInspection result;
    result.status = AccountAuditInspectionStatus::Contradiction;
    result.oldest_open = state.open;
    result.failure = {AccountAuditDurabilityReason::Contradiction, std::move(detail)};
    auto mutation = AccountAuditMutationState::None;
    if (state.open && state.open->any_forward_sent) {
        mutation = AccountAuditMutationState::Confirmed;
    } else if (state.open && state.open->dispatch_started) {
        mutation = AccountAuditMutationState::Possible;
    }
    result.terminal = incomplete_terminal(account, path, mutation,
                                          state.open ? state.open->completed_stages
                                                     : std::vector<AccountAuditStage>{});
    return result;
}

} // namespace

std::string_view account_audit_operation_name(AccountAuditOperation operation) {
    const auto* const found =
        std::find_if(kOperations.begin(), kOperations.end(),
                     [operation](const auto& item) { return item.first == operation; });
    return found == kOperations.end() ? std::string_view{} : found->second;
}

std::optional<AccountAuditOperation> parse_account_audit_operation(std::string_view value) {
    const auto* const found =
        std::find_if(kOperations.begin(), kOperations.end(),
                     [value](const auto& item) { return item.second == value; });
    return found == kOperations.end() ? std::nullopt : std::optional{found->first};
}

std::string_view account_audit_stage_name(AccountAuditStage stage) {
    const auto* const found = std::find_if(
        kStages.begin(), kStages.end(), [stage](const auto& item) { return item.first == stage; });
    return found == kStages.end() ? std::string_view{} : found->second;
}

std::optional<AccountAuditStage> parse_account_audit_stage(std::string_view value) {
    const auto* const found = std::find_if(
        kStages.begin(), kStages.end(), [value](const auto& item) { return item.second == value; });
    return found == kStages.end() ? std::nullopt : std::optional{found->first};
}

std::string_view account_audit_mutation_state_name(AccountAuditMutationState state) {
    switch (state) {
    case AccountAuditMutationState::None:
        return "none";
    case AccountAuditMutationState::Possible:
        return "possible";
    case AccountAuditMutationState::Confirmed:
        return "confirmed";
    }
    return {};
}

std::string_view account_audit_durability_reason_name(AccountAuditDurabilityReason reason) {
    const auto* const found =
        std::find_if(kDurabilityReasons.begin(), kDurabilityReasons.end(),
                     [reason](const auto& item) { return item.first == reason; });
    return found == kDurabilityReasons.end() ? std::string_view{} : found->second;
}

AccountAuditIntent::AccountAuditIntent(json document) : document_(std::move(document)) {}
const json& AccountAuditIntent::document() const {
    return document_;
}
AccountAuditCheckpoint::AccountAuditCheckpoint(json document) : document_(std::move(document)) {}
const json& AccountAuditCheckpoint::document() const {
    return document_;
}
AccountAuditOutcome::AccountAuditOutcome(json document) : document_(std::move(document)) {}
const json& AccountAuditOutcome::document() const {
    return document_;
}

std::optional<AccountAuditIntent> make_account_audit_intent(AccountAuditIntentInput input,
                                                            std::string& error) {
    if (input.request_source_bytes == 0 || input.request_source_bytes > kRequestSourceBytes) {
        error = "request source exceeds the v2 admission ceiling";
        return std::nullopt;
    }
    json document{{"schema_version", 2},
                  {"phase", "intent"},
                  {"invocation_id", std::move(input.identity.invocation_id)},
                  {"timestamp", std::move(input.identity.timestamp)},
                  {"account", std::move(input.account)},
                  {"command", account_audit_operation_name(input.operation)},
                  {"arguments", std::move(input.arguments)},
                  {"plan", std::move(input.plan)},
                  {"request_fingerprint", std::move(input.request_fingerprint)},
                  {"config_snapshot", std::move(input.config_snapshot)},
                  {"authority_source", std::move(input.authority_source)},
                  {"confirmation_source",
                   input.confirmation_source ? json(*input.confirmation_source) : json(nullptr)},
                  {"idempotency_key_hash",
                   input.idempotency_key_hash ? json(*input.idempotency_key_hash) : json(nullptr)}};
    if (!validate_intent_impl(document, error)) {
        return std::nullopt;
    }
    return AccountAuditIntent(std::move(document));
}

std::optional<AccountAuditCheckpoint>
make_account_audit_checkpoint(AccountAuditCheckpointInput input, std::string& error) {
    json document{{"schema_version", 2},
                  {"phase", "checkpoint"},
                  {"invocation_id", std::move(input.identity.invocation_id)},
                  {"timestamp", std::move(input.identity.timestamp)},
                  {"account", std::move(input.account)},
                  {"command", account_audit_operation_name(input.operation)},
                  {"checkpoint_sequence", input.checkpoint_sequence},
                  {"stage", account_audit_stage_name(input.stage)},
                  {"data", std::move(input.data)}};
    if (!validate_checkpoint_impl(document, error)) {
        return std::nullopt;
    }
    return AccountAuditCheckpoint(std::move(document));
}

std::optional<AccountAuditOutcome> make_account_audit_outcome(AccountAuditOutcomeInput input,
                                                              std::string& error) {
    json stages = json::array();
    for (const auto stage : input.completed_stages) {
        stages.push_back(account_audit_stage_name(stage));
    }
    json document{{"schema_version", 2},
                  {"phase", "outcome"},
                  {"invocation_id", std::move(input.identity.invocation_id)},
                  {"timestamp", std::move(input.identity.timestamp)},
                  {"account", std::move(input.account)},
                  {"command", account_audit_operation_name(input.operation)},
                  {"success", input.success},
                  {"mutation_state", account_audit_mutation_state_name(input.mutation_state)},
                  {"completed_stages", std::move(stages)},
                  {"terminal", std::move(input.terminal)}};
    if (!validate_outcome_impl(document, error)) {
        return std::nullopt;
    }
    return AccountAuditOutcome(std::move(document));
}

bool validate_account_audit_intent(const json& document, std::string& error) {
    return validate_intent_impl(document, error);
}
bool validate_account_audit_checkpoint(const json& document, std::string& error) {
    return validate_checkpoint_impl(document, error);
}
bool validate_account_audit_outcome(const json& document, std::string& error) {
    return validate_outcome_impl(document, error);
}
bool validate_account_audit_stage_history(AccountAuditOperation operation,
                                          const std::vector<AccountAuditCheckpointInput>& history,
                                          std::string& error) {
    return transition_history(operation, history, error);
}

std::string serialize_account_audit_record(const json& document) {
    return document.dump();
}

// NOLINTBEGIN(readability-function-cognitive-complexity): exact recovery cut order.
std::optional<AccountAuditRecoveryPlan>
classify_account_audit_recovery(const AccountAuditOpenGroup& group, std::string_view account,
                                std::string_view audit_path, const AccountAuditPinSource& pins,
                                std::string& error) {
    AccountAuditRecoveryPlan result;
    if (std::holds_alternative<UnavailableAccountAuditPins>(pins)) {
        error = "audit pin provider is unavailable";
        return std::nullopt;
    }
    if (std::holds_alternative<AbsentAccountAuditPinsByPolicy>(pins) &&
        (group.keyed || group.has_spool)) {
        error = "store-dependent audit group cannot be recovered without store policy";
        return std::nullopt;
    }
    if (!group.dispatch_started) {
        result.mutation_state = AccountAuditMutationState::None;
        result.terminal =
            incomplete_terminal(account, audit_path, result.mutation_state, group.completed_stages);
        if (group.has_spool) {
            result.boundaries.push_back(AccountAuditRecoveryBoundary::DeleteSpoolAndSyncRoot);
        }
        result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendOutcomeAndSync);
        if (group.keyed) {
            result.boundaries.push_back(AccountAuditRecoveryBoundary::TransitionStoreAndSync);
        }
        result.continue_current_request = true;
        error.clear();
        return result;
    }
    if (!group.mutation_confirmed && !group.forward_complete) {
        result.mutation_state = group.any_forward_sent ? AccountAuditMutationState::Confirmed
                                                       : AccountAuditMutationState::Possible;
        result.terminal =
            incomplete_terminal(account, audit_path, result.mutation_state, group.completed_stages);
        result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendOutcomeAndSync);
        result.retain_store = group.keyed;
        result.retain_spool = group.has_spool;
        error.clear();
        return result;
    }
    result.mutation_state = group.any_forward_sent || group.mutation_confirmed
                                ? AccountAuditMutationState::Confirmed
                                : AccountAuditMutationState::None;
    if (group.forward_complete && !group.mutation_confirmed) {
        auto terminal = derive_forward_terminal(group, error);
        if (!terminal) {
            return std::nullopt;
        }
        result.terminal = std::move(*terminal);
        if (!group.any_forward_sent) {
            const auto found = std::find_if(
                group.checkpoints.rbegin(), group.checkpoints.rend(),
                [](const json& checkpoint) { return checkpoint["stage"] == "forward_progress"; });
            const bool deletion_ambiguity =
                std::any_of((*found)["data"]["items"].begin(), (*found)["data"]["items"].end(),
                            [](const json& item) {
                                return item["failure_reason"] == "deleted_before_confirmation";
                            });
            result.mutation_state = deletion_ambiguity ? AccountAuditMutationState::Possible
                                                       : AccountAuditMutationState::None;
        }
    }
    if (group.forward_complete && group.any_forward_sent && !group.mutation_confirmed) {
        result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendMutationProofAndSync);
    }
    if (group.mutation_confirmed) {
        const auto found = std::find_if(
            group.checkpoints.rbegin(), group.checkpoints.rend(),
            [](const json& checkpoint) { return checkpoint["stage"] == "mutation_confirmed"; });
        if (found == group.checkpoints.rend()) {
            error = "mutation proof is missing its stored terminal";
            return std::nullopt;
        }
        result.terminal = (*found)["data"]["terminal"];
    }
    result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendOutcomeAndSync);
    if (group.keyed) {
        result.boundaries.push_back(AccountAuditRecoveryBoundary::TransitionStoreAndSync);
    }
    if (group.has_spool) {
        result.boundaries.push_back(AccountAuditRecoveryBoundary::CleanupSpoolAndSyncRoot);
    }
    result.continue_current_request = true;
    error.clear();
    return result;
}
// NOLINTEND(readability-function-cognitive-complexity)

AccountAuditCoordinator::Guard::Guard(std::unique_lock<std::mutex> lock,
                                      const AccountAuditCoordinator* owner)
    : lock_(std::move(lock)), owner_(owner) {}
bool AccountAuditCoordinator::Guard::valid() const {
    return owner_ != nullptr && lock_.owns_lock();
}
bool AccountAuditCoordinator::Guard::validate_lease(std::string& error) const {
    if (!valid()) {
        error = "account audit coordinator guard is not held";
        return false;
    }
    return owner_->validate_lease(error);
}
bool AccountAuditCoordinator::Guard::validate_lease(std::string_view state_directory,
                                                    std::string_view account, uid_t expected_uid,
                                                    std::string& error) const {
    if (!valid() || owner_->state_directory_ != state_directory || owner_->account_ != account ||
        owner_->expected_uid_ != expected_uid) {
        error = "account audit coordinator guard is bound to another account";
        return false;
    }
    return owner_->validate_lease(error);
}

AccountAuditCoordinator::AccountAuditCoordinator(std::string state_directory, std::string account,
                                                 uid_t expected_uid, int daemon_lock_descriptor,
                                                 std::uint64_t lock_device,
                                                 std::uint64_t lock_inode)
    : state_directory_(std::move(state_directory)), account_(std::move(account)),
      expected_uid_(expected_uid), daemon_lock_descriptor_(daemon_lock_descriptor),
      lock_device_(lock_device), lock_inode_(lock_inode) {}

std::unique_ptr<AccountAuditCoordinator>
AccountAuditCoordinator::create(std::string state_directory, std::string account,
                                uid_t expected_uid, int daemon_lock_descriptor,
                                std::string& error) {
    const auto separator = state_directory.rfind('/');
    if (!paths::valid_account_name(account) || daemon_lock_descriptor < 0 ||
        separator == std::string::npos || state_directory.substr(separator + 1) != account) {
        error = "invalid account audit coordinator input";
        return {};
    }
    struct stat descriptor_metadata {};
    struct stat named_metadata {};
    const Descriptor directory(
        ::open(state_directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0 || ::fstat(daemon_lock_descriptor, &descriptor_metadata) != 0 ||
        ::fstatat(directory.get(), "daemon.lock", &named_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(descriptor_metadata.st_mode) || descriptor_metadata.st_uid != expected_uid ||
        (descriptor_metadata.st_mode & 07777) != 0600 || descriptor_metadata.st_nlink != 1 ||
        descriptor_metadata.st_dev != named_metadata.st_dev ||
        descriptor_metadata.st_ino != named_metadata.st_ino) {
        error = "daemon.lock lifetime lease is invalid";
        return {};
    }
    std::array<char, 512> record{};
    const auto count = ::pread(daemon_lock_descriptor, record.data(), record.size(), 0);
    daemon_lock::Identity identity;
    std::string parse_error;
    if (count <= 0 ||
        !daemon_lock::parse_identity_record(
            std::string_view(record.data(), static_cast<std::size_t>(count)), identity,
            parse_error) ||
        identity.pid != ::getpid()) {
        error = "daemon.lock lifetime lease was not created by the current owner";
        return {};
    }
    error.clear();
    return std::unique_ptr<AccountAuditCoordinator>(new AccountAuditCoordinator(
        std::move(state_directory), std::move(account), expected_uid, daemon_lock_descriptor,
        static_cast<std::uint64_t>(descriptor_metadata.st_dev),
        static_cast<std::uint64_t>(descriptor_metadata.st_ino)));
}

AccountAuditCoordinator::Guard AccountAuditCoordinator::lock() {
    return {std::unique_lock(mutex_), this};
}

bool AccountAuditCoordinator::validate_lease(std::string& error) const {
    struct stat descriptor_metadata {};
    const Descriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat named_metadata {};
    if (directory.get() < 0 || ::fstat(daemon_lock_descriptor_, &descriptor_metadata) != 0 ||
        ::fstatat(directory.get(), "daemon.lock", &named_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        static_cast<std::uint64_t>(descriptor_metadata.st_dev) != lock_device_ ||
        static_cast<std::uint64_t>(descriptor_metadata.st_ino) != lock_inode_ ||
        descriptor_metadata.st_dev != named_metadata.st_dev ||
        descriptor_metadata.st_ino != named_metadata.st_ino ||
        descriptor_metadata.st_uid != expected_uid_ || !S_ISREG(descriptor_metadata.st_mode) ||
        (descriptor_metadata.st_mode & 07777) != 0600 || descriptor_metadata.st_nlink != 1) {
        error = "daemon.lock lifetime lease changed";
        return false;
    }
    error.clear();
    return true;
}

AccountAuditLog::AccountAuditLog(std::string state_directory, std::string account,
                                 uid_t expected_uid,
                                 std::shared_ptr<const testing::AccountAuditHooks> hooks)
    : state_directory_(std::move(state_directory)), audit_path_(state_directory_ + "/audit.log"),
      account_(std::move(account)), expected_uid_(expected_uid), hooks_(std::move(hooks)) {}

const std::string& AccountAuditLog::path() const {
    return audit_path_;
}

AccountAuditInspection
AccountAuditLog::inspect( // NOLINT(readability-function-cognitive-complexity):
                          // mixed-version scanner.
    const AccountAuditCoordinator::Guard& guard) const {
    AccountAuditInspection result;
    std::string lease_error;
    if (!guard.validate_lease(state_directory_, account_, expected_uid_, lease_error)) {
        result.status = AccountAuditInspectionStatus::Unavailable;
        result.failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return result;
    }
    if (injected(hooks_, AccountAuditFault::Open)) {
        result.status = AccountAuditInspectionStatus::Unavailable;
        result.failure = {AccountAuditDurabilityReason::OpenFailed, {}};
        return result;
    }
    const Descriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0) {
        if (errno == ENOENT) {
            return result;
        }
        result.status = AccountAuditInspectionStatus::Unavailable;
        result.failure = {AccountAuditDurabilityReason::OpenFailed, {}};
        return result;
    }
    if (!valid_directory_metadata(directory.get(), expected_uid_, result.failure)) {
        result.status = AccountAuditInspectionStatus::Unavailable;
        return result;
    }
    ScanState state;
    LogoutAuditRecordAdapter v1_adapter(account_);
    bool prior_nonempty = false;
    for (std::size_t segment_index = 0; segment_index < kSegmentNames.size(); ++segment_index) {
        const auto* name = kSegmentNames.at(segment_index);
        struct stat metadata {};
        if (::fstatat(directory.get(), name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            result.status = AccountAuditInspectionStatus::Unavailable;
            result.failure = {AccountAuditDurabilityReason::OpenFailed, {}};
            return result;
        }
        if (!valid_file_metadata(metadata, expected_uid_, result.failure)) {
            result.status = AccountAuditInspectionStatus::Unavailable;
            return result;
        }
        if (metadata.st_size < 0 ||
            static_cast<std::uint64_t>(metadata.st_size) > kMaximumSegmentBytes) {
            result.status = AccountAuditInspectionStatus::Unavailable;
            result.failure.reason = AccountAuditDurabilityReason::PathInvalid;
            return result;
        }
        if (state.open && metadata.st_size > 0 && prior_nonempty) {
            return contradiction(state, account_, audit_path_, "v2 group spans audit segments");
        }
        const Descriptor file(::openat(directory.get(), name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat descriptor_metadata {};
        if (file.get() < 0 || ::fstat(file.get(), &descriptor_metadata) != 0 ||
            descriptor_metadata.st_dev != metadata.st_dev ||
            descriptor_metadata.st_ino != metadata.st_ino ||
            descriptor_metadata.st_size != metadata.st_size) {
            result.status = AccountAuditInspectionStatus::Unavailable;
            result.failure = {AccountAuditDurabilityReason::OpenFailed, {}};
            return result;
        }
        std::string line;
        line.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(metadata.st_size), kIoChunkBytes)));
        std::array<char, kIoChunkBytes> chunk{};
        bool segment_positive_v2 = false;
        bool ended_with_lf = metadata.st_size == 0;
        std::uint64_t segment_offset = 0;
        std::uint64_t line_offset = 0;
        for (;;) {
            if (injected(hooks_, AccountAuditFault::Read)) {
                result.status = AccountAuditInspectionStatus::Unavailable;
                result.failure = {AccountAuditDurabilityReason::ReadFailed, {}};
                return result;
            }
            const auto count = ::read(file.get(), chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                result.status = AccountAuditInspectionStatus::Unavailable;
                result.failure = {AccountAuditDurabilityReason::ReadFailed, {}};
                return result;
            }
            if (count == 0) {
                break;
            }
            for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
                const char byte = chunk.at(index);
                ++segment_offset;
                if (byte != '\n') {
                    ended_with_lf = false;
                    if (line.size() == kIntentJsonBytes) {
                        if (state.open) {
                            return contradiction(state, account_, audit_path_,
                                                 "open v2 group has an oversized record");
                        }
                        result.status = AccountAuditInspectionStatus::Unavailable;
                        result.failure = {AccountAuditDurabilityReason::PathInvalid, {}};
                        return result;
                    }
                    line.push_back(byte);
                    continue;
                }
                ended_with_lf = true;
                if (line.empty()) {
                    result.status = AccountAuditInspectionStatus::Unavailable;
                    result.failure = {AccountAuditDurabilityReason::PathInvalid, {}};
                    return result;
                }
                auto document = json::parse(line, nullptr, false);
                if (document.is_discarded()) {
                    if (state.open) {
                        return contradiction(state, account_, audit_path_, "malformed v2 tail");
                    }
                    result.status = AccountAuditInspectionStatus::Unavailable;
                    result.failure = {(metadata.st_size > static_cast<off_t>(kLegacySegmentBytes) &&
                                       !state.positive_v2)
                                          ? AccountAuditDurabilityReason::PathInvalid
                                          : AccountAuditDurabilityReason::ParseError,
                                      {}};
                    return result;
                }
                const bool recognizable_v2 = document.is_object() &&
                                             document.contains("schema_version") &&
                                             document["schema_version"].is_number_integer() &&
                                             document["schema_version"] == 2;
                if (recognizable_v2) {
                    state.positive_v2 = true;
                    segment_positive_v2 = true;
                    if (document.value("phase", std::string{}) == "intent") {
                        if (v1_adapter.has_incomplete()) {
                            return contradiction(state, account_, audit_path_,
                                                 "v2 intent interleaves a v1 group");
                        }
                        const auto invocation = document.value("invocation_id", std::string{});
                        AccountAuditFailure rescan_failure;
                        const auto seen =
                            prior_invocation_seen(directory.get(), expected_uid_, segment_index,
                                                  line_offset, invocation, hooks_, rescan_failure);
                        if (!seen) {
                            result.status = AccountAuditInspectionStatus::Unavailable;
                            result.failure = std::move(rescan_failure);
                            return result;
                        }
                        if (*seen) {
                            return contradiction(state, account_, audit_path_,
                                                 "invocation id is reused in prior audit bytes");
                        }
                    }
                    std::string record_error;
                    if (!consume_v2(document, state, record_error)) {
                        return contradiction(state, account_, audit_path_, std::move(record_error));
                    }
                } else if (document.is_object() && document.value("schema_version", 0) == 1 &&
                           !state.positive_v2) {
                    bool previously_seen = false;
                    if (document.value("phase", std::string{}) == "intent") {
                        const auto invocation = document.value("invocation_id", std::string{});
                        AccountAuditFailure rescan_failure;
                        const auto seen =
                            prior_invocation_seen(directory.get(), expected_uid_, segment_index,
                                                  line_offset, invocation, hooks_, rescan_failure);
                        if (!seen) {
                            result.status = AccountAuditInspectionStatus::Unavailable;
                            result.failure = std::move(rescan_failure);
                            return result;
                        }
                        if (*seen) {
                            result.status = AccountAuditInspectionStatus::Unavailable;
                            result.failure = {AccountAuditDurabilityReason::PathInvalid,
                                              "reused v1 invocation"};
                            return result;
                        }
                        previously_seen = *seen;
                    }
                    LogoutAuditFailure legacy_failure;
                    if (!v1_adapter.consume(document, previously_seen, legacy_failure)) {
                        result.status = AccountAuditInspectionStatus::Unavailable;
                        result.failure = {AccountAuditDurabilityReason::PathInvalid,
                                          std::move(legacy_failure.reason)};
                        return result;
                    }
                } else if (state.positive_v2 && document.is_object() &&
                           document.contains("schema_version") &&
                           document["schema_version"].is_number_integer()) {
                    return contradiction(state, account_, audit_path_,
                                         "unsupported schema version after v2 recognition");
                } else {
                    result.status = AccountAuditInspectionStatus::Unavailable;
                    result.failure = {AccountAuditDurabilityReason::PathInvalid, {}};
                    return result;
                }
                line.clear();
                line_offset = segment_offset;
            }
        }
        if (!ended_with_lf) {
            if (state.open) {
                return contradiction(state, account_, audit_path_, "partial v2 tail");
            }
            result.status = AccountAuditInspectionStatus::Unavailable;
            result.failure = {
                (metadata.st_size > static_cast<off_t>(kLegacySegmentBytes) && !state.positive_v2)
                    ? AccountAuditDurabilityReason::PathInvalid
                    : AccountAuditDurabilityReason::ParseError,
                {}};
            return result;
        }
        if (metadata.st_size > static_cast<off_t>(kLegacySegmentBytes) && !segment_positive_v2 &&
            !state.positive_v2) {
            result.status = AccountAuditInspectionStatus::Unavailable;
            result.failure = {AccountAuditDurabilityReason::PathInvalid, {}};
            return result;
        }
        struct stat final_metadata {};
        struct stat final_named_metadata {};
        if (::fstat(file.get(), &final_metadata) != 0 ||
            ::fstatat(directory.get(), name, &final_named_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
            final_metadata.st_dev != descriptor_metadata.st_dev ||
            final_metadata.st_ino != descriptor_metadata.st_ino ||
            final_metadata.st_size != descriptor_metadata.st_size ||
            final_named_metadata.st_dev != descriptor_metadata.st_dev ||
            final_named_metadata.st_ino != descriptor_metadata.st_ino ||
            final_named_metadata.st_size != descriptor_metadata.st_size) {
            result.status = AccountAuditInspectionStatus::Unavailable;
            result.failure = {AccountAuditDurabilityReason::ReadFailed, {}};
            return result;
        }
        prior_nonempty = prior_nonempty || metadata.st_size > 0;
    }
    if (state.open) {
        result.status = AccountAuditInspectionStatus::Open;
        result.oldest_open = std::move(state.open);
        return result;
    }
    auto legacy = v1_adapter.finish();
    if (legacy.status == LogoutAuditInspectionStatus::Incomplete) {
        result.status = AccountAuditInspectionStatus::LegacyOpen;
        result.legacy_logout = std::move(legacy.incomplete);
    }
    return result;
}

namespace {
bool append_document(const std::string& state_directory, uid_t uid, const json& document,
                     std::string_view account, const AccountAuditCoordinator::Guard& guard,
                     const std::shared_ptr<const testing::AccountAuditHooks>& hooks,
                     AccountAuditFailure& failure, struct stat* appended_metadata = nullptr) {
    std::string lease_error;
    if (!guard.validate_lease(state_directory, account, uid, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return false;
    }
    if (injected(hooks, AccountAuditFault::Open)) {
        failure.reason = AccountAuditDurabilityReason::OpenFailed;
        return false;
    }
    const Descriptor directory(
        ::open(state_directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0 || !valid_directory_metadata(directory.get(), uid, failure)) {
        if (directory.get() < 0) {
            failure.reason = AccountAuditDurabilityReason::OpenFailed;
        }
        return false;
    }
    const Descriptor file(::openat(directory.get(), "audit.log",
                                   O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() < 0) {
        failure.reason = AccountAuditDurabilityReason::OpenFailed;
        return false;
    }
    struct stat metadata {};
    if (::fstat(file.get(), &metadata) != 0 || !valid_file_metadata(metadata, uid, failure)) {
        return false;
    }
    const auto line = serialize_account_audit_record(document) + '\n';
    if (injected(hooks, AccountAuditFault::Write) || !write_all(file.get(), line)) {
        failure.reason = AccountAuditDurabilityReason::WriteFailed;
        return false;
    }
    if (injected(hooks, AccountAuditFault::FileSync) || ::fsync(file.get()) != 0) {
        failure.reason = AccountAuditDurabilityReason::SyncFailed;
        return false;
    }
    if (injected(hooks, AccountAuditFault::DirectorySync) || ::fsync(directory.get()) != 0) {
        failure.reason = AccountAuditDurabilityReason::DirectorySyncFailed;
        return false;
    }
    if (appended_metadata != nullptr && ::fstat(file.get(), appended_metadata) != 0) {
        failure.reason = AccountAuditDurabilityReason::OpenFailed;
        return false;
    }
    failure.detail.clear();
    return true;
}
} // namespace

bool AccountAuditLog::append_intent(const AccountAuditIntent& intent,
                                    const AccountAuditPinSource& pins,
                                    const AccountAuditCoordinator::Guard& guard,
                                    AccountAuditAppendReceipt& receipt,
                                    AccountAuditFailure& failure) const {
    std::string lease_error;
    if (!guard.validate_lease(state_directory_, account_, expected_uid_, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return false;
    }
    if (intent.document()["account"] != account_) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "v2 intent is routed to another account"};
        return false;
    }
    const auto inspection = inspect(guard);
    if (inspection.status != AccountAuditInspectionStatus::Clean) {
        failure = inspection.status == AccountAuditInspectionStatus::Unavailable
                      ? inspection.failure
                      : AccountAuditFailure{AccountAuditDurabilityReason::Contradiction,
                                            "prior audit history is not terminal"};
        return false;
    }
    const auto line_size = serialize_account_audit_record(intent.document()).size() + 1;
    if (line_size > kIntentLineBytes) {
        failure.reason = AccountAuditDurabilityReason::TooLarge;
        return false;
    }
    const Descriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0 || !valid_directory_metadata(directory.get(), expected_uid_, failure)) {
        if (directory.get() < 0) {
            failure.reason = AccountAuditDurabilityReason::OpenFailed;
        }
        return false;
    }
    if (!rotate_for_intent(directory.get(), expected_uid_, line_size, pins, hooks_, failure)) {
        return false;
    }
    struct stat metadata {};
    if (!append_document(state_directory_, expected_uid_, intent.document(), account_, guard,
                         hooks_, failure, &metadata)) {
        return false;
    }
    receipt.audit_generation = static_cast<std::uint64_t>(metadata.st_ino);
    receipt.invocation_id = intent.document()["invocation_id"].get<std::string>();
    receipt.request_fingerprint = intent.document()["request_fingerprint"].get<std::string>();
    const auto operation =
        parse_account_audit_operation(intent.document()["command"].get<std::string>());
    if (!operation) {
        failure = {AccountAuditDurabilityReason::SchemaError,
                   "typed intent lost its operation identity"};
        return false;
    }
    receipt.operation = *operation;
    return true;
}

bool AccountAuditLog::append_checkpoint(const AccountAuditCheckpoint& checkpoint,
                                        const AccountAuditCoordinator::Guard& guard,
                                        AccountAuditFailure& failure) const {
    const auto inspection = inspect(guard);
    if (inspection.status != AccountAuditInspectionStatus::Open || !inspection.oldest_open) {
        failure = inspection.status == AccountAuditInspectionStatus::Unavailable
                      ? inspection.failure
                      : AccountAuditFailure{AccountAuditDurabilityReason::Contradiction,
                                            "checkpoint has no matching open group"};
        return false;
    }
    ScanState state;
    state.open = *inspection.oldest_open;
    std::string error;
    if (!consume_v2(checkpoint.document(), state, error)) {
        failure = {AccountAuditDurabilityReason::Contradiction, std::move(error)};
        return false;
    }
    return append_document(state_directory_, expected_uid_, checkpoint.document(), account_, guard,
                           hooks_, failure);
}

bool AccountAuditLog::append_outcome(const AccountAuditOutcome& outcome,
                                     const AccountAuditCoordinator::Guard& guard,
                                     AccountAuditFailure& failure) const {
    const auto inspection = inspect(guard);
    if (inspection.status != AccountAuditInspectionStatus::Open || !inspection.oldest_open) {
        failure = inspection.status == AccountAuditInspectionStatus::Unavailable
                      ? inspection.failure
                      : AccountAuditFailure{AccountAuditDurabilityReason::Contradiction,
                                            "outcome has no matching open group"};
        return false;
    }
    ScanState state;
    state.open = *inspection.oldest_open;
    std::string error;
    if (!consume_v2(outcome.document(), state, error)) {
        failure = {AccountAuditDurabilityReason::Contradiction, std::move(error)};
        return false;
    }
    return append_document(state_directory_, expected_uid_, outcome.document(), account_, guard,
                           hooks_, failure);
}

} // namespace tgcli::daemon
