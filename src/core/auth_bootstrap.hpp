#pragma once

#include "common/config.hpp"
#include "common/paths.hpp"
#include "common/secret_hook.hpp"
#include "core/td_client.hpp"

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tgcli::core {

enum class BootstrapFailure {
    InvalidSnapshot,
    InvalidCredential,
    InputRequired,
    HookFailed,
    PathInvalid,
    ConfigConflict,
    ConfigInvalid,
    TimedOut,
    Cancelled,
    AuthorizationChanged,
    Duplicate,
};

std::string_view bootstrap_failure_name(BootstrapFailure failure);

struct BootstrapError {
    BootstrapFailure failure = BootstrapFailure::InvalidSnapshot;
    std::vector<secret_hook::HookField> fields;
    std::optional<secret_hook::HookError> hook;
};

std::string describe(const BootstrapError& error);

struct BootstrapSnapshot {
    std::string account;
    std::string config_identity;
    std::string config_path;
    std::string config_namespace_directory;
    std::string data_namespace_directory;
    std::string state_namespace_directory;
    std::string runtime_namespace_directory;
    std::string account_data_directory;
    std::string account_state_directory;
    std::string database_directory;
    std::string files_directory;
    std::string application_version;
    uid_t uid = 0;
    bool test_dc = false;
    bool implicit_main = false;
    config::AccountConfig account_config;
    std::optional<std::string> environment_api_id;
    std::optional<std::string> environment_api_hash;
};

struct BootstrapCaptureResult {
    std::optional<BootstrapSnapshot> snapshot;
    std::optional<BootstrapError> error;
};

BootstrapCaptureResult capture_bootstrap_snapshot(
    std::string account, const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot,
    const paths::Environment& environment, bool daemon_test_dc_identity,
    std::string application_version, std::optional<std::string> environment_api_id = {},
    std::optional<std::string> environment_api_hash = {});

struct BootstrapAttempt {
    config::MutationControl control;
    bool interactive = false;
    config::PromptedAppCredentials prompted_app;
    std::optional<std::string> prompted_database_key;
};

struct BootstrapResult {
    std::optional<std::future<TdValue>> response;
    std::optional<BootstrapError> error;
    std::shared_ptr<const config::ConfigSnapshot> materialized_snapshot;

    explicit operator bool() const {
        return response.has_value() && !error;
    }
};

class AuthBootstrap {
  public:
    using HookRunner = std::function<secret_hook::HookResult(const secret_hook::HookRequest&)>;

    AuthBootstrap(TdClient& client, const config::Store& store, BootstrapSnapshot snapshot,
                  HookRunner hook_runner = secret_hook::run);
    ~AuthBootstrap();
    AuthBootstrap(const AuthBootstrap&) = delete;
    AuthBootstrap& operator=(const AuthBootstrap&) = delete;
    AuthBootstrap(AuthBootstrap&&) = delete;
    AuthBootstrap& operator=(AuthBootstrap&&) = delete;

    BootstrapResult run(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                        const BootstrapAttempt& attempt);

  private:
    struct State;

    TdClient& client_;
    const config::Store& store_;
    const BootstrapSnapshot snapshot_;
    HookRunner hook_runner_;
    std::unique_ptr<State> state_;
};

} // namespace tgcli::core
