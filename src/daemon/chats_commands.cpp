#include "daemon/chats_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/chat_summary.hpp"
#include "daemon/ready_read.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

using core::AuthState;
using core::AuthStateSnapshot;
using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;
constexpr std::int32_t kDialogBatch = 100;
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view kOperation = "chats";
constexpr std::string_view kUnreadOperation = "unread";

struct ChatKey {
    std::int64_t order = 0;
    std::int64_t chat_id = 0;

    bool operator==(const ChatKey&) const = default;
};

struct ChatsState {
    core::TdChatList list;
    bool unread = false;
    std::int32_t limit = kDefaultChatsLimit;
    std::optional<ChatsCursor> cursor;
};

struct Membership {
    bool valid = true;
    bool selected = false;
    bool archived = false;
    std::vector<std::int32_t> folder_ids;
};

struct Position {
    bool valid = true;
    std::optional<std::int64_t> order;
};

struct OptionalInt32Argument {
    bool valid = true;
    std::optional<std::int32_t> value;
};

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    return std::ranges::all_of(expected,
                               [&](const std::string& name) { return value.contains(name); });
}

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_user_id(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
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

std::string_view list_name(core::TdChatListKind kind) {
    switch (kind) {
    case core::TdChatListKind::Main:
        return "main";
    case core::TdChatListKind::Archive:
        return "archive";
    case core::TdChatListKind::Folder:
        return "folder";
    case core::TdChatListKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::optional<core::TdChatListKind> parse_list_kind(std::string_view value) {
    if (value == "main") {
        return core::TdChatListKind::Main;
    }
    if (value == "archive") {
        return core::TdChatListKind::Archive;
    }
    if (value == "folder") {
        return core::TdChatListKind::Folder;
    }
    return std::nullopt;
}

bool valid_list(const core::TdChatList& list) {
    switch (list.kind) {
    case core::TdChatListKind::Main:
    case core::TdChatListKind::Archive:
        return list.folder_id == 0;
    case core::TdChatListKind::Folder:
        return list.folder_id > 0;
    case core::TdChatListKind::Unknown:
        return false;
    }
    return false;
}

bool same_list(const core::TdChatList& left, const core::TdChatList& right) {
    return left.kind == right.kind &&
           (left.kind != core::TdChatListKind::Folder || left.folder_id == right.folder_id);
}

bool below(const ChatKey& candidate, const ChatKey& anchor) {
    return std::tie(candidate.order, candidate.chat_id) < std::tie(anchor.order, anchor.chat_id);
}

std::string base64url_encode(std::string_view input) {
    std::string output;
    output.reserve((input.size() * 4 + 2) / 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char byte : input) {
        accumulator = (accumulator << 8U) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(kBase64Alphabet[(accumulator >> bits) & 0x3FU]);
        }
    }
    if (bits != 0) {
        output.push_back(kBase64Alphabet[(accumulator << (6 - bits)) & 0x3FU]);
    }
    return output;
}

std::optional<std::string> base64url_decode(std::string_view input) {
    if (input.empty() || input.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string output;
    output.reserve(input.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char character : input) {
        const auto found = kBase64Alphabet.find(character);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(found);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    if (bits != 0 && (accumulator & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return output;
}

std::int32_t retry_after(std::string_view message) {
    static const std::regex pattern(
        R"((?:^|[^[:alnum:]_])(?:retry[[:space:]]+after[[:space:]]*|FLOOD_WAIT_)([0-9]+))",
        std::regex::icase);
    std::cmatch match;
    if (!std::regex_search(message.begin(), message.end(), match, pattern) || match.size() != 2) {
        return 0;
    }
    std::int32_t result = 0;
    for (const char character : match[1].str()) {
        const auto digit = static_cast<std::int32_t>(character - '0');
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        result = result > (maximum - digit) / 10 ? maximum : result * 10 + digit;
    }
    return result;
}

Position selected_position(const core::TdChat& chat, const core::TdChatList& selected) {
    Position result;
    for (const auto& position : chat.positions) {
        if (!valid_list(position.list)) {
            result.valid = false;
            return result;
        }
        if (!same_list(position.list, selected) || position.order == 0) {
            continue;
        }
        if (result.order) {
            result.valid = false;
            return result;
        }
        result.order = position.order;
    }
    return result;
}

Membership membership(const core::TdChat& chat, const core::TdChatList& selected) {
    Membership result;
    for (const auto& list : chat.chat_lists) {
        if (!valid_list(list)) {
            result.valid = false;
            return result;
        }
        result.selected = result.selected || same_list(list, selected);
        result.archived = result.archived || list.kind == core::TdChatListKind::Archive;
        if (list.kind == core::TdChatListKind::Folder) {
            result.folder_ids.push_back(list.folder_id);
        }
    }
    std::ranges::sort(result.folder_ids);
    result.folder_ids.erase(std::unique(result.folder_ids.begin(), result.folder_ids.end()),
                            result.folder_ids.end());
    return result;
}

bool valid_unread_fields(const core::TdChat& chat) {
    return chat.unread_count >= 0 && chat.unread_mention_count >= 0 &&
           chat.unread_reaction_count >= 0 && chat.unread_poll_vote_count >= 0;
}

bool is_unread(const core::TdChat& chat) {
    return chat.is_marked_unread || chat.unread_count > 0 || chat.unread_mention_count > 0 ||
           chat.unread_reaction_count > 0 || chat.unread_poll_vote_count > 0;
}

std::optional<json> unread_summary_json(const core::TdChat& chat, const ChatIdentity& identity,
                                        const Membership& lists) {
    if (!valid_unread_fields(chat) || (identity.type != "private" && identity.is_bot)) {
        return std::nullopt;
    }
    return json{{"id", identity.id},
                {"title", identity.title},
                {"type", identity.type},
                {"is_bot", identity.is_bot},
                {"is_archived", lists.archived},
                {"is_marked_unread", chat.is_marked_unread},
                {"unread_count", chat.unread_count},
                {"unread_mention_count", chat.unread_mention_count},
                {"unread_reaction_count", chat.unread_reaction_count},
                {"unread_poll_vote_count", chat.unread_poll_vote_count}};
}

void usage(RequestSession& session, std::string_view message, const json& argument,
           std::string_view reason = "invalid_argument") {
    session.error("USAGE", std::string(message), {{"argument", argument}, {"reason", reason}},
                  kUsage);
}

bool valid_chats_args_shape(const json& args) {
    return exact_fields(args, {"archived", "cursor", "folder", "limit", "unread"}) &&
           args["archived"].is_boolean() && args["unread"].is_boolean() &&
           (args["folder"].is_null() || args["folder"].is_number_integer()) &&
           (args["limit"].is_null() || args["limit"].is_number_integer()) &&
           (args["cursor"].is_null() || args["cursor"].is_string());
}

OptionalInt32Argument folder_argument(const json& value, RequestSession& session) {
    if (value.is_null()) {
        return {};
    }
    const auto parsed = integer64(value);
    if (!parsed || *parsed <= 0 || *parsed > std::numeric_limits<std::int32_t>::max()) {
        usage(session, "chat folder id must be a positive int32", "--folder");
        return {.valid = false, .value = std::nullopt};
    }
    return {.valid = true, .value = static_cast<std::int32_t>(*parsed)};
}

OptionalInt32Argument limit_argument(const json& value, RequestSession& session) {
    if (value.is_null()) {
        return {};
    }
    const auto parsed = integer64(value);
    if (!parsed || *parsed < 1 || *parsed > kMaximumChatsLimit) {
        usage(session, "chats limit must be between 1 and 100", "-n");
        return {.valid = false, .value = std::nullopt};
    }
    return {.valid = true, .value = static_cast<std::int32_t>(*parsed)};
}

std::optional<ChatsState> cursor_state(const json& args, RequestSession& session,
                                       const std::optional<std::int32_t>& folder,
                                       const std::optional<std::int32_t>& limit, bool archived,
                                       bool unread) {
    if (folder) {
        usage(session, "--folder is not accepted with a continuation cursor", "--folder");
        return std::nullopt;
    }
    if (archived) {
        usage(session, "--archived is not accepted with a continuation cursor", "--archived");
        return std::nullopt;
    }
    if (unread) {
        usage(session, "--unread is not accepted with a continuation cursor", "--unread");
        return std::nullopt;
    }
    if (limit) {
        usage(session, "-n is not accepted with a continuation cursor", "-n");
        return std::nullopt;
    }
    const auto cursor = decode_chats_cursor(args["cursor"].get_ref<const std::string&>());
    if (!cursor) {
        usage(session, "invalid chats cursor", "--cursor", "invalid_cursor");
        return std::nullopt;
    }
    return ChatsState{.list = {.kind = cursor->list,
                               .folder_id = cursor->folder_id.value_or(0),
                               .tdlib_type_id = 0},
                      .unread = cursor->unread,
                      .limit = cursor->limit,
                      .cursor = cursor};
}

std::optional<ChatsState> first_page_state(RequestSession& session,
                                           const std::optional<std::int32_t>& folder,
                                           const std::optional<std::int32_t>& limit, bool archived,
                                           bool unread) {
    if (folder && archived) {
        usage(session, "--archived and --folder are mutually exclusive", "--archived/--folder",
              "mutually_exclusive");
        return std::nullopt;
    }
    core::TdChatList list{.kind = core::TdChatListKind::Main, .folder_id = 0, .tdlib_type_id = 0};
    if (archived) {
        list.kind = core::TdChatListKind::Archive;
    } else if (folder) {
        list.kind = core::TdChatListKind::Folder;
        list.folder_id = *folder;
    }
    return ChatsState{.list = list,
                      .unread = unread,
                      .limit = limit.value_or(kDefaultChatsLimit),
                      .cursor = std::nullopt};
}

std::optional<ChatsState> parse_state(const proto::Request& request, RequestSession& session) {
    const auto& args = request.args;
    if (!valid_chats_args_shape(args)) {
        usage(session, "chats received malformed arguments", nullptr);
        return std::nullopt;
    }
    const bool archived = args["archived"].get<bool>();
    const bool unread = args["unread"].get<bool>();
    const auto folder = folder_argument(args["folder"], session);
    if (!folder.valid) {
        return std::nullopt;
    }
    const auto limit = limit_argument(args["limit"], session);
    if (!limit.valid) {
        return std::nullopt;
    }
    if (!args["cursor"].is_null()) {
        return cursor_state(args, session, folder.value, limit.value, archived, unread);
    }
    return first_page_state(session, folder.value, limit.value, archived, unread);
}

class ChatsRun {
    using Prefix = std::vector<std::pair<ChatKey, core::TdChat>>;
    enum class LoadStatus { Loaded, Exhausted, Failed };

  public:
    ChatsRun(core::TdClient& client, std::string_view account, RequestSession& session,
             ChatsState state)
        : client_(client), account_(account), session_(session), state_(std::move(state)),
          reads_(client, session) {
        if (state_.cursor) {
            last_scanned_ =
                ChatKey{.order = state_.cursor->order, .chat_id = state_.cursor->chat_id};
        }
    }

    void run() {
        if (!preflight()) {
            return;
        }
        json items = json::array();
        std::int32_t prefix_limit = kDialogBatch;
        while (items.size() < static_cast<std::size_t>(state_.limit)) {
            const auto prefix = materialize_prefix(prefix_limit);
            if (!prefix || !append_matches(*prefix, items)) {
                return;
            }
            if (items.size() == static_cast<std::size_t>(state_.limit)) {
                break;
            }
            const auto load_status = load_more();
            if (load_status == LoadStatus::Failed) {
                return;
            }
            if (load_status == LoadStatus::Exhausted) {
                break;
            }
            if (prefix_limit <= std::numeric_limits<std::int32_t>::max() - kDialogBatch) {
                prefix_limit += kDialogBatch;
            } else {
                prefix_limit = std::numeric_limits<std::int32_t>::max();
            }
        }
        auto next = continuation(items.size());
        session_.result({{"items", std::move(items)}, {"next", std::move(next)}});
    }

  private:
    std::optional<Prefix> materialize_prefix(std::int32_t prefix_limit) {
        auto listed = read([&](const auto& current) {
            return client_.get_chats(current, state_.list, prefix_limit);
        });
        if (!listed) {
            return std::nullopt;
        }
        if (const auto* error = listed->value.get_if<core::TdError>()) {
            list_error(*error);
            return std::nullopt;
        }
        const auto* chats = listed->value.get_if<core::TdChats>();
        if (chats == nullptr) {
            internal_error();
            return std::nullopt;
        }
        Prefix prefix;
        prefix.reserve(chats->chat_ids.size());
        for (const auto chat_id : chats->chat_ids) {
            if (!append_materialized_chat(prefix, chat_id)) {
                return std::nullopt;
            }
        }
        std::ranges::sort(prefix, [](const auto& left, const auto& right) {
            return std::tie(left.first.order, left.first.chat_id) >
                   std::tie(right.first.order, right.first.chat_id);
        });
        return prefix;
    }

    bool append_materialized_chat(Prefix& prefix, std::int64_t chat_id) {
        if (!valid_int53(chat_id)) {
            internal_error();
            return false;
        }
        auto fetched =
            read([&](const auto& current) { return client_.get_chat(current, chat_id); });
        if (!fetched) {
            return false;
        }
        if (const auto* error = fetched->value.get_if<core::TdError>()) {
            td_error(*error);
            return false;
        }
        const auto* chat = fetched->value.get_if<core::TdChat>();
        if (chat == nullptr || chat->id != chat_id || !valid_int53(chat->id)) {
            internal_error();
            return false;
        }
        const auto position = selected_position(*chat, state_.list);
        if (!position.valid) {
            internal_error();
            return false;
        }
        if (position.order) {
            prefix.emplace_back(ChatKey{.order = *position.order, .chat_id = chat->id}, *chat);
        }
        return true;
    }

    bool append_matches(const Prefix& prefix, json& items) {
        for (const auto& [key, chat] : prefix) {
            if (last_scanned_ && !below(key, *last_scanned_)) {
                continue;
            }
            last_scanned_ = key;
            const auto lists = membership(chat, state_.list);
            if (!lists.valid || !valid_unread_fields(chat)) {
                internal_error();
                return false;
            }
            if (!lists.selected || chat.kind == core::TdChatKind::Secret ||
                (state_.unread && !is_unread(chat))) {
                continue;
            }
            const auto identity = materialize_chat_identity(
                client_, chat, [&](const auto& start) { return read(start); });
            if (identity.status == ChatIdentityStatus::ReadStopped) {
                return false;
            }
            if (identity.status == ChatIdentityStatus::TdError && identity.error) {
                td_error(*identity.error);
                return false;
            }
            if (identity.status != ChatIdentityStatus::Success || !identity.identity) {
                internal_error();
                return false;
            }
            const auto summary = materialize_chat_summary(chat, *identity.identity);
            if (!summary) {
                internal_error();
                return false;
            }
            items.push_back(chat_summary_json(*summary));
            if (items.size() == static_cast<std::size_t>(state_.limit)) {
                break;
            }
        }
        return true;
    }

    LoadStatus load_more() {
        auto loaded = read([&](const auto& current) {
            return client_.load_chats(current, state_.list, kDialogBatch);
        });
        if (!loaded) {
            return LoadStatus::Failed;
        }
        if (const auto* error = loaded->value.get_if<core::TdError>()) {
            if (error->code == 404) {
                return LoadStatus::Exhausted;
            }
            list_error(*error);
            return LoadStatus::Failed;
        }
        if (loaded->value.get_if<core::TdOk>() == nullptr) {
            internal_error();
            return LoadStatus::Failed;
        }
        return LoadStatus::Loaded;
    }

    [[nodiscard]] json continuation(std::size_t item_count) const {
        if (item_count != static_cast<std::size_t>(state_.limit) || !last_scanned_) {
            return nullptr;
        }
        return encode_chats_cursor(
            {.version = 1,
             .operation = std::string(kOperation),
             .account = std::string(account_),
             .user_id = user_id_,
             .limit = state_.limit,
             .list = state_.list.kind,
             .folder_id = state_.list.kind == core::TdChatListKind::Folder
                              ? std::optional<std::int32_t>{state_.list.folder_id}
                              : std::nullopt,
             .unread = state_.unread,
             .order = last_scanned_->order,
             .chat_id = last_scanned_->chat_id});
    }

    bool preflight() {
        snapshot_ = reads_.current();
        if (!snapshot_ || snapshot_->data.state != AuthState::Ready) {
            not_authed(snapshot_, "not_ready");
            return false;
        }
        auto response = read([&](const auto& current) { return client_.get_me(current); });
        if (!response) {
            return false;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return false;
        }
        const auto* user = response->value.get_if<core::TdUserSummary>();
        if (user == nullptr || !valid_user_id(user->id)) {
            internal_error();
            return false;
        }
        user_id_ = user->id;
        if (user->is_bot) {
            session_.error("BOT_UNSUPPORTED", "chats requires a user account",
                           {{"operation", kOperation}}, kUsage);
            return false;
        }
        if (state_.cursor &&
            (state_.cursor->operation != kOperation || state_.cursor->account != account_ ||
             state_.cursor->user_id != user_id_)) {
            usage(session_, "chats cursor scope does not match this request", "--cursor",
                  "cursor_scope_mismatch");
            return false;
        }
        return true;
    }

    std::optional<ReadyReadResult> read(const ReadyReadStart& start) {
        auto waited = reads_.read(start, snapshot_);
        if (waited.status == ReadyReadStatus::AuthorizationLost) {
            if (!session_.cancellation_requested()) {
                not_authed(waited.snapshot, "authorization_lost");
            }
            return std::nullopt;
        }
        if (waited.status == ReadyReadStatus::TimedOut) {
            if (!session_.cancellation_requested()) {
                const auto current = reads_.current();
                session_.error("TIMEOUT", "chats request timed out",
                               {{"operation", kOperation},
                                {"state", current ? json(core::auth_state_name(current->data.state))
                                                  : json(nullptr)}},
                               kTimeout);
            }
            return std::nullopt;
        }
        if (waited.status != ReadyReadStatus::Response || session_.cancellation_requested()) {
            if (waited.status == ReadyReadStatus::Failed && !session_.cancellation_requested()) {
                internal_error();
            }
            return std::nullopt;
        }
        return waited;
    }

    void not_authed(const std::shared_ptr<const AuthStateSnapshot>& snapshot,
                    std::string_view reason) {
        session_.error("NOT_AUTHED", "chats requires an authenticated account",
                       {{"account", account_},
                        {"state", snapshot ? json(core::auth_state_name(snapshot->data.state))
                                           : json("unknown")},
                        {"reason", reason}},
                       kNotAuthed);
    }

    void td_error(const core::TdError& error) {
        if (error.code == 429) {
            session_.error("RATE_LIMITED", "Telegram rate limit",
                           {{"operation", kOperation},
                            {"tdlib_code", 429},
                            {"retry_after", retry_after(error.message)}},
                           kRateLimited);
            return;
        }
        session_.error("TDLIB_ERROR", "chats TDLib request failed",
                       {{"operation", kOperation}, {"tdlib_code", error.code}}, kGeneric);
    }

    void list_error(const core::TdError& error) {
        if (state_.list.kind == core::TdChatListKind::Folder && error.code == 400) {
            session_.error("NOT_FOUND", "chat folder was not found",
                           {{"folder_id", state_.list.folder_id}}, kNotFound);
            return;
        }
        td_error(error);
    }

    void internal_error() {
        session_.error("INTERNAL", "chats returned an unexpected object",
                       {{"operation", kOperation}, {"reason", "internal_error"}}, kGeneric);
    }

    core::TdClient& client_;
    std::string_view account_;
    RequestSession& session_;
    ChatsState state_;
    ReadyReadSession reads_;
    std::shared_ptr<const AuthStateSnapshot> snapshot_;
    std::optional<ChatKey> last_scanned_;
    std::int64_t user_id_ = 0;
};

class UnreadRun {
  public:
    UnreadRun(core::TdClient& client, std::string_view account, RequestSession& session)
        : client_(client), account_(account), session_(session), reads_(client, session) {}

    void run() {
        if (!preflight()) {
            return;
        }
        const auto main = load_complete_list(core::TdChatListKind::Main);
        if (!main) {
            return;
        }
        const auto archive = load_complete_list(core::TdChatListKind::Archive);
        if (!archive) {
            return;
        }

        json items = json::array();
        std::unordered_set<std::int64_t> seen;
        if (!append_list(*main, core::TdChatListKind::Main, seen, items) ||
            !append_list(*archive, core::TdChatListKind::Archive, seen, items)) {
            return;
        }
        session_.result({{"items", std::move(items)}, {"next", nullptr}});
    }

  private:
    std::optional<std::vector<std::int64_t>> load_complete_list(core::TdChatListKind kind) {
        const core::TdChatList list{.kind = kind, .folder_id = 0, .tdlib_type_id = 0};
        std::int32_t prefix_limit = kDialogBatch;
        for (;;) {
            auto listed = read([&](const auto& current) {
                return client_.get_chats(current, list, prefix_limit);
            });
            if (!listed) {
                return std::nullopt;
            }
            if (const auto* error = listed->value.get_if<core::TdError>()) {
                td_error(*error);
                return std::nullopt;
            }
            const auto* chats = listed->value.get_if<core::TdChats>();
            if (chats == nullptr || !std::ranges::all_of(chats->chat_ids, [](std::int64_t id) {
                    return valid_int53(id);
                })) {
                internal_error();
                return std::nullopt;
            }
            auto ids = chats->chat_ids;

            auto loaded = read([&](const auto& current) {
                return client_.load_chats(current, list, kDialogBatch);
            });
            if (!loaded) {
                return std::nullopt;
            }
            if (const auto* error = loaded->value.get_if<core::TdError>()) {
                if (error->code == 404) {
                    return ids;
                }
                td_error(*error);
                return std::nullopt;
            }
            if (loaded->value.get_if<core::TdOk>() == nullptr) {
                internal_error();
                return std::nullopt;
            }
            if (prefix_limit <= std::numeric_limits<std::int32_t>::max() - kDialogBatch) {
                prefix_limit += kDialogBatch;
            } else {
                prefix_limit = std::numeric_limits<std::int32_t>::max();
            }
        }
    }

    bool append_list(const std::vector<std::int64_t>& ids, core::TdChatListKind kind,
                     std::unordered_set<std::int64_t>& seen, json& items) {
        const core::TdChatList selected{.kind = kind, .folder_id = 0, .tdlib_type_id = 0};
        for (const auto chat_id : ids) {
            if (seen.contains(chat_id)) {
                continue;
            }
            if (!append_chat(chat_id, selected, seen, items)) {
                return false;
            }
        }
        return true;
    }

    bool append_chat(std::int64_t chat_id, const core::TdChatList& selected,
                     std::unordered_set<std::int64_t>& seen, json& items) {
        auto fetched =
            read([&](const auto& current) { return client_.get_chat(current, chat_id); });
        if (!fetched) {
            return false;
        }
        if (const auto* error = fetched->value.get_if<core::TdError>()) {
            td_error(*error);
            return false;
        }
        const auto* chat = fetched->value.get_if<core::TdChat>();
        if (chat == nullptr || chat->id != chat_id || !valid_int53(chat->id)) {
            internal_error();
            return false;
        }
        const auto lists = membership(*chat, selected);
        if (!lists.valid) {
            internal_error();
            return false;
        }
        if (!lists.selected) {
            return true;
        }
        seen.insert(chat_id);
        if (chat->kind == core::TdChatKind::Secret) {
            return true;
        }
        if (!valid_unread_fields(*chat)) {
            internal_error();
            return false;
        }
        if (!is_unread(*chat)) {
            return true;
        }
        return append_summary(*chat, lists, items);
    }

    bool append_summary(const core::TdChat& chat, const Membership& lists, json& items) {
        const auto identity = materialize_chat_identity(
            client_, chat, [&](const auto& start) { return read(start); });
        if (identity.status == ChatIdentityStatus::ReadStopped) {
            return false;
        }
        if (identity.status == ChatIdentityStatus::TdError && identity.error) {
            td_error(*identity.error);
            return false;
        }
        if (identity.status != ChatIdentityStatus::Success || !identity.identity) {
            internal_error();
            return false;
        }
        const auto summary = unread_summary_json(chat, *identity.identity, lists);
        if (!summary) {
            internal_error();
            return false;
        }
        items.push_back(*summary);
        return true;
    }

    bool preflight() {
        snapshot_ = reads_.current();
        if (!snapshot_ || snapshot_->data.state != AuthState::Ready) {
            not_authed(snapshot_, "not_ready");
            return false;
        }
        auto response = read([&](const auto& current) { return client_.get_me(current); });
        if (!response) {
            return false;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return false;
        }
        const auto* user = response->value.get_if<core::TdUserSummary>();
        if (user == nullptr || !valid_user_id(user->id)) {
            internal_error();
            return false;
        }
        if (user->is_bot) {
            session_.error("BOT_UNSUPPORTED", "unread requires a user account",
                           {{"operation", kUnreadOperation}}, kUsage);
            return false;
        }
        return true;
    }

    std::optional<ReadyReadResult> read(const ReadyReadStart& start) {
        auto waited = reads_.read(start, snapshot_);
        if (waited.status == ReadyReadStatus::AuthorizationLost) {
            if (!session_.cancellation_requested()) {
                not_authed(waited.snapshot, "authorization_lost");
            }
            return std::nullopt;
        }
        if (waited.status == ReadyReadStatus::TimedOut) {
            if (!session_.cancellation_requested()) {
                const auto current = reads_.current();
                session_.error("TIMEOUT", "unread request timed out",
                               {{"operation", kUnreadOperation},
                                {"state", current ? json(core::auth_state_name(current->data.state))
                                                  : json(nullptr)}},
                               kTimeout);
            }
            return std::nullopt;
        }
        if (waited.status != ReadyReadStatus::Response || session_.cancellation_requested()) {
            if (waited.status == ReadyReadStatus::Failed && !session_.cancellation_requested()) {
                internal_error();
            }
            return std::nullopt;
        }
        return waited;
    }

    void not_authed(const std::shared_ptr<const AuthStateSnapshot>& snapshot,
                    std::string_view reason) {
        session_.error("NOT_AUTHED", "unread requires an authenticated account",
                       {{"account", account_},
                        {"state", snapshot ? json(core::auth_state_name(snapshot->data.state))
                                           : json("unknown")},
                        {"reason", reason}},
                       kNotAuthed);
    }

    void td_error(const core::TdError& error) {
        if (error.code == 429) {
            session_.error("RATE_LIMITED", "Telegram rate limit",
                           {{"operation", kUnreadOperation},
                            {"tdlib_code", 429},
                            {"retry_after", retry_after(error.message)}},
                           kRateLimited);
            return;
        }
        session_.error("TDLIB_ERROR", "unread TDLib request failed",
                       {{"operation", kUnreadOperation}, {"tdlib_code", error.code}}, kGeneric);
    }

    void internal_error() {
        session_.error("INTERNAL", "unread returned an unexpected object",
                       {{"operation", kUnreadOperation}, {"reason", "internal_error"}}, kGeneric);
    }

    core::TdClient& client_;
    std::string_view account_;
    RequestSession& session_;
    ReadyReadSession reads_;
    std::shared_ptr<const AuthStateSnapshot> snapshot_;
};

} // namespace

std::string encode_chats_cursor(const ChatsCursor& cursor) {
    const json value{{"account", cursor.account},
                     {"chat_id", cursor.chat_id},
                     {"folder_id", cursor.folder_id ? json(*cursor.folder_id) : json(nullptr)},
                     {"limit", cursor.limit},
                     {"list", list_name(cursor.list)},
                     {"operation", cursor.operation},
                     {"order", cursor.order},
                     {"unread", cursor.unread},
                     {"user_id", cursor.user_id},
                     {"version", cursor.version}};
    return base64url_encode(value.dump());
}

std::optional<ChatsCursor> decode_chats_cursor(std::string_view token) {
    const auto decoded = base64url_decode(token);
    if (!decoded || !common::valid_utf8(*decoded)) {
        return std::nullopt;
    }
    const auto value = json::parse(*decoded, nullptr, false);
    if (!exact_fields(value, {"account", "chat_id", "folder_id", "limit", "list", "operation",
                              "order", "unread", "user_id", "version"}) ||
        !value["account"].is_string() || !value["chat_id"].is_number_integer() ||
        (!value["folder_id"].is_null() && !value["folder_id"].is_number_integer()) ||
        !value["limit"].is_number_integer() || !value["list"].is_string() ||
        !value["operation"].is_string() || !value["order"].is_number_integer() ||
        !value["unread"].is_boolean() || !value["user_id"].is_number_integer() ||
        !value["version"].is_number_integer()) {
        return std::nullopt;
    }
    ChatsCursor cursor;
    try {
        const auto version = integer64(value["version"]);
        const auto user_id = integer64(value["user_id"]);
        const auto limit = integer64(value["limit"]);
        const auto order = integer64(value["order"]);
        const auto chat_id = integer64(value["chat_id"]);
        if (!version || !user_id || !limit || !order || !chat_id ||
            *version < std::numeric_limits<std::int32_t>::min() ||
            *version > std::numeric_limits<std::int32_t>::max() ||
            *limit < std::numeric_limits<std::int32_t>::min() ||
            *limit > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        cursor.version = static_cast<std::int32_t>(*version);
        cursor.operation = value["operation"].get<std::string>();
        cursor.account = value["account"].get<std::string>();
        cursor.user_id = *user_id;
        cursor.limit = static_cast<std::int32_t>(*limit);
        const auto kind = parse_list_kind(value["list"].get_ref<const std::string&>());
        if (!kind) {
            return std::nullopt;
        }
        cursor.list = *kind;
        if (!value["folder_id"].is_null()) {
            const auto folder_id = integer64(value["folder_id"]);
            if (!folder_id || *folder_id < std::numeric_limits<std::int32_t>::min() ||
                *folder_id > std::numeric_limits<std::int32_t>::max()) {
                return std::nullopt;
            }
            cursor.folder_id = static_cast<std::int32_t>(*folder_id);
        }
        cursor.unread = value["unread"].get<bool>();
        cursor.order = *order;
        cursor.chat_id = *chat_id;
    } catch (const json::exception&) {
        return std::nullopt;
    }
    const bool folder_scope = cursor.list == core::TdChatListKind::Folder;
    if (cursor.version != 1 || cursor.operation.empty() || cursor.account.empty() ||
        !valid_user_id(cursor.user_id) || cursor.limit < 1 || cursor.limit > kMaximumChatsLimit ||
        cursor.order == 0 || !valid_int53(cursor.chat_id) ||
        (folder_scope != cursor.folder_id.has_value()) ||
        (cursor.folder_id && *cursor.folder_id <= 0) || encode_chats_cursor(cursor) != token) {
        return std::nullopt;
    }
    return cursor;
}

void ChatsCoordinator::chats(const proto::Request& request, RequestSession& session) {
    const auto state = parse_state(request, session);
    if (!state) {
        return;
    }
    ChatsRun(client_.get(), account_, session, *state).run();
}

void ChatsCoordinator::unread(const proto::Request& request, RequestSession& session) {
    if (!request.args.is_object() || !request.args.empty()) {
        usage(session, "unread received malformed arguments", nullptr);
        return;
    }
    UnreadRun(client_.get(), account_, session).run();
}

void register_chats_command(Dispatcher& dispatcher, ChatsCoordinator& coordinator) {
    dispatcher.register_command("chats", {Tier::Read, [&coordinator](const proto::Request& request,
                                                                     RequestSession& session) {
                                              coordinator.chats(request, session);
                                          }});
}

void register_unread_command(Dispatcher& dispatcher, ChatsCoordinator& coordinator) {
    dispatcher.register_command("unread", {Tier::Read, [&coordinator](const proto::Request& request,
                                                                      RequestSession& session) {
                                               coordinator.unread(request, session);
                                           }});
}

} // namespace tgcli::daemon
