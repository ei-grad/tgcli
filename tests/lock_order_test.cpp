#include "common/invite_redaction.hpp"
#include "core/query_registry.hpp"
#include "core/td_client.hpp"
#include "support/scripted_td_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using namespace tgcli;

namespace {

template <typename Operation> void require_bounded_completion(Operation&& operation) {
    std::packaged_task<void()> task(std::forward<Operation>(operation));
    auto completion = task.get_future();
    std::thread worker(std::move(task));
    if (completion.wait_for(2s) != std::future_status::ready) {
        std::fputs("reentrant wipe observer did not complete\n", stderr);
        ::_exit(86);
    }
    worker.join();
    completion.get();
}

std::shared_ptr<const core::AuthStateSnapshot> install_ready(core::TdClient& client,
                                                             test::ScriptedTdRuntime& runtime) {
    if (!runtime.wait_for_clients(1)) {
        throw std::runtime_error("scripted client was not created");
    }
    const auto clients = runtime.clients();
    if (clients.size() != 1) {
        throw std::runtime_error("unexpected scripted client count");
    }
    std::promise<void> ready_promise;
    auto ready = ready_promise.get_future();
    std::atomic_flag published;
    const auto subscription = client.subscribe_auth_states(
        [&](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
            if (snapshot->data.state == core::AuthState::Ready && !published.test_and_set()) {
                ready_promise.set_value();
            }
        });
    runtime.push_response(clients.front(), 1, {}, core::AuthStateData{core::AuthState::Ready});
    if (ready.wait_for(2s) != std::future_status::ready) {
        client.unsubscribe_auth_states(subscription);
        throw std::runtime_error("ready authorization was not published");
    }
    client.unsubscribe_auth_states(subscription);
    return client.auth_state();
}

} // namespace

TEST_CASE("invite registration and release permit reentrant redaction",
          "[lock-order][redaction][concurrency]") {
    auto& registry = redaction::InviteLinkRegistry::instance();
    const std::string invite = "https://t.me/+LockOrderRegistration";
    std::atomic<bool> releasing{false};
    std::atomic<std::size_t> registration_callbacks{0};
    std::atomic<std::size_t> release_callbacks{0};
    const secure::WipeObserver observer = [&](std::string_view, const char*, std::size_t) {
        static_cast<void>(registry.redact(invite));
        if (releasing.load(std::memory_order_acquire)) {
            release_callbacks.fetch_add(1, std::memory_order_relaxed);
        } else {
            registration_callbacks.fetch_add(1, std::memory_order_relaxed);
        }
    };

    require_bounded_completion([&] {
        auto lease = registry.register_link(invite, observer);
        if (!lease.valid()) {
            throw std::runtime_error("invite registration failed");
        }
        releasing.store(true, std::memory_order_release);
        lease.release();
    });

    CHECK(registration_callbacks.load(std::memory_order_relaxed) > 0);
    CHECK(release_callbacks.load(std::memory_order_relaxed) > 0);
    CHECK(registry.redact(invite) == invite);
}

TEST_CASE("query detachment releases reentrant correlation lifetimes outside its mutex",
          "[lock-order][query-registry][redaction][concurrency]") {
    using Payload = std::unique_ptr<int>;

    const auto exercise = [](std::string_view operation) {
        core::QueryRegistry<Payload> queries;
        auto& invites = redaction::InviteLinkRegistry::instance();
        const std::string invite = "https://t.me/+LockOrderQuery" + std::string(operation);
        std::atomic<bool> final_release{false};
        std::atomic<std::size_t> release_callbacks{0};
        const secure::WipeObserver observer = [&](std::string_view, const char*, std::size_t) {
            static_cast<void>(invites.redact(invite));
            static_cast<void>(queries.pending_count());
            if (final_release.load(std::memory_order_acquire)) {
                release_callbacks.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto lease = invites.register_link(invite, observer);
        if (!lease.valid()) {
            throw std::runtime_error("invite registration failed");
        }
        auto lifetime = lease.protection();
        lease.release();
        auto [id, future] = queries.reserve(lifetime);
        lifetime.reset();
        final_release.store(true, std::memory_order_release);

        if (operation == "take") {
            auto detached = queries.take(id).value_or(core::QueryRegistry<Payload>::Detached{});
            if (!detached.lifetime) {
                throw std::runtime_error("query take failed");
            }
            detached.lifetime.reset();
        } else if (operation == "fail") {
            if (!queries.fail(id, std::make_exception_ptr(std::runtime_error("failed")))) {
                throw std::runtime_error("query fail failed");
            }
        } else {
            queries.fail_all("closed");
        }
        if (release_callbacks.load(std::memory_order_relaxed) == 0) {
            throw std::runtime_error("final correlation release was not observed");
        }
        static_cast<void>(future);
    };

    SECTION("take") {
        std::atomic<bool> completed{false};
        require_bounded_completion([&] {
            exercise("take");
            completed.store(true, std::memory_order_release);
        });
        CHECK(completed.load(std::memory_order_acquire));
    }
    SECTION("fail") {
        std::atomic<bool> completed{false};
        require_bounded_completion([&] {
            exercise("fail");
            completed.store(true, std::memory_order_release);
        });
        CHECK(completed.load(std::memory_order_acquire));
    }
    SECTION("fail_all") {
        std::atomic<bool> completed{false};
        require_bounded_completion([&] {
            exercise("fail_all");
            completed.store(true, std::memory_order_release);
        });
        CHECK(completed.load(std::memory_order_acquire));
    }
}

TEST_CASE("abandoned prepared invite write releases locks before observer callbacks",
          "[lock-order][prepared-write][redaction][fake-boundary][concurrency]") {
    auto runtime_owner = std::make_unique<test::ScriptedTdRuntime>();
    auto* runtime = runtime_owner.get();
    core::TdClient client(std::move(runtime_owner));
    const auto authorization = install_ready(client, *runtime);
    REQUIRE(authorization);
    REQUIRE(authorization->data.state == core::AuthState::Ready);

    const std::string invite = "https://t.me/+LockOrderPrepared";
    std::atomic<bool> armed{false};
    std::atomic<std::size_t> nested_preparations{0};
    const secure::WipeObserver observer = [&](std::string_view, const char*, std::size_t) {
        if (!armed.load(std::memory_order_acquire)) {
            return;
        }
        auto nested = client.prepare_direct_mutation(
            authorization, core::TdDirectRequest{
                               core::TdViewMessagesRequest{.chat_id = -1001, .message_ids = {1}}});
        if (nested) {
            nested_preparations.fetch_add(1, std::memory_order_relaxed);
        }
    };
    require_bounded_completion([&] {
        auto redaction = redaction::InviteLinkRegistry::instance().register_link(invite, observer);
        if (!redaction.valid()) {
            throw std::runtime_error("invite registration failed");
        }
        auto prepared = client.prepare_direct_mutation(
            authorization,
            core::TdDirectRequest{
                core::TdJoinChatRequest{std::nullopt,
                                        std::optional<secure::SensitiveString>{
                                            std::in_place, invite, observer, "td_join_invite"},
                                        std::nullopt}},
            redaction.protection());
        if (!prepared) {
            throw std::runtime_error("prepared invite write was rejected");
        }
        redaction.release();
        armed.store(true, std::memory_order_release);
        prepared = {};
    });

    CHECK(nested_preparations.load(std::memory_order_relaxed) > 0);
    client.close();
}
