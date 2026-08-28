#include "daemon/raw_contract.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<tgcli::daemon::raw::TypedFunction>);
static_assert(!std::is_copy_assignable_v<tgcli::daemon::raw::TypedFunction>);
static_assert(std::is_nothrow_move_constructible_v<tgcli::daemon::raw::TypedFunction>);
static_assert(std::is_nothrow_move_assignable_v<tgcli::daemon::raw::TypedFunction>);
static_assert(tgcli::daemon::raw::kMaximumRequestBytes == 1'048'576);
static_assert(tgcli::daemon::raw::kMaximumResponseBytes == 16'777'216);
