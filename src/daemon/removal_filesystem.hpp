#pragma once

#include "proto/destructive_plan.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace tgcli::daemon {

enum class RemovalFilesystemStage {
    AfterParentOpen,
    BeforeRootRevalidation,
    BeforeStageRename,
    AfterStageRename,
    BeforeEntryRemoval,
    BeforeRootRemoval,
    BeforeParentSync,
};

namespace testing {

struct RemovalFilesystemHooks {
    std::function<void(RemovalFilesystemStage, std::string_view)> at_stage;
    std::function<bool(RemovalFilesystemStage, std::string_view)> should_fail;
    std::function<bool(std::string_view)> force_device_boundary;
    std::function<std::optional<std::uint64_t>(int)> mount_identity;
};

} // namespace testing

struct RemovalFilesystemFailure {
    std::string reason;
};

struct CapturedRemovalRoot {
    std::string path;
    std::optional<proto::RootIdentity> identity;
};

enum class RemovalRootObservation { PlannedAbsent, Captured, Staged, Absent, Changed };

[[nodiscard]] std::optional<CapturedRemovalRoot>
capture_removal_root(std::string path, uid_t expected_uid, RemovalFilesystemFailure& failure,
                     const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks = {});

[[nodiscard]] bool
revalidate_removal_root(const CapturedRemovalRoot& root, uid_t expected_uid,
                        RemovalFilesystemFailure& failure,
                        const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks = {});

[[nodiscard]] std::optional<RemovalRootObservation>
observe_removal_root(const CapturedRemovalRoot& root, std::string_view invocation_id,
                     std::string_view label, uid_t expected_uid, RemovalFilesystemFailure& failure);

// Called only after the matching *_remove_started checkpoint is durable.
// A deterministic staging name lets recovery finish deletion without ever
// following a path that was replaced after planning.
[[nodiscard]] bool
delete_removal_root(const CapturedRemovalRoot& root, std::string_view invocation_id,
                    std::string_view label, uid_t expected_uid, RemovalFilesystemFailure& failure,
                    const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks = {});

} // namespace tgcli::daemon
