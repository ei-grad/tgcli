#include "core/td_client.hpp"

#include "core/query_registry.hpp"
#include "core/request_lifecycle.hpp"
#include "core/td_authorization.hpp"
#include "core/update_bus.hpp"

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

} // namespace

struct TdSendLease::State {
    std::function<std::future<TdValue>(TdlibParameters)> submit;
};

struct TdOwnerLease::State {
    TdRequestOwner owner;
    std::function<void()> revoke;
};

class TdClient::Impl {
  public:
    explicit Impl(std::unique_ptr<TdRuntime> runtime) : runtime_(std::move(runtime)) {
        if (runtime_ == nullptr) {
            throw std::invalid_argument("TdClient runtime must not be null");
        }
        runtime_->initialize_process();
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
            (function != TdFunctionKind::GetOption && function != TdFunctionKind::GetMe)) {
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

    void close() {
        std::call_once(close_once_, [this] {
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

            {
                std::unique_lock<std::mutex> lock(closed_mutex_);
                if (generation != nullptr && !closed_cv_.wait_for(lock, kCloseTimeout, [this] {
                        return shutdown_closed_;
                    })) {
                    std::fputs("warning: tdlib did not reach authorizationStateClosed within 30s; "
                               "shutting down without the clean-close guarantee\n",
                               stderr);
                }
            }

            stop_.store(true, std::memory_order_release);
            if (receive_thread_.joinable()) {
                receive_thread_.join();
            }
            if (generation != nullptr) {
                generation->queries.fail_all("tdlib client closed");
            }
        });
    }

  private:
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
                              .data = AuthStateData{AuthState::Unknown}});
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
        const auto receive_event_sequence = next_receive_event_sequence_++;
        event.object.set_receive_event_sequence(receive_event_sequence);

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
                                   receive_event_sequence);
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
            install_auth_state(generation, *event.authorization_state, true,
                               receive_event_sequence);
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
                            std::uint64_t receive_event_sequence) {
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
                                  .data = state});
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
    std::once_flag close_once_;
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

TdClient::TdClient() : TdClient(make_production_td_runtime()) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime)
    : impl_(std::make_unique<Impl>(std::move(runtime))) {}

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
TdClient::send_login(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     const TdRequestOwner& owner, TdAuthRequest request) {
    return impl_->send_login(authorization, owner, std::move(request));
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

std::string TdClient::tdlib_version() {
    return production_tdlib_version();
}

} // namespace tgcli::core
