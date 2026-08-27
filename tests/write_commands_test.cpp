#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/invite_redaction.hpp"
#include "core/td_log.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/m6_model.hpp"
#include "daemon/request_fingerprint.hpp"
#include "daemon/request_session.hpp"
#include "daemon/write_commands.hpp"
#include "daemon/write_contract.hpp"
#include "daemon/write_domain.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using nlohmann::json;

namespace {

class WriteTree final {
  public:
    explicit WriteTree(bool allow_write = true) {
        std::string pattern = "/tmp/tgcli-write-command-XXXXXX";
        pattern.push_back('\0');
        const auto* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        for (const auto& directory :
             {root_ + "/config", root_ + "/config/tgcli", root_ + "/state", root_ + "/state/tgcli",
              root_ + "/state/tgcli/accounts", account_state()}) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
        std::ofstream output(config_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << "default_account = \"main\"\n\n[accounts.main]\nallow_write = "
               << (allow_write ? "true\n" : "false\n");
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~WriteTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    WriteTree(const WriteTree&) = delete;
    WriteTree& operator=(const WriteTree&) = delete;
    WriteTree(WriteTree&&) = delete;
    WriteTree& operator=(WriteTree&&) = delete;

    [[nodiscard]] std::string config_path() const {
        return root_ + "/config/tgcli/config.toml";
    }

    [[nodiscard]] std::string account_state() const {
        return root_ + "/state/tgcli/accounts/main";
    }

    [[nodiscard]] std::string audit_path() const {
        return account_state() + "/audit.log";
    }

    [[nodiscard]] std::string store_path() const {
        return account_state() + "/idempotency.db";
    }

    [[nodiscard]] std::string source_path(std::string_view name = "attachment.bin") const {
        return account_state() + "/" + std::string(name);
    }

    void write_source(std::string_view bytes, std::string_view name = "attachment.bin") const {
        std::ofstream output(source_path(name), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(source_path(name).c_str(), 0600) == 0);
    }

  private:
    std::string root_;
};

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    int exit_code = -1;
    int terminal_count = 0;
};

class WipeTrace final {
  public:
    [[nodiscard]] secure::WipeObserver observer() {
        return [state = state_](std::string_view stage, const char* bytes, std::size_t size) {
            const bool all_zero =
                size == 0 || std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                                         [](char value) { return value == '\0'; });
            const std::lock_guard lock(state->mutex);
            state->observations.emplace_back(std::string(stage), size, all_zero);
        };
    }

    [[nodiscard]] bool saw(std::string_view stage, std::size_t size) const {
        const std::lock_guard lock(state_->mutex);
        return std::ranges::any_of(state_->observations, [&](const auto& observation) {
            return std::get<0>(observation) == stage && std::get<1>(observation) == size &&
                   std::get<2>(observation);
        });
    }

  private:
    struct State {
        std::mutex mutex;
        std::vector<std::tuple<std::string, std::size_t, bool>> observations;
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

std::string read_bytes(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

core::TdUserSummary self() {
    return {.id = 42,
            .first_name = "Ada",
            .last_name = "",
            .usernames = {"ada"},
            .phone_number = "12025550123",
            .is_bot = false,
            .is_premium = false,
            .presence = core::TdUserPresence::Online};
}

core::TdChat basic_chat() {
    return {.id = -1001,
            .title = "Project",
            .kind = core::TdChatKind::BasicGroup,
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

core::TdChat private_chat(std::int64_t user_id) {
    auto chat = basic_chat();
    chat.id = user_id;
    chat.title = "Peer";
    chat.kind = core::TdChatKind::Private;
    chat.related_id = user_id;
    return chat;
}

core::TdChat supergroup_chat() {
    auto chat = basic_chat();
    chat.kind = core::TdChatKind::Supergroup;
    chat.related_id = 55;
    return chat;
}

core::TdUserSummary peer(core::TdUserPresence presence, bool bot = false, std::int64_t id = 77) {
    return {.id = id,
            .first_name = "Peer",
            .last_name = "",
            .usernames = {"peer"},
            .phone_number = "12025550124",
            .is_bot = bot,
            .is_premium = false,
            .presence = presence};
}

core::TdWriteMessage stable_message(std::string text = "hello",
                                    std::optional<std::int32_t> scheduled_at = std::nullopt) {
    return {
        .message = {.id = 101,
                    .chat_id = -1001,
                    .date = 1'785'924'000,
                    .sender = {.kind = core::TdMessageSenderKind::User,
                               .id = 42,
                               .tdlib_type_id = 1},
                    .is_outgoing = true,
                    .topic = std::nullopt,
                    .content_kind = core::TdMessageContentKind::Text,
                    .text = std::move(text)},
        .sending_state = {},
        .scheduling_state =
            scheduled_at
                ? core::TdMessageSchedulingState{.kind =
                                                     core::TdMessageSchedulingStateKind::SendAtDate,
                                                 .send_date = *scheduled_at,
                                                 .repeat_period = 0,
                                                 .unsupported_tdlib_type_id = std::nullopt}
                : core::TdMessageSchedulingState{},
        .has_reply_markup = false};
}

core::TdWriteMessage online_message() {
    auto message = stable_message();
    message.message.chat_id = 77;
    message.scheduling_state = {.kind = core::TdMessageSchedulingStateKind::SendWhenOnline,
                                .send_date = 0,
                                .repeat_period = 0,
                                .unsupported_tdlib_type_id = std::nullopt};
    return message;
}

core::TdWriteMessage saved_document_message(std::string caption = "experiment result") {
    auto message = stable_message(std::move(caption));
    message.message.id = 202;
    message.message.chat_id = 42;
    message.message.content_kind = core::TdMessageContentKind::Document;
    message.message.topic =
        core::TdTopic{.kind = core::TdTopicKind::Saved, .id = 19, .tdlib_type_id = 1};
    return message;
}

core::TdWriteMessage forwarded_message(std::int64_t id) {
    auto message = stable_message("forwarded");
    message.message.id = id;
    message.message.chat_id = -1002;
    return message;
}

core::TdPlanningMessage planning_message(std::int64_t id) {
    return {.id = id,
            .chat_id = -1001,
            .date = 1'785'924'000,
            .sender = {.kind = core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
            .is_outgoing = true,
            .topic = std::nullopt,
            .content_kind = core::TdMessageContentKind::Text,
            .text = "cleanup",
            .has_scheduling_state = false,
            .has_reply_markup = false};
}

core::TdPlanningMessage
saved_planning_message(std::int64_t id,
                       std::optional<core::TdTopic> topic = core::TdTopic{
                           .kind = core::TdTopicKind::Saved, .id = 19, .tdlib_type_id = 1}) {
    auto message = planning_message(id);
    message.chat_id = 42;
    message.topic = topic;
    return message;
}

template <typename T>
const T& function_field(const core::TdFunctionData& function, std::string_view name) {
    const auto found = std::ranges::find_if(
        function.fields(), [name](const auto& field) { return field.has_name(name); });
    REQUIRE(found != function.fields().end());
    const auto* value = std::get_if<T>(&found->value());
    REQUIRE(value != nullptr);
    return *value;
}

template <typename T>
bool function_field_holds(const core::TdFunctionData& function, std::string_view name) {
    const auto found = std::ranges::find_if(
        function.fields(), [name](const auto& field) { return field.has_name(name); });
    REQUIRE(found != function.fields().end());
    return std::holds_alternative<T>(found->value());
}

class FakeWrites final {
  public:
    explicit FakeWrites(
        bool allow_write = true,
        std::shared_ptr<const daemon::testing::IdempotencyStoreHooks> store_hooks = {})
        : tree_(allow_write), config_(tree_.config_path(), {}, ::getuid()),
          coordinator_hooks_(std::make_shared<daemon::testing::WriteCoordinatorHooks>()) {
        std::string error;
        lease_ =
            daemon_lock::acquire_lifetime(tree_.account_state() + "/daemon.lock", identity_, error);
        INFO(error);
        REQUIRE(lease_);
        auto created = daemon::IdempotencyFoundation::create(
            tree_.account_state(), "main", ::getuid(), lease_, {}, std::move(store_hooks));
        REQUIRE(std::holds_alternative<daemon::IdempotencyFoundation>(created));
        foundation_ = std::make_shared<daemon::IdempotencyFoundation>(
            std::get<daemon::IdempotencyFoundation>(std::move(created)));

        auto runtime = std::make_unique<test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {}, core::AuthStateData{core::AuthState::Ready});
        const auto ready_deadline = std::chrono::steady_clock::now() + 2s;
        while (client_->auth_state()->data.state != core::AuthState::Ready &&
               std::chrono::steady_clock::now() < ready_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(client_->auth_state()->data.state == core::AuthState::Ready);

        coordinator_ = std::make_unique<daemon::WriteCoordinator>(
            *client_, "main", tree_.config_path(), ::getuid(), foundation_, std::function<void()>{},
            coordinator_hooks_);
        daemon::register_write_commands(dispatcher_, *coordinator_);

        const auto admitted_result = config_.admit("main", std::chrono::steady_clock::now() + 2s);
        REQUIRE(admitted_result.refresh_status == daemon::ConfigRefreshStatus::Completed);
        REQUIRE(admitted_result.decision);
        REQUIRE(std::holds_alternative<std::shared_ptr<const daemon::AdmittedAccountConfig>>(
            *admitted_result.decision));
        admitted_ = std::get<std::shared_ptr<const daemon::AdmittedAccountConfig>>(
            *admitted_result.decision);
        REQUIRE(admitted_);
    }

    ~FakeWrites() {
        coordinator_.reset();
        client_.reset();
        foundation_.reset();
        lease_.reset();
    }

    FakeWrites(const FakeWrites&) = delete;
    FakeWrites& operator=(const FakeWrites&) = delete;
    FakeWrites(FakeWrites&&) = delete;
    FakeWrites& operator=(FakeWrites&&) = delete;

    std::future<Outcome>
    dispatch(const proto::Request& request,
             std::shared_ptr<daemon::RequestSession>* exposed_session = nullptr) {
        std::string error;
        auto frozen = proto::admit_request_source(request, error);
        INFO(error);
        REQUIRE(frozen);
        auto outcome = std::make_shared<Outcome>();
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [outcome](json result) {
                outcome->result = std::move(result);
                outcome->exit_code = kOk;
                ++outcome->terminal_count;
            },
            [outcome](std::string code, std::string message, json details, int exit_code) {
                outcome->error = json{{"error",
                                       {{"code", std::move(code)},
                                        {"message", std::move(message)},
                                        {"details", std::move(details)}}}};
                outcome->exit_code = exit_code;
                ++outcome->terminal_count;
            });
        auto session = std::make_shared<daemon::RequestSession>(
            std::move(*frozen), sink, 0, daemon::RequestSession::NonceGenerator{},
            daemon::ActivityTracker::Token{}, admitted_, std::nullopt,
            daemon::ConfigAdmissionMode::FrozenRuntime);
        if (exposed_session != nullptr) {
            *exposed_session = session;
        }
        return std::async(std::launch::async, [this, outcome, session] {
            dispatcher_.dispatch(*session);
            return *outcome;
        });
    }

    std::future<Outcome>
    m6_mutation(proto::M6Operation operation, const proto::Request& request,
                std::shared_ptr<daemon::RequestSession>* exposed_session = nullptr,
                daemon::CallbackSink::ChallengeFn on_challenge = {}) {
        std::string error;
        auto frozen = proto::admit_request_source(request, error);
        INFO(error);
        REQUIRE(frozen);
        auto outcome = std::make_shared<Outcome>();
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [outcome](json result) {
                outcome->result = std::move(result);
                outcome->exit_code = kOk;
                ++outcome->terminal_count;
            },
            [outcome](std::string code, std::string message, json details, int exit_code) {
                outcome->error = json{{"error",
                                       {{"code", std::move(code)},
                                        {"message", std::move(message)},
                                        {"details", std::move(details)}}}};
                outcome->exit_code = exit_code;
                ++outcome->terminal_count;
            },
            std::move(on_challenge));
        auto session = std::make_shared<daemon::RequestSession>(
            std::move(*frozen), sink, 0, daemon::RequestSession::NonceGenerator{},
            daemon::ActivityTracker::Token{}, admitted_, std::nullopt,
            daemon::ConfigAdmissionMode::FrozenRuntime);
        if (exposed_session != nullptr) {
            *exposed_session = session;
        }
        return std::async(std::launch::async, [this, operation, outcome, session] {
            coordinator_->m6_mutation(operation, session->request(), *session);
            return *outcome;
        });
    }

    std::future<Outcome>
    terminate_session(const proto::Request& request,
                      std::shared_ptr<daemon::RequestSession>* exposed_session = nullptr) {
        std::string error;
        auto frozen = proto::admit_request_source(request, error);
        INFO(error);
        REQUIRE(frozen);
        auto outcome = std::make_shared<Outcome>();
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [outcome](json result) {
                outcome->result = std::move(result);
                outcome->exit_code = kOk;
                ++outcome->terminal_count;
            },
            [outcome](std::string code, std::string message, json details, int exit_code) {
                outcome->error = json{{"error",
                                       {{"code", std::move(code)},
                                        {"message", std::move(message)},
                                        {"details", std::move(details)}}}};
                outcome->exit_code = exit_code;
                ++outcome->terminal_count;
            });
        auto session = std::make_shared<daemon::RequestSession>(
            std::move(*frozen), sink, 0, daemon::RequestSession::NonceGenerator{},
            daemon::ActivityTracker::Token{}, admitted_, std::nullopt,
            daemon::ConfigAdmissionMode::FrozenRuntime);
        if (exposed_session != nullptr) {
            *exposed_session = session;
        }
        return std::async(std::launch::async, [this, outcome, session] {
            coordinator_->terminate_session(session->request(), *session);
            return *outcome;
        });
    }

    template <typename T> core::TdFunctionData respond(core::TdFunctionKind expected, T value) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        auto function = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id,
                                core::TdValue::from(std::move(value)));
        ++sent_count_;
        return function;
    }

    core::TdFunctionData respond_value(core::TdFunctionKind expected, core::TdValue value) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        auto function = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id, std::move(value));
        ++sent_count_;
        return function;
    }

    core::TdFunctionData observe(core::TdFunctionKind expected) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        ++sent_count_;
        return sent.back().function;
    }

    std::uint64_t observe_query(core::TdFunctionKind expected) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        ++sent_count_;
        return sent.back().query_id;
    }

    std::pair<core::TdFunctionData, std::uint64_t> observe_call(core::TdFunctionKind expected) {
        CAPTURE(core::td_function_name(expected), sent_count_);
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        ++sent_count_;
        return {sent.back().function, sent.back().query_id};
    }

    template <typename T> void push_response(std::uint64_t query_id, T value) {
        runtime_->push_response(client_id_, query_id, core::TdValue::from(std::move(value)));
    }

    void push_authorization(core::AuthStateData state) {
        runtime_->push_update(client_id_, {}, std::move(state));
    }

    template <typename T> void push_update(T value) {
        runtime_->push_update(client_id_, core::TdValue::from(std::move(value)));
    }

    void push_forward_success(std::int64_t temporary_id,
                              std::optional<core::TdWriteMessage> message) {
        runtime_->push_message_send_succeeded(client_id_, temporary_id, std::move(message));
    }

    void push_forward_failure(std::int64_t temporary_id,
                              std::optional<core::TdWriteMessage> message,
                              std::optional<core::TdError> error) {
        runtime_->push_message_send_failed(client_id_, temporary_id, std::move(message),
                                           std::move(error));
    }

    std::future<void> observe_forward_progress() {
        auto observed = std::make_shared<std::promise<void>>();
        auto future = observed->get_future();
        auto delivered = std::make_shared<std::atomic<bool>>(false);
        coordinator_hooks_->forward.on_progress = [observed, delivered](const auto&) {
            if (!delivered->exchange(true)) {
                observed->set_value();
            }
        };
        return future;
    }

    template <typename T>
    bool try_respond(core::TdFunctionKind expected, T value,
                     std::chrono::milliseconds timeout = 100ms) {
        if (!runtime_->wait_for_sent(sent_count_ + 1, timeout)) {
            return false;
        }
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        runtime_->push_response(client_id_, sent.back().query_id,
                                core::TdValue::from(std::move(value)));
        ++sent_count_;
        return true;
    }

    [[nodiscard]] std::size_t count(core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
    }

    [[nodiscard]] const WriteTree& tree() const {
        return tree_;
    }

    [[nodiscard]] const std::shared_ptr<daemon::IdempotencyFoundation>& foundation() const {
        return foundation_;
    }

    void file_spool_hooks(std::shared_ptr<const daemon::testing::FileSpoolHooks> hooks) {
        coordinator_hooks_->file_spool = std::move(hooks);
    }

    void reject_before_request(core::TdFunctionKind kind) {
        auto advance_authorization = [this] {
            const auto sequence = client_->auth_state()->auth_sequence;
            runtime_->push_update(client_id_, {}, core::AuthStateData{core::AuthState::Ready});
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (client_->auth_state()->auth_sequence == sequence &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
        };
        if (kind == core::TdFunctionKind::SendMessage) {
            coordinator_hooks_->single_send.before_request = std::move(advance_authorization);
        } else {
            coordinator_hooks_->direct_rpc.before_request = std::move(advance_authorization);
        }
    }

    void lose_authorization_before_direct_request() {
        coordinator_hooks_->direct_rpc.before_request = [this] {
            const auto initial = client_->auth_state();
            REQUIRE(initial);
            const auto sequence = initial->auth_sequence;
            runtime_->push_update(client_id_, {}, core::AuthStateData{core::AuthState::LoggingOut});
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto current = client_->auth_state();
                if (!current || current->auth_sequence != sequence ||
                    current->data.state != core::AuthState::Ready) {
                    return;
                }
                std::this_thread::sleep_for(1ms);
            }
        };
    }

    void advance_authorization_before_principal_cas(bool before_dispatch = false) {
        auto advance = [this] {
            const auto initial = client_->auth_state();
            REQUIRE(initial);
            const auto sequence = initial->auth_sequence;
            std::promise<void> observed;
            auto future = observed.get_future();
            auto delivered = std::make_shared<std::atomic<bool>>(false);
            const auto subscription = client_->subscribe_auth_states(
                [sequence, delivered, &observed](const auto& current) {
                    if (current && current->auth_sequence > sequence &&
                        !delivered->exchange(true)) {
                        observed.set_value();
                    }
                });
            runtime_->push_update(client_id_, {}, core::AuthStateData{core::AuthState::Ready});
            REQUIRE(future.wait_for(2s) == std::future_status::ready);
            client_->unsubscribe_auth_states(subscription);
        };
        if (before_dispatch) {
            coordinator_hooks_->before_dispatch_principal_cas = std::move(advance);
        } else {
            coordinator_hooks_->before_principal_cas = std::move(advance);
        }
    }

    void replace_generation_before_principal_cas() {
        coordinator_hooks_->before_principal_cas = [this] {
            const auto initial = client_->auth_state();
            REQUIRE(initial);
            const auto sent_before = runtime_->sent_functions_including_current_state().size();
            runtime_->push_update(client_id_, {}, core::AuthStateData{core::AuthState::Closed});
            REQUIRE(runtime_->wait_for_sent_including_current_state(sent_before + 2));
            const auto clients = runtime_->clients();
            REQUIRE(clients.size() >= 2);
            const auto replacement = clients.back();
            runtime_->push_response(replacement, 2, core::TdValue::from(core::TdCurrentState{}));
            runtime_->push_response(replacement, 1, {},
                                    core::AuthStateData{core::AuthState::Ready});
            std::promise<void> observed;
            auto future = observed.get_future();
            auto delivered = std::make_shared<std::atomic<bool>>(false);
            const auto subscription =
                client_->subscribe_auth_states([generation = replacement.client_generation,
                                                delivered, &observed](const auto& current) {
                    if (current && current->client_generation == generation &&
                        current->data.state == core::AuthState::Ready &&
                        !delivered->exchange(true)) {
                        observed.set_value();
                    }
                });
            if (const auto current = client_->auth_state();
                current && current->client_generation == replacement.client_generation &&
                current->data.state == core::AuthState::Ready && !delivered->exchange(true)) {
                observed.set_value();
            }
            REQUIRE(future.wait_for(2s) == std::future_status::ready);
            client_->unsubscribe_auth_states(subscription);
        };
    }

    void replace_generation_with_folders(core::TdM6ChatFoldersUpdate update) {
        const auto sent_before = runtime_->sent_functions_including_current_state().size();
        runtime_->push_update(client_id_, {}, core::AuthStateData{core::AuthState::Closed});
        REQUIRE(runtime_->wait_for_sent_including_current_state(sent_before + 2));
        const auto clients = runtime_->clients();
        REQUIRE(clients.size() >= 2);
        const auto replacement = clients.back();
        core::TdCurrentState state;
        state.updates.push_back(core::TdValue::from(std::move(update)));
        runtime_->push_response(replacement, 2, core::TdValue::from(std::move(state)));
        runtime_->push_response(replacement, 1, {}, core::AuthStateData{core::AuthState::Ready});
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto current = client_->auth_state();
            if (current && current->client_generation == replacement.client_generation &&
                current->data.state == core::AuthState::Ready) {
                client_id_ = replacement;
                ++sent_count_;
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        FAIL("replacement M6 generation did not become Ready");
    }

    void expire_direct_before_request() {
        coordinator_hooks_->direct_rpc.now = [] { return core::TdEventClock::time_point::max(); };
    }

    void expire_direct_before_submit() {
        auto expired = std::make_shared<std::atomic<bool>>(false);
        coordinator_hooks_->direct_rpc.now = [expired] {
            return expired->load() ? core::TdEventClock::time_point::max()
                                   : core::TdEventClock::now();
        };
        coordinator_hooks_->direct_rpc.before_submit = [expired] { expired->store(true); };
    }

    void expire_forward_before_request() {
        coordinator_hooks_->forward.now = [] { return core::TdEventClock::time_point::max(); };
    }

    void expire_forward_before_submit() {
        auto expired = std::make_shared<std::atomic<bool>>(false);
        coordinator_hooks_->forward.now = [expired] {
            return expired->load() ? core::TdEventClock::time_point::max()
                                   : core::TdEventClock::now();
        };
        coordinator_hooks_->forward.before_submit = [expired] { expired->store(true); };
    }

    void expire_forward_after_progress() {
        auto expired = std::make_shared<std::atomic<bool>>(false);
        coordinator_hooks_->forward.now = [expired] {
            return expired->load() ? core::TdEventClock::time_point::max()
                                   : core::TdEventClock::now();
        };
        coordinator_hooks_->forward.on_progress = [expired](const auto&) { expired->store(true); };
    }

    void cancel_before_request(core::TdFunctionKind kind,
                               std::shared_ptr<daemon::RequestSession>* session) {
        auto disconnect = [session] { (*session)->disconnect(); };
        if (kind == core::TdFunctionKind::SendMessage) {
            coordinator_hooks_->single_send.before_request = std::move(disconnect);
        } else {
            coordinator_hooks_->direct_rpc.before_request = std::move(disconnect);
        }
    }

    void cancel_before_submit(core::TdFunctionKind kind,
                              std::shared_ptr<daemon::RequestSession>* session) {
        auto disconnect = [session] { (*session)->disconnect(); };
        if (kind == core::TdFunctionKind::SendMessage) {
            coordinator_hooks_->single_send.before_submit = std::move(disconnect);
        } else {
            coordinator_hooks_->direct_rpc.before_submit = std::move(disconnect);
        }
    }

    [[nodiscard]] core::TdFormattedText parsed_text(std::string text) const {
        return test::ScriptedTdRuntime::parsed_formatted_text(client_id_, std::move(text));
    }

  private:
    WriteTree tree_;
    daemon::ConfigRuntime config_;
    daemon_lock::Identity identity_;
    std::shared_ptr<daemon_lock::LifetimeLease> lease_;
    std::shared_ptr<daemon::IdempotencyFoundation> foundation_;
    test::ScriptedTdRuntime* runtime_ = nullptr;
    test::ScriptedClient client_id_{};
    std::unique_ptr<core::TdClient> client_;
    std::shared_ptr<daemon::testing::WriteCoordinatorHooks> coordinator_hooks_;
    std::unique_ptr<daemon::WriteCoordinator> coordinator_;
    daemon::Dispatcher dispatcher_;
    std::shared_ptr<const daemon::AdmittedAccountConfig> admitted_;
    std::size_t sent_count_ = 1;
};

proto::Request send_request(std::string text = "hello", bool dry_run = false,
                            std::optional<std::string> key = "m3-public-key-sentinel",
                            std::optional<std::int32_t> scheduled_at = std::nullopt) {
    proto::Request request("main");
    request.id = 41;
    request.command = {"send"};
    request.args = {{"chat", "-1001"},
                    {"text", std::move(text)},
                    {"parse_mode", "plain"},
                    {"reply_to", nullptr},
                    {"topic", nullptr},
                    {"silent", false},
                    {"schedule", scheduled_at ? json{{"kind", "at"}, {"send_date", *scheduled_at}}
                                              : json(nullptr)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.dry_run = dry_run;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request saved_attach_request(const WriteTree& tree, bool dry_run = false,
                                    std::optional<std::string> key = "saved-attach-key") {
    proto::Request request("main");
    request.id = 49;
    request.command = {"saved", "attach"};
    request.args = {
        {"message_id", -77}, {"path", tree.source_path()}, {"caption", "experiment result"}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.dry_run = dry_run;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request contact_mutation_request(std::string command,
                                        std::optional<std::string> key = std::nullopt,
                                        bool dry_run = false) {
    proto::Request request("main");
    request.id = 61;
    request.command = {"contact", std::move(command)};
    request.args = {{"user", "77"}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.idempotency_key = std::move(key);
    request.context.dry_run = dry_run;
    return request;
}

proto::Request folder_create_request(bool dry_run = false) {
    proto::Request request("main");
    request.id = 62;
    request.command = {"folder", "create"};
    request.args = {{"name", "Work"},
                    {"chats", json::array({"-1002", "-1001", "-1001"})},
                    {"icon", "work"},
                    {"color_id", 2}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.dry_run = dry_run;
    return request;
}

proto::Request folder_membership_request(bool add) {
    proto::Request request("main");
    request.id = 63;
    request.command = {"folder", add ? "add-chat" : "remove-chat"};
    request.args = {{"folder_id", 7}, {"chat", add ? "-1002" : "-1001"}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    return request;
}

proto::Request folder_edit_request() {
    proto::Request request("main");
    request.id = 68;
    request.command = {"folder", "edit"};
    request.args = {{"folder_id", 7},
                    {"name", "Other"},
                    {"icon", nullptr},
                    {"use_default_icon", false},
                    {"color_id", nullptr}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    return request;
}

proto::Request folder_delete_request() {
    proto::Request request("main");
    request.id = 69;
    request.command = {"folder", "delete"};
    request.args = {{"folder_id", 7}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = true;
    return request;
}

core::TdM6FolderInfo folder_info() {
    return {.id = 7,
            .name = {.text = "Work", .animate_custom_emoji = true, .custom_emoji_entities = {}},
            .icon = core::TdM6FolderIcon::Work,
            .color_id = 2,
            .is_shareable = false,
            .has_my_invite_links = false};
}

core::TdM6ChatFolder folder_snapshot() {
    return {.name = {.text = "Work", .animate_custom_emoji = true, .custom_emoji_entities = {}},
            .icon = core::TdM6FolderIcon::Work,
            .color_id = 2,
            .is_shareable = false,
            .pinned_chat_ids = {},
            .included_chat_ids = {-1001},
            .excluded_chat_ids = {-1002},
            .exclude_muted = false,
            .exclude_read = false,
            .exclude_archived = false,
            .include_contacts = false,
            .include_non_contacts = false,
            .include_bots = false,
            .include_groups = false,
            .include_channels = false};
}

proto::Request topic_mutation_request(const std::string& action) {
    proto::Request request("main");
    request.id = 64;
    request.command = {"topic", action};
    if (action == "create") {
        request.args = {{"chat", "-1001"}, {"name", "Updates"}, {"icon", "blue"}};
    } else if (action == "edit") {
        request.args = {{"chat", "-1001"}, {"topic_id", 9}, {"name", "Other"}};
    } else {
        request.args = {{"chat", "-1001"}, {"topic_id", 9}};
    }
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    return request;
}

core::TdSupergroup forum_supergroup() {
    return {.id = 55, .usernames = {"project"}, .is_channel = false, .is_forum = true};
}

core::TdM6MemberStatus topic_admin_status() {
    core::TdM6MemberStatus status;
    status.kind = core::TdM6MemberStatusKind::Administrator;
    status.can_be_edited = true;
    status.rights.can_manage_chat = true;
    status.rights.can_manage_topics = true;
    return status;
}

core::TdM6ForumTopicInfo topic_info(bool closed = false) {
    return {.chat_id = -1001,
            .id = 9,
            .name = "Updates",
            .icon = {.color = core::TdM6TopicColor::Blue, .custom_emoji_id = "0"},
            .creation_date = 1'700'000'000,
            .creator = {.kind = core::TdM6SenderKind::User,
                        .id = 42,
                        .unsupported_tdlib_type_id = std::nullopt},
            .is_general = false,
            .is_outgoing = true,
            .is_closed = closed,
            .is_hidden = false,
            .is_name_implicit = false};
}

proto::Request chat_admin_request(const std::string& action) {
    proto::Request request("main");
    request.id = 65;
    request.command = {"chat", action};
    if (action == "set-title") {
        request.args = {{"chat", "-1001"}, {"title", "Renamed"}};
    } else if (action == "set-description") {
        request.args = {{"chat", "-1001"}, {"description", "Description"}};
    } else if (action == "promote") {
        request.args = {
            {"chat", "-1001"}, {"user", "77"}, {"rights", json::array({"change-info"})}};
    } else if (action == "set-permissions") {
        request.args = {{"chat", "-1001"}, {"permissions", json::array({"send-basic-messages"})}};
    } else {
        request.args = {{"chat", "-1001"}, {"user", "77"}};
    }
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    return request;
}

proto::Request invite_link_request(std::optional<std::string> revoke = std::nullopt) {
    proto::Request request("main");
    request.id = 70;
    request.command = {"chat", "invite-link"};
    request.args = {{"chat", "-1001"}, {"revoke", revoke ? json(*revoke) : json(nullptr)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = true;
    return request;
}

proto::Request storage_optimize_request() {
    proto::Request request("main");
    request.id = 66;
    request.command = {"storage", "optimize"};
    request.args = json::object();
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = true;
    return request;
}

proto::Request session_terminate_request(bool dry_run = false) {
    proto::Request request("main");
    request.id = 67;
    request.command = {"session", "terminate"};
    request.args = {{"session_id", "7"}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = true;
    request.context.dry_run = dry_run;
    return request;
}

core::TdSessions active_sessions() {
    return {.items = {{.id = "7",
                       .is_current = false,
                       .is_password_pending = false,
                       .is_unconfirmed = false,
                       .can_accept_secret_chats = true,
                       .can_accept_calls = true,
                       .device_type = core::TdSessionDeviceType::Linux,
                       .api_id = 123,
                       .application_name = "Telegram Desktop",
                       .application_version = "5.1",
                       .is_official_application = true,
                       .device_model = "ThinkPad",
                       .platform = "Linux",
                       .system_version = "6.10",
                       .log_in_date = "2023-11-14T22:13:20Z",
                       .last_active_date = "2023-11-14T22:13:21Z",
                       .ip_address = "192.0.2.1",
                       .location = "Test"}},
            .inactive_session_ttl_days = 30};
}

core::TdM6MemberStatus chat_admin_status(bool promote = false) {
    core::TdM6MemberStatus status;
    status.kind = core::TdM6MemberStatusKind::Administrator;
    status.can_be_edited = true;
    status.rights.can_manage_chat = true;
    status.rights.can_change_info = true;
    status.rights.can_invite_users = true;
    status.rights.can_restrict_members = true;
    status.rights.can_promote_members = promote;
    return status;
}

proto::Request delete_request(std::int64_t message_id = 101,
                              std::optional<std::string> key = std::nullopt, bool yes = true) {
    proto::Request request("main");
    request.id = 42;
    request.command = {"msg", "delete"};
    request.args = {
        {"chat", "-1001"}, {"message_ids", json::array({message_id})}, {"for_all", false}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.yes = yes;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request forward_request(std::optional<std::string> key = std::nullopt,
                               bool dry_run = false) {
    proto::Request request("main");
    request.id = 48;
    request.command = {"msg", "forward"};
    request.args = {{"from", "-1001"},
                    {"to", "-1002"},
                    {"message_ids", json::array({1, 2})},
                    {"drop_author", false}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.idempotency_key = std::move(key);
    request.context.dry_run = dry_run;
    return request;
}

proto::Request edit_request(std::string text = "revised",
                            std::optional<std::string> key = std::nullopt, bool dry_run = false) {
    proto::Request request("main");
    request.id = 43;
    request.command = {"msg", "edit"};
    request.args = {{"chat", "-1001"}, {"message_id", 101}, {"text", std::move(text)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.idempotency_key = std::move(key);
    request.context.dry_run = dry_run;
    return request;
}

proto::Request react_request(bool remove = false, bool big = false,
                             std::optional<std::string> key = std::nullopt) {
    proto::Request request("main");
    request.id = 44;
    request.command = {"msg", "react"};
    request.args = {{"chat", "-1001"},
                    {"message_id", 101},
                    {"reaction", "👍"},
                    {"remove", remove},
                    {"big", big}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request message_pin_request(bool pinned, std::optional<std::string> key = std::nullopt) {
    proto::Request request("main");
    request.id = 45;
    request.command = {"msg", pinned ? "pin" : "unpin"};
    request.args = {{"chat", "-1001"}, {"message_id", 101}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.idempotency_key = std::move(key);
    return request;
}

proto::Request chat_target_request(std::string subcommand, std::string chat = "-1001",
                                   std::optional<std::string> key = std::nullopt,
                                   bool dry_run = false, bool yes = false) {
    proto::Request request("main");
    request.id = 46;
    request.command = {"chat", std::move(subcommand)};
    request.args = {{"chat", std::move(chat)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.idempotency_key = std::move(key);
    request.context.dry_run = dry_run;
    request.context.yes = yes;
    return request;
}

proto::Request chat_mute_request(bool muted, std::int32_t duration,
                                 std::optional<std::string> key = std::nullopt,
                                 bool dry_run = false) {
    auto request = chat_target_request(muted ? "mute" : "unmute", "-1001", std::move(key), dry_run);
    request.args["duration_seconds"] = duration;
    return request;
}

proto::Request chat_join_request(std::string target, std::optional<std::string> key = std::nullopt,
                                 bool dry_run = false, double timeout_seconds = 2.0,
                                 secure::WipeObserver wipe_observer = {}) {
    proto::Request request("main", std::move(wipe_observer));
    request.id = 47;
    request.command = {"chat", "join"};
    request.args = {{"target", std::move(target)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = timeout_seconds;
    request.context.idempotency_key = std::move(key);
    request.context.dry_run = dry_run;
    return request;
}

core::TdMessageWriteResult edited_message(std::string text = "revised") {
    return {.id = 101,
            .chat_id = -1001,
            .date = 1'785'924'000,
            .sender = {.kind = core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
            .is_outgoing = true,
            .topic = std::nullopt,
            .content_kind = core::TdMessageContentKind::Text,
            .text = std::move(text),
            .scheduled = false};
}

core::TdWriteMessage failed_forward_message(std::int64_t id, std::int32_t code,
                                            double retry_after) {
    auto message = forwarded_message(id);
    message.sending_state = {.kind = core::TdMessageSendingStateKind::Failed,
                             .sending_id = 0,
                             .error = core::TdError{code, "failed"},
                             .can_retry = false,
                             .need_another_sender = false,
                             .need_another_reply_quote = false,
                             .need_drop_reply = false,
                             .required_paid_message_star_count = 0,
                             .retry_after = retry_after,
                             .unsupported_tdlib_type_id = std::nullopt};
    return message;
}

core::TdWriteMessage pending_forward_message(std::int64_t id, std::int32_t sending_id) {
    auto message = forwarded_message(id);
    message.sending_state = {.kind = core::TdMessageSendingStateKind::Pending,
                             .sending_id = sending_id,
                             .error = std::nullopt,
                             .can_retry = false,
                             .need_another_sender = false,
                             .need_another_reply_quote = false,
                             .need_drop_reply = false,
                             .required_paid_message_star_count = 0,
                             .retry_after = 0,
                             .unsupported_tdlib_type_id = std::nullopt};
    return message;
}

core::TdMessageAvailableReactions available_reactions() {
    return {.top = {{.type = {.kind = core::TdReactionKind::Emoji,
                              .emoji = "👍",
                              .custom_emoji_id = 0,
                              .tdlib_type_id = 1},
                     .needs_premium = false}},
            .recent = {},
            .popular = {},
            .allow_custom_emoji = false,
            .are_tags = false,
            .unavailability_reason = core::TdReactionUnavailabilityReason::None,
            .unsupported_unavailability_tdlib_type_id = std::nullopt};
}

void resolve_basic(FakeWrites& fake) {
    fake.respond(core::TdFunctionKind::GetMe, self());
    fake.respond(core::TdFunctionKind::GetChat, basic_chat());
}

void bind_principal(FakeWrites& fake) {
    fake.respond(core::TdFunctionKind::GetMe, self());
}

void bind_saved_identity(FakeWrites& fake) {
    bind_principal(fake);
    fake.respond(core::TdFunctionKind::CreatePrivateChat, private_chat(42));
    fake.respond(core::TdFunctionKind::GetUser, self());
}

bool try_bind_saved_identity_after_principal(FakeWrites& fake) {
    if (!fake.try_respond(core::TdFunctionKind::CreatePrivateChat, private_chat(42))) {
        return false;
    }
    fake.respond(core::TdFunctionKind::GetUser, self());
    return true;
}

void plan_saved_attachment(FakeWrites& fake, core::TdMessageProperties properties = {},
                           std::optional<core::TdTopic> topic = core::TdTopic{
                               .kind = core::TdTopicKind::Saved, .id = 19, .tdlib_type_id = 1}) {
    bind_saved_identity(fake);
    fake.respond(core::TdFunctionKind::GetMessage, saved_planning_message(-77, topic));
    properties.can_be_replied = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
}

void plan_forward(FakeWrites& fake, bool drop_author = false,
                  core::TdMessageProperties properties = {}) {
    bind_principal(fake);
    fake.respond(core::TdFunctionKind::GetChat, basic_chat());
    auto destination = basic_chat();
    destination.id = -1002;
    destination.title = "Destination";
    fake.respond(core::TdFunctionKind::GetChat, destination);
    if (drop_author) {
        properties.can_be_copied = true;
    } else {
        properties.can_be_forwarded = true;
    }
    for (const auto id : {1, 2}) {
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(id));
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    }
}

std::uint64_t reach_invite_dispatch(FakeWrites& fake, const std::string& invite) {
    bind_principal(fake);
    fake.respond(core::TdFunctionKind::GetInternalLinkType,
                 core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                      .username = {},
                                      .url = invite,
                                      .tdlib_type_id = 1});
    fake.respond(core::TdFunctionKind::CheckChatInviteLink,
                 core::TdChatInviteLinkInfo{.chat_id = 0, .is_public = false});
    return fake.observe_query(core::TdFunctionKind::JoinChatByInviteLink);
}

std::unique_ptr<core::TdLogSink> invite_log_sink(FakeWrites& fake) {
    const auto log_directory = fake.tree().account_state() + "/logs";
    REQUIRE(std::filesystem::create_directory(log_directory));
    REQUIRE(::chmod(log_directory.c_str(), 0700) == 0);
    std::string error;
    auto sink = core::TdLogSink::create(
        {.file_path = log_directory + "/tdlib.log", .max_file_size = 96}, ::getuid(), error);
    INFO(error);
    REQUIRE(sink);
    return sink;
}

void append_invite_and_check_logs(core::TdLogSink& sink,
                                  const std::vector<std::string>& protected_values) {
    for (int index = 0; index < 8; ++index) {
        std::string append_error;
        std::string record;
        for (const auto& value : protected_values) {
            record.append(value);
            record.push_back(':');
        }
        record.append("late\n");
        REQUIRE(sink.append(1, record, append_error));
        INFO(append_error);
    }
    for (const auto& filename : sink.log_paths()) {
        const auto bytes = read_bytes(filename);
        for (const auto& value : protected_values) {
            CHECK(bytes.find(value) == std::string::npos);
        }
    }
}

void wait_for_invite_release(std::string_view invite) {
    const auto release_deadline = std::chrono::steady_clock::now() + 2s;
    while (redaction::InviteLinkRegistry::instance().redact(invite) != invite &&
           std::chrono::steady_clock::now() < release_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(redaction::InviteLinkRegistry::instance().redact(invite) == invite);
}

void check_closed_without_pending(FakeWrites& fake) {
    auto guard = fake.foundation()->acquire_epoch();
    CHECK(fake.foundation()->audit().inspect(guard).status ==
          daemon::AccountAuditInspectionStatus::Clean);
    const auto store = fake.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    CHECK(store.snapshot.entries.empty());
    CHECK(fake.foundation()->run_core_gate(guard, 1'800'000'000).status ==
          daemon::IdempotencyCoreGateStatus::Clean);
}

void check_completed_possible_with_pending(FakeWrites& fake) {
    auto guard = fake.foundation()->acquire_epoch();
    CHECK(fake.foundation()->audit().inspect(guard).status ==
          daemon::AccountAuditInspectionStatus::Clean);
    const auto store = fake.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(store.snapshot.entries.size() == 1);
    CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
    CHECK(fake.foundation()->run_core_gate(guard, 1'800'000'000).status ==
          daemon::IdempotencyCoreGateStatus::Clean);
}

void check_open_dispatch_with_pending(FakeWrites& fake, bool exercise_next_gate = true) {
    auto guard = fake.foundation()->acquire_epoch();
    const auto audit = fake.foundation()->audit().inspect(guard);
    CHECK(audit.status == daemon::AccountAuditInspectionStatus::Open);
    REQUIRE(audit.oldest_open);
    CHECK(audit.oldest_open->dispatch_started);
    const auto store = fake.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(store.snapshot.entries.size() == 1);
    CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
    if (!exercise_next_gate) {
        return;
    }
    const auto gate = fake.foundation()->run_core_gate(guard, 1'800'000'000);
    CHECK(gate.status == daemon::IdempotencyCoreGateStatus::AuditIncomplete);
    REQUIRE(gate.terminal);
    CHECK((*gate.terminal)["code"] == "AUDIT_INCOMPLETE");
    CHECK((*gate.terminal)["details"]["mutation_state"] == "possible");
    CHECK((*gate.terminal)["details"]["completed_stages"] ==
          json::array({"idempotency_pending", "dispatch_started"}));
}

} // namespace

TEST_CASE("saved attach materializes one private spool document and cleans it after success",
          "[write-command][saved-attach][spool][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("attachment bytes\n");
    const auto source_path = fake.tree().source_path();
    auto pending = fake.dispatch(saved_attach_request(fake.tree()));
    plan_saved_attachment(fake);
    const auto descriptor =
        fake.respond(core::TdFunctionKind::SendMessage, saved_document_message());
    const auto outcome = pending.get();

    REQUIRE(outcome.result);
    CHECK_THAT(*outcome.result,
               tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    CHECK((*outcome.result)["id"] == 202);
    CHECK((*outcome.result)["chat_id"] == 42);
    CHECK((*outcome.result)["type"] == "doc");
    CHECK(function_field<std::string>(descriptor, "content_kind") == "document");
    CHECK(function_field<std::string>(descriptor, "text") == "experiment result");
    const auto& local_path = function_field<std::string>(descriptor, "document_local_path");
    CHECK(local_path != source_path);
    CHECK(local_path.starts_with(fake.tree().account_state() + "/spool/"));
    CHECK(function_field<bool>(descriptor, "thumbnail_is_null"));
    CHECK(function_field<bool>(descriptor, "disable_content_type_detection"));
    CHECK(read_bytes(source_path) == "attachment bytes\n");
    std::size_t spooled_files = 0;
    const auto spool_root = fake.tree().account_state() + "/spool";
    if (std::filesystem::exists(spool_root)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(spool_root)) {
            spooled_files += entry.is_regular_file() ? 1U : 0U;
        }
    }
    CHECK(spooled_files == 0);
}

TEST_CASE("saved attach dry-run hashes and plans without creating durable state",
          "[write-command][saved-attach][dry-run][spool][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("dry bytes");
    auto pending = fake.dispatch(saved_attach_request(fake.tree(), true, std::nullopt));
    plan_saved_attachment(fake);
    const auto outcome = pending.get();

    REQUIRE(outcome.result);
    CHECK_THAT(*outcome.result,
               tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    CHECK((*outcome.result)["dry_run"] == true);
    CHECK((*outcome.result)["plan"]["operation"] == "saved_attach");
    CHECK((*outcome.result)["plan"]["message_id"] == -77);
    CHECK((*outcome.result)["plan"]["effective_topic"] == json{{"kind", "saved"}, {"id", 19}});
    CHECK((*outcome.result)["plan"]["file"]["size"] == 9);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
}

TEST_CASE("saved attach preserves a null root Saved topic",
          "[write-command][saved-attach][topic][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("root attachment");
    auto pending = fake.dispatch(saved_attach_request(fake.tree(), false, std::nullopt));
    plan_saved_attachment(fake, {}, std::nullopt);
    auto written = saved_document_message();
    written.message.topic.reset();
    const auto descriptor = fake.respond(core::TdFunctionKind::SendMessage, std::move(written));
    const auto outcome = pending.get();

    REQUIRE(outcome.result);
    CHECK((*outcome.result)["topic"] == nullptr);
    CHECK(function_field<std::string>(descriptor, "topic_kind") == "none");
}

TEST_CASE("saved attach replays and conflicts after principal-bound Saved materialization",
          "[write-command][saved-attach][idempotency][replay][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("replay attachment");
    const auto request = saved_attach_request(fake.tree());
    auto first_pending = fake.dispatch(request);
    plan_saved_attachment(fake);
    fake.respond(core::TdFunctionKind::SendMessage, saved_document_message());
    const auto first = first_pending.get();
    REQUIRE(first.result);

    auto replay_pending = fake.dispatch(request);
    bind_principal(fake);
    const bool replay_resolver_attempted =
        fake.try_respond(core::TdFunctionKind::CreatePrivateChat,
                         core::TdError{.code = 500, .message = "transient resolver failure"});
    const auto replay = replay_pending.get();
    REQUIRE(replay.result);
    CHECK(replay.result == first.result);
    CHECK_FALSE(replay_resolver_attempted);

    auto conflict_request = request;
    conflict_request.args["caption"] = "different caption";
    auto conflict_pending = fake.dispatch(conflict_request);
    bind_principal(fake);
    const bool conflict_resolver_attempted = try_bind_saved_identity_after_principal(fake);
    const auto conflict = conflict_pending.get();
    REQUIRE(conflict.error);
    CHECK((*conflict.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK_FALSE(conflict_resolver_attempted);
    CHECK(fake.count(core::TdFunctionKind::CreatePrivateChat) == 1);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
}

TEST_CASE("saved attach rejects a malformed authoritative document result after mutation proof",
          "[write-command][saved-attach][result][idempotency][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("attachment bytes");
    auto pending = fake.dispatch(saved_attach_request(fake.tree()));
    plan_saved_attachment(fake);
    fake.respond(core::TdFunctionKind::SendMessage, saved_document_message("wrong caption"));
    const auto outcome = pending.get();

    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    auto guard = fake.foundation()->acquire_epoch();
    const auto store = fake.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(store.snapshot.entries.size() == 1);
    CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    REQUIRE(store.snapshot.entries.front().terminal);
    CHECK((*store.snapshot.entries.front().terminal)["code"] == "INTERNAL");
}

TEST_CASE("saved attach rejects bots before and missing input after Saved identity binding",
          "[write-command][saved-attach][preflight][fake-boundary]") {
    SECTION("bot") {
        FakeWrites fake;
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        fake.respond(core::TdFunctionKind::GetMe, peer(core::TdUserPresence::Online, true, 42));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK(fake.count(core::TdFunctionKind::CreatePrivateChat) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("missing source") {
        FakeWrites fake;
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_principal(fake);
        const bool resolver_attempted = try_bind_saved_identity_after_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
        CHECK((*outcome.error)["error"]["message"] == "input file is unavailable");
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "saved_attach"},
                                                           {"path", fake.tree().source_path()},
                                                           {"reason", "missing"}});
        CHECK_FALSE(resolver_attempted);
        CHECK(fake.count(core::TdFunctionKind::CreatePrivateChat) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetUser) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }
}

TEST_CASE("saved attach binds its private Saved chat to the authenticated principal",
          "[write-command][saved-attach][principal-binding][fake-boundary]") {
    const auto exercise = [](const auto& respond_invalid_identity) {
        FakeWrites fake;
        fake.tree().write_source("principal-bound bytes");
        auto pass_one_reads = std::make_shared<std::atomic<std::size_t>>(0);
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->read = [pass_one_reads](daemon::FileSpoolIo operation, int descriptor, void* bytes,
                                       std::size_t size) {
            if (operation == daemon::FileSpoolIo::Pass1Read) {
                ++*pass_one_reads;
            }
            return ::read(descriptor, bytes, size);
        };
        fake.file_spool_hooks(hooks);

        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_principal(fake);
        respond_invalid_identity(fake);
        const auto outcome = pending.get();

        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK((*outcome.error)["error"]["details"]["operation"] == "resolve");
        CHECK(*pass_one_reads > 0);
        CHECK(fake.count(core::TdFunctionKind::GetMessage) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetMessageProperties) == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK(read_bytes(fake.tree().source_path()) == "principal-bound bytes");
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
        check_closed_without_pending(fake);
    };

    SECTION("malformed createPrivateChat response") {
        exercise([](FakeWrites& fake) {
            fake.respond(core::TdFunctionKind::CreatePrivateChat, core::TdOk{});
        });
    }
    SECTION("non-private createPrivateChat response") {
        exercise([](FakeWrites& fake) {
            fake.respond(core::TdFunctionKind::CreatePrivateChat, basic_chat());
            static_cast<void>(
                fake.try_respond(core::TdFunctionKind::GetMessage, saved_planning_message(-77)));
        });
    }
    SECTION("private chat for another user") {
        exercise([](FakeWrites& fake) {
            fake.respond(core::TdFunctionKind::CreatePrivateChat, private_chat(77));
            if (fake.try_respond(core::TdFunctionKind::GetUser,
                                 peer(core::TdUserPresence::Online))) {
                static_cast<void>(fake.try_respond(core::TdFunctionKind::GetMessage,
                                                   saved_planning_message(-77)));
            }
        });
    }
    SECTION("private user response mismatches the principal") {
        exercise([](FakeWrites& fake) {
            fake.respond(core::TdFunctionKind::CreatePrivateChat, private_chat(42));
            fake.respond(core::TdFunctionKind::GetUser,
                         peer(core::TdUserPresence::Online, false, 77));
        });
    }
    SECTION("private user classification is bot") {
        exercise([](FakeWrites& fake) {
            fake.respond(core::TdFunctionKind::CreatePrivateChat, private_chat(42));
            fake.respond(core::TdFunctionKind::GetUser,
                         peer(core::TdUserPresence::Online, true, 42));
            static_cast<void>(
                fake.try_respond(core::TdFunctionKind::GetMessage, planning_message(-77)));
        });
    }
}

TEST_CASE("saved attach resolver failures retain Resolve attribution",
          "[write-command][saved-attach][resolver-attribution][fake-boundary]") {
    for (const bool rate_limited : {false, true}) {
        CAPTURE(rate_limited);
        FakeWrites fake;
        fake.tree().write_source("resolver failure bytes");
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::CreatePrivateChat,
                     core::TdError{.code = rate_limited ? 429 : 500,
                                   .message = rate_limited ? "retry after 6" : "resolver failed"});
        const auto outcome = pending.get();

        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK((*outcome.error)["error"]["code"] == (rate_limited ? "RATE_LIMITED" : "TDLIB_ERROR"));
        CHECK((*outcome.error)["error"]["details"]["operation"] == "resolve");
        if (rate_limited) {
            CHECK((*outcome.error)["error"]["details"]["retry_after"] == 6);
        }
        CHECK(fake.count(core::TdFunctionKind::GetMessage) == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
        check_closed_without_pending(fake);
    }
}

TEST_CASE("saved attach spool contradiction precedes source and Saved materialization",
          "[write-command][saved-attach][spool][contradiction][ordering][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("contradiction bytes");
    const auto spool = fake.tree().account_state() + "/spool";
    const auto contradiction = spool + "/not-an-invocation";
    REQUIRE(std::filesystem::create_directory(spool));
    REQUIRE(::chmod(spool.c_str(), 0700) == 0);
    REQUIRE(std::filesystem::create_directory(contradiction));
    REQUIRE(::chmod(contradiction.c_str(), 0700) == 0);
    auto pass_one_reads = std::make_shared<std::atomic<std::size_t>>(0);
    auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
    hooks->read = [pass_one_reads](daemon::FileSpoolIo operation, int descriptor, void* bytes,
                                   std::size_t size) {
        if (operation == daemon::FileSpoolIo::Pass1Read) {
            ++*pass_one_reads;
        }
        return ::read(descriptor, bytes, size);
    };
    fake.file_spool_hooks(hooks);

    auto pending = fake.dispatch(saved_attach_request(fake.tree()));
    bind_principal(fake);
    const bool resolver_attempted = try_bind_saved_identity_after_principal(fake);
    const auto outcome = pending.get();

    REQUIRE(outcome.error);
    const auto diagnostic = daemon::encode_filesystem_diagnostic_path(contradiction);
    REQUIRE(diagnostic);
    CHECK(*outcome.error ==
          json{{"error",
                {{"code", "AUDIT_INCOMPLETE"},
                 {"message", "attachment spool recovery is incomplete"},
                 {"details",
                  {{"account", "main"},
                   {"path", {{"kind", "bytes_hex"}, {"value", diagnostic->bytes_hex}}},
                   {"mutation_state", "none"},
                   {"completed_stages", json::array()}}}}}});
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    CHECK_FALSE(resolver_attempted);
    CHECK(*pass_one_reads == 0);
    CHECK(fake.count(core::TdFunctionKind::CreatePrivateChat) == 0);
    CHECK(fake.count(core::TdFunctionKind::GetUser) == 0);
    CHECK(fake.count(core::TdFunctionKind::GetMessage) == 0);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    CHECK(read_bytes(fake.tree().source_path()) == "contradiction bytes");
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
}

TEST_CASE("saved attach maps pass-one spool failures to their exact public terminal",
          "[write-command][saved-attach][spool][pass-one][fake-boundary]") {
    const auto exercise = [](std::shared_ptr<daemon::testing::FileSpoolHooks> hooks,
                             std::string_view reason) {
        FakeWrites fake;
        fake.tree().write_source("pass-one bytes");
        fake.file_spool_hooks(std::move(hooks));

        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_principal(fake);
        const bool resolver_attempted = try_bind_saved_identity_after_principal(fake);
        const auto outcome = pending.get();

        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK((*outcome.error)["error"] == json{{"code", "SPOOL_UNAVAILABLE"},
                                                {"message", "attachment spool is unavailable"},
                                                {"details",
                                                 {{"operation", "saved_attach"},
                                                  {"path", fake.tree().source_path()},
                                                  {"reason", reason}}}});
        CHECK_FALSE(resolver_attempted);
        CHECK(fake.count(core::TdFunctionKind::CreatePrivateChat) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetUser) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetMessage) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetMessageProperties) == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK(read_bytes(fake.tree().source_path()) == "pass-one bytes");
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
        check_closed_without_pending(fake);
    };

    SECTION("open failure") {
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->should_fail = [](daemon::FileSpoolStage stage) {
            return stage == daemon::FileSpoolStage::BeforeSourceEntryStat;
        };
        exercise(std::move(hooks), "open_failed");
    }
    SECTION("read EIO") {
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->read = [](daemon::FileSpoolIo operation, int descriptor, void* bytes,
                         std::size_t size) -> ssize_t {
            if (operation == daemon::FileSpoolIo::Pass1Read) {
                errno = EIO;
                return -1;
            }
            return ::read(descriptor, bytes, size);
        };
        exercise(std::move(hooks), "read_failed");
    }
    SECTION("timestamp representation failure") {
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->mutate_metadata = [](daemon::FileSpoolMetadata point, struct stat& status) {
            if (point == daemon::FileSpoolMetadata::SourceBefore) {
#if defined(__APPLE__)
                status.st_mtimespec.tv_sec = std::numeric_limits<time_t>::max();
                status.st_mtimespec.tv_nsec = 999'999'999;
#else
                status.st_mtim.tv_sec = std::numeric_limits<time_t>::max();
                status.st_mtim.tv_nsec = 999'999'999;
#endif
            }
        };
        exercise(std::move(hooks), "schema_error");
    }
}

TEST_CASE("saved attach keeps replyability content topic and missing-original failures distinct",
          "[write-command][saved-attach][precondition][fake-boundary]") {
    const auto check_closed = [](FakeWrites& fake, const Outcome& outcome) {
        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
        check_closed_without_pending(fake);
    };

    SECTION("text is not replyable") {
        FakeWrites fake;
        fake.tree().write_source("not replyable");
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_saved_identity(fake);
        fake.respond(core::TdFunctionKind::GetMessage, saved_planning_message(-77));
        fake.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
        const auto outcome = pending.get();
        check_closed(fake, outcome);
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "saved_attach"},
                                                           {"chat_id", 42},
                                                           {"message_id", -77},
                                                           {"reason", "not_replyable"}});
    }
    SECTION("wrong content type") {
        FakeWrites fake;
        fake.tree().write_source("wrong content");
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_saved_identity(fake);
        auto message = saved_planning_message(-77);
        message.content_kind = core::TdMessageContentKind::Document;
        fake.respond(core::TdFunctionKind::GetMessage, std::move(message));
        core::TdMessageProperties properties;
        properties.can_be_replied = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        check_closed(fake, outcome);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "wrong_content_type");
    }
    SECTION("wrong topic") {
        FakeWrites fake;
        fake.tree().write_source("wrong topic");
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_saved_identity(fake);
        fake.respond(core::TdFunctionKind::GetMessage,
                     saved_planning_message(-77, core::TdTopic{.kind = core::TdTopicKind::Forum,
                                                               .id = 19,
                                                               .tdlib_type_id = 1}));
        core::TdMessageProperties properties;
        properties.can_be_replied = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        check_closed(fake, outcome);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "wrong_topic");
    }
    SECTION("missing original") {
        FakeWrites fake;
        fake.tree().write_source("missing original");
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        bind_saved_identity(fake);
        fake.respond(core::TdFunctionKind::GetMessage,
                     core::TdError{.code = 404, .message = "message not found"});
        const auto outcome = pending.get();
        check_closed(fake, outcome);
        CHECK((*outcome.error)["error"] ==
              json{{"code", "NOT_FOUND"},
                   {"message", "message was not found"},
                   {"details", {{"chat_id", 42}, {"message_id", -77}}}});
        CHECK(fake.count(core::TdFunctionKind::GetMessageProperties) == 0);
    }
}

TEST_CASE("saved attach closes a keyed pass-two source race without dispatch",
          "[write-command][saved-attach][input-changed][spool][idempotency][fake-boundary]") {
    FakeWrites fake;
    fake.tree().write_source("first bytes");
    auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
    auto changed = std::make_shared<std::atomic<bool>>(false);
    const auto source_path = fake.tree().source_path();
    hooks->read = [changed, source_path](daemon::FileSpoolIo operation, int descriptor, void* bytes,
                                         std::size_t size) {
        if (operation == daemon::FileSpoolIo::Pass2Read && !changed->exchange(true)) {
            std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
            output << "second bytes";
        }
        return ::read(descriptor, bytes, size);
    };
    fake.file_spool_hooks(hooks);
    auto pending = fake.dispatch(saved_attach_request(fake.tree()));
    plan_saved_attachment(fake);
    const auto outcome = pending.get();

    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "INPUT_CHANGED");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"operation", "saved_attach"}, {"path", source_path}});
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    check_closed_without_pending(fake);
}

TEST_CASE("saved attach closes cancelled and unsynced pass-two materialization",
          "[write-command][saved-attach][spool][cancel][fault][idempotency][fake-boundary]") {
    SECTION("cancelled") {
        FakeWrites fake;
        fake.tree().write_source("cancel bytes");
        std::shared_ptr<daemon::RequestSession> session;
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->at_stage = [&session](daemon::FileSpoolStage stage) {
            if (stage == daemon::FileSpoolStage::DuringPass2Read) {
                session->disconnect();
            }
        };
        fake.file_spool_hooks(hooks);
        auto pending = fake.dispatch(saved_attach_request(fake.tree()), &session);
        plan_saved_attachment(fake);
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        check_closed_without_pending(fake);
    }

    SECTION("file sync failure") {
        FakeWrites fake;
        fake.tree().write_source("sync bytes");
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->should_fail = [](daemon::FileSpoolStage stage) {
            return stage == daemon::FileSpoolStage::BeforeFileSync;
        };
        fake.file_spool_hooks(hooks);
        auto pending = fake.dispatch(saved_attach_request(fake.tree()));
        plan_saved_attachment(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK((*outcome.error)["error"] == json{{"code", "SPOOL_UNAVAILABLE"},
                                                {"message", "attachment spool is unavailable"},
                                                {"details",
                                                 {{"operation", "saved_attach"},
                                                  {"path", fake.tree().source_path()},
                                                  {"reason", "sync_failed"}}}});
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        check_closed_without_pending(fake);
    }
}

TEST_CASE("send text topic and RFC3339 schedule normalization is closed at boundaries",
          "[write-command][send][domain]") {
    CHECK(daemon::valid_send_text("x"));
    CHECK(daemon::valid_send_text(std::string(4096, 'x')));
    CHECK_FALSE(daemon::valid_send_text(std::string(4097, 'x')));
    std::string emoji;
    emoji.reserve(static_cast<std::size_t>(4096) * 4);
    for (std::size_t index = 0; index < 4096; ++index) {
        emoji.append("\xF0\x9F\x98\x80", 4);
    }
    CHECK(daemon::valid_send_text(emoji));
    emoji.append("\xF0\x9F\x98\x80", 4);
    CHECK_FALSE(daemon::valid_send_text(emoji));
    CHECK_FALSE(daemon::valid_send_text(std::string("a\0b", 3)));
    CHECK_FALSE(daemon::valid_send_text(std::string("\xF0\x28\x8C\x28", 4)));
    CHECK(daemon::valid_saved_attach_caption(""));
    std::string caption;
    for (std::size_t index = 0; index < 1'024; ++index) {
        caption.append("\xF0\x9F\x98\x80", 4);
    }
    CHECK(daemon::valid_saved_attach_caption(caption));
    caption.append("\xF0\x9F\x98\x80", 4);
    CHECK_FALSE(daemon::valid_saved_attach_caption(caption));
    CHECK_FALSE(daemon::valid_saved_attach_caption(std::string("a\0b", 3)));

    const auto bare_topic = daemon::parse_send_topic("2147483647");
    REQUIRE(bare_topic);
    CHECK(bare_topic->kind == daemon::TopicKind::Forum);
    CHECK(bare_topic->id == 2147483647);
    CHECK(daemon::parse_send_topic("forum:7") ==
          daemon::TopicRef{.kind = daemon::TopicKind::Forum, .id = 7});
    for (const auto* invalid : {"0", "2147483648", "forum:0", "thread:1", "+1", "01"}) {
        INFO(invalid);
        CHECK_FALSE(daemon::parse_send_topic(invalid));
    }

    const auto online = daemon::parse_send_schedule("online");
    REQUIRE(online);
    CHECK(online->kind == daemon::SendScheduleKind::Online);
    CHECK(online->send_date == 0);
    const auto epoch = daemon::parse_send_schedule("1970-01-01T00:00:01Z");
    REQUIRE(epoch);
    CHECK(epoch->kind == daemon::SendScheduleKind::At);
    CHECK(epoch->send_date == 1);
    CHECK(daemon::parse_send_schedule("1970-01-01T01:00:01+01:00") == epoch);
    const auto ceiling = daemon::parse_send_schedule("1970-01-01T00:00:01.000000001Z");
    REQUIRE(ceiling);
    CHECK(ceiling->send_date == 2);
    const auto maximum = daemon::parse_send_schedule("2038-01-19T03:14:07Z");
    REQUIRE(maximum);
    CHECK(maximum->send_date == std::numeric_limits<std::int32_t>::max());
    for (const auto* invalid :
         {"1970-01-01T00:00:00Z", "2038-01-19T03:14:08Z", "2026-02-29T12:00:00Z",
          "2026-08-21T12:00:00", "2026-08-21T12:00:60Z", "Online"}) {
        INFO(invalid);
        CHECK_FALSE(daemon::parse_send_schedule(invalid));
    }
}

TEST_CASE("public send dry-run plans through the read boundary without durable writes",
          "[write-command][send][dry-run][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("hello", true, std::nullopt);
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.terminal_count == 1);
    CHECK((*outcome.result)["dry_run"] == true);
    CHECK((*outcome.result)["plan"]["operation"] == "send");
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("send.result.schema.json"));
}

TEST_CASE("public send authority is frozen while dry-run remains read-only",
          "[write-command][send][authority][dry-run][fake-boundary]") {
    SECTION("standing deny rejects after principal binding") {
        FakeWrites fake(false);
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt));
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "WRITE_DENIED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "no_grant");
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("request grant authorizes a real mutation") {
        FakeWrites fake(false);
        auto request = send_request("hello", false, std::nullopt);
        request.context.write_authority = proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::SendMessage, stable_message());
        REQUIRE(pending.get().result);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    }

    SECTION("explicit request deny wins over a standing grant") {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.context.write_authority = proto::WriteAuthority::Deny;
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "WRITE_DENIED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "explicit_deny");
    }

    SECTION("dry-run plans despite standing deny and creates no write artifacts") {
        FakeWrites fake(false);
        auto pending = fake.dispatch(send_request("hello", true, std::nullopt));
        resolve_basic(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }
}

TEST_CASE("public send enforces the +10/+11 schedule boundary again before dispatch",
          "[write-command][send][schedule][fake-boundary]") {
    constexpr std::int32_t send_date = 2'000'000'000;
    SECTION("planning rejects equality at ten seconds") {
        FakeWrites fake;
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt, send_date));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 10});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "schedule_window_elapsed");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("commit recheck closes an elapsed plan without mutation") {
        FakeWrites fake;
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt, send_date));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 10});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK(std::filesystem::exists(fake.tree().audit_path()));
        check_closed_without_pending(fake);
    }

    SECTION("commit recheck handles the complete signed server-time domain") {
        for (const auto& [server_time, reason] :
             std::vector<std::pair<std::int64_t, std::string_view>>{
                 {std::numeric_limits<std::int64_t>::min(), "schedule_too_far"},
                 {std::numeric_limits<std::int64_t>::max(), "schedule_window_elapsed"}}) {
            CAPTURE(server_time, reason);
            FakeWrites fake;
            auto pending =
                fake.dispatch(send_request("hello", false, "schedule-extreme-key", send_date));
            resolve_basic(fake);
            fake.respond(core::TdFunctionKind::GetOption,
                         core::TdOptionInteger{.value = send_date - 11});
            fake.respond(core::TdFunctionKind::GetOption,
                         core::TdOptionInteger{.value = server_time});
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
            CHECK((*outcome.error)["error"]["details"]["reason"] == reason);
            CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
            check_closed_without_pending(fake);
        }
    }

    SECTION("commit recheck closes malformed and null unix-time responses") {
        for (const bool null_response : {false, true}) {
            CAPTURE(null_response);
            FakeWrites fake;
            auto pending =
                fake.dispatch(send_request("hello", false, "schedule-malformed-key", send_date));
            resolve_basic(fake);
            fake.respond(core::TdFunctionKind::GetOption,
                         core::TdOptionInteger{.value = send_date - 11});
            if (null_response) {
                fake.respond_value(core::TdFunctionKind::GetOption, {});
            } else {
                fake.respond(core::TdFunctionKind::GetOption,
                             core::TdDirectConversionError{.tdlib_type_id = 999});
            }
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
            CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
            check_closed_without_pending(fake);
        }
    }

    SECTION("eleven seconds at both reads dispatches a scheduled message") {
        FakeWrites fake;
        auto pending = fake.dispatch(send_request("hello", false, std::nullopt, send_date));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.respond(core::TdFunctionKind::SendMessage, stable_message("hello", send_date));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["scheduled"] == true);
        CHECK((*outcome.result)["date"] == nullptr);
        CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("send.result.schema.json"));
    }
}

TEST_CASE("pre-send deadline and cancellation close keyed schedule intents without mutation",
          "[write-command][send][schedule][deadline][cancellation][fake-boundary]") {
    constexpr std::int32_t send_date = 2'000'000'000;
    SECTION("deadline") {
        FakeWrites fake;
        auto request = send_request("hello", false, "schedule-deadline-key", send_date);
        request.context.timeout_seconds = 0.5;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.observe(core::TdFunctionKind::GetOption);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "preflight");
        CHECK((*outcome.error)["error"]["details"]["idempotency"] == "removed");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        check_closed_without_pending(fake);
    }

    SECTION("disconnect cancellation") {
        FakeWrites fake;
        auto request = send_request("hello", false, "schedule-cancel-key", send_date);
        std::shared_ptr<daemon::RequestSession> session;
        auto pending = fake.dispatch(request, &session);
        REQUIRE(session);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetOption,
                     core::TdOptionInteger{.value = send_date - 11});
        fake.observe(core::TdFunctionKind::GetOption);
        session->disconnect();
        const auto outcome = pending.get();
        CHECK(outcome.terminal_count == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        check_closed_without_pending(fake);
    }
}

TEST_CASE("online schedule requires a visible non-bot non-self private peer",
          "[write-command][send][online][fake-boundary]") {
    for (const auto presence : {core::TdUserPresence::Online, core::TdUserPresence::Offline}) {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["chat"] = "77";
        request.args["schedule"] = json{{"kind", "online"}};
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, private_chat(77));
        fake.respond(core::TdFunctionKind::GetUser, peer(presence));
        const auto descriptor = fake.respond(core::TdFunctionKind::SendMessage, online_message());
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["scheduled"] == true);
        CHECK((*outcome.result)["date"] == nullptr);
        CHECK(function_field<std::string>(descriptor, "schedule_kind") == "when_online");
    }

    for (const auto& [label, user_id, user] :
         std::vector<std::tuple<std::string, std::int64_t, core::TdUserSummary>>{
             {"hidden", 77, peer(core::TdUserPresence::Hidden)},
             {"bot", 77, peer(core::TdUserPresence::Online, true)},
             {"self", 42, peer(core::TdUserPresence::Online, false, 42)}}) {
        INFO(label);
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["chat"] = std::to_string(user_id);
        request.args["schedule"] = json{{"kind", "online"}};
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, private_chat(user_id));
        fake.respond(core::TdFunctionKind::GetUser, user);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "online_schedule_unsupported");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    }
}

TEST_CASE("send reply planning accepts signed ids and inherits only matching forum topics",
          "[write-command][send][reply][topic][fake-boundary]") {
    SECTION("signed reply inherits forum topic into the TD request") {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["reply_to"] = -7;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        auto reply = planning_message(-7);
        reply.topic = core::TdTopic{.kind = core::TdTopicKind::Forum, .id = 9, .tdlib_type_id = 1};
        fake.respond(core::TdFunctionKind::GetMessage, reply);
        core::TdMessageProperties properties;
        properties.can_be_replied = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        auto sent = stable_message();
        sent.message.topic = reply.topic;
        const auto descriptor = fake.respond(core::TdFunctionKind::SendMessage, sent);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(function_field<std::int64_t>(descriptor, "reply_to_message_id") == -7);
        CHECK(function_field<std::string>(descriptor, "topic_kind") == "forum");
        CHECK(function_field<std::int64_t>(descriptor, "topic_id") == 9);
    }

    SECTION("explicit topic mismatch fails before mutation") {
        FakeWrites fake;
        auto request = send_request("hello", false, std::nullopt);
        request.args["reply_to"] = -7;
        request.args["topic"] = json{{"kind", "forum"}, {"id", 8}};
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        auto reply = planning_message(-7);
        reply.topic = core::TdTopic{.kind = core::TdTopicKind::Forum, .id = 9, .tdlib_type_id = 1};
        fake.respond(core::TdFunctionKind::GetMessage, reply);
        core::TdMessageProperties properties;
        properties.can_be_replied = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "wrong_topic");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    }
}

TEST_CASE("Markdown send dispatches only the generation-bound parsed formatted text",
          "[write-command][send][format][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("**hello**", false, std::nullopt);
    request.args["parse_mode"] = "markdown_v2";
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::ParseTextEntities, fake.parsed_text("hello"));
    const auto descriptor =
        fake.respond(core::TdFunctionKind::SendMessage, stable_message("hello"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["text"] == "hello");
    CHECK(function_field<bool>(descriptor, "parsed"));
    CHECK(function_field<std::string>(descriptor, "text") == "hello");
}

TEST_CASE("parsed send text is validated before an intent or TD mutation",
          "[write-command][send][format][validation][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("**empty**", false, "parsed-empty-key");
    request.args["parse_mode"] = "markdown_v2";
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::ParseTextEntities, fake.parsed_text(""));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
}

TEST_CASE("pre-send coordinator rejection preserves no-mutation durability",
          "[write-command][send][delete][rejection][idempotency][fake-boundary]") {
    SECTION("send") {
        FakeWrites fake;
        fake.reject_before_request(core::TdFunctionKind::SendMessage);
        auto pending = fake.dispatch(send_request("hello", false, "send-rejected-key"));
        resolve_basic(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        check_closed_without_pending(fake);
    }

    SECTION("delete") {
        FakeWrites fake;
        fake.reject_before_request(core::TdFunctionKind::DeleteMessages);
        auto pending = fake.dispatch(delete_request(101, "delete-rejected-key"));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_only_for_self = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
        check_closed_without_pending(fake);
    }
}

TEST_CASE("prepared coordinator readiness failures close before a dispatch proof",
          "[write-command][send][delete][deadline][cancel][idempotency][fake-boundary]") {
    SECTION("delete deadline") {
        FakeWrites fake;
        fake.expire_direct_before_request();
        auto pending = fake.dispatch(delete_request(101, "delete-prepared-deadline-key"));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_only_for_self = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "preflight");
        CHECK((*outcome.error)["error"]["details"]["outcome"] == "not_started");
        CHECK((*outcome.error)["error"]["details"]["idempotency"] == "removed");
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
        check_closed_without_pending(fake);
    }

    for (const auto kind :
         {core::TdFunctionKind::SendMessage, core::TdFunctionKind::DeleteMessages}) {
        CAPTURE(core::td_function_name(kind));
        FakeWrites fake;
        std::shared_ptr<daemon::RequestSession> session;
        fake.cancel_before_request(kind, &session);
        auto pending =
            kind == core::TdFunctionKind::SendMessage
                ? fake.dispatch(send_request("hello", false, "prepared-cancel-key"), &session)
                : fake.dispatch(delete_request(101, "prepared-cancel-key"), &session);
        resolve_basic(fake);
        if (kind == core::TdFunctionKind::DeleteMessages) {
            fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
            core::TdMessageProperties properties;
            properties.can_be_deleted_only_for_self = true;
            fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        }
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.error);
        CHECK_FALSE(outcome.result);
        CHECK(outcome.terminal_count == 0);
        CHECK(fake.count(kind) == 0);
        check_closed_without_pending(fake);
    }
}

TEST_CASE("post-proof coordinator deadline and cancellation retain unknown keyed writes",
          "[write-command][send][delete][deadline][cancel][idempotency][fake-boundary]") {
    SECTION("delete deadline") {
        FakeWrites fake;
        fake.expire_direct_before_submit();
        auto pending = fake.dispatch(delete_request(101, "delete-direct-deadline-key"));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_only_for_self = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "dispatch");
        CHECK((*outcome.error)["error"]["details"]["outcome"] == "unknown");
        CHECK((*outcome.error)["error"]["details"]["idempotency"] == "pending");
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
        check_completed_possible_with_pending(fake);
    }

    SECTION("send cancellation") {
        FakeWrites fake;
        std::shared_ptr<daemon::RequestSession> session;
        fake.cancel_before_submit(core::TdFunctionKind::SendMessage, &session);
        auto pending =
            fake.dispatch(send_request("hello", false, "send-direct-cancel-key"), &session);
        resolve_basic(fake);
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.error);
        CHECK_FALSE(outcome.result);
        CHECK(outcome.terminal_count == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        check_open_dispatch_with_pending(fake, false);

        auto retry = fake.dispatch(send_request("hello", false, "send-direct-cancel-key"));
        bind_principal(fake);
        const auto retry_outcome = retry.get();
        REQUIRE(retry_outcome.error);
        REQUIRE((*retry_outcome.error)["error"]["code"] == "AUDIT_INCOMPLETE");
        CHECK((*retry_outcome.error)["error"]["details"]["mutation_state"] == "possible");
        CHECK((*retry_outcome.error)["error"]["details"]["completed_stages"] ==
              json::array({"idempotency_pending", "dispatch_started"}));
    }

    SECTION("delete cancellation") {
        FakeWrites fake;
        std::shared_ptr<daemon::RequestSession> session;
        fake.cancel_before_submit(core::TdFunctionKind::DeleteMessages, &session);
        auto pending = fake.dispatch(delete_request(101, "delete-direct-cancel-key"), &session);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_only_for_self = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.error);
        CHECK_FALSE(outcome.result);
        CHECK(outcome.terminal_count == 0);
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
        check_open_dispatch_with_pending(fake);
    }
}

TEST_CASE("msg delete rechecks deadline after its keyed pending entry is durable",
          "[write-command][delete][deadline][idempotency][fake-boundary]") {
    auto store_hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<bool> delayed{false};
    store_hooks->at_stage = [&](daemon::IdempotencyStoreStage stage) {
        if (stage == daemon::IdempotencyStoreStage::BeforeDirectorySync &&
            !delayed.exchange(true)) {
            std::this_thread::sleep_for(600ms);
        }
    };
    FakeWrites fake(true, store_hooks);
    auto request = delete_request(101, "delete-deadline-key");
    request.context.timeout_seconds = 0.5;
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK((*outcome.error)["error"]["details"]["phase"] == "preflight");
    CHECK((*outcome.error)["error"]["details"]["outcome"] == "not_started");
    CHECK((*outcome.error)["error"]["details"]["idempotency"] == "removed");
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    check_closed_without_pending(fake);
}

TEST_CASE("public writes preserve the exact retained spool contradiction path",
          "[write-command][send][delete][spool][schema][fake-boundary]") {
    for (const bool deleting : {false, true}) {
        CAPTURE(deleting);
        FakeWrites fake;
        const auto spool = fake.tree().account_state() + "/spool";
        const auto contradiction = spool + "/not-an-invocation";
        REQUIRE(std::filesystem::create_directory(spool));
        REQUIRE(::chmod(spool.c_str(), 0700) == 0);
        REQUIRE(std::filesystem::create_directory(contradiction));
        REQUIRE(::chmod(contradiction.c_str(), 0700) == 0);
        auto pending = deleting ? fake.dispatch(delete_request(101, "spool-delete-key"))
                                : fake.dispatch(send_request("hello", false, "spool-send-key"));
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        const auto diagnostic = daemon::encode_filesystem_diagnostic_path(contradiction);
        REQUIRE(diagnostic);
        CHECK(*outcome.error ==
              json{{"error",
                    {{"code", "AUDIT_INCOMPLETE"},
                     {"message", "attachment spool recovery is incomplete"},
                     {"details",
                      {{"account", "main"},
                       {"path", {{"kind", "bytes_hex"}, {"value", diagnostic->bytes_hex}}},
                       {"mutation_state", "none"},
                       {"completed_stages", json::array()}}}}}});
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 0);
        CHECK(fake.count(core::TdFunctionKind::SendMessage) == 0);
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    }
}

TEST_CASE("public send stores one mutation and replays before conflict without leaking the key",
          "[write-command][send][idempotency][fake-boundary]") {
    FakeWrites fake;
    auto first = fake.dispatch(send_request());
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::SendMessage, stable_message());
    const auto first_outcome = first.get();
    REQUIRE(first_outcome.result);
    CHECK(first_outcome.terminal_count == 1);
    CHECK((*first_outcome.result)["id"] == 101);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    CHECK_THAT(*first_outcome.result, tgcli::test::matches_json_schema("send.result.schema.json"));

    const auto audit_before_replay = read_bytes(fake.tree().audit_path());
    auto replay = fake.dispatch(send_request());
    bind_principal(fake);
    const auto replay_outcome = replay.get();
    CHECK(replay_outcome.result == first_outcome.result);
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    CHECK(read_bytes(fake.tree().audit_path()) == audit_before_replay);

    auto conflict = fake.dispatch(send_request("different"));
    bind_principal(fake);
    const auto conflict_outcome = conflict.get();
    REQUIRE(conflict_outcome.error);
    CHECK((*conflict_outcome.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK(fake.count(core::TdFunctionKind::SendMessage) == 1);
    CHECK_THAT(*conflict_outcome.error,
               tgcli::test::matches_json_schema("m3-write.error.schema.json"));

    const auto artifacts = first_outcome.result->dump() + replay_outcome.result->dump() +
                           conflict_outcome.error->dump() + read_bytes(fake.tree().audit_path()) +
                           read_bytes(fake.tree().store_path());
    CHECK(artifacts.find("m3-public-key-sentinel") == std::string::npos);
}

TEST_CASE("post-proof send timeout retains an unknown keyed invocation for recovery",
          "[write-command][send][timeout][idempotency][fake-boundary]") {
    FakeWrites fake;
    auto request = send_request("hello", false, "timeout-key-sentinel");
    request.context.timeout_seconds = 0.5;
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.observe(core::TdFunctionKind::SendMessage);
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK((*outcome.error)["error"]["details"]["phase"] == "confirmation");
    CHECK((*outcome.error)["error"]["details"]["outcome"] == "unknown");
    CHECK((*outcome.error)["error"]["details"]["idempotency"] == "pending");
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));

    const auto store = json::parse(read_bytes(fake.tree().store_path()));
    REQUIRE(store["entries"].size() == 1);
    CHECK(store["entries"][0]["state"] == "pending");
    CHECK(read_bytes(fake.tree().store_path()).find("timeout-key-sentinel") == std::string::npos);
}

TEST_CASE("public msg edit validates text state and returns the correlated message",
          "[write-command][edit][fake-boundary]") {
    FakeWrites fake;
    auto pending = fake.dispatch(edit_request("revised", "edit-key-sentinel"));
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_edited = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    const auto descriptor =
        fake.respond(core::TdFunctionKind::EditMessageText, edited_message("revised"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["id"] == 101);
    CHECK((*outcome.result)["chat_id"] == -1001);
    CHECK((*outcome.result)["text"] == "revised");
    CHECK((*outcome.result)["scheduled"] == false);
    CHECK(function_field<std::string>(descriptor, "text") == "revised");
    CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 1);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-edit.result.schema.json"));

    auto replay = fake.dispatch(edit_request("revised", "edit-key-sentinel"));
    bind_principal(fake);
    const auto replay_outcome = replay.get();
    CHECK(replay_outcome.result == outcome.result);
    CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 1);

    auto conflict = fake.dispatch(edit_request("different", "edit-key-sentinel"));
    bind_principal(fake);
    const auto conflict_outcome = conflict.get();
    REQUIRE(conflict_outcome.error);
    CHECK((*conflict_outcome.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 1);
    const auto artifacts = outcome.result->dump() + replay_outcome.result.value_or(json{}).dump();
    CHECK(artifacts.find("edit-key-sentinel") == std::string::npos);
}

TEST_CASE("msg edit property failures and dry-run stop before mutation",
          "[write-command][edit][dry-run][properties][fake-boundary]") {
    SECTION("non-text content is rejected") {
        FakeWrites fake;
        auto pending = fake.dispatch(edit_request());
        resolve_basic(fake);
        auto message = planning_message(101);
        message.content_kind = core::TdMessageContentKind::Photo;
        fake.respond(core::TdFunctionKind::GetMessage, message);
        core::TdMessageProperties properties;
        properties.can_be_edited = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "wrong_content_type");
        CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 0);
    }

    SECTION("reply markup is never silently discarded") {
        FakeWrites fake;
        auto pending = fake.dispatch(edit_request());
        resolve_basic(fake);
        auto message = planning_message(101);
        message.has_reply_markup = true;
        fake.respond(core::TdFunctionKind::GetMessage, message);
        core::TdMessageProperties properties;
        properties.can_be_edited = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] ==
              "reply_markup_preservation_unsupported");
        CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 0);
    }

    SECTION("dry-run plans with no persistence") {
        FakeWrites fake(false);
        auto pending = fake.dispatch(edit_request("revised", std::nullopt, true));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_edited = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK((*outcome.result)["plan"]["tdlib_request"] == "editMessageText");
        CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }
}

TEST_CASE("oversized correlated msg edit success is durable confirmed INTERNAL",
          "[write-command][edit][bounds][idempotency][fake-boundary]") {
    FakeWrites fake;
    auto pending = fake.dispatch(edit_request("revised", "edit-oversized-key"));
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_edited = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    fake.respond(core::TdFunctionKind::EditMessageText, edited_message(std::string(4'097, 'x')));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK((*outcome.error)["error"]["message"] ==
          "TDLib returned data outside the supported persistence bounds");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"operation", "msg_edit"}, {"reason", "internal_error"}});
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));

    auto guard = fake.foundation()->acquire_epoch();
    const auto store = fake.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(store.snapshot.entries.size() == 1);
    CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    REQUIRE(store.snapshot.entries.front().terminal);
    CHECK((*store.snapshot.entries.front().terminal)["code"] == "INTERNAL");
    const auto audit = fake.foundation()->audit().inspect(guard);
    CHECK(audit.status == daemon::AccountAuditInspectionStatus::Clean);
}

TEST_CASE("public msg react validates availability and exact add-remove options",
          "[write-command][react][fake-boundary]") {
    for (const auto& [remove, big, function] :
         std::vector<std::tuple<bool, bool, core::TdFunctionKind>>{
             {false, true, core::TdFunctionKind::AddMessageReaction},
             {true, false, core::TdFunctionKind::RemoveMessageReaction}}) {
        CAPTURE(remove, big);
        FakeWrites fake;
        auto pending = fake.dispatch(react_request(remove, big));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessageAvailableReactions, available_reactions());
        fake.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
        const auto descriptor = fake.respond(function, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"chat_id", -1001},
                                      {"message_id", 101},
                                      {"reaction", "👍"},
                                      {"removed", remove},
                                      {"big", big}});
        CHECK(function_field<std::string>(descriptor, "reaction") == "👍");
        if (!remove) {
            CHECK(function_field<bool>(descriptor, "is_big") == big);
            CHECK(function_field<bool>(descriptor, "update_recent_reactions"));
        }
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("msg-react.result.schema.json"));
    }

    SECTION("an unavailable reaction is a planning precondition") {
        FakeWrites fake;
        auto pending = fake.dispatch(react_request());
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessageAvailableReactions,
                     core::TdMessageAvailableReactions{});
        fake.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "reaction_unavailable");
        CHECK(fake.count(core::TdFunctionKind::AddMessageReaction) == 0);
    }

    SECTION("known unavailability reasons reject add even when the emoji is listed") {
        for (const auto reason : {core::TdReactionUnavailabilityReason::AnonymousAdministrator,
                                  core::TdReactionUnavailabilityReason::Guest,
                                  core::TdReactionUnavailabilityReason::Restricted}) {
            CAPTURE(static_cast<int>(reason));
            FakeWrites fake;
            auto pending = fake.dispatch(react_request());
            resolve_basic(fake);
            auto available = available_reactions();
            available.unavailability_reason = reason;
            fake.respond(core::TdFunctionKind::GetMessageAvailableReactions, std::move(available));
            fake.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
            static_cast<void>(
                fake.try_respond(core::TdFunctionKind::AddMessageReaction, core::TdOk{}));
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
            CHECK((*outcome.error)["error"]["details"]["reason"] == "reaction_unavailable");
            CHECK(fake.count(core::TdFunctionKind::AddMessageReaction) == 0);
            CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
            CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
        }
    }

    SECTION("remove is independent of add availability for every known reason") {
        for (const auto reason : {core::TdReactionUnavailabilityReason::None,
                                  core::TdReactionUnavailabilityReason::AnonymousAdministrator,
                                  core::TdReactionUnavailabilityReason::Guest,
                                  core::TdReactionUnavailabilityReason::Restricted}) {
            CAPTURE(static_cast<int>(reason));
            FakeWrites fake;
            auto pending = fake.dispatch(react_request(true));
            resolve_basic(fake);
            auto available = available_reactions();
            available.top.clear();
            available.unavailability_reason = reason;
            fake.respond(core::TdFunctionKind::GetMessageAvailableReactions, std::move(available));
            fake.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
            static_cast<void>(
                fake.try_respond(core::TdFunctionKind::RemoveMessageReaction, core::TdOk{}));
            const auto outcome = pending.get();
            REQUIRE(outcome.result);
            CHECK((*outcome.result)["removed"] == true);
            CHECK(fake.count(core::TdFunctionKind::RemoveMessageReaction) == 1);
        }
    }

    SECTION("unknown unavailability fails closed before intent") {
        FakeWrites fake;
        auto pending = fake.dispatch(react_request());
        resolve_basic(fake);
        auto available = available_reactions();
        available.unavailability_reason = core::TdReactionUnavailabilityReason::Unknown;
        fake.respond(core::TdFunctionKind::GetMessageAvailableReactions, std::move(available));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(fake.count(core::TdFunctionKind::GetMessageProperties) == 0);
        CHECK(fake.count(core::TdFunctionKind::AddMessageReaction) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }

    SECTION("a bot is rejected before reaction reads") {
        FakeWrites fake;
        auto pending = fake.dispatch(react_request());
        fake.respond(core::TdFunctionKind::GetMe, peer(core::TdUserPresence::Online, true, 42));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetMessageAvailableReactions) == 0);
    }
}

TEST_CASE("public msg forward is registered through the write dispatcher",
          "[write-command][forward][dispatch]") {
    FakeWrites fake;
    auto pending = fake.dispatch(forward_request());
    plan_forward(fake);
    core::TdForwardMessages forwarded;
    forwarded.messages.emplace_back(forwarded_message(101));
    forwarded.messages.emplace_back(forwarded_message(102));
    const auto descriptor =
        fake.respond(core::TdFunctionKind::ForwardMessages, std::move(forwarded));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["from_chat_id"] == -1001);
    CHECK((*outcome.result)["to_chat_id"] == -1002);
    REQUIRE((*outcome.result)["items"].size() == 2);
    CHECK((*outcome.result)["items"][0]["source_id"] == 1);
    CHECK((*outcome.result)["items"][0]["status"] == "sent");
    CHECK((*outcome.result)["items"][1]["source_id"] == 2);
    CHECK(function_field<std::vector<std::int64_t>>(descriptor, "message_ids") ==
          std::vector<std::int64_t>{1, 2});
    CHECK(function_field<bool>(descriptor, "send_copy") == false);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-forward.result.schema.json"));
}

TEST_CASE("msg forward plans copy capability and dry-run without persistence",
          "[write-command][forward][planning][dry-run]") {
    SECTION("drop author requires copy capability") {
        FakeWrites fake;
        auto request = forward_request();
        request.args["drop_author"] = true;
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        auto destination = basic_chat();
        destination.id = -1002;
        fake.respond(core::TdFunctionKind::GetChat, destination);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(1));
        const auto outcome = [&] {
            fake.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
            return pending.get();
        }();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "not_copyable");
        CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }

    SECTION("dry-run returns the immutable dual-chat plan") {
        FakeWrites fake(false);
        auto request = forward_request(std::nullopt, true);
        request.args["drop_author"] = true;
        auto pending = fake.dispatch(request);
        plan_forward(fake, true);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK((*outcome.result)["plan"]["operation"] == "msg_forward");
        CHECK((*outcome.result)["plan"]["from"]["id"] == -1001);
        CHECK((*outcome.result)["plan"]["to"]["id"] == -1002);
        CHECK((*outcome.result)["plan"]["drop_author"] == true);
        CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("msg-forward.result.schema.json"));
    }
}

TEST_CASE("msg forward aggregates partial and rate-limited vectors exactly",
          "[write-command][forward][aggregation][schema]") {
    SECTION("one sent and one upstream null is a durable partial") {
        FakeWrites fake;
        auto pending = fake.dispatch(forward_request("forward-partial-key"));
        plan_forward(fake);
        core::TdForwardMessages forwarded;
        forwarded.messages.emplace_back(forwarded_message(101));
        forwarded.messages.emplace_back(std::nullopt);
        fake.respond(core::TdFunctionKind::ForwardMessages, std::move(forwarded));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "FORWARD_PARTIAL");
        CHECK((*outcome.error)["error"]["details"]["items"][0]["status"] == "sent");
        CHECK((*outcome.error)["error"]["details"]["items"][1]["failure_reason"] ==
              "upstream_null");
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        auto guard = fake.foundation()->acquire_epoch();
        const auto store = fake.foundation()->store().inspect(guard);
        REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
        REQUIRE(store.snapshot.entries.size() == 1);
        CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    }

    SECTION("all 429 uses the maximum ceiling and closes mutation none") {
        FakeWrites fake;
        auto pending = fake.dispatch(forward_request("forward-rate-key"));
        plan_forward(fake);
        core::TdForwardMessages forwarded;
        forwarded.messages.emplace_back(failed_forward_message(-70, 429, 1.25));
        forwarded.messages.emplace_back(failed_forward_message(-71, 429, 3.01));
        fake.respond(core::TdFunctionKind::ForwardMessages, std::move(forwarded));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "RATE_LIMITED");
        CHECK((*outcome.error)["error"]["details"]["retry_after"] == 4);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        check_closed_without_pending(fake);
    }

    SECTION("top-level 429 has the strict empty post-dispatch vector") {
        FakeWrites fake;
        auto pending = fake.dispatch(forward_request("forward-top-rate-key"));
        plan_forward(fake);
        fake.respond(core::TdFunctionKind::ForwardMessages, core::TdError{429, "retry after 6"});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "RATE_LIMITED");
        CHECK((*outcome.error)["error"]["details"]["retry_after"] == 6);
        CHECK((*outcome.error)["error"]["details"]["items"] == json::array());
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        check_completed_possible_with_pending(fake);
    }

    SECTION("authorization loss after a sent item closes without fabricating a mutation proof") {
        FakeWrites fake;
        auto pending = fake.dispatch(forward_request("forward-auth-loss-key"));
        plan_forward(fake);
        const auto [descriptor, query_id] =
            fake.observe_call(core::TdFunctionKind::ForwardMessages);
        const auto sending_id =
            static_cast<std::int32_t>(function_field<std::int64_t>(descriptor, "sending_id"));
        core::TdForwardMessages forwarded;
        forwarded.messages.emplace_back(forwarded_message(101));
        forwarded.messages.emplace_back(pending_forward_message(-77, sending_id));
        fake.push_response(query_id, std::move(forwarded));
        fake.push_authorization(core::AuthStateData{core::AuthState::LoggingOut});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        REQUIRE((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        check_completed_possible_with_pending(fake);
        const auto audit = read_bytes(fake.tree().audit_path());
        CHECK(audit.find(R"("mutation_state":"confirmed")") != std::string::npos);
        CHECK(audit.find(R"("stage":"mutation_confirmed")") == std::string::npos);
    }
}

TEST_CASE("msg forward persists pending then sent vectors before completing",
          "[write-command][forward][progress][recovery]") {
    FakeWrites fake;
    auto pending = fake.dispatch(forward_request("forward-progress-key"));
    plan_forward(fake);
    const auto [descriptor, query_id] = fake.observe_call(core::TdFunctionKind::ForwardMessages);
    const auto sending_id =
        static_cast<std::int32_t>(function_field<std::int64_t>(descriptor, "sending_id"));
    core::TdForwardMessages immediate;
    immediate.messages.emplace_back(pending_forward_message(-77, sending_id));
    immediate.messages.emplace_back(std::nullopt);
    fake.push_response(query_id, std::move(immediate));
    auto success = forwarded_message(101);
    fake.push_forward_success(-77, std::move(success));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "FORWARD_PARTIAL");
    CHECK(sending_id != 0);
    const auto audit = read_bytes(fake.tree().audit_path());
    const auto pending_position = audit.find(R"("status":"pending")");
    const auto sent_position = audit.find(R"("status":"sent")");
    CHECK(pending_position != std::string::npos);
    CHECK(sent_position != std::string::npos);
    CHECK(sent_position > pending_position);
}

TEST_CASE("msg forward separates pre-proof and post-proof deadline cuts",
          "[write-command][forward][deadline][idempotency]") {
    SECTION("preparation deadline closes mutation none") {
        FakeWrites fake;
        fake.expire_forward_before_request();
        auto pending = fake.dispatch(forward_request("forward-preproof-key"));
        plan_forward(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "preflight");
        CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 0);
        check_closed_without_pending(fake);
    }

    SECTION("deadline after dispatch proof retains unknown pending") {
        FakeWrites fake;
        fake.expire_forward_before_submit();
        auto pending = fake.dispatch(forward_request("forward-postproof-key"));
        plan_forward(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "confirmation");
        CHECK((*outcome.error)["error"]["details"]["items"] == json::array());
        CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 0);
        check_completed_possible_with_pending(fake);
    }

    SECTION("deadline after a sent item confirms mutation but retains pending recovery") {
        FakeWrites fake;
        fake.expire_forward_after_progress();
        auto pending = fake.dispatch(forward_request("forward-confirmed-timeout-key"));
        plan_forward(fake);
        const auto [descriptor, query_id] =
            fake.observe_call(core::TdFunctionKind::ForwardMessages);
        const auto sending_id =
            static_cast<std::int32_t>(function_field<std::int64_t>(descriptor, "sending_id"));
        core::TdForwardMessages forwarded;
        forwarded.messages.emplace_back(forwarded_message(101));
        forwarded.messages.emplace_back(pending_forward_message(-77, sending_id));
        fake.push_response(query_id, std::move(forwarded));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["items"][0]["status"] == "sent");
        CHECK((*outcome.error)["error"]["details"]["items"][1]["status"] == "pending");
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        check_completed_possible_with_pending(fake);
        const auto audit = read_bytes(fake.tree().audit_path());
        CHECK(audit.find(R"("mutation_state":"confirmed")") != std::string::npos);
        CHECK(audit.find(R"("stage":"mutation_confirmed")") != std::string::npos);
    }
}

TEST_CASE("msg forward malformed late updates retain confirmed pending recovery",
          "[write-command][forward][malformed][recovery][idempotency]") {
    for (const bool success_update : {true, false}) {
        CAPTURE(success_update);
        FakeWrites fake;
        auto progress = fake.observe_forward_progress();
        auto pending = fake.dispatch(forward_request("forward-malformed-late-key"));
        plan_forward(fake);
        const auto [descriptor, query_id] =
            fake.observe_call(core::TdFunctionKind::ForwardMessages);
        const auto sending_id =
            static_cast<std::int32_t>(function_field<std::int64_t>(descriptor, "sending_id"));
        core::TdForwardMessages forwarded;
        forwarded.messages.emplace_back(forwarded_message(101));
        forwarded.messages.emplace_back(pending_forward_message(-77, sending_id));
        fake.push_response(query_id, std::move(forwarded));
        REQUIRE(progress.wait_for(2s) == std::future_status::ready);
        if (success_update) {
            fake.push_forward_success(-77, std::nullopt);
        } else {
            fake.push_forward_failure(-77, std::nullopt, std::nullopt);
        }
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        {
            auto guard = fake.foundation()->acquire_epoch();
            CHECK(fake.foundation()->audit().inspect(guard).status ==
                  daemon::AccountAuditInspectionStatus::Clean);
            const auto store = fake.foundation()->store().inspect(guard);
            REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
            REQUIRE(store.snapshot.entries.size() == 1);
            CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
        }
        const auto audit = read_bytes(fake.tree().audit_path());
        CHECK(audit.find(R"("mutation_state":"confirmed")") != std::string::npos);
        CHECK(audit.find(R"("stage":"mutation_confirmed")") == std::string::npos);

        auto replay = fake.dispatch(forward_request("forward-malformed-late-key"));
        bind_principal(fake);
        const auto replay_outcome = replay.get();
        REQUIRE(replay_outcome.error);
        INFO(replay_outcome.error->dump());
        CHECK((*replay_outcome.error)["error"]["code"] == "IDEMPOTENCY_PENDING");
        CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 1);
    }
}

TEST_CASE("completed msg forward replay is stable and conflicting vectors never re-dispatch",
          "[write-command][forward][idempotency][replay][conflict]") {
    FakeWrites fake;
    auto first = fake.dispatch(forward_request("forward-replay-key"));
    plan_forward(fake);
    core::TdForwardMessages forwarded;
    forwarded.messages.emplace_back(forwarded_message(101));
    forwarded.messages.emplace_back(forwarded_message(102));
    fake.respond(core::TdFunctionKind::ForwardMessages, std::move(forwarded));
    const auto first_outcome = first.get();
    REQUIRE(first_outcome.result);

    auto replay = fake.dispatch(forward_request("forward-replay-key"));
    bind_principal(fake);
    const auto replay_outcome = replay.get();
    CHECK(replay_outcome.result == first_outcome.result);
    CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 1);

    auto changed = forward_request("forward-replay-key");
    changed.args["message_ids"] = json::array({1, 3});
    auto conflict = fake.dispatch(changed);
    bind_principal(fake);
    const auto conflict_outcome = conflict.get();
    REQUIRE(conflict_outcome.error);
    CHECK((*conflict_outcome.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK(fake.count(core::TdFunctionKind::ForwardMessages) == 1);
}

TEST_CASE("public msg pin and unpin preserve exact property and TD request semantics",
          "[write-command][message-pin][fake-boundary]") {
    for (const bool pinned : {true, false}) {
        CAPTURE(pinned);
        FakeWrites fake;
        auto pending = fake.dispatch(message_pin_request(pinned));
        resolve_basic(fake);
        core::TdMessageProperties properties;
        properties.can_be_pinned = pinned;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto function =
            pinned ? core::TdFunctionKind::PinChatMessage : core::TdFunctionKind::UnpinChatMessage;
        const auto descriptor = fake.respond(function, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"chat_id", -1001}, {"message_id", 101}, {"pinned", pinned}});
        CHECK(function_field<std::int64_t>(descriptor, "message_id") == 101);
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema(pinned ? "msg-pin.result.schema.json"
                                                           : "msg-unpin.result.schema.json"));
    }

    FakeWrites denied;
    auto pending = denied.dispatch(message_pin_request(true));
    resolve_basic(denied);
    denied.respond(core::TdFunctionKind::GetMessageProperties, core::TdMessageProperties{});
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["details"]["reason"] == "not_pinnable");
    CHECK(denied.count(core::TdFunctionKind::PinChatMessage) == 0);
}

TEST_CASE("direct message operations separate pre-proof and post-proof deadline cuts",
          "[write-command][edit][deadline][idempotency][fake-boundary]") {
    SECTION("preparation deadline closes mutation none") {
        FakeWrites fake;
        fake.expire_direct_before_request();
        auto pending = fake.dispatch(edit_request("revised", "edit-preproof-key"));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_edited = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "preflight");
        CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 0);
        check_closed_without_pending(fake);
    }

    SECTION("deadline after dispatch proof retains unknown pending") {
        FakeWrites fake;
        fake.expire_direct_before_submit();
        auto pending = fake.dispatch(edit_request("revised", "edit-postproof-key"));
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_edited = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"]["phase"] == "dispatch");
        CHECK((*outcome.error)["error"]["details"]["outcome"] == "unknown");
        CHECK(fake.count(core::TdFunctionKind::EditMessageText) == 0);
        check_completed_possible_with_pending(fake);
    }
}

TEST_CASE("public msg delete confirms the immutable plan and accepts correlated ok",
          "[write-command][delete][confirmation][fake-boundary]") {
    FakeWrites fake;
    auto pending = fake.dispatch(delete_request());
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    fake.respond(core::TdFunctionKind::DeleteMessages, core::TdOk{});
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.terminal_count == 1);
    CHECK(*outcome.result == json{{"chat_id", -1001},
                                  {"message_ids", json::array({101})},
                                  {"for_all", false},
                                  {"deleted", true}});
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-delete.result.schema.json"));
}

TEST_CASE("msg delete applies exact self/revoke property rules before confirmation",
          "[write-command][delete][properties][fake-boundary]") {
    SECTION("private and basic self-delete require the self property") {
        FakeWrites fake;
        auto pending = fake.dispatch(delete_request());
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_for_all_users = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "not_deletable_for_self");
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    }

    SECTION("explicit for-all on a basic group dispatches revoke true") {
        FakeWrites fake;
        auto request = delete_request();
        request.args["for_all"] = true;
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
        core::TdMessageProperties properties;
        properties.can_be_deleted_for_all_users = true;
        fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
        const auto descriptor = fake.respond(core::TdFunctionKind::DeleteMessages, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["for_all"] == true);
        CHECK(function_field<bool>(descriptor, "revoke"));
    }

    SECTION("supergroups require an explicit for-all request before message reads") {
        FakeWrites fake;
        auto pending = fake.dispatch(delete_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup,
                     core::TdSupergroup{.id = 55, .usernames = {}, .is_channel = false});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "not_deletable_for_all");
        CHECK(fake.count(core::TdFunctionKind::GetMessage) == 0);
        CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    }
}

TEST_CASE("msg delete dry-run performs property planning without confirmation or writes",
          "[write-command][delete][dry-run][confirmation][fake-boundary]") {
    FakeWrites fake(false);
    auto request = delete_request(101, std::nullopt, false);
    request.context.dry_run = true;
    auto pending = fake.dispatch(request);
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["dry_run"] == true);
    CHECK((*outcome.result)["plan"]["operation"] == "msg_delete");
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 0);
    CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("msg-delete.result.schema.json"));
}

TEST_CASE("completed msg delete replay reconfirms its stored plan before returning",
          "[write-command][delete][confirmation][replay][fake-boundary]") {
    FakeWrites fake;
    auto first = fake.dispatch(delete_request(101, "delete-replay-key"));
    resolve_basic(fake);
    fake.respond(core::TdFunctionKind::GetMessage, planning_message(101));
    core::TdMessageProperties properties;
    properties.can_be_deleted_only_for_self = true;
    fake.respond(core::TdFunctionKind::GetMessageProperties, properties);
    fake.respond(core::TdFunctionKind::DeleteMessages, core::TdOk{});
    const auto first_outcome = first.get();
    REQUIRE(first_outcome.result);

    auto unconfirmed = fake.dispatch(delete_request(101, "delete-replay-key", false));
    bind_principal(fake);
    const auto unconfirmed_outcome = unconfirmed.get();
    REQUIRE(unconfirmed_outcome.error);
    CHECK((*unconfirmed_outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
    CHECK((*unconfirmed_outcome.error)["error"]["details"]["target"]["message_ids"] ==
          json::array({101}));
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);

    auto replay = fake.dispatch(delete_request(101, "delete-replay-key"));
    bind_principal(fake);
    const auto replay_outcome = replay.get();
    CHECK(replay_outcome.result == first_outcome.result);
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);

    auto conflict = fake.dispatch(delete_request(102, "delete-replay-key", false));
    bind_principal(fake);
    const auto conflict_outcome = conflict.get();
    REQUIRE(conflict_outcome.error);
    CHECK((*conflict_outcome.error)["error"]["code"] == "IDEMPOTENCY_CONFLICT");
    CHECK(fake.count(core::TdFunctionKind::DeleteMessages) == 1);
}

TEST_CASE("chat duration grammar and empty mark-read are exact durable operations",
          "[write-command][chat][mark-read][mute][domain][fake-boundary]") {
    CHECK(daemon::parse_mute_duration("1s") == 1);
    CHECK(daemon::parse_mute_duration("2m") == 120);
    CHECK(daemon::parse_mute_duration("1w") == 604800);
    CHECK(daemon::parse_mute_duration("366d") == 31'622'400);
    for (const auto* invalid :
         {"", "0s", "01s", "+1s", "1", "1M", "367d", "999999999999999999999w"}) {
        INFO(invalid);
        CHECK_FALSE(daemon::parse_mute_duration(invalid));
    }

    SECTION("nonempty chat dispatches the current last message exactly once") {
        FakeWrites fake;
        auto request = chat_target_request("mark-read");
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        auto chat = basic_chat();
        chat.last_message = stable_message().message;
        fake.respond(core::TdFunctionKind::GetChat, chat);
        const auto descriptor = fake.respond(core::TdFunctionKind::ViewMessages, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result ==
              json{{"chat_id", -1001}, {"last_read_message_id", 101}, {"marked_read", true}});
        CHECK(function_field<std::vector<std::int64_t>>(descriptor, "message_ids") ==
              std::vector<std::int64_t>{101});
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-mark-read.result.schema.json"));
    }

    SECTION("empty keyed chat completes without mutation and replays") {
        FakeWrites fake;
        const auto request = chat_target_request("mark-read", "-1001", "empty-read-key");
        auto pending = fake.dispatch(request);
        resolve_basic(fake);
        const auto first = pending.get();
        REQUIRE(first.result);
        CHECK((*first.result)["last_read_message_id"] == nullptr);
        CHECK(fake.count(core::TdFunctionKind::ViewMessages) == 0);
        {
            auto guard = fake.foundation()->acquire_epoch();
            const auto store = fake.foundation()->store().inspect(guard);
            REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
            REQUIRE(store.snapshot.entries.size() == 1);
            CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
            CHECK(fake.foundation()
                      ->run_core_gate(guard, store.snapshot.entries.front().created_at)
                      .status == daemon::IdempotencyCoreGateStatus::Clean);
        }
        auto replay = fake.dispatch(request);
        bind_principal(fake);
        CHECK(replay.get().result == first.result);
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 1);
        CHECK(fake.count(core::TdFunctionKind::ViewMessages) == 0);
    }
}

TEST_CASE("chat mute copies every observed setting and enforces Saved Messages",
          "[write-command][chat][mute][settings][fake-boundary]") {
    SECTION("mute changes only the two mute fields") {
        FakeWrites fake;
        auto pending = fake.dispatch(chat_mute_request(true, 3600));
        bind_principal(fake);
        auto chat = basic_chat();
        chat.notification_settings = core::TdChatNotificationSettings{
            .use_default_mute_for = true,
            .mute_for = 77,
            .use_default_sound = false,
            .sound_id = 99,
            .use_default_show_preview = false,
            .show_preview = true,
            .use_default_mute_stories = false,
            .mute_stories = true,
            .use_default_story_sound = false,
            .story_sound_id = 101,
            .use_default_show_story_poster = false,
            .show_story_poster = true,
            .use_default_disable_pinned_message_notifications = false,
            .disable_pinned_message_notifications = true,
            .use_default_disable_mention_notifications = false,
            .disable_mention_notifications = true};
        fake.respond(core::TdFunctionKind::GetChat, chat);
        const auto descriptor =
            fake.respond(core::TdFunctionKind::SetChatNotificationSettings, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result ==
              json{{"chat_id", -1001}, {"muted", true}, {"duration_seconds", 3600}});
        CHECK_FALSE(function_field<bool>(descriptor, "use_default_mute_for"));
        CHECK(function_field<std::int64_t>(descriptor, "mute_for") == 3600);
        CHECK(function_field<std::int64_t>(descriptor, "sound_id") == 99);
        CHECK(function_field<bool>(descriptor, "show_preview"));
        CHECK(function_field<std::int64_t>(descriptor, "story_sound_id") == 101);
        CHECK(function_field<bool>(descriptor, "disable_mention_notifications"));
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-mute.result.schema.json"));
    }

    SECTION("unmute rejects Saved Messages before mutation") {
        FakeWrites fake;
        auto request = chat_mute_request(false, 0);
        request.args["chat"] = "42";
        auto pending = fake.dispatch(request);
        bind_principal(fake);
        auto chat = private_chat(42);
        chat.notification_settings = core::TdChatNotificationSettings{};
        fake.respond(core::TdFunctionKind::GetChat, chat);
        fake.respond(core::TdFunctionKind::GetUser, self());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "saved_notifications_unsupported");
        CHECK(fake.count(core::TdFunctionKind::SetChatNotificationSettings) == 0);
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }
}

TEST_CASE("chat pin and archive planners preserve list semantics",
          "[write-command][chat][pin][archive][fake-boundary]") {
    SECTION("pin prefers Archive over Main") {
        FakeWrites fake;
        auto pending = fake.dispatch(chat_target_request("pin"));
        bind_principal(fake);
        auto chat = basic_chat();
        chat.chat_lists = {
            {.kind = core::TdChatListKind::Main, .folder_id = 0, .tdlib_type_id = 1},
            {.kind = core::TdChatListKind::Archive, .folder_id = 0, .tdlib_type_id = 2}};
        fake.respond(core::TdFunctionKind::GetChat, chat);
        const auto descriptor =
            fake.respond(core::TdFunctionKind::ToggleChatIsPinned, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["chat_list"] == "archive");
        CHECK(function_field<std::string>(descriptor, "chat_list") == "archive");
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-pin.result.schema.json"));
    }

    SECTION("unlisted chat fails before mutation") {
        FakeWrites fake;
        auto pending = fake.dispatch(chat_target_request("unpin"));
        resolve_basic(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "chat_not_listed");
        CHECK(fake.count(core::TdFunctionKind::ToggleChatIsPinned) == 0);
    }

    SECTION("archive uses the Archive list and exact result") {
        FakeWrites fake;
        auto pending = fake.dispatch(chat_target_request("archive"));
        resolve_basic(fake);
        const auto descriptor = fake.respond(core::TdFunctionKind::AddChatToList, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"chat_id", -1001}, {"archived", true}});
        CHECK(function_field<std::string>(descriptor, "chat_list") == "archive");
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-archive.result.schema.json"));
    }
}

TEST_CASE("chat join keeps invite bytes out of durable and TD descriptors",
          "[write-command][chat][join][secrecy][fake-boundary]") {
    SECTION("username joins the exact resolved chat") {
        FakeWrites fake;
        auto pending = fake.dispatch(chat_join_request("@project"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::SearchPublicChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup,
                     core::TdSupergroup{.id = 55, .usernames = {"project"}, .is_channel = false});
        const auto descriptor =
            fake.respond(core::TdFunctionKind::JoinChat,
                         core::TdChatJoinResult{.kind = core::TdChatJoinResultKind::Success,
                                                .chat_id = -1001,
                                                .guard_bot_user_id = std::nullopt,
                                                .guard_query_id = std::nullopt,
                                                .unsupported_tdlib_type_id = std::nullopt});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"status", "joined"}, {"chat_id", -1001}});
        CHECK(function_field<std::int64_t>(descriptor, "chat_id") == -1001);
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-join.result.schema.json"));
    }

    SECTION("invite mutation remains joinChatByInviteLink even with known metadata") {
        const std::string invite = "https://t.me/+ChatJoinSecretSentinel123";
        FakeWrites fake;
        auto pending = fake.dispatch(chat_join_request(invite, "join-invite-key"));
        bind_principal(fake);
        const auto classified =
            fake.respond(core::TdFunctionKind::GetInternalLinkType,
                         core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                              .username = {},
                                              .url = invite,
                                              .tdlib_type_id = 1});
        const auto checked =
            fake.respond(core::TdFunctionKind::CheckChatInviteLink,
                         core::TdChatInviteLinkInfo{.chat_id = -1001, .is_public = false});
        CHECK(function_field_holds<core::TdRedactedValue>(classified, "link"));
        CHECK(function_field_holds<core::TdRedactedValue>(checked, "link"));
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        const auto descriptor =
            fake.respond(core::TdFunctionKind::JoinChatByInviteLink,
                         core::TdChatJoinResult{.kind = core::TdChatJoinResultKind::RequestSent,
                                                .chat_id = std::nullopt,
                                                .guard_bot_user_id = std::nullopt,
                                                .guard_query_id = std::nullopt,
                                                .unsupported_tdlib_type_id = std::nullopt});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"status", "request_sent"}, {"chat_id", -1001}});
        CHECK(function_field<core::TdRedactedValue>(descriptor, "invite_link") ==
              core::TdRedactedValue::InviteLink);
        CHECK(read_bytes(fake.tree().audit_path()).find(invite) == std::string::npos);
        CHECK(read_bytes(fake.tree().store_path()).find(invite) == std::string::npos);
        CHECK(read_bytes(fake.tree().audit_path()).find(daemon::invite_link_hash(invite)) !=
              std::string::npos);
    }

    SECTION("guard is an explicit mutation-none terminal") {
        const std::string invite = "https://t.me/+ChatJoinGuardSentinel123";
        FakeWrites fake;
        auto pending = fake.dispatch(chat_join_request(invite, "join-guard-key"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetInternalLinkType,
                     core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                          .username = {},
                                          .url = invite,
                                          .tdlib_type_id = 1});
        fake.respond(core::TdFunctionKind::CheckChatInviteLink,
                     core::TdChatInviteLinkInfo{.chat_id = 0, .is_public = false});
        fake.respond(
            core::TdFunctionKind::JoinChatByInviteLink,
            core::TdChatJoinResult{.kind = core::TdChatJoinResultKind::GuardBotApprovalRequired,
                                   .chat_id = std::nullopt,
                                   .guard_bot_user_id = 77,
                                   .guard_query_id = 88,
                                   .unsupported_tdlib_type_id = std::nullopt});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "JOIN_APPROVAL_REQUIRED");
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
        check_closed_without_pending(fake);
    }

    SECTION("invite misses expose only the domain-separated hash") {
        const std::string invite = "https://t.me/+ChatJoinMissingSentinel123";
        FakeWrites fake;
        auto pending = fake.dispatch(chat_join_request(invite));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetInternalLinkType,
                     core::TdError{.code = 404, .message = invite});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        const auto rendered = outcome.error->dump();
        CHECK(rendered.find(invite) == std::string::npos);
        CHECK((*outcome.error)["error"]["details"]["selector"] == daemon::invite_link_hash(invite));
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("invite dry-run performs metadata reads without mutation or persistence") {
        const std::string invite = "https://t.me/+ChatJoinDryRunSentinel123";
        FakeWrites fake(false);
        auto pending = fake.dispatch(chat_join_request(invite, std::nullopt, true));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetInternalLinkType,
                     core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                          .username = {},
                                          .url = invite,
                                          .tdlib_type_id = 1});
        fake.respond(core::TdFunctionKind::CheckChatInviteLink,
                     core::TdChatInviteLinkInfo{.chat_id = 0, .is_public = false});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK((*outcome.result)["plan"]["invite_link_sha256"] == daemon::invite_link_hash(invite));
        CHECK(outcome.result->dump().find(invite) == std::string::npos);
        CHECK(fake.count(core::TdFunctionKind::JoinChatByInviteLink) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }
}

TEST_CASE("invite ownership wipes request handler response and direct DTO copies",
          "[write-command][chat][join][secrecy][wipe][fake-boundary]") {
    SECTION("parse failure wipes protocol-owned copies") {
        const std::string invite = "https://t.me/+ParseFailureWipeSentinel123";
        WipeTrace trace;
        {
            FakeWrites fake;
            auto request = chat_join_request(invite, std::nullopt, false, 2.0, trace.observer());
            request.args["unexpected"] = true;
            const auto outcome = fake.dispatch(request).get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "USAGE");
        }
        CHECK(trace.saw("request_args", invite.size()));
        CHECK(trace.saw("request_facts_args", invite.size()));
    }

    SECTION("bot and authority denial wipe the extracted handler input") {
        for (const bool bot : {true, false}) {
            CAPTURE(bot);
            const std::string invite = bot ? "https://t.me/+BotDenialWipeSentinel123"
                                           : "https://t.me/+AuthorityDenialWipeSentinel123";
            WipeTrace trace;
            {
                FakeWrites fake(bot);
                auto pending = fake.dispatch(
                    chat_join_request(invite, std::nullopt, false, 2.0, trace.observer()));
                fake.respond(core::TdFunctionKind::GetMe,
                             bot ? peer(core::TdUserPresence::Online, true, 42) : self());
                const auto outcome = pending.get();
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] ==
                      (bot ? "BOT_UNSUPPORTED" : "WRITE_DENIED"));
            }
            CHECK(trace.saw("chat_join_input", invite.size()));
            CHECK(trace.saw("invite_alias", invite.size()));
            CHECK(trace.saw("request_args", invite.size()));
            CHECK(trace.saw("request_facts_args", invite.size()));
        }
    }

    SECTION("planning error and success wipe TD response and mutation DTO copies") {
        for (const bool succeeds : {false, true}) {
            CAPTURE(succeeds);
            const std::string invite = succeeds ? "https://t.me/+SuccessWipeSentinel123"
                                                : "https://t.me/+PlanningErrorWipeSentinel123";
            WipeTrace trace;
            {
                FakeWrites fake;
                auto pending = fake.dispatch(
                    chat_join_request(invite, std::nullopt, false, 2.0, trace.observer()));
                bind_principal(fake);
                if (!succeeds) {
                    fake.respond(core::TdFunctionKind::GetInternalLinkType,
                                 core::TdError{.code = 404, .message = invite});
                    REQUIRE(pending.get().error);
                } else {
                    fake.respond(core::TdFunctionKind::GetInternalLinkType,
                                 core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                                      .username = {},
                                                      .url = invite,
                                                      .tdlib_type_id = 1});
                    fake.respond(core::TdFunctionKind::CheckChatInviteLink,
                                 core::TdChatInviteLinkInfo{.chat_id = 0, .is_public = false});
                    fake.respond(core::TdFunctionKind::JoinChatByInviteLink,
                                 core::TdChatJoinResult{.kind = core::TdChatJoinResultKind::Success,
                                                        .chat_id = -1001,
                                                        .guard_bot_user_id = std::nullopt,
                                                        .guard_query_id = std::nullopt,
                                                        .unsupported_tdlib_type_id = std::nullopt});
                    REQUIRE(pending.get().result);
                }
            }
            CHECK(trace.saw("chat_join_input", invite.size()));
            CHECK(trace.saw("invite_alias", invite.size()));
            CHECK(trace.saw("td_internal_link_request_source", invite.size()));
            CHECK(trace.saw(succeeds ? "td_internal_link_url" : "td_invite_error", invite.size()));
            if (succeeds) {
                CHECK(trace.saw("td_check_invite_request_source", invite.size()));
                CHECK((trace.saw("td_join_invite", invite.size()) ||
                       trace.saw("td_join_invite_move_source", invite.size())));
            }
            CHECK(trace.saw("request_args", invite.size()));
            CHECK(trace.saw("request_facts_args", invite.size()));
        }
    }
}

TEST_CASE("invite alias ownership covers exact forms and short strings through final release",
          "[write-command][chat][join][secrecy][redaction][wipe][sso][fake-boundary]") {
    const std::string invite = "t.me/+x";
    const std::array aliases{
        std::string_view{invite}, std::string_view{"https://t.me/+x"}, std::string_view{"t.me/+x"},
        std::string_view{"https://t.me/joinchat/x"}, std::string_view{"t.me/joinchat/x"}};
    WipeTrace trace;
    {
        FakeWrites fake;
        auto pending =
            fake.dispatch(chat_join_request(invite, std::nullopt, false, 2.0, trace.observer()));
        bind_principal(fake);
        const auto query_id = fake.observe_query(core::TdFunctionKind::GetInternalLinkType);
        for (const auto alias : aliases) {
            CAPTURE(alias);
            CHECK(redaction::InviteLinkRegistry::instance().redact(alias) != alias);
        }
        fake.push_response(query_id, core::TdError{.code = 404, .message = invite});
        REQUIRE(pending.get().error);
    }
    for (const auto alias : aliases) {
        CAPTURE(alias);
        CHECK(redaction::InviteLinkRegistry::instance().redact(alias) == alias);
        CHECK(trace.saw("invite_alias", alias.size()));
    }
    CHECK(trace.saw("sensitive_string_constructor_source", invite.size()));
    CHECK(trace.saw("sensitive_string_move_source", invite.size()));
    CHECK(trace.saw("td_internal_link_request_source", invite.size()));
}

TEST_CASE("unsupported invite classification wipes every returned link URL before terminal",
          "[write-command][chat][join][secrecy][wipe][fake-boundary]") {
    const std::string invite = "t.me/joinchat/UnsupportedUrlSentinel123";
    const std::string canonical = "https://t.me/+UnsupportedUrlSentinel123";
    WipeTrace trace;
    FakeWrites fake;
    auto pending =
        fake.dispatch(chat_join_request(invite, std::nullopt, false, 2.0, trace.observer()));
    bind_principal(fake);
    fake.respond(core::TdFunctionKind::GetInternalLinkType,
                 core::TdInternalLink{.kind = core::TdInternalLinkKind::Message,
                                      .username = {},
                                      .url = canonical,
                                      .tdlib_type_id = 1});
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "USAGE");
    CHECK(outcome.error->dump().find(invite) == std::string::npos);
    CHECK(outcome.error->dump().find(canonical) == std::string::npos);
    CHECK(trace.saw("td_internal_link_url", canonical.size()));
}

TEST_CASE("late invite planning response retains exact log redaction after timeout",
          "[write-command][chat][join][secrecy][redaction][lifetime][fake-boundary]") {
    const std::string invite = "t.me/joinchat/LatePlanningInviteSentinel123";
    WipeTrace trace;
    FakeWrites fake;
    auto sink = invite_log_sink(fake);

    auto pending = fake.dispatch(
        chat_join_request(invite, "late-planning-key", false, 0.05, trace.observer()));
    bind_principal(fake);
    const auto query_id = fake.observe_query(core::TdFunctionKind::GetInternalLinkType);
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK(trace.saw("chat_join_input", invite.size()));
    CHECK(redaction::InviteLinkRegistry::instance().redact(invite) != invite);

    append_invite_and_check_logs(*sink, {invite, "https://t.me/+LatePlanningInviteSentinel123"});

    fake.push_response(query_id, core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                                      .username = {},
                                                      .url = invite,
                                                      .tdlib_type_id = 1});
    wait_for_invite_release(invite);
}

TEST_CASE(
    "late invite planning response retains exact log redaction after cancellation or auth loss",
    "[write-command][chat][join][secrecy][redaction][lifetime][fake-boundary]") {
    SECTION("disconnect emits no terminal and retains protection until the late response") {
        const std::string invite = "t.me/joinchat/LatePlanningCancelSentinel123";
        WipeTrace trace;
        FakeWrites fake;
        auto sink = invite_log_sink(fake);
        std::shared_ptr<daemon::RequestSession> session;
        auto pending = fake.dispatch(
            chat_join_request(invite, "late-planning-cancel", false, 2.0, trace.observer()),
            &session);
        bind_principal(fake);
        const auto query_id = fake.observe_query(core::TdFunctionKind::GetInternalLinkType);
        REQUIRE(session);
        session->disconnect();
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 0);
        CHECK(trace.saw("chat_join_input", invite.size()));
        CHECK(redaction::InviteLinkRegistry::instance().redact(invite) != invite);
        append_invite_and_check_logs(*sink,
                                     {invite, "https://t.me/+LatePlanningCancelSentinel123"});

        fake.push_response(query_id,
                           core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                                .username = {},
                                                .url = invite,
                                                .tdlib_type_id = 1});
        wait_for_invite_release(invite);
    }

    SECTION("auth loss terminal retains protection until the late response") {
        const std::string invite = "t.me/joinchat/LatePlanningAuthSentinel123";
        WipeTrace trace;
        FakeWrites fake;
        auto sink = invite_log_sink(fake);
        auto pending = fake.dispatch(
            chat_join_request(invite, "late-planning-auth", false, 2.0, trace.observer()));
        bind_principal(fake);
        const auto query_id = fake.observe_query(core::TdFunctionKind::GetInternalLinkType);
        fake.push_authorization(core::AuthStateData{core::AuthState::WaitPhoneNumber});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(trace.saw("chat_join_input", invite.size()));
        CHECK(redaction::InviteLinkRegistry::instance().redact(invite) != invite);
        append_invite_and_check_logs(*sink, {invite, "https://t.me/+LatePlanningAuthSentinel123"});

        fake.push_response(query_id,
                           core::TdInternalLink{.kind = core::TdInternalLinkKind::ChatInvite,
                                                .username = {},
                                                .url = invite,
                                                .tdlib_type_id = 1});
        wait_for_invite_release(invite);
    }
}

TEST_CASE("post-dispatch invite query redaction survives terminal races until correlation release",
          "[write-command][chat][join][secrecy][redaction][lifetime][fake-boundary]") {
    const auto joined = [] {
        return core::TdChatJoinResult{.kind = core::TdChatJoinResultKind::Success,
                                      .chat_id = -1001,
                                      .guard_bot_user_id = std::nullopt,
                                      .guard_query_id = std::nullopt,
                                      .unsupported_tdlib_type_id = std::nullopt};
    };

    SECTION("deadline terminal retains protection until the late response") {
        const std::string invite = "https://t.me/+LateDispatchTimeoutSentinel123";
        WipeTrace trace;
        FakeWrites fake;
        auto sink = invite_log_sink(fake);
        auto pending = fake.dispatch(
            chat_join_request(invite, "late-dispatch-timeout", false, 0.1, trace.observer()));
        const auto query_id = reach_invite_dispatch(fake, invite);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK(trace.saw("chat_join_input", invite.size()));
        CHECK((trace.saw("td_join_invite", invite.size()) ||
               trace.saw("td_join_invite_move_source", invite.size())));
        CHECK(redaction::InviteLinkRegistry::instance().redact(invite) != invite);
        append_invite_and_check_logs(*sink,
                                     {invite, "t.me/joinchat/LateDispatchTimeoutSentinel123"});
        fake.push_response(query_id, joined());
        wait_for_invite_release(invite);
    }

    SECTION("disconnect emits no terminal and retains protection until the late response") {
        const std::string invite = "https://t.me/+LateDispatchCancelSentinel123";
        WipeTrace trace;
        FakeWrites fake;
        auto sink = invite_log_sink(fake);
        std::shared_ptr<daemon::RequestSession> session;
        auto pending = fake.dispatch(
            chat_join_request(invite, "late-dispatch-cancel", false, 2.0, trace.observer()),
            &session);
        const auto query_id = reach_invite_dispatch(fake, invite);
        REQUIRE(session);
        session->disconnect();
        const auto outcome = pending.get();
        CHECK_FALSE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 0);
        CHECK(trace.saw("chat_join_input", invite.size()));
        CHECK((trace.saw("td_join_invite", invite.size()) ||
               trace.saw("td_join_invite_move_source", invite.size())));
        CHECK(redaction::InviteLinkRegistry::instance().redact(invite) != invite);
        append_invite_and_check_logs(*sink,
                                     {invite, "t.me/joinchat/LateDispatchCancelSentinel123"});
        fake.push_response(query_id, joined());
        wait_for_invite_release(invite);
    }

    SECTION("auth loss terminal retains protection until the late response") {
        const std::string invite = "https://t.me/+LateDispatchAuthSentinel123";
        WipeTrace trace;
        FakeWrites fake;
        auto sink = invite_log_sink(fake);
        auto pending = fake.dispatch(
            chat_join_request(invite, "late-dispatch-auth", false, 2.0, trace.observer()));
        const auto query_id = reach_invite_dispatch(fake, invite);
        fake.push_authorization(core::AuthStateData{core::AuthState::WaitPhoneNumber});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(trace.saw("chat_join_input", invite.size()));
        CHECK((trace.saw("td_join_invite", invite.size()) ||
               trace.saw("td_join_invite_move_source", invite.size())));
        CHECK(redaction::InviteLinkRegistry::instance().redact(invite) != invite);
        append_invite_and_check_logs(*sink, {invite, "t.me/joinchat/LateDispatchAuthSentinel123"});
        fake.push_response(query_id, joined());
        wait_for_invite_release(invite);
    }

    SECTION("generation close releases the pending correlation protection") {
        const std::string invite = "https://t.me/+GenerationCloseInviteSentinel123";
        WipeTrace trace;
        FakeWrites fake;
        auto pending = fake.dispatch(
            chat_join_request(invite, "generation-close-invite", false, 2.0, trace.observer()));
        static_cast<void>(reach_invite_dispatch(fake, invite));
        fake.push_authorization(core::AuthStateData{core::AuthState::Closed});
        static_cast<void>(pending.get());
        CHECK(trace.saw("chat_join_input", invite.size()));
        CHECK((trace.saw("td_join_invite", invite.size()) ||
               trace.saw("td_join_invite_move_source", invite.size())));
        wait_for_invite_release(invite);
    }

    SECTION("TD error terminal wipes response and direct outcome owners") {
        const std::string invite = "t.me/+e";
        WipeTrace trace;
        FakeWrites fake;
        auto pending = fake.dispatch(
            chat_join_request(invite, "dispatch-error-invite", false, 2.0, trace.observer()));
        const auto query_id = reach_invite_dispatch(fake, invite);
        fake.push_response(query_id, core::TdError{.code = 400, .message = invite});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK(outcome.error->dump().find(invite) == std::string::npos);
        CHECK(trace.saw("direct_td_error_source", invite.size()));
        CHECK(trace.saw("direct_td_error", invite.size()));
        wait_for_invite_release(invite);
    }
}

TEST_CASE("chat leave requires immutable-plan confirmation and supports dry-run",
          "[write-command][chat][leave][confirmation][fake-boundary]") {
    SECTION("non-TTY invocation without yes is rejected before intent") {
        FakeWrites fake;
        auto pending = fake.dispatch(chat_target_request("leave"));
        resolve_basic(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
        CHECK((*outcome.error)["error"]["details"]["target"]["chat"]["id"] == -1001);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }

    SECTION("yes confirms one leave mutation") {
        FakeWrites fake;
        const auto confirmed =
            chat_target_request("leave", "-1001", "leave-replay-key", false, true);
        auto pending = fake.dispatch(confirmed);
        resolve_basic(fake);
        fake.respond(core::TdFunctionKind::LeaveChat, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"chat_id", -1001}, {"left", true}});
        CHECK(fake.count(core::TdFunctionKind::LeaveChat) == 1);
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-leave.result.schema.json"));

        auto unconfirmed_request = confirmed;
        unconfirmed_request.context.yes = false;
        auto unconfirmed = fake.dispatch(unconfirmed_request);
        bind_principal(fake);
        const auto rejected = unconfirmed.get();
        REQUIRE(rejected.error);
        CHECK((*rejected.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
        CHECK((*rejected.error)["error"]["details"]["target"]["chat"]["title"] == "Project");
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 1);

        auto replay = fake.dispatch(confirmed);
        bind_principal(fake);
        CHECK(replay.get().result == outcome.result);
        CHECK(fake.count(core::TdFunctionKind::LeaveChat) == 1);
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 1);
    }

    SECTION("dry-run plans without confirmation or persistence") {
        FakeWrites fake(false);
        auto pending = fake.dispatch(chat_target_request("leave", "-1001", std::nullopt, true));
        resolve_basic(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK(fake.count(core::TdFunctionKind::LeaveChat) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_THAT(*outcome.result,
                   tgcli::test::matches_json_schema("chat-leave.result.schema.json"));
    }
}

TEST_CASE("chat mutation handlers enforce the closed bot matrix before target reads",
          "[write-command][chat][bot][fake-boundary]") {
    const std::vector<proto::Request> user_only{
        chat_target_request("mark-read"), chat_mute_request(true, 3600),
        chat_mute_request(false, 0),      chat_target_request("pin"),
        chat_target_request("unpin"),     chat_target_request("archive"),
        chat_target_request("unarchive"), chat_join_request("@project"),
    };
    for (const auto& request : user_only) {
        CAPTURE(request.command);
        FakeWrites fake;
        auto pending = fake.dispatch(request);
        auto bot = self();
        bot.is_bot = true;
        fake.respond(core::TdFunctionKind::GetMe, bot);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "BOT_UNSUPPORTED");
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 0);
        CHECK(fake.count(core::TdFunctionKind::JoinChat) == 0);
    }

    FakeWrites fake;
    auto pending = fake.dispatch(chat_target_request("leave", "-1001", std::nullopt, false, true));
    auto bot = self();
    bot.is_bot = true;
    fake.respond(core::TdFunctionKind::GetMe, bot);
    fake.respond(core::TdFunctionKind::GetChat, basic_chat());
    fake.respond(core::TdFunctionKind::LeaveChat, core::TdOk{});
    REQUIRE(pending.get().result);
    CHECK(fake.count(core::TdFunctionKind::LeaveChat) == 1);
}

TEST_CASE("M6 contact mutations use the shared audited direct-write kernel",
          "[m6][write-command][contact][fake-boundary]") {
    const std::array cases{
        std::pair{proto::M6Operation::ContactAdd, core::TdFunctionKind::AddContact},
        std::pair{proto::M6Operation::ContactRemove, core::TdFunctionKind::RemoveContacts},
        std::pair{proto::M6Operation::ContactBlock,
                  core::TdFunctionKind::SetMessageSenderBlockList},
        std::pair{proto::M6Operation::ContactUnblock,
                  core::TdFunctionKind::SetMessageSenderBlockList},
    };
    for (const auto& [operation, function] : cases) {
        CAPTURE(proto::m6_operation_identity(operation)->canonical_name);
        FakeWrites fake;
        const auto command = std::string(proto::m6_operation_identity(operation)->command_path);
        auto pending = fake.m6_mutation(
            operation, contact_mutation_request(command.substr(command.find(' ') + 1)));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        const auto descriptor = fake.respond(function, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(outcome.terminal_count == 1);
        CHECK((*outcome.result)["user"]["id"] == 77);
        if (operation == proto::M6Operation::ContactAdd ||
            operation == proto::M6Operation::ContactRemove) {
            CHECK((*outcome.result)["is_contact"] == (operation == proto::M6Operation::ContactAdd));
        } else {
            CHECK((*outcome.result)["blocked"] == (operation == proto::M6Operation::ContactBlock));
        }
        CHECK(descriptor.kind() == function);
    }

    SECTION("dry-run performs planning but no mutating TD call or persistence") {
        FakeWrites fake(false);
        auto pending = fake.m6_mutation(proto::M6Operation::ContactAdd,
                                        contact_mutation_request("add", std::nullopt, true));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK((*outcome.result)["plan"]["phone_number_sha256"].get<std::string>().starts_with(
            "sha256:"));
        CHECK(fake.count(core::TdFunctionKind::AddContact) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("dry-run rejects a principal sequence replaced during planning") {
        FakeWrites fake(false);
        fake.advance_authorization_before_principal_cas();
        auto pending = fake.m6_mutation(proto::M6Operation::ContactAdd,
                                        contact_mutation_request("add", std::nullopt, true));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::AddContact) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("same-user auth-sequence replacement before intent leaves no durable write") {
        FakeWrites fake;
        fake.advance_authorization_before_principal_cas();
        auto pending =
            fake.m6_mutation(proto::M6Operation::ContactAdd, contact_mutation_request("add"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::AddContact) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("same-user auth replacement after intent still precedes contact submission") {
        FakeWrites fake;
        fake.advance_authorization_before_principal_cas(true);
        auto pending =
            fake.m6_mutation(proto::M6Operation::ContactAdd, contact_mutation_request("add"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::AddContact) == 0);
        const auto audit = read_bytes(fake.tree().audit_path());
        CHECK(audit.find(R"("phase":"intent")") != std::string::npos);
        CHECK(audit.find(R"("phase":"outcome")") != std::string::npos);
        CHECK(audit.find(R"("stage":"dispatch_started")") == std::string::npos);
    }

    SECTION("replacement generation before intent leaves no durable write") {
        FakeWrites fake;
        fake.replace_generation_before_principal_cas();
        auto pending =
            fake.m6_mutation(proto::M6Operation::ContactAdd, contact_mutation_request("add"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::AddContact) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("completed replay returns before target resolution and hydration") {
        FakeWrites fake;
        auto request = contact_mutation_request("add");
        request.context.idempotency_key = "contact-replay";
        auto first = fake.m6_mutation(proto::M6Operation::ContactAdd, request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        CHECK(first.wait_for(100ms) == std::future_status::timeout);
        fake.respond(core::TdFunctionKind::AddContact, core::TdM6Response{core::TdM6Ok{}});
        REQUIRE(first.get().result);
        const auto target_reads = fake.count(core::TdFunctionKind::GetUser);
        const auto mutations = fake.count(core::TdFunctionKind::AddContact);

        auto replay = fake.m6_mutation(proto::M6Operation::ContactAdd, request);
        bind_principal(fake);
        const auto outcome = replay.get();
        REQUIRE(outcome.result);
        CHECK(fake.count(core::TdFunctionKind::GetUser) == target_reads);
        CHECK(fake.count(core::TdFunctionKind::AddContact) == mutations);
    }
}

TEST_CASE("M6 folder mutations preserve full snapshots through the shared kernel",
          "[m6][write-command][folder][fake-boundary]") {
    SECTION("create resolves all chats atomically and sorts unique ids") {
        FakeWrites fake;
        auto pending = fake.m6_mutation(proto::M6Operation::FolderCreate, folder_create_request());
        bind_principal(fake);
        for (const auto id : {-1002, -1001, -1001}) {
            auto chat = basic_chat();
            chat.id = id;
            fake.respond(core::TdFunctionKind::GetChat, chat);
        }
        auto created = folder_info();
        created.name.animate_custom_emoji = false;
        fake.respond(core::TdFunctionKind::CreateChatFolder, core::TdM6Response{created});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["folder"]["included_chat_ids"] == json::array({-1002, -1001}));
        CHECK((*outcome.result)["folder"]["name"]["animate_custom_emoji"] == false);
        CHECK((*outcome.result)["folder"]["icon"] == "work");
    }

    SECTION("write rejects a replacement-generation folder cache before snapshot read") {
        FakeWrites fake;
        auto pending = fake.m6_mutation(proto::M6Operation::FolderEdit, folder_edit_request());
        bind_principal(fake);
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
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::GetChatFolder) == 0);
        CHECK(fake.count(core::TdFunctionKind::EditChatFolder) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("add-chat removes excluded membership and preserves animated folder name") {
        FakeWrites fake;
        fake.push_update(core::TdM6ChatFoldersUpdate{.folders = {folder_info()}});
        auto pending =
            fake.m6_mutation(proto::M6Operation::FolderAddChat, folder_membership_request(true));
        bind_principal(fake);
        auto chat = basic_chat();
        chat.id = -1002;
        fake.respond(core::TdFunctionKind::GetChat, chat);
        fake.respond(core::TdFunctionKind::GetChatFolder,
                     core::TdM6Response{core::TdM6MaybeChatFolder{folder_snapshot()}});
        fake.respond(core::TdFunctionKind::EditChatFolder, core::TdM6Response{folder_info()});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["included"] == true);
        CHECK((*outcome.result)["folder"]["included_chat_ids"] == json::array({-1001, -1002}));
        CHECK((*outcome.result)["folder"]["excluded_chat_ids"].empty());
        CHECK((*outcome.result)["folder"]["name"]["animate_custom_emoji"] == true);
    }

    SECTION("remove-chat moves included membership to excluded without a reread") {
        FakeWrites fake;
        fake.push_update(core::TdM6ChatFoldersUpdate{.folders = {folder_info()}});
        auto pending = fake.m6_mutation(proto::M6Operation::FolderRemoveChat,
                                        folder_membership_request(false));
        bind_principal(fake);
        auto chat = basic_chat();
        chat.id = -1001;
        fake.respond(core::TdFunctionKind::GetChat, chat);
        fake.respond(core::TdFunctionKind::GetChatFolder,
                     core::TdM6Response{core::TdM6MaybeChatFolder{folder_snapshot()}});
        fake.respond(core::TdFunctionKind::EditChatFolder, core::TdM6Response{folder_info()});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["included"] == false);
        CHECK((*outcome.result)["folder"]["included_chat_ids"].empty());
        CHECK((*outcome.result)["folder"]["excluded_chat_ids"] == json::array({-1002, -1001}));
        CHECK(fake.count(core::TdFunctionKind::GetChatFolder) == 1);
    }

    SECTION("edit changes only metadata and preserves animation and membership") {
        FakeWrites fake;
        fake.push_update(core::TdM6ChatFoldersUpdate{.folders = {folder_info()}});
        auto pending = fake.m6_mutation(proto::M6Operation::FolderEdit, folder_edit_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChatFolder,
                     core::TdM6Response{core::TdM6MaybeChatFolder{folder_snapshot()}});
        auto edited = folder_info();
        edited.name.text = "Other";
        fake.respond(core::TdFunctionKind::EditChatFolder, core::TdM6Response{edited});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["folder"]["name"]["text"] == "Other");
        CHECK((*outcome.result)["folder"]["name"]["animate_custom_emoji"] == true);
        CHECK((*outcome.result)["folder"]["included_chat_ids"] == json::array({-1001}));
    }

    SECTION("edit rejects every returned folder-info field mismatch") {
        enum class Field { Id, Text, Animation, Entities, Icon, Color, Shareable, InviteLinks };
        for (const auto field : {Field::Id, Field::Text, Field::Animation, Field::Entities,
                                 Field::Icon, Field::Color, Field::Shareable, Field::InviteLinks}) {
            CAPTURE(static_cast<int>(field));
            FakeWrites fake;
            fake.push_update(core::TdM6ChatFoldersUpdate{.folders = {folder_info()}});
            auto pending = fake.m6_mutation(proto::M6Operation::FolderEdit, folder_edit_request());
            bind_principal(fake);
            fake.respond(core::TdFunctionKind::GetChatFolder,
                         core::TdM6Response{core::TdM6MaybeChatFolder{folder_snapshot()}});
            auto returned = folder_info();
            returned.name.text = "Other";
            switch (field) {
            case Field::Id:
                returned.id = 8;
                break;
            case Field::Text:
                returned.name.text = "Different";
                break;
            case Field::Animation:
                returned.name.animate_custom_emoji = false;
                break;
            case Field::Entities:
                returned.name.custom_emoji_entities.push_back(
                    {.offset = 0, .length = 1, .custom_emoji_id = "1"});
                break;
            case Field::Icon:
                returned.icon = core::TdM6FolderIcon::Custom;
                break;
            case Field::Color:
                returned.color_id = 3;
                break;
            case Field::Shareable:
                returned.is_shareable = true;
                break;
            case Field::InviteLinks:
                returned.has_my_invite_links = true;
                break;
            }
            fake.respond(core::TdFunctionKind::EditChatFolder,
                         core::TdM6Response{std::move(returned)});
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
            CHECK((*outcome.error)["error"]["details"]["reason"] == "malformed_tdlib_response");
            CHECK(outcome.terminal_count == 1);
        }
    }

    SECTION("delete confirms the frozen folder and performs one mutation") {
        FakeWrites fake;
        fake.push_update(core::TdM6ChatFoldersUpdate{.folders = {folder_info()}});
        auto pending = fake.m6_mutation(proto::M6Operation::FolderDelete, folder_delete_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChatFolder,
                     core::TdM6Response{core::TdM6MaybeChatFolder{folder_snapshot()}});
        fake.respond(core::TdFunctionKind::DeleteChatFolder, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"folder_id", 7}, {"deleted", true}});
        CHECK(fake.count(core::TdFunctionKind::DeleteChatFolder) == 1);
    }

    SECTION("delete confirmation has one terminal for every rejection and cancellation path") {
        enum class Mode { NoTty, TtyNo, TtyYes, Deadline, Disconnect };
        for (const auto mode :
             {Mode::NoTty, Mode::TtyNo, Mode::TtyYes, Mode::Deadline, Mode::Disconnect}) {
            CAPTURE(static_cast<int>(mode));
            FakeWrites fake;
            fake.push_update(core::TdM6ChatFoldersUpdate{.folders = {folder_info()}});
            auto request = folder_delete_request();
            request.context.yes = false;
            request.context.tty = mode != Mode::NoTty;
            if (mode == Mode::Deadline) {
                request.context.timeout_seconds = 1.0;
            }
            std::shared_ptr<daemon::RequestSession> session;
            auto challenge_seen = std::make_shared<std::atomic<bool>>(false);
            daemon::CallbackSink::ChallengeFn challenge;
            if (mode != Mode::NoTty) {
                challenge = [mode, &session, challenge_seen](json value) -> std::optional<json> {
                    challenge_seen->store(value["details"]["action"] == "folder_delete",
                                          std::memory_order_release);
                    if (mode == Mode::Disconnect) {
                        session->disconnect();
                        return std::nullopt;
                    }
                    if (mode == Mode::Deadline) {
                        return std::nullopt;
                    }
                    return json{{"nonce", value["nonce"]},
                                {"sequence", value["sequence"]},
                                {"client_generation", value["client_generation"]},
                                {"auth_sequence", value["auth_sequence"]},
                                {"value", mode == Mode::TtyYes}};
                };
            }
            auto pending = fake.m6_mutation(proto::M6Operation::FolderDelete, request, &session,
                                            std::move(challenge));
            bind_principal(fake);
            fake.respond(core::TdFunctionKind::GetChatFolder,
                         core::TdM6Response{core::TdM6MaybeChatFolder{folder_snapshot()}});
            if (mode == Mode::TtyYes) {
                fake.respond(core::TdFunctionKind::DeleteChatFolder,
                             core::TdM6Response{core::TdM6Ok{}});
            }
            const auto outcome = pending.get();
            if (mode == Mode::TtyYes) {
                REQUIRE(outcome.result);
                CHECK((*outcome.result)["deleted"] == true);
            } else if (mode == Mode::Disconnect) {
                CHECK(outcome.terminal_count == 0);
            } else {
                REQUIRE(outcome.error);
                std::string_view expected = "CONFIRMATION_REQUIRED";
                if (mode == Mode::Deadline) {
                    expected = "TIMEOUT";
                }
                CHECK((*outcome.error)["error"]["code"] == expected);
                CHECK(outcome.terminal_count == 1);
            }
            CHECK(fake.count(core::TdFunctionKind::DeleteChatFolder) ==
                  (mode == Mode::TtyYes ? 1 : 0));
            CHECK(challenge_seen->load(std::memory_order_acquire) == (mode != Mode::NoTty));
        }
    }
}

TEST_CASE("M6 topic mutations bind forum capability and strict topic state",
          "[m6][write-command][topic][fake-boundary]") {
    SECTION("create binds forum membership before one typed mutation") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::TopicCreate, topic_mutation_request("create"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = topic_admin_status()}});
        fake.respond(core::TdFunctionKind::CreateForumTopic, core::TdM6Response{topic_info()});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["topic"]["id"] == 9);
        CHECK((*outcome.result)["topic"]["name"] == "Updates");
    }

    SECTION("close validates current topic then returns planned state without reread") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::TopicClose, topic_mutation_request("close"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
        fake.respond(core::TdFunctionKind::GetForumTopic,
                     core::TdM6Response{core::TdM6MaybeForumTopic{
                         core::TdM6ForumTopic{.info = topic_info(false)}}});
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = topic_admin_status()}});
        fake.respond(core::TdFunctionKind::ToggleForumTopicIsClosed,
                     core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["chat"]["id"] == -1001);
        CHECK((*outcome.result)["topic_id"] == 9);
        CHECK((*outcome.result)["closed"] == true);
        CHECK(fake.count(core::TdFunctionKind::GetForumTopic) == 1);
    }

    SECTION("edit returns the frozen name without a post-mutation topic read") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::TopicEdit, topic_mutation_request("edit"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
        fake.respond(core::TdFunctionKind::GetForumTopic,
                     core::TdM6Response{core::TdM6MaybeForumTopic{
                         core::TdM6ForumTopic{.info = topic_info(false)}}});
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = topic_admin_status()}});
        fake.respond(core::TdFunctionKind::EditForumTopic, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["topic_id"] == 9);
        CHECK((*outcome.result)["name"] == "Other");
        CHECK(fake.count(core::TdFunctionKind::GetForumTopic) == 1);
    }

    SECTION("reopen validates a closed topic and returns the planned open state") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::TopicReopen, topic_mutation_request("reopen"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
        fake.respond(core::TdFunctionKind::GetForumTopic,
                     core::TdM6Response{core::TdM6MaybeForumTopic{
                         core::TdM6ForumTopic{.info = topic_info(true)}}});
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = topic_admin_status()}});
        fake.respond(core::TdFunctionKind::ToggleForumTopicIsClosed,
                     core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["closed"] == false);
        CHECK(fake.count(core::TdFunctionKind::GetForumTopic) == 1);
    }
}

TEST_CASE("M6 administration and storage use typed plans without post-mutation rereads",
          "[m6][write-command][admin][storage][fake-boundary]") {
    SECTION("set-title checks the current member right") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::ChatSetTitle, chat_admin_request("set-title"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status()}});
        fake.respond(core::TdFunctionKind::SetChatTitle, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["chat"]["id"] == -1001);
        CHECK((*outcome.result)["title"] == "Renamed");
    }

    SECTION("set-title uses native chat permissions for a regular member") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::ChatSetTitle, chat_admin_request("set-title"));
        bind_principal(fake);
        auto chat = basic_chat();
        core::TdM6ChatPermissions permissions;
        permissions.can_change_info = true;
        chat.permissions = permissions;
        fake.respond(core::TdFunctionKind::GetChat, chat);
        core::TdM6MemberStatus member;
        member.kind = core::TdM6MemberStatusKind::Member;
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = member}});
        fake.respond(core::TdFunctionKind::SetChatTitle, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["title"] == "Renamed");
    }

    SECTION("set-description emits the frozen text without a chat reread") {
        FakeWrites fake;
        auto pending = fake.m6_mutation(proto::M6Operation::ChatSetDescription,
                                        chat_admin_request("set-description"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status()}});
        fake.respond(core::TdFunctionKind::SetChatDescription, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["description"] == "Description");
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 1);
    }

    SECTION("promote binds caller and target snapshots") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::ChatPromote, chat_admin_request("promote"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status(true)}});
        fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
        core::TdM6MemberStatus target_status;
        target_status.kind = core::TdM6MemberStatusKind::Member;
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 77,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = target_status}});
        fake.respond(core::TdFunctionKind::SetChatMemberStatus, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["status"] == "administrator");
        CHECK((*outcome.result)["can_manage_chat"] == true);
        CHECK((*outcome.result)["rights"] == json::array({"change-info"}));
    }

    SECTION("member lookup taxonomy is exact and malformed statuses are structural errors") {
        enum class Reply { Missing, Other400, WrongVariant, UnknownStatus };
        for (const auto reply :
             {Reply::Missing, Reply::Other400, Reply::WrongVariant, Reply::UnknownStatus}) {
            CAPTURE(static_cast<int>(reply));
            FakeWrites fake;
            auto pending = fake.m6_mutation(proto::M6Operation::ChatBan, chat_admin_request("ban"));
            bind_principal(fake);
            fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
            fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
            fake.respond(core::TdFunctionKind::GetChatMember,
                         core::TdM6Response{core::TdM6ChatMember{
                             .member = {.kind = core::TdM6SenderKind::User,
                                        .id = 42,
                                        .unsupported_tdlib_type_id = std::nullopt},
                             .status = chat_admin_status(true)}});
            fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
            if (reply == Reply::Missing) {
                fake.respond(core::TdFunctionKind::GetChatMember,
                             core::TdError{.code = 400, .message = "Member not found"});
            } else if (reply == Reply::Other400) {
                fake.respond(core::TdFunctionKind::GetChatMember,
                             core::TdError{.code = 400, .message = "member not found"});
            } else if (reply == Reply::WrongVariant) {
                fake.respond(core::TdFunctionKind::GetChatMember,
                             core::TdM6Response{core::TdM6Ok{}});
            } else {
                core::TdM6MemberStatus unknown;
                unknown.kind = core::TdM6MemberStatusKind::Unknown;
                fake.respond(core::TdFunctionKind::GetChatMember,
                             core::TdM6Response{core::TdM6ChatMember{
                                 .member = {.kind = core::TdM6SenderKind::User,
                                            .id = 77,
                                            .unsupported_tdlib_type_id = std::nullopt},
                                 .status = unknown}});
            }
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            if (reply == Reply::Missing) {
                CHECK((*outcome.error)["error"]["code"] == "NOT_FOUND");
                CHECK((*outcome.error)["error"]["details"] ==
                      json{{"operation", "chat_ban"}, {"chat_id", -1001}, {"user_id", 77}});
            } else if (reply == Reply::Other400) {
                CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
                CHECK((*outcome.error)["error"]["details"]["tdlib_code"] == 400);
            } else {
                CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
                CHECK((*outcome.error)["error"]["details"]["reason"] == "malformed_tdlib_response");
            }
            CHECK(fake.count(core::TdFunctionKind::SetChatMemberStatus) == 0);
            CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        }
    }
}

TEST_CASE("M6 photo administration and destructive operations preserve durable truth",
          "[m6][write-command][admin][storage][fake-boundary]") {
    SECTION("set-photo publishes only a validated private static-JPEG spool") {
        FakeWrites fake;
        const std::string jpeg{static_cast<char>(0xff), static_cast<char>(0xd8), 'x',
                               static_cast<char>(0xff), static_cast<char>(0xd9)};
        fake.tree().write_source(jpeg, "avatar.JPEG");
        auto request = chat_admin_request("set-photo");
        request.args = {{"chat", "-1001"}, {"path", fake.tree().source_path("avatar.JPEG")}};
        auto pending = fake.m6_mutation(proto::M6Operation::ChatSetPhoto, request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status()}});
        const auto function =
            fake.respond(core::TdFunctionKind::SetChatPhoto, core::TdM6Response{core::TdM6Ok{}});
        CHECK(function_field<bool>(function, "delete") == false);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["chat"]["id"] == -1001);
        CHECK((*outcome.result)["chat"]["type"] == "basic_group");
        CHECK((*outcome.result)["photo"] == "set");
        const auto spool_root = fake.tree().account_state() + "/spool";
        CHECK((!std::filesystem::exists(spool_root) || std::filesystem::is_empty(spool_root)));
    }

    SECTION("set-photo principal replacement after pass one creates no intent or spool") {
        FakeWrites fake;
        const std::string jpeg{static_cast<char>(0xff), static_cast<char>(0xd8), 'x',
                               static_cast<char>(0xff), static_cast<char>(0xd9)};
        fake.tree().write_source(jpeg, "principal.JPEG");
        auto request = chat_admin_request("set-photo");
        request.args = {{"chat", "-1001"}, {"path", fake.tree().source_path("principal.JPEG")}};
        fake.advance_authorization_before_principal_cas();
        auto pending = fake.m6_mutation(proto::M6Operation::ChatSetPhoto, request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status()}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::SetChatPhoto) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
    }

    SECTION("photo audit contradiction precedes missing-source validation and target planning") {
        FakeWrites fake;
        const auto spool = fake.tree().account_state() + "/spool";
        const auto contradiction = spool + "/not-an-invocation";
        REQUIRE(std::filesystem::create_directory(spool));
        REQUIRE(::chmod(spool.c_str(), 0700) == 0);
        REQUIRE(std::filesystem::create_directory(contradiction));
        REQUIRE(::chmod(contradiction.c_str(), 0700) == 0);
        auto source_stages = std::make_shared<std::atomic<std::size_t>>(0);
        auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        hooks->at_stage = [source_stages](daemon::FileSpoolStage) { ++*source_stages; };
        fake.file_spool_hooks(hooks);
        auto request = chat_admin_request("set-photo");
        request.args = {{"chat", "-1001"}, {"path", fake.tree().source_path("missing.JPEG")}};
        auto pending = fake.m6_mutation(proto::M6Operation::ChatSetPhoto, request);
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "AUDIT_INCOMPLETE");
        CHECK(*source_stages == 0);
        CHECK(fake.count(core::TdFunctionKind::GetChat) == 0);
        CHECK(fake.count(core::TdFunctionKind::SetChatPhoto) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
        CHECK_FALSE(std::filesystem::exists(fake.tree().store_path()));
    }

    SECTION("set-photo delete uses no source spool and returns the exact state") {
        FakeWrites fake;
        auto request = chat_admin_request("set-photo");
        request.args = {{"chat", "-1001"}, {"delete", true}};
        auto pending = fake.m6_mutation(proto::M6Operation::ChatSetPhoto, request);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status()}});
        const auto function =
            fake.respond(core::TdFunctionKind::SetChatPhoto, core::TdM6Response{core::TdM6Ok{}});
        CHECK(function_field<bool>(function, "delete") == true);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["photo"] == "deleted");
        CHECK_FALSE(std::filesystem::exists(fake.tree().account_state() + "/spool"));
    }

    SECTION("set-photo pass-two cancellation closes durable state without dispatch") {
        for (const bool keyed : {false, true}) {
            CAPTURE(keyed);
            FakeWrites fake;
            const std::string jpeg{static_cast<char>(0xff), static_cast<char>(0xd8), 'x',
                                   static_cast<char>(0xff), static_cast<char>(0xd9)};
            fake.tree().write_source(jpeg, "cancel.JPEG");
            auto request = chat_admin_request("set-photo");
            request.args = {{"chat", "-1001"}, {"path", fake.tree().source_path("cancel.JPEG")}};
            if (keyed) {
                request.context.idempotency_key = "photo-cancel";
            }
            std::shared_ptr<daemon::RequestSession> session;
            auto hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
            auto fired = std::make_shared<std::atomic<bool>>(false);
            hooks->at_stage = [fired, &session, keyed](daemon::FileSpoolStage stage) {
                if (stage == daemon::FileSpoolStage::DuringPass2Read && !fired->exchange(true)) {
                    if (keyed) {
                        session->disconnect();
                    } else {
                        session->shutdown();
                    }
                }
            };
            fake.file_spool_hooks(hooks);
            auto pending = fake.m6_mutation(proto::M6Operation::ChatSetPhoto, request, &session);
            bind_principal(fake);
            auto chat = basic_chat();
            core::TdM6ChatPermissions permissions;
            permissions.can_change_info = true;
            chat.permissions = permissions;
            fake.respond(core::TdFunctionKind::GetChat, chat);
            fake.respond(core::TdFunctionKind::GetChatMember,
                         core::TdM6Response{core::TdM6ChatMember{
                             .member = {.kind = core::TdM6SenderKind::User,
                                        .id = 42,
                                        .unsupported_tdlib_type_id = std::nullopt},
                             .status = chat_admin_status()}});
            const auto outcome = pending.get();
            CHECK(outcome.terminal_count == 0);
            CHECK(fake.count(core::TdFunctionKind::SetChatPhoto) == 0);
            const auto audit = read_bytes(fake.tree().audit_path());
            CHECK(audit.find(R"("phase":"outcome")") != std::string::npos);
            CHECK(audit.find(R"("mutation_state":"none")") != std::string::npos);
            const auto spool_root = fake.tree().account_state() + "/spool";
            CHECK((!std::filesystem::exists(spool_root) || std::filesystem::is_empty(spool_root)));
        }
    }

    SECTION("invite create returns the link while revoke keeps raw bytes out of audit") {
        {
            FakeWrites fake;
            auto pending =
                fake.m6_mutation(proto::M6Operation::ChatInviteLink, invite_link_request());
            bind_principal(fake);
            fake.respond(core::TdFunctionKind::GetChat, basic_chat());
            fake.respond(core::TdFunctionKind::GetChatMember,
                         core::TdM6Response{core::TdM6ChatMember{
                             .member = {.kind = core::TdM6SenderKind::User,
                                        .id = 42,
                                        .unsupported_tdlib_type_id = std::nullopt},
                             .status = chat_admin_status()}});
            core::TdM6ChatInviteLink link;
            link.invite_link = "https://t.me/+created";
            link.creator_user_id = 42;
            link.date = 1;
            fake.respond(core::TdFunctionKind::CreateChatInviteLink, core::TdM6Response{link});
            const auto outcome = pending.get();
            REQUIRE(outcome.result);
            CHECK((*outcome.result)["action"] == "create");
            CHECK((*outcome.result)["invite_link"] == "https://t.me/+created");
        }
        {
            constexpr std::string_view secret = "https://t.me/+revoked";
            FakeWrites fake;
            auto pending = fake.m6_mutation(proto::M6Operation::ChatInviteLink,
                                            invite_link_request(std::string(secret)));
            bind_principal(fake);
            fake.respond(core::TdFunctionKind::GetChat, basic_chat());
            fake.respond(core::TdFunctionKind::GetChatMember,
                         core::TdM6Response{core::TdM6ChatMember{
                             .member = {.kind = core::TdM6SenderKind::User,
                                        .id = 42,
                                        .unsupported_tdlib_type_id = std::nullopt},
                             .status = chat_admin_status()}});
            core::TdM6ChatInviteLink revoked;
            revoked.invite_link = secret;
            revoked.creator_user_id = 42;
            revoked.date = 1;
            revoked.is_revoked = true;
            fake.respond(core::TdFunctionKind::RevokeChatInviteLink,
                         core::TdM6Response{core::TdM6ChatInviteLinks{
                             .total_count = 1, .invite_links = {std::move(revoked)}}});
            const auto outcome = pending.get();
            REQUIRE(outcome.result);
            CHECK((*outcome.result)["action"] == "revoke");
            CHECK((*outcome.result)["invite_link"].is_null());
            CHECK(read_bytes(fake.tree().audit_path()).find(secret) == std::string::npos);
        }
    }

    SECTION("invite revoke rejects a structurally inconsistent total before result projection") {
        constexpr std::string_view secret = "https://t.me/+invalid-total";
        FakeWrites fake;
        auto pending = fake.m6_mutation(proto::M6Operation::ChatInviteLink,
                                        invite_link_request(std::string(secret)));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, basic_chat());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status()}});
        core::TdM6ChatInviteLink revoked;
        revoked.invite_link = secret;
        revoked.creator_user_id = 42;
        revoked.date = 1;
        revoked.is_revoked = true;
        fake.respond(core::TdFunctionKind::RevokeChatInviteLink,
                     core::TdM6Response{core::TdM6ChatInviteLinks{
                         .total_count = 2, .invite_links = {std::move(revoked)}}});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(outcome.terminal_count == 1);
    }

    SECTION("member mutations and permissions cover every closed administration branch") {
        const std::array member_cases{
            std::pair{proto::M6Operation::ChatDemote, core::TdM6MemberStatusKind::Administrator},
            std::pair{proto::M6Operation::ChatBan, core::TdM6MemberStatusKind::Member},
            std::pair{proto::M6Operation::ChatUnban, core::TdM6MemberStatusKind::Banned},
            std::pair{proto::M6Operation::ChatKick, core::TdM6MemberStatusKind::Member},
        };
        for (const auto& [operation, before_kind] : member_cases) {
            CAPTURE(proto::m6_operation_identity(operation)->canonical_name);
            FakeWrites fake;
            auto request = chat_admin_request(
                std::string(proto::m6_operation_identity(operation)->command_path).substr(5));
            request.context.yes = operation == proto::M6Operation::ChatBan ||
                                  operation == proto::M6Operation::ChatKick;
            auto pending = fake.m6_mutation(operation, request);
            bind_principal(fake);
            fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
            fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
            fake.respond(core::TdFunctionKind::GetChatMember,
                         core::TdM6Response{core::TdM6ChatMember{
                             .member = {.kind = core::TdM6SenderKind::User,
                                        .id = 42,
                                        .unsupported_tdlib_type_id = std::nullopt},
                             .status = chat_admin_status(true)}});
            fake.respond(core::TdFunctionKind::GetUser, peer(core::TdUserPresence::Online));
            core::TdM6MemberStatus before;
            before.kind = before_kind;
            before.can_be_edited = true;
            before.rights.can_manage_chat =
                before_kind == core::TdM6MemberStatusKind::Administrator;
            fake.respond(core::TdFunctionKind::GetChatMember,
                         core::TdM6Response{core::TdM6ChatMember{
                             .member = {.kind = core::TdM6SenderKind::User,
                                        .id = 77,
                                        .unsupported_tdlib_type_id = std::nullopt},
                             .status = before}});
            fake.respond(core::TdFunctionKind::SetChatMemberStatus,
                         core::TdM6Response{core::TdM6Ok{}});
            const auto outcome = pending.get();
            REQUIRE(outcome.result);
            CHECK((*outcome.result)["user"]["id"] == 77);
            CHECK(fake.count(core::TdFunctionKind::SetChatMemberStatus) == 1);
        }

        FakeWrites fake;
        auto pending = fake.m6_mutation(proto::M6Operation::ChatSetPermissions,
                                        chat_admin_request("set-permissions"));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetChat, supergroup_chat());
        fake.respond(core::TdFunctionKind::GetSupergroup, forum_supergroup());
        fake.respond(core::TdFunctionKind::GetChatMember,
                     core::TdM6Response{
                         core::TdM6ChatMember{.member = {.kind = core::TdM6SenderKind::User,
                                                         .id = 42,
                                                         .unsupported_tdlib_type_id = std::nullopt},
                                              .status = chat_admin_status(true)}});
        fake.respond(core::TdFunctionKind::SetChatPermissions, core::TdM6Response{core::TdM6Ok{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["permissions"] == json::array({"send-basic-messages"}));
    }

    SECTION("storage optimize uses frozen TD defaults and validates returned sums") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::StorageOptimize, storage_optimize_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::OptimizeStorage,
                     core::TdM6Response{core::TdM6StorageStatistics{}});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["optimized"] == true);
        CHECK((*outcome.result)["statistics"] ==
              json{{"size", 0}, {"count", 0}, {"by_chat", json::array()}});
    }

    SECTION("storage optimize rejects a top-level sum mismatch") {
        FakeWrites fake;
        auto pending =
            fake.m6_mutation(proto::M6Operation::StorageOptimize, storage_optimize_request());
        bind_principal(fake);
        core::TdM6StorageStatistics malformed;
        malformed.size = 1;
        fake.respond(core::TdFunctionKind::OptimizeStorage, core::TdM6Response{malformed});
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK(outcome.terminal_count == 1);
    }
}

TEST_CASE("session terminate uses the audited direct path without idempotency or reread",
          "[m6][write-command][session][fake-boundary]") {
    const auto target = daemon::m6_session_terminate_target_json(active_sessions().items.front());
    REQUIRE(target);
    std::string contract_error;
    const daemon::WriteOperation session_operation{daemon::AccountAuditOperation::SessionTerminate};
    auto arguments = daemon::write_contract::make_arguments(session_operation,
                                                            {{"session_id", "7"}}, contract_error);
    INFO(contract_error);
    REQUIRE(arguments);
    auto plan = daemon::write_contract::make_plan(session_operation, "main",
                                                  {{"operation", "session_terminate"},
                                                   {"account", "main"},
                                                   {"tdlib_request", "terminateSession"},
                                                   {"session", *target}},
                                                  contract_error);
    INFO(contract_error);
    REQUIRE(plan);
    SECTION("real termination resolves one immutable non-current target") {
        FakeWrites fake;
        {
            std::ofstream store(fake.tree().store_path(), std::ios::binary | std::ios::trunc);
            REQUIRE(store.good());
            store << "not an idempotency store";
        }
        const auto store_before = read_bytes(fake.tree().store_path());
        auto pending = fake.terminate_session(session_terminate_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        fake.respond(core::TdFunctionKind::TerminateSession, core::TdOk{});
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"session_id", "7"}, {"terminated", true}});
        CHECK(fake.count(core::TdFunctionKind::GetActiveSessions) == 1);
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 1);
        CHECK(read_bytes(fake.tree().store_path()) == store_before);
    }

    SECTION("dry-run returns the strict plan without a mutation") {
        FakeWrites fake(false);
        auto pending = fake.terminate_session(session_terminate_request(true));
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK((*outcome.result)["dry_run"] == true);
        CHECK((*outcome.result)["plan"]["session"]["id"] == "7");
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
    }

    SECTION("dry-run reconciliation fails before Ready and target reads") {
        FakeWrites fake;
        const auto spool = fake.tree().account_state() + "/spool";
        REQUIRE(std::filesystem::create_directory(spool));
        REQUIRE(::chmod(spool.c_str(), 0755) == 0);
        const auto outcome = fake.terminate_session(session_terminate_request(true)).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "SPOOL_UNAVAILABLE");
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "session_terminate"},
                                                           {"path", "spool/"},
                                                           {"reason", "wrong_mode"}});
        CHECK(fake.count(core::TdFunctionKind::GetMe) == 0);
        CHECK(fake.count(core::TdFunctionKind::GetActiveSessions) == 0);
    }

    SECTION("real reconciliation follows principal and authority but precedes target reads") {
        FakeWrites fake;
        const auto spool = fake.tree().account_state() + "/spool";
        REQUIRE(std::filesystem::create_directory(spool));
        REQUIRE(::chmod(spool.c_str(), 0755) == 0);
        auto pending = fake.terminate_session(session_terminate_request());
        bind_principal(fake);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "SPOOL_UNAVAILABLE");
        CHECK(fake.count(core::TdFunctionKind::GetMe) == 1);
        CHECK(fake.count(core::TdFunctionKind::GetActiveSessions) == 0);
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
    }

    SECTION("direct preparation deadline preserves the exact preflight timeout") {
        FakeWrites fake;
        fake.expire_direct_before_request();
        auto pending = fake.terminate_session(session_terminate_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"] == json{{"operation", "session_terminate"},
                                                           {"phase", "preflight"},
                                                           {"state", "ready"},
                                                           {"outcome", "not_started"},
                                                           {"idempotency", "not_requested"}});
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
    }

    SECTION("authorization loss during direct preparation preserves the first cause") {
        FakeWrites fake;
        fake.lose_authorization_before_direct_request();
        auto pending = fake.terminate_session(session_terminate_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "authorization_lost");
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
    }

    SECTION("same-user auth-sequence replacement before intent leaves no durable write") {
        FakeWrites fake;
        fake.advance_authorization_before_principal_cas();
        auto pending = fake.terminate_session(session_terminate_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
        CHECK_FALSE(std::filesystem::exists(fake.tree().audit_path()));
    }

    SECTION("same-user auth replacement after intent still precedes session submission") {
        FakeWrites fake;
        fake.advance_authorization_before_principal_cas(true);
        auto pending = fake.terminate_session(session_terminate_request());
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
        const auto audit = read_bytes(fake.tree().audit_path());
        CHECK(audit.find(R"("phase":"intent")") != std::string::npos);
        CHECK(audit.find(R"("phase":"outcome")") != std::string::npos);
        CHECK(audit.find(R"("stage":"dispatch_started")") == std::string::npos);
    }

    SECTION("disconnect during direct preparation emits no terminal or mutation") {
        FakeWrites fake;
        std::shared_ptr<daemon::RequestSession> session;
        fake.cancel_before_request(core::TdFunctionKind::TerminateSession, &session);
        auto pending = fake.terminate_session(session_terminate_request(), &session);
        bind_principal(fake);
        fake.respond(core::TdFunctionKind::GetActiveSessions, active_sessions());
        const auto outcome = pending.get();
        CHECK(outcome.terminal_count == 0);
        CHECK_FALSE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(fake.count(core::TdFunctionKind::TerminateSession) == 0);
    }
}
