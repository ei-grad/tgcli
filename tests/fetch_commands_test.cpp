#include "common/exit_codes.hpp"
#include "daemon/fetch_commands.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using nlohmann::json;

namespace {

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    std::vector<json> progress;
    int exit_code = -1;
    int terminal_count = 0;
};

class FakeFetch {
  public:
    struct ControlledDispatch {
        std::future<Outcome> outcome;
        std::future<std::shared_ptr<tgcli::daemon::RequestSession>> session;
    };

    explicit FakeFetch(tgcli::core::AuthState state = tgcli::core::AuthState::Ready) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {}, tgcli::core::AuthStateData{state});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ = std::make_unique<tgcli::daemon::FetchCoordinator>(
            *client_, "main", [] { return std::chrono::system_clock::time_point{}; });
        tgcli::daemon::register_fetch_command(dispatcher_, *coordinator_);
    }

    std::future<Outcome> dispatch(tgcli::proto::Request request) {
        return std::async(std::launch::async, [this, request = std::move(request)]() mutable {
            Outcome outcome;
            tgcli::daemon::CallbackSink sink(
                [](const json&) {},
                [&](json value) { outcome.progress.push_back(std::move(value)); },
                [&](json value) {
                    ++outcome.terminal_count;
                    outcome.result = std::move(value);
                    outcome.exit_code = tgcli::kOk;
                },
                [&](std::string code, std::string message, json details, int exit_code) {
                    ++outcome.terminal_count;
                    outcome.error = json{{"error",
                                          {{"code", std::move(code)},
                                           {"message", std::move(message)},
                                           {"details", std::move(details)}}}};
                    outcome.exit_code = exit_code;
                });
            dispatcher_.dispatch(request, sink);
            return outcome;
        });
    }

    ControlledDispatch dispatch_controlled(tgcli::proto::Request request) {
        auto published =
            std::make_shared<std::promise<std::shared_ptr<tgcli::daemon::RequestSession>>>();
        auto session = published->get_future();
        auto outcome = std::async(std::launch::async, [this, request = std::move(request),
                                                       published = std::move(published)]() mutable {
            Outcome result;
            tgcli::daemon::CallbackSink sink(
                [](const json&) {},
                [&](json value) { result.progress.push_back(std::move(value)); },
                [&](json value) {
                    ++result.terminal_count;
                    result.result = std::move(value);
                    result.exit_code = tgcli::kOk;
                },
                [&](std::string code, std::string message, json details, int exit_code) {
                    ++result.terminal_count;
                    result.error = json{{"error",
                                         {{"code", std::move(code)},
                                          {"message", std::move(message)},
                                          {"details", std::move(details)}}}};
                    result.exit_code = exit_code;
                });
            const auto deadline = tgcli::request_deadline(request.context.timeout_seconds,
                                                          tgcli::DeadlineDefault::Unlimited);
            REQUIRE(deadline);
            auto active = std::make_shared<tgcli::daemon::RequestSession>(
                std::move(request), sink, 0, tgcli::daemon::RequestSession::NonceGenerator{},
                tgcli::daemon::ActivityTracker::Token{}, nullptr, deadline);
            published->set_value(active);
            dispatcher_.dispatch(*active);
            return result;
        });
        return {std::move(outcome), std::move(session)};
    }

    template <typename T>
    tgcli::core::TdFunctionData respond(tgcli::core::TdFunctionKind expected, T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        const auto descriptor = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
        return descriptor;
    }

    void respond_me(bool bot = false) {
        respond(tgcli::core::TdFunctionKind::GetMe,
                tgcli::core::TdUserSummary{.id = 42,
                                           .first_name = "Ada",
                                           .last_name = "",
                                           .usernames = {"ada"},
                                           .phone_number = bot ? "" : "12025550123",
                                           .is_bot = bot,
                                           .is_premium = false});
    }

    void respond_chat(tgcli::core::TdChatKind kind = tgcli::core::TdChatKind::BasicGroup) {
        respond(tgcli::core::TdFunctionKind::GetChat,
                tgcli::core::TdChat{.id = -1001,
                                    .title = "Project",
                                    .kind = kind,
                                    .related_id = 0,
                                    .tdlib_type_id = 1,
                                    .positions = {},
                                    .chat_lists = {},
                                    .is_marked_unread = false,
                                    .unread_count = 0,
                                    .unread_mention_count = 0,
                                    .unread_reaction_count = 0,
                                    .unread_poll_vote_count = 0,
                                    .last_message = std::nullopt});
    }

    [[nodiscard]] bool wait_for_sent(std::size_t count) const {
        return runtime_->wait_for_sent(count);
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

    [[nodiscard]] std::size_t count(tgcli::core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

  private:
    template <typename Predicate> static bool eventually(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient client_id_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::FetchCoordinator> coordinator_;
    tgcli::daemon::Dispatcher dispatcher_;
    std::size_t sent_count_ = 1;
};

const tgcli::core::TdFieldValue* field(const tgcli::core::TdFunctionData& function,
                                       std::string_view name) {
    for (const auto& candidate : function.fields()) {
        if (candidate.has_name(name)) {
            return &candidate.value();
        }
    }
    return nullptr;
}

template <typename T>
const T& field_as(const tgcli::core::TdFunctionData& function, std::string_view name) {
    const auto* value = field(function, name);
    REQUIRE(value != nullptr);
    const auto* typed = std::get_if<T>(value);
    REQUIRE(typed != nullptr);
    return *typed;
}

tgcli::core::TdMessageSummary message(std::int64_t id, std::int32_t date = 20,
                                      std::int64_t chat_id = -1001) {
    return {
        .id = id,
        .chat_id = chat_id,
        .date = date,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = false,
        .topic = std::nullopt,
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "message"};
}

tgcli::proto::Request request(std::optional<std::int32_t> limit = std::nullopt, bool all = false,
                              std::optional<std::string> since = std::nullopt,
                              std::optional<double> timeout = std::nullopt) {
    tgcli::proto::Request result("main");
    result.command = {"fetch"};
    result.args = {{"chat", "-1001"},
                   {"limit", limit ? json(*limit) : json(nullptr)},
                   {"all", all},
                   {"since", since ? json(*since) : json(nullptr)}};
    result.context.timeout_seconds = timeout;
    result.context.cwd = "/";
    return result;
}

void resolve_basic(FakeFetch& fake) {
    fake.respond_me();
    fake.respond_chat();
}

} // namespace

TEST_CASE("fetch defaults to depth 100 and incorporates the complete local page",
          "[fetch][fake-boundary][default][schema]") {
    FakeFetch fake;
    auto pending = fake.dispatch(request());
    resolve_basic(fake);
    std::vector<std::optional<tgcli::core::TdMessageSummary>> messages;
    messages.reserve(100);
    for (std::int64_t id = 200; id > 100; --id) {
        messages.emplace_back(message(id));
    }
    fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                 tgcli::core::TdMessages{.total_count = 100, .messages = std::move(messages)});
    fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                 tgcli::core::TdMessages{.total_count = 0, .messages = {}});

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["cached_count"] == 100);
    CHECK((*outcome.result)["oldest_message_id"] == 101);
    CHECK((*outcome.result)["target"] == json{{"limit", 100}, {"all", false}, {"since", nullptr}});
    CHECK((*outcome.result)["stop_reason"] == "target_reached");
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("fetch.result.schema.json"));
    REQUIRE(outcome.progress.size() == 1);
    CHECK(outcome.progress.front()["cached"] == 100);
}

TEST_CASE("fetch seals the local boundary before since-over-limit success",
          "[fetch][fake-boundary][schema][progress]") {
    FakeFetch fake;
    auto pending = fake.dispatch(request(2, false, "1970-01-01T00:00:20Z"));
    resolve_basic(fake);
    const auto probe =
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, message(98, 19));
    CHECK(field_as<std::int64_t>(probe, "chat_id") == -1001);
    CHECK(field_as<std::int64_t>(probe, "date") == 19);

    const auto local =
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(100)}});
    CHECK(field_as<std::int64_t>(local, "from_message_id") == 0);
    CHECK(field_as<std::int64_t>(local, "offset") == 0);
    CHECK(field_as<std::int64_t>(local, "limit") == 100);
    CHECK(field_as<bool>(local, "only_local"));

    const auto older = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 3,
                                .messages = {message(100), message(99), message(98, 19)}});
    CHECK(field_as<std::int64_t>(older, "from_message_id") == 100);
    CHECK(field_as<bool>(older, "only_local"));

    const auto boundary = fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                                       tgcli::core::TdMessages{.total_count = 0, .messages = {}});
    CHECK(field_as<std::int64_t>(boundary, "from_message_id") == 98);
    CHECK(field_as<bool>(boundary, "only_local"));

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(*outcome.result ==
          json{{"chat_id", -1001},
               {"cached_count", 3},
               {"oldest_message_id", 98},
               {"target", {{"limit", 2}, {"all", false}, {"since", "1970-01-01T00:00:20Z"}}},
               {"target_reached", true},
               {"stop_reason", "since_anchor_reached"},
               {"resume_from_message_id", 98}});
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("fetch.result.schema.json"));
    REQUIRE(outcome.progress.size() == 2);
    CHECK(outcome.progress.front()["cached"] == 1);
    CHECK(outcome.progress.back() == json{{"operation", "fetch"},
                                          {"chat_id", -1001},
                                          {"cached", 3},
                                          {"target", 2},
                                          {"oldest_message_id", 98}});
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("fetch transitions once from the public local boundary to live fill",
          "[fetch][fake-boundary][local][network]") {
    FakeFetch fake;
    auto pending = fake.dispatch(request(3));
    resolve_basic(fake);

    auto first = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 2, .messages = {message(100), message(99)}});
    CHECK(field_as<bool>(first, "only_local"));
    auto boundary = fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                                 tgcli::core::TdMessages{.total_count = 0, .messages = {}});
    CHECK(field_as<bool>(boundary, "only_local"));
    CHECK(field_as<std::int64_t>(boundary, "from_message_id") == 99);
    auto live = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 2, .messages = {message(98), message(97)}});
    CHECK_FALSE(field_as<bool>(live, "only_local"));
    CHECK(field_as<std::int64_t>(live, "from_message_id") == 99);

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["cached_count"] == 4);
    CHECK((*outcome.result)["oldest_message_id"] == 97);
    CHECK((*outcome.result)["stop_reason"] == "target_reached");
    CHECK((*outcome.result)["target_reached"] == true);
    REQUIRE(outcome.progress.size() == 2);
    CHECK(outcome.progress[0]["cached"] == 2);
    CHECK(outcome.progress[1]["cached"] == 4);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 3);
}

TEST_CASE("bare all reports tdlib idle without claiming completeness for an empty prefix",
          "[fetch][fake-boundary][idle][schema]") {
    FakeFetch fake;
    auto pending = fake.dispatch(request(std::nullopt, true));
    resolve_basic(fake);
    fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                 tgcli::core::TdMessages{.total_count = 0, .messages = {}});
    const auto live = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 2, .messages = {std::nullopt, std::nullopt}});
    CHECK_FALSE(field_as<bool>(live, "only_local"));

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["cached_count"] == 0);
    CHECK((*outcome.result)["oldest_message_id"] == nullptr);
    CHECK((*outcome.result)["resume_from_message_id"] == nullptr);
    CHECK((*outcome.result)["target"] ==
          json{{"limit", nullptr}, {"all", true}, {"since", nullptr}});
    CHECK((*outcome.result)["target_reached"] == nullptr);
    CHECK((*outcome.result)["stop_reason"] == "tdlib_idle");
    CHECK(outcome.progress.empty());
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("fetch.result.schema.json"));
}

TEST_CASE("fetch since probing skips INT32_MIN and treats 404 as an absent cutoff",
          "[fetch][fake-boundary][since][probe]") {
    SECTION("INT32_MIN") {
        FakeFetch fake;
        auto pending = fake.dispatch(request(std::nullopt, true, "1901-12-13T20:45:52Z"));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["target"] ==
              json{{"limit", nullptr}, {"all", true}, {"since", "1901-12-13T20:45:52Z"}});
        CHECK((*outcome.result)["target_reached"] == false);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatMessageByDate) == 0);
    }
    SECTION("404") {
        FakeFetch fake;
        auto pending = fake.dispatch(request(std::nullopt, false, "1970-01-01T00:00:20Z"));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate,
                     tgcli::core::TdError{404, "Not Found"});
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["stop_reason"] == "tdlib_idle");
        CHECK((*outcome.result)["target_reached"] == false);
    }
}

TEST_CASE("fetch rejects every malformed since probe before history",
          "[fetch][fake-boundary][since][integrity]") {
    const auto run = [](auto probe) {
        FakeFetch fake;
        auto pending = fake.dispatch(request(std::nullopt, false, "1970-01-01T00:00:20Z"));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, std::move(probe));
        auto outcome = pending.get();
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 0);
        return outcome;
    };

    for (const auto& probe : {message(98, 0), message(98, 20), message(98, 19, -2001)}) {
        const auto outcome = run(probe);
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK_FALSE(outcome.result);
        CHECK(outcome.progress.empty());
    }
    const auto wrong_type = run(tgcli::core::TdMessages{.total_count = 0, .messages = {}});
    REQUIRE(wrong_type.error);
    CHECK((*wrong_type.error)["error"]["code"] == "INTERNAL");
}

TEST_CASE("fetch validates before Ready and rejects bots before selector work",
          "[fetch][fake-boundary][preflight]") {
    SECTION("malformed arguments") {
        FakeFetch fake(tgcli::core::AuthState::WaitPhoneNumber);
        auto invalid = request(1, true);
        const auto outcome = fake.dispatch(std::move(invalid)).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "mutually_exclusive");
        CHECK(fake.sent_count() == 1);
    }
    SECTION("bot") {
        FakeFetch fake;
        auto pending = fake.dispatch(request());
        fake.respond_me(true);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "fetch"}});
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 0);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 0);
    }
}

TEST_CASE("fetch preserves resolver failures and rejects secret targets before history",
          "[fetch][fake-boundary][resolver]") {
    SECTION("resolver rate limit") {
        FakeFetch fake;
        auto pending = fake.dispatch(request());
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChat,
                     tgcli::core::TdError{429, "FLOOD_WAIT_3"});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "RATE_LIMITED");
        CHECK((*outcome.error)["error"]["details"]["operation"] == "resolve");
    }
    SECTION("secret") {
        FakeFetch fake;
        auto pending = fake.dispatch(request());
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Secret);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "unsupported_chat_type");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 0);
    }
}

TEST_CASE("fetch reports strict page failures without a partial result",
          "[fetch][fake-boundary][integrity][error]") {
    const auto run = [](tgcli::core::TdMessages page) {
        FakeFetch fake;
        auto pending = fake.dispatch(request(5));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory, std::move(page));
        return pending.get();
    };

    SECTION("invalid count") {
        const auto outcome = run({.total_count = 0, .messages = {message(100)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK_FALSE(outcome.result);
        CHECK(outcome.progress.empty());
    }
    SECTION("mixed null") {
        const auto outcome = run({.total_count = 2, .messages = {message(100), std::nullopt}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK_FALSE(outcome.result);
        CHECK(outcome.progress.empty());
    }
    SECTION("non advancing") {
        const auto outcome = run({.total_count = 2, .messages = {message(100), message(100)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PAGINATION_INVALID");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "fetch"}, {"reason", "non_advancing_upstream"}});
        CHECK_FALSE(outcome.result);
        CHECK(outcome.progress.empty());
    }
}

TEST_CASE("fetch finite deadlines retain phase and committed-prefix details",
          "[fetch][fake-boundary][deadline]") {
    SECTION("target latch waits for the local boundary") {
        FakeFetch fake;
        auto pending = fake.dispatch(request(1, false, std::nullopt, 0.05));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(100)}});
        REQUIRE(fake.wait_for_sent(5));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kTimeout);
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "fetch"},
                                                           {"chat_id", -1001},
                                                           {"phase", "local_scan"},
                                                           {"state", "ready"},
                                                           {"cached_count", 1},
                                                           {"oldest_message_id", 100},
                                                           {"resume_from_message_id", 100}});
        CHECK_FALSE(outcome.result);
        REQUIRE(outcome.progress.size() == 1);

        fake.respond(
            tgcli::core::TdFunctionKind::GetChatHistory,
            tgcli::core::TdMessages{.total_count = 2, .messages = {message(100), message(99)}});
        auto repeated = fake.dispatch(request(1));
        resolve_basic(fake);
        const auto restarted = fake.respond(
            tgcli::core::TdFunctionKind::GetChatHistory,
            tgcli::core::TdMessages{.total_count = 2, .messages = {message(100), message(99)}});
        CHECK(field_as<std::int64_t>(restarted, "from_message_id") == 0);
        CHECK(field_as<bool>(restarted, "only_local"));
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        const auto repeated_outcome = repeated.get();
        REQUIRE(repeated_outcome.result);
        CHECK((*repeated_outcome.result)["cached_count"] == 2);
        CHECK((*repeated_outcome.result)["stop_reason"] == "target_reached");
    }
    SECTION("network phase begins only after the local zero-progress response") {
        FakeFetch fake;
        auto pending = fake.dispatch(request(1, false, std::nullopt, 0.05));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        REQUIRE(fake.wait_for_sent(5));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["phase"] == "network_fill");
        CHECK((*outcome.error)["error"]["details"]["cached_count"] == 0);
        CHECK((*outcome.error)["error"]["details"]["oldest_message_id"] == nullptr);
        CHECK(outcome.progress.empty());
    }
    SECTION("pre-target resolver timeout uses the common fetch shape") {
        FakeFetch fake;
        auto pending = fake.dispatch(request(std::nullopt, false, std::nullopt, 0.05));
        fake.respond_me();
        REQUIRE(fake.wait_for_sent(3));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "fetch"}, {"state", "ready"}});
        CHECK_FALSE((*outcome.error)["error"]["details"].contains("phase"));
    }
}

TEST_CASE("fetch unlimited waits remain cancellable and deadline equality stays exclusive",
          "[fetch][fake-boundary][cancel][deadline][equality]") {
    const auto instant = tgcli::RequestClock::time_point(10s);
    const tgcli::RequestDeadline deadline{instant};
    CHECK(tgcli::deadline_expired(deadline, instant));
    CHECK_FALSE(tgcli::event_precedes_deadline(instant, deadline));
    CHECK(tgcli::event_precedes_deadline(instant - 1ns, deadline));

    FakeFetch fake;
    auto controlled = fake.dispatch_controlled(request(std::nullopt, true));
    const auto session = controlled.session.get();
    resolve_basic(fake);
    REQUIRE(fake.wait_for_sent(4));
    session->disconnect();
    const auto outcome = controlled.outcome.get();
    CHECK_FALSE(outcome.result);
    CHECK_FALSE(outcome.error);
    CHECK(outcome.progress.empty());
    CHECK(outcome.terminal_count == 0);
}
