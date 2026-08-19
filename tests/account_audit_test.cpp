#include "common/daemon_lock.hpp"
#include "daemon/account_audit.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <atomic>
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
#include <thread>
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
    REQUIRE(log.append_intent(intent, daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt,
                              failure));
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
        const auto pins =
            mask == 15 ? daemon::AccountAuditPinSource{daemon::KnownAccountAuditPins{}}
                       : daemon::AccountAuditPinSource{daemon::AbsentAccountAuditPinsByPolicy{}};
        REQUIRE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send), pins, guard,
                                  receipt, failure));
        unsigned after = 0;
        for (unsigned slot = 1; slot <= 4; ++slot) {
            after += std::filesystem::exists(tree.audit("." + std::to_string(slot))) ? 1U : 0U;
        }
        CHECK(after == std::min(4U, before + 1));
    }
}

TEST_CASE("account audit full rotation refuses absent pins and preserves pinned inodes",
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
    CHECK_FALSE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send),
                                  daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt,
                                  failure));
    CHECK(failure.reason == daemon::AccountAuditDurabilityReason::CapacityExhausted);

    struct stat pinned {};
    REQUIRE(::stat(tree.audit(".4").c_str(), &pinned) == 0);
    const auto invocation = std::string(31, '4') + "a";
    daemon::KnownAccountAuditPins pins{
        {{static_cast<std::uint64_t>(pinned.st_ino), invocation, std::string(kFingerprint),
          daemon::AccountAuditOperation::Send}}};
    REQUIRE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send), pins, guard,
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
        const bool appended = log.append_intent(make_intent(daemon::AccountAuditOperation::Send),
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
        CHECK_THROWS_AS(first.append_intent(make_intent(daemon::AccountAuditOperation::Send),
                                            daemon::KnownAccountAuditPins{}, guard, receipt,
                                            failure),
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
        REQUIRE(resumed.append_intent(
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
        CHECK_FALSE(log.append_intent(
            make_intent(daemon::AccountAuditOperation::Send),
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
        CHECK_FALSE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send),
                                      daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt,
                                      failure));
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
            CHECK_FALSE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send),
                                          daemon::AbsentAccountAuditPinsByPolicy{}, guard, receipt,
                                          failure));
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
    CHECK_FALSE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send), pins, guard,
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
}

TEST_CASE("account audit scan and identity rescan share one interruption budget",
          "[account-audit][scanner][deadline][cancellation]") {
    SECTION("primary scan observes the absolute deadline") {
        AuditTree tree;
        daemon::AccountAuditLog log(tree.state(), "main", ::getuid());
        daemon::AccountAuditScanControl control;
        control.deadline = std::chrono::steady_clock::now();
        auto guard = tree.coordinator().lock(std::move(control));
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Interrupted);
        REQUIRE(inspection.failure.interruption);
        CHECK(*inspection.failure.interruption ==
              daemon::AccountAuditFailure::Interruption::Deadline);
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
        auto guard = tree.coordinator().lock(std::move(control));
        const auto inspection = log.inspect(guard);
        CHECK(inspection.status == daemon::AccountAuditInspectionStatus::Interrupted);
        REQUIRE(inspection.failure.interruption);
        CHECK(*inspection.failure.interruption ==
              daemon::AccountAuditFailure::Interruption::Cancelled);
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

    const auto schema = tgcli::test::load_schema_document("audit-intent.schema.json");
    CHECK(schema["$defs"]["messageWriteResult"]["properties"]["text"]["x-tgcli-maxUtf8Bytes"] ==
          16'384);
    CHECK(schema["$defs"]["fileSnapshot"]["properties"]["name"]["x-tgcli-forbidControlScalars"] ==
          true);
    const auto& delete_branches = schema["oneOf"];
    CHECK(std::any_of(delete_branches.begin(), delete_branches.end(), [](const json& branch) {
        return branch.contains("properties") && branch["properties"].contains("command") &&
               branch["properties"]["command"]["const"] == "msg_delete" &&
               branch["properties"]["arguments"]["properties"]["message_ids"]
                     ["x-tgcli-strictlyIncreasing"] == true;
    }));
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
        CHECK_FALSE(log.append_intent(make_intent(daemon::AccountAuditOperation::Send), pins, guard,
                                      receipt, failure));
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

// NOLINTEND(misc-const-correctness)
