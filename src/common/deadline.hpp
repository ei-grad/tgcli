#pragma once

#include <chrono>
#include <cmath>
#include <concepts>
#include <limits>
#include <optional>
#include <type_traits>

namespace tgcli {

using RequestClock = std::chrono::steady_clock;

struct RequestDeadline {
    RequestDeadline() = default;
    RequestDeadline(RequestClock::time_point expiry) : expires_at(expiry) {}

    std::optional<RequestClock::time_point> expires_at;

    friend bool operator==(const RequestDeadline&, const RequestDeadline&) = default;
};

enum class DeadlineDefault { Default60, Unlimited };

namespace detail {

template <std::floating_point Float, std::signed_integral Rep>
std::optional<Rep> ceil_request_ticks(Float requested_ticks, Rep maximum_ticks) {
    if (!std::isfinite(requested_ticks) || requested_ticks <= Float{0} || maximum_ticks <= Rep{0}) {
        return std::nullopt;
    }

    int exponent = 0;
    static_cast<void>(std::frexp(requested_ticks, &exponent));
    if (exponent > std::numeric_limits<Rep>::digits) {
        return std::nullopt;
    }

    const auto integral_ticks = std::floor(requested_ticks);
    using UnsignedRep = std::make_unsigned_t<Rep>;
    const auto integral_count = static_cast<UnsignedRep>(integral_ticks);
    const auto maximum_count = static_cast<UnsignedRep>(maximum_ticks);
    const bool has_fraction = requested_ticks > integral_ticks;
    if (integral_count > maximum_count || (has_fraction && integral_count == maximum_count)) {
        return std::nullopt;
    }
    return static_cast<Rep>(integral_count + static_cast<UnsignedRep>(has_fraction));
}

} // namespace detail

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
    const auto now_ticks = now.time_since_epoch().count();
    const auto available_ticks = now_ticks > Rep{0} ? std::numeric_limits<Rep>::max() - now_ticks
                                                    : std::numeric_limits<Rep>::max();
    const auto rounded_ticks = detail::ceil_request_ticks(requested_ticks, available_ticks);
    if (!rounded_ticks) {
        return std::nullopt;
    }
    const Duration duration(*rounded_ticks);
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
