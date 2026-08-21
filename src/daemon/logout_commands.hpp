#pragma once

#include "common/config.hpp"
#include "common/paths.hpp"
#include "core/td_client.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/request_session.hpp"
#include "proto/frame.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tgcli::daemon {

class Dispatcher;

class LogoutLifecycle final {
  public:
    [[nodiscard]] static core::TdClosedDecision
    begin(core::TdClient& client,
          const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
          RequestSession& session, std::function<void()> during_terminal_claim = {});
    static std::future<core::TdValue>
    send(core::TdClient& client,
         const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
         core::TdClosedDecision& decision);
};

namespace testing {

struct LogoutHooks {
    std::function<ChallengeOutcome(ChallengeSpec)> challenge_provider;
    std::function<std::string()> invocation_id;
    std::function<std::string()> timestamp;
    std::function<void()> before_intent;
    std::function<void()> before_send;
    std::function<void()> after_send;
    std::function<void()> after_operation_admission;
    std::function<void()> during_terminal_claim;
    std::shared_ptr<const LogoutAuditHooks> audit;
};

} // namespace testing

class LogoutCoordinator final {
  public:
    LogoutCoordinator(core::TdClient& client, ConfigRuntime& config_runtime,
                      paths::Environment environment, std::string account, std::string config_path,
                      std::function<void()> audit_fatal_shutdown = {},
                      std::shared_ptr<const testing::LogoutHooks> hooks = {});

    void logout(const proto::Request& request, RequestSession& session);
    bool preflight(RequestSession& session);
    bool preflight_read_only(RequestSession& session);

  private:
    enum class PreflightStep : std::uint8_t { Retry, Complete, Failed };

    class OperationPermit {
      public:
        OperationPermit() = default;
        explicit OperationPermit(LogoutCoordinator& owner) : owner_(&owner) {}
        ~OperationPermit();
        OperationPermit(const OperationPermit&) = delete;
        OperationPermit& operator=(const OperationPermit&) = delete;
        OperationPermit(OperationPermit&& other) noexcept;
        OperationPermit& operator=(OperationPermit&& other) noexcept;

      private:
        LogoutCoordinator* owner_ = nullptr;
    };

    static bool request_active(RequestSession& session);
    std::optional<OperationPermit> acquire_operation(RequestSession& session);
    void release_operation();
    void publish_audit_snapshot(const LogoutAuditInspection& snapshot);
    void publish_audit_incomplete(std::string invocation_id, proto::LogoutPlan plan,
                                  std::vector<AuditStage> completed_stages);
    void publish_audit_clean();
    void report_recovery_deadline(RequestSession& session,
                                  const std::optional<LogoutAuditInspection>& snapshot);
    [[nodiscard]] ChallengeOutcome request_challenge(RequestSession& session,
                                                     ChallengeSpec spec) const;
    PreflightStep reconcile_preflight(RequestSession& session);
    void report_audit_incomplete(RequestSession& session, const IncompleteLogoutAudit& incomplete,
                                 std::string_view message);
    void report_audit_unavailable(RequestSession& session, std::string reason = "path_invalid");
    bool append_unconfirmed_recovery(const IncompleteLogoutAudit& incomplete,
                                     core::AuthState observed);
    std::optional<core::AuthState> observe_recovery_state(RequestSession& session);
    std::optional<core::AuthState>
    wait_for_bootstrap_observation(RequestSession& session,
                                   const std::shared_ptr<const core::AuthStateSnapshot>& starting,
                                   std::future<core::TdValue>& response);

    core::TdClient& client_;
    ConfigRuntime& config_runtime_;
    paths::Environment environment_;
    std::string account_;
    std::string config_path_;
    config::Store config_store_;
    std::function<void()> audit_fatal_shutdown_;
    std::shared_ptr<const testing::LogoutHooks> hooks_;
    LogoutAuditLog audit_;
    std::mutex operation_mutex_;
    std::condition_variable_any operation_condition_;
    bool operation_active_ = false;
    std::optional<LogoutAuditInspection> durable_audit_snapshot_;
};

void register_logout_command(Dispatcher& dispatcher, LogoutCoordinator& coordinator);

} // namespace tgcli::daemon
