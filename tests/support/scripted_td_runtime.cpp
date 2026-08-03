#include "support/scripted_td_runtime.hpp"

#include <stdexcept>
#include <utility>

namespace tgcli::test {

ScriptedTdRuntime::ScriptedTdRuntime(bool close_automatically)
    : close_automatically_(close_automatically) {}

void ScriptedTdRuntime::initialize_process() {
    const std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
}

std::int32_t ScriptedTdRuntime::create_client(std::uint64_t client_generation) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        throw std::logic_error("TDLib process initialization must precede client creation");
    }
    if (clients_.empty()) {
        initialized_before_first_client_ = true;
    }
    const auto client_id = static_cast<std::int32_t>(1000 + client_generation);
    clients_.push_back({.client_id = client_id, .client_generation = client_generation});
    cv_.notify_all();
    return client_id;
}

core::TdValue ScriptedTdRuntime::make_function(core::TdBuiltinFunction function) {
    switch (function) {
    case core::TdBuiltinFunction::GetAuthorizationState:
        return core::TdValue::scripted_function(core::TdFunctionData{"getAuthorizationState"});
    case core::TdBuiltinFunction::Close:
        return core::TdValue::scripted_function(core::TdFunctionData{"close"});
    }
    throw std::logic_error("unknown built-in TDLib function");
}

void ScriptedTdRuntime::send(std::int32_t client_id, std::uint64_t client_generation,
                             std::uint64_t query_id, core::TdValue function) {
    if (!function.function_data().has_value()) {
        throw std::invalid_argument("scripted TDLib function lacks neutral data");
    }

    const auto function_data = *function.function_data();
    bool close_automatically = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back({.client_id = client_id,
                         .client_generation = client_generation,
                         .query_id = query_id,
                         .function = function_data});
        close_automatically = close_automatically_ && function_data.has_type("close");
        cv_.notify_all();
    }
    if (close_automatically) {
        push_update({.client_id = client_id, .client_generation = client_generation}, {},
                    core::AuthStateData{core::AuthState::Closed});
    }
}

std::optional<core::TdRuntimeEvent> ScriptedTdRuntime::receive(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return !events_.empty(); })) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void ScriptedTdRuntime::push_response(ScriptedClient client, std::uint64_t query_id,
                                      core::TdValue object,
                                      std::optional<core::AuthStateData> authorization_state) {
    push_event({.client_id = client.client_id,
                .client_generation = client.client_generation,
                .query_id = query_id,
                .object = std::move(object),
                .authorization_state = std::move(authorization_state)});
}

void ScriptedTdRuntime::push_update(ScriptedClient client, core::TdValue object,
                                    std::optional<core::AuthStateData> authorization_state) {
    push_event({.client_id = client.client_id,
                .client_generation = client.client_generation,
                .query_id = 0,
                .object = std::move(object),
                .authorization_state = std::move(authorization_state)});
}

bool ScriptedTdRuntime::wait_for_sent(std::size_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] { return sent_.size() >= count; });
}

bool ScriptedTdRuntime::wait_for_clients(std::size_t count,
                                         std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] { return clients_.size() >= count; });
}

std::vector<SentTdFunction> ScriptedTdRuntime::sent_functions() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
}

std::vector<ScriptedClient> ScriptedTdRuntime::clients() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return clients_;
}

bool ScriptedTdRuntime::initialized_before_first_client() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return initialized_before_first_client_;
}

void ScriptedTdRuntime::push_event(core::TdRuntimeEvent event) {
    const std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
    cv_.notify_all();
}

} // namespace tgcli::test
