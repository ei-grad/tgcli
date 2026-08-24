#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
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

class UserResolverHarness {
  public:
    UserResolverHarness() {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        REQUIRE(runtime_->clients().size() == 1);
        td_client_ = runtime_->clients().front();
        runtime_->push_response(td_client_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
    }

    std::future<tgcli::daemon::UserResolverOutcome>
    resolve(std::string selector, std::optional<tgcli::core::TdChat> domain = std::nullopt,
            std::optional<tgcli::daemon::RequestSession::Clock::time_point> deadline = {}) {
        return std::async(std::launch::async, [this, selector = std::move(selector),
                                               domain = std::move(domain), deadline]() mutable {
            tgcli::daemon::CallbackSink sink(
                [](const json&) {}, [](const json&) {}, [](const json&) {},
                [](const std::string&, const std::string&, const json&, int) {});
            tgcli::proto::Request request("main");
            request.id = 1;
            request.command = {"wait-for"};
            request.context.timeout_seconds = 1.0;
            request.context.cwd = "/";
            tgcli::daemon::RequestSession session(
                std::move(request), sink, 0, tgcli::daemon::RequestSession::NonceGenerator{},
                tgcli::daemon::ActivityTracker::Token{}, {}, deadline);
            tgcli::daemon::ResolverConsumer consumer(*client_, "main", session);
            auto principal = consumer.bind_principal(tgcli::daemon::M2Operation::Resolve);
            if (const auto* error = std::get_if<tgcli::daemon::ResolverError>(&principal)) {
                return tgcli::daemon::UserResolverOutcome{*error};
            }
            if (const auto* stopped = std::get_if<tgcli::daemon::ResolverStop>(&principal)) {
                return tgcli::daemon::UserResolverOutcome{*stopped};
            }
            return consumer.resolve_user(std::move(selector), domain);
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
        respond(
            tgcli::core::TdFunctionKind::GetMe,
            user(42, bot ? "Resolver" : "Ada", bot ? "Bot" : "Principal", bot,
                 bot ? std::vector<std::string>{"resolver_bot"} : std::vector<std::string>{"ada"}));
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

    static tgcli::core::TdUserSummary user(std::int64_t id, std::string first,
                                           std::string last = {}, bool bot = false,
                                           std::vector<std::string> usernames = {}) {
        return {.id = id,
                .first_name = std::move(first),
                .last_name = std::move(last),
                .usernames = std::move(usernames),
                .phone_number = {},
                .is_bot = bot,
                .is_premium = false,
                .presence = tgcli::core::TdUserPresence::Hidden};
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

tgcli::core::TdChat domain_chat(tgcli::core::TdChatKind kind, std::int64_t related_id) {
    return {.id = kind == tgcli::core::TdChatKind::BasicGroup ? -51 : -10055,
            .title = "Domain",
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
            .notification_settings = std::nullopt};
}

tgcli::core::TdChat private_chat(std::int64_t user_id, std::string title = "User") {
    return {.id = user_id,
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
            .notification_settings = std::nullopt};
}

tgcli::core::TdChatMember member(std::int64_t user_id) {
    return {.member = {.kind = tgcli::core::TdMessageSenderKind::User, .id = user_id},
            .tag = {},
            .inviter_user_id = 42,
            .joined_chat_date = 1,
            .status = {.kind = tgcli::core::TdChatMemberStatusKind::Member,
                       .is_member = true,
                       .unsupported_tdlib_type_id = std::nullopt}};
}

const tgcli::daemon::UserIdentity& resolved(const tgcli::daemon::UserResolverOutcome& outcome) {
    const auto* identity = std::get_if<tgcli::daemon::UserIdentity>(&outcome);
    REQUIRE(identity != nullptr);
    return *identity;
}

const tgcli::daemon::ResolverError& failed(const tgcli::daemon::UserResolverOutcome& outcome) {
    const auto* error = std::get_if<tgcli::daemon::ResolverError>(&outcome);
    REQUIRE(error != nullptr);
    return *error;
}

json terminal(const tgcli::daemon::UserResolverOutcome& outcome) {
    return tgcli::daemon::resolver_error_terminal(failed(outcome));
}

} // namespace

TEST_CASE("user resolver accepts positive numeric ids without enumerating a domain",
          "[resolver][user-resolver][numeric][fake-boundary]") {
    UserResolverHarness harness;
    auto pending = harness.resolve("77", domain_chat(tgcli::core::TdChatKind::Supergroup, 55));
    harness.respond_me(true);
    const auto get_user =
        harness.respond(tgcli::core::TdFunctionKind::GetUser,
                        UserResolverHarness::user(77, "Target", "User", true, {"target_bot"}));
    CHECK(field_as<std::int64_t>(get_user, "user_id") == 77);
    const auto outcome = pending.get();
    const auto& identity = resolved(outcome);
    CHECK(identity == tgcli::daemon::UserIdentity{.id = 77,
                                                  .display_name = "Target User",
                                                  .usernames = {"target_bot"},
                                                  .is_bot = true});
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetContacts) == 0);
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 0);
}

TEST_CASE("user resolver accepts exact usernames and public profile links for bots",
          "[resolver][user-resolver][public][bot][fake-boundary]") {
    for (const auto& selector :
         {std::string{"@Helper"}, std::string{"t.me/helper"}, std::string{"https://t.me/helper"}}) {
        DYNAMIC_SECTION(selector) {
            UserResolverHarness harness;
            auto pending = harness.resolve(selector);
            harness.respond_me(true);
            const auto search = harness.respond(tgcli::core::TdFunctionKind::SearchPublicChat,
                                                private_chat(88, "Helper"));
            CHECK(field_as<std::string>(search, "username") ==
                  (selector == "@Helper" ? "Helper" : "helper"));
            harness.respond(tgcli::core::TdFunctionKind::GetUser,
                            UserResolverHarness::user(88, "Helper", {}, true, {"helper"}));
            const auto outcome = pending.get();
            CHECK(resolved(outcome).id == 88);
            CHECK(resolved(outcome).is_bot);
            CHECK(harness.count(tgcli::core::TdFunctionKind::GetContacts) == 0);
            CHECK(harness.count(tgcli::core::TdFunctionKind::GetInternalLinkType) == 0);
        }
    }
}

TEST_CASE("global user substring scans the complete contacts domain case-insensitively",
          "[resolver][user-resolver][contacts][fake-boundary]") {
    UserResolverHarness harness;
    auto pending = harness.resolve("bOb buI");
    harness.respond_me();
    harness.respond(tgcli::core::TdFunctionKind::GetContacts,
                    tgcli::core::TdUsers{.total_count = 3, .user_ids = {10, 11, 12}});
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(10, "Alice", "Stone"));
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(11, "BOB", "Builder", false, {"bob"}));
    harness.respond(tgcli::core::TdFunctionKind::GetUser, UserResolverHarness::user(12, "Carol"));
    const auto outcome = pending.get();
    const auto& identity = resolved(outcome);
    CHECK(identity.id == 11);
    CHECK(identity.display_name == "BOB Builder");
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 3);
}

TEST_CASE("basic-group user substring consumes the full member vector",
          "[resolver][user-resolver][basic-group][fake-boundary]") {
    UserResolverHarness harness;
    auto pending = harness.resolve("love", domain_chat(tgcli::core::TdChatKind::BasicGroup, 51));
    harness.respond_me();
    const auto full = harness.respond(
        tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
        tgcli::core::TdBasicGroupFullInfo{
            .description = {}, .creator_user_id = 3, .members = {member(3), member(4)}});
    CHECK(field_as<std::int64_t>(full, "basic_group_id") == 51);
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(3, "Ada", "Lovelace"));
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(4, "Grace", "Hopper"));
    const auto outcome = pending.get();
    CHECK(resolved(outcome).id == 3);
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetContacts) == 0);
}

TEST_CASE("supergroup user substring probes complete Search pages with advancing offsets",
          "[resolver][user-resolver][supergroup][pagination][fake-boundary]") {
    UserResolverHarness harness;
    auto pending = harness.resolve("ada", domain_chat(tgcli::core::TdChatKind::Supergroup, 55));
    harness.respond_me();
    const auto first = harness.respond(
        tgcli::core::TdFunctionKind::GetSupergroupMembers,
        tgcli::core::TdChatMembers{.total_count = 3, .members = {member(7), member(8)}});
    CHECK(field_as<std::string>(first, "filter") == "search");
    CHECK(field_as<std::string>(first, "query") == "ada");
    CHECK(field_as<std::int64_t>(first, "offset") == 0);
    CHECK(field_as<std::int64_t>(first, "limit") == 200);
    const auto second =
        harness.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                        tgcli::core::TdChatMembers{.total_count = 3, .members = {member(9)}});
    CHECK(field_as<std::int64_t>(second, "offset") == 2);
    const auto empty = harness.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                                       tgcli::core::TdChatMembers{.total_count = 3, .members = {}});
    CHECK(field_as<std::int64_t>(empty, "offset") == 3);
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(7, "Grace", "Hopper"));
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(8, "Alan", "Turing"));
    harness.respond(tgcli::core::TdFunctionKind::GetUser,
                    UserResolverHarness::user(9, "ADA", "Lovelace"));
    const auto outcome = pending.get();
    CHECK(resolved(outcome).id == 9);
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 3);
}

TEST_CASE("user ambiguity retains TD order truncates at twenty and has no scope field",
          "[resolver][user-resolver][contacts][ambiguous][fake-boundary]") {
    UserResolverHarness harness;
    auto pending = harness.resolve("match");
    harness.respond_me();
    std::vector<std::int64_t> ids;
    for (std::int64_t id = 1; id <= 22; ++id) {
        ids.push_back(id);
    }
    harness.respond(tgcli::core::TdFunctionKind::GetContacts,
                    tgcli::core::TdUsers{.total_count = 22, .user_ids = ids});
    for (const auto id : ids) {
        harness.respond(tgcli::core::TdFunctionKind::GetUser,
                        UserResolverHarness::user(id, "Match", std::to_string(id), id == 22,
                                                  {"user" + std::to_string(id)}));
    }
    const auto outcome = pending.get();
    const auto mapped = terminal(outcome);
    CHECK(mapped["code"] == "AMBIGUOUS");
    CHECK(mapped["details"]["selector"] == "match");
    CHECK_FALSE(mapped["details"].contains("scope"));
    REQUIRE(mapped["details"]["candidates"].size() == 20);
    CHECK(mapped["details"]["candidates"].front()["id"] == 1);
    CHECK(mapped["details"]["candidates"].back()["id"] == 20);
    CHECK(mapped["details"]["truncated"] == true);
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 22);
}

TEST_CASE("bot user resolver rejects only the contacts-only substring branch",
          "[resolver][user-resolver][bot][ordering][fake-boundary]") {
    UserResolverHarness harness;
    auto pending = harness.resolve("Display Name");
    harness.respond_me(true);
    const auto outcome = pending.get();
    const auto mapped = terminal(outcome);
    CHECK(mapped["code"] == "BOT_UNSUPPORTED");
    CHECK(mapped["details"] == json{{"operation", "resolve"}});
    CHECK(harness.count(tgcli::core::TdFunctionKind::GetContacts) == 0);
    CHECK(harness.sent_count() == 2);
}

TEST_CASE("user resolver preserves request deadline and authorization-loss precedence",
          "[resolver][user-resolver][timeout][authorization][fake-boundary]") {
    SECTION("pre-expired admission sends no principal read") {
        UserResolverHarness harness;
        const auto expired = tgcli::daemon::RequestSession::Clock::now() - 1ms;
        const auto outcome = harness.resolve("77", std::nullopt, expired).get();
        const auto mapped = terminal(outcome);
        CHECK(mapped["code"] == "TIMEOUT");
        CHECK(mapped["details"]["operation"] == "resolve");
        CHECK(harness.sent_count() == 1);
    }

    SECTION("authorization loss wins over a late contacts response") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada");
        harness.respond_me();
        harness.lose_authorization_before_response(
            tgcli::core::TdFunctionKind::GetContacts,
            tgcli::core::TdUsers{.total_count = 1, .user_ids = {7}});
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "NOT_AUTHED");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }
}

TEST_CASE("user resolver attributes typed TD failures to resolve",
          "[resolver][user-resolver][error][fake-boundary]") {
    SECTION("numeric getUser TD failure") {
        UserResolverHarness harness;
        auto pending = harness.resolve("77");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetUser,
                        tgcli::core::TdError{500, "USER_FETCH_FAILED"});
        const auto mapped = terminal(pending.get());
        CHECK(mapped["code"] == "TDLIB_ERROR");
        CHECK(mapped["details"] == json{{"operation", "resolve"}, {"tdlib_code", 500}});
    }

    SECTION("contacts rate limit") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetContacts,
                        tgcli::core::TdError{429, "FLOOD_WAIT_17"});
        const auto mapped = terminal(pending.get());
        CHECK(mapped["code"] == "RATE_LIMITED");
        CHECK(mapped["details"] ==
              json{{"operation", "resolve"}, {"tdlib_code", 429}, {"retry_after", 17}});
    }
}

TEST_CASE("user resolver fails closed on malformed or duplicate complete domains",
          "[resolver][user-resolver][malformed][pagination][fake-boundary]") {
    SECTION("contacts count mismatch") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetContacts,
                        tgcli::core::TdUsers{.total_count = 2, .user_ids = {7}});
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("duplicate contacts") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetContacts,
                        tgcli::core::TdUsers{.total_count = 2, .user_ids = {7, 7}});
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("null native contacts conversion is unexpected") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetContacts,
                        tgcli::core::TdDirectConversionError{});
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("duplicate basic-group members") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada", domain_chat(tgcli::core::TdChatKind::BasicGroup, 51));
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                        tgcli::core::TdBasicGroupFullInfo{.description = {},
                                                          .creator_user_id = 7,
                                                          .members = {member(7), member(7)}});
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("basic-group member sender is outside the user domain") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada", domain_chat(tgcli::core::TdChatKind::BasicGroup, 51));
        harness.respond_me();
        auto invalid = member(7);
        invalid.member = {.kind = tgcli::core::TdMessageSenderKind::Chat, .id = -1007};
        harness.respond(tgcli::core::TdFunctionKind::GetBasicGroupFullInfo,
                        tgcli::core::TdBasicGroupFullInfo{
                            .description = {}, .creator_user_id = 7, .members = {invalid}});
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("duplicate supergroup member across advancing pages") {
        UserResolverHarness harness;
        auto pending = harness.resolve("Ada", domain_chat(tgcli::core::TdChatKind::Supergroup, 55));
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                        tgcli::core::TdChatMembers{.total_count = 2, .members = {member(7)}});
        harness.respond(tgcli::core::TdFunctionKind::GetSupergroupMembers,
                        tgcli::core::TdChatMembers{.total_count = 2, .members = {member(7)}});
        const auto outcome = pending.get();
        const auto mapped = terminal(outcome);
        CHECK(mapped["code"] == "PAGINATION_INVALID");
        CHECK(mapped["details"] ==
              json{{"operation", "resolve"}, {"reason", "non_advancing_upstream"}});
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("returned user identity must stay bound to the requested id") {
        UserResolverHarness harness;
        auto pending = harness.resolve("77");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::GetUser,
                        UserResolverHarness::user(78, "Foreign"));
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
    }

    SECTION("public lookup must return a private user chat") {
        UserResolverHarness harness;
        auto pending = harness.resolve("@channel");
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::SearchPublicChat,
                        domain_chat(tgcli::core::TdChatKind::Channel, 55));
        const auto outcome = pending.get();
        CHECK(terminal(outcome)["code"] == "INTERNAL");
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetUser) == 0);
    }
}

TEST_CASE("exact user selectors ignore a supplied member domain and malformed links do not search",
          "[resolver][user-resolver][ordering][fake-boundary]") {
    SECTION("exact username performs no member enumeration") {
        UserResolverHarness harness;
        auto pending =
            harness.resolve("@helper", domain_chat(tgcli::core::TdChatKind::Supergroup, 55));
        harness.respond_me();
        harness.respond(tgcli::core::TdFunctionKind::SearchPublicChat, private_chat(88));
        harness.respond(tgcli::core::TdFunctionKind::GetUser,
                        UserResolverHarness::user(88, "Helper", {}, false, {"helper"}));
        CHECK(resolved(pending.get()).id == 88);
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetSupergroupMembers) == 0);
    }

    SECTION("malformed profile link is local usage") {
        UserResolverHarness harness;
        auto pending = harness.resolve("t.me/helper/");
        harness.respond_me();
        const auto mapped = terminal(pending.get());
        CHECK(mapped["code"] == "USAGE");
        CHECK(mapped["details"] == json{{"argument", "from"}, {"reason", "invalid_argument"}});
        CHECK(harness.count(tgcli::core::TdFunctionKind::SearchPublicChat) == 0);
        CHECK(harness.count(tgcli::core::TdFunctionKind::GetContacts) == 0);
    }
}
