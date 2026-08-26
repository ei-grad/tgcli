#pragma once

#include "core/td_runtime.hpp"
#include "daemon/stream_commands.hpp"
#include "daemon/stream_subscription.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>

namespace tgcli::daemon {

class RequestSession;
struct StreamWaitScanResult;

namespace detail {
class StreamWaitScannerRun;
}

struct StreamHistoryRequest {
    std::int64_t chat_id = 0;
    std::int64_t from_message_id = 0;
    std::int32_t offset = 0;
    std::int32_t limit = 100;
    bool only_local = true;

    bool operator==(const StreamHistoryRequest&) const = default;
};

using StreamHistoryStart = std::function<std::future<core::TdValue>(const StreamHistoryRequest&)>;

struct StreamWaitScannerHooks {
    using Clock = StreamPollSchedule::Clock;
    std::function<Clock::time_point()> now;
    std::function<void(Clock::time_point)> sleep_until;
};

struct StreamWaitScannerOptions {
    std::int64_t chat_id = 0;
    std::int64_t after = 0;
    StreamMessageMatcher matcher;
    StreamHistoryStart start_history;
    std::shared_ptr<const StreamWaitScannerHooks> hooks;
};

class StreamWaitMatchState {
  public:
    class Impl;

    ~StreamWaitMatchState();
    StreamWaitMatchState(const StreamWaitMatchState&) = delete;
    StreamWaitMatchState& operator=(const StreamWaitMatchState&) = delete;
    StreamWaitMatchState(StreamWaitMatchState&&) = delete;
    StreamWaitMatchState& operator=(StreamWaitMatchState&&) = delete;

    [[nodiscard]] std::optional<nlohmann::json> initial_match() const;
    [[nodiscard]] std::optional<nlohmann::json> match_live(const StreamCopiedItem& item) const;

  private:
    explicit StreamWaitMatchState(StreamMessageMatcher matcher);
    std::unique_ptr<Impl> impl_;

    friend class detail::StreamWaitScannerRun;
};

struct StreamWaitScanResult {
    std::shared_ptr<StreamWaitMatchState> state;
};

[[nodiscard]] StreamWaitScanResult scan_wait_history(RequestSession& session,
                                                     StreamSubscriptionWorker& worker,
                                                     const StreamWaitScannerOptions& options);

} // namespace tgcli::daemon
