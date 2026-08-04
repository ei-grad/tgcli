#pragma once

#include "core/td_runtime.hpp"

#include <optional>

namespace tgcli::core {

std::optional<TdAuthorizationFailure> authorize_td_send(const TdSendDescriptor& descriptor,
                                                        const TdFunctionData* function,
                                                        const AuthStateSnapshot& current,
                                                        bool generation_closed);

} // namespace tgcli::core
