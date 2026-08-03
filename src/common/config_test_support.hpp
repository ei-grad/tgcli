#pragma once

#include "common/config.hpp"

#include <functional>

namespace tgcli::config::testing {

enum class MutationStage { AfterLock, BeforeCommit, AfterPrepare, AfterExchange };
enum class MutationFault {
    ParentDirectorySync,
    TemporaryFileSync,
    ReplacementRename,
    CommitDirectorySync,
    RollbackRename,
    RollbackDirectorySync,
};

struct StoreHooks {
    std::function<void(MutationStage)> at_stage;
    std::function<bool(MutationFault)> should_fail;
};

} // namespace tgcli::config::testing
