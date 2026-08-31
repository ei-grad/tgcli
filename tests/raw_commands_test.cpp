#include "common/exit_codes.hpp"
#include "core/td_client.hpp"
#include "daemon/raw_commands.hpp"
#include "daemon/raw_contract.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <td/telegram/td_api.h>

using namespace std::chrono_literals;

namespace {

using nlohmann::json;

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    int exit_code = -1;
};

class TempState final {
  public:
    TempState() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "tgcli-raw-command-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
    }
    ~TempState() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    TempState(const TempState&) = delete;
    TempState& operator=(const TempState&) = delete;
    TempState(TempState&&) = delete;
    TempState& operator=(TempState&&) = delete;
    [[nodiscard]] const std::string& root() const noexcept {
        return root_;
    }

  private:
    std::string root_;
};

class FakeRaw final {
  public:
    FakeRaw() {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([&] {
            const auto state = client_->auth_state();
            return state && state->auth_sequence == 1 &&
                   state->data.state == tgcli::core::AuthState::Ready;
        }));
        coordinator_ = std::make_unique<tgcli::daemon::RawCoordinator>(*client_, "main",
                                                                       state_.root(), ::getuid());
    }

    std::future<Outcome>
    dispatch(tgcli::proto::Request request,
             std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig> admitted = {},
             tgcli::daemon::CallbackSink::ChallengeFn challenge = {}) {
        return std::async(std::launch::async, [this, request = std::move(request),
                                               admitted = std::move(admitted),
                                               challenge = std::move(challenge)]() mutable {
            Outcome outcome;
            const auto deadline = tgcli::request_deadline(request.context.timeout_seconds,
                                                          tgcli::DeadlineDefault::Default60);
            if (!deadline) {
                throw std::logic_error("raw fixture request has an invalid deadline");
            }
            tgcli::daemon::CallbackSink sink(
                [](const json&) {}, [](const json&) {},
                [&](json value) {
                    outcome.result = std::move(value);
                    outcome.exit_code = tgcli::kOk;
                },
                [&](std::string code, std::string message, json details, int exit_code) {
                    outcome.error = json{{"error",
                                          {{"code", std::move(code)},
                                           {"message", std::move(message)},
                                           {"details", std::move(details)}}}};
                    outcome.exit_code = exit_code;
                },
                std::move(challenge));
            tgcli::daemon::RequestSession session(std::move(request), sink, 0, {}, {},
                                                  std::move(admitted), deadline);
            coordinator_->execute(session.request(), session);
            return outcome;
        });
    }

    tgcli::core::TdFunctionData wait_for(tgcli::core::TdFunctionKind kind) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == kind);
        return sent.back().function;
    }

    template <typename Value> void respond(tgcli::core::TdFunctionKind kind, Value value) {
        wait_for(kind);
        const auto sent = runtime_->sent_functions();
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
    }

    void respond_me(bool bot = false) {
        respond(tgcli::core::TdFunctionKind::GetMe,
                tgcli::core::TdUserSummary{.id = 42,
                                           .first_name = "Ada",
                                           .last_name = "",
                                           .usernames = {"ada"},
                                           .phone_number = bot ? "" : "12025550123",
                                           .is_bot = bot});
    }

    void respond_chat(tgcli::core::TdChatKind kind = tgcli::core::TdChatKind::Supergroup) {
        respond(tgcli::core::TdFunctionKind::GetChat,
                tgcli::core::TdChat{.id = -1001,
                                    .title = "Project",
                                    .kind = kind,
                                    .related_id = 77,
                                    .tdlib_type_id = 1,
                                    .positions = {},
                                    .chat_lists = {},
                                    .last_message = std::nullopt,
                                    .permissions = std::nullopt,
                                    .notification_settings = std::nullopt});
    }

    template <typename Native> void respond_raw(td::td_api::object_ptr<Native> response) {
        wait_for(tgcli::core::TdFunctionKind::RawRead);
        const auto sent = runtime_->sent_functions();
        td::td_api::object_ptr<td::td_api::Object> object(std::move(response));
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(object)));
        ++sent_count_;
    }

    template <typename Native>
    void
    respond_raw_write(td::td_api::object_ptr<Native> response,
                      tgcli::core::TdFunctionKind kind = tgcli::core::TdFunctionKind::RawWrite) {
        wait_for(kind);
        const auto sent = runtime_->sent_functions();
        td::td_api::object_ptr<td::td_api::Object> object(std::move(response));
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(object)));
        ++sent_count_;
    }

    [[nodiscard]] std::size_t sent_count() const {
        return runtime_->sent_functions().size();
    }

    [[nodiscard]] const std::string& state_root() const {
        return state_.root();
    }

    void push_auth(tgcli::core::AuthState state) {
        runtime_->push_update(client_id_, {}, tgcli::core::AuthStateData{state});
    }

    [[nodiscard]] std::uint64_t last_query_id() const {
        return runtime_->sent_functions().back().query_id;
    }

    void finish_query(std::uint64_t query_id, tgcli::core::TdValue value) {
        runtime_->push_response(client_id_, query_id, std::move(value));
    }

  private:
    template <typename Predicate> static bool eventually(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    TempState state_;
    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient client_id_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::RawCoordinator> coordinator_;
    std::size_t sent_count_ = 1;
};

tgcli::proto::Request raw_request(std::string input, bool dry_run = false,
                                  tgcli::secure::WipeObserver wipe_observer = {}) {
    tgcli::proto::Request request("main", std::move(wipe_observer));
    request.command = {"raw"};
    request.args = {{"input", std::move(input)}};
    request.context.cwd = "/";
    request.context.timeout_seconds = 2.0;
    request.context.dry_run = dry_run;
    return request;
}

std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig> write_admission() {
    auto admitted = std::make_shared<tgcli::daemon::AdmittedAccountConfig>();
    admitted->account = "main";
    admitted->settings.allow_write = true;
    admitted->standing_write_grants_valid = true;
    return admitted;
}

} // namespace

TEST_CASE("public generic send rejects every raw tier before runtime submission",
          "[raw][core][generic-send][deny]") {
    auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    tgcli::core::TdClient client(std::move(runtime));
    REQUIRE(scripted->wait_for_sent(1));
    const auto client_id = scripted->clients().front();
    scripted->push_response(client_id, 1, {},
                            tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
    const auto ready_deadline = std::chrono::steady_clock::now() + 2s;
    while (client.auth_state()->data.state != tgcli::core::AuthState::Ready &&
           std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(client.auth_state()->data.state == tgcli::core::AuthState::Ready);

    constexpr std::array tiers{
        std::pair{tgcli::daemon::Tier::Read, tgcli::core::DescriptorKind::Read},
        std::pair{tgcli::daemon::Tier::Write, tgcli::core::DescriptorKind::Write},
        std::pair{tgcli::daemon::Tier::Destructive, tgcli::core::DescriptorKind::Destructive}};
    for (const auto& [tier, descriptor_tier] : tiers) {
        auto parsed = tgcli::daemon::raw::parse(R"({"@type":"cleanFileName","file_name":"a"})");
        REQUIRE(std::holds_alternative<tgcli::daemon::raw::TypedFunction>(parsed));
        auto& function = std::get<tgcli::daemon::raw::TypedFunction>(parsed);
        auto request = function.release_for_dispatch(tier);
        REQUIRE(request);
        REQUIRE(request->function_data());
        REQUIRE(request->function_data()->kind());
        auto future = client.send({.function = *request->function_data()->kind(),
                                   .tier = descriptor_tier,
                                   .owner = {},
                                   .client_generation = 0,
                                   .auth_sequence = 0,
                                   .auth_state = tgcli::core::AuthState::Unknown},
                                  std::move(*request));
        try {
            static_cast<void>(future.get());
            FAIL("raw generic send unexpectedly reached the runtime");
        } catch (const tgcli::core::TdAuthorizationError& error) {
            CHECK(error.failure() == tgcli::core::TdAuthorizationFailure::FunctionDenied);
        }
        CHECK(scripted->sent_functions().size() == 1);
    }
    client.close();
}

TEST_CASE("raw read uses one native function and emits exact typed response",
          "[raw][command][read][native]") {
    FakeRaw fake;
    auto pending = fake.dispatch(raw_request(R"({"@type":"cleanFileName","file_name":"a"})"));
    fake.respond_me();
    fake.respond_raw(td::td_api::make_object<td::td_api::text>("clean"));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK_FALSE(outcome.error);
    CHECK(*outcome.result == json{{"@type", "text"}, {"text", "clean"}});
}

TEST_CASE("raw denied policy remains auth-bound and sends no raw function",
          "[raw][command][denied][principal]") {
    FakeRaw fake;
    auto pending = fake.dispatch(raw_request(R"({"@type":"getChat","chat_id":-1001})"));
    fake.respond_me();
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "DENIED");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "function_denied");
    CHECK(fake.sent_count() == 2);
}

TEST_CASE("raw write dry-run performs non-secret preflight without authority audit or dispatch",
          "[raw][command][dry-run][preflight]") {
    FakeRaw fake;
    auto request = raw_request(
        R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})",
        true);
    auto pending = fake.dispatch(std::move(request));
    fake.respond_me();
    fake.respond_chat();
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["dry_run"] == true);
    CHECK((*outcome.result)["plan"]["tier"] == "write");
    CHECK(fake.sent_count() == 3);
    CHECK_FALSE(std::filesystem::exists(fake.state_root() + "/raw-audit-v3.jsonl"));
}

TEST_CASE("raw write persists hash-only audit before dispatch and canonical success",
          "[raw][command][write][audit]") {
    FakeRaw fake;
    auto request = raw_request(
        R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
    request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
    auto pending = fake.dispatch(std::move(request), write_admission());
    fake.respond_me();
    fake.respond_chat();
    fake.respond_raw_write(td::td_api::make_object<td::td_api::ok>());
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(*outcome.result == json{{"@type", "ok"}});
    std::ifstream input(fake.state_root() + "/raw-audit-v3.jsonl");
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.find("toggleChatIsMarkedAsUnread") != std::string::npos);
    CHECK(bytes.find("is_marked_as_unread") == std::string::npos);
    CHECK(bytes.find("raw_dispatch_started") < bytes.find("raw_response_received"));
    CHECK(bytes.find("raw_response_received") < bytes.find("raw_outcome"));
}

TEST_CASE("raw audited malformed native result seals unexpected response without resend",
          "[raw][command][audit][malformed][recovery]") {
    FakeRaw fake;
    auto request = raw_request(
        R"({"@type":"createForumTopic","chat_id":-1001,"name":"topic","is_name_implicit":false,"icon":{"@type":"forumTopicIcon","color":1,"custom_emoji_id":"0"}})");
    request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
    auto pending = fake.dispatch(std::move(request), write_admission());
    fake.respond_me();
    fake.respond_chat();
    fake.respond_raw_write(td::td_api::make_object<td::td_api::forumTopicInfo>(
        -1001, 7, std::string("\xff", 1), td::td_api::make_object<td::td_api::forumTopicIcon>(1, 0),
        1, td::td_api::make_object<td::td_api::messageSenderUser>(42), false, true, false, false,
        false));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "unexpected_response");
    CHECK(fake.sent_count() == 4);

    std::ifstream input(fake.state_root() + "/raw-audit-v3.jsonl");
    std::vector<json> stored;
    for (std::string line; std::getline(input, line);) {
        stored.push_back(json::parse(line));
    }
    REQUIRE(stored.size() == 4);
    CHECK(stored.at(2)["data"]["kind"] == "malformed");
    CHECK(stored.at(2)["data"]["response_sha256"].is_null());
    CHECK(stored.at(3)["terminal"]["reason"] == "unexpected_response");
    const tgcli::daemon::raw::audit_v3::Log log(fake.state_root(), ::getuid());
    CHECK(log.recover().status == tgcli::daemon::raw::audit_v3::LogStatus::Clean);
    CHECK(fake.sent_count() == 4);
}

TEST_CASE("raw oversized response caps canonical staging and persists body-free outcome",
          "[raw][command][audit][bounds][wipe]") {
    std::atomic<std::size_t> maximum_response_staging = 0;
    const tgcli::secure::WipeObserver observer =
        [&maximum_response_staging](std::string_view stage, const char*, std::size_t size) {
            if (stage == "raw_response_canonical") {
                maximum_response_staging.store(size, std::memory_order_relaxed);
            }
        };
    FakeRaw fake;
    auto request = raw_request(
        R"({"@type":"createForumTopic","chat_id":-1001,"name":"topic","is_name_implicit":false,"icon":{"@type":"forumTopicIcon","color":1,"custom_emoji_id":"0"}})",
        false, observer);
    request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
    auto pending = fake.dispatch(std::move(request), write_admission());
    fake.respond_me();
    fake.respond_chat();
    fake.respond_raw_write(td::td_api::make_object<td::td_api::forumTopicInfo>(
        -1001, 7, std::string(tgcli::daemon::raw::kMaximumResponseBytes + 1, 'x'),
        td::td_api::make_object<td::td_api::forumTopicIcon>(1, 0), 1,
        td::td_api::make_object<td::td_api::messageSenderUser>(42), false, true, false, false,
        false));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "result_too_large");
    CHECK(maximum_response_staging.load(std::memory_order_relaxed) ==
          tgcli::daemon::raw::kMaximumResponseBytes + 1);

    std::ifstream input(fake.state_root() + "/raw-audit-v3.jsonl", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.size() < 4096);
    CHECK(bytes.find("result_too_large") != std::string::npos);
    CHECK(bytes.find(std::string(128, 'x')) == std::string::npos);
    const tgcli::daemon::raw::audit_v3::Log log(fake.state_root(), ::getuid());
    CHECK(log.recover().status == tgcli::daemon::raw::audit_v3::LogStatus::Clean);
}

TEST_CASE("raw secret preflight denies before native dispatch", "[raw][command][secret]") {
    FakeRaw fake;
    auto pending = fake.dispatch(raw_request(
        R"({"@type":"getChatHistory","chat_id":-1001,"from_message_id":0,"offset":0,"limit":1,"only_local":false})"));
    fake.respond_me();
    fake.respond_chat(tgcli::core::TdChatKind::Secret);
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["details"]["reason"] == "secret_chat_unsupported");
    CHECK(fake.sent_count() == 3);
}

TEST_CASE("raw principal policy admits pinned transforms for bots and denies user-only rows",
          "[raw][command][principal][bot]") {
    SECTION("both principal") {
        FakeRaw fake;
        auto pending = fake.dispatch(raw_request(R"({"@type":"cleanFileName","file_name":"a"})"));
        fake.respond_me(true);
        fake.respond_raw(td::td_api::make_object<td::td_api::text>("clean"));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"@type", "text"}, {"text", "clean"}});
    }
    SECTION("user only") {
        FakeRaw fake;
        auto pending = fake.dispatch(raw_request(
            R"({"@type":"getChatHistory","chat_id":-1001,"from_message_id":0,"offset":0,"limit":1,"only_local":false})"));
        fake.respond_me(true);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "principal_unsupported");
        CHECK(fake.sent_count() == 2);
    }
}

TEST_CASE("raw write authority denies before audit and native dispatch",
          "[raw][command][authority][audit]") {
    FakeRaw fake;
    auto request = raw_request(
        R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
    request.context.write_authority = tgcli::proto::WriteAuthority::Deny;
    auto pending = fake.dispatch(std::move(request), write_admission());
    fake.respond_me();
    fake.respond_chat();
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "DENIED");
    CHECK((*outcome.error)["error"]["details"]["reason"] == "write_grant_required");
    CHECK(fake.sent_count() == 3);
    CHECK_FALSE(std::filesystem::exists(fake.state_root() + "/raw-audit-v3.jsonl"));
}

TEST_CASE("raw destructive confirmation rejects without dispatch and yes dispatches once",
          "[raw][command][destructive][confirmation][audit]") {
    SECTION("no tty") {
        FakeRaw fake;
        auto request = raw_request(R"({"@type":"leaveChat","chat_id":-1001})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
        CHECK((*outcome.error)["error"]["details"]["action"] == "raw");
        CHECK(fake.sent_count() == 3);
        CHECK_FALSE(std::filesystem::exists(fake.state_root() + "/raw-audit-v3.jsonl"));
    }
    SECTION("tty rejects") {
        FakeRaw fake;
        auto challenge_seen = std::make_shared<json>();
        auto request = raw_request(R"({"@type":"leaveChat","chat_id":-1001})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        request.context.tty = true;
        auto pending =
            fake.dispatch(std::move(request), write_admission(),
                          [challenge_seen](json challenge) -> std::optional<json> {
                              *challenge_seen = challenge;
                              return json{{"nonce", challenge["nonce"]},
                                          {"sequence", challenge["sequence"]},
                                          {"client_generation", challenge["client_generation"]},
                                          {"auth_sequence", challenge["auth_sequence"]},
                                          {"value", false}};
                          });
        fake.respond_me();
        fake.respond_chat();
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*challenge_seen)["details"]["action"] == "raw");
        CHECK((*challenge_seen)["details"]["target"]["operation"] == "raw");
        CHECK((*outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
        CHECK(fake.sent_count() == 3);
    }
    SECTION("yes") {
        FakeRaw fake;
        auto request = raw_request(R"({"@type":"leaveChat","chat_id":-1001})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        request.context.yes = true;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        fake.respond_raw_write(td::td_api::make_object<td::td_api::ok>(),
                               tgcli::core::TdFunctionKind::RawDestructive);
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(*outcome.result == json{{"@type", "ok"}});
    }
}

TEST_CASE("raw typed response maps wrong result and TD errors without TD messages",
          "[raw][command][response][error][audit]") {
    SECTION("wrong declared result") {
        FakeRaw fake;
        auto pending = fake.dispatch(raw_request(R"({"@type":"cleanFileName","file_name":"a"})"));
        fake.respond_me();
        fake.respond_raw(td::td_api::make_object<td::td_api::ok>());
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "unexpected_response");
    }
    SECTION("429") {
        FakeRaw fake;
        auto request = raw_request(
            R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        fake.respond_raw_write(td::td_api::make_object<td::td_api::error>(429, "retry after 17"));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "RATE_LIMITED");
        CHECK((*outcome.error)["error"]["details"]["tdlib_code"] == 429);
        CHECK(outcome.error->dump().find("retry after") == std::string::npos);
    }
    SECTION("other TD error") {
        FakeRaw fake;
        auto request = raw_request(
            R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        fake.respond_raw_write(td::td_api::make_object<td::td_api::error>(400, "secret detail"));
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK((*outcome.error)["error"]["details"]["tdlib_code"] == 400);
        CHECK(outcome.error->dump().find("secret detail") == std::string::npos);
    }
}

TEST_CASE("raw direct request validation rejects before Ready or native dispatch",
          "[raw][command][frame][usage]") {
    for (int variant = 0; variant < 5; ++variant) {
        DYNAMIC_SECTION(variant) {
            FakeRaw fake;
            auto request = raw_request("{}");
            if (variant == 0) {
                request.args = json::object();
            } else if (variant == 1) {
                request.args["extra"] = true;
            } else if (variant == 2) {
                request.args["input"] = 7;
            } else if (variant == 3) {
                request.context.idempotency_key = "key";
            } else {
                request.context.media_dir = "/tmp";
            }
            const auto outcome = fake.dispatch(std::move(request)).get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "USAGE");
            CHECK(fake.sent_count() == 1);
        }
    }
}

TEST_CASE("raw lifecycle arbitration is exact before and after durable dispatch",
          "[raw][command][lifecycle][audit]") {
    SECTION("authorization lost before response") {
        FakeRaw fake;
        auto pending = fake.dispatch(raw_request(R"({"@type":"cleanFileName","file_name":"a"})"));
        fake.wait_for(tgcli::core::TdFunctionKind::GetMe);
        const auto query_id = fake.last_query_id();
        fake.push_auth(tgcli::core::AuthState::WaitPhoneNumber);
        const auto outcome = pending.get();
        fake.finish_query(query_id, tgcli::core::TdValue::from(tgcli::core::TdUserSummary{}));
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "authorization_lost");
        CHECK_FALSE(std::filesystem::exists(fake.state_root() + "/raw-audit-v3.jsonl"));
    }
    SECTION("read deadline") {
        FakeRaw fake;
        auto request = raw_request(R"({"@type":"cleanFileName","file_name":"a"})");
        request.context.timeout_seconds = 0.02;
        auto pending = fake.dispatch(std::move(request));
        fake.wait_for(tgcli::core::TdFunctionKind::GetMe);
        const auto query_id = fake.last_query_id();
        const auto outcome = pending.get();
        fake.finish_query(query_id, tgcli::core::TdValue::from(tgcli::core::TdUserSummary{}));
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK_FALSE(std::filesystem::exists(fake.state_root() + "/raw-audit-v3.jsonl"));
    }
    SECTION("authorization lost after dispatch") {
        FakeRaw fake;
        auto request = raw_request(
            R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        fake.wait_for(tgcli::core::TdFunctionKind::RawWrite);
        const auto query_id = fake.last_query_id();
        fake.push_auth(tgcli::core::AuthState::WaitPhoneNumber);
        const auto outcome = pending.get();
        td::td_api::object_ptr<td::td_api::Object> late = td::td_api::make_object<td::td_api::ok>();
        fake.finish_query(query_id, tgcli::core::TdValue::from(std::move(late)));
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "RAW_OUTCOME_UNCONFIRMED");
        CHECK((*outcome.error)["error"]["details"]["mutation_state"] == "possible");
        std::ifstream input(fake.state_root() + "/raw-audit-v3.jsonl");
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        CHECK(bytes.find("RAW_OUTCOME_UNCONFIRMED") != std::string::npos);
    }
}

TEST_CASE("raw coordinator repairs every prior audit cut without resending it",
          "[raw][command][audit][recovery][no-resend]") {
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view token = "abcdef0123456789abcdef0123456789";
    constexpr std::string_view request_hash =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto intent = json{{"schema_version", 3},
                             {"record_type", "raw_intent"},
                             {"invocation_id", invocation},
                             {"function", "toggleChatIsMarkedAsUnread"},
                             {"tier", "write"},
                             {"tdlib_sha", "a17f87c4cff7b90b278d12b91ba0614383aaee82"},
                             {"request_sha256", request_hash},
                             {"request_bytes", 96}};
    const auto dispatch = json{{"schema_version", 3},
                               {"record_type", "raw_checkpoint"},
                               {"invocation_id", invocation},
                               {"stage", "raw_dispatch_started"},
                               {"data", {{"dispatch_token", token}, {"generation", "1"}}}};
    SECTION("intent only repairs none then current request dispatches once") {
        FakeRaw fake;
        const tgcli::daemon::raw::audit_v3::Log log(fake.state_root(), ::getuid());
        REQUIRE(log.append(intent));
        auto request = raw_request(
            R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        fake.respond_raw_write(td::td_api::make_object<td::td_api::ok>());
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(fake.sent_count() == 4);
        std::ifstream input(fake.state_root() + "/raw-audit-v3.jsonl");
        bool found_none = false;
        for (std::string line; std::getline(input, line);) {
            const auto record = json::parse(line);
            found_none =
                found_none || (record.value("invocation_id", "") == invocation &&
                               record.value("mutation_state", "") == "none" &&
                               record.contains("terminal") && record["terminal"].is_null());
        }
        CHECK(found_none);
    }
    SECTION("dispatch only seals and reports unconfirmed without a second raw send") {
        FakeRaw fake;
        const tgcli::daemon::raw::audit_v3::Log log(fake.state_root(), ::getuid());
        REQUIRE(log.append(intent));
        REQUIRE(log.append(dispatch));
        auto request = raw_request(
            R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
        request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        auto pending = fake.dispatch(std::move(request), write_admission());
        fake.respond_me();
        fake.respond_chat();
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "RAW_OUTCOME_UNCONFIRMED");
        CHECK((*outcome.error)["error"]["details"]["request_sha256"] == request_hash);
        CHECK(fake.sent_count() == 3);
    }
}

TEST_CASE("raw malformed on-disk terminal emits audit incomplete without dispatch or append",
          "[raw][command][audit][recovery][negative]") {
    constexpr std::string_view invocation = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view token = "abcdef0123456789abcdef0123456789";
    constexpr std::string_view request_hash =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto intent = json{{"schema_version", 3},
                             {"record_type", "raw_intent"},
                             {"invocation_id", invocation},
                             {"function", "toggleChatIsMarkedAsUnread"},
                             {"tier", "write"},
                             {"tdlib_sha", "a17f87c4cff7b90b278d12b91ba0614383aaee82"},
                             {"request_sha256", request_hash},
                             {"request_bytes", 96}};
    const auto dispatch = json{{"schema_version", 3},
                               {"record_type", "raw_checkpoint"},
                               {"invocation_id", invocation},
                               {"stage", "raw_dispatch_started"},
                               {"data", {{"dispatch_token", token}, {"generation", "1"}}}};
    const auto malformed_outcome = json{{"schema_version", 3},
                                        {"record_type", "raw_outcome"},
                                        {"invocation_id", invocation},
                                        {"mutation_state", "possible"},
                                        {"terminal", json::object()}};

    FakeRaw fake;
    const auto audit_path = fake.state_root() + "/raw-audit-v3.jsonl";
    std::ofstream output(audit_path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << intent.dump() << '\n' << dispatch.dump() << '\n' << malformed_outcome.dump() << '\n';
    output.close();
    REQUIRE(output.good());
    REQUIRE(::chmod(audit_path.c_str(), 0600) == 0);
    const auto original_bytes = std::filesystem::file_size(audit_path);

    auto request = raw_request(
        R"({"@type":"toggleChatIsMarkedAsUnread","chat_id":-1001,"is_marked_as_unread":true})");
    request.context.write_authority = tgcli::proto::WriteAuthority::Grant;
    auto pending = fake.dispatch(std::move(request), write_admission());
    fake.respond_me();
    fake.respond_chat();
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "AUDIT_INCOMPLETE");
    CHECK(fake.sent_count() == 3);
    CHECK(std::filesystem::file_size(audit_path) == original_bytes);
}
