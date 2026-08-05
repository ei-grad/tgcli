#include "daemon/resolver.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/ready_read.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace tgcli::daemon {

namespace {

using core::AuthState;
using core::AuthStateSnapshot;
using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;
constexpr std::int32_t kDialogLoadBatch = 100;
constexpr std::size_t kMaximumAmbiguousCandidates = 20;
constexpr std::string_view kOperation = "resolve";

enum class SelectorKind { Numeric, Username, Link, Title };

struct ClassifiedSelector {
    SelectorKind kind = SelectorKind::Title;
    std::int64_t chat_id = 0;
    std::string value;
};

struct ResolveResult {
    explicit ResolveResult(ChatIdentity chat_value,
                           std::optional<std::int64_t> message_id_value = std::nullopt,
                           std::optional<core::TdTopic> topic_value = std::nullopt,
                           std::optional<std::string> link_type_value = std::nullopt,
                           std::optional<bool> is_public_value = std::nullopt)
        : chat(std::move(chat_value)), message_id(message_id_value), topic(topic_value),
          link_type(std::move(link_type_value)), is_public(is_public_value) {}

    ChatIdentity chat;
    std::optional<std::int64_t> message_id;
    std::optional<core::TdTopic> topic;
    std::optional<std::string> link_type;
    std::optional<bool> is_public;
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

bool decimal_syntax(std::string_view selector) {
    if (selector.empty()) {
        return false;
    }
    std::size_t offset = 0;
    if (selector.front() == '-' || selector.front() == '+') {
        offset = 1;
    }
    return offset < selector.size() &&
           std::ranges::all_of(selector.substr(offset),
                               [](char character) { return character >= '0' && character <= '9'; });
}

std::optional<std::int64_t> parse_decimal(std::string_view selector) {
    const bool positive_sign = selector.front() == '+';
    if (positive_sign) {
        selector.remove_prefix(1);
    }
    std::int64_t value = 0;
    const auto [end, error] =
        std::from_chars(selector.data(), selector.data() + selector.size(), value);
    if (error != std::errc{} || end != selector.data() + selector.size() || !valid_int53(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<ClassifiedSelector> classify_selector(std::string_view selector) {
    if (decimal_syntax(selector)) {
        const auto id = parse_decimal(selector);
        if (!id) {
            return std::nullopt;
        }
        return ClassifiedSelector{
            .kind = SelectorKind::Numeric, .chat_id = *id, .value = std::string(selector)};
    }
    if (selector.starts_with('@')) {
        if (selector.size() == 1) {
            return std::nullopt;
        }
        return ClassifiedSelector{
            .kind = SelectorKind::Username, .chat_id = 0, .value = std::string(selector.substr(1))};
    }
    if (selector.starts_with("https://t.me/") || selector.starts_with("t.me/")) {
        return ClassifiedSelector{
            .kind = SelectorKind::Link, .chat_id = 0, .value = std::string(selector)};
    }
    return ClassifiedSelector{
        .kind = SelectorKind::Title, .chat_id = 0, .value = std::string(selector)};
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

std::string chat_type_name(core::TdChatKind kind) {
    switch (kind) {
    case core::TdChatKind::Private:
        return "private";
    case core::TdChatKind::BasicGroup:
        return "basic_group";
    case core::TdChatKind::Supergroup:
        return "supergroup";
    case core::TdChatKind::Channel:
        return "channel";
    case core::TdChatKind::Secret:
        return "secret";
    case core::TdChatKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::optional<std::string> topic_kind_name(core::TdTopicKind kind) {
    switch (kind) {
    case core::TdTopicKind::Forum:
        return "forum";
    case core::TdTopicKind::Thread:
        return "thread";
    case core::TdTopicKind::Direct:
        return "direct";
    case core::TdTopicKind::Saved:
        return "saved";
    case core::TdTopicKind::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

json identity_json(const ChatIdentity& identity) {
    return {{"id", identity.id},
            {"title", identity.title},
            {"type", identity.type},
            {"is_bot", identity.is_bot},
            {"usernames", identity.usernames}};
}

std::optional<json> topic_json(const core::TdTopic& topic) {
    const auto kind = topic_kind_name(topic.kind);
    if (!kind || topic.id <= 0 || topic.id > kMaximumInt53 ||
        (topic.kind == core::TdTopicKind::Forum &&
         topic.id > std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    return json{{"kind", *kind}, {"id", topic.id}};
}

std::optional<json> result_json(const ResolveResult& result) {
    json topic = nullptr;
    if (result.topic) {
        const auto converted = topic_json(*result.topic);
        if (!converted) {
            return std::nullopt;
        }
        topic = *converted;
    }
    std::string kind = "chat";
    if (result.message_id) {
        kind = "message";
    } else if (result.topic) {
        kind = "topic";
    }
    return json{{"kind", kind},
                {"chat", identity_json(result.chat)},
                {"message_id", result.message_id ? json(*result.message_id) : json(nullptr)},
                {"topic", std::move(topic)},
                {"link_type", result.link_type ? json(*result.link_type) : json(nullptr)},
                {"is_public", result.is_public ? json(*result.is_public) : json(nullptr)}};
}

class ResolverRun {
  public:
    ResolverRun(core::TdClient& client, std::string_view account, RequestSession& session,
                std::string selector)
        : client_(client), account_(account), session_(session), selector_(std::move(selector)),
          reads_(client, session) {}

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
        me_ = *user;
        return true;
    }

    std::optional<ResolveResult> resolve(const ClassifiedSelector& selector, ResolverScope scope) {
        switch (selector.kind) {
        case SelectorKind::Numeric:
            return resolve_numeric(selector.chat_id);
        case SelectorKind::Username:
            if (scope == ResolverScope::LocalMaterialized) {
                return resolve_local_username(selector.value);
            }
            return resolve_public_username(selector.value, std::nullopt, false);
        case SelectorKind::Link:
            return resolve_link(selector.value, scope);
        case SelectorKind::Title:
            if (me_.is_bot) {
                bot_unsupported();
                return std::nullopt;
            }
            return resolve_title(selector.value, scope);
        }
        internal_error();
        return std::nullopt;
    }

  private:
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
                session_.error("TIMEOUT", "resolver request timed out",
                               {{"operation", kOperation},
                                {"state", reads_.current() ? json(core::auth_state_name(
                                                                 reads_.current()->data.state))
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
        session_.error("NOT_AUTHED", "resolve requires an authenticated account",
                       {{"account", account_},
                        {"state", snapshot ? json(core::auth_state_name(snapshot->data.state))
                                           : json("unknown")},
                        {"reason", reason}},
                       kNotAuthed);
    }

    void bot_unsupported() {
        session_.error("BOT_UNSUPPORTED", "this resolver branch requires a user account",
                       {{"operation", kOperation}}, kUsage);
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
        session_.error("TDLIB_ERROR", "resolver TDLib request failed",
                       {{"operation", kOperation}, {"tdlib_code", error.code}}, kGeneric);
    }

    void internal_error() {
        session_.error("INTERNAL", "resolver returned an unexpected object",
                       {{"operation", kOperation}, {"reason", "internal_error"}}, kGeneric);
    }

    void not_found(ResolverScope scope = ResolverScope::ActiveDialogs) {
        json details{{"selector", selector_}};
        if (scope == ResolverScope::LocalMaterialized) {
            details["scope"] = "local_materialized";
        }
        session_.error("NOT_FOUND", "no chat or link target matches the selector",
                       std::move(details), kNotFound);
    }

    void usage(std::string_view message, std::string_view reason) {
        session_.error("USAGE", std::string(message),
                       {{"argument", "selector"}, {"reason", reason}}, kUsage);
    }

    std::optional<core::TdChat> get_chat(std::int64_t chat_id, bool domain_scan = false) {
        auto response =
            read([&](const auto& current) { return client_.get_chat(current, chat_id); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            if (!domain_scan && (error->code == 400 || error->code == 404)) {
                not_found();
            } else {
                td_error(*error);
            }
            return std::nullopt;
        }
        const auto* chat = response->value.get_if<core::TdChat>();
        if (chat == nullptr || chat->id != chat_id || !valid_int53(chat->id)) {
            internal_error();
            return std::nullopt;
        }
        return *chat;
    }

    static bool valid_usernames(const std::vector<std::string>& usernames) {
        return std::ranges::all_of(usernames, [](const std::string& username) {
            return !username.empty() && common::valid_utf8(username);
        });
    }

    std::optional<ChatIdentity> private_identity(const core::TdChat& chat, ChatIdentity result) {
        if (!valid_user_id(chat.related_id)) {
            internal_error();
            return std::nullopt;
        }
        auto response =
            read([&](const auto& current) { return client_.get_user(current, chat.related_id); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* user = response->value.get_if<core::TdUserSummary>();
        if (user == nullptr || user->id != chat.related_id || !valid_usernames(user->usernames)) {
            internal_error();
            return std::nullopt;
        }
        result.is_bot = user->is_bot;
        result.usernames = user->usernames;
        return result;
    }

    std::optional<ChatIdentity> supergroup_identity(const core::TdChat& chat, ChatIdentity result) {
        if (!valid_user_id(chat.related_id)) {
            internal_error();
            return std::nullopt;
        }
        auto response = read(
            [&](const auto& current) { return client_.get_supergroup(current, chat.related_id); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* supergroup = response->value.get_if<core::TdSupergroup>();
        const bool expected_channel = chat.kind == core::TdChatKind::Channel;
        if (supergroup == nullptr || supergroup->id != chat.related_id ||
            supergroup->is_channel != expected_channel || !valid_usernames(supergroup->usernames)) {
            internal_error();
            return std::nullopt;
        }
        result.usernames = supergroup->usernames;
        return result;
    }

    std::optional<ChatIdentity> identity(const core::TdChat& chat, bool reject_secret = true) {
        if (!common::valid_utf8(chat.title)) {
            internal_error();
            return std::nullopt;
        }
        if (chat.kind == core::TdChatKind::Secret) {
            if (reject_secret) {
                usage("secret chats are not supported", "unsupported_chat_type");
            }
            return std::nullopt;
        }
        if (chat.kind == core::TdChatKind::Unknown) {
            internal_error();
            return std::nullopt;
        }
        ChatIdentity result{.id = chat.id,
                            .title = chat.title,
                            .type = chat_type_name(chat.kind),
                            .is_bot = false,
                            .usernames = {}};
        if (chat.kind == core::TdChatKind::Private) {
            return private_identity(chat, std::move(result));
        }
        if (chat.kind == core::TdChatKind::Supergroup || chat.kind == core::TdChatKind::Channel) {
            return supergroup_identity(chat, std::move(result));
        }
        return result;
    }

    std::optional<ResolveResult> resolve_numeric(std::int64_t chat_id) {
        const auto chat = get_chat(chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized));
    }

    std::optional<ResolveResult> resolve_public_username(const std::string& username,
                                                         std::optional<std::string> link_type,
                                                         bool require_bot) {
        auto response = read(
            [&](const auto& current) { return client_.search_public_chat(current, username); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            if (error->code == 400 && (error->message == "USERNAME_NOT_OCCUPIED" ||
                                       error->message == "USERNAME_INVALID")) {
                not_found();
            } else {
                td_error(*error);
            }
            return std::nullopt;
        }
        const auto* chat = response->value.get_if<core::TdChat>();
        if (chat == nullptr || !valid_int53(chat->id)) {
            internal_error();
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        if (require_bot && !materialized->is_bot) {
            internal_error();
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt,
                             std::move(link_type));
    }

    std::optional<std::vector<std::int64_t>> load_list(core::TdChatListKind list,
                                                       ResolverScope scope) {
        std::int32_t prefix_limit = kDialogLoadBatch;
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
            if (scope == ResolverScope::LocalMaterialized) {
                return chats->chat_ids;
            }
            auto loaded = read([&](const auto& current) {
                return client_.load_chats(current, list, kDialogLoadBatch);
            });
            if (!loaded) {
                return std::nullopt;
            }
            if (const auto* error = loaded->value.get_if<core::TdError>()) {
                if (error->code == 404) {
                    return chats->chat_ids;
                }
                td_error(*error);
                return std::nullopt;
            }
            if (loaded->value.get_if<core::TdOk>() == nullptr) {
                internal_error();
                return std::nullopt;
            }
            if (prefix_limit > std::numeric_limits<std::int32_t>::max() - kDialogLoadBatch) {
                internal_error();
                return std::nullopt;
            }
            prefix_limit += kDialogLoadBatch;
        }
    }

    std::optional<std::vector<std::int64_t>> active_dialog_ids(ResolverScope scope) {
        auto main = load_list(core::TdChatListKind::Main, scope);
        if (!main) {
            return std::nullopt;
        }
        auto archive = load_list(core::TdChatListKind::Archive, scope);
        if (!archive) {
            return std::nullopt;
        }
        std::vector<std::int64_t> result;
        result.reserve(main->size() + archive->size());
        std::unordered_set<std::int64_t> seen;
        for (const auto id : *main) {
            if (seen.insert(id).second) {
                result.push_back(id);
            }
        }
        for (const auto id : *archive) {
            if (seen.insert(id).second) {
                result.push_back(id);
            }
        }
        return result;
    }

    template <typename Predicate>
    std::optional<ResolveResult> resolve_materialized(Predicate predicate, ResolverScope scope) {
        const auto ids = active_dialog_ids(scope);
        if (!ids) {
            return std::nullopt;
        }
        std::vector<ChatIdentity> candidates;
        std::size_t matches = 0;
        std::optional<ChatIdentity> single;
        for (const auto id : *ids) {
            const auto chat = get_chat(id, true);
            if (!chat) {
                return std::nullopt;
            }
            if (chat->kind == core::TdChatKind::Secret || !predicate(*chat)) {
                continue;
            }
            ++matches;
            if (matches <= kMaximumAmbiguousCandidates) {
                auto materialized = identity(*chat, false);
                if (!materialized) {
                    return std::nullopt;
                }
                if (matches == 1) {
                    single = *materialized;
                }
                candidates.push_back(std::move(*materialized));
            }
        }
        if (matches == 0) {
            not_found(scope);
            return std::nullopt;
        }
        if (matches == 1 && single) {
            return ResolveResult(std::move(*single));
        }
        json values = json::array();
        for (const auto& candidate : candidates) {
            values.push_back(identity_json(candidate));
        }
        session_.error("AMBIGUOUS", "multiple chats match the selector",
                       {{"selector", selector_},
                        {"scope", scope == ResolverScope::ActiveDialogs ? "active_dialogs"
                                                                        : "local_materialized"},
                        {"candidates", std::move(values)},
                        {"truncated", matches > kMaximumAmbiguousCandidates}},
                       kUsage);
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_title(const std::string& title, ResolverScope scope) {
        return resolve_materialized(
            [&](const core::TdChat& chat) { return chat.title.find(title) != std::string::npos; },
            scope);
    }

    std::optional<ResolveResult> resolve_local_username(const std::string& username) {
        const auto ids = active_dialog_ids(ResolverScope::LocalMaterialized);
        if (!ids) {
            return std::nullopt;
        }
        std::vector<ChatIdentity> matches;
        for (const auto id : *ids) {
            const auto chat = get_chat(id, true);
            if (!chat) {
                return std::nullopt;
            }
            if (chat->kind == core::TdChatKind::Secret) {
                continue;
            }
            auto materialized = identity(*chat, false);
            if (!materialized) {
                return std::nullopt;
            }
            if (std::ranges::find(materialized->usernames, username) !=
                materialized->usernames.end()) {
                matches.push_back(std::move(*materialized));
            }
        }
        if (matches.empty()) {
            not_found(ResolverScope::LocalMaterialized);
            return std::nullopt;
        }
        if (matches.size() == 1) {
            return ResolveResult(std::move(matches.front()));
        }
        json candidates = json::array();
        for (std::size_t index = 0; index < std::min(matches.size(), kMaximumAmbiguousCandidates);
             ++index) {
            candidates.push_back(identity_json(matches[index]));
        }
        session_.error("AMBIGUOUS", "multiple chats match the selector",
                       {{"selector", selector_},
                        {"scope", "local_materialized"},
                        {"candidates", std::move(candidates)},
                        {"truncated", matches.size() > kMaximumAmbiguousCandidates}},
                       kUsage);
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_link(const std::string& link_value, ResolverScope scope) {
        auto classified = read([&](const auto& current) {
            return client_.get_internal_link_type(current, link_value);
        });
        if (!classified) {
            return std::nullopt;
        }
        if (const auto* error = classified->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* link = classified->value.get_if<core::TdInternalLink>();
        if (link == nullptr) {
            internal_error();
            return std::nullopt;
        }
        if (scope == ResolverScope::LocalMaterialized) {
            return resolve_local_link(*link);
        }
        switch (link->kind) {
        case core::TdInternalLinkKind::PublicChat:
            return resolve_public_username(link->username, "public_chat", false);
        case core::TdInternalLinkKind::BotStart:
            return resolve_public_username(link->username, "bot_start", true);
        case core::TdInternalLinkKind::Message:
            return resolve_message_link(link->url);
        case core::TdInternalLinkKind::ChatInvite:
            return resolve_invite_link(*link);
        case core::TdInternalLinkKind::DirectMessagesChat:
            return resolve_direct_messages_link(*link);
        case core::TdInternalLinkKind::SavedMessages:
            return resolve_saved_messages_link();
        case core::TdInternalLinkKind::Unsupported:
            usage("unsupported t.me link type", "unsupported_link_type");
            return std::nullopt;
        }
        internal_error();
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_local_link(const core::TdInternalLink& link) {
        switch (link.kind) {
        case core::TdInternalLinkKind::PublicChat:
        case core::TdInternalLinkKind::BotStart:
            return resolve_local_username(link.username);
        case core::TdInternalLinkKind::SavedMessages:
            return resolve_saved_messages_link();
        case core::TdInternalLinkKind::Message:
        case core::TdInternalLinkKind::ChatInvite:
        case core::TdInternalLinkKind::DirectMessagesChat:
            not_found(ResolverScope::LocalMaterialized);
            return std::nullopt;
        case core::TdInternalLinkKind::Unsupported:
            usage("unsupported t.me link type", "unsupported_link_type");
            return std::nullopt;
        }
        internal_error();
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_message_link(const std::string& url) {
        auto response =
            read([&](const auto& current) { return client_.get_message_link_info(current, url); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            if (error->code == 404) {
                not_found();
            } else {
                td_error(*error);
            }
            return std::nullopt;
        }
        const auto* info = response->value.get_if<core::TdMessageLinkInfo>();
        if (info == nullptr || !valid_int53(info->chat_id) ||
            (info->message_id && !valid_int53(*info->message_id))) {
            internal_error();
            return std::nullopt;
        }
        const auto chat = get_chat(info->chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), info->message_id, info->topic, "message",
                             info->is_public);
    }

    std::optional<ResolveResult> resolve_invite_link(const core::TdInternalLink& link) {
        if (me_.is_bot) {
            bot_unsupported();
            return std::nullopt;
        }
        auto response = read(
            [&](const auto& current) { return client_.check_chat_invite_link(current, link.url); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            if (error->code == 404) {
                not_found();
            } else {
                td_error(*error);
            }
            return std::nullopt;
        }
        const auto* info = response->value.get_if<core::TdChatInviteLinkInfo>();
        if (info == nullptr) {
            internal_error();
            return std::nullopt;
        }
        if (!valid_int53(info->chat_id)) {
            not_found();
            return std::nullopt;
        }
        const auto chat = get_chat(info->chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt, "chat_invite",
                             info->is_public);
    }

    std::optional<ResolveResult> resolve_direct_messages_link(const core::TdInternalLink& link) {
        if (me_.is_bot) {
            bot_unsupported();
            return std::nullopt;
        }
        auto channel_response = read([&](const auto& current) {
            return client_.search_public_chat(current, link.username);
        });
        if (!channel_response) {
            return std::nullopt;
        }
        if (const auto* error = channel_response->value.get_if<core::TdError>()) {
            if (error->code == 400 && (error->message == "USERNAME_NOT_OCCUPIED" ||
                                       error->message == "USERNAME_INVALID")) {
                not_found();
            } else {
                td_error(*error);
            }
            return std::nullopt;
        }
        const auto* channel = channel_response->value.get_if<core::TdChat>();
        if (channel == nullptr || channel->kind != core::TdChatKind::Channel ||
            !valid_user_id(channel->related_id)) {
            internal_error();
            return std::nullopt;
        }
        auto full_response = read([&](const auto& current) {
            return client_.get_supergroup_full_info(current, channel->related_id);
        });
        if (!full_response) {
            return std::nullopt;
        }
        if (const auto* error = full_response->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* full = full_response->value.get_if<core::TdSupergroupFullInfo>();
        if (full == nullptr) {
            internal_error();
            return std::nullopt;
        }
        if (!valid_int53(full->direct_messages_chat_id)) {
            not_found();
            return std::nullopt;
        }
        const auto chat = get_chat(full->direct_messages_chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt,
                             "direct_messages_chat");
    }

    std::optional<ResolveResult> resolve_saved_messages_link() {
        if (me_.is_bot) {
            bot_unsupported();
            return std::nullopt;
        }
        auto response = read([&](const auto& current) {
            return client_.create_private_chat(current, me_.id, false);
        });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* chat = response->value.get_if<core::TdChat>();
        if (chat == nullptr || !valid_int53(chat->id)) {
            internal_error();
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt,
                             "saved_messages");
    }

    core::TdClient& client_;
    std::string account_;
    RequestSession& session_;
    std::string selector_;
    ReadyReadSession reads_;
    std::shared_ptr<const AuthStateSnapshot> snapshot_;
    core::TdUserSummary me_;
};

} // namespace

bool valid_resolve_selector(std::string_view selector) {
    return !selector.empty() && common::valid_utf8(selector) && classify_selector(selector);
}

void ResolveCoordinator::resolve(const proto::Request& request, RequestSession& session) {
    static const std::set<std::string> fields{"selector"};
    if (!exact_fields(request.args, fields) || !request.args["selector"].is_string()) {
        session.error("USAGE", "resolve requires exactly one selector",
                      {{"argument", "selector"}, {"reason", "invalid_argument"}}, kUsage);
        return;
    }
    resolve_for_scope(request.args["selector"].get<std::string>(), ResolverScope::ActiveDialogs,
                      session);
}

void ResolveCoordinator::resolve_for_scope(std::string selector, ResolverScope scope,
                                           RequestSession& session) {
    const auto classified = !selector.empty() && common::valid_utf8(selector)
                                ? classify_selector(selector)
                                : std::nullopt;
    if (!classified) {
        session.error("USAGE", "resolve selector must be non-empty valid UTF-8",
                      {{"argument", "selector"}, {"reason", "invalid_argument"}}, kUsage);
        return;
    }
    const auto& classified_value = classified.value();
    ResolverRun run(client_.get(), account_, session, std::move(selector));
    if (!run.preflight()) {
        return;
    }
    const auto result = run.resolve(classified_value, scope);
    if (!result) {
        return;
    }
    const auto rendered = result_json(*result);
    if (!rendered) {
        session.error("INTERNAL", "resolver returned invalid result metadata",
                      {{"operation", kOperation}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    session.result(*rendered);
}

void register_resolve_command(Dispatcher& dispatcher, ResolveCoordinator& coordinator) {
    dispatcher.register_command(
        "resolve",
        {Tier::Read, [&coordinator](const proto::Request& request, RequestSession& session) {
             coordinator.resolve(request, session);
         }});
}

} // namespace tgcli::daemon
