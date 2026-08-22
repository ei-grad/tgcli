#pragma once

#include "common/deadline.hpp"
#include "core/td_client.hpp"
#include "daemon/single_send.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <variant>
#include <vector>

namespace tgcli::daemon {

class RequestSession;

enum class ForwardMutationState { None, Possible, Confirmed };
enum class ForwardFailureReason { UpstreamNull, TdlibError, DeletedBeforeConfirmation };

struct ForwardPending {
    std::int64_t source_id = 0;
    std::int64_t temporary_message_id = 0;

    bool operator==(const ForwardPending&) const = default;
};

struct ForwardSent {
    std::int64_t source_id = 0;
    core::TdMessageWriteResult message;

    bool operator==(const ForwardSent&) const = default;
};

struct ForwardFailed {
    std::int64_t source_id = 0;
    ForwardFailureReason reason = ForwardFailureReason::UpstreamNull;
    std::optional<std::int32_t> tdlib_code;
    std::optional<std::int32_t> retry_after;

    bool operator==(const ForwardFailed&) const = default;
};

using ForwardItem = std::variant<ForwardPending, ForwardSent, ForwardFailed>;

struct ForwardCompleted {
    std::vector<ForwardItem> items;
    ForwardMutationState mutation_state = ForwardMutationState::None;

    bool operator==(const ForwardCompleted&) const = default;
};

struct ForwardTopLevelError {
    core::TdError error;
    ForwardMutationState mutation_state = ForwardMutationState::Possible;

    bool operator==(const ForwardTopLevelError&) const = default;
};

struct ForwardTimedOut {
    std::vector<ForwardItem> items;
    ForwardMutationState mutation_state = ForwardMutationState::Possible;

    bool operator==(const ForwardTimedOut&) const = default;
};

struct ForwardAuthorizationLost {
    std::vector<ForwardItem> items;
    std::uint64_t client_generation = 0;
    std::uint64_t auth_sequence = 0;
    core::AuthState state = core::AuthState::Unknown;
    ForwardMutationState mutation_state = ForwardMutationState::Possible;

    bool operator==(const ForwardAuthorizationLost&) const = default;
};

struct ForwardGenerationClosed {
    std::vector<ForwardItem> items;
    std::uint64_t client_generation = 0;
    ForwardMutationState mutation_state = ForwardMutationState::Possible;

    bool operator==(const ForwardGenerationClosed&) const = default;
};

struct ForwardCancelled {
    std::vector<ForwardItem> items;
    ForwardMutationState mutation_state = ForwardMutationState::Possible;

    bool operator==(const ForwardCancelled&) const = default;
};

struct ForwardRejected {
    std::vector<ForwardItem> items;
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    ForwardMutationState mutation_state = ForwardMutationState::None;

    bool operator==(const ForwardRejected&) const = default;
};

struct ForwardMalformed {
    std::vector<ForwardItem> items;
    std::optional<std::int32_t> tdlib_type_id;
    ForwardMutationState mutation_state = ForwardMutationState::Possible;

    bool operator==(const ForwardMalformed&) const = default;
};

using ForwardOutcome =
    std::variant<ForwardCompleted, ForwardTopLevelError, ForwardTimedOut, ForwardAuthorizationLost,
                 ForwardGenerationClosed, ForwardCancelled, ForwardRejected, ForwardMalformed>;

struct ForwardPrepared {};
using ForwardPreparationOutcome =
    std::variant<ForwardPrepared, ForwardAuthorizationLost, ForwardGenerationClosed,
                 ForwardTimedOut, ForwardCancelled, ForwardRejected>;

struct ForwardHooks {
    std::function<core::TdEventClock::time_point()> now;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait;
    std::function<void()> before_request;
    std::function<void()> before_submit;
    std::function<void()> before_event_arbitration;
    std::function<void()> before_wait;
    std::function<void(const std::vector<std::int64_t>&)> on_temporary_ids;
    std::function<void(const std::vector<ForwardItem>&)> on_progress;
};

class ForwardCoordinator {
  public:
    ForwardCoordinator(core::TdClient& client, RequestSession& session, ForwardHooks hooks = {});
    ~ForwardCoordinator();
    ForwardCoordinator(const ForwardCoordinator&) = delete;
    ForwardCoordinator& operator=(const ForwardCoordinator&) = delete;
    ForwardCoordinator(ForwardCoordinator&&) = delete;
    ForwardCoordinator& operator=(ForwardCoordinator&&) = delete;

    ForwardOutcome execute(core::TdForwardMessagesRequest request,
                           const std::shared_ptr<const core::AuthStateSnapshot>& authorization);
    ForwardPreparationOutcome
    prepare(core::TdForwardMessagesRequest request,
            const std::shared_ptr<const core::AuthStateSnapshot>& authorization);
    ForwardOutcome execute_prepared();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::daemon
