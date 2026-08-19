#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <functional>
#include <string>

namespace tgcli::daemon {

class MessageCoordinator {
  public:
    MessageCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void get(const proto::Request& request, RequestSession& session);
    void link(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_message_commands(Dispatcher& dispatcher, MessageCoordinator& coordinator);

} // namespace tgcli::daemon
