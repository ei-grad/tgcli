#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace tgcli::core {

// Fans tdlib updates out to daemon-side subscribers (`listen`/`wait-for`
// streams, auth FSM, file progress). publish() runs on the tdlib receive
// thread and holds the bus lock while invoking handlers, which gives the
// strong guarantee that after unsubscribe() returns the handler is not
// running and never will be. The flip side: handlers must be fast, must not
// block on tdlib queries, and must not subscribe/unsubscribe from within a
// handler (self-deadlock).
template <typename Update> class UpdateBus {
  public:
    using Handler = std::function<void(const Update&)>;

    std::uint64_t subscribe(Handler handler) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto id = next_id_++;
        handlers_.emplace_back(id, std::move(handler));
        return id;
    }

    void unsubscribe(std::uint64_t id) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(handlers_, [id](const auto& entry) { return entry.first == id; });
    }

    void publish(const Update& update) {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, handler] : handlers_) {
            handler(update);
        }
    }

    std::size_t subscriber_count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return handlers_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1;
    std::vector<std::pair<std::uint64_t, Handler>> handlers_;
};

} // namespace tgcli::core
