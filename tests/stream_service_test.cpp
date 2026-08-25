#include "core/td_client.hpp"
#include "daemon/stream_service.hpp"
#include "support/scripted_td_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

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

core::TdValue current_stream_state() {
    core::TdCurrentState state;
    state.updates.push_back(core::TdValue::from(
        core::TdUpdateUser{.user = {.id = 42,
                                    .first_name = "Ada",
                                    .last_name = "Lovelace",
                                    .usernames = {"ada"},
                                    .phone_number = {},
                                    .is_bot = false,
                                    .is_premium = false,
                                    .presence = core::TdUserPresence::Online}}));
    state.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{
        .chat = {.id = -1001,
                 .title = "Project",
                 .kind = core::TdChatKind::Private,
                 .related_id = 42,
                 .tdlib_type_id = 0,
                 .positions = {},
                 .chat_lists = {{.kind = core::TdChatListKind::Main, .folder_id = 0}},
                 .is_marked_unread = false,
                 .unread_count = 0,
                 .unread_mention_count = 0,
                 .unread_reaction_count = 0,
                 .unread_poll_vote_count = 0,
                 .last_message = std::nullopt,
                 .notification_settings = std::nullopt}}));
    return core::TdValue::from(std::move(state));
}

core::TdValue stream_message() {
    return core::TdValue::from(core::TdUpdateNewMessage{
        .message = {
            .id = 123,
            .chat_id = -1001,
            .date = 1'785'924'000,
            .sender = {.kind = core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 0},
            .is_outgoing = false,
            .topic = std::nullopt,
            .content_kind = core::TdMessageContentKind::Text,
            .text = "boundary"}});
}

daemon::StreamIngressRequest stream_request(std::int32_t client_id = 1001,
                                            std::uint64_t generation = 1) {
    return {.client_id = client_id,
            .generation = generation,
            .operation = daemon::StreamOperation::Listen,
            .mode = daemon::StreamMode::Items,
            .type_mask = daemon::stream_event_mask(daemon::StreamEventClass::Message)};
}

daemon::StreamIngressReservation reserve_stream(daemon::StreamService& service) {
    auto result = service.ingress_hub().reserve(stream_request());
    REQUIRE(std::holds_alternative<daemon::StreamIngressReservation>(result));
    return std::move(std::get<daemon::StreamIngressReservation>(result));
}

daemon::StreamIngressFrontAction
capture_descriptor(void* context, const daemon::StreamIngressFrontCursor& cursor) {
    *static_cast<std::optional<daemon::StreamIngressDescriptor>*>(context) = cursor.descriptor();
    return daemon::StreamIngressFrontAction::Keep;
}

std::optional<daemon::StreamIngressDescriptor>
front_descriptor(daemon::StreamService& service, daemon::StreamIngressReservation& reservation) {
    std::optional<daemon::StreamIngressDescriptor> descriptor;
    const auto result =
        service.ingress_hub().visit_front(reservation, &descriptor, &capture_descriptor);
    if (result == daemon::StreamIngressFrontResult::Empty) {
        return std::nullopt;
    }
    REQUIRE(result == daemon::StreamIngressFrontResult::Visited);
    return descriptor;
}

struct BlockingSequenceProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    static void notify(void* context, daemon::detail::StreamStatusPublishPoint point) noexcept {
        if (point != daemon::detail::StreamStatusPublishPoint::Sequence) {
            return;
        }
        auto& probe = *static_cast<BlockingSequenceProbe*>(context);
        probe.entered.store(true, std::memory_order_release);
        while (!probe.release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

} // namespace

TEST_CASE("stream service exposes one stable shared ingress lifetime",
          "[stream][service][ingress][lifetime]") {
    daemon::StreamService service;
    const auto ingress = service.ingress_hub_handle();
    REQUIRE(ingress);
    CHECK(ingress.get() == &service.ingress_hub());
    CHECK(service.ingress_hub_handle() == ingress);
}

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

TEST_CASE("old observer destruction cannot publish during replacement callback",
          "[stream][service][generation][status]") {
    BlockingSequenceProbe probe;
    daemon::StreamService service(nullptr,
                                  {.context = &probe, .hook = &BlockingSequenceProbe::notify});
    auto factory = service.observer_factory();
    auto first = factory(1001, 1);
    auto second = factory(1002, 2);
    auto state = current_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});

    std::thread callback([&] { second->on_current_state(state); });
    REQUIRE(eventually([&] { return probe.entered.load(std::memory_order_acquire); }));
    auto destroy = std::async(std::launch::async, [&] { first.reset(); });
    CHECK(destroy.wait_for(2s) == std::future_status::ready);
    auto reader = std::async(std::launch::async, [&] { return service.status(); });
    CHECK(reader.wait_for(10ms) == std::future_status::timeout);

    probe.release.store(true, std::memory_order_release);
    callback.join();
    destroy.get();
    const auto status = reader.get();
    CHECK(status.client_id == 1002);
    CHECK(status.generation == 2);
    CHECK(status.phase == daemon::StreamNormalizationPhase::Ready);
}

TEST_CASE("stream service activates only at a ready ordered receive boundary",
          "[stream][service][ingress][activation]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    auto state = current_stream_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_current_state(state);
    observer->on_authorization_state(core::AuthStateData{core::AuthState::Ready}, 2);

    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    CHECK(service.ingress_hub().activation_state(reserved) == daemon::StreamIngressState::Armed);
    observer->on_receive_boundary(1);
    CHECK(service.ingress_hub().activation_state(reserved) == daemon::StreamIngressState::Armed);
    observer->on_receive_boundary(2);
    CHECK(service.ingress_hub().activation_state(reserved) ==
          daemon::StreamIngressState::Published);
    CHECK(service.ingress_hub().activation_projection(reserved)->activation_receive_sequence == 2);

    auto message = stream_message();
    message.set_receive_event_metadata(3, core::TdEventClock::time_point{});
    observer->on_update(message);
    observer->on_receive_boundary(3);
    const auto front = front_descriptor(service, reserved);
    REQUIRE(front);
    CHECK(front->receive_sequence == 3);
    CHECK(front->event_class == daemon::StreamEventClass::Message);
}

TEST_CASE("live incomplete new chat keeps activation armed through its FIFO barrier",
          "[stream][service][ingress][activation][ordering]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    auto state = current_stream_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_current_state(state);
    observer->on_authorization_state(core::AuthStateData{core::AuthState::Ready}, 2);

    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    auto incomplete = core::TdValue::from(core::TdUpdateNewChat{
        .chat = {.id = -2000,
                 .title = "Incomplete",
                 .kind = core::TdChatKind::Supergroup,
                 .related_id = 55,
                 .tdlib_type_id = 0,
                 .positions = {},
                 .chat_lists = {{.kind = core::TdChatListKind::Main, .folder_id = 0}},
                 .is_marked_unread = false,
                 .unread_count = 0,
                 .unread_mention_count = 0,
                 .unread_reaction_count = 0,
                 .unread_poll_vote_count = 0,
                 .last_message = std::nullopt,
                 .notification_settings = std::nullopt}});
    incomplete.set_receive_event_metadata(3, core::TdEventClock::time_point{});
    observer->on_update(incomplete);
    REQUIRE(service.status().ordering_barrier_open);
    observer->on_receive_boundary(3);
    CHECK(service.ingress_hub().activation_state(reserved) == daemon::StreamIngressState::Armed);

    auto entity = core::TdValue::from(core::TdUpdateSupergroup{
        .supergroup = {
            .id = 55, .usernames = {"complete"}, .is_channel = false, .is_forum = false}});
    entity.set_receive_event_metadata(4, core::TdEventClock::time_point{});
    observer->on_update(entity);
    REQUIRE_FALSE(service.status().ordering_barrier_open);
    observer->on_receive_boundary(4);
    REQUIRE(service.ingress_hub().activation_state(reserved) ==
            daemon::StreamIngressState::Published);
    REQUIRE(service.ingress_hub().activation_projection(reserved));
    CHECK(service.ingress_hub().activation_projection(reserved)->activation_receive_sequence == 4);
    CHECK_FALSE(front_descriptor(service, reserved));

    auto message = stream_message();
    message.set_receive_event_metadata(5, core::TdEventClock::time_point{});
    observer->on_update(message);
    const auto front = front_descriptor(service, reserved);
    REQUIRE(front);
    CHECK(front->receive_sequence == 5);
}

TEST_CASE("authorization loss closes dormant ingress before publication",
          "[stream][service][ingress][authorization]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    auto state = current_stream_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_current_state(state);
    observer->on_authorization_state(core::AuthStateData{core::AuthState::Ready}, 2);
    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));

    observer->on_authorization_state(core::AuthStateData{core::AuthState::Closing}, 3);
    observer->on_authorization_state(core::AuthStateData{core::AuthState::WaitPhoneNumber}, 4);
    observer->on_receive_boundary(3);
    CHECK(service.ingress_hub().activation_state(reserved) == daemon::StreamIngressState::Armed);
    const auto terminal = service.ingress_hub().claim_terminal(reserved);
    REQUIRE(terminal);
    CHECK(terminal->cause == daemon::StreamTerminalCause::AuthorizationLost);
    CHECK(terminal->auth_state == static_cast<std::int32_t>(core::AuthState::Closing));
}

TEST_CASE("generation replacement claims old ingress and stale destruction is inert",
          "[stream][service][ingress][generation]") {
    daemon::StreamService service;
    auto first = service.observer_factory()(1001, 1);
    auto state = current_stream_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    first->on_current_state(state);
    first->on_authorization_state(core::AuthStateData{core::AuthState::Ready}, 2);
    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    first->on_receive_boundary(2);
    REQUIRE(service.ingress_hub().activation_state(reserved) ==
            daemon::StreamIngressState::Published);

    auto second = service.observer_factory()(1002, 2);
    REQUIRE(second);
    first.reset();
    const auto terminal = service.ingress_hub().claim_terminal(reserved);
    REQUIRE(terminal);
    CHECK(terminal->cause == daemon::StreamTerminalCause::GenerationReplaced);
    CHECK(service.status().client_id == 1002);
    CHECK(service.status().generation == 2);
}

TEST_CASE("active metadata failure is retained generation wide",
          "[stream][service][ingress][failure]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    auto state = current_stream_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_current_state(state);
    observer->on_authorization_state(core::AuthStateData{core::AuthState::Ready}, 2);
    auto reserved = reserve_stream(service);
    auto second_reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    REQUIRE(service.ingress_hub().commit_activation(second_reserved));
    observer->on_receive_boundary(2);

    auto malformed = core::TdValue::from(
        core::TdMalformedSupportedUpdate{.kind = core::TdSupportedUpdateKind::NewMessage,
                                         .reason = core::TdMalformedUpdateReason::InvalidContent,
                                         .tdlib_type_id = 77});
    malformed.set_receive_event_metadata(3, core::TdEventClock::time_point{});
    observer->on_update(malformed);
    const auto terminal = service.ingress_hub().claim_terminal(reserved);
    REQUIRE(terminal);
    CHECK(terminal->cause == daemon::StreamTerminalCause::MetadataFailure);
    CHECK(terminal->metadata_failure.kind == daemon::StreamFailureKind::MalformedSupported);
    CHECK(terminal->metadata_failure.tdlib_type_id == 77);
    const auto second_terminal = service.ingress_hub().claim_terminal(second_reserved);
    REQUIRE(second_terminal);
    CHECK(second_terminal->cause == daemon::StreamTerminalCause::MetadataFailure);
    CHECK(second_terminal->metadata_failure == terminal->metadata_failure);
}

TEST_CASE("real receive idle boundary closes the activation gap before the next update",
          "[stream][service][ingress][boundary][fake-boundary]") {
    daemon::StreamService service;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime), {}, {}, service.observer_factory());
    REQUIRE(scripted->wait_for_sent(2));
    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, current_stream_state());
    scripted->push_response(first, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually([&] { return service.status().ready_for_admission(); }));

    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    REQUIRE(eventually([&] {
        return service.ingress_hub().activation_state(reserved) ==
               daemon::StreamIngressState::Published;
    }));
    const auto anchor =
        service.ingress_hub().activation_projection(reserved)->activation_receive_sequence;
    scripted->push_update(first, stream_message());
    REQUIRE(eventually([&] { return front_descriptor(service, reserved).has_value(); }));
    const auto front = front_descriptor(service, reserved);
    REQUIRE(front);
    CHECK(front->receive_sequence > anchor);
    client.close();
}

TEST_CASE("unexpected Closed claims authorization before generation replacement",
          "[stream][service][ingress][authorization][generation][fake-boundary]") {
    daemon::StreamService service;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime), {}, {}, service.observer_factory());
    REQUIRE(scripted->wait_for_sent(2));
    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, current_stream_state());
    scripted->push_response(first, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually([&] { return service.status().ready_for_admission(); }));

    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    REQUIRE(eventually([&] {
        return service.ingress_hub().activation_state(reserved) ==
               daemon::StreamIngressState::Published;
    }));
    scripted->push_update(first, {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(scripted->wait_for_clients(2));
    const auto terminal = service.ingress_hub().claim_terminal(reserved);
    REQUIRE(terminal);
    CHECK(terminal->cause == daemon::StreamTerminalCause::AuthorizationLost);
    CHECK(terminal->auth_state == static_cast<std::int32_t>(core::AuthState::Closed));
    REQUIRE(eventually([&] { return service.status().generation == 2; }));
    const auto second = scripted->clients().back();
    scripted->push_response(second, 2, current_stream_state());
    scripted->push_response(second, 1, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(eventually([&] {
        return client.auth_state()->client_generation == 2 &&
               client.auth_state()->data.state == core::AuthState::Ready;
    }));
    client.close();
}

TEST_CASE("stream service shutdown handoff claims current generation",
          "[stream][service][ingress][shutdown]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    auto state = current_stream_state();
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_current_state(state);
    observer->on_authorization_state(core::AuthStateData{core::AuthState::Ready}, 2);
    auto reserved = reserve_stream(service);
    REQUIRE(service.ingress_hub().commit_activation(reserved));
    observer->on_receive_boundary(2);

    service.claim_shutdown();
    const auto terminal = service.ingress_hub().claim_terminal(reserved);
    REQUIRE(terminal);
    CHECK(terminal->cause == daemon::StreamTerminalCause::Shutdown);
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

TEST_CASE("stream status readers observe coherent snapshots during live publication",
          "[stream][service][status][concurrency][fake-boundary]") {
    daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    REQUIRE(observer);
    core::TdCurrentState base;
    base.updates.push_back(core::TdValue::from(
        core::TdUpdateUser{.user = {.id = 42,
                                    .first_name = "Ada",
                                    .last_name = "Lovelace",
                                    .usernames = {"ada"},
                                    .phone_number = {},
                                    .is_bot = false,
                                    .is_premium = false,
                                    .presence = core::TdUserPresence::Online}}));
    base.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{
        .chat = {.id = -1001,
                 .title = "Project",
                 .kind = core::TdChatKind::Private,
                 .related_id = 42,
                 .tdlib_type_id = 0,
                 .positions = {},
                 .chat_lists = {{.kind = core::TdChatListKind::Main, .folder_id = 0}},
                 .is_marked_unread = false,
                 .unread_count = 0,
                 .unread_mention_count = 0,
                 .unread_reaction_count = 0,
                 .unread_poll_vote_count = 0,
                 .last_message = std::nullopt,
                 .notification_settings = std::nullopt}}));
    auto state = core::TdValue::from(std::move(base));
    state.set_receive_event_metadata(1, core::TdEventClock::time_point{});
    observer->on_current_state(state);
    REQUIRE(service.status().ready_for_admission());

    std::atomic<bool> stop{false};
    std::atomic<bool> invalid{false};
    std::vector<std::thread> readers;
    readers.reserve(2);
    for (int index = 0; index < 2; ++index) {
        readers.emplace_back([&] {
            std::uint64_t previous = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const auto status = service.status();
                if (status.client_id != 1001 || status.generation != 1 ||
                    status.phase != daemon::StreamNormalizationPhase::Ready ||
                    status.failure.kind != daemon::StreamFailureKind::None ||
                    status.ordering_barrier_open || status.receive_sequence < previous) {
                    invalid.store(true, std::memory_order_release);
                    return;
                }
                previous = status.receive_sequence;
            }
        });
    }
    for (std::uint64_t sequence = 2; sequence <= 2'000; ++sequence) {
        auto update = core::TdValue::from(
            core::TdUpdateChatTitle{.chat_id = -1001, .title = sequence % 2 == 0 ? "even" : "odd"});
        update.set_receive_event_metadata(sequence, core::TdEventClock::time_point{});
        observer->on_update(update);
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }
    CHECK_FALSE(invalid.load(std::memory_order_acquire));
    const auto final = service.status();
    CHECK(final.receive_sequence == 2'000);
    CHECK(final.ready_for_admission());
}
