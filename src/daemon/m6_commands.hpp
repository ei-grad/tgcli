#pragma once

#include "core/td_client.hpp"
#include "daemon/resolver.hpp"
#include "proto/frame.hpp"
#include "proto/operation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace tgcli::daemon {

class RequestSession;
class Dispatcher;
class IdempotencyFoundation;
class WriteCoordinator;
namespace testing {
struct FileSpoolHooks;
}

bool run_session_recovery_preflight(
    const std::shared_ptr<IdempotencyFoundation>& foundation, proto::SessionOperation operation,
    RequestSession& session,
    const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks = {});

std::optional<core::TdM6ChatFoldersUpdate>
m6_wait_for_folders(core::TdClient& client,
                    const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                    const ResolverCaller& caller, RequestSession& session);

class M6Coordinator final {
  public:
    M6Coordinator(core::TdClient& client, std::string account,
                  std::shared_ptr<IdempotencyFoundation> foundation = {})
        : client_(client), account_(std::move(account)), foundation_(std::move(foundation)) {}

    void contact(proto::M6Operation operation, const proto::Request& request,
                 RequestSession& session);
    void folder_list(const proto::Request& request, RequestSession& session);
    void folder_show(const proto::Request& request, RequestSession& session);
    void topic_list(const proto::Request& request, RequestSession& session);
    void storage_stats(const proto::Request& request, RequestSession& session);
    void session_list(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
    std::shared_ptr<IdempotencyFoundation> foundation_;
};

void register_m6_commands(Dispatcher& dispatcher, M6Coordinator& reads, WriteCoordinator& writes);

} // namespace tgcli::daemon
