#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace tgcli::core::detail {

template <typename Response> class RequestLifecycle {
  public:
    explicit RequestLifecycle(std::string closed_message)
        : closed_message_(std::move(closed_message)) {}

    template <typename Send> std::future<Response> send(Send&& send) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            std::promise<Response> promise;
            auto future = promise.get_future();
            promise.set_exception(std::make_exception_ptr(std::runtime_error(closed_message_)));
            return future;
        }
        return std::invoke(std::forward<Send>(send));
    }

    template <typename BeginClose> bool begin_close(BeginClose&& begin_close) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return false;
        }
        closing_ = true;
        std::invoke(std::forward<BeginClose>(begin_close));
        return true;
    }

  private:
    std::mutex mutex_;
    bool closing_ = false;
    std::string closed_message_;
};

} // namespace tgcli::core::detail
