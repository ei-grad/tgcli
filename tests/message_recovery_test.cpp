#include "common/config.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/account_removal.hpp"
#include "daemon/commands.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/context.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/message_commands.hpp"
#include "daemon/removal_journal.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
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
};

class RecoveryTree {
  public:
    RecoveryTree()
        : root_("/tmp/tgcli-msg-recovery-test-" + std::to_string(::getpid()) + "-" +
                std::to_string(sequence_.fetch_add(1))) {
        std::filesystem::create_directories(config_root() + "/tgcli");
        std::filesystem::create_directories(state_root() + "/tgcli/accounts/main");
        std::filesystem::create_directories(data_root());
        std::filesystem::create_directories(runtime_root());
        for (const auto& directory :
             {root_, config_root(), config_root() + "/tgcli", state_root(), state_root() + "/tgcli",
              state_root() + "/tgcli/accounts", state_root() + "/tgcli/accounts/main", data_root(),
              runtime_root()}) {
            std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        }
        std::ofstream config(config_path());
        config << "default_account = \"main\"\n[accounts.main]\nallow_write = false\n";
        config.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~RecoveryTree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    RecoveryTree(const RecoveryTree&) = delete;
    RecoveryTree& operator=(const RecoveryTree&) = delete;
    RecoveryTree(RecoveryTree&&) = delete;
    RecoveryTree& operator=(RecoveryTree&&) = delete;

    [[nodiscard]] std::string config_root() const {
        return root_ + "/config";
    }
    [[nodiscard]] std::string state_root() const {
        return root_ + "/state";
    }
    [[nodiscard]] std::string data_root() const {
        return root_ + "/data";
    }
    [[nodiscard]] std::string runtime_root() const {
        return root_ + "/runtime";
    }
    [[nodiscard]] std::string config_path() const {
        return config_root() + "/tgcli/config.toml";
    }
    [[nodiscard]] std::string account_state() const {
        return state_root() + "/tgcli/accounts/main";
    }
    [[nodiscard]] tgcli::paths::Environment environment() const {
        return {.xdg_runtime_dir = runtime_root(),
                .xdg_config_home = config_root(),
                .xdg_data_home = data_root(),
                .xdg_state_home = state_root(),
                .tmpdir = root_,
                .home = root_,
                .uid = ::getuid(),
                .test_dc = false};
    }

  private:
    static inline std::atomic<unsigned> sequence_{0};
    std::string root_;
};

class DummyRemovalRemote final : public tgcli::daemon::AccountRemovalRemote {
  public:
    tgcli::daemon::RemovalRemoteProof prove_remote_logout(
        const tgcli::proto::AccountRemovePlan& /*plan*/,
        const std::shared_ptr<const tgcli::config::ConfigSnapshot>& /*config_snapshot*/,
        bool /*send_checkpointed*/, tgcli::daemon::RequestSession& /*session*/,
        const tgcli::daemon::RemovalCheckpoint& /*checkpoint*/) override {
        return tgcli::daemon::AccountRemoveRemoteResult::Kept;
    }

    std::optional<tgcli::daemon::RemovalOperationError>
    quiesce(tgcli::daemon::RequestSession& /*session*/) override {
        return std::nullopt;
    }
};

class RecoveryFixture {
  public:
    RecoveryFixture() {
        const auto environment = tree_.environment();
        store_ = std::make_unique<tgcli::config::Store>(tree_.config_path());
        config_ = std::make_unique<tgcli::daemon::ConfigRuntime>(tree_.config_path(), nullptr,
                                                                 environment.uid);

        removal_hooks_ = std::make_shared<tgcli::daemon::testing::RemovalJournalHooks>();
        removal_hooks_->should_fail = [&](tgcli::daemon::RemovalJournalFault fault) {
            if (fault == tgcli::daemon::RemovalJournalFault::DirectoryOpen) {
                record("removal");
            }
            return false;
        };
        journal_ = std::make_unique<tgcli::daemon::RemovalJournal>(
            tgcli::paths::removals_state_dir(environment), environment.uid, removal_hooks_);

        audit_hooks_ = std::make_shared<tgcli::daemon::testing::LogoutAuditHooks>();
        audit_hooks_->should_fail = [&](tgcli::daemon::LogoutAuditFault fault) {
            if (fault == tgcli::daemon::LogoutAuditFault::InspectSync) {
                record("logout");
            }
            return fail_logout_write_ && fault == tgcli::daemon::LogoutAuditFault::Write;
        };
        logout_hooks_ = std::make_shared<tgcli::daemon::testing::LogoutHooks>();
        logout_hooks_->invocation_id = [] { return "0123456789abcdef0123456789abcdef"; };
        logout_hooks_->timestamp = [] { return "2026-08-04T12:00:00Z"; };
        logout_hooks_->audit = audit_hooks_;

        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        runtime_->set_before_make([&](tgcli::core::TdFunctionKind function) {
            if (function != tgcli::core::TdFunctionKind::GetAuthorizationState) {
                record(std::string(tgcli::core::td_function_name(function)));
            }
        });
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

        removal_ = std::make_unique<tgcli::daemon::AccountRemovalCoordinator>(
            *store_, *journal_, environment, "main", remote_);
        logout_ = std::make_unique<tgcli::daemon::LogoutCoordinator>(
            *client_, *config_, environment, "main", tree_.config_path(), std::function<void()>{},
            logout_hooks_);
        messages_ =
            std::make_unique<tgcli::daemon::MessageCoordinator>(*client_, std::string("main"));
        context_.account = "main";
        context_.account_removal = removal_.get();
        context_.logout = logout_.get();
        context_.messages = messages_.get();
        tgcli::daemon::register_commands(dispatcher_, context_);
    }

    void add_incomplete_removal() {
        const auto loaded = store_->load();
        REQUIRE(loaded.snapshot);
        std::string error;
        const auto environment = tree_.environment();
        auto plan = tgcli::proto::make_account_remove_plan(
            {.account = "main",
             .keep_session = true,
             .delete_paths = {tgcli::paths::account_data_dir("main", environment),
                              tgcli::paths::account_state_dir("main", environment)},
             .config_path = tree_.config_path(),
             .config_snapshot = loaded.snapshot->identity,
             .data_root = std::nullopt,
             .state_root = std::nullopt,
             .reassign_default = std::nullopt},
            error);
        INFO(error);
        REQUIRE(plan);
        tgcli::daemon::RemovalJournalFailure failure;
        REQUIRE(journal_->create(std::string(removal_invocation_), *plan, failure));
        clear_trace();
    }

    void add_incomplete_logout() {
        const auto loaded = store_->load();
        REQUIRE(loaded.snapshot);
        std::string error;
        const auto plan = tgcli::proto::make_logout_plan("main", error);
        REQUIRE(plan);
        const tgcli::daemon::LogoutAuditLog audit(tree_.account_state(), "main", ::getuid());
        const tgcli::daemon::AuditRecordIdentity identity{std::string(logout_invocation_),
                                                          "2026-08-04T11:00:00Z"};
        auto intent = tgcli::daemon::make_logout_audit_intent(
            identity, *plan, loaded.snapshot->identity, tgcli::daemon::AuthoritySource::Config,
            tgcli::daemon::ConfirmationSource::Yes, error);
        INFO(error);
        REQUIRE(intent);
        tgcli::daemon::LogoutAuditFailure failure;
        REQUIRE(audit.append(tgcli::daemon::serialize(*intent), failure, true));
        fail_logout_write_ = true;
        clear_trace();
    }

    std::future<Outcome> dispatch(std::vector<std::string> command) {
        return std::async(std::launch::async, [this, command = std::move(command)]() mutable {
            Outcome outcome;
            tgcli::daemon::CallbackSink sink(
                [](const json&) {}, [](const json&) {},
                [&](json value) {
                    outcome.result = std::move(value);
                    outcome.exit_code = tgcli::kOk;
                },
                [&](std::string code, std::string message, json details, int exit_code) {
                    outcome.error = json{{"error",
                                          {{"code", std::move(code)},
                                           {"message", std::move(message)},
                                           {"details", std::move(details)}}}};
                    outcome.exit_code = exit_code;
                });
            tgcli::proto::Request request("main");
            request.command = command;
            request.args = command == std::vector<std::string>{"msg", "get"}
                               ? json{{"chat", "-1001"}, {"message_ids", json::array({123})}}
                               : json{{"chat", "-1001"}, {"message_id", 123}};
            request.context.timeout_seconds = 1.0;
            request.context.cwd = "/";
            tgcli::daemon::RequestSession session(std::move(request), sink);
            dispatcher_.dispatch(session);
            return outcome;
        });
    }

    template <typename T> void respond(tgcli::core::TdFunctionKind expected, T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        REQUIRE(sent.back().function.kind() == expected);
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

    [[nodiscard]] std::vector<std::string> trace() const {
        const std::lock_guard lock(trace_mutex_);
        return trace_;
    }

    [[nodiscard]] const RecoveryTree& tree() const {
        return tree_;
    }

    [[nodiscard]] const tgcli::daemon::RemovalJournal& journal() const {
        return *journal_;
    }

    static constexpr std::string_view removal_invocation() {
        return removal_invocation_;
    }

  private:
    void record(std::string value) {
        const std::lock_guard lock(trace_mutex_);
        trace_.push_back(std::move(value));
    }

    void clear_trace() {
        const std::lock_guard lock(trace_mutex_);
        trace_.clear();
    }

    static constexpr std::string_view removal_invocation_ = "00112233445566778899aabbccddeeff";
    static constexpr std::string_view logout_invocation_ = "fedcba9876543210fedcba9876543210";
    RecoveryTree tree_;
    std::unique_ptr<tgcli::config::Store> store_;
    std::unique_ptr<tgcli::daemon::ConfigRuntime> config_;
    std::shared_ptr<tgcli::daemon::testing::RemovalJournalHooks> removal_hooks_;
    std::unique_ptr<tgcli::daemon::RemovalJournal> journal_;
    std::shared_ptr<tgcli::daemon::testing::LogoutAuditHooks> audit_hooks_;
    std::shared_ptr<tgcli::daemon::testing::LogoutHooks> logout_hooks_;
    DummyRemovalRemote remote_;
    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient client_id_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::AccountRemovalCoordinator> removal_;
    std::unique_ptr<tgcli::daemon::LogoutCoordinator> logout_;
    std::unique_ptr<tgcli::daemon::MessageCoordinator> messages_;
    tgcli::daemon::DaemonContext context_;
    tgcli::daemon::Dispatcher dispatcher_;
    std::size_t sent_count_ = 1;
    bool fail_logout_write_ = false;
    mutable std::mutex trace_mutex_;
    std::vector<std::string> trace_;
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
            .last_message = std::nullopt};
}

std::size_t first_index(const std::vector<std::string>& trace, std::string_view value) {
    const auto found = std::ranges::find(trace, value);
    return found == trace.end() ? trace.size() : static_cast<std::size_t>(found - trace.begin());
}

} // namespace

TEST_CASE("msg real dispatch stops removal before logout recovery and every TD read",
          "[msg][recovery][dispatch][fake-boundary]") {
    for (const auto& command :
         {std::vector<std::string>{"msg", "get"}, std::vector<std::string>{"msg", "link"}}) {
        RecoveryFixture fixture;
        fixture.add_incomplete_removal();
        const auto outcome = fixture.dispatch(command).get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "REMOVAL_INCOMPLETE");
        CHECK(
            (*outcome.error)["error"]["details"] ==
            json{{"account", "main"},
                 {"path", fixture.journal().tombstone_path(RecoveryFixture::removal_invocation())},
                 {"invocation_id", RecoveryFixture::removal_invocation()},
                 {"stage", "planned"},
                 {"completed_stages", json::array({"planned"})},
                 {"reason", "prior_crash"}});
        CHECK(fixture.trace() == std::vector<std::string>{"removal"});
        CHECK(fixture.sent_count() == 1);
        CHECK_FALSE(outcome.result);
    }
}

TEST_CASE("msg real dispatch stops unresolved logout after clean removal and before TD",
          "[msg][recovery][dispatch][fake-boundary]") {
    for (const auto& command :
         {std::vector<std::string>{"msg", "get"}, std::vector<std::string>{"msg", "link"}}) {
        RecoveryFixture fixture;
        fixture.add_incomplete_logout();
        const auto outcome = fixture.dispatch(command).get();
        REQUIRE(outcome.error);
        CHECK(outcome.exit_code == tgcli::kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "AUDIT_INCOMPLETE");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"account", "main"},
                   {"path", fixture.tree().account_state() + "/audit.log"},
                   {"mutation_state", "none"},
                   {"completed_stages", json::array({"intent_synced"})}});
        const auto trace = fixture.trace();
        CHECK(first_index(trace, "removal") < first_index(trace, "logout"));
        CHECK(first_index(trace, "logout") < trace.size());
        CHECK(fixture.sent_count() == 1);
        CHECK_FALSE(outcome.result);
    }
}

TEST_CASE("msg real dispatch clean trace orders both recoveries before resolver and target",
          "[msg][recovery][dispatch][fake-boundary]") {
    for (const auto& command :
         {std::vector<std::string>{"msg", "get"}, std::vector<std::string>{"msg", "link"}}) {
        RecoveryFixture fixture;
        auto pending = fixture.dispatch(command);
        fixture.respond(tgcli::core::TdFunctionKind::GetMe, user());
        fixture.respond(tgcli::core::TdFunctionKind::GetChat, chat());
        if (command.back() == "get") {
            fixture.respond(tgcli::core::TdFunctionKind::GetMessages,
                            tgcli::core::TdMessages{
                                .messages = {tgcli::core::TdMessageSummary{
                                    .id = 123,
                                    .chat_id = -1001,
                                    .date = 0,
                                    .sender = {.kind = tgcli::core::TdMessageSenderKind::User,
                                               .id = 42,
                                               .tdlib_type_id = 1},
                                    .is_outgoing = false,
                                    .topic = std::nullopt,
                                    .content_kind = tgcli::core::TdMessageContentKind::Text,
                                    .text = "message"}}});
        } else {
            fixture.respond(tgcli::core::TdFunctionKind::GetMessageLink,
                            tgcli::core::TdMessageLink{.link = "urn:message", .is_public = false});
        }
        REQUIRE(pending.get().result);
        const auto trace = fixture.trace();
        const auto* const target = command.back() == "get" ? "getMessages" : "getMessageLink";
        CHECK(first_index(trace, "removal") < first_index(trace, "logout"));
        CHECK(first_index(trace, "logout") < first_index(trace, "getMe"));
        CHECK(first_index(trace, "getMe") < first_index(trace, "getChat"));
        CHECK(first_index(trace, "getChat") < first_index(trace, target));
    }
}
