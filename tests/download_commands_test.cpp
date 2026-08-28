#include "daemon/download_commands.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using nlohmann::json;

namespace {

struct Outcome {
    std::vector<json> progress;
    std::optional<json> result;
    std::optional<json> error;
    int terminal_count = 0;
};

class TempDirectory {
  public:
    TempDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "tgcli-download-command-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;
    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
    void write(std::string_view name, std::string_view value) const {
        const auto file = root_ / name;
        const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor < 0 ||
            ::write(descriptor, value.data(), value.size()) != static_cast<ssize_t>(value.size()) ||
            ::close(descriptor) != 0) {
            throw std::runtime_error("write fixture failed");
        }
    }

  private:
    std::filesystem::path root_;
};

class FakeDownload {
  public:
    explicit FakeDownload(
        std::shared_ptr<const tgcli::daemon::testing::DownloadFilesystemHooks> hooks = {}) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        client_id_ = runtime_->clients().front();
        runtime_->push_response(client_id_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([&] { return client_->auth_state()->auth_sequence == 1; }));
        coordinator_ = std::make_unique<tgcli::daemon::DownloadCoordinator>(*client_, "main",
                                                                            std::move(hooks));
    }

    std::future<Outcome> dispatch(tgcli::proto::Request request) {
        return std::async(std::launch::async, [this, request = std::move(request)]() mutable {
            Outcome outcome;
            tgcli::daemon::CallbackSink sink(
                [](const json&) {},
                [&](json value) { outcome.progress.push_back(std::move(value)); },
                [&](json value) {
                    ++outcome.terminal_count;
                    outcome.result = std::move(value);
                },
                [&](std::string code, std::string message, json details, int) {
                    ++outcome.terminal_count;
                    outcome.error = json{{"error",
                                          {{"code", std::move(code)},
                                           {"message", std::move(message)},
                                           {"details", std::move(details)}}}};
                });
            tgcli::daemon::RequestSession session(std::move(request), sink);
            coordinator_->download(session.request(), session);
            return outcome;
        });
    }

    template <typename T>
    tgcli::core::TdFunctionData respond(tgcli::core::TdFunctionKind expected, T value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        CHECK(sent.back().function.kind() == expected);
        const auto descriptor = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id,
                                tgcli::core::TdValue::from(std::move(value)));
        ++sent_count_;
        return descriptor;
    }

    void respond_me() {
        respond(tgcli::core::TdFunctionKind::GetMe,
                tgcli::core::TdUserSummary{.id = 42,
                                           .first_name = "Ada",
                                           .last_name = "",
                                           .usernames = {"ada"},
                                           .phone_number = "12025550123",
                                           .is_bot = false,
                                           .is_premium = false});
    }

    void respond_chat(tgcli::core::TdChatKind kind = tgcli::core::TdChatKind::BasicGroup) {
        respond(tgcli::core::TdFunctionKind::GetChat,
                tgcli::core::TdChat{.id = -1001,
                                    .title = "Project",
                                    .kind = kind,
                                    .related_id = 77,
                                    .tdlib_type_id = 1,
                                    .positions = {},
                                    .chat_lists = {},
                                    .is_marked_unread = false,
                                    .unread_count = 0,
                                    .unread_mention_count = 0,
                                    .unread_reaction_count = 0,
                                    .unread_poll_vote_count = 0,
                                    .last_message = std::nullopt,
                                    .permissions = std::nullopt,
                                    .notification_settings = std::nullopt});
    }

    void push_file(tgcli::core::TdFile file) {
        runtime_->push_update(
            client_id_, tgcli::core::TdValue::from(tgcli::core::TdUpdateFile{std::move(file)}));
    }

    bool push_file_and_wait_for_delivery(tgcli::core::TdFile file) {
        std::mutex mutex;
        std::condition_variable cv;
        bool delivered = false;
        const auto file_id = file.id;
        const auto subscription = client_->subscribe_updates(
            [&mutex, &cv, &delivered, file_id](const tgcli::core::TdValue& value) {
                if (const auto* update = value.get_if<tgcli::core::TdUpdateFile>();
                    update != nullptr && update->file.id == file_id) {
                    {
                        const std::lock_guard lock(mutex);
                        delivered = true;
                    }
                    cv.notify_all();
                }
            });
        push_file(std::move(file));
        std::unique_lock lock(mutex);
        const bool observed = cv.wait_for(lock, 2s, [&] { return delivered; });
        lock.unlock();
        client_->unsubscribe_updates(subscription);
        return observed;
    }

    void push_auth(tgcli::core::AuthState state) {
        runtime_->push_update(client_id_, {}, tgcli::core::AuthStateData{state});
    }

    void before_download_send(tgcli::core::TdFile file) {
        runtime_->set_before_send([this, file = std::move(file)](const auto& function) mutable {
            if (function.kind() == tgcli::core::TdFunctionKind::DownloadFile) {
                push_file(std::move(file));
            }
        });
    }

    [[nodiscard]] std::size_t count(tgcli::core::TdFunctionKind kind) const {
        return std::ranges::count_if(runtime_->sent_functions(), [&](const auto& sent) {
            return sent.function.kind() == kind;
        });
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

    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient client_id_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::DownloadCoordinator> coordinator_;
    std::size_t sent_count_ = 1;
};

tgcli::proto::Request request(const TempDirectory& temporary,
                              std::optional<std::string> output = "result.bin") {
    tgcli::proto::Request value("main");
    value.command = {"download"};
    value.args = {
        {"chat", "-1001"}, {"message_id", 91}, {"output", output ? json(*output) : json(nullptr)}};
    value.context.cwd = temporary.root().string();
    value.context.timeout_seconds = 2.0;
    return value;
}

tgcli::core::TdFile file(std::int64_t downloaded, bool complete, std::string source = {}) {
    return {.id = 7,
            .size = 7,
            .expected_size = 7,
            .local = tgcli::core::TdLocalFile{.path = std::move(source),
                                              .can_be_downloaded = !complete,
                                              .is_downloading_active = !complete,
                                              .is_downloading_completed = complete,
                                              .download_offset = 0,
                                              .downloaded_prefix_size = downloaded,
                                              .downloaded_size = downloaded}};
}

tgcli::core::TdDownloadMessage
message(tgcli::core::TdDownloadMediaKind kind = tgcli::core::TdDownloadMediaKind::Document) {
    return {.id = 91,
            .chat_id = -1001,
            .media_album_id = 0,
            .media_kind = kind,
            .primary_file =
                tgcli::core::TdFile{.id = 7, .size = 7, .expected_size = 7, .local = std::nullopt},
            .photo_sizes = {}};
}

} // namespace

TEST_CASE("download observer precedes TD submission and final progress precedes result",
          "[download][commands]") {
    const TempDirectory temporary;
    temporary.write("source.bin", "payload");
    FakeDownload fake;
    fake.before_download_send(file(3, false));
    auto pending = fake.dispatch(request(temporary));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage, message());
    fake.respond(tgcli::core::TdFunctionKind::DownloadFile, file(4, false));
    fake.push_file(file(7, true, (temporary.root() / "source.bin").string()));

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK_THAT(*outcome.result, tgcli::test::matches_json_schema("download.result.schema.json"));
    CHECK(outcome.terminal_count == 1);
    CHECK((*outcome.result)["chat_id"] == -1001);
    CHECK((*outcome.result)["message_id"] == 91);
    CHECK((*outcome.result)["file_id"] == 7);
    CHECK((*outcome.result)["media_type"] == "document");
    CHECK((*outcome.result)["path"] == (temporary.root() / "result.bin").string());
    CHECK((*outcome.result)["bytes"] == 7);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetSuggestedFileName) == 0);
    REQUIRE(outcome.progress.size() == 4);
    CHECK(outcome.progress.front()["downloaded_bytes"] == 3);
    CHECK(outcome.progress[1]["downloaded_bytes"] == 4);
    CHECK(outcome.progress[2]["downloaded_bytes"] == 7);
    CHECK(outcome.progress.back() == json{{"operation", "download"},
                                          {"file_id", 7},
                                          {"downloaded_bytes", 7},
                                          {"total_bytes", 7}});
}

TEST_CASE("download directory mode uses the exact suggested-name RPC", "[download][commands]") {
    const TempDirectory temporary;
    temporary.write("source.bin", "payload");
    FakeDownload fake;
    auto pending = fake.dispatch(request(temporary, std::nullopt));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage, message());
    const auto suggested = fake.respond(tgcli::core::TdFunctionKind::GetSuggestedFileName,
                                        tgcli::core::TdSuggestedFileName{"suggested.bin"});
    REQUIRE(suggested.fields().size() == 2);
    fake.respond(tgcli::core::TdFunctionKind::DownloadFile,
                 file(7, true, (temporary.root() / "source.bin").string()));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["path"] == (temporary.root() / "suggested.bin").string());
}

TEST_CASE("download zero-byte already-complete response emits only authoritative final progress",
          "[download][commands]") {
    const TempDirectory temporary;
    temporary.write("empty.bin", "");
    FakeDownload fake;
    auto pending = fake.dispatch(request(temporary));
    fake.respond_me();
    fake.respond_chat();
    auto empty_message = message();
    empty_message.primary_file->size = 0;
    empty_message.primary_file->expected_size = 0;
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage, std::move(empty_message));
    auto empty = file(0, true, (temporary.root() / "empty.bin").string());
    empty.size = 0;
    empty.expected_size = 0;
    fake.respond(tgcli::core::TdFunctionKind::DownloadFile, std::move(empty));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["bytes"] == 0);
    REQUIRE(outcome.progress.size() == 1);
    CHECK(outcome.progress.front() == json{{"operation", "download"},
                                           {"file_id", 7},
                                           {"downloaded_bytes", 0},
                                           {"total_bytes", 0}});
}

TEST_CASE("download unsupported wrapper stops before observer and local I/O",
          "[download][commands]") {
    const TempDirectory temporary;
    FakeDownload fake;
    auto pending = fake.dispatch(request(temporary));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage,
                 message(tgcli::core::TdDownloadMediaKind::PaidMedia));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("download.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "PRECONDITION_FAILED");
    CHECK((*outcome.error)["error"]["details"] == json{{"operation", "download"},
                                                       {"chat_id", -1001},
                                                       {"message_id", 91},
                                                       {"reason", "paid_media_unsupported"}});
    CHECK(fake.count(tgcli::core::TdFunctionKind::DownloadFile) == 0);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));
}

TEST_CASE("download direct-frame grammar rejects before Ready or TD calls",
          "[download][commands][frame]") {
    const TempDirectory temporary;
    FakeDownload fake;
    auto invalid = request(temporary);
    invalid.args["message_id"] = 0;
    auto pending = fake.dispatch(std::move(invalid));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("download.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "USAGE");
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetMe) == 0);
    CHECK(fake.count(tgcli::core::TdFunctionKind::GetDownloadMessage) == 0);
    CHECK(fake.count(tgcli::core::TdFunctionKind::DownloadFile) == 0);
}

TEST_CASE("download auth loss while observing emits exact terminal without publication",
          "[download][commands]") {
    const TempDirectory temporary;
    FakeDownload fake;
    auto pending = fake.dispatch(request(temporary));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage, message());
    fake.respond(tgcli::core::TdFunctionKind::DownloadFile, file(4, false));
    fake.push_auth(tgcli::core::AuthState::LoggingOut);
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("download.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"account", "main"}, {"state", "logging_out"}, {"reason", "authorization_lost"}});
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));
    CHECK(outcome.progress.size() == 1);
}

TEST_CASE("download wrong-id response is malformed and has no final progress",
          "[download][commands]") {
    const TempDirectory temporary;
    FakeDownload fake;
    auto pending = fake.dispatch(request(temporary));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage, message());
    auto wrong = file(7, true, (temporary.root() / "source.bin").string());
    wrong.id = 8;
    fake.respond(tgcli::core::TdFunctionKind::DownloadFile, std::move(wrong));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("download.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK(outcome.progress.empty());
    CHECK_FALSE(outcome.result);
}

TEST_CASE("download conflicting completion at publication gate fails closed and cleans temp",
          "[download][commands][race]") {
    const TempDirectory temporary;
    temporary.write("source.bin", "payload");
    auto hooks = std::make_shared<tgcli::daemon::testing::DownloadFilesystemHooks>();
    hooks->random_hex = [] { return std::string(32, 'a'); };
    FakeDownload* fake_pointer = nullptr;
    hooks->observe = [&](tgcli::daemon::DownloadFilesystemStage stage) {
        if (stage != tgcli::daemon::DownloadFilesystemStage::BeforePublish) {
            return;
        }
        REQUIRE(fake_pointer != nullptr);
        REQUIRE(fake_pointer->push_file_and_wait_for_delivery(
            file(7, true, (temporary.root() / "other.bin").string())));
    };
    FakeDownload fake(hooks);
    fake_pointer = &fake;
    auto pending = fake.dispatch(request(temporary));
    fake.respond_me();
    fake.respond_chat();
    fake.respond(tgcli::core::TdFunctionKind::GetDownloadMessage, message());
    fake.respond(tgcli::core::TdFunctionKind::DownloadFile,
                 file(7, true, (temporary.root() / "source.bin").string()));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, tgcli::test::matches_json_schema("download.error.schema.json"));
    CHECK((*outcome.error)["error"]["code"] == "INTERNAL");
    CHECK_FALSE(outcome.result);
    CHECK(outcome.progress.size() == 1);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));
}
