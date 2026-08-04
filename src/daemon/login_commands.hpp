#pragma once

#include "common/config.hpp"
#include "common/paths.hpp"
#include "common/secret_hook.hpp"
#include "core/auth_bootstrap.hpp"
#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tgcli::daemon {

class LoginCoordinator {
  public:
    LoginCoordinator(core::TdClient& client, const config::Store& store,
                     paths::Environment environment, std::string account,
                     std::string application_version,
                     std::optional<std::string> environment_api_id = {},
                     std::optional<std::string> environment_api_hash = {},
                     core::AuthBootstrap::HookRunner hook_runner = secret_hook::run);

    void login(const proto::Request& request, RequestSession& session);
    void me(const proto::Request& request, RequestSession& session);

  private:
    core::TdClient& client_;
    const config::Store& store_;
    paths::Environment environment_;
    std::string account_;
    std::string application_version_;
    std::optional<std::string> environment_api_id_;
    std::optional<std::string> environment_api_hash_;
    core::AuthBootstrap::HookRunner hook_runner_;
    std::mutex lease_mutex_;
    bool login_active_ = false;
    std::jthread lifecycle_waiter_;
};

void register_login_commands(Dispatcher& dispatcher, LoginCoordinator& coordinator);

} // namespace tgcli::daemon
