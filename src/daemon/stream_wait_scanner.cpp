#include "daemon/stream_wait_scanner.hpp"

#include "daemon/message_summary.hpp"
#include "daemon/request_session.hpp"

#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;
using MessageKey = std::pair<std::int64_t, std::int64_t>;
inline constexpr std::size_t kHistoryKeyBytes = sizeof(std::int64_t) * 2;
static_assert(kHistoryKeyBytes == 16);

struct LiveRecord {
    MessageSummary message;
    std::size_t bytes = 0;
};

std::size_t message_wire_bytes(const MessageSummary& message) {
    return message_summary_json(message).dump().size() + 1;
}

void claim_internal(StreamSubscriptionWorker& worker) noexcept {
    static_cast<void>(
        worker.claim({.cause = StreamTerminalCause::Internal, .metadata_failure = {}}));
}

void claim_pagination(StreamSubscriptionWorker& worker) noexcept {
    static_cast<void>(
        worker.claim({.cause = StreamTerminalCause::PaginationInvalid, .metadata_failure = {}}));
}

} // namespace

class StreamWaitMatchState::Impl {
  public:
    explicit Impl(StreamMessageMatcher matcher_value) : matcher(std::move(matcher_value)) {}

    [[nodiscard]] bool add_live(const StreamCopiedItem& item, StreamSubscriptionWorker& worker) {
        auto message = parse_stream_message_item(item);
        if (!message || message->chat_id != chat_id) {
            claim_internal(worker);
            return false;
        }
        const MessageKey key{message->chat_id, message->id};
        if (history_keys.contains(key) || live_records.contains(key)) {
            return true;
        }
        if (!add_charge(item.wire_bytes, true, worker)) {
            return false;
        }
        live_records.emplace(key,
                             LiveRecord{.message = std::move(*message), .bytes = item.wire_bytes});
        live_order.push_back(key);
        return true;
    }

    [[nodiscard]] bool add_history_key(const MessageKey& key, StreamSubscriptionWorker& worker) {
        if (history_keys.contains(key)) {
            return true;
        }
        if (const auto found = live_records.find(key); found != live_records.end()) {
            remove_charge(found->second.bytes);
            live_records.erase(found);
        }
        if (!add_charge(kHistoryKeyBytes, false, worker)) {
            return false;
        }
        history_keys.insert(key);
        return true;
    }

    [[nodiscard]] bool consider_history(MessageSummary message, StreamSubscriptionWorker& worker) {
        if (!matcher.matches(message)) {
            return true;
        }
        if (history_candidate && message.id >= history_candidate->id) {
            return true;
        }
        if (history_candidate) {
            remove_charge(history_candidate_bytes);
            history_candidate.reset();
            history_candidate_bytes = 0;
        }
        const auto bytes = message_wire_bytes(message);
        if (!add_charge(bytes, true, worker)) {
            return false;
        }
        history_candidate = std::move(message);
        history_candidate_bytes = bytes;
        return true;
    }

    void finish() {
        if (history_candidate) {
            initial = message_summary_json(*history_candidate);
        } else {
            for (const auto& key : live_order) {
                const auto found = live_records.find(key);
                if (found != live_records.end() && found->second.message.id > after &&
                    matcher.matches(found->second.message)) {
                    initial = message_summary_json(found->second.message);
                    break;
                }
            }
        }
        live_records.clear();
        live_order.clear();
        history_candidate.reset();
        history_candidate_bytes = 0;
        queued_items = history_keys.size();
        queued_bytes = history_keys.size() * kHistoryKeyBytes;
    }

    [[nodiscard]] std::optional<nlohmann::json> match_live(const StreamCopiedItem& item) const {
        auto message = parse_stream_message_item(item);
        if (!message || message->chat_id != chat_id) {
            return std::nullopt;
        }
        const MessageKey key{message->chat_id, message->id};
        if (message->id <= after || history_keys.contains(key) || !matcher.matches(*message)) {
            return std::nullopt;
        }
        return message_summary_json(*message);
    }

    StreamMessageMatcher matcher;
    std::int64_t chat_id = 0;
    std::int64_t after = 0;
    std::map<MessageKey, LiveRecord> live_records;
    std::vector<MessageKey> live_order;
    std::set<MessageKey> history_keys;
    std::optional<MessageSummary> history_candidate;
    std::size_t history_candidate_bytes = 0;
    std::optional<nlohmann::json> initial;
    std::uint64_t queued_items = 0;
    std::uint64_t queued_bytes = 0;

  private:
    [[nodiscard]] bool add_charge(std::size_t bytes, bool item, StreamSubscriptionWorker& worker) {
        if ((item && bytes > kStreamQueueItemBytes) || queued_items >= kStreamQueueItems ||
            bytes > kStreamQueueBytes - queued_bytes) {
            return overflow(worker, bytes);
        }
        queued_items += 1;
        queued_bytes += bytes;
        return true;
    }

    void remove_charge(std::size_t bytes) noexcept {
        --queued_items;
        queued_bytes -= bytes;
    }

    [[nodiscard]] bool overflow(StreamSubscriptionWorker& worker,
                                std::size_t incoming) const noexcept {
        static_cast<void>(worker.claim({.cause = StreamTerminalCause::HistoryOverlap,
                                        .limit_items = kStreamQueueItems,
                                        .limit_bytes = kStreamQueueBytes,
                                        .queued_items = queued_items,
                                        .queued_bytes = queued_bytes,
                                        .incoming_bytes = incoming,
                                        .metadata_failure = {}}));
        return false;
    }
};

StreamWaitMatchState::StreamWaitMatchState(StreamMessageMatcher matcher)
    : impl_(std::make_unique<Impl>(std::move(matcher))) {}

StreamWaitMatchState::~StreamWaitMatchState() = default;

std::optional<nlohmann::json> StreamWaitMatchState::initial_match() const {
    return impl_->initial;
}

std::optional<nlohmann::json> StreamWaitMatchState::match_live(const StreamCopiedItem& item) const {
    return impl_->match_live(item);
}

class detail::StreamWaitScannerRun {
  private:
    struct PageResult {
        std::int64_t last_new_id = 0;
        bool boundary = false;
        bool past_after = false;
    };

    struct StructuredPage {
        std::vector<const core::TdMessageSummary*> messages;
        bool boundary = false;
    };

  public:
    StreamWaitScannerRun(RequestSession& session, StreamSubscriptionWorker& worker,
                         const StreamWaitScannerOptions& options)
        : session_(&session), worker_(&worker), options_(&options),
          state_(std::shared_ptr<StreamWaitMatchState>(new StreamWaitMatchState(options.matcher))),
          schedule_(now()) {
        state_->impl_->chat_id = options.chat_id;
        state_->impl_->after = options.after;
    }

    StreamWaitScanResult run() {
        if (!valid_options() || !wait_for_publication()) {
            return {};
        }
        std::int64_t from_message_id = 0;
        std::optional<std::int64_t> previous_raw;
        std::set<std::int64_t> consumed_raw;
        for (;;) {
            const auto page = next_page(from_message_id, previous_raw, consumed_raw);
            if (!page) {
                return {};
            }
            if (page->boundary || page->past_after) {
                break;
            }
            from_message_id = page->last_new_id;
        }
        if (!drain_live()) {
            return {};
        }
        state_->impl_->finish();
        return {state_};
    }

  private:
    std::optional<PageResult> next_page(std::int64_t from_message_id,
                                        std::optional<std::int64_t>& previous_raw,
                                        std::set<std::int64_t>& consumed_raw) {
        if (poll_stop()) {
            return std::nullopt;
        }
        auto response = options_->start_history({options_->chat_id, from_message_id, 0, 100, true});
        if (!wait_response(response)) {
            return std::nullopt;
        }
        core::TdValue value;
        try {
            value = response.get();
        } catch (const std::exception&) {
            if (!worker_->terminal_snapshot()) {
                claim_internal(*worker_);
            }
            return std::nullopt;
        }
        if (const auto* error = value.get_if<core::TdError>()) {
            const auto cause = error->code == 429 ? StreamTerminalCause::RateLimited
                                                  : StreamTerminalCause::TdlibError;
            static_cast<void>(worker_->claim(
                {.cause = cause,
                 .tdlib_code = error->code,
                 .retry_after = error->code == 429 ? stream_retry_after_seconds(error->message) : 0,
                 .metadata_failure = {}}));
            return std::nullopt;
        }
        const auto* messages = value.get_if<core::TdMessages>();
        if (messages == nullptr || messages->total_count < 0 ||
            static_cast<std::size_t>(messages->total_count) < messages->messages.size()) {
            claim_internal(*worker_);
            return std::nullopt;
        }
        return consume_page(*messages, from_message_id, previous_raw, consumed_raw);
    }

    [[nodiscard]] bool valid_options() {
        if (options_->chat_id == 0 || options_->after <= 0 || !options_->start_history) {
            claim_internal(*worker_);
            return false;
        }
        return true;
    }

    [[nodiscard]] StreamPollSchedule::Clock::time_point now() const {
        return options_->hooks && options_->hooks->now ? options_->hooks->now()
                                                       : StreamPollSchedule::Clock::now();
    }

    void sleep(StreamPollSchedule::Clock::time_point wake) const {
        if (options_->hooks && options_->hooks->sleep_until) {
            options_->hooks->sleep_until(wake);
        } else {
            std::this_thread::sleep_until(wake);
        }
    }

    [[nodiscard]] bool poll_stop() {
        if (worker_->terminal_snapshot()) {
            return true;
        }
        if (deadline_expired(session_->deadline(), now())) {
            static_cast<void>(
                worker_->claim({.cause = StreamTerminalCause::Deadline, .metadata_failure = {}}));
            return true;
        }
        if (session_->cancellation_requested()) {
            static_cast<void>(worker_->claim(
                {.cause = StreamTerminalCause::Disconnected, .metadata_failure = {}}));
            return true;
        }
        return false;
    }

    [[nodiscard]] bool wait_for_publication() {
        for (;;) {
            if (poll_stop()) {
                return false;
            }
            if (const auto projection = worker_->activation_projection()) {
                return projection->operation == StreamOperation::WaitFor &&
                       projection->mode == StreamMode::Match && projection->chat_count == 1 &&
                       projection->chat_ids[0] == options_->chat_id;
            }
            const auto wake = schedule_.next(session_->deadline().expires_at);
            sleep(wake);
            schedule_.advance(now());
        }
    }

    [[nodiscard]] bool drain_live() {
        for (;;) {
            std::optional<StreamCopiedItem> item;
            try {
                item = worker_->pop_front();
            } catch (const std::exception&) {
                claim_internal(*worker_);
                return false;
            }
            if (!item) {
                return true;
            }
            if (!state_->impl_->add_live(*item, *worker_)) {
                return false;
            }
        }
    }

    [[nodiscard]] bool wait_response(std::future<core::TdValue>& response) {
        for (;;) {
            if (!drain_live() || poll_stop()) {
                return false;
            }
            if (response.wait_for(0ms) == std::future_status::ready) {
                return true;
            }
            const auto wake = schedule_.next(session_->deadline().expires_at);
            sleep(wake);
            schedule_.advance(now());
        }
    }

    std::optional<StructuredPage> validate_page_structure(const core::TdMessages& page) {
        const auto null_count = static_cast<std::size_t>(std::ranges::count_if(
            page.messages, [](const auto& message) { return !message.has_value(); }));
        if (null_count != 0 && null_count != page.messages.size()) {
            claim_internal(*worker_);
            return std::nullopt;
        }
        if (null_count == page.messages.size() && null_count != 0) {
            return StructuredPage{.messages = {}, .boundary = true};
        }
        StructuredPage result;
        result.messages.reserve(page.messages.size());
        for (const auto& message : page.messages) {
            if (!message) {
                claim_internal(*worker_);
                return std::nullopt;
            }
            result.messages.push_back(&*message);
        }
        result.boundary = result.messages.empty();
        return result;
    }

    std::optional<PageResult> consume_page(const core::TdMessages& page,
                                           std::int64_t from_message_id,
                                           std::optional<std::int64_t>& previous_raw,
                                           std::set<std::int64_t>& consumed_raw) {
        auto structured = validate_page_structure(page);
        if (!structured) {
            return std::nullopt;
        }
        if (structured->boundary) {
            return PageResult{.boundary = true};
        }
        auto& messages = structured->messages;
        if (from_message_id != 0 && messages.front()->id == from_message_id) {
            messages.erase(messages.begin());
        }
        if (messages.empty()) {
            return PageResult{.boundary = true};
        }

        PageResult result;
        for (const auto* raw : messages) {
            if (raw->id <= 0 || raw->chat_id != options_->chat_id ||
                (previous_raw && raw->id >= *previous_raw) ||
                !consumed_raw.insert(raw->id).second) {
                claim_pagination(*worker_);
                return std::nullopt;
            }
            previous_raw = raw->id;
            result.last_new_id = raw->id;
            const MessageKey key{raw->chat_id, raw->id};
            if (!state_->impl_->add_history_key(key, *worker_)) {
                return std::nullopt;
            }
            if (raw->id <= options_->after) {
                result.past_after = true;
                break;
            }
            auto materialized = materialize_message_summary(*raw);
            if (!materialized ||
                !state_->impl_->consider_history(std::move(*materialized), *worker_)) {
                if (!worker_->terminal_snapshot()) {
                    claim_internal(*worker_);
                }
                return std::nullopt;
            }
        }
        return result;
    }

    RequestSession* session_ = nullptr;
    StreamSubscriptionWorker* worker_ = nullptr;
    const StreamWaitScannerOptions* options_ = nullptr;
    std::shared_ptr<StreamWaitMatchState> state_;
    StreamPollSchedule schedule_;
};

StreamWaitScanResult scan_wait_history(RequestSession& session, StreamSubscriptionWorker& worker,
                                       const StreamWaitScannerOptions& options) {
    return detail::StreamWaitScannerRun(session, worker, options).run();
}

} // namespace tgcli::daemon
