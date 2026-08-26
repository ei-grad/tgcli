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

namespace testing {

enum class StreamCoordinatorProbePoint : std::uint8_t { AfterResolve };
using StreamCoordinatorProbeHook = void (*)(void*, StreamCoordinatorProbePoint) noexcept;

struct StreamCoordinatorProbe {
    void* context = nullptr;
    StreamCoordinatorProbeHook hook = nullptr;
};

} // namespace testing

class StreamCoordinator {
  public:
    StreamCoordinator(core::TdClient& client, StreamService& service, std::string account,
                      StreamActivityMode activity_mode, testing::StreamCoordinatorProbe probe = {})
        : client_(client), service_(service), account_(std::move(account)),
          activity_mode_(activity_mode), probe_(probe) {}

    void listen(const proto::Request& request, RequestSession& session);
    void wait_for(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::reference_wrapper<StreamService> service_;
    std::string account_;
    StreamActivityMode activity_mode_ = StreamActivityMode::TrackedDaemon;
    testing::StreamCoordinatorProbe probe_;
};

void register_stream_commands(Dispatcher& dispatcher, StreamCoordinator& coordinator);

} // namespace tgcli::daemon
