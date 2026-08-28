#include "common/exit_codes.hpp"
#include "daemon/m2_read_commands.hpp"
#include "daemon/m2_read_domain.hpp"
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
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using nlohmann::json;

namespace {

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    int terminal_count = 0;
};

enum class Command { Search, Info, Members };

class FakeM2Read {
  public:
    FakeM2Read() {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ = std::make_unique<tgcli::daemon::M2ReadCoordinator>(*client_, "main");
    }

    std::future<Outcome> dispatch(Command command, tgcli::proto::Request request) {
        return std::async(std::launch::async,
                          [this, command, request = std::move(request)]() mutable {
                              Outcome outcome;
                              tgcli::daemon::CallbackSink sink(
                                  [](const json&) {}, [](const json&) {},
                                  [&](json value) {
                                      ++outcome.terminal_count;
                                      outcome.result = std::move(value);
                                  },
                                  [&](std::string code, std::string message, json details, int) {
                                      ++outcome.terminal_count;
                                      outcome.error = json{{"error",
                                                            {{"code", std::move(code)},
                                                             {"message", std::move(message)},
                                                             {"details", std::move(details)}}}};
                                  });
                              tgcli::daemon::RequestSession session(std::move(request), sink);
                              switch (command) {
                              case Command::Search:
                                  coordinator_->search(session.request(), session);
                                  break;
                              case Command::Info:
                                  coordinator_->chat_info(session.request(), session);
                                  break;
                              case Command::Members:
                                  coordinator_->chat_members(session.request(), session);
                                  break;
                              }
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

    void respond_chat(tgcli::core::TdChatKind kind = tgcli::core::TdChatKind::BasicGroup,
                      std::int64_t related_id = 77, std::optional<bool> is_forum = std::nullopt) {
        respond(tgcli::core::TdFunctionKind::GetChat,
                tgcli::core::TdChat{.id = -1001,
                                    .title = "Project",
                                    .kind = kind,
                                    .related_id = related_id,
                                    .tdlib_type_id = 1,
                                    .positions = {},
                                    .chat_lists = {{.kind = tgcli::core::TdChatListKind::Folder,
                                                    .folder_id = 2,
                                                    .tdlib_type_id = 1}},
                                    .is_marked_unread = false,
                                    .unread_count = 3,
                                    .unread_mention_count = 1,
                                    .unread_reaction_count = 0,
                                    .unread_poll_vote_count = 0,
                                    .last_message = std::nullopt,
                                    .permissions = std::nullopt,
                                    .notification_settings = std::nullopt});
        if (kind == tgcli::core::TdChatKind::Supergroup ||
            kind == tgcli::core::TdChatKind::Channel) {
            respond(
                tgcli::core::TdFunctionKind::GetSupergroup,
                tgcli::core::TdSupergroup{
                    .id = related_id,
                    .usernames = {"project"},
                    .is_channel = kind == tgcli::core::TdChatKind::Channel,
                    .is_forum = is_forum.value_or(kind == tgcli::core::TdChatKind::Supergroup)});
        } else if (kind == tgcli::core::TdChatKind::Private) {
            respond_user(related_id, "Private", false);
        }
    }

    void respond_user(std::int64_t id, std::string name, bool bot) {
        respond(tgcli::core::TdFunctionKind::GetUser,
                tgcli::core::TdUserSummary{.id = id,
                                           .first_name = std::move(name),
                                           .last_name = "",
                                           .usernames = {"user" + std::to_string(id)},
                                           .phone_number = bot ? "" : "12025550123",
                                           .is_bot = bot,
                                           .is_premium = false});
    }

    [[nodiscard]] std::size_t count(tgcli::core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    void push_auth(tgcli::core::AuthState state) {
        runtime_->push_update(client_id_, {}, tgcli::core::AuthStateData{state});
        REQUIRE(eventually([&] { return client_->auth_state()->data.state == state; }));
    }

    void wait_for_next(tgcli::core::TdFunctionKind expected) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
    }

    tgcli::daemon::M2ReadCoordinator& coordinator() {
        return *coordinator_;
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
    std::unique_ptr<tgcli::daemon::M2ReadCoordinator> coordinator_;
    std::size_t sent_count_ = 1;
};

tgcli::core::TdMessageSummary
message(std::int64_t id,
        tgcli::core::TdMessageContentKind type = tgcli::core::TdMessageContentKind::Text,
        std::int64_t chat_id = -1001) {
    return {.id = id,
            .chat_id = chat_id,
            .date = static_cast<std::int32_t>(id),
            .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 7, .tdlib_type_id = 1},
            .is_outgoing = false,
            .topic = std::nullopt,
            .content_kind = type,
            .text = "message"};
}

tgcli::core::TdChatMember member(std::int64_t id) {
    return {
        .member = {.kind = tgcli::core::TdMessageSenderKind::User, .id = id, .tdlib_type_id = 1},
        .tag = "tag",
        .inviter_user_id = 42,
        .joined_chat_date = 10,
        .status = {.kind = tgcli::core::TdChatMemberStatusKind::Member,
                   .is_member = true,
                   .unsupported_tdlib_type_id = std::nullopt}};
}

tgcli::core::TdChatMember chat_member(std::int64_t id) {
    auto value = member(1);
    value.member = {.kind = tgcli::core::TdMessageSenderKind::Chat, .id = id, .tdlib_type_id = 2};
    value.status = {.kind = tgcli::core::TdChatMemberStatusKind::Banned,
                    .is_member = false,
                    .unsupported_tdlib_type_id = std::nullopt};
    return value;
}

tgcli::proto::Request search_request(std::int32_t limit = 1) {
    tgcli::proto::Request request("main");
    request.command = {"search"};
    request.args = {{"chat", "-1001"}, {"cursor", nullptr}, {"from", nullptr}, {"global", false},
                    {"limit", limit},  {"query", "needle"}, {"type", "text"}};
    request.context.timeout_seconds = 2.0;
    request.context.cwd = "/";
    return request;
}

tgcli::proto::Request global_search_request(std::int32_t limit = 1) {
    auto request = search_request(limit);
    request.args["chat"] = nullptr;
    request.args["global"] = true;
    return request;
}

tgcli::proto::Request search_cursor_request(const tgcli::daemon::SearchCursor& cursor) {
    auto request = search_request();
    request.args = {{"chat", nullptr},  {"cursor", tgcli::daemon::encode_search_cursor(cursor)},
                    {"from", nullptr},  {"global", false},
                    {"limit", nullptr}, {"query", nullptr},
                    {"type", nullptr}};
    return request;
}

tgcli::proto::Request info_request() {
    tgcli::proto::Request request("main");
    request.command = {"chat", "info"};
    request.args = {{"chat", "-1001"}};
    request.context.timeout_seconds = 2.0;
    request.context.cwd = "/";
    return request;
}

tgcli::proto::Request members_request(std::int32_t limit = 1) {
    tgcli::proto::Request request("main");
    request.command = {"chat", "members"};
    request.args = {{"admins", false},   {"bots", true},   {"chat", "-1001"},
                    {"cursor", nullptr}, {"limit", limit}, {"query", nullptr}};
    request.context.timeout_seconds = 2.0;
    request.context.cwd = "/";
    return request;
}

tgcli::proto::Request members_cursor_request(const tgcli::daemon::MembersCursor& cursor) {
    auto request = members_request();
    request.args = {{"admins", false},  {"bots", false},
                    {"chat", nullptr},  {"cursor", tgcli::daemon::encode_members_cursor(cursor)},
                    {"limit", nullptr}, {"query", nullptr}};
    return request;
}

} // namespace

TEST_CASE("M2 long reads register atomically with frozen config admission",
          "[m2-long-read][dispatch]") {
    FakeM2Read fake;
    tgcli::daemon::Dispatcher dispatcher;
    tgcli::daemon::register_search_command(dispatcher, fake.coordinator());
    tgcli::daemon::register_chat_info_command(dispatcher, fake.coordinator());
    tgcli::daemon::register_chat_members_command(dispatcher, fake.coordinator());
    for (const auto& command :
         {std::vector<std::string>{"search"}, std::vector<std::string>{"chat", "info"},
          std::vector<std::string>{"chat", "members"}}) {
        tgcli::proto::Request request("main");
        request.command = command;
        CHECK(dispatcher.requires_frozen_config_admission(request));
    }
}

TEST_CASE("M2 chat search fills after text postfilter and validates raw markers",
          "[m2-long-read][search][fake-boundary]") {
    FakeM2Read fake;
    auto pending = fake.dispatch(Command::Search, search_request());
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::SearchChatMessages,
                 tgcli::core::TdFoundChatMessages{
                     .total_count = -1,
                     .messages = {message(100, tgcli::core::TdMessageContentKind::Photo)},
                     .next_from_message_id = 90});
    fake.respond(tgcli::core::TdFunctionKind::SearchChatMessages,
                 tgcli::core::TdFoundChatMessages{
                     .total_count = 2, .messages = {message(80)}, .next_from_message_id = 0});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK_FALSE(outcome.error);
    CHECK(outcome.terminal_count == 1);
    REQUIRE((*outcome.result)["items"].size() == 1);
    CHECK((*outcome.result)["items"][0]["id"] == 80);
    CHECK((*outcome.result)["next"].is_null());
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("search.result.schema.json"));
    CHECK(fake.count(tgcli::core::TdFunctionKind::SearchChatMessages) == 2);
}

TEST_CASE("M2 chat info uses observed identity and exact type-specific reads",
          "[m2-long-read][chat-info][fake-boundary]") {
    FakeM2Read fake;
    auto pending = fake.dispatch(Command::Info, info_request());
    fake.respond_me();
    fake.respond_chat();
    fake.respond(
        tgcli::core::TdFunctionKind::GetBasicGroup,
        tgcli::core::TdBasicGroup{
            .id = 77, .member_count = 5, .is_active = true, .upgraded_to_supergroup_id = 0});
    fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                 tgcli::core::TdBasicGroupFullInfo{
                     .description = "project room", .creator_user_id = 42, .members = {}});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["type"] == "basic_group");
    CHECK((*outcome.result)["description"] == "project room");
    CHECK((*outcome.result)["member_count"] == 5);
    CHECK((*outcome.result)["folder_ids"] == json::array({2}));
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("chat-info.result.schema.json"));
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 1);
}

TEST_CASE("M2 chat info covers private supergroup channel and secret capability branches",
          "[m2-long-read][chat-info][fake-boundary]") {
    SECTION("private") {
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Info, info_request());
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Private, 7);
        fake.respond(tgcli::core::TdFunctionKind::GetUserFullInfo,
                     tgcli::core::TdUserFullInfo{.description = "bio"});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["type"] == "private");
        CHECK((*outcome.result)["member_count"].is_null());
        CHECK((*outcome.result)["description"] == "bio");
    }
    for (const auto kind :
         {tgcli::core::TdChatKind::Supergroup, tgcli::core::TdChatKind::Channel}) {
        CAPTURE(kind);
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Info, info_request());
        fake.respond_me();
        fake.respond_chat(kind, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetSupergroupFullInfo,
                     tgcli::core::TdSupergroupFullInfo{.description = "full",
                                                       .member_count = 8,
                                                       .linked_chat_id = -2001,
                                                       .direct_messages_chat_id = 0});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["type"] ==
              (kind == tgcli::core::TdChatKind::Channel ? "channel" : "supergroup"));
        CHECK((*outcome.result)["member_count"] == 8);
        CHECK((*outcome.result)["linked_chat_id"] == -2001);
        CHECK((*outcome.result)["is_forum"] == (kind == tgcli::core::TdChatKind::Supergroup));
    }
    SECTION("secret") {
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Info, info_request());
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Secret, 77);
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "unsupported_chat_type");
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("chat-read.error.schema.json"));
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetUserFullInfo) == 0);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo) == 0);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupFullInfo) == 0);
    }
}

TEST_CASE("M2 channel info rejects a forum observation before full-info dispatch",
          "[m2-long-read][chat-info][integrity][fake-boundary]") {
    FakeM2Read fake;
    auto pending = fake.dispatch(Command::Info, info_request());
    fake.respond_me();
    fake.respond_chat(tgcli::core::TdChatKind::Channel, 77, true);
    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "malformed_tdlib_response");
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupFullInfo) == 0);
}

TEST_CASE("M2 basic members bind source count after full enrichment and filtering",
          "[m2-long-read][chat-members][fake-boundary]") {
    FakeM2Read fake;
    auto pending = fake.dispatch(Command::Members, members_request());
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                 tgcli::core::TdBasicGroupFullInfo{.description = "room",
                                                   .creator_user_id = 42,
                                                   .members = {member(7), member(8), member(9)}});
    fake.respond_user(7, "One", true);
    fake.respond_user(8, "Two", false);
    fake.respond_user(9, "Three", true);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE((*outcome.result)["items"].size() == 1);
    CHECK((*outcome.result)["items"][0]["sender"]["id"] == 7);
    const auto cursor =
        tgcli::daemon::decode_members_cursor((*outcome.result)["next"].get<std::string>());
    REQUIRE(cursor);
    CHECK(cursor->source_count == 2);
    CHECK(cursor->offset == 1);
    CHECK_THAT(*outcome.result,
               tgcli::test::matches_json_schema("chat-members.result.schema.json"));

    auto continuation = members_request();
    continuation.args = {
        {"admins", false},  {"bots", false},
        {"chat", nullptr},  {"cursor", tgcli::daemon::encode_members_cursor(*cursor)},
        {"limit", nullptr}, {"query", nullptr}};
    auto resumed = fake.dispatch(Command::Members, std::move(continuation));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                 tgcli::core::TdBasicGroupFullInfo{.description = "room",
                                                   .creator_user_id = 42,
                                                   .members = {member(7), member(8), member(9)}});
    fake.respond_user(7, "One", true);
    fake.respond_user(8, "Two", false);
    fake.respond_user(9, "Three", false);
    const auto changed = resumed.get();
    CHECK_FALSE(changed.result);
    REQUIRE(changed.error);
    CHECK((*changed.error)["error"]["code"] == "PAGINATION_INVALID");
    CHECK((*changed.error)["error"]["details"]["reason"] == "source_changed");
    CHECK_THAT(*changed.error, tgcli::test::matches_json_schema("chat-read.error.schema.json"));
}

TEST_CASE("M2 supergroup members return one raw page per invocation",
          "[m2-long-read][chat-members][fake-boundary]") {
    FakeM2Read fake;
    auto input = members_request(2);
    input.args["bots"] = false;
    auto pending = fake.dispatch(Command::Members, std::move(input));
    fake.respond_me();
    fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                 tgcli::core::TdChatMembers{.total_count = 1, .members = {member(7)}});
    fake.respond_user(7, "One", false);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE((*outcome.result)["items"].size() == 1);
    REQUIRE((*outcome.result)["next"].is_string());
    const auto cursor = tgcli::daemon::decode_members_cursor(
        (*outcome.result)["next"].get_ref<const std::string&>());
    REQUIRE(cursor);
    CHECK(cursor->offset == 1);
    CHECK_THAT(*outcome.result,
               tgcli::test::matches_json_schema("chat-members.result.schema.json"));
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 1);

    auto resumed = fake.dispatch(Command::Members, members_cursor_request(*cursor));
    fake.respond_me();
    fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                 tgcli::core::TdChatMembers{.total_count = 1, .members = {}});
    const auto exhausted = resumed.get();
    REQUIRE(exhausted.result);
    CHECK((*exhausted.result)["items"].empty());
    CHECK((*exhausted.result)["next"].is_null());
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 2);
}

TEST_CASE("M2 supergroup member cursors advance by a nonempty raw page filtered to zero",
          "[m2-long-read][chat-members][fake-boundary]") {
    FakeM2Read fake;
    auto input = members_request(2);
    auto pending = fake.dispatch(Command::Members, std::move(input));
    fake.respond_me();
    fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                 tgcli::core::TdChatMembers{.total_count = 1, .members = {member(7)}});
    fake.respond_user(7, "One", false);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"].empty());
    REQUIRE((*outcome.result)["next"].is_string());
    const auto cursor = tgcli::daemon::decode_members_cursor(
        (*outcome.result)["next"].get_ref<const std::string&>());
    REQUIRE(cursor);
    CHECK(cursor->offset == 1);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 1);
}

TEST_CASE("M2 members preserve and enrich valid chat senders",
          "[m2-long-read][chat-members][fake-boundary]") {
    FakeM2Read fake;
    auto input = members_request(50);
    input.args["bots"] = false;
    auto pending = fake.dispatch(Command::Members, std::move(input));
    fake.respond_me();
    fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                 tgcli::core::TdChatMembers{.total_count = 1, .members = {chat_member(-2001)}});
    fake.respond(tgcli::core::TdFunctionKind::GetChat,
                 tgcli::core::TdChat{.id = -2001,
                                     .title = "Linked",
                                     .kind = tgcli::core::TdChatKind::Channel,
                                     .related_id = 88,
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
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroup,
                 tgcli::core::TdSupergroup{
                     .id = 88, .usernames = {"linked"}, .is_channel = true, .is_forum = false});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE((*outcome.result)["items"].size() == 1);
    CHECK((*outcome.result)["items"][0] == json{{"sender", {{"type", "chat"}, {"id", -2001}}},
                                                {"is_bot", false},
                                                {"display_name", "Linked"},
                                                {"usernames", json::array({"linked"})},
                                                {"status", "banned"},
                                                {"tag", "tag"},
                                                {"joined_at", "1970-01-01T00:00:10Z"}});
    REQUIRE((*outcome.result)["next"].is_string());
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 1);
}

TEST_CASE("M2 member validation is owner-aware before and after identity hydration",
          "[m2-long-read][chat-members][integrity][fake-boundary]") {
    SECTION("a creator may have left") {
        for (const bool is_member : {false, true}) {
            CAPTURE(is_member);
            FakeM2Read fake;
            auto input = members_request(50);
            input.args["bots"] = false;
            auto creator = member(42);
            creator.status = {.kind = tgcli::core::TdChatMemberStatusKind::Creator,
                              .is_member = is_member,
                              .unsupported_tdlib_type_id = std::nullopt};
            auto pending = fake.dispatch(Command::Members, std::move(input));
            fake.respond_me();
            fake.respond_chat();
            fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                         tgcli::core::TdBasicGroupFullInfo{
                             .description = "room", .creator_user_id = 42, .members = {creator}});
            fake.respond_user(42, "Owner", false);
            const auto outcome = pending.get();
            REQUIRE(outcome.result);
            REQUIRE((*outcome.result)["items"].size() == 1);
            CHECK((*outcome.result)["items"][0]["status"] == "creator");
        }
    }

    for (const auto kind :
         {tgcli::core::TdChatKind::BasicGroup, tgcli::core::TdChatKind::Channel}) {
        CAPTURE(kind);
        FakeM2Read fake;
        auto input = members_request(50);
        input.args["bots"] = false;
        auto restricted = member(7);
        restricted.status = {.kind = tgcli::core::TdChatMemberStatusKind::Restricted,
                             .is_member = true,
                             .unsupported_tdlib_type_id = std::nullopt};
        auto pending = fake.dispatch(Command::Members, std::move(input));
        fake.respond_me();
        fake.respond_chat(kind, 77);
        if (kind == tgcli::core::TdChatKind::BasicGroup) {
            fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                         tgcli::core::TdBasicGroupFullInfo{.description = "room",
                                                           .creator_user_id = 42,
                                                           .members = {restricted}});
        } else {
            fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                         tgcli::core::TdChatMembers{.total_count = 1, .members = {restricted}});
        }
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("basic groups reject chat senders before hydration") {
        FakeM2Read fake;
        auto input = members_request(50);
        input.args["bots"] = false;
        auto pending = fake.dispatch(Command::Members, std::move(input));
        fake.respond_me();
        fake.respond_chat();
        fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                     tgcli::core::TdBasicGroupFullInfo{.description = "room",
                                                       .creator_user_id = 42,
                                                       .members = {chat_member(-2001)}});
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 1);
    }

    SECTION("supergroups and channels reject member-status chat senders before hydration") {
        for (const auto kind :
             {tgcli::core::TdChatKind::Supergroup, tgcli::core::TdChatKind::Channel}) {
            CAPTURE(kind);
            FakeM2Read fake;
            auto input = members_request(50);
            input.args["bots"] = false;
            auto invalid = chat_member(-2001);
            invalid.status = {.kind = tgcli::core::TdChatMemberStatusKind::Member,
                              .is_member = true,
                              .unsupported_tdlib_type_id = std::nullopt};
            auto pending = fake.dispatch(Command::Members, std::move(input));
            fake.respond_me();
            fake.respond_chat(kind, 77);
            fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                         tgcli::core::TdChatMembers{.total_count = 1, .members = {invalid}});
            const auto outcome = pending.get();
            CHECK_FALSE(outcome.result);
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
            CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 1);
        }
    }

    SECTION("chat sender hydration must resolve to supergroup or channel") {
        FakeM2Read fake;
        auto input = members_request(50);
        input.args["bots"] = false;
        auto pending = fake.dispatch(Command::Members, std::move(input));
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                     tgcli::core::TdChatMembers{.total_count = 1, .members = {chat_member(-2001)}});
        fake.respond(tgcli::core::TdFunctionKind::GetChat,
                     tgcli::core::TdChat{.id = -2001,
                                         .title = "Basic",
                                         .kind = tgcli::core::TdChatKind::BasicGroup,
                                         .related_id = 88,
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
        CHECK_FALSE(outcome.result);
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    }

    SECTION("supergroups retain restricted users") {
        FakeM2Read fake;
        auto input = members_request(50);
        input.args["bots"] = false;
        auto restricted = member(7);
        restricted.status = {.kind = tgcli::core::TdChatMemberStatusKind::Restricted,
                             .is_member = false,
                             .unsupported_tdlib_type_id = std::nullopt};
        auto pending = fake.dispatch(Command::Members, std::move(input));
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                     tgcli::core::TdChatMembers{.total_count = 1, .members = {restricted}});
        fake.respond_user(7, "Restricted", false);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        REQUIRE((*outcome.result)["items"].size() == 1);
        CHECK((*outcome.result)["items"][0]["status"] == "restricted");
    }
}

TEST_CASE("M2 rejects invalid continuation cursors before any TD call",
          "[m2-long-read][cursor][fake-boundary]") {
    SECTION("search marker relation") {
        FakeM2Read fake;
        const tgcli::daemon::SearchCursor cursor{.account = "main",
                                                 .user_id = 42,
                                                 .limit = 20,
                                                 .query = "needle",
                                                 .scope = tgcli::daemon::SearchScope::Chat,
                                                 .chat_id = -1001,
                                                 .sender_user_id = std::nullopt,
                                                 .type = tgcli::daemon::SearchType::Any,
                                                 .next_offset_message_id = 100,
                                                 .next_offset = std::nullopt,
                                                 .last_raw_message_id = 100,
                                                 .last_raw_order = std::nullopt};
        const auto outcome = fake.dispatch(Command::Search, search_cursor_request(cursor)).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }

    SECTION("members query bytes") {
        FakeM2Read fake;
        const tgcli::daemon::MembersCursor cursor{.account = "main",
                                                  .user_id = 42,
                                                  .limit = 50,
                                                  .chat_id = -1001,
                                                  .chat_type =
                                                      tgcli::daemon::MembersChatType::Supergroup,
                                                  .source_id = 77,
                                                  .filter = tgcli::daemon::MembersFilter::Query,
                                                  .query = std::string(257, 'q'),
                                                  .offset = 1,
                                                  .source_count = std::nullopt};
        const auto outcome = fake.dispatch(Command::Members, members_cursor_request(cursor)).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }
}

TEST_CASE("M2 accepts exact continuation cursor relation and query boundaries",
          "[m2-long-read][cursor][fake-boundary]") {
    SECTION("search marker is strictly below the last raw id") {
        FakeM2Read fake;
        const tgcli::daemon::SearchCursor cursor{.account = "main",
                                                 .user_id = 42,
                                                 .limit = 20,
                                                 .query = "needle",
                                                 .scope = tgcli::daemon::SearchScope::Chat,
                                                 .chat_id = -1001,
                                                 .sender_user_id = std::nullopt,
                                                 .type = tgcli::daemon::SearchType::Any,
                                                 .next_offset_message_id = 99,
                                                 .next_offset = std::nullopt,
                                                 .last_raw_message_id = 100,
                                                 .last_raw_order = std::nullopt};
        auto pending = fake.dispatch(Command::Search, search_cursor_request(cursor));
        fake.respond_me();
        fake.respond_chat();
        fake.respond(tgcli::core::TdFunctionKind::SearchChatMessages,
                     tgcli::core::TdFoundChatMessages{
                         .total_count = 0, .messages = {}, .next_from_message_id = 0});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["items"].empty());
        CHECK((*outcome.result)["next"].is_null());
    }

    SECTION("members search query has exactly 256 bytes") {
        FakeM2Read fake;
        const tgcli::daemon::MembersCursor cursor{.account = "main",
                                                  .user_id = 42,
                                                  .limit = 50,
                                                  .chat_id = -1001,
                                                  .chat_type =
                                                      tgcli::daemon::MembersChatType::Supergroup,
                                                  .source_id = 77,
                                                  .filter = tgcli::daemon::MembersFilter::Query,
                                                  .query = std::string(256, 'q'),
                                                  .offset = 1,
                                                  .source_count = std::nullopt};
        auto pending = fake.dispatch(Command::Members, members_cursor_request(cursor));
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Supergroup, 77);
        fake.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                     tgcli::core::TdChatMembers{.total_count = 0, .members = {}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["items"].empty());
        CHECK((*outcome.result)["next"].is_null());
    }
}

TEST_CASE("M2 members validate the complete structural page before identity hydration",
          "[m2-long-read][chat-members][integrity][fake-boundary]") {
    FakeM2Read fake;
    auto input = members_request(50);
    input.args["bots"] = false;
    auto malformed = member(8);
    malformed.status = {.kind = tgcli::core::TdChatMemberStatusKind::Unknown,
                        .is_member = false,
                        .unsupported_tdlib_type_id = 99};
    auto pending = fake.dispatch(Command::Members, std::move(input));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                 tgcli::core::TdBasicGroupFullInfo{.description = "room",
                                                   .creator_user_id = 42,
                                                   .members = {member(7), malformed}});
    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "malformed_tdlib_response");
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetUser) == 0);
}

TEST_CASE("M2 search rejects a nonadvancing marker without partial output",
          "[m2-long-read][search][fake-boundary]") {
    FakeM2Read fake;
    auto pending = fake.dispatch(Command::Search, search_request(2));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::SearchChatMessages,
                 tgcli::core::TdFoundChatMessages{
                     .total_count = 2, .messages = {message(100)}, .next_from_message_id = 100});
    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "PAGINATION_INVALID");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "marker_not_advancing");
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("search.error.schema.json"));
}

TEST_CASE("M2 search rejects a response that contradicts the native filter",
          "[m2-long-read][search][integrity][fake-boundary]") {
    FakeM2Read fake;
    auto input = search_request();
    input.args["type"] = "photo";
    auto pending = fake.dispatch(Command::Search, std::move(input));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::SearchChatMessages,
                 tgcli::core::TdFoundChatMessages{
                     .total_count = 1, .messages = {message(100)}, .next_from_message_id = 0});
    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "malformed_tdlib_response");
}

TEST_CASE("M2 global search rejects opaque marker cycles after atomic postfilter scans",
          "[m2-long-read][search][fake-boundary]") {
    FakeM2Read fake;
    auto pending = fake.dispatch(Command::Search, global_search_request());
    fake.respond_me();
    fake.respond(
        tgcli::core::TdFunctionKind::SearchMessages,
        tgcli::core::TdFoundMessages{.total_count = -1, .messages = {}, .next_offset = "offset-a"});
    fake.respond(tgcli::core::TdFunctionKind::SearchMessages,
                 tgcli::core::TdFoundMessages{
                     .total_count = 3,
                     .messages = {message(90, tgcli::core::TdMessageContentKind::Photo, -1001)},
                     .next_offset = "offset-b"});
    fake.respond(tgcli::core::TdFunctionKind::SearchMessages,
                 tgcli::core::TdFoundMessages{
                     .total_count = 3,
                     .messages = {message(80, tgcli::core::TdMessageContentKind::Photo, -1001)},
                     .next_offset = "offset-a"});
    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "PAGINATION_INVALID");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "marker_not_advancing");
    CHECK(fake.count(tgcli::core::TdFunctionKind::SearchMessages) == 3);
}

TEST_CASE("M2 search bounds raw scanned items without partial output",
          "[m2-long-read][search][capacity][fake-boundary]") {
    FakeM2Read fake;
    auto input = global_search_request(100);
    input.context.timeout_seconds = 10.0;
    auto pending = fake.dispatch(Command::Search, std::move(input));
    fake.respond_me();

    std::int64_t next_id = 10'000;
    for (std::size_t page_index = 0; page_index < 40; ++page_index) {
        std::vector<std::optional<tgcli::core::TdMessageSummary>> messages;
        messages.reserve(100);
        for (std::size_t item_index = 0; item_index < 100; ++item_index) {
            messages.emplace_back(
                message(next_id--, tgcli::core::TdMessageContentKind::Photo, -1001));
        }
        fake.respond(
            tgcli::core::TdFunctionKind::SearchMessages,
            tgcli::core::TdFoundMessages{.total_count = -1,
                                         .messages = std::move(messages),
                                         .next_offset = "raw-" + std::to_string(page_index)});
    }
    std::vector<std::optional<tgcli::core::TdMessageSummary>> boundary;
    boundary.reserve(96);
    for (std::size_t item_index = 0; item_index < 96; ++item_index) {
        boundary.emplace_back(message(next_id--, tgcli::core::TdMessageContentKind::Photo, -1001));
    }
    fake.respond(tgcli::core::TdFunctionKind::SearchMessages,
                 tgcli::core::TdFoundMessages{
                     .total_count = -1, .messages = std::move(boundary), .next_offset = "raw-40"});
    fake.respond(
        tgcli::core::TdFunctionKind::SearchMessages,
        tgcli::core::TdFoundMessages{
            .total_count = -1,
            .messages = {message(next_id, tgcli::core::TdMessageContentKind::Photo, -1001)},
            .next_offset = "raw-41"});

    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "RESOURCE_LIMIT");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"operation", "search"},
               {"resource", "raw_scanned_items"},
               {"limit", tgcli::daemon::kMaximumSearchRawScannedItems}});
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("search.error.schema.json"));
    CHECK(fake.count(tgcli::core::TdFunctionKind::SearchMessages) == 42);
}

TEST_CASE("M2 search bounds cumulative unique cursor marker bytes without partial output",
          "[m2-long-read][search][capacity][fake-boundary]") {
    FakeM2Read fake;
    auto input = global_search_request(100);
    input.context.timeout_seconds = 10.0;
    auto pending = fake.dispatch(Command::Search, std::move(input));
    fake.respond_me();

    const auto marker = [](std::size_t size, std::size_t index) {
        std::string value(size, 'a');
        const auto prefix = std::to_string(index) + "-";
        value.replace(0, prefix.size(), prefix);
        return value;
    };
    for (std::size_t index = 0; index < 29; ++index) {
        fake.respond(tgcli::core::TdFunctionKind::SearchMessages,
                     tgcli::core::TdFoundMessages{
                         .total_count = -1, .messages = {}, .next_offset = marker(34'996, index)});
    }
    fake.respond(tgcli::core::TdFunctionKind::SearchMessages,
                 tgcli::core::TdFoundMessages{
                     .total_count = -1, .messages = {}, .next_offset = marker(33'692, 29)});
    STATIC_REQUIRE(29 * 34'996 + 33'692 == tgcli::daemon::kMaximumSearchCursorMarkerBytes);
    fake.respond(
        tgcli::core::TdFunctionKind::SearchMessages,
        tgcli::core::TdFoundMessages{.total_count = -1, .messages = {}, .next_offset = "z"});

    const auto outcome = pending.get();
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "RESOURCE_LIMIT");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"operation", "search"},
               {"resource", "cursor_marker_bytes"},
               {"limit", tgcli::daemon::kMaximumSearchCursorMarkerBytes}});
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("search.error.schema.json"));
    CHECK(fake.count(tgcli::core::TdFunctionKind::SearchMessages) == 31);
}

TEST_CASE("M2 search closes bot and post-resolution authorization-loss boundaries",
          "[m2-long-read][search][lifecycle][fake-boundary]") {
    SECTION("bot") {
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Search, global_search_request());
        fake.respond_me(true);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "search"}});
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("search.error.schema.json"));
        CHECK(fake.count(tgcli::core::TdFunctionKind::SearchMessages) == 0);
    }
    SECTION("authorization loss") {
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Search, search_request());
        fake.respond_me();
        fake.respond_chat();
        fake.wait_for_next(tgcli::core::TdFunctionKind::SearchChatMessages);
        fake.push_auth(tgcli::core::AuthState::LoggingOut);
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK((*outcome.error)["error"]["details"]["state"] == "logging_out");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "authorization_lost");
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("search.error.schema.json"));
        CHECK(outcome.terminal_count == 1);
    }
}

TEST_CASE("M2 long-read capability matrix rejects secret/private work and allows bot reads",
          "[m2-long-read][capability][fake-boundary]") {
    SECTION("secret search") {
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Search, search_request());
        fake.respond_me();
        fake.respond_chat(tgcli::core::TdChatKind::Secret, 7);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.count(tgcli::core::TdFunctionKind::SearchChatMessages) == 0);
    }
    for (const auto kind : {tgcli::core::TdChatKind::Private, tgcli::core::TdChatKind::Secret}) {
        CAPTURE(kind);
        FakeM2Read fake;
        auto input = members_request();
        input.args["bots"] = false;
        auto pending = fake.dispatch(Command::Members, std::move(input));
        fake.respond_me();
        fake.respond_chat(kind, 7);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo) == 0);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 0);
    }
    SECTION("bot chat info") {
        FakeM2Read fake;
        auto pending = fake.dispatch(Command::Info, info_request());
        fake.respond_me(true);
        fake.respond_chat();
        fake.respond(
            tgcli::core::TdFunctionKind::GetBasicGroup,
            tgcli::core::TdBasicGroup{
                .id = 77, .member_count = 0, .is_active = true, .upgraded_to_supergroup_id = 0});
        fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                     tgcli::core::TdBasicGroupFullInfo{
                         .description = "room", .creator_user_id = 42, .members = {}});
        CHECK(pending.get().result);
    }
    SECTION("bot chat members") {
        FakeM2Read fake;
        auto input = members_request();
        input.args["bots"] = false;
        auto pending = fake.dispatch(Command::Members, std::move(input));
        fake.respond_me(true);
        fake.respond_chat();
        fake.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                     tgcli::core::TdBasicGroupFullInfo{
                         .description = "room", .creator_user_id = 42, .members = {}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["items"].empty());
    }
}
