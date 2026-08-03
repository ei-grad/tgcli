#include "core/query_registry.hpp"
#include "core/request_lifecycle.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using tgcli::core::QueryRegistry;
using tgcli::core::detail::RequestLifecycle;

// Move-only payload mirrors td_api::object_ptr.
using Payload = std::unique_ptr<int>;

TEST_CASE("reserve hands out distinct ids and fulfill resolves the future", "[core]") {
    QueryRegistry<Payload> registry;
    auto [id1, future1] = registry.reserve();
    auto [id2, future2] = registry.reserve();
    CHECK(id1 != id2);
    CHECK(registry.pending_count() == 2);

    CHECK(registry.fulfill(id2, std::make_unique<int>(22)));
    CHECK(*future2.get() == 22);
    CHECK(registry.fulfill(id1, std::make_unique<int>(11)));
    CHECK(*future1.get() == 11);
    CHECK(registry.pending_count() == 0);
}

TEST_CASE("fulfill of an unknown or already-fulfilled id reports false", "[core]") {
    QueryRegistry<Payload> registry;
    CHECK_FALSE(registry.fulfill(999, std::make_unique<int>(0)));

    auto [id, future] = registry.reserve();
    REQUIRE(registry.fulfill(id, std::make_unique<int>(1)));
    CHECK_FALSE(registry.fulfill(id, std::make_unique<int>(2)));
}

TEST_CASE("fail_all breaks every pending future", "[core]") {
    QueryRegistry<Payload> registry;
    auto [id1, future1] = registry.reserve();
    auto [id2, future2] = registry.reserve();
    registry.fail_all("closing");

    CHECK_THROWS_AS(future1.get(), std::runtime_error);
    CHECK_THROWS_AS(future2.get(), std::runtime_error);
    CHECK(registry.pending_count() == 0);
    CHECK_FALSE(registry.fulfill(id1, std::make_unique<int>(0)));
    // The registry stays usable after fail_all.
    auto [id3, future3] = registry.reserve();
    CHECK(registry.fulfill(id3, std::make_unique<int>(3)));
    CHECK(*future3.get() == 3);
    (void)id2;
}

TEST_CASE("fail resolves one pending future exactly once", "[core][lifecycle]") {
    QueryRegistry<Payload> registry;
    auto [id, future] = registry.reserve();

    CHECK(registry.fail(id, std::make_exception_ptr(std::runtime_error("send failed"))));
    CHECK_THROWS_AS(future.get(), std::runtime_error);
    CHECK_FALSE(registry.fail(id, std::make_exception_ptr(std::runtime_error("again"))));
    CHECK_FALSE(registry.fulfill(id, std::make_unique<int>(1)));
    CHECK(registry.pending_count() == 0);
}

TEST_CASE("concurrent reserve and fulfill stay consistent", "[core]") {
    QueryRegistry<Payload> registry;
    constexpr int kPerThread = 200;
    constexpr int kThreads = 4;

    // Catch2 assertion macros are not thread-safe: workers only count, the
    // main thread asserts after join.
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&registry, &ok] {
            for (int i = 0; i < kPerThread; ++i) {
                auto [id, future] = registry.reserve();
                if (registry.fulfill(id, std::make_unique<int>(static_cast<int>(id))) &&
                    *future.get() == static_cast<int>(id)) {
                    ++ok;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(ok == kThreads * kPerThread);
    CHECK(registry.pending_count() == 0);
}

TEST_CASE("send and close race leaves no unresolved request", "[core][lifecycle]") {
    for (int iteration = 0; iteration < 128; ++iteration) {
        QueryRegistry<Payload> registry;
        RequestLifecycle<Payload> lifecycle("closed");
        std::future<Payload> response;
        std::barrier start(3);

        std::thread sender([&] {
            start.arrive_and_wait();
            response = lifecycle.send([&registry] {
                auto [id, future] = registry.reserve();
                static_cast<void>(id);
                return std::move(future);
            });
        });
        std::thread closer([&] {
            start.arrive_and_wait();
            lifecycle.begin_close([&registry] { registry.fail_all("closed"); });
        });
        start.arrive_and_wait();
        sender.join();
        closer.join();

        REQUIRE(response.valid());
        REQUIRE(response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        CHECK_THROWS_AS(response.get(), std::runtime_error);
        CHECK(registry.pending_count() == 0);
    }
}
