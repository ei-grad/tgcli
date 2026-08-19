#include "daemon/login_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/secure_wipe.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;
using core::AuthState;
using core::AuthStateSnapshot;
using core::TdFunctionKind;
using nlohmann::json;

struct Occurrence {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;

    bool operator<(const Occurrence& other) const {
        return std::tie(generation, sequence) < std::tie(other.generation, other.sequence);
    }
    bool operator==(const Occurrence&) const = default;
};

enum class ReadyChangeKind { None, Advanced, Lost };

struct ReadyChange {
    ReadyChangeKind kind = ReadyChangeKind::None;
    std::shared_ptr<const AuthStateSnapshot> snapshot;
};

json user_json(const core::TdUserSummary& user) {
    return {{"id", user.id},
            {"first_name", user.first_name},
            {"last_name", user.last_name},
            {"usernames", user.usernames},
            {"phone_number", user.phone_number},
            {"is_bot", user.is_bot},
            {"is_premium", user.is_premium}};
}

class AuthTracker {
  public:
    AuthTracker(core::TdClient& client, RequestSession& session)
        : client_(client), session_(session) {
        subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
                observe(snapshot);
            });
        static_cast<void>(record(client_.auth_state()));
    }

    ~AuthTracker() {
        client_.unsubscribe_auth_states(subscription_);
    }

    AuthTracker(const AuthTracker&) = delete;
    AuthTracker& operator=(const AuthTracker&) = delete;
    AuthTracker(AuthTracker&&) = delete;
    AuthTracker& operator=(AuthTracker&&) = delete;

    std::shared_ptr<const AuthStateSnapshot> current() const {
        const std::lock_guard lock(mutex_);
        return latest_;
    }

    std::shared_ptr<const AuthStateSnapshot> consume() {
        const std::lock_guard lock(mutex_);
        if (pending_.empty()) {
            return latest_;
        }
        auto result = std::move(pending_.front());
        pending_.pop_front();
        return result;
    }

    std::shared_ptr<const AuthStateSnapshot> first_change_after(const AuthStateSnapshot& previous) {
        observe(client_.auth_state());
        const std::lock_guard lock(mutex_);
        const auto changed_from_previous = [&](const auto& candidate) {
            return candidate && (candidate->client_generation != previous.client_generation ||
                                 candidate->auth_sequence != previous.auth_sequence);
        };
        const auto found = std::ranges::find_if(pending_, changed_from_previous);
        if (found != pending_.end()) {
            return *found;
        }
        return changed_from_previous(latest_) ? latest_ : nullptr;
    }

    ReadyChange ready_change_after(const AuthStateSnapshot& previous) {
        observe(client_.auth_state());
        const std::lock_guard lock(mutex_);
        const auto after_previous = [&](const auto& candidate) {
            return candidate && std::tie(candidate->client_generation, candidate->auth_sequence) >
                                    std::tie(previous.client_generation, previous.auth_sequence);
        };
        const auto authorization_lost = [&](const auto& candidate) {
            return after_previous(candidate) && candidate->data.state != AuthState::Ready;
        };
        const auto lost = std::ranges::find_if(pending_, authorization_lost);
        if (lost != pending_.end()) {
            return {ReadyChangeKind::Lost, *lost};
        }
        if (after_previous(latest_) && latest_->data.state == AuthState::Ready) {
            return {ReadyChangeKind::Advanced, latest_};
        }
        return {};
    }

    std::shared_ptr<const AuthStateSnapshot>
    wait_after(const AuthStateSnapshot& previous, RequestSession::Clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, deadline, [&] {
            return !latest_ || latest_->client_generation != previous.client_generation ||
                   latest_->auth_sequence != previous.auth_sequence ||
                   session_.cancellation_requested();
        });
        return latest_;
    }

  private:
    bool record(const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
        const std::lock_guard lock(mutex_);
        if (!snapshot ||
            (latest_ && std::tie(snapshot->client_generation, snapshot->auth_sequence) <=
                            std::tie(latest_->client_generation, latest_->auth_sequence))) {
            return false;
        }
        latest_ = snapshot;
        pending_.push_back(snapshot);
        return true;
    }

    void observe(const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
        if (record(snapshot)) {
            session_.supersede(snapshot->client_generation, snapshot->auth_sequence);
        }
        cv_.notify_all();
    }

    core::TdClient& client_;
    RequestSession& session_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const AuthStateSnapshot> latest_;
    std::deque<std::shared_ptr<const AuthStateSnapshot>> pending_;
    std::uint64_t subscription_ = 0;
};

enum class WaitKind { Response, Updated, ReadyAdvanced, TimedOut, Cancelled, Failed };

struct WaitResult {
    WaitKind kind = WaitKind::Failed;
    core::TdValue value;
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    std::shared_ptr<const AuthStateSnapshot> snapshot;
};

WaitResult consume_query_response(std::future<core::TdValue>& response,
                                  const AuthStateSnapshot& sent, AuthTracker& tracker) {
    try {
        auto value = response.get();
        const auto first_change = tracker.first_change_after(sent);
        if (first_change && first_change->receive_event_sequence != 0 &&
            (value.receive_event_sequence() == 0 ||
             first_change->receive_event_sequence < value.receive_event_sequence())) {
            return {WaitKind::Updated, {}, std::nullopt, nullptr};
        }
        return {WaitKind::Response, std::move(value), std::nullopt, nullptr};
    } catch (const core::TdAuthorizationError& error) {
        if (tracker.first_change_after(sent)) {
            return {WaitKind::Updated, {}, std::nullopt, nullptr};
        }
        return {WaitKind::Failed, {}, error.failure(), nullptr};
    } catch (const std::exception&) {
        if (tracker.first_change_after(sent)) {
            return {WaitKind::Updated, {}, std::nullopt, nullptr};
        }
        return {WaitKind::Failed, {}, std::nullopt, nullptr};
    }
}

WaitResult wait_query(std::future<core::TdValue>& response, const AuthStateSnapshot& sent,
                      AuthTracker& tracker, RequestSession& session) {
    for (;;) {
        if (response.wait_for(0ms) == std::future_status::ready) {
            return consume_query_response(response, sent, tracker);
        }
        if (tracker.first_change_after(sent)) {
            return {WaitKind::Updated, {}, std::nullopt, nullptr};
        }
        if (RequestSession::Clock::now() >= session.deadline()) {
            return {WaitKind::TimedOut, {}, std::nullopt, nullptr};
        }
        if (session.cancellation_requested() &&
            session.in_flight_state() != InFlightState::Orphaned) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        std::this_thread::sleep_for(1ms);
    }
}

WaitResult consume_ready_query_response(std::future<core::TdValue>& response,
                                        const AuthStateSnapshot& sent, AuthTracker& tracker) {
    try {
        auto value = response.get();
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost && change.snapshot &&
            change.snapshot->receive_event_sequence != 0 &&
            (value.receive_event_sequence() == 0 ||
             change.snapshot->receive_event_sequence < value.receive_event_sequence())) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        return {WaitKind::Response, std::move(value), std::nullopt, nullptr};
    } catch (const core::TdAuthorizationError& error) {
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        if (change.kind == ReadyChangeKind::Advanced) {
            return {WaitKind::ReadyAdvanced, {}, std::nullopt, change.snapshot};
        }
        return {WaitKind::Failed, {}, error.failure(), nullptr};
    } catch (const std::exception&) {
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        if (change.kind == ReadyChangeKind::Advanced) {
            return {WaitKind::ReadyAdvanced, {}, std::nullopt, change.snapshot};
        }
        return {WaitKind::Failed, {}, std::nullopt, nullptr};
    }
}

WaitResult wait_ready_query(std::future<core::TdValue>& response, const AuthStateSnapshot& sent,
                            AuthTracker& tracker, RequestSession& session) {
    for (;;) {
        if (response.wait_for(0ms) == std::future_status::ready) {
            return consume_ready_query_response(response, sent, tracker);
        }
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        if (RequestSession::Clock::now() >= session.deadline()) {
            return {WaitKind::TimedOut, {}, std::nullopt, nullptr};
        }
        if (session.cancellation_requested() &&
            session.in_flight_state() != InFlightState::Orphaned) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        std::this_thread::sleep_for(1ms);
    }
}

WaitResult wait_get_me(core::TdClient& client, AuthTracker& tracker, RequestSession& session,
                       std::shared_ptr<const AuthStateSnapshot>& snapshot) {
    for (;;) {
        if (!session.reserve_direct_in_flight()) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        auto future = client.get_me(snapshot);
        auto waited = wait_ready_query(future, *snapshot, tracker, session);
        session.settle_in_flight();
        if (waited.kind != WaitKind::ReadyAdvanced) {
            return waited;
        }
        if (session.cancellation_requested()) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        if (!waited.snapshot || waited.snapshot->data.state != AuthState::Ready) {
            return {WaitKind::Failed, {}, std::nullopt, nullptr};
        }
        snapshot = std::move(waited.snapshot);
    }
}

std::string_view auth_function_name(TdFunctionKind function) {
    switch (function) {
    case TdFunctionKind::GetAuthorizationState:
    case TdFunctionKind::SetTdlibParameters:
    case TdFunctionKind::SetAuthenticationPhoneNumber:
    case TdFunctionKind::RequestQrCodeAuthentication:
    case TdFunctionKind::CheckAuthenticationBotToken:
    case TdFunctionKind::SetAuthenticationEmailAddress:
    case TdFunctionKind::CheckAuthenticationEmailCode:
    case TdFunctionKind::CheckAuthenticationCode:
    case TdFunctionKind::RegisterUser:
    case TdFunctionKind::CheckAuthenticationPassword:
    case TdFunctionKind::GetMe:
    case TdFunctionKind::LogOut:
    case TdFunctionKind::Close:
        return core::td_function_name(function);
    case TdFunctionKind::GetOption:
    case TdFunctionKind::GetSavedMessagesTags:
    case TdFunctionKind::SearchSavedMessages:
    case TdFunctionKind::GetActiveSessions:
    case TdFunctionKind::TerminateSession:
    case TdFunctionKind::GetChat:
    case TdFunctionKind::GetMessages:
    case TdFunctionKind::GetMessageLink:
    case TdFunctionKind::GetChats:
    case TdFunctionKind::LoadChats:
    case TdFunctionKind::SearchPublicChat:
    case TdFunctionKind::GetInternalLinkType:
    case TdFunctionKind::GetMessageLinkInfo:
    case TdFunctionKind::CheckChatInviteLink:
    case TdFunctionKind::GetUser:
    case TdFunctionKind::GetSupergroup:
    case TdFunctionKind::GetSupergroupFullInfo:
    case TdFunctionKind::CreatePrivateChat:
        return "other";
    }
    return "other";
}

void function_denied(RequestSession& session, std::string_view account,
                     const AuthStateSnapshot& snapshot, TdFunctionKind function) {
    session.error("AUTH_FUNCTION_DENIED", "TDLib authorization function was denied",
                  {{"account", account},
                   {"state", core::auth_state_name(snapshot.data.state)},
                   {"function", auth_function_name(function)}},
                  kDenied);
}

std::optional<std::string_view> credential_for(TdFunctionKind function, std::int32_t code,
                                               std::string_view message) {
    struct Entry {
        TdFunctionKind function;
        std::int32_t code;
        std::string_view message;
        std::string_view credential;
    };
    static constexpr std::array<Entry, 25> entries{{
        {TdFunctionKind::SetTdlibParameters, 400,
         "Valid api_id must be provided. Can be obtained at https://my.telegram.org",
         "app_credentials"},
        {TdFunctionKind::SetTdlibParameters, 400,
         "Valid api_hash must be provided. Can be obtained at https://my.telegram.org",
         "app_credentials"},
        {TdFunctionKind::SetTdlibParameters, 401, "Wrong database encryption key", "database_key"},
        {TdFunctionKind::SetAuthenticationPhoneNumber, 400, "Phone number must be non-empty",
         "phone_number"},
        {TdFunctionKind::SetAuthenticationPhoneNumber, 406, "PHONE_NUMBER_INVALID", "phone_number"},
        {TdFunctionKind::SetAuthenticationPhoneNumber, 400, "API_ID_INVALID", "app_credentials"},
        {TdFunctionKind::RequestQrCodeAuthentication, 400, "API_ID_INVALID", "app_credentials"},
        {TdFunctionKind::CheckAuthenticationBotToken, 400, "API_ID_INVALID", "app_credentials"},
        {TdFunctionKind::CheckAuthenticationBotToken, 400, "ACCESS_TOKEN_INVALID", "bot_token"},
        {TdFunctionKind::CheckAuthenticationBotToken, 400, "ACCESS_TOKEN_EXPIRED", "bot_token"},
        {TdFunctionKind::SetAuthenticationEmailAddress, 400, "Email address must be non-empty",
         "email_address"},
        {TdFunctionKind::SetAuthenticationEmailAddress, 400, "EMAIL_INVALID", "email_address"},
        {TdFunctionKind::CheckAuthenticationEmailCode, 400, "Code must be non-empty", "email_code"},
        {TdFunctionKind::CheckAuthenticationEmailCode, 400, "CODE_INVALID", "email_code"},
        {TdFunctionKind::CheckAuthenticationEmailCode, 400, "EMAIL_VERIFY_EXPIRED", "email_code"},
        {TdFunctionKind::CheckAuthenticationEmailCode, 400, "PHONE_CODE_EMPTY", "email_code"},
        {TdFunctionKind::CheckAuthenticationEmailCode, 400, "PHONE_CODE_INVALID", "email_code"},
        {TdFunctionKind::CheckAuthenticationEmailCode, 400, "PHONE_CODE_EXPIRED", "email_code"},
        {TdFunctionKind::CheckAuthenticationCode, 400, "PHONE_CODE_EMPTY", "authentication_code"},
        {TdFunctionKind::CheckAuthenticationCode, 400, "PHONE_CODE_INVALID", "authentication_code"},
        {TdFunctionKind::CheckAuthenticationCode, 400, "PHONE_CODE_EXPIRED", "authentication_code"},
        {TdFunctionKind::RegisterUser, 400, "First name must be non-empty", "registration_name"},
        {TdFunctionKind::RegisterUser, 400, "FIRSTNAME_INVALID", "registration_name"},
        {TdFunctionKind::RegisterUser, 400, "LASTNAME_INVALID", "registration_name"},
        {TdFunctionKind::CheckAuthenticationPassword, 400, "PASSWORD_HASH_INVALID", "password"},
    }};
    const auto* const found = std::ranges::find_if(entries, [&](const Entry& entry) {
        return entry.function == function && entry.code == code && entry.message == message;
    });
    return found == std::end(entries) ? std::nullopt
                                      : std::optional<std::string_view>{found->credential};
}

std::int32_t retry_after(std::string_view message) {
    static const std::regex pattern(
        R"((?:^|[^[:alnum:]_])(?:retry[[:space:]]+after[[:space:]]*|FLOOD_WAIT_)([0-9]+))",
        std::regex::icase);
    std::cmatch match;
    if (!std::regex_search(message.begin(), message.end(), match, pattern) || match.size() != 2) {
        return 0;
    }
    std::int32_t value = 0;
    for (const char character : match[1].str()) {
        const auto digit = static_cast<std::int32_t>(character - '0');
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        value = value > (maximum - digit) / 10 ? maximum : value * 10 + digit;
    }
    return value;
}

std::string_view delivery_name(core::AuthCodeDelivery delivery) {
    switch (delivery) {
    case core::AuthCodeDelivery::TelegramMessage:
        return "telegram_message";
    case core::AuthCodeDelivery::Sms:
        return "sms";
    case core::AuthCodeDelivery::SmsWord:
        return "sms_word";
    case core::AuthCodeDelivery::SmsPhrase:
        return "sms_phrase";
    case core::AuthCodeDelivery::Call:
        return "call";
    case core::AuthCodeDelivery::FlashCall:
        return "flash_call";
    case core::AuthCodeDelivery::MissedCall:
        return "missed_call";
    case core::AuthCodeDelivery::Fragment:
        return "fragment";
    case core::AuthCodeDelivery::FirebaseAndroid:
        return "firebase_android";
    case core::AuthCodeDelivery::FirebaseIos:
        return "firebase_ios";
    case core::AuthCodeDelivery::Unknown:
        return "unknown";
    }
    return "unknown";
}

void timeout(RequestSession& session, std::string_view operation,
             const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
    session.error(
        "TIMEOUT", "authentication deadline elapsed",
        {{"operation", operation},
         {"state", snapshot ? json(core::auth_state_name(snapshot->data.state)) : json(nullptr)}},
        kTimeout);
}

enum class InputKind { Value, Retry, Terminal };

void wipe(std::string& value) {
    secure::wipe(value);
}

struct InputResult {
    InputResult() = default;
    InputResult(InputKind kind_value, std::string value_value, bool boolean_value)
        : kind(kind_value), value(std::move(value_value)), boolean(boolean_value) {}
    ~InputResult() {
        wipe(value);
    }
    InputResult(const InputResult&) = delete;
    InputResult& operator=(const InputResult&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    InputResult(InputResult&& other) : kind(other.kind), boolean(other.boolean) {
        secure::transfer(other.value, value, {}, "login_input_move_source");
    }
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    InputResult& operator=(InputResult&& other) {
        if (this != &other) {
            wipe(value);
            kind = other.kind;
            boolean = other.boolean;
            secure::transfer(other.value, value, {}, "login_input_move_source");
        }
        return *this;
    }

    InputKind kind = InputKind::Terminal;
    std::string value;
    bool boolean = false;
};

void wipe(core::BootstrapAttempt& attempt) {
    if (attempt.prompted_app.api_hash) {
        wipe(*attempt.prompted_app.api_hash);
        attempt.prompted_app.api_hash.reset();
    }
    if (attempt.prompted_database_key) {
        wipe(*attempt.prompted_database_key);
        attempt.prompted_database_key.reset();
    }
}

InputResult challenge_value(RequestSession& session, const AuthStateSnapshot& snapshot,
                            std::string_view account, proto::ChallengeKind kind, std::string prompt,
                            json details = json::object(), bool reserves_query = true) {
    auto outcome = session.challenge({kind, snapshot.client_generation, snapshot.auth_sequence,
                                      std::move(prompt), std::move(details), reserves_query});
    switch (outcome.status()) {
    case ChallengeStatus::Answered: {
        InputResult result{InputKind::Value, {}, false};
        if (!outcome.take_string(result.value)) {
            const auto value = outcome.take_boolean();
            if (value) {
                result.boolean = *value;
            }
        }
        return result;
    }
    case ChallengeStatus::Superseded:
        return {InputKind::Retry, {}, false};
    case ChallengeStatus::NoTty:
        session.error("AUTH_INPUT_REQUIRED", "authentication input requires a TTY",
                      {{"account", account},
                       {"state", core::auth_state_name(snapshot.data.state)},
                       {"challenge", proto::challenge_kind_name(kind)}},
                      kNotAuthed);
        break;
    case ChallengeStatus::Cancelled:
        session.error("AUTH_CANCELLED", "authentication input was cancelled",
                      {{"account", account},
                       {"state", core::auth_state_name(snapshot.data.state)},
                       {"challenge", proto::challenge_kind_name(kind)}},
                      kNotAuthed);
        break;
    case ChallengeStatus::TimedOut:
        timeout(session, "login", std::make_shared<const AuthStateSnapshot>(snapshot));
        break;
    case ChallengeStatus::Disconnected:
    case ChallengeStatus::Shutdown:
    case ChallengeStatus::ProtocolError:
        break;
    }
    return {InputKind::Terminal, {}, false};
}

} // namespace

LoginCoordinator::LoginCoordinator(core::TdClient& client, const config::Store& store,
                                   paths::Environment environment, std::string account,
                                   std::string application_version,
                                   std::optional<std::string> environment_api_id,
                                   std::optional<std::string> environment_api_hash,
                                   core::AuthBootstrap::HookRunner hook_runner)
    : client_(client), store_(store), environment_(std::move(environment)),
      account_(std::move(account)), application_version_(std::move(application_version)),
      environment_api_id_(std::move(environment_api_id)),
      environment_api_hash_(std::move(environment_api_hash)), hook_runner_(std::move(hook_runner)) {
}

// Authentication is state-driven: a response never rewinds a newer snapshot.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void LoginCoordinator::login(const proto::Request& request, RequestSession& session) {
    const bool qr = request.args.value("qr", false);
    const bool bot = request.args.value("bot", false);
    if (qr && bot) {
        session.error("USAGE", "--qr and --bot are mutually exclusive",
                      {{"argument", "--qr/--bot"}, {"reason", "mutually_exclusive"}}, kUsage);
        return;
    }

    {
        const std::lock_guard lock(lease_mutex_);
        if (login_active_) {
            const auto state = client_.auth_state();
            session.error("AUTH_FLOW_IN_PROGRESS", "another login is already active",
                          {{"account", account_},
                           {"state", state ? core::auth_state_name(state->data.state) : "unknown"}},
                          kNotAuthed);
            return;
        }
        login_active_ = true;
    }
    struct Release {
        explicit Release(LoginCoordinator* value) : coordinator(value) {}
        LoginCoordinator* coordinator;
        bool transferred = false;
        ~Release() {
            if (transferred) {
                return;
            }
            const std::lock_guard lock(coordinator->lease_mutex_);
            coordinator->login_active_ = false;
        }
        Release(const Release&) = delete;
        Release& operator=(const Release&) = delete;
        Release(Release&&) = delete;
        Release& operator=(Release&&) = delete;
    } release{this};

    AuthTracker tracker(client_, session);
    auto owner_lease = client_.issue_login_owner();
    if (!owner_lease) {
        session.error("INTERNAL", "cannot acquire login owner",
                      {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    auto owner = owner_lease.owner();
    config::LoadResult fallback_admission;
    std::shared_ptr<const config::ConfigSnapshot> active_config_snapshot;
    std::optional<config::AccountConfig> admission_account_config;
    if (const auto& runtime_admission = session.admitted_config();
        runtime_admission && runtime_admission->account == account_) {
        active_config_snapshot = runtime_admission->account_snapshot;
        if (active_config_snapshot) {
            const auto found = active_config_snapshot->accounts.find(account_);
            if (found != active_config_snapshot->accounts.end()) {
                admission_account_config = found->second;
            }
        }
    } else {
        fallback_admission = store_.load({session.deadline(), session.cancellation_token()});
        if (fallback_admission && fallback_admission.snapshot) {
            active_config_snapshot = fallback_admission.snapshot;
            const auto found = fallback_admission.snapshot->accounts.find(account_);
            if (found != fallback_admission.snapshot->accounts.end()) {
                admission_account_config = found->second;
            }
        }
    }
    std::map<Occurrence, bool> force_prompt;
    std::map<Occurrence, std::string> rejected_credential;
    std::map<Occurrence, bool> hook_attempted;
    std::map<Occurrence, std::string> app_config_identity;
    struct AppReplacement {
        Occurrence occurrence;
        std::string expected_identity;
    };
    std::optional<AppReplacement> pending_app_replacement;
    bool app_environment_rejected = false;
    std::optional<Occurrence> qr_sent;
    std::optional<Occurrence> qr_rendered;
    std::unique_ptr<core::AuthBootstrap> bootstrap;
    std::optional<Occurrence> bootstrap_occurrence;

    auto restart_and_wait = [&](const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
        if (!snapshot || !client_.restart_generation(snapshot)) {
            return;
        }
        const auto deadline = RequestSession::Clock::now() + 30s;
        auto current = snapshot;
        while (current && current->client_generation == snapshot->client_generation &&
               RequestSession::Clock::now() < deadline) {
            current = tracker.wait_after(*current, deadline);
        }
    };

    auto transfer_timeout_lifecycle = [&] {
        const auto snapshot = tracker.current();
        if (!snapshot) {
            return;
        }
        const auto current = client_.auth_state();
        if (!current || current->client_generation != snapshot->client_generation) {
            return;
        }
        static_cast<void>(client_.restart_generation(snapshot));
        release.transferred = true;
        lifecycle_waiter_ = std::jthread(
            [this, generation = snapshot->client_generation,
             held_owner = std::move(owner_lease)](const std::stop_token& stop) mutable {
                while (!stop.stop_requested()) {
                    const auto live = client_.auth_state();
                    if (live && live->client_generation != generation) {
                        const std::lock_guard lock(lease_mutex_);
                        login_active_ = false;
                        return;
                    }
                    std::this_thread::sleep_for(10ms);
                }
            });
    };
    struct TimeoutLifecycle {
        TimeoutLifecycle(RequestSession& session_value, std::function<void()> transfer_value)
            : session(session_value), transfer(std::move(transfer_value)) {}
        RequestSession& session;
        std::function<void()> transfer;
        ~TimeoutLifecycle() {
            if (RequestSession::Clock::now() >= session.deadline()) {
                transfer();
            }
        }
        TimeoutLifecycle(const TimeoutLifecycle&) = delete;
        TimeoutLifecycle& operator=(const TimeoutLifecycle&) = delete;
        TimeoutLifecycle(TimeoutLifecycle&&) = delete;
        TimeoutLifecycle& operator=(TimeoutLifecycle&&) = delete;
    };
    const TimeoutLifecycle timeout_lifecycle{session, transfer_timeout_lifecycle};

    auto fail_td = [&](TdFunctionKind function, const AuthStateSnapshot& snapshot,
                       const core::TdError& error) -> bool {
        const auto credential = credential_for(function, error.code, error.message);
        if (credential) {
            session.progress({{"kind", "auth_retry"},
                              {"auth_sequence", snapshot.auth_sequence},
                              {"state", core::auth_state_name(snapshot.data.state)},
                              {"credential", *credential},
                              {"tdlib_code", error.code}});
            if (request.context.tty) {
                const Occurrence occurrence{snapshot.client_generation, snapshot.auth_sequence};
                if (*credential == "app_credentials") {
                    app_environment_rejected = true;
                    const auto identity = app_config_identity.find(occurrence);
                    if (identity == app_config_identity.end()) {
                        session.error("INTERNAL", "app credential admission was not captured",
                                      {{"operation", "login"}, {"reason", "internal_error"}},
                                      kGeneric);
                        return true;
                    }
                    pending_app_replacement = AppReplacement{occurrence, identity->second};
                } else {
                    force_prompt[occurrence] = true;
                    rejected_credential[occurrence] = std::string(*credential);
                }
                return false;
            }
            session.error("AUTH_CREDENTIAL_REJECTED", "authentication credential was rejected",
                          {{"account", account_},
                           {"state", core::auth_state_name(snapshot.data.state)},
                           {"credential", *credential},
                           {"tdlib_code", error.code}},
                          kNotAuthed);
            return true;
        }
        if (error.code == 429) {
            session.error("RATE_LIMITED", "Telegram rate limit",
                          {{"operation", "login"},
                           {"tdlib_code", 429},
                           {"retry_after", retry_after(error.message)}},
                          kRateLimited);
        } else {
            session.error("TDLIB_ERROR", "TDLib authentication request failed",
                          {{"operation", "login"}, {"tdlib_code", error.code}}, kGeneric);
        }
        return true;
    };

    auto submit = [&](const std::shared_ptr<const AuthStateSnapshot>& snapshot,
                      core::TdAuthRequest auth_request, bool reserved) -> bool {
        const auto function = auth_request.function;
        const Occurrence occurrence{snapshot->client_generation, snapshot->auth_sequence};
        if ((function == TdFunctionKind::SetAuthenticationPhoneNumber ||
             function == TdFunctionKind::RequestQrCodeAuthentication ||
             function == TdFunctionKind::CheckAuthenticationBotToken) &&
            !app_config_identity.contains(occurrence)) {
            if (!active_config_snapshot) {
                session.error(
                    "CONFIG_INVALID", "cannot capture app credential admission",
                    {{"path", store_.path()},
                     {"reason", fallback_admission.error
                                    ? config::reason_name(fallback_admission.error->reason)
                                    : "io_error"}},
                    kGeneric);
                return true;
            }
            app_config_identity.emplace(occurrence, active_config_snapshot->identity);
        }
        if (!(reserved ? session.reserve_in_flight() : session.reserve_direct_in_flight())) {
            return false;
        }
        auto future = client_.send_login(snapshot, owner, std::move(auth_request));
        auto waited = wait_query(future, *snapshot, tracker, session);
        session.settle_in_flight();
        if (waited.kind == WaitKind::Updated) {
            return session.cancellation_requested();
        }
        if (waited.kind == WaitKind::TimedOut) {
            if (!session.cancellation_requested()) {
                timeout(session, "login", tracker.current());
            }
            return true;
        }
        if (session.cancellation_requested() || waited.kind == WaitKind::Cancelled) {
            return true;
        }
        if (waited.authorization_failure) {
            function_denied(session, account_, *snapshot, function);
            return true;
        }
        if (waited.kind == WaitKind::Failed) {
            session.error("INTERNAL", "authentication request failed locally",
                          {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
            return true;
        }
        if (const auto* error = waited.value.get_if<core::TdError>()) {
            return fail_td(function, *snapshot, *error);
        }
        return false;
    };

    for (;;) {
        auto snapshot = tracker.consume();
        if (!snapshot) {
            session.error("INTERNAL", "authorization state is unavailable",
                          {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
            return;
        }
        if (session.cancellation_requested() && session.in_flight_state() == InFlightState::None) {
            return;
        }
        if (RequestSession::Clock::now() >= session.deadline()) {
            timeout(session, "login", snapshot);
            return;
        }
        if (snapshot->data.unsupported_tdlib_type_id) {
            session.error("UNSUPPORTED_AUTH_STATE", "unsupported TDLib authorization state",
                          {{"account", account_},
                           {"tdlib_type_id", *snapshot->data.unsupported_tdlib_type_id}},
                          kGeneric);
            return;
        }

        const Occurrence occurrence{snapshot->client_generation, snapshot->auth_sequence};
        if (!client_.owns(owner, snapshot->client_generation)) {
            owner_lease = client_.issue_login_owner();
            if (!owner_lease) {
                session.error("INTERNAL", "cannot resume login on the current TDLib generation",
                              {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                return;
            }
            owner = owner_lease.owner();
        }
        if (pending_app_replacement) {
            if (pending_app_replacement->occurrence != occurrence) {
                pending_app_replacement.reset();
            } else {
                auto api_id = challenge_value(session, *snapshot, account_,
                                              proto::ChallengeKind::ApiId, "Telegram api_id: ");
                if (api_id.kind == InputKind::Retry) {
                    continue;
                }
                if (api_id.kind != InputKind::Value) {
                    return;
                }
                std::int32_t parsed_api_id = 0;
                if (!secret_hook::parse_api_id(api_id.value, parsed_api_id)) {
                    continue;
                }
                auto api_hash =
                    challenge_value(session, *snapshot, account_, proto::ChallengeKind::ApiHash,
                                    "Telegram api_hash: ");
                if (api_hash.kind == InputKind::Retry) {
                    continue;
                }
                if (api_hash.kind != InputKind::Value) {
                    return;
                }
                const auto expected_identity = pending_app_replacement->expected_identity;
                config::PromptedAppCredentials prompted{.api_id = parsed_api_id,
                                                        .api_hash = std::nullopt};
                prompted.api_hash.emplace(api_hash.value);
                wipe(api_hash.value);
                const auto replacement = store_.replace_app_credentials(
                    expected_identity, account_, prompted,
                    {session.deadline(), session.cancellation_token()});
                if (prompted.api_hash) {
                    wipe(*prompted.api_hash);
                    prompted.api_hash.reset();
                }
                if (replacement.status != config::MutationStatus::Applied) {
                    if (replacement.status == config::MutationStatus::Conflict) {
                        session.error(
                            "CONFIG_CONFLICT", "cannot replace rejected app credentials",
                            {{"path", store_.path()},
                             {"expected", expected_identity},
                             {"current", replacement.snapshot ? replacement.snapshot->identity
                                                              : std::string{}}},
                            kGeneric);
                    } else if (replacement.status == config::MutationStatus::TimedOut) {
                        timeout(session, "login", snapshot);
                    } else if (replacement.status != config::MutationStatus::Cancelled) {
                        session.error("CONFIG_INVALID", "cannot replace rejected app credentials",
                                      {{"path", store_.path()},
                                       {"reason", replacement.error ? config::reason_name(
                                                                          replacement.error->reason)
                                                                    : "io_error"}},
                                      kGeneric);
                    }
                    return;
                }
                active_config_snapshot = replacement.snapshot;
                pending_app_replacement.reset();
                restart_and_wait(snapshot);
                if (const auto current = tracker.current();
                    !current || current->client_generation == snapshot->client_generation) {
                    session.error("INTERNAL", "cannot replace rejected TDLib generation",
                                  {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                    return;
                }
                bootstrap.reset();
                bootstrap_occurrence.reset();
                continue;
            }
        }
        switch (snapshot->data.state) {
        case AuthState::Unknown:
        case AuthState::LoggingOut:
        case AuthState::Closing:
        case AuthState::Closed:
            tracker.wait_after(*snapshot, session.deadline());
            continue;
        case AuthState::WaitTdlibParameters: {
            if (!bootstrap || bootstrap_occurrence != occurrence) {
                if (!active_config_snapshot) {
                    if (fallback_admission.timed_out) {
                        timeout(session, "login", snapshot);
                    } else {
                        session.error(
                            "CONFIG_INVALID", "cannot load authentication config",
                            {{"path", store_.path()},
                             {"reason", fallback_admission.error
                                            ? config::reason_name(fallback_admission.error->reason)
                                            : "io_error"}},
                            kGeneric);
                    }
                    return;
                }
                app_config_identity[occurrence] = active_config_snapshot->identity;
                auto captured = core::capture_bootstrap_snapshot(
                    account_, active_config_snapshot, environment_, environment_.test_dc,
                    application_version_,
                    app_environment_rejected ? std::nullopt : environment_api_id_,
                    app_environment_rejected ? std::nullopt : environment_api_hash_);
                if (!captured.snapshot) {
                    session.error("CONFIG_INVALID", "invalid authentication bootstrap config",
                                  {{"path", store_.path()}, {"reason", "path_invalid"}}, kGeneric);
                    return;
                }
                bootstrap = std::make_unique<core::AuthBootstrap>(
                    client_, store_, std::move(*captured.snapshot), hook_runner_);
                bootstrap_occurrence = occurrence;
            }

            core::BootstrapAttempt attempt{
                {session.deadline(), session.cancellation_token()}, false, {}, {}};
            if (force_prompt[occurrence] && rejected_credential[occurrence] == "database_key") {
                auto database_key =
                    challenge_value(session, *snapshot, account_, proto::ChallengeKind::DatabaseKey,
                                    "Database encryption key: ");
                if (database_key.kind == InputKind::Retry) {
                    continue;
                }
                if (database_key.kind != InputKind::Value) {
                    return;
                }
                if (!bootstrap->retry_after_rejection(snapshot)) {
                    continue;
                }
                attempt.interactive = true;
                attempt.prompted_database_key.emplace(database_key.value);
                wipe(database_key.value);
                force_prompt.erase(occurrence);
                rejected_credential.erase(occurrence);
            }
            auto result = bootstrap->run(snapshot, attempt);
            if (result.error && (result.error->failure == core::BootstrapFailure::InputRequired ||
                                 (result.error->failure == core::BootstrapFailure::HookFailed &&
                                  request.context.tty))) {
                const bool database_key_only =
                    result.error->fields.size() == 1 &&
                    result.error->fields.front() == secret_hook::HookField::DatabaseKey;
                if (database_key_only) {
                    auto database_key = challenge_value(session, *snapshot, account_,
                                                        proto::ChallengeKind::DatabaseKey,
                                                        "Database encryption key: ");
                    if (database_key.kind == InputKind::Retry) {
                        continue;
                    }
                    if (database_key.kind != InputKind::Value) {
                        return;
                    }
                    attempt.interactive = true;
                    attempt.prompted_database_key.emplace(database_key.value);
                    wipe(database_key.value);
                    result = bootstrap->run(snapshot, attempt);
                } else {
                    auto api_id = challenge_value(session, *snapshot, account_,
                                                  proto::ChallengeKind::ApiId, "Telegram api_id: ");
                    if (api_id.kind != InputKind::Value) {
                        if (api_id.kind == InputKind::Retry) {
                            continue;
                        }
                        return;
                    }
                    std::int32_t parsed_api_id = 0;
                    if (!secret_hook::parse_api_id(api_id.value, parsed_api_id)) {
                        continue;
                    }
                    auto api_hash =
                        challenge_value(session, *snapshot, account_, proto::ChallengeKind::ApiHash,
                                        "Telegram api_hash: ");
                    if (api_hash.kind != InputKind::Value) {
                        if (api_hash.kind == InputKind::Retry) {
                            continue;
                        }
                        return;
                    }
                    attempt.interactive = true;
                    attempt.prompted_app.api_id = parsed_api_id;
                    attempt.prompted_app.api_hash.emplace(api_hash.value);
                    wipe(api_hash.value);
                    result = bootstrap->run(snapshot, attempt);
                }
            }
            wipe(attempt);
            if (result.error) {
                const auto failure = result.error->failure;
                if (failure == core::BootstrapFailure::HookFailed && result.error->hook) {
                    session.error(
                        "HOOK_FAILED", "authentication credential hook failed",
                        {{"hook", secret_hook::field_name(result.error->hook->field)},
                         {"reason", secret_hook::failure_name(result.error->hook->reason)},
                         {"status", result.error->hook->status ? json(*result.error->hook->status)
                                                               : json(nullptr)}},
                        kGeneric);
                } else if (failure == core::BootstrapFailure::TimedOut) {
                    timeout(session, "login", snapshot);
                } else if (failure == core::BootstrapFailure::Cancelled) {
                    return;
                } else if (failure == core::BootstrapFailure::ConfigConflict) {
                    session.error("CONFIG_CONFLICT",
                                  "authentication config changed before persistence",
                                  {{"path", store_.path()},
                                   {"expected", app_config_identity[occurrence]},
                                   {"current", result.materialized_snapshot
                                                   ? result.materialized_snapshot->identity
                                                   : std::string{}}},
                                  kGeneric);
                } else {
                    session.error("CONFIG_INVALID", "authentication bootstrap failed",
                                  {{"path", store_.path()}, {"reason", "io_error"}}, kGeneric);
                }
                return;
            }
            if (result.materialized_snapshot) {
                active_config_snapshot = result.materialized_snapshot;
                app_config_identity[occurrence] = result.materialized_snapshot->identity;
            }
            if (!result.response) {
                session.error("INTERNAL", "bootstrap omitted its TDLib response",
                              {{"operation", "auth_bootstrap"}, {"reason", "internal_error"}},
                              kGeneric);
                return;
            }
            if (!(attempt.interactive ? session.reserve_in_flight()
                                      : session.reserve_direct_in_flight())) {
                return;
            }
            auto waited = wait_query(*result.response, *snapshot, tracker, session);
            session.settle_in_flight();
            if (waited.kind == WaitKind::Updated) {
                if (session.cancellation_requested()) {
                    return;
                }
                continue;
            }
            if (waited.kind == WaitKind::TimedOut) {
                if (!session.cancellation_requested()) {
                    timeout(session, "login", tracker.current());
                }
                return;
            }
            if (session.cancellation_requested()) {
                return;
            }
            if (waited.authorization_failure) {
                function_denied(session, account_, *snapshot, TdFunctionKind::SetTdlibParameters);
                return;
            }
            if (waited.kind != WaitKind::Response) {
                return;
            }
            if (const auto* error = waited.value.get_if<core::TdError>()) {
                if (fail_td(TdFunctionKind::SetTdlibParameters, *snapshot, *error)) {
                    return;
                }
                continue;
            }
            tracker.wait_after(*snapshot, session.deadline());
            continue;
        }
        case AuthState::WaitPhoneNumber: {
            if (qr) {
                if (qr_sent == occurrence) {
                    tracker.wait_after(*snapshot, session.deadline());
                    continue;
                }
                qr_sent = occurrence;
                if (submit(snapshot,
                           core::TdAuthRequest{TdFunctionKind::RequestQrCodeAuthentication},
                           false)) {
                    return;
                }
                continue;
            }
            std::string credential;
            bool reserved = false;
            if (bot && !force_prompt[occurrence]) {
                if (admission_account_config && admission_account_config->bot_token_cmd &&
                    !hook_attempted[occurrence]) {
                    hook_attempted[occurrence] = true;
                    auto hook = hook_runner_({secret_hook::HookField::BotToken,
                                              *admission_account_config->bot_token_cmd,
                                              session.deadline(), session.cancellation_token()});
                    if (hook) {
                        secure::transfer(hook.value, credential, {}, "bot_hook_value_source");
                    } else if (!request.context.tty) {
                        if (hook.cancelled) {
                            return;
                        }
                        session.error(
                            "HOOK_FAILED", "bot token hook failed",
                            {{"hook", "bot_token_cmd"},
                             {"reason",
                              hook.error ? secret_hook::failure_name(hook.error->reason) : "spawn"},
                             {"status", hook.error && hook.error->status ? json(*hook.error->status)
                                                                         : json(nullptr)}},
                            kGeneric);
                        return;
                    }
                }
            }
            if (credential.empty()) {
                const auto kind =
                    bot ? proto::ChallengeKind::BotToken : proto::ChallengeKind::PhoneNumber;
                auto input = challenge_value(session, *snapshot, account_, kind,
                                             bot ? "Bot token: " : "Phone number: ");
                if (input.kind == InputKind::Retry) {
                    continue;
                }
                if (input.kind != InputKind::Value) {
                    return;
                }
                secure::transfer(input.value, credential, {}, "login_input_value_source");
                reserved = true;
            }
            const auto function = bot ? TdFunctionKind::CheckAuthenticationBotToken
                                      : TdFunctionKind::SetAuthenticationPhoneNumber;
            if (submit(snapshot, core::TdAuthRequest{function, std::move(credential)}, reserved)) {
                return;
            }
            continue;
        }
        case AuthState::WaitPremiumPurchase: {
            const auto* metadata =
                std::get_if<core::AuthWaitPremiumPurchase>(&snapshot->data.metadata);
            if (metadata == nullptr) {
                session.error("INTERNAL", "premium state metadata is missing",
                              {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                return;
            }
            session.error("AUTH_PREMIUM_REQUIRED", "Telegram Premium purchase is required",
                          {{"account", account_},
                           {"state", "wait_premium_purchase"},
                           {"store_product_id", metadata->store_product_id},
                           {"premium_day_count", metadata->premium_day_count},
                           {"support_email_address", metadata->support_email_address},
                           {"support_email_subject", metadata->support_email_subject}},
                          kNotAuthed);
            return;
        }
        case AuthState::WaitEmailAddress: {
            auto input = challenge_value(session, *snapshot, account_,
                                         proto::ChallengeKind::EmailAddress, "Email address: ");
            if (input.kind == InputKind::Retry) {
                continue;
            }
            if (input.kind != InputKind::Value) {
                return;
            }
            if (submit(snapshot,
                       core::TdAuthRequest{TdFunctionKind::SetAuthenticationEmailAddress,
                                           std::move(input.value)},
                       true)) {
                return;
            }
            continue;
        }
        case AuthState::WaitEmailCode: {
            const auto* metadata = std::get_if<core::AuthWaitEmailCode>(&snapshot->data.metadata);
            if (metadata == nullptr || metadata->unsupported_reset_tdlib_type_id) {
                session.error("UNSUPPORTED_AUTH_STATE", "unsupported email authorization state",
                              {{"account", account_},
                               {"tdlib_type_id",
                                metadata != nullptr && metadata->unsupported_reset_tdlib_type_id
                                    ? *metadata->unsupported_reset_tdlib_type_id
                                    : 0}},
                              kGeneric);
                return;
            }
            auto input = challenge_value(session, *snapshot, account_,
                                         proto::ChallengeKind::EmailCode, "Email code: ",
                                         {{"address_pattern", metadata->email_address_pattern},
                                          {"expected_length", metadata->expected_length}});
            if (input.kind == InputKind::Retry) {
                continue;
            }
            if (input.kind != InputKind::Value) {
                return;
            }
            if (submit(snapshot,
                       core::TdAuthRequest{TdFunctionKind::CheckAuthenticationEmailCode,
                                           std::move(input.value)},
                       true)) {
                return;
            }
            continue;
        }
        case AuthState::WaitCode: {
            const auto* metadata = std::get_if<core::AuthWaitCode>(&snapshot->data.metadata);
            if (metadata == nullptr || metadata->delivery.unsupported_tdlib_type_id ||
                metadata->delivery.type == core::AuthCodeDelivery::Unknown) {
                session.error("UNSUPPORTED_AUTH_STATE", "unsupported authentication code type",
                              {{"account", account_},
                               {"tdlib_type_id",
                                metadata != nullptr && metadata->delivery.unsupported_tdlib_type_id
                                    ? *metadata->delivery.unsupported_tdlib_type_id
                                    : 0}},
                              kGeneric);
                return;
            }
            auto input =
                challenge_value(session, *snapshot, account_,
                                proto::ChallengeKind::AuthenticationCode, "Authentication code: ",
                                {{"delivery_type", delivery_name(metadata->delivery.type)},
                                 {"expected_length", metadata->delivery.expected_length
                                                         ? json(*metadata->delivery.expected_length)
                                                         : json(nullptr)},
                                 {"resend_timeout", metadata->resend_timeout}});
            if (input.kind == InputKind::Retry) {
                continue;
            }
            if (input.kind != InputKind::Value) {
                return;
            }
            if (submit(snapshot,
                       core::TdAuthRequest{TdFunctionKind::CheckAuthenticationCode,
                                           std::move(input.value)},
                       true)) {
                return;
            }
            continue;
        }
        case AuthState::WaitOtherDeviceConfirmation: {
            const auto* metadata =
                std::get_if<core::AuthWaitOtherDeviceConfirmation>(&snapshot->data.metadata);
            if (metadata == nullptr) {
                session.error("INTERNAL", "QR authorization metadata is missing",
                              {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                return;
            }
            if (qr_rendered != occurrence) {
                session.progress({{"kind", "auth_qr"},
                                  {"auth_sequence", snapshot->auth_sequence},
                                  {"link", metadata->link}});
                qr_rendered = occurrence;
            }
            tracker.wait_after(*snapshot, session.deadline());
            continue;
        }
        case AuthState::WaitRegistration: {
            const auto* metadata =
                std::get_if<core::AuthWaitRegistration>(&snapshot->data.metadata);
            if (metadata == nullptr) {
                session.error("INTERNAL", "registration metadata is missing",
                              {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                return;
            }
            auto terms = challenge_value(session, *snapshot, account_,
                                         proto::ChallengeKind::RegistrationTerms,
                                         "Accept Telegram terms? [y/N] ",
                                         {{"text", metadata->terms_text},
                                          {"min_user_age", metadata->minimum_user_age},
                                          {"show_popup", metadata->show_popup}});
            if (terms.kind == InputKind::Retry) {
                continue;
            }
            if (terms.kind != InputKind::Value) {
                return;
            }
            if (!terms.boolean) {
                session.error("AUTH_CANCELLED", "registration terms were declined",
                              {{"account", account_},
                               {"state", "wait_registration"},
                               {"challenge", "registration_terms"}},
                              kNotAuthed);
                return;
            }
            InputResult first;
            for (;;) {
                first =
                    challenge_value(session, *snapshot, account_,
                                    proto::ChallengeKind::RegistrationFirstName, "First name: ");
                if (first.kind != InputKind::Value || !first.value.empty()) {
                    break;
                }
            }
            if (first.kind == InputKind::Retry) {
                continue;
            }
            if (first.kind != InputKind::Value) {
                return;
            }
            auto last = challenge_value(session, *snapshot, account_,
                                        proto::ChallengeKind::RegistrationLastName,
                                        "Last name (optional): ");
            if (last.kind == InputKind::Retry) {
                continue;
            }
            if (last.kind != InputKind::Value) {
                return;
            }
            if (submit(snapshot,
                       core::TdAuthRequest{TdFunctionKind::RegisterUser, std::move(first.value),
                                           std::move(last.value)},
                       true)) {
                return;
            }
            continue;
        }
        case AuthState::WaitPassword: {
            const auto* metadata = std::get_if<core::AuthWaitPassword>(&snapshot->data.metadata);
            if (metadata == nullptr) {
                session.error("INTERNAL", "password metadata is missing",
                              {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                return;
            }
            std::string credential;
            bool reserved = false;
            if (!force_prompt[occurrence] && !hook_attempted[occurrence]) {
                if (admission_account_config && admission_account_config->password_cmd) {
                    hook_attempted[occurrence] = true;
                    auto hook = hook_runner_({secret_hook::HookField::Password,
                                              *admission_account_config->password_cmd,
                                              session.deadline(), session.cancellation_token()});
                    if (hook) {
                        secure::transfer(hook.value, credential, {}, "password_hook_value_source");
                    } else if (!request.context.tty) {
                        if (hook.cancelled) {
                            return;
                        }
                        session.error(
                            "HOOK_FAILED", "password hook failed",
                            {{"hook", "password_cmd"},
                             {"reason",
                              hook.error ? secret_hook::failure_name(hook.error->reason) : "spawn"},
                             {"status", hook.error && hook.error->status ? json(*hook.error->status)
                                                                         : json(nullptr)}},
                            kGeneric);
                        return;
                    }
                }
            }
            if (credential.empty()) {
                auto input = challenge_value(
                    session, *snapshot, account_, proto::ChallengeKind::Password, "2FA password: ",
                    {{"hint", metadata->hint},
                     {"has_recovery_email", metadata->has_recovery_email_address},
                     {"has_passport_data", metadata->has_passport_data},
                     {"recovery_email_pattern", metadata->recovery_email_address_pattern}});
                if (input.kind == InputKind::Retry) {
                    continue;
                }
                if (input.kind != InputKind::Value) {
                    return;
                }
                secure::transfer(input.value, credential, {}, "login_input_value_source");
                reserved = true;
            }
            if (submit(snapshot,
                       core::TdAuthRequest{TdFunctionKind::CheckAuthenticationPassword,
                                           std::move(credential)},
                       reserved)) {
                return;
            }
            continue;
        }
        case AuthState::Ready: {
            auto ready_snapshot = snapshot;
            auto waited = wait_get_me(client_, tracker, session, ready_snapshot);
            if (waited.kind == WaitKind::Updated) {
                if (session.cancellation_requested()) {
                    return;
                }
                session.error(
                    "NOT_AUTHED", "authorization was lost before login identity completed",
                    {{"account", account_},
                     {"state", waited.snapshot ? core::auth_state_name(waited.snapshot->data.state)
                                               : "unknown"},
                     {"reason", "authorization_lost"}},
                    kNotAuthed);
                return;
            }
            if (waited.kind == WaitKind::TimedOut) {
                if (!session.cancellation_requested()) {
                    timeout(session, "login", tracker.current());
                }
                return;
            }
            if (session.cancellation_requested() || waited.kind == WaitKind::Cancelled) {
                return;
            }
            if (waited.authorization_failure) {
                function_denied(session, account_, *ready_snapshot, TdFunctionKind::GetMe);
                return;
            }
            if (waited.kind != WaitKind::Response) {
                return;
            }
            if (const auto* error = waited.value.get_if<core::TdError>()) {
                if (error->code == 429) {
                    session.error("RATE_LIMITED", "Telegram rate limit",
                                  {{"operation", "login"},
                                   {"tdlib_code", 429},
                                   {"retry_after", retry_after(error->message)}},
                                  kRateLimited);
                } else {
                    session.error("TDLIB_ERROR", "getMe failed",
                                  {{"operation", "login"}, {"tdlib_code", error->code}}, kGeneric);
                }
                return;
            }
            const auto* user = waited.value.get_if<core::TdUserSummary>();
            if (user == nullptr) {
                session.error("INTERNAL", "getMe returned an unexpected object",
                              {{"operation", "login"}, {"reason", "internal_error"}}, kGeneric);
                return;
            }
            session.result(
                {{"account", account_}, {"auth_state", "ready"}, {"user", user_json(*user)}});
            return;
        }
        }
    }
}

void LoginCoordinator::me(const proto::Request& request, RequestSession& session) {
    static_cast<void>(request);
    AuthTracker tracker(client_, session);
    auto snapshot = tracker.current();
    if (!snapshot || snapshot->data.state != AuthState::Ready) {
        session.error(
            "NOT_AUTHED", "account is not authorized",
            {{"account", account_},
             {"state", snapshot ? core::auth_state_name(snapshot->data.state) : "unknown"},
             {"reason", "not_ready"}},
            kNotAuthed);
        return;
    }
    auto waited = wait_get_me(client_, tracker, session, snapshot);
    if (waited.kind == WaitKind::Updated) {
        if (session.cancellation_requested()) {
            return;
        }
        session.error(
            "NOT_AUTHED", "authorization was lost",
            {{"account", account_},
             {"state",
              waited.snapshot ? core::auth_state_name(waited.snapshot->data.state) : "unknown"},
             {"reason", "authorization_lost"}},
            kNotAuthed);
        return;
    }
    if (waited.kind == WaitKind::TimedOut) {
        if (!session.cancellation_requested()) {
            timeout(session, "me", tracker.current());
        }
        return;
    }
    if (session.cancellation_requested() || waited.kind == WaitKind::Cancelled) {
        return;
    }
    if (waited.authorization_failure) {
        function_denied(session, account_, *snapshot, TdFunctionKind::GetMe);
        return;
    }
    if (waited.kind != WaitKind::Response) {
        return;
    }
    if (const auto* error = waited.value.get_if<core::TdError>()) {
        if (error->code == 429) {
            session.error("RATE_LIMITED", "Telegram rate limit",
                          {{"operation", "me"},
                           {"tdlib_code", 429},
                           {"retry_after", retry_after(error->message)}},
                          kRateLimited);
        } else {
            session.error("TDLIB_ERROR", "getMe failed",
                          {{"operation", "me"}, {"tdlib_code", error->code}}, kGeneric);
        }
        return;
    }
    const auto* user = waited.value.get_if<core::TdUserSummary>();
    if (user == nullptr) {
        session.error("INTERNAL", "getMe returned an unexpected object",
                      {{"operation", "me"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    session.result(user_json(*user));
}

void register_login_commands(Dispatcher& dispatcher, LoginCoordinator& coordinator) {
    dispatcher.register_command("login", {Tier::Read, [&coordinator](const proto::Request& request,
                                                                     RequestSession& session) {
                                              coordinator.login(request, session);
                                          }});
    dispatcher.register_command(
        "me", {Tier::Read, [&coordinator](const proto::Request& request, RequestSession& session) {
                   coordinator.me(request, session);
               }});
}

} // namespace tgcli::daemon
