#pragma once

#include "common/deadline.hpp"
#include "common/secure_wipe.hpp"
#include "core/td_client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace tgcli::daemon {

class RequestSession;

enum class DirectMutationState { None, Possible, Confirmed };

struct DirectDeleteResult {
    std::int64_t chat_id = 0;
    std::vector<std::int64_t> message_ids;
    bool for_all = false;
    bool deleted = true;

    bool operator==(const DirectDeleteResult&) const = default;
};

struct DirectReactionResult {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::string reaction;
    bool removed = false;
    bool big = false;

    bool operator==(const DirectReactionResult&) const = default;
};

struct DirectMessagePinResult {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    bool pinned = false;

    bool operator==(const DirectMessagePinResult&) const = default;
};

struct DirectMarkReadResult {
    std::int64_t chat_id = 0;
    std::optional<std::int64_t> last_read_message_id;
    bool marked_read = true;

    bool operator==(const DirectMarkReadResult&) const = default;
};

struct DirectMuteResult {
    std::int64_t chat_id = 0;
    bool muted = false;
    std::int32_t duration_seconds = 0;

    bool operator==(const DirectMuteResult&) const = default;
};

struct DirectChatPinResult {
    std::int64_t chat_id = 0;
    core::TdDirectChatList chat_list = core::TdDirectChatList::Main;
    bool pinned = false;

    bool operator==(const DirectChatPinResult&) const = default;
};

struct DirectArchiveResult {
    std::int64_t chat_id = 0;
    bool archived = false;

    bool operator==(const DirectArchiveResult&) const = default;
};

enum class DirectJoinStatus { Joined, RequestSent };

struct DirectJoinResult {
    DirectJoinStatus status = DirectJoinStatus::Joined;
    std::optional<std::int64_t> chat_id;

    bool operator==(const DirectJoinResult&) const = default;
};

struct DirectLeaveResult {
    std::int64_t chat_id = 0;
    bool left = true;

    bool operator==(const DirectLeaveResult&) const = default;
};

struct DirectTerminateSessionResult {
    std::int64_t session_id = 0;

    bool operator==(const DirectTerminateSessionResult&) const = default;
};

using DirectResult =
    std::variant<core::TdMessageWriteResult, DirectDeleteResult, DirectReactionResult,
                 DirectMessagePinResult, DirectMarkReadResult, DirectMuteResult,
                 DirectChatPinResult, DirectArchiveResult, DirectJoinResult, DirectLeaveResult,
                 DirectTerminateSessionResult, core::TdM6Response>;

struct DirectSuccess {
    DirectResult result;
    DirectMutationState mutation_state = DirectMutationState::Confirmed;

    bool operator==(const DirectSuccess&) const = default;
};

struct DirectTdError {
    DirectTdError(core::TdError& source, secure::WipeObserver wipe_observer = {});
    ~DirectTdError();
    DirectTdError(const DirectTdError&) = delete;
    DirectTdError& operator=(const DirectTdError&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    DirectTdError(DirectTdError&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    DirectTdError& operator=(DirectTdError&& other);

    core::TdError error; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
    DirectMutationState mutation_state = DirectMutationState::Possible;

  private:
    secure::WipeObserver wipe_observer_;
};

struct DirectAuthorizationLost {
    std::shared_ptr<const core::AuthStateSnapshot> snapshot;
    DirectMutationState mutation_state = DirectMutationState::Possible;
};

struct DirectTimedOut {
    DirectMutationState mutation_state = DirectMutationState::Possible;

    bool operator==(const DirectTimedOut&) const = default;
};

struct DirectCancelled {
    DirectMutationState mutation_state = DirectMutationState::Possible;

    bool operator==(const DirectCancelled&) const = default;
};

struct DirectRejected {
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    DirectMutationState mutation_state = DirectMutationState::None;

    bool operator==(const DirectRejected&) const = default;
};

struct DirectMalformed {
    std::optional<std::int32_t> tdlib_type_id;
    DirectMutationState mutation_state = DirectMutationState::Possible;

    bool operator==(const DirectMalformed&) const = default;
};

struct DirectOversizedMessage {
    DirectMutationState mutation_state = DirectMutationState::Confirmed;

    bool operator==(const DirectOversizedMessage&) const = default;
};

struct DirectJoinGuardRequired {
    std::int64_t bot_user_id = 0;
    std::int64_t query_id = 0;
    DirectMutationState mutation_state = DirectMutationState::None;

    bool operator==(const DirectJoinGuardRequired&) const = default;
};

struct DirectJoinDeclined {
    DirectMutationState mutation_state = DirectMutationState::None;

    bool operator==(const DirectJoinDeclined&) const = default;
};

using DirectOutcome =
    std::variant<DirectSuccess, DirectTdError, DirectAuthorizationLost, DirectTimedOut,
                 DirectCancelled, DirectRejected, DirectMalformed, DirectOversizedMessage,
                 DirectJoinGuardRequired, DirectJoinDeclined>;

struct DirectPrepared {};
using DirectPreparationOutcome = std::variant<DirectPrepared, DirectAuthorizationLost,
                                              DirectTimedOut, DirectCancelled, DirectRejected>;

struct DirectRpcHooks {
    std::function<core::TdEventClock::time_point()> now;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait;
    std::function<void()> before_request;
    std::function<void()> before_submit;
    std::function<void()> before_event_arbitration;
    std::function<void()> before_wait;
};

class DirectRpcCoordinator {
  public:
    DirectRpcCoordinator(core::TdClient& client, RequestSession& session,
                         DirectRpcHooks hooks = {});
    ~DirectRpcCoordinator();
    DirectRpcCoordinator(const DirectRpcCoordinator&) = delete;
    DirectRpcCoordinator& operator=(const DirectRpcCoordinator&) = delete;
    DirectRpcCoordinator(DirectRpcCoordinator&&) = delete;
    DirectRpcCoordinator& operator=(DirectRpcCoordinator&&) = delete;

    DirectOutcome execute(const core::TdDirectRequest& request,
                          const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                          core::TdQueryLifetime lifetime = {});
    DirectPreparationOutcome
    prepare(core::TdDirectRequest request,
            const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
            core::TdQueryLifetime lifetime = {});
    DirectOutcome execute_prepared();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::daemon
