#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tgcli::secret_hook {

inline constexpr std::size_t kMaxOutputBytes = std::size_t{64} * 1024;
inline constexpr auto kMaximumRuntime = std::chrono::seconds(10);

enum class HookField { ApiId, ApiHash, DatabaseKey, Password, BotToken };
enum class HookFailure {
    Spawn,
    Exit,
    Signal,
    Timeout,
    StdoutEmpty,
    StdoutInvalid,
    StdoutTooLarge,
    StderrTooLarge,
};

std::string_view field_name(HookField field);
std::string_view failure_name(HookFailure reason);

struct HookError {
    HookField field = HookField::ApiId;
    HookFailure reason = HookFailure::Spawn;
    std::optional<int> status;
};

struct HookRequest {
    HookField field;
    std::string command;
    std::optional<std::chrono::steady_clock::time_point> request_deadline;
};

struct HookResult {
    std::string value;
    std::optional<HookError> error;

    explicit operator bool() const {
        return !error;
    }
};

HookResult run(const HookRequest& request);
bool parse_api_id(std::string_view value, std::int32_t& parsed);

// Public diagnostic text includes only the field, closed failure class and
// numeric exit/signal status. It never contains command or captured output.
std::string describe(const HookError& error);

} // namespace tgcli::secret_hook
