#pragma once

#include "proto/frame.hpp"

#include <functional>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

// Safety tiers (DESIGN.md §6), statically declared per command so the gate
// cannot be forgotten. Full gate semantics land with M3; until then the
// dispatcher fails every non-Read command closed.
enum class Tier { Read, Write, Destructive };

// Where a handler emits its response frames. A terminal call (result or
// error) ends the request; item/progress may precede it.
class ResponseSink {
  public:
    ResponseSink() = default;
    ResponseSink(const ResponseSink&) = delete;
    ResponseSink& operator=(const ResponseSink&) = delete;
    ResponseSink(ResponseSink&&) = delete;
    ResponseSink& operator=(ResponseSink&&) = delete;
    virtual ~ResponseSink() = default;

    virtual void item(nlohmann::json data) = 0;
    virtual void progress(nlohmann::json data) = 0;
    virtual void result(nlohmann::json data) = 0;
    virtual void error(std::string code, std::string message, nlohmann::json details,
                       int exit_code) = 0;
};

// ResponseSink over plain callbacks; used by the in-process --no-daemon mode
// and by contract tests.
class CallbackSink final : public ResponseSink {
  public:
    using DataFn = std::function<void(nlohmann::json)>;
    using ErrorFn = std::function<void(std::string, std::string, nlohmann::json, int)>;

    CallbackSink(DataFn on_item, DataFn on_progress, DataFn on_result, ErrorFn on_error)
        : on_item_(std::move(on_item)), on_progress_(std::move(on_progress)),
          on_result_(std::move(on_result)), on_error_(std::move(on_error)) {}

    void item(nlohmann::json data) override {
        on_item_(std::move(data));
    }
    void progress(nlohmann::json data) override {
        on_progress_(std::move(data));
    }
    void result(nlohmann::json data) override {
        on_result_(std::move(data));
    }
    void error(std::string code, std::string message, nlohmann::json details,
               int exit_code) override {
        on_error_(std::move(code), std::move(message), std::move(details), exit_code);
    }

  private:
    DataFn on_item_;
    DataFn on_progress_;
    DataFn on_result_;
    ErrorFn on_error_;
};

struct CommandDescriptor {
    Tier tier = Tier::Read;
    std::function<void(const proto::Request&, ResponseSink&)> handler;
};

// The daemon-side dispatch table and the single safety chokepoint (DESIGN.md
// §7): every request — socket or --no-daemon — passes through dispatch().
class Dispatcher {
  public:
    void register_command(const std::string& path, CommandDescriptor descriptor);
    void dispatch(const proto::Request& request, ResponseSink& sink) const;

  private:
    std::map<std::string, CommandDescriptor> commands_;
};

} // namespace tgcli::daemon
