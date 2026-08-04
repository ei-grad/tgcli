#pragma once

#include "core/td_client.hpp"

namespace tgcli::test {

template <typename Client>
concept CanObtainInternalAuthOwner =
    requires(const Client& client) { client.internal_auth_owner(); };

template <typename Client>
concept CanMintLoginOwner = requires(Client& client) { client.issue_login_owner(); };

static_assert(!CanObtainInternalAuthOwner<core::TdClient>);
static_assert(!CanMintLoginOwner<core::TdClient>);

} // namespace tgcli::test
