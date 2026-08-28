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

struct StampedFile {
    core::TdFile file;
    std::uint64_t sequence = 0;
    std::optional<core::TdEventClock::time_point> observed_at;
};

class FileUpdateQueue final {
  public:
    FileUpdateQueue(core::TdClient& client, std::int32_t file_id)
        : client_(client), file_id_(file_id),
          subscription_(client.subscribe_updates([this](const core::TdValue& value) {
              const auto* file = value.get_if<core::TdUpdateFile>();
              const auto* malformed = value.get_if<core::TdMalformedSupportedUpdate>();
              if (file == nullptr &&
                  (malformed == nullptr || malformed->kind != core::TdSupportedUpdateKind::File)) {
                  return;
              }
              if (file != nullptr && file->file.id != file_id_) {
                  return;
              }
              {
                  const std::lock_guard lock(mutex_);
                  if (file != nullptr) {
                      if (pending_.size() == kMaximumPending) {
                          malformed_ = true;
                      } else {
                          pending_.push_back({.file = file->file,
                                              .sequence = value.receive_event_sequence(),
                                              .observed_at = value.receive_observed_at()});
                      }
                  } else {
                      malformed_ = true;
                  }
              }
              cv_.notify_all();
          })) {}

    ~FileUpdateQueue() {
        client_.get().unsubscribe_updates(subscription_);
    }
    FileUpdateQueue(const FileUpdateQueue&) = delete;
    FileUpdateQueue& operator=(const FileUpdateQueue&) = delete;
    FileUpdateQueue(FileUpdateQueue&&) = delete;
    FileUpdateQueue& operator=(FileUpdateQueue&&) = delete;

    std::vector<StampedFile> take() {
        const std::lock_guard lock(mutex_);
        std::vector<StampedFile> result;
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

    void wait_until(core::TdEventClock::time_point deadline) {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, deadline, [&] { return malformed_ || !pending_.empty(); });
    }

  private:
    static constexpr std::size_t kMaximumPending = 4'096;
    std::reference_wrapper<core::TdClient> client_;
    std::int32_t file_id_ = 0;
    std::uint64_t subscription_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<StampedFile> pending_;
    bool malformed_ = false;
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

enum class StopReason { None, Cancelled, Shutdown, TimedOut, AuthorizationLost, Malformed };

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
               std::string_view account, RequestSession& session) {
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
        authorization_lost(client, resolver, account, session);
        return;
    case StopReason::Malformed:
        internal(session);
        return;
    }
}

bool process_file_events(std::vector<StampedFile> events, DownloadFileTracker& tracker,
                         RequestSession& session, StopReason& stop) {
    std::ranges::sort(events, {}, &StampedFile::sequence);
    for (const auto& item : events) {
        if (item.sequence == 0 || !item.observed_at) {
            stop = StopReason::Malformed;
            return false;
        }
        if (!event_precedes_deadline(item.observed_at, session.deadline())) {
            stop = StopReason::TimedOut;
            return false;
        }
        auto event = tracker.observe(item.file, false);
        if (event.status == DownloadFileEventStatus::Malformed ||
            event.status == DownloadFileEventStatus::ConflictingCompletion) {
            stop = StopReason::Malformed;
            return false;
        }
        if (event.advisory_progress) {
            session.progress(std::move(*event.advisory_progress));
        }
    }
    return true;
}

void emit_filesystem_error(const DownloadFilesystemError& failure, RequestSession& session) {
    if (failure.kind == DownloadFilesystemErrorKind::Stopped) {
        return;
    }
    if (failure.kind == DownloadFilesystemErrorKind::OutputExists) {
        session.error("OUTPUT_EXISTS", "download output already exists",
                      {{"operation", "download"}, {"path", failure.final_path}}, kGeneric);
        return;
    }
    session.error("OUTPUT_UNAVAILABLE", "download output is unavailable",
                  {{"operation", "download"},
                   {"path", failure.final_path},
                   {"reason", download_filesystem_reason_name(failure.reason)}},
                  kGeneric);
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): ordered download protocol.
void DownloadCoordinator::download(const proto::Request& request, RequestSession& session) {
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

    FileUpdateQueue updates(client_.get(), media.file.id);
    auto download_read = resolver.read_target([&](const auto& authorization) {
        return client_.get().download_file(authorization, media.file.id);
    });
    if (read_stopped(download_read, client_.get(), account_, session)) {
        return;
    }
    if (const auto* error = download_read.value.get_if<core::TdError>()) {
        td_error(session, *error);
        return;
    }
    const auto* response = download_read.value.get_if<core::TdFile>();
    if (response == nullptr || download_read.value.receive_event_sequence() == 0 ||
        !download_read.value.receive_observed_at()) {
        internal(session);
        return;
    }
    DownloadFileTracker tracker(media.file.id);
    auto pending = updates.take();
    pending.push_back({.file = *response,
                       .sequence = download_read.value.receive_event_sequence(),
                       .observed_at = download_read.value.receive_observed_at()});
    std::ranges::sort(pending, {}, &StampedFile::sequence);
    StopReason stop = StopReason::None;
    for (const auto& item : pending) {
        if (item.sequence == 0 || !item.observed_at ||
            !event_precedes_deadline(item.observed_at, session.deadline())) {
            stop = item.sequence == 0 || !item.observed_at ? StopReason::Malformed
                                                           : StopReason::TimedOut;
            break;
        }
        auto event = tracker.observe(item.file,
                                     item.sequence == download_read.value.receive_event_sequence());
        if (event.status == DownloadFileEventStatus::Malformed ||
            event.status == DownloadFileEventStatus::ConflictingCompletion) {
            stop = StopReason::Malformed;
            break;
        }
        if (event.advisory_progress) {
            session.progress(std::move(*event.advisory_progress));
        }
    }
    while (stop == StopReason::None && !tracker.completed_file()) {
        stop = current_stop(client_.get(), resolver, session);
        if (stop != StopReason::None) {
            break;
        }
        if (updates.malformed()) {
            stop = StopReason::Malformed;
            break;
        }
        const auto wake = session.deadline().expires_at.value_or(core::TdEventClock::now() +
                                                                 std::chrono::milliseconds(50));
        updates.wait_until(
            std::min(wake, core::TdEventClock::now() + std::chrono::milliseconds(50)));
        if (!process_file_events(updates.take(), tracker, session, stop)) {
            break;
        }
    }
    if (stop != StopReason::None) {
        emit_stop(stop, client_.get(), resolver, account_, session);
        return;
    }

    const auto control = [&] {
        if (updates.malformed()) {
            stop = StopReason::Malformed;
            return false;
        }
        if (!process_file_events(updates.take(), tracker, session, stop)) {
            return false;
        }
        stop = current_stop(client_.get(), resolver, session);
        return stop == StopReason::None;
    };
    auto published =
        publish_download_file(*tracker.completed_file(), plan, control, filesystem_hooks_);
    if (const auto* failure = std::get_if<DownloadFilesystemError>(&published)) {
        if (failure->kind == DownloadFilesystemErrorKind::Stopped) {
            emit_stop(stop, client_.get(), resolver, account_, session);
        } else {
            emit_filesystem_error(*failure, session);
        }
        return;
    }
    const auto& result = std::get<PublishedDownload>(published);
    session.progress({{"operation", "download"},
                      {"file_id", media.file.id},
                      {"downloaded_bytes", result.bytes},
                      {"total_bytes", result.bytes}});
    session.result({{"chat_id", target.chat.id},
                    {"message_id", arguments->message_id},
                    {"file_id", media.file.id},
                    {"media_type", download_media_type_name(media.media_type)},
                    {"path", result.path},
                    {"bytes", result.bytes}});
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
