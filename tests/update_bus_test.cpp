#include "core/update_bus.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using tgcli::core::UpdateBus;

TEST_CASE("subscribers receive published updates in subscription order", "[core]") {
    UpdateBus<std::string> bus;
    std::vector<std::string> seen;
    bus.subscribe([&seen](const std::string& u) { seen.push_back("a:" + u); });
    bus.subscribe([&seen](const std::string& u) { seen.push_back("b:" + u); });

    bus.publish("x");
    CHECK(seen == std::vector<std::string>{"a:x", "b:x"});
}

TEST_CASE("unsubscribe stops delivery; other subscribers are unaffected", "[core]") {
    UpdateBus<int> bus;
    int a_count = 0;
    int b_count = 0;
    const auto a = bus.subscribe([&a_count](const int&) { ++a_count; });
    bus.subscribe([&b_count](const int&) { ++b_count; });

    bus.publish(1);
    bus.unsubscribe(a);
    bus.publish(2);

    CHECK(a_count == 1);
    CHECK(b_count == 2);
    CHECK(bus.subscriber_count() == 1);
}

TEST_CASE("unsubscribe of an unknown id is a no-op", "[core]") {
    UpdateBus<int> bus;
    bus.subscribe([](const int&) {});
    bus.unsubscribe(12345);
    CHECK(bus.subscriber_count() == 1);
}

TEST_CASE("publish from another thread is safe against subscribe churn", "[core]") {
    UpdateBus<int> bus;
    std::atomic<int> delivered{0};
    std::atomic<bool> stop{false};

    std::thread publisher([&bus, &stop] {
        while (!stop.load()) {
            bus.publish(1);
        }
    });
    for (int i = 0; i < 100; ++i) {
        const auto id = bus.subscribe([&delivered](const int&) { ++delivered; });
        bus.unsubscribe(id);
    }
    stop.store(true);
    publisher.join();
    CHECK(bus.subscriber_count() == 0);
}
