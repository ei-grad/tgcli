#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <memory>
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
    struct Detached {
        std::promise<Response> promise;
        std::shared_ptr<const void> lifetime;
    };

    std::pair<std::uint64_t, std::future<Response>>
    reserve(const std::shared_ptr<const void>& lifetime = {}) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto id = next_id_++;
        auto [it, inserted] = pending_.emplace(id, Entry{.promise = {}, .lifetime = lifetime});
        static_cast<void>(inserted);
        return {id, it->second.promise.get_future()};
    }

    // Returns false for an id the registry is not tracking (never reserved,
    // already fulfilled, or failed).
    bool fulfill(std::uint64_t id, Response response) {
        auto detached = take(id);
        if (!detached) {
            return false;
        }
        detached->promise.set_value(std::move(response));
        return true;
    }

    // Detaches promise and lifetime so callers can enter a narrower
    // publication critical section without running lifetime callbacks under
    // this registry mutex.
    std::optional<Detached> take(std::uint64_t id) {
        std::optional<Detached> detached;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(id);
            if (it == pending_.end()) {
                return std::nullopt;
            }
            detached.emplace(Detached{.promise = std::move(it->second.promise),
                                      .lifetime = std::move(it->second.lifetime)});
            pending_.erase(it);
        }
        return detached;
    }

    bool fail(std::uint64_t id, std::exception_ptr error) {
        auto detached = take(id);
        if (!detached) {
            return false;
        }
        detached->promise.set_exception(std::move(error));
        return true;
    }

    // Breaks every pending future with a std::runtime_error(message); used
    // on shutdown so no caller blocks forever.
    void fail_all(const std::string& message) {
        std::unordered_map<std::uint64_t, Entry> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(pending_);
        }
        for (auto& [id, entry] : pending) {
            static_cast<void>(id);
            entry.promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
        }
    }

    std::size_t pending_count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

  private:
    using Entry = Detached;

    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Entry> pending_;
};

} // namespace tgcli::core
