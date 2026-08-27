#include "daemon/account_audit.hpp"

#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "common/utf8.hpp"
#include "daemon/m6_audit_contract.hpp"
#include "daemon/m6_write_policy.hpp"
#include "daemon/write_operation.hpp"
#include "proto/destructive_plan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <istream>
#include <limits>
#include <map>
#include <set>
#include <streambuf>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
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

constexpr std::array<std::pair<AccountAuditOperation, std::string_view>, 42> kOperations{{
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
    {AccountAuditOperation::ContactAdd, "contact_add"},
    {AccountAuditOperation::ContactRemove, "contact_remove"},
    {AccountAuditOperation::ContactBlock, "contact_block"},
    {AccountAuditOperation::ContactUnblock, "contact_unblock"},
    {AccountAuditOperation::FolderCreate, "folder_create"},
    {AccountAuditOperation::FolderEdit, "folder_edit"},
    {AccountAuditOperation::FolderDelete, "folder_delete"},
    {AccountAuditOperation::FolderAddChat, "folder_add_chat"},
    {AccountAuditOperation::FolderRemoveChat, "folder_remove_chat"},
    {AccountAuditOperation::TopicCreate, "topic_create"},
    {AccountAuditOperation::TopicEdit, "topic_edit"},
    {AccountAuditOperation::TopicClose, "topic_close"},
    {AccountAuditOperation::TopicReopen, "topic_reopen"},
    {AccountAuditOperation::ChatSetTitle, "chat_set_title"},
    {AccountAuditOperation::ChatSetPhoto, "chat_set_photo"},
    {AccountAuditOperation::ChatSetDescription, "chat_set_description"},
    {AccountAuditOperation::ChatInviteLink, "chat_invite_link"},
    {AccountAuditOperation::ChatPromote, "chat_promote"},
    {AccountAuditOperation::ChatDemote, "chat_demote"},
    {AccountAuditOperation::ChatBan, "chat_ban"},
    {AccountAuditOperation::ChatUnban, "chat_unban"},
    {AccountAuditOperation::ChatKick, "chat_kick"},
    {AccountAuditOperation::ChatSetPermissions, "chat_set_permissions"},
    {AccountAuditOperation::StorageOptimize, "storage_optimize"},
}};

std::optional<proto::M6Operation> m6_operation_for_audit(AccountAuditOperation operation) noexcept {
    const auto found = std::ranges::find_if(
        kOperations, [operation](const auto& entry) { return entry.first == operation; });
    if (found == kOperations.end()) {
        return std::nullopt;
    }
    const auto parsed = proto::parse_m6_operation(found->second);
    return parsed && m6_write_policy(*parsed) != nullptr ? parsed : std::nullopt;
}

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

bool serialized_size_at_most(const json& value, std::uint64_t maximum) {
    try {
        return value.dump().size() <= maximum;
    } catch (const json::exception&) {
        return false;
    }
}

struct ParsedAuditJson {
    json document;
    bool valid = false;
    bool duplicate_key = false;
    bool duplicate_schema_version = false;
    std::size_t top_level_schema_versions = 0;
    std::optional<AccountAuditFailure::Interruption> interruption;
};

std::optional<AccountAuditFailure::Interruption>
current_scan_interruption(const AccountAuditScanControl& control) {
    if (control.cancelled && control.cancelled()) {
        return AccountAuditFailure::Interruption::Cancelled;
    }
    if (deadline_expired(control.deadline)) {
        return AccountAuditFailure::Interruption::Deadline;
    }
    return std::nullopt;
}

void assign_scan_interruption(AccountAuditFailure::Interruption interruption,
                              AccountAuditFailure& failure) {
    failure.interruption = interruption;
    failure.detail = interruption == AccountAuditFailure::Interruption::Cancelled
                         ? "account audit scan was cancelled"
                         : "account audit scan reached its absolute deadline";
}

class PollingAuditStreamBuffer final : public std::streambuf {
  public:
    PollingAuditStreamBuffer(std::string_view bytes, AccountAuditScanControl control,
                             std::shared_ptr<const testing::AccountAuditHooks> hooks)
        : bytes_(bytes), control_(std::move(control)), hooks_(std::move(hooks)) {}

    [[nodiscard]] std::optional<AccountAuditFailure::Interruption> interruption() const {
        return interruption_;
    }

  protected:
    int_type underflow() override {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (offset_ >= bytes_.size() || !poll()) {
            return traits_type::eof();
        }
        const auto count = std::min<std::size_t>(kIoChunkBytes, bytes_.size() - offset_);
        std::copy_n(bytes_.data() + offset_, count, chunk_.data());
        setg(chunk_.data(), chunk_.data(), chunk_.data() + count);
        offset_ += count;
        return traits_type::to_int_type(*gptr());
    }

  private:
    bool poll() {
        if (hooks_ && hooks_->after_parser_poll) {
            hooks_->after_parser_poll();
        }
        interruption_ = current_scan_interruption(control_);
        return !interruption_;
    }

    std::string_view bytes_;
    AccountAuditScanControl control_;
    std::shared_ptr<const testing::AccountAuditHooks> hooks_;
    std::optional<AccountAuditFailure::Interruption> interruption_;
    std::array<char, kIoChunkBytes> chunk_{};
    std::size_t offset_ = 0;
};

ParsedAuditJson parse_audit_json( // NOLINT(readability-function-cognitive-complexity):
                                  // duplicate-aware streaming parser.
    std::string_view bytes, const AccountAuditScanControl& control,
    const std::shared_ptr<const testing::AccountAuditHooks>& hooks = {}) {
    ParsedAuditJson result;
    result.interruption = current_scan_interruption(control);
    if (result.interruption) {
        return result;
    }
    std::map<int, std::set<std::string>> object_keys;
    std::size_t parse_events = 0;
    try {
        const json::parser_callback_t callback = [&](int depth, json::parse_event_t event,
                                                     json& parsed) {
            ++parse_events;
            if ((parse_events & 127U) == 0U) {
                if (hooks && hooks->after_parser_poll) {
                    hooks->after_parser_poll();
                }
                result.interruption = current_scan_interruption(control);
                if (result.interruption) {
                    return false;
                }
            }
            if (event == json::parse_event_t::object_start) {
                object_keys[depth + 1].clear();
            } else if (event == json::parse_event_t::key && parsed.is_string()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!object_keys[depth].emplace(key).second) {
                    result.duplicate_key = true;
                    if (depth == 1 && key == "schema_version") {
                        result.duplicate_schema_version = true;
                    }
                }
                if (depth == 1 && key == "schema_version") {
                    ++result.top_level_schema_versions;
                }
            } else if (event == json::parse_event_t::object_end) {
                object_keys.erase(depth + 1);
            }
            return true;
        };
        PollingAuditStreamBuffer buffer(bytes, control, hooks);
        std::istream input(&buffer);
        result.document = json::parse(input, callback, false, true);
        if (buffer.interruption()) {
            result.interruption = buffer.interruption();
        }
        if (!result.interruption) {
            result.interruption = current_scan_interruption(control);
        }
        result.valid = !result.interruption && !result.document.is_discarded();
    } catch (const json::exception&) {
        result.valid = false;
    }
    return result;
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

std::optional<std::int64_t> json_int64(const json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(number);
    }
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    return value.get<std::int64_t>();
}

bool valid_int64(const json& value) {
    return json_int64(value).has_value();
}

bool valid_int32(const json& value) {
    const auto number = json_int64(value);
    return number && *number >= std::numeric_limits<std::int32_t>::min() &&
           *number <= std::numeric_limits<std::int32_t>::max();
}

bool valid_nonnegative_int32(const json& value) {
    const auto number = json_int64(value);
    return number && *number >= 0 && *number <= std::numeric_limits<std::int32_t>::max();
}

bool valid_positive_int32(const json& value) {
    const auto number = json_int64(value);
    return number && *number >= 1 && *number <= std::numeric_limits<std::int32_t>::max();
}

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

bool valid_utf8_text(const json& value, std::uint64_t maximum_bytes, std::size_t maximum_scalars,
                     bool allow_empty) {
    if (!valid_string(value, maximum_bytes)) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    const auto scalars = utf8_scalar_count(text);
    return (allow_empty || !text.empty()) && !contains_nul(text) && scalars &&
           *scalars <= maximum_scalars;
}

bool valid_selector_string(const json& value) {
    return valid_string(value) && !value.get_ref<const std::string&>().empty() &&
           !contains_nul(value.get_ref<const std::string&>());
}

bool valid_argument_path(const json& value) {
    if (!valid_string(value, 4'096)) {
        return false;
    }
    const auto& path = value.get_ref<const std::string&>();
    return !path.empty() && !contains_nul(path) && !path.ends_with('/');
}

bool valid_safe_basename(const json& value) {
    if (!valid_string(value, 255)) {
        return false;
    }
    const auto& name = value.get_ref<const std::string&>();
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) {
        return false;
    }
    for (std::size_t index = 0; index < name.size();) {
        const auto lead = static_cast<unsigned char>(name[index]);
        std::uint32_t scalar = lead;
        std::size_t width = 1;
        if ((lead & 0xe0U) == 0xc0U) {
            scalar = lead & 0x1fU;
            width = 2;
        } else if ((lead & 0xf0U) == 0xe0U) {
            scalar = lead & 0x0fU;
            width = 3;
        } else if ((lead & 0xf8U) == 0xf0U) {
            scalar = lead & 0x07U;
            width = 4;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            scalar = (scalar << 6U) | (static_cast<unsigned char>(name[index + offset]) & 0x3fU);
        }
        if (scalar <= 0x1fU || (scalar >= 0x7fU && scalar <= 0x9fU)) {
            return false;
        }
        index += width;
    }
    return true;
}

bool valid_canonical_absolute_path(const json& value, std::string_view basename) {
    if (!valid_string(value, 4'096)) {
        return false;
    }
    const auto& path = value.get_ref<const std::string&>();
    if (path.size() < 2 || path.front() != '/' || path.back() == '/' || contains_nul(path)) {
        return false;
    }
    std::size_t component_begin = 1;
    for (;;) {
        const auto separator = path.find('/', component_begin);
        const auto component = std::string_view(path).substr(
            component_begin,
            separator == std::string::npos ? std::string::npos : separator - component_begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (separator == std::string::npos) {
            return component == basename;
        }
        component_begin = separator + 1;
    }
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

std::optional<std::int64_t> timestamp_unix_seconds(const json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    const auto digits = [&text](std::size_t begin, std::size_t count) {
        return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(begin),
                           text.begin() + static_cast<std::ptrdiff_t>(begin + count),
                           [](char character) { return character >= '0' && character <= '9'; });
    };
    if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2) || !digits(11, 2) || !digits(14, 2) ||
        !digits(17, 2)) {
        return std::nullopt;
    }
    const auto number = [&text](std::size_t begin) {
        return (text[begin] - '0') * 10 + (text[begin + 1] - '0');
    };
    const auto year = std::stoi(text.substr(0, 4));
    const auto date = std::chrono::year_month_day{
        std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(number(5))},
        std::chrono::day{static_cast<unsigned>(number(8))}};
    if (year < 1970 || !date.ok() || number(11) > 23 || number(14) > 59 || number(17) > 59) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days(date).time_since_epoch().count();
    return static_cast<std::int64_t>(days) * 86'400 +
           static_cast<std::int64_t>(number(11)) * 3'600 +
           static_cast<std::int64_t>(number(14)) * 60 + number(17);
}

bool valid_timestamp(const json& value) {
    return timestamp_unix_seconds(value).has_value();
}

bool valid_session_timestamp(const json& value) {
    const auto seconds = timestamp_unix_seconds(value);
    return seconds && *seconds >= 1 && *seconds <= std::numeric_limits<std::int32_t>::max();
}

bool valid_int53(const json& value, bool positive = false) {
    constexpr std::int64_t maximum = 9'007'199'254'740'991LL;
    const auto number = json_int64(value);
    return number && *number >= (positive ? 1 : -maximum) && *number <= maximum;
}

bool valid_message_id(const json& value) {
    const auto number = json_int64(value);
    return number && *number != 0 && valid_int53(value);
}

bool valid_message_id_array(const json& value, std::size_t minimum = 1, std::size_t maximum = 100) {
    if (!value.is_array() || value.size() < minimum || value.size() > maximum) {
        return false;
    }
    std::set<std::int64_t> unique;
    for (const auto& item : value) {
        const auto number = json_int64(item);
        if (!number || !valid_message_id(item) || !unique.emplace(*number).second) {
            return false;
        }
    }
    return true;
}

bool valid_strict_message_id_array(const json& value, std::size_t minimum = 1,
                                   std::size_t maximum = 100) {
    if (!value.is_array() || value.size() < minimum || value.size() > maximum) {
        return false;
    }
    std::optional<std::int64_t> previous;
    for (const auto& item : value) {
        const auto number = json_int64(item);
        if (!number || !valid_message_id(item) || (previous && *number <= *previous)) {
            return false;
        }
        previous = number;
    }
    return true;
}

bool valid_mute_duration(const json& value) {
    const auto duration = json_int64(value);
    return duration && ((*duration >= 1 && *duration <= 31'622'400) ||
                        *duration == std::numeric_limits<std::int32_t>::max());
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
           valid_positive_int32(value["send_date"]);
}

bool valid_file_snapshot(const json& value) {
    return exact_fields(value, {"path", "name", "size", "sha256", "device", "inode", "mtime_ns",
                                "ctime_ns"}) &&
           valid_safe_basename(value["name"]) &&
           valid_canonical_absolute_path(value["path"],
                                         value["name"].get_ref<const std::string&>()) &&
           value["size"].is_number_unsigned() && valid_hash(value["sha256"]) &&
           value["device"].is_number_unsigned() && value["inode"].is_number_unsigned() &&
           valid_int64(value["mtime_ns"]) && valid_int64(value["ctime_ns"]);
}

bool valid_schedule_plan(const json& schedule, const json& observed_server_unix_time) {
    if (!valid_schedule(schedule)) {
        return false;
    }
    if (schedule.is_null() || schedule["kind"] == "online") {
        return observed_server_unix_time.is_null();
    }
    const auto send_date = json_int64(schedule["send_date"]);
    const auto server_now = json_int64(observed_server_unix_time);
    constexpr std::int64_t maximum_window = 367LL * 86'400LL;
    return send_date && server_now && *server_now <= *send_date - 11 &&
           *server_now >= *send_date - maximum_window;
}

bool valid_arguments(AccountAuditOperation operation, const json& value) {
    switch (operation) {
    case AccountAuditOperation::Send:
        return exact_fields(value, {"chat", "text", "parse_mode", "reply_to", "topic", "silent",
                                    "schedule"}) &&
               valid_selector_string(value["chat"]) &&
               valid_utf8_text(value["text"], kMessageTextBytes, kMessageTextScalars, false) &&
               (value["parse_mode"] == "plain" || value["parse_mode"] == "markdown_v2" ||
                value["parse_mode"] == "html") &&
               (value["reply_to"].is_null() || valid_message_id(value["reply_to"])) &&
               valid_topic(value["topic"], "forum", true) && value["silent"].is_boolean() &&
               valid_schedule(value["schedule"]);
    case AccountAuditOperation::MsgEdit:
        return exact_fields(value, {"chat", "message_id", "text"}) &&
               valid_selector_string(value["chat"]) && valid_message_id(value["message_id"]) &&
               valid_utf8_text(value["text"], kMessageTextBytes, kMessageTextScalars, false);
    case AccountAuditOperation::MsgDelete:
        return exact_fields(value, {"chat", "message_ids", "for_all"}) &&
               valid_selector_string(value["chat"]) &&
               valid_strict_message_id_array(value["message_ids"]) && value["for_all"].is_boolean();
    case AccountAuditOperation::MsgForward:
        return exact_fields(value, {"from", "to", "message_ids", "drop_author"}) &&
               valid_selector_string(value["from"]) && valid_selector_string(value["to"]) &&
               valid_strict_message_id_array(value["message_ids"]) &&
               value["drop_author"].is_boolean();
    case AccountAuditOperation::MsgReact:
        return exact_fields(value, {"chat", "message_id", "reaction", "remove", "big"}) &&
               valid_selector_string(value["chat"]) && valid_message_id(value["message_id"]) &&
               valid_utf8_text(value["reaction"], 64, 64, false) && value["remove"].is_boolean() &&
               value["big"].is_boolean() && !(value["remove"] == true && value["big"] == true);
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return exact_fields(value, {"chat", "message_id"}) &&
               valid_selector_string(value["chat"]) && valid_message_id(value["message_id"]);
    case AccountAuditOperation::ChatMarkRead:
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
    case AccountAuditOperation::ChatLeave:
        return exact_fields(value, {"chat"}) && valid_selector_string(value["chat"]);
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return exact_fields(value, {"chat", "duration_seconds"}) &&
               valid_selector_string(value["chat"]) &&
               (operation == AccountAuditOperation::ChatMute
                    ? valid_mute_duration(value["duration_seconds"])
                    : json_int64(value["duration_seconds"]) == std::optional<std::int64_t>{0});
    case AccountAuditOperation::ChatJoin:
        if (exact_fields(value, {"source", "username"})) {
            return value["source"] == "username" && valid_selector_string(value["username"]);
        }
        return exact_fields(value, {"source", "invite_link_sha256"}) &&
               value["source"] == "invite_link" && valid_hash(value["invite_link_sha256"]);
    case AccountAuditOperation::SavedAttach:
        return exact_fields(value, {"message_id", "path", "caption"}) &&
               valid_message_id(value["message_id"]) && valid_argument_path(value["path"]) &&
               valid_utf8_text(value["caption"], 4'096, 1'024, true);
    case AccountAuditOperation::SessionTerminate:
        return exact_fields(value, {"session_id"}) && valid_session_id(value["session_id"]);
    default: {
        const auto m6 = m6_operation_for_audit(operation);
        return m6 && valid_m6_audit_arguments(*m6, value);
    }
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
    default: {
        const auto m6 = m6_operation_for_audit(operation);
        const auto* policy = m6 ? m6_write_policy(*m6) : nullptr;
        if (policy == nullptr) {
            return {};
        }
        return {policy->tdlib_functions.begin(),
                policy->tdlib_functions.begin() +
                    static_cast<std::ptrdiff_t>(policy->tdlib_function_count)};
    }
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
    return value["last_active_date"].is_null() ||
           valid_session_timestamp(value["last_active_date"]);
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
               chat("chat") &&
               valid_utf8_text(value["text"], kMessageTextBytes, kMessageTextScalars, false) &&
               (value["parse_mode"] == "plain" || value["parse_mode"] == "markdown_v2" ||
                value["parse_mode"] == "html") &&
               (value["reply_to"].is_null() || valid_message_id(value["reply_to"])) &&
               valid_topic(value["requested_topic"], "forum", true) &&
               valid_topic(value["effective_topic"], "forum", true) &&
               value["silent"].is_boolean() &&
               valid_schedule_plan(value["schedule"], value["observed_server_unix_time"]);
    case AccountAuditOperation::MsgEdit:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "text"}) &&
               chat("chat") && valid_message_id(value["message_id"]) &&
               valid_utf8_text(value["text"], kMessageTextBytes, kMessageTextScalars, false);
    case AccountAuditOperation::MsgDelete:
        if (!exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_ids",
                                  "requested_for_all", "effective_for_all"}) ||
            !chat("chat") || !valid_strict_message_id_array(value["message_ids"]) ||
            !value["requested_for_all"].is_boolean() || !value["effective_for_all"].is_boolean()) {
            return false;
        }
        return value["chat"]["type"] == "supergroup" || value["chat"]["type"] == "channel"
                   ? value["requested_for_all"] == true && value["effective_for_all"] == true
                   : value["effective_for_all"] == value["requested_for_all"];
    case AccountAuditOperation::MsgForward:
        return exact_fields(value, {"operation", "account", "tdlib_request", "from", "to",
                                    "message_ids", "drop_author"}) &&
               chat("from") && chat("to") && valid_strict_message_id_array(value["message_ids"]) &&
               value["drop_author"].is_boolean();
    case AccountAuditOperation::MsgReact:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "reaction", "remove", "big"}) &&
               chat("chat") && valid_message_id(value["message_id"]) &&
               valid_utf8_text(value["reaction"], 64, 64, false) && value["remove"].is_boolean() &&
               value["big"].is_boolean() && !(value["remove"] == true && value["big"] == true) &&
               value["tdlib_request"] ==
                   (value["remove"] == true ? "removeMessageReaction" : "addMessageReaction");
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "pinned"}) &&
               chat("chat") && valid_message_id(value["message_id"]) &&
               value["pinned"].is_boolean();
    case AccountAuditOperation::ChatMarkRead:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "last_message_id"}) &&
               chat("chat") &&
               (value["last_message_id"].is_null() || valid_message_id(value["last_message_id"])) &&
               (value["last_message_id"].is_null() ? value["tdlib_request"].is_null()
                                                   : value["tdlib_request"] == "viewMessages");
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "muted",
                                    "duration_seconds"}) &&
               chat("chat") && value["muted"].is_boolean() &&
               value["muted"] == (operation == AccountAuditOperation::ChatMute) &&
               (operation == AccountAuditOperation::ChatMute
                    ? valid_mute_duration(value["duration_seconds"])
                    : json_int64(value["duration_seconds"]) == std::optional<std::int64_t>{0});
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
        if (!exact_fields(value, {"operation", "account", "tdlib_request", "source", "chat",
                                  "invite_link_sha256"})) {
            return false;
        }
        if (value["source"] == "username") {
            return value["tdlib_request"] == "joinChat" && valid_chat_identity(value["chat"]) &&
                   value["invite_link_sha256"].is_null();
        }
        return value["source"] == "invite_link" &&
               value["tdlib_request"] == "joinChatByInviteLink" &&
               (value["chat"].is_null() || valid_chat_identity(value["chat"])) &&
               valid_hash(value["invite_link_sha256"]);
    case AccountAuditOperation::ChatLeave:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat"}) &&
               chat("chat") && value["chat"]["type"] != "private";
    case AccountAuditOperation::SavedAttach:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_id",
                                    "effective_topic", "caption", "file"}) &&
               chat("chat") && valid_message_id(value["message_id"]) &&
               valid_topic(value["effective_topic"], "saved", false) &&
               valid_utf8_text(value["caption"], 4'096, 1'024, true) &&
               valid_file_snapshot(value["file"]);
    case AccountAuditOperation::SessionTerminate:
        return exact_fields(value, {"operation", "account", "tdlib_request", "session"}) &&
               valid_session_target(value["session"]);
    default: {
        const auto m6 = m6_operation_for_audit(operation);
        return m6 && valid_m6_audit_plan(*m6, value, account);
    }
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
    default: {
        const auto m6 = m6_operation_for_audit(operation);
        return m6 && m6_arguments_match_plan(*m6, arguments, plan);
    }
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
        !valid_message_id(value["id"]) || !valid_int53(value["chat_id"]) || value["chat_id"] == 0 ||
        !value["is_outgoing"].is_boolean() || !valid_message_topic(value["topic"]) ||
        !value["type"].is_string() || !valid_string(value["text"], kMessageTextBytes) ||
        !value["scheduled"].is_boolean()) {
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

bool valid_result_data( // NOLINT(readability-function-cognitive-complexity)
    AccountAuditOperation operation, const json& value) {
    switch (operation) {
    case AccountAuditOperation::Send:
    case AccountAuditOperation::MsgEdit:
    case AccountAuditOperation::SavedAttach:
        return valid_message_write_result(value);
    case AccountAuditOperation::MsgDelete:
        return exact_fields(value, {"chat_id", "message_ids", "for_all", "deleted"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               valid_strict_message_id_array(value["message_ids"]) &&
               value["for_all"].is_boolean() && value["deleted"] == true;
    case AccountAuditOperation::MsgForward:
        if (!exact_fields(value, {"from_chat_id", "to_chat_id", "items"}) ||
            !valid_int53(value["from_chat_id"]) || value["from_chat_id"] == 0 ||
            !valid_int53(value["to_chat_id"]) || value["to_chat_id"] == 0 ||
            !value["items"].is_array() || value["items"].empty() ||
            value["items"].size() > kForwardItemCount) {
            return false;
        }
        {
            std::optional<std::int64_t> previous;
            for (const auto& item : value["items"]) {
                if (!valid_forward_item(item) || item["status"] != "sent") {
                    return false;
                }
                const auto source = json_int64(item["source_id"]);
                if (!source || (previous && *source <= *previous)) {
                    return false;
                }
                previous = source;
            }
        }
        return true;
    case AccountAuditOperation::MsgReact:
        return exact_fields(value, {"chat_id", "message_id", "reaction", "removed", "big"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               valid_message_id(value["message_id"]) &&
               valid_utf8_text(value["reaction"], 64, 64, false) && value["removed"].is_boolean() &&
               value["big"].is_boolean();
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return exact_fields(value, {"chat_id", "message_id", "pinned"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               valid_message_id(value["message_id"]) && value["pinned"].is_boolean() &&
               value["pinned"] == (operation == AccountAuditOperation::MsgPin);
    case AccountAuditOperation::ChatMarkRead:
        return exact_fields(value, {"chat_id", "last_read_message_id", "marked_read"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               (value["last_read_message_id"].is_null() ||
                valid_message_id(value["last_read_message_id"])) &&
               value["marked_read"] == true;
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return exact_fields(value, {"chat_id", "muted", "duration_seconds"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               value["muted"].is_boolean() &&
               value["muted"] == (operation == AccountAuditOperation::ChatMute) &&
               (operation == AccountAuditOperation::ChatMute
                    ? valid_mute_duration(value["duration_seconds"])
                    : json_int64(value["duration_seconds"]) == std::optional<std::int64_t>{0});
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
        return exact_fields(value, {"chat_id", "chat_list", "pinned"}) &&
               valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
               (value["chat_list"] == "main" || value["chat_list"] == "archive") &&
               value["pinned"].is_boolean() &&
               value["pinned"] == (operation == AccountAuditOperation::ChatPin);
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
        return exact_fields(value, {"chat_id", "archived"}) && valid_int53(value["chat_id"]) &&
               value["chat_id"] != 0 && value["archived"].is_boolean() &&
               value["archived"] == (operation == AccountAuditOperation::ChatArchive);
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
    default: {
        const auto m6 = m6_operation_for_audit(operation);
        return m6 && valid_m6_audit_result(*m6, value);
    }
    }
    return false;
}

bool valid_json_tree(const json& value, std::size_t depth, std::size_t& nodes) {
    std::vector<std::pair<const json*, std::size_t>> pending{{&value, depth}};
    while (!pending.empty()) {
        const auto [current, current_depth] = pending.back();
        pending.pop_back();
        if (current_depth > 32 || ++nodes > 16'384) {
            return false;
        }
        if (current->is_string()) {
            if (!valid_string(*current) || contains_nul(current->get_ref<const std::string&>())) {
                return false;
            }
            continue;
        }
        if (current->is_array()) {
            for (const auto& item : *current) {
                pending.emplace_back(&item, current_depth + 1);
            }
            continue;
        }
        if (!current->is_object()) {
            continue;
        }
        for (const auto& [key, item] : current->items()) {
            if (!common::valid_utf8(key) || contains_nul(key) || key == "idempotency_key" ||
                key == "invite") {
                return false;
            }
            pending.emplace_back(&item, current_depth + 1);
        }
    }
    return true;
}

bool valid_auth_state(const json& value, bool nullable = true) {
    if (nullable && value.is_null()) {
        return true;
    }
    if (!value.is_string()) {
        return false;
    }
    constexpr std::array<std::string_view, 14> states{"unknown",
                                                      "wait_tdlib_parameters",
                                                      "wait_phone_number",
                                                      "wait_premium_purchase",
                                                      "wait_email_address",
                                                      "wait_email_code",
                                                      "wait_code",
                                                      "wait_other_device_confirmation",
                                                      "wait_registration",
                                                      "wait_password",
                                                      "ready",
                                                      "logging_out",
                                                      "closing",
                                                      "closed"};
    return std::find(states.begin(), states.end(), value.get_ref<const std::string&>()) !=
           states.end();
}

bool valid_operation_field(AccountAuditOperation operation, const json& details) {
    return details.contains("operation") && details["operation"].is_string() &&
           details["operation"] == account_audit_operation_name(operation);
}

bool valid_forward_items(const json& items, bool allow_empty, bool require_terminal) {
    if (!items.is_array() || (!allow_empty && items.empty()) || items.size() > kForwardItemCount) {
        return false;
    }
    std::optional<std::int64_t> previous;
    std::set<std::int64_t> temporary_ids;
    std::set<std::int64_t> final_ids;
    for (const auto& item : items) {
        if (!valid_forward_item(item)) {
            return false;
        }
        const auto source = json_int64(item["source_id"]);
        if (!source || (previous && *source <= *previous) ||
            (require_terminal && item["status"] == "pending")) {
            return false;
        }
        previous = source;
        if (item["status"] == "pending") {
            const auto temporary = json_int64(item["temporary_message_id"]);
            if (!temporary || !temporary_ids.emplace(*temporary).second) {
                return false;
            }
        } else if (item["status"] == "sent") {
            const auto final_id = json_int64(item["message"]["id"]);
            if (!final_id || !final_ids.emplace(*final_id).second) {
                return false;
            }
        }
    }
    return true;
}

bool valid_forward_timeout_items(const json& items) {
    return valid_forward_items(items, true, false) &&
           (items.empty() || std::ranges::any_of(items, [](const json& item) {
                return item["status"] == "pending";
            }));
}

bool legal_completed_stages( // NOLINT(readability-function-cognitive-complexity)
    AccountAuditOperation operation, const std::vector<AccountAuditStage>& completed) {
    const WriteOperation write_operation(operation);
    const auto is_prefix = [&completed](const std::vector<AccountAuditStage>& complete) {
        return completed.size() <= complete.size() &&
               std::equal(completed.begin(), completed.end(), complete.begin());
    };
    if (completed.empty()) {
        return true;
    }
    for (const bool keyed : {false, true}) {
        for (const bool temporary : {false, true}) {
            for (const bool progress : {false, true}) {
                for (const bool mutation_proof : {false, true}) {
                    std::vector<AccountAuditStage> complete;
                    if (keyed && write_operation.idempotent()) {
                        complete.push_back(AccountAuditStage::IdempotencyPending);
                    }
                    if (operation == AccountAuditOperation::SavedAttach ||
                        (write_operation.uses_photo_spool() && temporary)) {
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
                    if (is_prefix(complete)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool valid_audit_incomplete_details(AccountAuditOperation operation, const json& details) {
    if (!exact_fields(details, {"account", "path", "mutation_state", "completed_stages"}) ||
        !details["account"].is_string() ||
        !paths::valid_account_name(details["account"].get_ref<const std::string&>()) ||
        !valid_selector_string(details["path"]) || !details["mutation_state"].is_string() ||
        !details["completed_stages"].is_array()) {
        return false;
    }
    const auto& mutation = details["mutation_state"].get_ref<const std::string&>();
    if (mutation != "none" && mutation != "possible" && mutation != "confirmed") {
        return false;
    }
    std::set<AccountAuditStage> seen;
    std::vector<AccountAuditStage> completed;
    for (const auto& item : details["completed_stages"]) {
        if (!item.is_string()) {
            return false;
        }
        const auto stage = parse_account_audit_stage(item.get_ref<const std::string&>());
        if (!stage || !seen.emplace(*stage).second) {
            return false;
        }
        completed.push_back(*stage);
    }
    if (!legal_completed_stages(operation, completed)) {
        return false;
    }
    const bool dispatch = seen.contains(AccountAuditStage::DispatchStarted);
    const bool progress = seen.contains(AccountAuditStage::ForwardProgress);
    const bool proof = seen.contains(AccountAuditStage::MutationConfirmed);
    if (!dispatch) {
        return mutation == "none";
    }
    if (proof) {
        return mutation == "confirmed";
    }
    return progress ? (mutation == "none" || mutation == "possible" || mutation == "confirmed")
                    : mutation == "possible";
}

bool valid_timeout_details(AccountAuditOperation operation, const json& details) {
    if (operation == AccountAuditOperation::SessionTerminate) {
        return exact_fields(details, {"operation", "phase", "state", "outcome", "idempotency"}) &&
               valid_operation_field(operation, details) && valid_auth_state(details["state"]) &&
               (details["phase"] == "preflight" || details["phase"] == "dispatch") &&
               details["outcome"] ==
                   (details["phase"] == "preflight" ? "not_started" : "unknown") &&
               details["idempotency"] == "not_requested";
    }
    if (!valid_operation_field(operation, details) || !details.contains("phase") ||
        !details["phase"].is_string() || !details.contains("state") ||
        !valid_auth_state(details["state"]) || !details.contains("outcome") ||
        !details["outcome"].is_string() || !details.contains("idempotency") ||
        !details["idempotency"].is_string()) {
        return false;
    }
    const auto idempotency = details["idempotency"].get_ref<const std::string&>();
    if (details["phase"] == "preflight") {
        return exact_fields(details, {"operation", "phase", "state", "outcome", "idempotency"}) &&
               details["outcome"] == "not_started" &&
               (idempotency == "not_requested" || idempotency == "not_created" ||
                idempotency == "removed");
    }
    if (details["phase"] == "replay_confirmation") {
        return exact_fields(details, {"operation", "phase", "state", "outcome", "idempotency"}) &&
               (operation == AccountAuditOperation::MsgDelete ||
                operation == AccountAuditOperation::ChatLeave) &&
               details["outcome"] == "not_started" && idempotency == "completed_unchanged";
    }
    if (details["phase"] == "dispatch") {
        const bool direct = operation != AccountAuditOperation::Send &&
                            operation != AccountAuditOperation::SavedAttach &&
                            operation != AccountAuditOperation::MsgForward;
        return direct &&
               exact_fields(details, {"operation", "phase", "state", "outcome", "idempotency"}) &&
               details["outcome"] == "unknown" &&
               (idempotency == "not_requested" || idempotency == "pending");
    }
    if (details["phase"] != "confirmation" || details["outcome"] != "unknown" ||
        (idempotency != "not_requested" && idempotency != "pending")) {
        return false;
    }
    if (operation == AccountAuditOperation::MsgForward) {
        return exact_fields(details,
                            {"operation", "phase", "state", "outcome", "idempotency", "items"}) &&
               valid_forward_timeout_items(details["items"]);
    }
    return (operation == AccountAuditOperation::Send ||
            operation == AccountAuditOperation::SavedAttach) &&
           exact_fields(details, {"operation", "phase", "state", "outcome", "idempotency",
                                  "temporary_message_id"}) &&
           (details["temporary_message_id"].is_null() ||
            valid_message_id(details["temporary_message_id"]));
}

bool valid_forward_error_details(std::string_view code, const json& details) {
    if (code == "RATE_LIMITED") {
        if (!exact_fields(details, {"operation", "tdlib_code", "retry_after", "items"}) ||
            details["operation"] != "msg_forward" || details["tdlib_code"] != 429 ||
            !valid_nonnegative_int32(details["retry_after"]) ||
            !valid_forward_items(details["items"], true, true)) {
            return false;
        }
        if (details["items"].empty()) {
            return true;
        }
        std::int64_t maximum_retry = 0;
        for (const auto& item : details["items"]) {
            if (item["status"] != "failed" || item["failure_reason"] != "tdlib_error" ||
                item["tdlib_code"] != 429 || !valid_nonnegative_int32(item["retry_after"])) {
                return false;
            }
            maximum_retry = std::max(maximum_retry, json_int64(item["retry_after"]).value_or(0));
        }
        return details["retry_after"] == maximum_retry;
    }
    if (!exact_fields(details, {"operation", "from_chat_id", "to_chat_id", "items"}) ||
        details["operation"] != "msg_forward" || !valid_int53(details["from_chat_id"]) ||
        details["from_chat_id"] == 0 || !valid_int53(details["to_chat_id"]) ||
        details["to_chat_id"] == 0 || !valid_forward_items(details["items"], false, true)) {
        return false;
    }
    const auto sent = static_cast<std::size_t>(
        std::count_if(details["items"].begin(), details["items"].end(),
                      [](const json& item) { return item["status"] == "sent"; }));
    return code == "FORWARD_PARTIAL" ? sent > 0 && sent < details["items"].size() : sent == 0;
}

bool valid_stored_error_message(std::string_view code, const json& message) {
    if (!message.is_string()) {
        return false;
    }
    const auto& text = message.get_ref<const std::string&>();
    static const std::map<std::string_view, std::set<std::string_view>> messages{
        {"AUDIT_INCOMPLETE", {"a prior audited invocation did not reach a terminal proof"}},
        {"TIMEOUT", {"request timed out"}},
        {"DAEMON_SHUTDOWN", {"daemon is shutting down"}},
        {"NOT_AUTHED", {"authorization was lost"}},
        {"TDLIB_ERROR", {"Telegram request failed"}},
        {"RATE_LIMITED", {"Telegram rate limit exceeded"}},
        {"INTERNAL",
         {"internal error", "TDLib returned data outside the supported persistence bounds",
          "TDLib returned malformed session data"}},
        {"SEND_FAILED", {"message was deleted before confirmation"}},
        {"FORWARD_FAILED", {"messages could not be forwarded"}},
        {"FORWARD_PARTIAL", {"some messages could not be forwarded"}},
        {"JOIN_APPROVAL_REQUIRED", {"join request requires approval"}},
        {"JOIN_DECLINED", {"join request was declined"}},
        {"INPUT_CHANGED", {"input file changed while being read"}},
        {"SPOOL_UNAVAILABLE", {"attachment spool is unavailable"}},
        {"PRECONDITION_FAILED", {"operation precondition failed"}},
    };
    const auto found = messages.find(code);
    return found != messages.end() && found->second.contains(text);
}

bool valid_stored_error( // NOLINT(readability-function-cognitive-complexity)
    AccountAuditOperation operation, const json& value) {
    if (!exact_fields(value, {"kind", "code", "message", "details", "exit_code"}) ||
        value["kind"] != "error" || !value["code"].is_string() || !value["details"].is_object() ||
        !valid_int32(value["exit_code"])) {
        return false;
    }
    std::size_t nodes = 0;
    if (!valid_json_tree(value, 0, nodes)) {
        return false;
    }
    const auto& code = value["code"].get_ref<const std::string&>();
    if (!valid_stored_error_message(code, value["message"])) {
        return false;
    }
    const auto& details = value["details"];
    const auto exit = json_int64(value["exit_code"]).value_or(0);
    if (code == "AUDIT_INCOMPLETE") {
        return exit == kGeneric &&
               value["message"] == "a prior audited invocation did not reach a terminal proof" &&
               valid_audit_incomplete_details(operation, details);
    }
    if (code == "TIMEOUT") {
        return exit == kTimeout && valid_timeout_details(operation, details);
    }
    if (code == "DAEMON_SHUTDOWN") {
        return exit == kGeneric && exact_fields(details, {"reason"}) &&
               details["reason"] == "daemon_shutdown";
    }
    if (code == "NOT_AUTHED") {
        return exit == kNotAuthed && exact_fields(details, {"account", "state", "reason"}) &&
               details["account"].is_string() &&
               paths::valid_account_name(details["account"].get_ref<const std::string&>()) &&
               valid_auth_state(details["state"], false) &&
               details["reason"] == "authorization_lost";
    }
    if (code == "TDLIB_ERROR") {
        return exit == kGeneric && exact_fields(details, {"operation", "tdlib_code"}) &&
               valid_operation_field(operation, details) && valid_int32(details["tdlib_code"]);
    }
    if (code == "RATE_LIMITED") {
        if (exit != kRateLimited) {
            return false;
        }
        if (operation == AccountAuditOperation::MsgForward && details.contains("items")) {
            return valid_forward_error_details(code, details);
        }
        return exact_fields(details, {"operation", "tdlib_code", "retry_after"}) &&
               valid_operation_field(operation, details) && details["tdlib_code"] == 429 &&
               valid_nonnegative_int32(details["retry_after"]);
    }
    if (code == "INTERNAL") {
        if (exit != kGeneric || !valid_operation_field(operation, details)) {
            return false;
        }
        if (operation == AccountAuditOperation::SessionTerminate) {
            return exact_fields(details, {"operation", "reason", "tdlib_type_id"}) &&
                   details["reason"] == "malformed_tdlib_response" &&
                   (details["tdlib_type_id"].is_null() || valid_int32(details["tdlib_type_id"]));
        }
        return exact_fields(details, {"operation", "reason"}) &&
               details["reason"] == "internal_error";
    }
    if (code == "SEND_FAILED") {
        return exit == kGeneric &&
               (operation == AccountAuditOperation::Send ||
                operation == AccountAuditOperation::SavedAttach) &&
               exact_fields(details, {"operation", "chat_id", "temporary_message_id", "reason"}) &&
               valid_operation_field(operation, details) && valid_int53(details["chat_id"]) &&
               details["chat_id"] != 0 && valid_message_id(details["temporary_message_id"]) &&
               details["reason"] == "deleted_before_confirmation";
    }
    if (code == "FORWARD_FAILED" || code == "FORWARD_PARTIAL") {
        return exit == kGeneric && operation == AccountAuditOperation::MsgForward &&
               valid_forward_error_details(code, details);
    }
    if (code == "JOIN_APPROVAL_REQUIRED") {
        return exit == kGeneric && operation == AccountAuditOperation::ChatJoin &&
               exact_fields(details, {"operation", "bot_user_id", "query_id"}) &&
               valid_operation_field(operation, details) &&
               valid_int53(details["bot_user_id"], true) && valid_int53(details["query_id"], true);
    }
    if (code == "JOIN_DECLINED") {
        return exit == kGeneric && operation == AccountAuditOperation::ChatJoin &&
               exact_fields(details, {"operation"}) && valid_operation_field(operation, details);
    }
    if (code == "INPUT_CHANGED") {
        return exit == kGeneric && operation == AccountAuditOperation::SavedAttach &&
               exact_fields(details, {"operation", "path"}) &&
               valid_operation_field(operation, details) && valid_argument_path(details["path"]);
    }
    if (code == "SPOOL_UNAVAILABLE") {
        if ((operation != AccountAuditOperation::SavedAttach &&
             !WriteOperation(operation).uses_photo_spool()) ||
            exit != kGeneric || !exact_fields(details, {"operation", "path", "reason"}) ||
            !valid_operation_field(operation, details) || !valid_argument_path(details["path"]) ||
            !details["reason"].is_string()) {
            return false;
        }
        const auto& reason = details["reason"].get_ref<const std::string&>();
        return std::any_of(kDurabilityReasons.begin(), kDurabilityReasons.end(),
                           [&](const auto& item) { return item.second == reason; });
    }
    if (code == "PRECONDITION_FAILED") {
        constexpr std::array<std::string_view, 17> reasons{"not_editable",
                                                           "not_deletable_for_self",
                                                           "not_deletable_for_all",
                                                           "not_forwardable",
                                                           "not_copyable",
                                                           "not_pinnable",
                                                           "not_replyable",
                                                           "wrong_content_type",
                                                           "wrong_chat_type",
                                                           "wrong_topic",
                                                           "chat_not_listed",
                                                           "saved_notifications_unsupported",
                                                           "online_schedule_unsupported",
                                                           "schedule_window_elapsed",
                                                           "schedule_too_far",
                                                           "reply_markup_preservation_unsupported",
                                                           "reaction_unavailable"};
        if (exit != kGeneric ||
            !exact_fields(details, {"operation", "chat_id", "message_id", "reason"}) ||
            !valid_operation_field(operation, details) ||
            !(details["chat_id"].is_null() ||
              (valid_int53(details["chat_id"]) && details["chat_id"] != 0)) ||
            !(details["message_id"].is_null() || valid_message_id(details["message_id"])) ||
            !details["reason"].is_string()) {
            return false;
        }
        const auto& reason = details["reason"].get_ref<const std::string&>();
        if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
            return false;
        }
        const auto one_of = [&reason](std::initializer_list<std::string_view> allowed) {
            return std::find(allowed.begin(), allowed.end(), reason) != allowed.end();
        };
        switch (operation) {
        case AccountAuditOperation::Send:
            return one_of({"not_replyable", "wrong_topic", "online_schedule_unsupported",
                           "schedule_window_elapsed", "schedule_too_far"});
        case AccountAuditOperation::MsgEdit:
            return one_of(
                {"not_editable", "wrong_content_type", "reply_markup_preservation_unsupported"});
        case AccountAuditOperation::MsgDelete:
            return one_of({"not_deletable_for_self", "not_deletable_for_all"});
        case AccountAuditOperation::MsgForward:
            return one_of({"not_forwardable", "not_copyable"});
        case AccountAuditOperation::MsgReact:
            return reason == "reaction_unavailable";
        case AccountAuditOperation::MsgPin:
            return reason == "not_pinnable";
        case AccountAuditOperation::ChatMute:
        case AccountAuditOperation::ChatUnmute:
            return reason == "saved_notifications_unsupported";
        case AccountAuditOperation::ChatPin:
        case AccountAuditOperation::ChatUnpin:
            return reason == "chat_not_listed";
        case AccountAuditOperation::ChatLeave:
            return reason == "wrong_chat_type";
        case AccountAuditOperation::SavedAttach:
            return one_of({"wrong_content_type", "wrong_topic"});
        case AccountAuditOperation::MsgUnpin:
        case AccountAuditOperation::ChatMarkRead:
        case AccountAuditOperation::ChatArchive:
        case AccountAuditOperation::ChatUnarchive:
        case AccountAuditOperation::ChatJoin:
        case AccountAuditOperation::SessionTerminate:
            return false;
        default:
            return false;
        }
        return false;
    }
    return false;
}

bool valid_terminal(AccountAuditOperation operation, const json& value) {
    if (exact_fields(value, {"kind", "data"})) {
        std::size_t nodes = 0;
        return value["kind"] == "result" && valid_json_tree(value, 0, nodes) &&
               valid_result_data(operation, value["data"]);
    }
    return valid_stored_error(operation, value);
}

bool terminal_proves_mutation(AccountAuditOperation operation, const json& terminal) {
    if (!valid_terminal(operation, terminal)) {
        return false;
    }
    if (terminal["kind"] == "result") {
        return true;
    }
    const auto& code = terminal["code"].get_ref<const std::string&>();
    if (operation == AccountAuditOperation::MsgForward) {
        if (code == "TIMEOUT") {
            const auto& items = terminal["details"]["items"];
            return std::any_of(items.begin(), items.end(),
                               [](const json& item) { return item["status"] == "sent"; });
        }
        return code == "FORWARD_PARTIAL" || code == "INTERNAL";
    }
    return code == "INTERNAL" && (operation == AccountAuditOperation::Send ||
                                  operation == AccountAuditOperation::MsgEdit ||
                                  operation == AccountAuditOperation::SavedAttach);
}

bool terminal_proves_explicit_no_mutation(AccountAuditOperation operation, const json& terminal) {
    if (!valid_terminal(operation, terminal) || terminal["kind"] != "error") {
        return false;
    }
    const auto& code = terminal["code"].get_ref<const std::string&>();
    if (operation == AccountAuditOperation::ChatJoin) {
        return code == "JOIN_APPROVAL_REQUIRED" || code == "JOIN_DECLINED";
    }
    return (operation == AccountAuditOperation::Send ||
            operation == AccountAuditOperation::SavedAttach) &&
           (code == "TDLIB_ERROR" || code == "RATE_LIMITED");
}

std::uint64_t terminal_byte_ceiling(AccountAuditOperation operation) {
    if (operation == AccountAuditOperation::MsgForward) {
        return kForwardTerminalBytes;
    }
    if (operation == AccountAuditOperation::Send || operation == AccountAuditOperation::MsgEdit ||
        operation == AccountAuditOperation::SavedAttach) {
        return kSingleMessageTerminalBytes;
    }
    if (m6_operation_for_audit(operation)) {
        return kM6TerminalBytes;
    }
    return kOtherTerminalBytes;
}

bool valid_forward_item(const json& value) {
    if (!value.is_object() || !value.contains("source_id") ||
        !valid_message_id(value["source_id"]) || !value.contains("status") ||
        !value["status"].is_string()) {
        return false;
    }
    if (value["status"] == "pending") {
        return exact_fields(value, {"source_id", "status", "temporary_message_id"}) &&
               valid_message_id(value["temporary_message_id"]);
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
    const bool code_valid = value["tdlib_code"].is_null() || valid_int32(value["tdlib_code"]);
    const bool retry_valid =
        value["retry_after"].is_null() || valid_nonnegative_int32(value["retry_after"]);
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
        return WriteOperation(operation).idempotent() &&
               exact_fields(value, {"key_hash", "request_fingerprint", "expires_at",
                                    "reserved_terminal_bytes"}) &&
               valid_hash(value["key_hash"]) && valid_hash(value["request_fingerprint"]) &&
               value["expires_at"].is_number_unsigned() &&
               value["reserved_terminal_bytes"].is_number_unsigned();
    case AccountAuditStage::SpoolReady:
        return (operation == AccountAuditOperation::SavedAttach ||
                WriteOperation(operation).uses_photo_spool()) &&
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
               valid_message_id_array(value["temporary_message_ids"]);
    case AccountAuditStage::ForwardProgress:
        return operation == AccountAuditOperation::MsgForward && exact_fields(value, {"items"}) &&
               valid_forward_items(value["items"], false, false);
    case AccountAuditStage::MutationConfirmed:
        return exact_fields(value, {"terminal"}) &&
               terminal_proves_mutation(operation, value["terminal"]);
    }
    return false;
}

bool destructive_operation(AccountAuditOperation operation) {
    return WriteOperation(operation).destructive();
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
    std::size_t nodes = 0;
    if (!valid_json_tree(document, 0, nodes) ||
        !serialized_size_at_most(document, kIntentJsonBytes)) {
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
        !serialized_size_at_most(document["data"]["terminal"], terminal_byte_ceiling(operation))) {
        error = "v2 mutation terminal exceeds its operation ceiling";
        return false;
    }
    const auto maximum = *stage == AccountAuditStage::ForwardProgress ||
                                 *stage == AccountAuditStage::MutationConfirmed
                             ? kVectorJsonBytes
                             : kNonVectorJsonBytes;
    std::size_t nodes = 0;
    if (!valid_json_tree(document, 0, nodes) || !serialized_size_at_most(document, maximum)) {
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
        !document["mutation_state"].is_string() || !document["completed_stages"].is_array()) {
        error = "invalid v2 outcome envelope";
        return false;
    }
    const auto parsed_operation =
        parse_account_audit_operation(document["command"].get_ref<const std::string&>());
    if (!parsed_operation || !valid_terminal(*parsed_operation, document["terminal"])) {
        error = "invalid v2 outcome terminal";
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
    const auto operation = *parsed_operation;
    const auto& terminal = document["terminal"];
    if (terminal["kind"] == "error" && terminal["details"].contains("account") &&
        terminal["details"]["account"] != document["account"]) {
        error = "v2 outcome terminal account contradicts the envelope";
        return false;
    }
    if (terminal["kind"] == "error" && terminal["code"] == "AUDIT_INCOMPLETE" &&
        (terminal["details"]["mutation_state"] != mutation ||
         terminal["details"]["completed_stages"] != document["completed_stages"])) {
        error = "v2 incomplete terminal contradicts the outcome prefix";
        return false;
    }
    const bool legal_prefix = legal_completed_stages(operation, completed);
    const bool has_dispatch = unique.contains(AccountAuditStage::DispatchStarted);
    const bool has_progress = unique.contains(AccountAuditStage::ForwardProgress);
    const bool has_proof = unique.contains(AccountAuditStage::MutationConfirmed);
    const bool success = document["success"].get<bool>();
    const bool successful_mark_read_noop = operation == AccountAuditOperation::ChatMarkRead &&
                                           success && mutation == "none" && !has_dispatch &&
                                           !has_proof && terminal["kind"] == "result" &&
                                           terminal["data"]["last_read_message_id"].is_null();
    const bool spool_unavailable = document["terminal"]["kind"] == "error" &&
                                   document["terminal"]["code"] == "SPOOL_UNAVAILABLE";
    if (!legal_prefix || (document["terminal"]["kind"] == "result") != success ||
        (success && !successful_mark_read_noop && (mutation != "confirmed" || !has_proof)) ||
        (!has_dispatch && mutation != "none") || (has_proof && mutation != "confirmed") ||
        (has_dispatch && !has_progress && !has_proof && mutation != "possible" &&
         !(mutation == "none" && terminal_proves_explicit_no_mutation(operation, terminal))) ||
        (mutation == "confirmed" && !has_proof && !has_progress) ||
        ((mutation == "possible" || mutation == "confirmed") && !has_dispatch) ||
        (spool_unavailable && ((operation != AccountAuditOperation::SavedAttach &&
                                !WriteOperation(operation).uses_photo_spool()) ||
                               has_dispatch || mutation != "none" ||
                               std::any_of(completed.begin(), completed.end(),
                                           [](AccountAuditStage stage) {
                                               return stage !=
                                                          AccountAuditStage::IdempotencyPending &&
                                                      stage != AccountAuditStage::SpoolReady;
                                           }))) ||
        !serialized_size_at_most(document["terminal"], terminal_byte_ceiling(operation)) ||
        !serialized_size_at_most(document, kVectorJsonBytes)) {
        error = "invalid v2 outcome terminal";
        return false;
    }
    error.clear();
    return true;
}

std::vector<AccountAuditStage> allowed_order(AccountAuditOperation operation) {
    const WriteOperation write_operation(operation);
    if (operation == AccountAuditOperation::SavedAttach) {
        return {AccountAuditStage::IdempotencyPending, AccountAuditStage::SpoolReady,
                AccountAuditStage::DispatchStarted, AccountAuditStage::TemporaryIdsObserved,
                AccountAuditStage::MutationConfirmed};
    }
    if (write_operation.uses_photo_spool()) {
        return {AccountAuditStage::IdempotencyPending, AccountAuditStage::SpoolReady,
                AccountAuditStage::DispatchStarted, AccountAuditStage::MutationConfirmed};
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
    if (!write_operation.idempotent()) {
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
            !WriteOperation(operation).idempotent()) {
            error = "operation cannot use idempotency";
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
            if ((operation != AccountAuditOperation::SavedAttach &&
                 !WriteOperation(operation).uses_photo_spool()) ||
                saw_spool || saw_dispatch || (previous_position && !saw_idempotency)) {
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

bool scan_interrupted(const AccountAuditScanControl& control, AccountAuditFailure& failure) {
    if (const auto interruption = current_scan_interruption(control)) {
        assign_scan_interruption(*interruption, failure);
        return true;
    }
    return false;
}

bool parsed_scan_interrupted(const ParsedAuditJson& parsed, AccountAuditFailure& failure) {
    if (!parsed.interruption) {
        return false;
    }
    assign_scan_interruption(*parsed.interruption, failure);
    return true;
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
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
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
                result.push_back({name, false, 0, 0, 0});
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
        result.push_back({name, true, static_cast<std::uint64_t>(metadata.st_dev), inode,
                          static_cast<std::uint64_t>(metadata.st_size)});
    }
    return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact hole-first crash automaton.
bool rotate_for_intent(int directory, uid_t uid, std::uint64_t incoming, bool absent_by_policy,
                       const std::vector<AccountAuditPin>& pins,
                       const std::vector<std::uint64_t>& retained_generations,
                       const std::vector<SegmentIdentity>& expected_segments,
                       const AccountAuditScanControl& scan_control,
                       const std::shared_ptr<const testing::AccountAuditHooks>& hooks,
                       AccountAuditFailure& failure) {
    if (scan_interrupted(scan_control, failure)) {
        return false;
    }
    auto segments = inspect_segments(directory, uid, failure);
    if (!segments) {
        return false;
    }
    const auto same_segment = [](const SegmentIdentity& left, const SegmentIdentity& right) {
        return left.name == right.name && left.present == right.present &&
               left.device == right.device && left.inode == right.inode && left.size == right.size;
    };
    for (const auto& expected : expected_segments) {
        const auto found = std::ranges::find_if(
            *segments, [&](const auto& observed) { return observed.name == expected.name; });
        if (found == segments->end() || !same_segment(expected, *found)) {
            failure = {AccountAuditDurabilityReason::Contradiction,
                       "audit segment changed after append permit validation"};
            return false;
        }
    }
    std::set<std::uint64_t> pinned_inodes;
    for (const auto& pin : pins) {
        pinned_inodes.insert(pin.audit_generation);
    }
    for (const auto generation : retained_generations) {
        pinned_inodes.insert(generation);
    }
    auto& active = segments->at(0);
    if (!active.present) {
        return true;
    }
    const auto threshold = hooks ? hooks->rotation_bytes : kRotationBytes;
    const auto active_size = active.size;
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
        if (absent_by_policy) {
            failure.reason = AccountAuditDurabilityReason::CapacityExhausted;
            return false;
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

bool terminal_matches_plan(AccountAuditOperation operation, const json& terminal,
                           const json& intent) {
    if (terminal["kind"] == "error") {
        const auto& details = terminal["details"];
        if (details.contains("account") && details["account"] != intent["account"]) {
            return false;
        }
        return !details.contains("operation") ||
               details["operation"] == account_audit_operation_name(operation);
    }
    const auto& data = terminal["data"];
    const auto& plan = intent["plan"];
    switch (operation) {
    case AccountAuditOperation::Send:
    case AccountAuditOperation::MsgEdit:
    case AccountAuditOperation::SavedAttach:
        return data["chat_id"] == plan["chat"]["id"] &&
               (operation != AccountAuditOperation::MsgEdit || data["id"] == plan["message_id"]);
    case AccountAuditOperation::MsgDelete:
        return data["chat_id"] == plan["chat"]["id"] &&
               data["message_ids"] == plan["message_ids"] &&
               data["for_all"] == plan["effective_for_all"];
    case AccountAuditOperation::MsgForward:
        return data["from_chat_id"] == plan["from"]["id"] && data["to_chat_id"] == plan["to"]["id"];
    case AccountAuditOperation::MsgReact:
        return data["chat_id"] == plan["chat"]["id"] && data["message_id"] == plan["message_id"] &&
               data["reaction"] == plan["reaction"] && data["removed"] == plan["remove"] &&
               data["big"] == plan["big"];
    case AccountAuditOperation::MsgPin:
    case AccountAuditOperation::MsgUnpin:
        return data["chat_id"] == plan["chat"]["id"] && data["message_id"] == plan["message_id"] &&
               data["pinned"] == plan["pinned"];
    case AccountAuditOperation::ChatMarkRead:
        return data["chat_id"] == plan["chat"]["id"] &&
               data["last_read_message_id"] == plan["last_message_id"];
    case AccountAuditOperation::ChatMute:
    case AccountAuditOperation::ChatUnmute:
        return data["chat_id"] == plan["chat"]["id"] && data["muted"] == plan["muted"] &&
               data["duration_seconds"] == plan["duration_seconds"];
    case AccountAuditOperation::ChatPin:
    case AccountAuditOperation::ChatUnpin:
        return data["chat_id"] == plan["chat"]["id"] && data["chat_list"] == plan["chat_list"] &&
               data["pinned"] == plan["pinned"];
    case AccountAuditOperation::ChatArchive:
    case AccountAuditOperation::ChatUnarchive:
        return data["chat_id"] == plan["chat"]["id"] && data["archived"] == plan["archived"];
    case AccountAuditOperation::ChatJoin:
        return data["chat_id"].is_null() || plan["chat"].is_null() ||
               data["chat_id"] == plan["chat"]["id"];
    case AccountAuditOperation::ChatLeave:
        return data["chat_id"] == plan["chat"]["id"];
    case AccountAuditOperation::SessionTerminate:
        return data["session_id"] == plan["session"]["id"];
    default: {
        const auto m6 = m6_operation_for_audit(operation);
        return m6 && m6_result_matches_plan(*m6, data, plan);
    }
    }
    return false;
}

bool canonical_json_equal(const json& left, const json& right) {
    try {
        return left.dump() == right.dump();
    } catch (const json::exception&) {
        return false;
    }
}

const json* forward_terminal_items(const json& terminal) {
    if (terminal["kind"] == "result") {
        const auto& data = terminal["data"];
        return data.contains("items") ? &data["items"] : nullptr;
    }
    const auto& details = terminal["details"];
    return details.contains("items") ? &details["items"] : nullptr;
}

struct ScanState {
    std::optional<AccountAuditOpenGroup> open;
    bool positive_v2 = false;
};

struct AccountAuditHoldSeed {
    std::uint64_t audit_generation = 0;
    std::string invocation_id;
    SpoolRef spool;
};

struct AccountAuditPinMatch {
    AccountAuditPin pin;
    bool matched = false;
};

using AccountAuditPinKey = std::pair<std::uint64_t, std::string>;

std::optional<std::map<AccountAuditPinKey, AccountAuditPinMatch>>
make_pin_index(const AccountAuditPinSource* source, std::string& error) {
    std::map<AccountAuditPinKey, AccountAuditPinMatch> result;
    if (source == nullptr || !std::holds_alternative<KnownAccountAuditPins>(*source)) {
        return result;
    }
    const auto& pins = std::get<KnownAccountAuditPins>(*source).pins;
    constexpr std::size_t maximum_pins = 10'000;
    if (pins.size() > maximum_pins) {
        error = "audit pin count exceeds the bounded store maximum";
        return std::nullopt;
    }
    for (const auto& pin : pins) {
        const json fingerprint = pin.request_fingerprint;
        if (pin.audit_generation == 0 || !valid_hex(pin.invocation_id, 32) ||
            !valid_hash(fingerprint) || account_audit_operation_name(pin.operation).empty()) {
            error = "audit pin has an invalid tuple";
            return std::nullopt;
        }
        const AccountAuditPinKey key{pin.audit_generation, pin.invocation_id};
        if (!result.emplace(key, AccountAuditPinMatch{pin, false}).second) {
            error = "audit pin tuple is duplicated";
            return std::nullopt;
        }
    }
    return result;
}

bool match_pin(std::map<AccountAuditPinKey, AccountAuditPinMatch>& pins,
               std::uint64_t audit_generation, const json& intent, std::string& error) {
    const AccountAuditPinKey key{audit_generation,
                                 intent["invocation_id"].get_ref<const std::string&>()};
    const auto found = pins.find(key);
    if (found == pins.end()) {
        return true;
    }
    const auto operation =
        parse_account_audit_operation(intent["command"].get_ref<const std::string&>());
    if (found->second.matched || !operation ||
        found->second.pin.request_fingerprint !=
            intent["request_fingerprint"].get_ref<const std::string&>() ||
        found->second.pin.operation != *operation) {
        error = "audit pin does not uniquely match its durable intent";
        return false;
    }
    found->second.matched = true;
    return true;
}

bool all_pins_matched(const std::map<AccountAuditPinKey, AccountAuditPinMatch>& pins,
                      std::string& error) {
    if (std::ranges::any_of(pins, [](const auto& item) { return !item.second.matched; })) {
        error = "audit pin is dangling or mismatched";
        return false;
    }
    return true;
}

FileSnapshot file_snapshot_from_audit(const json& value) {
    return {.path = value["path"].get<std::string>(),
            .name = value["name"].get<std::string>(),
            .size = value["size"].get<std::uint64_t>(),
            .sha256 = value["sha256"].get<std::string>(),
            .device = value["device"].get<std::uint64_t>(),
            .inode = value["inode"].get<std::uint64_t>(),
            .mtime_ns = value["mtime_ns"].get<std::int64_t>(),
            .ctime_ns = value["ctime_ns"].get<std::int64_t>()};
}

std::optional<AccountAuditCompletedGroupView>
make_completed_view(const AccountAuditOpenGroup& group, const json& outcome, std::string& error) {
    const auto operation =
        parse_account_audit_operation(group.intent["command"].get_ref<const std::string&>());
    const auto unix_seconds = timestamp_unix_seconds(group.intent["timestamp"]);
    if (!operation || !unix_seconds) {
        error = "validated completed group lost its typed intent facts";
        return std::nullopt;
    }
    AccountAuditCompletedGroupView view;
    view.audit_generation = group.audit_generation;
    view.invocation_id = group.intent["invocation_id"].get<std::string>();
    view.account = group.intent["account"].get<std::string>();
    view.operation = *operation;
    view.request_fingerprint = group.intent["request_fingerprint"].get<std::string>();
    if (!group.intent["idempotency_key_hash"].is_null()) {
        view.idempotency_key_hash = group.intent["idempotency_key_hash"].get<std::string>();
    }
    view.plan = group.intent["plan"];
    view.intent_timestamp = group.intent["timestamp"].get<std::string>();
    view.intent_unix_seconds = *unix_seconds;
    for (const auto& checkpoint : group.checkpoints) {
        const auto& stage = checkpoint["stage"].get_ref<const std::string&>();
        const auto& data = checkpoint["data"];
        if (stage == "idempotency_pending") {
            view.idempotency_pending = data;
        } else if (stage == "spool_ready") {
            view.spool = SpoolRef{.relative_path = data["relative_path"].get<std::string>(),
                                  .file = file_snapshot_from_audit(data["file"])};
        } else if (stage == "temporary_ids_observed") {
            view.temporary_message_ids = data["temporary_message_ids"];
        } else if (stage == "forward_progress") {
            view.forward_progress = data["items"];
        } else if (stage == "mutation_confirmed") {
            view.mutation_proof = data;
        }
    }
    view.completed_stages = group.completed_stages;
    view.outcome = outcome;
    return view;
}

std::optional<AccountAuditHoldSeed> make_spool_hold_seed(const AccountAuditOpenGroup& group) {
    const auto found = std::ranges::find_if(group.checkpoints, [](const json& checkpoint) {
        return checkpoint["stage"] == "spool_ready";
    });
    if (found == group.checkpoints.end()) {
        return std::nullopt;
    }
    const auto& data = (*found)["data"];
    return AccountAuditHoldSeed{group.audit_generation,
                                group.intent["invocation_id"].get<std::string>(),
                                SpoolRef{.relative_path = data["relative_path"].get<std::string>(),
                                         .file = file_snapshot_from_audit(data["file"])}};
}

constexpr std::array<const char*, 5> kSegmentNames{"audit.log.4", "audit.log.3", "audit.log.2",
                                                   "audit.log.1", "audit.log"};

std::optional<bool>
// NOLINTNEXTLINE(readability-function-cognitive-complexity): bounded five-segment rescan.
prior_invocation_seen(int directory, uid_t uid, std::size_t current_segment,
                      std::uint64_t current_record_offset, std::string_view invocation,
                      const AccountAuditScanControl& scan_control,
                      const std::shared_ptr<const testing::AccountAuditHooks>& hooks,
                      AccountAuditFailure& failure) {
    if (hooks && hooks->before_identity_rescan) {
        hooks->before_identity_rescan();
    }
    std::array<char, kIoChunkBytes> chunk{};
    for (std::size_t segment = 0; segment <= current_segment; ++segment) {
        if (scan_interrupted(scan_control, failure)) {
            return std::nullopt;
        }
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
            if (scan_interrupted(scan_control, failure)) {
                return std::nullopt;
            }
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
                const auto parsed = parse_audit_json(line, scan_control, hooks);
                if (parsed_scan_interrupted(parsed, failure)) {
                    return std::nullopt;
                }
                if (!parsed.valid) {
                    failure = {AccountAuditDurabilityReason::ParseError, {}};
                    return std::nullopt;
                }
                if (parsed.duplicate_key) {
                    failure = {AccountAuditDurabilityReason::PathInvalid, {}};
                    return std::nullopt;
                }
                const auto& document = parsed.document;
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

const json* final_forward_items(const AccountAuditOpenGroup& group) {
    const auto found = std::find_if(
        group.checkpoints.rbegin(), group.checkpoints.rend(),
        [](const json& checkpoint) { return checkpoint["stage"] == "forward_progress"; });
    return found == group.checkpoints.rend() ? nullptr : &(*found)["data"]["items"];
}

bool terminal_matches_latest_forward(const AccountAuditOpenGroup& group, const json& terminal) {
    const auto* terminal_items = forward_terminal_items(terminal);
    if (terminal_items == nullptr) {
        return true;
    }
    const auto* durable_items = final_forward_items(group);
    if (durable_items == nullptr) {
        return terminal["kind"] == "error" && terminal_items->empty() &&
               (terminal["code"] == "RATE_LIMITED" || terminal["code"] == "TIMEOUT");
    }
    if (!canonical_json_equal(*terminal_items, *durable_items)) {
        return false;
    }
    const auto& source_ids = group.intent["arguments"]["message_ids"];
    if (terminal_items->size() != source_ids.size()) {
        return false;
    }
    for (std::size_t index = 0; index < source_ids.size(); ++index) {
        if ((*terminal_items)[index]["source_id"] != source_ids[index]) {
            return false;
        }
    }
    return true;
}

AccountAuditMutationState derive_mutation_state(const AccountAuditOpenGroup& group) {
    if (group.mutation_confirmed || group.any_forward_sent) {
        return AccountAuditMutationState::Confirmed;
    }
    if (!group.dispatch_started) {
        return AccountAuditMutationState::None;
    }
    if (!group.forward_complete) {
        return AccountAuditMutationState::Possible;
    }
    const auto* items = final_forward_items(group);
    if (items == nullptr) {
        return AccountAuditMutationState::Possible;
    }
    const bool deletion_ambiguity = std::any_of(items->begin(), items->end(), [](const json& item) {
        return item["status"] == "failed" &&
               item["failure_reason"] == "deleted_before_confirmation";
    });
    return deletion_ambiguity ? AccountAuditMutationState::Possible
                              : AccountAuditMutationState::None;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): strict v2 group automaton.
bool consume_v2(const json& document, ScanState& state, std::string& error,
                std::uint64_t audit_generation = 0,
                const AccountAuditCompletedGroupVisitor* completed_visitor = nullptr,
                std::vector<AccountAuditHoldSeed>* hold_seeds = nullptr) {
    if (!document.is_object() || !document.contains("phase") || !document["phase"].is_string()) {
        error = "invalid v2 record phase";
        return false;
    }
    const auto& phase = document["phase"].get_ref<const std::string&>();
    if (phase == "intent") {
        if (state.open || !validate_intent_impl(document, error)) {
            error = error.empty() ? "v2 intent contradicts chronology" : error;
            return false;
        }
        AccountAuditOpenGroup group;
        group.audit_generation = audit_generation;
        group.intent = document;
        group.keyed = !document["idempotency_key_hash"].is_null();
        state.open = std::move(group);
        return true;
    }
    if (phase != "checkpoint" && phase != "outcome") {
        error = "invalid v2 record phase";
        return false;
    }
    if (phase == "checkpoint") {
        if (!validate_checkpoint_impl(document, error)) {
            return false;
        }
    } else if (!validate_outcome_impl(document, error)) {
        return false;
    }
    const auto& invocation = document["invocation_id"].get_ref<const std::string&>();
    if (!state.open || state.open->intent["invocation_id"] != invocation ||
        state.open->intent["command"] != document["command"] ||
        state.open->intent["account"] != document["account"]) {
        error = "v2 record has no matching open group";
        return false;
    }
    if (phase == "checkpoint") {
        AccountAuditCheckpointInput checkpoint;
        checkpoint.identity = {invocation, document["timestamp"].get<std::string>()};
        checkpoint.account = document["account"].get<std::string>();
        const auto operation =
            parse_account_audit_operation(document["command"].get_ref<const std::string&>());
        const auto stage =
            parse_account_audit_stage(document["stage"].get_ref<const std::string&>());
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
            const auto maximum_count = checkpoint.operation == AccountAuditOperation::MsgForward
                                           ? state.open->intent["arguments"]["message_ids"].size()
                                           : std::size_t{1};
            if (checkpoint.data["temporary_message_ids"].size() > maximum_count ||
                (checkpoint.operation != AccountAuditOperation::MsgForward &&
                 checkpoint.data["temporary_message_ids"].size() != 1)) {
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
            const auto temporary = std::find_if(
                state.open->checkpoints.begin(), state.open->checkpoints.end(),
                [](const json& prior) { return prior["stage"] == "temporary_ids_observed"; });
            json pending_ids = json::array();
            for (const auto& item : items) {
                if (item["status"] == "pending") {
                    pending_ids.push_back(item["temporary_message_id"]);
                }
            }
            const auto prior_progress = std::find_if(
                state.open->checkpoints.begin(), state.open->checkpoints.end(),
                [](const json& prior) { return prior["stage"] == "forward_progress"; });
            if (prior_progress == state.open->checkpoints.end()) {
                if ((temporary == state.open->checkpoints.end() && !pending_ids.empty()) ||
                    (temporary != state.open->checkpoints.end() &&
                     (*temporary)["data"]["temporary_message_ids"] != pending_ids)) {
                    error = "forward pending ids do not match the durable temporary ids";
                    return false;
                }
            }
        }
        std::vector<AccountAuditCheckpointInput> history;
        history.reserve(state.open->checkpoints.size() + 1);
        for (const auto& prior : state.open->checkpoints) {
            const auto prior_operation =
                parse_account_audit_operation(prior["command"].get_ref<const std::string&>());
            const auto prior_stage =
                parse_account_audit_stage(prior["stage"].get_ref<const std::string&>());
            if (!prior_operation || !prior_stage) {
                error = "validated v2 history lost its enum identity";
                return false;
            }
            history.push_back(
                {{prior["invocation_id"].get<std::string>(), prior["timestamp"].get<std::string>()},
                 prior["account"].get<std::string>(),
                 *prior_operation,
                 prior["checkpoint_sequence"].get<std::uint32_t>(),
                 *prior_stage,
                 prior["data"]});
        }
        history.push_back(checkpoint);
        if (!transition_history(checkpoint.operation, history, error)) {
            return false;
        }
        if (checkpoint.stage == AccountAuditStage::MutationConfirmed &&
            !terminal_matches_plan(checkpoint.operation, checkpoint.data["terminal"],
                                   state.open->intent)) {
            error = "mutation proof terminal contradicts the immutable plan";
            return false;
        }
        if (checkpoint.stage == AccountAuditStage::MutationConfirmed &&
            checkpoint.operation == AccountAuditOperation::MsgForward &&
            !terminal_matches_latest_forward(*state.open, checkpoint.data["terminal"])) {
            error = "mutation proof terminal contradicts the latest durable forward vector";
            return false;
        }
        if (checkpoint.stage == AccountAuditStage::MutationConfirmed &&
            checkpoint.operation == AccountAuditOperation::MsgForward) {
            const bool oversized_internal = checkpoint.data["terminal"]["kind"] == "error" &&
                                            checkpoint.data["terminal"]["code"] == "INTERNAL";
            const bool pending_timeout = checkpoint.data["terminal"]["kind"] == "error" &&
                                         checkpoint.data["terminal"]["code"] == "TIMEOUT";
            if (oversized_internal) {
                if (final_forward_items(*state.open) != nullptr) {
                    error = "forward oversized terminal follows a persisted vector";
                    return false;
                }
            } else if (pending_timeout) {
                if (state.open->forward_complete) {
                    error = "forward timeout mutation proof follows a complete vector";
                    return false;
                }
            } else if (!state.open->forward_complete) {
                error = "forward mutation proof precedes a complete vector";
                return false;
            } else {
                auto derived = derive_forward_terminal(*state.open, error);
                if (!derived || !canonical_json_equal(checkpoint.data["terminal"], *derived)) {
                    if (error.empty()) {
                        error = "forward mutation proof contradicts the terminal vector";
                    }
                    return false;
                }
            }
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
    json completed = json::array();
    for (const auto stage : state.open->completed_stages) {
        completed.push_back(account_audit_stage_name(stage));
    }
    if (document["completed_stages"] != completed) {
        error = "v2 outcome stages do not match the durable prefix";
        return false;
    }
    const auto operation =
        parse_account_audit_operation(state.open->intent["command"].get_ref<const std::string&>());
    const auto expected_mutation = derive_mutation_state(*state.open);
    const bool explicit_no_mutation =
        operation && expected_mutation == AccountAuditMutationState::Possible &&
        document["mutation_state"] == "none" &&
        terminal_proves_explicit_no_mutation(*operation, document["terminal"]);
    if (document["mutation_state"] != account_audit_mutation_state_name(expected_mutation) &&
        !explicit_no_mutation) {
        error = "v2 outcome mutation state contradicts the durable prefix";
        return false;
    }
    if (!operation ||
        !terminal_matches_plan(*operation, document["terminal"], state.open->intent)) {
        error = "v2 outcome terminal contradicts the immutable plan";
        return false;
    }
    if (*operation == AccountAuditOperation::MsgForward &&
        !terminal_matches_latest_forward(*state.open, document["terminal"])) {
        error = "v2 outcome terminal contradicts the latest durable forward vector";
        return false;
    }
    if (state.open->mutation_confirmed) {
        const auto found = std::find_if(
            state.open->checkpoints.rbegin(), state.open->checkpoints.rend(),
            [](const json& checkpoint) { return checkpoint["stage"] == "mutation_confirmed"; });
        if (found == state.open->checkpoints.rend() ||
            !canonical_json_equal(document["terminal"], (*found)["data"]["terminal"])) {
            error = "v2 outcome terminal contradicts the mutation proof";
            return false;
        }
    } else if (state.open->forward_complete) {
        auto derived = derive_forward_terminal(*state.open, error);
        if (!derived || !canonical_json_equal(document["terminal"], *derived)) {
            if (error.empty()) {
                error = "v2 outcome contradicts the terminal forward vector";
            }
            return false;
        }
    }
    auto completed_view = make_completed_view(*state.open, document, error);
    if (!completed_view) {
        return false;
    }
    if (completed_visitor != nullptr && *completed_visitor) {
        (*completed_visitor)(*completed_view);
    }
    if (hold_seeds != nullptr && completed_view->spool) {
        hold_seeds->push_back({completed_view->audit_generation, completed_view->invocation_id,
                               *completed_view->spool});
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
    const auto mutation =
        state.open ? derive_mutation_state(*state.open) : AccountAuditMutationState::None;
    result.terminal = incomplete_terminal(account, path, mutation,
                                          state.open ? state.open->completed_stages
                                                     : std::vector<AccountAuditStage>{});
    return result;
}

} // namespace

struct AccountAuditHoldPermitState {
    struct HoldRecord {
        std::uint64_t hold_id = 0;
        AccountAuditHoldSeed seed;
        bool released = false;
    };

    std::uint64_t permit_id = 0;
    std::string state_directory;
    std::string account;
    uid_t expected_uid = 0;
    std::shared_ptr<const AccountAuditCoordinator> coordinator;
    bool holds_issued = false;
    std::vector<HoldRecord> holds;
};

struct AccountAuditAppendPermit::Impl : AccountAuditHoldPermitState {
    bool absent_by_policy = false;
    bool consumed = false;
    std::string intent_line;
    std::vector<SegmentIdentity> segments;
    std::vector<AccountAuditPin> pins;
};

struct AccountAuditRecoveryPermit::Impl : AccountAuditHoldPermitState {};

namespace {

std::uint64_t next_spool_permit_id() {
    static std::atomic<std::uint64_t> next{1};
    auto result = next.fetch_add(1, std::memory_order_relaxed);
    if (result == 0) {
        result = next.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

} // namespace

AccountAuditSpoolHold::AccountAuditSpoolHold(
    std::uint64_t permit_id, std::uint64_t hold_id, std::string state_directory,
    std::string account, uid_t expected_uid,
    std::shared_ptr<const AccountAuditCoordinator> coordinator, std::uint64_t audit_generation,
    std::string invocation_id, SpoolRef spool)
    : permit_id_(permit_id), hold_id_(hold_id), state_directory_(std::move(state_directory)),
      account_(std::move(account)), expected_uid_(expected_uid),
      coordinator_(std::move(coordinator)), audit_generation_(audit_generation),
      invocation_id_(std::move(invocation_id)), spool_(std::move(spool)) {}

AccountAuditSpoolHold::AccountAuditSpoolHold(AccountAuditSpoolHold&& other) noexcept
    : permit_id_(std::exchange(other.permit_id_, 0)), hold_id_(std::exchange(other.hold_id_, 0)),
      state_directory_(std::move(other.state_directory_)), account_(std::move(other.account_)),
      expected_uid_(std::exchange(other.expected_uid_, 0)),
      coordinator_(std::move(other.coordinator_)),
      audit_generation_(std::exchange(other.audit_generation_, 0)),
      invocation_id_(std::move(other.invocation_id_)), spool_(std::move(other.spool_)) {}

AccountAuditSpoolHold& AccountAuditSpoolHold::operator=(AccountAuditSpoolHold&& other) noexcept {
    if (this != &other) {
        permit_id_ = std::exchange(other.permit_id_, 0);
        hold_id_ = std::exchange(other.hold_id_, 0);
        state_directory_ = std::move(other.state_directory_);
        account_ = std::move(other.account_);
        expected_uid_ = std::exchange(other.expected_uid_, 0);
        coordinator_ = std::move(other.coordinator_);
        audit_generation_ = std::exchange(other.audit_generation_, 0);
        invocation_id_ = std::move(other.invocation_id_);
        spool_ = std::move(other.spool_);
    }
    return *this;
}

bool AccountAuditSpoolHold::valid() const {
    return permit_id_ != 0 && hold_id_ != 0 && !state_directory_.empty() && !account_.empty() &&
           coordinator_ && audit_generation_ != 0 && !invocation_id_.empty() &&
           !spool_.relative_path.empty();
}

std::uint64_t AccountAuditSpoolHold::audit_generation() const {
    return audit_generation_;
}

const std::string& AccountAuditSpoolHold::invocation_id() const {
    return invocation_id_;
}

const SpoolRef& AccountAuditSpoolHold::spool() const {
    return spool_;
}

void AccountAuditSpoolHold::invalidate() {
    permit_id_ = 0;
    hold_id_ = 0;
    state_directory_.clear();
    account_.clear();
    expected_uid_ = 0;
    coordinator_.reset();
    audit_generation_ = 0;
    invocation_id_.clear();
    spool_ = {};
}

AccountAuditSpoolReleaseReceipt::AccountAuditSpoolReleaseReceipt(AccountAuditSpoolHold hold)
    : permit_id_(std::exchange(hold.permit_id_, 0)), hold_id_(std::exchange(hold.hold_id_, 0)),
      state_directory_(std::move(hold.state_directory_)), account_(std::move(hold.account_)),
      expected_uid_(std::exchange(hold.expected_uid_, 0)),
      coordinator_(std::move(hold.coordinator_)),
      audit_generation_(std::exchange(hold.audit_generation_, 0)),
      invocation_id_(std::move(hold.invocation_id_)), spool_(std::move(hold.spool_)) {}

AccountAuditSpoolReleaseReceipt::AccountAuditSpoolReleaseReceipt(
    AccountAuditSpoolReleaseReceipt&& other) noexcept
    : permit_id_(std::exchange(other.permit_id_, 0)), hold_id_(std::exchange(other.hold_id_, 0)),
      state_directory_(std::move(other.state_directory_)), account_(std::move(other.account_)),
      expected_uid_(std::exchange(other.expected_uid_, 0)),
      coordinator_(std::move(other.coordinator_)),
      audit_generation_(std::exchange(other.audit_generation_, 0)),
      invocation_id_(std::move(other.invocation_id_)), spool_(std::move(other.spool_)) {}

AccountAuditSpoolReleaseReceipt&
AccountAuditSpoolReleaseReceipt::operator=(AccountAuditSpoolReleaseReceipt&& other) noexcept {
    if (this != &other) {
        permit_id_ = std::exchange(other.permit_id_, 0);
        hold_id_ = std::exchange(other.hold_id_, 0);
        state_directory_ = std::move(other.state_directory_);
        account_ = std::move(other.account_);
        expected_uid_ = std::exchange(other.expected_uid_, 0);
        coordinator_ = std::move(other.coordinator_);
        audit_generation_ = std::exchange(other.audit_generation_, 0);
        invocation_id_ = std::move(other.invocation_id_);
        spool_ = std::move(other.spool_);
    }
    return *this;
}

bool AccountAuditSpoolReleaseReceipt::valid() const {
    return permit_id_ != 0 && hold_id_ != 0 && !state_directory_.empty() && !account_.empty() &&
           coordinator_ && audit_generation_ != 0 && !invocation_id_.empty() &&
           !spool_.relative_path.empty();
}

void AccountAuditSpoolReleaseReceipt::invalidate() {
    permit_id_ = 0;
    hold_id_ = 0;
    state_directory_.clear();
    account_.clear();
    expected_uid_ = 0;
    coordinator_.reset();
    audit_generation_ = 0;
    invocation_id_.clear();
    spool_ = {};
}

struct AccountAuditSpoolHoldAccess {
    static AccountAuditSpoolCleanupCallResult
    cleanup(AccountAuditSpoolHold hold, const AccountAuditCoordinator::Guard& guard,
            const FileSpoolControl& control,
            const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
        const auto contradiction = [] {
            FileSpoolError error;
            error.kind = FileSpoolErrorKind::Contradiction;
            error.durability_reason = DurabilityReason::Contradiction;
            return error;
        };
        if (!hold.valid() || guard.owner_ != hold.coordinator_) {
            return contradiction();
        }
        std::string lease_error;
        if (!guard.validate_lease(hold.state_directory_, hold.account_, hold.expected_uid_,
                                  lease_error)) {
            FileSpoolError error;
            error.kind = FileSpoolErrorKind::DurabilityFailure;
            error.durability_reason = DurabilityReason::LockFailed;
            return error;
        }
        const auto effective_control = guard.constrain_file_spool_control(control);
        auto cleanup = cleanup_spool_file(hold.state_directory_, hold.spool_, hold.expected_uid_,
                                          effective_control, hooks);
        if (auto* error = std::get_if<FileSpoolError>(&cleanup)) {
            return *error;
        }
        if (!std::get<SpoolCleanupResult>(cleanup).root_synced) {
            return contradiction();
        }
        return AccountAuditSpoolReleaseReceipt(std::move(hold));
    }
};

AccountAuditSpoolCleanupCallResult cleanup_spool_file_with_hold(
    AccountAuditSpoolHold hold, const AccountAuditCoordinator::Guard& guard,
    const FileSpoolControl& control, const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    return AccountAuditSpoolHoldAccess::cleanup(std::move(hold), guard, control, hooks);
}

AccountAuditAppendPermit::AccountAuditAppendPermit() = default;
AccountAuditAppendPermit::AccountAuditAppendPermit(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
AccountAuditAppendPermit::AccountAuditAppendPermit(AccountAuditAppendPermit&&) noexcept = default;
AccountAuditAppendPermit&
AccountAuditAppendPermit::operator=(AccountAuditAppendPermit&&) noexcept = default;
AccountAuditAppendPermit::~AccountAuditAppendPermit() = default;

bool AccountAuditAppendPermit::valid() const {
    return implementation_ && !implementation_->consumed && implementation_->permit_id != 0 &&
           implementation_->coordinator;
}

std::vector<AccountAuditSpoolHold> AccountAuditAppendPermit::issue_spool_holds() {
    std::vector<AccountAuditSpoolHold> result;
    if (!valid() || implementation_->holds_issued) {
        return result;
    }
    implementation_->holds_issued = true;
    result.reserve(implementation_->holds.size());
    for (const auto& hold : implementation_->holds) {
        result.push_back(AccountAuditSpoolHold(
            implementation_->permit_id, hold.hold_id, implementation_->state_directory,
            implementation_->account, implementation_->expected_uid, implementation_->coordinator,
            hold.seed.audit_generation, hold.seed.invocation_id, hold.seed.spool));
    }
    return result;
}

bool AccountAuditAppendPermit::release_spool_hold(AccountAuditSpoolReleaseReceipt receipt,
                                                  const AccountAuditCoordinator::Guard& guard,
                                                  AccountAuditFailure& failure) {
    if (!valid() || !receipt.valid()) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "spool release receipt is invalid or stale"};
        return false;
    }
    if (guard.owner_ != implementation_->coordinator) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "spool permit is bound to another account epoch"};
        return false;
    }
    std::string lease_error;
    if (!guard.validate_lease(implementation_->state_directory, implementation_->account,
                              implementation_->expected_uid, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return false;
    }
    const auto found = std::ranges::find_if(implementation_->holds, [&](const auto& hold) {
        return hold.hold_id == receipt.hold_id_ &&
               hold.seed.audit_generation == receipt.audit_generation_ &&
               hold.seed.invocation_id == receipt.invocation_id_ &&
               hold.seed.spool == receipt.spool_;
    });
    if (receipt.permit_id_ != implementation_->permit_id ||
        receipt.state_directory_ != implementation_->state_directory ||
        receipt.account_ != implementation_->account ||
        receipt.expected_uid_ != implementation_->expected_uid ||
        receipt.coordinator_ != implementation_->coordinator ||
        found == implementation_->holds.end() || found->released) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "spool release receipt does not match an unreleased hold"};
        receipt.invalidate();
        return false;
    }
    found->released = true;
    receipt.invalidate();
    failure.detail.clear();
    return true;
}

AccountAuditRecoveryPermit::AccountAuditRecoveryPermit() = default;
AccountAuditRecoveryPermit::AccountAuditRecoveryPermit(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
AccountAuditRecoveryPermit::AccountAuditRecoveryPermit(AccountAuditRecoveryPermit&&) noexcept =
    default;
AccountAuditRecoveryPermit&
AccountAuditRecoveryPermit::operator=(AccountAuditRecoveryPermit&&) noexcept = default;
AccountAuditRecoveryPermit::~AccountAuditRecoveryPermit() = default;

bool AccountAuditRecoveryPermit::valid() const {
    return implementation_ && implementation_->permit_id != 0 && implementation_->coordinator;
}

std::vector<AccountAuditSpoolHold> AccountAuditRecoveryPermit::issue_spool_holds() {
    std::vector<AccountAuditSpoolHold> result;
    if (!valid() || implementation_->holds_issued) {
        return result;
    }
    implementation_->holds_issued = true;
    result.reserve(implementation_->holds.size());
    for (const auto& hold : implementation_->holds) {
        result.push_back(AccountAuditSpoolHold(
            implementation_->permit_id, hold.hold_id, implementation_->state_directory,
            implementation_->account, implementation_->expected_uid, implementation_->coordinator,
            hold.seed.audit_generation, hold.seed.invocation_id, hold.seed.spool));
    }
    return result;
}

bool AccountAuditRecoveryPermit::release_spool_hold(AccountAuditSpoolReleaseReceipt receipt,
                                                    const AccountAuditCoordinator::Guard& guard,
                                                    AccountAuditFailure& failure) {
    if (!valid() || !receipt.valid()) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "spool release receipt is invalid or stale"};
        return false;
    }
    if (guard.owner_ != implementation_->coordinator) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "spool permit is bound to another account epoch"};
        return false;
    }
    std::string lease_error;
    if (!guard.validate_lease(implementation_->state_directory, implementation_->account,
                              implementation_->expected_uid, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return false;
    }
    const auto found = std::ranges::find_if(implementation_->holds, [&](const auto& hold) {
        return hold.hold_id == receipt.hold_id_ &&
               hold.seed.audit_generation == receipt.audit_generation_ &&
               hold.seed.invocation_id == receipt.invocation_id_ &&
               hold.seed.spool == receipt.spool_;
    });
    if (receipt.permit_id_ != implementation_->permit_id ||
        receipt.state_directory_ != implementation_->state_directory ||
        receipt.account_ != implementation_->account ||
        receipt.expected_uid_ != implementation_->expected_uid ||
        receipt.coordinator_ != implementation_->coordinator ||
        found == implementation_->holds.end() || found->released) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "spool release receipt does not match an unreleased hold"};
        receipt.invalidate();
        return false;
    }
    found->released = true;
    receipt.invalidate();
    failure.detail.clear();
    return true;
}

bool AccountAuditAppendPermit::narrow_pins(std::vector<AccountAuditPin> surviving,
                                           AccountAuditFailure& failure) {
    if (!valid() || implementation_->absent_by_policy) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "pin narrowing is unavailable for this append permit"};
        return false;
    }
    std::set<std::tuple<std::uint64_t, std::string, std::string, AccountAuditOperation>> unique;
    for (const auto& pin : surviving) {
        const auto identity = std::tuple{pin.audit_generation, pin.invocation_id,
                                         pin.request_fingerprint, pin.operation};
        const bool present = std::ranges::any_of(implementation_->pins, [&](const auto& original) {
            return original.audit_generation == pin.audit_generation &&
                   original.invocation_id == pin.invocation_id &&
                   original.request_fingerprint == pin.request_fingerprint &&
                   original.operation == pin.operation;
        });
        if (!present || !unique.emplace(identity).second) {
            failure = {AccountAuditDurabilityReason::Contradiction,
                       "surviving pins are not a unique subset of validated pins"};
            return false;
        }
    }
    implementation_->pins = std::move(surviving);
    failure.detail.clear();
    return true;
}

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

bool validate_account_audit_persisted_plan(AccountAuditOperation operation, const json& plan,
                                           std::string_view account) {
    return operation != AccountAuditOperation::SessionTerminate &&
           valid_plan(operation, plan, account);
}

bool validate_account_audit_persisted_arguments(AccountAuditOperation operation,
                                                const json& arguments) {
    return operation != AccountAuditOperation::SessionTerminate &&
           valid_arguments(operation, arguments);
}

bool validate_account_audit_persisted_result(AccountAuditOperation operation, const json& result) {
    return operation != AccountAuditOperation::SessionTerminate &&
           valid_result_data(operation, result);
}

bool validate_account_audit_persisted_stored_terminal(AccountAuditOperation operation,
                                                      const json& terminal) {
    return operation != AccountAuditOperation::SessionTerminate &&
           valid_terminal(operation, terminal);
}

bool validate_account_audit_persisted_terminal(AccountAuditOperation operation,
                                               const json& terminal, const json& plan,
                                               std::string_view account) {
    if (!validate_account_audit_persisted_plan(operation, plan, account) ||
        !valid_terminal(operation, terminal)) {
        return false;
    }
    const json intent{{"account", account}, {"plan", plan}};
    return terminal_matches_plan(operation, terminal, intent);
}

bool validate_account_audit_persisted_temporary_ids(AccountAuditOperation operation,
                                                    const json& temporary_ids, const json& plan) {
    if (!temporary_ids.is_array() || temporary_ids.empty()) {
        return temporary_ids.is_array();
    }
    if (operation != AccountAuditOperation::Send &&
        operation != AccountAuditOperation::SavedAttach &&
        operation != AccountAuditOperation::MsgForward) {
        return false;
    }
    const auto maximum = operation == AccountAuditOperation::MsgForward
                             ? plan.value("message_ids", json::array()).size()
                             : std::size_t{1};
    return valid_message_id_array(temporary_ids, 1, maximum) &&
           (operation == AccountAuditOperation::MsgForward || temporary_ids.size() == 1);
}

bool validate_account_audit_persisted_forward_progress(AccountAuditOperation operation,
                                                       const json& items, const json& plan) {
    if (!items.is_array() || items.empty()) {
        return items.is_array();
    }
    if (operation != AccountAuditOperation::MsgForward || !plan.is_object() ||
        !plan.contains("message_ids") || !plan["message_ids"].is_array() ||
        !valid_forward_items(items, false, false) || items.size() != plan["message_ids"].size()) {
        return false;
    }
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items.at(index)["source_id"] != plan["message_ids"].at(index)) {
            return false;
        }
    }
    return true;
}

bool validate_account_audit_persisted_spool(const SpoolRef& spool, std::string_view invocation_id) {
    const json file{{"path", spool.file.path},         {"name", spool.file.name},
                    {"size", spool.file.size},         {"sha256", spool.file.sha256},
                    {"device", spool.file.device},     {"inode", spool.file.inode},
                    {"mtime_ns", spool.file.mtime_ns}, {"ctime_ns", spool.file.ctime_ns}};
    return valid_spool_reference(spool, invocation_id) && valid_file_snapshot(file);
}

std::uint32_t account_audit_terminal_reservation(AccountAuditOperation operation) {
    return static_cast<std::uint32_t>(terminal_byte_ceiling(operation));
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
    const auto operation =
        parse_account_audit_operation(group.intent["command"].get_ref<const std::string&>());
    if (!operation) {
        error = "audit recovery group has an invalid operation";
        return std::nullopt;
    }
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
        const bool mark_read_noop = operation == AccountAuditOperation::ChatMarkRead &&
                                    group.intent["plan"]["tdlib_request"].is_null() &&
                                    group.intent["plan"]["last_message_id"].is_null();
        result.terminal = mark_read_noop
                              ? json{{"kind", "result"},
                                     {"data",
                                      {{"chat_id", group.intent["plan"]["chat"]["id"]},
                                       {"last_read_message_id", nullptr},
                                       {"marked_read", true}}}}
                              : incomplete_terminal(account, audit_path, result.mutation_state,
                                                    group.completed_stages);
        if (group.has_spool) {
            result.boundaries.push_back(AccountAuditRecoveryBoundary::DeleteSpoolAndSyncRoot);
        }
        result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendOutcomeAndSync);
        if (group.keyed) {
            result.boundaries.push_back(AccountAuditRecoveryBoundary::TransitionStoreAndSync);
            result.complete_store =
                mark_read_noop &&
                std::ranges::find(group.completed_stages, AccountAuditStage::IdempotencyPending) !=
                    group.completed_stages.end();
        }
        result.continue_current_request = true;
        error.clear();
        return result;
    }
    if (!group.mutation_confirmed && !group.forward_complete) {
        result.mutation_state = derive_mutation_state(group);
        result.terminal =
            incomplete_terminal(account, audit_path, result.mutation_state, group.completed_stages);
        result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendOutcomeAndSync);
        result.retain_store = group.keyed;
        result.retain_spool = group.has_spool;
        error.clear();
        return result;
    }
    result.mutation_state = derive_mutation_state(group);
    if (group.forward_complete && !group.mutation_confirmed) {
        auto terminal = derive_forward_terminal(group, error);
        if (!terminal) {
            return std::nullopt;
        }
        result.terminal = std::move(*terminal);
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
    const bool retain_incomplete_forward = *operation == AccountAuditOperation::MsgForward &&
                                           group.mutation_confirmed && !group.forward_complete &&
                                           final_forward_items(group) != nullptr;
    result.retain_store = group.keyed && retain_incomplete_forward;
    result.retain_spool = group.has_spool && retain_incomplete_forward;
    result.boundaries.push_back(AccountAuditRecoveryBoundary::AppendOutcomeAndSync);
    if (group.keyed) {
        result.boundaries.push_back(AccountAuditRecoveryBoundary::TransitionStoreAndSync);
    }
    if (group.has_spool && !retain_incomplete_forward) {
        result.boundaries.push_back(AccountAuditRecoveryBoundary::CleanupSpoolAndSyncRoot);
    }
    result.continue_current_request = true;
    error.clear();
    return result;
}
// NOLINTEND(readability-function-cognitive-complexity)

AccountAuditCoordinator::Guard::Guard(std::unique_lock<std::timed_mutex> lock,
                                      std::shared_ptr<const AccountAuditCoordinator> owner,
                                      AccountAuditScanControl scan_control)
    : lock_(std::move(lock)), owner_(std::move(owner)), scan_control_(std::move(scan_control)) {}
bool AccountAuditCoordinator::Guard::valid() const {
    return owner_ && lock_.owns_lock();
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
bool AccountAuditCoordinator::Guard::interrupted(AccountAuditFailure& failure) const {
    if (post_intent_durability_) {
        return false;
    }
    return scan_interrupted(scan_control_, failure);
}
FileSpoolControl
AccountAuditCoordinator::Guard::constrain_file_spool_control(FileSpoolControl control) const {
    if (post_intent_durability_) {
        return {};
    }
    if (scan_control_.deadline.expires_at &&
        (!control.deadline || *scan_control_.deadline.expires_at < *control.deadline)) {
        control.deadline = scan_control_.deadline.expires_at;
    }
    const auto caller_cancelled = std::move(control.cancelled);
    const auto epoch_cancelled = scan_control_.cancelled;
    control.cancelled = [caller_cancelled, epoch_cancelled] {
        return (caller_cancelled && caller_cancelled()) || (epoch_cancelled && epoch_cancelled());
    };
    return control;
}

bool AccountAuditCoordinator::Guard::enter_post_intent_durability(
    const AccountAuditAppendReceipt& receipt, AccountAuditFailure& failure) {
    if (!valid() || receipt.coordinator_ != owner_ || receipt.audit_generation == 0 ||
        receipt.invocation_id.empty() || receipt.request_fingerprint.empty()) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "post-intent receipt is invalid or bound to another account epoch"};
        return false;
    }
    std::string lease_error;
    if (!validate_lease(lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return false;
    }
    post_intent_durability_ = true;
    scan_control_ = {};
    failure = {};
    return true;
}

AccountAuditCoordinator::AccountAuditCoordinator(
    std::string state_directory, std::string account, uid_t expected_uid,
    std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease)
    : state_directory_(std::move(state_directory)), account_(std::move(account)),
      expected_uid_(expected_uid), daemon_lock_lease_(std::move(daemon_lock_lease)) {}

std::shared_ptr<AccountAuditCoordinator> AccountAuditCoordinator::create(
    std::string state_directory, std::string account, uid_t expected_uid,
    std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease, std::string& error) {
    const auto separator = state_directory.rfind('/');
    if (!paths::valid_account_name(account) || !daemon_lock_lease ||
        separator == std::string::npos || state_directory.substr(separator + 1) != account ||
        daemon_lock_lease->path() != state_directory + "/daemon.lock") {
        error = "invalid account audit coordinator input";
        return {};
    }
    if (!daemon_lock_lease->validate(expected_uid, error)) {
        return {};
    }
    const std::string key = state_directory + '\0' + account + '\0' + std::to_string(expected_uid) +
                            ':' + std::to_string(daemon_lock_lease->device()) + ':' +
                            std::to_string(daemon_lock_lease->inode());
    static std::mutex registry_mutex;
    static std::map<std::string, std::weak_ptr<AccountAuditCoordinator>> registry;
    const std::lock_guard lock(registry_mutex);
    if (const auto found = registry.find(key); found != registry.end()) {
        if (auto existing = found->second.lock()) {
            error.clear();
            return existing;
        }
        registry.erase(found);
    }
    auto created = std::shared_ptr<AccountAuditCoordinator>(
        new AccountAuditCoordinator(std::move(state_directory), std::move(account), expected_uid,
                                    std::move(daemon_lock_lease)));
    registry.emplace(key, created);
    error.clear();
    return created;
}

AccountAuditCoordinator::Guard AccountAuditCoordinator::lock() {
    return {std::unique_lock(mutex_), shared_from_this(), {}};
}

AccountAuditCoordinator::LockResult
AccountAuditCoordinator::lock(AccountAuditScanControl scan_control) {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!scan_control.cancelled && !scan_control.deadline.expires_at) {
        lock.lock();
        return Guard(std::move(lock), shared_from_this(), std::move(scan_control));
    }
    constexpr auto cancellation_poll = std::chrono::milliseconds(1);
    for (;;) {
        if (const auto interruption = current_scan_interruption(scan_control)) {
            AccountAuditFailure failure;
            assign_scan_interruption(*interruption, failure);
            return failure;
        }
        if (lock.try_lock()) {
            if (const auto interruption = current_scan_interruption(scan_control)) {
                AccountAuditFailure failure;
                assign_scan_interruption(*interruption, failure);
                return failure;
            }
            return Guard(std::move(lock), shared_from_this(), std::move(scan_control));
        }
        const auto now = std::chrono::steady_clock::now();
        const auto poll_deadline =
            scan_control.deadline.expires_at
                ? std::min(*scan_control.deadline.expires_at, now + cancellation_poll)
                : now + cancellation_poll;
        if (lock.try_lock_until(poll_deadline)) {
            if (const auto interruption = current_scan_interruption(scan_control)) {
                AccountAuditFailure failure;
                assign_scan_interruption(*interruption, failure);
                return failure;
            }
            return Guard(std::move(lock), shared_from_this(), std::move(scan_control));
        }
    }
}

bool AccountAuditCoordinator::validate_lease(std::string& error) const {
    return daemon_lock_lease_ && daemon_lock_lease_->validate(expected_uid_, error);
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
AccountAuditLog::inspect_unfinalized( // NOLINT(readability-function-cognitive-complexity):
                                      // mixed-version scanner.
    const AccountAuditCoordinator::Guard& guard, const AccountAuditPinSource* pins,
    const AccountAuditCompletedGroupVisitor* completed_visitor, AccountAuditAppendPermit* permit,
    const AccountAuditIntent* next_intent, AccountAuditRecoveryPermit* recovery_permit) const {
    AccountAuditInspection result;
    if (const auto* unavailable =
            pins == nullptr ? nullptr : std::get_if<UnavailableAccountAuditPins>(pins)) {
        result.status = AccountAuditInspectionStatus::Unavailable;
        result.failure = {unavailable->reason, "audit pin source is unavailable"};
        return result;
    }
    std::string pin_error;
    auto pin_index = make_pin_index(pins, pin_error);
    if (!pin_index) {
        result.status = AccountAuditInspectionStatus::Contradiction;
        result.failure = {AccountAuditDurabilityReason::Contradiction, std::move(pin_error)};
        return result;
    }
    std::vector<SegmentIdentity> scanned_segments;
    scanned_segments.reserve(kSegmentNames.size());
    std::vector<AccountAuditHoldSeed> hold_seeds;
    std::string lease_error;
    if (!guard.validate_lease(state_directory_, account_, expected_uid_, lease_error)) {
        result.status = AccountAuditInspectionStatus::Unavailable;
        result.failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return result;
    }
    if (scan_interrupted(guard.scan_control_, result.failure)) {
        result.status = AccountAuditInspectionStatus::Interrupted;
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
        if (scan_interrupted(guard.scan_control_, result.failure)) {
            result.status = AccountAuditInspectionStatus::Interrupted;
            return result;
        }
        const auto* name = kSegmentNames.at(segment_index);
        struct stat metadata {};
        if (::fstatat(directory.get(), name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                scanned_segments.push_back({name, false, 0, 0, 0});
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
        scanned_segments.push_back({name, true, static_cast<std::uint64_t>(metadata.st_dev),
                                    static_cast<std::uint64_t>(metadata.st_ino),
                                    static_cast<std::uint64_t>(metadata.st_size)});
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
        if (hooks_ && hooks_->before_segment_scan) {
            hooks_->before_segment_scan(name);
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
            if (scan_interrupted(guard.scan_control_, result.failure)) {
                result.status = AccountAuditInspectionStatus::Interrupted;
                return result;
            }
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
                const auto parsed = parse_audit_json(line, guard.scan_control_, hooks_);
                if (parsed_scan_interrupted(parsed, result.failure)) {
                    result.status = AccountAuditInspectionStatus::Interrupted;
                    return result;
                }
                if (!parsed.valid) {
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
                const auto& document = parsed.document;
                const bool one_integer_version = document.is_object() &&
                                                 parsed.top_level_schema_versions == 1 &&
                                                 document.contains("schema_version") &&
                                                 document["schema_version"].is_number_integer();
                const bool recognizable_v2 = one_integer_version &&
                                             !parsed.duplicate_schema_version &&
                                             document["schema_version"] == 2;
                if (recognizable_v2) {
                    state.positive_v2 = true;
                    segment_positive_v2 = true;
                    if (parsed.duplicate_key) {
                        return contradiction(state, account_, audit_path_,
                                             "duplicate key in recognized v2 record");
                    }
                    const bool intent_phase = document.contains("phase") &&
                                              document["phase"].is_string() &&
                                              document["phase"] == "intent";
                    std::string intent_error;
                    const bool valid_intent =
                        intent_phase && validate_intent_impl(document, intent_error);
                    if (valid_intent) {
                        if (v1_adapter.has_incomplete()) {
                            return contradiction(state, account_, audit_path_,
                                                 "v2 intent interleaves a v1 group");
                        }
                        const auto& invocation =
                            document["invocation_id"].get_ref<const std::string&>();
                        AccountAuditFailure rescan_failure;
                        const auto seen = prior_invocation_seen(
                            directory.get(), expected_uid_, segment_index, line_offset, invocation,
                            guard.scan_control_, hooks_, rescan_failure);
                        if (!seen) {
                            result.status = rescan_failure.interruption
                                                ? AccountAuditInspectionStatus::Interrupted
                                                : AccountAuditInspectionStatus::Unavailable;
                            result.failure = std::move(rescan_failure);
                            return result;
                        }
                        if (*seen) {
                            return contradiction(state, account_, audit_path_,
                                                 "invocation id is reused in prior audit bytes");
                        }
                        if (!match_pin(*pin_index,
                                       static_cast<std::uint64_t>(descriptor_metadata.st_ino),
                                       document, pin_error)) {
                            return contradiction(state, account_, audit_path_,
                                                 std::move(pin_error));
                        }
                    }
                    std::string record_error;
                    if (!consume_v2(document, state, record_error,
                                    static_cast<std::uint64_t>(descriptor_metadata.st_ino),
                                    completed_visitor,
                                    permit != nullptr || recovery_permit != nullptr ? &hold_seeds
                                                                                    : nullptr)) {
                        return contradiction(state, account_, audit_path_, std::move(record_error));
                    }
                } else if (state.positive_v2) {
                    return contradiction(state, account_, audit_path_,
                                         "unsupported or malformed version after v2 recognition");
                } else if (one_integer_version && !parsed.duplicate_key &&
                           document["schema_version"] == 1) {
                    bool previously_seen = false;
                    if (document.contains("phase") && document["phase"].is_string() &&
                        document["phase"] == "intent" && document.contains("invocation_id") &&
                        document["invocation_id"].is_string()) {
                        const auto& invocation =
                            document["invocation_id"].get_ref<const std::string&>();
                        AccountAuditFailure rescan_failure;
                        const auto seen = prior_invocation_seen(
                            directory.get(), expected_uid_, segment_index, line_offset, invocation,
                            guard.scan_control_, hooks_, rescan_failure);
                        if (!seen) {
                            result.status = rescan_failure.interruption
                                                ? AccountAuditInspectionStatus::Interrupted
                                                : AccountAuditInspectionStatus::Unavailable;
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
    if (!all_pins_matched(*pin_index, pin_error)) {
        return contradiction(state, account_, audit_path_, std::move(pin_error));
    }
    if (recovery_permit != nullptr) {
        if (state.open && state.open->has_spool) {
            const auto seed = make_spool_hold_seed(*state.open);
            if (!seed) {
                return contradiction(state, account_, audit_path_,
                                     "spool-ready group lost its recovery reference");
            }
            hold_seeds.push_back(*seed);
        }
        if (!hold_seeds.empty()) {
            auto implementation = std::make_unique<AccountAuditRecoveryPermit::Impl>();
            implementation->permit_id = next_spool_permit_id();
            implementation->state_directory = state_directory_;
            implementation->account = account_;
            implementation->expected_uid = expected_uid_;
            implementation->coordinator = guard.owner_;
            implementation->holds.reserve(hold_seeds.size());
            std::uint64_t hold_id = 1;
            for (auto& seed : hold_seeds) {
                implementation->holds.push_back({hold_id++, std::move(seed), false});
            }
            *recovery_permit = AccountAuditRecoveryPermit(std::move(implementation));
        }
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
    if (result.status == AccountAuditInspectionStatus::Clean && permit != nullptr &&
        next_intent != nullptr && pins != nullptr) {
        auto implementation = std::make_unique<AccountAuditAppendPermit::Impl>();
        implementation->permit_id = next_spool_permit_id();
        implementation->state_directory = state_directory_;
        implementation->account = account_;
        implementation->expected_uid = expected_uid_;
        implementation->coordinator = guard.owner_;
        implementation->absent_by_policy =
            std::holds_alternative<AbsentAccountAuditPinsByPolicy>(*pins);
        implementation->intent_line =
            serialize_account_audit_record(next_intent->document()) + '\n';
        implementation->segments = std::move(scanned_segments);
        if (const auto* known = std::get_if<KnownAccountAuditPins>(pins)) {
            implementation->pins = known->pins;
        }
        implementation->holds.reserve(hold_seeds.size());
        std::uint64_t hold_id = 1;
        for (auto& seed : hold_seeds) {
            implementation->holds.push_back({hold_id++, std::move(seed), false});
        }
        *permit = AccountAuditAppendPermit(std::move(implementation));
    }
    return result;
}

AccountAuditInspection AccountAuditLog::inspect(const AccountAuditCoordinator::Guard& guard) const {
    auto result = inspect_unfinalized(guard);
    if (hooks_ && hooks_->before_final_classification) {
        hooks_->before_final_classification();
    }
    AccountAuditFailure interruption;
    if (scan_interrupted(guard.scan_control_, interruption)) {
        result.status = AccountAuditInspectionStatus::Interrupted;
        result.failure = std::move(interruption);
    }
    return result;
}

AccountAuditInspection
AccountAuditLog::prepare_append(const AccountAuditIntent& intent, const AccountAuditPinSource& pins,
                                const AccountAuditCoordinator::Guard& guard,
                                AccountAuditAppendPermit& permit,
                                const AccountAuditCompletedGroupVisitor& completed_visitor) const {
    permit = AccountAuditAppendPermit{};
    AccountAuditInspection result;
    if (intent.document()["account"] != account_) {
        result.status = AccountAuditInspectionStatus::Contradiction;
        result.failure = {AccountAuditDurabilityReason::Contradiction,
                          "v2 intent is routed to another account"};
        return result;
    }
    const auto operation =
        parse_account_audit_operation(intent.document()["command"].get<std::string>());
    if (!operation) {
        result.status = AccountAuditInspectionStatus::Contradiction;
        result.failure = {AccountAuditDurabilityReason::SchemaError,
                          "typed intent lost its operation identity"};
        return result;
    }
    const bool absent_by_policy = std::holds_alternative<AbsentAccountAuditPinsByPolicy>(pins);
    if ((*operation == AccountAuditOperation::SessionTerminate) != absent_by_policy) {
        result.status = AccountAuditInspectionStatus::Contradiction;
        result.failure = {AccountAuditDurabilityReason::Contradiction,
                          "pin source contradicts the operation policy"};
        return result;
    }
    const auto line_size = serialize_account_audit_record(intent.document()).size() + 1;
    if (line_size > kIntentLineBytes) {
        result.status = AccountAuditInspectionStatus::Unavailable;
        result.failure = {AccountAuditDurabilityReason::TooLarge, {}};
        return result;
    }
    result = inspect_unfinalized(guard, &pins, &completed_visitor, &permit, &intent);
    if (hooks_ && hooks_->before_final_classification) {
        hooks_->before_final_classification();
    }
    AccountAuditFailure interruption;
    if (scan_interrupted(guard.scan_control_, interruption)) {
        result.status = AccountAuditInspectionStatus::Interrupted;
        result.failure = std::move(interruption);
    }
    if (result.status != AccountAuditInspectionStatus::Clean) {
        permit = AccountAuditAppendPermit{};
    }
    return result;
}

AccountAuditInspection AccountAuditLog::prepare_recovery(
    const AccountAuditPinSource& pins, const AccountAuditCoordinator::Guard& guard,
    AccountAuditRecoveryPermit& permit,
    const AccountAuditCompletedGroupVisitor& completed_visitor) const {
    permit = AccountAuditRecoveryPermit{};
    auto result = inspect_unfinalized(guard, &pins, &completed_visitor, nullptr, nullptr, &permit);
    if (result.status == AccountAuditInspectionStatus::Open && result.oldest_open &&
        std::holds_alternative<AbsentAccountAuditPinsByPolicy>(pins) &&
        (result.oldest_open->keyed || result.oldest_open->has_spool)) {
        result.status = AccountAuditInspectionStatus::Contradiction;
        result.failure = {AccountAuditDurabilityReason::Contradiction,
                          "store-dependent audit group contradicts absent store policy"};
    }
    if (hooks_ && hooks_->before_final_classification) {
        hooks_->before_final_classification();
    }
    AccountAuditFailure interruption;
    if (scan_interrupted(guard.scan_control_, interruption)) {
        result.status = AccountAuditInspectionStatus::Interrupted;
        result.failure = std::move(interruption);
    }
    if (result.status != AccountAuditInspectionStatus::Open &&
        result.status != AccountAuditInspectionStatus::Clean) {
        permit = AccountAuditRecoveryPermit{};
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
                                    AccountAuditAppendPermit permit,
                                    AccountAuditCoordinator::Guard& guard,
                                    AccountAuditAppendReceipt& receipt,
                                    AccountAuditFailure& failure) const {
    std::string lease_error;
    if (!guard.validate_lease(state_directory_, account_, expected_uid_, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return false;
    }
    if (!permit.valid() || permit.implementation_->state_directory != state_directory_ ||
        permit.implementation_->account != account_ ||
        permit.implementation_->expected_uid != expected_uid_) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "append permit is invalid or bound to another account"};
        return false;
    }
    auto& permitted = *permit.implementation_;
    const auto line = serialize_account_audit_record(intent.document()) + '\n';
    if (line != permitted.intent_line) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "append intent differs from its validated permit"};
        return false;
    }
    const auto operation =
        parse_account_audit_operation(intent.document()["command"].get<std::string>());
    if (!operation) {
        failure = {AccountAuditDurabilityReason::SchemaError,
                   "typed intent lost its operation identity"};
        return false;
    }
    if ((*operation == AccountAuditOperation::SessionTerminate) != permitted.absent_by_policy) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "pin source contradicts the operation policy"};
        return false;
    }
    permitted.consumed = true;
    const Descriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0 || !valid_directory_metadata(directory.get(), expected_uid_, failure)) {
        if (directory.get() < 0) {
            failure.reason = AccountAuditDurabilityReason::OpenFailed;
        }
        return false;
    }
    std::vector<std::uint64_t> retained_generations;
    for (const auto& hold : permitted.holds) {
        if (!hold.released) {
            retained_generations.push_back(hold.seed.audit_generation);
        }
    }
    if (!rotate_for_intent(directory.get(), expected_uid_, line.size(), permitted.absent_by_policy,
                           permitted.pins, retained_generations, permitted.segments,
                           guard.scan_control_, hooks_, failure)) {
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
    receipt.operation = *operation;
    receipt.coordinator_ = guard.owner_;
    return guard.enter_post_intent_durability(receipt, failure);
}

bool AccountAuditLog::append_checkpoint(const AccountAuditCheckpoint& checkpoint,
                                        const AccountAuditCoordinator::Guard& guard,
                                        AccountAuditFailure& failure) const {
    const auto inspection = inspect(guard);
    if (inspection.status != AccountAuditInspectionStatus::Open || !inspection.oldest_open) {
        failure = (inspection.status == AccountAuditInspectionStatus::Unavailable ||
                   inspection.status == AccountAuditInspectionStatus::Interrupted)
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
        failure = (inspection.status == AccountAuditInspectionStatus::Unavailable ||
                   inspection.status == AccountAuditInspectionStatus::Interrupted)
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

std::optional<AccountAuditSpoolHold>
AccountAuditLog::hold_current_spool(const AccountAuditAppendReceipt& receipt, const SpoolRef& spool,
                                    const AccountAuditCoordinator::Guard& guard,
                                    AccountAuditFailure& failure) const {
    if (!guard.valid() || receipt.coordinator_ != guard.owner_ || receipt.audit_generation == 0 ||
        receipt.operation != AccountAuditOperation::SavedAttach || receipt.invocation_id.empty() ||
        !valid_spool_reference(spool, receipt.invocation_id)) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "current spool hold does not match its durable intent receipt"};
        return std::nullopt;
    }
    std::string lease_error;
    if (!guard.validate_lease(state_directory_, account_, expected_uid_, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        return std::nullopt;
    }
    failure = {};
    return AccountAuditSpoolHold(next_spool_permit_id(), 1, state_directory_, account_,
                                 expected_uid_, guard.owner_, receipt.audit_generation,
                                 receipt.invocation_id, spool);
}

bool AccountAuditLog::release_current_spool(AccountAuditSpoolReleaseReceipt release,
                                            const AccountAuditAppendReceipt& receipt,
                                            const AccountAuditCoordinator::Guard& guard,
                                            AccountAuditFailure& failure) const {
    if (!release.valid() || !guard.valid() || receipt.coordinator_ != guard.owner_ ||
        release.coordinator_ != guard.owner_ || release.state_directory_ != state_directory_ ||
        release.account_ != account_ || release.expected_uid_ != expected_uid_ ||
        release.audit_generation_ != receipt.audit_generation ||
        release.invocation_id_ != receipt.invocation_id) {
        failure = {AccountAuditDurabilityReason::Contradiction,
                   "current spool release does not match its durable intent receipt"};
        release.invalidate();
        return false;
    }
    std::string lease_error;
    if (!guard.validate_lease(state_directory_, account_, expected_uid_, lease_error)) {
        failure = {AccountAuditDurabilityReason::LockFailed, std::move(lease_error)};
        release.invalidate();
        return false;
    }
    release.invalidate();
    failure = {};
    return true;
}

} // namespace tgcli::daemon
