#include "daemon/removal_journal.hpp"

#include "common/paths.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::size_t kTombstoneLimit = std::size_t{1024} * 1024;
constexpr std::size_t kAuditLimit = std::size_t{64} * 1024 * 1024;

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
    explicit operator bool() const {
        return value_ >= 0;
    }

  private:
    int value_;
};

struct OpenDirectoryResult {
    Descriptor descriptor;
    bool missing = false;
};

std::atomic<std::uint64_t>& temporary_sequence() {
    static std::atomic<std::uint64_t> sequence = 0;
    return sequence;
}

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) {
        return false;
    }
    return std::ranges::all_of(
        fields, [&value](std::string_view field) { return value.contains(std::string(field)); });
}

bool valid_invocation_id(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool injected(const std::shared_ptr<const testing::RemovalJournalHooks>& hooks,
              RemovalJournalFault fault) {
    return hooks && hooks->should_fail && hooks->should_fail(fault);
}

bool valid_private_directory(int descriptor, uid_t expected_uid) {
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

Descriptor acquire_journal_lock(int directory, uid_t expected_uid, RemovalJournalFailure& failure) {
    Descriptor lock(
        ::openat(directory, ".journal.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!lock || !valid_file(lock.get(), expected_uid) || ::flock(lock.get(), LOCK_EX) != 0) {
        failure.reason = "path_invalid";
        return Descriptor{};
    }
    return lock;
}

OpenDirectoryResult open_directory_tree(const std::string& directory, uid_t expected_uid,
                                        bool create, RemovalJournalFailure& failure) {
    const std::filesystem::path candidate(directory);
    if (!candidate.is_absolute() || candidate == candidate.root_path() ||
        candidate.lexically_normal().generic_string() != directory) {
        failure.reason = "path_invalid";
        return {Descriptor{}, false};
    }
    Descriptor current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (!current) {
        failure.reason = "open_failed";
        return {Descriptor{}, false};
    }
    std::vector<std::string> components;
    for (const auto& component : candidate.relative_path()) {
        components.push_back(component.string());
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        int raw = ::openat(current.get(), components[index].c_str(),
                           O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (raw < 0 && errno == ENOENT && create) {
            if (::mkdirat(current.get(), components[index].c_str(), 0700) != 0 && errno != EEXIST) {
                failure.reason = "open_failed";
                return {Descriptor{}, false};
            }
            raw = ::openat(current.get(), components[index].c_str(),
                           O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        }
        if (raw < 0) {
            if (errno == ENOENT && !create) {
                failure.reason.clear();
                return {Descriptor{}, true};
            }
            failure.reason = errno == ELOOP ? "path_invalid" : "open_failed";
            return {Descriptor{}, false};
        }
        current = Descriptor(raw);
        if (index + 2 >= components.size() &&
            !valid_private_directory(current.get(), expected_uid)) {
            failure.reason = "path_invalid";
            return {Descriptor{}, false};
        }
    }
    failure.reason.clear();
    return {std::move(current), false};
}

OpenDirectoryResult
open_journal_directory(const std::string& directory, uid_t expected_uid, bool create,
                       const std::shared_ptr<const testing::RemovalJournalHooks>& hooks,
                       RemovalJournalFailure& failure) {
    if (injected(hooks, RemovalJournalFault::DirectoryOpen)) {
        failure.reason = "open_failed";
        return {Descriptor{}, false};
    }
    return open_directory_tree(directory, expected_uid, create, failure);
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

std::optional<std::string> read_file(int directory, const std::string& name, uid_t expected_uid,
                                     std::size_t limit, bool allow_missing,
                                     RemovalJournalFailure& failure) {
    const Descriptor file(::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file) {
        if (allow_missing && errno == ENOENT) {
            failure.reason.clear();
            return std::string{};
        }
        failure.reason = errno == ELOOP ? "path_invalid" : "open_failed";
        return std::nullopt;
    }
    if (!valid_file(file.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    struct stat metadata {};
    if (::fstat(file.get(), &metadata) != 0 || metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > limit) {
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
    failure.reason.clear();
    return bytes;
}

std::optional<AuditStage> expected_next(const RemovalTombstone& tombstone) {
    switch (tombstone.stage) {
    case AuditStage::Planned:
        return AuditStage::IntentSynced;
    case AuditStage::IntentSynced:
        return tombstone.plan.keep_session() ? std::optional{AuditStage::RemoteKept} : std::nullopt;
    case AuditStage::RemoteLogoutSendStarted:
        return std::nullopt;
    case AuditStage::RemoteConfirmed:
    case AuditStage::RemoteNotPresent:
    case AuditStage::RemoteKept:
        return AuditStage::ClientCloseStarted;
    case AuditStage::ClientCloseStarted:
        return AuditStage::ClientClosed;
    case AuditStage::ClientClosed:
        return AuditStage::ConfigRemoveStarted;
    case AuditStage::ConfigRemoveStarted:
        return AuditStage::ConfigRemoved;
    case AuditStage::ConfigRemoved:
        return AuditStage::DataRemoveStarted;
    case AuditStage::DataRemoveStarted:
        return AuditStage::DataRemoved;
    case AuditStage::DataRemoved:
        return AuditStage::StateRemoveStarted;
    case AuditStage::StateRemoveStarted:
        return AuditStage::StateRemoved;
    case AuditStage::StateRemoved:
        return AuditStage::OutcomeSynced;
    case AuditStage::OutcomeSynced:
    case AuditStage::LogoutSendStarted:
    case AuditStage::LogoutClosedConfirmed:
        return std::nullopt;
    }
    return std::nullopt;
}

bool stages_match_plan(const proto::AccountRemovePlan& plan,
                       const std::vector<AuditStage>& stages) {
    const bool kept = std::ranges::find(stages, AuditStage::RemoteKept) != stages.end();
    const bool remote =
        std::ranges::find(stages, AuditStage::RemoteLogoutSendStarted) != stages.end() ||
        std::ranges::find(stages, AuditStage::RemoteConfirmed) != stages.end() ||
        std::ranges::find(stages, AuditStage::RemoteNotPresent) != stages.end();
    if (!kept && !remote) {
        return true;
    }
    return (plan.keep_session() ? kept && !remote : !kept);
}

std::optional<RemovalTombstone> parse_tombstone(const json& value, RemovalJournalFailure& failure) {
    if (!exact_fields(value,
                      {"schema_version", "invocation_id", "account", "stage", "completed_stages",
                       "next_stage", "plan", "config_snapshot", "data_root", "state_root"}) ||
        value["schema_version"] != 1 || !value["invocation_id"].is_string() ||
        !valid_invocation_id(value["invocation_id"].get_ref<const std::string&>()) ||
        !value["account"].is_string() || !paths::valid_account_name(value["account"]) ||
        !value["stage"].is_string() || !value["completed_stages"].is_array() ||
        (!value["next_stage"].is_null() && !value["next_stage"].is_string()) ||
        !value["config_snapshot"].is_string()) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    std::string plan_error;
    auto plan = proto::parse_account_remove_plan(value["plan"], plan_error);
    if (!plan || plan->account() != value["account"].get_ref<const std::string&>() ||
        plan->config_snapshot() != value["config_snapshot"].get_ref<const std::string&>() ||
        value["data_root"] != value["plan"]["data_root"] ||
        value["state_root"] != value["plan"]["state_root"]) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    std::vector<AuditStage> stages;
    for (const auto& entry : value["completed_stages"]) {
        if (!entry.is_string()) {
            failure.reason = "path_invalid";
            return std::nullopt;
        }
        const auto parsed = parse_audit_stage(entry.get_ref<const std::string&>());
        if (!parsed) {
            failure.reason = "path_invalid";
            return std::nullopt;
        }
        stages.push_back(*parsed);
    }
    const auto stage = parse_audit_stage(value["stage"].get_ref<const std::string&>());
    if (!stage || stages.empty() || stages.back() != *stage) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    auto prefix = stages;
    if (*stage == AuditStage::OutcomeSynced) {
        prefix.pop_back();
        if (prefix.size() < 2 || prefix[0] != AuditStage::Planned ||
            prefix[1] != AuditStage::IntentSynced) {
            failure.reason = "path_invalid";
            return std::nullopt;
        }
    }
    std::string stage_error;
    if (!validate_audit_stage_prefix(DestructiveCommand::AccountRemove, prefix, stage_error) ||
        !stages_match_plan(*plan, prefix)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    RemovalTombstone result{value["invocation_id"].get<std::string>(),
                            value["account"].get<std::string>(),
                            *stage,
                            std::move(stages),
                            std::nullopt,
                            std::move(*plan)};
    result.next_stage = expected_next(result);
    const json expected_next_json =
        result.next_stage ? json(audit_stage_name(*result.next_stage)) : json(nullptr);
    if (value["next_stage"] != expected_next_json || serialize(result) != value) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    failure.reason.clear();
    return result;
}

bool rename_exclusive(int directory, const std::string& from, const std::string& to) {
#if defined(__linux__)
    return ::syscall(SYS_renameat2, directory, from.c_str(), directory, to.c_str(),
                     RENAME_NOREPLACE) == 0;
#elif defined(__APPLE__)
    return ::renameatx_np(directory, from.c_str(), directory, to.c_str(), RENAME_EXCL) == 0;
#else
    errno = ENOTSUP;
    return false;
#endif
}

bool write_tombstone(int directory, const RemovalTombstone& tombstone, uid_t expected_uid,
                     bool create, const std::shared_ptr<const testing::RemovalJournalHooks>& hooks,
                     RemovalJournalFailure& failure) {
    const std::string final_name = tombstone.invocation_id + ".json";
    const std::string temporary_name = "." + final_name + ".tmp." + std::to_string(::getpid()) +
                                       "." + std::to_string(temporary_sequence().fetch_add(1));
    const Descriptor temporary(::openat(directory, temporary_name.c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                        0600));
    if (!temporary || !valid_file(temporary.get(), expected_uid)) {
        failure.reason = "open_failed";
        return false;
    }
    const std::string bytes = serialize(tombstone).dump() + '\n';
    if (injected(hooks, RemovalJournalFault::TombstoneWrite) ||
        !write_all(temporary.get(), bytes)) {
        failure.reason = "write_failed";
        return false;
    }
    if (injected(hooks, RemovalJournalFault::TombstoneSync) || ::fsync(temporary.get()) != 0) {
        failure.reason = "sync_failed";
        return false;
    }
    const bool renamed =
        create ? rename_exclusive(directory, temporary_name, final_name)
               : ::renameat(directory, temporary_name.c_str(), directory, final_name.c_str()) == 0;
    if (!renamed) {
        failure.reason = errno == EEXIST ? "path_invalid" : "write_failed";
        return false;
    }
    if (injected(hooks, RemovalJournalFault::TombstoneSync) || ::fsync(directory) != 0) {
        failure.reason = "sync_failed";
        return false;
    }
    if (hooks && hooks->after_tombstone_sync) {
        hooks->after_tombstone_sync(tombstone.invocation_id, tombstone.stage);
    }
    failure.reason.clear();
    return true;
}

std::optional<RemovalTombstone> load_from_directory(int directory, std::string_view invocation_id,
                                                    uid_t expected_uid,
                                                    RemovalJournalFailure& failure) {
    if (!valid_invocation_id(invocation_id)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    auto bytes = read_file(directory, std::string(invocation_id) + ".json", expected_uid,
                           kTombstoneLimit, false, failure);
    if (!bytes) {
        return std::nullopt;
    }
    const auto parsed = json::parse(*bytes, nullptr, false);
    if (parsed.is_discarded()) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    auto tombstone = parse_tombstone(parsed, failure);
    if (tombstone && tombstone->invocation_id != invocation_id) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    return tombstone;
}

std::optional<AuthoritySource> parse_authority(const json& value) {
    if (value == "request") {
        return AuthoritySource::Request;
    }
    if (value == "config") {
        return AuthoritySource::Config;
    }
    return std::nullopt;
}

std::optional<ConfirmationSource> parse_confirmation(const json& value) {
    if (value == "yes") {
        return ConfirmationSource::Yes;
    }
    if (value == "tty") {
        return ConfirmationSource::Tty;
    }
    return std::nullopt;
}

std::optional<std::vector<AuditStage>> parse_stages(const json& value) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<AuditStage> stages;
    for (const auto& entry : value) {
        if (!entry.is_string()) {
            return std::nullopt;
        }
        auto stage = parse_audit_stage(entry.get_ref<const std::string&>());
        if (!stage) {
            return std::nullopt;
        }
        stages.push_back(*stage);
    }
    return stages;
}

std::optional<std::vector<std::string>> tombstone_invocations(int directory,
                                                              RemovalJournalFailure& failure) {
    const int duplicate = ::dup(directory);
    if (duplicate < 0) {
        failure.reason = "open_failed";
        return std::nullopt;
    }
    DIR* stream = ::fdopendir(duplicate);
    if (stream == nullptr) {
        ::close(duplicate);
        failure.reason = "open_failed";
        return std::nullopt;
    }
    std::vector<std::string> invocations;
    errno = 0;
    while (const auto* entry = ::readdir(stream)) {
        const std::string name(entry->d_name);
        if (name.ends_with(".json")) {
            invocations.push_back(name.substr(0, name.size() - 5));
        }
    }
    const int read_error = errno;
    ::closedir(stream);
    if (read_error != 0) {
        failure.reason = "open_failed";
        return std::nullopt;
    }
    std::ranges::sort(invocations);
    failure.reason.clear();
    return invocations;
}

struct AuditScan {
    std::map<std::string, proto::AccountRemovePlan, std::less<>> pending;
    std::set<std::string, std::less<>> completed;
    std::map<std::string, proto::AccountRemovePlan, std::less<>> plans;
    std::map<std::string, std::vector<AuditStage>, std::less<>> completed_stages;
    std::map<std::string, json, std::less<>> outcomes;
    std::map<std::string, std::string, std::less<>> generations;
    std::set<std::string, std::less<>> oldest_invocations;
};

AuditRecordIdentity audit_identity(const json& record, const std::string& invocation) {
    std::string timestamp;
    if (record.contains("timestamp") && record["timestamp"].is_string()) {
        timestamp = record["timestamp"].get<std::string>();
    }
    return {invocation, std::move(timestamp)};
}

std::optional<AccountRemoveRemoteResult> parse_remote_result(const json& result) {
    if (!result.is_object() || !result.contains("remote_logout") ||
        !result["remote_logout"].is_string()) {
        return std::nullopt;
    }
    if (result["remote_logout"] == "confirmed") {
        return AccountRemoveRemoteResult::Confirmed;
    }
    if (result["remote_logout"] == "not_present") {
        return AccountRemoveRemoteResult::NotPresent;
    }
    if (result["remote_logout"] == "kept") {
        return AccountRemoveRemoteResult::Kept;
    }
    return std::nullopt;
}

bool consume_intent(const json& record, const AuditRecordIdentity& identity,
                    const std::string& invocation, std::string_view generation, AuditScan& scan) {
    if (!exact_fields(record, {"schema_version", "phase", "invocation_id", "timestamp", "account",
                               "command", "arguments", "plan", "config_snapshot",
                               "authority_source", "confirmation_source"}) ||
        scan.pending.contains(invocation) || scan.completed.contains(invocation)) {
        return false;
    }
    std::string error;
    auto plan = proto::parse_account_remove_plan(record["plan"], error);
    const auto authority = parse_authority(record["authority_source"]);
    const auto confirmation = parse_confirmation(record["confirmation_source"]);
    if (!plan || !authority || !confirmation) {
        return false;
    }
    auto canonical =
        make_account_remove_audit_intent(identity, *plan, *authority, *confirmation, error);
    if (!canonical || serialize(*canonical) != record) {
        return false;
    }
    scan.plans.emplace(invocation, *plan);
    scan.generations.emplace(invocation, generation);
    scan.pending.emplace(invocation, std::move(*plan));
    return true;
}

bool consume_outcome(const json& record, const AuditRecordIdentity& identity,
                     const std::string& invocation, std::string_view generation, AuditScan& scan) {
    std::string error;
    const auto found = scan.pending.find(invocation);
    const auto found_generation = scan.generations.find(invocation);
    if (found == scan.pending.end() || found_generation == scan.generations.end() ||
        found_generation->second != generation) {
        return false;
    }
    const auto stages = parse_stages(record["completed_stages"]);
    if (!stages) {
        return false;
    }
    std::optional<AuditOutcome> canonical;
    if (record["success"] == true && record["error"].is_null()) {
        const auto remote = parse_remote_result(record["result"]);
        if (remote) {
            canonical = make_account_remove_success_audit_outcome(identity, found->second, *remote,
                                                                  *stages, error);
        }
    } else if (record["success"] == false && record["result"].is_null()) {
        auto structured = parse_structured_outcome_error(record["error"], error);
        if (structured) {
            canonical = make_failure_audit_outcome(identity, proto::DestructivePlan{found->second},
                                                   *stages, *structured, error);
        }
    }
    if (!canonical || serialize(*canonical) != record) {
        return false;
    }
    scan.pending.erase(found);
    scan.completed.insert(invocation);
    scan.completed_stages.emplace(invocation, *stages);
    scan.outcomes.emplace(invocation, record);
    return true;
}

bool consume_audit_record(const json& record, std::string_view generation, AuditScan& scan) {
    if (!record.is_object() || !record.contains("phase") || !record["phase"].is_string() ||
        !record.contains("invocation_id") || !record["invocation_id"].is_string() ||
        !valid_invocation_id(record["invocation_id"].get_ref<const std::string&>()) ||
        !record.contains("command") || record["command"] != "account_remove") {
        return false;
    }
    const auto invocation = record["invocation_id"].get<std::string>();
    const auto identity = audit_identity(record, invocation);
    if (record["phase"] == "intent") {
        return consume_intent(record, identity, invocation, generation, scan);
    }
    if (record["phase"] != "outcome" ||
        !exact_fields(record, {"schema_version", "phase", "invocation_id", "timestamp", "account",
                               "command", "success", "mutation_state", "completed_stages", "result",
                               "error"})) {
        return false;
    }
    return consume_outcome(record, identity, invocation, generation, scan);
}

bool consume_audit_bytes(std::string_view bytes, std::string_view generation, AuditScan& scan,
                         bool oldest) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto newline = bytes.find('\n', offset);
        if (newline == std::string_view::npos || newline == offset) {
            return false;
        }
        const auto parsed = json::parse(bytes.substr(offset, newline - offset), nullptr, false);
        if (parsed.is_discarded() || !consume_audit_record(parsed, generation, scan)) {
            return false;
        }
        if (oldest) {
            scan.oldest_invocations.insert(parsed["invocation_id"].get<std::string>());
        }
        offset = newline + 1;
    }
    return true;
}

std::optional<AuditScan> scan_audit(int directory, uid_t expected_uid,
                                    RemovalJournalFailure& failure) {
    AuditScan scan;
    constexpr std::array<const char*, 5> names{"audit.log.4", "audit.log.3", "audit.log.2",
                                               "audit.log.1", "audit.log"};
    for (const auto* name : names) {
        auto bytes = read_file(directory, name, expected_uid, kAuditLimit, true, failure);
        if (!bytes) {
            return std::nullopt;
        }
        if (!bytes->empty() &&
            !consume_audit_bytes(*bytes, name, scan, std::string_view{name} == "audit.log.4")) {
            failure.reason = "path_invalid";
            return std::nullopt;
        }
    }
    failure.reason.clear();
    return scan;
}

bool verified_terminal_tombstone(const RemovalTombstone& tombstone, const AuditScan& scan) {
    if (tombstone.stage != AuditStage::OutcomeSynced || tombstone.completed_stages.size() < 3 ||
        tombstone.completed_stages.back() != AuditStage::OutcomeSynced ||
        !scan.completed.contains(tombstone.invocation_id)) {
        return false;
    }
    const auto found_plan = scan.plans.find(tombstone.invocation_id);
    const auto found_stages = scan.completed_stages.find(tombstone.invocation_id);
    if (found_plan == scan.plans.end() || found_stages == scan.completed_stages.end() ||
        proto::serialize(found_plan->second) != proto::serialize(tombstone.plan)) {
        return false;
    }
    auto tombstone_prefix = tombstone.completed_stages;
    tombstone_prefix.pop_back();
    return found_stages->second == tombstone_prefix;
}

struct IncomingAuditIntent {
    std::string invocation_id;
    proto::AccountRemovePlan plan;
};

std::optional<IncomingAuditIntent> parse_incoming_intent(const json& record) {
    AuditScan scan;
    if (!consume_audit_record(record, "incoming", scan) || scan.pending.size() != 1) {
        return std::nullopt;
    }
    const auto invocation = record["invocation_id"].get<std::string>();
    const auto found = scan.pending.find(invocation);
    if (found == scan.pending.end()) {
        return std::nullopt;
    }
    return IncomingAuditIntent{invocation, found->second};
}

std::optional<IncomingAuditIntent> validate_incoming_tombstone(int directory, uid_t expected_uid,
                                                               const AuditScan& scan,
                                                               const json& incoming_record,
                                                               RemovalJournalFailure& failure) {
    auto incoming = parse_incoming_intent(incoming_record);
    if (!incoming || scan.plans.contains(incoming->invocation_id)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    auto tombstone = load_from_directory(directory, incoming->invocation_id, expected_uid, failure);
    if (!tombstone || tombstone->stage != AuditStage::Planned ||
        tombstone->completed_stages != std::vector{AuditStage::Planned} ||
        tombstone->next_stage != AuditStage::IntentSynced ||
        proto::serialize(tombstone->plan) != proto::serialize(incoming->plan)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    return incoming;
}

bool retire_terminal_tombstones(int directory, const std::vector<std::string>& invocations,
                                RemovalJournalFailure& failure) {
    for (const auto& invocation : invocations) {
        const std::string name = invocation + ".json";
        if (::unlinkat(directory, name.c_str(), 0) != 0) {
            failure.reason = "rotate_failed";
            return false;
        }
    }
    if (!invocations.empty() && ::fsync(directory) != 0) {
        failure.reason = "rotate_failed";
        return false;
    }
    return true;
}

std::optional<bool> prepare_audit_rotation(int directory, uid_t expected_uid, const AuditScan& scan,
                                           const IncomingAuditIntent& incoming,
                                           RemovalJournalFailure& failure) {
    auto invocations = tombstone_invocations(directory, failure);
    if (!invocations) {
        return std::nullopt;
    }
    bool protected_audit_group = !scan.pending.empty();
    bool incoming_tombstone = false;
    std::vector<std::string> retired;
    for (const auto& invocation : *invocations) {
        auto tombstone = load_from_directory(directory, invocation, expected_uid, failure);
        if (!tombstone) {
            return std::nullopt;
        }
        if (invocation == incoming.invocation_id) {
            incoming_tombstone = true;
            continue;
        }
        if (tombstone->stage == AuditStage::OutcomeSynced) {
            if (!verified_terminal_tombstone(*tombstone, scan)) {
                return true;
            }
            if (scan.oldest_invocations.contains(invocation)) {
                retired.push_back(invocation);
            }
            continue;
        }
        protected_audit_group = true;
    }
    if (!incoming_tombstone) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    if (protected_audit_group) {
        return true;
    }
    if (!retire_terminal_tombstones(directory, retired, failure)) {
        return std::nullopt;
    }
    return false;
}

bool rotate_audit_if_needed(int directory, uid_t expected_uid, std::size_t incoming_size,
                            const json& incoming_record,
                            const std::shared_ptr<const testing::RemovalJournalHooks>& hooks,
                            RemovalJournalFailure& failure) {
    auto scanned = scan_audit(directory, expected_uid, failure);
    if (!scanned) {
        return false;
    }
    auto incoming =
        validate_incoming_tombstone(directory, expected_uid, *scanned, incoming_record, failure);
    if (!incoming) {
        return false;
    }
    struct stat current {};
    if (::fstatat(directory, "audit.log", &current, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        failure.reason = "open_failed";
        return false;
    }
    const auto threshold = hooks ? hooks->rotation_bytes : std::size_t{32} * 1024 * 1024;
    if (current.st_size < 0 ||
        static_cast<std::uint64_t>(current.st_size) + incoming_size <= threshold) {
        return true;
    }
    const auto protected_audit =
        prepare_audit_rotation(directory, expected_uid, *scanned, *incoming, failure);
    if (!protected_audit) {
        return false;
    }
    if (*protected_audit) {
        return true;
    }
    if (injected(hooks, RemovalJournalFault::AuditRotate)) {
        failure.reason = "rotate_failed";
        return false;
    }
    constexpr std::array<const char*, 5> names{"audit.log", "audit.log.1", "audit.log.2",
                                               "audit.log.3", "audit.log.4"};
    for (std::size_t index = names.size() - 1; index > 0; --index) {
        struct stat metadata {};
        if (::fstatat(directory, names.at(index - 1), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            failure.reason = "rotate_failed";
            return false;
        }
        if (!S_ISREG(metadata.st_mode) || metadata.st_uid != expected_uid ||
            (metadata.st_mode & 07777) != 0600 || metadata.st_nlink != 1 ||
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

bool append_audit_record(int directory, const json& record, uid_t expected_uid, bool begin_group,
                         const std::shared_ptr<const testing::RemovalJournalHooks>& hooks,
                         RemovalJournalFailure& failure) {
    const std::string line = record.dump() + '\n';
    auto journal_lock = acquire_journal_lock(directory, expected_uid, failure);
    if (!journal_lock) {
        return false;
    }
    if (begin_group &&
        !rotate_audit_if_needed(directory, expected_uid, line.size(), record, hooks, failure)) {
        return false;
    }
    const Descriptor audit(::openat(directory, "audit.log",
                                    O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!audit) {
        failure.reason = "open_failed";
        return false;
    }
    if (::flock(audit.get(), LOCK_EX) != 0 || !valid_file(audit.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return false;
    }
    if (injected(hooks, RemovalJournalFault::AuditWrite) || !write_all(audit.get(), line)) {
        failure.reason = "write_failed";
        return false;
    }
    if (injected(hooks, RemovalJournalFault::AuditSync) || ::fsync(audit.get()) != 0 ||
        ::fsync(directory) != 0) {
        failure.reason = "sync_failed";
        return false;
    }
    if (hooks && hooks->after_audit_sync) {
        hooks->after_audit_sync(record.value("phase", std::string{}));
    }
    failure.reason.clear();
    return true;
}

} // namespace

json serialize(const RemovalTombstone& tombstone) {
    json stages = json::array();
    for (const auto stage : tombstone.completed_stages) {
        stages.push_back(audit_stage_name(stage));
    }
    const auto plan = proto::serialize(tombstone.plan);
    return {{"schema_version", 1},
            {"invocation_id", tombstone.invocation_id},
            {"account", tombstone.account},
            {"stage", audit_stage_name(tombstone.stage)},
            {"completed_stages", std::move(stages)},
            {"next_stage",
             tombstone.next_stage ? json(audit_stage_name(*tombstone.next_stage)) : json(nullptr)},
            {"plan", plan},
            {"config_snapshot", tombstone.plan.config_snapshot()},
            {"data_root", plan["data_root"]},
            {"state_root", plan["state_root"]}};
}

RemovalJournal::RemovalJournal(std::string directory, uid_t expected_uid,
                               std::shared_ptr<const testing::RemovalJournalHooks> hooks)
    : directory_(std::move(directory)), expected_uid_(expected_uid), hooks_(std::move(hooks)) {}

const std::string& RemovalJournal::directory() const {
    return directory_;
}

std::string RemovalJournal::audit_path() const {
    return directory_ + "/audit.log";
}

std::string RemovalJournal::tombstone_path(std::string_view invocation_id) const {
    return directory_ + "/" + std::string(invocation_id) + ".json";
}

bool RemovalJournal::create(const std::string& invocation_id, const proto::AccountRemovePlan& plan,
                            RemovalJournalFailure& failure) const {
    if (!valid_invocation_id(invocation_id)) {
        failure.reason = "path_invalid";
        return false;
    }
    auto opened = open_journal_directory(directory_, expected_uid_, true, hooks_, failure);
    if (!opened.descriptor) {
        return false;
    }
    auto journal_lock = acquire_journal_lock(opened.descriptor.get(), expected_uid_, failure);
    if (!journal_lock) {
        return false;
    }
    const RemovalTombstone tombstone{
        invocation_id,         plan.account(),           AuditStage::Planned,
        {AuditStage::Planned}, AuditStage::IntentSynced, plan};
    return write_tombstone(opened.descriptor.get(), tombstone, expected_uid_, true, hooks_,
                           failure);
}

bool RemovalJournal::advance(const std::string& invocation_id, AuditStage stage,
                             RemovalJournalFailure& failure) const {
    auto opened = open_journal_directory(directory_, expected_uid_, false, hooks_, failure);
    if (!opened.descriptor) {
        return false;
    }
    auto journal_lock = acquire_journal_lock(opened.descriptor.get(), expected_uid_, failure);
    if (!journal_lock) {
        return false;
    }
    auto tombstone =
        load_from_directory(opened.descriptor.get(), invocation_id, expected_uid_, failure);
    if (!tombstone) {
        return false;
    }
    if (tombstone->stage == stage) {
        if (stage == AuditStage::OutcomeSynced) {
            auto scan = scan_audit(opened.descriptor.get(), expected_uid_, failure);
            if (!scan) {
                return false;
            }
            if (!verified_terminal_tombstone(*tombstone, *scan)) {
                failure.reason = "path_invalid";
                return false;
            }
        }
        failure.reason.clear();
        return true;
    }
    if (tombstone->stage == AuditStage::OutcomeSynced) {
        failure.reason = "path_invalid";
        return false;
    }
    auto candidate = tombstone->completed_stages;
    candidate.push_back(stage);
    auto prefix = candidate;
    if (stage == AuditStage::OutcomeSynced) {
        prefix.pop_back();
    }
    std::string error;
    if (!validate_audit_stage_prefix(DestructiveCommand::AccountRemove, prefix, error) ||
        !stages_match_plan(tombstone->plan, prefix)) {
        failure.reason = "path_invalid";
        return false;
    }
    tombstone->stage = stage;
    tombstone->completed_stages = std::move(candidate);
    tombstone->next_stage = expected_next(*tombstone);
    if (stage == AuditStage::OutcomeSynced) {
        auto scan = scan_audit(opened.descriptor.get(), expected_uid_, failure);
        if (!scan) {
            return false;
        }
        if (!verified_terminal_tombstone(*tombstone, *scan)) {
            failure.reason = "path_invalid";
            return false;
        }
    }
    return write_tombstone(opened.descriptor.get(), *tombstone, expected_uid_, false, hooks_,
                           failure);
}

std::optional<RemovalTombstone> RemovalJournal::load(std::string_view invocation_id,
                                                     RemovalJournalFailure& failure) const {
    auto opened = open_journal_directory(directory_, expected_uid_, false, hooks_, failure);
    if (!opened.descriptor) {
        return std::nullopt;
    }
    return load_from_directory(opened.descriptor.get(), invocation_id, expected_uid_, failure);
}

RemovalInspection RemovalJournal::inspect_account(std::string_view account) const {
    RemovalJournalFailure failure;
    auto opened = open_journal_directory(directory_, expected_uid_, false, hooks_, failure);
    if (opened.missing) {
        return {};
    }
    if (!opened.descriptor) {
        return {RemovalInspectionStatus::Invalid, {}, directory_, std::move(failure)};
    }
    auto journal_lock = acquire_journal_lock(opened.descriptor.get(), expected_uid_, failure);
    if (!journal_lock) {
        return {RemovalInspectionStatus::Invalid, {}, directory_, std::move(failure)};
    }
    auto tombstones = tombstone_invocations(opened.descriptor.get(), failure);
    if (!tombstones) {
        return {RemovalInspectionStatus::Invalid, {}, directory_, std::move(failure)};
    }
    std::unique_ptr<RemovalTombstone> matched;
    std::optional<AuditScan> scan;
    for (const auto& invocation : *tombstones) {
        auto tombstone =
            load_from_directory(opened.descriptor.get(), invocation, expected_uid_, failure);
        if (!tombstone) {
            return {RemovalInspectionStatus::Invalid,
                    {},
                    tombstone_path(invocation),
                    std::move(failure)};
        }
        if (tombstone->stage == AuditStage::OutcomeSynced) {
            if (!scan) {
                scan = scan_audit(opened.descriptor.get(), expected_uid_, failure);
                if (!scan) {
                    return {RemovalInspectionStatus::Invalid, {}, audit_path(), std::move(failure)};
                }
            }
            if (!verified_terminal_tombstone(*tombstone, *scan)) {
                return {RemovalInspectionStatus::Invalid,
                        {},
                        tombstone_path(invocation),
                        {"path_invalid"}};
            }
        } else if (tombstone->account == account) {
            if (matched) {
                return {RemovalInspectionStatus::Invalid, {}, directory_, {"path_invalid"}};
            }
            matched = std::make_unique<RemovalTombstone>(std::move(*tombstone));
        }
    }
    if (matched) {
        auto tombstone = std::move(*matched);
        const auto path = tombstone_path(tombstone.invocation_id);
        return {RemovalInspectionStatus::Incomplete, std::move(tombstone), path, {}};
    }
    return {};
}

bool RemovalJournal::append_intent(const AuditIntent& intent,
                                   RemovalJournalFailure& failure) const {
    const auto record = serialize(intent);
    if (!record.is_object() || record.value("command", std::string{}) != "account_remove" ||
        record.value("phase", std::string{}) != "intent") {
        failure.reason = "path_invalid";
        return false;
    }
    auto opened = open_journal_directory(directory_, expected_uid_, true, hooks_, failure);
    return opened.descriptor && append_audit_record(opened.descriptor.get(), record, expected_uid_,
                                                    true, hooks_, failure);
}

bool RemovalJournal::append_outcome(const AuditOutcome& outcome,
                                    RemovalJournalFailure& failure) const {
    const auto record = serialize(outcome);
    if (!record.is_object() || record.value("command", std::string{}) != "account_remove" ||
        record.value("phase", std::string{}) != "outcome") {
        failure.reason = "path_invalid";
        return false;
    }
    auto opened = open_journal_directory(directory_, expected_uid_, false, hooks_, failure);
    return opened.descriptor && append_audit_record(opened.descriptor.get(), record, expected_uid_,
                                                    false, hooks_, failure);
}

std::optional<RemovalAuditPresence>
RemovalJournal::audit_presence(std::string_view invocation_id,
                               RemovalJournalFailure& failure) const {
    if (!valid_invocation_id(invocation_id)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    auto opened = open_journal_directory(directory_, expected_uid_, false, hooks_, failure);
    if (!opened.descriptor) {
        return std::nullopt;
    }
    auto journal_lock = acquire_journal_lock(opened.descriptor.get(), expected_uid_, failure);
    if (!journal_lock) {
        return std::nullopt;
    }
    auto scan = scan_audit(opened.descriptor.get(), expected_uid_, failure);
    if (!scan) {
        return std::nullopt;
    }
    return RemovalAuditPresence{scan->pending.contains(std::string(invocation_id)) ||
                                    scan->completed.contains(std::string(invocation_id)),
                                scan->completed.contains(std::string(invocation_id))};
}

std::optional<json> RemovalJournal::audit_outcome(std::string_view invocation_id,
                                                  RemovalJournalFailure& failure) const {
    if (!valid_invocation_id(invocation_id)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    auto opened = open_journal_directory(directory_, expected_uid_, false, hooks_, failure);
    if (!opened.descriptor) {
        return std::nullopt;
    }
    auto journal_lock = acquire_journal_lock(opened.descriptor.get(), expected_uid_, failure);
    if (!journal_lock) {
        return std::nullopt;
    }
    auto scan = scan_audit(opened.descriptor.get(), expected_uid_, failure);
    if (!scan) {
        return std::nullopt;
    }
    const auto found = scan->outcomes.find(std::string(invocation_id));
    if (found == scan->outcomes.end()) {
        failure.reason = "open_failed";
        return std::nullopt;
    }
    failure.reason.clear();
    return found->second;
}

} // namespace tgcli::daemon
