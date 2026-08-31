#include "daemon/raw_audit_log.hpp"

#include "daemon/raw_audit_contract.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tgcli::daemon::raw::audit_v3 {

namespace {

using nlohmann::json;

constexpr std::string_view kFilename = "raw-audit-v3.jsonl";
constexpr std::size_t kMaximumAuditBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumAuditRecords = 262'144;

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
    [[nodiscard]] int get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ >= 0;
    }

  private:
    int value_ = -1;
};

bool private_directory(int descriptor, uid_t expected_uid) {
    struct stat metadata {};
    return ::fstat(descriptor, &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
           metadata.st_uid == expected_uid && (metadata.st_mode & 0077) == 0;
}

bool private_file(int descriptor, uid_t expected_uid) {
    struct stat metadata {};
    return ::fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
           metadata.st_uid == expected_uid && metadata.st_nlink == 1 &&
           (metadata.st_mode & 0077) == 0 && metadata.st_size >= 0 &&
           static_cast<std::uint64_t>(metadata.st_size) <= kMaximumAuditBytes;
}

bool write_all(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto written = ::write(descriptor, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

std::optional<json> parse_record(std::string_view line) {
    std::vector<std::set<std::string, std::less<>>> object_keys;
    bool duplicate_key = false;
    const auto reject_duplicates =
        [&object_keys, &duplicate_key](int /*depth*/, json::parse_event_t event, json& value) {
            if (event == json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == json::parse_event_t::key) {
                if (object_keys.empty() ||
                    !object_keys.back().insert(value.get<std::string>()).second) {
                    duplicate_key = true;
                }
            } else if (event == json::parse_event_t::object_end && !object_keys.empty()) {
                object_keys.pop_back();
            }
            return !duplicate_key;
        };
    auto record = json::parse(line.begin(), line.end(), reject_duplicates, false);
    if (record.is_discarded() || duplicate_key) {
        return std::nullopt;
    }
    return record;
}

std::optional<std::vector<json>> read_records(int directory, uid_t expected_uid) {
    const Descriptor file(::openat(directory, kFilename.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file) {
        return errno == ENOENT ? std::optional<std::vector<json>>{std::vector<json>{}}
                               : std::nullopt;
    }
    if (!private_file(file.get(), expected_uid)) {
        return std::nullopt;
    }
    struct stat metadata {};
    if (::fstat(file.get(), &metadata) != 0) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::pread(file.get(), bytes.data() + offset, bytes.size() - offset,
                                   static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (!bytes.empty() && bytes.back() != '\n') {
        return std::nullopt;
    }
    std::vector<json> records;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\n', begin);
        if (end == std::string::npos || end == begin || records.size() == kMaximumAuditRecords) {
            return std::nullopt;
        }
        auto record = parse_record(
            std::string_view(bytes).substr(begin, static_cast<std::size_t>(end - begin)));
        if (!record) {
            return std::nullopt;
        }
        records.push_back(std::move(*record));
        begin = end + 1;
    }
    return records;
}

bool append_record(int directory, uid_t expected_uid, const json& record) {
    const bool existed = ::faccessat(directory, kFilename.data(), F_OK, AT_SYMLINK_NOFOLLOW) == 0;
    const Descriptor file(::openat(directory, kFilename.data(),
                                   O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!file || !private_file(file.get(), expected_uid)) {
        return false;
    }
    struct stat metadata {};
    std::string line = record.dump();
    line.push_back('\n');
    if (::fstat(file.get(), &metadata) != 0 || metadata.st_size < 0 ||
        line.size() > kMaximumAuditBytes - static_cast<std::size_t>(metadata.st_size) ||
        !write_all(file.get(), line) || ::fsync(file.get()) != 0) {
        return false;
    }
    return existed || ::fsync(directory) == 0;
}

bool valid_record(const json& record) {
    if (!record.is_object() || !record.contains("record_type") ||
        !record["record_type"].is_string()) {
        return false;
    }
    const auto& type = record["record_type"].get_ref<const std::string&>();
    return (type == "raw_intent" && valid_intent(record)) ||
           (type == "raw_checkpoint" && valid_checkpoint(record)) ||
           (type == "raw_outcome" && valid_outcome(record));
}

struct Group {
    const json* intent = nullptr;
    const json* response = nullptr;
};

std::map<std::string, Group, std::less<>> group_records(std::span<const json> records) {
    std::map<std::string, Group, std::less<>> groups;
    for (const auto& record : records) {
        if (!record.is_object() || !record.contains("invocation_id") ||
            !record["invocation_id"].is_string()) {
            continue;
        }
        auto& group = groups[record["invocation_id"].get<std::string>()];
        if (record.value("record_type", "") == "raw_intent") {
            group.intent = &record;
        } else if (record.value("stage", "") == "raw_response_received") {
            group.response = &record;
        }
    }
    return groups;
}

json outcome_for(const RecoveryDecision& decision, const Group& group) {
    json terminal = nullptr;
    std::string_view mutation = "none";
    if (decision.action == RecoveryAction::EmitUnconfirmed) {
        mutation = "possible";
        terminal = {{"kind", "error_summary"},
                    {"code", "RAW_OUTCOME_UNCONFIRMED"},
                    {"td_error_code", nullptr}};
    } else if (decision.action == RecoveryAction::RepairConfirmedResult) {
        mutation = "confirmed";
        const auto& data = (*group.response)["data"];
        terminal = {{"kind", "result_digest"},
                    {"response_type", data["response_type"]},
                    {"response_sha256", data["response_sha256"]},
                    {"response_bytes", data["response_bytes"]}};
    } else if (decision.action == RecoveryAction::RepairPossibleError) {
        mutation = "possible";
        const auto& data = (*group.response)["data"];
        terminal = {{"kind", "error_summary"},
                    {"code", data["td_error_code"] == 429 ? "RATE_LIMITED" : "TDLIB_ERROR"},
                    {"td_error_code", data["td_error_code"]}};
    } else if (decision.action == RecoveryAction::RepairPossibleInternal) {
        mutation = "possible";
        terminal = {{"kind", "error_summary"},
                    {"code", "INTERNAL"},
                    {"reason", "unexpected_response"},
                    {"td_error_code", nullptr}};
    } else if (decision.action == RecoveryAction::RepairPossibleTooLarge) {
        mutation = "possible";
        terminal = {{"kind", "error_summary"},
                    {"code", "INTERNAL"},
                    {"reason", "result_too_large"},
                    {"td_error_code", nullptr}};
    }
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", decision.invocation_id},
            {"mutation_state", mutation},
            {"terminal", std::move(terminal)}};
}

} // namespace

Log::Log(std::string state_directory, uid_t expected_uid)
    : state_directory_(std::move(state_directory)),
      path_(state_directory_ + "/" + std::string(kFilename)), expected_uid_(expected_uid) {}

const std::string& Log::path() const noexcept {
    return path_;
}

bool Log::append(const nlohmann::json& record) const {
    if (!valid_record(record)) {
        return false;
    }
    const Descriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    return directory && private_directory(directory.get(), expected_uid_) &&
           append_record(directory.get(), expected_uid_, record);
}

LogRecovery Log::recover() const {
    const Descriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!directory || !private_directory(directory.get(), expected_uid_)) {
        return {.status = LogStatus::Unavailable, .unconfirmed = std::nullopt};
    }
    auto records = read_records(directory.get(), expected_uid_);
    if (!records) {
        return {.status = LogStatus::Unavailable, .unconfirmed = std::nullopt};
    }
    const auto scanned = scan(*records);
    if (!scanned.valid) {
        return {.status = LogStatus::Contradiction, .unconfirmed = std::nullopt};
    }
    const auto groups = group_records(*records);
    bool repaired = false;
    std::optional<UnconfirmedTerminal> unconfirmed;
    for (const auto& decision : scanned.decisions) {
        if (decision.action == RecoveryAction::Complete) {
            continue;
        }
        if (decision.action == RecoveryAction::FailClosed) {
            return {.status = LogStatus::Contradiction, .unconfirmed = std::nullopt};
        }
        const auto group = groups.find(decision.invocation_id);
        if (group == groups.end() || group->second.intent == nullptr ||
            ((decision.action == RecoveryAction::RepairConfirmedResult ||
              decision.action == RecoveryAction::RepairPossibleError ||
              decision.action == RecoveryAction::RepairPossibleInternal ||
              decision.action == RecoveryAction::RepairPossibleTooLarge) &&
             group->second.response == nullptr)) {
            return {.status = LogStatus::Contradiction, .unconfirmed = std::nullopt};
        }
        auto outcome = outcome_for(decision, group->second);
        if (!valid_outcome(outcome) || !append_record(directory.get(), expected_uid_, outcome)) {
            return {.status = LogStatus::Unavailable, .unconfirmed = std::nullopt};
        }
        repaired = true;
        if (decision.action == RecoveryAction::EmitUnconfirmed) {
            unconfirmed = UnconfirmedTerminal{
                .function = (*group->second.intent)["function"].get<std::string>(),
                .request_sha256 = (*group->second.intent)["request_sha256"].get<std::string>()};
        }
    }
    if (unconfirmed) {
        return {.status = LogStatus::Unconfirmed, .unconfirmed = std::move(unconfirmed)};
    }
    return {.status = repaired ? LogStatus::Repaired : LogStatus::Clean,
            .unconfirmed = std::nullopt};
}

} // namespace tgcli::daemon::raw::audit_v3
