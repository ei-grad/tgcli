#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace tgcli {

template <typename Value> class SharedPublication final {
  public:
    SharedPublication() = default;
    ~SharedPublication() = default;
    SharedPublication(const SharedPublication&) = delete;
    SharedPublication& operator=(const SharedPublication&) = delete;
    SharedPublication(SharedPublication&&) = delete;
    SharedPublication& operator=(SharedPublication&&) = delete;

    void store(std::shared_ptr<Value> value) {
        std::shared_ptr<Value> previous;
        {
            const std::lock_guard lock(mutex_);
            previous = std::exchange(value_, std::move(value));
        }
    }

    [[nodiscard]] std::shared_ptr<Value> load() const {
        const std::lock_guard lock(mutex_);
        return value_;
    }

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<Value> value_;
};

} // namespace tgcli
