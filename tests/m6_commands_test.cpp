#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/m6_commands.hpp"
#include "daemon/m6_model.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

using nlohmann::json;
namespace core = tgcli::core;
namespace daemon = tgcli::daemon;
namespace daemon_lock = tgcli::daemon_lock;
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
        std::string pattern = "/tmp/tgcli-m6-commands-XXXXXX";
        pattern.push_back('\0');
        const auto* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        state_ = root_ + "/state/tgcli/accounts/main";
        for (const auto& directory :
             {root_ + "/state", root_ + "/state/tgcli", root_ + "/state/tgcli/accounts", state_}) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
        std::string lock_error;
        lease_ = daemon_lock::acquire_lifetime(state_ + "/daemon.lock", lock_identity_, lock_error);
        INFO(lock_error);
        REQUIRE(lease_);
        auto created_foundation =
            daemon::IdempotencyFoundation::create(state_, "main", ::getuid(), lease_);
        REQUIRE(std::holds_alternative<daemon::IdempotencyFoundation>(created_foundation));
        foundation_ = std::make_shared<daemon::IdempotencyFoundation>(
            std::get<daemon::IdempotencyFoundation>(std::move(created_foundation)));
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {}, core::AuthStateData{core::AuthState::Ready});
        REQUIRE(eventually(
            [&] { return client_->auth_state()->data.state == core::AuthState::Ready; }));
        coordinator_ = std::make_unique<daemon::M6Coordinator>(*client_, "main", foundation_);
    }

    ~FakeM6() {
        coordinator_.reset();
        client_.reset();
        foundation_.reset();
        lease_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    FakeM6(const FakeM6&) = delete;
    FakeM6& operator=(const FakeM6&) = delete;
    FakeM6(FakeM6&&) = delete;
    FakeM6& operator=(FakeM6&&) = delete;

    template <typename Call>
    std::future<Outcome> run(proto::Request request, Call call,
                             std::shared_ptr<daemon::RequestSession>* exposed_session = nullptr) {
        auto outcome = std::make_shared<Outcome>();
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [outcome](json value) {
                ++outcome->terminal_count;
                outcome->result = std::move(value);
                outcome->exit_code = tgcli::kOk;
            },
            [outcome](std::string code, std::string message, json details, int exit_code) {
                ++outcome->terminal_count;
                outcome->error = json{{"code", std::move(code)},
                                      {"message", std::move(message)},
                                      {"details", std::move(details)}};
                outcome->exit_code = exit_code;
            });
        auto session =
            std::make_shared<daemon::RequestSession>(std::move(request), std::move(sink));
        if (exposed_session != nullptr) {
            *exposed_session = session;
        }
        return std::async(std::launch::async,
                          [this, call = std::move(call), outcome, session]() mutable {
                              call(*coordinator_, *session);
                              return *outcome;
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

    void observe(core::TdFunctionKind expected) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        ++sent_count_;
    }

    void push_authorization(core::AuthStateData state) {
        runtime_->push_update(client_id_, {}, std::move(state));
    }

    void fail_before_send(core::TdFunctionKind selected) {
        runtime_->set_before_send([selected](const core::TdFunctionData& function) {
            if (function.kind() == selected) {
                throw std::runtime_error("scripted M6 read submission failure");
            }
        });
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

    void replace_generation_with_folders(core::TdM6ChatFoldersUpdate update) {
        const auto original = client_->auth_state();
        REQUIRE(original);
        const auto sent_before = runtime_->sent_functions_including_current_state().size();
        runtime_->push_update(client_id_, {}, core::AuthStateData{core::AuthState::Closed});
        REQUIRE(runtime_->wait_for_clients(2));
        REQUIRE(runtime_->wait_for_sent_including_current_state(sent_before + 2));
        const auto replacement = runtime_->clients().back();
        core::TdCurrentState state;
        state.updates.push_back(core::TdValue::from(std::move(update)));
        runtime_->push_response(replacement, 2, core::TdValue::from(std::move(state)));
        runtime_->push_response(replacement, 1, {}, core::AuthStateData{core::AuthState::Ready});
        REQUIRE(eventually([&] {
            const auto current = client_->auth_state();
            return current->client_generation == replacement.client_generation &&
                   current->data.state == core::AuthState::Ready;
        }));
        client_id_ = replacement;
        ++sent_count_;
    }

    void make_spool_wrong_mode() const {
        const auto spool = state_ + "/spool";
        REQUIRE(std::filesystem::create_directory(spool));
        REQUIRE(::chmod(spool.c_str(), 0755) == 0);
    }

    [[nodiscard]] std::size_t count(core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    [[nodiscard]] core::TdClient& client() const {
        return *client_;
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
    std::string root_;
    std::string state_;
    daemon_lock::Identity lock_identity_;
    std::shared_ptr<daemon_lock::LifetimeLease> lease_;
    std::shared_ptr<daemon::IdempotencyFoundation> foundation_;
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
    fake.respond(core::TdFunctionKind::GetM6Contacts,
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

TEST_CASE("M6 contact search binds the exact query and 100-result response bound",
          "[m6][commands][contact]") {
    FakeM6 fake;
    auto pending = fake.run(
        request({"contact", "search"}, {{"query", "ada"}}),
        [](daemon::M6Coordinator& coordinator, daemon::RequestSession& session) {
            coordinator.contact(proto::M6Operation::ContactSearch, session.request(), session);
        });
    fake.respond_me();
    fake.respond(core::TdFunctionKind::SearchContacts,
                 core::TdM6Response{core::TdM6Users{.total_count = 1, .user_ids = {7}}});
    fake.respond(core::TdFunctionKind::GetUser, core::TdUserSummary{.id = 7,
                                                                    .first_name = "Ada",
                                                                    .last_name = "Lovelace",
                                                                    .usernames = {"ada"},
                                                                    .phone_number = {},
                                                                    .is_bot = false,
                                                                    .is_premium = false});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["display_name"] == "Ada Lovelace");
    CHECK(fake.count(core::TdFunctionKind::SearchContacts) == 1);
}

TEST_CASE("M6 contact bounds and structural IDs are decided before hydration",
          "[m6][commands][contact][capacity]") {
    SECTION("invalid ID in a later slot prevents every hydration") {
        FakeM6 fake;
        auto pending = fake.run(request({"contact", "list"}), [](daemon::M6Coordinator& coordinator,
                                                                 daemon::RequestSession& session) {
            coordinator.contact(proto::M6Operation::ContactList, session.request(), session);
        });
        fake.respond_me();
        fake.respond(core::TdFunctionKind::GetM6Contacts,
                     core::TdM6Response{core::TdM6Users{.total_count = 2, .user_ids = {7, 0}}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["code"] == "INTERNAL");
        CHECK(fake.count(core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("list item count cap plus one reports its exact resource") {
        FakeM6 fake;
        auto pending = fake.run(request({"contact", "list"}), [](daemon::M6Coordinator& coordinator,
                                                                 daemon::RequestSession& session) {
            coordinator.contact(proto::M6Operation::ContactList, session.request(), session);
        });
        fake.respond_me();
        std::vector<std::int64_t> ids(131'073);
        std::iota(ids.begin(), ids.end(), std::int64_t{1});
        fake.respond(
            core::TdFunctionKind::GetM6Contacts,
            core::TdM6Response{core::TdM6Users{.total_count = static_cast<std::int32_t>(ids.size()),
                                               .user_ids = std::move(ids)}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["details"] == json{{"operation", "contact_list"},
                                                  {"reason", "capacity_exhausted"},
                                                  {"resource", "users"},
                                                  {"limit", 131'072}});
        CHECK(fake.count(core::TdFunctionKind::GetUser) == 0);
    }

    SECTION("one serialized identity fits exactly and plus one is item capacity") {
        const auto sized_user = [](std::size_t bytes) {
            core::TdUserSummary user{.id = 7,
                                     .first_name = {},
                                     .last_name = {},
                                     .usernames = {},
                                     .phone_number = {},
                                     .is_bot = false,
                                     .is_premium = false};
            const auto base = daemon::m6_user_identity_json(daemon::UserIdentity{.id = 7,
                                                                                 .display_name = {},
                                                                                 .usernames = {},
                                                                                 .is_bot = false})
                                  .dump()
                                  .size();
            REQUIRE(bytes >= base);
            user.first_name.assign(bytes - base, 'a');
            REQUIRE(daemon::m6_user_identity_json(*daemon::m6_user_identity(user)).dump().size() ==
                    bytes);
            return user;
        };
        for (const auto bytes : {std::size_t{262'144}, std::size_t{262'145}}) {
            CAPTURE(bytes);
            FakeM6 fake;
            auto pending =
                fake.run(request({"contact", "search"}, {{"query", "a"}}),
                         [](daemon::M6Coordinator& coordinator, daemon::RequestSession& session) {
                             coordinator.contact(proto::M6Operation::ContactSearch,
                                                 session.request(), session);
                         });
            fake.respond_me();
            fake.respond(core::TdFunctionKind::SearchContacts,
                         core::TdM6Response{core::TdM6Users{.total_count = 1, .user_ids = {7}}});
            fake.respond(core::TdFunctionKind::GetUser, sized_user(bytes));
            const auto outcome = pending.get();
            if (bytes == 262'144) {
                REQUIRE(outcome.result);
            } else {
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["details"] == json{{"operation", "contact_search"},
                                                          {"reason", "capacity_exhausted"},
                                                          {"resource", "item_bytes"},
                                                          {"limit", 262'144}});
            }
        }
    }

    SECTION("aggregate serialized identities fit exactly and the next item is byte capacity") {
        const auto sized_user = [](std::int64_t id, std::size_t bytes) {
            core::TdUserSummary user{.id = id,
                                     .first_name = {},
                                     .last_name = {},
                                     .usernames = {},
                                     .phone_number = {},
                                     .is_bot = false,
                                     .is_premium = false};
            const auto base = daemon::m6_user_identity_json(daemon::UserIdentity{.id = id,
                                                                                 .display_name = {},
                                                                                 .usernames = {},
                                                                                 .is_bot = false})
                                  .dump()
                                  .size();
            REQUIRE(bytes >= base);
            user.first_name.assign(bytes - base, 'b');
            REQUIRE(daemon::m6_user_identity_json(*daemon::m6_user_identity(user)).dump().size() ==
                    bytes);
            return user;
        };
        for (const bool overflow : {false, true}) {
            CAPTURE(overflow);
            FakeM6 fake;
            auto input = request({"contact", "list"});
            input.context.timeout_seconds = 60.0;
            auto pending = fake.run(std::move(input), [](daemon::M6Coordinator& coordinator,
                                                         daemon::RequestSession& session) {
                coordinator.contact(proto::M6Operation::ContactList, session.request(), session);
            });
            fake.respond_me();
            std::vector<std::int64_t> ids(overflow ? 65 : 64);
            std::iota(ids.begin(), ids.end(), std::int64_t{1});
            fake.respond(
                core::TdFunctionKind::GetM6Contacts,
                core::TdM6Response{core::TdM6Users{
                    .total_count = static_cast<std::int32_t>(ids.size()), .user_ids = ids}});
            for (std::size_t index = 0; index < 64; ++index) {
                fake.respond(core::TdFunctionKind::GetUser, sized_user(ids[index], 262'144));
            }
            if (overflow) {
                fake.respond(core::TdFunctionKind::GetUser, sized_user(ids.back(), 64));
            }
            const auto outcome = pending.get();
            if (!overflow) {
                REQUIRE(outcome.result);
                CHECK((*outcome.result)["items"].size() == 64);
            } else {
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["details"] == json{{"operation", "contact_list"},
                                                          {"reason", "capacity_exhausted"},
                                                          {"resource", "bytes"},
                                                          {"limit", 16'777'216}});
            }
        }
    }
}

TEST_CASE("M6 contact hydration maps every lifecycle stop exactly",
          "[m6][commands][contact][lifecycle]") {
    enum class Stop { Authorization, Deadline, Cancel, Shutdown, Failed };
    for (const auto stop :
         {Stop::Authorization, Stop::Deadline, Stop::Cancel, Stop::Shutdown, Stop::Failed}) {
        CAPTURE(static_cast<int>(stop));
        FakeM6 fake;
        if (stop == Stop::Failed) {
            fake.fail_before_send(core::TdFunctionKind::GetUser);
        }
        auto input = request({"contact", "list"});
        if (stop == Stop::Deadline) {
            input.context.timeout_seconds = 0.5;
        }
        std::shared_ptr<daemon::RequestSession> session;
        auto pending = fake.run(
            std::move(input),
            [](daemon::M6Coordinator& coordinator, daemon::RequestSession& current) {
                coordinator.contact(proto::M6Operation::ContactList, current.request(), current);
            },
            &session);
        fake.respond_me();
        fake.respond(core::TdFunctionKind::GetM6Contacts,
                     core::TdM6Response{core::TdM6Users{.total_count = 1, .user_ids = {7}}});
        if (stop != Stop::Failed) {
            fake.observe(core::TdFunctionKind::GetUser);
        }
        if (stop == Stop::Authorization) {
            fake.push_authorization(core::AuthStateData{core::AuthState::LoggingOut});
        } else if (stop == Stop::Cancel) {
            session->disconnect();
        } else if (stop == Stop::Shutdown) {
            session->shutdown();
        }
        const auto outcome = pending.get();
        if (stop == Stop::Cancel) {
            CHECK(outcome.terminal_count == 0);
            continue;
        }
        REQUIRE(outcome.error);
        std::string_view code = "INTERNAL";
        if (stop == Stop::Authorization) {
            code = "NOT_AUTHED";
            CHECK((*outcome.error)["details"]["state"] == "logging_out");
        } else if (stop == Stop::Deadline) {
            code = "TIMEOUT";
        } else if (stop == Stop::Shutdown) {
            code = "DAEMON_SHUTDOWN";
        }
        CHECK((*outcome.error)["code"] == code);
        CHECK(outcome.terminal_count == 1);
    }
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

TEST_CASE("M6 folder cache cannot beat deadline equality", "[m6][commands][folder][deadline]") {
    FakeM6 fake;
    fake.publish_folders(
        {.folders = {
             {.id = 7,
              .name = {.text = "Work", .animate_custom_emoji = false, .custom_emoji_entities = {}},
              .icon = core::TdM6FolderIcon::Work,
              .color_id = 3,
              .is_shareable = false,
              .has_my_invite_links = false}}});
    Outcome outcome;
    daemon::CallbackSink sink(
        [](const json&) {}, [](const json&) {},
        [&](json value) {
            ++outcome.terminal_count;
            outcome.result = std::move(value);
        },
        [&](std::string code, std::string message, json details, int exit_code) {
            ++outcome.terminal_count;
            outcome.error = json{{"code", std::move(code)},
                                 {"message", std::move(message)},
                                 {"details", std::move(details)}};
            outcome.exit_code = exit_code;
        });
    auto value = request({"folder", "list"});
    daemon::RequestSession session(
        std::move(value), sink, 0, daemon::RequestSession::NonceGenerator{},
        daemon::ActivityTracker::Token{}, {}, tgcli::RequestDeadline{tgcli::RequestClock::now()});
    const daemon::ResolverCaller caller{proto::M6Operation::FolderList};
    CHECK_FALSE(
        daemon::m6_wait_for_folders(fake.client(), fake.client().auth_state(), caller, session));
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["code"] == "TIMEOUT");
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("M6 folder reads reject a replacement generation cache",
          "[m6][commands][folder][generation]") {
    for (const bool show : {false, true}) {
        CAPTURE(show);
        FakeM6 fake;
        auto input =
            show ? request({"folder", "show"}, {{"folder_id", 7}}) : request({"folder", "list"});
        auto pending = fake.run(std::move(input), [show](daemon::M6Coordinator& coordinator,
                                                         daemon::RequestSession& session) {
            if (show) {
                coordinator.folder_show(session.request(), session);
            } else {
                coordinator.folder_list(session.request(), session);
            }
        });
        fake.respond_me();
        fake.replace_generation_with_folders({.folders = {{.id = 7,
                                                           .name = {.text = "New",
                                                                    .animate_custom_emoji = false,
                                                                    .custom_emoji_entities = {}},
                                                           .icon = core::TdM6FolderIcon::Work,
                                                           .color_id = 1,
                                                           .is_shareable = false,
                                                           .has_my_invite_links = false}}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::GetChatFolder) == 0);
        CHECK(outcome.terminal_count == 1);
    }
}

TEST_CASE("M6 folder show combines one cached identity with one exact snapshot",
          "[m6][commands][folder]") {
    FakeM6 fake;
    const core::TdM6FolderInfo info{
        .id = 7,
        .name = {.text = "Work", .animate_custom_emoji = true, .custom_emoji_entities = {}},
        .icon = core::TdM6FolderIcon::Work,
        .color_id = 3,
        .is_shareable = false,
        .has_my_invite_links = true};
    fake.publish_folders({.folders = {info}});
    auto pending =
        fake.run(request({"folder", "show"}, {{"folder_id", 7}}),
                 [](daemon::M6Coordinator& coordinator, daemon::RequestSession& session) {
                     coordinator.folder_show(session.request(), session);
                 });
    fake.respond_me();
    core::TdM6ChatFolder folder;
    folder.name = info.name;
    folder.icon = info.icon;
    folder.color_id = info.color_id;
    folder.is_shareable = false;
    folder.included_chat_ids = {-1001};
    fake.respond(core::TdFunctionKind::GetChatFolder,
                 core::TdM6Response{core::TdM6MaybeChatFolder{folder}});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["folder"]["id"] == 7);
    CHECK((*outcome.result)["folder"]["name"]["animate_custom_emoji"] == true);
    CHECK((*outcome.result)["folder"]["included_chat_ids"] == json::array({-1001}));
}

TEST_CASE("M6 topic list consumes a complete bounded page with the frozen chat identity",
          "[m6][commands][topic]") {
    FakeM6 fake;
    auto pending =
        fake.run(request({"topic", "list"}, {{"chat", "-1001"}}),
                 [](daemon::M6Coordinator& coordinator, daemon::RequestSession& session) {
                     coordinator.topic_list(session.request(), session);
                 });
    fake.respond_me();
    fake.respond(core::TdFunctionKind::GetChat,
                 core::TdChat{.id = -1001,
                              .title = "Project",
                              .kind = core::TdChatKind::Supergroup,
                              .related_id = 55,
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
    fake.respond(core::TdFunctionKind::GetSupergroup,
                 core::TdSupergroup{
                     .id = 55, .usernames = {"project"}, .is_channel = false, .is_forum = true});
    core::TdM6ForumTopic topic;
    topic.info = {.chat_id = -1001,
                  .id = 9,
                  .name = "Updates",
                  .icon = {.color = core::TdM6TopicColor::Blue, .custom_emoji_id = "0"},
                  .creation_date = 1,
                  .creator = {.kind = core::TdM6SenderKind::User,
                              .id = 42,
                              .unsupported_tdlib_type_id = std::nullopt},
                  .is_general = false,
                  .is_outgoing = true,
                  .is_closed = false,
                  .is_hidden = false,
                  .is_name_implicit = false};
    fake.respond(
        core::TdFunctionKind::GetForumTopics,
        core::TdM6Response{core::TdM6ForumTopics{.total_count = 1, .topics = {std::move(topic)}}});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["items"][0]["id"] == 9);
    CHECK((*outcome.result)["items"][0]["name"] == "Updates");
    CHECK((*outcome.result)["next"].is_null());
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

TEST_CASE("M6 session target read maps every lifecycle stop exactly",
          "[m6][commands][session][lifecycle]") {
    enum class Stop { Authorization, Deadline, Cancel, Shutdown, Failed };
    for (const auto stop :
         {Stop::Authorization, Stop::Deadline, Stop::Cancel, Stop::Shutdown, Stop::Failed}) {
        CAPTURE(static_cast<int>(stop));
        FakeM6 fake;
        if (stop == Stop::Failed) {
            fake.fail_before_send(core::TdFunctionKind::GetActiveSessions);
        }
        auto input = request({"session", "list"});
        if (stop == Stop::Deadline) {
            input.context.timeout_seconds = 0.5;
        }
        std::shared_ptr<daemon::RequestSession> session;
        auto pending = fake.run(
            std::move(input),
            [](daemon::M6Coordinator& coordinator, daemon::RequestSession& current) {
                coordinator.session_list(current.request(), current);
            },
            &session);
        fake.respond_me();
        if (stop != Stop::Failed) {
            fake.observe(core::TdFunctionKind::GetActiveSessions);
        }
        if (stop == Stop::Authorization) {
            fake.push_authorization(core::AuthStateData{core::AuthState::LoggingOut});
        } else if (stop == Stop::Cancel) {
            session->disconnect();
        } else if (stop == Stop::Shutdown) {
            session->shutdown();
        }
        const auto outcome = pending.get();
        if (stop == Stop::Cancel) {
            CHECK(outcome.terminal_count == 0);
            continue;
        }
        REQUIRE(outcome.error);
        std::string_view code = "INTERNAL";
        if (stop == Stop::Authorization) {
            code = "NOT_AUTHED";
            CHECK((*outcome.error)["details"]["state"] == "logging_out");
        } else if (stop == Stop::Deadline) {
            code = "TIMEOUT";
        } else if (stop == Stop::Shutdown) {
            code = "DAEMON_SHUTDOWN";
        }
        CHECK((*outcome.error)["code"] == code);
        CHECK(outcome.terminal_count == 1);
    }
}

TEST_CASE("session list reconciles AbsentByPolicy before Ready", "[m6][commands][session]") {
    FakeM6 fake;
    fake.make_spool_wrong_mode();
    auto pending = fake.run(request({"session", "list"}), [](daemon::M6Coordinator& coordinator,
                                                             daemon::RequestSession& session) {
        coordinator.session_list(session.request(), session);
    });
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["code"] == "SPOOL_UNAVAILABLE");
    CHECK((*outcome.error)["details"] ==
          json{{"operation", "session_list"}, {"path", "spool/"}, {"reason", "wrong_mode"}});
    CHECK(fake.count(core::TdFunctionKind::GetMe) == 0);
    CHECK(fake.count(core::TdFunctionKind::GetActiveSessions) == 0);
}

} // namespace
