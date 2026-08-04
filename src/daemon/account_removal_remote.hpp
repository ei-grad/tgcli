#pragma once

#include "core/auth_bootstrap.hpp"
#include "daemon/account_removal.hpp"

#include <functional>
#include <optional>
#include <string>

namespace tgcli::daemon {

namespace testing {

struct AccountRemovalRemoteHooks {
    std::function<void()> before_send;
    std::function<void()> during_terminal_claim;
};

} // namespace testing

class TdAccountRemovalRemote final : public AccountRemovalRemote {
  public:
    TdAccountRemovalRemote(core::TdClient& client, const config::Store& store,
                           paths::Environment environment, std::string account,
                           std::string application_version,
                           std::optional<std::string> environment_api_id = {},
                           std::optional<std::string> environment_api_hash = {},
                           core::AuthBootstrap::HookRunner hook_runner = secret_hook::run,
                           std::shared_ptr<const testing::AccountRemovalRemoteHooks> hooks = {});

    [[nodiscard]] RemovalRemoteProof
    prove_remote_logout(const proto::AccountRemovePlan& plan,
                        const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot,
                        bool send_checkpointed, RequestSession& session,
                        const RemovalCheckpoint& checkpoint) override;
    [[nodiscard]] std::optional<RemovalOperationError> quiesce(RequestSession& session) override;

  private:
    core::TdClient& client_;
    const config::Store& store_;
    paths::Environment environment_;
    std::string account_;
    std::string application_version_;
    std::optional<std::string> environment_api_id_;
    std::optional<std::string> environment_api_hash_;
    core::AuthBootstrap::HookRunner hook_runner_;
    std::shared_ptr<const testing::AccountRemovalRemoteHooks> hooks_;
};

} // namespace tgcli::daemon
