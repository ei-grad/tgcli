#include "common/daemon_lock.hpp"
#include "daemon/account_audit.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using nlohmann::json;

namespace {

// NOLINTBEGIN(misc-const-correctness): Catch2 fixtures expose mutable filesystem state.

constexpr std::string_view kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
    "dev:1;ino:2;size:3;ctime_ns:4";
constexpr std::string_view kFingerprint =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kKeyHash =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

template <typename T>
concept HasAuditStoreDisposition = requires(const T& value) { value.store_disposition; };

class AuditTree final {
  public:
    AuditTree() {
        std::string pattern = "/tmp/tgcli-account-audit-XXXXXX";
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
        lock_lease_ = daemon_lock::acquire_lifetime(state_ + "/daemon.lock", lock_identity_, error);
        REQUIRE(lock_lease_);
        coordinator_ =
            daemon::AccountAuditCoordinator::create(state_, "main", ::getuid(), lock_lease_, error);
        REQUIRE(coordinator_ != nullptr);
    }

    ~AuditTree() {
        coordinator_.reset();
        lock_lease_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    AuditTree(const AuditTree&) = delete;
    AuditTree& operator=(const AuditTree&) = delete;
    AuditTree(AuditTree&&) = delete;
    AuditTree& operator=(AuditTree&&) = delete;

    [[nodiscard]] const std::string& state() const {
        return state_;
    }
    [[nodiscard]] std::string audit(std::string_view suffix = {}) const {
        return state_ + "/audit.log" + std::string(suffix);
    }
    [[nodiscard]] daemon::AccountAuditCoordinator& coordinator() const {
        return *coordinator_;
    }
    [[nodiscard]] const std::shared_ptr<daemon_lock::LifetimeLease>& lock_lease() const {
        return lock_lease_;
    }

    void write(std::string_view suffix, std::string_view bytes) const {
        std::ofstream output(audit(suffix), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(audit(suffix).c_str(), 0600) == 0);
    }

  private:
    std::string root_;
    std::string state_;
    daemon_lock::Identity lock_identity_;
    std::shared_ptr<daemon_lock::LifetimeLease> lock_lease_;
    std::shared_ptr<daemon::AccountAuditCoordinator> coordinator_;
};

json chat(std::int64_t id = -1001) {
    return {{"id", id},
            {"title", "Project"},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", json::array({"project"})}};
}

std::string tdlib_request(daemon::AccountAuditOperation operation) {
    using O = daemon::AccountAuditOperation;
    switch (operation) {
    case O::Send:
    case O::SavedAttach:
        return "sendMessage";
    case O::MsgEdit:
        return "editMessageText";
    case O::MsgDelete:
        return "deleteMessages";
    case O::MsgForward:
        return "forwardMessages";
    case O::MsgReact:
        return "addMessageReaction";
    case O::MsgPin:
        return "pinChatMessage";
    case O::MsgUnpin:
        return "unpinChatMessage";
    case O::ChatMarkRead:
        return "viewMessages";
    case O::ChatMute:
    case O::ChatUnmute:
        return "setChatNotificationSettings";
    case O::ChatPin:
    case O::ChatUnpin:
        return "toggleChatIsPinned";
    case O::ChatArchive:
    case O::ChatUnarchive:
        return "addChatToList";
    case O::ChatJoin:
        return "joinChat";
    case O::ChatLeave:
        return "leaveChat";
    case O::SessionTerminate:
        return "terminateSession";
    }
    return {};
}

json arguments(daemon::AccountAuditOperation operation) {
    using O = daemon::AccountAuditOperation;
    switch (operation) {
    case O::Send:
        return {{"chat", "@project"},  {"text", "hello"},  {"parse_mode", "plain"},
                {"reply_to", nullptr}, {"topic", nullptr}, {"silent", false},
                {"schedule", nullptr}};
    case O::MsgEdit:
        return {{"chat", "@project"}, {"message_id", 1}, {"text", "edit"}};
    case O::MsgDelete:
        return {{"chat", "@project"}, {"message_ids", json::array({1})}, {"for_all", true}};
    case O::MsgForward:
        return {{"from", "@project"},
                {"to", "@target"},
                {"message_ids", json::array({1})},
                {"drop_author", false}};
    case O::MsgReact:
        return {{"chat", "@project"},
                {"message_id", 1},
                {"reaction", "ok"},
                {"remove", false},
                {"big", false}};
    case O::MsgPin:
    case O::MsgUnpin:
        return {{"chat", "@project"}, {"message_id", 1}};
    case O::ChatMute:
    case O::ChatUnmute:
        return {{"chat", "@project"}, {"duration_seconds", operation == O::ChatMute ? 3'600 : 0}};
    case O::ChatJoin:
        return {{"source", "username"}, {"username", "project"}};
    case O::SavedAttach:
        return {{"message_id", 1}, {"path", "/tmp/input"}, {"caption", ""}};
    case O::SessionTerminate:
        return {{"session_id", "-9223372036854775808"}};
    default:
        return {{"chat", "@project"}};
    }
}

json session_target() {
    return {{"id", "-9223372036854775808"}, {"is_current", false},
            {"is_password_pending", false}, {"is_unconfirmed", false},
            {"device_type", "linux"},       {"application_name", "Telegram"},
            {"application_version", "1"},   {"device_model", "PC"},
            {"platform", "Linux"},          {"system_version", "1"},
            {"last_active_date", nullptr}};
}

json plan(daemon::AccountAuditOperation operation) {
    using O = daemon::AccountAuditOperation;
    json result{{"operation", daemon::account_audit_operation_name(operation)},
                {"account", "main"},
                {"tdlib_request", tdlib_request(operation)}};
    switch (operation) {
    case O::Send:
        result.update({{"chat", chat()},
                       {"text", "hello"},
                       {"parse_mode", "plain"},
                       {"reply_to", nullptr},
                       {"requested_topic", nullptr},
                       {"effective_topic", nullptr},
                       {"silent", false},
                       {"schedule", nullptr},
                       {"observed_server_unix_time", nullptr}});
        break;
    case O::MsgEdit:
        result.update({{"chat", chat()}, {"message_id", 1}, {"text", "edit"}});
        break;
    case O::MsgDelete:
        result.update({{"chat", chat()},
                       {"message_ids", json::array({1})},
                       {"requested_for_all", true},
                       {"effective_for_all", true}});
        break;
    case O::MsgForward:
        result.update({{"from", chat()},
                       {"to", chat(-1002)},
                       {"message_ids", json::array({1})},
                       {"drop_author", false}});
        break;
    case O::MsgReact:
        result.update({{"chat", chat()},
                       {"message_id", 1},
                       {"reaction", "ok"},
                       {"remove", false},
                       {"big", false}});
        break;
    case O::MsgPin:
    case O::MsgUnpin:
        result.update({{"chat", chat()}, {"message_id", 1}, {"pinned", operation == O::MsgPin}});
        break;
    case O::ChatMarkRead:
        result.update({{"chat", chat()}, {"last_message_id", 1}});
        break;
    case O::ChatMute:
    case O::ChatUnmute:
        result.update({{"chat", chat()},
                       {"muted", operation == O::ChatMute},
                       {"duration_seconds", operation == O::ChatMute ? 3'600 : 0}});
        break;
    case O::ChatPin:
    case O::ChatUnpin:
        result.update(
            {{"chat", chat()}, {"chat_list", "main"}, {"pinned", operation == O::ChatPin}});
        break;
    case O::ChatArchive:
    case O::ChatUnarchive:
        result.update({{"chat", chat()}, {"archived", operation == O::ChatArchive}});
        break;
    case O::ChatJoin:
        result.update({{"source", "username"}, {"chat", chat()}, {"invite_link_sha256", nullptr}});
        break;
    case O::ChatLeave:
        result.update({{"chat", chat()}});
        break;
    case O::SavedAttach:
        result.update({{"chat", chat()},
                       {"message_id", 1},
                       {"effective_topic", nullptr},
                       {"caption", ""},
                       {"file",
                        {{"path", "/tmp/input"},
                         {"name", "input"},
                         {"size", std::uint64_t{1}},
                         {"sha256", kFingerprint},
                         {"device", std::uint64_t{1}},
                         {"inode", std::uint64_t{2}},
                         {"mtime_ns", 3},
                         {"ctime_ns", 4}}}});
        break;
    case O::SessionTerminate:
        result.update({{"session", session_target()}});
        break;
    }
    return result;
}

bool destructive(daemon::AccountAuditOperation operation) {
    using O = daemon::AccountAuditOperation;
    return operation == O::MsgDelete || operation == O::ChatLeave ||
           operation == O::SessionTerminate;
}

daemon::AccountAuditIntent make_intent(daemon::AccountAuditOperation operation,
                                       std::string invocation = "0123456789abcdef0123456789abcdef",
                                       std::optional<std::string> key = std::nullopt) {
    std::string error;
    auto value = daemon::make_account_audit_intent(
        {{std::move(invocation), "2026-08-19T12:00:00Z"},
         "main",
         operation,
         arguments(operation),
         plan(operation),
         std::string(kFingerprint),
         std::string(kSnapshot),
         "request",
         destructive(operation) ? std::optional<std::string>{"yes"} : std::nullopt,
         std::move(key),
         100},
        error);
    REQUIRE(value.has_value());
    return std::move(*value);
}

json error_terminal(daemon::AccountAuditOperation operation = daemon::AccountAuditOperation::Send) {
    return {{"kind", "error"},
            {"code", "TIMEOUT"},
            {"message", "request timed out"},
            {"details",
             {{"operation", daemon::account_audit_operation_name(operation)},
              {"phase", "preflight"},
              {"state", "ready"},
              {"outcome", "not_started"},
              {"idempotency", "not_requested"}}},
            {"exit_code", 7}};
}

json message_write(std::int64_t id = 101) {
    return {{"id", id},
            {"chat_id", -1001},
            {"date", "2026-08-19T12:00:01Z"},
            {"sender", {{"type", "user"}, {"id", 42}}},
            {"is_outgoing", true},
            {"topic", nullptr},
            {"type", "text"},
            {"text", "sent"},
            {"scheduled", false}};
}

json result_data(daemon::AccountAuditOperation operation) {
    using O = daemon::AccountAuditOperation;
    switch (operation) {
    case O::Send:
    case O::MsgEdit:
    case O::SavedAttach:
        return message_write();
    case O::MsgDelete:
        return {{"chat_id", -1001},
                {"message_ids", json::array({1})},
                {"for_all", true},
                {"deleted", true}};
    case O::MsgForward:
        return {{"from_chat_id", -1001},
                {"to_chat_id", -1002},
                {"items",
                 json::array(
                     {json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}}})}};
    case O::MsgReact:
        return {{"chat_id", -1001},
                {"message_id", 1},
                {"reaction", "ok"},
                {"removed", false},
                {"big", false}};
    case O::MsgPin:
    case O::MsgUnpin:
        return {{"chat_id", -1001}, {"message_id", 1}, {"pinned", operation == O::MsgPin}};
    case O::ChatMarkRead:
        return {{"chat_id", -1001}, {"last_read_message_id", 1}, {"marked_read", true}};
    case O::ChatMute:
    case O::ChatUnmute:
        return {{"chat_id", -1001},
                {"muted", operation == O::ChatMute},
                {"duration_seconds", operation == O::ChatMute ? 3'600 : 0}};
    case O::ChatPin:
    case O::ChatUnpin:
        return {{"chat_id", -1001}, {"chat_list", "main"}, {"pinned", operation == O::ChatPin}};
    case O::ChatArchive:
    case O::ChatUnarchive:
        return {{"chat_id", -1001}, {"archived", operation == O::ChatArchive}};
    case O::ChatJoin:
        return {{"status", "joined"}, {"chat_id", -1001}};
    case O::ChatLeave:
        return {{"chat_id", -1001}, {"left", true}};
    case O::SessionTerminate:
        return {{"session_id", "-9223372036854775808"}, {"terminated", true}};
    }
    return {};
}

json result_terminal(daemon::AccountAuditOperation operation) {
    return {{"kind", "result"}, {"data", result_data(operation)}};
}

daemon::AccountAuditCheckpoint checkpoint(daemon::AccountAuditOperation operation,
                                          daemon::AccountAuditStage stage, std::uint32_t sequence,
                                          json data) {
    std::string error;
    auto value = daemon::make_account_audit_checkpoint(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:01Z"},
         "main",
         operation,
         sequence,
         stage,
         std::move(data)},
        error);
    REQUIRE(value.has_value());
    return std::move(*value);
}

void append_line(const AuditTree& tree, const json& document, std::string_view suffix = {}) {
    std::ofstream output(tree.audit(suffix), std::ios::binary | std::ios::app);
    REQUIRE(output.good());
    output << document.dump() << '\n';
    output.close();
    REQUIRE(::chmod(tree.audit(suffix).c_str(), 0600) == 0);
}

std::vector<json> logout_records(std::string invocation, bool complete) {
    std::string error;
    const auto logout_plan = proto::make_logout_plan("main", error);
    REQUIRE(logout_plan);
    const daemon::AuditRecordIdentity identity{std::move(invocation), "2026-08-19T11:00:00Z"};
    auto intent = daemon::make_logout_audit_intent(identity, *logout_plan, std::string(kSnapshot),
                                                   daemon::AuthoritySource::Request,
                                                   daemon::ConfirmationSource::Yes, error);
    REQUIRE(intent);
    std::vector<json> records{daemon::serialize(*intent)};
    if (!complete) {
        return records;
    }
    std::vector<daemon::AuditStage> stages{daemon::AuditStage::IntentSynced};
    for (const auto stage :
         {daemon::AuditStage::LogoutSendStarted, daemon::AuditStage::LogoutClosedConfirmed}) {
        auto checkpoint =
            daemon::make_logout_audit_checkpoint(identity, *logout_plan, stage, error);
        REQUIRE(checkpoint);
        records.push_back(daemon::serialize(*checkpoint));
        stages.push_back(stage);
    }
    auto outcome = daemon::make_logout_success_audit_outcome(identity, *logout_plan, stages, error);
    REQUIRE(outcome);
    records.push_back(daemon::serialize(*outcome));
    return records;
}

std::string json_lines(const std::vector<json>& records) {
    std::string bytes;
    for (const auto& record : records) {
        bytes += record.dump();
        bytes.push_back('\n');
    }
    return bytes;
}

std::vector<json> complete_v2_records(std::string invocation) {
    auto intent = make_intent(daemon::AccountAuditOperation::Send, invocation).document();
    std::string error;
    auto outcome =
        daemon::make_account_audit_outcome({{std::move(invocation), "2026-08-19T12:00:02Z"},
                                            "main",
                                            daemon::AccountAuditOperation::Send,
                                            false,
                                            daemon::AccountAuditMutationState::None,
                                            {},
                                            error_terminal()},
                                           error);
    REQUIRE(outcome);
    return {std::move(intent), outcome->document()};
}

std::string hex_invocation(std::uint64_t value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t index = result.size(); index > 0 && value != 0; --index) {
        result[index - 1] = digits[value & 0x0fU];
        value >>= 4U;
    }
    return result;
}

json checkpoint_record(daemon::AccountAuditOperation operation, daemon::AccountAuditStage stage,
                       std::uint32_t sequence, json data, std::string_view invocation) {
    std::string error;
    auto value =
        daemon::make_account_audit_checkpoint({{std::string(invocation), "2026-08-19T12:00:01Z"},
                                               "main",
                                               operation,
                                               sequence,
                                               stage,
                                               std::move(data)},
                                              error);
    INFO(error);
    REQUIRE(value);
    return value->document();
}

json outcome_record(daemon::AccountAuditOperation operation, std::string invocation,
                    daemon::AccountAuditMutationState mutation,
                    std::vector<daemon::AccountAuditStage> stages, json terminal) {
    std::string error;
    auto outcome =
        daemon::make_account_audit_outcome({{std::move(invocation), "2026-08-19T12:00:06Z"},
                                            "main",
                                            operation,
                                            terminal["kind"] == "result",
                                            mutation,
                                            std::move(stages),
                                            std::move(terminal)},
                                           error);
    INFO(error);
    REQUIRE(outcome);
    return outcome->document();
}

json audit_incomplete_terminal(std::string path, daemon::AccountAuditMutationState mutation,
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
              {"path", std::move(path)},
              {"mutation_state", daemon::account_audit_mutation_state_name(mutation)},
              {"completed_stages", std::move(completed)}}},
            {"exit_code", 1}};
}

std::vector<json> complete_saved_attach_records(const std::string& invocation) {
    using S = daemon::AccountAuditStage;
    const auto intent =
        make_intent(daemon::AccountAuditOperation::SavedAttach, invocation, std::string(kKeyHash))
            .document();
    const json pending{{"key_hash", kKeyHash},
                       {"request_fingerprint", kFingerprint},
                       {"expires_at", std::uint64_t{1}},
                       {"reserved_terminal_bytes", std::uint64_t{65'536}}};
    const json spool{{"file", intent["plan"]["file"]},
                     {"relative_path", "spool/" + invocation + "/input"}};
    const json dispatch{{"tdlib_function", "sendMessage"},
                        {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                        {"client_generation", std::uint64_t{1}}};
    const json temporary{{"temporary_message_ids", json::array({-1})}};
    const auto terminal = result_terminal(daemon::AccountAuditOperation::SavedAttach);
    const json proof{{"terminal", terminal}};
    const std::vector<S> stages{S::IdempotencyPending, S::SpoolReady, S::DispatchStarted,
                                S::TemporaryIdsObserved, S::MutationConfirmed};
    return {intent,
            checkpoint_record(daemon::AccountAuditOperation::SavedAttach, S::IdempotencyPending, 1,
                              pending, invocation),
            checkpoint_record(daemon::AccountAuditOperation::SavedAttach, S::SpoolReady, 2, spool,
                              invocation),
            checkpoint_record(daemon::AccountAuditOperation::SavedAttach, S::DispatchStarted, 3,
                              dispatch, invocation),
            checkpoint_record(daemon::AccountAuditOperation::SavedAttach, S::TemporaryIdsObserved,
                              4, temporary, invocation),
            checkpoint_record(daemon::AccountAuditOperation::SavedAttach, S::MutationConfirmed, 5,
                              proof, invocation),
            outcome_record(daemon::AccountAuditOperation::SavedAttach, invocation,
                           daemon::AccountAuditMutationState::Confirmed, stages, terminal)};
}

std::vector<json> complete_forward_records(const std::string& invocation) {
    using S = daemon::AccountAuditStage;
    const auto intent =
        make_intent(daemon::AccountAuditOperation::MsgForward, invocation, std::string(kKeyHash))
            .document();
    const json pending{{"key_hash", kKeyHash},
                       {"request_fingerprint", kFingerprint},
                       {"expires_at", std::uint64_t{1}},
                       {"reserved_terminal_bytes", std::uint64_t{4'194'304}}};
    const json dispatch{{"tdlib_function", "forwardMessages"},
                        {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                        {"client_generation", std::uint64_t{1}}};
    const json temporary{{"temporary_message_ids", json::array({-1})}};
    const json pending_vector{
        {"items", json::array({json{
                      {"source_id", 1}, {"status", "pending"}, {"temporary_message_id", -1}}})}};
    const json sent_vector{
        {"items",
         json::array({json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}}})}};
    const auto terminal = result_terminal(daemon::AccountAuditOperation::MsgForward);
    const json proof{{"terminal", terminal}};
    const std::vector<S> stages{S::IdempotencyPending, S::DispatchStarted, S::TemporaryIdsObserved,
                                S::ForwardProgress, S::MutationConfirmed};
    return {intent,
            checkpoint_record(daemon::AccountAuditOperation::MsgForward, S::IdempotencyPending, 1,
                              pending, invocation),
            checkpoint_record(daemon::AccountAuditOperation::MsgForward, S::DispatchStarted, 2,
                              dispatch, invocation),
            checkpoint_record(daemon::AccountAuditOperation::MsgForward, S::TemporaryIdsObserved, 3,
                              temporary, invocation),
            checkpoint_record(daemon::AccountAuditOperation::MsgForward, S::ForwardProgress, 4,
                              pending_vector, invocation),
            checkpoint_record(daemon::AccountAuditOperation::MsgForward, S::ForwardProgress, 5,
                              sent_vector, invocation),
            checkpoint_record(daemon::AccountAuditOperation::MsgForward, S::MutationConfirmed, 6,
                              proof, invocation),
            outcome_record(daemon::AccountAuditOperation::MsgForward, invocation,
                           daemon::AccountAuditMutationState::Confirmed, stages, terminal)};
}

void create_spool_object(const AuditTree& tree, std::string_view invocation) {
    const auto root = tree.state() + "/spool";
    const auto directory = root + "/" + std::string(invocation);
    REQUIRE(std::filesystem::create_directory(root));
    REQUIRE(::chmod(root.c_str(), 0700) == 0);
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE(::chmod(directory.c_str(), 0700) == 0);
    std::ofstream output(directory + "/input", std::ios::binary);
    REQUIRE(output.good());
    output.put('x');
    output.close();
    REQUIRE(::chmod((directory + "/input").c_str(), 0600) == 0);
}

bool append_intent(daemon::AccountAuditLog& log, const daemon::AccountAuditIntent& intent,
                   const daemon::AccountAuditPinSource& pins,
                   const daemon::AccountAuditCoordinator::Guard& guard,
                   daemon::AccountAuditAppendReceipt& receipt,
                   daemon::AccountAuditFailure& failure) {
    daemon::AccountAuditAppendPermit permit;
    const auto inspection = log.prepare_append(intent, pins, guard, permit);
    if (inspection.status != daemon::AccountAuditInspectionStatus::Clean) {
        failure = inspection.failure;
        if (inspection.status != daemon::AccountAuditInspectionStatus::Unavailable &&
            inspection.status != daemon::AccountAuditInspectionStatus::Interrupted &&
            inspection.status != daemon::AccountAuditInspectionStatus::Contradiction) {
            failure = {daemon::AccountAuditDurabilityReason::Contradiction,
                       "prior audit history is not terminal"};
        }
        return false;
    }
    return log.append_intent(intent, std::move(permit), guard, receipt, failure);
}

daemon::AccountAuditCoordinator::Guard acquire_guard(daemon::AccountAuditCoordinator& coordinator,
                                                     daemon::AccountAuditScanControl control) {
    auto result = coordinator.lock(std::move(control));
    REQUIRE(std::holds_alternative<daemon::AccountAuditCoordinator::Guard>(result));
    return std::get<daemon::AccountAuditCoordinator::Guard>(std::move(result));
}

daemon::AccountAuditDurabilityReason expected_fault_reason(daemon::AccountAuditFault fault) {
    switch (fault) {
    case daemon::AccountAuditFault::Open:
        return daemon::AccountAuditDurabilityReason::OpenFailed;
    case daemon::AccountAuditFault::Read:
        return daemon::AccountAuditDurabilityReason::ReadFailed;
    case daemon::AccountAuditFault::Write:
        return daemon::AccountAuditDurabilityReason::WriteFailed;
    case daemon::AccountAuditFault::FileSync:
        return daemon::AccountAuditDurabilityReason::SyncFailed;
    case daemon::AccountAuditFault::DirectorySync:
        return daemon::AccountAuditDurabilityReason::DirectorySyncFailed;
    case daemon::AccountAuditFault::Unlink:
    case daemon::AccountAuditFault::Rename:
        return daemon::AccountAuditDurabilityReason::RenameFailed;
    }
    return daemon::AccountAuditDurabilityReason::Contradiction;
}

} // namespace

TEST_CASE("account audit limits preserve the accepted equations", "[account-audit][limits]") {
    using namespace daemon::account_audit_limits;
    CHECK(kRequestSourceBytes == 16'842'751);
    CHECK(kMaximumEscapedChatIdentityBytes == 6'295'469);
    CHECK(kMaximumIntentProofBytes == 130'490'195);
    CHECK(kMaximumSessionIntentProofBytes == 48'300'031);
    CHECK(kVectorJsonBytes == 4'198'400);
    CHECK(kMaximumGroupBytes == 562'651'242);
    CHECK(kMaximumNonRotatingSegmentBytes == 461'987'945);
    CHECK(kMaximumAuditBytes == 2'813'256'210ULL);
}

TEST_CASE("account audit intent factories cover the closed operation enum",
          "[account-audit][contract]") {
    using O = daemon::AccountAuditOperation;
    constexpr std::array operations{
        O::Send,      O::MsgEdit,     O::MsgDelete,       O::MsgForward,    O::MsgReact,
        O::MsgPin,    O::MsgUnpin,    O::ChatMarkRead,    O::ChatMute,      O::ChatUnmute,
        O::ChatPin,   O::ChatUnpin,   O::ChatArchive,     O::ChatUnarchive, O::ChatJoin,
        O::ChatLeave, O::SavedAttach, O::SessionTerminate};
    for (const auto operation : operations) {
        INFO(daemon::account_audit_operation_name(operation));
        auto intent = make_intent(operation);
        std::string error;
        CHECK(daemon::validate_account_audit_intent(intent.document(), error));
        CHECK_THAT(intent.document(), tgcli::test::matches_json_schema("audit-intent.schema.json"));
        CHECK(daemon::parse_account_audit_operation(
                  daemon::account_audit_operation_name(operation)) == operation);
        auto extra = intent.document();
        extra["extra"] = true;
        CHECK_FALSE(daemon::validate_account_audit_intent(extra, error));
    }

    auto session = make_intent(O::SessionTerminate).document();
    session["idempotency_key_hash"] = kKeyHash;
    std::string error;
    CHECK_FALSE(daemon::validate_account_audit_intent(session, error));
    session = make_intent(O::SessionTerminate).document();
    session["arguments"]["session_id"] = "9223372036854775808";
    CHECK_FALSE(daemon::validate_account_audit_intent(session, error));

    daemon::AccountAuditIntentInput admitted{
        {"11111111111111111111111111111111", "2026-08-19T12:00:00Z"},
        "main",
        O::Send,
        arguments(O::Send),
        plan(O::Send),
        std::string(kFingerprint),
        std::string(kSnapshot),
        "request",
        std::nullopt,
        std::nullopt,
        daemon::account_audit_limits::kRequestSourceBytes};
    CHECK(daemon::make_account_audit_intent(admitted, error));
    admitted.request_source_bytes = daemon::account_audit_limits::kRequestSourceBytes + 1;
    CHECK_FALSE(daemon::make_account_audit_intent(admitted, error));
}

TEST_CASE("account audit stage automata reject repeats regressions and nonadvancing vectors",
          "[account-audit][contract][stages]") {
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    const auto identity = daemon::AccountAuditRecordIdentity{"0123456789abcdef0123456789abcdef",
                                                             "2026-08-19T12:00:01Z"};
    const auto dispatch = json{{"tdlib_function", "forwardMessages"},
                               {"dispatch_token", "11111111111111111111111111111111"},
                               {"client_generation", std::uint64_t{1}}};
    const auto pending =
        json{{"items",
              json::array(
                  {json{{"source_id", 1}, {"status", "pending"}, {"temporary_message_id", -1}}})}};
    const auto sent = json{
        {"items",
         json::array({json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}}})}};
    std::vector<daemon::AccountAuditCheckpointInput> history{
        {identity, "main", O::MsgForward, 1, S::DispatchStarted, dispatch},
        {identity, "main", O::MsgForward, 2, S::ForwardProgress, pending},
        {identity, "main", O::MsgForward, 3, S::ForwardProgress, sent},
        {identity,
         "main",
         O::MsgForward,
         4,
         S::MutationConfirmed,
         {{"terminal", result_terminal(O::MsgForward)}}},
    };
    std::string error;
    CHECK(daemon::validate_account_audit_stage_history(O::MsgForward, history, error));

    auto invalid = history;
    invalid[2].data = pending;
    CHECK_FALSE(daemon::validate_account_audit_stage_history(O::MsgForward, invalid, error));
    invalid = history;
    invalid[3].checkpoint_sequence = 3;
    CHECK_FALSE(daemon::validate_account_audit_stage_history(O::MsgForward, invalid, error));
    invalid = history;
    invalid.push_back({identity, "main", O::MsgForward, 5, S::DispatchStarted, dispatch});
    CHECK_FALSE(daemon::validate_account_audit_stage_history(O::MsgForward, invalid, error));

    std::vector<daemon::AccountAuditCheckpointInput> session_history{
        {identity,
         "main",
         O::SessionTerminate,
         1,
         S::IdempotencyPending,
         {{"key_hash", kKeyHash},
          {"request_fingerprint", kFingerprint},
          {"expires_at", std::uint64_t{1}},
          {"reserved_terminal_bytes", std::uint64_t{32'768}}}},
    };
    CHECK_FALSE(
        daemon::validate_account_audit_stage_history(O::SessionTerminate, session_history, error));
}

TEST_CASE("account audit checkpoint and outcome factories enforce exact envelopes",
          "[account-audit][contract]") {
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    auto dispatch = checkpoint(O::Send, S::DispatchStarted, 1,
                               {{"tdlib_function", "sendMessage"},
                                {"dispatch_token", "11111111111111111111111111111111"},
                                {"client_generation", std::uint64_t{1}}});
    std::string error;
    CHECK(daemon::validate_account_audit_checkpoint(dispatch.document(), error));
    CHECK_THAT(dispatch.document(),
               tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));
    auto wrong = dispatch.document();
    wrong["data"]["extra"] = true;
    CHECK_FALSE(daemon::validate_account_audit_checkpoint(wrong, error));

    auto outcome = daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         O::Send,
         false,
         daemon::AccountAuditMutationState::Possible,
         {S::DispatchStarted},
         error_terminal()},
        error);
    REQUIRE(outcome);
    CHECK(daemon::validate_account_audit_outcome(outcome->document(), error));
    CHECK_THAT(outcome->document(), tgcli::test::matches_json_schema("audit-outcome.schema.json"));
    wrong = outcome->document();
    wrong["success"] = true;
    CHECK_FALSE(daemon::validate_account_audit_outcome(wrong, error));
}

TEST_CASE("account audit factories cover every legal record branch",
          "[account-audit][contract][matrix]") {
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    constexpr std::array operations{
        O::Send,      O::MsgEdit,     O::MsgDelete,       O::MsgForward,    O::MsgReact,
        O::MsgPin,    O::MsgUnpin,    O::ChatMarkRead,    O::ChatMute,      O::ChatUnmute,
        O::ChatPin,   O::ChatUnpin,   O::ChatArchive,     O::ChatUnarchive, O::ChatJoin,
        O::ChatLeave, O::SavedAttach, O::SessionTerminate};
    for (const auto operation : operations) {
        INFO(daemon::account_audit_operation_name(operation));
        auto dispatch = checkpoint(operation, S::DispatchStarted, 1,
                                   {{"tdlib_function", tdlib_request(operation)},
                                    {"dispatch_token", "11111111111111111111111111111111"},
                                    {"client_generation", std::uint64_t{1}}});
        CHECK_THAT(dispatch.document(),
                   tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));
        auto proof = checkpoint(operation, S::MutationConfirmed, 2,
                                {{"terminal", result_terminal(operation)}});
        CHECK_THAT(proof.document(),
                   tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));

        std::string error;
        auto outcome = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
             "main",
             operation,
             false,
             daemon::AccountAuditMutationState::None,
             {},
             error_terminal(operation)},
            error);
        REQUIRE(outcome);
        CHECK_THAT(outcome->document(),
                   tgcli::test::matches_json_schema("audit-outcome.schema.json"));

        std::vector<S> success_stages;
        if (operation == O::SavedAttach) {
            success_stages.push_back(S::SpoolReady);
        }
        success_stages.push_back(S::DispatchStarted);
        success_stages.push_back(S::MutationConfirmed);
        auto success = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
             "main",
             operation,
             true,
             daemon::AccountAuditMutationState::Confirmed,
             success_stages,
             {{"kind", "result"}, {"data", result_data(operation)}}},
            error);
        REQUIRE(success);
        CHECK_THAT(success->document(),
                   tgcli::test::matches_json_schema("audit-outcome.schema.json"));

        if (operation != O::SessionTerminate) {
            auto pending = checkpoint(operation, S::IdempotencyPending, 1,
                                      {{"key_hash", kKeyHash},
                                       {"request_fingerprint", kFingerprint},
                                       {"expires_at", std::uint64_t{253'402'300'799ULL}},
                                       {"reserved_terminal_bytes", std::uint64_t{32'768}}});
            CHECK_THAT(pending.document(),
                       tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));
        }
    }

    auto spool = checkpoint(O::SavedAttach, S::SpoolReady, 1,
                            {{"file", plan(O::SavedAttach)["file"]},
                             {"relative_path", "spool/0123456789abcdef0123456789abcdef/input"}});
    CHECK_THAT(spool.document(), tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));
    for (const auto operation : {O::Send, O::MsgForward, O::SavedAttach}) {
        auto temporary = checkpoint(operation, S::TemporaryIdsObserved, 1,
                                    {{"temporary_message_ids", json::array({-1})}});
        CHECK_THAT(temporary.document(),
                   tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));
    }
    auto progress = checkpoint(
        O::MsgForward, S::ForwardProgress, 1,
        {{"items", json::array({json{
                       {"source_id", 1}, {"status", "pending"}, {"temporary_message_id", -1}}})}});
    CHECK_THAT(progress.document(),
               tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));

    const auto identity = daemon::AccountAuditRecordIdentity{"0123456789abcdef0123456789abcdef",
                                                             "2026-08-19T12:00:01Z"};
    std::string error;
    const auto dispatch_data = json{{"tdlib_function", "sendMessage"},
                                    {"dispatch_token", "11111111111111111111111111111111"},
                                    {"client_generation", std::uint64_t{1}}};
    const std::vector<daemon::AccountAuditCheckpointInput> mutation_first{
        {identity,
         "main",
         O::Send,
         1,
         S::MutationConfirmed,
         {{"terminal", result_terminal(O::Send)}}}};
    CHECK_FALSE(daemon::validate_account_audit_stage_history(O::Send, mutation_first, error));
    const std::vector<daemon::AccountAuditCheckpointInput> saved_without_spool{
        {identity,
         "main",
         O::SavedAttach,
         1,
         S::DispatchStarted,
         {{"tdlib_function", "sendMessage"},
          {"dispatch_token", "11111111111111111111111111111111"},
          {"client_generation", std::uint64_t{1}}}}};
    CHECK_FALSE(
        daemon::validate_account_audit_stage_history(O::SavedAttach, saved_without_spool, error));
    const std::vector<daemon::AccountAuditCheckpointInput> pending_after_dispatch{
        {identity, "main", O::Send, 1, S::DispatchStarted, dispatch_data},
        {identity,
         "main",
         O::Send,
         2,
         S::IdempotencyPending,
         {{"key_hash", kKeyHash},
          {"request_fingerprint", kFingerprint},
          {"expires_at", std::uint64_t{1}},
          {"reserved_terminal_bytes", std::uint64_t{32'768}}}}};
    CHECK_FALSE(
        daemon::validate_account_audit_stage_history(O::Send, pending_after_dispatch, error));
}

TEST_CASE("account audit recovery freezes sent proof and cleanup ordering",
          "[account-audit][recovery]") {
    daemon::AccountAuditOpenGroup group;
    group.intent = make_intent(daemon::AccountAuditOperation::SavedAttach,
                               "0123456789abcdef0123456789abcdef", std::string(kKeyHash))
                       .document();
    group.keyed = true;
    group.has_spool = true;
    std::string error;
    const daemon::AccountAuditPinSource known = daemon::KnownAccountAuditPins{};
    auto plan =
        daemon::classify_account_audit_recovery(group, "main", "/state/audit.log", known, error);
    REQUIRE(plan);
    CHECK(plan->mutation_state == daemon::AccountAuditMutationState::None);
    CHECK(plan->continue_current_request);
    CHECK(plan->boundaries ==
          std::vector{daemon::AccountAuditRecoveryBoundary::DeleteSpoolAndSyncRoot,
                      daemon::AccountAuditRecoveryBoundary::AppendOutcomeAndSync,
                      daemon::AccountAuditRecoveryBoundary::TransitionStoreAndSync});

    group.dispatch_started = true;
    group.any_forward_sent = false;
    plan = daemon::classify_account_audit_recovery(group, "main", "/state/audit.log", known, error);
    REQUIRE(plan);
    CHECK(plan->mutation_state == daemon::AccountAuditMutationState::Possible);
    CHECK_FALSE(plan->continue_current_request);
    CHECK(plan->retain_store);
    CHECK(plan->retain_spool);

    group.any_forward_sent = true;
    plan = daemon::classify_account_audit_recovery(group, "main", "/state/audit.log", known, error);
    REQUIRE(plan);
    CHECK(plan->mutation_state == daemon::AccountAuditMutationState::Confirmed);
    CHECK(plan->terminal["details"]["mutation_state"] == "confirmed");

    CHECK_FALSE(daemon::classify_account_audit_recovery(
        group, "main", "/state/audit.log", daemon::AbsentAccountAuditPinsByPolicy{}, error));

    daemon::AccountAuditOpenGroup forward;
    forward.intent = make_intent(daemon::AccountAuditOperation::MsgForward).document();
    forward.dispatch_started = true;
    forward.forward_complete = true;
    forward.any_forward_sent = true;
    forward.completed_stages = {daemon::AccountAuditStage::DispatchStarted,
                                daemon::AccountAuditStage::ForwardProgress};
    forward.checkpoints.push_back(
        checkpoint(
            daemon::AccountAuditOperation::MsgForward, daemon::AccountAuditStage::ForwardProgress,
            2,
            {{"items", json::array({json{
                           {"source_id", 1}, {"status", "sent"}, {"message", message_write()}}})}})
            .document());
    plan =
        daemon::classify_account_audit_recovery(forward, "main", "/state/audit.log", known, error);
    REQUIRE(plan);
    CHECK(plan->mutation_state == daemon::AccountAuditMutationState::Confirmed);
    CHECK(plan->terminal["kind"] == "result");
    REQUIRE_FALSE(plan->boundaries.empty());
    CHECK(plan->boundaries.front() ==
          daemon::AccountAuditRecoveryBoundary::AppendMutationProofAndSync);
}

TEST_CASE("account audit streams mixed history and applies recognition precedence",
          "[account-audit][scanner]") {
    AuditTree tree;
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    daemon::AccountAuditAppendReceipt receipt;
    daemon::AccountAuditFailure failure;
    const auto intent = make_intent(daemon::AccountAuditOperation::Send);
    REQUIRE(append_intent(log, intent, daemon::KnownAccountAuditPins{}, guard, receipt, failure));
    CHECK(receipt.audit_generation != 0);
    auto inspection = log.inspect(guard);
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Open);
    REQUIRE(inspection.oldest_open);
    CHECK(inspection.oldest_open->intent == intent.document());

    append_line(tree, json{{"schema_version", 3},
                           {"phase", "checkpoint"},
                           {"invocation_id", "0123456789abcdef0123456789abcdef"}});
    inspection = log.inspect(guard);
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
    CHECK(inspection.terminal);
    CHECK((*inspection.terminal)["details"]["mutation_state"] == "none");

    AuditTree first_unknown;
    first_unknown.write({}, "{\"schema_version\":3,\"phase\":\"checkpoint\"}\n");
    daemon::AccountAuditLog unknown_log(first_unknown.state(), "main", ::getuid());
    auto unknown_guard = first_unknown.coordinator().lock();
    inspection = unknown_log.inspect(unknown_guard);
    CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Unavailable);
    CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
}

TEST_CASE("account audit preserves the M1 adapter and rescans invocation identities",
          "[account-audit][scanner][v1][parity]") {
    const auto invocation = std::string{"fedcba9876543210fedcba9876543210"};
    SECTION("complete and incomplete v1 histories match the M1 public inspector") {
        for (const bool complete : {false, true}) {
            INFO(complete);
            AuditTree tree;
            tree.write({}, json_lines(logout_records(invocation, complete)));
            daemon::LogoutAuditLog legacy(tree.state(), "main", ::getuid());
            const auto legacy_inspection = legacy.inspect();
            daemon::AccountAuditLog mixed(tree.state(), "main", ::getuid());
            auto guard = tree.coordinator().lock();
            const auto mixed_inspection = mixed.inspect(guard);
            if (complete) {
                CHECK(legacy_inspection.status == daemon::LogoutAuditInspectionStatus::Clean);
                CHECK(mixed_inspection.status == daemon::AccountAuditInspectionStatus::Clean);
            } else {
                REQUIRE(legacy_inspection.status ==
                        daemon::LogoutAuditInspectionStatus::Incomplete);
                REQUIRE(mixed_inspection.status ==
                        daemon::AccountAuditInspectionStatus::LegacyOpen);
                REQUIRE(legacy_inspection.incomplete);
                REQUIRE(mixed_inspection.legacy_logout);
                CHECK(mixed_inspection.legacy_logout->invocation_id ==
                      legacy_inspection.incomplete->invocation_id);
                CHECK(mixed_inspection.legacy_logout->plan == legacy_inspection.incomplete->plan);
                CHECK(mixed_inspection.legacy_logout->completed_stages ==
                      legacy_inspection.incomplete->completed_stages);
            }
        }
    }

    SECTION("a prior segment identity cannot be reused by v2 or another command") {
        AuditTree tree;
        tree.write(".1", json_lines(logout_records(invocation, true)));
        tree.write({}, make_intent(daemon::AccountAuditOperation::SessionTerminate, invocation)
                               .document()
                               .dump() +
                           "\n");
        daemon::AccountAuditLog mixed(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = mixed.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
        CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    }

    SECTION("noncanonical v1 remains the legacy path-invalid result") {
        AuditTree tree;
        auto records = logout_records(invocation, false);
        records.front()["extra"] = true;
        tree.write({}, json_lines(records));
        daemon::LogoutAuditLog legacy(tree.state(), "main", ::getuid());
        CHECK(legacy.inspect().status == daemon::LogoutAuditInspectionStatus::Invalid);
        daemon::AccountAuditLog mixed(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = mixed.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Unavailable);
        CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
    }
}

TEST_CASE("account audit partial tails retain the trustworthy open prefix",
          "[account-audit][scanner][partial]") {
    AuditTree tree;
    tree.write({}, make_intent(daemon::AccountAuditOperation::Send).document().dump() +
                       "\n{\"schema_version\":2");
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    const auto inspection = log.inspect(guard);
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
    REQUIRE(inspection.oldest_open);
    CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
}

TEST_CASE("account audit hole-first rotation covers every occupancy mask",
          "[account-audit][rotation]") {
    for (unsigned mask = 0; mask < 16; ++mask) {
        INFO(mask);
        AuditTree tree;
        tree.write({}, json_lines(complete_v2_records("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
        unsigned before = 0;
        for (unsigned slot = 1; slot <= 4; ++slot) {
            if ((mask & (1U << (slot - 1))) != 0U) {
                tree.write("." + std::to_string(slot),
                           json_lines(complete_v2_records(
                               std::string(31, static_cast<char>('0' + slot)) + "b")));
                ++before;
            }
        }
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->rotation_bytes = 1;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        const daemon::AccountAuditPinSource pins = daemon::KnownAccountAuditPins{};
        REQUIRE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send), pins, guard,
                              receipt, failure));
        unsigned after = 0;
        for (unsigned slot = 1; slot <= 4; ++slot) {
            after += std::filesystem::exists(tree.audit("." + std::to_string(slot))) ? 1U : 0U;
        }
        CHECK(after == std::min(4U, before + 1));
    }
}

TEST_CASE("account audit ordinary writes refuse absent pins and preserve pinned inodes",
          "[account-audit][rotation][pins]") {
    AuditTree tree;
    tree.write({}, json_lines(complete_v2_records("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
    for (unsigned slot = 1; slot <= 4; ++slot) {
        tree.write(
            "." + std::to_string(slot),
            json_lines(complete_v2_records(std::string(31, static_cast<char>('0' + slot)) + "a")));
    }
    auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
    hooks->rotation_bytes = 1;
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
    auto guard = tree.coordinator().lock();
    daemon::AccountAuditAppendReceipt receipt;
    daemon::AccountAuditFailure failure;
    CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send),
                              daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt, failure));
    CHECK(failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);

    struct stat pinned {};
    REQUIRE(::stat(tree.audit(".4").c_str(), &pinned) == 0);
    const auto invocation = std::string(31, '4') + "a";
    daemon::KnownAccountAuditPins pins{
        {{static_cast<std::uint64_t>(pinned.st_ino), invocation, std::string(kFingerprint),
          daemon::AccountAuditOperation::Send}}};
    REQUIRE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send), pins, guard,
                          receipt, failure));
    struct stat retained {};
    REQUIRE(::stat(tree.audit(".4").c_str(), &retained) == 0);
    CHECK(retained.st_ino == pinned.st_ino);
}

TEST_CASE("account audit full rotation covers every pin mask",
          "[account-audit][rotation][pins][matrix]") {
    for (unsigned mask = 0; mask < 16; ++mask) {
        INFO(mask);
        AuditTree tree;
        tree.write({}, json_lines(complete_v2_records("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
        std::array<std::uint64_t, 4> inodes{};
        daemon::KnownAccountAuditPins pins;
        for (unsigned slot = 1; slot <= 4; ++slot) {
            const auto invocation = std::string(31, static_cast<char>('0' + slot)) + "c";
            tree.write("." + std::to_string(slot), json_lines(complete_v2_records(invocation)));
            struct stat metadata {};
            REQUIRE(::stat(tree.audit("." + std::to_string(slot)).c_str(), &metadata) == 0);
            inodes.at(slot - 1) = static_cast<std::uint64_t>(metadata.st_ino);
            if ((mask & (1U << (slot - 1))) != 0U) {
                pins.pins.push_back({inodes.at(slot - 1), invocation, std::string(kFingerprint),
                                     daemon::AccountAuditOperation::Send});
            }
        }
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->rotation_bytes = 1;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        const bool appended = append_intent(log, make_intent(daemon::AccountAuditOperation::Send),
                                            pins, guard, receipt, failure);
        if (mask == 15) {
            CHECK_FALSE(appended);
            CHECK(failure.reason == daemon::AccountAuditDurabilityReason::CapacityExhausted);
            continue;
        }
        REQUIRE(appended);
        unsigned evicted_slot = 4;
        while ((mask & (1U << (evicted_slot - 1))) != 0U) {
            --evicted_slot;
        }
        std::set<std::uint64_t> retained;
        for (unsigned slot = 1; slot <= 4; ++slot) {
            struct stat metadata {};
            REQUIRE(::stat(tree.audit("." + std::to_string(slot)).c_str(), &metadata) == 0);
            retained.insert(static_cast<std::uint64_t>(metadata.st_ino));
        }
        CHECK_FALSE(retained.contains(inodes.at(evicted_slot - 1)));
        for (unsigned slot = 1; slot <= 4; ++slot) {
            if ((mask & (1U << (slot - 1))) != 0U) {
                CHECK(retained.contains(inodes.at(slot - 1)));
            }
        }
    }
}

TEST_CASE("account audit rotation crash states resume without a second eviction",
          "[account-audit][rotation][crash]") {
    struct RotationCrash {};
    for (unsigned cut = 1; cut <= 5; ++cut) {
        INFO(cut);
        AuditTree tree;
        tree.write({}, json_lines(complete_v2_records("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
        for (unsigned slot = 1; slot <= 4; ++slot) {
            tree.write("." + std::to_string(slot),
                       json_lines(complete_v2_records(
                           std::string(31, static_cast<char>('0' + slot)) + "d")));
        }
        auto crashing = std::make_shared<daemon::testing::AccountAuditHooks>();
        crashing->rotation_bytes = 1;
        unsigned step = 0;
        crashing->after_rotation_step = [&](std::string_view) {
            if (++step == cut) {
                throw RotationCrash{};
            }
        };
        daemon::AccountAuditLog first(tree.state(), "main", ::getuid(), crashing);
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_THROWS_AS(append_intent(first, make_intent(daemon::AccountAuditOperation::Send),
                                      daemon::KnownAccountAuditPins{}, guard, receipt, failure),
                        RotationCrash);

        auto resumed_hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        resumed_hooks->rotation_bytes = 1;
        std::vector<std::string> resumed_steps;
        resumed_hooks->after_rotation_step = [&](std::string_view value) {
            resumed_steps.emplace_back(value);
        };
        daemon::AccountAuditLog resumed(tree.state(), "main", ::getuid(), resumed_hooks);
        CHECK(resumed.inspect(guard).status == daemon::AccountAuditInspectionStatus::Clean);
        if (cut == 5) {
            CHECK_FALSE(std::filesystem::exists(tree.audit()));
        }
        REQUIRE(append_intent(
            resumed,
            make_intent(daemon::AccountAuditOperation::Send, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"),
            daemon::KnownAccountAuditPins{}, guard, receipt, failure));
        CHECK(std::find(resumed_steps.begin(), resumed_steps.end(), "unlink") ==
              resumed_steps.end());
        CHECK(resumed.inspect(guard).status == daemon::AccountAuditInspectionStatus::Open);
    }
}

TEST_CASE("account audit maps filesystem faults and binds the coordinator account",
          "[account-audit][filesystem][faults][lock]") {
    SECTION("pin provider unavailability prevents even a nonrotating append") {
        AuditTree tree;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(append_intent(
            log, make_intent(daemon::AccountAuditOperation::Send),
            daemon::UnavailableAccountAuditPins{daemon::AccountAuditDurabilityReason::ReadFailed},
            guard, receipt, failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::ReadFailed);
        CHECK_FALSE(std::filesystem::exists(tree.audit()));
    }

    SECTION("a guard from another account state is rejected") {
        AuditTree first;
        AuditTree second;
        daemon::AccountAuditLog log(first.state(), "main", ::getuid());
        auto guard = second.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send),
                                  daemon::KnownAccountAuditPins{}, guard, receipt, failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::LockFailed);
    }

    SECTION("open read write and sync faults keep their exact reason") {
        for (const auto fault :
             {daemon::AccountAuditFault::Open, daemon::AccountAuditFault::Write,
              daemon::AccountAuditFault::FileSync, daemon::AccountAuditFault::DirectorySync}) {
            INFO(static_cast<int>(fault));
            AuditTree tree;
            auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
            hooks->should_fail = [fault,
                                  fired = false](daemon::AccountAuditFault candidate) mutable {
                if (!fired && candidate == fault) {
                    fired = true;
                    return true;
                }
                return false;
            };
            daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
            auto guard = tree.coordinator().lock();
            daemon::AccountAuditAppendReceipt receipt;
            daemon::AccountAuditFailure failure;
            CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send),
                                      daemon::KnownAccountAuditPins{}, guard, receipt, failure));
            CHECK(failure.reason == expected_fault_reason(fault));
        }
    }

    SECTION("verified metadata rejects a hard-link alias") {
        AuditTree tree;
        tree.write({}, json_lines(complete_v2_records("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
        REQUIRE(::link(tree.audit().c_str(), tree.audit(".1").c_str()) == 0);
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Unavailable);
        CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::WrongLinkCount);
        CHECK_FALSE(std::filesystem::exists(tree.state() + "/.audit.lock"));
    }
}

TEST_CASE("account audit requires a live daemon lock lease and serializes guards",
          "[account-audit][lock][concurrency]") {
    AuditTree tree;
    std::atomic<bool> acquired = false;
    std::optional<daemon::AccountAuditCoordinator::Guard> first;
    first.emplace(tree.coordinator().lock());
    auto waiter = std::async(std::launch::async, [&] {
        auto second = tree.coordinator().lock();
        acquired.store(second.valid(), std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(acquired.load(std::memory_order_acquire));
    first.reset();
    REQUIRE(waiter.wait_for(2s) == std::future_status::ready);
    CHECK(acquired.load(std::memory_order_acquire));

    std::string error;
    CHECK_FALSE(daemon::AccountAuditCoordinator::create(tree.state(), "other", ::getuid(),
                                                        tree.lock_lease(), error));
}

TEST_CASE("account audit scanner classifies hostile envelopes without throwing",
          "[account-audit][scanner][regression]") {
    SECTION("duplicate schema version is not positive v2 recognition") {
        AuditTree tree;
        tree.write({}, R"({"schema_version":2,"schema_version":2,"phase":"intent"})"
                       "\n");
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Unavailable);
        CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
    }

    SECTION("wrong typed recognized envelope is an empty-prefix contradiction") {
        AuditTree tree;
        tree.write({}, R"({"schema_version":2,"phase":1,"invocation_id":false})"
                       "\n");
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditInspection inspection;
        CHECK_NOTHROW(inspection = log.inspect(guard));
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
        REQUIRE(inspection.terminal);
        CHECK((*inspection.terminal)["details"]["mutation_state"] == "none");
        CHECK((*inspection.terminal)["details"]["completed_stages"] == json::array());
    }

    SECTION("noninteger version remains unrecognized") {
        AuditTree tree;
        tree.write({}, R"({"schema_version":"2","phase":"intent"})"
                       "\n");
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Unavailable);
        CHECK(inspection.failure.reason == daemon::AccountAuditDurabilityReason::PathInvalid);
    }
}

TEST_CASE("account audit rejects unbounded or secret-bearing stored errors",
          "[account-audit][contract][terminal][secrecy]") {
    std::string error;
    auto terminal = error_terminal();
    terminal["code"] = "UNDECLARED";
    terminal["details"] = {{"idempotency_key", "raw-key-sentinel"},
                           {"invite_link", "raw-invite-sentinel"}};
    terminal["exit_code"] = 99;
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::Send,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         terminal},
        error));

    terminal = error_terminal();
    terminal["details"] = {{"nested", json::array({std::string("\xff", 1)})}};
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::Send,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         terminal},
        error));

    terminal = error_terminal();
    terminal["message"] = "raw-key-sentinel raw-invite-sentinel";
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::Send,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         terminal},
        error));
    auto stored = complete_v2_records("22222222222222222222222222222222").back();
    stored["terminal"] = terminal;
    CHECK_FALSE(tgcli::test::matches_json_schema("audit-outcome.schema.json").match(stored));
}

TEST_CASE("account audit derives one mutation state for every terminal prefix",
          "[account-audit][scanner][mutation][regression]") {
    SECTION("durable mutation proof stays confirmed after corruption") {
        AuditTree tree;
        append_line(tree, make_intent(daemon::AccountAuditOperation::Send).document());
        append_line(tree, checkpoint(daemon::AccountAuditOperation::Send,
                                     daemon::AccountAuditStage::DispatchStarted, 1,
                                     {{"tdlib_function", "sendMessage"},
                                      {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"client_generation", std::uint64_t{1}}})
                              .document());
        append_line(tree,
                    checkpoint(daemon::AccountAuditOperation::Send,
                               daemon::AccountAuditStage::MutationConfirmed, 2,
                               {{"terminal", result_terminal(daemon::AccountAuditOperation::Send)}})
                        .document());
        append_line(tree, json{{"schema_version", 3}, {"phase", "checkpoint"}});
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = log.inspect(guard);
        REQUIRE(inspection.terminal);
        CHECK((*inspection.terminal)["details"]["mutation_state"] == "confirmed");
    }

    SECTION("complete explicit all-failed forward stays none after corruption") {
        AuditTree tree;
        append_line(tree, make_intent(daemon::AccountAuditOperation::MsgForward).document());
        append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                     daemon::AccountAuditStage::DispatchStarted, 1,
                                     {{"tdlib_function", "forwardMessages"},
                                      {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"client_generation", std::uint64_t{1}}})
                              .document());
        append_line(tree,
                    checkpoint(daemon::AccountAuditOperation::MsgForward,
                               daemon::AccountAuditStage::ForwardProgress, 2,
                               {{"items", json::array({json{{"source_id", 1},
                                                            {"status", "failed"},
                                                            {"failure_reason", "upstream_null"},
                                                            {"tdlib_code", nullptr},
                                                            {"retry_after", nullptr}}})}})
                        .document());
        append_line(tree, json{{"schema_version", 3}, {"phase", "checkpoint"}});
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto inspection = log.inspect(guard);
        REQUIRE(inspection.terminal);
        CHECK((*inspection.terminal)["details"]["mutation_state"] == "none");
    }
}

TEST_CASE("account audit enforces operation persistence constraints",
          "[account-audit][contract][operation][regression]") {
    const auto rejected_intent = [](daemon::AccountAuditOperation operation, json args,
                                    json immutable_plan) {
        std::string error;
        return daemon::make_account_audit_intent(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:00Z"},
             "main",
             operation,
             std::move(args),
             std::move(immutable_plan),
             std::string(kFingerprint),
             std::string(kSnapshot),
             "request",
             destructive(operation) ? std::optional<std::string>{"yes"} : std::nullopt,
             std::nullopt,
             100},
            error);
    };

    auto send_args = arguments(daemon::AccountAuditOperation::Send);
    auto send_plan = plan(daemon::AccountAuditOperation::Send);
    send_args["text"] = "";
    send_plan["text"] = "";
    CHECK_FALSE(rejected_intent(daemon::AccountAuditOperation::Send, send_args, send_plan));

    send_args = arguments(daemon::AccountAuditOperation::Send);
    send_plan = plan(daemon::AccountAuditOperation::Send);
    send_args["schedule"] = {{"kind", "at"}, {"send_date", 100}};
    send_plan["schedule"] = send_args["schedule"];
    send_plan["observed_server_unix_time"] = nullptr;
    CHECK_FALSE(rejected_intent(daemon::AccountAuditOperation::Send, send_args, send_plan));

    auto delete_args = arguments(daemon::AccountAuditOperation::MsgDelete);
    auto delete_plan = plan(daemon::AccountAuditOperation::MsgDelete);
    delete_args["message_ids"] = json::array({2, 1});
    delete_plan["message_ids"] = delete_args["message_ids"];
    CHECK_FALSE(
        rejected_intent(daemon::AccountAuditOperation::MsgDelete, delete_args, delete_plan));

    auto mute_args = arguments(daemon::AccountAuditOperation::ChatMute);
    auto mute_plan = plan(daemon::AccountAuditOperation::ChatMute);
    mute_args["duration_seconds"] = 0;
    mute_plan["duration_seconds"] = 0;
    CHECK_FALSE(rejected_intent(daemon::AccountAuditOperation::ChatMute, mute_args, mute_plan));

    auto react_args = arguments(daemon::AccountAuditOperation::MsgReact);
    auto react_plan = plan(daemon::AccountAuditOperation::MsgReact);
    react_args["remove"] = true;
    react_args["big"] = true;
    react_plan["remove"] = true;
    react_plan["big"] = true;
    react_plan["tdlib_request"] = "removeMessageReaction";
    CHECK_FALSE(rejected_intent(daemon::AccountAuditOperation::MsgReact, react_args, react_plan));

    auto saved_plan = plan(daemon::AccountAuditOperation::SavedAttach);
    saved_plan["file"]["name"] = ".";
    CHECK_FALSE(rejected_intent(daemon::AccountAuditOperation::SavedAttach,
                                arguments(daemon::AccountAuditOperation::SavedAttach), saved_plan));

    auto old = make_intent(daemon::AccountAuditOperation::Send).document();
    old["timestamp"] = "1969-12-31T23:59:59Z";
    std::string error;
    CHECK_FALSE(daemon::validate_account_audit_intent(old, error));

    auto forward = checkpoint(daemon::AccountAuditOperation::MsgForward,
                              daemon::AccountAuditStage::ForwardProgress, 1,
                              {{"items", json::array({json{{"source_id", 1},
                                                           {"status", "failed"},
                                                           {"failure_reason", "tdlib_error"},
                                                           {"tdlib_code", 429},
                                                           {"retry_after", 1}}})}})
                       .document();
    forward["data"]["items"][0]["retry_after"] = 0;
    CHECK_FALSE(daemon::validate_account_audit_checkpoint(forward, error));
}

TEST_CASE("account audit v2 accepts signed message ids and the mute default sentinel",
          "[account-audit][contract][schema][message-id][mute]") {
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    constexpr std::int64_t minimum = -9'007'199'254'740'991LL;
    constexpr std::int64_t maximum = 9'007'199'254'740'991LL;

    const auto accepts_intent = [](O operation, json args, json immutable_plan) {
        std::string error;
        auto value = daemon::make_account_audit_intent(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:00Z"},
             "main",
             operation,
             std::move(args),
             std::move(immutable_plan),
             std::string(kFingerprint),
             std::string(kSnapshot),
             "request",
             destructive(operation) ? std::optional<std::string>{"yes"} : std::nullopt,
             std::nullopt,
             100},
            error);
        REQUIRE(value);
        CHECK(
            tgcli::test::matches_json_schema("audit-intent.schema.json").match(value->document()));
    };

    auto args = arguments(O::Send);
    auto immutable_plan = plan(O::Send);
    args["reply_to"] = minimum;
    immutable_plan["reply_to"] = minimum;
    accepts_intent(O::Send, args, immutable_plan);

    for (const auto operation : {O::MsgEdit, O::MsgReact, O::MsgPin, O::MsgUnpin, O::SavedAttach}) {
        args = arguments(operation);
        immutable_plan = plan(operation);
        args["message_id"] = minimum;
        immutable_plan["message_id"] = minimum;
        accepts_intent(operation, args, immutable_plan);
    }

    for (const auto operation : {O::MsgDelete, O::MsgForward}) {
        args = arguments(operation);
        immutable_plan = plan(operation);
        args["message_ids"] = json::array({minimum, -1, maximum});
        immutable_plan["message_ids"] = args["message_ids"];
        accepts_intent(operation, args, immutable_plan);
    }

    args = arguments(O::ChatMarkRead);
    immutable_plan = plan(O::ChatMarkRead);
    immutable_plan["last_message_id"] = minimum;
    accepts_intent(O::ChatMarkRead, args, immutable_plan);

    args = arguments(O::ChatMute);
    immutable_plan = plan(O::ChatMute);
    args["duration_seconds"] = std::numeric_limits<std::int32_t>::max();
    immutable_plan["duration_seconds"] = std::numeric_limits<std::int32_t>::max();
    accepts_intent(O::ChatMute, args, immutable_plan);

    const auto rejects_intent_document = [](const json& document) {
        std::string error;
        CHECK_FALSE(daemon::validate_account_audit_intent(document, error));
        CHECK_FALSE(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
    };
    auto document = make_intent(O::Send).document();
    document["arguments"]["reply_to"] = 0;
    document["plan"]["reply_to"] = 0;
    rejects_intent_document(document);
    document = make_intent(O::MsgEdit).document();
    document["arguments"]["message_id"] = 0;
    document["plan"]["message_id"] = 0;
    rejects_intent_document(document);
    document = make_intent(O::MsgDelete).document();
    document["arguments"]["message_ids"] = json::array({minimum, 0, maximum});
    document["plan"]["message_ids"] = document["arguments"]["message_ids"];
    rejects_intent_document(document);
    document = make_intent(O::SavedAttach).document();
    document["arguments"]["message_id"] = 0;
    document["plan"]["message_id"] = 0;
    rejects_intent_document(document);
    for (const auto invalid_duration : {31'622'401, std::numeric_limits<std::int32_t>::max() - 1}) {
        document = make_intent(O::ChatMute).document();
        document["arguments"]["duration_seconds"] = invalid_duration;
        document["plan"]["duration_seconds"] = invalid_duration;
        rejects_intent_document(document);
    }
    document = make_intent(O::Send).document();
    document["arguments"]["topic"] = {{"kind", "forum"}, {"id", -1}};
    document["plan"]["requested_topic"] = document["arguments"]["topic"];
    document["plan"]["effective_topic"] = document["arguments"]["topic"];
    rejects_intent_document(document);

    const auto accepts_proof = [](O operation, json data) {
        std::string error;
        auto value = daemon::make_account_audit_checkpoint(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:01Z"},
             "main",
             operation,
             1,
             S::MutationConfirmed,
             {{"terminal", {{"kind", "result"}, {"data", std::move(data)}}}}},
            error);
        REQUIRE(value);
        CHECK(tgcli::test::matches_json_schema("audit-checkpoint.schema.json")
                  .match(value->document()));
    };

    accepts_proof(O::Send, message_write(minimum));
    auto data = result_data(O::MsgDelete);
    data["message_ids"] = json::array({minimum, -1, maximum});
    accepts_proof(O::MsgDelete, data);
    data = result_data(O::MsgForward);
    data["items"] = json::array(
        {json{{"source_id", minimum}, {"status", "sent"}, {"message", message_write(-2)}},
         json{{"source_id", -1}, {"status", "sent"}, {"message", message_write(-1)}}});
    accepts_proof(O::MsgForward, data);
    for (const auto operation : {O::MsgReact, O::MsgPin, O::MsgUnpin}) {
        data = result_data(operation);
        data["message_id"] = minimum;
        accepts_proof(operation, data);
    }
    data = result_data(O::ChatMarkRead);
    data["last_read_message_id"] = minimum;
    accepts_proof(O::ChatMarkRead, data);
    data = result_data(O::ChatMute);
    data["duration_seconds"] = std::numeric_limits<std::int32_t>::max();
    accepts_proof(O::ChatMute, data);

    std::string error;
    auto progress = daemon::make_account_audit_checkpoint(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:01Z"},
         "main",
         O::MsgForward,
         1,
         S::ForwardProgress,
         {{"items",
           json::array({json{
               {"source_id", minimum}, {"status", "pending"}, {"temporary_message_id", -1}}})}}},
        error);
    REQUIRE(progress);
    CHECK(tgcli::test::matches_json_schema("audit-checkpoint.schema.json")
              .match(progress->document()));

    const auto rejects_checkpoint_document = [](const json& value) {
        std::string validation_error;
        CHECK_FALSE(daemon::validate_account_audit_checkpoint(value, validation_error));
        CHECK_FALSE(tgcli::test::matches_json_schema("audit-checkpoint.schema.json").match(value));
    };
    document =
        checkpoint(O::Send, S::MutationConfirmed, 1, {{"terminal", result_terminal(O::Send)}})
            .document();
    document["data"]["terminal"]["data"]["id"] = 0;
    rejects_checkpoint_document(document);
    document =
        checkpoint(O::Send, S::MutationConfirmed, 1, {{"terminal", result_terminal(O::Send)}})
            .document();
    document["data"]["terminal"]["data"]["sender"]["id"] = -1;
    rejects_checkpoint_document(document);
    document = checkpoint(O::Send, S::TemporaryIdsObserved, 1,
                          {{"temporary_message_ids", json::array({-1})}})
                   .document();
    document["data"]["temporary_message_ids"] = json::array({0});
    rejects_checkpoint_document(document);
    document = progress->document();
    document["data"]["items"][0]["source_id"] = 0;
    rejects_checkpoint_document(document);

    const json precondition_terminal{{"kind", "error"},
                                     {"code", "PRECONDITION_FAILED"},
                                     {"message", "operation precondition failed"},
                                     {"details",
                                      {{"operation", "msg_edit"},
                                       {"chat_id", -1001},
                                       {"message_id", minimum},
                                       {"reason", "not_editable"}}},
                                     {"exit_code", 1}};
    auto outcome = daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         O::MsgEdit,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         precondition_terminal},
        error);
    REQUIRE(outcome);
    CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json").match(outcome->document()));
}

TEST_CASE("account audit correlates forward temporary progress and proof vectors",
          "[account-audit][forward][regression]") {
    std::string error;
    CHECK_FALSE(daemon::make_account_audit_checkpoint(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:01Z"},
         "main",
         daemon::AccountAuditOperation::MsgForward,
         2,
         daemon::AccountAuditStage::TemporaryIdsObserved,
         {{"temporary_message_ids", json::array({-1, -1})}}},
        error));

    AuditTree tree;
    auto intent = make_intent(daemon::AccountAuditOperation::MsgForward).document();
    intent["arguments"]["message_ids"] = json::array({1, 2});
    intent["plan"]["message_ids"] = json::array({1, 2});
    append_line(tree, intent);
    append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                 daemon::AccountAuditStage::DispatchStarted, 1,
                                 {{"tdlib_function", "forwardMessages"},
                                  {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                  {"client_generation", std::uint64_t{1}}})
                          .document());
    append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                 daemon::AccountAuditStage::TemporaryIdsObserved, 2,
                                 {{"temporary_message_ids", json::array({-1, -2})}})
                          .document());
    append_line(
        tree,
        checkpoint(
            daemon::AccountAuditOperation::MsgForward, daemon::AccountAuditStage::ForwardProgress,
            3,
            {{"items",
              json::array(
                  {json{{"source_id", 1}, {"status", "pending"}, {"temporary_message_id", -2}},
                   json{{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", -1}}})}})
            .document());
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    CHECK(log.inspect(guard).status == daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit validates every known pin before any rotation decision",
          "[account-audit][rotation][pins][regression]") {
    AuditTree tree;
    tree.write({}, json_lines(complete_v2_records("11111111111111111111111111111111")));
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    daemon::KnownAccountAuditPins pins{
        {{999999999, "11111111111111111111111111111111", std::string(kFingerprint),
          daemon::AccountAuditOperation::Send}}};
    daemon::AccountAuditAppendReceipt receipt;
    daemon::AccountAuditFailure failure;
    CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send), pins, guard,
                              receipt, failure));
    CHECK(failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
}

TEST_CASE("account audit coordinator rejects unlocked files and reuses one mutex",
          "[account-audit][lock][regression]") {
    AuditTree tree;
    std::string error;
    auto duplicate = daemon::AccountAuditCoordinator::create(tree.state(), "main", ::getuid(),
                                                             tree.lock_lease(), error);
    REQUIRE(duplicate);
    CHECK(duplicate.get() == &tree.coordinator());

    const auto forged_state = std::filesystem::path(tree.state()).parent_path() / "forged";
    REQUIRE(std::filesystem::create_directory(forged_state));
    REQUIRE(::chmod(forged_state.c_str(), 0700) == 0);
    std::ifstream input(tree.state() + "/daemon.lock", std::ios::binary);
    const std::string record((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    std::ofstream output(forged_state / "daemon.lock", std::ios::binary);
    output << record;
    output.close();
    REQUIRE(::chmod((forged_state / "daemon.lock").c_str(), 0600) == 0);
    auto forged = daemon::AccountAuditCoordinator::create(forged_state.string(), "forged",
                                                          ::getuid(), {}, error);
    CHECK_FALSE(forged);

    for (int iteration = 0; iteration < 8; ++iteration) {
        auto repeated = daemon::AccountAuditCoordinator::create(tree.state(), "main", ::getuid(),
                                                                tree.lock_lease(), error);
        REQUIRE(repeated);
        CHECK(repeated.get() == &tree.coordinator());
    }

    const pid_t competitor = ::fork();
    REQUIRE(competitor >= 0);
    if (competitor == 0) {
        const int descriptor =
            ::open((tree.state() + "/daemon.lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) {
            ::_exit(91);
        }
        struct flock lock {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = 0;
        lock.l_len = 0;
        const int acquired = ::fcntl(descriptor, F_SETLK, &lock);
        const int failure = errno;
        ::close(descriptor);
        ::_exit(acquired < 0 && (failure == EACCES || failure == EAGAIN) ? 0 : 92);
    }
    int status = 0;
    REQUIRE(::waitpid(competitor, &status, 0) == competitor);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("account audit scan and identity rescan share one interruption budget",
          "[account-audit][scanner][deadline][cancellation]") {
    SECTION("primary scan observes the absolute deadline") {
        AuditTree tree;
        daemon::AccountAuditScanControl control;
        control.deadline = RequestDeadline{std::chrono::steady_clock::now()};
        auto result = tree.coordinator().lock(std::move(control));
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        const auto& failure = std::get<daemon::AccountAuditFailure>(result);
        REQUIRE(failure.interruption);
        CHECK(*failure.interruption == daemon::AccountAuditFailure::Interruption::Deadline);
    }

    SECTION("deterministic identity rescan observes the same cancellation") {
        AuditTree tree;
        tree.write({}, json_lines(complete_v2_records("11111111111111111111111111111111")));
        std::atomic<bool> cancelled = false;
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->before_identity_rescan = [&] { cancelled.store(true, std::memory_order_release); };
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        daemon::AccountAuditScanControl control;
        control.cancelled = [&] { return cancelled.load(std::memory_order_acquire); };
        auto guard = acquire_guard(tree.coordinator(), std::move(control));
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Interrupted);
        REQUIRE(inspection.failure.interruption);
        CHECK(*inspection.failure.interruption ==
              daemon::AccountAuditFailure::Interruption::Cancelled);
    }

    SECTION("maximum record parsing observes cancellation inside the parser") {
        AuditTree tree;
        constexpr std::string_view prefix = R"({"schema_version":2,"phase":"intent","padding":")";
        constexpr std::string_view suffix = R"("})";
        REQUIRE(prefix.size() + suffix.size() < daemon::account_audit_limits::kIntentJsonBytes);
        std::string record(prefix);
        record.append(static_cast<std::size_t>(daemon::account_audit_limits::kIntentJsonBytes -
                                               prefix.size() - suffix.size()),
                      'x');
        record += suffix;
        record.push_back('\n');
        tree.write({}, record);

        std::atomic<bool> cancelled = false;
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->after_parser_poll = [&] { cancelled.store(true, std::memory_order_release); };
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        daemon::AccountAuditScanControl control;
        control.cancelled = [&] { return cancelled.load(std::memory_order_acquire); };
        auto guard = acquire_guard(tree.coordinator(), std::move(control));
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Interrupted);
        REQUIRE(inspection.failure.interruption);
        CHECK(*inspection.failure.interruption ==
              daemon::AccountAuditFailure::Interruption::Cancelled);
    }

    SECTION("final classification rechecks the same cancellation") {
        AuditTree tree;
        std::atomic<bool> cancelled = false;
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->before_final_classification = [&] {
            cancelled.store(true, std::memory_order_release);
        };
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        daemon::AccountAuditScanControl control;
        control.cancelled = [&] { return cancelled.load(std::memory_order_acquire); };
        auto guard = acquire_guard(tree.coordinator(), std::move(control));
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Interrupted);
    }
}

TEST_CASE("account audit scanner rejects every wrong-typed v2 envelope field without throwing",
          "[account-audit][scanner][types]") {
    std::string error;
    const auto intent = make_intent(daemon::AccountAuditOperation::Send).document();
    const auto dispatch = checkpoint(daemon::AccountAuditOperation::Send,
                                     daemon::AccountAuditStage::DispatchStarted, 1,
                                     {{"tdlib_function", "sendMessage"},
                                      {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"client_generation", std::uint64_t{1}}})
                              .document();
    const auto outcome = daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::Send,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         error_terminal()},
        error);
    REQUIRE(outcome);
    const auto wrong_type = [](const json& value) -> json {
        if (value.is_string()) {
            return false;
        }
        if (value.is_boolean()) {
            return "wrong";
        }
        if (value.is_array()) {
            return json::object();
        }
        if (value.is_object()) {
            return json::array();
        }
        if (value.is_null()) {
            return true;
        }
        return "wrong";
    };
    for (const auto& record : {intent, dispatch, outcome->document()}) {
        for (const auto& [field, value] : record.items()) {
            INFO(record["phase"]);
            INFO(field);
            AuditTree tree;
            auto malformed = record;
            malformed[field] = wrong_type(value);
            append_line(tree, malformed);
            daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
            auto guard = tree.coordinator().lock();
            daemon::AccountAuditInspection inspection;
            CHECK_NOTHROW(inspection = log.inspect(guard));
            if (field == "schema_version") {
                CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Unavailable);
                CHECK(inspection.failure.reason ==
                      daemon::AccountAuditDurabilityReason::PathInvalid);
            } else {
                CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
            }
        }
    }

    AuditTree duplicate;
    auto bytes = intent.dump();
    const auto phase = bytes.find(R"("phase":"intent")");
    REQUIRE(phase != std::string::npos);
    bytes.replace(phase, std::string_view(R"("phase":"intent")").size(),
                  R"("phase":"intent","phase":"intent")");
    duplicate.write({}, bytes + '\n');
    daemon::AccountAuditLog duplicate_log(duplicate.state(), "main", ::getuid());
    auto duplicate_guard = duplicate.coordinator().lock();
    CHECK(duplicate_log.inspect(duplicate_guard).status ==
          daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit runtime and generated schemas share operation boundaries",
          "[account-audit][contract][schema][boundaries]") {
    const auto rejects_intent = [](const json& document) {
        std::string error;
        CHECK_FALSE(daemon::validate_account_audit_intent(document, error));
        CHECK_FALSE(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
    };
    auto document = make_intent(daemon::AccountAuditOperation::Send).document();
    document["arguments"]["text"] = "";
    document["plan"]["text"] = "";
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::Send).document();
    document["timestamp"] = "1969-12-31T23:59:59Z";
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::Send).document();
    document["arguments"]["schedule"] = {{"kind", "at"}, {"send_date", 100}};
    document["plan"]["schedule"] = document["arguments"]["schedule"];
    document["plan"]["observed_server_unix_time"] = nullptr;
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::MsgReact).document();
    document["arguments"]["remove"] = true;
    document["arguments"]["big"] = true;
    document["plan"]["remove"] = true;
    document["plan"]["big"] = true;
    document["plan"]["tdlib_request"] = "removeMessageReaction";
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::ChatMute).document();
    document["arguments"]["duration_seconds"] = 0;
    document["plan"]["duration_seconds"] = 0;
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
    document["plan"]["file"]["name"] = ".";
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
    document["plan"]["file"]["mtime_ns"] = std::numeric_limits<std::uint64_t>::max();
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::Send).document();
    document["plan"]["chat"]["is_bot"] = true;
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::Send).document();
    const std::string maximal_text(4'096, 'x');
    document["arguments"]["text"] = maximal_text;
    document["plan"]["text"] = maximal_text;
    std::string error;
    CHECK(daemon::validate_account_audit_intent(document, error));
    CHECK(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
    document["arguments"]["text"].get_ref<std::string&>().push_back('x');
    document["plan"]["text"] = document["arguments"]["text"];
    rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
    const std::string maximal_caption(1'024, 'x');
    document["arguments"]["caption"] = maximal_caption;
    document["plan"]["caption"] = maximal_caption;
    CHECK(daemon::validate_account_audit_intent(document, error));
    CHECK(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
    document["arguments"]["caption"].get_ref<std::string&>().push_back('x');
    document["plan"]["caption"] = document["arguments"]["caption"];
    rejects_intent(document);

    auto forward = checkpoint(daemon::AccountAuditOperation::MsgForward,
                              daemon::AccountAuditStage::ForwardProgress, 1,
                              {{"items", json::array({json{{"source_id", 1},
                                                           {"status", "failed"},
                                                           {"failure_reason", "tdlib_error"},
                                                           {"tdlib_code", 429},
                                                           {"retry_after", 1}}})}})
                       .document();
    forward["data"]["items"][0]["retry_after"] = 0;
    CHECK_FALSE(daemon::validate_account_audit_checkpoint(forward, error));
    CHECK_FALSE(tgcli::test::matches_json_schema("audit-checkpoint.schema.json").match(forward));

    const std::vector<std::pair<std::string, std::vector<std::string>>> expected_rules{
        {"audit-checkpoint.schema.json",
         {"aggregate_serialized_bytes", "cross_record_equality_and_derivation",
          "projected_uniqueness", "same_record_equality_and_derivation", "strict_numeric_order",
          "utf8_byte_limits"}},
        {"audit-intent.schema.json",
         {"aggregate_serialized_bytes", "contextual_normalization",
          "same_record_equality_and_derivation", "strict_numeric_order", "utf8_byte_limits"}},
        {"audit-outcome.schema.json",
         {"aggregate_serialized_bytes", "cross_record_equality_and_derivation",
          "projected_uniqueness", "same_record_equality_and_derivation", "strict_numeric_order",
          "utf8_byte_limits"}},
    };
    constexpr std::array<std::string_view, 8> forbidden_pseudo_assertions{
        "x-tgcli-maxUtf8Bytes",
        "x-tgcli-forbidControlScalars",
        "x-tgcli-strictlyIncreasing",
        "x-tgcli-strictlyIncreasingField",
        "x-tgcli-serverWindow",
        "x-tgcli-legalStagePrefixFor",
        "x-tgcli-retryAfterEqualsMaximum",
        "x-tgcli-terminalClass"};
    for (const auto& [filename, rules] : expected_rules) {
        const auto schema = tgcli::test::load_schema_document(filename);
        CHECK(schema["$comment"] ==
              "For schema_version 2, full tgcli contract validation also requires the "
              "documentation-only x-tgcli-semanticValidation rules; an ordinary Draft 2020-12 "
              "validator ignores that annotation.");
        REQUIRE(schema.contains("x-tgcli-semanticValidation"));
        const auto& marker = schema["x-tgcli-semanticValidation"];
        CHECK(marker == json{{"annotationOnly", true},
                             {"ordinaryDraft202012ValidationIsInsufficient", true},
                             {"schemaVersion", 2},
                             {"validator", "tgcli-runtime-v1"},
                             {"rules", rules}});
        CHECK(std::ranges::is_sorted(rules));
        CHECK(std::set<std::string>(rules.begin(), rules.end()).size() == rules.size());
        const auto bytes = schema.dump();
        for (const auto pseudo_assertion : forbidden_pseudo_assertions) {
            INFO(filename);
            INFO(pseudo_assertion);
            CHECK(bytes.find(pseudo_assertion) == std::string::npos);
        }
        REQUIRE(schema["oneOf"].is_array());
        REQUIRE_FALSE(schema["oneOf"].empty());
        const auto& v1_branch = schema["oneOf"].front();
        const auto& v1_base = v1_branch.contains("properties") ? v1_branch : v1_branch["allOf"][0];
        CHECK(v1_base["properties"]["schema_version"]["const"] == 1);
        CHECK_FALSE(v1_branch.contains("x-tgcli-semanticValidation"));
    }
}

TEST_CASE("account audit schemas assert every portable calendar path stage and terminal relation",
          "[account-audit][contract][schema][ordinary]") {
    const auto rejects_intent = [](const json& document) {
        std::string error;
        CHECK_FALSE(daemon::validate_account_audit_intent(document, error));
        CHECK_FALSE(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
    };
    for (const auto* timestamp : {"1969-12-31T23:59:59Z", "2001-02-29T00:00:00Z",
                                  "2000-04-31T00:00:00Z", "9999-02-29T00:00:00Z"}) {
        auto document = make_intent(daemon::AccountAuditOperation::Send).document();
        document["timestamp"] = timestamp;
        rejects_intent(document);
    }
    for (const auto* timestamp :
         {"1972-02-29T00:00:00Z", "2000-02-29T23:59:59Z", "9996-02-29T12:34:56Z"}) {
        auto document = make_intent(daemon::AccountAuditOperation::Send).document();
        document["timestamp"] = timestamp;
        std::string error;
        CHECK(daemon::validate_account_audit_intent(document, error));
        CHECK(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
    }

    for (const std::string& invalid_name :
         {std::string("bad\x1f", 4), std::string("bad\x7f", 4), std::string("bad\xc2\x80", 5)}) {
        auto document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
        document["plan"]["file"]["name"] = invalid_name;
        document["plan"]["file"]["path"] = "/tmp/" + invalid_name;
        rejects_intent(document);
    }
    for (const auto* invalid_path :
         {"relative/input", "/tmp//input", "/tmp/./input", "/tmp/../input", "/tmp/input/"}) {
        auto document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
        document["plan"]["file"]["path"] = invalid_path;
        rejects_intent(document);
    }
    {
        auto document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
        document["arguments"]["path"] = "input/";
        rejects_intent(document);
    }

    const json audit_incomplete{
        {"kind", "error"},
        {"code", "AUDIT_INCOMPLETE"},
        {"message", "a prior audited invocation did not reach a terminal proof"},
        {"details",
         {{"account", "main"},
          {"path", "/tmp/audit.log"},
          {"mutation_state", "none"},
          {"completed_stages", json::array()}}},
        {"exit_code", 1}};
    std::string error;
    auto outcome = daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::Send,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         audit_incomplete},
        error);
    REQUIRE(outcome);
    CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json").match(outcome->document()));
    auto illegal_stage_relation = outcome->document();
    illegal_stage_relation["terminal"]["details"]["mutation_state"] = "possible";
    CHECK_FALSE(daemon::validate_account_audit_outcome(illegal_stage_relation, error));
    CHECK_FALSE(tgcli::test::matches_json_schema("audit-outcome.schema.json")
                    .match(illegal_stage_relation));

    const json sent{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}};
    const json failed{{"source_id", 2},
                      {"status", "failed"},
                      {"failure_reason", "upstream_null"},
                      {"tdlib_code", nullptr},
                      {"retry_after", nullptr}};
    const auto forward_terminal = [](std::string code, std::string message, json items) {
        return json{{"kind", "error"},
                    {"code", std::move(code)},
                    {"message", std::move(message)},
                    {"details",
                     {{"operation", "msg_forward"},
                      {"from_chat_id", -1001},
                      {"to_chat_id", -1002},
                      {"items", std::move(items)}}},
                    {"exit_code", 1}};
    };
    auto failed_outcome = daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::MsgForward,
         false,
         daemon::AccountAuditMutationState::None,
         {daemon::AccountAuditStage::DispatchStarted, daemon::AccountAuditStage::ForwardProgress},
         forward_terminal("FORWARD_FAILED", "messages could not be forwarded",
                          json::array({failed}))},
        error);
    REQUIRE(failed_outcome);
    CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json")
              .match(failed_outcome->document()));
    auto failed_with_sent = failed_outcome->document();
    failed_with_sent["terminal"]["details"]["items"] = json::array({sent});
    CHECK_FALSE(daemon::validate_account_audit_outcome(failed_with_sent, error));
    CHECK_FALSE(
        tgcli::test::matches_json_schema("audit-outcome.schema.json").match(failed_with_sent));

    auto partial_outcome = daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         daemon::AccountAuditOperation::MsgForward,
         false,
         daemon::AccountAuditMutationState::Confirmed,
         {daemon::AccountAuditStage::DispatchStarted, daemon::AccountAuditStage::ForwardProgress},
         forward_terminal("FORWARD_PARTIAL", "some messages could not be forwarded",
                          json::array({sent, failed}))},
        error);
    REQUIRE(partial_outcome);
    CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json")
              .match(partial_outcome->document()));
    auto partial_without_sent = partial_outcome->document();
    partial_without_sent["terminal"]["details"]["items"] = json::array({failed});
    CHECK_FALSE(daemon::validate_account_audit_outcome(partial_without_sent, error));
    CHECK_FALSE(
        tgcli::test::matches_json_schema("audit-outcome.schema.json").match(partial_without_sent));
}

TEST_CASE("account audit semantic marker rules are runtime-only conjunction checks",
          "[account-audit][contract][schema][runtime-only]") {
    const auto schema_accepts_runtime_rejects_intent = [](const json& document) {
        std::string error;
        CHECK(tgcli::test::matches_json_schema("audit-intent.schema.json").match(document));
        CHECK_FALSE(daemon::validate_account_audit_intent(document, error));
    };

    auto document = make_intent(daemon::AccountAuditOperation::MsgDelete).document();
    document["arguments"]["message_ids"] = json::array({2, 1});
    document["plan"]["message_ids"] = document["arguments"]["message_ids"];
    schema_accepts_runtime_rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::Send).document();
    document["arguments"]["text"] = "different";
    schema_accepts_runtime_rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::SavedAttach).document();
    document["plan"]["file"]["path"] = "/tmp/other";
    schema_accepts_runtime_rejects_intent(document);

    document = make_intent(daemon::AccountAuditOperation::Send).document();
    std::string wide_title;
    wide_title.reserve(1'048'578);
    for (std::size_t index = 0; index < 524'289; ++index) {
        wide_title += "\xc3\xa9";
    }
    document["plan"]["chat"]["title"] = std::move(wide_title);
    schema_accepts_runtime_rejects_intent(document);

    std::string error;
    auto progress = checkpoint(daemon::AccountAuditOperation::MsgForward,
                               daemon::AccountAuditStage::ForwardProgress, 1,
                               {{"items", json::array({json{{"source_id", 1},
                                                            {"status", "pending"},
                                                            {"temporary_message_id", -1}},
                                                       json{{"source_id", 2},
                                                            {"status", "failed"},
                                                            {"failure_reason", "upstream_null"},
                                                            {"tdlib_code", nullptr},
                                                            {"retry_after", nullptr}}})}})
                        .document();
    progress["data"]["items"][1]["source_id"] = 1;
    CHECK(tgcli::test::matches_json_schema("audit-checkpoint.schema.json").match(progress));
    CHECK_FALSE(daemon::validate_account_audit_checkpoint(progress, error));

    const json incomplete{{"kind", "error"},
                          {"code", "AUDIT_INCOMPLETE"},
                          {"message", "a prior audited invocation did not reach a terminal proof"},
                          {"details",
                           {{"account", "main"},
                            {"path", std::string(6'000, '\x01')},
                            {"mutation_state", "none"},
                            {"completed_stages", json::array()}}},
                          {"exit_code", 1}};
    json aggregate_outcome{{"schema_version", 2},
                           {"phase", "outcome"},
                           {"invocation_id", "0123456789abcdef0123456789abcdef"},
                           {"timestamp", "2026-08-19T12:00:02Z"},
                           {"account", "main"},
                           {"command", "chat_mark_read"},
                           {"success", false},
                           {"mutation_state", "none"},
                           {"completed_stages", json::array()},
                           {"terminal", incomplete}};
    CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json").match(aggregate_outcome));
    CHECK_FALSE(daemon::validate_account_audit_outcome(aggregate_outcome, error));

    auto mismatched_incomplete = aggregate_outcome;
    mismatched_incomplete["terminal"]["details"]["path"] = "/tmp/audit.log";
    mismatched_incomplete["terminal"]["details"]["mutation_state"] = "possible";
    mismatched_incomplete["terminal"]["details"]["completed_stages"] =
        json::array({"dispatch_started"});
    CHECK(
        tgcli::test::matches_json_schema("audit-outcome.schema.json").match(mismatched_incomplete));
    CHECK_FALSE(daemon::validate_account_audit_outcome(mismatched_incomplete, error));

    const json rate_limited{{"kind", "error"},
                            {"code", "RATE_LIMITED"},
                            {"message", "Telegram rate limit exceeded"},
                            {"details",
                             {{"operation", "msg_forward"},
                              {"tdlib_code", 429},
                              {"retry_after", 2},
                              {"items", json::array({json{{"source_id", 1},
                                                          {"status", "failed"},
                                                          {"failure_reason", "tdlib_error"},
                                                          {"tdlib_code", 429},
                                                          {"retry_after", 2}},
                                                     json{{"source_id", 2},
                                                          {"status", "failed"},
                                                          {"failure_reason", "tdlib_error"},
                                                          {"tdlib_code", 429},
                                                          {"retry_after", 3}}})}}},
                            {"exit_code", 5}};
    json rate_outcome{{"schema_version", 2},
                      {"phase", "outcome"},
                      {"invocation_id", "0123456789abcdef0123456789abcdef"},
                      {"timestamp", "2026-08-19T12:00:02Z"},
                      {"account", "main"},
                      {"command", "msg_forward"},
                      {"success", false},
                      {"mutation_state", "none"},
                      {"completed_stages", json::array({"dispatch_started", "forward_progress"})},
                      {"terminal", rate_limited}};
    CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json").match(rate_outcome));
    CHECK_FALSE(daemon::validate_account_audit_outcome(rate_outcome, error));

    AuditTree tree;
    append_line(tree, make_intent(daemon::AccountAuditOperation::Send).document());
    auto cross_record = checkpoint(daemon::AccountAuditOperation::Send,
                                   daemon::AccountAuditStage::DispatchStarted, 1,
                                   {{"tdlib_function", "sendMessage"},
                                    {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                    {"client_generation", std::uint64_t{1}}})
                            .document();
    cross_record["account"] = "other";
    CHECK(daemon::validate_account_audit_checkpoint(cross_record, error));
    CHECK(tgcli::test::matches_json_schema("audit-checkpoint.schema.json").match(cross_record));
    append_line(tree, cross_record);
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    CHECK(log.inspect(guard).status == daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit forward proof is byte-semantic with its final vector and plan",
          "[account-audit][forward][proof]") {
    AuditTree tree;
    append_line(tree, make_intent(daemon::AccountAuditOperation::MsgForward).document());
    append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                 daemon::AccountAuditStage::DispatchStarted, 1,
                                 {{"tdlib_function", "forwardMessages"},
                                  {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                  {"client_generation", std::uint64_t{1}}})
                          .document());
    append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                 daemon::AccountAuditStage::ForwardProgress, 2,
                                 {{"items", json::array({json{{"source_id", 1},
                                                              {"status", "sent"},
                                                              {"message", message_write()}}})}})
                          .document());
    auto mismatched = result_terminal(daemon::AccountAuditOperation::MsgForward);
    mismatched["data"]["to_chat_id"] = -9999;
    append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                 daemon::AccountAuditStage::MutationConfirmed, 3,
                                 {{"terminal", mismatched}})
                          .document());
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    CHECK(log.inspect(guard).status == daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit correlates every later item terminal with latest forward progress",
          "[account-audit][forward][terminal][regression]") {
    const auto inspect_with_terminal_items = [](json terminal_items) {
        AuditTree tree;
        auto intent = make_intent(daemon::AccountAuditOperation::MsgForward).document();
        intent["arguments"]["message_ids"] = json::array({1, 2});
        intent["plan"]["message_ids"] = json::array({1, 2});
        append_line(tree, intent);
        append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                     daemon::AccountAuditStage::DispatchStarted, 1,
                                     {{"tdlib_function", "forwardMessages"},
                                      {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"client_generation", std::uint64_t{1}}})
                              .document());
        append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                     daemon::AccountAuditStage::TemporaryIdsObserved, 2,
                                     {{"temporary_message_ids", json::array({-2})}})
                              .document());
        const json latest = json::array(
            {json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}},
             json{{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", -2}}});
        append_line(tree,
                    checkpoint(daemon::AccountAuditOperation::MsgForward,
                               daemon::AccountAuditStage::ForwardProgress, 3, {{"items", latest}})
                        .document());
        const json terminal{{"kind", "error"},
                            {"code", "TIMEOUT"},
                            {"message", "request timed out"},
                            {"details",
                             {{"operation", "msg_forward"},
                              {"phase", "confirmation"},
                              {"state", "ready"},
                              {"outcome", "unknown"},
                              {"idempotency", "not_requested"},
                              {"items", std::move(terminal_items)}}},
                            {"exit_code", 7}};
        std::string error;
        auto outcome = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:04Z"},
             "main",
             daemon::AccountAuditOperation::MsgForward,
             false,
             daemon::AccountAuditMutationState::Confirmed,
             {daemon::AccountAuditStage::DispatchStarted,
              daemon::AccountAuditStage::TemporaryIdsObserved,
              daemon::AccountAuditStage::ForwardProgress},
             terminal},
            error);
        INFO(error);
        REQUIRE(outcome);
        append_line(tree, outcome->document());
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        return log.inspect(guard).status;
    };

    const json latest =
        json::array({json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}},
                     json{{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", -2}}});
    CHECK(inspect_with_terminal_items(latest) == daemon::AccountAuditInspectionStatus::Clean);

    auto changed_pending = latest;
    changed_pending[1]["temporary_message_id"] = -3;
    CHECK(inspect_with_terminal_items(changed_pending) ==
          daemon::AccountAuditInspectionStatus::Contradiction);

    auto wrong_sources = latest;
    wrong_sources[0]["source_id"] = 2;
    wrong_sources[1]["source_id"] = 3;
    CHECK(inspect_with_terminal_items(wrong_sources) ==
          daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit validates dangling pins for every numbered occupancy mask",
          "[account-audit][rotation][pins][matrix][regression]") {
    for (std::uint32_t mask = 0; mask < 16; ++mask) {
        INFO(mask);
        AuditTree tree;
        for (std::size_t slot = 1; slot <= 4; ++slot) {
            if ((mask & (1U << (slot - 1))) != 0) {
                const auto invocation = std::string(31, static_cast<char>('0' + slot)) + "a";
                tree.write("." + std::to_string(slot), json_lines(complete_v2_records(invocation)));
            }
        }
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::KnownAccountAuditPins pins{
            {{std::numeric_limits<std::uint64_t>::max(), "11111111111111111111111111111111",
              std::string(kFingerprint), daemon::AccountAuditOperation::Send}}};
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send), pins,
                                  guard, receipt, failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    }
}

TEST_CASE("account audit stored errors are closed by operation stage and exit",
          "[account-audit][contract][terminal][matrix]") {
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    const auto stored_error = [](std::string code, std::string message, json details, int exit) {
        return json{{"kind", "error"},
                    {"code", std::move(code)},
                    {"message", std::move(message)},
                    {"details", std::move(details)},
                    {"exit_code", exit}};
    };
    const auto accepts = [&](O operation, daemon::AccountAuditMutationState mutation,
                             std::vector<S> stages, json terminal) {
        std::string error;
        const auto outcome = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
             "main",
             operation,
             false,
             mutation,
             std::move(stages),
             std::move(terminal)},
            error);
        INFO(error);
        REQUIRE(outcome);
        CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json")
                  .match(outcome->document()));
    };

    accepts(O::Send, daemon::AccountAuditMutationState::Confirmed,
            {S::DispatchStarted, S::MutationConfirmed},
            stored_error("INTERNAL", "internal error",
                         {{"operation", "send"}, {"reason", "internal_error"}}, 1));
    accepts(O::Send, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error("DAEMON_SHUTDOWN", "daemon is shutting down",
                         {{"reason", "daemon_shutdown"}}, 1));
    accepts(O::Send, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error(
                "NOT_AUTHED", "authorization was lost",
                {{"account", "main"}, {"state", "closing"}, {"reason", "authorization_lost"}}, 3));
    accepts(O::MsgEdit, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error("TDLIB_ERROR", "Telegram request failed",
                         {{"operation", "msg_edit"}, {"tdlib_code", 400}}, 1));
    accepts(O::MsgDelete, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error("RATE_LIMITED", "Telegram rate limit exceeded",
                         {{"operation", "msg_delete"}, {"tdlib_code", 429}, {"retry_after", 1}},
                         5));
    accepts(O::Send, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error("SEND_FAILED", "message was deleted before confirmation",
                         {{"operation", "send"},
                          {"chat_id", -1001},
                          {"temporary_message_id", -1},
                          {"reason", "deleted_before_confirmation"}},
                         1));

    const auto sent = json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}};
    const auto failed = json{{"source_id", 2},
                             {"status", "failed"},
                             {"failure_reason", "upstream_null"},
                             {"tdlib_code", nullptr},
                             {"retry_after", nullptr}};
    accepts(O::MsgForward, daemon::AccountAuditMutationState::Confirmed,
            {S::DispatchStarted, S::ForwardProgress},
            stored_error("FORWARD_PARTIAL", "some messages could not be forwarded",
                         {{"operation", "msg_forward"},
                          {"from_chat_id", -1001},
                          {"to_chat_id", -1002},
                          {"items", json::array({sent, failed})}},
                         1));
    accepts(O::MsgForward, daemon::AccountAuditMutationState::None,
            {S::DispatchStarted, S::ForwardProgress},
            stored_error("FORWARD_FAILED", "messages could not be forwarded",
                         {{"operation", "msg_forward"},
                          {"from_chat_id", -1001},
                          {"to_chat_id", -1002},
                          {"items", json::array({json{{"source_id", 1},
                                                      {"status", "failed"},
                                                      {"failure_reason", "upstream_null"},
                                                      {"tdlib_code", nullptr},
                                                      {"retry_after", nullptr}}})}},
                         1));
    accepts(O::ChatJoin, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error("JOIN_DECLINED", "join request was declined", {{"operation", "chat_join"}},
                         1));
    accepts(O::SavedAttach, daemon::AccountAuditMutationState::None, {S::SpoolReady},
            stored_error("INPUT_CHANGED", "input file changed while being read",
                         {{"operation", "saved_attach"}, {"path", "/tmp/input"}}, 1));
    accepts(O::SavedAttach, daemon::AccountAuditMutationState::None, {S::SpoolReady},
            stored_error(
                "SPOOL_UNAVAILABLE", "attachment spool is unavailable",
                {{"operation", "saved_attach"}, {"path", "/tmp/input"}, {"reason", "write_failed"}},
                1));
    accepts(O::Send, daemon::AccountAuditMutationState::None, {},
            stored_error("PRECONDITION_FAILED", "operation precondition failed",
                         {{"operation", "send"},
                          {"chat_id", -1001},
                          {"message_id", nullptr},
                          {"reason", "schedule_window_elapsed"}},
                         1));
    accepts(O::SessionTerminate, daemon::AccountAuditMutationState::Possible, {S::DispatchStarted},
            stored_error("INTERNAL", "TDLib returned malformed session data",
                         {{"operation", "session_terminate"},
                          {"reason", "malformed_tdlib_response"},
                          {"tdlib_type_id", nullptr}},
                         1));

    std::string error;
    CHECK_FALSE(daemon::make_account_audit_checkpoint(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:01Z"},
         "main",
         O::Send,
         2,
         S::MutationConfirmed,
         {{"terminal", error_terminal(O::Send)}}},
        error));
    auto wrong_operation = error_terminal(O::MsgEdit);
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         O::Send,
         false,
         daemon::AccountAuditMutationState::None,
         {},
         wrong_operation},
        error));
}

TEST_CASE("account audit forward rate limits correlate empty and durable vectors exactly",
          "[account-audit][forward][rate-limit][scanner][regression]") {
    using M = daemon::AccountAuditMutationState;
    using S = daemon::AccountAuditStage;
    const auto failed_item = [](std::int64_t source_id, std::int64_t retry_after) {
        return json{{"source_id", source_id},
                    {"status", "failed"},
                    {"failure_reason", "tdlib_error"},
                    {"tdlib_code", 429},
                    {"retry_after", retry_after}};
    };
    const auto rate_limited = [](json items, std::int64_t retry_after) {
        return json{{"kind", "error"},
                    {"code", "RATE_LIMITED"},
                    {"message", "Telegram rate limit exceeded"},
                    {"details",
                     {{"operation", "msg_forward"},
                      {"tdlib_code", 429},
                      {"retry_after", retry_after},
                      {"items", std::move(items)}}},
                    {"exit_code", 5}};
    };
    const auto outcome = [](json terminal, M mutation, std::vector<S> stages) {
        std::string error;
        auto value = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:03Z"},
             "main",
             daemon::AccountAuditOperation::MsgForward,
             false,
             mutation,
             std::move(stages),
             std::move(terminal)},
            error);
        INFO(error);
        REQUIRE(value);
        CHECK(
            tgcli::test::matches_json_schema("audit-outcome.schema.json").match(value->document()));
        return *value;
    };
    const auto inspect = [](const daemon::AccountAuditOutcome& stored,
                            const std::optional<json>& progress) {
        AuditTree tree;
        append_line(tree, make_intent(daemon::AccountAuditOperation::MsgForward).document());
        append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                     daemon::AccountAuditStage::DispatchStarted, 1,
                                     {{"tdlib_function", "forwardMessages"},
                                      {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"client_generation", std::uint64_t{1}}})
                              .document());
        if (progress) {
            append_line(tree, checkpoint(daemon::AccountAuditOperation::MsgForward,
                                         daemon::AccountAuditStage::ForwardProgress, 2,
                                         {{"items", *progress}})
                                  .document());
        }
        append_line(tree, stored.document());
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        return log.inspect(guard).status;
    };

    const auto empty = outcome(rate_limited(json::array(), 9), M::Possible, {S::DispatchStarted});
    CHECK(inspect(empty, std::nullopt) == daemon::AccountAuditInspectionStatus::Clean);

    const json timeout_empty{{"kind", "error"},
                             {"code", "TIMEOUT"},
                             {"message", "request timed out"},
                             {"details",
                              {{"operation", "msg_forward"},
                               {"phase", "confirmation"},
                               {"state", "ready"},
                               {"outcome", "unknown"},
                               {"idempotency", "not_requested"},
                               {"items", json::array()}}},
                             {"exit_code", 7}};
    const auto unchanged_timeout = outcome(timeout_empty, M::Possible, {S::DispatchStarted});
    CHECK(inspect(unchanged_timeout, std::nullopt) ==
          daemon::AccountAuditInspectionStatus::Contradiction);

    const json one_failed = json::array({failed_item(1, 3)});
    const auto unexpected = outcome(rate_limited(one_failed, 3), M::Possible, {S::DispatchStarted});
    CHECK(inspect(unexpected, std::nullopt) == daemon::AccountAuditInspectionStatus::Contradiction);

    const auto exact =
        outcome(rate_limited(one_failed, 3), M::None, {S::DispatchStarted, S::ForwardProgress});
    CHECK(inspect(exact, one_failed) == daemon::AccountAuditInspectionStatus::Clean);

    const json altered = json::array({failed_item(1, 4)});
    const auto changed =
        outcome(rate_limited(altered, 4), M::None, {S::DispatchStarted, S::ForwardProgress});
    CHECK(inspect(changed, one_failed) == daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit stores spool failures only at saved attach spool prefixes",
          "[account-audit][spool][terminal][regression]") {
    using M = daemon::AccountAuditMutationState;
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    const auto spool_error = [](std::string operation) {
        return json{{"kind", "error"},
                    {"code", "SPOOL_UNAVAILABLE"},
                    {"message", "attachment spool is unavailable"},
                    {"details",
                     {{"operation", std::move(operation)},
                      {"path", "/tmp/input"},
                      {"reason", "write_failed"}}},
                    {"exit_code", 1}};
    };
    std::string error;
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         O::Send,
         false,
         M::None,
         {},
         spool_error("send")},
        error));

    const std::vector<std::vector<S>> prefixes{
        {}, {S::IdempotencyPending}, {S::SpoolReady}, {S::IdempotencyPending, S::SpoolReady}};
    for (const auto& prefix : prefixes) {
        CAPTURE(prefix.size());
        const bool keyed = std::ranges::find(prefix, S::IdempotencyPending) != prefix.end();
        auto intent =
            make_intent(O::SavedAttach, "0123456789abcdef0123456789abcdef",
                        keyed ? std::optional<std::string>{std::string(kKeyHash)} : std::nullopt);
        auto outcome = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
             "main",
             O::SavedAttach,
             false,
             M::None,
             prefix,
             spool_error("saved_attach")},
            error);
        INFO(error);
        REQUIRE(outcome);
        CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json")
                  .match(outcome->document()));

        AuditTree tree;
        append_line(tree, intent.document());
        std::uint32_t sequence = 1;
        for (const auto stage : prefix) {
            if (stage == S::IdempotencyPending) {
                append_line(tree, checkpoint(O::SavedAttach, stage, sequence++,
                                             {{"key_hash", kKeyHash},
                                              {"request_fingerprint", kFingerprint},
                                              {"expires_at", std::uint64_t{1}},
                                              {"reserved_terminal_bytes", std::uint64_t{32'768}}})
                                      .document());
            } else {
                append_line(tree, checkpoint(O::SavedAttach, stage, sequence++,
                                             {{"file", intent.document()["plan"]["file"]},
                                              {"relative_path",
                                               "spool/0123456789abcdef0123456789abcdef/input"}})
                                      .document());
            }
        }
        append_line(tree, outcome->document());
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        CHECK(log.inspect(guard).status == daemon::AccountAuditInspectionStatus::Clean);
    }

    const auto raw_outcome = [&](O operation, std::string mutation, json stages) {
        return json{{"schema_version", 2},
                    {"phase", "outcome"},
                    {"invocation_id", "0123456789abcdef0123456789abcdef"},
                    {"timestamp", "2026-08-19T12:00:02Z"},
                    {"account", "main"},
                    {"command", daemon::account_audit_operation_name(operation)},
                    {"success", false},
                    {"mutation_state", std::move(mutation)},
                    {"completed_stages", std::move(stages)},
                    {"terminal",
                     spool_error(std::string(daemon::account_audit_operation_name(operation)))}};
    };
    const auto scan_rejected = [](O operation, const std::vector<json>& checkpoints,
                                  const json& outcome) {
        AuditTree tree;
        append_line(tree, make_intent(operation).document());
        for (const auto& record : checkpoints) {
            append_line(tree, record);
        }
        append_line(tree, outcome);
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        CHECK(log.inspect(guard).status == daemon::AccountAuditInspectionStatus::Contradiction);
    };

    auto saved_intent = make_intent(O::SavedAttach);
    const auto spool =
        checkpoint(O::SavedAttach, S::SpoolReady, 1,
                   {{"file", saved_intent.document()["plan"]["file"]},
                    {"relative_path", "spool/0123456789abcdef0123456789abcdef/input"}})
            .document();
    const auto dispatch = checkpoint(O::SavedAttach, S::DispatchStarted, 2,
                                     {{"tdlib_function", "sendMessage"},
                                      {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"client_generation", std::uint64_t{1}}})
                              .document();
    auto after_dispatch =
        raw_outcome(O::SavedAttach, "possible", json::array({"spool_ready", "dispatch_started"}));
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         O::SavedAttach,
         false,
         M::Possible,
         {S::SpoolReady, S::DispatchStarted},
         spool_error("saved_attach")},
        error));
    CHECK_FALSE(
        tgcli::test::matches_json_schema("audit-outcome.schema.json").match(after_dispatch));
    scan_rejected(O::SavedAttach, {spool, dispatch}, after_dispatch);

    const auto proof = checkpoint(O::SavedAttach, S::MutationConfirmed, 3,
                                  {{"terminal", result_terminal(O::SavedAttach)}})
                           .document();
    auto after_proof =
        raw_outcome(O::SavedAttach, "confirmed",
                    json::array({"spool_ready", "dispatch_started", "mutation_confirmed"}));
    CHECK_FALSE(daemon::make_account_audit_outcome(
        {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
         "main",
         O::SavedAttach,
         false,
         M::Confirmed,
         {S::SpoolReady, S::DispatchStarted, S::MutationConfirmed},
         spool_error("saved_attach")},
        error));
    CHECK_FALSE(tgcli::test::matches_json_schema("audit-outcome.schema.json").match(after_proof));
    scan_rejected(O::SavedAttach, {spool, dispatch, proof}, after_proof);

    for (const auto operation : {O::Send, O::SessionTerminate}) {
        CAPTURE(daemon::account_audit_operation_name(operation));
        const auto wrong_operation = raw_outcome(operation, "none", json::array());
        CHECK_FALSE(daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
             "main",
             operation,
             false,
             M::None,
             {},
             spool_error(std::string(daemon::account_audit_operation_name(operation)))},
            error));
        CHECK_FALSE(
            tgcli::test::matches_json_schema("audit-outcome.schema.json").match(wrong_operation));
        scan_rejected(operation, {}, wrong_operation);
    }
}

TEST_CASE("account audit pin absence is reserved for session terminate policy",
          "[account-audit][pins][session][regression]") {
    SECTION("ordinary writes require an actual pin snapshot") {
        AuditTree tree;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::Send),
                                  daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt,
                                  failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    }
    SECTION("session terminate requires AbsentByPolicy") {
        AuditTree tree;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(append_intent(log, make_intent(daemon::AccountAuditOperation::SessionTerminate),
                                  daemon::KnownAccountAuditPins{}, guard, receipt, failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    }
    SECTION("session terminate accepts only non-evicting AbsentByPolicy capacity") {
        AuditTree tree;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK(append_intent(log, make_intent(daemon::AccountAuditOperation::SessionTerminate),
                            daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt, failure));
    }
}

TEST_CASE("account audit enforces file path roles and positive-int32 session time",
          "[account-audit][contract][path][session][regression]") {
    const auto make_saved = [](json args, json immutable_plan) {
        std::string error;
        return daemon::make_account_audit_intent(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:00Z"},
             "main",
             daemon::AccountAuditOperation::SavedAttach,
             std::move(args),
             std::move(immutable_plan),
             std::string(kFingerprint),
             std::string(kSnapshot),
             "request",
             std::nullopt,
             std::nullopt,
             100},
            error);
    };

    auto args = arguments(daemon::AccountAuditOperation::SavedAttach);
    auto immutable_plan = plan(daemon::AccountAuditOperation::SavedAttach);
    args["path"] = "relative//./input";
    immutable_plan["file"]["path"] = "/cwd/relative/input";
    auto accepted = make_saved(args, immutable_plan);
    REQUIRE(accepted);
    CHECK(accepted->document()["arguments"]["path"] == "relative//./input");
    CHECK(accepted->document()["plan"]["file"]["path"] == "/cwd/relative/input");

    for (const auto& invalid : {"relative/input", "/tmp//input", "/tmp/./input", "/tmp/../input",
                                "/tmp/input/", "/tmp/other"}) {
        args = arguments(daemon::AccountAuditOperation::SavedAttach);
        immutable_plan = plan(daemon::AccountAuditOperation::SavedAttach);
        immutable_plan["file"]["path"] = invalid;
        CHECK_FALSE(make_saved(args, immutable_plan));
    }
    args = arguments(daemon::AccountAuditOperation::SavedAttach);
    immutable_plan = plan(daemon::AccountAuditOperation::SavedAttach);
    immutable_plan["file"]["path"] = "/" + std::string(4'096, 'a');
    CHECK_FALSE(make_saved(args, immutable_plan));

    for (const auto& invalid : {std::string("input/"), std::string(4'097, 'a')}) {
        args = arguments(daemon::AccountAuditOperation::SavedAttach);
        immutable_plan = plan(daemon::AccountAuditOperation::SavedAttach);
        args["path"] = invalid;
        CHECK_FALSE(make_saved(args, immutable_plan));
    }

    const auto make_session = [](std::string timestamp) {
        auto immutable_plan = plan(daemon::AccountAuditOperation::SessionTerminate);
        immutable_plan["session"]["last_active_date"] = std::move(timestamp);
        std::string error;
        return daemon::make_account_audit_intent(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:00Z"},
             "main",
             daemon::AccountAuditOperation::SessionTerminate,
             arguments(daemon::AccountAuditOperation::SessionTerminate),
             std::move(immutable_plan),
             std::string(kFingerprint),
             std::string(kSnapshot),
             "request",
             std::optional<std::string>{"yes"},
             std::nullopt,
             100},
            error);
    };
    CHECK(make_session("1970-01-01T00:00:01Z").has_value());
    CHECK(make_session("2038-01-19T03:14:07Z").has_value());
    CHECK_FALSE(make_session("1970-01-01T00:00:00Z"));
    CHECK_FALSE(make_session("2038-01-19T03:14:08Z"));
}

TEST_CASE("account audit streams immutable completed views across every generation",
          "[account-audit][completed-view][stream][generation]") {
    AuditTree tree;
    const std::array invocations{hex_invocation(1), hex_invocation(2), hex_invocation(3),
                                 hex_invocation(4), hex_invocation(5)};
    tree.write(".4", json_lines(complete_v2_records(invocations[0])));
    tree.write(".3", json_lines(complete_saved_attach_records(invocations[1])));
    tree.write(".2", json_lines(complete_v2_records(invocations[2])));
    tree.write(".1", json_lines(complete_forward_records(invocations[3])));
    tree.write({}, json_lines(complete_v2_records(invocations[4])));

    std::array<std::uint64_t, 5> generations{};
    for (std::size_t index = 0; index < generations.size(); ++index) {
        const auto suffix = index == 4 ? std::string{} : "." + std::to_string(4 - index);
        struct stat metadata {};
        REQUIRE(::lstat(tree.audit(suffix).c_str(), &metadata) == 0);
        generations.at(index) = static_cast<std::uint64_t>(metadata.st_ino);
    }

    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    daemon::AccountAuditAppendPermit permit;
    std::vector<daemon::AccountAuditCompletedGroupView> completed;
    const auto inspection = log.prepare_append(
        make_intent(daemon::AccountAuditOperation::Send, hex_invocation(15)),
        daemon::KnownAccountAuditPins{}, guard, permit,
        [&](const daemon::AccountAuditCompletedGroupView& view) { completed.push_back(view); });
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Clean);
    CHECK(permit.valid());
    REQUIRE(completed.size() == invocations.size());
    for (std::size_t index = 0; index < completed.size(); ++index) {
        CHECK(completed.at(index).invocation_id == invocations.at(index));
        CHECK(completed.at(index).audit_generation == generations.at(index));
        CHECK(completed.at(index).account == "main");
        CHECK(completed.at(index).request_fingerprint == kFingerprint);
        CHECK(completed.at(index).intent_timestamp == "2026-08-19T12:00:00Z");
        CHECK(completed.at(index).intent_unix_seconds == 1'787'140'800);
        REQUIRE(completed.at(index).outcome);
    }
    const auto& saved = completed[1];
    CHECK(saved.operation == daemon::AccountAuditOperation::SavedAttach);
    CHECK(saved.idempotency_key_hash == kKeyHash);
    CHECK(saved.plan == plan(daemon::AccountAuditOperation::SavedAttach));
    REQUIRE(saved.idempotency_pending);
    CHECK((*saved.idempotency_pending)["reserved_terminal_bytes"] == 65'536);
    REQUIRE(saved.spool);
    CHECK(saved.spool->relative_path == "spool/" + invocations[1] + "/input");
    CHECK(saved.temporary_message_ids == json::array({-1}));
    CHECK(saved.forward_progress == json::array());
    REQUIRE(saved.mutation_proof);
    CHECK((*saved.mutation_proof)["terminal"] ==
          result_terminal(daemon::AccountAuditOperation::SavedAttach));

    const auto& forward = completed[3];
    CHECK(forward.operation == daemon::AccountAuditOperation::MsgForward);
    CHECK(forward.temporary_message_ids == json::array({-1}));
    CHECK(forward.forward_progress ==
          result_terminal(daemon::AccountAuditOperation::MsgForward)["data"]["items"]);
    CHECK(forward.completed_stages == std::vector{daemon::AccountAuditStage::IdempotencyPending,
                                                  daemon::AccountAuditStage::DispatchStarted,
                                                  daemon::AccountAuditStage::TemporaryIdsObserved,
                                                  daemon::AccountAuditStage::ForwardProgress,
                                                  daemon::AccountAuditStage::MutationConfirmed});
}

TEST_CASE("account audit open groups expose their intent generation",
          "[account-audit][open][generation]") {
    AuditTree tree;
    tree.write({}, make_intent(daemon::AccountAuditOperation::Send).document().dump() + "\n");
    struct stat metadata {};
    REQUIRE(::lstat(tree.audit().c_str(), &metadata) == 0);
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    const auto inspection = log.inspect(guard);
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Open);
    REQUIRE(inspection.oldest_open);
    CHECK(inspection.oldest_open->audit_generation == static_cast<std::uint64_t>(metadata.st_ino));
}

TEST_CASE("account audit completed views preserve keyed outcomes without store policy",
          "[account-audit][completed-view][store-policy]") {
    using M = daemon::AccountAuditMutationState;
    using S = daemon::AccountAuditStage;
    const auto removed_invocation = hex_invocation(20);
    const auto retained_invocation = hex_invocation(21);
    const json pending{{"key_hash", kKeyHash},
                       {"request_fingerprint", kFingerprint},
                       {"expires_at", std::uint64_t{1}},
                       {"reserved_terminal_bytes", std::uint64_t{32'768}}};
    const json dispatch{{"tdlib_function", "sendMessage"},
                        {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                        {"client_generation", std::uint64_t{1}}};
    const json shutdown{{"kind", "error"},
                        {"code", "DAEMON_SHUTDOWN"},
                        {"message", "daemon is shutting down"},
                        {"details", {{"reason", "daemon_shutdown"}}},
                        {"exit_code", 1}};
    std::vector<json> records;
    records.push_back(
        make_intent(daemon::AccountAuditOperation::Send, removed_invocation, std::string(kKeyHash))
            .document());
    records.push_back(checkpoint_record(daemon::AccountAuditOperation::Send, S::IdempotencyPending,
                                        1, pending, removed_invocation));
    records.push_back(outcome_record(daemon::AccountAuditOperation::Send, removed_invocation,
                                     M::None, {S::IdempotencyPending}, error_terminal()));
    records.push_back(
        make_intent(daemon::AccountAuditOperation::Send, retained_invocation, std::string(kKeyHash))
            .document());
    records.push_back(checkpoint_record(daemon::AccountAuditOperation::Send, S::IdempotencyPending,
                                        1, pending, retained_invocation));
    records.push_back(checkpoint_record(daemon::AccountAuditOperation::Send, S::DispatchStarted, 2,
                                        dispatch, retained_invocation));
    records.push_back(outcome_record(daemon::AccountAuditOperation::Send, retained_invocation,
                                     M::Possible, {S::IdempotencyPending, S::DispatchStarted},
                                     shutdown));

    AuditTree tree;
    tree.write({}, json_lines(records));
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    daemon::AccountAuditAppendPermit permit;
    std::vector<daemon::AccountAuditCompletedGroupView> completed;
    const auto inspection = log.prepare_append(
        make_intent(daemon::AccountAuditOperation::Send, hex_invocation(22)),
        daemon::KnownAccountAuditPins{}, guard, permit,
        [&](const daemon::AccountAuditCompletedGroupView& view) { completed.push_back(view); });
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Clean);
    REQUIRE(completed.size() == 2);
    static_assert(!HasAuditStoreDisposition<daemon::AccountAuditCompletedGroupView>);
    REQUIRE(completed[0].outcome);
    REQUIRE(completed[1].outcome);
    CHECK((*completed[0].outcome)["mutation_state"] == "none");
    CHECK((*completed[1].outcome)["mutation_state"] == "possible");
    CHECK(completed[1].idempotency_pending == pending);
}

TEST_CASE("account audit completed views cannot prescribe keyed store transitions",
          "[account-audit][completed-view][review-red]") {
    using M = daemon::AccountAuditMutationState;
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    const auto incumbent_invocation = hex_invocation(23);
    const auto forward_invocation = hex_invocation(24);
    const json incumbent_pending{{"key_hash", kKeyHash},
                                 {"request_fingerprint", kFingerprint},
                                 {"expires_at", std::uint64_t{1}},
                                 {"reserved_terminal_bytes", std::uint64_t{32'768}}};
    const json forward_pending{{"key_hash", kKeyHash},
                               {"request_fingerprint", kFingerprint},
                               {"expires_at", std::uint64_t{1}},
                               {"reserved_terminal_bytes", std::uint64_t{4'194'304}}};
    const json dispatch{{"tdlib_function", "forwardMessages"},
                        {"dispatch_token", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                        {"client_generation", std::uint64_t{1}}};
    const json temporary{{"temporary_message_ids", json::array({-1})}};
    const json partial{
        {"items",
         json::array(
             {json{{"source_id", 1}, {"status", "sent"}, {"message", message_write()}},
              json{{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", -1}}})}};

    auto forward_intent =
        make_intent(O::MsgForward, forward_invocation, std::string(kKeyHash)).document();
    forward_intent["arguments"]["message_ids"] = json::array({1, 2});
    forward_intent["plan"]["message_ids"] = json::array({1, 2});
    const std::vector<S> incumbent_stages{S::IdempotencyPending};
    const std::vector<S> forward_stages{S::IdempotencyPending, S::DispatchStarted,
                                        S::TemporaryIdsObserved, S::ForwardProgress};
    const std::vector<json> records{
        make_intent(O::Send, incumbent_invocation, std::string(kKeyHash)).document(),
        checkpoint_record(O::Send, S::IdempotencyPending, 1, incumbent_pending,
                          incumbent_invocation),
        outcome_record(O::Send, incumbent_invocation, M::None, incumbent_stages,
                       audit_incomplete_terminal("/tmp/audit.log", M::None, incumbent_stages)),
        forward_intent,
        checkpoint_record(O::MsgForward, S::IdempotencyPending, 1, forward_pending,
                          forward_invocation),
        checkpoint_record(O::MsgForward, S::DispatchStarted, 2, dispatch, forward_invocation),
        checkpoint_record(O::MsgForward, S::TemporaryIdsObserved, 3, temporary, forward_invocation),
        checkpoint_record(O::MsgForward, S::ForwardProgress, 4, partial, forward_invocation),
        outcome_record(O::MsgForward, forward_invocation, M::Confirmed, forward_stages,
                       audit_incomplete_terminal("/tmp/audit.log", M::Confirmed, forward_stages))};

    AuditTree tree;
    tree.write({}, json_lines(records));
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
    auto guard = tree.coordinator().lock();
    daemon::AccountAuditAppendPermit permit;
    std::vector<daemon::AccountAuditCompletedGroupView> completed;
    const auto inspection = log.prepare_append(
        make_intent(O::Send, hex_invocation(25)), daemon::KnownAccountAuditPins{}, guard, permit,
        [&](const daemon::AccountAuditCompletedGroupView& view) { completed.push_back(view); });
    REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Clean);
    REQUIRE(completed.size() == 2);
    static_assert(!HasAuditStoreDisposition<daemon::AccountAuditCompletedGroupView>);
    REQUIRE(completed[0].outcome);
    REQUIRE(completed[1].outcome);
    CHECK((*completed[0].outcome)["mutation_state"] == "none");
    CHECK((*completed[1].outcome)["mutation_state"] == "confirmed");
    CHECK(completed[0].idempotency_pending == incumbent_pending);
    CHECK(completed[1].idempotency_pending == forward_pending);
    CHECK(completed[1].forward_progress == partial["items"]);
}

TEST_CASE("account audit validates bounded pin indexes in one segment pass",
          "[account-audit][pins][one-pass][bounded]") {
    AuditTree tree;
    const auto existing = hex_invocation(42);
    tree.write({}, json_lines(complete_v2_records(existing)));
    struct stat metadata {};
    REQUIRE(::lstat(tree.audit().c_str(), &metadata) == 0);
    const auto generation = static_cast<std::uint64_t>(metadata.st_ino);
    std::atomic<unsigned> segment_scans = 0;
    auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
    hooks->before_segment_scan = [&](std::string_view) {
        segment_scans.fetch_add(1, std::memory_order_relaxed);
    };
    daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
    auto guard = tree.coordinator().lock();
    const auto next = make_intent(daemon::AccountAuditOperation::Send, hex_invocation(43));

    daemon::AccountAuditAppendPermit one_permit;
    const daemon::KnownAccountAuditPins one{
        {{generation, existing, std::string(kFingerprint), daemon::AccountAuditOperation::Send}}};
    CHECK(log.prepare_append(next, one, guard, one_permit).status ==
          daemon::AccountAuditInspectionStatus::Clean);
    CHECK(segment_scans.load(std::memory_order_relaxed) == 1);

    std::vector<daemon::AccountAuditPin> many;
    many.reserve(10'000);
    many.push_back(one.pins.front());
    for (std::uint64_t index = 1; index < 10'000; ++index) {
        many.push_back({generation, hex_invocation(1'000 + index), std::string(kFingerprint),
                        daemon::AccountAuditOperation::Send});
    }
    daemon::AccountAuditAppendPermit many_permit;
    const auto before = segment_scans.load(std::memory_order_relaxed);
    const auto many_inspection = log.prepare_append(
        next, daemon::KnownAccountAuditPins{std::move(many)}, guard, many_permit);
    CHECK(many_inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
    CHECK(segment_scans.load(std::memory_order_relaxed) == before + 1);
    CHECK_FALSE(many_permit.valid());

    daemon::AccountAuditAppendPermit duplicate_permit;
    CHECK(log.prepare_append(next,
                             daemon::KnownAccountAuditPins{{one.pins.front(), one.pins.front()}},
                             guard, duplicate_permit)
              .status == daemon::AccountAuditInspectionStatus::Contradiction);
    daemon::AccountAuditAppendPermit mismatch_permit;
    auto mismatch = one.pins.front();
    mismatch.request_fingerprint =
        "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    CHECK(
        log.prepare_append(next, daemon::KnownAccountAuditPins{{mismatch}}, guard, mismatch_permit)
            .status == daemon::AccountAuditInspectionStatus::Contradiction);
}

TEST_CASE("account audit spool holds require durable cleanup receipts before eviction",
          "[account-audit][spool-hold][rotation][receipt][review-red]") {
    static_assert(!std::is_copy_constructible_v<daemon::AccountAuditAppendPermit>);
    static_assert(!std::is_copy_constructible_v<daemon::AccountAuditSpoolHold>);
    static_assert(!std::is_copy_constructible_v<daemon::AccountAuditSpoolReleaseReceipt>);

    const auto exercise = [](bool release) {
        AuditTree tree;
        const auto spool_invocation = hex_invocation(100);
        tree.write(".4", json_lines(complete_saved_attach_records(spool_invocation)));
        tree.write(".3", json_lines(complete_v2_records(hex_invocation(101))));
        tree.write(".2", json_lines(complete_v2_records(hex_invocation(102))));
        tree.write(".1", json_lines(complete_v2_records(hex_invocation(103))));
        tree.write({}, json_lines(complete_v2_records(hex_invocation(104))));
        create_spool_object(tree, spool_invocation);
        struct stat oldest {};
        REQUIRE(::lstat(tree.audit(".4").c_str(), &oldest) == 0);

        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->rotation_bytes = 1;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        auto guard = tree.coordinator().lock();
        const auto next = make_intent(daemon::AccountAuditOperation::Send, hex_invocation(105));
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(next, daemon::KnownAccountAuditPins{}, guard, permit).status ==
                daemon::AccountAuditInspectionStatus::Clean);
        auto holds = permit.issue_spool_holds();
        REQUIRE(holds.size() == 1);
        CHECK(holds.front().audit_generation() == static_cast<std::uint64_t>(oldest.st_ino));
        CHECK(holds.front().invocation_id() == spool_invocation);
        CHECK(permit.issue_spool_holds().empty());
        if (release) {
            auto cleanup = daemon::cleanup_spool_file_with_hold(std::move(holds.front()), guard);
            REQUIRE(std::holds_alternative<daemon::AccountAuditSpoolReleaseReceipt>(cleanup));
            daemon::AccountAuditFailure release_failure;
            REQUIRE(permit.release_spool_hold(
                std::move(std::get<daemon::AccountAuditSpoolReleaseReceipt>(cleanup)), guard,
                release_failure));
        }
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        REQUIRE(log.append_intent(next, std::exchange(permit, daemon::AccountAuditAppendPermit{}),
                                  guard, receipt, failure));
        bool oldest_survived = false;
        for (const auto* suffix : {".1", ".2", ".3", ".4"}) {
            struct stat observed {};
            if (::lstat(tree.audit(suffix).c_str(), &observed) == 0 &&
                observed.st_ino == oldest.st_ino) {
                oldest_survived = true;
            }
        }
        CHECK(oldest_survived == !release);
    };

    SECTION("unreleased hold protects its generation") {
        exercise(false);
    }
    SECTION("cleanup and root fsync receipt releases its generation") {
        exercise(true);
    }
    SECTION("an absent spool root cannot mint a release receipt") {
        AuditTree tree;
        const auto spool_invocation = hex_invocation(110);
        tree.write({}, json_lines(complete_saved_attach_records(spool_invocation)));
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto next = make_intent(daemon::AccountAuditOperation::Send, hex_invocation(111));
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(next, daemon::KnownAccountAuditPins{}, guard, permit).status ==
                daemon::AccountAuditInspectionStatus::Clean);
        auto holds = permit.issue_spool_holds();
        REQUIRE(holds.size() == 1);
        auto cleanup = daemon::cleanup_spool_file_with_hold(std::move(holds.front()), guard);
        REQUIRE(std::holds_alternative<daemon::FileSpoolError>(cleanup));
        const auto& error = std::get<daemon::FileSpoolError>(cleanup);
        CHECK(error.kind == daemon::FileSpoolErrorKind::Contradiction);
        CHECK(error.durability_reason == daemon::DurabilityReason::Contradiction);
    }

    SECTION("a hold cannot clean another account root") {
        AuditTree first;
        AuditTree second;
        const auto spool_invocation = hex_invocation(112);
        first.write({}, json_lines(complete_saved_attach_records(spool_invocation)));
        create_spool_object(first, spool_invocation);
        create_spool_object(second, spool_invocation);
        daemon::AccountAuditLog log(first.state(), "main", ::getuid());
        auto guard = first.coordinator().lock();
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(
                       make_intent(daemon::AccountAuditOperation::Send, hex_invocation(113)),
                       daemon::KnownAccountAuditPins{}, guard, permit)
                    .status == daemon::AccountAuditInspectionStatus::Clean);
        auto holds = permit.issue_spool_holds();
        REQUIRE(holds.size() == 1);
        auto wrong_guard = second.coordinator().lock();
        auto cleanup = daemon::cleanup_spool_file_with_hold(std::move(holds.front()), wrong_guard);
        CHECK(std::holds_alternative<daemon::FileSpoolError>(cleanup));
        CHECK(std::filesystem::exists(first.state() + "/spool/" + spool_invocation + "/input"));
        CHECK(std::filesystem::exists(second.state() + "/spool/" + spool_invocation + "/input"));

        daemon::AccountAuditAppendPermit receipt_permit;
        REQUIRE(log.prepare_append(
                       make_intent(daemon::AccountAuditOperation::Send, hex_invocation(114)),
                       daemon::KnownAccountAuditPins{}, guard, receipt_permit)
                    .status == daemon::AccountAuditInspectionStatus::Clean);
        auto receipt_holds = receipt_permit.issue_spool_holds();
        REQUIRE(receipt_holds.size() == 1);
        auto correct_cleanup =
            daemon::cleanup_spool_file_with_hold(std::move(receipt_holds.front()), guard);
        REQUIRE(std::holds_alternative<daemon::AccountAuditSpoolReleaseReceipt>(correct_cleanup));
        daemon::AccountAuditFailure release_failure;
        CHECK_FALSE(receipt_permit.release_spool_hold(
            std::move(std::get<daemon::AccountAuditSpoolReleaseReceipt>(correct_cleanup)),
            wrong_guard, release_failure));
        CHECK(release_failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
        CHECK(std::filesystem::exists(second.state() + "/spool/" + spool_invocation + "/input"));
    }
}

TEST_CASE("account audit open spool groups expose typed recovery holds",
          "[account-audit][spool-hold][recovery][review-red]") {
    using O = daemon::AccountAuditOperation;
    using S = daemon::AccountAuditStage;
    static_assert(!std::is_copy_constructible_v<daemon::AccountAuditRecoveryPermit>);
    for (const bool durable_proof : {false, true}) {
        AuditTree tree;
        const auto invocation = hex_invocation(durable_proof ? 121 : 120);
        auto records = complete_saved_attach_records(invocation);
        records.pop_back();
        if (!durable_proof) {
            records.resize(3);
        }
        tree.write({}, json_lines(records));
        create_spool_object(tree, invocation);
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        daemon::AccountAuditRecoveryPermit permit;
        const daemon::KnownAccountAuditPins pins;
        const auto inspection = log.prepare_recovery(pins, guard, permit);
        REQUIRE(inspection.status == daemon::AccountAuditInspectionStatus::Open);
        CHECK(permit.valid());
        REQUIRE(inspection.oldest_open);
        CHECK(inspection.oldest_open->has_spool);
        CHECK(inspection.oldest_open->mutation_confirmed == durable_proof);
        CHECK(inspection.oldest_open->completed_stages.front() == S::IdempotencyPending);
        auto holds = permit.issue_spool_holds();
        REQUIRE(holds.size() == 1);
        CHECK(holds.front().invocation_id() == invocation);

        std::string error;
        auto plan = daemon::classify_account_audit_recovery(*inspection.oldest_open, "main",
                                                            log.path(), pins, error);
        INFO(error);
        REQUIRE(plan);
        REQUIRE_FALSE(plan->boundaries.empty());
        CHECK(plan->boundaries.front() ==
              (durable_proof ? daemon::AccountAuditRecoveryBoundary::AppendOutcomeAndSync
                             : daemon::AccountAuditRecoveryBoundary::DeleteSpoolAndSyncRoot));
        auto outcome = daemon::make_account_audit_outcome({{invocation, "2026-08-19T12:00:07Z"},
                                                           "main",
                                                           O::SavedAttach,
                                                           plan->terminal["kind"] == "result",
                                                           plan->mutation_state,
                                                           inspection.oldest_open->completed_stages,
                                                           plan->terminal},
                                                          error);
        INFO(error);
        REQUIRE(outcome);

        daemon::AccountAuditFailure failure;
        const auto cleanup_and_release = [&] {
            auto cleanup = daemon::cleanup_spool_file_with_hold(std::move(holds.front()), guard);
            REQUIRE(std::holds_alternative<daemon::AccountAuditSpoolReleaseReceipt>(cleanup));
            REQUIRE(permit.release_spool_hold(
                std::move(std::get<daemon::AccountAuditSpoolReleaseReceipt>(cleanup)), guard,
                failure));
        };
        if (durable_proof) {
            REQUIRE(log.append_outcome(*outcome, guard, failure));
            cleanup_and_release();
        } else {
            cleanup_and_release();
            REQUIRE(log.append_outcome(*outcome, guard, failure));
        }
        CHECK_FALSE(std::filesystem::exists(tree.state() + "/spool/" + invocation));
        CHECK(log.inspect(guard).status == daemon::AccountAuditInspectionStatus::Clean);
    }

    AuditTree policy_tree;
    const auto policy_invocation = hex_invocation(124);
    auto policy_records = complete_saved_attach_records(policy_invocation);
    policy_records.resize(3);
    policy_tree.write({}, json_lines(policy_records));
    create_spool_object(policy_tree, policy_invocation);
    daemon::AccountAuditLog policy_log(policy_tree.state(), "main", ::getuid());
    auto policy_guard = policy_tree.coordinator().lock();
    daemon::AccountAuditRecoveryPermit policy_permit;
    const auto policy_inspection = policy_log.prepare_recovery(
        daemon::AbsentAccountAuditPinsByPolicy{}, policy_guard, policy_permit);
    CHECK(policy_inspection.status == daemon::AccountAuditInspectionStatus::Contradiction);
    CHECK_FALSE(policy_permit.valid());
    CHECK(std::filesystem::exists(policy_tree.state() + "/spool/" + policy_invocation + "/input"));
}

TEST_CASE("account audit permits revalidate segments and narrow only validated pins",
          "[account-audit][permit][pins][replacement]") {
    SECTION("segment replacement invalidates the permit before rotation") {
        AuditTree tree;
        tree.write({}, json_lines(complete_v2_records(hex_invocation(200))));
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        auto guard = tree.coordinator().lock();
        const auto next = make_intent(daemon::AccountAuditOperation::Send, hex_invocation(201));
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(next, daemon::KnownAccountAuditPins{}, guard, permit).status ==
                daemon::AccountAuditInspectionStatus::Clean);
        REQUIRE_NOTHROW(std::filesystem::rename(tree.audit(), tree.audit(".replaced")));
        tree.write({}, json_lines(complete_v2_records(hex_invocation(202))));
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(log.append_intent(next,
                                      std::exchange(permit, daemon::AccountAuditAppendPermit{}),
                                      guard, receipt, failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::Contradiction);
    }

    SECTION("surviving pins are an exact unique subset") {
        AuditTree tree;
        const std::array invocations{hex_invocation(210), hex_invocation(211), hex_invocation(212),
                                     hex_invocation(213), hex_invocation(214)};
        tree.write(".4", json_lines(complete_v2_records(invocations[0])));
        tree.write(".3", json_lines(complete_v2_records(invocations[1])));
        tree.write(".2", json_lines(complete_v2_records(invocations[2])));
        tree.write(".1", json_lines(complete_v2_records(invocations[3])));
        tree.write({}, json_lines(complete_v2_records(invocations[4])));
        struct stat fourth {};
        struct stat third {};
        REQUIRE(::lstat(tree.audit(".4").c_str(), &fourth) == 0);
        REQUIRE(::lstat(tree.audit(".3").c_str(), &third) == 0);
        const daemon::AccountAuditPin pin4{static_cast<std::uint64_t>(fourth.st_ino),
                                           invocations[0], std::string(kFingerprint),
                                           daemon::AccountAuditOperation::Send};
        const daemon::AccountAuditPin pin3{static_cast<std::uint64_t>(third.st_ino), invocations[1],
                                           std::string(kFingerprint),
                                           daemon::AccountAuditOperation::Send};
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->rotation_bytes = 1;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        auto guard = tree.coordinator().lock();
        const auto next = make_intent(daemon::AccountAuditOperation::Send, hex_invocation(215));
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(next, daemon::KnownAccountAuditPins{{pin4, pin3}}, guard, permit)
                    .status == daemon::AccountAuditInspectionStatus::Clean);
        daemon::AccountAuditFailure failure;
        auto foreign = pin4;
        foreign.invocation_id = hex_invocation(999);
        CHECK_FALSE(permit.narrow_pins({foreign}, failure));
        REQUIRE(permit.narrow_pins({pin4}, failure));
        daemon::AccountAuditAppendReceipt receipt;
        REQUIRE(log.append_intent(next, std::exchange(permit, daemon::AccountAuditAppendPermit{}),
                                  guard, receipt, failure));
        struct stat retained {};
        REQUIRE(::lstat(tree.audit(".4").c_str(), &retained) == 0);
        CHECK(retained.st_ino == fourth.st_ino);
        bool third_survived = false;
        for (const auto* suffix : {".1", ".2", ".3", ".4"}) {
            struct stat observed {};
            if (::lstat(tree.audit(suffix).c_str(), &observed) == 0 &&
                observed.st_ino == third.st_ino) {
                third_survived = true;
            }
        }
        CHECK_FALSE(third_survived);
    }
}

TEST_CASE("account audit AbsentByPolicy rotation is hole-only",
          "[account-audit][absent-by-policy][rotation][session]") {
    SECTION("a missing numbered slot is consumed without eviction") {
        AuditTree tree;
        tree.write(".3", json_lines(complete_v2_records(hex_invocation(301))));
        tree.write(".2", json_lines(complete_v2_records(hex_invocation(302))));
        tree.write(".1", json_lines(complete_v2_records(hex_invocation(303))));
        tree.write({}, json_lines(complete_v2_records(hex_invocation(304))));
        std::vector<std::string> steps;
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->rotation_bytes = 1;
        hooks->after_rotation_step = [&](std::string_view step) { steps.emplace_back(step); };
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        auto guard = tree.coordinator().lock();
        const auto next =
            make_intent(daemon::AccountAuditOperation::SessionTerminate, hex_invocation(305));
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(next, daemon::AbsentAccountAuditPinsByPolicy{}, guard, permit)
                    .status == daemon::AccountAuditInspectionStatus::Clean);
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK(log.append_intent(next, std::exchange(permit, daemon::AccountAuditAppendPermit{}),
                                guard, receipt, failure));
        CHECK(std::ranges::find(steps, "unlink") == steps.end());
    }
    SECTION("a full set never evicts without pin knowledge") {
        AuditTree tree;
        for (std::size_t slot = 1; slot <= 4; ++slot) {
            tree.write("." + std::to_string(slot),
                       json_lines(complete_v2_records(hex_invocation(310 + slot))));
        }
        tree.write({}, json_lines(complete_v2_records(hex_invocation(315))));
        auto hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
        hooks->rotation_bytes = 1;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid(), hooks);
        auto guard = tree.coordinator().lock();
        const auto next =
            make_intent(daemon::AccountAuditOperation::SessionTerminate, hex_invocation(316));
        daemon::AccountAuditAppendPermit permit;
        REQUIRE(log.prepare_append(next, daemon::AbsentAccountAuditPinsByPolicy{}, guard, permit)
                    .status == daemon::AccountAuditInspectionStatus::Clean);
        daemon::AccountAuditAppendReceipt receipt;
        daemon::AccountAuditFailure failure;
        CHECK_FALSE(log.append_intent(next,
                                      std::exchange(permit, daemon::AccountAuditAppendPermit{}),
                                      guard, receipt, failure));
        CHECK(failure.reason == daemon::AccountAuditDurabilityReason::CapacityExhausted);
    }
}

TEST_CASE("account audit controlled mutex acquisition obeys deadline and cancellation",
          "[account-audit][lock][deadline][cancellation][concurrency]") {
    SECTION("default scan control carries the shared unlimited deadline") {
        daemon::AccountAuditScanControl control;
        CHECK_FALSE(control.deadline.expires_at);
    }

    SECTION("deadline before and at acquisition fails before taking a free mutex") {
        AuditTree tree;
        for (const auto deadline :
             {std::chrono::steady_clock::now() - 1ms, std::chrono::steady_clock::now()}) {
            daemon::AccountAuditScanControl control;
            control.deadline = RequestDeadline{deadline};
            auto result = tree.coordinator().lock(std::move(control));
            REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
            const auto& failure = std::get<daemon::AccountAuditFailure>(result);
            REQUIRE(failure.interruption);
            CHECK(*failure.interruption == daemon::AccountAuditFailure::Interruption::Deadline);
        }
    }

    SECTION("deadline expires while another epoch remains held") {
        AuditTree tree;
        auto first = tree.coordinator().lock();
        auto waiter = std::async(std::launch::async, [&] {
            daemon::AccountAuditScanControl control;
            control.deadline = RequestDeadline{std::chrono::steady_clock::now() + 30ms};
            return tree.coordinator().lock(std::move(control));
        });
        REQUIRE(waiter.wait_for(500ms) == std::future_status::ready);
        auto result = waiter.get();
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        CHECK(std::get<daemon::AccountAuditFailure>(result).interruption ==
              daemon::AccountAuditFailure::Interruption::Deadline);
        CHECK(first.valid());
    }

    SECTION("cancellation interrupts a queued acquisition") {
        AuditTree tree;
        auto first = tree.coordinator().lock();
        std::atomic<bool> cancelled = false;
        auto waiter = std::async(std::launch::async, [&] {
            daemon::AccountAuditScanControl control;
            control.cancelled = [&] { return cancelled.load(std::memory_order_acquire); };
            return tree.coordinator().lock(std::move(control));
        });
        std::this_thread::sleep_for(10ms);
        cancelled.store(true, std::memory_order_release);
        REQUIRE(waiter.wait_for(500ms) == std::future_status::ready);
        auto result = waiter.get();
        REQUIRE(std::holds_alternative<daemon::AccountAuditFailure>(result));
        CHECK(std::get<daemon::AccountAuditFailure>(result).interruption ==
              daemon::AccountAuditFailure::Interruption::Cancelled);
        CHECK(first.valid());
    }
}

// NOLINTEND(misc-const-correctness)
