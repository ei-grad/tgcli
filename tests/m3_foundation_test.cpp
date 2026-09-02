#include "common/config.hpp"
#include "common/config_test_support.hpp"
#include "common/invite_redaction.hpp"
#include "core/td_log.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/message_summary.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "proto/destructive_plan.hpp"
#include "proto/frame.hpp"
#include "proto/frame_io.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using nlohmann::json;

namespace {

class PrivateTree final {
  public:
    explicit PrivateTree(std::string_view prefix) {
        std::string pattern = "/tmp/" + std::string(prefix) + "-XXXXXX";
        pattern.push_back('\0');
        const auto* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        REQUIRE(::chmod(root_.c_str(), 0700) == 0);
    }

    ~PrivateTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    PrivateTree(const PrivateTree&) = delete;
    PrivateTree& operator=(const PrivateTree&) = delete;
    PrivateTree(PrivateTree&&) = delete;
    PrivateTree& operator=(PrivateTree&&) = delete;

    [[nodiscard]] std::string directory(std::string_view name) const {
        const auto result = root_ + "/" + std::string(name);
        std::filesystem::create_directories(result);
        REQUIRE(::chmod(result.c_str(), 0700) == 0);
        return result;
    }

    [[nodiscard]] std::string file(std::string_view name) const {
        return root_ + "/" + std::string(name);
    }

  private:
    std::string root_;
};

void write_bytes(const std::string& filename, std::string_view bytes, mode_t mode = 0600) {
    std::ofstream output(filename, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(::chmod(filename.c_str(), mode) == 0);
}

std::string read_bytes(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

proto::Request request_with_blob(std::size_t bytes) {
    proto::Request request("main");
    request.id = 7;
    request.command = {"version"};
    request.args = {{"blob", std::string(bytes, 'x')}};
    request.context.cwd = "/tmp";
    return request;
}

bool write_all(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::send(descriptor, bytes.data(), bytes.size(), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

json challenge(std::string action, json target) {
    return {{"kind", "destructive_confirmation"},
            {"nonce", "00112233445566778899aabbccddeeff"},
            {"sequence", 1},
            {"client_generation", nullptr},
            {"auth_sequence", nullptr},
            {"secret", false},
            {"prompt", "Confirm? [y/N] "},
            {"details", {{"action", std::move(action)}, {"target", std::move(target)}}}};
}

json chat(std::string type = "supergroup") {
    return {{"id", -1001},
            {"title", "Project"},
            {"type", std::move(type)},
            {"is_bot", false},
            {"usernames", json::array({"project"})}};
}

} // namespace

TEST_CASE("request source bytes are exact and the socket/direct ceilings agree",
          "[m3-foundation][request-source]") {
    auto request = request_with_blob(32);
    const auto canonical = proto::serialize(proto::Frame{request});
    std::string error;
    auto admitted = proto::admit_request_source(request, error);
    INFO(error);
    REQUIRE(admitted);
    CHECK(admitted->source_bytes() == canonical.size());

    const auto padded = std::string(" \t") + canonical + "  ";
    auto parsed = proto::parse(padded, error);
    INFO(error);
    REQUIRE(parsed);
    const auto* parsed_request = std::get_if<proto::Request>(&*parsed);
    REQUIRE(parsed_request != nullptr);
    CHECK(parsed_request->source_bytes() == padded.size());

    const auto base = proto::serialize(proto::Frame{request_with_blob(0)}).size();
    REQUIRE(base < proto::kMaximumRequestSourceBytes);
    auto exact_request = request_with_blob(proto::kMaximumRequestSourceBytes - base);
    auto exact_source = proto::serialize(proto::Frame{exact_request});
    REQUIRE(exact_source.size() == proto::kMaximumRequestSourceBytes);
    auto exact_admitted = proto::admit_request_source(exact_request, error);
    REQUIRE(exact_admitted);

    auto immutable = *admitted;
    immutable.id = 99;
    immutable.account = "other";
    immutable.args["blob"] = "mutated";
    immutable.context.cwd = "/mutated";
    const auto immutable_copy = proto::admit_request_source(immutable, error);
    REQUIRE(immutable_copy);
    CHECK(immutable_copy->id == request.id);
    CHECK(immutable_copy->account == request.account);
    CHECK(immutable_copy->args == request.args);
    CHECK(immutable_copy->context.cwd == request.context.cwd);
    CHECK(proto::serialize(proto::Frame{immutable}) == canonical);

    auto oversized_request = request_with_blob(proto::kMaximumRequestSourceBytes - base + 1);
    CHECK_FALSE(proto::admit_request_source(oversized_request, error));

    std::array<int, 2> sockets{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0);
    std::atomic<bool> writer_ok{false};
    tgcli::cancellation::Thread writer([&] {
        exact_source.push_back('\n');
        writer_ok = write_all(sockets[0], exact_source);
        static_cast<void>(::shutdown(sockets[0], SHUT_WR));
    });
    proto::FrameReader reader(sockets[1]);
    const auto line = reader.read_line(error);
    INFO(error);
    REQUIRE(line);
    CHECK(line->size() == proto::kMaximumRequestSourceBytes);
    writer.join();
    CHECK(writer_ok);
    REQUIRE(::close(sockets[0]) == 0);
    REQUIRE(::close(sockets[1]) == 0);

    auto oversized_source = proto::serialize(proto::Frame{oversized_request});
    REQUIRE(oversized_source.size() == proto::kMaximumRequestSourceBytes + 1);
    std::array<int, 2> oversized_sockets{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, oversized_sockets.data()) == 0);
    std::atomic<bool> oversized_writer_ok{false};
    tgcli::cancellation::Thread oversized_writer([&] {
        oversized_source.push_back('\n');
        oversized_writer_ok = write_all(oversized_sockets[0], oversized_source);
        static_cast<void>(::shutdown(oversized_sockets[0], SHUT_WR));
    });
    proto::FrameReader oversized_reader(oversized_sockets[1]);
    CHECK_FALSE(oversized_reader.read_line(error));
    CHECK(error == "frame exceeds " + std::to_string(proto::kMaximumRequestSourceBytes) + " bytes");
    oversized_writer.join();
    CHECK(oversized_writer_ok);
    REQUIRE(::close(oversized_sockets[0]) == 0);
    REQUIRE(::close(oversized_sockets[1]) == 0);

    daemon::Dispatcher dispatcher;
    std::atomic<int> handlers{0};
    std::atomic<std::uint64_t> observed_source_bytes{0};
    dispatcher.register_command(
        "version",
        {daemon::Tier::Read, [&](const proto::Request&, daemon::RequestSession& session) {
             ++handlers;
             observed_source_bytes = session.request_source_bytes();
         }});
    daemon::CallbackSink sink([](const json&) {}, [](const json&) {}, [](const json&) {},
                              [](const std::string&, const std::string&, const json&, int) {});
    dispatcher.dispatch(request, sink);
    CHECK(observed_source_bytes == canonical.size());
    dispatcher.dispatch(*parsed_request, sink);
    CHECK(observed_source_bytes == padded.size());
    CHECK_THROWS_AS(dispatcher.dispatch(oversized_request, sink), std::invalid_argument);
    CHECK(handlers == 2);
}

TEST_CASE("verify-only config grant CAS reports every stable outcome without rewriting",
          "[m3-foundation][config-cas]") {
    const PrivateTree tree("tgcli-config-cas");
    const auto config_dir = tree.directory("tgcli");
    const auto config_path = config_dir + "/config.toml";
    const std::string granted = "[accounts.main]\nallow_write = true\n";
    write_bytes(config_path, granted);
    const config::Store store(config_path, ::getuid());
    const auto loaded = store.load();
    REQUIRE(loaded);
    const auto identity = loaded.snapshot->identity;

    auto verified = store.verify_write_grant(identity, "main");
    CHECK(verified.status == config::GrantVerificationStatus::Matched);
    CHECK(read_bytes(config_path) == granted);

    const std::string denied = "[accounts.main]\nallow_write = false\n";
    write_bytes(config_path, denied);
    const auto denied_load = store.load();
    REQUIRE(denied_load);
    CHECK(store.verify_write_grant(denied_load.snapshot->identity, "main").status ==
          config::GrantVerificationStatus::Denied);
    CHECK(store.verify_write_grant(identity, "main").status ==
          config::GrantVerificationStatus::Conflict);

    write_bytes(config_path, "[accounts.main\n");
    CHECK(store.verify_write_grant(denied_load.snapshot->identity, "main").status ==
          config::GrantVerificationStatus::Invalid);

    write_bytes(config_path, granted);
    const auto current = store.load();
    REQUIRE(current);
    const tgcli::cancellation::Source cancelled;
    static_cast<void>(cancelled.request_stop());
    CHECK(store
              .verify_write_grant(current.snapshot->identity, "main",
                                  config::MutationControl{std::nullopt, cancelled.get_token()})
              .status == config::GrantVerificationStatus::Cancelled);
    CHECK(store
              .verify_write_grant(current.snapshot->identity, "main",
                                  config::MutationControl{std::chrono::steady_clock::now(), {}})
              .status == config::GrantVerificationStatus::TimedOut);

    const auto lock_path = config_dir + "/config.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    REQUIRE(lock_fd >= 0);
    REQUIRE(::fchmod(lock_fd, 0600) == 0);
    REQUIRE(::flock(lock_fd, LOCK_EX | LOCK_NB) == 0);
    const auto blocked = store.verify_write_grant(
        current.snapshot->identity, "main",
        config::MutationControl{std::chrono::steady_clock::now() + 5ms, {}});
    CHECK(blocked.status == config::GrantVerificationStatus::TimedOut);
    REQUIRE(::flock(lock_fd, LOCK_UN) == 0);
    REQUIRE(::close(lock_fd) == 0);

    auto hooks = std::make_shared<config::testing::StoreHooks>();
    std::optional<std::chrono::steady_clock::time_point> hook_deadline;
    hooks->at_stage = [&](config::testing::MutationStage stage) {
        if (stage == config::testing::MutationStage::AfterLock) {
            REQUIRE(hook_deadline);
            std::this_thread::sleep_until(*hook_deadline + 1ms);
        }
    };
    const config::Store hooked_store(config_path, hooks, ::getuid());
    hook_deadline = std::chrono::steady_clock::now() + 20ms;
    CHECK(hooked_store
              .verify_write_grant(current.snapshot->identity, "main",
                                  config::MutationControl{hook_deadline, {}})
              .status == config::GrantVerificationStatus::TimedOut);

    REQUIRE(::chmod(config_dir.c_str(), 0755) == 0);
    CHECK(store.verify_write_grant(current.snapshot->identity, "main").status ==
          config::GrantVerificationStatus::IoError);
    REQUIRE(::chmod(config_dir.c_str(), 0700) == 0);
    CHECK(read_bytes(config_path) == granted);
}

TEST_CASE("invite redaction is correlated move-only and precedes concurrent log writes",
          "[m3-foundation][redaction][concurrency]") {
    const PrivateTree tree("tgcli-invite-redaction");
    const auto log_dir = tree.directory("logs");
    std::string error;
    auto sink = core::TdLogSink::create({.file_path = log_dir + "/tdlib.log", .max_file_size = 512},
                                        ::getuid(), error);
    INFO(error);
    REQUIRE(sink);
    const std::string invite_a = "https://t.me/+InviteSentinelAlpha";
    const std::string invite_b = "https://t.me/+InviteSentinelBeta";
    auto lease_a = redaction::InviteLinkRegistry::instance().register_link(invite_a);
    auto lease_b = redaction::InviteLinkRegistry::instance().register_link(invite_b);
    const std::array matrix_phases{"td_error",      "td_timeout",     "audit_failure",
                                   "store_failure", "crash_recovery", "release_race"};
    std::vector<std::string> matrix_links;
    std::vector<redaction::CorrelatedInviteLink> matrix_leases;
    std::vector<redaction::CorrelatedInviteLink> matrix_shadow_leases;
    matrix_links.reserve(matrix_phases.size());
    matrix_leases.reserve(matrix_phases.size());
    matrix_shadow_leases.reserve(matrix_phases.size());
    for (const auto* const phase : matrix_phases) {
        matrix_links.push_back("https://t.me/+InviteSentinel-" + std::string(phase));
        matrix_leases.push_back(
            redaction::InviteLinkRegistry::instance().register_link(matrix_links.back()));
        matrix_shadow_leases.push_back(
            redaction::InviteLinkRegistry::instance().register_link(matrix_links.back()));
        REQUIRE(matrix_leases.back().valid());
        REQUIRE(matrix_shadow_leases.back().valid());
    }
    REQUIRE(lease_a.valid());
    REQUIRE(lease_b.valid());
    auto moved = std::move(lease_a);

    std::atomic<bool> append_ok{true};
    std::barrier start(9);
    std::vector<std::thread> writers;
    writers.reserve(8);
    for (int index = 0; index < 8; ++index) {
        writers.emplace_back([&, index] {
            start.arrive_and_wait();
            for (int record = 0; record < 20; ++record) {
                std::string payload = std::to_string(index);
                payload.append(invite_a);
                payload.append(invite_b);
                payload.append(
                    matrix_links.at(static_cast<std::size_t>(record) % matrix_links.size()));
                payload.push_back('\n');
                std::string append_error;
                if (!sink->append(1, payload, append_error)) {
                    append_ok = false;
                }
            }
        });
    }
    std::thread release_racer([&] {
        start.arrive_and_wait();
        for (auto& lease : matrix_leases) {
            lease.release();
        }
    });
    for (auto& writer : writers) {
        writer.join();
    }
    release_racer.join();
    REQUIRE(append_ok);
    for (const auto& filename : sink->log_paths()) {
        const auto bytes = read_bytes(filename);
        CHECK(bytes.find(invite_a) == std::string::npos);
        CHECK(bytes.find(invite_b) == std::string::npos);
        for (const auto& link : matrix_links) {
            CHECK(bytes.find(link) == std::string::npos);
        }
    }
    moved.release();
    lease_b.release();
    for (auto& lease : matrix_shadow_leases) {
        lease.release();
    }
    CHECK(redaction::InviteLinkRegistry::instance().redact(invite_a) == invite_a);
}

TEST_CASE("M3 destructive challenges accept only strict neutral plans",
          "[m3-foundation][challenge]") {
    const json deletion{{"operation", "msg_delete"},          {"account", "main"},
                        {"tdlib_request", "deleteMessages"},  {"chat", chat()},
                        {"message_ids", json::array({1, 2})}, {"requested_for_all", true},
                        {"effective_for_all", true}};
    const json leave{{"operation", "chat_leave"},
                     {"account", "main"},
                     {"tdlib_request", "leaveChat"},
                     {"chat", chat()}};
    std::string error;
    CHECK(proto::validate_challenge_payload(challenge("msg_delete", deletion), error));
    CHECK(proto::validate_challenge_payload(challenge("chat_leave", leave), error));

    auto implicit_revoke = deletion;
    implicit_revoke["requested_for_all"] = false;
    CHECK_FALSE(proto::validate_challenge_payload(challenge("msg_delete", implicit_revoke), error));
    auto private_leave = leave;
    private_leave["chat"] = chat("private");
    CHECK_FALSE(proto::validate_challenge_payload(challenge("chat_leave", private_leave), error));
}

TEST_CASE("exact write selectors and persistence bounds do not narrow M2 materialization",
          "[m3-foundation][resolver][bounds]") {
    CHECK(daemon::classify_exact_write_selector("-1001") ==
          daemon::ExactWriteSelectorStatus::Exact);
    CHECK(daemon::classify_exact_write_selector("@project") ==
          daemon::ExactWriteSelectorStatus::Exact);
    CHECK(daemon::classify_exact_write_selector("https://t.me/project") ==
          daemon::ExactWriteSelectorStatus::Exact);
    CHECK(daemon::classify_exact_write_selector("Project Team") ==
          daemon::ExactWriteSelectorStatus::Title);

    daemon::ChatIdentity identity{
        -1001, std::string(1'048'576, 'x'), "supergroup", false, {"project"}};
    CHECK(daemon::persistable_chat_identity(identity));
    identity.title.push_back('x');
    CHECK_FALSE(daemon::persistable_chat_identity(identity));

    daemon::MessageSummary message;
    message.id = 1;
    message.chat_id = -1001;
    message.sender = {daemon::MessageSenderKind::User, 1};
    message.text = std::string(4'096, 'x');
    CHECK(daemon::persistable_message_summary(message));
    message.text.push_back('x');
    CHECK_FALSE(daemon::persistable_message_summary(message));
}
