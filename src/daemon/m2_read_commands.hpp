#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <functional>
#include <string>

namespace tgcli::daemon {

class M2ReadCoordinator {
  public:
    M2ReadCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void search(const proto::Request& request, RequestSession& session);
    void chat_info(const proto::Request& request, RequestSession& session);
    void chat_members(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_search_command(Dispatcher& dispatcher, M2ReadCoordinator& coordinator);
void register_chat_info_command(Dispatcher& dispatcher, M2ReadCoordinator& coordinator);
void register_chat_members_command(Dispatcher& dispatcher, M2ReadCoordinator& coordinator);

} // namespace tgcli::daemon
