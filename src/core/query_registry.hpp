#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace tgcli::core {

// Correlates tdlib request ids with waiting futures: the sender reserves an
// id, the receive loop fulfills it. Thread-safe.
template <typename Response> class QueryRegistry {
  public:
    std::pair<std::uint64_t, std::future<Response>> reserve() {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto id = next_id_++;
        auto [it, inserted] = pending_.emplace(id, std::promise<Response>{});
        return {id, it->second.get_future()};
    }

    // Returns false for an id the registry is not tracking (never reserved,
    // already fulfilled, or failed).
    bool fulfill(std::uint64_t id, Response response) {
        auto promise = take(id);
        if (!promise) {
            return false;
        }
        promise->set_value(std::move(response));
        return true;
    }

    // Detaches a tracked promise so callers can acquire the registry mutex
    // before entering a narrower publication critical section.
    std::optional<std::promise<Response>> take(std::uint64_t id) {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return std::nullopt;
        }
        std::optional<std::promise<Response>> promise(std::in_place, std::move(it->second));
        pending_.erase(it);
        return promise;
    }

    bool fail(std::uint64_t id, std::exception_ptr error) {
        std::promise<Response> promise;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(id);
            if (it == pending_.end()) {
                return false;
            }
            promise = std::move(it->second);
            pending_.erase(it);
        }
        promise.set_exception(std::move(error));
        return true;
    }

    // Breaks every pending future with a std::runtime_error(message); used
    // on shutdown so no caller blocks forever.
    void fail_all(const std::string& message) {
        std::unordered_map<std::uint64_t, std::promise<Response>> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(pending_);
        }
        for (auto& [id, promise] : pending) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
        }
    }

    std::size_t pending_count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, std::promise<Response>> pending_;
};

} // namespace tgcli::core
