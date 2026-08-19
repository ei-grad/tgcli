#include "common/exit_codes.hpp"
#include "daemon/message_commands.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <future>
#include <limits>
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
    int exit_code = -1;
    int terminal_count = 0;
};

class FakeMessages {
  public:
    explicit FakeMessages(tgcli::core::AuthState state = tgcli::core::AuthState::Ready) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {}, tgcli::core::AuthStateData{state});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ =
            std::make_unique<tgcli::daemon::MessageCoordinator>(*client_, std::string("main"));
        tgcli::daemon::register_message_commands(dispatcher_, *coordinator_);
    }

    std::future<Outcome> dispatch(tgcli::proto::Request request) {
        return std::async(std::launch::async, [this, request = std::move(request)]() mutable {
            Outcome outcome;
            tgcli::daemon::CallbackSink sink(
                [](const json&) {}, [](const json&) {},
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
            tgcli::daemon::RequestSession session(std::move(request), sink);
            dispatcher_.dispatch(session);
            return outcome;
        });
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
    std::unique_ptr<tgcli::daemon::MessageCoordinator> coordinator_;
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

tgcli::core::TdChat chat(std::int64_t id,
                         tgcli::core::TdChatKind kind = tgcli::core::TdChatKind::BasicGroup) {
    return {.id = id,
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
            .last_message = std::nullopt};
}

tgcli::core::TdMessageSummary message(std::int64_t chat_id, std::int64_t id) {
    return {
        .id = id,
        .chat_id = chat_id,
        .date = 1785924000,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = false,
        .topic = tgcli::core::TdTopic{.kind = tgcli::core::TdTopicKind::Forum,
                                      .id = 7,
                                      .tdlib_type_id = 1},
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "message or caption"};
}

tgcli::proto::Request get_request(std::string selector, std::vector<std::int64_t> ids) {
    tgcli::proto::Request request("main");
    request.command = {"msg", "get"};
    request.args = {{"chat", std::move(selector)}, {"message_ids", std::move(ids)}};
    request.context.timeout_seconds = 1.0;
    request.context.cwd = "/";
    return request;
}

tgcli::proto::Request link_request(std::string selector, std::int64_t id) {
    tgcli::proto::Request request("main");
    request.command = {"msg", "link"};
    request.args = {{"chat", std::move(selector)}, {"message_id", id}};
    request.context.timeout_seconds = 1.0;
    request.context.cwd = "/";
    return request;
}

void resolve_numeric(FakeMessages& fake, std::int64_t chat_id = -1001, bool bot = false) {
    fake.respond_me(bot);
    fake.respond(tgcli::core::TdFunctionKind::GetChat, chat(chat_id));
}

} // namespace

TEST_CASE("msg get preserves argv order and duplicates in one atomic vector result",
          "[msg][get][schema][fake-boundary]") {
    FakeMessages fake;
    auto pending = fake.dispatch(get_request("-1001", {123, 123, 124}));
    resolve_numeric(fake);
    auto pre_epoch = message(-1001, 123);
    pre_epoch.date = -1;
    const auto descriptor = fake.respond(
        tgcli::core::TdFunctionKind::GetMessages,
        tgcli::core::TdMessages{
            .total_count = std::numeric_limits<std::int32_t>::max(),
            .messages = {std::move(pre_epoch), message(-1001, 123), message(-1001, 124)}});
    CHECK(field_as<std::int64_t>(descriptor, "chat_id") == -1001);
    CHECK(field_as<std::vector<std::int64_t>>(descriptor, "message_ids") ==
          std::vector<std::int64_t>{123, 123, 124});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["id"] == 123);
    CHECK((*outcome.result)["items"][0]["date"] == "1969-12-31T23:59:59Z");
    CHECK((*outcome.result)["items"][1]["id"] == 123);
    CHECK((*outcome.result)["items"][2]["id"] == 124);
    CHECK((*outcome.result)["next"] == nullptr);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-get.result.schema.json"));
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessages) == 1);
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("msg get validates every found position before classifying nulls",
          "[msg][get][error][fake-boundary]") {
    const auto run = [](tgcli::core::TdMessages response) {
        FakeMessages fake;
        auto pending = fake.dispatch(get_request("-1001", {123, 124}));
        resolve_numeric(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetMessages, std::move(response));
        return pending.get();
    };

    SECTION("wrong vector length") {
        const auto outcome = run({.messages = {message(-1001, 123)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("wrong positional id wins over null") {
        const auto outcome = run({.messages = {std::nullopt, message(-1001, 999)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("wrong chat wins over null") {
        const auto outcome = run({.messages = {std::nullopt, message(-1002, 124)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("invalid DTO wins over null") {
        auto invalid = message(-1001, 124);
        invalid.sender.kind = tgcli::core::TdMessageSenderKind::Unknown;
        const auto outcome = run({.messages = {std::nullopt, std::move(invalid)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("negative total count wins over null") {
        const auto outcome =
            run({.total_count = -1, .messages = {std::nullopt, message(-1001, 124)}});
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK_FALSE(outcome.result);
    }
}

TEST_CASE("msg get reports structurally valid nulls uniquely in first occurrence order",
          "[msg][get][not-found][fake-boundary]") {
    FakeMessages fake;
    auto pending = fake.dispatch(get_request("-1001", {5, 7, 5, 9}));
    resolve_numeric(fake);
    fake.respond(tgcli::core::TdFunctionKind::GetMessages,
                 tgcli::core::TdMessages{
                     .messages = {std::nullopt, std::nullopt, std::nullopt, message(-1001, 9)}});
    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"chat_id", -1001}, {"missing_ids", json::array({5, 7})}});
    CHECK(outcome.exit_code == tgcli::kNotFound);
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("msg link sends every exact constant and accepts any nonempty UTF-8 link",
          "[msg][link][schema][fake-boundary]") {
    FakeMessages fake;
    auto pending = fake.dispatch(link_request("-1001", 123));
    resolve_numeric(fake, -1001, true);
    const auto descriptor = fake.respond(
        tgcli::core::TdFunctionKind::GetMessageLink,
        tgcli::core::TdMessageLink{.link = "urn:telegram:message", .is_public = false});
    CHECK(field_as<std::int64_t>(descriptor, "chat_id") == -1001);
    CHECK(field_as<std::int64_t>(descriptor, "message_id") == 123);
    CHECK(field_as<std::int64_t>(descriptor, "media_timestamp") == 0);
    CHECK(field_as<std::int64_t>(descriptor, "checklist_task_id") == 0);
    CHECK(field_as<std::string>(descriptor, "poll_option_id").empty());
    CHECK_FALSE(field_as<bool>(descriptor, "for_album"));
    CHECK_FALSE(field_as<bool>(descriptor, "in_message_thread"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(*outcome.result == json{{"chat_id", -1001},
                                  {"message_id", 123},
                                  {"link", "urn:telegram:message"},
                                  {"is_public", false}});
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-link.result.schema.json"));
}

TEST_CASE("msg link integrity and TD failures keep their closed outer attribution",
          "[msg][link][error][fake-boundary]") {
    SECTION("empty link") {
        FakeMessages fake;
        auto pending = fake.dispatch(link_request("-1001", 123));
        resolve_numeric(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageLink,
                     tgcli::core::TdMessageLink{.link = "", .is_public = true});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "msg_link"}, {"reason", "internal_error"}});
    }
    SECTION("invalid UTF-8 link") {
        FakeMessages fake;
        auto pending = fake.dispatch(link_request("-1001", 123));
        resolve_numeric(fake);
        fake.respond(
            tgcli::core::TdFunctionKind::GetMessageLink,
            tgcli::core::TdMessageLink{.link = std::string("\xED\xA0\x80", 3), .is_public = true});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("documented 404") {
        FakeMessages fake;
        auto pending = fake.dispatch(link_request("-1001", 123));
        resolve_numeric(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageLink,
                     tgcli::core::TdError{404, "Not Found"});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"chat_id", -1001}, {"message_id", 123}});
    }
    SECTION("permission failure") {
        FakeMessages fake;
        auto pending = fake.dispatch(link_request("-1001", 123));
        resolve_numeric(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageLink,
                     tgcli::core::TdError{400, "MESSAGE_LINK_UNAVAILABLE"});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "msg_link"}, {"tdlib_code", 400}});
    }
}

TEST_CASE("msg consumers use explicit ids while retaining contextual selector metadata",
          "[msg][resolver][bot][fake-boundary]") {
    FakeMessages fake;
    auto pending = fake.dispatch(get_request("https://t.me/project/999", {123}));
    fake.respond_me(true);
    fake.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                 tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::Message,
                                             .username = {},
                                             .url = "https://t.me/project/999",
                                             .tdlib_type_id = 1});
    fake.respond(tgcli::core::TdFunctionKind::GetMessageLinkInfo,
                 tgcli::core::TdMessageLinkInfo{
                     .is_public = true,
                     .chat_id = -1001,
                     .message_id = 999,
                     .topic = tgcli::core::TdTopic{
                         .kind = tgcli::core::TdTopicKind::Forum, .id = 7, .tdlib_type_id = 1}});
    fake.respond(tgcli::core::TdFunctionKind::GetChat, chat(-1001));
    const auto descriptor =
        fake.respond(tgcli::core::TdFunctionKind::GetMessages,
                     tgcli::core::TdMessages{.messages = {message(-1001, 123)}});
    CHECK(field_as<std::vector<std::int64_t>>(descriptor, "message_ids") ==
          std::vector<std::int64_t>{123});
    CHECK(pending.get().result.has_value());
}

TEST_CASE("msg secret targets stop before either selected target call",
          "[msg][resolver][secret][fake-boundary]") {
    for (const auto& request : {get_request("-1001", {123}), link_request("-1001", 123)}) {
        FakeMessages fake;
        auto pending = fake.dispatch(request);
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChat,
                     chat(-1001, tgcli::core::TdChatKind::Secret));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"argument", "selector"}, {"reason", "unsupported_chat_type"}});
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessages) == 0);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageLink) == 0);
    }
}

TEST_CASE("msg validation rejects invalid id collections before TD calls",
          "[msg][usage][fake-boundary]") {
    for (auto request : {get_request("-1001", {}), get_request("-1001", {0}),
                         get_request("-1001", {9007199254740992LL}), link_request("-1001", 0)}) {
        FakeMessages fake;
        const auto outcome = fake.dispatch(std::move(request)).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }
}
