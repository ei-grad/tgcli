#include "common/exit_codes.hpp"
#include "daemon/chats_commands.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
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

class FakeChats {
  public:
    explicit FakeChats(tgcli::core::AuthState state = tgcli::core::AuthState::Ready) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(
            std::move(runtime), tgcli::core::TdLogConfiguration{},
            tgcli::core::TdClientEventHooks{.now = [this] { return event_now(); },
                                            .after_observed = {},
                                            .before_lifecycle_callback_drain_wait = {},
                                            .before_closed_decisions_drain_wait = {}});
        REQUIRE(runtime_->wait_for_sent(1));
        REQUIRE(runtime_->clients().size() == 1);
        td_client_ = runtime_->clients().front();
        runtime_->push_response(td_client_, 1, {}, tgcli::core::AuthStateData{state});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ =
            std::make_unique<tgcli::daemon::ChatsCoordinator>(*client_, std::string("main"));
        tgcli::daemon::register_chats_command(dispatcher_, *coordinator_);
        tgcli::daemon::register_unread_command(dispatcher_, *coordinator_);
    }

    std::future<Outcome> dispatch(
        tgcli::proto::Request request,
        std::optional<tgcli::daemon::RequestSession::Clock::time_point> admission_deadline = {}) {
        return std::async(
            std::launch::async, [this, request = std::move(request), admission_deadline]() mutable {
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
                tgcli::daemon::RequestSession session(
                    std::move(request), sink, 0, tgcli::daemon::RequestSession::NonceGenerator{},
                    tgcli::daemon::ActivityTracker::Token{}, {}, admission_deadline);
                dispatcher_.dispatch(session);
                if (outcome.result) {
                    outcome.exit_code = tgcli::kOk;
                }
                return outcome;
            });
    }

    template <typename T>
    tgcli::core::TdFunctionData
    respond(tgcli::core::TdFunctionKind expected, T value,
            tgcli::core::TdEventClock::time_point observed_at = tgcli::core::TdEventClock::now()) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        const auto descriptor = sent.back().function;
        set_event_now(observed_at);
        runtime_->push_response(td_client_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
        return descriptor;
    }

    void respond_me(
        bool bot = false, std::int64_t id = 42,
        tgcli::core::TdEventClock::time_point observed_at = tgcli::core::TdEventClock::now()) {
        respond(tgcli::core::TdFunctionKind::GetMe,
                tgcli::core::TdUserSummary{.id = id,
                                           .first_name = "Ada",
                                           .last_name = "",
                                           .usernames = {"ada"},
                                           .phone_number = bot ? "" : "12025550123",
                                           .is_bot = bot,
                                           .is_premium = false},
                observed_at);
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

    [[nodiscard]] tgcli::core::TdEventClock::time_point event_now() const {
        const std::lock_guard lock(event_clock_mutex_);
        return event_now_;
    }

    void set_event_now(tgcli::core::TdEventClock::time_point now) {
        const std::lock_guard lock(event_clock_mutex_);
        event_now_ = now;
    }

    mutable std::mutex event_clock_mutex_;
    tgcli::core::TdEventClock::time_point event_now_ = tgcli::core::TdEventClock::now();
    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient td_client_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::ChatsCoordinator> coordinator_;
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

tgcli::core::TdChatList main_list() {
    return {.kind = tgcli::core::TdChatListKind::Main, .folder_id = 0, .tdlib_type_id = 0};
}

tgcli::core::TdChatList archive_list() {
    return {.kind = tgcli::core::TdChatListKind::Archive, .folder_id = 0, .tdlib_type_id = 0};
}

tgcli::core::TdChatList folder_list(std::int32_t id) {
    return {.kind = tgcli::core::TdChatListKind::Folder, .folder_id = id, .tdlib_type_id = 0};
}

tgcli::core::TdChat chat(std::int64_t id, std::int64_t order, tgcli::core::TdChatList selected,
                         bool unread = false) {
    return {.id = id,
            .title = "Chat " + std::to_string(id),
            .kind = tgcli::core::TdChatKind::BasicGroup,
            .related_id = 0,
            .tdlib_type_id = 1,
            .positions = {{.list = selected, .order = order}},
            .chat_lists = {selected},
            .is_marked_unread = false,
            .unread_count = unread ? 1 : 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .permissions = std::nullopt,
            .notification_settings = std::nullopt};
}

tgcli::proto::Request chats_request(std::optional<std::int32_t> folder = std::nullopt,
                                    bool archived = false, bool unread = false,
                                    std::optional<std::int32_t> limit = std::nullopt,
                                    std::optional<std::string> cursor = std::nullopt,
                                    std::string account = "main") {
    tgcli::proto::Request request(std::move(account));
    request.id = 1;
    request.command = {"chats"};
    request.args = {{"archived", archived},
                    {"cursor", cursor ? json(*cursor) : json(nullptr)},
                    {"folder", folder ? json(*folder) : json(nullptr)},
                    {"limit", limit ? json(*limit) : json(nullptr)},
                    {"unread", unread}};
    request.context.timeout_seconds = 2.0;
    request.context.cwd = "/";
    return request;
}

tgcli::proto::Request unread_request(json args = json::object()) {
    tgcli::proto::Request request("main");
    request.id = 1;
    request.command = {"unread"};
    request.args = std::move(args);
    request.context.timeout_seconds = 2.0;
    request.context.cwd = "/";
    return request;
}

} // namespace

TEST_CASE("chats cursors are canonical and validate structural state", "[chats][cursor]") {
    const tgcli::daemon::ChatsCursor cursor{.version = 1,
                                            .operation = "chats",
                                            .account = "main",
                                            .user_id = 42,
                                            .limit = 17,
                                            .list = tgcli::core::TdChatListKind::Folder,
                                            .folder_id = 2,
                                            .unread = true,
                                            .order = 900,
                                            .chat_id = -1001};
    const auto encoded = tgcli::daemon::encode_chats_cursor(cursor);
    REQUIRE(tgcli::daemon::decode_chats_cursor(encoded));
    CHECK(*tgcli::daemon::decode_chats_cursor(encoded) == cursor);
    CHECK_FALSE(tgcli::daemon::decode_chats_cursor(encoded + "A"));

    auto invalid = cursor;
    invalid.folder_id.reset();
    CHECK_FALSE(tgcli::daemon::decode_chats_cursor(tgcli::daemon::encode_chats_cursor(invalid)));
    invalid = cursor;
    invalid.limit = 101;
    CHECK_FALSE(tgcli::daemon::decode_chats_cursor(tgcli::daemon::encode_chats_cursor(invalid)));
}

TEST_CASE("chats materializes exact folder membership and message summary",
          "[chats][contract][schema]") {
    FakeChats fake;
    auto future = fake.dispatch(chats_request(2, false, false, 1));
    fake.respond_me();
    const auto listed = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                                     tgcli::core::TdChats{.chat_ids = {-1001}});
    CHECK(field_as<std::string>(listed, "list") == "folder");
    CHECK(field_as<std::int64_t>(listed, "folder_id") == 2);
    CHECK(field_as<std::int64_t>(listed, "limit") == 100);

    auto item = chat(-1001, 900, folder_list(2));
    item.title = "Project";
    item.chat_lists = {folder_list(3), archive_list(), folder_list(2), folder_list(3)};
    item.unread_count = 3;
    item.unread_mention_count = 1;
    item.last_message = tgcli::core::TdMessageSummary{
        .id = 123,
        .chat_id = -1001,
        .date = 1785924000,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = false,
        .topic = tgcli::core::TdTopic{.kind = tgcli::core::TdTopicKind::Forum,
                                      .id = 7,
                                      .tdlib_type_id = 1},
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "experiment result"};
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(item));

    const auto outcome = future.get();
    REQUIRE(outcome.result);
    CHECK(outcome.exit_code == tgcli::kOk);
    CHECK(outcome.terminal_count == 1);
    REQUIRE(outcome.result->at("items").size() == 1);
    const auto& summary = outcome.result->at("items").front();
    CHECK(summary.at("folder_ids") == json::array({2, 3}));
    CHECK(summary.at("is_archived") == true);
    CHECK(summary.at("last_message").at("topic") == json{{"kind", "forum"}, {"id", 7}});
    CHECK(outcome.result->at("next").is_string());
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("chats.result.schema.json"));
}

TEST_CASE("chats sparse unread scan grows the raw prefix without a cap",
          "[chats][pagination][unread]") {
    FakeChats fake;
    auto future = fake.dispatch(chats_request(std::nullopt, false, true, 1));
    fake.respond_me();

    std::vector<std::int64_t> first_ids;
    for (std::int64_t id = 1; id <= 100; ++id) {
        first_ids.push_back(id);
    }
    fake.respond(tgcli::core::TdFunctionKind::GetChats,
                 tgcli::core::TdChats{.chat_ids = first_ids});
    for (const auto id : first_ids) {
        fake.respond(tgcli::core::TdFunctionKind::GetChat, chat(id, 1000 - id, main_list()));
    }
    const auto loaded = fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdOk{});
    CHECK(field_as<std::string>(loaded, "list") == "main");
    CHECK(field_as<std::int64_t>(loaded, "limit") == 100);

    auto grown_ids = first_ids;
    grown_ids.push_back(101);
    const auto grown = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                                    tgcli::core::TdChats{.chat_ids = grown_ids});
    CHECK(field_as<std::int64_t>(grown, "limit") == 200);
    for (const auto id : grown_ids) {
        fake.respond(tgcli::core::TdFunctionKind::GetChat,
                     chat(id, 1000 - id, main_list(), id == 101));
    }

    const auto outcome = future.get();
    REQUIRE(outcome.result);
    REQUIRE(outcome.result->at("items").size() == 1);
    CHECK(outcome.result->at("items").front().at("id") == 101);
    CHECK(fake.count(tgcli::core::TdFunctionKind::LoadChats) == 1);
}

TEST_CASE("chats cursor orders equal positions and survives a missing anchor",
          "[chats][pagination][cursor]") {
    FakeChats first;
    auto first_future = first.dispatch(chats_request(std::nullopt, false, false, 2));
    first.respond_me();
    first.respond(tgcli::core::TdFunctionKind::GetChats,
                  tgcli::core::TdChats{.chat_ids = {-3, -1, -2}});
    for (const auto id : {-3LL, -1LL, -2LL}) {
        first.respond(tgcli::core::TdFunctionKind::GetChat, chat(id, 900, main_list()));
    }
    const auto first_outcome = first_future.get();
    REQUIRE(first_outcome.result);
    REQUIRE(first_outcome.result->at("items").size() == 2);
    CHECK(first_outcome.result->at("items")[0].at("id") == -1);
    CHECK(first_outcome.result->at("items")[1].at("id") == -2);
    const auto cursor = first_outcome.result->at("next").get<std::string>();

    FakeChats second;
    auto second_future =
        second.dispatch(chats_request(std::nullopt, false, false, std::nullopt, cursor));
    second.respond_me();
    second.respond(tgcli::core::TdFunctionKind::GetChats,
                   tgcli::core::TdChats{.chat_ids = {-1, -3}});
    second.respond(tgcli::core::TdFunctionKind::GetChat, chat(-1, 900, main_list()));
    second.respond(tgcli::core::TdFunctionKind::GetChat, chat(-3, 900, main_list()));
    second.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"});
    const auto second_outcome = second_future.get();
    REQUIRE(second_outcome.result);
    REQUIRE(second_outcome.result->at("items").size() == 1);
    CHECK(second_outcome.result->at("items").front().at("id") == -3);
    CHECK(second_outcome.result->at("next").is_null());
}

TEST_CASE("chats covers archive scope and skips secret chats without ending the page",
          "[chats][lists]") {
    SECTION("archive reaches documented end") {
        FakeChats fake;
        auto future = fake.dispatch(chats_request(std::nullopt, true));
        fake.respond_me();
        const auto listed = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                                         tgcli::core::TdChats{.chat_ids = {}});
        CHECK(field_as<std::string>(listed, "list") == "archive");
        const auto loaded = fake.respond(tgcli::core::TdFunctionKind::LoadChats,
                                         tgcli::core::TdError{404, "Not Found"});
        CHECK(field_as<std::string>(loaded, "list") == "archive");
        const auto outcome = future.get();
        CHECK(outcome.result == json{{"items", json::array()}, {"next", nullptr}});
    }

    SECTION("secret chat does not consume a match slot") {
        FakeChats fake;
        auto future = fake.dispatch(chats_request(std::nullopt, false, false, 1));
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChats,
                     tgcli::core::TdChats{.chat_ids = {-1, -2}});
        auto secret = chat(-1, 900, main_list());
        secret.kind = tgcli::core::TdChatKind::Secret;
        secret.related_id = 42;
        fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(secret));
        fake.respond(tgcli::core::TdFunctionKind::GetChat, chat(-2, 800, main_list()));
        const auto outcome = future.get();
        REQUIRE(outcome.result);
        REQUIRE(outcome.result->at("items").size() == 1);
        CHECK(outcome.result->at("items").front().at("id") == -2);
    }
}

TEST_CASE("chats cursor scope binds operation account and current user before listing",
          "[chats][cursor][preflight]") {
    const tgcli::daemon::ChatsCursor base{.version = 1,
                                          .operation = "chats",
                                          .account = "main",
                                          .user_id = 42,
                                          .limit = 1,
                                          .list = tgcli::core::TdChatListKind::Main,
                                          .folder_id = std::nullopt,
                                          .unread = false,
                                          .order = 900,
                                          .chat_id = -2};
    for (const std::string_view mismatch : {"operation", "account", "user"}) {
        CAPTURE(mismatch);
        auto cursor = base;
        if (mismatch == "operation") {
            cursor.operation = "read";
        } else if (mismatch == "account") {
            cursor.account = "work";
        } else {
            cursor.user_id = 43;
        }
        FakeChats fake;
        auto future = fake.dispatch(chats_request(std::nullopt, false, false, std::nullopt,
                                                  tgcli::daemon::encode_chats_cursor(cursor)));
        fake.respond_me();
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "USAGE");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"argument", "--cursor"}, {"reason", "cursor_scope_mismatch"}});
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChats) == 0);
    }
}

TEST_CASE("chats preflight and list failures are terminal and do not leak reads",
          "[chats][error]") {
    SECTION("non-ready account stops before getMe") {
        FakeChats fake(tgcli::core::AuthState::WaitCode);
        const auto outcome = fake.dispatch(chats_request()).get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "NOT_AUTHED");
        CHECK(outcome.exit_code == tgcli::kNotAuthed);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }

    SECTION("bot stops before getChats") {
        FakeChats fake;
        auto future = fake.dispatch(chats_request());
        fake.respond_me(true);
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "BOT_UNSUPPORTED");
        CHECK(outcome.error->at("error").at("details") == json{{"operation", "chats"}});
        CHECK(outcome.exit_code == tgcli::kUsage);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChats) == 0);
    }

    SECTION("missing folder is contextual NOT_FOUND") {
        FakeChats fake;
        auto future = fake.dispatch(chats_request(7));
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChats,
                     tgcli::core::TdError{400, "CHAT_LIST_NOT_FOUND"});
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "NOT_FOUND");
        CHECK(outcome.error->at("error").at("details") == json{{"folder_id", 7}});
        CHECK(outcome.exit_code == tgcli::kNotFound);
        CHECK(outcome.terminal_count == 1);
    }

    SECTION("list rate limit preserves the standard retry details") {
        FakeChats fake;
        auto future = fake.dispatch(chats_request());
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChats,
                     tgcli::core::TdError{429, "FLOOD_WAIT_12"});
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "RATE_LIMITED");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"operation", "chats"}, {"tdlib_code", 429}, {"retry_after", 12}});
        CHECK(outcome.exit_code == tgcli::kRateLimited);
        CHECK(outcome.terminal_count == 1);
    }

    SECTION("malformed arguments stop before getMe") {
        FakeChats fake;
        auto request = chats_request();
        request.args["extra"] = true;
        const auto outcome = fake.dispatch(std::move(request)).get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "USAGE");
        CHECK(outcome.exit_code == tgcli::kUsage);
        CHECK(outcome.terminal_count == 1);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }
}

TEST_CASE("chats rejects list responses observed at or after its absolute deadline",
          "[chats][deadline][ready-read][error]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;
    for (const auto observed_at : {deadline, deadline + 1ns}) {
        DYNAMIC_SECTION((observed_at == deadline ? "equal" : "after")) {
            FakeChats fake;
            auto future = fake.dispatch(chats_request(), deadline);
            fake.respond_me(false, 42, deadline - 2ns);
            fake.respond(tgcli::core::TdFunctionKind::GetChats,
                         tgcli::core::TdChats{.chat_ids = {}}, observed_at);
            const auto outcome = future.get();
            REQUIRE(outcome.error);
            CHECK(outcome.error->at("error").at("code") == "TIMEOUT");
            CHECK(outcome.error->at("error").at("details") ==
                  json{{"operation", "chats"}, {"state", "ready"}});
            CHECK(outcome.exit_code == tgcli::kTimeout);
            CHECK(outcome.terminal_count == 1);
            CHECK(fake.count(tgcli::core::TdFunctionKind::LoadChats) == 0);
        }
    }
}

TEST_CASE("unread completely loads Main then Archive and emits exact deduplicated summaries",
          "[unread][contract][lists][schema]") {
    FakeChats fake;
    auto future = fake.dispatch(unread_request());
    fake.respond_me();

    auto listed =
        fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {-1}});
    CHECK(field_as<std::string>(listed, "list") == "main");
    CHECK(field_as<std::int64_t>(listed, "limit") == 100);
    auto loaded = fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdOk{});
    CHECK(field_as<std::string>(loaded, "list") == "main");
    CHECK(field_as<std::int64_t>(loaded, "limit") == 100);

    const std::vector<std::int64_t> main_ids{-1, -2, -3, -4, -5, -6, -7, -8, -9};
    listed = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                          tgcli::core::TdChats{.chat_ids = main_ids});
    CHECK(field_as<std::string>(listed, "list") == "main");
    CHECK(field_as<std::int64_t>(listed, "limit") == 200);
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"});

    const std::vector<std::int64_t> archive_ids{-2, -3, -10};
    listed = fake.respond(tgcli::core::TdFunctionKind::GetChats,
                          tgcli::core::TdChats{.chat_ids = archive_ids});
    CHECK(field_as<std::string>(listed, "list") == "archive");
    CHECK(field_as<std::int64_t>(listed, "limit") == 100);
    loaded = fake.respond(tgcli::core::TdFunctionKind::LoadChats,
                          tgcli::core::TdError{404, "Not Found"});
    CHECK(field_as<std::string>(loaded, "list") == "archive");

    auto marked = chat(-1, 900, main_list());
    marked.title = "Marked";
    marked.is_marked_unread = true;
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(marked));

    auto stale_main = chat(-2, 800, archive_list(), true);
    stale_main.title = "Moved to archive";
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(stale_main));

    auto mention = chat(-3, 700, main_list());
    mention.title = "Mention";
    mention.unread_mention_count = 1;
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(mention));

    auto reaction = chat(-4, 600, main_list());
    reaction.title = "Reaction";
    reaction.unread_reaction_count = 2;
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(reaction));

    auto poll = chat(-5, 500, main_list());
    poll.title = "Poll";
    poll.unread_poll_vote_count = 3;
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(poll));

    fake.respond(tgcli::core::TdFunctionKind::GetChat, chat(-6, 400, main_list()));

    auto secret = chat(-7, 300, main_list(), true);
    secret.kind = tgcli::core::TdChatKind::Secret;
    secret.related_id = 707;
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(secret));

    auto private_bot = chat(-8, 200, main_list(), true);
    private_bot.title = "Build Bot";
    private_bot.kind = tgcli::core::TdChatKind::Private;
    private_bot.related_id = 808;
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(private_bot));
    fake.respond(tgcli::core::TdFunctionKind::GetUser,
                 tgcli::core::TdUserSummary{.id = 808,
                                            .first_name = "Build",
                                            .last_name = "Bot",
                                            .usernames = {"build_bot"},
                                            .phone_number = "",
                                            .is_bot = true,
                                            .is_premium = false});

    auto channel = chat(-9, 100, main_list(), true);
    channel.title = "Announcements";
    channel.kind = tgcli::core::TdChatKind::Channel;
    channel.related_id = 909;
    channel.chat_lists = {main_list(), archive_list()};
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(channel));
    fake.respond(
        tgcli::core::TdFunctionKind::GetSupergroup,
        tgcli::core::TdSupergroup{.id = 909, .usernames = {"announcements"}, .is_channel = true});

    auto archived = chat(-2, 90, archive_list(), true);
    archived.title = "Moved to archive";
    fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(archived));
    fake.respond(tgcli::core::TdFunctionKind::GetChat, chat(-10, 80, archive_list()));

    const auto outcome = future.get();
    REQUIRE(outcome.result);
    CHECK(outcome.exit_code == tgcli::kOk);
    CHECK(outcome.terminal_count == 1);
    CHECK(outcome.result->at("next").is_null());
    const auto& items = outcome.result->at("items");
    REQUIRE(items.size() == 7);
    CHECK(items[0].at("id") == -1);
    CHECK(items[0].at("is_marked_unread") == true);
    CHECK(items[1].at("id") == -3);
    CHECK(items[1].at("unread_mention_count") == 1);
    CHECK(items[2].at("id") == -4);
    CHECK(items[2].at("unread_reaction_count") == 2);
    CHECK(items[3].at("id") == -5);
    CHECK(items[3].at("unread_poll_vote_count") == 3);
    CHECK(items[4].at("id") == -8);
    CHECK(items[4].at("type") == "private");
    CHECK(items[4].at("is_bot") == true);
    CHECK(items[5].at("id") == -9);
    CHECK(items[5].at("type") == "channel");
    CHECK(items[5].at("is_bot") == false);
    CHECK(items[5].at("is_archived") == true);
    CHECK(items[6].at("id") == -2);
    CHECK(items[6].at("unread_count") == 1);
    CHECK(items[6].at("is_archived") == true);
    for (const auto& item : items) {
        CHECK(item.size() == 10);
        CHECK_FALSE(item.contains("usernames"));
        CHECK_FALSE(item.contains("folder_ids"));
        CHECK_FALSE(item.contains("last_message"));
    }
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChats) == 3);
    CHECK(fake.count(tgcli::core::TdFunctionKind::LoadChats) == 3);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("unread.result.schema.json"));
}

TEST_CASE("unread preflight and TD failures keep exact terminal errors", "[unread][error]") {
    SECTION("malformed arguments stop before getMe") {
        FakeChats fake;
        const auto outcome = fake.dispatch(unread_request({{"extra", true}})).get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "USAGE");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"argument", nullptr}, {"reason", "invalid_argument"}});
        CHECK(outcome.exit_code == tgcli::kUsage);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }

    SECTION("non-ready account stops before getMe") {
        FakeChats fake(tgcli::core::AuthState::WaitCode);
        const auto outcome = fake.dispatch(unread_request()).get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "NOT_AUTHED");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"account", "main"}, {"state", "wait_code"}, {"reason", "not_ready"}});
        CHECK(outcome.exit_code == tgcli::kNotAuthed);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    }

    SECTION("bot stops before user-only list reads") {
        FakeChats fake;
        auto future = fake.dispatch(unread_request());
        fake.respond_me(true);
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "BOT_UNSUPPORTED");
        CHECK(outcome.error->at("error").at("details") == json{{"operation", "unread"}});
        CHECK(outcome.exit_code == tgcli::kUsage);
        CHECK(fake.count(tgcli::core::TdFunctionKind::GetChats) == 0);
    }

    SECTION("getChats 404 is not list exhaustion") {
        FakeChats fake;
        auto future = fake.dispatch(unread_request());
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdError{404, "Not Found"});
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "TDLIB_ERROR");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"operation", "unread"}, {"tdlib_code", 404}});
        CHECK(outcome.exit_code == tgcli::kGeneric);
    }

    SECTION("loadChats 429 preserves rate-limit details") {
        FakeChats fake;
        auto future = fake.dispatch(unread_request());
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {}});
        fake.respond(tgcli::core::TdFunctionKind::LoadChats,
                     tgcli::core::TdError{429, "FLOOD_WAIT_17"});
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "RATE_LIMITED");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"operation", "unread"}, {"tdlib_code", 429}, {"retry_after", 17}});
        CHECK(outcome.exit_code == tgcli::kRateLimited);
    }

    SECTION("invalid unread counters are internal errors") {
        FakeChats fake;
        auto future = fake.dispatch(unread_request());
        fake.respond_me();
        fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {-1}});
        fake.respond(tgcli::core::TdFunctionKind::LoadChats,
                     tgcli::core::TdError{404, "Not Found"});
        fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {}});
        fake.respond(tgcli::core::TdFunctionKind::LoadChats,
                     tgcli::core::TdError{404, "Not Found"});
        auto invalid = chat(-1, 1, main_list(), true);
        invalid.unread_count = -1;
        fake.respond(tgcli::core::TdFunctionKind::GetChat, std::move(invalid));
        const auto outcome = future.get();
        REQUIRE(outcome.error);
        CHECK(outcome.error->at("error").at("code") == "INTERNAL");
        CHECK(outcome.error->at("error").at("details") ==
              json{{"operation", "unread"}, {"reason", "internal_error"}});
        CHECK(outcome.exit_code == tgcli::kGeneric);
    }
}

TEST_CASE("unread returns no partial Main result when Archive reaches the deadline",
          "[unread][deadline][error]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;
    FakeChats fake;
    auto future = fake.dispatch(unread_request(), deadline);
    fake.respond_me(false, 42, deadline - 5ns);
    fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {-1}},
                 deadline - 4ns);
    fake.respond(tgcli::core::TdFunctionKind::LoadChats, tgcli::core::TdError{404, "Not Found"},
                 deadline - 3ns);
    fake.respond(tgcli::core::TdFunctionKind::GetChats, tgcli::core::TdChats{.chat_ids = {}},
                 deadline);

    const auto outcome = future.get();
    REQUIRE(outcome.error);
    CHECK_FALSE(outcome.result);
    CHECK(outcome.error->at("error").at("code") == "TIMEOUT");
    CHECK(outcome.error->at("error").at("details") ==
          json{{"operation", "unread"}, {"state", "ready"}});
    CHECK(outcome.exit_code == tgcli::kTimeout);
    CHECK(outcome.terminal_count == 1);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetChat) == 0);
}
