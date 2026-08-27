#include "common/exit_codes.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/local_selector.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
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

class FakeResolver {
  public:
    explicit FakeResolver(tgcli::core::AuthState state = tgcli::core::AuthState::Ready) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        REQUIRE(runtime_->clients().size() == 1);
        td_client_ = runtime_->clients().front();
        runtime_->push_response(td_client_, 1, {}, tgcli::core::AuthStateData{state});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ =
            std::make_unique<tgcli::daemon::ResolveCoordinator>(*client_, std::string("main"));
        tgcli::daemon::register_resolve_command(dispatcher_, *coordinator_);
    }

    std::future<Outcome>
    dispatch(std::string selector,
             std::optional<tgcli::daemon::RequestSession::Clock::time_point> deadline = {},
             tgcli::daemon::ResolverScope scope = tgcli::daemon::ResolverScope::ActiveDialogs) {
        return std::async(
            std::launch::async, [this, selector = std::move(selector), deadline, scope]() mutable {
                Outcome outcome;
                tgcli::daemon::CallbackSink sink(
                    [](const json&) {}, [](const json&) {},
                    [&](json value) {
                        ++outcome.terminal_count;
                        outcome.result = std::move(value);
                    },
                    [&](std::string code, std::string message, json details, int exit_code) {
                        ++outcome.terminal_count;
                        outcome.error = json{{"error",
                                              {{"code", std::move(code)},
                                               {"message", std::move(message)},
                                               {"details", std::move(details)}}}};
                        outcome.exit_code = exit_code;
                    });
                tgcli::proto::Request request("main");
                request.id = 1;
                request.command = {"resolve"};
                request.args = {{"selector", selector}};
                request.context.timeout_seconds = 1.0;
                request.context.cwd = "/";
                tgcli::daemon::RequestSession session(
                    std::move(request), sink, 0, tgcli::daemon::RequestSession::NonceGenerator{},
                    tgcli::daemon::ActivityTracker::Token{}, {}, deadline);
                if (scope == tgcli::daemon::ResolverScope::ActiveDialogs) {
                    dispatcher_.dispatch(session);
                } else {
                    coordinator_->resolve_for_scope(std::move(selector), scope, session);
                }
                if (outcome.result) {
                    outcome.exit_code = tgcli::kOk;
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
        runtime_->push_response(td_client_, sent.back().query_id,
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

    void auth_update(tgcli::core::AuthState state) {
        runtime_->push_update(td_client_, {}, tgcli::core::AuthStateData{state});
    }

    template <typename T>
    void lose_authorization_before_response(tgcli::core::TdFunctionKind expected, T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        auth_update(tgcli::core::AuthState::WaitCode);
        runtime_->push_response(td_client_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    [[nodiscard]] std::size_t count(tgcli::core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

    [[nodiscard]] std::size_t forbidden_local_read_count() const {
        return std::ranges::count_if(runtime_->sent_functions(), [](const auto& sent) {
            const auto kind = sent.function.kind();
            if (!kind) {
                return false;
            }
            switch (*kind) {
            case tgcli::core::TdFunctionKind::LoadChats:
            case tgcli::core::TdFunctionKind::SearchPublicChat:
            case tgcli::core::TdFunctionKind::GetInternalLinkType:
            case tgcli::core::TdFunctionKind::GetMessageLinkInfo:
            case tgcli::core::TdFunctionKind::CheckChatInviteLink:
            case tgcli::core::TdFunctionKind::GetSupergroupFullInfo:
                return true;
            default:
                return false;
            }
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
    tgcli::test::ScriptedClient td_client_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::ResolveCoordinator> coordinator_;
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

tgcli::core::TdChat basic_chat(std::int64_t id, std::string title) {
    return {.id = id,
            .title = std::move(title),
            .kind = tgcli::core::TdChatKind::BasicGroup,
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
            .permissions = std::nullopt,
            .notification_settings = std::nullopt};
}

tgcli::core::TdChat private_chat(std::int64_t id, std::int64_t user_id, std::string title) {
    return {.id = id,
            .title = std::move(title),
            .kind = tgcli::core::TdChatKind::Private,
            .related_id = user_id,
            .tdlib_type_id = 2,
            .positions = {},
            .chat_lists = {},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .permissions = std::nullopt,
            .notification_settings = std::nullopt};
}

tgcli::core::TdChat channel_chat(std::int64_t id, std::int64_t supergroup_id, std::string title) {
    return {.id = id,
            .title = std::move(title),
            .kind = tgcli::core::TdChatKind::Channel,
            .related_id = supergroup_id,
            .tdlib_type_id = 3,
            .positions = {},
            .chat_lists = {},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .permissions = std::nullopt,
            .notification_settings = std::nullopt};
}

void finish_loaded_lists(FakeResolver& fake, const std::vector<std::int64_t>& main,
                         const std::vector<std::int64_t>& archive = {}) {
    auto main_get =
        fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = main});
    CHECK(field_as<std::string>(main_get, "list") == "main");
    CHECK(field_as<std::int64_t>(main_get, "limit") == 100);
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"});
    auto archive_get = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                                    tgcli::core::TdChats{.chat_ids = archive});
    CHECK(field_as<std::string>(archive_get, "list") == "archive");
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"});
}

void finish_local_lists(FakeResolver& fake, const std::vector<std::int64_t>& main,
                        const std::vector<std::int64_t>& archive = {}) {
    fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = main});
    fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = archive});
}

} // namespace

TEST_CASE("resolve selector validation is strict UTF-8 and int53", "[resolver][selector]") {
    CHECK(tgcli::daemon::valid_resolve_selector("-100123"));
    CHECK(tgcli::daemon::valid_resolve_selector("+42"));
    CHECK(tgcli::daemon::valid_resolve_selector("@example"));
    CHECK(tgcli::daemon::valid_resolve_selector("https://t.me/example/5"));
    CHECK(tgcli::daemon::valid_resolve_selector("mid-word title"));
    CHECK_FALSE(tgcli::daemon::valid_resolve_selector(""));
    CHECK_FALSE(tgcli::daemon::valid_resolve_selector("0"));
    CHECK_FALSE(tgcli::daemon::valid_resolve_selector("9007199254740992"));
    CHECK_FALSE(tgcli::daemon::valid_resolve_selector("@"));
    CHECK_FALSE(tgcli::daemon::valid_resolve_selector(std::string("\xED\xA0\x80", 3)));
}

TEST_CASE("local selector classifier implements the complete closed ASCII link table",
          "[resolver][selector][local]") {
    using tgcli::daemon::LocalSelectorKind;
    struct Case {
        std::string selector;
        LocalSelectorKind kind;
        std::string value;
    };
    const std::vector<Case> recognized{
        {"-1001", LocalSelectorKind::Numeric, "-1001"},
        {"@cached", LocalSelectorKind::Username, "cached"},
        {"t.me/project", LocalSelectorKind::PublicChatLink, "project"},
        {"https://t.me/helper?start=opaque_-", LocalSelectorKind::BotStartLink, "helper"},
        {"t.me/project/5", LocalSelectorKind::MessageLink, ""},
        {"https://t.me/c/7/9", LocalSelectorKind::MessageLink, ""},
        {"t.me/+invite_-", LocalSelectorKind::ChatInviteLink, ""},
        {"https://t.me/joinchat/invite_-", LocalSelectorKind::ChatInviteLink, ""},
        {"t.me/project?direct", LocalSelectorKind::DirectMessagesChatLink, "project"},
        {"Development", LocalSelectorKind::Title, "Development"},
        {"example.com/x", LocalSelectorKind::Title, "example.com/x"},
        {"notes/http", LocalSelectorKind::Title, "notes/http"},
    };
    for (const auto& test_case : recognized) {
        DYNAMIC_SECTION(test_case.selector) {
            const auto classified = tgcli::daemon::classify_local_selector(test_case.selector);
            REQUIRE(classified);
            CHECK(classified->kind == test_case.kind);
            CHECK(classified->value == test_case.value);
        }
    }

    const std::vector<std::string> malformed{
        "t.me/",
        "https://t.me/project/",
        "t.me/project//5",
        "t.me/project%20name",
        "t.me/project#fragment",
        "t.me/prøject",
        "t.me/_project",
        "t.me/project_",
        "t.me/project__room",
        "t.me/abcdefghijklmnopqrstuvwxyzabcdefg",
        "t.me/project/0",
        "t.me/project/01",
        "t.me/_project/1",
        "t.me/c/0/1",
        "t.me/c/x/y",
        "t.me/+",
        "t.me/joinchat/+",
        "t.me/project?start",
        "t.me/project?starter=x",
        "t.me/project?start=x&other=y",
        "t.me/project?start=x?other=y",
        "http://t.me/project",
        "HTTPS://t.me/project",
        "https://T.me/project",
        "https://www.t.me/project",
        "https://user@t.me/project",
        "https://t.me:443/project",
        "https://example.com/project",
        "T.ME/project",
        "www.t.me/project",
        "t.me:443/project",
    };
    for (const auto& selector : malformed) {
        DYNAMIC_SECTION(selector) {
            const auto classified = tgcli::daemon::classify_local_selector(selector);
            REQUIRE(classified);
            CHECK(classified->kind == LocalSelectorKind::InvalidLink);
        }
    }

    for (const auto& selector :
         {"t.me/project/settings", "t.me/project?other=value", "t.me/project?direct&other=value"}) {
        DYNAMIC_SECTION(selector) {
            const auto classified = tgcli::daemon::classify_local_selector(selector);
            REQUIRE(classified);
            CHECK(classified->kind == LocalSelectorKind::UnsupportedLink);
        }
    }
}

TEST_CASE("numeric resolve materializes an exact chat identity",
          "[resolver][schema][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("-1001");
    fake.respond_me();
    const auto get_chat =
        fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-1001, "Build room"));
    CHECK(field_as<std::int64_t>(get_chat, "chat_id") == -1001);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(*outcome.result == json{{"kind", "chat"},
                                  {"chat",
                                   {{"id", -1001},
                                    {"title", "Build room"},
                                    {"type", "basic_group"},
                                    {"is_bot", false},
                                    {"usernames", json::array()}}},
                                  {"message_id", nullptr},
                                  {"topic", nullptr},
                                  {"link_type", nullptr},
                                  {"is_public", nullptr}});
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("resolve.result.schema.json"));
    CHECK(outcome.exit_code == tgcli::kOk);
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("username resolve normalizes exact TD errors", "[resolver][error][fake-boundary]") {
    struct Case {
        std::int32_t code;
        std::string message;
        std::string expected;
        int exit_code;
    };
    const std::vector<Case> cases{{400, "USERNAME_NOT_OCCUPIED", "NOT_FOUND", tgcli::kNotFound},
                                  {400, "USERNAME_INVALID", "NOT_FOUND", tgcli::kNotFound},
                                  {400, "CHAT_ADMIN_REQUIRED", "TDLIB_ERROR", tgcli::kGeneric},
                                  {429, "FLOOD_WAIT_17", "RATE_LIMITED", tgcli::kRateLimited}};
    for (const auto& test_case : cases) {
        DYNAMIC_SECTION(test_case.message) {
            FakeResolver fake;
            auto pending = fake.dispatch("@missing");
            fake.respond_me();
            const auto search =
                fake.respond(tgcli::core::TdFunctionKind::SearchPublicChat,
                             tgcli::core::TdError{test_case.code, test_case.message});
            CHECK(field_as<std::string>(search, "username") == "missing");
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == test_case.expected);
            CHECK(outcome.exit_code == test_case.exit_code);
            CHECK_THAT(*outcome.error,
                       tgcli::test::matches_json_schema("resolve.error.schema.json"));
        }
    }
}

TEST_CASE("username resolve enriches channel usernames", "[resolver][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("@project");
    fake.respond_me();
    fake.respond(tgcli::core::TdFunctionKind::SearchPublicChat, channel_chat(-1005, 55, "Project"));
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroup,
                 tgcli::core::TdSupergroup{
                     .id = 55, .usernames = {"project", "project_news"}, .is_channel = true});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["chat"]["type"] == "channel");
    CHECK((*outcome.result)["chat"]["usernames"] == json::array({"project", "project_news"}));
}

TEST_CASE("title resolution loads Main and Archive through explicit 404 EOF",
          "[resolver][dialogs][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("needle");
    fake.respond_me();
    fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {-1}});
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdOk{});
    auto grown = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                              tgcli::core::TdChats{.chat_ids = {-1, -2}});
    CHECK(field_as<std::int64_t>(grown, "limit") == 200);
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"});
    fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {-3}});
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"});
    fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-1, "ordinary"));
    fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-2, "mid-needle-word"));
    fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-3, "archive"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["chat"]["id"] == -2);
    CHECK(fake.count(tgcli::core::TdFunctionKind::LoadChats) == 3);
}

TEST_CASE("title ambiguity retains stable order and truncates after twenty",
          "[resolver][dialogs][error][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("match");
    fake.respond_me();
    std::vector<std::int64_t> ids;
    for (std::int64_t id = 1; id <= 21; ++id) {
        ids.push_back(-id);
    }
    finish_loaded_lists(fake, ids);
    for (const auto id : ids) {
        fake.respond(tgcli::core::TdFunctionKind::GetChat,
                     basic_chat(id, "match " + std::to_string(-id)));
    }
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "AMBIGUOUS");
    const auto& details = (*outcome.error)["error"]["details"];
    CHECK(details["candidates"].size() == 20);
    CHECK(details["candidates"][0]["id"] == -1);
    CHECK(details["candidates"][19]["id"] == -20);
    CHECK(details["truncated"] == true);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("resolve.error.schema.json"));
}

TEST_CASE("local username resolution uses only materialized dialog reads",
          "[resolver][local][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("@cached", {}, tgcli::daemon::ResolverScope::LocalMaterialized);
    fake.respond_me();
    finish_local_lists(fake, {-10});
    fake.respond(tgcli::core::TdFunctionKind::GetChat, private_chat(-10, 10, "Cached"));
    fake.respond(tgcli::core::TdFunctionKind::GetUser,
                 tgcli::core::TdUserSummary{.id = 10,
                                            .first_name = "Cached",
                                            .last_name = "",
                                            .usernames = {"cached"},
                                            .phone_number = "",
                                            .is_bot = false,
                                            .is_premium = false});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["chat"]["usernames"] == json::array({"cached"}));
    CHECK(fake.count(tgcli::core::TdFunctionKind::SearchPublicChat) == 0);
    CHECK(fake.count(tgcli::core::TdFunctionKind::LoadChats) == 0);
}

TEST_CASE("local link table never delegates classification to TDLib",
          "[resolver][local][link][fake-boundary]") {
    SECTION("PublicChat and BotStart hits retain link type") {
        struct Case {
            std::string selector;
            std::string link_type;
        };
        for (const auto& test_case : {Case{"t.me/helper", "public_chat"},
                                      Case{"https://t.me/helper?start=opaque_-", "bot_start"}}) {
            DYNAMIC_SECTION(test_case.selector) {
                FakeResolver fake;
                auto pending = fake.dispatch(test_case.selector, {},
                                             tgcli::daemon::ResolverScope::LocalMaterialized);
                fake.respond_me();
                finish_local_lists(fake, {-10});
                fake.respond(tgcli::core::TdFunctionKind::GetChat, private_chat(-10, 10, "Helper"));
                fake.respond(tgcli::core::TdFunctionKind::GetUser,
                             tgcli::core::TdUserSummary{.id = 10,
                                                        .first_name = "Helper",
                                                        .last_name = "",
                                                        .usernames = {"helper"},
                                                        .phone_number = "",
                                                        .is_bot = true,
                                                        .is_premium = false});
                const auto outcome = pending.get();
                REQUIRE(outcome.result);
                CHECK((*outcome.result)["chat"]["id"] == -10);
                CHECK((*outcome.result)["link_type"] == test_case.link_type);
                CHECK(fake.forbidden_local_read_count() == 0);
            }
        }
    }

    SECTION("PublicChat and BotStart misses remain local") {
        for (const auto& selector : {"t.me/missing", "t.me/missing?start="}) {
            DYNAMIC_SECTION(selector) {
                FakeResolver fake;
                auto pending =
                    fake.dispatch(selector, {}, tgcli::daemon::ResolverScope::LocalMaterialized);
                fake.respond_me();
                finish_local_lists(fake, {});
                const auto outcome = pending.get();
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
                CHECK((*outcome.error)["error"]["details"]["scope"] == "local_materialized");
                CHECK(fake.forbidden_local_read_count() == 0);
            }
        }
    }

    SECTION("BotStart requires an active private bot") {
        FakeResolver fake;
        auto pending = fake.dispatch("t.me/helper?start=", {},
                                     tgcli::daemon::ResolverScope::LocalMaterialized);
        fake.respond_me();
        finish_local_lists(fake, {-10});
        fake.respond(tgcli::core::TdFunctionKind::GetChat, private_chat(-10, 10, "Helper"));
        fake.respond(tgcli::core::TdFunctionKind::GetUser,
                     tgcli::core::TdUserSummary{.id = 10,
                                                .first_name = "Helper",
                                                .last_name = "",
                                                .usernames = {"helper"},
                                                .phone_number = "12025550123",
                                                .is_bot = false,
                                                .is_premium = false});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
        CHECK(fake.forbidden_local_read_count() == 0);
    }

    SECTION("Message Invite and Direct forms are immediate local misses") {
        for (const auto& selector : {"t.me/project/5", "https://t.me/c/7/9", "t.me/+invite_-",
                                     "https://t.me/joinchat/invite_-", "t.me/project?direct"}) {
            DYNAMIC_SECTION(selector) {
                FakeResolver fake;
                auto pending =
                    fake.dispatch(selector, {}, tgcli::daemon::ResolverScope::LocalMaterialized);
                fake.respond_me();
                const auto outcome = pending.get();
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
                CHECK((*outcome.error)["error"]["details"]["scope"] == "local_materialized");
                CHECK(fake.forbidden_local_read_count() == 0);
                CHECK(fake.sent_count() == 2);
            }
        }
    }

    SECTION("malformed and unsupported links fail before Ready") {
        struct Case {
            std::string selector;
            std::string reason;
        };
        for (const auto& test_case : {Case{"t.me/project/", "invalid_argument"},
                                      Case{"HTTPS://t.me/project", "invalid_argument"},
                                      Case{"t.me/project/settings", "unsupported_link_type"}}) {
            DYNAMIC_SECTION(test_case.selector) {
                FakeResolver fake;
                auto pending = fake.dispatch(test_case.selector, {},
                                             tgcli::daemon::ResolverScope::LocalMaterialized);
                REQUIRE(pending.wait_for(2s) == std::future_status::ready);
                const auto outcome = pending.get();
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "USAGE");
                CHECK((*outcome.error)["error"]["details"]["reason"] == test_case.reason);
                CHECK(fake.sent_count() == 1);
            }
        }
    }

    SECTION("non-URL title fallback remains local") {
        FakeResolver fake;
        auto pending =
            fake.dispatch("Development", {}, tgcli::daemon::ResolverScope::LocalMaterialized);
        fake.respond_me();
        finish_local_lists(fake, {-20});
        fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-20, "Product Development"));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["chat"]["id"] == -20);
        CHECK((*outcome.result)["link_type"] == nullptr);
        CHECK(fake.forbidden_local_read_count() == 0);
    }
}

TEST_CASE("message links preserve message precedence and topic metadata",
          "[resolver][link][schema][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("https://t.me/project/123");
    fake.respond_me(true);
    fake.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                 tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::Message,
                                             .username = {},
                                             .url = "https://t.me/project/123",
                                             .tdlib_type_id = 1});
    fake.respond(tgcli::core::TdFunctionKind::GetMessageLinkInfo,
                 tgcli::core::TdMessageLinkInfo{
                     .is_public = true,
                     .chat_id = -1001,
                     .message_id = 123,
                     .topic = tgcli::core::TdTopic{
                         .kind = tgcli::core::TdTopicKind::Forum, .id = 7, .tdlib_type_id = 2}});
    fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-1001, "Project"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["kind"] == "message");
    CHECK((*outcome.result)["message_id"] == 123);
    CHECK((*outcome.result)["topic"] == json{{"kind", "forum"}, {"id", 7}});
    CHECK((*outcome.result)["link_type"] == "message");
    CHECK((*outcome.result)["is_public"] == true);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("resolve.result.schema.json"));
}

TEST_CASE("bot invite rejection occurs after classification without checking the invite",
          "[resolver][bot][link][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("t.me/+invite");
    fake.respond_me(true);
    fake.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                 tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::ChatInvite,
                                             .username = {},
                                             .url = "t.me/+invite",
                                             .tdlib_type_id = 1});
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
    CHECK(fake.count(tgcli::core::TdFunctionKind::CheckChatInviteLink) == 0);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("resolve.error.schema.json"));
}

TEST_CASE("user invite resolution checks without joining", "[resolver][link][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("t.me/+invite");
    fake.respond_me();
    fake.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                 tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::ChatInvite,
                                             .username = {},
                                             .url = "t.me/+invite",
                                             .tdlib_type_id = 1});
    fake.respond(tgcli::core::TdFunctionKind::CheckChatInviteLink,
                 tgcli::core::TdChatInviteLinkInfo{.chat_id = -7, .is_public = false});
    fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-7, "Invite"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["link_type"] == "chat_invite");
    CHECK((*outcome.result)["is_public"] == false);
}

TEST_CASE("Saved Messages link materializes the current user without force",
          "[resolver][link][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("t.me/saved");
    fake.respond_me();
    fake.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                 tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::SavedMessages,
                                             .username = {},
                                             .url = "t.me/saved",
                                             .tdlib_type_id = 1});
    const auto create = fake.respond(tgcli::core::TdFunctionKind::CreatePrivateChat,
                                     private_chat(42, 42, "Saved Messages"));
    CHECK(field_as<std::int64_t>(create, "user_id") == 42);
    CHECK_FALSE(field_as<bool>(create, "force"));
    fake.respond(tgcli::core::TdFunctionKind::GetUser,
                 tgcli::core::TdUserSummary{.id = 42,
                                            .first_name = "Ada",
                                            .last_name = "",
                                            .usernames = {"ada"},
                                            .phone_number = "12025550123",
                                            .is_bot = false,
                                            .is_premium = false});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["link_type"] == "saved_messages");
}

TEST_CASE("bot-start links resolve the bot without executing the parameter",
          "[resolver][link][bot][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("t.me/helper?start=opaque");
    fake.respond_me(true);
    fake.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                 tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::BotStart,
                                             .username = "helper",
                                             .url = {},
                                             .tdlib_type_id = 1});
    fake.respond(tgcli::core::TdFunctionKind::SearchPublicChat, private_chat(50, 50, "Helper"));
    fake.respond(tgcli::core::TdFunctionKind::GetUser,
                 tgcli::core::TdUserSummary{.id = 50,
                                            .first_name = "Helper",
                                            .last_name = "",
                                            .usernames = {"helper"},
                                            .phone_number = "",
                                            .is_bot = true,
                                            .is_premium = false});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["chat"]["is_bot"] == true);
    CHECK((*outcome.result)["link_type"] == "bot_start");
}

TEST_CASE("direct-message links return the final direct chat id",
          "[resolver][link][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("t.me/project?direct");
    fake.respond_me();
    fake.respond(
        tgcli::core::TdFunctionKind::GetInternalLinkType,
        tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::DirectMessagesChat,
                                    .username = "project",
                                    .url = {},
                                    .tdlib_type_id = 1});
    fake.respond(tgcli::core::TdFunctionKind::SearchPublicChat, channel_chat(-1001, 55, "Project"));
    fake.respond(tgcli::core::TdFunctionKind::GetSupergroupFullInfo,
                 tgcli::core::TdSupergroupFullInfo{.direct_messages_chat_id = -2002});
    fake.respond(tgcli::core::TdFunctionKind::GetChat, basic_chat(-2002, "Project discussions"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["chat"]["id"] == -2002);
    CHECK((*outcome.result)["link_type"] == "direct_messages_chat");
}

TEST_CASE("resolver rejects closed link and chat classes exactly",
          "[resolver][link][error][fake-boundary]") {
    SECTION("unsupported internal link type is usage") {
        FakeResolver fake;
        auto pending = fake.dispatch("t.me/calls");
        fake.respond_me();
        fake.respond(
            tgcli::core::TdFunctionKind::GetInternalLinkType,
            tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::Unsupported,
                                        .username = {},
                                        .url = {},
                                        .tdlib_type_id = -718405184});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "unsupported_link_type");
    }

    SECTION("secret numeric target is usage") {
        FakeResolver fake;
        auto pending = fake.dispatch("-9");
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChat,
                     tgcli::core::TdChat{.id = -9,
                                         .title = "Secret",
                                         .kind = tgcli::core::TdChatKind::Secret,
                                         .related_id = 10,
                                         .tdlib_type_id = 4,
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
        CHECK((*outcome.error)["error"]["details"]["reason"] == "unsupported_chat_type");
    }
}

TEST_CASE("bot direct-message rejection precedes public and full-info lookups",
          "[resolver][bot][link][fake-boundary]") {
    FakeResolver fake;
    auto pending = fake.dispatch("t.me/project?direct");
    fake.respond_me(true);
    fake.respond(
        tgcli::core::TdFunctionKind::GetInternalLinkType,
        tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::DirectMessagesChat,
                                    .username = "project",
                                    .url = {},
                                    .tdlib_type_id = 1});
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
    CHECK(fake.count(tgcli::core::TdFunctionKind::SearchPublicChat) == 0);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSupergroupFullInfo) == 0);
}

TEST_CASE("bot title and Saved Messages branches stop before user-only reads",
          "[resolver][bot][fake-boundary]") {
    SECTION("title") {
        FakeResolver fake;
        auto pending = fake.dispatch("Project");
        fake.respond_me(true);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChats) == 0);
    }

    SECTION("Saved Messages") {
        FakeResolver fake;
        auto pending = fake.dispatch("t.me/saved");
        fake.respond_me(true);
        fake.respond(
            tgcli::core::TdFunctionKind::GetInternalLinkType,
            tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::SavedMessages,
                                        .username = {},
                                        .url = "t.me/saved",
                                        .tdlib_type_id = 1});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK(fake.count(tgcli::core::TdFunctionKind::CreatePrivateChat) == 0);
    }
}

TEST_CASE("resolver auth loss ordering and absolute deadline are terminal",
          "[resolver][authorization][ordering][timeout][fake-boundary]") {
    SECTION("authorization loss before the TD response wins") {
        FakeResolver fake;
        auto pending = fake.dispatch("-1");
        fake.respond_me();
        fake.lose_authorization_before_response(tgcli::core::TdFunctionKind::GetChat,
                                                basic_chat(-1, "late"));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(outcome.terminal_count == 1);
    }

    SECTION("pre-expired request sends no resolver read") {
        FakeResolver fake;
        const auto expired = tgcli::daemon::RequestSession::Clock::now() - 1ms;
        const auto outcome = fake.dispatch("-1", expired).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK(fake.sent_count() == 1);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("resolve.error.schema.json"));
    }
}
