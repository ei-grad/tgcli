#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/request_session.hpp"
#include "daemon/write_commands.hpp"
#include "daemon/write_domain.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <tuple>
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
    explicit WriteTree(bool allow_write = true) {
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
        output << "default_account = \"main\"\n\n[accounts.main]\nallow_write = "
               << (allow_write ? "true\n" : "false\n");
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~WriteTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    WriteTree(const WriteTree&) = delete;
    WriteTree& operator=(const WriteTree&) = delete;
    WriteTree(WriteTree&&) = delete;
    WriteTree& operator=(WriteTree&&) = delete;

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

core::TdChat private_chat(std::int64_t user_id) {
    auto chat = basic_chat();
    chat.id = user_id;
    chat.title = "Peer";
    chat.kind = core::TdChatKind::Private;
    chat.related_id = user_id;
    return chat;
}

core::TdChat supergroup_chat() {
    auto chat = basic_chat();
    chat.kind = core::TdChatKind::Supergroup;
    chat.related_id = 55;
    return chat;
}

core::TdUserSummary peer(core::TdUserPresence presence, bool bot = false, std::int64_t id = 77) {
    return {.id = id,
            .first_name = "Peer",
            .last_name = "",
            .usernames = {"peer"},
            .phone_number = "12025550124",
            .is_bot = bot,
            .is_premium = false,
            .presence = presence};
}

core::TdWriteMessage stable_message(std::string text = "hello",
                                    std::optional<std::int32_t> scheduled_at = std::nullopt) {
    return {
        .message = {.id = 101,
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
        .scheduling_state =
            scheduled_at
                ? core::TdMessageSchedulingState{.kind =
                                                     core::TdMessageSchedulingStateKind::SendAtDate,
                                                 .send_date = *scheduled_at,
                                                 .repeat_period = 0,
                                                 .unsupported_tdlib_type_id = std::nullopt}
                : core::TdMessageSchedulingState{},
        .has_reply_markup = false};
}

core::TdWriteMessage online_message() {
    auto message = stable_message();
    message.message.chat_id = 77;
    message.scheduling_state = {.kind = core::TdMessageSchedulingStateKind::SendWhenOnline,
                                .send_date = 0,
                                .repeat_period = 0,
                                .unsupported_tdlib_type_id = std::nullopt};
    return message;
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

template <typename T>
const T& function_field(const core::TdFunctionData& function, std::string_view name) {
    const auto found = std::ranges::find_if(
        function.fields(), [name](const auto& field) { return field.has_name(name); });
    REQUIRE(found != function.fields().end());
    const auto* value = std::get_if<T>(&found->value());
    REQUIRE(value != nullptr);
    return *value;
}

class FakeWrites final {
  public:
    explicit FakeWrites(bool allow_write = true)
        : tree_(allow_write), config_(tree_.config_path(), {}, ::getuid()) {
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
    FakeWrites(FakeWrites&&) = delete;
    FakeWrites& operator=(FakeWrites&&) = delete;

    std::future<Outcome> dispatch(const proto::Request& request) {
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

    template <typename T> core::TdFunctionData respond(core::TdFunctionKind expected, T value) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        auto function = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id,
                                core::TdValue::from(std::move(value)));
        ++sent_count_;
        return function;
    }

    core::TdFunctionData observe(core::TdFunctionKind expected) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        ++sent_count_;
        return sent.back().function;
    }

    [[nodiscard]] std::size_t count(core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    [[nodiscard]] const WriteTree& tree() const {
        return tree_;
    }

    [[nodiscard]] core::TdFormattedText parsed_text(std::string text) const {
        return test::ScriptedTdRuntime::parsed_formatted_text(client_id_, std::move(text));
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
                            std::optional<std::string> key = "m3-public-key-sentinel",
                            std::optional<std::int32_t> scheduled_at = std::nullopt) {
    proto::Request request("main");
    request.id = 41;
    request.command = {"send"};
    request.args = {{"chat", "-1001"},
                    {"text", std::move(text)},
                    {"parse_mode", "plain"},
                    {"reply_to", nullptr},
                    {"topic", nullptr},
                    {"silent", false},
                    {"schedule", scheduled_at ? json{{"kind", "at"}, {"send_date", *scheduled_at}}
                                              : json(nullptr)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.dry_run = dry_run;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request delete_request(std::int64_t message_id = 101,
                              std::optional<std::string> key = std::nullopt, bool yes = true) {
    proto::Request request("main");
    request.id = 42;
    request.command = {"msg", "delete"};
    request.args = {
        {"chat", "-1001"}, {"message_ids", json::array({message_id})}, {"for_all", false}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = yes;
    request.context.idempotency_key = std::move(key);
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

TEST_CASE("send text topic and RFC3339 schedule normalization is closed at boundaries",
          "[write-command][send][domain]") {
    CHECK(daemon::valid_send_text("x"));
    CHECK(daemon::valid_send_text(std::string(4096, 'x')));
    CHECK_FALSE(daemon::valid_send_text(std::string(4097, 'x')));
    std::string emoji;
    emoji.reserve(static_cast<std::size_t>(4096) * 4);
    for (std::size_t index = 0; index < 4096; ++index) {
        emoji.append("\xF0\x9F\x98\x80", 4);
    }
    CHECK(daemon::valid_send_text(emoji));
    emoji.append("\xF0\x9F\x98\x80", 4);
    CHECK_FALSE(daemon::valid_send_text(emoji));
    CHECK_FALSE(daemon::valid_send_text(std::string("a\0b", 3)));
    CHECK_FALSE(daemon::valid_send_text(std::string("\xF0\x28\x8C\x28", 4)));

    const auto bare_topic = daemon::parse_send_topic("2147483647");
    REQUIRE(bare_topic);
    CHECK(bare_topic->kind == daemon::TopicKind::Forum);
    CHECK(bare_topic->id == 2147483647);
    CHECK(daemon::parse_send_topic("forum:7") ==
          daemon::TopicRef{.kind = daemon::TopicKind::Forum, .id = 7});
    for (const auto* invalid : {"0", "2147483648", "forum:0", "thread:1", "+1", "01"}) {
        INFO(invalid);
        CHECK_FALSE(daemon::parse_send_topic(invalid));
    }

    const auto online = daemon::parse_send_schedule("online");
    REQUIRE(online);
    CHECK(online->kind == daemon::SendScheduleKind::Online);
    CHECK(online->send_date == 0);
    const auto epoch = daemon::parse_send_schedule("1970-01-01T00:00:01Z");
    REQUIRE(epoch);
    CHECK(epoch->kind == daemon::SendScheduleKind::At);
    CHECK(epoch->send_date == 1);
    CHECK(daemon::parse_send_schedule("1970-01-01T01:00:01+01:00") == epoch);
    const auto ceiling = daemon::parse_send_schedule("1970-01-01T00:00:01.000000001Z");
    REQUIRE(ceiling);
    CHECK(ceiling->send_date == 2);
    const auto maximum = daemon::parse_send_schedule("2038-01-19T03:14:07Z");
    REQUIRE(maximum);
    CHECK(maximum->send_date == std::numeric_limits<std::int32_t>::max());
    for (const auto* invalid :
         {"1970-01-01T00:00:00Z", "2038-01-19T03:14:08Z", "2026-02-29T12:00:00Z",
          "2026-08-21T12:00:00", "2026-08-21T12:00:60Z", "Online"}) {
        INFO(invalid);
        CHECK_FALSE(daemon::parse_send_schedule(invalid));
    }
}

TEST_CASE("public send dry-run plans through the read boundary without durable writes",
          "[write-command][send][dry-run][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("hello", true, std::nullopt);
    auto pending = fake.dispatch(request);
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

TEST_CASE("public send authority is frozen while dry-run remains read-only",
          "[write-command][send][authority][dry-run][fake-boundary]") {
    SECTION("standing deny rejects after principal binding") {
        FakeWrites fake(false);
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt));
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "WRITE_DENIED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "no_grant");
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("request grant authorizes a real mutation") {
        FakeWrites fake(false);
        auto request = send_request("hello", false, std::nullopt);
        request.context.write_authority = proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::SendMessage, stable_message());
        REQUIRE(pending.get().result);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    }

    SECTION("explicit request deny wins over a standing grant") {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.context.write_authority = proto::WriteAuthority::Deny;
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "WRITE_DENIED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "explicit_deny");
    }

    SECTION("dry-run plans despite standing deny and creates no write artifacts") {
        FakeWrites fake(false);
        auto pending = fake.dispatch(send_request("hello", true, std::nullopt));
        resolve_basic(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }
}

TEST_CASE("public send enforces the +10/+11 schedule boundary again before dispatch",
          "[write-command][send][schedule][fake-boundary]") {
    constexpr std::int32_t send_date = 2'000'000'000;
    SECTION("planning rejects equality at ten seconds") {
        FakeWrites fake;
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt, send_date));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 10});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "schedule_window_elapsed");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("commit recheck closes an elapsed plan without mutation") {
        FakeWrites fake;
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt, send_date));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 10});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("eleven seconds at both reads dispatches a scheduled message") {
        FakeWrites fake;
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt, send_date));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.respond(core::TdFunctionKind::SendMessage, stable_message("hello", send_date));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["scheduled"] == true);
        CHECK((*outcome.result)["date"] == nullptr);
        CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("send.result.schema.json"));
    }
}

TEST_CASE("online schedule requires a visible non-bot non-self private peer",
          "[write-command][send][online][fake-boundary]") {
    for (const auto presence : {core::TdUserPresence::Online, core::TdUserPresence::Offline}) {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["chat"] = "77";
        request.args["schedule"] = json{{"kind", "online"}};
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, private_chat(77));
        fake.respond(core::TdFunctionKind::GetUser, peer(presence));
        const auto descriptor = fake.respond(core::TdFunctionKind::SendMessage, online_message());
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["scheduled"] == true);
        CHECK((*outcome.result)["date"] == nullptr);
        CHECK(function_field<std::string>(descriptor, "schedule_kind") == "when_online");
    }

    for (const auto& [label, user_id, user] :
         std::vector<std::tuple<std::string, std::int64_t, core::TdUserSummary>>{
             {"hidden", 77, peer(core::TdUserPresence::Hidden)},
             {"bot", 77, peer(core::TdUserPresence::Online, true)},
             {"self", 42, peer(core::TdUserPresence::Online, false, 42)}}) {
        INFO(label);
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["chat"] = std::to_string(user_id);
        request.args["schedule"] = json{{"kind", "online"}};
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, private_chat(user_id));
        fake.respond(core::TdFunctionKind::GetUser, user);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "online_schedule_unsupported");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    }
}

TEST_CASE("send reply planning accepts signed ids and inherits only matching forum topics",
          "[write-command][send][reply][topic][fake-boundary]") {
    SECTION("signed reply inherits forum topic into the TD request") {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["reply_to"] = -7;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        auto reply = planning_message(-7);
        reply.topic = core::TdTopic{.kind = core::TdTopicKind::Forum, .id = 9, .tdlib_type_id = 1};
        fake.respond(core::TdFunctionKind::GetMessage, reply);
        core::TdMessageProperties properties;
        properties.can_be_replied = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        auto sent = stable_message();
        sent.message.topic = reply.topic;
        const auto descriptor = fake.respond(core::TdFunctionKind::SendMessage, sent);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(function_field<std::int64_t>(descriptor, "reply_to_message_id") == -7);
        CHECK(function_field<std::string>(descriptor, "topic_kind") == "forum");
        CHECK(function_field<std::int64_t>(descriptor, "topic_id") == 9);
    }

    SECTION("explicit topic mismatch fails before mutation") {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["reply_to"] = -7;
        request.args["topic"] = json{{"kind", "forum"}, {"id", 8}};
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        auto reply = planning_message(-7);
        reply.topic = core::TdTopic{.kind = core::TdTopicKind::Forum, .id = 9, .tdlib_type_id = 1};
        fake.respond(core::TdFunctionKind::GetMessage, reply);
        core::TdMessageProperties properties;
        properties.can_be_replied = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "wrong_topic");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    }
}

TEST_CASE("Markdown send dispatches only the generation-bound parsed formatted text",
          "[write-command][send][format][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("**hello**", false, std::nullopt);
    request.args["parse_mode"] = "markdown_v2";
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::ParseTextEntities, fake.parsed_text("hello"));
    const auto descriptor =
        fake.respond(core::TdFunctionKind::SendMessage, stable_message("hello"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["text"] == "hello");
    CHECK(function_field<bool>(descriptor, "parsed"));
    CHECK(function_field<std::string>(descriptor, "text") == "hello");
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

TEST_CASE("post-proof send timeout retains an unknown keyed invocation for recovery",
          "[write-command][send][timeout][idempotency][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("hello", false, "timeout-key-sentinel");
    request.context.timeout_seconds = 0.5;
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.observe(core::TdFunctionKind::SendMessage);
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK((*outcome.error)["error"]["details"]["phase"] == "confirmation");
    CHECK((*outcome.error)["error"]["details"]["outcome"] == "unknown");
    CHECK((*outcome.error)["error"]["details"]["idempotency"] == "pending");
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));

    const auto store = json::parse(read_bytes(fake.tree().store_path()));
    REQUIRE(store["entries"].size() == 1);
    CHECK(store["entries"][0]["state"] == "pending");
    CHECK(read_bytes(fake.tree().store_path()).find("timeout-key-sentinel") == std::string::npos);
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

TEST_CASE("msg delete applies exact self/revoke property rules before confirmation",
          "[write-command][delete][properties][fake-boundary]") {
    SECTION("private and basic self-delete require the self property") {
        FakeWrites fake;
        auto pending = fake.dispatch(delete_request());
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_for_all_users = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "not_deletable_for_self");
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    }

    SECTION("explicit for-all on a basic group dispatches revoke true") {
        FakeWrites fake;
        auto request = delete_request();
        request.args["for_all"] = true;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_for_all_users = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto descriptor = fake.respond(core::TdFunctionKind::DeleteMessages, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["for_all"] == true);
        CHECK(function_field<bool>(descriptor, "revoke"));
    }

    SECTION("supergroups require an explicit for-all request before message reads") {
        FakeWrites fake;
        auto pending = fake.dispatch(delete_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup,
                     core::TdSupergroup{.id = 55, .usernames = {}, .is_channel = false});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "not_deletable_for_all");
        CHECK(fake.count(core::TdFunctionKind::GetMessage) == 0);
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    }
}

TEST_CASE("msg delete dry-run performs property planning without confirmation or writes",
          "[write-command][delete][dry-run][confirmation][fake-boundary]") {
    FakeWrites fake(false);
    auto request = delete_request(101, std::nullopt, false);
    request.context.dry_run = true;
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["dry_run"] == true);
    CHECK((*outcome.result)["plan"]["operation"] == "msg_delete");
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-delete.result.schema.json"));
}

TEST_CASE("completed msg delete replay reconfirms its stored plan before returning",
          "[write-command][delete][confirmation][replay][fake-boundary]") {
    FakeWrites fake;
    auto first = fake.dispatch(delete_request(101, "delete-replay-key"));
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    fake.respond(core::TdFunctionKind::DeleteMessages, core::TdOk{});
    const auto first_outcome = first.get();
    REQUIRE(first_outcome.result);

    auto unconfirmed = fake.dispatch(delete_request(101, "delete-replay-key", false));
    bind_principal(fake);
    const auto unconfirmed_outcome = unconfirmed.get();
    REQUIRE(unconfirmed_outcome.error);
    CHECK((*unconfirmed_outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
    CHECK((*unconfirmed_outcome.error)["error"]["details"]["target"]["message_ids"] ==
          json::array({101}));
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);

    auto replay = fake.dispatch(delete_request(101, "delete-replay-key"));
    bind_principal(fake);
    const auto replay_outcome = replay.get();
    CHECK(replay_outcome.result == first_outcome.result);
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);

    auto conflict = fake.dispatch(delete_request(102, "delete-replay-key", false));
    bind_principal(fake);
    const auto conflict_outcome = conflict.get();
    REQUIRE(conflict_outcome.error);
    CHECK((*conflict_outcome.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);
}
