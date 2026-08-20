#pragma once

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

namespace tgcli {

using RequestClock = std::chrono::steady_clock;

struct RequestDeadline {
    RequestDeadline() = default;
    RequestDeadline(RequestClock::time_point expiry) : expires_at(expiry) {}

    std::optional<RequestClock::time_point> expires_at;

    friend bool operator==(const RequestDeadline&, const RequestDeadline&) = default;
};

enum class DeadlineDefault { Default60, Unlimited };

inline std::optional<RequestDeadline>
request_deadline(std::optional<double> timeout_seconds, DeadlineDefault policy,
                 RequestClock::time_point now = RequestClock::now()) {
    if (!timeout_seconds) {
        if (policy == DeadlineDefault::Unlimited) {
            return RequestDeadline{};
        }
        timeout_seconds = 60.0;
    }
    if (!std::isfinite(*timeout_seconds) || *timeout_seconds <= 0.0) {
        return std::nullopt;
    }

    using Duration = RequestClock::duration;
    using Rep = Duration::rep;
    using TickDuration = std::chrono::duration<long double, Duration::period>;
    const auto requested_ticks =
        TickDuration(std::chrono::duration<long double>(static_cast<long double>(*timeout_seconds)))
            .count();
    const auto now_ticks = static_cast<long double>(now.time_since_epoch().count());
    const auto available_ticks =
        static_cast<long double>(std::numeric_limits<Rep>::max()) - now_ticks;
    const auto rounded_ticks = std::ceil(requested_ticks);
    if (!std::isfinite(requested_ticks) || requested_ticks <= 0.0L ||
        rounded_ticks > available_ticks ||
        rounded_ticks > static_cast<long double>(std::numeric_limits<Rep>::max())) {
        return std::nullopt;
    }
    const Duration duration(static_cast<Rep>(rounded_ticks));
    if (duration <= Duration::zero()) {
        return std::nullopt;
    }
    return RequestDeadline{now + duration};
}

inline bool deadline_expired(const RequestDeadline& deadline,
                             RequestClock::time_point now = RequestClock::now()) {
    return deadline.expires_at && now >= *deadline.expires_at;
}

inline bool event_precedes_deadline(const std::optional<RequestClock::time_point>& observed_at,
                                    const RequestDeadline& deadline) {
    return observed_at && (!deadline.expires_at || *observed_at < *deadline.expires_at);
}

} // namespace tgcli
