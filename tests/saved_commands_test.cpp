#include "common/exit_codes.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "daemon/saved_commands.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

class FakeSaved {
  public:
    explicit FakeSaved(tgcli::core::AuthState state = tgcli::core::AuthState::Ready) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        REQUIRE(runtime_->clients().size() == 1);
        td_client_ = runtime_->clients().front();
        runtime_->push_response(td_client_, 1, {}, tgcli::core::AuthStateData{state});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ =
            std::make_unique<tgcli::daemon::SavedCoordinator>(*client_, std::string("main"));
        tgcli::daemon::register_saved_commands(dispatcher_, *coordinator_);
    }

    FakeSaved(const FakeSaved&) = delete;
    FakeSaved& operator=(const FakeSaved&) = delete;
    FakeSaved(FakeSaved&&) = delete;
    FakeSaved& operator=(FakeSaved&&) = delete;
    ~FakeSaved() = default;

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

    void respond_user(bool bot = false) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == tgcli::core::TdFunctionKind::GetMe);
        runtime_->push_response(td_client_, sent.back().query_id,
                                tgcli::core::TdValue::from(tgcli::core::TdUserSummary{
                                    .id = 42,
                                    .first_name = "Ada",
                                    .last_name = "",
                                    .usernames = {},
                                    .phone_number = bot ? "" : "12025550123",
                                    .is_bot = bot,
                                    .is_premium = true}));
        ++sent_count_;
    }

    template <typename T> void respond_selected(T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        runtime_->push_response(td_client_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    void respond_selected_error(std::int32_t code, std::string message) {
        respond_selected(tgcli::core::TdError{code, std::move(message)});
    }

    void lose_authorization(tgcli::core::AuthState state = tgcli::core::AuthState::WaitCode) {
        runtime_->push_update(td_client_, {}, tgcli::core::AuthStateData{state});
    }

    [[nodiscard]] bool wait_for_selected() const {
        return runtime_->wait_for_sent(sent_count_ + 1);
    }

    [[nodiscard]] tgcli::core::TdFunctionData selected_function() const {
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() >= sent_count_ + 1);
        return sent.back().function;
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

    [[nodiscard]] tgcli::test::ScriptedTdRuntime& runtime() const {
        return *runtime_;
    }

    [[nodiscard]] tgcli::test::ScriptedClient td_client() const {
        return td_client_;
    }

    [[nodiscard]] bool wait_state_sequence(std::uint64_t sequence) const {
        return eventually([&] { return client_->auth_state()->auth_sequence >= sequence; });
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
    std::unique_ptr<tgcli::daemon::SavedCoordinator> coordinator_;
    tgcli::daemon::Dispatcher dispatcher_;
    std::size_t sent_count_ = 1;
};

tgcli::proto::Request tags_request(std::string account = "main") {
    tgcli::proto::Request request(std::move(account));
    request.id = 1;
    request.command = {"saved", "tags"};
    request.args = json::object();
    request.context.timeout_seconds = 1.0;
    request.context.cwd = "/";
    return request;
}

tgcli::proto::Request search_request(std::optional<std::string> query,
                                     std::optional<std::string> tag,
                                     std::optional<std::int32_t> limit = std::nullopt,
                                     std::optional<std::string> cursor = std::nullopt,
                                     std::string account = "main") {
    tgcli::proto::Request request(std::move(account));
    request.id = 1;
    request.command = {"saved", "search"};
    request.args = {
        {"query", query ? json(*query) : json(nullptr)},
        {"tag", tag ? json(*tag) : json(nullptr)},
        {"limit", limit ? json(*limit) : json(nullptr)},
        {"cursor", cursor ? json(*cursor) : json(nullptr)},
    };
    request.context.timeout_seconds = 1.0;
    request.context.cwd = "/";
    return request;
}

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

tgcli::core::TdReactionType emoji(std::string value) {
    return {.kind = tgcli::core::TdReactionKind::Emoji,
            .emoji = std::move(value),
            .custom_emoji_id = 0,
            .tdlib_type_id = -1942084920};
}

tgcli::core::TdReactionType custom(std::int64_t id) {
    return {.kind = tgcli::core::TdReactionKind::CustomEmoji,
            .emoji = {},
            .custom_emoji_id = id,
            .tdlib_type_id = -989117709};
}

tgcli::core::TdUserSummary saved_user() {
    return {.id = 42,
            .first_name = "Ada",
            .last_name = "",
            .usernames = {},
            .phone_number = "12025550123",
            .is_bot = false,
            .is_premium = true};
}

tgcli::proto::Request saved_request(tgcli::core::TdFunctionKind selected) {
    if (selected == tgcli::core::TdFunctionKind::GetSavedMessagesTags) {
        return tags_request();
    }
    return search_request(std::nullopt, "🧪");
}

void respond_saved_success(FakeSaved& fake, tgcli::core::TdFunctionKind selected) {
    const auto sent = fake.runtime().sent_functions();
    REQUIRE_FALSE(sent.empty());
    const auto query_id = sent.back().query_id;
    if (selected == tgcli::core::TdFunctionKind::GetSavedMessagesTags) {
        fake.runtime().push_response(
            fake.td_client(), query_id,
            tgcli::core::TdValue::from(tgcli::core::TdSavedMessagesTags{.tags = {}}));
        return;
    }
    fake.runtime().push_response(fake.td_client(), query_id,
                                 tgcli::core::TdValue::from(tgcli::core::TdFoundSavedMessages{
                                     .messages = {}, .next_from_message_id = 0}));
}

std::size_t sent_function_count(const FakeSaved& fake, tgcli::core::TdFunctionKind function) {
    return std::ranges::count_if(fake.runtime().sent_functions(), [&](const auto& sent) {
        return sent.function.kind() == function;
    });
}

} // namespace

TEST_CASE("Saved reaction selectors preserve exact UTF-8 and canonical custom ids",
          "[saved][selector]") {
    const std::vector<std::string> emoji_selectors{"🧪", "👍🏽", "👩🏽‍💻", "❤️", "✈️"};
    for (const auto& value : emoji_selectors) {
        const auto parsed = tgcli::daemon::parse_saved_reaction_selector(value);
        REQUIRE(parsed);
        CHECK(parsed->canonical == value);
        CHECK(parsed->reaction.kind == tgcli::core::TdReactionKind::Emoji);
        CHECK(parsed->reaction.emoji == value);
    }
    for (const auto& value : {"custom:1", "custom:9223372036854775807"}) {
        const auto parsed = tgcli::daemon::parse_saved_reaction_selector(value);
        REQUIRE(parsed);
        CHECK(parsed->canonical == value);
        CHECK(parsed->reaction.kind == tgcli::core::TdReactionKind::CustomEmoji);
    }

    std::vector<std::string> invalid{
        "",          "custom:",   "custom:0",  "custom:-1",
        "custom:+1", "custom:01", "custom:1x", "custom:9223372036854775808"};
    invalid.emplace_back("\xF0\x28\x8C\x28", 4);
    invalid.emplace_back("\xED\xA0\x80", 3);
    for (const auto& value : invalid) {
        INFO(value);
        CHECK_FALSE(tgcli::daemon::parse_saved_reaction_selector(value));
    }
}

TEST_CASE("Saved search cursors bind every continuation field strictly", "[saved][cursor]") {
    const tgcli::daemon::SavedSearchCursor original{
        .operation = "saved.search",
        .account = "main",
        .saved_messages_topic_id = 0,
        .tag = "👩🏽‍💻",
        .query = "idea 🧪",
        .limit = 100,
        .from_message_id = 900,
        .offset = 0,
    };
    const auto token = tgcli::daemon::encode_saved_search_cursor(original);
    CHECK(token.find("idea") == std::string::npos);
    const auto decoded = tgcli::daemon::decode_saved_search_cursor(token);
    REQUIRE(decoded);
    CHECK(decoded->operation == original.operation);
    CHECK(decoded->account == original.account);
    CHECK(decoded->saved_messages_topic_id == 0);
    CHECK(decoded->tag == original.tag);
    CHECK(decoded->query == original.query);
    CHECK(decoded->limit == original.limit);
    CHECK(decoded->from_message_id == original.from_message_id);
    CHECK(decoded->offset == original.offset);

    auto cross_operation = original;
    cross_operation.operation = "search";
    CHECK_FALSE(tgcli::daemon::decode_saved_search_cursor(
        tgcli::daemon::encode_saved_search_cursor(cross_operation)));
    auto cross_scope = original;
    cross_scope.saved_messages_topic_id = 1;
    CHECK_FALSE(tgcli::daemon::decode_saved_search_cursor(
        tgcli::daemon::encode_saved_search_cursor(cross_scope)));
    auto malformed = token;
    malformed.back() = malformed.back() == 'A' ? 'B' : 'A';
    CHECK_FALSE(tgcli::daemon::decode_saved_search_cursor(malformed));
    CHECK_FALSE(tgcli::daemon::decode_saved_search_cursor("not-a-cursor"));
}

TEST_CASE("saved tags preserves TD order and exact emoji/custom selectors",
          "[saved][dispatch][schema][fake-boundary]") {
    FakeSaved fake;
    auto pending = fake.dispatch(tags_request());
    fake.respond_user();
    CHECK(fake.wait_for_selected());
    const auto function = fake.selected_function();
    CHECK(function.kind() == tgcli::core::TdFunctionKind::GetSavedMessagesTags);
    CHECK(field_as<std::int64_t>(function, "saved_messages_topic_id") == 0);
    fake.respond_selected(tgcli::core::TdSavedMessagesTags{
        .tags = {{.tag = emoji("👩🏽‍💻"), .label = "ideas", .count = 7},
                 {.tag = emoji("❤️"), .label = "", .count = 7},
                 {.tag = custom(9223372036854775807LL), .label = "", .count = 2}}});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.exit_code == tgcli::kOk);
    CHECK(*outcome.result ==
          json{{"items",
                json::array(
                    {json{{"tag", "👩🏽‍💻"}, {"label", "ideas"}, {"count", 7}},
                     json{{"tag", "❤️"}, {"label", ""}, {"count", 7}},
                     json{{"tag", "custom:9223372036854775807"}, {"label", ""}, {"count", 2}}})},
               {"next", nullptr}});
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("saved-tags.result.schema.json"));
}

TEST_CASE("saved namespace preflight is Ready-only and rejects bots before selected reads",
          "[saved][dispatch][fake-boundary]") {
    SECTION("not authorized") {
        FakeSaved fake(tgcli::core::AuthState::WaitCode);
        const auto outcome = fake.dispatch(tags_request()).get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kNotAuthed);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"account", "main"}, {"state", "wait_code"}, {"reason", "not_ready"}});
        CHECK(fake.sent_count() == 1);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("saved.error.schema.json"));
    }
    SECTION("bot") {
        FakeSaved fake;
        auto pending = fake.dispatch(search_request(std::nullopt, "🧪"));
        fake.respond_user(true);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kUsage);
        CHECK((*outcome.error)["error"] ==
              json{{"code", "BOT_UNSUPPORTED"},
                   {"message", "saved commands require a user account"},
                   {"details", json::object()}});
        CHECK(fake.sent_count() == 2);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("saved.error.schema.json"));
    }
}

TEST_CASE("saved reads preserve response ordering across repeated Ready updates",
          "[saved][dispatch][fake-boundary][ordering]") {
    for (const auto selected : {tgcli::core::TdFunctionKind::GetSavedMessagesTags,
                                tgcli::core::TdFunctionKind::SearchSavedMessages}) {
        for (const bool response_first : {false, true}) {
            DYNAMIC_SECTION(tgcli::core::td_function_name(selected)
                            << (response_first ? " response then Ready" : " Ready then response")) {
                FakeSaved fake;
                auto pending = fake.dispatch(saved_request(selected));
                REQUIRE(fake.runtime().wait_for_sent(2));
                const auto get_me = fake.runtime().sent_functions().at(1);
                REQUIRE(get_me.function.kind() == tgcli::core::TdFunctionKind::GetMe);

                fake.runtime().set_receive_paused(true);
                if (response_first) {
                    fake.runtime().push_response(fake.td_client(), get_me.query_id,
                                                 tgcli::core::TdValue::from(saved_user()));
                    fake.runtime().push_update(
                        fake.td_client(), {},
                        tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
                } else {
                    fake.runtime().push_update(
                        fake.td_client(), {},
                        tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
                    fake.runtime().push_update(
                        fake.td_client(), {},
                        tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
                    fake.runtime().push_response(fake.td_client(), get_me.query_id,
                                                 tgcli::core::TdValue::from(saved_user()));
                }
                fake.runtime().set_receive_paused(false);
                REQUIRE(fake.wait_state_sequence(response_first ? 2 : 3));
                REQUIRE(fake.runtime().wait_for_sent(3));
                respond_saved_success(fake, selected);

                const auto outcome = pending.get();
                REQUIRE(outcome.result);
                CHECK(outcome.error == std::nullopt);
                CHECK(outcome.exit_code == tgcli::kOk);
                CHECK(outcome.terminal_count == 1);
                CHECK(sent_function_count(fake, tgcli::core::TdFunctionKind::GetMe) == 1);
                CHECK(sent_function_count(fake, selected) == 1);
            }
        }
    }
}

TEST_CASE("saved reads retain the first non-Ready state and skip the selected read",
          "[saved][dispatch][fake-boundary][ordering][authorization]") {
    for (const auto selected : {tgcli::core::TdFunctionKind::GetSavedMessagesTags,
                                tgcli::core::TdFunctionKind::SearchSavedMessages}) {
        DYNAMIC_SECTION(tgcli::core::td_function_name(selected)) {
            FakeSaved fake;
            auto pending = fake.dispatch(saved_request(selected));
            REQUIRE(fake.runtime().wait_for_sent(2));
            const auto get_me = fake.runtime().sent_functions().at(1);

            fake.runtime().set_receive_paused(true);
            fake.runtime().push_update(fake.td_client(), {},
                                       tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
            fake.runtime().push_update(
                fake.td_client(), {},
                tgcli::core::AuthStateData{tgcli::core::AuthState::WaitPhoneNumber});
            fake.runtime().push_update(fake.td_client(), {},
                                       tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
            fake.runtime().push_response(fake.td_client(), get_me.query_id,
                                         tgcli::core::TdValue::from(saved_user()));
            fake.runtime().set_receive_paused(false);
            REQUIRE(fake.wait_state_sequence(4));

            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
            CHECK((*outcome.error)["error"]["details"] == json{{"account", "main"},
                                                               {"state", "wait_phone_number"},
                                                               {"reason", "authorization_lost"}});
            CHECK(outcome.exit_code == tgcli::kNotAuthed);
            CHECK(outcome.terminal_count == 1);
            CHECK(sent_function_count(fake, selected) == 0);
        }
    }
}

TEST_CASE("saved reads retry typed stale admission on a newer Ready snapshot",
          "[saved][dispatch][fake-boundary][ordering][authorization]") {
    for (const auto selected : {tgcli::core::TdFunctionKind::GetSavedMessagesTags,
                                tgcli::core::TdFunctionKind::SearchSavedMessages}) {
        for (const auto stale_function : {tgcli::core::TdFunctionKind::GetMe, selected}) {
            DYNAMIC_SECTION(tgcli::core::td_function_name(selected)
                            << " stale " << tgcli::core::td_function_name(stale_function)) {
                FakeSaved fake;
                std::mutex barrier_mutex;
                std::condition_variable barrier_cv;
                bool blocked = false;
                bool release = false;
                std::size_t matching_makes = 0;
                fake.runtime().set_before_make([&](tgcli::core::TdFunctionKind function) {
                    if (function != stale_function || matching_makes++ != 0) {
                        return;
                    }
                    std::unique_lock lock(barrier_mutex);
                    blocked = true;
                    barrier_cv.notify_all();
                    static_cast<void>(barrier_cv.wait_for(lock, 2s, [&] { return release; }));
                });

                auto pending = fake.dispatch(saved_request(selected));
                if (stale_function != tgcli::core::TdFunctionKind::GetMe) {
                    fake.respond_user();
                }
                {
                    std::unique_lock lock(barrier_mutex);
                    REQUIRE(barrier_cv.wait_for(lock, 2s, [&] { return blocked; }));
                }
                fake.runtime().push_update(
                    fake.td_client(), {},
                    tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
                REQUIRE(fake.wait_state_sequence(2));
                {
                    const std::lock_guard lock(barrier_mutex);
                    release = true;
                }
                barrier_cv.notify_all();

                if (stale_function == tgcli::core::TdFunctionKind::GetMe) {
                    fake.respond_user();
                }
                REQUIRE(fake.wait_for_selected());
                respond_saved_success(fake, selected);

                const auto outcome = pending.get();
                REQUIRE(outcome.result);
                CHECK(outcome.error == std::nullopt);
                CHECK(outcome.exit_code == tgcli::kOk);
                CHECK(outcome.terminal_count == 1);
                CHECK(matching_makes == 2);
                CHECK(sent_function_count(fake, stale_function) == 1);
            }
        }
    }
}

TEST_CASE("saved reads reject pre-expired requests without a TD read",
          "[saved][dispatch][fake-boundary][timeout]") {
    for (const auto selected : {tgcli::core::TdFunctionKind::GetSavedMessagesTags,
                                tgcli::core::TdFunctionKind::SearchSavedMessages}) {
        DYNAMIC_SECTION(tgcli::core::td_function_name(selected)) {
            FakeSaved fake;
            const auto expired = tgcli::daemon::RequestSession::Clock::now() - 1ms;
            const auto outcome = fake.dispatch(saved_request(selected), expired).get();

            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
            CHECK((*outcome.error)["error"]["details"]["state"] == "ready");
            CHECK(outcome.exit_code == tgcli::kTimeout);
            CHECK(outcome.terminal_count == 1);
            CHECK(sent_function_count(fake, tgcli::core::TdFunctionKind::GetMe) == 0);
            CHECK(sent_function_count(fake, selected) == 0);
        }
    }
}

TEST_CASE("saved stale retries do not cross the absolute deadline",
          "[saved][dispatch][fake-boundary][ordering][authorization][timeout]") {
    for (const auto selected : {tgcli::core::TdFunctionKind::GetSavedMessagesTags,
                                tgcli::core::TdFunctionKind::SearchSavedMessages}) {
        for (const auto stale_function : {tgcli::core::TdFunctionKind::GetMe, selected}) {
            DYNAMIC_SECTION(tgcli::core::td_function_name(selected)
                            << " stale " << tgcli::core::td_function_name(stale_function)) {
                FakeSaved fake;
                std::mutex barrier_mutex;
                std::condition_variable barrier_cv;
                bool blocked = false;
                bool release = false;
                std::size_t matching_makes = 0;
                fake.runtime().set_before_make([&](tgcli::core::TdFunctionKind function) {
                    if (function != stale_function || matching_makes++ != 0) {
                        return;
                    }
                    std::unique_lock lock(barrier_mutex);
                    blocked = true;
                    barrier_cv.notify_all();
                    static_cast<void>(barrier_cv.wait_for(lock, 2s, [&] { return release; }));
                });

                const auto deadline = tgcli::daemon::RequestSession::Clock::now() + 250ms;
                auto pending = fake.dispatch(saved_request(selected), deadline);
                if (stale_function != tgcli::core::TdFunctionKind::GetMe) {
                    fake.respond_user();
                }
                {
                    std::unique_lock lock(barrier_mutex);
                    REQUIRE(barrier_cv.wait_for(lock, 2s, [&] { return blocked; }));
                }
                fake.runtime().push_update(
                    fake.td_client(), {},
                    tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
                REQUIRE(fake.wait_state_sequence(2));
                std::this_thread::sleep_until(deadline + 5ms);
                {
                    const std::lock_guard lock(barrier_mutex);
                    release = true;
                }
                barrier_cv.notify_all();

                const auto outcome = pending.get();
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
                CHECK((*outcome.error)["error"]["details"]["state"] == "ready");
                CHECK(outcome.exit_code == tgcli::kTimeout);
                CHECK(outcome.terminal_count == 1);
                CHECK(matching_makes == 1);
                CHECK(sent_function_count(fake, stale_function) == 0);
                CHECK(sent_function_count(fake, selected) == 0);
            }
        }
    }
}

TEST_CASE("saved Ready update floods retain one absolute deadline",
          "[saved][dispatch][fake-boundary][ordering][timeout]") {
    for (const auto selected : {tgcli::core::TdFunctionKind::GetSavedMessagesTags,
                                tgcli::core::TdFunctionKind::SearchSavedMessages}) {
        DYNAMIC_SECTION(tgcli::core::td_function_name(selected)) {
            FakeSaved fake;
            auto request = saved_request(selected);
            request.context.timeout_seconds = 0.05;
            auto pending = fake.dispatch(std::move(request));
            REQUIRE(fake.runtime().wait_for_sent(2));
            for (std::size_t index = 0; index < 64; ++index) {
                fake.runtime().push_update(
                    fake.td_client(), {},
                    tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
            }
            REQUIRE(fake.wait_state_sequence(65));

            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
            CHECK((*outcome.error)["error"]["details"]["state"] == "ready");
            CHECK(outcome.exit_code == tgcli::kTimeout);
            CHECK(outcome.terminal_count == 1);
            CHECK(sent_function_count(fake, selected) == 0);
        }
    }
}

TEST_CASE("saved search binds tag text page size and complete TD continuation",
          "[saved][dispatch][schema][fake-boundary]") {
    FakeSaved fake;
    auto first = fake.dispatch(search_request("experiment", "👩🏽‍💻", 100));
    fake.respond_user();
    CHECK(fake.wait_for_selected());
    const auto first_function = fake.selected_function();
    CHECK(first_function.kind() == tgcli::core::TdFunctionKind::SearchSavedMessages);
    CHECK(field_as<std::int64_t>(first_function, "saved_messages_topic_id") == 0);
    CHECK(field_as<std::string>(first_function, "tag") == "👩🏽‍💻");
    CHECK(field_as<std::string>(first_function, "query") == "experiment");
    CHECK(field_as<std::int64_t>(first_function, "from_message_id") == 0);
    CHECK(field_as<std::int64_t>(first_function, "offset") == 0);
    CHECK(field_as<std::int64_t>(first_function, "limit") == 100);
    fake.respond_selected(tgcli::core::TdFoundSavedMessages{
        .messages = {{.id = 199, .chat_id = 42, .date = 1782993540, .text = ""},
                     {.id = 200, .chat_id = 42, .date = 1782993600, .text = "experiment 🧪"}},
        .next_from_message_id = 150});
    const auto first_outcome = first.get();
    REQUIRE(first_outcome.result);
    CHECK((*first_outcome.result)["items"][0]["id"] == 200);
    CHECK((*first_outcome.result)["items"][1]["id"] == 199);
    CHECK((*first_outcome.result)["items"][1]["text"].get<std::string>().empty());
    CHECK_THAT(*first_outcome.result,
               tgcli::test::matches_json_schema("saved-search.result.schema.json"));
    const auto cursor = (*first_outcome.result)["next"].get<std::string>();

    auto continuation =
        fake.dispatch(search_request(std::nullopt, std::nullopt, std::nullopt, cursor));
    fake.respond_user();
    CHECK(fake.wait_for_selected());
    const auto continuation_function = fake.selected_function();
    CHECK(field_as<std::int64_t>(continuation_function, "saved_messages_topic_id") == 0);
    CHECK(field_as<std::string>(continuation_function, "tag") == "👩🏽‍💻");
    CHECK(field_as<std::string>(continuation_function, "query") == "experiment");
    CHECK(field_as<std::int64_t>(continuation_function, "from_message_id") == 150);
    CHECK(field_as<std::int64_t>(continuation_function, "offset") == 0);
    CHECK(field_as<std::int64_t>(continuation_function, "limit") == 100);
    fake.respond_selected(
        tgcli::core::TdFoundSavedMessages{.messages = {}, .next_from_message_id = 0});
    const auto continuation_outcome = continuation.get();
    REQUIRE(continuation_outcome.result);
    CHECK(*continuation_outcome.result == json{{"items", json::array()}, {"next", nullptr}});
}

TEST_CASE("saved search defaults to 20 and unused exact tags succeed empty",
          "[saved][dispatch][fake-boundary]") {
    FakeSaved fake;
    auto pending = fake.dispatch(search_request("emoji 🧪 appears only in text", "❤️"));
    fake.respond_user();
    CHECK(fake.wait_for_selected());
    const auto function = fake.selected_function();
    CHECK(field_as<std::string>(function, "tag") == "❤️");
    CHECK(field_as<std::string>(function, "query") == "emoji 🧪 appears only in text");
    CHECK(field_as<std::int64_t>(function, "limit") == 20);
    fake.respond_selected(
        tgcli::core::TdFoundSavedMessages{.messages = {}, .next_from_message_id = 0});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(*outcome.result == json{{"items", json::array()}, {"next", nullptr}});

    auto custom_pending = fake.dispatch(search_request(std::nullopt, "custom:9223372036854775807"));
    fake.respond_user();
    CHECK(fake.wait_for_selected());
    const auto custom_function = fake.selected_function();
    CHECK(field_as<std::string>(custom_function, "tag") == "custom:9223372036854775807");
    fake.respond_selected(
        tgcli::core::TdFoundSavedMessages{.messages = {}, .next_from_message_id = 0});
    CHECK(custom_pending.get().result.has_value());
}

TEST_CASE("saved argument and cursor mismatches fail before Telegram preflight",
          "[saved][dispatch][cursor][fake-boundary]") {
    const tgcli::daemon::SavedSearchCursor bound{.operation = "saved.search",
                                                 .account = "main",
                                                 .saved_messages_topic_id = 0,
                                                 .tag = "🧪",
                                                 .query = "idea",
                                                 .limit = 20,
                                                 .from_message_id = 100,
                                                 .offset = 0};
    const auto cursor = tgcli::daemon::encode_saved_search_cursor(bound);
    const std::vector<tgcli::proto::Request> invalid{
        search_request("different", std::nullopt, std::nullopt, cursor),
        search_request(std::nullopt, "❤️", std::nullopt, cursor),
        search_request(std::nullopt, std::nullopt, 20, cursor),
        search_request(std::nullopt, std::nullopt, std::nullopt, "bad"),
        search_request(std::nullopt, std::nullopt),
        search_request(std::nullopt, "custom:01"),
    };
    for (const auto& request : invalid) {
        FakeSaved fake;
        const auto outcome = fake.dispatch(request).get();
        REQUIRE(outcome.error);
        INFO(outcome.error->dump());
        CHECK(outcome.exit_code == tgcli::kUsage);
        CHECK((*outcome.error)["error"]["code"] == "USAGE");
        CHECK(fake.sent_count() == 1);
    }

    FakeSaved cross_account;
    auto cross = bound;
    cross.account = "work";
    const auto cross_outcome =
        cross_account
            .dispatch(search_request(std::nullopt, std::nullopt, std::nullopt,
                                     tgcli::daemon::encode_saved_search_cursor(cross)))
            .get();
    REQUIRE(cross_outcome.error);
    CHECK((*cross_outcome.error)["error"]["code"] == "USAGE");
    CHECK(cross_account.sent_count() == 1);

    FakeSaved tags_cursor;
    auto tags = tags_request();
    tags.args = {{"cursor", cursor}};
    const auto tags_outcome = tags_cursor.dispatch(std::move(tags)).get();
    REQUIRE(tags_outcome.error);
    CHECK((*tags_outcome.error)["error"]["code"] == "USAGE");
    CHECK(tags_cursor.sent_count() == 1);

    FakeSaved oversized_limit;
    auto oversized_request = search_request(std::nullopt, "🧪");
    oversized_request.args["limit"] = std::numeric_limits<std::uint64_t>::max();
    const auto oversized_outcome = oversized_limit.dispatch(std::move(oversized_request)).get();
    REQUIRE(oversized_outcome.error);
    CHECK((*oversized_outcome.error)["error"]["code"] == "USAGE");
    CHECK(oversized_limit.sent_count() == 1);
}

TEST_CASE("saved tags rejects paid unknown invalid and non-positive returned variants",
          "[saved][dispatch][fake-boundary]") {
    const std::vector<tgcli::core::TdReactionType> invalid{
        {.kind = tgcli::core::TdReactionKind::Paid,
         .emoji = {},
         .custom_emoji_id = 0,
         .tdlib_type_id = 436294381},
        {.kind = tgcli::core::TdReactionKind::Unknown,
         .emoji = {},
         .custom_emoji_id = 0,
         .tdlib_type_id = 700000004},
        {.kind = tgcli::core::TdReactionKind::Unknown,
         .emoji = {},
         .custom_emoji_id = 0,
         .tdlib_type_id = 0},
        {.kind = tgcli::core::TdReactionKind::CustomEmoji,
         .emoji = {},
         .custom_emoji_id = 0,
         .tdlib_type_id = -989117709},
        {.kind = tgcli::core::TdReactionKind::Emoji,
         .emoji = std::string("\xF0\x28\x8C\x28", 4),
         .custom_emoji_id = 0,
         .tdlib_type_id = -1942084920},
    };
    for (const auto& reaction : invalid) {
        FakeSaved fake;
        auto pending = fake.dispatch(tags_request());
        fake.respond_user();
        fake.respond_selected(
            tgcli::core::TdSavedMessagesTags{.tags = {{.tag = reaction, .label = "", .count = 1}}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        INFO(outcome.error->dump());
        CHECK(outcome.exit_code == tgcli::kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("saved.error.schema.json"));
    }

    const std::vector<tgcli::core::TdSavedMessagesTag> invalid_metadata{
        {.tag = emoji("🧪"), .label = std::string("\xED\xA0\x80", 3), .count = 1},
        {.tag = emoji("🧪"), .label = "", .count = -1},
        {.tag = custom(123), .label = std::string("\xED\xA0\x80", 3), .count = 1},
        {.tag = custom(123), .label = "", .count = -1},
    };
    for (const auto& tag : invalid_metadata) {
        FakeSaved fake;
        auto pending = fake.dispatch(tags_request());
        fake.respond_user();
        fake.respond_selected(tgcli::core::TdSavedMessagesTags{.tags = {tag}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK_FALSE((*outcome.error)["error"]["details"].contains("custom_emoji_id"));
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("saved.error.schema.json"));
    }
}

TEST_CASE("saved search rejects malformed TD results without partial output",
          "[saved][dispatch][fake-boundary]") {
    const std::vector<tgcli::core::TdFoundSavedMessages> invalid{
        {.messages = {{.id = 0, .chat_id = 42, .date = 1782993600, .text = "bad id"}},
         .next_from_message_id = 0},
        {.messages = {{.id = 1,
                       .chat_id = 42,
                       .date = 1782993600,
                       .text = std::string("\xF0\x28\x8C\x28", 4)}},
         .next_from_message_id = 0},
        {.messages = {}, .next_from_message_id = -1},
    };
    for (const auto& response : invalid) {
        FakeSaved fake;
        auto pending = fake.dispatch(search_request(std::nullopt, "🧪"));
        fake.respond_user();
        fake.respond_selected(response);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kGeneric);
        CHECK((*outcome.error)["error"] ==
              json{{"code", "INTERNAL"},
                   {"message", "Saved Messages request returned an unexpected object"},
                   {"details", {{"operation", "saved_search"}, {"reason", "internal_error"}}}});
        CHECK_FALSE(outcome.result);
    }
}

TEST_CASE("saved maps Premium TD errors rate limits and deadlines exactly",
          "[saved][dispatch][fake-boundary]") {
    SECTION("Premium failure") {
        FakeSaved fake;
        auto pending = fake.dispatch(search_request(std::nullopt, "🧪"));
        fake.respond_user();
        fake.respond_selected_error(400, "This method is available to Telegram Premium users only");
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "saved_search"}, {"tdlib_code", 400}});
        CHECK(outcome.error->dump().find("Premium users") == std::string::npos);
    }
    SECTION("rate limit") {
        FakeSaved fake;
        auto pending = fake.dispatch(tags_request());
        fake.respond_user();
        fake.respond_selected_error(429, "FLOOD_WAIT_17");
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kRateLimited);
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "saved_tags"}, {"tdlib_code", 429}, {"retry_after", 17}});
    }
    SECTION("timeout") {
        FakeSaved fake;
        auto request = search_request(std::nullopt, "🧪");
        request.context.timeout_seconds = 0.02;
        auto pending = fake.dispatch(std::move(request));
        fake.respond_user();
        REQUIRE(fake.wait_for_selected());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kTimeout);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "saved_search"}, {"state", "ready"}});
    }
}

TEST_CASE("saved query completion is ordered against authorization loss events",
          "[saved][dispatch][fake-boundary][concurrency]") {
    SECTION("authorization loss first") {
        FakeSaved fake;
        auto pending = fake.dispatch(tags_request());
        fake.respond_user();
        REQUIRE(fake.wait_for_selected());
        fake.runtime().set_receive_paused(true);
        fake.lose_authorization();
        fake.respond_selected(tgcli::core::TdSavedMessagesTags{.tags = {}});
        fake.runtime().set_receive_paused(false);
        REQUIRE(fake.wait_state_sequence(2));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kNotAuthed);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "authorization_lost");
        CHECK(outcome.terminal_count == 1);
    }
    SECTION("response first") {
        FakeSaved fake;
        auto pending = fake.dispatch(tags_request());
        fake.respond_user();
        REQUIRE(fake.wait_for_selected());
        fake.runtime().set_receive_paused(true);
        fake.respond_selected(tgcli::core::TdSavedMessagesTags{.tags = {}});
        fake.lose_authorization();
        fake.runtime().set_receive_paused(false);
        REQUIRE(fake.wait_state_sequence(2));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"items", json::array()}, {"next", nullptr}});
        CHECK(outcome.terminal_count == 1);
    }
}
