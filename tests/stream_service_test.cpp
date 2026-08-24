#include "core/td_client.hpp"
#include "daemon/stream_service.hpp"
#include "support/scripted_td_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using namespace tgcli;

bool eventually(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

core::TdValue current_state() {
    return core::TdValue::from(core::TdCurrentState{});
}

} // namespace

TEST_CASE("stream service factory begins each generation before current-state dispatch",
          "[stream][service][bootstrap][fake-boundary]") {
    daemon::StreamService service;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    std::atomic<bool> bootstrap_visible{false};
    std::atomic<bool> submission_during_callback{false};
    scripted->set_before_send([&](const core::TdFunctionData& function) {
        submission_during_callback.store(daemon::detail::stream_callback_active(),
                                         std::memory_order_release);
        if (function.kind() == core::TdFunctionKind::GetCurrentState) {
            const auto status = service.status();
            bootstrap_visible.store(status.phase == daemon::StreamNormalizationPhase::Bootstrap &&
                                        status.client_id == 1001 && status.generation == 1,
                                    std::memory_order_release);
        }
    });
    core::TdClient client(std::move(runtime), {}, {}, service.observer_factory());
    REQUIRE(scripted->wait_for_sent(2));
    CHECK(bootstrap_visible.load(std::memory_order_acquire));
    CHECK_FALSE(submission_during_callback.load(std::memory_order_acquire));

    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, current_state());
    REQUIRE(eventually(
        [&] { return service.status().phase == daemon::StreamNormalizationPhase::Ready; }));
    scripted->push_response(first, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == core::AuthState::Ready; }));
    client.close();
}

TEST_CASE("old observer destruction and stale callbacks cannot clear a replacement",
          "[stream][service][generation]") {
    daemon::StreamService service;
    auto factory = service.observer_factory();
    auto first = factory(1001, 1);
    REQUIRE(first);
    CHECK(service.status().generation == 1);
    auto first_failure = core::TdValue::from(
        core::TdMalformedSupportedUpdate{.kind = core::TdSupportedUpdateKind::NewMessage,
                                         .reason = core::TdMalformedUpdateReason::InvalidContent,
                                         .tdlib_type_id = 66});
    first_failure.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    first->on_update(first_failure);
    CHECK(service.status().phase == daemon::StreamNormalizationPhase::Failed);
    auto second = factory(1002, 2);
    REQUIRE(second);
    CHECK(service.status().generation == 2);

    auto stale = core::TdValue::from(
        core::TdMalformedSupportedUpdate{.kind = core::TdSupportedUpdateKind::NewMessage,
                                         .reason = core::TdMalformedUpdateReason::InvalidContent,
                                         .tdlib_type_id = 77});
    stale.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    first->on_update(stale);
    CHECK(service.status().generation == 2);
    CHECK(service.status().phase == daemon::StreamNormalizationPhase::Bootstrap);
    first.reset();
    CHECK(service.status().generation == 2);

    auto state = current_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    second->on_current_state(state);
    CHECK(service.status().phase == daemon::StreamNormalizationPhase::Ready);
}

TEST_CASE("stream service observes synchronous current-state dispatch failure",
          "[stream][service][bootstrap][failure][fake-boundary]") {
    daemon::StreamService service;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    scripted->set_before_send([](const core::TdFunctionData& function) {
        if (function.kind() == core::TdFunctionKind::GetCurrentState) {
            throw std::runtime_error("current-state dispatch failed");
        }
    });
    core::TdClient client(std::move(runtime), {}, {}, service.observer_factory());
    REQUIRE(scripted->wait_for_sent(1));
    REQUIRE(eventually(
        [&] { return service.status().phase == daemon::StreamNormalizationPhase::Failed; }));
    CHECK(service.status().failure.kind == daemon::StreamFailureKind::DispatchFailure);
    const auto first = scripted->clients().front();
    scripted->set_before_send({});
    scripted->push_response(first, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == core::AuthState::Ready; }));
    client.close();
}

TEST_CASE("real generation replacement resets stream service and rejects old traffic",
          "[stream][service][generation][fake-boundary]") {
    daemon::StreamService service;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime), {}, {}, service.observer_factory());
    REQUIRE(scripted->wait_for_sent(2));
    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, current_state());
    scripted->push_response(first, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually(
        [&] { return service.status().phase == daemon::StreamNormalizationPhase::Ready; }));

    scripted->push_update(first, {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(scripted->wait_for_clients(2));
    REQUIRE(scripted->wait_for_sent(4));
    const auto second = scripted->clients().back();
    REQUIRE(eventually([&] {
        const auto status = service.status();
        return status.client_id == second.client_id && status.generation == 2 &&
               status.phase == daemon::StreamNormalizationPhase::Bootstrap;
    }));
    scripted->push_update(first, core::TdValue::from(core::TdMalformedSupportedUpdate{
                                     .kind = core::TdSupportedUpdateKind::NewMessage,
                                     .reason = core::TdMalformedUpdateReason::InvalidContent,
                                     .tdlib_type_id = 88}));
    REQUIRE(scripted->wait_for_received(4));
    CHECK(service.status().generation == 2);
    CHECK(service.status().phase == daemon::StreamNormalizationPhase::Bootstrap);

    scripted->push_response(second, 2, current_state());
    scripted->push_response(second, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually([&] {
        return service.status().generation == 2 &&
               service.status().phase == daemon::StreamNormalizationPhase::Ready &&
               client.auth_state()->client_generation == 2 &&
               client.auth_state()->data.state == core::AuthState::Ready;
    }));
    client.close();
}

TEST_CASE("stream service publishes immutable failure before concurrent observation",
          "[stream][service][status][fake-boundary]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    REQUIRE(observer);
    std::atomic<bool> stop{false};
    std::atomic<bool> inconsistent{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto status = service.status();
            if (status.phase == daemon::StreamNormalizationPhase::Failed &&
                (status.generation != 1 ||
                 status.failure.kind != daemon::StreamFailureKind::MalformedSupported ||
                 status.failure.update_kind != core::TdSupportedUpdateKind::MessageContent ||
                 status.failure.tdlib_type_id != 91)) {
                inconsistent.store(true, std::memory_order_release);
            }
        }
    });
    auto malformed = core::TdValue::from(
        core::TdMalformedSupportedUpdate{.kind = core::TdSupportedUpdateKind::MessageContent,
                                         .reason = core::TdMalformedUpdateReason::InvalidContent,
                                         .tdlib_type_id = 91});
    malformed.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_update(malformed);
    REQUIRE(eventually(
        [&] { return service.status().phase == daemon::StreamNormalizationPhase::Failed; }));
    stop.store(true, std::memory_order_release);
    reader.join();
    CHECK_FALSE(inconsistent.load(std::memory_order_acquire));
}
