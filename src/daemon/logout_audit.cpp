#include "daemon/logout_audit.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

class FileDescriptor final {
  public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
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

bool injected(const std::shared_ptr<const testing::LogoutAuditHooks>& hooks,
              LogoutAuditFault fault) {
    return hooks && hooks->should_fail && hooks->should_fail(fault);
}

bool valid_directory(int descriptor, uid_t expected_uid) {
    struct stat metadata {};
    return ::fstat(descriptor, &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
           metadata.st_uid == expected_uid && (metadata.st_mode & 07777) == 0700;
}

bool valid_file(int descriptor, uid_t expected_uid) {
    struct stat metadata {};
    return ::fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
           metadata.st_uid == expected_uid && (metadata.st_mode & 07777) == 0600 &&
           metadata.st_nlink == 1;
}

bool valid_named_file(int directory, const char* name, uid_t expected_uid, bool& present) {
    struct stat metadata {};
    if (::fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        present = false;
        return errno == ENOENT;
    }
    present = true;
    return S_ISREG(metadata.st_mode) && metadata.st_uid == expected_uid &&
           (metadata.st_mode & 07777) == 0600 && metadata.st_nlink == 1;
}

bool rotate_if_needed(int directory, uid_t expected_uid, std::size_t incoming_size,
                      bool begin_group,
                      const std::shared_ptr<const testing::LogoutAuditHooks>& hooks,
                      LogoutAuditFailure& failure) {
    const auto rotation_bytes = hooks ? hooks->rotation_bytes : std::size_t{32} * 1024 * 1024;
    struct stat current {};
    if (!begin_group || (::fstatat(directory, "audit.log", &current, AT_SYMLINK_NOFOLLOW) != 0 &&
                         errno == ENOENT)) {
        return true;
    }
    if (!S_ISREG(current.st_mode) || current.st_uid != expected_uid ||
        (current.st_mode & 07777) != 0600 || current.st_nlink != 1) {
        failure.reason = "path_invalid";
        return false;
    }
    if (current.st_size < 0 ||
        static_cast<std::uint64_t>(current.st_size) + incoming_size <= rotation_bytes) {
        return true;
    }
    if (injected(hooks, LogoutAuditFault::Rotate)) {
        failure.reason = "rotate_failed";
        return false;
    }

    constexpr std::array<const char*, 5> names{"audit.log", "audit.log.1", "audit.log.2",
                                               "audit.log.3", "audit.log.4"};
    for (const auto* name : names) {
        bool present = false;
        if (!valid_named_file(directory, name, expected_uid, present)) {
            failure.reason = "path_invalid";
            return false;
        }
    }
    bool oldest_present = false;
    if (!valid_named_file(directory, names.back(), expected_uid, oldest_present)) {
        failure.reason = "path_invalid";
        return false;
    }
    if (oldest_present && ::unlinkat(directory, names.back(), 0) != 0) {
        failure.reason = "rotate_failed";
        return false;
    }
    for (std::size_t index = names.size() - 1; index > 0; --index) {
        bool present = false;
        if (!valid_named_file(directory, names.at(index - 1), expected_uid, present)) {
            failure.reason = "path_invalid";
            return false;
        }
        if (present &&
            ::renameat(directory, names.at(index - 1), directory, names.at(index)) != 0) {
            failure.reason = "rotate_failed";
            return false;
        }
    }
    if (::fsync(directory) != 0) {
        failure.reason = "rotate_failed";
        return false;
    }
    return true;
}

bool write_all(int descriptor, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
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

bool exact_fields(const nlohmann::json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) {
        return false;
    }
    return std::all_of(fields.begin(), fields.end(), [&value](std::string_view field) {
        return value.contains(std::string(field));
    });
}

bool valid_invocation_id(std::string_view value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

std::optional<std::string>
read_named_file(int directory, const char* name, uid_t expected_uid,
                const std::shared_ptr<const testing::LogoutAuditHooks>& hooks,
                LogoutAuditFailure& failure) {
    const FileDescriptor file(::openat(directory, name, O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
        if (errno == ENOENT) {
            return std::string{};
        }
        failure.reason = "open_failed";
        return std::nullopt;
    }
    if (!valid_file(file.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    if (injected(hooks, LogoutAuditFault::InspectSync) || ::fsync(file.get()) != 0) {
        failure.reason = "sync_failed";
        return std::nullopt;
    }
    struct stat metadata {};
    constexpr std::uint64_t kReadLimit = std::uint64_t{64} * 1024 * 1024;
    if (::fstat(file.get(), &metadata) != 0 || metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > kReadLimit) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            failure.reason = "open_failed";
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }
    return bytes;
}

struct PendingAudit {
    proto::LogoutPlan plan;
    std::vector<AuditStage> stages;
};

std::optional<AuthoritySource> parse_authority_source(const nlohmann::json& value) {
    if (value == "request") {
        return AuthoritySource::Request;
    }
    if (value == "config") {
        return AuthoritySource::Config;
    }
    return std::nullopt;
}

std::optional<ConfirmationSource> parse_confirmation_source(const nlohmann::json& value) {
    if (value == "yes") {
        return ConfirmationSource::Yes;
    }
    if (value == "tty") {
        return ConfirmationSource::Tty;
    }
    return std::nullopt;
}

using PendingAudits = std::map<std::string, PendingAudit, std::less<>>;
using SeenInvocations = std::set<std::string, std::less<>>;

std::string audit_timestamp(const nlohmann::json& record) {
    return record["timestamp"].is_string() ? record["timestamp"].get<std::string>() : std::string{};
}

bool consume_intent(const nlohmann::json& record, std::string_view account,
                    const std::string& invocation, PendingAudits& pending,
                    SeenInvocations& seen_invocations) {
    if (!exact_fields(record, {"schema_version", "phase", "invocation_id", "timestamp", "account",
                               "command", "arguments", "plan", "config_snapshot",
                               "authority_source", "confirmation_source"}) ||
        record["schema_version"] != 1 || record["arguments"] != nlohmann::json::object() ||
        seen_invocations.contains(invocation)) {
        return false;
    }
    std::string error;
    auto plan = proto::parse_logout_plan(record["plan"], error);
    const auto authority = parse_authority_source(record["authority_source"]);
    const auto confirmation = parse_confirmation_source(record["confirmation_source"]);
    if (!plan || plan->account() != account || !record["config_snapshot"].is_string() ||
        !authority || !confirmation) {
        return false;
    }
    auto canonical = make_logout_audit_intent({invocation, audit_timestamp(record)}, *plan,
                                              record["config_snapshot"].get<std::string>(),
                                              *authority, *confirmation, error);
    if (!canonical || serialize(*canonical) != record) {
        return false;
    }
    pending.emplace(invocation, PendingAudit{std::move(*plan), {AuditStage::IntentSynced}});
    seen_invocations.emplace(invocation);
    return true;
}

bool consume_checkpoint(const nlohmann::json& record, const std::string& invocation,
                        PendingAudit& pending) {
    if (!exact_fields(record, {"schema_version", "phase", "invocation_id", "timestamp", "account",
                               "command", "stage"}) ||
        record["schema_version"] != 1 || !record["stage"].is_string()) {
        return false;
    }
    const auto stage = parse_audit_stage(record["stage"].get_ref<const std::string&>());
    if (!stage ||
        (*stage != AuditStage::LogoutSendStarted && *stage != AuditStage::LogoutClosedConfirmed)) {
        return false;
    }
    auto candidate = pending.stages;
    candidate.push_back(*stage);
    std::string error;
    if (!validate_audit_stage_prefix(DestructiveCommand::Logout, candidate, error)) {
        return false;
    }
    auto canonical = make_logout_audit_checkpoint({invocation, audit_timestamp(record)},
                                                  pending.plan, *stage, error);
    if (!canonical || serialize(*canonical) != record) {
        return false;
    }
    pending.stages = std::move(candidate);
    return true;
}

bool valid_outcome_shape(const nlohmann::json& record, std::size_t stage_count) {
    return exact_fields(record, {"schema_version", "phase", "invocation_id", "timestamp", "account",
                                 "command", "success", "mutation_state", "completed_stages",
                                 "result", "error"}) &&
           record["schema_version"] == 1 && record["success"].is_boolean() &&
           record["mutation_state"].is_string() && record["completed_stages"].is_array() &&
           record["completed_stages"].size() == stage_count;
}

bool completed_stages_match(const nlohmann::json& record, const std::vector<AuditStage>& expected) {
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& stage = record["completed_stages"].at(index);
        if (!stage.is_string() || stage != audit_stage_name(expected.at(index))) {
            return false;
        }
    }
    return true;
}

bool consume_outcome(const nlohmann::json& record, const std::string& invocation,
                     const PendingAudit& pending) {
    if (!valid_outcome_shape(record, pending.stages.size()) ||
        !completed_stages_match(record, pending.stages)) {
        return false;
    }
    std::string error;
    const AuditRecordIdentity identity{invocation, audit_timestamp(record)};
    std::optional<AuditOutcome> canonical;
    if (record["success"].get<bool>()) {
        if (!record["error"].is_null()) {
            return false;
        }
        canonical =
            make_logout_success_audit_outcome(identity, pending.plan, pending.stages, error);
    } else {
        if (!record["result"].is_null()) {
            return false;
        }
        auto outcome_error = parse_structured_outcome_error(record["error"], error);
        if (!outcome_error) {
            return false;
        }
        canonical = make_failure_audit_outcome(identity, proto::DestructivePlan{pending.plan},
                                               pending.stages, *outcome_error, error);
    }
    return canonical && serialize(*canonical) == record;
}

bool consume_audit_line(const nlohmann::json& record, std::string_view account,
                        PendingAudits& pending, SeenInvocations& seen_invocations) {
    if (!record.is_object() || !record.contains("phase") || !record["phase"].is_string() ||
        !record.contains("invocation_id") || !record["invocation_id"].is_string() ||
        !valid_invocation_id(record["invocation_id"].get_ref<const std::string&>()) ||
        !record.contains("account") || !record["account"].is_string() ||
        record["account"] != account || !record.contains("command") ||
        record["command"] != "logout") {
        return false;
    }
    const auto& invocation = record["invocation_id"].get_ref<const std::string&>();
    const auto& phase = record["phase"].get_ref<const std::string&>();
    if (phase == "intent") {
        return consume_intent(record, account, invocation, pending, seen_invocations);
    }
    const auto found = pending.find(invocation);
    if (found == pending.end()) {
        return false;
    }
    if (phase == "checkpoint") {
        return consume_checkpoint(record, invocation, found->second);
    }
    if (phase != "outcome" || !consume_outcome(record, invocation, found->second)) {
        return false;
    }
    pending.erase(found);
    return true;
}

std::optional<IncompleteLogoutAudit> take_first_incomplete(PendingAudits& pending) {
    if (pending.empty()) {
        return std::nullopt;
    }
    auto first = pending.begin();
    return IncompleteLogoutAudit{first->first, std::move(first->second.plan),
                                 std::move(first->second.stages)};
}

LogoutAuditInspection invalid_inspection(LogoutAuditFailure failure, PendingAudits& pending) {
    return {LogoutAuditInspectionStatus::Invalid, take_first_incomplete(pending),
            std::move(failure)};
}

} // namespace

LogoutAuditLog::LogoutAuditLog(std::string state_directory, std::string account, uid_t expected_uid,
                               std::shared_ptr<const testing::LogoutAuditHooks> hooks)
    : state_directory_(std::move(state_directory)), account_(std::move(account)),
      expected_uid_(expected_uid), hooks_(std::move(hooks)) {
    audit_path_ = state_directory_ + "/audit.log";
}

const std::string& LogoutAuditLog::path() const {
    return audit_path_;
}

bool LogoutAuditLog::append(const nlohmann::json& record, LogoutAuditFailure& failure,
                            bool begin_group) const {
    const auto slash = path().rfind('/');
    if (slash == std::string::npos || path().substr(0, slash) != state_directory_) {
        failure.reason = "path_invalid";
        return false;
    }
    if (injected(hooks_, LogoutAuditFault::Open)) {
        failure.reason = "open_failed";
        return false;
    }

    const FileDescriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0 || !valid_directory(directory.get(), expected_uid_)) {
        failure.reason = "path_invalid";
        return false;
    }

    const std::string line = record.dump() + '\n';
    if (!rotate_if_needed(directory.get(), expected_uid_, line.size(), begin_group, hooks_,
                          failure)) {
        return false;
    }

    const FileDescriptor file(::openat(directory.get(), "audit.log",
                                       O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                       0600));
    if (file.get() < 0) {
        failure.reason = "open_failed";
        return false;
    }
    if (::flock(file.get(), LOCK_EX) != 0 || !valid_file(file.get(), expected_uid_)) {
        failure.reason = "path_invalid";
        return false;
    }

    if (injected(hooks_, LogoutAuditFault::Write) || !write_all(file.get(), line)) {
        failure.reason = "write_failed";
        return false;
    }
    if (injected(hooks_, LogoutAuditFault::Sync) || ::fsync(file.get()) != 0 ||
        ::fsync(directory.get()) != 0) {
        failure.reason = "sync_failed";
        return false;
    }
    if (hooks_ && hooks_->after_sync) {
        hooks_->after_sync(record.value("phase", std::string{}));
    }
    failure.reason.clear();
    return true;
}

LogoutAuditInspection LogoutAuditLog::inspect() const {
    LogoutAuditFailure failure;
    const FileDescriptor directory(
        ::open(state_directory_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directory.get() < 0 && errno == ENOENT) {
        return {};
    }
    if (directory.get() < 0 || !valid_directory(directory.get(), expected_uid_)) {
        failure.reason = "path_invalid";
        return {LogoutAuditInspectionStatus::Invalid, std::nullopt, std::move(failure)};
    }
    if (injected(hooks_, LogoutAuditFault::InspectSync) || ::fsync(directory.get()) != 0) {
        failure.reason = "sync_failed";
        return {LogoutAuditInspectionStatus::Invalid, std::nullopt, std::move(failure)};
    }

    constexpr std::array<const char*, 5> names{"audit.log.4", "audit.log.3", "audit.log.2",
                                               "audit.log.1", "audit.log"};
    std::map<std::string, PendingAudit, std::less<>> pending;
    std::set<std::string, std::less<>> seen_invocations;
    for (const auto* name : names) {
        auto bytes = read_named_file(directory.get(), name, expected_uid_, hooks_, failure);
        if (!bytes) {
            return invalid_inspection(std::move(failure), pending);
        }
        auto contents = std::move(bytes).value();
        if (contents.empty()) {
            continue;
        }
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const auto end = contents.find('\n', offset);
            if (end == std::string::npos) {
                failure.reason = "path_invalid";
                return invalid_inspection(std::move(failure), pending);
            }
            if (end == offset) {
                failure.reason = "path_invalid";
                return invalid_inspection(std::move(failure), pending);
            }
            auto parsed = nlohmann::json::parse(
                contents.begin() + static_cast<std::ptrdiff_t>(offset),
                contents.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
            if (parsed.is_discarded() ||
                !consume_audit_line(parsed, account_, pending, seen_invocations)) {
                failure.reason = "path_invalid";
                return invalid_inspection(std::move(failure), pending);
            }
            offset = end + 1;
        }
    }
    if (pending.empty()) {
        return {};
    }
    return {LogoutAuditInspectionStatus::Incomplete, take_first_incomplete(pending), {}};
}

LogoutAuditReconcileResult
reconcile_definite_logout_audit(const LogoutAuditLog& audit,
                                const std::function<std::string()>& timestamp) {
    for (;;) {
        auto inspection = audit.inspect();
        if (inspection.status == LogoutAuditInspectionStatus::Clean) {
            return {};
        }
        if (inspection.status == LogoutAuditInspectionStatus::Invalid || !inspection.incomplete) {
            return {LogoutAuditReconcileStatus::Invalid, std::move(inspection.incomplete),
                    std::move(inspection.failure)};
        }

        auto incomplete = std::move(*inspection.incomplete);
        if (incomplete.completed_stages.size() > 1 &&
            incomplete.completed_stages.back() != AuditStage::LogoutClosedConfirmed) {
            return {LogoutAuditReconcileStatus::ObservationRequired, std::move(incomplete), {}};
        }

        const AuditRecordIdentity identity{incomplete.invocation_id, timestamp()};
        std::string error;
        std::optional<AuditOutcome> outcome;
        if (incomplete.completed_stages.size() == 1) {
            auto structured = parse_structured_outcome_error(
                {{"code", "INTERNAL"},
                 {"details", {{"operation", "logout"}, {"reason", "internal_error"}}}},
                error);
            if (structured) {
                outcome =
                    make_failure_audit_outcome(identity, proto::DestructivePlan{incomplete.plan},
                                               incomplete.completed_stages, *structured, error);
            }
        } else {
            outcome = make_logout_success_audit_outcome(identity, incomplete.plan,
                                                        incomplete.completed_stages, error);
        }

        LogoutAuditFailure failure;
        if (!outcome || !audit.append(serialize(*outcome), failure)) {
            return {LogoutAuditReconcileStatus::AppendFailed, std::move(incomplete),
                    std::move(failure)};
        }
    }
}

} // namespace tgcli::daemon
