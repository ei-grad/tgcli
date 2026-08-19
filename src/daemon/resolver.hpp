#pragma once

#include "core/td_client.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/dispatch.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace tgcli::daemon {

enum class ResolverScope { ActiveDialogs, LocalMaterialized };

bool valid_resolve_selector(std::string_view selector);

class ResolveCoordinator {
  public:
    ResolveCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void resolve(const proto::Request& request, RequestSession& session);
    void resolve_for_scope(std::string selector, ResolverScope scope, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_resolve_command(Dispatcher& dispatcher, ResolveCoordinator& coordinator);

} // namespace tgcli::daemon
