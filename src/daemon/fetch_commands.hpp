#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <utility>

namespace tgcli::daemon {

class FetchCoordinator {
  public:
    using WallClock = std::function<std::chrono::system_clock::time_point()>;

    FetchCoordinator(core::TdClient& client, std::string account, WallClock wall_clock = {})
        : client_(client), account_(std::move(account)), wall_clock_(std::move(wall_clock)) {}

    void fetch(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
    WallClock wall_clock_;
};

void register_fetch_command(Dispatcher& dispatcher, FetchCoordinator& coordinator);

} // namespace tgcli::daemon
