#include "core/td_authorization.hpp"
#include "core/td_client.hpp"
#include "support/scripted_td_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using namespace tgcli::core;
using tgcli::test::ScriptedClient;
using tgcli::test::ScriptedTdRuntime;

namespace {

template <typename Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

struct ObserverState {
    std::atomic<std::uint64_t> next_sequence{1};
    std::atomic<std::uint64_t> update_sequence{0};
    std::atomic<std::uint64_t> current_state_sequence{0};
    std::atomic<std::uint64_t> public_sequence{0};
    std::atomic<std::size_t> update_count{0};
    std::atomic<std::size_t> current_state_count{0};
    std::atomic<std::size_t> current_state_failure_count{0};
    std::atomic<bool> current_state_failure_exact{false};
    std::atomic<std::size_t> authorization_count{0};
    std::atomic<std::size_t> boundary_count{0};
    std::atomic<std::uint64_t> last_authorization_sequence{0};
    std::atomic<std::uint64_t> last_boundary_sequence{0};
    std::atomic<std::uint64_t> authorization_order{0};
    std::atomic<std::uint64_t> boundary_order{0};
    std::atomic<AuthState> last_authorization{AuthState::Unknown};
};

class RecordingGenerationObserver final : public TdGenerationObserver {
  public:
    explicit RecordingGenerationObserver(std::shared_ptr<ObserverState> state)
        : state_(std::move(state)) {}

    void on_update(const TdValue& update) noexcept override {
        if (update.get_if<TdUpdateChatTitle>() != nullptr) {
            state_->update_count.fetch_add(1, std::memory_order_relaxed);
            state_->update_sequence.store(
                state_->next_sequence.fetch_add(1, std::memory_order_relaxed),
                std::memory_order_release);
        }
    }

    void on_current_state(const TdValue& state) noexcept override {
        if (state.get_if<TdCurrentState>() != nullptr) {
            state_->current_state_count.fetch_add(1, std::memory_order_relaxed);
            state_->current_state_sequence.store(
                state_->next_sequence.fetch_add(1, std::memory_order_relaxed),
                std::memory_order_release);
        }
    }

    void on_current_state_failure(const std::exception_ptr& failure) noexcept override {
        bool exact = false;
        try {
            std::rethrow_exception(failure);
        } catch (const std::runtime_error& error) {
            exact = std::string_view(error.what()) == "scripted current-state dispatch failure";
        } catch (const std::exception&) {
            exact = false;
        }
        state_->current_state_failure_exact.store(exact, std::memory_order_release);
        state_->current_state_failure_count.fetch_add(1, std::memory_order_release);
    }

    void on_authorization_state(const AuthStateData& state,
                                std::uint64_t receive_sequence) noexcept override {
        state_->last_authorization.store(state.state, std::memory_order_relaxed);
        state_->last_authorization_sequence.store(receive_sequence, std::memory_order_relaxed);
        state_->authorization_order.store(
            state_->next_sequence.fetch_add(1, std::memory_order_relaxed),
            std::memory_order_release);
        state_->authorization_count.fetch_add(1, std::memory_order_release);
    }

    void on_receive_boundary(std::uint64_t receive_sequence) noexcept override {
        state_->last_boundary_sequence.store(receive_sequence, std::memory_order_relaxed);
        state_->boundary_order.store(state_->next_sequence.fetch_add(1, std::memory_order_relaxed),
                                     std::memory_order_release);
        state_->boundary_count.fetch_add(1, std::memory_order_release);
    }

  private:
    std::shared_ptr<ObserverState> state_;
};

const TdFieldValue* field(const TdFunctionData& function, std::string_view name) {
    for (const auto& item : function.fields()) {
        if (item.has_name(name)) {
            return &item.value();
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("scripted M5 resolver factories expose exact neutral descriptors",
          "[stream][resolver][core][fake-boundary]") {
    ScriptedTdRuntime runtime;

    auto current = runtime.make_get_current_state();
    REQUIRE(current.function_data());
    CHECK(current.function_data()->kind() == TdFunctionKind::GetCurrentState);
    CHECK(current.function_data()->fields().empty());

    auto contacts = runtime.make_get_contacts();
    REQUIRE(contacts.function_data());
    CHECK(contacts.function_data()->kind() == TdFunctionKind::GetContacts);
    CHECK(contacts.function_data()->fields().empty());

    auto basic = runtime.make_get_basic_group_full_info(51);
    REQUIRE(basic.function_data());
    CHECK(basic.function_data()->kind() == TdFunctionKind::GetBasicGroupFullInfo);
    REQUIRE(field(*basic.function_data(), "basic_group_id") != nullptr);
    CHECK(std::get<std::int64_t>(*field(*basic.function_data(), "basic_group_id")) == 51);

    auto members = runtime.make_get_supergroup_members(55, "ada", 20, 100);
    REQUIRE(members.function_data());
    CHECK(members.function_data()->kind() == TdFunctionKind::GetSupergroupMembers);
    CHECK(std::get<std::int64_t>(*field(*members.function_data(), "supergroup_id")) == 55);
    CHECK(std::get<std::string>(*field(*members.function_data(), "filter")) == "search");
    CHECK(std::get<std::string>(*field(*members.function_data(), "query")) == "ada");
    CHECK(std::get<std::int64_t>(*field(*members.function_data(), "offset")) == 20);
    CHECK(std::get<std::int64_t>(*field(*members.function_data(), "limit")) == 100);
}

TEST_CASE("M5 resolver reads are Ready request-owner calls and current state is internal bootstrap",
          "[stream][resolver][core][authorization][fake-boundary]") {
    const auto request_capability = std::make_shared<const std::uint64_t>(9);
    const AuthStateSnapshot ready{.client_id = 1001,
                                  .client_generation = 1,
                                  .auth_sequence = 3,
                                  .receive_event_sequence = 7,
                                  .data = AuthStateData{AuthState::Ready},
                                  .receive_observed_at = std::nullopt};
    for (const auto function : {TdFunctionKind::GetContacts, TdFunctionKind::GetBasicGroupFullInfo,
                                TdFunctionKind::GetSupergroupMembers}) {
        const TdFunctionData function_data{function};
        const TdSendDescriptor descriptor{.function = function,
                                          .tier = DescriptorKind::Read,
                                          .owner = {TdOwnerKind::Request, 9, request_capability},
                                          .client_generation = 1,
                                          .auth_sequence = 3,
                                          .auth_state = AuthState::Ready};
        CHECK_FALSE(authorize_td_send(descriptor, &function_data, ready, false));
    }

    const auto internal_capability = std::make_shared<const std::uint64_t>(10);
    const AuthStateSnapshot unknown{.client_id = 1001,
                                    .client_generation = 1,
                                    .auth_sequence = 0,
                                    .receive_event_sequence = 0,
                                    .data = AuthStateData{AuthState::Unknown},
                                    .receive_observed_at = std::nullopt};
    const TdSendDescriptor current_state{
        .function = TdFunctionKind::GetCurrentState,
        .tier = DescriptorKind::AuthBootstrap,
        .owner = {TdOwnerKind::InternalAuth, 10, internal_capability},
        .client_generation = 1,
        .auth_sequence = 0,
        .auth_state = AuthState::Unknown};
    const TdFunctionData function{TdFunctionKind::GetCurrentState};
    CHECK_FALSE(authorize_td_send(current_state, &function, unknown, false));
    auto wrong_tier = current_state;
    wrong_tier.tier = DescriptorKind::Read;
    CHECK(authorize_td_send(wrong_tier, &function, unknown, false) ==
          TdAuthorizationFailure::TierMismatch);
    auto public_owner = current_state;
    public_owner.owner = {TdOwnerKind::Request, 9, request_capability};
    CHECK(authorize_td_send(public_owner, &function, unknown, false) ==
          TdAuthorizationFailure::OwnerMismatch);
}

TEST_CASE("TdClient submits typed M5 resolver calls with one scripted trace",
          "[stream][resolver][core][authorization][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    TdClient client(std::move(runtime));
    REQUIRE(scripted->wait_for_sent(1));
    const auto first = scripted->clients().front();
    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == AuthState::Ready; }));

    auto contacts = client.get_contacts(client.auth_state());
    REQUIRE(scripted->wait_for_sent(2));
    auto sent = scripted->sent_functions();
    CHECK(sent.back().function.kind() == TdFunctionKind::GetContacts);
    scripted->push_response(first, sent.back().query_id,
                            TdValue::from(TdUsers{.total_count = 2, .user_ids = {42, 43}}));
    REQUIRE(contacts.wait_for(2s) == std::future_status::ready);
    REQUIRE(contacts.get().get_if<TdUsers>() != nullptr);

    auto basic = client.get_basic_group_full_info(client.auth_state(), 51);
    REQUIRE(scripted->wait_for_sent(3));
    sent = scripted->sent_functions();
    CHECK(sent.back().function.kind() == TdFunctionKind::GetBasicGroupFullInfo);
    scripted->push_response(first, sent.back().query_id, TdValue::from(TdBasicGroupFullInfo{}));
    REQUIRE(basic.wait_for(2s) == std::future_status::ready);
    REQUIRE(basic.get().get_if<TdBasicGroupFullInfo>() != nullptr);

    auto members = client.get_supergroup_members(client.auth_state(), 55, "ada", 0, 100);
    REQUIRE(scripted->wait_for_sent(4));
    sent = scripted->sent_functions();
    CHECK(sent.back().function.kind() == TdFunctionKind::GetSupergroupMembers);
    CHECK(std::get<std::string>(*field(sent.back().function, "query")) == "ada");
    scripted->push_response(first, sent.back().query_id, TdValue::from(TdChatMembers{}));
    REQUIRE(members.wait_for(2s) == std::future_status::ready);
    REQUIRE(members.get().get_if<TdChatMembers>() != nullptr);
}

TEST_CASE("generation observer is installed before current-state send and precedes public updates",
          "[stream][core][td-runtime][bootstrap][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    auto state = std::make_shared<ObserverState>();
    std::atomic<bool> observer_installed{false};
    std::atomic<bool> current_state_sent_after_install{false};
    scripted->set_before_send([&](const TdFunctionData& function) {
        if (function.kind() == TdFunctionKind::GetCurrentState) {
            current_state_sent_after_install.store(
                observer_installed.load(std::memory_order_acquire), std::memory_order_release);
        }
    });
    TdGenerationObserverFactory observer_factory = [&](std::int32_t client_id,
                                                       std::uint64_t generation) {
        CHECK(client_id == 1001);
        CHECK(generation == 1);
        observer_installed.store(true, std::memory_order_release);
        return std::make_unique<RecordingGenerationObserver>(state);
    };
    TdClient client(std::move(runtime), {}, {}, std::move(observer_factory));

    REQUIRE(scripted->wait_for_sent_including_current_state(2));
    const auto sent = scripted->sent_functions_including_current_state();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].query_id == 1);
    CHECK(sent[0].function.kind() == TdFunctionKind::GetAuthorizationState);
    CHECK(sent[1].query_id == 2);
    CHECK(sent[1].function.kind() == TdFunctionKind::GetCurrentState);
    CHECK(current_state_sent_after_install.load(std::memory_order_acquire));

    const ScriptedClient first{.client_id = 1001, .client_generation = 1};
    const auto subscription = client.subscribe_updates([&](const TdValue& update) {
        if (update.get_if<TdUpdateChatTitle>() != nullptr) {
            state->public_sequence.store(
                state->next_sequence.fetch_add(1, std::memory_order_relaxed),
                std::memory_order_release);
        }
    });
    scripted->push_update(first,
                          TdValue::from(TdUpdateChatTitle{.chat_id = -1001, .title = "first"}));
    REQUIRE(eventually([&] { return state->update_count.load(std::memory_order_acquire) == 1; }));
    CHECK(state->public_sequence.load(std::memory_order_acquire) == 0);

    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    REQUIRE(eventually(
        [&] { return state->current_state_count.load(std::memory_order_acquire) == 1; }));
    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(
        eventually([&] { return state->public_sequence.load(std::memory_order_acquire) != 0; }));
    CHECK(state->update_sequence.load(std::memory_order_acquire) <
          state->public_sequence.load(std::memory_order_acquire));
    CHECK(state->current_state_sequence.load(std::memory_order_acquire) <
          state->public_sequence.load(std::memory_order_acquire));
    client.unsubscribe_updates(subscription);
}

TEST_CASE("every generation reserves auth query one and current-state query two without observer",
          "[core][td-runtime][bootstrap][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    const TdClient client(std::move(runtime));

    REQUIRE(scripted->wait_for_sent_including_current_state(2));
    auto sent = scripted->sent_functions_including_current_state();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].query_id == 1);
    CHECK(sent[0].function.kind() == TdFunctionKind::GetAuthorizationState);
    CHECK(sent[1].query_id == 2);
    CHECK(sent[1].function.kind() == TdFunctionKind::GetCurrentState);

    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    scripted->push_update(first, {}, AuthStateData{AuthState::Closed});
    REQUIRE(scripted->wait_for_clients(2));
    REQUIRE(scripted->wait_for_sent_including_current_state(4));
    sent = scripted->sent_functions_including_current_state();
    CHECK(sent[2].query_id == 1);
    CHECK(sent[2].function.kind() == TdFunctionKind::GetAuthorizationState);
    CHECK(sent[3].query_id == 2);
    CHECK(sent[3].function.kind() == TdFunctionKind::GetCurrentState);
}

TEST_CASE("generation observer receives auth then one boundary for events and empty polls",
          "[stream][core][boundary][authorization][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    auto state = std::make_shared<ObserverState>();
    TdGenerationObserverFactory observer_factory = [&](std::int32_t, std::uint64_t) {
        return std::make_unique<RecordingGenerationObserver>(state);
    };
    const TdClient client(std::move(runtime), {}, {}, std::move(observer_factory));
    REQUIRE(scripted->wait_for_sent_including_current_state(2));
    const auto first = scripted->clients().front();
    REQUIRE(eventually([&] { return state->boundary_count.load(std::memory_order_acquire) > 0; }));
    const auto empty_boundaries = state->boundary_count.load(std::memory_order_acquire);
    CHECK(state->last_boundary_sequence.load(std::memory_order_acquire) == 0);

    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    REQUIRE(scripted->wait_for_received(1));
    REQUIRE(eventually(
        [&] { return state->last_boundary_sequence.load(std::memory_order_acquire) == 1; }));
    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(scripted->wait_for_received(2));
    REQUIRE(eventually([&] {
        return state->last_authorization.load(std::memory_order_acquire) == AuthState::Ready &&
               state->last_boundary_sequence.load(std::memory_order_acquire) == 2;
    }));
    CHECK(state->last_authorization_sequence.load(std::memory_order_acquire) == 2);
    CHECK(state->authorization_order.load(std::memory_order_acquire) <
          state->boundary_order.load(std::memory_order_acquire));

    const auto before_orphan = state->boundary_count.load(std::memory_order_acquire);
    scripted->push_response(first, 999, TdValue::from(TdOk{}));
    REQUIRE(scripted->wait_for_received(3));
    REQUIRE(eventually(
        [&] { return state->boundary_count.load(std::memory_order_acquire) > before_orphan; }));
    CHECK(state->last_boundary_sequence.load(std::memory_order_acquire) == 3);
    CHECK(state->boundary_count.load(std::memory_order_acquire) >= empty_boundaries + 3);
}

TEST_CASE("generation observers reject stale callbacks and reset on replacement",
          "[stream][core][td-runtime][generation][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    auto first_state = std::make_shared<ObserverState>();
    auto second_state = std::make_shared<ObserverState>();
    TdGenerationObserverFactory observer_factory = [&](std::int32_t, std::uint64_t generation) {
        return std::make_unique<RecordingGenerationObserver>(generation == 1 ? first_state
                                                                             : second_state);
    };
    TdClient client(std::move(runtime), {}, {}, std::move(observer_factory));
    REQUIRE(scripted->wait_for_sent_including_current_state(2));
    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == AuthState::Ready; }));

    scripted->push_update(first, {}, AuthStateData{AuthState::Closed});
    REQUIRE(scripted->wait_for_clients(2));
    REQUIRE(scripted->wait_for_sent_including_current_state(4));
    const auto second = scripted->clients().back();
    CHECK(scripted->sent_functions_including_current_state()[2].function.kind() ==
          TdFunctionKind::GetAuthorizationState);
    CHECK(scripted->sent_functions_including_current_state()[3].function.kind() ==
          TdFunctionKind::GetCurrentState);

    const auto received_before_stale = scripted->received_count();
    const auto boundaries_before_stale =
        second_state->boundary_count.load(std::memory_order_acquire);
    scripted->push_update(first,
                          TdValue::from(TdUpdateChatTitle{.chat_id = -1001, .title = "stale"}));
    REQUIRE(scripted->wait_for_received(received_before_stale + 1));
    REQUIRE(eventually([&] {
        return second_state->boundary_count.load(std::memory_order_acquire) >
               boundaries_before_stale;
    }));
    CHECK(second_state->last_boundary_sequence.load(std::memory_order_acquire) == 0);
    CHECK(first_state->update_count.load(std::memory_order_acquire) == 0);

    scripted->push_update(second,
                          TdValue::from(TdUpdateChatTitle{.chat_id = -1001, .title = "current"}));
    REQUIRE(eventually(
        [&] { return second_state->update_count.load(std::memory_order_acquire) == 1; }));

    scripted->push_response(second, 2, TdValue::from(TdCurrentState{}));
    scripted->push_response(second, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] {
        return client.auth_state()->client_generation == 2 &&
               client.auth_state()->data.state == AuthState::Ready;
    }));
}

TEST_CASE("initial generation reports synchronous current-state dispatch failure once",
          "[stream][core][td-runtime][bootstrap][failure][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    auto state = std::make_shared<ObserverState>();
    scripted->set_before_send([](const TdFunctionData& function) {
        if (function.kind() == TdFunctionKind::GetCurrentState) {
            throw std::runtime_error("scripted current-state dispatch failure");
        }
    });
    TdGenerationObserverFactory observer_factory = [&](std::int32_t, std::uint64_t) {
        return std::make_unique<RecordingGenerationObserver>(state);
    };
    TdClient client(std::move(runtime), {}, {}, std::move(observer_factory));

    REQUIRE(scripted->wait_for_sent(1));
    REQUIRE(eventually(
        [&] { return state->current_state_failure_count.load(std::memory_order_acquire) == 1; }));
    CHECK(state->current_state_failure_exact.load(std::memory_order_acquire));
    CHECK(state->current_state_count.load(std::memory_order_acquire) == 0);
    REQUIRE(scripted->sent_functions().size() == 1);

    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    REQUIRE(scripted->wait_for_received(1));
    CHECK(state->current_state_failure_count.load(std::memory_order_acquire) == 1);
    CHECK(state->current_state_count.load(std::memory_order_acquire) == 0);
    scripted->set_before_send({});
    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == AuthState::Ready; }));
}

TEST_CASE("replacement generation reports synchronous current-state dispatch failure once",
          "[stream][core][td-runtime][bootstrap][generation][failure][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    auto first_state = std::make_shared<ObserverState>();
    auto second_state = std::make_shared<ObserverState>();
    TdGenerationObserverFactory observer_factory = [&](std::int32_t, std::uint64_t generation) {
        return std::make_unique<RecordingGenerationObserver>(generation == 1 ? first_state
                                                                             : second_state);
    };
    TdClient client(std::move(runtime), {}, {}, std::move(observer_factory));
    REQUIRE(scripted->wait_for_sent_including_current_state(2));
    const auto first = scripted->clients().front();
    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == AuthState::Ready; }));

    scripted->set_before_send([](const TdFunctionData& function) {
        if (function.kind() == TdFunctionKind::GetCurrentState) {
            throw std::runtime_error("scripted current-state dispatch failure");
        }
    });
    scripted->push_update(first, {}, AuthStateData{AuthState::Closed});
    REQUIRE(scripted->wait_for_clients(2));
    REQUIRE(scripted->wait_for_sent_including_current_state(3));
    REQUIRE(eventually([&] {
        return second_state->current_state_failure_count.load(std::memory_order_acquire) == 1;
    }));
    CHECK(second_state->current_state_failure_exact.load(std::memory_order_acquire));
    CHECK(second_state->current_state_count.load(std::memory_order_acquire) == 0);
    CHECK(scripted->sent_functions().back().function.kind() ==
          TdFunctionKind::GetAuthorizationState);

    const auto second = scripted->clients().back();
    scripted->push_response(second, 2, TdValue::from(TdCurrentState{}));
    REQUIRE(scripted->wait_for_received(4));
    CHECK(second_state->current_state_failure_count.load(std::memory_order_acquire) == 1);
    CHECK(second_state->current_state_count.load(std::memory_order_acquire) == 0);
    scripted->set_before_send({});
    scripted->push_response(second, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] {
        return client.auth_state()->client_generation == 2 &&
               client.auth_state()->data.state == AuthState::Ready;
    }));
}

TEST_CASE("current-state observer consumes the response barrier exactly once",
          "[stream][core][td-runtime][bootstrap][duplicate][fake-boundary]") {
    auto runtime = std::make_unique<ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    auto state = std::make_shared<ObserverState>();
    TdGenerationObserverFactory observer_factory = [&](std::int32_t, std::uint64_t) {
        return std::make_unique<RecordingGenerationObserver>(state);
    };
    TdClient client(std::move(runtime), {}, {}, std::move(observer_factory));
    REQUIRE(scripted->wait_for_sent_including_current_state(2));
    const auto first = scripted->clients().front();

    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    REQUIRE(eventually(
        [&] { return state->current_state_count.load(std::memory_order_acquire) == 1; }));
    scripted->push_response(first, 2, TdValue::from(TdCurrentState{}));
    REQUIRE(scripted->wait_for_received(2));
    CHECK(state->current_state_count.load(std::memory_order_acquire) == 1);
    CHECK(state->current_state_failure_count.load(std::memory_order_acquire) == 0);

    scripted->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->data.state == AuthState::Ready; }));
}
