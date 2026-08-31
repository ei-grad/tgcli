#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

#include <nlohmann/json.hpp>

namespace tgcli::daemon::raw::audit_v3 {

enum class LogStatus { Clean, Repaired, Unconfirmed, Contradiction, Unavailable };

struct UnconfirmedTerminal {
    std::string function;
    std::string request_sha256;
};

struct LogRecovery {
    LogStatus status = LogStatus::Unavailable;
    std::optional<UnconfirmedTerminal> unconfirmed;
};

class Log final {
  public:
    Log(std::string state_directory, uid_t expected_uid);

    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] bool append(const nlohmann::json& record) const;
    [[nodiscard]] LogRecovery recover() const;

  private:
    std::string state_directory_;
    std::string path_;
    uid_t expected_uid_ = 0;
};

} // namespace tgcli::daemon::raw::audit_v3
