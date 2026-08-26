#pragma once

#include "daemon/dispatch.hpp"
#include "daemon/stream_subscription.hpp"

#include <functional>
#include <string>

namespace tgcli::core {
class TdClient;
}

namespace tgcli::daemon {

class StreamService;

class StreamCoordinator {
  public:
    StreamCoordinator(core::TdClient& client, StreamService& service, std::string account,
                      StreamActivityMode activity_mode)
        : client_(client), service_(service), account_(std::move(account)),
          activity_mode_(activity_mode) {}

    void listen(const proto::Request& request, RequestSession& session);
    void wait_for(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::reference_wrapper<StreamService> service_;
    std::string account_;
    StreamActivityMode activity_mode_ = StreamActivityMode::TrackedDaemon;
};

void register_stream_commands(Dispatcher& dispatcher, StreamCoordinator& coordinator);

} // namespace tgcli::daemon
