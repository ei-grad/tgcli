#pragma once

#include "daemon/request_observer.hpp"
#include "proto/frame.hpp"
#include "proto/operation.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

class RequestSession;
namespace detail {
class StreamDeliveryRunner;
}

// Safety tiers (DESIGN.md §6), statically declared per command so the gate
// cannot be forgotten. Full gate semantics land with M3; until then the
// dispatcher fails every non-Read command closed.
enum class Tier { Read, Write, Destructive };

using M3Operation = proto::M3Operation;

enum class M3BotPolicy { Allowed, ImmediateOnly, UserOnly };
enum class M3ScheduleKind { None, At, Online };
enum class M3BotAdmission { Allowed, Unsupported };

struct M3OperationPolicy {
    M3Operation operation;
    std::string_view canonical_name;
    std::string_view command_path;
    Tier tier;
    M3BotPolicy bot_policy;
};

std::span<const M3OperationPolicy> m3_operation_policies();
const M3OperationPolicy* m3_operation_policy(M3Operation operation);
std::optional<M3Operation> parse_m3_operation(std::string_view canonical_name);
std::optional<M3Operation> m3_operation_for_command(std::string_view command_path);
M3BotAdmission evaluate_m3_bot_admission(M3Operation operation, bool is_bot,
                                         M3ScheduleKind schedule);

struct ChallengeFailure {
    std::string code;
    std::string message;
    nlohmann::json details = nlohmann::json::object();
    int exit_code = 1;
};

struct ChallengeReply {
    std::optional<nlohmann::json> answer;
    std::optional<ChallengeFailure> failure;
};

enum class DeliveryOutcome { Complete, Suppressed, Disconnected };

// Where a handler emits its response frames. The first terminal call (result
// or error) ends the request; concurrent or later frames are suppressed.
class ResponseSink {
  public:
    ResponseSink() = default;
    ResponseSink(const ResponseSink&) = delete;
    ResponseSink& operator=(const ResponseSink&) = delete;
    ResponseSink(ResponseSink&&) = delete;
    ResponseSink& operator=(ResponseSink&&) = delete;
    virtual ~ResponseSink() = default;

    DeliveryOutcome item(nlohmann::json data);
    void progress(nlohmann::json data);
    DeliveryOutcome result(nlohmann::json data);
    DeliveryOutcome error(std::string code, std::string message, nlohmann::json details,
                          int exit_code);
    std::optional<nlohmann::json> challenge(nlohmann::json data);
    void abort_transport() noexcept;

    [[nodiscard]] bool has_terminal() const;

  protected:
    virtual DeliveryOutcome emit_item(nlohmann::json data) = 0;
    virtual void emit_progress(nlohmann::json data) = 0;
    virtual DeliveryOutcome emit_result(nlohmann::json data) = 0;
    virtual DeliveryOutcome emit_error(std::string code, std::string message,
                                       nlohmann::json details, int exit_code) = 0;
    virtual ChallengeReply emit_challenge(nlohmann::json data) = 0;
    virtual void emit_abort() noexcept {}
    virtual void before_direct_terminal_bit() noexcept {}
    [[nodiscard]] virtual bool claim_public_terminal() {
        return true;
    }
    [[nodiscard]] virtual bool claim_stream_forward_terminal() {
        return true;
    }

  private:
    [[nodiscard]] bool claim_protocol_fallback_terminal();
    DeliveryOutcome forward_stream_result(nlohmann::json data);
    DeliveryOutcome forward_stream_error(std::string code, std::string message,
                                         nlohmann::json details, int exit_code);

    mutable std::mutex mutex_;
    bool terminal_ = false;

    friend class RequestSession;
    friend class detail::StreamDeliveryRunner;
};

// ResponseSink over plain callbacks; used by the in-process --no-daemon mode
// and by contract tests.
class CallbackSink final : public ResponseSink {
  public:
    using DataFn = std::function<void(nlohmann::json)>;
    using ErrorFn = std::function<void(std::string, std::string, nlohmann::json, int)>;
    using ChallengeFn = std::function<std::optional<nlohmann::json>(nlohmann::json)>;

    CallbackSink(DataFn on_item, DataFn on_progress, DataFn on_result, ErrorFn on_error,
                 ChallengeFn on_challenge = {})
        : on_item_(std::move(on_item)), on_progress_(std::move(on_progress)),
          on_result_(std::move(on_result)), on_error_(std::move(on_error)),
          on_challenge_(std::move(on_challenge)) {}

  private:
    DeliveryOutcome emit_item(nlohmann::json data) override {
        on_item_(std::move(data));
        return DeliveryOutcome::Complete;
    }
    void emit_progress(nlohmann::json data) override {
        on_progress_(std::move(data));
    }
    DeliveryOutcome emit_result(nlohmann::json data) override {
        on_result_(std::move(data));
        return DeliveryOutcome::Complete;
    }
    DeliveryOutcome emit_error(std::string code, std::string message, nlohmann::json details,
                               int exit_code) override {
        on_error_(std::move(code), std::move(message), std::move(details), exit_code);
        return DeliveryOutcome::Complete;
    }
    ChallengeReply emit_challenge(nlohmann::json data) override {
        if (!on_challenge_) {
            return {};
        }
        return {on_challenge_(std::move(data)), std::nullopt};
    }

    DataFn on_item_;
    DataFn on_progress_;
    DataFn on_result_;
    ErrorFn on_error_;
    ChallengeFn on_challenge_;
};

struct CommandDescriptor {
    Tier tier = Tier::Read;
    std::function<void(const proto::Request&, RequestSession&)> handler;
    bool m1_destructive_kernel = false;
    std::optional<M3Operation> m3_operation = std::nullopt;
    DeadlineDefault deadline_default = DeadlineDefault::Default60;
};

// The daemon-side dispatch table and the single safety chokepoint (DESIGN.md
// §7): every request — socket or --no-daemon — passes through dispatch().
class Dispatcher {
  public:
    explicit Dispatcher(testing::RequestObservationObserver request_observer = {},
                        testing::RequestWallClock request_wall_clock = {})
        : request_observer_(std::move(request_observer)),
          request_wall_clock_(std::move(request_wall_clock)) {}

    void register_command(const std::string& path, CommandDescriptor descriptor);
    void set_request_preflight(
        std::function<bool(const std::string&, RequestSession&)> request_preflight);
    [[nodiscard]] DeadlineDefault deadline_default(const proto::Request& request) const;
    [[nodiscard]] bool requires_frozen_config_admission(const proto::Request& request) const;
    void dispatch(RequestSession& session) const;
    void dispatch(const proto::Request& request, ResponseSink& sink) const;

  private:
    std::map<std::string, CommandDescriptor> commands_;
    std::function<bool(const std::string&, RequestSession&)> request_preflight_;
    testing::RequestObservationObserver request_observer_;
    testing::RequestWallClock request_wall_clock_;
};

} // namespace tgcli::daemon
