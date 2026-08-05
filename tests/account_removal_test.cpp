#include "daemon/account_removal.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::daemon;

namespace {

class TempRemoval final {
  public:
    TempRemoval() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-account-remove-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        environment_.home = root_.string();
        environment_.xdg_config_home = (root_ / "config").string();
        environment_.xdg_data_home = (root_ / "data").string();
        environment_.xdg_state_home = (root_ / "state").string();
        environment_.uid = ::getuid();
        private_directory(root_ / "config" / "tgcli");
        private_directory(root_ / "data" / "tgcli" / "accounts" / "work");
        private_directory(root_ / "state" / "tgcli" / "accounts" / "work");
        { std::ofstream(data_root() / "database") << "data"; }
        { std::ofstream(state_root() / "audit.log") << "state"; }
        write_config();
    }

    ~TempRemoval() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempRemoval(const TempRemoval&) = delete;
    TempRemoval& operator=(const TempRemoval&) = delete;
    TempRemoval(TempRemoval&&) = delete;
    TempRemoval& operator=(TempRemoval&&) = delete;

    [[nodiscard]] tgcli::config::Store store() const {
        return tgcli::config::Store(config_file().string(), environment_.uid);
    }

    [[nodiscard]] RemovalJournal
    journal(std::shared_ptr<const testing::RemovalJournalHooks> hooks = {}) const {
        return {tgcli::paths::removals_state_dir(environment_), environment_.uid, std::move(hooks)};
    }

    [[nodiscard]] const tgcli::paths::Environment& environment() const {
        return environment_;
    }

    [[nodiscard]] std::filesystem::path data_root() const {
        return root_ / "data" / "tgcli" / "accounts" / "work";
    }

    [[nodiscard]] std::filesystem::path state_root() const {
        return root_ / "state" / "tgcli" / "accounts" / "work";
    }

  private:
    void private_directory(const std::filesystem::path& directory) const {
        std::filesystem::create_directories(directory);
        auto current = directory;
        while (current != root_ && current.string().starts_with(root_.string())) {
            REQUIRE(::chmod(current.c_str(), 0700) == 0);
            current = current.parent_path();
        }
    }

    [[nodiscard]] std::filesystem::path config_file() const {
        return root_ / "config" / "tgcli" / "config.toml";
    }

    void write_config() const {
        std::ofstream output(config_file(), std::ios::binary);
        REQUIRE(output.good());
        output << "default_account = \"work\"\n[accounts.work]\nallow_write = true\n";
        output.close();
        REQUIRE(::chmod(config_file().c_str(), 0600) == 0);
    }

    std::filesystem::path root_;
    tgcli::paths::Environment environment_;
};

class FakeRemote final : public AccountRemovalRemote {
  public:
    enum class Proof { NotPresent, Confirmed, MissingCheckpoint, FailBeforeSend, FailAfterSend };

    RemovalRemoteProof
    prove_remote_logout(const tgcli::proto::AccountRemovePlan& /*plan*/,
                        const std::shared_ptr<const tgcli::config::ConfigSnapshot>&
                        /*config_snapshot*/,
                        bool send_checkpointed, RequestSession& /*session*/,
                        const RemovalCheckpoint& checkpoint) override {
        ++proof_calls;
        if (proof == Proof::FailBeforeSend) {
            return RemovalOperationError{
                "REMOTE_LOGOUT_UNCONFIRMED",
                "remote logout failed",
                {{"account", "work"}, {"state", "unknown"}, {"reason", "state_unproven"}},
                1};
        }
        if (proof == Proof::NotPresent) {
            if (!checkpoint(AuditStage::RemoteNotPresent)) {
                return RemovalOperationError{
                    "INTERNAL",
                    "checkpoint failed",
                    {{"operation", "account_remove"}, {"reason", "internal_error"}},
                    1};
            }
            return AccountRemoveRemoteResult::NotPresent;
        }
        if (proof == Proof::MissingCheckpoint) {
            return AccountRemoveRemoteResult::Confirmed;
        }
        if (!send_checkpointed) {
            if (!checkpoint(AuditStage::RemoteLogoutSendStarted)) {
                return RemovalOperationError{
                    "INTERNAL",
                    "checkpoint failed",
                    {{"operation", "account_remove"}, {"reason", "internal_error"}},
                    1};
            }
        }
        if (proof == Proof::FailAfterSend) {
            return RemovalOperationError{
                "REMOTE_LOGOUT_UNCONFIRMED",
                "remote logout failed",
                {{"account", "work"}, {"state", "logging_out"}, {"reason", "timeout"}},
                1};
        }
        if (!checkpoint(AuditStage::RemoteConfirmed)) {
            return RemovalOperationError{
                "INTERNAL",
                "checkpoint failed",
                {{"operation", "account_remove"}, {"reason", "internal_error"}},
                1};
        }
        return AccountRemoveRemoteResult::Confirmed;
    }

    std::optional<RemovalOperationError> quiesce(RequestSession& /*session*/) override {
        ++close_calls;
        return close_failure;
    }

    Proof proof = Proof::NotPresent;
    std::optional<RemovalOperationError> close_failure;
    int proof_calls = 0;
    int close_calls = 0;
};

struct CapturedTerminal {
    std::optional<nlohmann::json> result;
    std::optional<RemovalOperationError> error;
    std::optional<nlohmann::json> challenge;
};

tgcli::proto::Request request(bool keep_session = false, bool dry_run = false) {
    tgcli::proto::Request value("work");
    value.id = 17;
    value.command = {"account", "remove"};
    value.args = {{"account", "work"},
                  {"global_account_supplied", false},
                  {"keep_session", keep_session},
                  {"reassign_default", nullptr}};
    value.context.yes = true;
    value.context.dry_run = dry_run;
    return value;
}

CapturedTerminal run(AccountRemovalCoordinator& coordinator, tgcli::proto::Request value,
                     std::optional<bool> challenge_answer = std::nullopt) {
    CapturedTerminal captured;
    CallbackSink sink(
        [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
        [&](nlohmann::json result) { captured.result = std::move(result); },
        [&](std::string code, std::string message, nlohmann::json details, int exit_code) {
            captured.error = RemovalOperationError{std::move(code), std::move(message),
                                                   std::move(details), exit_code};
        },
        [&](nlohmann::json challenge) -> std::optional<nlohmann::json> {
            captured.challenge = challenge;
            if (!challenge_answer) {
                return std::nullopt;
            }
            return nlohmann::json{{"nonce", challenge["nonce"]},
                                  {"sequence", challenge["sequence"]},
                                  {"client_generation", challenge["client_generation"]},
                                  {"auth_sequence", challenge["auth_sequence"]},
                                  {"value", *challenge_answer}};
        });
    RequestSession session(std::move(value), sink, 1,
                           [] { return std::string("00112233445566778899aabbccddeeff"); });
    coordinator.remove(session.request(), session);
    return captured;
}

std::shared_ptr<testing::AccountRemovalHooks> deterministic_hooks() {
    auto hooks = std::make_shared<testing::AccountRemovalHooks>();
    hooks->invocation_id = [] { return std::string("ffeeddccbbaa99887766554433221100"); };
    hooks->timestamp = [] { return std::string("2026-08-04T10:11:12Z"); };
    return hooks;
}

void force_unverified_terminal(const RemovalJournal& journal, std::string_view invocation) {
    RemovalJournalFailure failure;
    auto tombstone = journal.load(invocation, failure);
    REQUIRE(tombstone);
    auto document = serialize(*tombstone);
    document["stage"] = "outcome_synced";
    document["completed_stages"] =
        nlohmann::json::array({"planned", "intent_synced", "outcome_synced"});
    document["next_stage"] = nullptr;
    std::ofstream output(journal.tombstone_path(invocation), std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << document.dump() << '\n';
    output.close();
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("keep-session removal never requests remote logout and commits every local stage",
          "[removal][command]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks());
    const auto terminal = run(coordinator, request(true));
    REQUIRE(terminal.result);
    CHECK(*terminal.result == nlohmann::json{{"account", "work"},
                                             {"removed", true},
                                             {"remote_logout", "kept"},
                                             {"default_account", nullptr}});
    CHECK(remote.proof_calls == 0);
    CHECK(remote.close_calls == 1);
    CHECK_FALSE(std::filesystem::exists(temp.data_root()));
    CHECK_FALSE(std::filesystem::exists(temp.state_root()));
    const auto current = store.load();
    REQUIRE(current);
    CHECK(current.snapshot->accounts.empty());
    CHECK(journal.inspect_account("work").status == RemovalInspectionStatus::Clean);
}

TEST_CASE("daemon shutdown is requested only after the removal terminal is emitted",
          "[removal][command][lifecycle]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    bool terminal_emitted = false;
    bool shutdown_called = false;
    bool shutdown_followed_terminal = false;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks(), [&] {
                                              shutdown_called = true;
                                              shutdown_followed_terminal = terminal_emitted;
                                          });
    CallbackSink sink([](const nlohmann::json&) {}, [](const nlohmann::json&) {},
                      [&](const nlohmann::json&) { terminal_emitted = true; },
                      [&](const std::string&, const std::string&, const nlohmann::json&, int) {
                          terminal_emitted = true;
                      });
    auto value = request(true);
    RequestSession session(std::move(value), sink, 1,
                           [] { return std::string("00112233445566778899aabbccddeeff"); });
    coordinator.remove(session.request(), session);
    CHECK(terminal_emitted);
    CHECK(shutdown_called);
    CHECK(shutdown_followed_terminal);
}

TEST_CASE("dry-run validates the full plan without authority audit remote or mutation",
          "[removal][command]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote);
    auto value = request(false, true);
    value.context.write_authority = tgcli::proto::WriteAuthority::Deny;
    const auto terminal = run(coordinator, std::move(value));
    REQUIRE(terminal.result);
    CHECK((*terminal.result)["dry_run"] == true);
    CHECK((*terminal.result)["plan"]["remote_logout"] == true);
    CHECK(remote.proof_calls == 0);
    CHECK(remote.close_calls == 0);
    CHECK(std::filesystem::exists(temp.data_root()));
    CHECK(store.load().snapshot->accounts.contains("work"));
    CHECK_FALSE(std::filesystem::exists(journal.directory()));
}

TEST_CASE("remote uncertainty writes a failure outcome and preserves all local state",
          "[removal][command][audit]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    remote.proof = FakeRemote::Proof::FailAfterSend;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks());
    const auto terminal = run(coordinator, request());
    REQUIRE(terminal.error);
    CHECK(terminal.error->code == "REMOTE_LOGOUT_UNCONFIRMED");
    CHECK(terminal.error->details["reason"] == "timeout");
    const nlohmann::json error_document{{"error",
                                         {{"code", terminal.error->code},
                                          {"message", terminal.error->message},
                                          {"details", terminal.error->details}}}};
    CHECK_THAT(error_document,
               tgcli::test::matches_json_schema("account-remove.error.schema.json"));
    CHECK(std::filesystem::exists(temp.data_root()));
    CHECK(std::filesystem::exists(temp.state_root()));
    CHECK(store.load().snapshot->accounts.contains("work"));
    CHECK(journal.inspect_account("work").status == RemovalInspectionStatus::Clean);
    RemovalJournalFailure failure;
    const auto presence = journal.audit_presence("ffeeddccbbaa99887766554433221100", failure);
    REQUIRE(presence);
    CHECK(presence->outcome);
    const auto outcome = journal.audit_outcome("ffeeddccbbaa99887766554433221100", failure);
    REQUIRE(outcome);
    CHECK_THAT(*outcome, tgcli::test::matches_json_schema("audit-outcome.schema.json"));
    const auto tombstone = journal.load("ffeeddccbbaa99887766554433221100", failure);
    REQUIRE(tombstone);
    CHECK_THAT(serialize(*tombstone),
               tgcli::test::matches_json_schema("removal-tombstone.schema.json"));
}

TEST_CASE("unaudited terminal blocks removal before remote or local destructive work",
          "[removal][command][audit][recovery]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    const auto fresh =
        plan_account_removal(store, journal, temp.environment(), "work", false, std::nullopt);
    REQUIRE(fresh);
    const std::string invocation = "00112233445566778899aabbccddeeff";
    RemovalJournalFailure failure;
    REQUIRE(journal.create(invocation, fresh.planned->plan, failure));
    force_unverified_terminal(journal, invocation);

    FakeRemote remote;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks());
    const auto terminal = run(coordinator, request());
    REQUIRE(terminal.error);
    CHECK(terminal.error->code == "AUDIT_UNAVAILABLE");
    CHECK(terminal.error->details == nlohmann::json{{"account", "work"},
                                                    {"path", journal.tombstone_path(invocation)},
                                                    {"reason", "path_invalid"}});
    CHECK(remote.proof_calls == 0);
    CHECK(remote.close_calls == 0);
    CHECK(std::filesystem::exists(temp.data_root()));
    CHECK(std::filesystem::exists(temp.state_root()));
    const auto current = store.load();
    REQUIRE(current);
    CHECK(current.snapshot->accounts.contains("work"));
}

TEST_CASE("remote return without its durable proof checkpoint fails closed",
          "[removal][command][audit]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    remote.proof = FakeRemote::Proof::MissingCheckpoint;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks());
    const auto terminal = run(coordinator, request());
    REQUIRE(terminal.error);
    CHECK(terminal.error->code == "INTERNAL");
    CHECK(std::filesystem::exists(temp.data_root()));
    CHECK(std::filesystem::exists(temp.state_root()));
    CHECK(store.load().snapshot->accounts.contains("work"));
}

TEST_CASE("interactive confirmation binds the exact plan and denial writes no journal",
          "[removal][command][confirmation]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks());
    auto value = request();
    value.context.yes = false;
    value.context.tty = true;
    auto terminal = run(coordinator, value, false);
    REQUIRE(terminal.error);
    CHECK(terminal.error->code == "CONFIRMATION_REQUIRED");
    REQUIRE(terminal.challenge);
    CHECK((*terminal.challenge)["details"]["target"] == terminal.error->details["target"]);
    CHECK_FALSE(std::filesystem::exists(journal.directory()));

    value = request();
    value.context.write_authority = tgcli::proto::WriteAuthority::Deny;
    terminal = run(coordinator, std::move(value));
    REQUIRE(terminal.error);
    CHECK(terminal.error->code == "WRITE_DENIED");
    CHECK_FALSE(std::filesystem::exists(journal.directory()));
}

TEST_CASE("dispatcher admits only the explicitly marked M1 removal descriptor",
          "[removal][command][dispatch]") {
    const TempRemoval temp;
    const auto store = temp.store();
    auto journal = temp.journal();
    FakeRemote remote;
    AccountRemovalCoordinator coordinator(store, journal, temp.environment(), "work", remote, {},
                                          deterministic_hooks());
    Dispatcher dispatcher;
    register_account_removal_command(dispatcher, coordinator);
    dispatcher.register_command(
        "blocked", {Tier::Destructive, [](const tgcli::proto::Request&, RequestSession& session) {
                        session.result({{"unsafe", true}});
                    }});

    auto value = request(true);
    CapturedTerminal captured;
    CallbackSink sink(
        [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
        [&](nlohmann::json result) { captured.result = std::move(result); },
        [&](std::string code, std::string message, nlohmann::json details, int exit_code) {
            captured.error = RemovalOperationError{std::move(code), std::move(message),
                                                   std::move(details), exit_code};
        });
    dispatcher.dispatch(value, sink);
    REQUIRE(captured.result);

    tgcli::proto::Request blocked("work");
    blocked.id = 18;
    blocked.command = {"blocked"};
    CapturedTerminal denied;
    CallbackSink denied_sink(
        [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
        [&](nlohmann::json result) { denied.result = std::move(result); },
        [&](std::string code, std::string message, nlohmann::json details, int exit_code) {
            denied.error = RemovalOperationError{std::move(code), std::move(message),
                                                 std::move(details), exit_code};
        });
    dispatcher.dispatch(blocked, denied_sink);
    REQUIRE(denied.error);
    CHECK(denied.error->code == "DENIED");
}

TEST_CASE("freshly approved retry resumes after every durable nonterminal checkpoint",
          "[removal][command][recovery]") {
    constexpr std::array crash_stages{
        AuditStage::Planned,          AuditStage::IntentSynced,
        AuditStage::RemoteNotPresent, AuditStage::ClientCloseStarted,
        AuditStage::ClientClosed,     AuditStage::ConfigRemoveStarted,
        AuditStage::ConfigRemoved,    AuditStage::DataRemoveStarted,
        AuditStage::DataRemoved,      AuditStage::StateRemoveStarted,
        AuditStage::StateRemoved,
    };

    for (const auto crash_stage : crash_stages) {
        INFO("crash after " << audit_stage_name(crash_stage));
        const TempRemoval temp;
        const auto store = temp.store();
        auto crash_hooks = std::make_shared<testing::RemovalJournalHooks>();
        crash_hooks->after_tombstone_sync = [crash_stage](std::string_view, AuditStage stage) {
            if (stage == crash_stage) {
                throw std::runtime_error("injected crash");
            }
        };
        auto crashing_journal = temp.journal(crash_hooks);
        FakeRemote remote;
        remote.proof = FakeRemote::Proof::NotPresent;
        AccountRemovalCoordinator crashing(store, crashing_journal, temp.environment(), "work",
                                           remote, {}, deterministic_hooks());
        CHECK_THROWS_AS(run(crashing, request()), std::runtime_error);

        auto journal = temp.journal();
        FakeRemote resumed_remote;
        AccountRemovalCoordinator resumed(store, journal, temp.environment(), "work",
                                          resumed_remote, {}, deterministic_hooks());
        auto retry = request();
        retry.context.write_authority = tgcli::proto::WriteAuthority::Grant;
        const auto terminal = run(resumed, std::move(retry));
        REQUIRE(terminal.result);
        CHECK((*terminal.result)["removed"] == true);
        CHECK((*terminal.result)["remote_logout"] == "not_present");
        CHECK_FALSE(std::filesystem::exists(temp.data_root()));
        CHECK_FALSE(std::filesystem::exists(temp.state_root()));
        const auto loaded = store.load();
        REQUIRE(loaded);
        CHECK_FALSE(loaded.snapshot->accounts.contains("work"));
    }
}
