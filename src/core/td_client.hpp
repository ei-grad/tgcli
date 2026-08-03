#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace tgcli::core {

// Move-only type erasure keeps generated TDLib types out of project headers.
// Daemon implementation translation units that use typed TDLib requests box
// the native pointer and unbox the response; client-side code never sees it.
class TdValue {
  public:
    TdValue() = default;
    TdValue(const TdValue&) = delete;
    TdValue& operator=(const TdValue&) = delete;
    TdValue(TdValue&&) noexcept = default;
    TdValue& operator=(TdValue&&) noexcept = default;
    ~TdValue() = default;

    template <typename T> static TdValue from(T value) {
        TdValue result;
        result.value_ = std::make_unique<Holder<T>>(std::move(value));
        return result;
    }

    template <typename T> [[nodiscard]] T* get_if() {
        auto* holder = dynamic_cast<Holder<T>*>(value_.get());
        return holder == nullptr ? nullptr : &holder->value;
    }

    template <typename T> [[nodiscard]] const T* get_if() const {
        const auto* holder = dynamic_cast<const Holder<T>*>(value_.get());
        return holder == nullptr ? nullptr : &holder->value;
    }

    [[nodiscard]] bool has_value() const {
        return value_ != nullptr;
    }

  private:
    struct ValueBase {
        ValueBase() = default;
        ValueBase(const ValueBase&) = delete;
        ValueBase& operator=(const ValueBase&) = delete;
        ValueBase(ValueBase&&) = delete;
        ValueBase& operator=(ValueBase&&) = delete;
        virtual ~ValueBase() = default;
    };

    template <typename T> struct Holder final : ValueBase {
        explicit Holder(T stored) : value(std::move(stored)) {}
        T value;
    };

    std::unique_ptr<ValueBase> value_;
};

// Owns tdlib's ClientManager and its receive loop on a dedicated thread
// (DESIGN.md §7). tdlib object lifecycle and receive-loop rules live
// entirely here; command handlers construct typed requests but never touch
// the ClientManager or the update loop directly.
class TdClient {
  public:
    using UpdateHandler = std::function<void(const TdValue&)>;

    TdClient();
    ~TdClient();
    TdClient(const TdClient&) = delete;
    TdClient& operator=(const TdClient&) = delete;
    TdClient(TdClient&&) = delete;
    TdClient& operator=(TdClient&&) = delete;

    // Thread-safe. Native request/response objects stay inside TdValue so
    // generated TDLib types remain daemon-implementation details. Once
    // close begins, a valid request returns a ready future that throws
    // std::runtime_error instead of entering the request registry.
    std::future<TdValue> send(TdValue request);

    // Handlers run on the receive thread under the bus lock: fast, no tdlib
    // calls, no (un)subscribe from within a handler (see UpdateBus).
    std::uint64_t subscribe_updates(UpdateHandler handler);
    void unsubscribe_updates(std::uint64_t id);

    // Graceful shutdown (DESIGN.md §10): asks tdlib to close, waits for
    // authorizationStateClosed so the database is flushed, then stops the
    // receive thread and breaks any still-pending futures. Idempotent.
    void close();

    // tdlib version string, synchronously and without a client.
    static std::string tdlib_version();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::core
