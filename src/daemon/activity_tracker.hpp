#pragma once

#include "common/cancellation.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace tgcli::daemon {

namespace testing {

struct ActivityTrackerHooks {
    using Clock = std::chrono::steady_clock;

    std::function<Clock::time_point()> now;
    std::function<void()> before_admit_locked;
    std::function<void()> before_expire_locked;
};

} // namespace testing

class ActivityTracker {
  private:
    enum class Kind { Request, Subscription };
    struct State;

  public:
    using Clock = std::chrono::steady_clock;
    using IdleExit = std::optional<std::chrono::seconds>;

    class Token {
      public:
        using PromotionCommit = bool (*)(void*) noexcept;
        Token() = default;
        ~Token();

        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&& other) noexcept;
        Token& operator=(Token&& other) noexcept;

        [[nodiscard]] explicit operator bool() const {
            return state_ != nullptr;
        }

        [[nodiscard]] bool promote_to_subscription();
        [[nodiscard]] bool promote_to_subscription(void* context, PromotionCommit commit);
        void reset();

      private:
        friend class ActivityTracker;
        Token(std::shared_ptr<State> state, Kind kind);

        std::shared_ptr<State> state_;
        Kind kind_ = Kind::Request;
    };

    struct Snapshot {
        bool daemon_ready = false;
        bool expired = false;
        std::size_t requests = 0;
        std::size_t subscriptions = 0;
        IdleExit idle_exit;
        std::optional<Clock::time_point> zero_since;
        std::optional<Clock::time_point> deadline;
    };

    explicit ActivityTracker(std::function<void()> on_expired,
                             std::shared_ptr<const testing::ActivityTrackerHooks> hooks = {});

    [[nodiscard]] bool daemon_ready(IdleExit idle_exit);
    [[nodiscard]] std::optional<Token> try_request();
    [[nodiscard]] std::optional<Token> try_subscription();
    [[nodiscard]] bool update_idle_exit(IdleExit idle_exit);
    [[nodiscard]] bool expire_if_due();
    [[nodiscard]] Snapshot snapshot() const;
    // Callback failures are contained after expiry is claimed. Expiry remains
    // final and the callback is never retried.
    void watch(const cancellation::Token& stop);

  private:
    [[nodiscard]] std::optional<Token> try_admit(Kind kind);

    std::shared_ptr<State> state_;
};

} // namespace tgcli::daemon
