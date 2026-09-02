#include "common/cancellation.hpp"
#include "common/shared_publication.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace std::chrono_literals;

namespace {

class EnrollmentBarrierCondition final {
  public:
    template <typename Lock, typename Predicate> void wait(Lock& lock, Predicate predicate) {
        entered_.set_value();
        release_.get_future().wait();
        condition_.wait(lock, std::move(predicate));
    }

    template <typename Lock, typename Clock, typename Duration, typename Predicate>
    bool wait_until(Lock& lock, const std::chrono::time_point<Clock, Duration>& deadline,
                    Predicate predicate) {
        entered_.set_value();
        release_.get_future().wait();
        return condition_.wait_until(lock, deadline, std::move(predicate));
    }

    void notify_all() {
        condition_.notify_all();
    }

    [[nodiscard]] std::future<void> entered() {
        return entered_.get_future();
    }

    void release() {
        release_.set_value();
    }

  private:
    std::condition_variable_any condition_;
    std::promise<void> entered_;
    std::promise<void> release_;
};

} // namespace

TEST_CASE("cancellation registration and stop are linearizable", "[cancellation][concurrency]") {
    for (int iteration = 0; iteration < 256; ++iteration) {
        const tgcli::cancellation::Source source;
        std::atomic<int> calls{0};
        std::atomic<bool> start{false};
        std::unique_ptr<tgcli::cancellation::Callback> callback;
        std::thread registrar([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            callback = std::make_unique<tgcli::cancellation::Callback>(
                source.get_token(), [&] { calls.fetch_add(1, std::memory_order_relaxed); });
        });
        std::thread stopper([&] {
            start.store(true, std::memory_order_release);
            static_cast<void>(source.request_stop());
        });
        registrar.join();
        stopper.join();
        REQUIRE(source.stop_requested());
        CHECK(calls.load(std::memory_order_relaxed) == 1);
        CHECK_FALSE(source.request_stop());
        CHECK(calls.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("cancellation source destruction does not request stop", "[cancellation]") {
    tgcli::cancellation::Token token;
    {
        const tgcli::cancellation::Source source;
        token = source.get_token();
        CHECK_FALSE(token.stop_requested());
    }
    CHECK(token.stop_possible());
    CHECK_FALSE(token.stop_requested());
}

TEST_CASE("cancellation callback registration after stop invokes exactly once", "[cancellation]") {
    const tgcli::cancellation::Source source;
    REQUIRE(source.request_stop());
    int calls = 0;
    {
        const tgcli::cancellation::Callback callback(source.get_token(), [&] { ++calls; });
        CHECK(calls == 1);
    }
    CHECK(calls == 1);
}

TEST_CASE("cancellation callback can destroy its own registration", "[cancellation]") {
    const tgcli::cancellation::Source source;
    std::unique_ptr<tgcli::cancellation::Callback> callback;
    bool invoked = false;
    callback = std::make_unique<tgcli::cancellation::Callback>(source.get_token(), [&] {
        callback.reset();
        invoked = true;
    });
    REQUIRE(source.request_stop());
    CHECK(invoked);
    CHECK_FALSE(callback);
}

TEST_CASE("cancellation callback destruction waits for an in-flight callback",
          "[cancellation][concurrency]") {
    tgcli::cancellation::Source source;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    auto callback = std::make_unique<tgcli::cancellation::Callback>(source.get_token(), [&] {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    });
    std::thread stopper([&] { static_cast<void>(source.request_stop()); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return entered; }));
    }
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        callback.reset();
        destroyed.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(10ms);
    CHECK_FALSE(destroyed.load(std::memory_order_acquire));
    {
        const std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    stopper.join();
    destroyer.join();
    CHECK(destroyed.load(std::memory_order_acquire));
}

TEST_CASE("cancellation wait cannot lose stop before condition enrollment",
          "[cancellation][concurrency]") {
    const tgcli::cancellation::Source source;
    EnrollmentBarrierCondition condition;
    auto entered = condition.entered();
    std::promise<void> completed_promise;
    auto completed = completed_promise.get_future();
    std::mutex mutex;
    bool result = true;
    const bool timed = GENERATE(false, true);
    std::thread waiter([&] {
        std::unique_lock lock(mutex);
        if (timed) {
            result = tgcli::cancellation::wait_until(condition, lock, source.get_token(),
                                                     std::chrono::steady_clock::now() + 10s,
                                                     [] { return false; });
        } else {
            result = tgcli::cancellation::wait(condition, lock, source.get_token(),
                                               [] { return false; });
        }
        completed_promise.set_value();
    });
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    std::thread stopper([&] { static_cast<void>(source.request_stop()); });
    while (!source.stop_requested()) {
        std::this_thread::yield();
    }
    condition.release();
    const bool rescued = completed.wait_for(2s) != std::future_status::ready;
    if (rescued) {
        condition.notify_all();
    }
    stopper.join();
    waiter.join();
    CHECK_FALSE(rescued);
    CHECK_FALSE(result);
}

TEST_CASE("cancellation callback may reenter the stop source", "[cancellation]") {
    const tgcli::cancellation::Source source;
    bool invoked = false;
    const tgcli::cancellation::Callback callback(source.get_token(), [&] {
        invoked = true;
        CHECK_FALSE(source.request_stop());
    });
    REQUIRE(source.request_stop());
    CHECK(invoked);
}

TEST_CASE("cancellation thread teardown requests stop wakes and joins",
          "[cancellation][concurrency]") {
    std::atomic<bool> entered{false};
    std::atomic<bool> exited{false};
    std::mutex mutex;
    std::condition_variable condition;
    {
        const tgcli::cancellation::Thread worker([&](const tgcli::cancellation::Token& token) {
            entered.store(true, std::memory_order_release);
            std::unique_lock lock(mutex);
            static_cast<void>(tgcli::cancellation::wait(condition, lock, token,
                                                        [&] { return token.stop_requested(); }));
            exited.store(true, std::memory_order_release);
        });
        while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    CHECK(exited.load(std::memory_order_acquire));
}

TEST_CASE("cancellation thread move assignment stops and joins the old worker",
          "[cancellation][concurrency]") {
    std::atomic<bool> old_started{false};
    std::atomic<bool> old_exited{false};
    std::atomic<bool> replacement_started{false};
    std::atomic<bool> replacement_exited{false};
    auto worker = [](std::atomic<bool>& started, std::atomic<bool>& exited,
                     const tgcli::cancellation::Token& token) {
        std::mutex mutex;
        std::condition_variable condition;
        started.store(true, std::memory_order_release);
        std::unique_lock lock(mutex);
        static_cast<void>(tgcli::cancellation::wait(condition, lock, token,
                                                    [&] { return token.stop_requested(); }));
        exited.store(true, std::memory_order_release);
    };
    {
        tgcli::cancellation::Thread current(
            [&](const auto& token) { worker(old_started, old_exited, token); });
        tgcli::cancellation::Thread replacement(
            [&](const auto& token) { worker(replacement_started, replacement_exited, token); });
        while (!old_started.load(std::memory_order_acquire) ||
               !replacement_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        current = std::move(replacement);
        CHECK(old_exited.load(std::memory_order_acquire));
        CHECK_FALSE(replacement_exited.load(std::memory_order_acquire));
    }
    CHECK(replacement_exited.load(std::memory_order_acquire));
}

TEST_CASE("shared publication exposes one immutable generation", "[publication][concurrency]") {
    struct Snapshot {
        std::uint64_t generation = 0;
        std::uint64_t complement = 0;
    };
    tgcli::SharedPublication<const Snapshot> publication;
    publication.store(std::make_shared<const Snapshot>(
        Snapshot{.generation = 0, .complement = ~std::uint64_t{0}}));
    std::atomic<bool> done{false};
    std::atomic<bool> malformed{false};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                const auto snapshot = publication.load();
                if (!snapshot || snapshot->complement != ~snapshot->generation) {
                    malformed.store(true, std::memory_order_release);
                }
            }
        });
    }
    for (std::uint64_t generation = 1; generation <= 4096; ++generation) {
        publication.store(std::make_shared<const Snapshot>(
            Snapshot{.generation = generation, .complement = ~generation}));
    }
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }
    CHECK_FALSE(malformed.load(std::memory_order_acquire));
    const auto final = publication.load();
    REQUIRE(final);
    CHECK(final->generation == 4096);
    CHECK(final->complement == ~std::uint64_t{4096});
}

TEST_CASE("shared publication releases the prior owner outside its mutex", "[publication]") {
    struct ReentrantValue {
        ReentrantValue() = default;
        ReentrantValue(tgcli::SharedPublication<const ReentrantValue>* publication_value,
                       bool* observed_replacement_value)
            : publication(publication_value), observed_replacement(observed_replacement_value) {}
        ReentrantValue(const ReentrantValue&) = delete;
        ReentrantValue& operator=(const ReentrantValue&) = delete;
        ReentrantValue(ReentrantValue&&) = delete;
        ReentrantValue& operator=(ReentrantValue&&) = delete;

        tgcli::SharedPublication<const ReentrantValue>* publication = nullptr;
        bool* observed_replacement = nullptr;

        ~ReentrantValue() {
            if (publication != nullptr) {
                *observed_replacement = static_cast<bool>(publication->load());
            }
        }
    };

    tgcli::SharedPublication<const ReentrantValue> publication;
    bool observed_replacement = false;
    auto first = std::make_shared<const ReentrantValue>(&publication, &observed_replacement);
    publication.store(first);
    first.reset();
    publication.store(std::make_shared<const ReentrantValue>());
    CHECK(observed_replacement);
}
