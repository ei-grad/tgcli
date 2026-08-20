#pragma once

#include "common/deadline.hpp"
#include "core/td_client.hpp"

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>

namespace tgcli::daemon {

class RequestSession;

enum class ReadyReadStatus { Response, AuthorizationLost, TimedOut, Cancelled, Failed };

struct ReadyReadResult {
    ReadyReadStatus status = ReadyReadStatus::Failed;
    core::TdValue value;
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    std::shared_ptr<const core::AuthStateSnapshot> snapshot;
};

using ReadyReadStart = std::function<std::future<core::TdValue>(
    const std::shared_ptr<const core::AuthStateSnapshot>&)>;

struct ReadyReadHooks {
    std::function<core::TdEventClock::time_point()> now;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait;
    std::function<void()> before_event_arbitration;
    std::function<void()> before_wait;
};

class ReadyReadSession {
  public:
    ReadyReadSession(core::TdClient& client, RequestSession& session, ReadyReadHooks hooks = {});
    ~ReadyReadSession();
    ReadyReadSession(const ReadyReadSession&) = delete;
    ReadyReadSession& operator=(const ReadyReadSession&) = delete;
    ReadyReadSession(ReadyReadSession&&) = delete;
    ReadyReadSession& operator=(ReadyReadSession&&) = delete;

    [[nodiscard]] std::shared_ptr<const core::AuthStateSnapshot> current() const;
    ReadyReadResult read(const ReadyReadStart& start,
                         std::shared_ptr<const core::AuthStateSnapshot>& snapshot);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::daemon
