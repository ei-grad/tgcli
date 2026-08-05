#include "core/td_client.hpp"

#include "core/query_registry.hpp"
#include "core/request_lifecycle.hpp"
#include "core/td_authorization.hpp"
#include "core/update_bus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tgcli::core {

namespace {

constexpr auto kCloseTimeout = std::chrono::seconds(30);
constexpr auto kReceiveTimeout = std::chrono::milliseconds(100);

std::future<TdValue> failed_future(const std::string& message) {
    std::promise<TdValue> promise;
    auto future = promise.get_future();
    promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
    return future;
}

std::future<TdValue> failed_future(TdAuthorizationFailure failure) {
    std::promise<TdValue> promise;
    auto future = promise.get_future();
    promise.set_exception(std::make_exception_ptr(TdAuthorizationError(failure)));
    return future;
}

TdClosedDecisionStatus terminal_decision(TdLifecycleClaimStatus claim) {
    switch (claim) {
    case TdLifecycleClaimStatus::Active:
        return TdClosedDecisionStatus::Pending;
    case TdLifecycleClaimStatus::Disconnected:
        return TdClosedDecisionStatus::Disconnected;
    case TdLifecycleClaimStatus::Shutdown:
        return TdClosedDecisionStatus::Shutdown;
    case TdLifecycleClaimStatus::TimedOut:
        return TdClosedDecisionStatus::TimedOut;
    case TdLifecycleClaimStatus::Rejected:
        return TdClosedDecisionStatus::Rejected;
    }
    return TdClosedDecisionStatus::Rejected;
}

} // namespace

struct TdSendLease::State {
    std::function<std::future<TdValue>(TdlibParameters)> submit;
};

struct TdOwnerLease::State {
    TdRequestOwner owner;
    std::function<void()> revoke;
};

struct TdClosedDecision::State {
    std::mutex mutex;
    TdClosedDecisionStatus status = TdClosedDecisionStatus::Pending;
    std::uint64_t query_id = 0;
    bool expected_transition = false;
    bool released = false;
    std::size_t callbacks_in_flight = 0;
    std::condition_variable callbacks_done;
    std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim;
    std::function<void()> release;
};

class TdClient::Impl {
  public:
    Impl(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging)
        : runtime_(std::move(runtime)) {
        if (runtime_ == nullptr) {
            throw std::invalid_argument("TdClient runtime must not be null");
        }
        runtime_->initialize_process(logging);
        activate_initial_generation();
        receive_thread_ = std::thread([this] { receive_loop(); });
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        close();
    }

    std::future<TdValue> send(TdSendDescriptor descriptor, TdValue request) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr) {
            return failed_future("tdlib client closed");
        }

        return generation->lifecycle.send([this, generation, descriptor = std::move(descriptor),
                                           request = std::move(request)]() mutable {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                return failed_future(TdAuthorizationFailure::AuthStateMismatch);
            }
            return submit_locked(generation, descriptor, std::move(request)).future;
        });
    }

    std::future<TdValue> send(TdSendDescriptor descriptor, TdlibParameters parameters) {
        return send(std::move(descriptor),
                    runtime_->make_set_tdlib_parameters(std::move(parameters)));
    }

    std::future<TdValue> send_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdFunctionKind function, TdValue request) {
        if (!authorization ||
            (function != TdFunctionKind::GetOption && function != TdFunctionKind::GetMe &&
             function != TdFunctionKind::GetSavedMessagesTags &&
             function != TdFunctionKind::SearchSavedMessages &&
             function != TdFunctionKind::GetActiveSessions && function != TdFunctionKind::GetChat &&
             function != TdFunctionKind::GetChats && function != TdFunctionKind::LoadChats &&
             function != TdFunctionKind::SearchPublicChat &&
             function != TdFunctionKind::GetInternalLinkType &&
             function != TdFunctionKind::GetMessageLinkInfo &&
             function != TdFunctionKind::CheckChatInviteLink &&
             function != TdFunctionKind::GetUser && function != TdFunctionKind::GetSupergroup &&
             function != TdFunctionKind::GetSupergroupFullInfo &&
             function != TdFunctionKind::CreatePrivateChat)) {
            return failed_future(TdAuthorizationFailure::FunctionDenied);
        }
        auto owner = issue_owner(TdOwnerKind::Request);
        if (!owner) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        return send(TdSendDescriptor{.function = function,
                                     .tier = DescriptorKind::Read,
                                     .owner = owner.owner(),
                                     .client_generation = authorization->client_generation,
                                     .auth_sequence = authorization->auth_sequence,
                                     .auth_state = authorization->data.state},
                    std::move(request));
    }

    std::future<TdValue> get_me(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        if (!authorization) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        return send_read(authorization, TdFunctionKind::GetMe,
                         runtime_->make_auth_function(TdAuthRequest{TdFunctionKind::GetMe}));
    }

    std::future<TdValue>
    get_saved_messages_tags(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            std::int64_t saved_messages_topic_id) {
        return send_read(authorization, TdFunctionKind::GetSavedMessagesTags,
                         runtime_->make_get_saved_messages_tags(saved_messages_topic_id));
    }

    std::future<TdValue>
    search_saved_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          TdSearchSavedMessagesRequest request) {
        return send_read(authorization, TdFunctionKind::SearchSavedMessages,
                         runtime_->make_search_saved_messages(std::move(request)));
    }

    std::future<TdValue>
    get_active_sessions(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        return send_read(authorization, TdFunctionKind::GetActiveSessions,
                         runtime_->make_get_active_sessions());
    }

    std::future<TdValue> get_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t chat_id) {
        return send_read(authorization, TdFunctionKind::GetChat, runtime_->make_get_chat(chat_id));
    }

    std::future<TdValue> get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdChatListKind list, std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::GetChats,
                         runtime_->make_get_chats(list, limit));
    }

    std::future<TdValue> load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    TdChatListKind list, std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::LoadChats,
                         runtime_->make_load_chats(list, limit));
    }

    std::future<TdValue>
    search_public_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       std::string username) {
        return send_read(authorization, TdFunctionKind::SearchPublicChat,
                         runtime_->make_search_public_chat(std::move(username)));
    }

    std::future<TdValue>
    get_internal_link_type(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::string link) {
        return send_read(authorization, TdFunctionKind::GetInternalLinkType,
                         runtime_->make_get_internal_link_type(std::move(link)));
    }

    std::future<TdValue>
    get_message_link_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          std::string url) {
        return send_read(authorization, TdFunctionKind::GetMessageLinkInfo,
                         runtime_->make_get_message_link_info(std::move(url)));
    }

    std::future<TdValue>
    check_chat_invite_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::string link) {
        return send_read(authorization, TdFunctionKind::CheckChatInviteLink,
                         runtime_->make_check_chat_invite_link(std::move(link)));
    }

    std::future<TdValue> get_user(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t user_id) {
        return send_read(authorization, TdFunctionKind::GetUser, runtime_->make_get_user(user_id));
    }

    std::future<TdValue>
    get_supergroup(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t supergroup_id) {
        return send_read(authorization, TdFunctionKind::GetSupergroup,
                         runtime_->make_get_supergroup(supergroup_id));
    }

    std::future<TdValue>
    get_supergroup_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::int64_t supergroup_id) {
        return send_read(authorization, TdFunctionKind::GetSupergroupFullInfo,
                         runtime_->make_get_supergroup_full_info(supergroup_id));
    }

    std::future<TdValue>
    create_private_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                        std::int64_t user_id, bool force) {
        return send_read(authorization, TdFunctionKind::CreatePrivateChat,
                         runtime_->make_create_private_chat(user_id, force));
    }

    std::future<TdValue> send_login(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    const TdRequestOwner& owner, TdAuthRequest request) {
        if (!authorization) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        const auto function = request.function;
        return send(TdSendDescriptor{.function = function,
                                     .tier = DescriptorKind::AuthBootstrap,
                                     .owner = owner,
                                     .client_generation = authorization->client_generation,
                                     .auth_sequence = authorization->auth_sequence,
                                     .auth_state = authorization->data.state},
                    runtime_->make_auth_function(std::move(request)));
    }

    std::future<TdValue> send_logout(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     TdClosedDecision& decision) {
        if (!authorization || !decision.state_) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        auto owner = issue_owner(TdOwnerKind::Request);
        if (!owner) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard lock(state_mutex_);
            generation = current_;
        }
        if (!generation || generation->number != authorization->client_generation) {
            return failed_future(TdAuthorizationFailure::GenerationMismatch);
        }
        const TdSendDescriptor descriptor{.function = TdFunctionKind::LogOut,
                                          .tier = DescriptorKind::Destructive,
                                          .owner = owner.owner(),
                                          .client_generation = authorization->client_generation,
                                          .auth_sequence = authorization->auth_sequence,
                                          .auth_state = authorization->data.state};
        auto function = runtime_->make_function(TdBuiltinFunction::LogOut);
        auto decision_state = decision.state_;
        return generation->lifecycle.send([this, generation, descriptor,
                                           function = std::move(function),
                                           decision_state = std::move(decision_state)]() mutable {
            const std::lock_guard lock(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                return failed_future(TdAuthorizationFailure::AuthStateMismatch);
            }
            const auto& function_data = function.function_data();
            if (const auto failure = authorization_failure_locked(
                    generation, descriptor, function_data ? &*function_data : nullptr)) {
                return failed_future(*failure);
            }
            auto [query_id, future] = generation->queries.reserve();
            {
                const std::lock_guard decision_lock(decision_state->mutex);
                if (decision_state->status != TdClosedDecisionStatus::Pending ||
                    decision_state->query_id != 0) {
                    static_cast<void>(generation->queries.fail(
                        query_id, std::make_exception_ptr(TdAuthorizationError(
                                      TdAuthorizationFailure::GenerationClosed))));
                    return std::move(future);
                }
                decision_state->query_id = query_id;
            }
            try {
                runtime_->send(generation->client_id, generation->number, query_id,
                               std::move(function));
            } catch (const std::exception&) {
                {
                    const std::lock_guard decision_lock(decision_state->mutex);
                    if (decision_state->status == TdClosedDecisionStatus::Pending) {
                        decision_state->status = TdClosedDecisionStatus::Rejected;
                    }
                }
                static_cast<void>(generation->queries.fail(query_id, std::current_exception()));
            }
            return std::move(future);
        });
    }

    TdClosedDecision begin_logout_decision(
        const std::shared_ptr<const AuthStateSnapshot>& authorization,
        std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim) {
        if (!authorization || authorization->data.state != AuthState::Ready || !claim) {
            return {};
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard lock(state_mutex_);
            generation = current_;
        }
        if (!generation || generation->number != authorization->client_generation ||
            generation->client_id != authorization->client_id) {
            return {};
        }
        auto state = std::make_shared<TdClosedDecision::State>();
        state->claim = std::move(claim);
        state->release = [generation] {
            {
                const std::lock_guard lock(generation->closed_decision_mutex);
                if (generation->pending_closed_decisions > 0) {
                    --generation->pending_closed_decisions;
                }
            }
            generation->closed_decision_cv.notify_all();
        };
        {
            const std::lock_guard commit_lock(generation->auth_commit_mutex);
            const auto current = auth_state_.load(std::memory_order_acquire);
            if (!current || current->client_generation != authorization->client_generation ||
                current->auth_sequence != authorization->auth_sequence ||
                current->data.state != AuthState::Ready) {
                return {};
            }
            const std::lock_guard decision_lock(generation->closed_decision_mutex);
            if (generation->closed_committed) {
                return {};
            }
            std::erase_if(generation->closed_decisions,
                          [](const auto& entry) { return entry.expired(); });
            ++generation->pending_closed_decisions;
            generation->closed_decisions.emplace_back(state);
        }
        return TdClosedDecision(std::move(state));
    }

    bool restart_generation(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (!authorization || !generation ||
            generation->number != authorization->client_generation) {
            return false;
        }
        return generation->lifecycle.begin_close([this, generation] {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->close_requested = true;
            if (generation->initial_state_installed && !generation->final) {
                send_close_locked(generation);
            }
        });
    }

    TdSendLease acquire_send_lease(TdSendDescriptor descriptor) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr) {
            return {};
        }

        TdSendLease lease;
        const bool admitted = generation->lifecycle.admit([&] {
            auto held = std::make_shared<LeaseLocks>();
            held->auth_commit = std::unique_lock<std::mutex>(generation->auth_commit_mutex);
            held->outbound = std::unique_lock<std::mutex>(generation->outbound_mutex);
            if (!generation->initial_state_installed ||
                authorization_failure_locked(generation, descriptor,
                                             TdFunctionData{descriptor.function})) {
                return;
            }
            auto state = std::make_shared<TdSendLease::State>();
            state->submit = [this, generation, descriptor,
                             held = std::move(held)](TdlibParameters parameters) mutable {
                auto submission = submit_admitted_locked(
                    generation, descriptor,
                    runtime_->make_set_tdlib_parameters(std::move(parameters)));
                held.reset();
                return std::move(submission.future);
            };
            lease = TdSendLease(std::move(state));
        });
        return admitted ? std::move(lease) : TdSendLease{};
    }

    static std::future<TdValue> send(TdSendLease lease, TdlibParameters parameters) {
        if (!lease.state_ || !lease.state_->submit) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        auto state = std::move(lease.state_);
        return state->submit(std::move(parameters));
    }

    TdRequestOwner internal_auth_owner() const {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_ == nullptr) {
            return {};
        }
        const std::lock_guard<std::mutex> outbound_lock(current_->outbound_mutex);
        return {TdOwnerKind::InternalAuth, current_->internal_auth_owner_id,
                current_->internal_auth_owner_capability};
    }

    TdOwnerLease issue_owner(TdOwnerKind kind) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr || (kind != TdOwnerKind::Login && kind != TdOwnerKind::Request)) {
            return {};
        }
        const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
        if (generation->final) {
            return {};
        }
        const auto id = next_owner_id_.fetch_add(1, std::memory_order_relaxed);
        auto capability = std::make_shared<const std::uint64_t>(id);
        (kind == TdOwnerKind::Login ? generation->login_owner_capabilities
                                    : generation->request_owner_capabilities)
            .insert(capability);
        auto state = std::make_unique<TdOwnerLease::State>();
        state->owner = {kind, id, capability};
        state->revoke = [generation, kind, capability = std::move(capability)] {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            (kind == TdOwnerKind::Login ? generation->login_owner_capabilities
                                        : generation->request_owner_capabilities)
                .erase(capability);
        };
        return TdOwnerLease(std::move(state));
    }

    bool owns(const TdRequestOwner& owner, std::uint64_t client_generation) const {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr || generation->number != client_generation) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
        switch (owner.kind) {
        case TdOwnerKind::InternalAuth:
            return owner.capability == generation->internal_auth_owner_capability;
        case TdOwnerKind::Lifecycle:
            return owner.capability == generation->lifecycle_owner_capability;
        case TdOwnerKind::Login:
            return generation->login_owner_capabilities.contains(owner.capability);
        case TdOwnerKind::Request:
            return generation->request_owner_capabilities.contains(owner.capability);
        }
        return false;
    }

    std::uint64_t subscribe_updates(UpdateHandler handler) {
        return updates_.subscribe(std::move(handler));
    }

    void unsubscribe_updates(std::uint64_t id) {
        updates_.unsubscribe(id);
    }

    std::shared_ptr<const AuthStateSnapshot> auth_state() const {
        return auth_state_.load(std::memory_order_acquire);
    }

    std::uint64_t subscribe_auth_states(AuthStateHandler handler) {
        return auth_states_.subscribe(std::move(handler));
    }

    void unsubscribe_auth_states(std::uint64_t id) {
        auth_states_.unsubscribe(id);
    }

    bool close_until(std::chrono::steady_clock::time_point deadline) {
        std::call_once(close_begin_once_, [this] {
            std::shared_ptr<Generation> generation;
            {
                const std::lock_guard<std::mutex> lock(state_mutex_);
                shutting_down_ = true;
                generation = current_;
                shutdown_generation_ = generation == nullptr ? 0 : generation->number;
            }

            if (generation != nullptr) {
                static_cast<void>(generation->lifecycle.begin_close([this, generation] {
                    const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
                    generation->close_requested = true;
                    if (generation->initial_state_installed && !generation->final) {
                        send_close_locked(generation);
                    }
                }));
            }
        });

        {
            std::unique_lock<std::mutex> lock(closed_mutex_);
            if (shutdown_generation_ != 0 &&
                !closed_cv_.wait_until(lock, deadline, [this] { return shutdown_closed_; })) {
                return false;
            }
        }
        finalize_shutdown();
        return true;
    }

    void close() {
        if (!close_until(std::chrono::steady_clock::now() + kCloseTimeout)) {
            std::fputs("warning: tdlib did not reach authorizationStateClosed within 30s; "
                       "shutting down without the clean-close guarantee\n",
                       stderr);
            finalize_shutdown();
        }
    }

  private:
    void finalize_shutdown() {
        std::call_once(finalize_once_, [this] {
            stop_.store(true, std::memory_order_release);
            if (receive_thread_.joinable()) {
                receive_thread_.join();
            }
            std::shared_ptr<Generation> generation;
            {
                const std::lock_guard<std::mutex> lock(state_mutex_);
                generation = current_;
            }
            if (generation != nullptr) {
                generation->queries.fail_all("tdlib client closed");
            }
        });
    }

    struct Submission {
        std::uint64_t query_id = 0;
        std::future<TdValue> future;
    };

    struct LeaseLocks {
        std::unique_lock<std::mutex> auth_commit;
        std::unique_lock<std::mutex> outbound;
    };

    struct Generation {
        Generation(std::int32_t client_id_value, std::uint64_t number_value)
            : client_id(client_id_value), number(number_value) {}

        std::int32_t client_id;
        std::uint64_t number;
        QueryRegistry<TdValue> queries;
        detail::RequestLifecycle<TdValue> lifecycle{"tdlib client generation closed"};
        std::mutex auth_commit_mutex;
        std::mutex outbound_mutex;
        std::mutex closed_decision_mutex;
        std::condition_variable closed_decision_cv;
        std::size_t pending_closed_decisions = 0;
        bool closed_committed = false;
        std::vector<std::weak_ptr<TdClosedDecision::State>> closed_decisions;
        std::uint64_t internal_auth_owner_id = 0;
        std::uint64_t lifecycle_owner_id = 0;
        std::shared_ptr<const void> internal_auth_owner_capability;
        std::shared_ptr<const void> lifecycle_owner_capability;
        std::unordered_set<std::shared_ptr<const void>> login_owner_capabilities;
        std::unordered_set<std::shared_ptr<const void>> request_owner_capabilities;
        bool initial_state_installed = false;
        bool accepted_auth_update = false;
        bool close_requested = false;
        bool close_sent = false;
        bool final = false;
        std::deque<TdValue> pending_updates;
    };

    std::shared_ptr<Generation> make_generation() {
        const auto generation_number = next_generation_++;
        const auto client_id = runtime_->create_client(generation_number);
        auto generation = std::make_shared<Generation>(client_id, generation_number);
        generation->internal_auth_owner_id = next_owner_id_.fetch_add(1, std::memory_order_relaxed);
        generation->lifecycle_owner_id = next_owner_id_.fetch_add(1, std::memory_order_relaxed);
        generation->internal_auth_owner_capability =
            std::make_shared<const std::uint64_t>(generation->internal_auth_owner_id);
        generation->lifecycle_owner_capability =
            std::make_shared<const std::uint64_t>(generation->lifecycle_owner_id);
        auto unknown = std::make_shared<const AuthStateSnapshot>(
            AuthStateSnapshot{.client_id = client_id,
                              .client_generation = generation_number,
                              .auth_sequence = 0,
                              .data = AuthStateData{AuthState::Unknown},
                              .receive_observed_at = std::nullopt});
        auth_state_.store(std::move(unknown), std::memory_order_release);

        const TdSendDescriptor descriptor{
            .function = TdFunctionKind::GetAuthorizationState,
            .tier = DescriptorKind::AuthBootstrap,
            .owner = {TdOwnerKind::InternalAuth, generation->internal_auth_owner_id,
                      generation->internal_auth_owner_capability},
            .client_generation = generation_number,
            .auth_sequence = 0,
            .auth_state = AuthState::Unknown,
        };
        auto submission =
            submit_locked(generation, descriptor,
                          runtime_->make_function(TdBuiltinFunction::GetAuthorizationState));
        if (submission.query_id != 1) {
            throw std::logic_error("authorization bootstrap must reserve query id 1");
        }
        return generation;
    }

    void activate_initial_generation() {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        current_ = make_generation();
    }

    Submission submit_locked(const std::shared_ptr<Generation>& generation,
                             const TdSendDescriptor& descriptor, TdValue function) {
        const auto& function_data = function.function_data();
        if (const auto failure = authorization_failure_locked(
                generation, descriptor, function_data ? &*function_data : nullptr)) {
            return {0, failed_future(*failure)};
        }
        return submit_admitted_locked(generation, descriptor, std::move(function));
    }

    std::optional<TdAuthorizationFailure>
    authorization_failure_locked(const std::shared_ptr<Generation>& generation,
                                 const TdSendDescriptor& descriptor,
                                 const TdFunctionData& function) const {
        return authorization_failure_locked(generation, descriptor, &function);
    }

    std::optional<TdAuthorizationFailure>
    authorization_failure_locked(const std::shared_ptr<Generation>& generation,
                                 const TdSendDescriptor& descriptor,
                                 const TdFunctionData* function) const {
        const auto snapshot = auth_state_.load(std::memory_order_acquire);
        if (snapshot == nullptr) {
            return TdAuthorizationFailure::AuthStateMismatch;
        }
        if (const auto failure =
                authorize_td_send(descriptor, function, *snapshot, generation->final)) {
            return failure;
        }
        const bool owner_registered = [&] {
            switch (descriptor.owner.kind) {
            case TdOwnerKind::InternalAuth:
                return descriptor.owner.capability == generation->internal_auth_owner_capability;
            case TdOwnerKind::Lifecycle:
                return descriptor.owner.capability == generation->lifecycle_owner_capability;
            case TdOwnerKind::Login:
                return generation->login_owner_capabilities.contains(descriptor.owner.capability);
            case TdOwnerKind::Request:
                return generation->request_owner_capabilities.contains(descriptor.owner.capability);
            }
            return false;
        }();
        if (!owner_registered) {
            return TdAuthorizationFailure::OwnerMismatch;
        }
        return std::nullopt;
    }

    Submission submit_admitted_locked(const std::shared_ptr<Generation>& generation,
                                      const TdSendDescriptor& admitted_descriptor,
                                      TdValue function) {
        static_cast<void>(admitted_descriptor);
        auto [query_id, future] = generation->queries.reserve();
        try {
            runtime_->send(generation->client_id, generation->number, query_id,
                           std::move(function));
        } catch (const std::exception&) {
            static_cast<void>(generation->queries.fail(query_id, std::current_exception()));
        }
        return {query_id, std::move(future)};
    }

    void send_close_locked(const std::shared_ptr<Generation>& generation) {
        if (generation->close_sent) {
            return;
        }
        generation->close_sent = true;
        const auto snapshot = auth_state_.load(std::memory_order_acquire);
        if (snapshot == nullptr) {
            return;
        }
        const TdSendDescriptor descriptor{
            .function = TdFunctionKind::Close,
            .tier = DescriptorKind::Lifecycle,
            .owner = {TdOwnerKind::Lifecycle, generation->lifecycle_owner_id,
                      generation->lifecycle_owner_capability},
            .client_generation = generation->number,
            .auth_sequence = snapshot->auth_sequence,
            .auth_state = snapshot->data.state,
        };
        static_cast<void>(submit_locked(generation, descriptor,
                                        runtime_->make_function(TdBuiltinFunction::Close)));
    }

    static std::vector<std::shared_ptr<TdClosedDecision::State>>
    active_closed_decisions(const std::shared_ptr<Generation>& generation) {
        std::vector<std::shared_ptr<TdClosedDecision::State>> result;
        const std::lock_guard lock(generation->closed_decision_mutex);
        std::erase_if(generation->closed_decisions,
                      [](const auto& entry) { return entry.expired(); });
        result.reserve(generation->closed_decisions.size());
        for (const auto& entry : generation->closed_decisions) {
            if (auto state = entry.lock()) {
                result.push_back(std::move(state));
            }
        }
        return result;
    }

    static void claim_lifecycle_event(const std::shared_ptr<TdClosedDecision::State>& state,
                                      TdClosedDecisionStatus accepted_status,
                                      std::uint64_t query_id = 0) {
        std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim;
        std::chrono::steady_clock::time_point committed_at;
        {
            std::unique_lock lock(state->mutex);
            state->callbacks_done.wait(lock, [&] { return state->callbacks_in_flight == 0; });
            if (state->released || state->status != TdClosedDecisionStatus::Pending ||
                (accepted_status == TdClosedDecisionStatus::Error &&
                 (state->query_id != query_id || state->expected_transition))) {
                return;
            }
            committed_at = std::chrono::steady_clock::now();
            ++state->callbacks_in_flight;
            claim = state->claim;
        }
        const auto claimed = claim ? claim(committed_at) : TdLifecycleClaimStatus::Rejected;
        {
            const std::lock_guard lock(state->mutex);
            if (!state->released && state->status == TdClosedDecisionStatus::Pending) {
                if (claimed == TdLifecycleClaimStatus::Active) {
                    if (accepted_status == TdClosedDecisionStatus::Pending) {
                        state->expected_transition = true;
                    } else if (accepted_status == TdClosedDecisionStatus::Closed &&
                               state->query_id == 0) {
                        state->status = TdClosedDecisionStatus::Rejected;
                    } else {
                        state->status = accepted_status;
                    }
                } else {
                    state->status = terminal_decision(claimed);
                }
            }
            --state->callbacks_in_flight;
        }
        state->callbacks_done.notify_all();
    }

    static void note_expected_transition(const std::shared_ptr<Generation>& generation) {
        for (const auto& state : active_closed_decisions(generation)) {
            claim_lifecycle_event(state, TdClosedDecisionStatus::Pending);
        }
    }

    static void resolve_lifecycle_event(const std::shared_ptr<Generation>& generation,
                                        TdClosedDecisionStatus accepted_status,
                                        std::uint64_t query_id = 0) {
        if (accepted_status == TdClosedDecisionStatus::Closed) {
            const std::lock_guard lock(generation->closed_decision_mutex);
            generation->closed_committed = true;
        }
        for (const auto& state : active_closed_decisions(generation)) {
            claim_lifecycle_event(state, accepted_status, query_id);
        }
    }

    void receive_loop() {
        while (!stop_.load(std::memory_order_acquire)) {
            auto event = runtime_->receive(kReceiveTimeout);
            if (!event.has_value()) {
                continue;
            }
            handle_event(std::move(*event));
        }
    }

    void handle_event(TdRuntimeEvent event) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr || event.client_id != generation->client_id ||
            event.client_generation != generation->number) {
            return;
        }
        const bool closes_generation =
            event.authorization_state && event.authorization_state->state == AuthState::Closed;
        if (closes_generation) {
            close_generation_admission(generation);
        }
        const auto receive_event_sequence = next_receive_event_sequence_++;
        event.object.set_receive_event_metadata(receive_event_sequence, event.observed_at);

        if (event.authorization_state) {
            if (event.authorization_state->state == AuthState::LoggingOut ||
                event.authorization_state->state == AuthState::Closing) {
                note_expected_transition(generation);
            } else if (closes_generation) {
                resolve_lifecycle_event(generation, TdClosedDecisionStatus::Closed);
            }
        }
        if (event.query_id != 0 && event.object.get_if<TdError>() != nullptr) {
            resolve_lifecycle_event(generation, TdClosedDecisionStatus::Error, event.query_id);
        }

        if (event.query_id == 0) {
            handle_update(generation, std::move(event), receive_event_sequence);
            return;
        }

        if (event.query_id == 1 && event.authorization_state.has_value()) {
            bool install_response = false;
            {
                const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
                install_response =
                    !generation->accepted_auth_update && !generation->initial_state_installed;
            }
            if (install_response) {
                install_auth_state(generation, *event.authorization_state, false,
                                   receive_event_sequence, event.observed_at);
                if (event.authorization_state->state == AuthState::Closed) {
                    handle_closed(generation);
                }
            }
        }
        static_cast<void>(generation->queries.fulfill(event.query_id, std::move(event.object)));
    }

    void handle_update(const std::shared_ptr<Generation>& generation, TdRuntimeEvent event,
                       std::uint64_t receive_event_sequence) {
        if (event.authorization_state.has_value()) {
            const auto closed = event.authorization_state->state == AuthState::Closed;
            install_auth_state(generation, *event.authorization_state, true, receive_event_sequence,
                               event.observed_at);
            updates_.publish(event.object);
            if (closed) {
                handle_closed(generation);
            }
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                generation->pending_updates.push_back(std::move(event.object));
                return;
            }
        }
        updates_.publish(event.object);
    }

    void install_auth_state(const std::shared_ptr<Generation>& generation,
                            const AuthStateData& state, bool from_update,
                            std::uint64_t receive_event_sequence,
                            TdEventClock::time_point observed_at) {
        if (state.state == AuthState::Closed) {
            close_generation_admission(generation);
        }

        std::shared_ptr<const AuthStateSnapshot> snapshot;
        std::deque<TdValue> pending_updates;
        {
            const std::lock_guard<std::mutex> commit_lock(generation->auth_commit_mutex);
            const std::lock_guard<std::mutex> outbound_lock(generation->outbound_mutex);
            const auto previous = auth_state_.load(std::memory_order_acquire);
            const auto sequence =
                previous != nullptr && previous->client_generation == generation->number
                    ? previous->auth_sequence + 1
                    : 1;
            snapshot = std::make_shared<const AuthStateSnapshot>(
                AuthStateSnapshot{.client_id = generation->client_id,
                                  .client_generation = generation->number,
                                  .auth_sequence = sequence,
                                  .receive_event_sequence = receive_event_sequence,
                                  .data = state,
                                  .receive_observed_at = observed_at});
            auth_state_.store(snapshot, std::memory_order_release);
            generation->initial_state_installed = true;
            generation->accepted_auth_update |= from_update;
            if (state.state != AuthState::Closed) {
                if (generation->close_requested) {
                    send_close_locked(generation);
                }
                pending_updates.swap(generation->pending_updates);
            }
        }

        for (const auto& update : pending_updates) {
            updates_.publish(update);
        }
        auth_states_.publish(snapshot);
    }

    static void close_generation_admission(const std::shared_ptr<Generation>& generation) {
        const auto closed_now = generation->lifecycle.begin_close([generation] {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->final = true;
            generation->pending_updates.clear();
        });
        if (!closed_now) {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->final = true;
            generation->pending_updates.clear();
        }
    }

    void handle_closed(const std::shared_ptr<Generation>& generation) {
        {
            std::unique_lock lock(generation->closed_decision_mutex);
            generation->closed_decision_cv.wait(
                lock, [&] { return generation->pending_closed_decisions == 0; });
        }
        generation->queries.fail_all("tdlib client generation closed");

        bool notify_shutdown = false;
        std::shared_ptr<const AuthStateSnapshot> unknown;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            if (current_ != generation) {
                return;
            }
            if (shutting_down_ && shutdown_generation_ == generation->number) {
                notify_shutdown = true;
            } else if (!shutting_down_) {
                current_ = make_generation();
                unknown = auth_state_.load(std::memory_order_acquire);
            }
        }
        if (notify_shutdown) {
            {
                const std::lock_guard<std::mutex> lock(closed_mutex_);
                shutdown_closed_ = true;
            }
            closed_cv_.notify_all();
            return;
        }
        if (unknown != nullptr) {
            auth_states_.publish(unknown);
        }
    }

    std::unique_ptr<TdRuntime> runtime_;
    mutable std::mutex state_mutex_;
    std::shared_ptr<Generation> current_;
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_receive_event_sequence_ = 1;
    std::atomic<std::uint64_t> next_owner_id_{1};
    bool shutting_down_ = false;
    std::uint64_t shutdown_generation_ = 0;
    std::atomic<std::shared_ptr<const AuthStateSnapshot>> auth_state_;
    UpdateBus<TdValue> updates_;
    UpdateBus<std::shared_ptr<const AuthStateSnapshot>> auth_states_;
    std::atomic<bool> stop_{false};
    std::mutex closed_mutex_;
    std::condition_variable closed_cv_;
    bool shutdown_closed_ = false;
    std::once_flag close_begin_once_;
    std::once_flag finalize_once_;
    std::thread receive_thread_;
};

TdAuthorizationError::TdAuthorizationError(TdAuthorizationFailure failure)
    : std::runtime_error("TDLib send denied: " + std::string(authorization_failure_name(failure))),
      failure_(failure) {}

TdSendLease::TdSendLease() = default;
TdSendLease::~TdSendLease() = default;
TdSendLease::TdSendLease(std::shared_ptr<State> state) : state_(std::move(state)) {}
TdSendLease::TdSendLease(TdSendLease&&) noexcept = default;
TdSendLease& TdSendLease::operator=(TdSendLease&& other) noexcept = default;

TdSendLease::operator bool() const noexcept {
    return state_ != nullptr;
}

TdClosedDecision::TdClosedDecision() = default;
TdClosedDecision::~TdClosedDecision() {
    if (state_) {
        std::function<void()> release;
        {
            std::unique_lock lock(state_->mutex);
            state_->released = true;
            state_->claim = {};
            state_->callbacks_done.wait(lock, [this] { return state_->callbacks_in_flight == 0; });
            release = std::move(state_->release);
        }
        if (release) {
            release();
        }
    }
}
TdClosedDecision::TdClosedDecision(std::shared_ptr<State> state) : state_(std::move(state)) {}
TdClosedDecision::TdClosedDecision(TdClosedDecision&&) noexcept = default;
TdClosedDecision& TdClosedDecision::operator=(TdClosedDecision&& other) noexcept {
    if (this != &other) {
        if (state_) {
            std::function<void()> release;
            {
                std::unique_lock lock(state_->mutex);
                state_->released = true;
                state_->claim = {};
                state_->callbacks_done.wait(lock,
                                            [this] { return state_->callbacks_in_flight == 0; });
                release = std::move(state_->release);
            }
            if (release) {
                release();
            }
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

TdClosedDecision::operator bool() const noexcept {
    return state_ != nullptr;
}

TdClosedDecisionStatus TdClosedDecision::status() const {
    if (!state_) {
        return TdClosedDecisionStatus::Rejected;
    }
    const std::lock_guard lock(state_->mutex);
    return state_->status;
}

TdClosedDecisionStatus TdClosedDecision::settle_terminal() {
    if (!state_) {
        return TdClosedDecisionStatus::Rejected;
    }
    std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim;
    {
        std::unique_lock lock(state_->mutex);
        state_->callbacks_done.wait(lock, [this] { return state_->callbacks_in_flight == 0; });
        if (state_->released || state_->status != TdClosedDecisionStatus::Pending) {
            return state_->status;
        }
        ++state_->callbacks_in_flight;
        claim = state_->claim;
    }
    const auto claimed =
        claim ? claim(std::chrono::steady_clock::now()) : TdLifecycleClaimStatus::Rejected;
    {
        const std::lock_guard lock(state_->mutex);
        if (!state_->released && state_->status == TdClosedDecisionStatus::Pending) {
            state_->status = terminal_decision(claimed);
        }
        --state_->callbacks_in_flight;
    }
    state_->callbacks_done.notify_all();
    return status();
}

TdOwnerLease::TdOwnerLease() = default;
TdOwnerLease::~TdOwnerLease() {
    if (state_ && state_->revoke) {
        state_->revoke();
    }
}
TdOwnerLease::TdOwnerLease(std::unique_ptr<State> state) : state_(std::move(state)) {}
TdOwnerLease::TdOwnerLease(TdOwnerLease&&) noexcept = default;
TdOwnerLease& TdOwnerLease::operator=(TdOwnerLease&& other) noexcept {
    if (this != &other) {
        if (state_ && state_->revoke) {
            state_->revoke();
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

TdRequestOwner TdOwnerLease::owner() const noexcept {
    return state_ ? state_->owner : TdRequestOwner{};
}

TdOwnerLease::operator bool() const noexcept {
    return state_ != nullptr;
}

TdClient::TdClient(const TdLogConfiguration& logging)
    : TdClient(make_production_td_runtime(), logging) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime)
    : TdClient(std::move(runtime), TdLogConfiguration{}) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging)
    : impl_(std::make_unique<Impl>(std::move(runtime), logging)) {}

TdClient::~TdClient() = default;

std::future<TdValue> TdClient::send(TdSendDescriptor descriptor, TdValue request) {
    return impl_->send(std::move(descriptor), std::move(request));
}

std::future<TdValue> TdClient::send(TdSendDescriptor descriptor, TdlibParameters parameters) {
    return impl_->send(std::move(descriptor), std::move(parameters));
}

TdSendLease TdClient::acquire_send_lease(TdSendDescriptor descriptor) {
    return impl_->acquire_send_lease(std::move(descriptor));
}

std::future<TdValue> TdClient::send(TdSendLease&& lease, TdlibParameters parameters) {
    return impl_->send(std::move(lease), std::move(parameters));
}

std::future<TdValue>
TdClient::send_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                    TdFunctionKind function, TdValue request) {
    return impl_->send_read(authorization, function, std::move(request));
}

std::future<TdValue>
TdClient::get_me(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->get_me(authorization);
}

std::future<TdValue>
TdClient::get_saved_messages_tags(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t saved_messages_topic_id) {
    return impl_->get_saved_messages_tags(authorization, saved_messages_topic_id);
}

std::future<TdValue>
TdClient::search_saved_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                TdSearchSavedMessagesRequest request) {
    return impl_->search_saved_messages(authorization, std::move(request));
}

std::future<TdValue>
TdClient::get_active_sessions(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->get_active_sessions(authorization);
}

std::future<TdValue>
TdClient::get_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t chat_id) {
    return impl_->get_chat(authorization, chat_id);
}

std::future<TdValue>
TdClient::get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                    TdChatListKind list, std::int32_t limit) {
    return impl_->get_chats(authorization, list, limit);
}

std::future<TdValue>
TdClient::load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     TdChatListKind list, std::int32_t limit) {
    return impl_->load_chats(authorization, list, limit);
}

std::future<TdValue>
TdClient::search_public_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::string username) {
    return impl_->search_public_chat(authorization, std::move(username));
}

std::future<TdValue>
TdClient::get_internal_link_type(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 std::string link) {
    return impl_->get_internal_link_type(authorization, std::move(link));
}

std::future<TdValue>
TdClient::get_message_link_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                std::string url) {
    return impl_->get_message_link_info(authorization, std::move(url));
}

std::future<TdValue>
TdClient::check_chat_invite_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 std::string link) {
    return impl_->check_chat_invite_link(authorization, std::move(link));
}

std::future<TdValue>
TdClient::get_user(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t user_id) {
    return impl_->get_user(authorization, user_id);
}

std::future<TdValue>
TdClient::get_supergroup(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                         std::int64_t supergroup_id) {
    return impl_->get_supergroup(authorization, supergroup_id);
}

std::future<TdValue>
TdClient::get_supergroup_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   std::int64_t supergroup_id) {
    return impl_->get_supergroup_full_info(authorization, supergroup_id);
}

std::future<TdValue>
TdClient::create_private_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                              std::int64_t user_id, bool force) {
    return impl_->create_private_chat(authorization, user_id, force);
}

std::future<TdValue>
TdClient::send_login(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     const TdRequestOwner& owner, TdAuthRequest request) {
    return impl_->send_login(authorization, owner, std::move(request));
}

std::future<TdValue>
TdClient::send_logout(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                      TdClosedDecision& decision) {
    return impl_->send_logout(authorization, decision);
}

TdClosedDecision TdClient::begin_logout_decision(
    const std::shared_ptr<const AuthStateSnapshot>& authorization,
    std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim) {
    return impl_->begin_logout_decision(authorization, std::move(claim));
}

bool TdClient::restart_generation(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->restart_generation(authorization);
}

TdRequestOwner TdClient::internal_auth_owner() const {
    return impl_->internal_auth_owner();
}

TdOwnerLease TdClient::issue_login_owner() {
    return impl_->issue_owner(TdOwnerKind::Login);
}

bool TdClient::owns(const TdRequestOwner& owner, std::uint64_t client_generation) const {
    return impl_->owns(owner, client_generation);
}

std::uint64_t TdClient::subscribe_updates(UpdateHandler handler) {
    return impl_->subscribe_updates(std::move(handler));
}

void TdClient::unsubscribe_updates(std::uint64_t id) {
    impl_->unsubscribe_updates(id);
}

std::shared_ptr<const AuthStateSnapshot> TdClient::auth_state() const {
    return impl_->auth_state();
}

std::uint64_t TdClient::subscribe_auth_states(AuthStateHandler handler) {
    return impl_->subscribe_auth_states(std::move(handler));
}

void TdClient::unsubscribe_auth_states(std::uint64_t id) {
    impl_->unsubscribe_auth_states(id);
}

void TdClient::close() {
    impl_->close();
}

bool TdClient::close_until(std::chrono::steady_clock::time_point deadline) {
    return impl_->close_until(deadline);
}

std::string TdClient::tdlib_version() {
    return production_tdlib_version();
}

} // namespace tgcli::core
