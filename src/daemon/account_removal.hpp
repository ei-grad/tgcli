#pragma once

#include "common/config.hpp"
#include "common/paths.hpp"
#include "daemon/removal_journal.hpp"
#include "daemon/removal_planner.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace tgcli::daemon {

class Dispatcher;
class RequestSession;

[[nodiscard]] bool preflight_account_removal_journal(const RemovalJournal& journal,
                                                     std::string_view account,
                                                     RequestSession& session);

struct RemovalOperationError {
    std::string code;
    std::string message;
    nlohmann::json details;
    int exit_code = 1;
};

using RemovalCheckpoint = std::function<bool(AuditStage)>;
using RemovalRemoteProof = std::variant<AccountRemoveRemoteResult, RemovalOperationError>;

class AccountRemovalRemote {
  public:
    AccountRemovalRemote() = default;
    virtual ~AccountRemovalRemote() = default;
    AccountRemovalRemote(const AccountRemovalRemote&) = delete;
    AccountRemovalRemote& operator=(const AccountRemovalRemote&) = delete;
    AccountRemovalRemote(AccountRemovalRemote&&) = delete;
    AccountRemovalRemote& operator=(AccountRemovalRemote&&) = delete;
    [[nodiscard]] virtual RemovalRemoteProof
    prove_remote_logout(const proto::AccountRemovePlan& plan,
                        const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot,
                        bool send_checkpointed, RequestSession& session,
                        const RemovalCheckpoint& checkpoint) = 0;
    [[nodiscard]] virtual std::optional<RemovalOperationError> quiesce(RequestSession& session) = 0;
};

namespace testing {

struct AccountRemovalHooks {
    std::function<std::string()> invocation_id;
    std::function<std::string()> timestamp;
    std::shared_ptr<const RemovalFilesystemHooks> filesystem;
};

} // namespace testing

class AccountRemovalCoordinator final {
  public:
    AccountRemovalCoordinator(const config::Store& store, RemovalJournal& journal,
                              paths::Environment environment, std::string account,
                              AccountRemovalRemote& remote,
                              std::function<void()> audit_fatal_shutdown = {},
                              std::shared_ptr<const testing::AccountRemovalHooks> hooks = {},
                              std::function<void()> shutdown_after_terminal = {});

    void remove(const proto::Request& request, RequestSession& session);
    bool preflight(std::string_view account, RequestSession& session) const;

  private:
    const config::Store& store_;
    RemovalJournal& journal_;
    paths::Environment environment_;
    std::string account_;
    AccountRemovalRemote& remote_;
    std::function<void()> audit_fatal_shutdown_;
    std::shared_ptr<const testing::AccountRemovalHooks> hooks_;
    std::function<void()> shutdown_after_terminal_;
    std::mutex operation_mutex_;
};

void register_account_removal_command(Dispatcher& dispatcher,
                                      AccountRemovalCoordinator& coordinator);

} // namespace tgcli::daemon
