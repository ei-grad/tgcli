#pragma once

#include "common/deadline.hpp"
#include "core/td_client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <variant>

namespace tgcli::daemon {

class RequestSession;

enum class SingleSendMutationState { None, Possible, Confirmed };

struct SingleSendTemporaryId {
    std::uint64_t client_generation = 0;
    std::int64_t chat_id = 0;
    std::int64_t temporary_message_id = 0;
    std::int32_t sending_id = 0;

    bool operator==(const SingleSendTemporaryId&) const = default;
};

struct SingleSendSucceeded {
    std::optional<SingleSendTemporaryId> temporary;
    core::TdWriteMessage authoritative_message;
    core::TdMessageWriteResult result;
    SingleSendMutationState mutation_state = SingleSendMutationState::Confirmed;

    bool operator==(const SingleSendSucceeded&) const = default;
};

struct SingleSendFailed {
    std::optional<SingleSendTemporaryId> temporary;
    core::TdError error;
    SingleSendMutationState mutation_state = SingleSendMutationState::None;

    bool operator==(const SingleSendFailed&) const = default;
};

struct SingleSendRateLimited {
    std::optional<SingleSendTemporaryId> temporary;
    core::TdError error;
    std::int32_t retry_after = 0;
    SingleSendMutationState mutation_state = SingleSendMutationState::None;

    bool operator==(const SingleSendRateLimited&) const = default;
};

struct SingleSendDeletedBeforeConfirmation {
    SingleSendTemporaryId temporary;
    SingleSendMutationState mutation_state = SingleSendMutationState::Possible;

    bool operator==(const SingleSendDeletedBeforeConfirmation&) const = default;
};

struct SingleSendTimedOut {
    std::optional<SingleSendTemporaryId> temporary;
    SingleSendMutationState mutation_state = SingleSendMutationState::Possible;

    bool operator==(const SingleSendTimedOut&) const = default;
};

struct SingleSendAuthorizationLost {
    std::optional<SingleSendTemporaryId> temporary;
    std::uint64_t client_generation = 0;
    std::uint64_t auth_sequence = 0;
    core::AuthState state = core::AuthState::Unknown;
    SingleSendMutationState mutation_state = SingleSendMutationState::Possible;

    bool operator==(const SingleSendAuthorizationLost&) const = default;
};

struct SingleSendGenerationClosed {
    std::optional<SingleSendTemporaryId> temporary;
    std::uint64_t client_generation = 0;
    SingleSendMutationState mutation_state = SingleSendMutationState::Possible;

    bool operator==(const SingleSendGenerationClosed&) const = default;
};

struct SingleSendCancelled {
    std::optional<SingleSendTemporaryId> temporary;
    SingleSendMutationState mutation_state = SingleSendMutationState::Possible;

    bool operator==(const SingleSendCancelled&) const = default;
};

struct SingleSendRejected {
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    SingleSendMutationState mutation_state = SingleSendMutationState::None;

    bool operator==(const SingleSendRejected&) const = default;
};

struct SingleSendMalformed {
    std::optional<SingleSendTemporaryId> temporary;
    std::optional<std::int32_t> tdlib_type_id;
    SingleSendMutationState mutation_state = SingleSendMutationState::Possible;

    bool operator==(const SingleSendMalformed&) const = default;
};

using SingleSendOutcome =
    std::variant<SingleSendSucceeded, SingleSendFailed, SingleSendRateLimited,
                 SingleSendDeletedBeforeConfirmation, SingleSendTimedOut,
                 SingleSendAuthorizationLost, SingleSendGenerationClosed, SingleSendCancelled,
                 SingleSendRejected, SingleSendMalformed>;

struct SingleSendPrepared {};
using SingleSendPreparationOutcome =
    std::variant<SingleSendPrepared, SingleSendAuthorizationLost, SingleSendGenerationClosed,
                 SingleSendTimedOut, SingleSendCancelled, SingleSendRejected>;

struct SingleSendHooks {
    std::function<core::TdEventClock::time_point()> now;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait;
    std::function<void()> before_request;
    std::function<void()> before_submit;
    std::function<void()> before_event_arbitration;
    std::function<void()> before_wait;
    std::function<void(const SingleSendTemporaryId&)> on_temporary_id;
};

class SingleSendCoordinator {
  public:
    SingleSendCoordinator(core::TdClient& client, RequestSession& session,
                          SingleSendHooks hooks = {});
    ~SingleSendCoordinator();
    SingleSendCoordinator(const SingleSendCoordinator&) = delete;
    SingleSendCoordinator& operator=(const SingleSendCoordinator&) = delete;
    SingleSendCoordinator(SingleSendCoordinator&&) = delete;
    SingleSendCoordinator& operator=(SingleSendCoordinator&&) = delete;

    SingleSendOutcome execute(core::TdSendMessageRequest request,
                              const std::shared_ptr<const core::AuthStateSnapshot>& authorization);
    SingleSendPreparationOutcome
    prepare(core::TdSendMessageRequest request,
            const std::shared_ptr<const core::AuthStateSnapshot>& authorization);
    SingleSendOutcome execute_prepared();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::daemon
