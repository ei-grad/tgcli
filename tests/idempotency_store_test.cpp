#include "common/daemon_lock.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/idempotency_store.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using nlohmann::json;

namespace {

constexpr std::string_view kFingerprint =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kConfigSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
    "dev:1;ino:2;size:3;ctime_ns:4";

std::string digest(char digit) {
    return "sha256:" + std::string(64, digit);
}

std::string hexadecimal(std::uint64_t value, int width) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

std::string indexed_digest(std::uint64_t value) {
    return "sha256:" + hexadecimal(value, 64);
}

std::string indexed_invocation(std::uint64_t value) {
    return hexadecimal(value, 32);
}

json chat() {
    return {{"id", -1001},
            {"title", "Project"},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", json::array({"project"})}};
}

json archive_plan() {
    return {{"operation", "chat_archive"},
            {"account", "main"},
            {"tdlib_request", "addChatToList"},
            {"chat", chat()},
            {"archived", true}};
}

json archive_terminal() {
    return {{"kind", "result"}, {"data", {{"chat_id", -1001}, {"archived", true}}}};
}

json forward_plan() {
    auto target = chat();
    target["id"] = -1002;
    return {{"operation", "msg_forward"},
            {"account", "main"},
            {"tdlib_request", "forwardMessages"},
            {"from", chat()},
            {"to", std::move(target)},
            {"message_ids", json::array({1, 2})},
            {"drop_author", false}};
}

json sent_message(std::int64_t id) {
    return {{"id", id},
            {"chat_id", -1002},
            {"date", "2026-08-20T12:00:01Z"},
            {"sender", {{"type", "user"}, {"id", 42}}},
            {"is_outgoing", true},
            {"topic", nullptr},
            {"type", "text"},
            {"text", "sent"},
            {"scheduled", false}};
}

json pending_forward_progress() {
    return json::array({{{"source_id", 1}, {"status", "pending"}, {"temporary_message_id", 101}},
                        {{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", 102}}});
}

json sent_forward_progress() {
    return json::array({{{"source_id", 1}, {"status", "sent"}, {"message", sent_message(201)}},
                        {{"source_id", 2}, {"status", "sent"}, {"message", sent_message(202)}}});
}

json partial_forward_progress() {
    return json::array({{{"source_id", 1}, {"status", "sent"}, {"message", sent_message(201)}},
                        {{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", 102}}});
}

json forward_terminal() {
    return {{"kind", "result"},
            {"data",
             {{"from_chat_id", -1001}, {"to_chat_id", -1002}, {"items", sent_forward_progress()}}}};
}

class StoreTree final {
  public:
    explicit StoreTree(std::shared_ptr<const daemon::testing::IdempotencyStoreHooks> hooks = {},
                       std::shared_ptr<const daemon::testing::AccountAuditHooks> audit_hooks = {}) {
        std::string pattern = "/tmp/tgcli-idempotency-store-XXXXXX";
        pattern.push_back('\0');
        const auto* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        REQUIRE(std::filesystem::create_directory(root_ + "/accounts"));
        REQUIRE(::chmod((root_ + "/accounts").c_str(), 0700) == 0);
        state_ = root_ + "/accounts/main";
        REQUIRE(std::filesystem::create_directory(state_));
        REQUIRE(::chmod(state_.c_str(), 0700) == 0);
        std::string error;
        lease_ = daemon_lock::acquire_lifetime(state_ + "/daemon.lock", identity_, error);
        REQUIRE(lease_);
        auto created_foundation = daemon::IdempotencyFoundation::create(
            state_, "main", ::getuid(), lease_, std::move(audit_hooks), std::move(hooks));
        REQUIRE(std::holds_alternative<daemon::IdempotencyFoundation>(created_foundation));
        foundation_.emplace(std::get<daemon::IdempotencyFoundation>(std::move(created_foundation)));
    }

    ~StoreTree() {
        foundation_.reset();
        lease_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    StoreTree(const StoreTree&) = delete;
    StoreTree& operator=(const StoreTree&) = delete;
    StoreTree(StoreTree&&) = delete;
    StoreTree& operator=(StoreTree&&) = delete;

    [[nodiscard]] daemon::IdempotencyStore& store() {
        return foundation_->store();
    }
    [[nodiscard]] daemon::AccountAuditCoordinator::Guard guard() {
        return foundation_->acquire_epoch();
    }
    [[nodiscard]] daemon::IdempotencyFoundation& foundation() {
        return *foundation_;
    }
    [[nodiscard]] daemon::AccountAuditLog& audit() {
        return foundation_->audit();
    }
    [[nodiscard]] const std::string& state() const {
        return state_;
    }
    [[nodiscard]] std::string final_path() const {
        return state_ + "/idempotency.db";
    }
    [[nodiscard]] std::string temp_path() const {
        return state_ + "/.idempotency.db.tmp";
    }
    [[nodiscard]] std::string source_path() const {
        return state_ + "/attachment.bin";
    }

    void write_final(std::string_view bytes) const {
        std::ofstream output(final_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(final_path().c_str(), 0600) == 0);
    }

    void write_temp(std::string_view bytes = "partial") const {
        std::ofstream output(temp_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(temp_path().c_str(), 0600) == 0);
    }

    void write_source(std::string_view bytes = "attachment bytes") const {
        std::ofstream output(source_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(source_path().c_str(), 0600) == 0);
    }

  private:
    std::string root_;
    std::string state_;
    daemon_lock::Identity identity_;
    std::shared_ptr<daemon_lock::LifetimeLease> lease_;
    std::optional<daemon::IdempotencyFoundation> foundation_;
};

daemon::IdempotencyKeyHash key_hash(char digit = 'b') {
    auto parsed = daemon::parse_idempotency_key_hash(digest(digit));
    REQUIRE(parsed);
    return std::move(*parsed);
}

daemon::IdempotencyRequestFingerprint fingerprint(char digit = 'a') {
    auto parsed = daemon::parse_idempotency_request_fingerprint(digest(digit));
    REQUIRE(parsed);
    return std::move(*parsed);
}

daemon::IdempotencyEntry pending(char key_digit = 'b', char fingerprint_digit = 'a',
                                 std::string invocation = "0123456789abcdef0123456789abcdef",
                                 std::uint64_t created_at = 1'700'000'000) {
    auto result = daemon::make_idempotency_pending_entry(
        {key_hash(key_digit), fingerprint(fingerprint_digit),
         daemon::AccountAuditOperation::ChatArchive, std::move(invocation), 99, created_at,
         archive_plan()},
        "main", "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(result));
    return std::get<daemon::IdempotencyEntry>(std::move(result));
}

daemon::IdempotencyEntry indexed_entry(std::uint64_t index, bool completed = false) {
    auto key = daemon::parse_idempotency_key_hash(indexed_digest(index));
    auto request_fingerprint = daemon::parse_idempotency_request_fingerprint(digest('a'));
    REQUIRE(key);
    REQUIRE(request_fingerprint);
    auto result = daemon::make_idempotency_pending_entry(
        {std::move(*key), std::move(*request_fingerprint),
         daemon::AccountAuditOperation::ChatArchive, indexed_invocation(index), index + 1,
         1'700'000'000, archive_plan()},
        "main", "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(result));
    auto entry = std::get<daemon::IdempotencyEntry>(std::move(result));
    if (completed) {
        entry.state = daemon::IdempotencyEntryState::Completed;
        entry.reserved_terminal_bytes = 0;
        entry.terminal = archive_terminal();
    }
    return entry;
}

daemon::AccountAuditIntent audit_intent(char key_digit, char fingerprint_digit,
                                        std::string invocation) {
    std::string error;
    auto intent =
        daemon::make_account_audit_intent({{std::move(invocation), "2026-08-20T12:00:00Z"},
                                           "main",
                                           daemon::AccountAuditOperation::ChatArchive,
                                           {{"chat", "@project"}},
                                           archive_plan(),
                                           digest(fingerprint_digit),
                                           std::string(kConfigSnapshot),
                                           "request",
                                           std::nullopt,
                                           digest(key_digit),
                                           1},
                                          error);
    INFO(error);
    REQUIRE(intent);
    return std::move(*intent);
}

daemon::AccountAuditIntent forward_audit_intent(std::string invocation) {
    std::string error;
    auto intent =
        daemon::make_account_audit_intent({{std::move(invocation), "2026-08-20T12:00:00Z"},
                                           "main",
                                           daemon::AccountAuditOperation::MsgForward,
                                           {{"from", "@project"},
                                            {"to", "@target"},
                                            {"message_ids", json::array({1, 2})},
                                            {"drop_author", false}},
                                           forward_plan(),
                                           digest('e'),
                                           std::string(kConfigSnapshot),
                                           "request",
                                           std::nullopt,
                                           digest('d'),
                                           1},
                                          error);
    INFO(error);
    REQUIRE(intent);
    return std::move(*intent);
}

json incomplete_terminal(const StoreTree& tree, daemon::AccountAuditMutationState mutation,
                         const std::vector<daemon::AccountAuditStage>& stages) {
    json completed = json::array();
    for (const auto stage : stages) {
        completed.push_back(daemon::account_audit_stage_name(stage));
    }
    return {{"kind", "error"},
            {"code", "AUDIT_INCOMPLETE"},
            {"message", "a prior audited invocation did not reach a terminal proof"},
            {"details",
             {{"account", "main"},
              {"path", tree.state() + "/audit.log"},
              {"mutation_state", daemon::account_audit_mutation_state_name(mutation)},
              {"completed_stages", std::move(completed)}}},
            {"exit_code", 1}};
}

json file_snapshot_json(const daemon::FileSnapshot& file) {
    return {{"path", file.path},         {"name", file.name},        {"size", file.size},
            {"sha256", file.sha256},     {"device", file.device},    {"inode", file.inode},
            {"mtime_ns", file.mtime_ns}, {"ctime_ns", file.ctime_ns}};
}

json saved_plan(const daemon::FileSnapshot& file) {
    return {{"operation", "saved_attach"},
            {"account", "main"},
            {"tdlib_request", "sendMessage"},
            {"chat", chat()},
            {"message_id", 1},
            {"effective_topic", nullptr},
            {"caption", ""},
            {"file", file_snapshot_json(file)}};
}

json saved_terminal() {
    auto message = sent_message(301);
    message["chat_id"] = -1001;
    return {{"kind", "result"}, {"data", std::move(message)}};
}

json saved_unknown_terminal(const StoreTree& tree, bool keyed = true) {
    return {{"kind", "error"},
            {"code", "AUDIT_INCOMPLETE"},
            {"message", "a prior audited invocation did not reach a terminal proof"},
            {"details",
             {{"account", "main"},
              {"path", tree.state() + "/audit.log"},
              {"mutation_state", "possible"},
              {"completed_stages",
               keyed ? json::array({"idempotency_pending", "spool_ready", "dispatch_started"})
                     : json::array({"spool_ready", "dispatch_started"})}}},
            {"exit_code", 1}};
}

daemon::AccountAuditIntent saved_intent(const daemon::FileSnapshot& file, std::string invocation,
                                        bool keyed = true) {
    std::string error;
    auto intent = daemon::make_account_audit_intent(
        {{std::move(invocation), "2026-08-20T12:00:00Z"},
         "main",
         daemon::AccountAuditOperation::SavedAttach,
         {{"message_id", 1}, {"path", file.path}, {"caption", ""}},
         saved_plan(file),
         digest('a'),
         std::string(kConfigSnapshot),
         "request",
         std::nullopt,
         keyed ? std::optional<std::string>(digest('b')) : std::nullopt,
         1},
        error);
    INFO(error);
    REQUIRE(intent);
    return std::move(*intent);
}

daemon::AccountAuditAppendReceipt append_intent(StoreTree& tree,
                                                daemon::AccountAuditCoordinator::Guard& guard,
                                                const daemon::AccountAuditIntent& intent) {
    const auto inspection = tree.store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    daemon::AccountAuditAppendPermit permit;
    const auto prepared = tree.audit().prepare_append(
        intent, daemon::IdempotencyStore::pins(inspection.snapshot), guard, permit);
    REQUIRE(prepared.status == daemon::AccountAuditInspectionStatus::Clean);
    daemon::AccountAuditAppendReceipt receipt;
    daemon::AccountAuditFailure failure;
    const bool appended =
        tree.audit().append_intent(intent, std::move(permit), guard, receipt, failure);
    REQUIRE(appended);
    return receipt;
}

void append_checkpoint_for(StoreTree& tree, daemon::AccountAuditCoordinator::Guard& guard,
                           daemon::AccountAuditOperation operation, std::string_view invocation,
                           std::uint32_t sequence, daemon::AccountAuditStage stage, json data) {
    std::string error;
    auto checkpoint =
        daemon::make_account_audit_checkpoint({{std::string(invocation), "2026-08-20T12:00:01Z"},
                                               "main",
                                               operation,
                                               sequence,
                                               stage,
                                               std::move(data)},
                                              error);
    INFO(error);
    REQUIRE(checkpoint);
    daemon::AccountAuditFailure failure;
    REQUIRE(tree.audit().append_checkpoint(*checkpoint, guard, failure));
}

void append_checkpoint(StoreTree& tree, daemon::AccountAuditCoordinator::Guard& guard,
                       std::string_view invocation, std::uint32_t sequence,
                       daemon::AccountAuditStage stage, json data) {
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::ChatArchive, invocation,
                          sequence, stage, std::move(data));
}

void append_outcome_for(StoreTree& tree, daemon::AccountAuditCoordinator::Guard& guard,
                        daemon::AccountAuditOperation operation, std::string_view invocation,
                        std::vector<daemon::AccountAuditStage> stages,
                        daemon::AccountAuditMutationState mutation, json terminal) {
    std::string error;
    auto outcome =
        daemon::make_account_audit_outcome({{std::string(invocation), "2026-08-20T12:00:02Z"},
                                            "main",
                                            operation,
                                            terminal["kind"] == "result",
                                            mutation,
                                            std::move(stages),
                                            std::move(terminal)},
                                           error);
    INFO(error);
    REQUIRE(outcome);
    daemon::AccountAuditFailure failure;
    REQUIRE(tree.audit().append_outcome(*outcome, guard, failure));
}

void append_outcome(StoreTree& tree, daemon::AccountAuditCoordinator::Guard& guard,
                    std::string_view invocation, std::vector<daemon::AccountAuditStage> stages,
                    daemon::AccountAuditMutationState mutation, json terminal) {
    append_outcome_for(tree, guard, daemon::AccountAuditOperation::ChatArchive, invocation,
                       std::move(stages), mutation, std::move(terminal));
}

daemon::IdempotencyEntry
complete_archive_invocation(StoreTree& tree, daemon::AccountAuditCoordinator::Guard& guard,
                            const std::string& invocation = "0123456789abcdef0123456789abcdef") {
    const auto receipt = append_intent(tree, guard, audit_intent('b', 'a', invocation));
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive, invocation,
         receipt.audit_generation, 1'700'000'000, archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint(tree, guard, invocation, 1, daemon::AccountAuditStage::IdempotencyPending,
                      {{"key_hash", entry.key_hash.value()},
                       {"request_fingerprint", entry.request_fingerprint.value()},
                       {"expires_at", entry.expires_at},
                       {"reserved_terminal_bytes", entry.reserved_terminal_bytes}});
    append_checkpoint(tree, guard, invocation, 2, daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});
    append_checkpoint(tree, guard, invocation, 3, daemon::AccountAuditStage::MutationConfirmed,
                      {{"terminal", archive_terminal()}});
    append_outcome(tree, guard, invocation,
                   {daemon::AccountAuditStage::IdempotencyPending,
                    daemon::AccountAuditStage::DispatchStarted,
                    daemon::AccountAuditStage::MutationConfirmed},
                   daemon::AccountAuditMutationState::Confirmed, archive_terminal());
    REQUIRE(tree.store().complete(entry.key_hash, invocation, archive_terminal(), guard).status ==
            daemon::IdempotencyWriteStatus::Applied);
    entry.state = daemon::IdempotencyEntryState::Completed;
    entry.reserved_terminal_bytes = 0;
    entry.terminal = archive_terminal();
    return entry;
}

struct SavedOpenState {
    daemon::IdempotencyEntry entry;
    daemon::SpoolRef spool;
};

SavedOpenState
create_saved_open(StoreTree& tree, daemon::AccountAuditCoordinator::Guard& guard,
                  const std::string& invocation = "0123456789abcdef0123456789abcdef") {
    tree.write_source();
    auto prepared_result = daemon::prepare_spool_source(tree.source_path(), "/");
    REQUIRE(std::holds_alternative<daemon::PreparedSource>(prepared_result));
    auto prepared = std::get<daemon::PreparedSource>(std::move(prepared_result));
    const auto source_snapshot = prepared.snapshot();
    const auto intent = saved_intent(source_snapshot, invocation);
    const auto receipt = append_intent(tree, guard, intent);
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::SavedAttach, invocation,
         receipt.audit_generation, 1'700'000'000, saved_plan(source_snapshot)},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach, invocation, 1,
                          daemon::AccountAuditStage::IdempotencyPending,
                          {{"key_hash", entry.key_hash.value()},
                           {"request_fingerprint", entry.request_fingerprint.value()},
                           {"expires_at", entry.expires_at},
                           {"reserved_terminal_bytes", entry.reserved_terminal_bytes}});
    auto spool_result = daemon::create_spool_file(prepared, tree.state(), invocation, ::getuid());
    REQUIRE(std::holds_alternative<daemon::CreatedSpool>(spool_result));
    auto spool = std::get<daemon::CreatedSpool>(std::move(spool_result)).reference;
    append_checkpoint_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, invocation, 2,
        daemon::AccountAuditStage::SpoolReady,
        {{"file", file_snapshot_json(spool.file)}, {"relative_path", spool.relative_path}});
    REQUIRE(tree.store().update_spool(entry.key_hash, invocation, spool, guard).status ==
            daemon::IdempotencyWriteStatus::Applied);
    return {entry, std::move(spool)};
}

} // namespace

TEST_CASE("idempotency store writes one strict canonical snapshot and classifies lookups",
          "[idempotency-store][canonical][lookup]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto missing = tree.store().inspect(guard);
    REQUIRE(missing.status == daemon::IdempotencyInspectionStatus::Clean);
    CHECK(missing.snapshot.canonical_bytes == R"({"entries":[],"schema_version":1})");
    CHECK_FALSE(std::filesystem::exists(tree.final_path()));

    const auto entry = pending();
    const auto inserted = tree.store().insert_if_absent(entry, guard);
    INFO(daemon::account_audit_durability_reason_name(inserted.failure.reason));
    INFO(inserted.failure.detail);
    REQUIRE(inserted.status == daemon::IdempotencyInsertStatus::Inserted);
    REQUIRE(inserted.snapshot.entries.size() == 1);
    const std::string golden =
        R"({"entries":[{"audit_generation":99,"created_at":1700000000,"expires_at":1700604800,)"
        R"("forward_progress":[],"invocation_id":"0123456789abcdef0123456789abcdef",)"
        R"("key_hash":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)"
        R"("operation":"chat_archive","plan":{"account":"main","archived":true,"chat":{)"
        R"("id":-1001,"is_bot":false,"title":"Project","type":"supergroup",)"
        R"("usernames":["project"]},"operation":"chat_archive","tdlib_request":"addChatToList"},)"
        R"("request_fingerprint":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
        R"("reserved_terminal_bytes":32768,"spool":null,"state":"pending",)"
        R"("temporary_message_ids":[],"terminal":null}],"schema_version":1})";
    CHECK(inserted.snapshot.canonical_bytes == golden);
    std::ifstream input(tree.final_path(), std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    CHECK(bytes == inserted.snapshot.canonical_bytes);
    CHECK_FALSE(bytes.ends_with('\n'));
    CHECK(
        (std::filesystem::status(tree.final_path()).permissions() & std::filesystem::perms::all) ==
        (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));

    CHECK(daemon::IdempotencyStore::lookup(inserted.snapshot, entry.key_hash,
                                           entry.request_fingerprint)
              .status == daemon::IdempotencyLookupStatus::Pending);
    CHECK(daemon::IdempotencyStore::lookup(inserted.snapshot, entry.key_hash, fingerprint('c'))
              .status == daemon::IdempotencyLookupStatus::Conflict);
    CHECK(daemon::IdempotencyStore::lookup(inserted.snapshot, key_hash('c'),
                                           entry.request_fingerprint)
              .status == daemon::IdempotencyLookupStatus::Miss);
    const auto pins = daemon::IdempotencyStore::pins(inserted.snapshot);
    REQUIRE(pins.pins.size() == 1);
    CHECK(pins.pins.front().audit_generation == 99);
}

TEST_CASE("idempotency store rejects malformed duplicate and noncanonical final bytes",
          "[idempotency-store][canonical][errors]") {
    std::vector<std::pair<std::string, daemon::AccountAuditDurabilityReason>> cases{
        {"", daemon::AccountAuditDurabilityReason::ParseError},
        {"{", daemon::AccountAuditDurabilityReason::ParseError},
        {R"(/*comment*/{"entries":[],"schema_version":1})",
         daemon::AccountAuditDurabilityReason::ParseError},
        {R"({"entries":[],"schema_version":1} trailing)",
         daemon::AccountAuditDurabilityReason::ParseError},
        {R"({"entries":[],"entries":[],"schema_version":1})",
         daemon::AccountAuditDurabilityReason::ParseError},
        {R"({"schema_version":1,"entries":[]})", daemon::AccountAuditDurabilityReason::SchemaError},
        {R"({"entries":[],"schema_version":2})", daemon::AccountAuditDurabilityReason::SchemaError},
        {"{\"entries\":[],\"schema_version\":1}\n",
         daemon::AccountAuditDurabilityReason::SchemaError},
    };
    std::string invalid_utf8 = R"({"entries":[],"schema_version":")";
    invalid_utf8.push_back(static_cast<char>(0xff));
    invalid_utf8 += R"("})";
    cases.emplace_back(std::move(invalid_utf8), daemon::AccountAuditDurabilityReason::ParseError);
    for (const auto& [bytes, reason] : cases) {
        StoreTree tree;
        tree.write_final(bytes);
        auto guard = tree.guard();
        const auto inspection = tree.store().inspect(guard);
        INFO(bytes);
        CHECK(inspection.status == daemon::IdempotencyInspectionStatus::Unavailable);
        CHECK(inspection.failure.reason == reason);
        CHECK(inspection.failure.path == tree.final_path());
    }

    const daemon::IdempotencySnapshot snapshot{{pending()}, {}};
    auto serialized = daemon::serialize_idempotency_snapshot(snapshot, "main",
                                                             "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<std::string>(serialized));
    auto nested_duplicate = std::get<std::string>(serialized);
    constexpr std::string_view account_field = R"("account":"main")";
    const auto account = nested_duplicate.find(account_field);
    REQUIRE(account != std::string::npos);
    nested_duplicate.insert(account + account_field.size(), R"(,"account":"main")");
    StoreTree duplicate_tree;
    duplicate_tree.write_final(nested_duplicate);
    auto duplicate_guard = duplicate_tree.guard();
    CHECK(duplicate_tree.store().inspect(duplicate_guard).failure.reason ==
          daemon::AccountAuditDurabilityReason::ParseError);

    auto escaped = std::get<std::string>(serialized);
    const auto escaped_account = escaped.find(account_field);
    REQUIRE(escaped_account != std::string::npos);
    escaped.replace(escaped_account, account_field.size(), R"("account":"m\u0061in")");
    StoreTree escaped_tree;
    escaped_tree.write_final(escaped);
    auto escaped_guard = escaped_tree.guard();
    CHECK(escaped_tree.store().inspect(escaped_guard).failure.reason ==
          daemon::AccountAuditDurabilityReason::SchemaError);
}

TEST_CASE("idempotency stale temp is nonauthoritative and requires durable cleanup",
          "[idempotency-store][temp][durability]") {
    StoreTree tree;
    tree.write_temp(R"({"entries":[],"schema_version":999})");
    auto guard = tree.guard();
    const auto inspection = tree.store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    CHECK(inspection.stale_temp_present);
    CHECK(inspection.snapshot.entries.empty());
    const auto cleanup = tree.store().cleanup_stale_temp(guard);
    CHECK(cleanup.status == daemon::IdempotencyWriteStatus::Applied);
    CHECK_FALSE(std::filesystem::exists(tree.temp_path()));
    CHECK_FALSE(std::filesystem::exists(tree.final_path()));
}

TEST_CASE("idempotency wall sample validation occurs after prior gate reconciliation",
          "[idempotency-store][expiry][clock][precedence]") {
    SECTION("final parse failure precedes the invalid sample") {
        StoreTree tree;
        tree.write_final("{");
        tree.write_temp();
        auto guard = tree.guard();
        const auto result =
            tree.foundation().run_core_gate(guard, daemon::kIdempotencyMaximumUnixSeconds + 1,
                                            [] { return "2026-08-20T12:00:03Z"; });
        CHECK(result.status == daemon::IdempotencyCoreGateStatus::StoreUnavailable);
        CHECK(result.store_failure.reason == daemon::AccountAuditDurabilityReason::ParseError);
        CHECK(std::filesystem::exists(tree.temp_path()));
    }
    SECTION("safe stale temp cleanup precedes the invalid sample") {
        StoreTree tree;
        tree.write_temp();
        auto guard = tree.guard();
        const auto result =
            tree.foundation().run_core_gate(guard, daemon::kIdempotencyMaximumUnixSeconds + 1,
                                            [] { return "2026-08-20T12:00:03Z"; });
        CHECK(result.status == daemon::IdempotencyCoreGateStatus::StoreUnavailable);
        CHECK(result.store_failure.reason == daemon::AccountAuditDurabilityReason::SchemaError);
        CHECK_FALSE(std::filesystem::exists(tree.temp_path()));
    }
}

TEST_CASE("idempotency unexpected incumbent is selected before insertion capacity",
          "[idempotency-store][insert][precedence]") {
    auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<int> capacity_calls = 0;
    hooks->at_stage = [&](daemon::IdempotencyStoreStage stage) {
        if (stage == daemon::IdempotencyStoreStage::BeforeCapacity) {
            capacity_calls.fetch_add(1, std::memory_order_relaxed);
        }
    };
    StoreTree tree(hooks);
    auto guard = tree.guard();
    REQUIRE(tree.store().insert_if_absent(pending(), guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    CHECK(capacity_calls.load(std::memory_order_relaxed) == 1);
    const auto loss =
        tree.store().insert_if_absent(pending('b', 'c', "fedcba9876543210fedcba9876543210"), guard);
    REQUIRE(loss.status == daemon::IdempotencyInsertStatus::UnexpectedIncumbent);
    REQUIRE(loss.incumbent);
    CHECK(loss.incumbent->invocation_id == "0123456789abcdef0123456789abcdef");
    CHECK(capacity_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("idempotency unexpected incumbent closure durably records INTERNAL and preserves owner",
          "[idempotency-store][unexpected-incumbent][audit][durability-fatal]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view incumbent_invocation = "0123456789abcdef0123456789abcdef";
    const auto incumbent_receipt =
        append_intent(tree, guard, audit_intent('b', 'a', std::string(incumbent_invocation)));
    auto incumbent_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(incumbent_invocation), incumbent_receipt.audit_generation, 1'700'000'000,
         archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(incumbent_result));
    const auto incumbent = std::get<daemon::IdempotencyEntry>(std::move(incumbent_result));
    REQUIRE(tree.store().insert_if_absent(incumbent, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint(tree, guard, incumbent_invocation, 1,
                      daemon::AccountAuditStage::IdempotencyPending,
                      {{"key_hash", incumbent.key_hash.value()},
                       {"request_fingerprint", incumbent.request_fingerprint.value()},
                       {"expires_at", incumbent.expires_at},
                       {"reserved_terminal_bytes", incumbent.reserved_terminal_bytes}});
    append_checkpoint(tree, guard, incumbent_invocation, 2,
                      daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});
    append_checkpoint(tree, guard, incumbent_invocation, 3,
                      daemon::AccountAuditStage::MutationConfirmed,
                      {{"terminal", archive_terminal()}});
    append_outcome(tree, guard, incumbent_invocation,
                   {daemon::AccountAuditStage::IdempotencyPending,
                    daemon::AccountAuditStage::DispatchStarted,
                    daemon::AccountAuditStage::MutationConfirmed},
                   daemon::AccountAuditMutationState::Confirmed, archive_terminal());
    REQUIRE(tree.store()
                .complete(incumbent.key_hash, incumbent.invocation_id, archive_terminal(), guard)
                .status == daemon::IdempotencyWriteStatus::Applied);

    constexpr std::string_view loser_invocation = "fedcba9876543210fedcba9876543210";
    const auto loser_receipt =
        append_intent(tree, guard, audit_intent('b', 'a', std::string(loser_invocation)));
    auto loser_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(loser_invocation), loser_receipt.audit_generation, 1'700'000'001,
         archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(loser_result));
    REQUIRE(tree.store()
                .insert_if_absent(std::get<daemon::IdempotencyEntry>(loser_result), guard)
                .status == daemon::IdempotencyInsertStatus::UnexpectedIncumbent);
    const auto closure = tree.foundation().close_unexpected_incumbent(
        loser_receipt, guard, [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(closure.status == daemon::IdempotencyUnexpectedIncumbentClosureStatus::DurableFatal);
    REQUIRE(closure.terminal);
    CHECK(*closure.terminal == daemon::unexpected_idempotency_incumbent_terminal(
                                   daemon::AccountAuditOperation::ChatArchive));
    const auto store = tree.store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(store.snapshot.entries.size() == 1);
    CHECK(store.snapshot.entries.front().invocation_id == incumbent_invocation);
    CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    CHECK(tree.audit().inspect(guard).status == daemon::AccountAuditInspectionStatus::Clean);
}

TEST_CASE("idempotency unexpected incumbent closure emits nothing when outcome sync fails",
          "[idempotency-store][unexpected-incumbent][audit-fatal][fault]") {
    auto audit_hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
    std::atomic<bool> fail_sync = false;
    audit_hooks->should_fail = [&](daemon::AccountAuditFault fault) {
        return fail_sync.load(std::memory_order_relaxed) &&
               fault == daemon::AccountAuditFault::FileSync;
    };
    StoreTree tree({}, audit_hooks);
    auto guard = tree.guard();
    const auto receipt =
        append_intent(tree, guard, audit_intent('b', 'a', "0123456789abcdef0123456789abcdef"));
    fail_sync.store(true, std::memory_order_relaxed);
    const auto closure = tree.foundation().close_unexpected_incumbent(
        receipt, guard, [] { return "2026-08-20T12:00:03Z"; });
    CHECK(closure.status == daemon::IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal);
    CHECK_FALSE(closure.terminal);
    CHECK(closure.audit_failure.reason == daemon::AccountAuditDurabilityReason::SyncFailed);
}

TEST_CASE("idempotency completion clears mutable progress and expiry is equality exact",
          "[idempotency-store][completion][expiry]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto entry = pending();
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    const auto completed =
        tree.store().complete(entry.key_hash, entry.invocation_id, archive_terminal(), guard);
    REQUIRE(completed.status == daemon::IdempotencyWriteStatus::Applied);
    REQUIRE(completed.snapshot.entries.size() == 1);
    const auto& stored = completed.snapshot.entries.front();
    CHECK(stored.state == daemon::IdempotencyEntryState::Completed);
    CHECK(stored.reserved_terminal_bytes == 0);
    CHECK(stored.temporary_message_ids.empty());
    CHECK(stored.forward_progress.empty());
    REQUIRE(stored.terminal);

    const auto before = tree.store().sweep_expired(entry.expires_at - 1, guard);
    CHECK(before.status == daemon::IdempotencyWriteStatus::Unchanged);
    CHECK(before.snapshot.entries.size() == 1);
    const auto equality = tree.store().sweep_expired(entry.expires_at, guard);
    CHECK(equality.status == daemon::IdempotencyWriteStatus::Applied);
    CHECK(equality.snapshot.entries.empty());
    REQUIRE(equality.removed.size() == 1);
}

TEST_CASE("idempotency store APIs reject raw string key misuse at compile time",
          "[idempotency-store][api][secrecy]") {
    static_assert(!std::is_constructible_v<daemon::IdempotencyKeyHash, std::string>);
    static_assert(
        !std::is_constructible_v<daemon::IdempotencyRequestFingerprint, std::string_view>);
    CHECK_FALSE(daemon::parse_idempotency_key_hash("raw-key"));
    CHECK_FALSE(daemon::parse_idempotency_request_fingerprint(std::string(kFingerprint) + "x"));
}

TEST_CASE("idempotency core gate leaves a clean missing store absent",
          "[idempotency-store][reconciliation][zero-write]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'000,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    CHECK(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(result.snapshot.entries.empty());
    CHECK_FALSE(std::filesystem::exists(tree.final_path()));
    CHECK_FALSE(std::filesystem::exists(tree.temp_path()));
}

TEST_CASE("two initial misses serialize to one commit group and one completed incumbent",
          "[idempotency-store][epoch][concurrency][dual-miss]") {
    auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<int> capacity_calls = 0;
    hooks->at_stage = [&](daemon::IdempotencyStoreStage stage) {
        if (stage == daemon::IdempotencyStoreStage::BeforeCapacity) {
            capacity_calls.fetch_add(1, std::memory_order_relaxed);
        }
    };
    StoreTree tree(hooks);
    const auto hash = key_hash('b');
    const auto request_fingerprint = fingerprint('a');
    const auto initial_lookup = [&] {
        auto guard = tree.guard();
        const auto gate = tree.foundation().run_core_gate(guard, 1'700'000'000,
                                                          [] { return "2026-08-20T12:00:03Z"; });
        if (gate.status != daemon::IdempotencyCoreGateStatus::Clean) {
            return daemon::IdempotencyLookupStatus::Conflict;
        }
        return daemon::IdempotencyStore::lookup(gate.snapshot, hash, request_fingerprint).status;
    };
    auto first_initial = std::async(std::launch::async, initial_lookup);
    auto second_initial = std::async(std::launch::async, initial_lookup);
    CHECK(first_initial.get() == daemon::IdempotencyLookupStatus::Miss);
    CHECK(second_initial.get() == daemon::IdempotencyLookupStatus::Miss);
    CHECK(capacity_calls.load(std::memory_order_relaxed) == 0);

    {
        auto guard = tree.guard();
        const auto gate = tree.foundation().run_core_gate(guard, 1'700'000'000,
                                                          [] { return "2026-08-20T12:00:03Z"; });
        REQUIRE(gate.status == daemon::IdempotencyCoreGateStatus::Clean);
        REQUIRE(daemon::IdempotencyStore::lookup(gate.snapshot, hash, request_fingerprint).status ==
                daemon::IdempotencyLookupStatus::Miss);
        static_cast<void>(complete_archive_invocation(tree, guard));
    }
    {
        auto guard = tree.guard();
        const auto gate = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                          [] { return "2026-08-20T12:00:04Z"; });
        REQUIRE(gate.status == daemon::IdempotencyCoreGateStatus::Clean);
        CHECK(daemon::IdempotencyStore::lookup(gate.snapshot, hash, request_fingerprint).status ==
              daemon::IdempotencyLookupStatus::Completed);
        CHECK(tree.audit().inspect(guard).status == daemon::AccountAuditInspectionStatus::Clean);
    }
    CHECK(capacity_calls.load(std::memory_order_relaxed) == 1);
    std::ifstream audit_input(tree.state() + "/audit.log", std::ios::binary);
    const std::string audit_bytes{std::istreambuf_iterator<char>(audit_input), {}};
    std::size_t intents = 0;
    std::size_t offset = 0;
    while ((offset = audit_bytes.find(R"("phase":"intent")", offset)) != std::string::npos) {
        ++intents;
        offset += 16;
    }
    CHECK(intents == 1);
}

TEST_CASE("idempotency core gate repairs audit-ahead completed state before lookup",
          "[idempotency-store][reconciliation][audit-ahead]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    const auto intent = audit_intent('b', 'a', std::string(invocation));
    const auto receipt = append_intent(tree, guard, intent);
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(invocation), receipt.audit_generation, 1'700'000'000, archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint(tree, guard, invocation, 1, daemon::AccountAuditStage::IdempotencyPending,
                      {{"key_hash", entry.key_hash.value()},
                       {"request_fingerprint", entry.request_fingerprint.value()},
                       {"expires_at", entry.expires_at},
                       {"reserved_terminal_bytes", entry.reserved_terminal_bytes}});
    append_checkpoint(tree, guard, invocation, 2, daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});
    append_checkpoint(tree, guard, invocation, 3, daemon::AccountAuditStage::MutationConfirmed,
                      {{"terminal", archive_terminal()}});
    append_outcome(tree, guard, invocation,
                   {daemon::AccountAuditStage::IdempotencyPending,
                    daemon::AccountAuditStage::DispatchStarted,
                    daemon::AccountAuditStage::MutationConfirmed},
                   daemon::AccountAuditMutationState::Confirmed, archive_terminal());

    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(result.snapshot.entries.size() == 1);
    CHECK(result.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    REQUIRE(result.snapshot.entries.front().terminal);
    CHECK(*result.snapshot.entries.front().terminal == archive_terminal());
}

TEST_CASE("idempotency core gate advances store lag to the latest durable forward vector",
          "[idempotency-store][reconciliation][audit-ahead][forward-progress]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    const auto receipt = append_intent(tree, guard, forward_audit_intent(std::string(invocation)));
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('d'), fingerprint('e'), daemon::AccountAuditOperation::MsgForward,
         std::string(invocation), receipt.audit_generation, 1'700'000'000, forward_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::MsgForward, invocation, 1,
                          daemon::AccountAuditStage::IdempotencyPending,
                          {{"key_hash", entry.key_hash.value()},
                           {"request_fingerprint", entry.request_fingerprint.value()},
                           {"expires_at", entry.expires_at},
                           {"reserved_terminal_bytes", entry.reserved_terminal_bytes}});
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::MsgForward, invocation, 2,
                          daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "forwardMessages"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::MsgForward, invocation, 3,
                          daemon::AccountAuditStage::TemporaryIdsObserved,
                          {{"temporary_message_ids", json::array({101, 102})}});
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::MsgForward, invocation, 4,
                          daemon::AccountAuditStage::ForwardProgress,
                          {{"items", pending_forward_progress()}});
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::MsgForward, invocation, 5,
                          daemon::AccountAuditStage::ForwardProgress,
                          {{"items", partial_forward_progress()}});
    const std::vector stages{daemon::AccountAuditStage::IdempotencyPending,
                             daemon::AccountAuditStage::DispatchStarted,
                             daemon::AccountAuditStage::TemporaryIdsObserved,
                             daemon::AccountAuditStage::ForwardProgress};
    const auto terminal =
        incomplete_terminal(tree, daemon::AccountAuditMutationState::Confirmed, stages);
    append_outcome_for(tree, guard, daemon::AccountAuditOperation::MsgForward, invocation, stages,
                       daemon::AccountAuditMutationState::Confirmed, terminal);

    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(result.snapshot.entries.size() == 1);
    const auto& stored = result.snapshot.entries.front();
    CHECK(stored.state == daemon::IdempotencyEntryState::Pending);
    CHECK(stored.temporary_message_ids == json::array({101, 102}));
    CHECK(stored.forward_progress == partial_forward_progress());
    CHECK_FALSE(stored.terminal);
}

TEST_CASE("idempotency core gate emits the durable prior terminal for ambiguous dispatch",
          "[idempotency-store][reconciliation][audit-incomplete][dispatch]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    const auto receipt =
        append_intent(tree, guard, audit_intent('b', 'a', std::string(invocation)));
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(invocation), receipt.audit_generation, 1'700'000'000, archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint(tree, guard, invocation, 1, daemon::AccountAuditStage::IdempotencyPending,
                      {{"key_hash", entry.key_hash.value()},
                       {"request_fingerprint", entry.request_fingerprint.value()},
                       {"expires_at", entry.expires_at},
                       {"reserved_terminal_bytes", entry.reserved_terminal_bytes}});
    append_checkpoint(tree, guard, invocation, 2, daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});

    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::AuditIncomplete);
    const std::vector stages{daemon::AccountAuditStage::IdempotencyPending,
                             daemon::AccountAuditStage::DispatchStarted};
    REQUIRE(result.terminal);
    CHECK(*result.terminal ==
          incomplete_terminal(tree, daemon::AccountAuditMutationState::Possible, stages));
    const auto inspection = tree.store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(inspection.snapshot.entries.size() == 1);
    CHECK(inspection.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
    CHECK(tree.audit().inspect(guard).status == daemon::AccountAuditInspectionStatus::Clean);
}

TEST_CASE("unexpired dispatch-unknown completed audit requires its pending store entry",
          "[idempotency-store][reconciliation][audit-store][expiry][contradiction]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    const auto receipt =
        append_intent(tree, guard, audit_intent('b', 'a', std::string(invocation)));
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(invocation), receipt.audit_generation, 1'700'000'000, archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint(tree, guard, invocation, 1, daemon::AccountAuditStage::IdempotencyPending,
                      {{"key_hash", entry.key_hash.value()},
                       {"request_fingerprint", entry.request_fingerprint.value()},
                       {"expires_at", entry.expires_at},
                       {"reserved_terminal_bytes", entry.reserved_terminal_bytes}});
    append_checkpoint(tree, guard, invocation, 2, daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});
    const std::vector stages{daemon::AccountAuditStage::IdempotencyPending,
                             daemon::AccountAuditStage::DispatchStarted};
    append_outcome(tree, guard, invocation, stages, daemon::AccountAuditMutationState::Possible,
                   incomplete_terminal(tree, daemon::AccountAuditMutationState::Possible, stages));
    REQUIRE(tree.store().remove_owned(entry.key_hash, invocation, guard).status ==
            daemon::IdempotencyWriteStatus::Applied);

    const auto before = tree.foundation().run_core_gate(guard, entry.expires_at - 1,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    CHECK(before.status == daemon::IdempotencyCoreGateStatus::StoreUnavailable);
    CHECK(before.store_failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    const auto equality = tree.foundation().run_core_gate(guard, entry.expires_at,
                                                          [] { return "2026-08-20T12:00:04Z"; });
    CHECK(equality.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(equality.snapshot.entries.empty());
}

TEST_CASE("completed keyed mutation requires its durable idempotency checkpoint",
          "[idempotency-store][reconciliation][audit-store][contradiction]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    static_cast<void>(append_intent(tree, guard, audit_intent('b', 'a', std::string(invocation))));
    append_checkpoint(tree, guard, invocation, 1, daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});
    append_checkpoint(tree, guard, invocation, 2, daemon::AccountAuditStage::MutationConfirmed,
                      {{"terminal", archive_terminal()}});
    append_outcome(
        tree, guard, invocation,
        {daemon::AccountAuditStage::DispatchStarted, daemon::AccountAuditStage::MutationConfirmed},
        daemon::AccountAuditMutationState::Confirmed, archive_terminal());
    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    CHECK(result.status == daemon::IdempotencyCoreGateStatus::StoreUnavailable);
    CHECK(result.store_failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
}

TEST_CASE("idempotency core gate closes the own insert-before-checkpoint crash window",
          "[idempotency-store][reconciliation][crash-cut]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    const auto intent = audit_intent('b', 'a', std::string(invocation));
    const auto receipt = append_intent(tree, guard, intent);
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(invocation), receipt.audit_generation, 1'700'000'000, archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);

    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(result.snapshot.entries.empty());
    const auto audit = tree.audit().inspect(guard);
    CHECK(audit.status == daemon::AccountAuditInspectionStatus::Clean);
}

TEST_CASE("AbsentByPolicy core gate invokes no idempotency store hook or file IO",
          "[idempotency-store][absent-by-policy][zero-io]") {
    auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<int> store_stages = 0;
    hooks->at_stage = [&](daemon::IdempotencyStoreStage) {
        store_stages.fetch_add(1, std::memory_order_relaxed);
    };
    StoreTree tree(hooks);
    tree.write_temp("must remain untouched");
    auto guard = tree.guard();
    const auto result = tree.foundation().run_absent_by_policy_gate(
        guard, 1'700'000'000, [] { return "2026-08-20T12:00:03Z"; });
    CHECK(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(store_stages.load(std::memory_order_relaxed) == 0);
    CHECK(std::filesystem::exists(tree.temp_path()));
    CHECK_FALSE(std::filesystem::exists(tree.final_path()));
}

TEST_CASE("open unexpected-insert group closes none without removing the incumbent",
          "[idempotency-store][reconciliation][unexpected-incumbent]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view incumbent_invocation = "0123456789abcdef0123456789abcdef";
    const auto incumbent_intent = audit_intent('b', 'a', std::string(incumbent_invocation));
    const auto incumbent_receipt = append_intent(tree, guard, incumbent_intent);
    auto incumbent_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(incumbent_invocation), incumbent_receipt.audit_generation, 1'700'000'000,
         archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(incumbent_result));
    const auto incumbent = std::get<daemon::IdempotencyEntry>(std::move(incumbent_result));
    REQUIRE(tree.store().insert_if_absent(incumbent, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    append_checkpoint(tree, guard, incumbent_invocation, 1,
                      daemon::AccountAuditStage::IdempotencyPending,
                      {{"key_hash", incumbent.key_hash.value()},
                       {"request_fingerprint", incumbent.request_fingerprint.value()},
                       {"expires_at", incumbent.expires_at},
                       {"reserved_terminal_bytes", incumbent.reserved_terminal_bytes}});
    append_checkpoint(tree, guard, incumbent_invocation, 2,
                      daemon::AccountAuditStage::DispatchStarted,
                      {{"tdlib_function", "addChatToList"},
                       {"dispatch_token", "11111111111111111111111111111111"},
                       {"client_generation", std::uint64_t{1}}});
    append_checkpoint(tree, guard, incumbent_invocation, 3,
                      daemon::AccountAuditStage::MutationConfirmed,
                      {{"terminal", archive_terminal()}});
    append_outcome(tree, guard, incumbent_invocation,
                   {daemon::AccountAuditStage::IdempotencyPending,
                    daemon::AccountAuditStage::DispatchStarted,
                    daemon::AccountAuditStage::MutationConfirmed},
                   daemon::AccountAuditMutationState::Confirmed, archive_terminal());
    REQUIRE(tree.store()
                .complete(incumbent.key_hash, incumbent.invocation_id, archive_terminal(), guard)
                .status == daemon::IdempotencyWriteStatus::Applied);

    constexpr std::string_view loser_invocation = "fedcba9876543210fedcba9876543210";
    const auto loser_intent = audit_intent('b', 'a', std::string(loser_invocation));
    static_cast<void>(append_intent(tree, guard, loser_intent));
    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(result.snapshot.entries.size() == 1);
    CHECK(result.snapshot.entries.front().invocation_id == incumbent_invocation);
    CHECK(result.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    const auto audit = tree.audit().inspect(guard);
    CHECK(audit.status == daemon::AccountAuditInspectionStatus::Clean);
}

TEST_CASE("open audit rejects a store-ahead completed transition",
          "[idempotency-store][reconciliation][store-ahead]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    const auto intent = audit_intent('b', 'a', std::string(invocation));
    const auto receipt = append_intent(tree, guard, intent);
    auto entry_result = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         std::string(invocation), receipt.audit_generation, 1'700'000'000, archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    REQUIRE(tree.store()
                .complete(entry.key_hash, entry.invocation_id, archive_terminal(), guard)
                .status == daemon::IdempotencyWriteStatus::Applied);
    tree.write_temp("safe but nonauthoritative");

    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    CHECK(result.status == daemon::IdempotencyCoreGateStatus::StoreUnavailable);
    CHECK(result.store_failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    CHECK(result.store_failure.path == tree.final_path());
    CHECK(std::filesystem::exists(tree.temp_path()));
}

TEST_CASE("spool contradiction precedes store and retains a safe stale temp",
          "[idempotency-store][reconciliation][spool][temp][precedence]") {
    StoreTree tree;
    REQUIRE(std::filesystem::create_directory(tree.state() + "/spool"));
    REQUIRE(::chmod((tree.state() + "/spool").c_str(), 0700) == 0);
    REQUIRE(std::filesystem::create_directory(tree.state() + "/spool/not-an-invocation"));
    REQUIRE(::chmod((tree.state() + "/spool/not-an-invocation").c_str(), 0700) == 0);
    tree.write_final("{");
    tree.write_temp("safe but nonauthoritative");
    auto guard = tree.guard();
    const auto result = tree.foundation().run_core_gate(
        guard, daemon::kIdempotencyMaximumUnixSeconds + 1, [] { return "2026-08-20T12:00:03Z"; });
    CHECK(result.status == daemon::IdempotencyCoreGateStatus::AuditIncomplete);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["code"] == "AUDIT_INCOMPLETE");
    CHECK(std::filesystem::exists(tree.temp_path()));
}

TEST_CASE("idempotency store metadata failures follow the public precedence",
          "[idempotency-store][metadata][precedence]") {
    SECTION("final symlink precedes type owner mode and link checks") {
        StoreTree tree;
        REQUIRE(::symlink("elsewhere", tree.final_path().c_str()) == 0);
        auto guard = tree.guard();
        const auto result = tree.store().inspect(guard);
        CHECK(result.failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
        CHECK(result.failure.path == tree.final_path());
    }
    SECTION("wrong final type") {
        StoreTree tree;
        REQUIRE(std::filesystem::create_directory(tree.final_path()));
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongType);
    }
    SECTION("wrong final mode") {
        StoreTree tree;
        tree.write_final(R"({"entries":[],"schema_version":1})");
        REQUIRE(::chmod(tree.final_path().c_str(), 0640) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongMode);
    }
    SECTION("wrong link count") {
        StoreTree tree;
        tree.write_final(R"({"entries":[],"schema_version":1})");
        REQUIRE(::link(tree.final_path().c_str(), (tree.state() + "/other").c_str()) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongLinkCount);
    }
    SECTION("too large final") {
        StoreTree tree;
        tree.write_final("x");
        REQUIRE(::truncate(tree.final_path().c_str(),
                           static_cast<off_t>(daemon::kIdempotencyStoreMaximumBytes + 1)) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::TooLarge);
    }
    SECTION("descriptor replacement") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->mutate_metadata = [](daemon::IdempotencyStoreMetadata target,
                                    struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::FinalDescriptor) {
                ++metadata.st_ino;
            }
        };
        StoreTree tree(hooks);
        tree.write_final(R"({"entries":[],"schema_version":1})");
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::PathInvalid);
    }
    SECTION("final parse failure precedes unsafe temp metadata") {
        StoreTree tree;
        tree.write_final("{");
        tree.write_temp();
        REQUIRE(::chmod(tree.temp_path().c_str(), 0644) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::ParseError);
    }
    SECTION("safe final then unsafe temp") {
        StoreTree tree;
        tree.write_final(R"({"entries":[],"schema_version":1})");
        tree.write_temp();
        REQUIRE(::chmod(tree.temp_path().c_str(), 0644) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongMode);
        CHECK(std::filesystem::exists(tree.temp_path()));
    }
}

TEST_CASE("idempotency atomic rewrite exposes every exact durable failure boundary",
          "[idempotency-store][fault][crash-cut]") {
    const std::vector<
        std::pair<daemon::IdempotencyStoreFault, daemon::AccountAuditDurabilityReason>>
        cases{
            {daemon::IdempotencyStoreFault::Write,
             daemon::AccountAuditDurabilityReason::WriteFailed},
            {daemon::IdempotencyStoreFault::FileSync,
             daemon::AccountAuditDurabilityReason::SyncFailed},
            {daemon::IdempotencyStoreFault::Rename,
             daemon::AccountAuditDurabilityReason::RenameFailed},
            {daemon::IdempotencyStoreFault::DirectorySync,
             daemon::AccountAuditDurabilityReason::DirectorySyncFailed},
        };
    for (const auto& [fault, reason] : cases) {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->should_fail = [fault](daemon::IdempotencyStoreFault observed) {
            return observed == fault;
        };
        StoreTree tree(hooks);
        auto guard = tree.guard();
        const auto result = tree.store().insert_if_absent(pending(), guard);
        INFO(static_cast<int>(fault));
        REQUIRE(result.status == daemon::IdempotencyInsertStatus::Failed);
        CHECK(result.failure.reason == reason);
        CHECK(result.failure.path == tree.final_path());
        if (fault == daemon::IdempotencyStoreFault::DirectorySync) {
            CHECK_FALSE(std::filesystem::exists(tree.temp_path()));
            const auto recovered = tree.store().inspect(guard);
            REQUIRE(recovered.status == daemon::IdempotencyInspectionStatus::Clean);
            CHECK(recovered.snapshot.entries.size() == 1);
        } else {
            CHECK(std::filesystem::exists(tree.temp_path()));
            CHECK_FALSE(std::filesystem::exists(tree.final_path()));
        }
    }
}

TEST_CASE("idempotency restart treats only the canonical name as authority after rewrite cuts",
          "[idempotency-store][fault][crash-cut][restart]") {
    const std::vector<daemon::IdempotencyStoreFault> old_final_faults{
        daemon::IdempotencyStoreFault::Write, daemon::IdempotencyStoreFault::FileSync,
        daemon::IdempotencyStoreFault::Rename};
    for (const auto fault : old_final_faults) {
        std::atomic<bool> armed = false;
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->should_fail = [&, fault](daemon::IdempotencyStoreFault observed) {
            return armed.load(std::memory_order_relaxed) && observed == fault;
        };
        StoreTree tree(hooks);
        auto guard = tree.guard();
        const auto entry = pending();
        REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
                daemon::IdempotencyInsertStatus::Inserted);
        armed.store(true, std::memory_order_relaxed);
        const auto failed =
            tree.store().complete(entry.key_hash, entry.invocation_id, archive_terminal(), guard);
        INFO(static_cast<int>(fault));
        REQUIRE(failed.status == daemon::IdempotencyWriteStatus::Failed);
        const auto crash_image = tree.store().inspect(guard);
        REQUIRE(crash_image.status == daemon::IdempotencyInspectionStatus::Clean);
        CHECK(crash_image.stale_temp_present);
        REQUIRE(crash_image.snapshot.entries.size() == 1);
        CHECK(crash_image.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
        armed.store(false, std::memory_order_relaxed);
        REQUIRE(tree.store().cleanup_stale_temp(guard).status ==
                daemon::IdempotencyWriteStatus::Applied);
        const auto recovered = tree.store().inspect(guard);
        REQUIRE(recovered.status == daemon::IdempotencyInspectionStatus::Clean);
        CHECK_FALSE(recovered.stale_temp_present);
        CHECK(recovered.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
    }

    std::atomic<bool> fail_directory_sync = false;
    auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    hooks->should_fail = [&](daemon::IdempotencyStoreFault observed) {
        return fail_directory_sync.load(std::memory_order_relaxed) &&
               observed == daemon::IdempotencyStoreFault::DirectorySync;
    };
    StoreTree tree(hooks);
    auto guard = tree.guard();
    const auto entry = pending();
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    fail_directory_sync.store(true, std::memory_order_relaxed);
    const auto failed =
        tree.store().complete(entry.key_hash, entry.invocation_id, archive_terminal(), guard);
    REQUIRE(failed.status == daemon::IdempotencyWriteStatus::Failed);
    CHECK(failed.failure.reason == daemon::AccountAuditDurabilityReason::DirectorySyncFailed);
    const auto crash_image = tree.store().inspect(guard);
    REQUIRE(crash_image.status == daemon::IdempotencyInspectionStatus::Clean);
    CHECK_FALSE(crash_image.stale_temp_present);
    REQUIRE(crash_image.snapshot.entries.size() == 1);
    CHECK(crash_image.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
}

TEST_CASE("idempotency stale-temp cleanup maps unlink and root-sync cuts exactly",
          "[idempotency-store][temp][fault]") {
    SECTION("unlink") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->should_fail = [](daemon::IdempotencyStoreFault fault) {
            return fault == daemon::IdempotencyStoreFault::Unlink;
        };
        StoreTree tree(hooks);
        tree.write_temp();
        auto guard = tree.guard();
        const auto result = tree.store().cleanup_stale_temp(guard);
        CHECK(result.failure.reason == daemon::AccountAuditDurabilityReason::RenameFailed);
        CHECK(std::filesystem::exists(tree.temp_path()));
    }
    SECTION("directory fsync after unlink") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->should_fail = [](daemon::IdempotencyStoreFault fault) {
            return fault == daemon::IdempotencyStoreFault::DirectorySync;
        };
        StoreTree tree(hooks);
        tree.write_temp();
        auto guard = tree.guard();
        const auto result = tree.store().cleanup_stale_temp(guard);
        CHECK(result.failure.reason == daemon::AccountAuditDurabilityReason::DirectorySyncFailed);
        CHECK_FALSE(std::filesystem::exists(tree.temp_path()));
    }
}

TEST_CASE("idempotency epoch deadline and cancellation win before store observation",
          "[idempotency-store][epoch][deadline][cancellation][precedence]") {
    auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<int> stages = 0;
    hooks->at_stage = [&](daemon::IdempotencyStoreStage) {
        stages.fetch_add(1, std::memory_order_relaxed);
    };
    StoreTree tree(hooks);
    SECTION("deadline equality") {
        daemon::AccountAuditScanControl control;
        control.deadline = RequestDeadline{RequestClock::now()};
        const auto result = tree.foundation().acquire_epoch(std::move(control));
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        CHECK(std::get<daemon::AccountAuditFailure>(result).interruption ==
              daemon::AccountAuditFailure::Interruption::Deadline);
    }
    SECTION("cancellation") {
        daemon::AccountAuditScanControl control;
        control.cancelled = [] { return true; };
        const auto result = tree.foundation().acquire_epoch(std::move(control));
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        CHECK(std::get<daemon::AccountAuditFailure>(result).interruption ==
              daemon::AccountAuditFailure::Interruption::Cancelled);
    }
    CHECK(stages.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("idempotency epoch wait observes deadline cancellation and successful handoff",
          "[idempotency-store][epoch][mutex][deadline][cancellation][concurrency]") {
    StoreTree tree;
    SECTION("deadline during wait") {
        auto held = tree.guard();
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        auto waiter = std::async(std::launch::async, [&] {
            daemon::AccountAuditScanControl control;
            control.deadline = RequestDeadline{RequestClock::now() + 20ms};
            entered.set_value();
            return tree.foundation().acquire_epoch(std::move(control));
        });
        entered_future.wait();
        const auto result = waiter.get();
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        CHECK(std::get<daemon::AccountAuditFailure>(result).interruption ==
              daemon::AccountAuditFailure::Interruption::Deadline);
    }
    SECTION("cancellation during wait") {
        auto held = tree.guard();
        std::atomic<bool> cancelled = false;
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        auto waiter = std::async(std::launch::async, [&] {
            daemon::AccountAuditScanControl control;
            control.cancelled = [&] { return cancelled.load(std::memory_order_relaxed); };
            entered.set_value();
            return tree.foundation().acquire_epoch(std::move(control));
        });
        entered_future.wait();
        cancelled.store(true, std::memory_order_relaxed);
        const auto result = waiter.get();
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        CHECK(std::get<daemon::AccountAuditFailure>(result).interruption ==
              daemon::AccountAuditFailure::Interruption::Cancelled);
    }
    SECTION("release transfers one valid epoch") {
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        std::future<bool> waiter;
        {
            auto held = tree.guard();
            waiter = std::async(std::launch::async, [&] {
                entered.set_value();
                auto result = tree.foundation().acquire_epoch();
                return result.valid();
            });
            entered_future.wait();
            CHECK(waiter.wait_for(5ms) == std::future_status::timeout);
        }
        CHECK(waiter.get());
    }
}

TEST_CASE("idempotency cancellation during a long final read interrupts before audit scan",
          "[idempotency-store][epoch][cancellation][read][precedence]") {
    daemon::IdempotencySnapshot snapshot;
    for (std::size_t index = 1; index <= 200; ++index) {
        snapshot.entries.push_back(indexed_entry(index, true));
    }
    const auto serialized = daemon::serialize_idempotency_snapshot(
        snapshot, "main", "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<std::string>(serialized));
    REQUIRE(std::get<std::string>(serialized).size() > 65'536);
    std::atomic<bool> cancelled = false;
    std::atomic<int> read_stages = 0;
    auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    hooks->at_stage = [&](daemon::IdempotencyStoreStage stage) {
        if (stage == daemon::IdempotencyStoreStage::DuringFinalRead &&
            read_stages.fetch_add(1, std::memory_order_relaxed) == 0) {
            cancelled.store(true, std::memory_order_relaxed);
        }
    };
    StoreTree tree(hooks);
    tree.write_final(std::get<std::string>(serialized));
    daemon::AccountAuditScanControl control;
    control.cancelled = [&] { return cancelled.load(std::memory_order_relaxed); };
    auto epoch = tree.foundation().acquire_epoch(std::move(control));
    REQUIRE(std::holds_alternative<daemon::AccountAuditCoordinator::Guard>(epoch));
    auto guard = std::get<daemon::AccountAuditCoordinator::Guard>(std::move(epoch));
    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    CHECK(result.status == daemon::IdempotencyCoreGateStatus::Interrupted);
    CHECK(result.store_failure.interruption ==
          daemon::AccountAuditFailure::Interruption::Cancelled);
    CHECK(read_stages.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("idempotency epoch cancellation automatically constrains spool enumeration",
          "[idempotency-store][epoch][cancellation][spool][precedence]") {
    std::atomic<int> store_stages = 0;
    auto store_hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    store_hooks->at_stage = [&](daemon::IdempotencyStoreStage) {
        store_stages.fetch_add(1, std::memory_order_relaxed);
    };
    StoreTree tree(store_hooks);
    REQUIRE(std::filesystem::create_directory(tree.state() + "/spool"));
    REQUIRE(::chmod((tree.state() + "/spool").c_str(), 0700) == 0);
    std::atomic<bool> cancelled = false;
    auto spool_hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
    spool_hooks->at_stage = [&](daemon::FileSpoolStage stage) {
        if (stage == daemon::FileSpoolStage::BeforeRootEnumeration) {
            cancelled.store(true, std::memory_order_relaxed);
        }
    };
    daemon::AccountAuditScanControl control;
    control.cancelled = [&] { return cancelled.load(std::memory_order_relaxed); };
    auto epoch = tree.foundation().acquire_epoch(std::move(control));
    REQUIRE(std::holds_alternative<daemon::AccountAuditCoordinator::Guard>(epoch));
    auto guard = std::get<daemon::AccountAuditCoordinator::Guard>(std::move(epoch));
    const auto result = tree.foundation().run_core_gate(
        guard, 1'700'000'001, [] { return "2026-08-20T12:00:03Z"; }, {}, spool_hooks);
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::SpoolUnavailable);
    REQUIRE(result.spool_failure);
    CHECK(result.spool_failure->kind == daemon::FileSpoolErrorKind::Cancelled);
    CHECK(store_stages.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("idempotency insertion enforces exact count and mutable-headroom capacity",
          "[idempotency-store][quota][capacity]") {
    SECTION("ten thousand completed entries") {
        daemon::IdempotencySnapshot snapshot;
        snapshot.entries.reserve(daemon::kIdempotencyStoreMaximumEntries);
        for (std::size_t index = 0; index < daemon::kIdempotencyStoreMaximumEntries; ++index) {
            snapshot.entries.push_back(indexed_entry(index + 1, true));
        }
        const auto bytes = daemon::serialize_idempotency_snapshot(
            snapshot, "main", "/tmp/accounts/main/idempotency.db");
        REQUIRE(std::holds_alternative<std::string>(bytes));
        StoreTree tree;
        tree.write_final(std::get<std::string>(bytes));
        auto guard = tree.guard();
        const auto inserted = tree.store().insert_if_absent(indexed_entry(10'001), guard);
        CHECK(inserted.status == daemon::IdempotencyInsertStatus::Failed);
        CHECK(inserted.failure.reason == daemon::AccountAuditDurabilityReason::CapacityExhausted);
    }

    SECTION("actual bytes plus reusable pending reservation") {
        daemon::IdempotencySnapshot snapshot;
        std::string last_bytes;
        std::size_t first_rejected = 0;
        for (std::size_t index = 1; index <= 600; ++index) {
            snapshot.entries.push_back(indexed_entry(index));
            const auto candidate = daemon::serialize_idempotency_snapshot(
                snapshot, "main", "/tmp/accounts/main/idempotency.db");
            if (const auto* serialized = std::get_if<std::string>(&candidate)) {
                last_bytes = *serialized;
                continue;
            }
            first_rejected = index;
            snapshot.entries.pop_back();
            break;
        }
        REQUIRE(first_rejected > 1);
        REQUIRE_FALSE(last_bytes.empty());
        StoreTree tree;
        tree.write_final(last_bytes);
        auto guard = tree.guard();
        const auto inserted = tree.store().insert_if_absent(indexed_entry(first_rejected), guard);
        CHECK(inserted.status == daemon::IdempotencyInsertStatus::Failed);
        CHECK(inserted.failure.reason == daemon::AccountAuditDurabilityReason::CapacityExhausted);
        REQUIRE(snapshot.entries.size() >= 2);
        REQUIRE(tree.store()
                    .complete(snapshot.entries[0].key_hash, snapshot.entries[0].invocation_id,
                              archive_terminal(), guard)
                    .status == daemon::IdempotencyWriteStatus::Applied);
        REQUIRE(tree.store()
                    .complete(snapshot.entries[1].key_hash, snapshot.entries[1].invocation_id,
                              archive_terminal(), guard)
                    .status == daemon::IdempotencyWriteStatus::Applied);
        const auto released = tree.store().insert_if_absent(indexed_entry(first_rejected), guard);
        CHECK(released.status == daemon::IdempotencyInsertStatus::Inserted);
    }
}

TEST_CASE("idempotency forward progress consumes one reservation and completion clears it",
          "[idempotency-store][quota][forward-progress][completion]") {
    auto key = daemon::parse_idempotency_key_hash(digest('d'));
    auto request_fingerprint = daemon::parse_idempotency_request_fingerprint(digest('e'));
    REQUIRE(key);
    REQUIRE(request_fingerprint);
    auto entry_result = daemon::make_idempotency_pending_entry(
        {std::move(*key), std::move(*request_fingerprint),
         daemon::AccountAuditOperation::MsgForward, "0123456789abcdef0123456789abcdef", 77,
         1'700'000'000, forward_plan()},
        "main", "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(entry_result));
    const auto entry = std::get<daemon::IdempotencyEntry>(std::move(entry_result));
    CHECK(entry.reserved_terminal_bytes == 4'194'304);
    StoreTree tree;
    auto guard = tree.guard();
    REQUIRE(tree.store().insert_if_absent(entry, guard).status ==
            daemon::IdempotencyInsertStatus::Inserted);
    auto temporary = tree.store().update_temporary_message_ids(entry.key_hash, entry.invocation_id,
                                                               json::array({101, 102}), guard);
    REQUIRE(temporary.status == daemon::IdempotencyWriteStatus::Applied);
    const auto changed_temporary = tree.store().update_temporary_message_ids(
        entry.key_hash, entry.invocation_id, json::array({103, 104}), guard);
    CHECK(changed_temporary.status == daemon::IdempotencyWriteStatus::Failed);
    CHECK(changed_temporary.failure.reason == daemon::AccountAuditDurabilityReason::SchemaError);
    auto pending_progress = tree.store().update_forward_progress(
        entry.key_hash, entry.invocation_id, pending_forward_progress(), guard);
    REQUIRE(pending_progress.status == daemon::IdempotencyWriteStatus::Applied);
    CHECK(pending_progress.snapshot.entries.front().forward_progress == pending_forward_progress());
    auto partial_progress = tree.store().update_forward_progress(
        entry.key_hash, entry.invocation_id, partial_forward_progress(), guard);
    REQUIRE(partial_progress.status == daemon::IdempotencyWriteStatus::Applied);
    const auto regressed_progress = tree.store().update_forward_progress(
        entry.key_hash, entry.invocation_id, pending_forward_progress(), guard);
    CHECK(regressed_progress.status == daemon::IdempotencyWriteStatus::Failed);
    CHECK(regressed_progress.failure.reason == daemon::AccountAuditDurabilityReason::SchemaError);
    auto sent_progress = tree.store().update_forward_progress(entry.key_hash, entry.invocation_id,
                                                              sent_forward_progress(), guard);
    REQUIRE(sent_progress.status == daemon::IdempotencyWriteStatus::Applied);
    auto completed =
        tree.store().complete(entry.key_hash, entry.invocation_id, forward_terminal(), guard);
    REQUIRE(completed.status == daemon::IdempotencyWriteStatus::Applied);
    const auto& stored = completed.snapshot.entries.front();
    CHECK(stored.state == daemon::IdempotencyEntryState::Completed);
    CHECK(stored.reserved_terminal_bytes == 0);
    CHECK(stored.forward_progress.empty());
    CHECK(stored.temporary_message_ids.empty());
    REQUIRE(stored.terminal);
    CHECK(*stored.terminal == forward_terminal());
}

TEST_CASE("idempotency snapshot rejects duplicate invocation and pin ownership",
          "[idempotency-store][schema][uniqueness]") {
    auto first = indexed_entry(1);
    auto second = indexed_entry(2);
    second.invocation_id = first.invocation_id;
    const daemon::IdempotencySnapshot snapshot{{first, second}, {}};
    const auto result = daemon::serialize_idempotency_snapshot(snapshot, "main",
                                                               "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(result));
    CHECK(std::get<daemon::IdempotencyFailure>(result).reason ==
          daemon::AccountAuditDurabilityReason::SchemaError);
}

TEST_CASE("idempotency snapshot rejects every standalone entry relation",
          "[idempotency-store][schema][relations]") {
    using Mutation = std::function<void(daemon::IdempotencyEntry&)>;
    const std::vector<std::pair<std::string, Mutation>> cases{
        {"zero generation", [](auto& entry) { entry.audit_generation = 0; }},
        {"invalid invocation", [](auto& entry) { entry.invocation_id = "not-hex"; }},
        {"unrepresentable created time",
         [](auto& entry) { entry.created_at = daemon::kIdempotencyMaximumUnixSeconds; }},
        {"wrong expiry", [](auto& entry) { ++entry.expires_at; }},
        {"wrong reservation", [](auto& entry) { --entry.reserved_terminal_bytes; }},
        {"pending terminal", [](auto& entry) { entry.terminal = archive_terminal(); }},
        {"pending temporary ids for direct operation",
         [](auto& entry) { entry.temporary_message_ids = json::array({1}); }},
        {"pending forward vector for direct operation",
         [](auto& entry) { entry.forward_progress = pending_forward_progress(); }},
        {"plan account mismatch", [](auto& entry) { entry.plan["account"] = "other"; }},
        {"completed without terminal",
         [](auto& entry) {
             entry.state = daemon::IdempotencyEntryState::Completed;
             entry.reserved_terminal_bytes = 0;
         }},
        {"completed with reservation",
         [](auto& entry) {
             entry.state = daemon::IdempotencyEntryState::Completed;
             entry.terminal = archive_terminal();
         }},
        {"completed with mutable progress",
         [](auto& entry) {
             entry.operation = daemon::AccountAuditOperation::MsgForward;
             entry.plan = forward_plan();
             entry.state = daemon::IdempotencyEntryState::Completed;
             entry.reserved_terminal_bytes = 0;
             entry.temporary_message_ids = json::array({101, 102});
             entry.terminal = forward_terminal();
         }},
    };
    for (const auto& [name, mutate] : cases) {
        auto entry = pending();
        mutate(entry);
        const auto result = daemon::serialize_idempotency_snapshot(
            {{entry}, {}}, "main", "/tmp/accounts/main/idempotency.db");
        INFO(name);
        REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(result));
        CHECK(std::get<daemon::IdempotencyFailure>(result).reason ==
              daemon::AccountAuditDurabilityReason::SchemaError);
    }

    auto first = indexed_entry(1);
    auto second = indexed_entry(2);
    const auto unsorted = daemon::serialize_idempotency_snapshot(
        {{second, first}, {}}, "main", "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(unsorted));
    const auto duplicate_key = daemon::serialize_idempotency_snapshot(
        {{first, first}, {}}, "main", "/tmp/accounts/main/idempotency.db");
    REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(duplicate_key));
}

TEST_CASE("idempotency invalid frozen paths still report a canonical absolute final path",
          "[idempotency-store][path][error-contract]") {
    const auto relative = daemon::IdempotencyStore::create("accounts/main", "main", ::getuid());
    REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(relative));
    const auto& relative_failure = std::get<daemon::IdempotencyFailure>(relative);
    CHECK(relative_failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
    CHECK(std::filesystem::path(relative_failure.path).is_absolute());
    CHECK(std::filesystem::path(relative_failure.path).lexically_normal().string() ==
          relative_failure.path);
    CHECK(std::filesystem::path(relative_failure.path).filename() == "idempotency.db");

    const auto noncanonical =
        daemon::IdempotencyStore::create("/tmp/accounts/../main", "main", ::getuid());
    REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(noncanonical));
    const auto& failure = std::get<daemon::IdempotencyFailure>(noncanonical);
    CHECK(failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
    CHECK(failure.path == "/tmp/main/idempotency.db");

    const StoreTree tree;
    const auto overflow = daemon::make_idempotency_pending_entry(
        {key_hash('b'), fingerprint('a'), daemon::AccountAuditOperation::ChatArchive,
         "0123456789abcdef0123456789abcdef", 1,
         daemon::kIdempotencyMaximumUnixSeconds - daemon::kIdempotencyRetentionSeconds + 1,
         archive_plan()},
        "main", tree.final_path());
    REQUIRE(std::holds_alternative<daemon::IdempotencyFailure>(overflow));
    const auto& overflow_failure = std::get<daemon::IdempotencyFailure>(overflow);
    CHECK(overflow_failure.reason == daemon::AccountAuditDurabilityReason::SchemaError);
    CHECK(overflow_failure.path == tree.final_path());
}

TEST_CASE("predispatch spool recovery syncs cleanup before outcome and store removal",
          "[idempotency-store][reconciliation][spool][ordering][crash-cut]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto state = create_saved_open(tree, guard);
    std::vector<std::string> boundaries;
    auto hooks = std::make_shared<daemon::testing::IdempotencyReconciliationHooks>();
    hooks->after_boundary = [&](std::string_view boundary) { boundaries.emplace_back(boundary); };
    const auto result = tree.foundation().run_core_gate(
        guard, 1'700'000'001, [] { return "2026-08-20T12:00:03Z"; }, {}, {}, hooks);
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(result.snapshot.entries.empty());
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));
    const auto cleanup = std::ranges::find(boundaries, "open_spool_cleanup_synced");
    const auto outcome = std::ranges::find(boundaries, "recovery_outcome_synced");
    const auto transition = std::ranges::find(boundaries, "open_store_transition_synced");
    const auto release = std::ranges::find(boundaries, "open_spool_hold_released");
    REQUIRE(cleanup != boundaries.end());
    REQUIRE(outcome != boundaries.end());
    REQUIRE(transition != boundaries.end());
    REQUIRE(release != boundaries.end());
    CHECK(cleanup < outcome);
    CHECK(outcome < transition);
    CHECK(transition < release);
}

TEST_CASE("durable-proof spool recovery orders outcome store cleanup clear and release",
          "[idempotency-store][reconciliation][spool][proof][ordering]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto state = create_saved_open(tree, guard);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach,
                          state.entry.invocation_id, 3, daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "sendMessage"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_checkpoint_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id, 4,
        daemon::AccountAuditStage::MutationConfirmed, {{"terminal", saved_terminal()}});
    std::vector<std::string> boundaries;
    auto hooks = std::make_shared<daemon::testing::IdempotencyReconciliationHooks>();
    hooks->after_boundary = [&](std::string_view boundary) { boundaries.emplace_back(boundary); };
    const auto result = tree.foundation().run_core_gate(
        guard, 1'700'000'001, [] { return "2026-08-20T12:00:03Z"; }, {}, {}, hooks);
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(result.snapshot.entries.size() == 1);
    CHECK(result.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    CHECK_FALSE(result.snapshot.entries.front().spool);
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));
    const auto outcome = std::ranges::find(boundaries, "recovery_outcome_synced");
    const auto transition = std::ranges::find(boundaries, "open_store_transition_synced");
    const auto cleanup = std::ranges::find(boundaries, "open_spool_cleanup_synced");
    const auto clear = std::ranges::find(boundaries, "open_store_spool_cleared");
    const auto release = std::ranges::find(boundaries, "open_spool_hold_released");
    REQUIRE(outcome != boundaries.end());
    REQUIRE(transition != boundaries.end());
    REQUIRE(cleanup != boundaries.end());
    REQUIRE(clear != boundaries.end());
    REQUIRE(release != boundaries.end());
    CHECK(outcome < transition);
    CHECK(transition < cleanup);
    CHECK(cleanup < clear);
    CHECK(clear < release);
}

TEST_CASE("completed spool cleanup uses a completed-view recovery hold",
          "[idempotency-store][reconciliation][spool][completed-hold]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto state = create_saved_open(tree, guard);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach,
                          state.entry.invocation_id, 3, daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "sendMessage"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_checkpoint_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id, 4,
        daemon::AccountAuditStage::MutationConfirmed, {{"terminal", saved_terminal()}});
    append_outcome_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id,
        {daemon::AccountAuditStage::IdempotencyPending, daemon::AccountAuditStage::SpoolReady,
         daemon::AccountAuditStage::DispatchStarted, daemon::AccountAuditStage::MutationConfirmed},
        daemon::AccountAuditMutationState::Confirmed, saved_terminal());
    REQUIRE(tree.store()
                .complete(state.entry.key_hash, state.entry.invocation_id, saved_terminal(), guard)
                .status == daemon::IdempotencyWriteStatus::Applied);
    const auto result = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(result.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(result.snapshot.entries.size() == 1);
    CHECK_FALSE(result.snapshot.entries.front().spool);
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));
}

TEST_CASE("expired pending spool is store-removed before cleanup at equality",
          "[idempotency-store][reconciliation][spool][expiry][ordering]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto state = create_saved_open(tree, guard);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach,
                          state.entry.invocation_id, 3, daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "sendMessage"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_outcome_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id,
        {daemon::AccountAuditStage::IdempotencyPending, daemon::AccountAuditStage::SpoolReady,
         daemon::AccountAuditStage::DispatchStarted},
        daemon::AccountAuditMutationState::Possible, saved_unknown_terminal(tree));

    const auto before = tree.foundation().run_core_gate(guard, state.entry.expires_at - 1,
                                                        [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(before.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(before.snapshot.entries.size() == 1);
    CHECK(before.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
    CHECK(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));

    std::vector<std::string> boundaries;
    auto hooks = std::make_shared<daemon::testing::IdempotencyReconciliationHooks>();
    hooks->after_boundary = [&](std::string_view boundary) { boundaries.emplace_back(boundary); };
    const auto equality = tree.foundation().run_core_gate(
        guard, state.entry.expires_at, [] { return "2026-08-20T12:00:04Z"; }, {}, {}, hooks);
    REQUIRE(equality.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(equality.snapshot.entries.empty());
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));
    const auto sweep = std::ranges::find(boundaries, "expiry_store_swept");
    const auto cleanup = std::ranges::find(boundaries, "spool_cleanup_synced");
    REQUIRE(sweep != boundaries.end());
    REQUIRE(cleanup != boundaries.end());
    CHECK(sweep < cleanup);
}

TEST_CASE("restart after expired store removal remints the audit spool hold",
          "[idempotency-store][reconciliation][spool][expiry][crash-cut]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto state = create_saved_open(tree, guard);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach,
                          state.entry.invocation_id, 3, daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "sendMessage"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_outcome_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id,
        {daemon::AccountAuditStage::IdempotencyPending, daemon::AccountAuditStage::SpoolReady,
         daemon::AccountAuditStage::DispatchStarted},
        daemon::AccountAuditMutationState::Possible, saved_unknown_terminal(tree));
    const auto swept = tree.store().sweep_expired(state.entry.expires_at, guard);
    REQUIRE(swept.status == daemon::IdempotencyWriteStatus::Applied);
    CHECK(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));

    const auto recovered = tree.foundation().run_core_gate(guard, state.entry.expires_at,
                                                           [] { return "2026-08-20T12:00:04Z"; });
    REQUIRE(recovered.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(recovered.snapshot.entries.empty());
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));
}

TEST_CASE("unkeyed unknown spool expiry uses intent time with equality and rollback",
          "[idempotency-store][reconciliation][spool][unkeyed][expiry]") {
    StoreTree tree;
    auto guard = tree.guard();
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    tree.write_source();
    auto prepared_result = daemon::prepare_spool_source(tree.source_path(), "/");
    REQUIRE(std::holds_alternative<daemon::PreparedSource>(prepared_result));
    auto prepared = std::get<daemon::PreparedSource>(std::move(prepared_result));
    const auto source_snapshot = prepared.snapshot();
    const auto intent = saved_intent(source_snapshot, std::string(invocation), false);
    static_cast<void>(append_intent(tree, guard, intent));
    auto spool_result = daemon::create_spool_file(prepared, tree.state(), invocation, ::getuid());
    REQUIRE(std::holds_alternative<daemon::CreatedSpool>(spool_result));
    const auto spool = std::get<daemon::CreatedSpool>(std::move(spool_result)).reference;
    append_checkpoint_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, invocation, 1,
        daemon::AccountAuditStage::SpoolReady,
        {{"file", file_snapshot_json(spool.file)}, {"relative_path", spool.relative_path}});
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach, invocation, 2,
                          daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "sendMessage"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_outcome_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, invocation,
        {daemon::AccountAuditStage::SpoolReady, daemon::AccountAuditStage::DispatchStarted},
        daemon::AccountAuditMutationState::Possible, saved_unknown_terminal(tree, false));
    constexpr std::uint64_t intent_seconds = 1'787'227'200;
    constexpr std::uint64_t expiry = intent_seconds + daemon::kIdempotencyRetentionSeconds;
    const auto rollback = tree.foundation().run_core_gate(guard, intent_seconds - 1,
                                                          [] { return "2026-08-20T12:00:03Z"; });
    REQUIRE(rollback.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(std::filesystem::exists(tree.state() + "/" + spool.relative_path));
    const auto before =
        tree.foundation().run_core_gate(guard, expiry - 1, [] { return "2026-08-20T12:00:04Z"; });
    REQUIRE(before.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK(std::filesystem::exists(tree.state() + "/" + spool.relative_path));
    const auto equality =
        tree.foundation().run_core_gate(guard, expiry, [] { return "2026-08-20T12:00:05Z"; });
    REQUIRE(equality.status == daemon::IdempotencyCoreGateStatus::Clean);
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + spool.relative_path));
    CHECK_FALSE(std::filesystem::exists(tree.final_path()));
}

TEST_CASE("completed spool cleanup failure retains the store reference and retries missing",
          "[idempotency-store][reconciliation][spool][retry][crash-cut]") {
    StoreTree tree;
    auto guard = tree.guard();
    const auto state = create_saved_open(tree, guard);
    append_checkpoint_for(tree, guard, daemon::AccountAuditOperation::SavedAttach,
                          state.entry.invocation_id, 3, daemon::AccountAuditStage::DispatchStarted,
                          {{"tdlib_function", "sendMessage"},
                           {"dispatch_token", "11111111111111111111111111111111"},
                           {"client_generation", std::uint64_t{1}}});
    append_checkpoint_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id, 4,
        daemon::AccountAuditStage::MutationConfirmed, {{"terminal", saved_terminal()}});
    append_outcome_for(
        tree, guard, daemon::AccountAuditOperation::SavedAttach, state.entry.invocation_id,
        {daemon::AccountAuditStage::IdempotencyPending, daemon::AccountAuditStage::SpoolReady,
         daemon::AccountAuditStage::DispatchStarted, daemon::AccountAuditStage::MutationConfirmed},
        daemon::AccountAuditMutationState::Confirmed, saved_terminal());
    REQUIRE(tree.store()
                .complete(state.entry.key_hash, state.entry.invocation_id, saved_terminal(), guard)
                .status == daemon::IdempotencyWriteStatus::Applied);

    auto spool_hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
    std::atomic<bool> fail_once = true;
    spool_hooks->should_fail = [&](daemon::FileSpoolStage stage) {
        return stage == daemon::FileSpoolStage::BeforeCleanupRootSync &&
               fail_once.exchange(false, std::memory_order_acq_rel);
    };
    const auto failed = tree.foundation().run_core_gate(
        guard, 1'700'000'001, [] { return "2026-08-20T12:00:03Z"; }, {}, spool_hooks);
    REQUIRE(failed.status == daemon::IdempotencyCoreGateStatus::SpoolUnavailable);
    const auto retained = tree.store().inspect(guard);
    REQUIRE(retained.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(retained.snapshot.entries.size() == 1);
    CHECK(retained.snapshot.entries.front().spool == state.spool);

    const auto retried = tree.foundation().run_core_gate(guard, 1'700'000'001,
                                                         [] { return "2026-08-20T12:00:04Z"; });
    REQUIRE(retried.status == daemon::IdempotencyCoreGateStatus::Clean);
    REQUIRE(retried.snapshot.entries.size() == 1);
    CHECK_FALSE(retried.snapshot.entries.front().spool);
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/" + state.spool.relative_path));
}

TEST_CASE("idempotency public unavailable terminal is exact and always names the final path",
          "[idempotency-store][error-contract][secrecy]") {
    StoreTree tree;
    tree.write_final("{");
    tree.write_temp("raw-idempotency-key sentinel");
    auto guard = tree.guard();
    const auto inspection = tree.store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Unavailable);
    const auto terminal = daemon::idempotency_unavailable_terminal(inspection.failure);
    CHECK(terminal ==
          json{{"kind", "error"},
               {"code", "IDEMPOTENCY_UNAVAILABLE"},
               {"message", "idempotency store is unavailable"},
               {"details",
                {{"account", "main"}, {"path", tree.final_path()}, {"reason", "parse_error"}}},
               {"exit_code", 6}});
    const auto bytes = terminal.dump();
    CHECK(bytes.find(".idempotency.db.tmp") == std::string::npos);
    CHECK(bytes.find("raw-idempotency-key") == std::string::npos);
}

TEST_CASE("idempotency classifies owner open read root and lease failures exactly",
          "[idempotency-store][error-contract][metadata]") {
    SECTION("root symlink classification") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->mutate_metadata = [](daemon::IdempotencyStoreMetadata target,
                                    struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::StateEntry) {
                metadata.st_mode = (metadata.st_mode & ~S_IFMT) | S_IFLNK;
            }
        };
        StoreTree tree(hooks);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::PathInvalid);
    }
    SECTION("root wrong type") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->mutate_metadata = [](daemon::IdempotencyStoreMetadata target,
                                    struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::StateEntry) {
                metadata.st_mode = (metadata.st_mode & ~S_IFMT) | S_IFREG;
            }
        };
        StoreTree tree(hooks);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongType);
    }
    SECTION("root owner") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->mutate_metadata = [](daemon::IdempotencyStoreMetadata target,
                                    struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::StateEntry) {
                ++metadata.st_uid;
            }
        };
        StoreTree tree(hooks);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongOwner);
    }
    SECTION("final owner") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->mutate_metadata = [](daemon::IdempotencyStoreMetadata target,
                                    struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::FinalEntry) {
                ++metadata.st_uid;
            }
        };
        StoreTree tree(hooks);
        tree.write_final(R"({"entries":[],"schema_version":1})");
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongOwner);
    }
    SECTION("root mode") {
        StoreTree tree;
        REQUIRE(::chmod(tree.state().c_str(), 0750) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongMode);
    }
    SECTION("open") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->should_fail = [](daemon::IdempotencyStoreFault fault) {
            return fault == daemon::IdempotencyStoreFault::Open;
        };
        StoreTree tree(hooks);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::OpenFailed);
    }
    SECTION("read") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->should_fail = [](daemon::IdempotencyStoreFault fault) {
            return fault == daemon::IdempotencyStoreFault::Read;
        };
        StoreTree tree(hooks);
        tree.write_final(R"({"entries":[],"schema_version":1})");
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::ReadFailed);
    }
    SECTION("premature eof") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->read = [](int, void*, std::size_t) { return ssize_t{0}; };
        StoreTree tree(hooks);
        tree.write_final(R"({"entries":[],"schema_version":1})");
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::ReadFailed);
    }
    SECTION("extra eof byte") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->read = [](int fd, void* buffer, std::size_t count) {
            if (count == 1) {
                *static_cast<char*>(buffer) = 'x';
                return ssize_t{1};
            }
            return ::read(fd, buffer, count);
        };
        StoreTree tree(hooks);
        tree.write_final(R"({"entries":[],"schema_version":1})");
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::ReadFailed);
    }
    SECTION("descriptor size changes after read") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        std::atomic<int> descriptor_observations = 0;
        hooks->mutate_metadata = [&](daemon::IdempotencyStoreMetadata target,
                                     struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::FinalDescriptor &&
                descriptor_observations.fetch_add(1, std::memory_order_relaxed) != 0) {
                ++metadata.st_size;
            }
        };
        StoreTree tree(hooks);
        tree.write_final(R"({"entries":[],"schema_version":1})");
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::ReadFailed);
    }
    SECTION("temp symlink") {
        StoreTree tree;
        REQUIRE(::symlink("elsewhere", tree.temp_path().c_str()) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::PathInvalid);
    }
    SECTION("temp wrong type") {
        StoreTree tree;
        REQUIRE(std::filesystem::create_directory(tree.temp_path()));
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongType);
    }
    SECTION("temp owner") {
        auto hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
        hooks->mutate_metadata = [](daemon::IdempotencyStoreMetadata target,
                                    struct stat& metadata) {
            if (target == daemon::IdempotencyStoreMetadata::TempEntry) {
                ++metadata.st_uid;
            }
        };
        StoreTree tree(hooks);
        tree.write_temp();
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongOwner);
    }
    SECTION("temp link count") {
        StoreTree tree;
        tree.write_temp();
        REQUIRE(::link(tree.temp_path().c_str(), (tree.state() + "/temp-link").c_str()) == 0);
        auto guard = tree.guard();
        CHECK(tree.store().inspect(guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::WrongLinkCount);
    }
    SECTION("wrong account guard") {
        StoreTree first;
        StoreTree second;
        auto wrong_guard = second.guard();
        CHECK(first.store().inspect(wrong_guard).failure.reason ==
              daemon::AccountAuditDurabilityReason::LockFailed);
    }
}
