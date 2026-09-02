#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace tgcli::cancellation {

namespace detail {

class CallbackNode final {
  public:
    explicit CallbackNode(std::function<void()> callback) : callback_(std::move(callback)) {}

    void invoke() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (!active_) {
                return;
            }
            executing_ = true;
            executor_ = std::this_thread::get_id();
        }
        try {
            callback_();
        } catch (...) {
            std::terminate();
        }
        {
            const std::lock_guard lock(mutex_);
            executing_ = false;
            executor_ = {};
        }
        condition_.notify_all();
    }

    void deactivate() noexcept {
        std::unique_lock lock(mutex_);
        active_ = false;
        if (executing_ && executor_ != std::this_thread::get_id()) {
            condition_.wait(lock, [this] { return !executing_; });
        }
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::function<void()> callback_;
    std::thread::id executor_;
    bool active_ = true;
    bool executing_ = false;
};

class State final {
  public:
    struct Registration {
        std::uint64_t id = 0;
        bool invoke_now = false;
    };

    [[nodiscard]] bool stop_requested() const noexcept {
        const std::lock_guard lock(mutex_);
        return stop_requested_;
    }

    [[nodiscard]] Registration register_callback(const std::shared_ptr<CallbackNode>& callback) {
        const std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return {.invoke_now = true};
        }
        const auto id = next_id_++;
        callbacks_.emplace(id, callback);
        return {.id = id};
    }

    void unregister_callback(std::uint64_t id) noexcept {
        const std::lock_guard lock(mutex_);
        callbacks_.erase(id);
    }

    bool request_stop() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (stop_requested_) {
                return false;
            }
            stop_requested_ = true;
            callbacks_.swap(dispatch_callbacks_);
        }
        for (const auto& [unused_id, callback] : dispatch_callbacks_) {
            if (auto retained = callback.lock()) {
                retained->invoke();
            }
        }
        dispatch_callbacks_.clear();
        return true;
    }

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::weak_ptr<CallbackNode>> callbacks_;
    std::unordered_map<std::uint64_t, std::weak_ptr<CallbackNode>> dispatch_callbacks_;
    std::uint64_t next_id_ = 1;
    bool stop_requested_ = false;
};

} // namespace detail

class Token final {
  public:
    Token() = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stop_requested();
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return static_cast<bool>(state_);
    }

  private:
    explicit Token(std::shared_ptr<detail::State> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::State> state_;

    friend class Source;
    friend class Callback;
};

class Source final {
  public:
    Source() : state_(std::make_shared<detail::State>()) {}

    [[nodiscard]] Token get_token() const noexcept {
        return Token(state_);
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stop_requested();
    }

    [[nodiscard]] bool request_stop() const noexcept {
        return state_ && state_->request_stop();
    }

  private:
    std::shared_ptr<detail::State> state_;
};

class Callback final {
  public:
    template <typename Function>
    Callback(const Token& token, Function&& function)
        : state_(token.state_), callback_(std::make_shared<detail::CallbackNode>(
                                    std::function<void()>(std::forward<Function>(function)))) {
        if (!state_) {
            callback_.reset();
            return;
        }
        const auto registration = state_->register_callback(callback_);
        registration_id_ = registration.id;
        if (registration.invoke_now) {
            callback_->invoke();
        }
    }

    ~Callback() {
        reset();
    }

    Callback(const Callback&) = delete;
    Callback& operator=(const Callback&) = delete;
    Callback(Callback&&) = delete;
    Callback& operator=(Callback&&) = delete;

  private:
    void reset() noexcept {
        if (state_ && registration_id_ != 0) {
            state_->unregister_callback(registration_id_);
        }
        if (callback_) {
            callback_->deactivate();
        }
        registration_id_ = 0;
        callback_.reset();
        state_.reset();
    }

    std::shared_ptr<detail::State> state_;
    std::shared_ptr<detail::CallbackNode> callback_;
    std::uint64_t registration_id_ = 0;
};

class Thread final {
  public:
    Thread() = default;

    template <typename Function>
        requires(!std::is_same_v<std::remove_cvref_t<Function>, Thread>)
    explicit Thread(Function function) {
        auto token = source_.get_token();
        thread_ = std::thread([callable = std::move(function), token]() mutable {
            if constexpr (std::is_invocable_v<decltype(callable)&, const Token&>) {
                std::invoke(callable, token);
            } else {
                static_assert(std::is_invocable_v<decltype(callable)&>);
                std::invoke(callable);
            }
        });
    }

    ~Thread() {
        stop_and_join();
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&& other) noexcept
        : source_(std::move(other.source_)), thread_(std::move(other.thread_)) {}

    Thread& operator=(Thread&& other) noexcept {
        if (this != &other) {
            stop_and_join();
            source_ = std::move(other.source_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    [[nodiscard]] bool request_stop() noexcept {
        return source_.request_stop();
    }

    [[nodiscard]] bool joinable() const noexcept {
        return thread_.joinable();
    }

    void join() {
        thread_.join();
    }

  private:
    void stop_and_join() noexcept {
        static_cast<void>(source_.request_stop());
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Source source_;
    std::thread thread_;
};

// Predicate state is protected by the lock passed to wait(). Every mutation that
// can satisfy a predicate must be followed by notify_all().
class Condition final {
  public:
    Condition() = default;
    ~Condition() = default;
    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;
    Condition(Condition&&) = delete;
    Condition& operator=(Condition&&) = delete;

    void notify_all() noexcept {
        {
            const std::lock_guard lock(mutex_);
            ++generation_;
        }
        condition_.notify_all();
    }

    template <typename Lock, typename Predicate> void wait(Lock& lock, Predicate predicate) {
        for (;;) {
            std::uint64_t generation = 0;
            {
                const std::lock_guard state_lock(mutex_);
                generation = generation_;
            }
            if (predicate()) {
                return;
            }
            std::unique_lock state_lock(mutex_);
            if (generation_ != generation) {
                continue;
            }
            lock.unlock();
            try {
                condition_.wait(state_lock);
            } catch (...) {
                state_lock.unlock();
                lock.lock();
                throw;
            }
            state_lock.unlock();
            lock.lock();
        }
    }

    template <typename Lock, typename Clock, typename Duration, typename Predicate>
    bool wait_until(Lock& lock, const std::chrono::time_point<Clock, Duration>& deadline,
                    Predicate predicate) {
        for (;;) {
            std::uint64_t generation = 0;
            {
                const std::lock_guard state_lock(mutex_);
                generation = generation_;
            }
            if (predicate()) {
                return true;
            }
            std::unique_lock state_lock(mutex_);
            if (generation_ != generation) {
                continue;
            }
            lock.unlock();
            std::cv_status status = std::cv_status::no_timeout;
            try {
                status = condition_.wait_until(state_lock, deadline);
            } catch (...) {
                state_lock.unlock();
                lock.lock();
                throw;
            }
            state_lock.unlock();
            lock.lock();
            if (status == std::cv_status::timeout) {
                return predicate();
            }
        }
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    // The generation covers only the bounded predicate-to-wait enrollment handoff.
    // Once enrolled, the condition-variable notification itself wakes the waiter.
    std::uint64_t generation_ = 0;
};

namespace detail {

struct NoWaitProbe final {
    void operator()() const noexcept {}
};

template <typename Lock, typename Predicate, typename RegistrationProbe, typename WakeProbe>
bool wait_with_probes(Condition& condition, Lock& lock, const Token& token, Predicate predicate,
                      RegistrationProbe registration_probe, WakeProbe wake_probe) {
    const Callback wake(token, [&condition, &wake_probe] {
        std::invoke(wake_probe);
        condition.notify_all();
    });
    std::invoke(registration_probe);
    condition.wait(lock, [&] { return predicate() || token.stop_requested(); });
    return predicate();
}

template <typename Lock, typename Clock, typename Duration, typename Predicate,
          typename RegistrationProbe, typename WakeProbe>
bool wait_until_with_probes(Condition& condition, Lock& lock, const Token& token,
                            const std::chrono::time_point<Clock, Duration>& deadline,
                            Predicate predicate, RegistrationProbe registration_probe,
                            WakeProbe wake_probe) {
    const Callback wake(token, [&condition, &wake_probe] {
        std::invoke(wake_probe);
        condition.notify_all();
    });
    std::invoke(registration_probe);
    static_cast<void>(condition.wait_until(lock, deadline,
                                           [&] { return predicate() || token.stop_requested(); }));
    return predicate();
}

} // namespace detail

template <typename Lock, typename Predicate>
bool wait(Condition& condition, Lock& lock, const Token& token, Predicate predicate) {
    return detail::wait_with_probes(condition, lock, token, std::move(predicate),
                                    detail::NoWaitProbe{}, detail::NoWaitProbe{});
}

template <typename Lock, typename Clock, typename Duration, typename Predicate>
bool wait_until(Condition& condition, Lock& lock, const Token& token,
                const std::chrono::time_point<Clock, Duration>& deadline, Predicate predicate) {
    return detail::wait_until_with_probes(condition, lock, token, deadline, std::move(predicate),
                                          detail::NoWaitProbe{}, detail::NoWaitProbe{});
}

namespace testing {

template <typename Lock, typename Predicate, typename RegistrationProbe, typename WakeProbe>
bool wait_with_probes(Condition& condition, Lock& lock, const Token& token, Predicate predicate,
                      RegistrationProbe registration_probe, WakeProbe wake_probe) {
    return detail::wait_with_probes(condition, lock, token, std::move(predicate),
                                    std::move(registration_probe), std::move(wake_probe));
}

template <typename Lock, typename Clock, typename Duration, typename Predicate,
          typename RegistrationProbe, typename WakeProbe>
bool wait_until_with_probes(Condition& condition, Lock& lock, const Token& token,
                            const std::chrono::time_point<Clock, Duration>& deadline,
                            Predicate predicate, RegistrationProbe registration_probe,
                            WakeProbe wake_probe) {
    return detail::wait_until_with_probes(condition, lock, token, deadline, std::move(predicate),
                                          std::move(registration_probe), std::move(wake_probe));
}

} // namespace testing

} // namespace tgcli::cancellation
