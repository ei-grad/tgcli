#include "core/auth_bootstrap.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace tgcli::core {

namespace {

using HookField = secret_hook::HookField;

struct ResolvedCredentials {
    ResolvedCredentials() = default;
    ResolvedCredentials(const ResolvedCredentials&) = delete;
    ResolvedCredentials& operator=(const ResolvedCredentials&) = delete;
    ResolvedCredentials(ResolvedCredentials&&) = delete;
    ResolvedCredentials& operator=(ResolvedCredentials&&) = delete;
    ~ResolvedCredentials();

    std::int32_t api_id = 0;
    std::string api_hash;
    std::string database_key;
    bool interactive_source_used = false;
};

void wipe(std::string& value) {
    volatile char* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

ResolvedCredentials::~ResolvedCredentials() {
    wipe(api_hash);
    wipe(database_key);
}

struct BootstrapHookCache {
    BootstrapHookCache() = default;
    BootstrapHookCache(const BootstrapHookCache&) = delete;
    BootstrapHookCache& operator=(const BootstrapHookCache&) = delete;
    BootstrapHookCache(BootstrapHookCache&&) = delete;
    BootstrapHookCache& operator=(BootstrapHookCache&&) = delete;
    ~BootstrapHookCache() = default;
    bool attempted = false;
    bool succeeded = false;
    bool cancelled = false;
    std::optional<secret_hook::HookError> error;
};

struct BootstrapOccurrence {
    bool sent = false;
    BootstrapHookCache api_id;
    BootstrapHookCache api_hash;
    BootstrapHookCache database_key;
};

BootstrapResult failure(BootstrapFailure reason, std::vector<HookField> fields = {},
                        std::optional<secret_hook::HookError> hook = {}) {
    return {{}, BootstrapError{reason, std::move(fields), hook}, {}};
}

bool expired(const config::MutationControl& control) {
    return control.deadline && std::chrono::steady_clock::now() >= *control.deadline;
}

std::optional<BootstrapResult> stopped(const config::MutationControl& control) {
    if (control.cancellation.stop_requested()) {
        return failure(BootstrapFailure::Cancelled);
    }
    if (expired(control)) {
        return failure(BootstrapFailure::TimedOut);
    }
    return std::nullopt;
}

std::string normalized(std::string value) {
    return std::filesystem::path(std::move(value)).lexically_normal().string();
}

bool exact_namespace_leaf(const std::string& directory, bool test_dc, bool runtime, uid_t uid) {
    const auto leaf = std::filesystem::path(directory).filename().string();
    const auto expected = test_dc ? std::string("tgcli-test") : std::string("tgcli");
    if (leaf == expected) {
        return true;
    }
    return runtime && leaf == expected + "-" + std::to_string(uid);
}

bool absolute_normalized(const std::string& value) {
    const std::filesystem::path candidate(value);
    return candidate.is_absolute() && candidate.lexically_normal() == candidate;
}

bool valid_path_identity(const BootstrapSnapshot& snapshot) {
    return absolute_normalized(snapshot.config_path) &&
           absolute_normalized(snapshot.config_namespace_directory) &&
           absolute_normalized(snapshot.data_namespace_directory) &&
           absolute_normalized(snapshot.state_namespace_directory) &&
           absolute_normalized(snapshot.runtime_namespace_directory) &&
           absolute_normalized(snapshot.account_data_directory) &&
           absolute_normalized(snapshot.account_state_directory) &&
           absolute_normalized(snapshot.database_directory) &&
           absolute_normalized(snapshot.files_directory) &&
           exact_namespace_leaf(snapshot.config_namespace_directory, snapshot.test_dc, false,
                                snapshot.uid) &&
           exact_namespace_leaf(snapshot.data_namespace_directory, snapshot.test_dc, false,
                                snapshot.uid) &&
           exact_namespace_leaf(snapshot.state_namespace_directory, snapshot.test_dc, false,
                                snapshot.uid) &&
           exact_namespace_leaf(snapshot.runtime_namespace_directory, snapshot.test_dc, true,
                                snapshot.uid) &&
           snapshot.config_path == snapshot.config_namespace_directory + "/config.toml" &&
           snapshot.account_data_directory ==
               snapshot.data_namespace_directory + "/accounts/" + snapshot.account &&
           snapshot.account_state_directory ==
               snapshot.state_namespace_directory + "/accounts/" + snapshot.account &&
           snapshot.database_directory == snapshot.account_data_directory + "/tdlib/db" &&
           snapshot.files_directory == snapshot.account_data_directory + "/tdlib/files";
}

bool ensure_storage_tree(const BootstrapSnapshot& snapshot) {
    if (!valid_path_identity(snapshot)) {
        return false;
    }

    const std::vector<std::string> directories{
        snapshot.data_namespace_directory, snapshot.data_namespace_directory + "/accounts",
        snapshot.account_data_directory,   snapshot.account_data_directory + "/tdlib",
        snapshot.database_directory,       snapshot.files_directory,
    };
    std::string ignored;
    return std::ranges::all_of(directories, [&](const std::string& directory) {
        return paths::ensure_private_dir(directory, snapshot.uid, ignored);
    });
}

// The branches preserve one-shot hook execution, fallback, cancellation and
// redaction as one local state transition.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<BootstrapResult> hook_value(AuthBootstrap::HookRunner& runner,
                                          BootstrapHookCache& cache, HookField field,
                                          const std::string& command,
                                          const BootstrapAttempt& attempt,
                                          const std::optional<std::string>& prompted,
                                          std::string& value, bool& interactive_source_used) {
    if (!cache.attempted) {
        cache.attempted = true;
        auto result =
            runner({field, command, attempt.control.deadline, attempt.control.cancellation});
        if (result.cancelled) {
            cache.cancelled = true;
            wipe(result.value);
        } else if (result) {
            if (field == HookField::ApiId) {
                std::int32_t parsed = 0;
                if (!secret_hook::parse_api_id(result.value, parsed)) {
                    cache.error = secret_hook::HookError{
                        HookField::ApiId, secret_hook::HookFailure::StdoutInvalid, {}};
                    wipe(result.value);
                } else {
                    cache.succeeded = true;
                    value.swap(result.value);
                    wipe(result.value);
                    return std::nullopt;
                }
            } else {
                cache.succeeded = true;
                value.swap(result.value);
                wipe(result.value);
                return std::nullopt;
            }
        } else {
            cache.error = result.error;
            wipe(result.value);
        }
    }
    if (cache.cancelled) {
        return failure(BootstrapFailure::Cancelled, {field});
    }
    if (cache.succeeded) {
        if (attempt.interactive && prompted) {
            value = *prompted;
            interactive_source_used = true;
            return std::nullopt;
        }
        return failure(attempt.interactive ? BootstrapFailure::InputRequired
                                           : BootstrapFailure::HookFailed,
                       {field});
    }
    if (cache.error) {
        if (attempt.interactive) {
            if (prompted) {
                value = *prompted;
                interactive_source_used = true;
                return std::nullopt;
            }
            return failure(BootstrapFailure::InputRequired, {field}, cache.error);
        }
        return failure(BootstrapFailure::HookFailed, {field}, cache.error);
    }
    if (attempt.interactive && prompted) {
        value = *prompted;
        interactive_source_used = true;
        return std::nullopt;
    }
    return failure(BootstrapFailure::HookFailed, {field});
}

// Field-local precedence is intentionally explicit so mixed-source accounts
// cannot accidentally apply one field's fallback to another.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<BootstrapResult> resolve_credentials(const BootstrapSnapshot& snapshot,
                                                   const BootstrapAttempt& attempt,
                                                   AuthBootstrap::HookRunner& runner,
                                                   BootstrapOccurrence& occurrence,
                                                   ResolvedCredentials& credentials) {
    if (snapshot.environment_api_id) {
        if (!secret_hook::parse_api_id(*snapshot.environment_api_id, credentials.api_id)) {
            return failure(BootstrapFailure::InvalidCredential, {HookField::ApiId});
        }
    } else if (attempt.interactive && attempt.prompted_app.api_id) {
        credentials.api_id = *attempt.prompted_app.api_id;
        credentials.interactive_source_used = true;
    } else if (snapshot.account_config.api_id) {
        credentials.api_id = *snapshot.account_config.api_id;
    } else if (snapshot.account_config.api_id_cmd) {
        std::string value;
        const std::optional<std::string> prompted =
            attempt.prompted_app.api_id
                ? std::optional<std::string>(std::to_string(*attempt.prompted_app.api_id))
                : std::nullopt;
        if (auto error = hook_value(runner, occurrence.api_id, HookField::ApiId,
                                    *snapshot.account_config.api_id_cmd, attempt, prompted, value,
                                    credentials.interactive_source_used)) {
            wipe(value);
            return error;
        }
        if (!secret_hook::parse_api_id(value, credentials.api_id)) {
            wipe(value);
            return failure(BootstrapFailure::InvalidCredential, {HookField::ApiId});
        }
        wipe(value);
    } else {
        return failure(BootstrapFailure::InputRequired, {HookField::ApiId});
    }

    if (snapshot.environment_api_hash) {
        if (snapshot.environment_api_hash->empty()) {
            return failure(BootstrapFailure::InvalidCredential, {HookField::ApiHash});
        }
        credentials.api_hash = *snapshot.environment_api_hash;
    } else if (attempt.interactive && attempt.prompted_app.api_hash) {
        credentials.api_hash = *attempt.prompted_app.api_hash;
        credentials.interactive_source_used = true;
    } else if (snapshot.account_config.api_hash) {
        credentials.api_hash = *snapshot.account_config.api_hash;
    } else if (snapshot.account_config.api_hash_cmd) {
        if (auto error = hook_value(runner, occurrence.api_hash, HookField::ApiHash,
                                    *snapshot.account_config.api_hash_cmd, attempt,
                                    attempt.prompted_app.api_hash, credentials.api_hash,
                                    credentials.interactive_source_used)) {
            return error;
        }
    } else {
        return failure(BootstrapFailure::InputRequired, {HookField::ApiHash});
    }

    if (snapshot.account_config.db_key_cmd) {
        if (auto error = hook_value(runner, occurrence.database_key, HookField::DatabaseKey,
                                    *snapshot.account_config.db_key_cmd, attempt,
                                    attempt.prompted_database_key, credentials.database_key,
                                    credentials.interactive_source_used)) {
            return error;
        }
    } else if (attempt.prompted_database_key) {
        credentials.database_key = *attempt.prompted_database_key;
        credentials.interactive_source_used = true;
    }
    if (credentials.api_id <= 0) {
        return failure(BootstrapFailure::InvalidCredential, {HookField::ApiId});
    }
    if (credentials.api_hash.empty()) {
        return failure(BootstrapFailure::InvalidCredential, {HookField::ApiHash});
    }
    return std::nullopt;
}

std::optional<BootstrapResult> mutation_failure(const config::MutationResult& result) {
    switch (result.status) {
    case config::MutationStatus::Applied:
        return std::nullopt;
    case config::MutationStatus::Conflict:
        return BootstrapResult{
            {}, BootstrapError{BootstrapFailure::ConfigConflict, {}, {}}, result.snapshot};
    case config::MutationStatus::TimedOut:
        return failure(BootstrapFailure::TimedOut);
    case config::MutationStatus::Cancelled:
        return failure(BootstrapFailure::Cancelled);
    case config::MutationStatus::PreconditionFailed:
        return failure(BootstrapFailure::AuthorizationChanged);
    case config::MutationStatus::Invalid:
    case config::MutationStatus::IoError:
    case config::MutationStatus::DurabilityUnknown:
        return failure(BootstrapFailure::ConfigInvalid);
    }
    return failure(BootstrapFailure::ConfigInvalid);
}

} // namespace

struct AuthBootstrap::State {
    std::mutex mutex;
    std::map<std::pair<std::uint64_t, std::uint64_t>, BootstrapOccurrence> occurrences;
};

std::string_view bootstrap_failure_name(BootstrapFailure failure) {
    switch (failure) {
    case BootstrapFailure::InvalidSnapshot:
        return "invalid_snapshot";
    case BootstrapFailure::InvalidCredential:
        return "invalid_credential";
    case BootstrapFailure::InputRequired:
        return "input_required";
    case BootstrapFailure::HookFailed:
        return "hook_failed";
    case BootstrapFailure::PathInvalid:
        return "path_invalid";
    case BootstrapFailure::ConfigConflict:
        return "config_conflict";
    case BootstrapFailure::ConfigInvalid:
        return "config_invalid";
    case BootstrapFailure::TimedOut:
        return "timed_out";
    case BootstrapFailure::Cancelled:
        return "cancelled";
    case BootstrapFailure::AuthorizationChanged:
        return "authorization_changed";
    case BootstrapFailure::Duplicate:
        return "duplicate";
    }
    return "invalid_snapshot";
}

std::string describe(const BootstrapError& error) {
    std::string result(bootstrap_failure_name(error.failure));
    if (error.hook) {
        result += ": " + secret_hook::describe(*error.hook);
    }
    return result;
}

BootstrapCaptureResult capture_bootstrap_snapshot(
    std::string account, const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot,
    const paths::Environment& environment, bool daemon_test_dc_identity,
    std::string application_version, std::optional<std::string> environment_api_id,
    std::optional<std::string> environment_api_hash) {
    if (!config_snapshot || !paths::valid_account_name(account) || application_version.empty() ||
        environment.test_dc != daemon_test_dc_identity) {
        return {{}, BootstrapError{BootstrapFailure::InvalidSnapshot, {}, {}}};
    }

    config::AccountConfig account_config;
    bool implicit_main = false;
    if (const auto found = config_snapshot->accounts.find(account);
        found != config_snapshot->accounts.end()) {
        account_config = found->second;
    } else {
        implicit_main = account == "main" && config_snapshot->accounts.empty();
        if (!implicit_main) {
            return {{}, BootstrapError{BootstrapFailure::InvalidSnapshot, {}, {}}};
        }
    }

    const auto config_path = normalized(paths::config_file(environment));
    const auto account_data = normalized(paths::account_data_dir(account, environment));
    const auto account_state = normalized(paths::account_state_dir(account, environment));
    const auto runtime_namespace = normalized(paths::runtime_dir(environment));
    BootstrapSnapshot snapshot{
        .account = std::move(account),
        .config_identity = config_snapshot->identity,
        .config_path = config_path,
        .config_namespace_directory = std::filesystem::path(config_path).parent_path().string(),
        .data_namespace_directory =
            std::filesystem::path(account_data).parent_path().parent_path().string(),
        .state_namespace_directory =
            std::filesystem::path(account_state).parent_path().parent_path().string(),
        .runtime_namespace_directory = runtime_namespace,
        .account_data_directory = account_data,
        .account_state_directory = account_state,
        .database_directory = account_data + "/tdlib/db",
        .files_directory = account_data + "/tdlib/files",
        .application_version = std::move(application_version),
        .uid = environment.uid,
        .test_dc = daemon_test_dc_identity,
        .implicit_main = implicit_main,
        .account_config = std::move(account_config),
        .environment_api_id = std::move(environment_api_id),
        .environment_api_hash = std::move(environment_api_hash),
    };
    if (!valid_path_identity(snapshot)) {
        return {{}, BootstrapError{BootstrapFailure::InvalidSnapshot, {}, {}}};
    }
    return {std::move(snapshot), {}};
}

AuthBootstrap::AuthBootstrap(TdClient& client, const config::Store& store,
                             BootstrapSnapshot snapshot, HookRunner hook_runner)
    : client_(client), store_(store), snapshot_(std::move(snapshot)),
      hook_runner_(std::move(hook_runner)), state_(std::make_unique<State>()) {
    if (!hook_runner_) {
        throw std::invalid_argument("bootstrap hook runner must not be empty");
    }
}

AuthBootstrap::~AuthBootstrap() = default;

// Bootstrap deliberately keeps validation, secret resolution, local CAS and
// the commit-spanning send lease in one serialized occurrence.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
BootstrapResult AuthBootstrap::run(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   const BootstrapAttempt& attempt) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    auto owner = client_.internal_auth_owner();
    if (!authorization || authorization->data.state != AuthState::WaitTdlibParameters ||
        !client_.owns(owner, authorization ? authorization->client_generation : 0)) {
        return failure(BootstrapFailure::AuthorizationChanged);
    }
    if (!attempt.interactive && (attempt.prompted_app.api_id || attempt.prompted_app.api_hash ||
                                 attempt.prompted_database_key)) {
        return failure(BootstrapFailure::AuthorizationChanged);
    }
    if (normalized(store_.path()) != snapshot_.config_path) {
        return failure(BootstrapFailure::InvalidSnapshot);
    }
    if (auto control_failure = stopped(attempt.control)) {
        return std::move(*control_failure);
    }
    if (attempt.prompted_app.api_id.has_value() != attempt.prompted_app.api_hash.has_value()) {
        return failure(BootstrapFailure::InvalidCredential, {HookField::ApiId, HookField::ApiHash});
    }

    const auto live = client_.auth_state();
    if (!live || live->client_generation != authorization->client_generation ||
        live->auth_sequence != authorization->auth_sequence ||
        live->data.state != authorization->data.state) {
        return failure(BootstrapFailure::AuthorizationChanged);
    }

    auto& occurrence =
        state_->occurrences[{authorization->client_generation, authorization->auth_sequence}];
    if (occurrence.sent) {
        return failure(BootstrapFailure::Duplicate);
    }

    ResolvedCredentials credentials;
    if (auto resolution =
            resolve_credentials(snapshot_, attempt, hook_runner_, occurrence, credentials)) {
        return std::move(*resolution);
    }
    TdOwnerLease login_owner;
    if (attempt.interactive && !credentials.interactive_source_used) {
        return failure(BootstrapFailure::AuthorizationChanged);
    }
    if (attempt.interactive) {
        login_owner = client_.issue_login_owner();
        if (!login_owner) {
            return failure(BootstrapFailure::AuthorizationChanged);
        }
        owner = login_owner.owner();
    }
    if (auto control_failure = stopped(attempt.control)) {
        return std::move(*control_failure);
    }
    const auto after_resolution = client_.auth_state();
    if (!after_resolution ||
        after_resolution->client_generation != authorization->client_generation ||
        after_resolution->auth_sequence != authorization->auth_sequence ||
        after_resolution->data.state != authorization->data.state) {
        return failure(BootstrapFailure::AuthorizationChanged);
    }
    if (!ensure_storage_tree(snapshot_)) {
        return failure(BootstrapFailure::PathInvalid);
    }

    TdSendDescriptor descriptor{
        .function = TdFunctionKind::SetTdlibParameters,
        .tier = DescriptorKind::AuthBootstrap,
        .owner = owner,
        .client_generation = authorization->client_generation,
        .auth_sequence = authorization->auth_sequence,
        .auth_state = authorization->data.state,
    };
    TdSendLease lease;
    std::shared_ptr<const config::ConfigSnapshot> materialized;
    if (snapshot_.implicit_main) {
        auto mutation_control = attempt.control;
        mutation_control.commit_admission = [&] {
            lease = client_.acquire_send_lease(descriptor);
            return static_cast<bool>(lease);
        };
        const auto mutation = store_.materialize_implicit_main(
            snapshot_.config_identity, attempt.prompted_app, mutation_control);
        if (auto mutation_error = mutation_failure(mutation)) {
            return std::move(*mutation_error);
        }
        materialized = mutation.snapshot;
    } else if (attempt.interactive && attempt.prompted_app.api_id &&
               attempt.prompted_app.api_hash) {
        auto mutation_control = attempt.control;
        mutation_control.commit_admission = [&] {
            lease = client_.acquire_send_lease(descriptor);
            return static_cast<bool>(lease);
        };
        const auto mutation = store_.replace_app_credentials(
            snapshot_.config_identity, snapshot_.account, attempt.prompted_app, mutation_control);
        if (auto mutation_error = mutation_failure(mutation)) {
            return std::move(*mutation_error);
        }
        materialized = mutation.snapshot;
    } else {
        lease = client_.acquire_send_lease(descriptor);
        if (!lease) {
            return failure(BootstrapFailure::AuthorizationChanged);
        }
    }
    if (!lease) {
        return failure(BootstrapFailure::AuthorizationChanged);
    }

#if defined(__APPLE__)
    constexpr std::string_view system_version = "macOS";
#elif defined(__linux__)
    constexpr std::string_view system_version = "Linux";
#else
#error "tgcli bootstrap supports only Linux and macOS"
#endif

    TdlibParameters parameters;
    parameters.use_test_dc = snapshot_.test_dc;
    parameters.database_directory = snapshot_.database_directory;
    parameters.files_directory = snapshot_.files_directory;
    parameters.database_encryption_key = credentials.database_key;
    wipe(credentials.database_key);
    parameters.use_file_database = true;
    parameters.use_chat_info_database = true;
    parameters.use_message_database = true;
    parameters.use_secret_chats = false;
    parameters.api_id = credentials.api_id;
    parameters.api_hash = credentials.api_hash;
    wipe(credentials.api_hash);
    parameters.system_language_code = "en";
    parameters.device_model = "tgcli";
    parameters.system_version = system_version;
    parameters.application_version = snapshot_.application_version;
    occurrence.sent = true;
    auto response = client_.send(std::move(lease), std::move(parameters));
    return {std::optional<std::future<TdValue>>(std::move(response)), {}, std::move(materialized)};
}

bool AuthBootstrap::retry_after_rejection(
    const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (!authorization || authorization->data.state != AuthState::WaitTdlibParameters) {
        return false;
    }
    const auto live = client_.auth_state();
    if (!live || live->client_generation != authorization->client_generation ||
        live->auth_sequence != authorization->auth_sequence ||
        live->data.state != authorization->data.state) {
        return false;
    }
    const auto found =
        state_->occurrences.find({authorization->client_generation, authorization->auth_sequence});
    if (found == state_->occurrences.end() || !found->second.sent) {
        return false;
    }
    found->second.sent = false;
    return true;
}

} // namespace tgcli::core
