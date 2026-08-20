#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <functional>
#include <string>
#include <utility>

namespace tgcli::daemon {

class FetchCoordinator {
  public:
    FetchCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void fetch(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_fetch_command(Dispatcher& dispatcher, FetchCoordinator& coordinator);

} // namespace tgcli::daemon
