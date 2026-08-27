#include "common/exit_codes.hpp"
#include "daemon/read_commands.hpp"
#include "daemon/read_domain.hpp"
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
    int exit_code = -1;
    int terminal_count = 0;
};

class FakeRead {
  public:
    FakeRead() {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ = std::make_unique<tgcli::daemon::ReadCoordinator>(
            *client_, "main", [] { return std::chrono::system_clock::time_point{}; });
        tgcli::daemon::register_read_command(dispatcher_, *coordinator_);
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
        return respond_value(expected, tgcli::core::TdValue::from(std::move(value)));
    }

    tgcli::core::TdFunctionData respond_value(tgcli::core::TdFunctionKind expected,
                                              tgcli::core::TdValue value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        const auto descriptor = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id, std::move(value));
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

    void respond_chat(std::int64_t id = -1001,
                      tgcli::core::TdChatKind kind = tgcli::core::TdChatKind::BasicGroup,
                      std::int64_t related_id = 0) {
        respond(tgcli::core::TdFunctionKind::GetChat,
                tgcli::core::TdChat{.id = id,
                                    .title = "Project",
                                    .kind = kind,
                                    .related_id = related_id,
                                    .tdlib_type_id = 1,
                                    .positions = {},
                                    .chat_lists = {},
                                    .is_marked_unread = false,
                                    .unread_count = 0,
                                    .unread_mention_count = 0,
                                    .unread_reaction_count = 0,
                                    .unread_poll_vote_count = 0,
                                    .last_message = std::nullopt,
                                    .permissions = std::nullopt,
                                    .notification_settings = std::nullopt});
        if (kind == tgcli::core::TdChatKind::Supergroup ||
            kind == tgcli::core::TdChatKind::Channel) {
            respond(
                tgcli::core::TdFunctionKind::GetSupergroup,
                tgcli::core::TdSupergroup{.id = related_id,
                                          .usernames = {"project"},
                                          .is_channel = kind == tgcli::core::TdChatKind::Channel});
        } else if (kind == tgcli::core::TdChatKind::Private) {
            respond(tgcli::core::TdFunctionKind::GetUser,
                    tgcli::core::TdUserSummary{.id = related_id,
                                               .first_name = "Ada",
                                               .last_name = "",
                                               .usernames = {"ada"},
                                               .phone_number = "12025550123",
                                               .is_bot = false,
                                               .is_premium = false});
        }
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

    [[nodiscard]] std::size_t count(tgcli::core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    [[nodiscard]] std::vector<tgcli::core::TdFunctionKind> command_calls() const {
        std::vector<tgcli::core::TdFunctionKind> result;
        const auto sent = runtime_->sent_functions();
        result.reserve(sent.size() > 1 ? sent.size() - 1 : 0);
        for (const auto& call : sent | std::views::drop(1)) {
            const auto kind = call.function.kind();
            REQUIRE(kind);
            result.push_back(*kind);
        }
        return result;
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
    std::unique_ptr<tgcli::daemon::ReadCoordinator> coordinator_;
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

tgcli::core::TdMessageSummary message(std::int64_t chat_id, std::int64_t id, std::int32_t date = 20,
                                      std::optional<tgcli::core::TdTopic> topic = std::nullopt) {
    return {
        .id = id,
        .chat_id = chat_id,
        .date = date,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = false,
        .topic = topic,
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "message or caption"};
}

tgcli::proto::Request request(std::string selector = "-1001", std::int32_t limit = 20,
                              bool local = false) {
    tgcli::proto::Request result("main");
    result.command = {"read"};
    result.args = {{"chat", std::move(selector)},
                   {"before", nullptr},
                   {"since", nullptr},
                   {"until", nullptr},
                   {"topic", nullptr},
                   {"local", local},
                   {"limit", limit},
                   {"cursor", nullptr}};
    result.context.timeout_seconds = 2.0;
    result.context.cwd = "/";
    return result;
}

tgcli::proto::Request cursor_request(const tgcli::daemon::ReadCursor& cursor) {
    auto result = request();
    result.args = {{"chat", nullptr},  {"before", nullptr},
                   {"since", nullptr}, {"until", nullptr},
                   {"topic", nullptr}, {"local", false},
                   {"limit", nullptr}, {"cursor", tgcli::daemon::encode_read_cursor(cursor)}};
    return result;
}

void resolve_basic(FakeRead& fake) {
    fake.respond_me();
    fake.respond_chat();
}

} // namespace

TEST_CASE("read returns an advancing raw cursor and schema-valid shared summaries",
          "[read][fake-boundary][schema]") {
    FakeRead fake;
    auto pending = fake.dispatch(request("-1001", 1));
    resolve_basic(fake);
    const auto descriptor =
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100)}});
    CHECK(field_as<std::int64_t>(descriptor, "chat_id") == -1001);
    CHECK(field_as<std::int64_t>(descriptor, "from_message_id") == 0);
    CHECK(field_as<std::int64_t>(descriptor, "offset") == 0);
    CHECK(field_as<std::int64_t>(descriptor, "limit") == 1);
    CHECK_FALSE(field_as<bool>(descriptor, "only_local"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["boundary"] == "page");
    REQUIRE((*outcome.result)["items"].size() == 1);
    const auto cursor =
        tgcli::daemon::decode_read_cursor((*outcome.result)["next"].get<std::string>());
    REQUIRE(cursor);
    CHECK(cursor->from_message_id == 100);
    CHECK(cursor->history_chat_id == -1001);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("read.result.schema.json"));
}

TEST_CASE("read refetches an unconsumed match after an exclusive anchor vanished",
          "[read][anchor][filter][fake-boundary]") {
    FakeRead fake;
    auto input = request("-1001", 1, true);
    input.args["before"] = 100;
    input.args["topic"] = "forum:7";
    auto pending = fake.dispatch(std::move(input));
    resolve_basic(fake);
    const tgcli::core::TdTopic forum{
        .kind = tgcli::core::TdTopicKind::Forum, .id = 7, .tdlib_type_id = 1};

    const auto missing_anchor = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 2,
                                .messages = {message(-1001, 99), message(-1001, 98, 20, forum)}});
    CHECK(field_as<std::int64_t>(missing_anchor, "from_message_id") == 100);
    CHECK(field_as<std::int64_t>(missing_anchor, "limit") == 2);
    CHECK(field_as<bool>(missing_anchor, "only_local"));

    const auto resumed = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 2,
                                .messages = {message(-1001, 99), message(-1001, 98, 20, forum)}});
    CHECK(field_as<std::int64_t>(resumed, "from_message_id") == 99);
    CHECK(field_as<std::int64_t>(resumed, "limit") == 2);
    CHECK(field_as<bool>(resumed, "only_local"));

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK_FALSE(outcome.error);
    CHECK(outcome.terminal_count == 1);
    REQUIRE((*outcome.result)["items"].size() == 1);
    CHECK((*outcome.result)["items"][0]["id"] == 98);
    const auto cursor =
        tgcli::daemon::decode_read_cursor((*outcome.result)["next"].get<std::string>());
    REQUIRE(cursor);
    CHECK(cursor->from_message_id == 98);
    CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe,
                                              tgcli::core::TdFunctionKind::GetChat,
                                              tgcli::core::TdFunctionKind::GetChatHistory,
                                              tgcli::core::TdFunctionKind::GetChatHistory});
}

TEST_CASE("read preserves before plus until ordering and terminates only on exact since anchor",
          "[read][anchor][fake-boundary]") {
    FakeRead fake;
    auto input = request("-1001", 2);
    input.args["before"] = 500;
    input.args["since"] = "1970-01-01T00:00:10Z";
    input.args["until"] = "1970-01-01T00:00:20Z";
    auto pending = fake.dispatch(std::move(input));
    resolve_basic(fake);
    const auto until =
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, message(-1001, 800, 20));
    CHECK(field_as<std::int64_t>(until, "chat_id") == -1001);
    CHECK(field_as<std::int64_t>(until, "date") == 20);
    const auto since =
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, message(-1001, 100, 9));
    CHECK(field_as<std::int64_t>(since, "date") == 9);
    const auto first = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 3,
                                .messages = {message(-1001, 500, 40), message(-1001, 400, 30),
                                             message(-1001, 300, 15)}});
    CHECK(field_as<std::int64_t>(first, "from_message_id") == 500);
    CHECK(field_as<std::int64_t>(first, "limit") == 3);
    fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 2,
                                .messages = {message(-1001, 300, 15), message(-1001, 100, 9)}});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["boundary"] == "time_anchor");
    CHECK((*outcome.result)["next"].is_null());
    REQUIRE((*outcome.result)["items"].size() == 1);
    CHECK((*outcome.result)["items"][0]["id"] == 300);
}

TEST_CASE("read handles empty-until and INT32_MIN since without a nonexistent earlier probe",
          "[read][anchor][fake-boundary]") {
    SECTION("until 404 is terminal before history") {
        FakeRead fake;
        auto input = request();
        input.args["until"] = "1970-01-01";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate,
                     tgcli::core::TdError{404, "not found"});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result) == json{{"items", json::array()},
                                        {"next", nullptr},
                                        {"boundary", "empty_before_until"}});
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 0);
    }
    SECTION("minimum since skips the date probe") {
        FakeRead fake;
        auto input = request();
        input.args["since"] = "1901-12-13T20:45:52Z";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 100, .messages = {}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["boundary"] == "tdlib_idle");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatMessageByDate) == 0);
    }
}

TEST_CASE("read thread metadata maps channel history while preserving original history arguments",
          "[read][thread][fake-boundary]") {
    FakeRead fake;
    auto input = request("-1001", 1);
    input.args["topic"] = "thread:500";
    auto pending = fake.dispatch(std::move(input));
    fake.respond_me();
    fake.respond_chat(-1001, tgcli::core::TdChatKind::Channel, 77);
    const auto metadata =
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThread,
                     tgcli::core::TdMessageThreadInfo{
                         .history_chat_id = -2001,
                         .history_thread_id = 150,
                         .starting_messages = {message(-2001, 200), message(-2001, 150)}});
    CHECK(field_as<std::int64_t>(metadata, "chat_id") == -1001);
    CHECK(field_as<std::int64_t>(metadata, "message_id") == 500);
    const auto history =
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThreadHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(-2001, 140)}});
    CHECK(field_as<std::int64_t>(history, "chat_id") == -1001);
    CHECK(field_as<std::int64_t>(history, "message_id") == 500);
    CHECK(field_as<std::int64_t>(history, "from_message_id") == 0);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["chat_id"] == -2001);
    const auto cursor =
        tgcli::daemon::decode_read_cursor((*outcome.result)["next"].get<std::string>());
    REQUIRE(cursor);
    CHECK(cursor->chat_id == -1001);
    CHECK(cursor->history_chat_id == -2001);
}

TEST_CASE("read validates same-chat thread metadata and rejects cross-chat metadata elsewhere",
          "[read][thread][integrity][fake-boundary]") {
    SECTION("same supergroup") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "thread:500";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Supergroup, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThread,
                     tgcli::core::TdMessageThreadInfo{
                         .history_chat_id = -1001,
                         .history_thread_id = 500,
                         .starting_messages = {message(-1001, 600), message(-1001, 500)}});
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThreadHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 400)}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["items"][0]["chat_id"] == -1001);
    }
    SECTION("basic group cannot map to another chat") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "thread:500";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThread,
                     tgcli::core::TdMessageThreadInfo{
                         .history_chat_id = -2001,
                         .history_thread_id = 150,
                         .starting_messages = {message(-2001, 200), message(-2001, 150)}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThreadHistory) == 0);
    }
    SECTION("null starting position is malformed") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "thread:500";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Channel, 77);
        fake.respond(
            tgcli::core::TdFunctionKind::GetMessageThread,
            tgcli::core::TdMessageThreadInfo{
                .history_chat_id = -2001,
                .history_thread_id = 150,
                .starting_messages = {message(-2001, 200), std::nullopt, message(-2001, 150)}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
}

TEST_CASE("read dispatches every non-thread topic seam and enforces Saved ownership",
          "[read][topic][fake-boundary]") {
    SECTION("forum") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "forum:7";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        const auto descriptor = fake.respond(
            tgcli::core::TdFunctionKind::GetForumTopicHistory,
            tgcli::core::TdMessages{
                .total_count = 1,
                .messages = {message(
                    -1001, 100, 20, tgcli::core::TdTopic{tgcli::core::TdTopicKind::Forum, 7, 1})}});
        CHECK(field_as<std::int64_t>(descriptor, "forum_topic_id") == 7);
        CHECK(pending.get().result);
    }
    SECTION("direct") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "direct:8";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        const auto descriptor =
            fake.respond(tgcli::core::TdFunctionKind::GetDirectMessagesChatTopicHistory,
                         tgcli::core::TdMessages{
                             .total_count = 1,
                             .messages = {message(
                                 -1001, 100, 20,
                                 tgcli::core::TdTopic{tgcli::core::TdTopicKind::Direct, 8, 1})}});
        CHECK(field_as<std::int64_t>(descriptor, "topic_id") == 8);
        CHECK(pending.get().result);
    }
    SECTION("saved") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "saved:9";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Private, 42);
        const auto ownership =
            fake.respond(tgcli::core::TdFunctionKind::CreatePrivateChat,
                         tgcli::core::TdChat{.id = -1001,
                                             .title = "Saved Messages",
                                             .kind = tgcli::core::TdChatKind::Private,
                                             .related_id = 42,
                                             .tdlib_type_id = 1,
                                             .positions = {},
                                             .chat_lists = {},
                                             .is_marked_unread = false,
                                             .unread_count = 0,
                                             .unread_mention_count = 0,
                                             .unread_reaction_count = 0,
                                             .unread_poll_vote_count = 0,
                                             .last_message = std::nullopt,
                                             .permissions = std::nullopt,
                                             .notification_settings = std::nullopt});
        CHECK(field_as<std::int64_t>(ownership, "user_id") == 42);
        CHECK_FALSE(field_as<bool>(ownership, "force"));
        const auto descriptor = fake.respond(
            tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory,
            tgcli::core::TdMessages{
                .total_count = 1,
                .messages = {message(
                    -1001, 100, 20, tgcli::core::TdTopic{tgcli::core::TdTopicKind::Saved, 9, 1})}});
        CHECK(field_as<std::int64_t>(descriptor, "saved_messages_topic_id") == 9);
        CHECK(pending.get().result);
    }
}

TEST_CASE("read Saved ownership reuses resolver materialization and closes failure mapping",
          "[read][saved][integrity][fake-boundary]") {
    SECTION("Saved link cache avoids a second ownership call") {
        FakeRead fake;
        auto input = request("t.me/saved", 1);
        input.args["topic"] = "saved:9";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond(
            tgcli::core::TdFunctionKind::GetInternalLinkType,
            tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::SavedMessages,
                                        .username = {},
                                        .url = "t.me/saved",
                                        .tdlib_type_id = 1});
        fake.respond(tgcli::core::TdFunctionKind::CreatePrivateChat,
                     tgcli::core::TdChat{.id = -1001,
                                         .title = "Saved Messages",
                                         .kind = tgcli::core::TdChatKind::Private,
                                         .related_id = 42,
                                         .tdlib_type_id = 1,
                                         .positions = {},
                                         .chat_lists = {},
                                         .is_marked_unread = false,
                                         .unread_count = 0,
                                         .unread_mention_count = 0,
                                         .unread_reaction_count = 0,
                                         .unread_poll_vote_count = 0,
                                         .last_message = std::nullopt,
                                         .permissions = std::nullopt,
                                         .notification_settings = std::nullopt});
        fake.respond(tgcli::core::TdFunctionKind::GetUser,
                     tgcli::core::TdUserSummary{.id = 42,
                                                .first_name = "Ada",
                                                .last_name = "",
                                                .usernames = {"ada"},
                                                .phone_number = "12025550123",
                                                .is_bot = false,
                                                .is_premium = false});
        fake.respond(
            tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory,
            tgcli::core::TdMessages{
                .total_count = 1,
                .messages = {message(
                    -1001, 100, 20, tgcli::core::TdTopic{tgcli::core::TdTopicKind::Saved, 9, 1})}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::CreatePrivateChat) == 1);
        CHECK(fake.command_calls() ==
              std::vector{tgcli::core::TdFunctionKind::GetMe,
                          tgcli::core::TdFunctionKind::GetInternalLinkType,
                          tgcli::core::TdFunctionKind::CreatePrivateChat,
                          tgcli::core::TdFunctionKind::GetUser,
                          tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory});
    }

    SECTION("wrong ownership chat is refused without history") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "saved:9";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Private, 42);
        fake.respond(tgcli::core::TdFunctionKind::CreatePrivateChat,
                     tgcli::core::TdChat{.id = -2001,
                                         .title = "Saved Messages",
                                         .kind = tgcli::core::TdChatKind::Private,
                                         .related_id = 42,
                                         .tdlib_type_id = 1,
                                         .positions = {},
                                         .chat_lists = {},
                                         .is_marked_unread = false,
                                         .unread_count = 0,
                                         .unread_mention_count = 0,
                                         .unread_reaction_count = 0,
                                         .unread_poll_vote_count = 0,
                                         .last_message = std::nullopt,
                                         .permissions = std::nullopt,
                                         .notification_settings = std::nullopt});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"argument", "--topic"}, {"reason", "invalid_argument"}});
        CHECK_FALSE(outcome.result);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory) == 0);
        CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe,
                                                  tgcli::core::TdFunctionKind::GetChat,
                                                  tgcli::core::TdFunctionKind::GetUser,
                                                  tgcli::core::TdFunctionKind::CreatePrivateChat});
    }

    SECTION("malformed ownership response is internal") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "saved:9";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Private, 42);
        fake.respond_value(tgcli::core::TdFunctionKind::CreatePrivateChat,
                           tgcli::core::TdValue::from(tgcli::core::TdMessages{}));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "read"}, {"reason", "internal_error"}});
        CHECK_FALSE(outcome.result);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory) == 0);
        CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe,
                                                  tgcli::core::TdFunctionKind::GetChat,
                                                  tgcli::core::TdFunctionKind::GetUser,
                                                  tgcli::core::TdFunctionKind::CreatePrivateChat});
    }

    SECTION("ownership TD error keeps read attribution") {
        FakeRead fake;
        auto input = request("-1001", 1);
        input.args["topic"] = "saved:9";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Private, 42);
        fake.respond(tgcli::core::TdFunctionKind::CreatePrivateChat,
                     tgcli::core::TdError{400, "bad request"});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "read"}, {"tdlib_code", 400}});
        CHECK_FALSE(outcome.result);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory) == 0);
        CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe,
                                                  tgcli::core::TdFunctionKind::GetChat,
                                                  tgcli::core::TdFunctionKind::GetUser,
                                                  tgcli::core::TdFunctionKind::CreatePrivateChat});
    }

    SECTION("Saved cursor repeats ownership validation") {
        FakeRead fake;
        const tgcli::daemon::ReadCursor cursor{
            .version = 1,
            .operation = "read",
            .account = "main",
            .user_id = 42,
            .limit = 1,
            .chat_id = -1001,
            .history_chat_id = -1001,
            .topic = tgcli::daemon::TopicRef{tgcli::daemon::TopicKind::Saved, 9},
            .local = false,
            .since = std::nullopt,
            .until = std::nullopt,
            .since_cutoff_message_id = std::nullopt,
            .from_message_id = 100};
        auto pending = fake.dispatch(cursor_request(cursor));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Private, 42);
        fake.respond(tgcli::core::TdFunctionKind::CreatePrivateChat,
                     tgcli::core::TdChat{.id = -1001,
                                         .title = "Saved Messages",
                                         .kind = tgcli::core::TdChatKind::Private,
                                         .related_id = 42,
                                         .tdlib_type_id = 1,
                                         .positions = {},
                                         .chat_lists = {},
                                         .is_marked_unread = false,
                                         .unread_count = 0,
                                         .unread_mention_count = 0,
                                         .unread_reaction_count = 0,
                                         .unread_poll_vote_count = 0,
                                         .last_message = std::nullopt,
                                         .permissions = std::nullopt,
                                         .notification_settings = std::nullopt});
        const auto history = fake.respond(
            tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory,
            tgcli::core::TdMessages{
                .total_count = 2,
                .messages = {
                    message(-1001, 100),
                    message(-1001, 99, 20,
                            tgcli::core::TdTopic{tgcli::core::TdTopicKind::Saved, 9, 1})}});
        CHECK(field_as<std::int64_t>(history, "from_message_id") == 100);
        CHECK(field_as<std::int64_t>(history, "limit") == 2);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::CreatePrivateChat) == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatMessageByDate) == 0);
        CHECK(fake.command_calls() ==
              std::vector{tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdFunctionKind::GetChat,
                          tgcli::core::TdFunctionKind::GetUser,
                          tgcli::core::TdFunctionKind::CreatePrivateChat,
                          tgcli::core::TdFunctionKind::GetSavedMessagesTopicHistory});
    }
}

TEST_CASE("read local admission makes no link call and refuses channel thread mapping",
          "[read][local][fake-boundary]") {
    SECTION("invalid URL-like syntax stops before Ready") {
        FakeRead fake;
        auto pending = fake.dispatch(request("https://T.me/project", 20, true));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.sent_count() == 1);
    }
    SECTION("channel thread stops before metadata or history") {
        FakeRead fake;
        auto input = request("-1001", 20, true);
        input.args["topic"] = "thread:500";
        auto pending = fake.dispatch(std::move(input));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Channel, 77);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"argument", "--topic"}, {"reason", "unsupported_mode"}});
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThread) == 0);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 0);
    }
    SECTION("filtered local progress returns a cursor before exposing local boundary") {
        FakeRead fake;
        auto input = request("-1001", 1, true);
        input.args["since"] = "1970-01-01T00:00:10Z";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        const auto first = fake.respond(
            tgcli::core::TdFunctionKind::GetChatHistory,
            tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100, 1)}});
        CHECK(field_as<bool>(first, "only_local"));
        const auto second = fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                                         tgcli::core::TdMessages{.total_count = 0, .messages = {}});
        CHECK(field_as<std::int64_t>(second, "from_message_id") == 100);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["boundary"] == "page");
        CHECK((*outcome.result)["items"].empty());
        CHECK((*outcome.result)["next"].is_string());
    }
}

TEST_CASE("read local supergroup threads use same-chat local history without metadata",
          "[read][local][thread][fake-boundary]") {
    FakeRead fake;
    auto input = request("-1001", 1, true);
    input.args["topic"] = "thread:500";
    auto pending = fake.dispatch(std::move(input));
    fake.respond_me();
    fake.respond_chat(-1001, tgcli::core::TdChatKind::Supergroup, 77);
    const auto history = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{
            .total_count = 1,
            .messages = {message(-1001, 100, 20,
                                 tgcli::core::TdTopic{tgcli::core::TdTopicKind::Thread, 500, 1})}});
    CHECK(field_as<std::int64_t>(history, "chat_id") == -1001);
    CHECK(field_as<bool>(history, "only_local"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK_FALSE(outcome.error);
    CHECK(outcome.terminal_count == 1);
    CHECK((*outcome.result)["items"][0]["topic"] == json{{"kind", "thread"}, {"id", 500}});
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThread) == 0);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThreadHistory) == 0);
    CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe,
                                              tgcli::core::TdFunctionKind::GetChat,
                                              tgcli::core::TdFunctionKind::GetSupergroup,
                                              tgcli::core::TdFunctionKind::GetChatHistory});
}

TEST_CASE("read continues live short pages until the output limit",
          "[read][pagination][fake-boundary]") {
    FakeRead fake;
    auto pending = fake.dispatch(request("-1001", 3));
    resolve_basic(fake);
    const auto first =
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100)}});
    CHECK(field_as<std::int64_t>(first, "from_message_id") == 0);
    CHECK(field_as<std::int64_t>(first, "limit") == 3);
    const auto second =
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{
                         .total_count = 2, .messages = {message(-1001, 100), message(-1001, 99)}});
    CHECK(field_as<std::int64_t>(second, "from_message_id") == 100);
    CHECK(field_as<std::int64_t>(second, "limit") == 3);
    const auto third =
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 2,
                                             .messages = {message(-1001, 99), message(-1001, 98)}});
    CHECK(field_as<std::int64_t>(third, "from_message_id") == 99);
    CHECK(field_as<std::int64_t>(third, "limit") == 2);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK_FALSE(outcome.error);
    CHECK(outcome.terminal_count == 1);
    REQUIRE((*outcome.result)["items"].size() == 3);
    CHECK((*outcome.result)["items"][0]["id"] == 100);
    CHECK((*outcome.result)["items"][1]["id"] == 99);
    CHECK((*outcome.result)["items"][2]["id"] == 98);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 3);
    CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe,
                                              tgcli::core::TdFunctionKind::GetChat,
                                              tgcli::core::TdFunctionKind::GetChatHistory,
                                              tgcli::core::TdFunctionKind::GetChatHistory,
                                              tgcli::core::TdFunctionKind::GetChatHistory});
}

TEST_CASE("read continuation exposes a terminal boundary after an empty filtered page",
          "[read][pagination][filter][cursor][fake-boundary]") {
    FakeRead fake;
    auto input = request("-1001", 1, true);
    input.args["since"] = "1970-01-01T00:00:10Z";
    auto first_pending = fake.dispatch(std::move(input));
    resolve_basic(fake);
    fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                 tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100, 1)}});
    fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                 tgcli::core::TdMessages{.total_count = 0, .messages = {}});
    const auto first = first_pending.get();
    REQUIRE(first.result);
    CHECK_FALSE(first.error);
    CHECK(first.terminal_count == 1);
    CHECK((*first.result)["items"].empty());
    CHECK((*first.result)["boundary"] == "page");
    const auto cursor =
        tgcli::daemon::decode_read_cursor((*first.result)["next"].get<std::string>());
    REQUIRE(cursor);
    CHECK(cursor->from_message_id == 100);

    auto second_pending = fake.dispatch(cursor_request(*cursor));
    resolve_basic(fake);
    const auto boundary = fake.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100, 1)}});
    CHECK(field_as<std::int64_t>(boundary, "from_message_id") == 100);
    CHECK(field_as<bool>(boundary, "only_local"));
    const auto second = second_pending.get();
    REQUIRE(second.result);
    CHECK_FALSE(second.error);
    CHECK(second.terminal_count == 1);
    CHECK((*second.result) ==
          json{{"items", json::array()}, {"next", nullptr}, {"boundary", "local_boundary"}});
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatMessageByDate) == 0);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatHistory) == 3);
    CHECK(fake.command_calls() ==
          std::vector{tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdFunctionKind::GetChat,
                      tgcli::core::TdFunctionKind::GetChatHistory,
                      tgcli::core::TdFunctionKind::GetChatHistory,
                      tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdFunctionKind::GetChat,
                      tgcli::core::TdFunctionKind::GetChatHistory});
}

TEST_CASE("read bot preflight and cursor continuation preserve call exclusions and scope",
          "[read][bot][cursor][fake-boundary]") {
    SECTION("bot stops before selector") {
        FakeRead fake;
        auto pending = fake.dispatch(request());
        fake.respond_me(true);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 0);
    }
    SECTION("continuation repeats no date probe") {
        FakeRead fake;
        const tgcli::daemon::ReadCursor cursor{.version = 1,
                                               .operation = "read",
                                               .account = "main",
                                               .user_id = 42,
                                               .limit = 20,
                                               .chat_id = -1001,
                                               .history_chat_id = -1001,
                                               .topic = std::nullopt,
                                               .local = false,
                                               .since = 10,
                                               .until = 20,
                                               .since_cutoff_message_id = 50,
                                               .from_message_id = 100};
        auto pending = fake.dispatch(cursor_request(cursor));
        resolve_basic(fake);
        const auto history = fake.respond(
            tgcli::core::TdFunctionKind::GetChatHistory,
            tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100, 20)}});
        CHECK(field_as<std::int64_t>(history, "from_message_id") == 100);
        CHECK(field_as<std::int64_t>(history, "limit") == 21);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["boundary"] == "tdlib_idle");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatMessageByDate) == 0);
    }
    SECTION("account scope mismatch stops before target resolution") {
        FakeRead fake;
        const tgcli::daemon::ReadCursor cursor{.version = 1,
                                               .operation = "read",
                                               .account = "other",
                                               .user_id = 42,
                                               .limit = 20,
                                               .chat_id = -1001,
                                               .history_chat_id = -1001,
                                               .topic = std::nullopt,
                                               .local = false,
                                               .since = std::nullopt,
                                               .until = std::nullopt,
                                               .since_cutoff_message_id = std::nullopt,
                                               .from_message_id = 100};
        auto pending = fake.dispatch(cursor_request(cursor));
        fake.respond_me();
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "cursor_scope_mismatch");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 0);
    }
    SECTION("operation and user mismatches stop before target resolution") {
        for (const auto& [operation, user_id] :
             std::vector<std::pair<std::string, std::int64_t>>{{"history", 42}, {"read", 43}}) {
            FakeRead fake;
            const tgcli::daemon::ReadCursor cursor{.version = 1,
                                                   .operation = operation,
                                                   .account = "main",
                                                   .user_id = user_id,
                                                   .limit = 20,
                                                   .chat_id = -1001,
                                                   .history_chat_id = -1001,
                                                   .topic = std::nullopt,
                                                   .local = false,
                                                   .since = std::nullopt,
                                                   .until = std::nullopt,
                                                   .since_cutoff_message_id = std::nullopt,
                                                   .from_message_id = 100};
            auto pending = fake.dispatch(cursor_request(cursor));
            fake.respond_me();
            const auto outcome = pending.get();
            INFO(operation << " " << user_id);
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["details"]["reason"] == "cursor_scope_mismatch");
            CHECK_FALSE(outcome.result);
            CHECK(outcome.terminal_count == 1);
            CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 0);
            CHECK(fake.command_calls() == std::vector{tgcli::core::TdFunctionKind::GetMe});
        }
    }
    SECTION("thread metadata mismatch is a cursor scope failure") {
        FakeRead fake;
        const tgcli::daemon::ReadCursor cursor{
            .version = 1,
            .operation = "read",
            .account = "main",
            .user_id = 42,
            .limit = 20,
            .chat_id = -1001,
            .history_chat_id = -2001,
            .topic = tgcli::daemon::TopicRef{tgcli::daemon::TopicKind::Thread, 500},
            .local = false,
            .since = std::nullopt,
            .until = std::nullopt,
            .since_cutoff_message_id = std::nullopt,
            .from_message_id = 100};
        auto pending = fake.dispatch(cursor_request(cursor));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Channel, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThread,
                     tgcli::core::TdMessageThreadInfo{
                         .history_chat_id = -2002,
                         .history_thread_id = 150,
                         .starting_messages = {message(-2002, 200), message(-2002, 150)}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "cursor_scope_mismatch");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThreadHistory) == 0);
    }
    SECTION("thread continuation revalidates matching cross-chat metadata") {
        FakeRead fake;
        const tgcli::daemon::ReadCursor cursor{
            .version = 1,
            .operation = "read",
            .account = "main",
            .user_id = 42,
            .limit = 1,
            .chat_id = -1001,
            .history_chat_id = -2001,
            .topic = tgcli::daemon::TopicRef{tgcli::daemon::TopicKind::Thread, 500},
            .local = false,
            .since = std::nullopt,
            .until = std::nullopt,
            .since_cutoff_message_id = std::nullopt,
            .from_message_id = 100};
        auto pending = fake.dispatch(cursor_request(cursor));
        fake.respond_me();
        fake.respond_chat(-1001, tgcli::core::TdChatKind::Channel, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetMessageThread,
                     tgcli::core::TdMessageThreadInfo{
                         .history_chat_id = -2001,
                         .history_thread_id = 150,
                         .starting_messages = {message(-2001, 200), message(-2001, 150)}});
        const auto history = fake.respond(
            tgcli::core::TdFunctionKind::GetMessageThreadHistory,
            tgcli::core::TdMessages{.total_count = 2,
                                    .messages = {message(-2001, 100), message(-2001, 99)}});
        CHECK(field_as<std::int64_t>(history, "chat_id") == -1001);
        CHECK(field_as<std::int64_t>(history, "message_id") == 500);
        CHECK(field_as<std::int64_t>(history, "from_message_id") == 100);
        CHECK(field_as<std::int64_t>(history, "limit") == 2);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 1);
        CHECK((*outcome.result)["items"][0]["chat_id"] == -2001);
        const auto next =
            tgcli::daemon::decode_read_cursor((*outcome.result)["next"].get<std::string>());
        REQUIRE(next);
        CHECK(next->operation == "read");
        CHECK(next->history_chat_id == -2001);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThread) == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMessageThreadHistory) == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChatMessageByDate) == 0);
        CHECK(fake.command_calls() ==
              std::vector{tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdFunctionKind::GetChat,
                          tgcli::core::TdFunctionKind::GetSupergroup,
                          tgcli::core::TdFunctionKind::GetMessageThread,
                          tgcli::core::TdFunctionKind::GetMessageThreadHistory});
    }
}

TEST_CASE("read date probes reject wrong-chat zero-date and too-new successes as internal",
          "[read][anchor][integrity][fake-boundary]") {
    SECTION("until wrong chat") {
        FakeRead fake;
        auto input = request();
        input.args["until"] = "1970-01-01T00:00:20Z";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, message(-2001, 100, 20));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("since too new") {
        FakeRead fake;
        auto input = request();
        input.args["since"] = "1970-01-01T00:00:10Z";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, message(-1001, 100, 10));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
    SECTION("since zero date") {
        FakeRead fake;
        auto input = request();
        input.args["since"] = "1970-01-01T00:00:10Z";
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatMessageByDate, message(-1001, 100, 0));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }
}

TEST_CASE("read rejects malformed and non-advancing pages without partial results",
          "[read][integrity][fake-boundary]") {
    SECTION("non-advancing") {
        FakeRead fake;
        auto input = request();
        input.args["before"] = 100;
        auto pending = fake.dispatch(std::move(input));
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 101)}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PAGINATION_INVALID");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "read"}, {"reason", "non_advancing_upstream"}});
        CHECK_FALSE(outcome.result);
    }
    SECTION("mixed null") {
        FakeRead fake;
        auto pending = fake.dispatch(request());
        resolve_basic(fake);
        fake.respond(tgcli::core::TdFunctionKind::GetChatHistory,
                     tgcli::core::TdMessages{.total_count = 2,
                                             .messages = {message(-1001, 100), std::nullopt}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK_FALSE(outcome.result);
    }
    SECTION("null-only ignores total_count and is idle") {
        FakeRead fake;
        auto pending = fake.dispatch(request());
        resolve_basic(fake);
        fake.respond(
            tgcli::core::TdFunctionKind::GetChatHistory,
            tgcli::core::TdMessages{.total_count = 100, .messages = {std::nullopt, std::nullopt}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["boundary"] == "tdlib_idle");
        CHECK((*outcome.result)["next"].is_null());
    }
}
