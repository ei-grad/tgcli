#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tgcli::daemon {

enum class ResolverScope { ActiveDialogs, LocalMaterialized };

struct ChatIdentity {
    std::int64_t id = 0;
    std::string title;
    std::string type;
    bool is_bot = false;
    std::vector<std::string> usernames;

    bool operator==(const ChatIdentity&) const = default;
};

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
