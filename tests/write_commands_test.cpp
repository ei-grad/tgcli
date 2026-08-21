#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/request_session.hpp"
#include "daemon/write_commands.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using nlohmann::json;

namespace {

class WriteTree final {
  public:
    WriteTree() {
        std::string pattern = "/tmp/tgcli-write-command-XXXXXX";
        pattern.push_back('\0');
        const auto* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        for (const auto& directory :
             {root_ + "/config", root_ + "/config/tgcli", root_ + "/state", root_ + "/state/tgcli",
              root_ + "/state/tgcli/accounts", account_state()}) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
        std::ofstream output(config_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << "default_account = \"main\"\n\n[accounts.main]\nallow_write = true\n";
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~WriteTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    WriteTree(const WriteTree&) = delete;
    WriteTree& operator=(const WriteTree&) = delete;

    [[nodiscard]] std::string config_path() const {
        return root_ + "/config/tgcli/config.toml";
    }

    [[nodiscard]] std::string account_state() const {
        return root_ + "/state/tgcli/accounts/main";
    }

    [[nodiscard]] std::string audit_path() const {
        return account_state() + "/audit.log";
    }

    [[nodiscard]] std::string store_path() const {
        return account_state() + "/idempotency.db";
    }

  private:
    std::string root_;
};

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    int exit_code = -1;
    int terminal_count = 0;
};

std::string read_bytes(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

core::TdUserSummary self() {
    return {.id = 42,
            .first_name = "Ada",
            .last_name = "",
            .usernames = {"ada"},
            .phone_number = "12025550123",
            .is_bot = false,
            .is_premium = false,
            .presence = core::TdUserPresence::Online};
}

core::TdChat basic_chat() {
    return {.id = -1001,
            .title = "Project",
            .kind = core::TdChatKind::BasicGroup,
            .related_id = 0,
            .tdlib_type_id = 1,
            .positions = {},
            .chat_lists = {},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .notification_settings = std::nullopt};
}

core::TdWriteMessage stable_message(std::string text = "hello") {
    return {.message = {.id = 101,
                        .chat_id = -1001,
                        .date = 1'785'924'000,
                        .sender = {.kind = core::TdMessageSenderKind::User,
                                   .id = 42,
                                   .tdlib_type_id = 1},
                        .is_outgoing = true,
                        .topic = std::nullopt,
                        .content_kind = core::TdMessageContentKind::Text,
                        .text = std::move(text)},
            .sending_state = {},
            .scheduling_state = {},
            .has_reply_markup = false};
}

core::TdPlanningMessage planning_message(std::int64_t id) {
    return {.id = id,
            .chat_id = -1001,
            .date = 1'785'924'000,
            .sender = {.kind = core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
            .is_outgoing = true,
            .topic = std::nullopt,
            .content_kind = core::TdMessageContentKind::Text,
            .text = "cleanup",
            .has_scheduling_state = false,
            .has_reply_markup = false};
}

class FakeWrites final {
  public:
    FakeWrites() : config_(tree_.config_path(), {}, ::getuid()) {
        std::string error;
        lease_ =
            daemon_lock::acquire_lifetime(tree_.account_state() + "/daemon.lock", identity_, error);
        INFO(error);
        REQUIRE(lease_);
        auto created = daemon::IdempotencyFoundation::create(tree_.account_state(), "main",
                                                             ::getuid(), lease_);
        REQUIRE(std::holds_alternative<daemon::IdempotencyFoundation>(created));
        foundation_ = std::make_shared<daemon::IdempotencyFoundation>(
            std::get<daemon::IdempotencyFoundation>(std::move(created)));

        auto runtime = std::make_unique<test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {}, core::AuthStateData{core::AuthState::Ready});
        const auto ready_deadline = std::chrono::steady_clock::now() + 2s;
        while (client_->auth_state()->data.state != core::AuthState::Ready &&
               std::chrono::steady_clock::now() < ready_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(client_->auth_state()->data.state == core::AuthState::Ready);

        coordinator_ = std::make_unique<daemon::WriteCoordinator>(
            *client_, "main", tree_.config_path(), ::getuid(), foundation_);
        daemon::register_write_commands(dispatcher_, *coordinator_);

        const auto admitted_result = config_.admit("main", std::chrono::steady_clock::now() + 2s);
        REQUIRE(admitted_result.refresh_status == daemon::ConfigRefreshStatus::Completed);
        REQUIRE(admitted_result.decision);
        REQUIRE(std::holds_alternative<std::shared_ptr<const daemon::AdmittedAccountConfig>>(
            *admitted_result.decision));
        admitted_ = std::get<std::shared_ptr<const daemon::AdmittedAccountConfig>>(
            *admitted_result.decision);
        REQUIRE(admitted_);
    }

    ~FakeWrites() {
        coordinator_.reset();
        client_.reset();
        foundation_.reset();
        lease_.reset();
    }

    FakeWrites(const FakeWrites&) = delete;
    FakeWrites& operator=(const FakeWrites&) = delete;

    std::future<Outcome> dispatch(proto::Request request) {
        std::string error;
        auto frozen = proto::admit_request_source(request, error);
        INFO(error);
        REQUIRE(frozen);
        auto outcome = std::make_shared<Outcome>();
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [outcome](json result) {
                outcome->result = std::move(result);
                outcome->exit_code = kOk;
                ++outcome->terminal_count;
            },
            [outcome](std::string code, std::string message, json details, int exit_code) {
                outcome->error = json{{"error",
                                       {{"code", std::move(code)},
                                        {"message", std::move(message)},
                                        {"details", std::move(details)}}}};
                outcome->exit_code = exit_code;
                ++outcome->terminal_count;
            });
        auto session = std::make_shared<daemon::RequestSession>(
            std::move(*frozen), sink, 0, daemon::RequestSession::NonceGenerator{},
            daemon::ActivityTracker::Token{}, admitted_, std::nullopt,
            daemon::ConfigAdmissionMode::FrozenRuntime);
        return std::async(std::launch::async, [this, outcome, session] {
            dispatcher_.dispatch(*session);
            return *outcome;
        });
    }

    template <typename T> void respond(core::TdFunctionKind expected, T value) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        runtime_->push_response(client_id_, sent.back().query_id,
                                core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    [[nodiscard]] std::size_t count(core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    [[nodiscard]] const WriteTree& tree() const {
        return tree_;
    }

  private:
    WriteTree tree_;
    daemon::ConfigRuntime config_;
    daemon_lock::Identity identity_;
    std::shared_ptr<daemon_lock::LifetimeLease> lease_;
    std::shared_ptr<daemon::IdempotencyFoundation> foundation_;
    test::ScriptedTdRuntime* runtime_ = nullptr;
    test::ScriptedClient client_id_{};
    std::unique_ptr<core::TdClient> client_;
    std::unique_ptr<daemon::WriteCoordinator> coordinator_;
    daemon::Dispatcher dispatcher_;
    std::shared_ptr<const daemon::AdmittedAccountConfig> admitted_;
    std::size_t sent_count_ = 1;
};

proto::Request send_request(std::string text = "hello", bool dry_run = false,
                            std::optional<std::string> key = "m3-public-key-sentinel") {
    proto::Request request("main");
    request.id = 41;
    request.command = {"send"};
    request.args = {{"chat", "-1001"},     {"text", std::move(text)}, {"parse_mode", "plain"},
                    {"reply_to", nullptr}, {"topic", nullptr},        {"silent", false},
                    {"schedule", nullptr}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.dry_run = dry_run;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request delete_request() {
    proto::Request request("main");
    request.id = 42;
    request.command = {"msg", "delete"};
    request.args = {{"chat", "-1001"}, {"message_ids", json::array({101})}, {"for_all", false}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = true;
    return request;
}

void resolve_basic(FakeWrites& fake) {
    fake.respond(core::TdFunctionKind::GetMe, self());
    fake.respond(core::TdFunctionKind::GetChat, basic_chat());
}

void bind_principal(FakeWrites& fake) {
    fake.respond(core::TdFunctionKind::GetMe, self());
}

} // namespace

TEST_CASE("public send dry-run plans through the read boundary without durable writes",
          "[write-command][send][dry-run][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("hello", true, std::nullopt);
    auto pending = fake.dispatch(std::move(request));
    resolve_basic(fake);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.terminal_count == 1);
    CHECK((*outcome.result)["dry_run"] == true);
    CHECK((*outcome.result)["plan"]["operation"] == "send");
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("send.result.schema.json"));
}

TEST_CASE("public send stores one mutation and replays before conflict without leaking the key",
          "[write-command][send][idempotency][fake-boundary]") {
    FakeWrites fake;
    auto first = fake.dispatch(send_request());
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::SendMessage, stable_message());
    const auto first_outcome = first.get();
    REQUIRE(first_outcome.result);
    CHECK(first_outcome.terminal_count == 1);
    CHECK((*first_outcome.result)["id"] == 101);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    CHECK_THAT(*first_outcome.result, tgcli::test::matches_json_schema("send.result.schema.json"));

    const auto audit_before_replay = read_bytes(fake.tree().audit_path());
    auto replay = fake.dispatch(send_request());
    bind_principal(fake);
    const auto replay_outcome = replay.get();
    CHECK(replay_outcome.result == first_outcome.result);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    CHECK(read_bytes(fake.tree().audit_path()) == audit_before_replay);

    auto conflict = fake.dispatch(send_request("different"));
    bind_principal(fake);
    const auto conflict_outcome = conflict.get();
    REQUIRE(conflict_outcome.error);
    CHECK((*conflict_outcome.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    CHECK_THAT(*conflict_outcome.error,
               tgcli::test::matches_json_schema("m3-write.error.schema.json"));

    const auto artifacts = first_outcome.result->dump() + replay_outcome.result->dump() +
                           conflict_outcome.error->dump() + read_bytes(fake.tree().audit_path()) +
                           read_bytes(fake.tree().store_path());
    CHECK(artifacts.find("m3-public-key-sentinel") == std::string::npos);
}

TEST_CASE("public msg delete confirms the immutable plan and accepts correlated ok",
          "[write-command][delete][confirmation][fake-boundary]") {
    FakeWrites fake;
    auto pending = fake.dispatch(delete_request());
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    fake.respond(core::TdFunctionKind::DeleteMessages, core::TdOk{});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.terminal_count == 1);
    CHECK(*outcome.result == json{{"chat_id", -1001},
                                  {"message_ids", json::array({101})},
                                  {"for_all", false},
                                  {"deleted", true}});
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-delete.result.schema.json"));
}
