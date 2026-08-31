#pragma once

#include "daemon/raw_audit_log.hpp"
#include "daemon/raw_contract.hpp"
#include "proto/frame.hpp"

#include <mutex>
#include <string>
#include <sys/types.h>

namespace tgcli::core {
class TdClient;
}

namespace tgcli::daemon {

class Dispatcher;
class RequestSession;

class RawCoordinator final {
  public:
    RawCoordinator(core::TdClient& client, std::string account, std::string state_directory,
                   uid_t expected_uid);

    void execute(const proto::Request& request, RequestSession& session);

  private:
    core::TdClient& client_;
    std::string account_;
    raw::audit_v3::Log audit_;
    std::mutex audit_mutex_;
};

void register_raw_command(Dispatcher& dispatcher, RawCoordinator& coordinator);

} // namespace tgcli::daemon
