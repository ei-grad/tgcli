#pragma once

#include <chrono>
#include <cstddef>
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
        Token() = default;
        ~Token();

        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&& other) noexcept;
        Token& operator=(Token&& other) noexcept;

        [[nodiscard]] explicit operator bool() const {
            return state_ != nullptr;
        }

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

  private:
    [[nodiscard]] std::optional<Token> try_admit(Kind kind);

    std::shared_ptr<State> state_;
};

} // namespace tgcli::daemon
