#include "core/td_client.hpp"

#include "core/query_registry.hpp"
#include "core/request_lifecycle.hpp"
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

} // namespace

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

    std::future<TdValue> send(TdValue request) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr) {
            return failed_future("tdlib client closed");
        }

        return generation->lifecycle.send(
            [this, generation, request = std::move(request)]() mutable {
                auto [query_id, future] = generation->queries.reserve();
                const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
                if (!generation->initial_state_installed) {
                    generation->pending.emplace_back(query_id, std::move(request));
                } else {
                    send_or_fail(generation, query_id, std::move(request));
                }
                return std::move(future);
            });
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
                    fail_pending_locked(generation, "tdlib client closing");
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
    struct PendingFunction {
        PendingFunction(std::uint64_t query_id_value, TdValue function_value)
            : query_id(query_id_value), function(std::move(function_value)) {}

        std::uint64_t query_id;
        TdValue function;
    };

    struct Generation {
        Generation(std::int32_t client_id_value, std::uint64_t number_value)
            : client_id(client_id_value), number(number_value) {}

        std::int32_t client_id;
        std::uint64_t number;
        QueryRegistry<TdValue> queries;
        detail::RequestLifecycle<TdValue> lifecycle{"tdlib client generation closed"};
        std::mutex outbound_mutex;
        bool initial_state_installed = false;
        bool accepted_auth_update = false;
        bool close_requested = false;
        bool close_sent = false;
        bool final = false;
        std::deque<PendingFunction> pending;
        std::deque<TdValue> pending_updates;
    };

    std::shared_ptr<Generation> make_generation() {
        const auto generation_number = next_generation_++;
        const auto client_id = runtime_->create_client(generation_number);
        auto generation = std::make_shared<Generation>(client_id, generation_number);
        auto unknown = std::make_shared<const AuthStateSnapshot>(
            AuthStateSnapshot{.client_id = client_id,
                              .client_generation = generation_number,
                              .auth_sequence = 0,
                              .data = AuthStateData{AuthState::Unknown}});
        auth_state_.store(std::move(unknown), std::memory_order_release);

        auto [query_id, future] = generation->queries.reserve();
        static_cast<void>(future);
        if (query_id != 1) {
            throw std::logic_error("authorization bootstrap must reserve query id 1");
        }
        runtime_->send(client_id, generation_number, query_id,
                       runtime_->make_function(TdBuiltinFunction::GetAuthorizationState));
        return generation;
    }

    void activate_initial_generation() {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        current_ = make_generation();
    }

    void send_or_fail(const std::shared_ptr<Generation>& generation, std::uint64_t query_id,
                      TdValue function) {
        try {
            runtime_->send(generation->client_id, generation->number, query_id,
                           std::move(function));
        } catch (const std::exception&) {
            static_cast<void>(generation->queries.fail(query_id, std::current_exception()));
        }
    }

    static void fail_pending_locked(const std::shared_ptr<Generation>& generation,
                                    const std::string& message) {
        while (!generation->pending.empty()) {
            const auto query_id = generation->pending.front().query_id;
            generation->pending.pop_front();
            static_cast<void>(generation->queries.fail(
                query_id, std::make_exception_ptr(std::runtime_error(message))));
        }
    }

    void send_close_locked(const std::shared_ptr<Generation>& generation) {
        if (generation->close_sent) {
            return;
        }
        generation->close_sent = true;
        auto [query_id, future] = generation->queries.reserve();
        static_cast<void>(future);
        send_or_fail(generation, query_id, runtime_->make_function(TdBuiltinFunction::Close));
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

        if (event.query_id == 0) {
            handle_update(generation, std::move(event));
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
                install_auth_state(generation, *event.authorization_state, false);
                if (event.authorization_state->state == AuthState::Closed) {
                    handle_closed(generation);
                }
            }
        }
        static_cast<void>(generation->queries.fulfill(event.query_id, std::move(event.object)));
    }

    void handle_update(const std::shared_ptr<Generation>& generation, TdRuntimeEvent event) {
        if (event.authorization_state.has_value()) {
            const auto closed = event.authorization_state->state == AuthState::Closed;
            install_auth_state(generation, *event.authorization_state, true);
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
                            const AuthStateData& state, bool from_update) {
        if (state.state == AuthState::Closed) {
            close_generation_admission(generation);
        }

        std::shared_ptr<const AuthStateSnapshot> snapshot;
        {
            const auto previous = auth_state_.load(std::memory_order_acquire);
            const auto sequence =
                previous != nullptr && previous->client_generation == generation->number
                    ? previous->auth_sequence + 1
                    : 1;
            snapshot = std::make_shared<const AuthStateSnapshot>(
                AuthStateSnapshot{.client_id = generation->client_id,
                                  .client_generation = generation->number,
                                  .auth_sequence = sequence,
                                  .data = state});
            auth_state_.store(snapshot, std::memory_order_release);
        }

        std::deque<TdValue> pending_updates;
        {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->initial_state_installed = true;
            generation->accepted_auth_update |= from_update;
            if (state.state != AuthState::Closed) {
                if (generation->close_requested) {
                    send_close_locked(generation);
                } else {
                    while (!generation->pending.empty() && !generation->final) {
                        auto pending = std::move(generation->pending.front());
                        generation->pending.pop_front();
                        send_or_fail(generation, pending.query_id, std::move(pending.function));
                    }
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
            generation->pending.clear();
            generation->pending_updates.clear();
        });
        if (!closed_now) {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->final = true;
            generation->pending.clear();
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

TdClient::TdClient() : TdClient(make_production_td_runtime()) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime)
    : impl_(std::make_unique<Impl>(std::move(runtime))) {}

TdClient::~TdClient() = default;

std::future<TdValue> TdClient::send(TdValue request) {
    return impl_->send(std::move(request));
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
