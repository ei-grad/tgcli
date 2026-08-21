#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <variant>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

namespace {

class ConsumerFixture {
  public:
    explicit ConsumerFixture(
        std::optional<tgcli::daemon::RequestSession::Clock::time_point> admission_deadline = {}) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (client_->auth_state()->auth_sequence != 1 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(client_->auth_state()->auth_sequence == 1);
        tgcli::proto::Request request("main");
        request.command = {"msg", "get"};
        request.context.timeout_seconds = 1.0;
        request.context.cwd = "/";
        sink_ = std::make_unique<tgcli::daemon::CallbackSink>(
            [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
            [&](const nlohmann::json&) { ++terminals_; },
            [&](const std::string&, const std::string&, const nlohmann::json&, int) {
                ++terminals_;
            });
        session_ = std::make_unique<tgcli::daemon::RequestSession>(
            std::move(request), *sink_, 0, tgcli::daemon::RequestSession::NonceGenerator{},
            tgcli::daemon::ActivityTracker::Token{}, nullptr, admission_deadline);
        consumer_ = std::make_unique<tgcli::daemon::ResolverConsumer>(*client_, "main", *session_);
    }

    template <typename T> void respond(tgcli::core::TdFunctionKind expected, T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    [[nodiscard]] tgcli::daemon::ResolverConsumer& consumer() {
        return *consumer_;
    }

    [[nodiscard]] tgcli::daemon::RequestSession& session() {
        return *session_;
    }

    [[nodiscard]] int terminals() const {
        return terminals_;
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

  private:
    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient client_id_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::CallbackSink> sink_;
    std::unique_ptr<tgcli::daemon::RequestSession> session_;
    std::unique_ptr<tgcli::daemon::ResolverConsumer> consumer_;
    std::size_t sent_count_ = 1;
    int terminals_ = 0;
};

tgcli::core::TdUserSummary user() {
    return {.id = 42,
            .first_name = "Ada",
            .last_name = "",
            .usernames = {"ada"},
            .phone_number = "12025550123",
            .is_bot = false,
            .is_premium = false};
}

tgcli::core::TdChat chat() {
    return {.id = -1001,
            .title = "Project",
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
            .notification_settings = std::nullopt};
}

} // namespace

TEST_CASE("ResolverConsumer rejects title-like write targets before every TD read",
          "[resolver][consumer][m3-foundation][fake-boundary]") {
    ConsumerFixture fixture;
    const auto sent_before = fixture.sent_count();
    const auto outcome = fixture.consumer().resolve_exact_chat("Project Team");
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverError>(outcome));
    const auto& error = std::get<tgcli::daemon::ResolverError>(outcome);
    const auto* usage = std::get_if<tgcli::daemon::ResolverUsageError>(&error);
    REQUIRE(usage != nullptr);
    CHECK(usage->argument == "chat");
    CHECK(usage->reason == tgcli::daemon::ResolverUsageReason::InvalidArgument);
    CHECK(fixture.sent_count() == sent_before);
}

TEST_CASE("ResolverConsumer returns immutable context without emitting a terminal",
          "[resolver][consumer][fake-boundary]") {
    ConsumerFixture fixture;
    auto bound = std::async(std::launch::async, [&] {
        return fixture.consumer().bind_principal(tgcli::daemon::M2Operation::MsgGet);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, user());
    const auto principal = bound.get();
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverPrincipal>(principal));
    CHECK(std::get<tgcli::daemon::ResolverPrincipal>(principal).id == 42);

    auto resolved = std::async(std::launch::async, [&] {
        return fixture.consumer().resolve_chat("https://t.me/project/999",
                                               tgcli::daemon::ResolverScope::ActiveDialogs);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetInternalLinkType,
                    tgcli::core::TdInternalLink{.kind = tgcli::core::TdInternalLinkKind::Message,
                                                .username = {},
                                                .url = "https://t.me/project/999",
                                                .tdlib_type_id = 1});
    fixture.respond(tgcli::core::TdFunctionKind::GetMessageLinkInfo,
                    tgcli::core::TdMessageLinkInfo{
                        .is_public = true,
                        .chat_id = -1001,
                        .message_id = 999,
                        .topic = tgcli::core::TdTopic{
                            .kind = tgcli::core::TdTopicKind::Forum, .id = 7, .tdlib_type_id = 1}});
    fixture.respond(tgcli::core::TdFunctionKind::GetChat, chat());
    const auto outcome = resolved.get();
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolvedChatTarget>(outcome));
    const auto& target = std::get<tgcli::daemon::ResolvedChatTarget>(outcome);
    CHECK(target.principal.id == 42);
    CHECK(target.chat.id == -1001);
    CHECK(target.contextual_message_id == 999);
    const tgcli::daemon::TopicRef expected_topic{.kind = tgcli::daemon::TopicKind::Forum, .id = 7};
    CHECK(target.contextual_topic == expected_topic);
    CHECK(target.link_type == tgcli::daemon::ResolvedLinkType::Message);
    CHECK(target.is_public == true);
    CHECK(fixture.terminals() == 0);

    const auto sent_before_repeat = fixture.sent_count();
    const auto repeated = fixture.consumer().bind_principal(tgcli::daemon::M2Operation::MsgLink);
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverError>(repeated));
    const auto& error = std::get<tgcli::daemon::ResolverError>(repeated);
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverInternalError>(error));
    CHECK(fixture.sent_count() == sent_before_repeat);
    CHECK(fixture.terminals() == 0);
}

TEST_CASE("ResolverConsumer keeps outer and selector failure attribution distinct",
          "[resolver][consumer][attribution][fake-boundary]") {
    SECTION("getMe failure retains an M3 caller identity") {
        ConsumerFixture fixture;
        auto bound = std::async(std::launch::async, [&] {
            return fixture.consumer().bind_principal(tgcli::proto::M3Operation::SavedAttach);
        });
        fixture.respond(tgcli::core::TdFunctionKind::GetMe,
                        tgcli::core::TdError{429, "FLOOD_WAIT_8"});
        const auto outcome = bound.get();
        REQUIRE(std::holds_alternative<tgcli::daemon::ResolverError>(outcome));
        const auto& error = std::get<tgcli::daemon::ResolverError>(outcome);
        const auto* rate = std::get_if<tgcli::daemon::ResolverRateLimitedError>(&error);
        REQUIRE(rate != nullptr);
        CHECK(std::get<tgcli::proto::M3Operation>(rate->operation) ==
              tgcli::proto::M3Operation::SavedAttach);
        CHECK(rate->retry_after == 8);
        CHECK(fixture.terminals() == 0);
    }
    SECTION("selector failure is always resolve") {
        ConsumerFixture fixture;
        auto bound = std::async(std::launch::async, [&] {
            return fixture.consumer().bind_principal(tgcli::daemon::M2Operation::MsgLink);
        });
        fixture.respond(tgcli::core::TdFunctionKind::GetMe, user());
        REQUIRE(std::holds_alternative<tgcli::daemon::ResolverPrincipal>(bound.get()));
        auto resolved = std::async(std::launch::async, [&] {
            return fixture.consumer().resolve_chat("-1001",
                                                   tgcli::daemon::ResolverScope::ActiveDialogs);
        });
        fixture.respond(tgcli::core::TdFunctionKind::GetChat,
                        tgcli::core::TdError{429, "retry after 3"});
        const auto outcome = resolved.get();
        REQUIRE(std::holds_alternative<tgcli::daemon::ResolverError>(outcome));
        const auto& error = std::get<tgcli::daemon::ResolverError>(outcome);
        const auto* rate = std::get_if<tgcli::daemon::ResolverRateLimitedError>(&error);
        REQUIRE(rate != nullptr);
        CHECK(std::get<tgcli::daemon::M2Operation>(rate->operation) ==
              tgcli::daemon::M2Operation::Resolve);
        CHECK(fixture.terminals() == 0);
    }
}

TEST_CASE("ResolverConsumer classifies local message links without a TD link request",
          "[resolver][consumer][local][fake-boundary]") {
    ConsumerFixture fixture;
    auto bound = std::async(std::launch::async, [&] {
        return fixture.consumer().bind_principal(tgcli::daemon::M2Operation::Read);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, user());
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverPrincipal>(bound.get()));
    const auto sent_before_resolve = fixture.sent_count();

    auto resolved = std::async(std::launch::async, [&] {
        return fixture.consumer().resolve_chat("t.me/project/5",
                                               tgcli::daemon::ResolverScope::LocalMaterialized);
    });
    REQUIRE(resolved.wait_for(200ms) == std::future_status::ready);
    const auto outcome = resolved.get();
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverError>(outcome));
    const auto& error = std::get<tgcli::daemon::ResolverError>(outcome);
    const auto* missing = std::get_if<tgcli::daemon::ResolverNotFoundError>(&error);
    REQUIRE(missing != nullptr);
    CHECK(missing->scope == tgcli::daemon::ResolverScope::LocalMaterialized);
    CHECK(fixture.sent_count() == sent_before_resolve);
    CHECK(fixture.terminals() == 0);
}

TEST_CASE("ResolverConsumer target reads retain ReadyRead cancellation unchanged",
          "[resolver][consumer][cancel][fake-boundary]") {
    ConsumerFixture fixture;
    auto bound = std::async(std::launch::async, [&] {
        return fixture.consumer().bind_principal(tgcli::daemon::M2Operation::MsgGet);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, user());
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverPrincipal>(bound.get()));
    auto resolved = std::async(std::launch::async, [&] {
        return fixture.consumer().resolve_chat("-1001",
                                               tgcli::daemon::ResolverScope::ActiveDialogs);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetChat, chat());
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolvedChatTarget>(resolved.get()));
    fixture.session().disconnect();
    bool started = false;
    const auto target = fixture.consumer().read_target([&](const auto&) {
        started = true;
        return std::future<tgcli::core::TdValue>{};
    });
    CHECK(target.status == tgcli::daemon::ReadyReadStatus::Cancelled);
    CHECK_FALSE(started);
    CHECK(fixture.terminals() == 0);
}

TEST_CASE("ResolverConsumer reuses the request absolute deadline for target reads",
          "[resolver][consumer][deadline][fake-boundary]") {
    const auto deadline = tgcli::daemon::RequestSession::Clock::now() + 2s;
    ConsumerFixture fixture(deadline);
    auto bound = std::async(std::launch::async, [&] {
        return fixture.consumer().bind_principal(tgcli::daemon::M2Operation::MsgLink);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, user());
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolverPrincipal>(bound.get()));
    auto resolved = std::async(std::launch::async, [&] {
        return fixture.consumer().resolve_chat("-1001",
                                               tgcli::daemon::ResolverScope::ActiveDialogs);
    });
    fixture.respond(tgcli::core::TdFunctionKind::GetChat, chat());
    REQUIRE(std::holds_alternative<tgcli::daemon::ResolvedChatTarget>(resolved.get()));
    std::this_thread::sleep_until(deadline);
    bool started = false;
    const auto target = fixture.consumer().read_target([&](const auto&) {
        started = true;
        return std::future<tgcli::core::TdValue>{};
    });
    CHECK(target.status == tgcli::daemon::ReadyReadStatus::TimedOut);
    CHECK_FALSE(started);
    CHECK(fixture.terminals() == 0);
}
