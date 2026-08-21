#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "core/td_log.hpp"
#include "daemon/write_kernel.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

constexpr std::string_view kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
    "dev:1;ino:2;size:3;ctime_ns:4";
constexpr std::string_view kTimestamp = "2026-08-21T12:00:00Z";

std::string digest(char digit) {
    return "sha256:" + std::string(64, digit);
}

json chat(std::int64_t id = -1001, std::string title = "Project") {
    return {{"id", id},
            {"title", std::move(title)},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", json::array({"project"})}};
}

daemon::write_contract::Arguments archive_arguments() {
    std::string error;
    auto value = daemon::write_contract::make_arguments(proto::M3Operation::ChatArchive,
                                                        {{"chat", "@project"}}, error);
    INFO(error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::Plan archive_plan(std::int64_t id = -1001, std::string title = "Project") {
    std::string error;
    auto value = daemon::write_contract::make_plan(proto::M3Operation::ChatArchive, "main",
                                                   {{"operation", "chat_archive"},
                                                    {"account", "main"},
                                                    {"tdlib_request", "addChatToList"},
                                                    {"chat", chat(id, std::move(title))},
                                                    {"archived", true}},
                                                   error);
    INFO(error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::StoredTerminal archive_success(std::int64_t id = -1001) {
    std::string error;
    auto result = daemon::write_contract::make_result(proto::M3Operation::ChatArchive,
                                                      {{"chat_id", id}, {"archived", true}}, error);
    if (!result) {
        throw std::runtime_error(error);
    }
    auto terminal = daemon::write_contract::make_result_terminal(*result, error);
    if (!terminal) {
        throw std::runtime_error(error);
    }
    return std::move(*terminal);
}

daemon::write_contract::StoredTerminal archive_timeout() {
    std::string error;
    auto terminal = daemon::write_contract::make_error_terminal(proto::M3Operation::ChatArchive,
                                                                "TIMEOUT", "request timed out",
                                                                {{"operation", "chat_archive"},
                                                                 {"phase", "dispatch"},
                                                                 {"state", nullptr},
                                                                 {"outcome", "unknown"},
                                                                 {"idempotency", "pending"}},
                                                                kTimeout, error);
    INFO(error);
    REQUIRE(terminal);
    return std::move(*terminal);
}

daemon::write_contract::Arguments send_arguments() {
    std::string error;
    auto value = daemon::write_contract::make_arguments(proto::M3Operation::Send,
                                                        {{"chat", "@project"},
                                                         {"text", "hello"},
                                                         {"parse_mode", "plain"},
                                                         {"reply_to", nullptr},
                                                         {"topic", nullptr},
                                                         {"silent", false},
                                                         {"schedule", nullptr}},
                                                        error);
    INFO(error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::Plan send_plan() {
    std::string error;
    auto value = daemon::write_contract::make_plan(proto::M3Operation::Send, "main",
                                                   {{"operation", "send"},
                                                    {"account", "main"},
                                                    {"tdlib_request", "sendMessage"},
                                                    {"chat", chat()},
                                                    {"text", "hello"},
                                                    {"parse_mode", "plain"},
                                                    {"reply_to", nullptr},
                                                    {"requested_topic", nullptr},
                                                    {"effective_topic", nullptr},
                                                    {"silent", false},
                                                    {"schedule", nullptr},
                                                    {"observed_server_unix_time", nullptr}},
                                                   error);
    INFO(error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::StoredTerminal send_tdlib_error() {
    std::string error;
    auto terminal = daemon::write_contract::make_error_terminal(
        proto::M3Operation::Send, "TDLIB_ERROR", "Telegram request failed",
        {{"operation", "send"}, {"tdlib_code", 400}}, kGeneric, error);
    INFO(error);
    REQUIRE(terminal);
    return std::move(*terminal);
}

daemon::write_contract::Arguments delete_arguments() {
    std::string error;
    auto value = daemon::write_contract::make_arguments(
        proto::M3Operation::MsgDelete,
        {{"chat", "@project"}, {"message_ids", json::array({1, 2})}, {"for_all", true}}, error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::Plan delete_plan() {
    std::string error;
    auto value = daemon::write_contract::make_plan(proto::M3Operation::MsgDelete, "main",
                                                   {{"operation", "msg_delete"},
                                                    {"account", "main"},
                                                    {"tdlib_request", "deleteMessages"},
                                                    {"chat", chat()},
                                                    {"message_ids", json::array({1, 2})},
                                                    {"requested_for_all", true},
                                                    {"effective_for_all", true}},
                                                   error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::StoredTerminal delete_success() {
    std::string error;
    auto result = daemon::write_contract::make_result(proto::M3Operation::MsgDelete,
                                                      {{"chat_id", -1001},
                                                       {"message_ids", json::array({1, 2})},
                                                       {"for_all", true},
                                                       {"deleted", true}},
                                                      error);
    REQUIRE(result);
    auto terminal = daemon::write_contract::make_result_terminal(*result, error);
    REQUIRE(terminal);
    return std::move(*terminal);
}

daemon::write_contract::Arguments attach_arguments() {
    std::string error;
    auto value = daemon::write_contract::make_arguments(
        proto::M3Operation::SavedAttach,
        {{"message_id", 1}, {"path", "/tmp/input"}, {"caption", ""}}, error);
    REQUIRE(value);
    return std::move(*value);
}

json attachment_file() {
    return {{"path", "/tmp/input"},
            {"name", "input"},
            {"size", std::uint64_t{1}},
            {"sha256", digest('d')},
            {"device", std::uint64_t{1}},
            {"inode", std::uint64_t{2}},
            {"mtime_ns", 3},
            {"ctime_ns", 4}};
}

daemon::write_contract::Plan attach_plan() {
    std::string error;
    auto value = daemon::write_contract::make_plan(proto::M3Operation::SavedAttach, "main",
                                                   {{"operation", "saved_attach"},
                                                    {"account", "main"},
                                                    {"tdlib_request", "sendMessage"},
                                                    {"chat", chat()},
                                                    {"message_id", 1},
                                                    {"effective_topic", nullptr},
                                                    {"caption", ""},
                                                    {"file", attachment_file()}},
                                                   error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::StoredTerminal attach_success() {
    std::string error;
    auto result = daemon::write_contract::make_result(proto::M3Operation::SavedAttach,
                                                      {{"id", 101},
                                                       {"chat_id", -1001},
                                                       {"date", "2026-08-21T12:00:01Z"},
                                                       {"sender", {{"type", "user"}, {"id", 42}}},
                                                       {"is_outgoing", true},
                                                       {"topic", nullptr},
                                                       {"type", "doc"},
                                                       {"text", ""},
                                                       {"scheduled", false}},
                                                      error);
    REQUIRE(result);
    auto terminal = daemon::write_contract::make_result_terminal(*result, error);
    REQUIRE(terminal);
    return std::move(*terminal);
}

daemon::write_contract::StoredTerminal attach_input_changed() {
    std::string error;
    auto terminal = daemon::write_contract::make_error_terminal(
        proto::M3Operation::SavedAttach, "INPUT_CHANGED", "input file changed while being read",
        {{"operation", "saved_attach"}, {"path", "/tmp/input"}}, kGeneric, error);
    INFO(error);
    REQUIRE(terminal);
    return std::move(*terminal);
}

daemon::write_contract::Arguments forward_arguments() {
    std::string error;
    auto value = daemon::write_contract::make_arguments(proto::M3Operation::MsgForward,
                                                        {{"from", "@project"},
                                                         {"to", "@destination"},
                                                         {"message_ids", json::array({1})},
                                                         {"drop_author", false}},
                                                        error);
    INFO(error);
    REQUIRE(value);
    return std::move(*value);
}

daemon::write_contract::Plan forward_plan() {
    std::string error;
    auto value = daemon::write_contract::make_plan(proto::M3Operation::MsgForward, "main",
                                                   {{"operation", "msg_forward"},
                                                    {"account", "main"},
                                                    {"tdlib_request", "forwardMessages"},
                                                    {"from", chat()},
                                                    {"to", chat(-1002, "Destination")},
                                                    {"message_ids", json::array({1})},
                                                    {"drop_author", false}},
                                                   error);
    INFO(error);
    REQUIRE(value);
    return std::move(*value);
}

json forwarded_message() {
    auto message = attach_success().value()["data"];
    message["chat_id"] = -1002;
    message["type"] = "text";
    message["text"] = "forwarded";
    return message;
}

json forward_pending() {
    return json::array(
        {json{{"source_id", 1}, {"status", "pending"}, {"temporary_message_id", -1}}});
}

json forward_sent() {
    return json::array(
        {json{{"source_id", 1}, {"status", "sent"}, {"message", forwarded_message()}}});
}

daemon::write_contract::StoredTerminal forward_success() {
    std::string error;
    auto result = daemon::write_contract::make_result(
        proto::M3Operation::MsgForward,
        {{"from_chat_id", -1001}, {"to_chat_id", -1002}, {"items", forward_sent()}}, error);
    INFO(error);
    REQUIRE(result);
    auto terminal = daemon::write_contract::make_result_terminal(*result, error);
    INFO(error);
    REQUIRE(terminal);
    return std::move(*terminal);
}

std::string read_file(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

daemon::IdempotencyKeyHash key_hash(char digit = 'b') {
    auto value = daemon::parse_idempotency_key_hash(digest(digit));
    REQUIRE(value);
    return std::move(*value);
}

daemon::IdempotencyRequestFingerprint fingerprint(char digit = 'a') {
    auto value = daemon::parse_idempotency_request_fingerprint(digest(digit));
    REQUIRE(value);
    return std::move(*value);
}

std::function<std::uint64_t()> sample_at(std::uint64_t value) {
    return [value] { return value; };
}

class KernelTree final {
  public:
    explicit KernelTree(
        std::shared_ptr<const daemon::testing::AccountAuditHooks> audit_hooks = {},
        std::shared_ptr<const daemon::testing::IdempotencyStoreHooks> store_hooks = {}) {
        std::string pattern = "/tmp/tgcli-write-kernel-XXXXXX";
        pattern.push_back('\0');
        const auto* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        REQUIRE(std::filesystem::create_directory(root_ + "/accounts"));
        REQUIRE(::chmod((root_ + "/accounts").c_str(), 0700) == 0);
        state_ = root_ + "/accounts/main";
        REQUIRE(std::filesystem::create_directory(state_));
        REQUIRE(::chmod(state_.c_str(), 0700) == 0);
        std::string error;
        lease_ = daemon_lock::acquire_lifetime(state_ + "/daemon.lock", identity_, error);
        INFO(error);
        REQUIRE(lease_);
        auto created_foundation = daemon::IdempotencyFoundation::create(
            state_, "main", ::getuid(), lease_, std::move(audit_hooks), std::move(store_hooks));
        REQUIRE(std::holds_alternative<daemon::IdempotencyFoundation>(created_foundation));
        foundation_ = std::make_shared<daemon::IdempotencyFoundation>(
            std::get<daemon::IdempotencyFoundation>(std::move(created_foundation)));
    }

    ~KernelTree() {
        foundation_.reset();
        lease_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    KernelTree(const KernelTree&) = delete;
    KernelTree& operator=(const KernelTree&) = delete;
    KernelTree(KernelTree&&) = delete;
    KernelTree& operator=(KernelTree&&) = delete;

    [[nodiscard]] const std::shared_ptr<daemon::IdempotencyFoundation>& foundation() const {
        return foundation_;
    }

    [[nodiscard]] const std::string& state() const {
        return state_;
    }

  private:
    std::string root_;
    std::string state_;
    daemon_lock::Identity identity_;
    std::shared_ptr<daemon_lock::LifetimeLease> lease_;
    std::shared_ptr<daemon::IdempotencyFoundation> foundation_;
};

daemon::WritePostIntentPreparation
create_attachment_spool(const KernelTree& tree, const daemon::WriteKernelRequest& request) {
    const auto root = tree.state() + "/spool";
    const auto invocation = root + "/" + request.invocation_id;
    REQUIRE(std::filesystem::create_directory(root));
    REQUIRE(::chmod(root.c_str(), 0700) == 0);
    REQUIRE(std::filesystem::create_directory(invocation));
    REQUIRE(::chmod(invocation.c_str(), 0700) == 0);
    std::ofstream output(invocation + "/input", std::ios::binary);
    REQUIRE(output.good());
    output.put('x');
    output.close();
    REQUIRE(::chmod((invocation + "/input").c_str(), 0600) == 0);
    const auto file = attachment_file();
    return {daemon::SpoolRef{
                "spool/" + request.invocation_id + "/input",
                {file["path"].get<std::string>(), file["name"].get<std::string>(),
                 file["size"].get<std::uint64_t>(), file["sha256"].get<std::string>(),
                 file["device"].get<std::uint64_t>(), file["inode"].get<std::uint64_t>(),
                 file["mtime_ns"].get<std::int64_t>(), file["ctime_ns"].get<std::int64_t>()}},
            std::nullopt};
}

daemon::WriteKernelRequest archive_request(std::string invocation, std::uint64_t sampled_now,
                                           bool dry_run = false) {
    return {proto::M3Operation::ChatArchive,
            "main",
            dry_run ? std::optional<daemon::IdempotencyKeyHash>{}
                    : std::optional<daemon::IdempotencyKeyHash>{key_hash()},
            std::move(invocation),
            std::string(kTimestamp),
            "/tmp/config.toml",
            std::string(kSnapshot),
            daemon::AuthoritySource::Request,
            128,
            sample_at(sampled_now),
            dry_run,
            {},
            {},
            {}};
}

daemon::WriteKernelHooks archive_hooks(const daemon::write_contract::Plan& proposed,
                                       std::atomic<int>& dispatches, char fingerprint_digit = 'a') {
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [fingerprint_digit] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{archive_arguments(), fingerprint(fingerprint_digit), {}, {}}};
    };
    hooks.plan = [proposed](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{proposed};
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "addChatToList"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [&dispatches](const daemon::write_contract::Plan& plan,
                                   const daemon::WriteDispatchPreparation&,
                                   daemon::WriteDurableObservationSink&) {
        ++dispatches;
        return daemon::WriteDispatchOutcome{
            archive_success(plan.value()["chat"]["id"].get<std::int64_t>()),
            daemon::AccountAuditMutationState::Confirmed, true};
    };
    return hooks;
}

} // namespace

TEST_CASE("write kernel admits two initial misses but one commit winner",
          "[write-kernel][concurrency]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::barrier planners(2);
    std::atomic<int> dispatches{0};
    std::array<daemon::WriteKernelResult, 2> results;
    const auto first_request = archive_request("00000000000000000000000000000001", 1'700'000'000);
    const auto second_request = archive_request("00000000000000000000000000000002", 1'700'000'000);
    auto first_hooks = archive_hooks(archive_plan(-1001, "First"), dispatches);
    auto second_hooks = archive_hooks(archive_plan(-1002, "Second"), dispatches);
    const auto first_planner = first_hooks.plan;
    const auto second_planner = second_hooks.plan;
    first_hooks.plan = [&](const daemon::WriteAdmission& admission) {
        planners.arrive_and_wait();
        return first_planner(admission);
    };
    second_hooks.plan = [&](const daemon::WriteAdmission& admission) {
        planners.arrive_and_wait();
        return second_planner(admission);
    };
    std::jthread first_worker([&] { results.front() = kernel.run(first_request, first_hooks); });
    std::jthread second_worker([&] { results.back() = kernel.run(second_request, second_hooks); });
    first_worker.join();
    second_worker.join();
    CHECK(dispatches == 1);
    CHECK((results[0].status == daemon::WriteKernelStatus::Completed ||
           results[0].status == daemon::WriteKernelStatus::Replayed));
    CHECK((results[1].status == daemon::WriteKernelStatus::Completed ||
           results[1].status == daemon::WriteKernelStatus::Replayed));
    REQUIRE(results[0].terminal);
    REQUIRE(results[1].terminal);
    CHECK(*results[0].terminal == *results[1].terminal);
}

TEST_CASE("write admission and pass1 handoff occur inside the initial epoch",
          "[write-kernel][admission][ordering]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    const auto base_admit = hooks.admit;
    hooks.admit = [&] {
        auto contender = std::async(std::launch::async, [&] {
            return tree.foundation()->acquire_epoch(daemon::AccountAuditScanControl{
                RequestDeadline{RequestClock::now() + std::chrono::milliseconds(5)}, {}});
        });
        auto lock_result = contender.get();
        const auto* failure = std::get_if<daemon::AccountAuditFailure>(&lock_result);
        REQUIRE(failure != nullptr);
        CHECK(failure->interruption == daemon::AccountAuditFailure::Interruption::Deadline);
        return base_admit();
    };
    const auto base_plan = hooks.plan;
    const daemon::WriteAdmission* planned_admission = nullptr;
    hooks.plan = [&](const daemon::WriteAdmission& admission) {
        planned_admission = &admission;
        return base_plan(admission);
    };
    hooks.post_intent = [&](const daemon::write_contract::Plan&,
                            const daemon::WriteAdmission& admission) {
        CHECK(&admission == planned_admission);
        return daemon::WritePostIntentPreparation{};
    };
    const auto request = archive_request("00000000000000000000000000000003", 1'700'000'000);
    CHECK(kernel.run(request, hooks).status == daemon::WriteKernelStatus::Completed);
    CHECK(dispatches == 1);
}

TEST_CASE("write kernel resamples both epochs and hands off dispatch revalidation immutably",
          "[write-kernel][clock][dispatch][revalidation]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> samples{0};
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    json revalidated_proof;
    hooks.revalidate_auth_and_schedule = [&](const daemon::write_contract::Plan&) {
        CHECK(samples == 2);
        daemon::WriteDispatchPreparation preparation{
            {{"tdlib_function", "addChatToList"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
        revalidated_proof = preparation.proof;
        return daemon::WriteDispatchAdmissionOutcome{std::move(preparation)};
    };
    hooks.dispatch = [&](const daemon::write_contract::Plan& plan,
                         const daemon::WriteDispatchPreparation& preparation,
                         daemon::WriteDurableObservationSink&) {
        ++dispatches;
        CHECK(preparation.proof == revalidated_proof);
        return daemon::WriteDispatchOutcome{
            archive_success(plan.value()["chat"]["id"].get<std::int64_t>()),
            daemon::AccountAuditMutationState::Confirmed, true};
    };
    auto request = archive_request("00000000000000000000000000000006", 0);
    request.sample_now = [&] {
        return std::uint64_t{1'700'000'000} + static_cast<std::uint64_t>(samples.fetch_add(1));
    };

    CHECK(kernel.run(request, hooks).status == daemon::WriteKernelStatus::Completed);
    CHECK(samples == 2);
    CHECK(dispatches == 1);
}

TEST_CASE("dispatch observations are audit-first and store-second before later TD events",
          "[write-kernel][dispatch][observation][ordering]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    const daemon::WriteKernelRequest request{proto::M3Operation::MsgForward,
                                             "main",
                                             key_hash('4'),
                                             "00000000000000000000000000000004",
                                             std::string(kTimestamp),
                                             "/tmp/config.toml",
                                             std::string(kSnapshot),
                                             daemon::AuthoritySource::Request,
                                             128,
                                             sample_at(1'700'000'000),
                                             false,
                                             {},
                                             {},
                                             {}};
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{forward_arguments(), fingerprint('4'), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{forward_plan()};
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "forwardMessages"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [&](const daemon::write_contract::Plan&,
                         const daemon::WriteDispatchPreparation&,
                         daemon::WriteDurableObservationSink& observations) {
        REQUIRE(observations.temporary_message_ids(json::array({-1})));
        auto audit = read_file(tree.state() + "/audit.log");
        auto store = read_file(tree.state() + "/idempotency.db");
        CHECK(audit.find("\"temporary_message_ids\":[-1]") != std::string::npos);
        CHECK(store.find("\"temporary_message_ids\":[-1]") != std::string::npos);

        REQUIRE(observations.forward_progress(forward_pending()));
        audit = read_file(tree.state() + "/audit.log");
        store = read_file(tree.state() + "/idempotency.db");
        const auto pending_position = audit.find(R"("status":"pending")");
        CHECK(pending_position != std::string::npos);
        CHECK(store.find("\"status\":\"pending\"") != std::string::npos);

        REQUIRE(observations.forward_progress(forward_sent()));
        audit = read_file(tree.state() + "/audit.log");
        store = read_file(tree.state() + "/idempotency.db");
        const auto sent_position = audit.find(R"("status":"sent")");
        CHECK(sent_position != std::string::npos);
        CHECK(sent_position > pending_position);
        CHECK(store.find("\"status\":\"sent\"") != std::string::npos);
        return daemon::WriteDispatchOutcome{forward_success(),
                                            daemon::AccountAuditMutationState::Confirmed, true};
    };
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["kind"] == "result");
}

TEST_CASE("write admission retains invite redaction across durable kernel boundaries",
          "[write-kernel][redaction][lifetime]") {
    const KernelTree tree;
    const auto log_directory = tree.state() + "/logs";
    REQUIRE(std::filesystem::create_directory(log_directory));
    REQUIRE(::chmod(log_directory.c_str(), 0700) == 0);
    std::string error;
    auto sink = core::TdLogSink::create(
        {.file_path = log_directory + "/tdlib.log", .max_file_size = 512}, ::getuid(), error);
    INFO(error);
    REQUIRE(sink);

    const std::string sentinel = "https://t.me/+KernelInviteSentinel";
    const auto append_phase = [&](std::string_view phase) {
        std::string append_error;
        REQUIRE(sink->append(1, std::string(phase) + ":" + sentinel + "\n", append_error));
        INFO(append_error);
    };

    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    hooks.admit = [&] {
        std::vector<redaction::CorrelatedInviteLink> redactions;
        redactions.push_back(redaction::InviteLinkRegistry::instance().register_link(sentinel));
        REQUIRE(redactions.back().valid());
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{archive_arguments(), fingerprint(), {}, std::move(redactions)}};
    };
    hooks.before_insert = [&](const daemon::AccountAuditAppendReceipt&,
                              const daemon::AccountAuditCoordinator::Guard&) {
        append_phase("audit_failure");
    };
    hooks.post_intent = [&](const daemon::write_contract::Plan&, const daemon::WriteAdmission&) {
        append_phase("store_failure");
        return daemon::WritePostIntentPreparation{};
    };
    hooks.revalidate_auth_and_schedule = [&](const daemon::write_contract::Plan&) {
        append_phase("crash_recovery");
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "addChatToList"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [&](const daemon::write_contract::Plan&,
                         const daemon::WriteDispatchPreparation&,
                         daemon::WriteDurableObservationSink&) {
        ++dispatches;
        append_phase("td_error");
        append_phase("td_timeout");
        append_phase("release_race");
        return daemon::WriteDispatchOutcome{archive_timeout(),
                                            daemon::AccountAuditMutationState::Possible, false};
    };

    const auto request = archive_request("00000000000000000000000000000005", 1'700'000'000);
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    CHECK(dispatches == 1);
    CHECK(redaction::InviteLinkRegistry::instance().redact(sentinel) == sentinel);
    for (const auto& filename : sink->log_paths()) {
        CHECK(read_file(filename).find(sentinel) == std::string::npos);
    }
}

TEST_CASE("completed lookup adopts stored plan and pending conflict never confirm",
          "[write-kernel][idempotency]") {
    SECTION("completed retarget") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        auto first = archive_request("00000000000000000000000000000011", 1'700'000'000);
        auto hooks = archive_hooks(archive_plan(), dispatches);
        REQUIRE(kernel.run(first, hooks).status == daemon::WriteKernelStatus::Completed);

        int planners = 0;
        auto replay_hooks = archive_hooks(archive_plan(-2002, "Retargeted"), dispatches);
        replay_hooks.plan = [&](const daemon::WriteAdmission&) {
            ++planners;
            return daemon::WritePlanningOutcome{archive_plan(-2002, "Retargeted")};
        };
        auto second = archive_request("00000000000000000000000000000012", 1'700'000'001);
        const auto replay = kernel.run(second, replay_hooks);
        CHECK(replay.status == daemon::WriteKernelStatus::Replayed);
        CHECK(planners == 0);
        REQUIRE(replay.plan);
        CHECK(replay.plan->value()["chat"]["id"] == -1001);
        CHECK(dispatches == 1);
    }

    SECTION("pending and conflict") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        hooks.dispatch = [&dispatches](const daemon::write_contract::Plan&,
                                       const daemon::WriteDispatchPreparation&,
                                       daemon::WriteDurableObservationSink&) {
            ++dispatches;
            return daemon::WriteDispatchOutcome{archive_timeout(),
                                                daemon::AccountAuditMutationState::Possible, false};
        };
        auto first = archive_request("00000000000000000000000000000021", 1'700'000'000);
        REQUIRE(kernel.run(first, hooks).status == daemon::WriteKernelStatus::Completed);

        std::atomic<int> confirmations{0};
        auto lookup_hooks = archive_hooks(archive_plan(), dispatches);
        lookup_hooks.confirm = [&](const auto&, bool) {
            ++confirmations;
            return daemon::WriteConfirmationOutcome{daemon::WriteConfirmationStatus::ConfirmedYes,
                                                    std::nullopt};
        };
        auto pending = archive_request("00000000000000000000000000000022", 1'700'000'001);
        CHECK(kernel.run(pending, lookup_hooks).status == daemon::WriteKernelStatus::Pending);
        auto conflict = archive_request("00000000000000000000000000000023", 1'700'000'001);
        auto conflict_hooks = archive_hooks(archive_plan(), dispatches, 'c');
        CHECK(kernel.run(conflict, conflict_hooks).status == daemon::WriteKernelStatus::Conflict);
        CHECK(confirmations == 0);
        CHECK(dispatches == 1);
    }
}

TEST_CASE("write kernel expiry equality and post-intent cancellation preserve ordering",
          "[write-kernel][expiry][durability]") {
    SECTION("expiry equality") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        auto pending_hooks = archive_hooks(archive_plan(), dispatches);
        pending_hooks.dispatch = [&dispatches](const daemon::write_contract::Plan&,
                                               const daemon::WriteDispatchPreparation&,
                                               daemon::WriteDurableObservationSink&) {
            ++dispatches;
            return daemon::WriteDispatchOutcome{archive_timeout(),
                                                daemon::AccountAuditMutationState::Possible, false};
        };
        auto first = archive_request("00000000000000000000000000000031", 100);
        REQUIRE(kernel.run(first, pending_hooks).status == daemon::WriteKernelStatus::Completed);

        auto second = archive_request("00000000000000000000000000000032",
                                      100 + daemon::kIdempotencyRetentionSeconds);
        auto success_hooks = archive_hooks(archive_plan(-2002), dispatches);
        CHECK(kernel.run(second, success_hooks).status == daemon::WriteKernelStatus::Completed);
        CHECK(dispatches == 2);
    }

    SECTION("cancellation after intent") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<bool> cancelled{false};
        std::atomic<int> dispatches{0};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        hooks.revalidate_auth_and_schedule = [&](const daemon::write_contract::Plan&) {
            cancelled = true;
            return daemon::WriteDispatchPreparation{
                {{"tdlib_function", "addChatToList"},
                 {"dispatch_token", "0123456789abcdef0123456789abcdef"},
                 {"client_generation", std::uint64_t{7}}}};
        };
        auto request = archive_request("00000000000000000000000000000033", 1'700'000'000);
        request.cancelled = [&] { return cancelled.load(); };
        CHECK(kernel.run(request, hooks).status == daemon::WriteKernelStatus::Completed);
        CHECK(dispatches == 1);
    }

    SECTION("deadline after intent") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return daemon::WriteDispatchPreparation{
                {{"tdlib_function", "addChatToList"},
                 {"dispatch_token", "0123456789abcdef0123456789abcdef"},
                 {"client_generation", std::uint64_t{7}}}};
        };
        auto request = archive_request("00000000000000000000000000000034", 1'700'000'000);
        request.deadline = tgcli::RequestClock::now() + std::chrono::milliseconds(5);
        CHECK(kernel.run(request, hooks).status == daemon::WriteKernelStatus::Completed);
        CHECK(dispatches == 1);
    }
}

TEST_CASE("cancellation observed after dispatch retains an open unknown invocation",
          "[write-kernel][dispatch][cancellation][durability]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<bool> cancelled{false};
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    hooks.dispatch = [&](const daemon::write_contract::Plan& plan,
                         const daemon::WriteDispatchPreparation&,
                         daemon::WriteDurableObservationSink&) {
        ++dispatches;
        cancelled = true;
        return daemon::WriteDispatchOutcome{
            archive_success(plan.value()["chat"]["id"].get<std::int64_t>()),
            daemon::AccountAuditMutationState::Confirmed, true};
    };
    auto request = archive_request("00000000000000000000000000000035", 1'700'000'000);
    request.cancelled = [&] { return cancelled.load(); };

    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Rejected);
    CHECK_FALSE(result.terminal);
    CHECK(dispatches == 1);

    auto guard = tree.foundation()->acquire_epoch();
    const auto audit = tree.foundation()->audit().inspect(guard);
    CHECK(audit.status == daemon::AccountAuditInspectionStatus::Open);
    REQUIRE(audit.oldest_open);
    CHECK(audit.oldest_open->dispatch_started);
    const auto store = tree.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(store.snapshot.entries.size() == 1);
    CHECK(store.snapshot.entries.front().state == daemon::IdempotencyEntryState::Pending);
}

TEST_CASE("explicit send failure closes mutation-none without proof and removes pending",
          "[write-kernel][send][dispatch][mutation-none]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    const daemon::WriteKernelRequest request{proto::M3Operation::Send,
                                             "main",
                                             key_hash('7'),
                                             "00000000000000000000000000000036",
                                             std::string(kTimestamp),
                                             "/tmp/config.toml",
                                             std::string(kSnapshot),
                                             daemon::AuthoritySource::Request,
                                             128,
                                             sample_at(1'700'000'000),
                                             false,
                                             {},
                                             {},
                                             {}};
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{send_arguments(), fingerprint('7'), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{send_plan()};
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchAdmissionOutcome{daemon::WriteDispatchPreparation{
            {{"tdlib_function", "sendMessage"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}}};
    };
    hooks.dispatch = [](const daemon::write_contract::Plan&,
                        const daemon::WriteDispatchPreparation&,
                        daemon::WriteDurableObservationSink&) {
        return daemon::WriteDispatchOutcome{send_tdlib_error(),
                                            daemon::AccountAuditMutationState::None, false};
    };

    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["code"] == "TDLIB_ERROR");
    const auto audit_bytes = read_file(tree.state() + "/audit.log");
    CHECK(audit_bytes.find("\"mutation_state\":\"none\"") != std::string::npos);
    CHECK(audit_bytes.find("mutation_confirmed") == std::string::npos);

    auto guard = tree.foundation()->acquire_epoch();
    const auto store = tree.foundation()->store().inspect(guard);
    REQUIRE(store.status == daemon::IdempotencyInspectionStatus::Clean);
    CHECK(store.snapshot.entries.empty());
}

TEST_CASE("destructive completed replay freshly confirms its stored plan",
          "[write-kernel][confirmation]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    const daemon::WriteKernelRequest first{proto::M3Operation::MsgDelete,
                                           "main",
                                           key_hash(),
                                           "00000000000000000000000000000071",
                                           std::string(kTimestamp),
                                           "/tmp/config.toml",
                                           std::string(kSnapshot),
                                           daemon::AuthoritySource::Request,
                                           128,
                                           sample_at(1'700'000'000),
                                           false,
                                           {},
                                           {},
                                           {}};
    std::atomic<int> confirmations{0};
    std::atomic<int> dispatches{0};
    auto hooks = daemon::WriteKernelHooks{};
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{delete_arguments(), fingerprint(), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{delete_plan()};
    };
    hooks.confirm = [&](const daemon::write_contract::Plan& plan, bool replay) {
        CHECK(plan.value()["chat"]["title"] == "Project");
        CHECK(replay == (confirmations.load() != 0));
        ++confirmations;
        return daemon::WriteConfirmationOutcome{daemon::WriteConfirmationStatus::ConfirmedYes,
                                                std::nullopt};
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "deleteMessages"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [&](const daemon::write_contract::Plan&,
                         const daemon::WriteDispatchPreparation&,
                         daemon::WriteDurableObservationSink&) {
        ++dispatches;
        return daemon::WriteDispatchOutcome{delete_success(),
                                            daemon::AccountAuditMutationState::Confirmed, true};
    };
    REQUIRE(kernel.run(first, hooks).status == daemon::WriteKernelStatus::Completed);

    auto second = first;
    second.invocation_id = "00000000000000000000000000000072";
    second.sample_now = sample_at(1'700'000'001);
    int planners = 0;
    hooks.plan = [&](const daemon::WriteAdmission&) {
        ++planners;
        return daemon::WritePlanningOutcome{delete_plan()};
    };
    const auto replay = kernel.run(second, hooks);
    CHECK(replay.status == daemon::WriteKernelStatus::Replayed);
    CHECK(confirmations == 2);
    CHECK(dispatches == 1);
    CHECK(planners == 0);

    const auto audit_before_timeout = read_file(tree.state() + "/audit.log");
    const auto store_before_timeout = read_file(tree.state() + "/idempotency.db");
    auto deadline_replay = first;
    deadline_replay.invocation_id = "00000000000000000000000000000073";
    deadline_replay.sample_now = sample_at(1'700'000'002);
    deadline_replay.deadline = RequestClock::now() + std::chrono::milliseconds(50);
    hooks.confirm = [&](const daemon::write_contract::Plan&, bool replay_confirmation) {
        CHECK(replay_confirmation);
        std::this_thread::sleep_until(*deadline_replay.deadline.expires_at +
                                      std::chrono::milliseconds(1));
        return daemon::WriteConfirmationOutcome{daemon::WriteConfirmationStatus::ConfirmedYes,
                                                std::nullopt};
    };
    const auto timed_out = kernel.run(deadline_replay, hooks);
    CHECK(timed_out.status == daemon::WriteKernelStatus::Rejected);
    REQUIRE(timed_out.terminal);
    CHECK((*timed_out.terminal)["details"] == json{{"operation", "msg_delete"},
                                                   {"phase", "replay_confirmation"},
                                                   {"state", "ready"},
                                                   {"outcome", "not_started"},
                                                   {"idempotency", "completed_unchanged"}});
    CHECK(read_file(tree.state() + "/audit.log") == audit_before_timeout);
    CHECK(read_file(tree.state() + "/idempotency.db") == store_before_timeout);
}

TEST_CASE("write kernel repairs audit outcome and store lag before replay",
          "[write-kernel][fault][recovery]") {
    auto store_hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<int> file_syncs{0};
    store_hooks->sync = [&](daemon::IdempotencyStoreStage stage, int descriptor) {
        if (stage == daemon::IdempotencyStoreStage::BeforeTempFileSync && ++file_syncs == 2) {
            errno = EIO;
            return -1;
        }
        return ::fsync(descriptor);
    };
    const KernelTree tree({}, store_hooks);
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    auto first = archive_request("00000000000000000000000000000041", 1'700'000'000);
    const auto cut = kernel.run(first, hooks);
    CHECK(cut.status == daemon::WriteKernelStatus::AuditFatal);
    CHECK_FALSE(cut.terminal);
    CHECK(dispatches == 1);

    auto second = archive_request("00000000000000000000000000000042", 1'700'000'001);
    const auto repaired = kernel.run(second, hooks);
    CHECK(repaired.status == daemon::WriteKernelStatus::Replayed);
    CHECK(dispatches == 1);
}

TEST_CASE("write kernel dry-run performs planning without durability authority or dispatch",
          "[write-kernel][dry-run]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    std::atomic<int> persistence_hooks{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    hooks.verify_config_grant = [&](std::string_view, std::string_view,
                                    const config::MutationControl&) {
        ++persistence_hooks;
        return config::GrantVerificationResult{config::GrantVerificationStatus::Matched, {}, {}};
    };
    hooks.post_intent = [&](const daemon::write_contract::Plan&, const daemon::WriteAdmission&) {
        ++persistence_hooks;
        return daemon::WritePostIntentPreparation{};
    };
    hooks.before_insert = [&](const daemon::AccountAuditAppendReceipt&,
                              const daemon::AccountAuditCoordinator::Guard&) {
        ++persistence_hooks;
    };
    hooks.cleanup_spool = [&](daemon::AccountAuditSpoolHold,
                              const daemon::AccountAuditCoordinator::Guard&) {
        ++persistence_hooks;
        return daemon::AccountAuditSpoolCleanupCallResult{daemon::FileSpoolError{}};
    };
    hooks.cleanup = [&](const daemon::write_contract::Plan&, const daemon::WriteDispatchOutcome&) {
        ++persistence_hooks;
        return true;
    };
    auto request = archive_request("00000000000000000000000000000051", 1'700'000'000, true);
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::DryRunPlanned);
    CHECK(dispatches == 0);
    CHECK(persistence_hooks == 0);
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/audit.log"));
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/idempotency.db"));
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/spool"));
}

TEST_CASE("write kernel audit fsync cut owns no terminal", "[write-kernel][fault][audit]") {
    auto audit_hooks = std::make_shared<daemon::testing::AccountAuditHooks>();
    audit_hooks->should_fail = [](daemon::AccountAuditFault fault) {
        return fault == daemon::AccountAuditFault::FileSync;
    };
    const KernelTree tree(audit_hooks);
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    auto request = archive_request("00000000000000000000000000000061", 1'700'000'000);
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::AuditFatal);
    CHECK_FALSE(result.terminal);
    CHECK(dispatches == 0);
}

TEST_CASE("write kernel closes pass2 input changes without requiring a spool reference",
          "[write-kernel][post-intent][input-changed]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    const daemon::WriteKernelRequest request{proto::M3Operation::SavedAttach,
                                             "main",
                                             key_hash('e'),
                                             "00000000000000000000000000000092",
                                             std::string(kTimestamp),
                                             "/tmp/config.toml",
                                             std::string(kSnapshot),
                                             daemon::AuthoritySource::Request,
                                             128,
                                             sample_at(1'700'000'000),
                                             false,
                                             {},
                                             {},
                                             {}};
    std::atomic<int> dispatches{0};
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{attach_arguments(), fingerprint('d'), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{attach_plan()};
    };
    hooks.post_intent = [](const daemon::write_contract::Plan&, const daemon::WriteAdmission&) {
        return daemon::WritePostIntentPreparation{std::nullopt, attach_input_changed()};
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{};
    };
    hooks.dispatch = [&](const daemon::write_contract::Plan&,
                         const daemon::WriteDispatchPreparation&,
                         daemon::WriteDurableObservationSink&) {
        ++dispatches;
        return daemon::WriteDispatchOutcome{attach_success(),
                                            daemon::AccountAuditMutationState::Confirmed, true};
    };
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["code"] == "INPUT_CHANGED");
    CHECK(dispatches == 0);
}

TEST_CASE("write kernel contains post-intent exceptions without a dispatcher generic",
          "[write-kernel][post-intent][exception]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    std::atomic<int> shutdowns{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    hooks.audit_fatal_shutdown = [&] { ++shutdowns; };
    SECTION("before insert") {
        hooks.before_insert = [](const daemon::AccountAuditAppendReceipt&,
                                 const daemon::AccountAuditCoordinator::Guard&) {
            throw std::runtime_error("insert failure");
        };
    }
    SECTION("pass2") {
        hooks.post_intent =
            [](const daemon::write_contract::Plan&,
               const daemon::WriteAdmission&) -> daemon::WritePostIntentPreparation {
            throw std::runtime_error("post-intent failure");
        };
    }
    SECTION("dispatch proof") {
        hooks.revalidate_auth_and_schedule =
            [](const daemon::write_contract::Plan&) -> daemon::WriteDispatchPreparation {
            throw std::runtime_error("proof failure");
        };
    }
    SECTION("dispatch") {
        hooks.dispatch = [](const daemon::write_contract::Plan&,
                            const daemon::WriteDispatchPreparation&,
                            daemon::WriteDurableObservationSink&) -> daemon::WriteDispatchOutcome {
            throw std::runtime_error("dispatch failure");
        };
    }
    SECTION("audit timestamp") {
        hooks.timestamp = []() -> std::string { throw std::runtime_error("timestamp failure"); };
    }
    const auto request = archive_request("00000000000000000000000000000093", 1'700'000'000);
    std::optional<daemon::WriteKernelResult> result;
    CHECK_NOTHROW(result = kernel.run(request, hooks));
    REQUIRE(result);
    CHECK(result->status == daemon::WriteKernelStatus::AuditFatal);
    CHECK_FALSE(result->terminal);
    CHECK(dispatches == 0);
    CHECK(shutdowns == 1);
}

TEST_CASE("write kernel emits exact preflight timeout after waits and config CAS",
          "[write-kernel][timeout][config-cas]") {
    SECTION("outer epoch wait") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        auto held = tree.foundation()->acquire_epoch();
        std::atomic<int> dispatches{0};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        auto request = archive_request("00000000000000000000000000000094", 1'700'000'000);
        request.deadline = RequestClock::now() + std::chrono::milliseconds(5);
        const auto result = kernel.run(request, hooks);
        CHECK(result.status == daemon::WriteKernelStatus::Rejected);
        REQUIRE(result.terminal);
        CHECK((*result.terminal)["details"] == json{{"operation", "chat_archive"},
                                                    {"phase", "preflight"},
                                                    {"state", "ready"},
                                                    {"outcome", "not_started"},
                                                    {"idempotency", "not_created"}});
        CHECK(dispatches == 0);
    }

    SECTION("config CAS completion at the deadline") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        hooks.verify_config_grant = [](std::string_view, std::string_view,
                                       const config::MutationControl& control) {
            REQUIRE(control.deadline);
            std::this_thread::sleep_until(*control.deadline + std::chrono::milliseconds(1));
            return config::GrantVerificationResult{
                config::GrantVerificationStatus::Matched, {}, {}};
        };
        auto request = archive_request("00000000000000000000000000000095", 1'700'000'000);
        request.authority_source = daemon::AuthoritySource::Config;
        request.deadline = RequestClock::now() + std::chrono::milliseconds(100);
        const auto result = kernel.run(request, hooks);
        CHECK(result.status == daemon::WriteKernelStatus::Rejected);
        REQUIRE(result.terminal);
        CHECK((*result.terminal)["details"] == json{{"operation", "chat_archive"},
                                                    {"phase", "preflight"},
                                                    {"state", "ready"},
                                                    {"outcome", "not_started"},
                                                    {"idempotency", "not_created"}});
        CHECK(dispatches == 0);
    }

    SECTION("spool inspection deadline is not a spool durability error") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        auto spool_hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        std::optional<RequestClock::time_point> deadline;
        spool_hooks->at_stage = [&](daemon::FileSpoolStage stage) {
            if (stage == daemon::FileSpoolStage::BeforeRootInspect) {
                REQUIRE(deadline);
                std::this_thread::sleep_until(*deadline + std::chrono::milliseconds(1));
            }
        };
        hooks.spool_hooks = spool_hooks;
        auto request = archive_request("00000000000000000000000000000097", 1'700'000'000);
        request.deadline = RequestClock::now() + std::chrono::milliseconds(100);
        deadline = request.deadline.expires_at;
        const auto result = kernel.run(request, hooks);
        CHECK(result.status == daemon::WriteKernelStatus::Rejected);
        REQUIRE(result.terminal);
        CHECK((*result.terminal)["code"] == "TIMEOUT");
        CHECK((*result.terminal)["details"]["phase"] == "preflight");
        CHECK(dispatches == 0);
    }

    SECTION("spool inspection cancellation emits no spool error") {
        const KernelTree tree;
        const daemon::WriteKernel kernel(tree.foundation());
        std::atomic<int> dispatches{0};
        std::atomic<bool> cancelled{false};
        auto hooks = archive_hooks(archive_plan(), dispatches);
        auto spool_hooks = std::make_shared<daemon::testing::FileSpoolHooks>();
        spool_hooks->at_stage = [&](daemon::FileSpoolStage stage) {
            if (stage == daemon::FileSpoolStage::BeforeRootInspect) {
                cancelled = true;
            }
        };
        hooks.spool_hooks = spool_hooks;
        auto request = archive_request("00000000000000000000000000000098", 1'700'000'000);
        request.cancelled = [&] { return cancelled.load(); };
        const auto result = kernel.run(request, hooks);
        CHECK(result.status == daemon::WriteKernelStatus::Rejected);
        CHECK_FALSE(result.terminal);
        CHECK(dispatches == 0);
    }
}

TEST_CASE("unexpected post-intent insert loss closes mutation-none and preserves incumbent",
          "[write-kernel][fault][insert]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    std::atomic<int> dispatches{0};
    auto hooks = archive_hooks(archive_plan(), dispatches);
    hooks.before_insert = [&](const daemon::AccountAuditAppendReceipt& receipt,
                              const daemon::AccountAuditCoordinator::Guard& guard) {
        auto incumbent = daemon::make_idempotency_pending_entry(
            {key_hash(), fingerprint(), daemon::AccountAuditOperation::ChatArchive,
             "ffffffffffffffffffffffffffffffff", receipt.audit_generation, 1'700'000'000,
             archive_plan().value()},
            "main", tree.foundation()->store().path());
        REQUIRE(std::holds_alternative<daemon::IdempotencyEntry>(incumbent));
        const auto inserted = tree.foundation()->store().insert_if_absent(
            std::get<daemon::IdempotencyEntry>(incumbent), guard);
        REQUIRE(inserted.status == daemon::IdempotencyInsertStatus::Inserted);
    };
    auto request = archive_request("00000000000000000000000000000081", 1'700'000'000);
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::DurabilityFatal);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["code"] == "INTERNAL");
    CHECK(dispatches == 0);

    auto guard = tree.foundation()->acquire_epoch();
    const auto inspection = tree.foundation()->store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(inspection.snapshot.entries.size() == 1);
    CHECK(inspection.snapshot.entries.front().invocation_id == "ffffffffffffffffffffffffffffffff");
}

TEST_CASE("saved attachment cleanup releases its receipt before clearing completed store spool",
          "[write-kernel][spool][cleanup]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    daemon::WriteKernelRequest request{proto::M3Operation::SavedAttach,
                                       "main",
                                       key_hash('e'),
                                       "00000000000000000000000000000091",
                                       std::string(kTimestamp),
                                       "/tmp/config.toml",
                                       std::string(kSnapshot),
                                       daemon::AuthoritySource::Request,
                                       128,
                                       sample_at(1'700'000'000),
                                       false,
                                       {},
                                       {},
                                       {}};
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{attach_arguments(), fingerprint('d'), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{attach_plan()};
    };
    hooks.post_intent = [&](const daemon::write_contract::Plan&, const daemon::WriteAdmission&) {
        return create_attachment_spool(tree, request);
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "sendMessage"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [](const daemon::write_contract::Plan&,
                        const daemon::WriteDispatchPreparation&,
                        daemon::WriteDurableObservationSink&) {
        return daemon::WriteDispatchOutcome{attach_success(),
                                            daemon::AccountAuditMutationState::Confirmed, true};
    };
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/spool/" + request.invocation_id));
    auto guard = tree.foundation()->acquire_epoch();
    const auto inspection = tree.foundation()->store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(inspection.snapshot.entries.size() == 1);
    CHECK(inspection.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    CHECK_FALSE(inspection.snapshot.entries.front().spool);
}

TEST_CASE("durable terminal survives attachment cleanup and reference-clear failure",
          "[write-kernel][spool][cleanup][terminal-ownership]") {
    const KernelTree tree;
    const daemon::WriteKernel kernel(tree.foundation());
    daemon::WriteKernelRequest request{proto::M3Operation::SavedAttach,
                                       "main",
                                       key_hash('1'),
                                       "00000000000000000000000000000096",
                                       std::string(kTimestamp),
                                       "/tmp/config.toml",
                                       std::string(kSnapshot),
                                       daemon::AuthoritySource::Request,
                                       128,
                                       sample_at(1'700'000'000),
                                       false,
                                       {},
                                       {},
                                       {}};
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{attach_arguments(), fingerprint('f'), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{attach_plan()};
    };
    hooks.post_intent = [&](const daemon::write_contract::Plan&, const daemon::WriteAdmission&) {
        return create_attachment_spool(tree, request);
    };
    hooks.cleanup_spool = [](daemon::AccountAuditSpoolHold,
                             const daemon::AccountAuditCoordinator::Guard&)
        -> daemon::AccountAuditSpoolCleanupCallResult {
        throw std::runtime_error("cleanup failure");
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "sendMessage"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [](const daemon::write_contract::Plan&,
                        const daemon::WriteDispatchPreparation&,
                        daemon::WriteDurableObservationSink&) {
        return daemon::WriteDispatchOutcome{attach_success(),
                                            daemon::AccountAuditMutationState::Confirmed, true};
    };
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["kind"] == "result");
    auto guard = tree.foundation()->acquire_epoch();
    const auto inspection = tree.foundation()->store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(inspection.snapshot.entries.size() == 1);
    CHECK(inspection.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    CHECK(inspection.snapshot.entries.front().spool);
}

TEST_CASE("durable terminal survives completed attachment reference-clear fsync failure",
          "[write-kernel][spool][store][terminal-ownership]") {
    auto store_hooks = std::make_shared<daemon::testing::IdempotencyStoreHooks>();
    std::atomic<int> file_syncs{0};
    store_hooks->sync = [&](daemon::IdempotencyStoreStage stage, int descriptor) {
        if (stage == daemon::IdempotencyStoreStage::BeforeTempFileSync && ++file_syncs == 4) {
            errno = EIO;
            return -1;
        }
        return ::fsync(descriptor);
    };
    const KernelTree tree({}, store_hooks);
    const daemon::WriteKernel kernel(tree.foundation());
    daemon::WriteKernelRequest request{proto::M3Operation::SavedAttach,
                                       "main",
                                       key_hash('2'),
                                       "00000000000000000000000000000099",
                                       std::string(kTimestamp),
                                       "/tmp/config.toml",
                                       std::string(kSnapshot),
                                       daemon::AuthoritySource::Request,
                                       128,
                                       sample_at(1'700'000'000),
                                       false,
                                       {},
                                       {},
                                       {}};
    daemon::WriteKernelHooks hooks;
    hooks.audit_fatal_shutdown = [] {};
    hooks.admit = [] {
        return daemon::WriteAdmissionOutcome{
            daemon::WriteAdmission{attach_arguments(), fingerprint('2'), {}, {}}};
    };
    hooks.plan = [](const daemon::WriteAdmission&) {
        return daemon::WritePlanningOutcome{attach_plan()};
    };
    hooks.post_intent = [&](const daemon::write_contract::Plan&, const daemon::WriteAdmission&) {
        return create_attachment_spool(tree, request);
    };
    hooks.revalidate_auth_and_schedule = [](const daemon::write_contract::Plan&) {
        return daemon::WriteDispatchPreparation{
            {{"tdlib_function", "sendMessage"},
             {"dispatch_token", "0123456789abcdef0123456789abcdef"},
             {"client_generation", std::uint64_t{7}}}};
    };
    hooks.dispatch = [](const daemon::write_contract::Plan&,
                        const daemon::WriteDispatchPreparation&,
                        daemon::WriteDurableObservationSink&) {
        return daemon::WriteDispatchOutcome{attach_success(),
                                            daemon::AccountAuditMutationState::Confirmed, true};
    };
    const auto result = kernel.run(request, hooks);
    CHECK(result.status == daemon::WriteKernelStatus::Completed);
    REQUIRE(result.terminal);
    CHECK((*result.terminal)["kind"] == "result");
    CHECK_FALSE(std::filesystem::exists(tree.state() + "/spool/" + request.invocation_id));
    auto guard = tree.foundation()->acquire_epoch();
    const auto inspection = tree.foundation()->store().inspect(guard);
    REQUIRE(inspection.status == daemon::IdempotencyInspectionStatus::Clean);
    REQUIRE(inspection.snapshot.entries.size() == 1);
    CHECK(inspection.snapshot.entries.front().state == daemon::IdempotencyEntryState::Completed);
    CHECK(inspection.snapshot.entries.front().spool);
}
