#include "core/td_client.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace tgcli::core {

namespace td_api = td::td_api;

namespace {

// A client that never reaches authorizationStateClosed would hang shutdown
// forever; after this long we abandon the clean-close guarantee and say so.
constexpr auto kCloseTimeout = std::chrono::seconds(30);

constexpr double kReceiveTimeoutSeconds = 0.5;

// Quiets tdlib's default stderr chatter (level 1 keeps errors only) and
// only then allocates the client id — tdlib logs client creation at its
// default verbosity. Log-file routing arrives with config (M1).
std::int32_t create_client_quietly(td::ClientManager& manager) {
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(1));
    return manager.create_client_id();
}

} // namespace

TdClient::TdClient() : client_id_(create_client_quietly(manager_)) {
    receive_thread_ = std::thread([this] { receive_loop(); });
}

TdClient::~TdClient() {
    close();
}

std::future<TdClient::ObjectPtr> TdClient::send(td_api::object_ptr<td_api::Function> request) {
    auto [id, future] = queries_.reserve();
    manager_.send(client_id_, id, std::move(request));
    return std::move(future);
}

std::uint64_t TdClient::subscribe_updates(UpdateHandler handler) {
    return updates_.subscribe(std::move(handler));
}

void TdClient::unsubscribe_updates(std::uint64_t id) {
    updates_.unsubscribe(id);
}

void TdClient::close() {
    std::call_once(close_once_, [this] {
        // tdlib itself answers every in-flight request with an error once
        // closing starts, so pending futures resolve through the normal
        // receive path; fail_all below only covers requests that raced in
        // after the loop stopped.
        auto [id, future] = queries_.reserve();
        manager_.send(client_id_, id, td_api::make_object<td_api::close>());
        {
            std::unique_lock<std::mutex> lock(closed_mutex_);
            if (!closed_cv_.wait_for(lock, kCloseTimeout, [this] { return closed_; })) {
                std::fputs("warning: tdlib did not reach authorizationStateClosed within 30s; "
                           "shutting down without the clean-close guarantee\n",
                           stderr);
            }
        }
        stop_.store(true, std::memory_order_release);
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        queries_.fail_all("tdlib client closed");
    });
}

std::string TdClient::tdlib_version() {
    auto value = td::ClientManager::execute(td_api::make_object<td_api::getOption>("version"));
    if (value != nullptr && value->get_id() == td_api::optionValueString::ID) {
        return static_cast<td_api::optionValueString&>(*value).value_;
    }
    return "unknown";
}

void TdClient::receive_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        auto response = manager_.receive(kReceiveTimeoutSeconds);
        if (response.object == nullptr) {
            continue;
        }
        if (response.request_id == 0) {
            handle_update(std::move(response.object));
            continue;
        }
        queries_.fulfill(response.request_id, std::move(response.object));
    }
}

void TdClient::handle_update(ObjectPtr update) {
    if (update->get_id() == td_api::updateAuthorizationState::ID) {
        const auto& state =
            *static_cast<td_api::updateAuthorizationState&>(*update).authorization_state_;
        if (state.get_id() == td_api::authorizationStateClosed::ID) {
            {
                const std::lock_guard<std::mutex> lock(closed_mutex_);
                closed_ = true;
            }
            closed_cv_.notify_all();
        }
    }
    updates_.publish(*update);
}

} // namespace tgcli::core
