#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <chrono>
#include <functional>
#include <string>

namespace tgcli::daemon {

class ReadCoordinator {
  public:
    using WallClock = std::function<std::chrono::system_clock::time_point()>;

    ReadCoordinator(core::TdClient& client, std::string account, WallClock wall_clock = {})
        : client_(client), account_(std::move(account)), wall_clock_(std::move(wall_clock)) {}

    void read(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
    WallClock wall_clock_;
};

void register_read_command(Dispatcher& dispatcher, ReadCoordinator& coordinator);

} // namespace tgcli::daemon
