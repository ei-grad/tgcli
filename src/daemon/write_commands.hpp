#pragma once

#include "common/config.hpp"
#include "core/td_client.hpp"
#include "daemon/direct_rpc.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/forward.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/single_send.hpp"

#include <functional>
#include <memory>
#include <string>

namespace tgcli::daemon {

namespace testing {
struct WriteCoordinatorHooks {
    SingleSendHooks single_send;
    ForwardHooks forward;
    DirectRpcHooks direct_rpc;
    std::shared_ptr<const FileSpoolHooks> file_spool;
};
} // namespace testing

class WriteCoordinator final {
  public:
    WriteCoordinator(core::TdClient& client, std::string account, std::string config_path,
                     uid_t expected_uid, std::shared_ptr<IdempotencyFoundation> foundation,
                     std::function<void()> audit_fatal_shutdown = {},
                     std::shared_ptr<const testing::WriteCoordinatorHooks> hooks = {});

    void send(const proto::Request& request, RequestSession& session);
    void attach_saved_file(const proto::Request& request, RequestSession& session);
    void edit_message(const proto::Request& request, RequestSession& session);
    void forward_messages(const proto::Request& request, RequestSession& session);
    void delete_messages(const proto::Request& request, RequestSession& session);
    void react_to_message(const proto::Request& request, RequestSession& session);
    void pin_message(const proto::Request& request, RequestSession& session, bool pinned);
    void mark_chat_read(const proto::Request& request, RequestSession& session);
    void mute_chat(const proto::Request& request, RequestSession& session, bool muted);
    void pin_chat(const proto::Request& request, RequestSession& session, bool pinned);
    void archive_chat(const proto::Request& request, RequestSession& session, bool archived);
    void join_chat(const proto::Request& request, RequestSession& session);
    void leave_chat(const proto::Request& request, RequestSession& session);
    void m6_mutation(proto::M6Operation operation, const proto::Request& request,
                     RequestSession& session);
    void terminate_session(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
    config::Store config_store_;
    std::shared_ptr<IdempotencyFoundation> foundation_;
    std::function<void()> audit_fatal_shutdown_;
    std::shared_ptr<const testing::WriteCoordinatorHooks> hooks_;
};

void register_write_commands(Dispatcher& dispatcher, WriteCoordinator& coordinator);

} // namespace tgcli::daemon
