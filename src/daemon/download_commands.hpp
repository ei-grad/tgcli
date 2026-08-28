#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/download_filesystem.hpp"

#include <memory>
#include <string>

namespace tgcli::daemon {

class DownloadCoordinator {
  public:
    DownloadCoordinator(
        core::TdClient& client, std::string account,
        std::shared_ptr<const testing::DownloadFilesystemHooks> filesystem_hooks = {})
        : client_(client), account_(std::move(account)),
          filesystem_hooks_(std::move(filesystem_hooks)) {}

    void download(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
    std::shared_ptr<const testing::DownloadFilesystemHooks> filesystem_hooks_;
};

void register_download_command(Dispatcher& dispatcher, DownloadCoordinator& coordinator);

} // namespace tgcli::daemon
