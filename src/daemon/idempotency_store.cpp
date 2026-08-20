#include "daemon/idempotency_store.hpp"

#include "common/canonical_json.hpp"
#include "common/paths.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::string_view kFinalName = "idempotency.db";
constexpr std::string_view kTempName = ".idempotency.db.tmp";
constexpr std::size_t kIoChunk = 65'536;

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
    int value_ = -1;
};

bool exact_fields(const json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object() || value.size() != names.size()) {
        return false;
    }
    return std::ranges::all_of(
        names, [&](std::string_view name) { return value.contains(std::string(name)); });
}

bool valid_digest(std::string_view value) {
    if (!value.starts_with("sha256:") || value.size() != 71) {
        return false;
    }
    return std::ranges::all_of(value.substr(7), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool valid_invocation_id(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool valid_frozen_state_path(std::string_view state_directory, std::string_view account) {
    if (!paths::valid_account_name(std::string(account)) || state_directory.empty() ||
        state_directory.find('\0') != std::string_view::npos) {
        return false;
    }
    try {
        const std::filesystem::path state(state_directory);
        return state.is_absolute() && state.lexically_normal().string() == state_directory &&
               state.filename().string() == account;
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

bool valid_public_store_path(std::string_view store_path, std::string_view account) {
    try {
        const std::filesystem::path value(store_path);
        return value.is_absolute() && value.lexically_normal().string() == store_path &&
               value.filename() == kFinalName && value.parent_path().filename() == account;
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

IdempotencyFailure
make_failure(std::string_view account, std::string_view path, AccountAuditDurabilityReason reason,
             std::string detail = {},
             std::optional<AccountAuditFailure::Interruption> interruption = {}) {
    return {reason, std::string(account), std::string(path), std::move(detail), interruption};
}

json file_snapshot_json(const FileSnapshot& file) {
    return {{"path", file.path},         {"name", file.name},        {"size", file.size},
            {"sha256", file.sha256},     {"device", file.device},    {"inode", file.inode},
            {"mtime_ns", file.mtime_ns}, {"ctime_ns", file.ctime_ns}};
}

json spool_json(const SpoolRef& spool) {
    return {{"relative_path", spool.relative_path}, {"file", file_snapshot_json(spool.file)}};
}

std::optional<SpoolRef> parse_spool(const json& value, std::string_view invocation_id) {
    if (!exact_fields(value, {"relative_path", "file"}) || !value["relative_path"].is_string() ||
        !exact_fields(value["file"], {"path", "name", "size", "sha256", "device", "inode",
                                      "mtime_ns", "ctime_ns"})) {
        return std::nullopt;
    }
    const auto& file = value["file"];
    if (!file["path"].is_string() || !file["name"].is_string() ||
        !file["size"].is_number_unsigned() || !file["sha256"].is_string() ||
        !file["device"].is_number_unsigned() || !file["inode"].is_number_unsigned() ||
        !file["mtime_ns"].is_number_integer() || !file["ctime_ns"].is_number_integer()) {
        return std::nullopt;
    }
    SpoolRef result{.relative_path = value["relative_path"].get<std::string>(),
                    .file = {.path = file["path"].get<std::string>(),
                             .name = file["name"].get<std::string>(),
                             .size = file["size"].get<std::uint64_t>(),
                             .sha256 = file["sha256"].get<std::string>(),
                             .device = file["device"].get<std::uint64_t>(),
                             .inode = file["inode"].get<std::uint64_t>(),
                             .mtime_ns = file["mtime_ns"].get<std::int64_t>(),
                             .ctime_ns = file["ctime_ns"].get<std::int64_t>()}};
    if (!validate_account_audit_persisted_spool(result, invocation_id)) {
        return std::nullopt;
    }
    return result;
}

json entry_json(const IdempotencyEntry& entry) {
    json spool = nullptr;
    if (entry.spool) {
        spool = spool_json(*entry.spool);
    }
    return {{"key_hash", entry.key_hash.value()},
            {"request_fingerprint", entry.request_fingerprint.value()},
            {"operation", account_audit_operation_name(entry.operation)},
            {"state", entry.state == IdempotencyEntryState::Pending ? "pending" : "completed"},
            {"invocation_id", entry.invocation_id},
            {"audit_generation", entry.audit_generation},
            {"created_at", entry.created_at},
            {"expires_at", entry.expires_at},
            {"reserved_terminal_bytes", entry.reserved_terminal_bytes},
            {"plan", entry.plan},
            {"temporary_message_ids", entry.temporary_message_ids},
            {"forward_progress", entry.forward_progress},
            {"spool", std::move(spool)},
            {"terminal", entry.terminal ? *entry.terminal : json(nullptr)}};
}

std::optional<std::uint64_t> mutable_charge(const IdempotencyEntry& entry) {
    const json baseline{{"temporary_message_ids", json::array()},
                        {"forward_progress", json::array()},
                        {"terminal", nullptr}};
    const json current{{"temporary_message_ids", entry.temporary_message_ids},
                       {"forward_progress", entry.forward_progress},
                       {"terminal", entry.terminal ? *entry.terminal : json(nullptr)}};
    const auto baseline_bytes = common::canonical_json(baseline);
    const auto current_bytes = common::canonical_json(current);
    if (!std::holds_alternative<std::string>(baseline_bytes) ||
        !std::holds_alternative<std::string>(current_bytes)) {
        return std::nullopt;
    }
    const auto baseline_size = std::get<std::string>(baseline_bytes).size();
    const auto current_size = std::get<std::string>(current_bytes).size();
    if (current_size < baseline_size) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(current_size - baseline_size);
}

bool valid_entry(const IdempotencyEntry& entry, std::string_view account) {
    if (!valid_digest(entry.key_hash.value()) || !valid_digest(entry.request_fingerprint.value()) ||
        entry.operation == AccountAuditOperation::SessionTerminate ||
        !valid_invocation_id(entry.invocation_id) || entry.audit_generation == 0 ||
        entry.created_at > kIdempotencyMaximumUnixSeconds ||
        entry.expires_at > kIdempotencyMaximumUnixSeconds ||
        entry.created_at > kIdempotencyMaximumUnixSeconds - kIdempotencyRetentionSeconds ||
        entry.expires_at != entry.created_at + kIdempotencyRetentionSeconds ||
        !validate_account_audit_persisted_plan(entry.operation, entry.plan, account) ||
        !validate_account_audit_persisted_temporary_ids(entry.operation,
                                                        entry.temporary_message_ids, entry.plan) ||
        !validate_account_audit_persisted_forward_progress(entry.operation, entry.forward_progress,
                                                           entry.plan) ||
        (entry.spool &&
         !validate_account_audit_persisted_spool(*entry.spool, entry.invocation_id))) {
        return false;
    }
    const auto charge = mutable_charge(entry);
    if (!charge) {
        return false;
    }
    if (entry.state == IdempotencyEntryState::Pending) {
        return !entry.terminal &&
               entry.reserved_terminal_bytes ==
                   account_audit_terminal_reservation(entry.operation) &&
               *charge <= entry.reserved_terminal_bytes;
    }
    return entry.reserved_terminal_bytes == 0 && entry.temporary_message_ids.empty() &&
           entry.forward_progress.empty() && entry.terminal &&
           validate_account_audit_persisted_terminal(entry.operation, *entry.terminal, entry.plan,
                                                     account) &&
           *charge <= account_audit_terminal_reservation(entry.operation);
}

std::variant<IdempotencySnapshot, AccountAuditDurabilityReason>
canonicalize_snapshot(std::vector<IdempotencyEntry> entries, std::string_view account) {
    if (entries.size() > kIdempotencyStoreMaximumEntries) {
        return AccountAuditDurabilityReason::SchemaError;
    }
    std::string prior;
    bool first = true;
    std::uint64_t remaining = 0;
    std::set<std::string, std::less<>> invocations;
    std::set<std::tuple<std::uint64_t, std::string, std::string, AccountAuditOperation>> pins;
    json serialized_entries = json::array();
    for (const auto& entry : entries) {
        if (!valid_entry(entry, account) || (!first && prior >= entry.key_hash.value()) ||
            !invocations.emplace(entry.invocation_id).second ||
            !pins.emplace(entry.audit_generation, entry.invocation_id,
                          entry.request_fingerprint.value(), entry.operation)
                 .second) {
            return AccountAuditDurabilityReason::SchemaError;
        }
        first = false;
        prior = entry.key_hash.value();
        const auto charge = mutable_charge(entry);
        if (!charge) {
            return AccountAuditDurabilityReason::SchemaError;
        }
        const auto entry_remaining = entry.state == IdempotencyEntryState::Pending
                                         ? entry.reserved_terminal_bytes - *charge
                                         : std::uint64_t{0};
        if (remaining > kIdempotencyStoreMaximumBytes - entry_remaining) {
            return AccountAuditDurabilityReason::SchemaError;
        }
        remaining += entry_remaining;
        serialized_entries.push_back(entry_json(entry));
    }
    const json document{{"schema_version", 1}, {"entries", std::move(serialized_entries)}};
    const auto canonical = common::canonical_json(document);
    if (!std::holds_alternative<std::string>(canonical)) {
        return AccountAuditDurabilityReason::SchemaError;
    }
    auto bytes = std::get<std::string>(canonical);
    if (bytes.size() > kIdempotencyStoreMaximumBytes ||
        remaining > kIdempotencyStoreMaximumBytes - bytes.size()) {
        return AccountAuditDurabilityReason::SchemaError;
    }
    return IdempotencySnapshot{std::move(entries), std::move(bytes)};
}

std::optional<IdempotencyEntry> parse_entry(const json& value, std::string_view account) {
    if (!exact_fields(value,
                      {"key_hash", "request_fingerprint", "operation", "state", "invocation_id",
                       "audit_generation", "created_at", "expires_at", "reserved_terminal_bytes",
                       "plan", "temporary_message_ids", "forward_progress", "spool", "terminal"}) ||
        !value["key_hash"].is_string() || !value["request_fingerprint"].is_string() ||
        !value["operation"].is_string() || !value["state"].is_string() ||
        !value["invocation_id"].is_string() || !value["audit_generation"].is_number_unsigned() ||
        !value["created_at"].is_number_unsigned() || !value["expires_at"].is_number_unsigned() ||
        !value["reserved_terminal_bytes"].is_number_unsigned() ||
        value["reserved_terminal_bytes"].get<std::uint64_t>() >
            std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    auto key = parse_idempotency_key_hash(value["key_hash"].get<std::string>());
    auto fingerprint =
        parse_idempotency_request_fingerprint(value["request_fingerprint"].get<std::string>());
    const auto operation =
        parse_account_audit_operation(value["operation"].get_ref<const std::string&>());
    if (!key || !fingerprint || !operation ||
        *operation == AccountAuditOperation::SessionTerminate) {
        return std::nullopt;
    }
    IdempotencyEntryState state{};
    if (value["state"] == "pending") {
        state = IdempotencyEntryState::Pending;
    } else if (value["state"] == "completed") {
        state = IdempotencyEntryState::Completed;
    } else {
        return std::nullopt;
    }
    std::optional<SpoolRef> spool;
    if (!value["spool"].is_null()) {
        spool = parse_spool(value["spool"], value["invocation_id"].get_ref<const std::string&>());
        if (!spool) {
            return std::nullopt;
        }
    }
    std::optional<json> terminal;
    if (!value["terminal"].is_null()) {
        terminal = value["terminal"];
    }
    IdempotencyEntry entry{std::move(*key),
                           std::move(*fingerprint),
                           *operation,
                           state,
                           value["invocation_id"].get<std::string>(),
                           value["audit_generation"].get<std::uint64_t>(),
                           value["created_at"].get<std::uint64_t>(),
                           value["expires_at"].get<std::uint64_t>(),
                           value["reserved_terminal_bytes"].get<std::uint32_t>(),
                           value["plan"],
                           value["temporary_message_ids"],
                           value["forward_progress"],
                           std::move(spool),
                           std::move(terminal)};
    if (!valid_entry(entry, account)) {
        return std::nullopt;
    }
    return entry;
}

struct ParsedDocument {
    json document;
    bool valid = false;
    bool duplicate = false;
};

ParsedDocument parse_document(std::string_view bytes) {
    ParsedDocument result;
    std::map<int, std::set<std::string>> keys;
    try {
        const json::parser_callback_t callback = [&](int depth, json::parse_event_t event,
                                                     json& parsed) {
            if (event == json::parse_event_t::object_start) {
                keys[depth + 1].clear();
            } else if (event == json::parse_event_t::key && parsed.is_string()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!keys[depth].emplace(key).second) {
                    result.duplicate = true;
                }
            } else if (event == json::parse_event_t::object_end) {
                keys.erase(depth + 1);
            }
            return true;
        };
        result.document = json::parse(bytes.begin(), bytes.end(), callback, false, false);
        result.valid = !result.document.is_discarded();
    } catch (const json::exception&) {
        result.valid = false;
    }
    return result;
}

std::variant<IdempotencySnapshot, AccountAuditDurabilityReason>
decode_snapshot(std::string_view bytes, std::string_view account) {
    if (bytes.empty()) {
        return AccountAuditDurabilityReason::ParseError;
    }
    const auto parsed = parse_document(bytes);
    if (!parsed.valid || parsed.duplicate) {
        return AccountAuditDurabilityReason::ParseError;
    }
    if (!exact_fields(parsed.document, {"schema_version", "entries"}) ||
        !parsed.document["schema_version"].is_number_unsigned() ||
        parsed.document["schema_version"] != 1 || !parsed.document["entries"].is_array() ||
        parsed.document["entries"].size() > kIdempotencyStoreMaximumEntries) {
        return AccountAuditDurabilityReason::SchemaError;
    }
    std::vector<IdempotencyEntry> entries;
    entries.reserve(parsed.document["entries"].size());
    for (const auto& value : parsed.document["entries"]) {
        auto entry = parse_entry(value, account);
        if (!entry) {
            return AccountAuditDurabilityReason::SchemaError;
        }
        entries.push_back(std::move(*entry));
    }
    auto canonical = canonicalize_snapshot(std::move(entries), account);
    if (const auto* reason = std::get_if<AccountAuditDurabilityReason>(&canonical)) {
        return *reason;
    }
    auto snapshot = std::move(std::get<IdempotencySnapshot>(canonical));
    if (snapshot.canonical_bytes != bytes) {
        return AccountAuditDurabilityReason::SchemaError;
    }
    return snapshot;
}

} // namespace

IdempotencyKeyHash::IdempotencyKeyHash(std::string value) : value_(std::move(value)) {}
const std::string& IdempotencyKeyHash::value() const {
    return value_;
}

IdempotencyRequestFingerprint::IdempotencyRequestFingerprint(std::string value)
    : value_(std::move(value)) {}
const std::string& IdempotencyRequestFingerprint::value() const {
    return value_;
}

std::optional<IdempotencyKeyHash> parse_idempotency_key_hash(std::string value) {
    if (!valid_digest(value)) {
        return std::nullopt;
    }
    return IdempotencyKeyHash(std::move(value));
}

std::optional<IdempotencyRequestFingerprint>
parse_idempotency_request_fingerprint(std::string value) {
    if (!valid_digest(value)) {
        return std::nullopt;
    }
    return IdempotencyRequestFingerprint(std::move(value));
}

IdempotencyEntryResult make_idempotency_pending_entry(IdempotencyPendingInput input,
                                                      std::string_view account,
                                                      std::string_view store_path) {
    if (!valid_public_store_path(store_path, account)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid,
                            "invalid public idempotency path");
    }
    if (input.created_at > kIdempotencyMaximumUnixSeconds - kIdempotencyRetentionSeconds) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::SchemaError,
                            "idempotency expiry cannot be represented");
    }
    IdempotencyEntry entry{std::move(input.key_hash),
                           std::move(input.request_fingerprint),
                           input.operation,
                           IdempotencyEntryState::Pending,
                           std::move(input.invocation_id),
                           input.audit_generation,
                           input.created_at,
                           input.created_at + kIdempotencyRetentionSeconds,
                           account_audit_terminal_reservation(input.operation),
                           std::move(input.plan),
                           json::array(),
                           json::array(),
                           std::nullopt,
                           std::nullopt};
    if (!valid_entry(entry, account)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::SchemaError,
                            "invalid pending idempotency entry");
    }
    return entry;
}

std::variant<std::string, IdempotencyFailure>
serialize_idempotency_snapshot(const IdempotencySnapshot& snapshot, std::string_view account,
                               std::string_view store_path) {
    auto canonical = canonicalize_snapshot(snapshot.entries, account);
    if (const auto* reason = std::get_if<AccountAuditDurabilityReason>(&canonical)) {
        return make_failure(account, store_path, *reason, "invalid idempotency snapshot");
    }
    return std::get<IdempotencySnapshot>(std::move(canonical)).canonical_bytes;
}

nlohmann::json idempotency_unavailable_terminal(const IdempotencyFailure& failure) {
    return {{"kind", "error"},
            {"code", "IDEMPOTENCY_UNAVAILABLE"},
            {"message", "idempotency store is unavailable"},
            {"details",
             {{"account", failure.account},
              {"path", failure.path},
              {"reason", account_audit_durability_reason_name(failure.reason)}}},
            {"exit_code", 6}};
}

namespace {

bool injected(const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
              IdempotencyStoreFault fault) {
    return hooks && hooks->should_fail && hooks->should_fail(fault);
}

void notify(const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
            IdempotencyStoreStage stage) {
    if (hooks && hooks->at_stage) {
        hooks->at_stage(stage);
    }
}

void mutate_metadata(const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
                     IdempotencyStoreMetadata target, struct stat& metadata) {
    if (hooks && hooks->mutate_metadata) {
        hooks->mutate_metadata(target, metadata);
    }
}

ssize_t read_bytes(const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks, int fd,
                   void* buffer, std::size_t count) {
    return hooks && hooks->read ? hooks->read(fd, buffer, count) : ::read(fd, buffer, count);
}

ssize_t write_bytes(const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks, int fd,
                    const void* buffer, std::size_t count) {
    return hooks && hooks->write ? hooks->write(fd, buffer, count) : ::write(fd, buffer, count);
}

int sync_descriptor(const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
                    IdempotencyStoreStage stage, int fd) {
    return hooks && hooks->sync ? hooks->sync(stage, fd) : ::fsync(fd);
}

bool same_identity(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size && left.st_mode == right.st_mode &&
           left.st_uid == right.st_uid && left.st_nlink == right.st_nlink;
}

bool same_file_object(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_mode == right.st_mode && left.st_uid == right.st_uid &&
           left.st_nlink == right.st_nlink;
}

std::optional<AccountAuditDurabilityReason> classify_root(const struct stat& metadata,
                                                          uid_t expected_uid) {
    if (!S_ISDIR(metadata.st_mode)) {
        return AccountAuditDurabilityReason::WrongType;
    }
    if (metadata.st_uid != expected_uid) {
        return AccountAuditDurabilityReason::WrongOwner;
    }
    if ((metadata.st_mode & 07777) != 0700) {
        return AccountAuditDurabilityReason::WrongMode;
    }
    return std::nullopt;
}

std::optional<AccountAuditDurabilityReason> classify_file(const struct stat& metadata,
                                                          uid_t expected_uid, bool enforce_size) {
    if (S_ISLNK(metadata.st_mode)) {
        return AccountAuditDurabilityReason::PathInvalid;
    }
    if (!S_ISREG(metadata.st_mode)) {
        return AccountAuditDurabilityReason::WrongType;
    }
    if (metadata.st_uid != expected_uid) {
        return AccountAuditDurabilityReason::WrongOwner;
    }
    if ((metadata.st_mode & 07777) != 0600) {
        return AccountAuditDurabilityReason::WrongMode;
    }
    if (metadata.st_nlink != 1) {
        return AccountAuditDurabilityReason::WrongLinkCount;
    }
    if (enforce_size && (metadata.st_size < 0 || static_cast<std::uint64_t>(metadata.st_size) >
                                                     kIdempotencyStoreMaximumBytes)) {
        return AccountAuditDurabilityReason::TooLarge;
    }
    return std::nullopt;
}

struct OpenedRoot {
    Descriptor descriptor;
    struct stat metadata {};
};

std::variant<OpenedRoot, IdempotencyFailure>
open_root(std::string_view state_directory, std::string_view account, std::string_view store_path,
          uid_t expected_uid, const AccountAuditCoordinator::Guard& guard,
          const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
          bool honor_interruption) {
    std::string lease_error;
    if (!guard.validate_lease(state_directory, account, expected_uid, lease_error)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::LockFailed,
                            std::move(lease_error));
    }
    if (honor_interruption) {
        AccountAuditFailure interruption;
        if (guard.interrupted(interruption)) {
            return make_failure(account, store_path, interruption.reason,
                                std::move(interruption.detail), interruption.interruption);
        }
    }
    notify(hooks, IdempotencyStoreStage::BeforeStateOpen);
    struct stat named {};
    if (injected(hooks, IdempotencyStoreFault::Open) ||
        ::lstat(std::string(state_directory).c_str(), &named) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::StateEntry, named);
    if (S_ISLNK(named.st_mode)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    if (const auto reason = classify_root(named, expected_uid)) {
        return make_failure(account, store_path, *reason);
    }
    Descriptor directory(::open(std::string(state_directory).c_str(),
                                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat opened {};
    if (directory.get() < 0 || ::fstat(directory.get(), &opened) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::StateDescriptor, opened);
    if (!same_identity(named, opened)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    if (const auto reason = classify_root(opened, expected_uid)) {
        return make_failure(account, store_path, *reason);
    }
    notify(hooks, IdempotencyStoreStage::AfterStateOpen);
    return OpenedRoot{std::move(directory), opened};
}

struct StoreImage {
    IdempotencySnapshot snapshot;
    bool final_present = false;
    struct stat final_metadata {};
    bool temp_present = false;
    struct stat temp_metadata {};
};

std::variant<std::string, IdempotencyFailure>
read_stable_file(int directory_fd, const char* name, const struct stat& named_metadata,
                 std::string_view account, std::string_view store_path, uid_t expected_uid,
                 const AccountAuditCoordinator::Guard& guard,
                 const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
                 bool honor_interruption, struct stat& descriptor_metadata) {
    const Descriptor file(::openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0 || ::fstat(file.get(), &descriptor_metadata) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::FinalDescriptor, descriptor_metadata);
    if (!same_identity(named_metadata, descriptor_metadata)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    if (const auto reason = classify_file(descriptor_metadata, expected_uid, true)) {
        return make_failure(account, store_path, *reason);
    }
    notify(hooks, IdempotencyStoreStage::AfterFinalOpen);
    std::string bytes(static_cast<std::size_t>(descriptor_metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (honor_interruption) {
            AccountAuditFailure interruption;
            if (guard.interrupted(interruption)) {
                return make_failure(account, store_path, interruption.reason,
                                    std::move(interruption.detail), interruption.interruption);
            }
        }
        notify(hooks, IdempotencyStoreStage::DuringFinalRead);
        if (injected(hooks, IdempotencyStoreFault::Read)) {
            return make_failure(account, store_path, AccountAuditDurabilityReason::ReadFailed);
        }
        const auto count = read_bytes(hooks, file.get(), bytes.data() + offset,
                                      std::min(kIoChunk, bytes.size() - offset));
        if (count <= 0) {
            return make_failure(account, store_path, AccountAuditDurabilityReason::ReadFailed);
        }
        offset += static_cast<std::size_t>(count);
    }
    std::array<char, 1> extra{};
    const auto extra_count = read_bytes(hooks, file.get(), extra.data(), extra.size());
    if (extra_count != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::ReadFailed);
    }
    struct stat final_descriptor {};
    struct stat final_named {};
    if (::fstat(file.get(), &final_descriptor) != 0 ||
        ::fstatat(directory_fd, name, &final_named, AT_SYMLINK_NOFOLLOW) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::ReadFailed);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::FinalDescriptor, final_descriptor);
    mutate_metadata(hooks, IdempotencyStoreMetadata::FinalEntry, final_named);
    if (!same_identity(descriptor_metadata, final_descriptor) ||
        !same_identity(descriptor_metadata, final_named)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::ReadFailed);
    }
    return bytes;
}

std::variant<StoreImage, IdempotencyFailure> inspect_image(
    std::string_view state_directory, std::string_view account, std::string_view store_path,
    uid_t expected_uid, const AccountAuditCoordinator::Guard& guard,
    const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks, bool honor_interruption) {
    auto root_result = open_root(state_directory, account, store_path, expected_uid, guard, hooks,
                                 honor_interruption);
    if (auto* failure = std::get_if<IdempotencyFailure>(&root_result)) {
        return std::move(*failure);
    }
    auto root = std::move(std::get<OpenedRoot>(root_result));
    StoreImage image;
    notify(hooks, IdempotencyStoreStage::BeforeFinalInspect);
    struct stat final_named {};
    if (::fstatat(root.descriptor.get(), kFinalName.data(), &final_named, AT_SYMLINK_NOFOLLOW) !=
        0) {
        if (errno != ENOENT) {
            return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
        }
        auto empty = canonicalize_snapshot({}, account);
        if (const auto* reason = std::get_if<AccountAuditDurabilityReason>(&empty)) {
            return make_failure(account, store_path, *reason);
        }
        image.snapshot = std::move(std::get<IdempotencySnapshot>(empty));
    } else {
        mutate_metadata(hooks, IdempotencyStoreMetadata::FinalEntry, final_named);
        if (const auto reason = classify_file(final_named, expected_uid, true)) {
            return make_failure(account, store_path, *reason);
        }
        struct stat descriptor_metadata {};
        auto bytes = read_stable_file(root.descriptor.get(), kFinalName.data(), final_named,
                                      account, store_path, expected_uid, guard, hooks,
                                      honor_interruption, descriptor_metadata);
        if (auto* failure = std::get_if<IdempotencyFailure>(&bytes)) {
            return std::move(*failure);
        }
        auto decoded = decode_snapshot(std::get<std::string>(bytes), account);
        if (const auto* reason = std::get_if<AccountAuditDurabilityReason>(&decoded)) {
            return make_failure(account, store_path, *reason);
        }
        image.snapshot = std::move(std::get<IdempotencySnapshot>(decoded));
        image.final_present = true;
        image.final_metadata = descriptor_metadata;
    }
    notify(hooks, IdempotencyStoreStage::BeforeTempInspect);
    struct stat temp_named {};
    if (::fstatat(root.descriptor.get(), kTempName.data(), &temp_named, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
            return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
        }
    } else {
        mutate_metadata(hooks, IdempotencyStoreMetadata::TempEntry, temp_named);
        if (const auto reason = classify_file(temp_named, expected_uid, false)) {
            return make_failure(account, store_path, *reason);
        }
        image.temp_present = true;
        image.temp_metadata = temp_named;
    }
    return image;
}

bool write_all(int fd, std::string_view bytes,
               const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        notify(hooks, IdempotencyStoreStage::DuringTempWrite);
        if (injected(hooks, IdempotencyStoreFault::Write)) {
            return false;
        }
        const auto count = write_bytes(hooks, fd, bytes.data() + offset,
                                       std::min(kIoChunk, bytes.size() - offset));
        if (count <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

// NOLINTBEGIN(readability-function-cognitive-complexity): exact atomic durability boundaries.
std::variant<IdempotencySnapshot, IdempotencyFailure>
rewrite_snapshot(std::string_view state_directory, std::string_view account,
                 std::string_view store_path, uid_t expected_uid,
                 const IdempotencySnapshot& expected, IdempotencySnapshot desired,
                 const AccountAuditCoordinator::Guard& guard,
                 const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks) {
    auto canonical = canonicalize_snapshot(std::move(desired.entries), account);
    if (const auto* reason = std::get_if<AccountAuditDurabilityReason>(&canonical)) {
        return make_failure(account, store_path, *reason, "invalid prospective snapshot");
    }
    desired = std::move(std::get<IdempotencySnapshot>(canonical));
    auto image_result =
        inspect_image(state_directory, account, store_path, expected_uid, guard, hooks, true);
    if (auto* failure = std::get_if<IdempotencyFailure>(&image_result)) {
        return std::move(*failure);
    }
    auto image = std::move(std::get<StoreImage>(image_result));
    if (image.snapshot.canonical_bytes != expected.canonical_bytes || image.temp_present) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::Contradiction,
                            "store changed or retained a temp before rewrite");
    }
    auto root_result =
        open_root(state_directory, account, store_path, expected_uid, guard, hooks, false);
    if (auto* failure = std::get_if<IdempotencyFailure>(&root_result)) {
        return std::move(*failure);
    }
    auto root = std::move(std::get<OpenedRoot>(root_result));
    notify(hooks, IdempotencyStoreStage::BeforeTempCreate);
    const Descriptor temp(::openat(root.descriptor.get(), kTempName.data(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (temp.get() < 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
    }
    if (::fchmod(temp.get(), 0600) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::WriteFailed);
    }
    notify(hooks, IdempotencyStoreStage::AfterTempCreate);
    struct stat temp_metadata {};
    if (::fstat(temp.get(), &temp_metadata) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::OpenFailed);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::TempDescriptor, temp_metadata);
    if (const auto reason = classify_file(temp_metadata, expected_uid, false)) {
        return make_failure(account, store_path, *reason);
    }
    if (!write_all(temp.get(), desired.canonical_bytes, hooks)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::WriteFailed);
    }
    notify(hooks, IdempotencyStoreStage::BeforeTempFileSync);
    if (injected(hooks, IdempotencyStoreFault::FileSync) ||
        sync_descriptor(hooks, IdempotencyStoreStage::BeforeTempFileSync, temp.get()) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::SyncFailed);
    }
    notify(hooks, IdempotencyStoreStage::BeforeTempRevalidate);
    struct stat synced_temp {};
    struct stat named_temp {};
    if (::fstat(temp.get(), &synced_temp) != 0 ||
        ::fstatat(root.descriptor.get(), kTempName.data(), &named_temp, AT_SYMLINK_NOFOLLOW) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::TempDescriptor, synced_temp);
    mutate_metadata(hooks, IdempotencyStoreMetadata::TempEntry, named_temp);
    if (!same_file_object(temp_metadata, synced_temp) ||
        !same_file_object(temp_metadata, named_temp) ||
        synced_temp.st_size != static_cast<off_t>(desired.canonical_bytes.size()) ||
        named_temp.st_size != synced_temp.st_size) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    notify(hooks, IdempotencyStoreStage::BeforeFinalRevalidate);
    struct stat current_final {};
    if (::fstatat(root.descriptor.get(), kFinalName.data(), &current_final, AT_SYMLINK_NOFOLLOW) !=
        0) {
        if (errno != ENOENT || image.final_present) {
            return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
        }
    } else if (!image.final_present || !same_identity(image.final_metadata, current_final)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    notify(hooks, IdempotencyStoreStage::BeforeRename);
    if (injected(hooks, IdempotencyStoreFault::Rename) ||
        ::renameat(root.descriptor.get(), kTempName.data(), root.descriptor.get(),
                   kFinalName.data()) != 0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::RenameFailed);
    }
    notify(hooks, IdempotencyStoreStage::AfterRename);
    notify(hooks, IdempotencyStoreStage::BeforeFinalNameRevalidate);
    struct stat final_named {};
    if (::fstatat(root.descriptor.get(), kFinalName.data(), &final_named, AT_SYMLINK_NOFOLLOW) !=
        0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    mutate_metadata(hooks, IdempotencyStoreMetadata::FinalEntry, final_named);
    if (!same_identity(synced_temp, final_named)) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::PathInvalid);
    }
    notify(hooks, IdempotencyStoreStage::BeforeDirectorySync);
    if (injected(hooks, IdempotencyStoreFault::DirectorySync) ||
        sync_descriptor(hooks, IdempotencyStoreStage::BeforeDirectorySync, root.descriptor.get()) !=
            0) {
        return make_failure(account, store_path, AccountAuditDurabilityReason::DirectorySyncFailed);
    }
    return desired;
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace

IdempotencyStore::IdempotencyStore(std::string state_directory, std::string account,
                                   uid_t expected_uid,
                                   std::shared_ptr<const testing::IdempotencyStoreHooks> hooks)
    : state_directory_(std::move(state_directory)),
      store_path_(state_directory_ + "/" + std::string(kFinalName)), account_(std::move(account)),
      expected_uid_(expected_uid), hooks_(std::move(hooks)) {}

std::variant<IdempotencyStore, IdempotencyFailure>
IdempotencyStore::create(std::string state_directory, std::string account, uid_t expected_uid,
                         std::shared_ptr<const testing::IdempotencyStoreHooks> hooks) {
    std::string public_path;
    try {
        auto frozen = std::filesystem::path(state_directory);
        if (!frozen.is_absolute()) {
            frozen = std::filesystem::absolute(frozen);
        }
        public_path = (frozen.lexically_normal() / kFinalName).lexically_normal().string();
    } catch (const std::filesystem::filesystem_error&) {
        try {
            public_path = (std::filesystem::current_path() / account / kFinalName)
                              .lexically_normal()
                              .string();
        } catch (const std::filesystem::filesystem_error&) {
            public_path = "/" + account + "/" + std::string(kFinalName);
        }
    }
    if (!valid_frozen_state_path(state_directory, account)) {
        return make_failure(account, public_path, AccountAuditDurabilityReason::PathInvalid,
                            "invalid frozen idempotency path");
    }
    return IdempotencyStore(std::move(state_directory), std::move(account), expected_uid,
                            std::move(hooks));
}

const std::string& IdempotencyStore::path() const {
    return store_path_;
}

const std::string& IdempotencyStore::account() const {
    return account_;
}

IdempotencyStoreInspection
IdempotencyStore::inspect(const AccountAuditCoordinator::Guard& guard) const {
    auto image =
        inspect_image(state_directory_, account_, store_path_, expected_uid_, guard, hooks_, true);
    if (auto* failure = std::get_if<IdempotencyFailure>(&image)) {
        return {failure->interruption ? IdempotencyInspectionStatus::Interrupted
                                      : IdempotencyInspectionStatus::Unavailable,
                {},
                false,
                std::move(*failure)};
    }
    auto result = std::move(std::get<StoreImage>(image));
    return {
        IdempotencyInspectionStatus::Clean, std::move(result.snapshot), result.temp_present, {}};
}

IdempotencyWriteResult
IdempotencyStore::cleanup_stale_temp(const AccountAuditCoordinator::Guard& guard) const {
    auto image_result =
        inspect_image(state_directory_, account_, store_path_, expected_uid_, guard, hooks_, true);
    if (auto* failure = std::get_if<IdempotencyFailure>(&image_result)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    auto image = std::move(std::get<StoreImage>(image_result));
    if (!image.temp_present) {
        return {IdempotencyWriteStatus::Unchanged, std::move(image.snapshot), {}};
    }
    auto root_result =
        open_root(state_directory_, account_, store_path_, expected_uid_, guard, hooks_, true);
    if (auto* failure = std::get_if<IdempotencyFailure>(&root_result)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    auto root = std::move(std::get<OpenedRoot>(root_result));
    struct stat current {};
    if (::fstatat(root.descriptor.get(), kTempName.data(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_identity(image.temp_metadata, current)) {
        return {IdempotencyWriteStatus::Failed,
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::PathInvalid)};
    }
    notify(hooks_, IdempotencyStoreStage::BeforeTempUnlink);
    if (injected(hooks_, IdempotencyStoreFault::Unlink) ||
        ::unlinkat(root.descriptor.get(), kTempName.data(), 0) != 0) {
        return {IdempotencyWriteStatus::Failed,
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::RenameFailed)};
    }
    notify(hooks_, IdempotencyStoreStage::BeforeTempCleanupDirectorySync);
    if (injected(hooks_, IdempotencyStoreFault::DirectorySync) ||
        sync_descriptor(hooks_, IdempotencyStoreStage::BeforeTempCleanupDirectorySync,
                        root.descriptor.get()) != 0) {
        return {
            IdempotencyWriteStatus::Failed,
            {},
            make_failure(account_, store_path_, AccountAuditDurabilityReason::DirectorySyncFailed)};
    }
    return {IdempotencyWriteStatus::Applied, std::move(image.snapshot), {}};
}

KnownAccountAuditPins IdempotencyStore::pins(const IdempotencySnapshot& snapshot) {
    KnownAccountAuditPins result;
    result.pins.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        result.pins.push_back({entry.audit_generation, entry.invocation_id,
                               entry.request_fingerprint.value(), entry.operation});
    }
    return result;
}

IdempotencyLookup IdempotencyStore::lookup(const IdempotencySnapshot& snapshot,
                                           const IdempotencyKeyHash& key_hash,
                                           const IdempotencyRequestFingerprint& fingerprint) {
    const auto found = std::ranges::lower_bound(
        snapshot.entries, key_hash.value(), {},
        [](const IdempotencyEntry& entry) { return entry.key_hash.value(); });
    if (found == snapshot.entries.end() || found->key_hash != key_hash) {
        return {};
    }
    if (found->request_fingerprint != fingerprint) {
        return {IdempotencyLookupStatus::Conflict, &*found};
    }
    return {found->state == IdempotencyEntryState::Pending ? IdempotencyLookupStatus::Pending
                                                           : IdempotencyLookupStatus::Completed,
            &*found};
}

IdempotencyInsertResult
IdempotencyStore::insert_if_absent(const IdempotencyEntry& entry,
                                   const AccountAuditCoordinator::Guard& guard) const {
    if (entry.state != IdempotencyEntryState::Pending || !valid_entry(entry, account_)) {
        return {IdempotencyInsertStatus::Failed,
                {},
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::SchemaError,
                             "invalid pending idempotency entry")};
    }
    auto inspection = inspect(guard);
    if (inspection.status != IdempotencyInspectionStatus::Clean) {
        return {IdempotencyInsertStatus::Failed, {}, {}, std::move(inspection.failure)};
    }
    if (inspection.stale_temp_present) {
        return {IdempotencyInsertStatus::Failed,
                {},
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::Contradiction,
                             "stale temp was not reconciled before insertion")};
    }
    const auto found = std::ranges::lower_bound(
        inspection.snapshot.entries, entry.key_hash.value(), {},
        [](const IdempotencyEntry& candidate) { return candidate.key_hash.value(); });
    if (found != inspection.snapshot.entries.end() && found->key_hash == entry.key_hash) {
        return {IdempotencyInsertStatus::UnexpectedIncumbent,
                *found,
                std::move(inspection.snapshot),
                {}};
    }
    notify(hooks_, IdempotencyStoreStage::BeforeCapacity);
    if (inspection.snapshot.entries.size() >= kIdempotencyStoreMaximumEntries) {
        return {
            IdempotencyInsertStatus::Failed,
            {},
            std::move(inspection.snapshot),
            make_failure(account_, store_path_, AccountAuditDurabilityReason::CapacityExhausted)};
    }
    IdempotencySnapshot desired = inspection.snapshot;
    const auto position = std::ranges::lower_bound(
        desired.entries, entry.key_hash.value(), {},
        [](const IdempotencyEntry& candidate) { return candidate.key_hash.value(); });
    desired.entries.insert(position, entry);
    auto prospective = canonicalize_snapshot(desired.entries, account_);
    if (std::holds_alternative<AccountAuditDurabilityReason>(prospective)) {
        return {
            IdempotencyInsertStatus::Failed,
            {},
            std::move(inspection.snapshot),
            make_failure(account_, store_path_, AccountAuditDurabilityReason::CapacityExhausted)};
    }
    desired = std::move(std::get<IdempotencySnapshot>(prospective));
    auto written = rewrite_snapshot(state_directory_, account_, store_path_, expected_uid_,
                                    inspection.snapshot, std::move(desired), guard, hooks_);
    if (auto* failure = std::get_if<IdempotencyFailure>(&written)) {
        return {IdempotencyInsertStatus::Failed, {}, {}, std::move(*failure)};
    }
    return {IdempotencyInsertStatus::Inserted,
            {},
            std::move(std::get<IdempotencySnapshot>(written)),
            {}};
}

namespace {

using EntryMutation = std::function<bool(IdempotencyEntry&, IdempotencyFailure&)>;

IdempotencyWriteResult
mutate_owned_entry(std::string_view state_directory, std::string_view account,
                   std::string_view store_path, uid_t expected_uid,
                   const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                   const AccountAuditCoordinator::Guard& guard,
                   const std::shared_ptr<const testing::IdempotencyStoreHooks>& hooks,
                   const EntryMutation& mutation, bool preserve_other_incumbent) {
    auto image_result =
        inspect_image(state_directory, account, store_path, expected_uid, guard, hooks, true);
    if (auto* failure = std::get_if<IdempotencyFailure>(&image_result)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    auto image = std::move(std::get<StoreImage>(image_result));
    if (image.temp_present) {
        return {IdempotencyWriteStatus::Failed,
                {},
                make_failure(account, store_path, AccountAuditDurabilityReason::Contradiction,
                             "stale temp was not reconciled before store transition")};
    }
    const auto found = std::ranges::lower_bound(
        image.snapshot.entries, key_hash.value(), {},
        [](const IdempotencyEntry& entry) { return entry.key_hash.value(); });
    if (found == image.snapshot.entries.end() || found->key_hash != key_hash) {
        return {IdempotencyWriteStatus::Failed, std::move(image.snapshot),
                make_failure(account, store_path, AccountAuditDurabilityReason::Contradiction,
                             "owned idempotency entry is missing")};
    }
    if (found->invocation_id != invocation_id) {
        if (preserve_other_incumbent) {
            return {IdempotencyWriteStatus::IncumbentPreserved, std::move(image.snapshot), {}};
        }
        return {IdempotencyWriteStatus::Failed, std::move(image.snapshot),
                make_failure(account, store_path, AccountAuditDurabilityReason::Contradiction,
                             "idempotency entry belongs to another invocation")};
    }
    IdempotencySnapshot desired = image.snapshot;
    auto desired_entry = std::ranges::lower_bound(
        desired.entries, key_hash.value(), {},
        [](const IdempotencyEntry& entry) { return entry.key_hash.value(); });
    IdempotencyFailure mutation_failure;
    if (!mutation(*desired_entry, mutation_failure)) {
        if (mutation_failure.path.empty()) {
            mutation_failure.account = std::string(account);
            mutation_failure.path = std::string(store_path);
        }
        return {IdempotencyWriteStatus::Failed, std::move(image.snapshot),
                std::move(mutation_failure)};
    }
    if (*desired_entry == *found) {
        return {IdempotencyWriteStatus::Unchanged, std::move(image.snapshot), {}};
    }
    auto written = rewrite_snapshot(state_directory, account, store_path, expected_uid,
                                    image.snapshot, std::move(desired), guard, hooks);
    if (auto* failure = std::get_if<IdempotencyFailure>(&written)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    return {IdempotencyWriteStatus::Applied, std::move(std::get<IdempotencySnapshot>(written)), {}};
}

IdempotencyFailure schema_transition_failure(std::string detail) {
    return make_failure({}, {}, AccountAuditDurabilityReason::SchemaError, std::move(detail));
}

} // namespace

IdempotencyWriteResult
IdempotencyStore::update_spool(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                               const SpoolRef& spool,
                               const AccountAuditCoordinator::Guard& guard) const {
    return mutate_owned_entry(
        state_directory_, account_, store_path_, expected_uid_, key_hash, invocation_id, guard,
        hooks_,
        [&spool](IdempotencyEntry& entry, IdempotencyFailure& failure) {
            if (entry.state != IdempotencyEntryState::Pending ||
                !validate_account_audit_persisted_spool(spool, entry.invocation_id) ||
                (entry.spool && *entry.spool != spool)) {
                failure = schema_transition_failure("invalid spool transition");
                return false;
            }
            entry.spool = spool;
            return true;
        },
        false);
}

IdempotencyWriteResult
IdempotencyStore::update_temporary_message_ids(const IdempotencyKeyHash& key_hash,
                                               std::string_view invocation_id, json temporary_ids,
                                               const AccountAuditCoordinator::Guard& guard) const {
    return mutate_owned_entry(
        state_directory_, account_, store_path_, expected_uid_, key_hash, invocation_id, guard,
        hooks_,
        [temporary_ids = std::move(temporary_ids)](IdempotencyEntry& entry,
                                                   IdempotencyFailure& failure) mutable {
            if (entry.state != IdempotencyEntryState::Pending ||
                !validate_account_audit_persisted_temporary_ids(entry.operation, temporary_ids,
                                                                entry.plan) ||
                (!entry.temporary_message_ids.empty() &&
                 entry.temporary_message_ids != temporary_ids)) {
                failure = schema_transition_failure("invalid temporary-id transition");
                return false;
            }
            entry.temporary_message_ids = std::move(temporary_ids);
            return true;
        },
        false);
}

IdempotencyWriteResult
IdempotencyStore::update_forward_progress(const IdempotencyKeyHash& key_hash,
                                          std::string_view invocation_id, json items,
                                          const AccountAuditCoordinator::Guard& guard) const {
    return mutate_owned_entry(
        state_directory_, account_, store_path_, expected_uid_, key_hash, invocation_id, guard,
        hooks_,
        [items = std::move(items)](IdempotencyEntry& entry, IdempotencyFailure& failure) mutable {
            const auto advances = [&]() {
                if (entry.forward_progress.empty() || entry.forward_progress == items) {
                    return true;
                }
                if (!entry.forward_progress.is_array() || !items.is_array() ||
                    entry.forward_progress.size() != items.size()) {
                    return false;
                }
                for (std::size_t index = 0; index < items.size(); ++index) {
                    const auto& before = entry.forward_progress.at(index);
                    const auto& after = items.at(index);
                    if (before["source_id"] != after["source_id"] ||
                        (before["status"] != "pending" && before != after) ||
                        (before["status"] == "pending" && after["status"] == "pending" &&
                         before != after)) {
                        return false;
                    }
                }
                return true;
            };
            if (entry.state != IdempotencyEntryState::Pending ||
                !validate_account_audit_persisted_forward_progress(entry.operation, items,
                                                                   entry.plan) ||
                !advances()) {
                failure = schema_transition_failure("invalid forward-progress transition");
                return false;
            }
            entry.forward_progress = std::move(items);
            return true;
        },
        false);
}

IdempotencyWriteResult
IdempotencyStore::complete(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                           json terminal, const AccountAuditCoordinator::Guard& guard) const {
    return mutate_owned_entry(
        state_directory_, account_, store_path_, expected_uid_, key_hash, invocation_id, guard,
        hooks_,
        [terminal = std::move(terminal), account = account_](IdempotencyEntry& entry,
                                                             IdempotencyFailure& failure) mutable {
            if (!validate_account_audit_persisted_terminal(entry.operation, terminal, entry.plan,
                                                           account)) {
                failure = schema_transition_failure("invalid completed terminal");
                return false;
            }
            if (entry.state == IdempotencyEntryState::Completed) {
                if (!entry.terminal || *entry.terminal != terminal) {
                    failure = schema_transition_failure("completed terminal changed");
                    return false;
                }
                return true;
            }
            IdempotencyEntry prospective = entry;
            prospective.state = IdempotencyEntryState::Completed;
            prospective.reserved_terminal_bytes = 0;
            prospective.temporary_message_ids = json::array();
            prospective.forward_progress = json::array();
            prospective.terminal = terminal;
            const auto charge = mutable_charge(prospective);
            if (!charge || *charge > account_audit_terminal_reservation(entry.operation)) {
                failure = schema_transition_failure("completed terminal exceeds reservation");
                return false;
            }
            entry = std::move(prospective);
            return true;
        },
        false);
}

IdempotencyWriteResult
IdempotencyStore::remove_owned(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                               const AccountAuditCoordinator::Guard& guard) const {
    auto image_result =
        inspect_image(state_directory_, account_, store_path_, expected_uid_, guard, hooks_, true);
    if (auto* failure = std::get_if<IdempotencyFailure>(&image_result)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    auto image = std::move(std::get<StoreImage>(image_result));
    if (image.temp_present) {
        return {IdempotencyWriteStatus::Failed,
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::Contradiction,
                             "stale temp was not reconciled before removal")};
    }
    const auto found = std::ranges::lower_bound(
        image.snapshot.entries, key_hash.value(), {},
        [](const IdempotencyEntry& entry) { return entry.key_hash.value(); });
    if (found == image.snapshot.entries.end() || found->key_hash != key_hash) {
        return {IdempotencyWriteStatus::Unchanged, std::move(image.snapshot), {}};
    }
    if (found->invocation_id != invocation_id) {
        return {IdempotencyWriteStatus::IncumbentPreserved, std::move(image.snapshot), {}};
    }
    IdempotencySnapshot desired = image.snapshot;
    const auto position = std::ranges::lower_bound(
        desired.entries, key_hash.value(), {},
        [](const IdempotencyEntry& entry) { return entry.key_hash.value(); });
    desired.entries.erase(position);
    auto written = rewrite_snapshot(state_directory_, account_, store_path_, expected_uid_,
                                    image.snapshot, std::move(desired), guard, hooks_);
    if (auto* failure = std::get_if<IdempotencyFailure>(&written)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    return {IdempotencyWriteStatus::Applied, std::move(std::get<IdempotencySnapshot>(written)), {}};
}

IdempotencyWriteResult
IdempotencyStore::clear_spool(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                              const AccountAuditCoordinator::Guard& guard) const {
    return mutate_owned_entry(
        state_directory_, account_, store_path_, expected_uid_, key_hash, invocation_id, guard,
        hooks_,
        [](IdempotencyEntry& entry, IdempotencyFailure& failure) {
            if (entry.state != IdempotencyEntryState::Completed) {
                failure = schema_transition_failure("pending spool reference cannot be cleared");
                return false;
            }
            entry.spool.reset();
            return true;
        },
        false);
}

IdempotencySweepResult
IdempotencyStore::sweep_expired(std::uint64_t sampled_now,
                                const AccountAuditCoordinator::Guard& guard) const {
    if (sampled_now > kIdempotencyMaximumUnixSeconds) {
        return {IdempotencyWriteStatus::Failed,
                {},
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::SchemaError,
                             "wall-clock sample is unrepresentable")};
    }
    auto inspection = inspect(guard);
    if (inspection.status != IdempotencyInspectionStatus::Clean) {
        return {IdempotencyWriteStatus::Failed, {}, {}, std::move(inspection.failure)};
    }
    if (inspection.stale_temp_present) {
        return {IdempotencyWriteStatus::Failed,
                {},
                {},
                make_failure(account_, store_path_, AccountAuditDurabilityReason::Contradiction,
                             "stale temp was not reconciled before expiry")};
    }
    IdempotencySnapshot desired;
    desired.entries.reserve(inspection.snapshot.entries.size());
    std::vector<IdempotencyEntry> removed;
    for (const auto& entry : inspection.snapshot.entries) {
        if (sampled_now >= entry.expires_at) {
            removed.push_back(entry);
        } else {
            desired.entries.push_back(entry);
        }
    }
    if (removed.empty()) {
        return {IdempotencyWriteStatus::Unchanged, std::move(inspection.snapshot), {}, {}};
    }
    auto written = rewrite_snapshot(state_directory_, account_, store_path_, expected_uid_,
                                    inspection.snapshot, std::move(desired), guard, hooks_);
    if (auto* failure = std::get_if<IdempotencyFailure>(&written)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(removed), std::move(*failure)};
    }
    return {IdempotencyWriteStatus::Applied,
            std::move(std::get<IdempotencySnapshot>(written)),
            std::move(removed),
            {}};
}

IdempotencyWriteResult
IdempotencyStore::apply_reconciled_snapshot(const IdempotencySnapshot& expected,
                                            IdempotencySnapshot desired,
                                            const AccountAuditCoordinator::Guard& guard) const {
    auto canonical = canonicalize_snapshot(std::move(desired.entries), account_);
    if (const auto* reason = std::get_if<AccountAuditDurabilityReason>(&canonical)) {
        return {IdempotencyWriteStatus::Failed,
                {},
                make_failure(account_, store_path_, *reason,
                             "invalid reconciled idempotency snapshot")};
    }
    desired = std::move(std::get<IdempotencySnapshot>(canonical));
    if (desired.canonical_bytes == expected.canonical_bytes) {
        return {IdempotencyWriteStatus::Unchanged, expected, {}};
    }
    auto written = rewrite_snapshot(state_directory_, account_, store_path_, expected_uid_,
                                    expected, std::move(desired), guard, hooks_);
    if (auto* failure = std::get_if<IdempotencyFailure>(&written)) {
        return {IdempotencyWriteStatus::Failed, {}, std::move(*failure)};
    }
    auto snapshot = std::move(std::get<IdempotencySnapshot>(written));
    return {IdempotencyWriteStatus::Applied, std::move(snapshot), {}};
}

} // namespace tgcli::daemon
