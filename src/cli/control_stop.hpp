#pragma once

#include "common/paths.hpp"
#include "proto/frame_io.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace tgcli::cli::detail {

enum class ControlStopOutcome { Sent, AlreadyGone, Failed };
enum class ControlConnectOutcome { Connected, AlreadyGone, Failed };

using ControlRetryObserver = std::function<void()>;

ControlConnectOutcome
connect_verified_control_endpoint(const std::string& control_socket_path,
                                  const paths::SocketIdentity& frozen_identity, uid_t uid, int& fd,
                                  std::string& error);

ControlStopOutcome send_connected_control_stop(int fd, std::string_view control_token,
                                               proto::IoDeadline deadline,
                                               const ControlRetryObserver& retry_observer,
                                               std::string& error);

ControlStopOutcome send_verified_control_stop(const std::string& control_socket_path,
                                              const paths::SocketIdentity& frozen_identity,
                                              uid_t uid, std::string_view control_token,
                                              proto::IoDeadline deadline, std::string& error);

} // namespace tgcli::cli::detail
