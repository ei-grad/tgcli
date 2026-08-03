#include "core/td_client.hpp"

#include "core/query_registry.hpp"
#include "core/request_lifecycle.hpp"
#include "core/update_bus.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

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

class TdClient::Impl {
  public:
    using NativeObjectPtr = td_api::object_ptr<td_api::Object>;
    using NativeFunctionPtr = td_api::object_ptr<td_api::Function>;

    Impl() : client_id_(create_client_quietly(manager_)) {
        receive_thread_ = std::thread([this] { receive_loop(); });
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        close();
    }

    std::future<TdValue> send(TdValue request) {
        auto* native_request = request.get_if<NativeFunctionPtr>();
        if (native_request == nullptr || *native_request == nullptr) {
            throw std::invalid_argument("TdClient request does not contain a native function");
        }
        return lifecycle_.send([this, native_request] {
            auto [id, future] = queries_.reserve();
            manager_.send(client_id_, id, std::move(*native_request));
            return std::move(future);
        });
    }

    std::uint64_t subscribe_updates(UpdateHandler handler) {
        return updates_.subscribe(std::move(handler));
    }

    void unsubscribe_updates(std::uint64_t id) {
        updates_.unsubscribe(id);
    }

    void close() {
        std::call_once(close_once_, [this] {
            // The lifecycle gate orders every accepted send before the close
            // request and rejects every later send without reserving a query.
            static_cast<void>(lifecycle_.begin_close([this] {
                auto [id, future] = queries_.reserve();
                static_cast<void>(future);
                manager_.send(client_id_, id, td_api::make_object<td_api::close>());
            }));
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

  private:
    void receive_loop() {
        while (!stop_.load(std::memory_order_acquire)) {
            auto response = manager_.receive(kReceiveTimeoutSeconds);
            if (response.object == nullptr) {
                continue;
            }
            if (response.request_id == 0) {
                handle_update(std::move(response.object));
                continue;
            }
            queries_.fulfill(response.request_id, TdValue::from(std::move(response.object)));
        }
    }

    void handle_update(NativeObjectPtr update) {
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
        updates_.publish(TdValue::from(static_cast<const td_api::Object*>(update.get())));
    }

    td::ClientManager manager_;
    std::int32_t client_id_ = 0;
    QueryRegistry<TdValue> queries_;
    detail::RequestLifecycle<TdValue> lifecycle_{"tdlib client closed"};
    UpdateBus<TdValue> updates_;
    std::atomic<bool> stop_{false};
    std::mutex closed_mutex_;
    std::condition_variable closed_cv_;
    bool closed_ = false;
    std::once_flag close_once_;
    std::thread receive_thread_;
};

TdClient::TdClient() : impl_(std::make_unique<Impl>()) {}

TdClient::~TdClient() = default;

std::future<TdValue> TdClient::send(TdValue request) {
    return impl_->send(std::move(request));
}

std::uint64_t TdClient::subscribe_updates(UpdateHandler handler) {
    return impl_->subscribe_updates(std::move(handler));
}

void TdClient::unsubscribe_updates(std::uint64_t id) {
    impl_->unsubscribe_updates(id);
}

void TdClient::close() {
    impl_->close();
}

std::string TdClient::tdlib_version() {
    auto value = td::ClientManager::execute(td_api::make_object<td_api::getOption>("version"));
    if (value != nullptr && value->get_id() == td_api::optionValueString::ID) {
        return static_cast<td_api::optionValueString&>(*value).value_;
    }
    return "unknown";
}

} // namespace tgcli::core
