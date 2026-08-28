#include "daemon/download_commands.hpp"

#include "common/deadline.hpp"
#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/download_domain.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

struct DownloadArguments {
    std::string chat;
    std::int64_t message_id = 0;
    std::optional<std::string> output;
};

using DownloadEventPayload = std::variant<core::TdFile, core::TdError, std::monostate>;

struct StampedDownloadEvent {
    DownloadEventPayload payload;
    bool response = false;
    std::uint64_t sequence = 0;
    std::optional<core::TdEventClock::time_point> observed_at;
};

bool same_authorization(const core::AuthStateSnapshot& left, const core::AuthStateSnapshot& right);

class DownloadEventQueue final {
  public:
    DownloadEventQueue(core::TdClient& client,
                       std::shared_ptr<const core::AuthStateSnapshot> authorization,
                       std::int32_t file_id)
        : client_(client), authorization_(std::move(authorization)), file_id_(file_id),
          subscription_(client.subscribe_ordered_updates([this](const core::TdValue& value) {
              const auto current = client_.get().auth_state();
              const auto* file = value.get_if<core::TdUpdateFile>();
              const auto* malformed = value.get_if<core::TdMalformedSupportedUpdate>();
              {
                  const std::lock_guard lock(mutex_);
                  if (phase_ != Phase::Open) {
                      return;
                  }
                  if (!authorization_lost_ && (!current || !authorization_ ||
                                               !same_authorization(*authorization_, *current))) {
                      authorization_lost_ = current;
                  }
                  if (file != nullptr && file->file.id == file_id_) {
                      if (pending_.size() == kMaximumPending) {
                          malformed_ = true;
                      } else {
                          pending_.push_back({.payload = file->file,
                                              .response = false,
                                              .sequence = value.receive_event_sequence(),
                                              .observed_at = value.receive_observed_at()});
                      }
                  } else if (malformed != nullptr &&
                             malformed->kind == core::TdSupportedUpdateKind::File) {
                      malformed_ = true;
                  }
              }
              cv_.notify_all();
          })) {}

    ~DownloadEventQueue() {
        client_.get().unsubscribe_ordered_updates(subscription_);
    }
    DownloadEventQueue(const DownloadEventQueue&) = delete;
    DownloadEventQueue& operator=(const DownloadEventQueue&) = delete;
    DownloadEventQueue(DownloadEventQueue&&) = delete;
    DownloadEventQueue& operator=(DownloadEventQueue&&) = delete;

    void enqueue_response(const core::TdValue& value) {
        DownloadEventPayload payload = std::monostate{};
        if (const auto* file = value.get_if<core::TdFile>()) {
            payload = *file;
        } else if (const auto* error = value.get_if<core::TdError>()) {
            payload = *error;
        }
        const std::lock_guard lock(mutex_);
        if (phase_ != Phase::Open || pending_.size() == kMaximumPending) {
            malformed_ = true;
            return;
        }
        pending_.push_back({.payload = std::move(payload),
                            .response = true,
                            .sequence = value.receive_event_sequence(),
                            .observed_at = value.receive_observed_at()});
        cv_.notify_all();
    }

    std::vector<StampedDownloadEvent> take() {
        const std::lock_guard lock(mutex_);
        std::vector<StampedDownloadEvent> result;
        result.reserve(pending_.size());
        while (!pending_.empty()) {
            result.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
        return result;
    }

    bool malformed() const {
        const std::lock_guard lock(mutex_);
        return malformed_;
    }

    std::shared_ptr<const core::AuthStateSnapshot> authorization_lost() const {
        const std::lock_guard lock(mutex_);
        return authorization_lost_;
    }

    bool claim() {
        const std::lock_guard lock(mutex_);
        if (phase_ != Phase::Open) {
            return false;
        }
        phase_ = Phase::Claimed;
        return true;
    }

    bool claimed() const {
        const std::lock_guard lock(mutex_);
        return phase_ == Phase::Claimed;
    }

    void wait_until(core::TdEventClock::time_point deadline) {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, deadline, [&] { return malformed_ || !pending_.empty(); });
    }

  private:
    enum class Phase { Open, Claimed };
    static constexpr std::size_t kMaximumPending = 4'096;
    std::reference_wrapper<core::TdClient> client_;
    std::shared_ptr<const core::AuthStateSnapshot> authorization_;
    std::int32_t file_id_ = 0;
    std::uint64_t subscription_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<StampedDownloadEvent> pending_;
    std::shared_ptr<const core::AuthStateSnapshot> authorization_lost_;
    bool malformed_ = false;
    Phase phase_ = Phase::Open;
};

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    return value.is_object() && value.size() == expected.size() &&
           std::ranges::all_of(expected,
                               [&](const std::string& name) { return value.contains(name); });
}

std::optional<std::int64_t> integer64(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    if (value.is_number_unsigned()) {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    }
    return value.get<std::int64_t>();
}

std::optional<DownloadArguments> parse_arguments(const json& args, RequestSession& session) {
    static const std::set<std::string> fields{"chat", "message_id", "output"};
    if (!exact_fields(args, fields) || !args["chat"].is_string() ||
        (!args["output"].is_null() && !args["output"].is_string())) {
        session.error("USAGE", "download arguments are invalid",
                      {{"argument", "download"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const auto message_id = integer64(args["message_id"]);
    const auto& chat = args["chat"].get_ref<const std::string&>();
    if (!message_id || !core::valid_td_nonzero_int53(*message_id) || chat.empty() ||
        !common::valid_utf8(chat)) {
        session.error(
            "USAGE", "download locator is invalid",
            {{"argument", !message_id || !core::valid_td_nonzero_int53(message_id.value_or(0))
                              ? "msg-id"
                              : "chat"},
             {"reason", "invalid_argument"}},
            kUsage);
        return std::nullopt;
    }
    std::optional<std::string> output;
    if (args["output"].is_string()) {
        output = args["output"].get<std::string>();
        if (output->empty() || !common::valid_utf8(*output)) {
            session.error("USAGE", "download output is invalid",
                          {{"argument", "-O"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
    }
    return DownloadArguments{.chat = chat, .message_id = *message_id, .output = std::move(output)};
}

void internal(RequestSession& session, std::string_view reason = "malformed_tdlib_response") {
    session.error("INTERNAL", "download returned an unexpected object",
                  {{"operation", "download"}, {"reason", reason}}, kGeneric);
}

std::int32_t retry_after(std::string_view message) {
    constexpr std::string_view prefix = "FLOOD_WAIT_";
    const auto position = message.find(prefix);
    if (position == std::string_view::npos) {
        return 0;
    }
    std::int32_t result = 0;
    for (const auto byte : message.substr(position + prefix.size())) {
        if (byte < '0' || byte > '9') {
            break;
        }
        const auto digit = static_cast<std::int32_t>(byte - '0');
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        result = result > (maximum - digit) / 10 ? maximum : result * 10 + digit;
    }
    return result;
}

void td_error(RequestSession& session, const core::TdError& error) {
    if (error.code == 429) {
        session.error("RATE_LIMITED", "Telegram rate limit",
                      {{"operation", "download"},
                       {"tdlib_code", 429},
                       {"retry_after", retry_after(error.message)}},
                      kRateLimited);
        return;
    }
    session.error("TDLIB_ERROR", "download TDLib request failed",
                  {{"operation", "download"}, {"tdlib_code", error.code}}, kGeneric);
}

bool read_stopped(const ReadyReadResult& result, core::TdClient& client, std::string_view account,
                  RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        session.error(
            "NOT_AUTHED", "download requires authentication",
            {{"account", account},
             {"state", result.snapshot ? json(core::auth_state_name(result.snapshot->data.state))
                                       : json("unknown")},
             {"reason", "authorization_lost"}},
            kNotAuthed);
        return true;
    case ReadyReadStatus::TimedOut: {
        const auto current = client.auth_state();
        session.error(
            "TIMEOUT", "download request timed out",
            {{"operation", "download"},
             {"state", current ? json(core::auth_state_name(current->data.state)) : json(nullptr)}},
            kTimeout);
        return true;
    }
    case ReadyReadStatus::Cancelled:
        if (session.shutdown_requested()) {
            session.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                          {{"reason", "daemon_shutdown"}}, kGeneric);
        }
        return true;
    case ReadyReadStatus::Failed:
        internal(session, "internal_error");
        return true;
    }
    return true;
}

bool same_authorization(const core::AuthStateSnapshot& left, const core::AuthStateSnapshot& right) {
    return left.client_id == right.client_id && left.client_generation == right.client_generation &&
           left.auth_sequence == right.auth_sequence && left.data.state == right.data.state;
}

void authorization_lost(core::TdClient& client, ResolverConsumer& resolver,
                        std::string_view account, RequestSession& session) {
    auto state = resolver.first_non_ready_after_bound();
    if (!state) {
        state = client.auth_state();
    }
    session.error(
        "NOT_AUTHED", "download requires authentication",
        {{"account", account},
         {"state", state ? json(core::auth_state_name(state->data.state)) : json("unknown")},
         {"reason", "authorization_lost"}},
        kNotAuthed);
}

enum class StopReason {
    None,
    Cancelled,
    Shutdown,
    TimedOut,
    AuthorizationLost,
    Malformed,
    TdError
};

struct DownloadArbitration {
    explicit DownloadArbitration(std::int32_t file_id) : tracker(file_id) {}
    DownloadFileTracker tracker;
    StopReason stop = StopReason::None;
    std::optional<core::TdError> td_error;
    std::uint64_t last_sequence = 0;
};

StopReason current_stop(core::TdClient& client, ResolverConsumer& resolver,
                        RequestSession& session) {
    if (session.cancellation_requested()) {
        return session.shutdown_requested() ? StopReason::Shutdown : StopReason::Cancelled;
    }
    const auto first_non_ready = resolver.first_non_ready_after_bound();
    if (first_non_ready &&
        event_precedes_deadline(first_non_ready->receive_observed_at, session.deadline())) {
        return StopReason::AuthorizationLost;
    }
    const auto bound = resolver.bound_authorization();
    const auto current = client.auth_state();
    if (!bound || !current || !same_authorization(*bound, *current)) {
        if (current && !event_precedes_deadline(current->receive_observed_at, session.deadline())) {
            return StopReason::TimedOut;
        }
        return StopReason::AuthorizationLost;
    }
    if (deadline_expired(session.deadline())) {
        return StopReason::TimedOut;
    }
    return StopReason::None;
}

void emit_stop(StopReason stop, core::TdClient& client, ResolverConsumer& resolver,
               std::string_view account, RequestSession& session,
               const std::optional<core::TdError>& error = std::nullopt,
               const std::shared_ptr<const core::AuthStateSnapshot>& auth_loss = {}) {
    switch (stop) {
    case StopReason::None:
    case StopReason::Cancelled:
        return;
    case StopReason::Shutdown:
        session.error("DAEMON_SHUTDOWN", "daemon is shutting down", {{"reason", "daemon_shutdown"}},
                      kGeneric);
        return;
    case StopReason::TimedOut: {
        const auto current = client.auth_state();
        session.error(
            "TIMEOUT", "download request timed out",
            {{"operation", "download"},
             {"state", current ? json(core::auth_state_name(current->data.state)) : json(nullptr)}},
            kTimeout);
        return;
    }
    case StopReason::AuthorizationLost:
        if (auth_loss) {
            session.error("NOT_AUTHED", "download requires authentication",
                          {{"account", account},
                           {"state", core::auth_state_name(auth_loss->data.state)},
                           {"reason", "authorization_lost"}},
                          kNotAuthed);
        } else {
            authorization_lost(client, resolver, account, session);
        }
        return;
    case StopReason::Malformed:
        internal(session);
        return;
    case StopReason::TdError:
        if (error) {
            td_error(session, *error);
        } else {
            internal(session);
        }
        return;
    }
}

bool process_file_events(std::vector<StampedDownloadEvent> events, DownloadArbitration& state,
                         RequestSession& session) {
    std::ranges::sort(events, {}, &StampedDownloadEvent::sequence);
    for (const auto& item : events) {
        if (item.sequence == 0 || item.sequence <= state.last_sequence || !item.observed_at) {
            state.stop = StopReason::Malformed;
            return false;
        }
        state.last_sequence = item.sequence;
        if (state.tracker.completed_file()) {
            const auto* file = std::get_if<core::TdFile>(&item.payload);
            if (file == nullptr || !file->local || !file->local->is_downloading_completed) {
                continue;
            }
            const auto event = state.tracker.observe(*file, item.response);
            if (event.status == DownloadFileEventStatus::Malformed ||
                event.status == DownloadFileEventStatus::ConflictingCompletion) {
                state.stop = StopReason::Malformed;
                return false;
            }
            continue;
        }
        if (!event_precedes_deadline(item.observed_at, session.deadline())) {
            state.stop = StopReason::TimedOut;
            return false;
        }
        if (const auto* error = std::get_if<core::TdError>(&item.payload)) {
            state.td_error = *error;
            state.stop = StopReason::TdError;
            return false;
        }
        const auto* file = std::get_if<core::TdFile>(&item.payload);
        if (file == nullptr) {
            state.stop = StopReason::Malformed;
            return false;
        }
        auto event = state.tracker.observe(*file, item.response);
        if (event.status == DownloadFileEventStatus::Malformed ||
            event.status == DownloadFileEventStatus::ConflictingCompletion) {
            state.stop = StopReason::Malformed;
            return false;
        }
        if (event.advisory_progress) {
            session.progress(std::move(*event.advisory_progress));
        }
    }
    return true;
}

void emit_filesystem_error(const DownloadFilesystemError& failure, RequestSession& session,
                           bool terminal_batch = false) {
    if (failure.kind == DownloadFilesystemErrorKind::Stopped) {
        return;
    }
    if (failure.kind == DownloadFilesystemErrorKind::OutputExists) {
        if (terminal_batch) {
            static_cast<void>(session.fail_terminal_batch(
                "OUTPUT_EXISTS", "download output already exists",
                {{"operation", "download"}, {"path", failure.final_path}}, kGeneric));
        } else {
            session.error("OUTPUT_EXISTS", "download output already exists",
                          {{"operation", "download"}, {"path", failure.final_path}}, kGeneric);
        }
        return;
    }
    auto details = json{{"operation", "download"},
                        {"path", failure.final_path},
                        {"reason", download_filesystem_reason_name(failure.reason)}};
    if (terminal_batch) {
        static_cast<void>(session.fail_terminal_batch(
            "OUTPUT_UNAVAILABLE", "download output is unavailable", std::move(details), kGeneric));
    } else {
        session.error("OUTPUT_UNAVAILABLE", "download output is unavailable", std::move(details),
                      kGeneric);
    }
}

StopReason terminal_batch_stop(TerminalBatchStatus status) {
    switch (status) {
    case TerminalBatchStatus::Designated:
        return StopReason::None;
    case TerminalBatchStatus::Disconnected:
        return StopReason::Cancelled;
    case TerminalBatchStatus::Shutdown:
        return StopReason::Shutdown;
    case TerminalBatchStatus::TimedOut:
        return StopReason::TimedOut;
    case TerminalBatchStatus::ProtocolError:
        return StopReason::Malformed;
    }
    return StopReason::Malformed;
}

bool reject_unsupported_context(const proto::RequestContext& context, RequestSession& session) {
    std::string_view argument;
    if (context.dry_run) {
        argument = "--dry-run";
    } else if (context.yes) {
        argument = "--yes";
    } else if (context.write_authority != proto::WriteAuthority::Unset) {
        argument = "--allow-write";
    } else if (context.idempotency_key) {
        argument = "--idempotency-key";
    } else {
        return false;
    }
    session.error("USAGE", "download does not accept write options",
                  {{"argument", argument}, {"reason", "unsupported_mode"}}, kUsage);
    return true;
}

bool reject_invalid_cwd(std::string_view cwd, RequestSession& session) {
    constexpr std::size_t kMaximumCwdBytes = 4'096;
    if (!cwd.empty() && cwd.size() <= kMaximumCwdBytes && cwd.front() == '/' &&
        common::valid_utf8(cwd) && cwd != kUnavailableDownloadCwd) {
        return false;
    }
    emit_filesystem_error({.kind = DownloadFilesystemErrorKind::OutputUnavailable,
                           .reason = DownloadFilesystemReason::InvalidPath,
                           .final_path = std::string(kUnavailableDownloadCwd)},
                          session);
    return true;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): ordered download protocol.
void DownloadCoordinator::download(const proto::Request& request, RequestSession& session) {
    if (reject_unsupported_context(request.context, session)) {
        return;
    }
    if (reject_invalid_cwd(request.context.cwd, session)) {
        return;
    }
    const auto arguments = parse_arguments(request.args, session);
    if (!arguments) {
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal = resolver.bind_principal(M2Operation::Download);
    if (const auto* error = std::get_if<ResolverError>(&principal)) {
        emit_resolver_error(*error, session, M2Operation::Download);
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal)) {
        return;
    }
    const auto resolved = resolver.resolve_chat(arguments->chat, ResolverScope::ActiveDialogs);
    if (const auto* error = std::get_if<ResolverError>(&resolved)) {
        emit_resolver_error(*error, session, M2Operation::Download);
        return;
    }
    if (std::holds_alternative<ResolverStop>(resolved)) {
        return;
    }
    const auto target = std::get<ResolvedChatTarget>(resolved);
    if (!target.observed_chat || target.observed_chat->kind == core::TdChatKind::Secret ||
        target.observed_chat->kind == core::TdChatKind::Unknown) {
        emit_resolver_error(
            ResolverError{ResolverUsageError{.argument = "chat",
                                             .reason = ResolverUsageReason::UnsupportedChatType}},
            session, M2Operation::Download);
        return;
    }
    auto message_read = resolver.read_target([&](const auto& authorization) {
        return client_.get().get_download_message(authorization, target.chat.id,
                                                  arguments->message_id);
    });
    if (read_stopped(message_read, client_.get(), account_, session)) {
        return;
    }
    if (const auto* error = message_read.value.get_if<core::TdError>()) {
        if (error->code == 404 || (error->code == 400 && error->message == "Message not found")) {
            session.error("NOT_FOUND", "message not found",
                          {{"chat_id", target.chat.id}, {"message_id", arguments->message_id}},
                          kNotFound);
        } else {
            td_error(session, *error);
        }
        return;
    }
    const auto* message = message_read.value.get_if<core::TdDownloadMessage>();
    if (message == nullptr || message->id != arguments->message_id ||
        message->chat_id != target.chat.id) {
        internal(session);
        return;
    }
    const auto selected = select_download_media(*message);
    if (const auto* media_error = std::get_if<DownloadMediaError>(&selected)) {
        if (*media_error == DownloadMediaError::Malformed) {
            internal(session);
        } else {
            session.error("PRECONDITION_FAILED", "message media cannot be downloaded",
                          {{"operation", "download"},
                           {"chat_id", target.chat.id},
                           {"message_id", arguments->message_id},
                           {"reason", download_media_error_reason(*media_error)}},
                          kGeneric);
        }
        return;
    }
    const auto media = std::get<DownloadMediaSelection>(selected);
    auto destination = prepare_download_destination(arguments->output, request.context.media_dir,
                                                    request.context.cwd);
    if (const auto* failure = std::get_if<DownloadFilesystemError>(&destination)) {
        emit_filesystem_error(*failure, session);
        return;
    }
    auto plan = std::get<DownloadDestination>(std::move(destination));
    if (plan.directory_mode) {
        auto name_read = resolver.read_target([&](const auto& authorization) {
            return client_.get().get_suggested_file_name(authorization, media.file.id,
                                                         plan.directory);
        });
        if (read_stopped(name_read, client_.get(), account_, session)) {
            return;
        }
        if (const auto* error = name_read.value.get_if<core::TdError>()) {
            td_error(session, *error);
            return;
        }
        const auto* suggested = name_read.value.get_if<core::TdSuggestedFileName>();
        if (suggested == nullptr) {
            emit_filesystem_error({.kind = DownloadFilesystemErrorKind::OutputUnavailable,
                                   .reason = DownloadFilesystemReason::InvalidPath,
                                   .final_path = plan.directory},
                                  session);
            return;
        }
        auto named = apply_suggested_file_name(std::move(plan), suggested->value);
        if (const auto* failure = std::get_if<DownloadFilesystemError>(&named)) {
            emit_filesystem_error(*failure, session);
            return;
        }
        plan = std::get<DownloadDestination>(std::move(named));
    }

    const auto bound_authorization = resolver.bound_authorization();
    if (!bound_authorization) {
        internal(session);
        return;
    }
    DownloadEventQueue updates(client_.get(), bound_authorization, media.file.id);
    auto download_read = resolver.read_target([&](const auto& authorization) {
        return client_.get().download_file(authorization, media.file.id);
    });
    if (read_stopped(download_read, client_.get(), account_, session)) {
        return;
    }
    updates.enqueue_response(download_read.value);
    DownloadArbitration arbitration(media.file.id);
    static_cast<void>(process_file_events(updates.take(), arbitration, session));
    while (arbitration.stop == StopReason::None && !arbitration.tracker.completed_file()) {
        arbitration.stop = current_stop(client_.get(), resolver, session);
        if (arbitration.stop != StopReason::None) {
            break;
        }
        if (updates.malformed()) {
            arbitration.stop = StopReason::Malformed;
            break;
        }
        const auto wake = session.deadline().expires_at.value_or(core::TdEventClock::now() +
                                                                 std::chrono::milliseconds(50));
        updates.wait_until(
            std::min(wake, core::TdEventClock::now() + std::chrono::milliseconds(50)));
        if (!process_file_events(updates.take(), arbitration, session)) {
            break;
        }
    }
    if (arbitration.stop != StopReason::None) {
        emit_stop(arbitration.stop, client_.get(), resolver, account_, session,
                  arbitration.td_error, updates.authorization_lost());
        return;
    }

    const auto control = [&] {
        const auto ordered_events = client_.get().lock_ordered_events();
        static_cast<void>(ordered_events);
        if (updates.malformed()) {
            arbitration.stop = StopReason::Malformed;
            return false;
        }
        if (!process_file_events(updates.take(), arbitration, session)) {
            return false;
        }
        if (updates.authorization_lost()) {
            arbitration.stop = StopReason::AuthorizationLost;
            return false;
        }
        arbitration.stop = current_stop(client_.get(), resolver, session);
        if (arbitration.stop != StopReason::None) {
            return false;
        }
        arbitration.stop = terminal_batch_stop(session.begin_terminal_batch());
        return arbitration.stop == StopReason::None && updates.claim();
    };
    auto published = publish_download_file(*arbitration.tracker.completed_file(), plan, control,
                                           filesystem_hooks_);
    if (const auto* failure = std::get_if<DownloadFilesystemError>(&published)) {
        if (failure->kind == DownloadFilesystemErrorKind::Stopped) {
            emit_stop(arbitration.stop, client_.get(), resolver, account_, session,
                      arbitration.td_error, updates.authorization_lost());
        } else {
            emit_filesystem_error(*failure, session, updates.claimed());
        }
        return;
    }
    const auto& result = std::get<PublishedDownload>(published);
    static_cast<void>(
        session.complete_terminal_batch({{"operation", "download"},
                                         {"file_id", media.file.id},
                                         {"downloaded_bytes", result.bytes},
                                         {"total_bytes", result.bytes}},
                                        {{"chat_id", target.chat.id},
                                         {"message_id", arguments->message_id},
                                         {"file_id", media.file.id},
                                         {"media_type", download_media_type_name(media.media_type)},
                                         {"path", result.path},
                                         {"bytes", result.bytes}}));
}

void register_download_command(Dispatcher& dispatcher, DownloadCoordinator& coordinator) {
    dispatcher.register_command(
        "download", {.tier = Tier::Read,
                     .handler =
                         [&coordinator](const proto::Request& request, RequestSession& session) {
                             coordinator.download(request, session);
                         },
                     .deadline_default = DeadlineDefault::Unlimited,
                     .config_admission = true});
}

} // namespace tgcli::daemon
