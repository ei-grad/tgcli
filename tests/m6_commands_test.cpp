#include "common/exit_codes.hpp"
#include "daemon/m6_commands.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

using nlohmann::json;
namespace core = tgcli::core;
namespace daemon = tgcli::daemon;
namespace proto = tgcli::proto;

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    int exit_code = -1;
    int terminal_count = 0;
};

class FakeM6 {
  public:
    FakeM6() {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {}, core::AuthStateData{core::AuthState::Ready});
        REQUIRE(eventually(
            [&] { return client_->auth_state()->data.state == core::AuthState::Ready; }));
        coordinator_ = std::make_unique<daemon::M6Coordinator>(*client_, "main");
    }

    template <typename Call> std::future<Outcome> run(proto::Request request, Call call) {
        return std::async(std::launch::async, [this, request = std::move(request),
                                               call = std::move(call)]() mutable {
            Outcome outcome;
            daemon::CallbackSink sink(
                [](const json&) {}, [](const json&) {},
                [&](json value) {
                    ++outcome.terminal_count;
                    outcome.result = std::move(value);
                    outcome.exit_code = tgcli::kOk;
                },
                [&](std::string code, std::string message, json details, int exit_code) {
                    ++outcome.terminal_count;
                    outcome.error = json{{"code", std::move(code)},
                                         {"message", std::move(message)},
                                         {"details", std::move(details)}};
                    outcome.exit_code = exit_code;
                });
            daemon::RequestSession session(std::move(request), sink);
            call(*coordinator_, session);
            return outcome;
        });
    }

    template <typename T> void respond(core::TdFunctionKind expected, T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        runtime_->push_response(client_id_, sent.back().query_id,
                                core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    void respond_me(bool bot = false) {
        respond(core::TdFunctionKind::GetMe,
                core::TdUserSummary{.id = 42,
                                    .first_name = "Ada",
                                    .last_name = {},
                                    .usernames = {"ada"},
                                    .phone_number = bot ? "" : "12025550123",
                                    .is_bot = bot,
                                    .is_premium = false});
    }

    void publish_folders(core::TdM6ChatFoldersUpdate update) {
        runtime_->push_update(client_id_, core::TdValue::from(update));
        REQUIRE(
            eventually([&] { return client_->m6_chat_folders(client_->auth_state()) == update; }));
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
    std::unique_ptr<core::TdClient> client_;
    std::unique_ptr<daemon::M6Coordinator> coordinator_;
    std::size_t sent_count_ = 1;
};

proto::Request request(std::vector<std::string> command, json args = json::object()) {
    proto::Request value("main");
    value.command = std::move(command);
    value.args = std::move(args);
    value.context.timeout_seconds = 1.0;
    value.context.cwd = "/";
    return value;
}

TEST_CASE("M6 contact list hydrates every id under one bound resolver", "[m6][commands]") {
    FakeM6 fake;
    auto pending = fake.run(request({"contact", "list"}), [](daemon::M6Coordinator& coordinator,
                                                             daemon::RequestSession& session) {
        coordinator.contact(proto::M6Operation::ContactList, session.request(), session);
    });
    fake.respond_me();
    fake.respond(core::TdFunctionKind::GetContacts,
                 core::TdM6Response{core::TdM6Users{.total_count = 2, .user_ids = {7, 8}}});
    fake.respond(core::TdFunctionKind::GetUser, core::TdUserSummary{.id = 7,
                                                                    .first_name = "Grace",
                                                                    .last_name = "Hopper",
                                                                    .usernames = {"grace"},
                                                                    .phone_number = {},
                                                                    .is_bot = false,
                                                                    .is_premium = false});
    fake.respond(core::TdFunctionKind::GetUser, core::TdUserSummary{.id = 8,
                                                                    .first_name = "Alan",
                                                                    .last_name = "Turing",
                                                                    .usernames = {"alan"},
                                                                    .phone_number = {},
                                                                    .is_bot = false,
                                                                    .is_premium = false});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["id"] == 7);
    CHECK((*outcome.result)["items"][1]["display_name"] == "Alan Turing");
    CHECK((*outcome.result)["next"].is_null());
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("M6 folder list uses the exact generation cache without a TD read", "[m6][commands]") {
    FakeM6 fake;
    fake.publish_folders({.folders = {{.id = 7,
                                       .name = {.text = "Work",
                                                .animate_custom_emoji = false,
                                                .custom_emoji_entities = {}},
                                       .icon = core::TdM6FolderIcon::Work,
                                       .color_id = 3,
                                       .is_shareable = false,
                                       .has_my_invite_links = true}},
                          .main_chat_list_position = 1,
                          .are_tags_enabled = true});
    auto pending = fake.run(request({"folder", "list"}), [](daemon::M6Coordinator& coordinator,
                                                            daemon::RequestSession& session) {
        coordinator.folder_list(session.request(), session);
    });
    fake.respond_me();
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["id"] == 7);
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("M6 storage stats validates the complete returned tree", "[m6][commands]") {
    FakeM6 fake;
    auto pending = fake.run(request({"storage", "stats"}), [](daemon::M6Coordinator& coordinator,
                                                              daemon::RequestSession& session) {
        coordinator.storage_stats(session.request(), session);
    });
    fake.respond_me();
    fake.respond(core::TdFunctionKind::GetStorageStatistics,
                 core::TdM6Response{core::TdM6StorageStatistics{
                     .size = 5,
                     .count = 1,
                     .by_chat = {{.chat_id = -1001,
                                  .size = 5,
                                  .count = 1,
                                  .by_file_type = {{.file_type = core::TdM6StorageFileType::Photo,
                                                    .size = 5,
                                                    .count = 1}}}}}});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["size"] == 5);
    CHECK((*outcome.result)["by_chat"][0]["by_file_type"][0]["file_type"] == "photo");
}

TEST_CASE("M6 session list preserves the typed session order", "[m6][commands]") {
    FakeM6 fake;
    auto pending = fake.run(request({"session", "list"}), [](daemon::M6Coordinator& coordinator,
                                                             daemon::RequestSession& session) {
        coordinator.session_list(session.request(), session);
    });
    fake.respond_me();
    fake.respond(core::TdFunctionKind::GetActiveSessions,
                 core::TdSessions{.items = {{.id = "7",
                                             .is_current = true,
                                             .device_type = core::TdSessionDeviceType::Linux,
                                             .application_name = "tgcli",
                                             .application_version = "1",
                                             .device_model = "PC",
                                             .platform = "Linux",
                                             .system_version = "1",
                                             .log_in_date = std::nullopt,
                                             .last_active_date = std::nullopt,
                                             .ip_address = {},
                                             .location = {}}},
                                  .inactive_session_ttl_days = 30});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["id"] == "7");
    CHECK((*outcome.result)["inactive_session_ttl_days"] == 30);
    CHECK((*outcome.result)["next"].is_null());
}

} // namespace
