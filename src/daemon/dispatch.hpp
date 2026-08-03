#pragma once

#include "proto/frame.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

class RequestSession;

// Safety tiers (DESIGN.md §6), statically declared per command so the gate
// cannot be forgotten. Full gate semantics land with M3; until then the
// dispatcher fails every non-Read command closed.
enum class Tier { Read, Write, Destructive };

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

    void item(nlohmann::json data);
    void progress(nlohmann::json data);
    void result(nlohmann::json data);
    void error(std::string code, std::string message, nlohmann::json details, int exit_code);
    std::optional<nlohmann::json> challenge(nlohmann::json data);

    [[nodiscard]] bool has_terminal() const;

  protected:
    virtual void emit_item(nlohmann::json data) = 0;
    virtual void emit_progress(nlohmann::json data) = 0;
    virtual void emit_result(nlohmann::json data) = 0;
    virtual void emit_error(std::string code, std::string message, nlohmann::json details,
                            int exit_code) = 0;
    virtual ChallengeReply emit_challenge(nlohmann::json data) = 0;

  private:
    mutable std::mutex mutex_;
    bool terminal_ = false;
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
    void emit_item(nlohmann::json data) override {
        on_item_(std::move(data));
    }
    void emit_progress(nlohmann::json data) override {
        on_progress_(std::move(data));
    }
    void emit_result(nlohmann::json data) override {
        on_result_(std::move(data));
    }
    void emit_error(std::string code, std::string message, nlohmann::json details,
                    int exit_code) override {
        on_error_(std::move(code), std::move(message), std::move(details), exit_code);
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
};

// The daemon-side dispatch table and the single safety chokepoint (DESIGN.md
// §7): every request — socket or --no-daemon — passes through dispatch().
class Dispatcher {
  public:
    void register_command(const std::string& path, CommandDescriptor descriptor);
    void dispatch(RequestSession& session) const;
    void dispatch(const proto::Request& request, ResponseSink& sink) const;

  private:
    std::map<std::string, CommandDescriptor> commands_;
};

} // namespace tgcli::daemon
