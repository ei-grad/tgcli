#pragma once

#include "core/td_client.hpp"
#include "proto/frame.hpp"
#include "proto/operation.hpp"

#include <functional>
#include <string>

namespace tgcli::daemon {

class RequestSession;

class M6Coordinator final {
  public:
    M6Coordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

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
};

} // namespace tgcli::daemon
