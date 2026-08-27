#include "daemon/resolver.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/local_selector.hpp"
#include "daemon/rate_limit.hpp"
#include "daemon/ready_read.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
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
constexpr std::int32_t kUserMemberPageLimit = 200;
constexpr std::size_t kMaximumAmbiguousCandidates = 20;

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
                           std::optional<ResolvedLinkType> link_type_value = std::nullopt,
                           std::optional<bool> is_public_value = std::nullopt,
                           std::optional<core::TdChat> observed_chat_value = std::nullopt)
        : chat(std::move(chat_value)), message_id(message_id_value), topic(topic_value),
          link_type(link_type_value), is_public(is_public_value),
          observed_chat(std::move(observed_chat_value)) {}

    ChatIdentity chat;
    std::optional<std::int64_t> message_id;
    std::optional<core::TdTopic> topic;
    std::optional<ResolvedLinkType> link_type;
    std::optional<bool> is_public;
    std::optional<core::TdChat> observed_chat;
};

std::optional<ChatIdentity> shallow_candidate_identity(const core::TdChat& chat) {
    std::string type;
    switch (chat.kind) {
    case core::TdChatKind::Private:
        type = "private";
        break;
    case core::TdChatKind::BasicGroup:
        type = "basic_group";
        break;
    case core::TdChatKind::Supergroup:
        type = "supergroup";
        break;
    case core::TdChatKind::Channel:
        type = "channel";
        break;
    case core::TdChatKind::Secret:
    case core::TdChatKind::Unknown:
        return std::nullopt;
    }
    if (chat.id == 0 || chat.id < -kMaximumInt53 || chat.id > kMaximumInt53 ||
        !common::valid_utf8(chat.title)) {
        return std::nullopt;
    }
    return ChatIdentity{.id = chat.id,
                        .title = chat.title,
                        .type = std::move(type),
                        .is_bot = false,
                        .usernames = {}};
}

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

bool valid_usernames(const std::vector<std::string>& usernames) {
    std::unordered_set<std::string> seen;
    for (const auto& username : usernames) {
        if (username.empty() || !common::valid_utf8(username) || !seen.insert(username).second) {
            return false;
        }
    }
    return true;
}

std::optional<UserIdentity> user_identity(const core::TdUserSummary& user) {
    if (!valid_user_id(user.id) || !common::valid_utf8(user.first_name) ||
        !common::valid_utf8(user.last_name) || !common::valid_utf8(user.phone_number) ||
        !valid_usernames(user.usernames)) {
        return std::nullopt;
    }
    std::string display_name = user.first_name;
    if (!user.last_name.empty()) {
        display_name.push_back(' ');
        display_name.append(user.last_name);
    }
    return UserIdentity{.id = user.id,
                        .display_name = std::move(display_name),
                        .usernames = user.usernames,
                        .is_bot = user.is_bot};
}

bool valid_domain_member(const core::TdChatMember& member) {
    return member.member.kind == core::TdMessageSenderKind::User &&
           valid_user_id(member.member.id) && common::valid_utf8(member.tag) &&
           member.inviter_user_id >= 0 && member.inviter_user_id <= kMaximumInt53 &&
           member.joined_chat_date >= 0 &&
           member.status.kind != core::TdChatMemberStatusKind::Unknown &&
           !member.status.unsupported_tdlib_type_id;
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

std::string_view link_type_name(ResolvedLinkType type) {
    switch (type) {
    case ResolvedLinkType::PublicChat:
        return "public_chat";
    case ResolvedLinkType::BotStart:
        return "bot_start";
    case ResolvedLinkType::Message:
        return "message";
    case ResolvedLinkType::ChatInvite:
        return "chat_invite";
    case ResolvedLinkType::DirectMessagesChat:
        return "direct_messages_chat";
    case ResolvedLinkType::SavedMessages:
        return "saved_messages";
    }
    return "message";
}

json result_json(const ResolvedChatTarget& result) {
    json topic = nullptr;
    if (result.contextual_topic) {
        topic = topic_ref_json(*result.contextual_topic);
    }
    std::string kind = "chat";
    if (result.contextual_message_id) {
        kind = "message";
    } else if (result.contextual_topic) {
        kind = "topic";
    }
    return json{
        {"kind", kind},
        {"chat", chat_identity_json(result.chat)},
        {"message_id",
         result.contextual_message_id ? json(*result.contextual_message_id) : json(nullptr)},
        {"topic", std::move(topic)},
        {"link_type", result.link_type ? json(link_type_name(*result.link_type)) : json(nullptr)},
        {"is_public", result.is_public ? json(*result.is_public) : json(nullptr)}};
}

class ResolverRun {
  public:
    ResolverRun(core::TdClient& client, std::string_view account, RequestSession& session)
        : client_(client), account_(account), session_(session), reads_(client, session) {}

    ResolverPrincipalOutcome bind_principal(ResolverCaller caller) {
        if (bound_) {
            return ResolverError{ResolverInternalError{.operation = caller}};
        }
        bound_ = true;
        caller_ = caller;
        error_.reset();
        snapshot_ = reads_.current();
        if (!snapshot_ || snapshot_->data.state != AuthState::Ready) {
            not_authed(snapshot_, "not_ready");
            return take_error_or_stop();
        }
        auto response = read([&](const auto& current) { return client_.get_me(current); });
        if (!response) {
            return take_error_or_stop();
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return take_error_or_stop();
        }
        const auto* user = response->value.get_if<core::TdUserSummary>();
        if (user == nullptr || !valid_user_id(user->id)) {
            internal_error();
            return take_error_or_stop();
        }
        me_ = *user;
        principal_ = ResolverPrincipal{.id = user->id, .is_bot = user->is_bot};
        return *principal_;
    }

    ResolverOutcome resolve_chat(std::string selector, ResolverScope scope) {
        caller_ = M2Operation::Resolve;
        error_.reset();
        selector_ = std::move(selector);
        if (!principal_) {
            internal_error();
            return take_resolve_error_or_stop();
        }
        if (scope == ResolverScope::LocalMaterialized) {
            const auto classified = classify_local_selector(selector_);
            if (!classified) {
                usage("resolve selector must be non-empty valid UTF-8", "invalid_argument");
                return take_resolve_error_or_stop();
            }
            auto result = resolve_local(*classified);
            if (!result) {
                return take_resolve_error_or_stop();
            }
            return materialize_result(std::move(*result));
        }
        const auto classified = !selector_.empty() && common::valid_utf8(selector_)
                                    ? classify_selector(selector_)
                                    : std::nullopt;
        if (!classified) {
            usage("resolve selector must be non-empty valid UTF-8", "invalid_argument");
            return take_resolve_error_or_stop();
        }
        auto result = resolve(*classified, scope);
        if (!result) {
            return take_resolve_error_or_stop();
        }
        return materialize_result(std::move(*result));
    }

    ResolverOutcome resolve_saved_messages() {
        caller_ = M2Operation::Resolve;
        error_.reset();
        if (!principal_) {
            internal_error();
            return take_resolve_error_or_stop();
        }
        auto result = resolve_saved_messages_link();
        if (!result) {
            return take_resolve_error_or_stop();
        }
        return materialize_result(std::move(*result));
    }

    ResolverOutcome resolve_saved_messages_for_write() {
        caller_ = M2Operation::Resolve;
        error_.reset();
        if (!principal_) {
            internal_error();
            return take_resolve_error_or_stop();
        }
        auto result = resolve_saved_messages_link(true);
        if (!result) {
            return take_resolve_error_or_stop();
        }
        return materialize_result(std::move(*result));
    }

    UserResolverOutcome resolve_user(std::string selector,
                                     const std::optional<core::TdChat>& domain) {
        caller_ = M2Operation::Resolve;
        error_.reset();
        selector_ = std::move(selector);
        const auto classified = classify_local_selector(selector_);
        if (!classified) {
            usage("invalid user selector", "invalid_argument", "from");
            return take_user_error_or_stop();
        }
        if (classified->kind == LocalSelectorKind::Numeric && !valid_user_id(classified->chat_id)) {
            usage("invalid user id", "invalid_argument", "from");
            return take_user_error_or_stop();
        }
        if (classified->kind == LocalSelectorKind::InvalidLink) {
            usage("invalid user profile link", "invalid_argument", "from");
            return take_user_error_or_stop();
        }
        if (classified->kind == LocalSelectorKind::BotStartLink ||
            classified->kind == LocalSelectorKind::MessageLink ||
            classified->kind == LocalSelectorKind::ChatInviteLink ||
            classified->kind == LocalSelectorKind::DirectMessagesChatLink ||
            classified->kind == LocalSelectorKind::UnsupportedLink) {
            usage("unsupported user profile link", "unsupported_link_type", "from");
            return take_user_error_or_stop();
        }
        if (!principal_) {
            internal_error();
            return take_user_error_or_stop();
        }
        switch (classified->kind) {
        case LocalSelectorKind::Numeric:
            if (auto identity = read_user_identity(classified->chat_id, true)) {
                return std::move(*identity);
            }
            return take_user_error_or_stop();
        case LocalSelectorKind::Username:
        case LocalSelectorKind::PublicChatLink:
            if (auto identity = resolve_public_user(classified->value)) {
                return std::move(*identity);
            }
            return take_user_error_or_stop();
        case LocalSelectorKind::Title:
            return resolve_user_substring(classified->value, domain);
        case LocalSelectorKind::InvalidLink:
        case LocalSelectorKind::BotStartLink:
        case LocalSelectorKind::MessageLink:
        case LocalSelectorKind::ChatInviteLink:
        case LocalSelectorKind::DirectMessagesChatLink:
        case LocalSelectorKind::UnsupportedLink:
            break;
        }
        internal_error();
        return take_user_error_or_stop();
    }

    ResolverOutcome materialize_result(ResolveResult result) {
        if (!principal_) {
            internal_error();
            return take_resolve_error_or_stop();
        }
        const auto principal = principal_.value();
        std::optional<TopicRef> topic;
        if (result.topic) {
            topic = materialize_topic_ref(*result.topic);
            if (!topic) {
                internal_error();
                return take_resolve_error_or_stop();
            }
        }
        return ResolvedChatTarget{.principal = principal,
                                  .chat = std::move(result.chat),
                                  .observed_chat = std::move(result.observed_chat),
                                  .contextual_message_id = result.message_id,
                                  .contextual_topic = topic,
                                  .link_type = result.link_type,
                                  .is_public = result.is_public,
                                  .private_user_id = last_private_user_id_,
                                  .private_user_presence = last_private_user_presence_};
    }

    ReadyReadResult read_target(const ReadyReadStart& start) {
        if (!principal_ || !snapshot_) {
            return {.status = ReadyReadStatus::Failed,
                    .value = {},
                    .authorization_failure = std::nullopt,
                    .snapshot = nullptr};
        }
        return reads_.read(start, snapshot_);
    }

    [[nodiscard]] std::optional<core::TdChat> cached_saved_messages_chat() const {
        return saved_messages_chat_;
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> bound_authorization() const {
        return snapshot_;
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> first_non_ready_after_bound() const {
        return snapshot_ ? reads_.first_non_ready_after(*snapshot_) : nullptr;
    }

  private:
    ResolverPrincipalOutcome take_error_or_stop() {
        if (error_) {
            return *error_;
        }
        return ResolverStop::Cancelled;
    }

    ResolverOutcome take_resolve_error_or_stop() {
        if (error_) {
            return *error_;
        }
        return ResolverStop::Cancelled;
    }

    UserResolverOutcome take_user_error_or_stop() {
        if (error_) {
            return *error_;
        }
        return ResolverStop::Cancelled;
    }

    std::optional<UserIdentity> read_user_identity(std::int64_t user_id,
                                                   bool direct_selector = false) {
        auto response =
            read([&](const auto& current) { return client_.get_user(current, user_id); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            if (direct_selector && (error->code == 400 || error->code == 404)) {
                not_found();
            } else {
                td_error(*error);
            }
            return std::nullopt;
        }
        const auto* user = response->value.get_if<core::TdUserSummary>();
        if (user == nullptr || user->id != user_id) {
            internal_error();
            return std::nullopt;
        }
        auto identity = user_identity(*user);
        if (!identity) {
            internal_error();
            return std::nullopt;
        }
        return identity;
    }

    std::optional<UserIdentity> resolve_public_user(const std::string& username) {
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
        if (chat == nullptr || !valid_user_id(chat->id) ||
            chat->kind != core::TdChatKind::Private || chat->id != chat->related_id) {
            internal_error();
            return std::nullopt;
        }
        auto identity = read_user_identity(chat->related_id);
        if (!identity) {
            return std::nullopt;
        }
        if (!std::ranges::any_of(identity->usernames, [&](const std::string& candidate) {
                return candidate == username;
            })) {
            internal_error();
            return std::nullopt;
        }
        return identity;
    }

    std::optional<std::vector<std::int64_t>> contact_user_ids() {
        auto response = read([&](const auto& current) { return client_.get_contacts(current); });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* contacts = response->value.get_if<core::TdUsers>();
        if (contacts == nullptr || contacts->total_count < 0 ||
            static_cast<std::uint64_t>(contacts->total_count) != contacts->user_ids.size()) {
            internal_error();
            return std::nullopt;
        }
        std::unordered_set<std::int64_t> seen;
        for (const auto id : contacts->user_ids) {
            if (!valid_user_id(id) || !seen.insert(id).second) {
                internal_error();
                return std::nullopt;
            }
        }
        return contacts->user_ids;
    }

    std::optional<std::vector<std::int64_t>> basic_group_user_ids(std::int64_t group_id) {
        auto response = read([&](const auto& current) {
            return client_.get_basic_group_full_info(current, group_id);
        });
        if (!response) {
            return std::nullopt;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(*error);
            return std::nullopt;
        }
        const auto* full = response->value.get_if<core::TdBasicGroupFullInfo>();
        if (full == nullptr || !common::valid_utf8(full->description) ||
            (full->creator_user_id != 0 && !valid_user_id(full->creator_user_id))) {
            internal_error();
            return std::nullopt;
        }
        std::vector<std::int64_t> ids;
        ids.reserve(full->members.size());
        std::unordered_set<std::int64_t> seen;
        for (const auto& member : full->members) {
            if (!valid_domain_member(member) || !seen.insert(member.member.id).second) {
                internal_error();
                return std::nullopt;
            }
            ids.push_back(member.member.id);
        }
        return ids;
    }

    std::optional<std::vector<std::int64_t>> supergroup_user_ids(std::int64_t group_id,
                                                                 const std::string& query) {
        std::vector<std::int64_t> ids;
        std::unordered_set<std::int64_t> seen;
        std::int32_t offset = 0;
        for (;;) {
            auto response = read([&](const auto& current) {
                return client_.get_supergroup_members(current, group_id, query, offset,
                                                      kUserMemberPageLimit);
            });
            if (!response) {
                return std::nullopt;
            }
            if (const auto* error = response->value.get_if<core::TdError>()) {
                td_error(*error);
                return std::nullopt;
            }
            const auto* page = response->value.get_if<core::TdChatMembers>();
            if (page == nullptr || page->total_count < 0 ||
                page->members.size() > static_cast<std::size_t>(kUserMemberPageLimit) ||
                page->members.size() > static_cast<std::size_t>(page->total_count)) {
                internal_error();
                return std::nullopt;
            }
            if (page->members.empty()) {
                return ids;
            }
            for (const auto& member : page->members) {
                if (!valid_domain_member(member)) {
                    internal_error();
                    return std::nullopt;
                }
                if (!seen.insert(member.member.id).second) {
                    pagination_invalid();
                    return std::nullopt;
                }
                ids.push_back(member.member.id);
            }
            if (page->members.size() >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - offset)) {
                internal_error();
                return std::nullopt;
            }
            offset += static_cast<std::int32_t>(page->members.size());
        }
    }

    UserResolverOutcome resolve_user_substring(const std::string& substring,
                                               const std::optional<core::TdChat>& domain) {
        std::optional<std::vector<std::int64_t>> ids;
        if (!domain) {
            if (me_.is_bot) {
                bot_unsupported();
                return take_user_error_or_stop();
            }
            ids = contact_user_ids();
        } else if (!valid_int53(domain->id) || !valid_user_id(domain->related_id) ||
                   !common::valid_utf8(domain->title)) {
            internal_error();
            return take_user_error_or_stop();
        } else if (domain->kind == core::TdChatKind::BasicGroup) {
            ids = basic_group_user_ids(domain->related_id);
        } else if (domain->kind == core::TdChatKind::Supergroup ||
                   domain->kind == core::TdChatKind::Channel) {
            ids = supergroup_user_ids(domain->related_id, substring);
        } else {
            usage("user member domain requires a group chat", "unsupported_chat_type", "from");
            return take_user_error_or_stop();
        }
        if (!ids) {
            return take_user_error_or_stop();
        }
        std::vector<UserIdentity> candidates;
        std::size_t matches = 0;
        std::optional<UserIdentity> single;
        for (const auto id : *ids) {
            auto identity = read_user_identity(id);
            if (!identity) {
                return take_user_error_or_stop();
            }
            if (identity->display_name.find(substring) == std::string::npos) {
                continue;
            }
            ++matches;
            if (matches == 1) {
                single = *identity;
            }
            if (candidates.size() < kMaximumAmbiguousCandidates) {
                candidates.push_back(std::move(*identity));
            }
        }
        if (matches == 0) {
            not_found();
            return take_user_error_or_stop();
        }
        if (matches == 1 && single) {
            return std::move(*single);
        }
        error_ = ResolverUserAmbiguousError{.selector = selector_,
                                            .candidates = std::move(candidates),
                                            .truncated = matches > kMaximumAmbiguousCandidates};
        return take_user_error_or_stop();
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
            return resolve_link(selector.value);
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

    std::optional<ResolveResult> resolve_local(const LocalSelector& selector) {
        switch (selector.kind) {
        case LocalSelectorKind::Numeric:
            return resolve_numeric(selector.chat_id);
        case LocalSelectorKind::Username:
            return resolve_local_username(selector.value);
        case LocalSelectorKind::PublicChatLink:
            return resolve_local_username(selector.value, ResolvedLinkType::PublicChat, false);
        case LocalSelectorKind::BotStartLink:
            return resolve_local_username(selector.value, ResolvedLinkType::BotStart, true);
        case LocalSelectorKind::MessageLink:
        case LocalSelectorKind::ChatInviteLink:
        case LocalSelectorKind::DirectMessagesChatLink:
            not_found(ResolverScope::LocalMaterialized);
            return std::nullopt;
        case LocalSelectorKind::InvalidLink:
            usage("invalid local link", "invalid_argument");
            return std::nullopt;
        case LocalSelectorKind::UnsupportedLink:
            usage("unsupported t.me link type", "unsupported_link_type");
            return std::nullopt;
        case LocalSelectorKind::Title:
            if (me_.is_bot) {
                bot_unsupported();
                return std::nullopt;
            }
            return resolve_title(selector.value, ResolverScope::LocalMaterialized);
        }
        internal_error();
        return std::nullopt;
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
                error_ = ResolverTimeoutError{
                    .operation = caller_,
                    .state = reads_.current()
                                 ? std::optional<AuthState>{reads_.current()->data.state}
                                 : std::nullopt};
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
        error_ = ResolverNotAuthenticatedError{
            .account = account_,
            .state = snapshot ? snapshot->data.state : AuthState::Unknown,
            .reason = reason == "authorization_lost" ? ResolverNotAuthedReason::AuthorizationLost
                                                     : ResolverNotAuthedReason::NotReady};
    }

    void bot_unsupported() {
        error_ = ResolverBotUnsupportedError{.operation = caller_};
    }

    void td_error(const core::TdError& error) {
        if (error.code == 429) {
            error_ = ResolverRateLimitedError{
                .operation = caller_, .retry_after = parse_retry_after_seconds(error.message)};
            return;
        }
        error_ = ResolverTdlibError{.operation = caller_, .tdlib_code = error.code};
    }

    void internal_error() {
        error_ = ResolverInternalError{.operation = caller_};
    }

    void pagination_invalid() {
        error_ = ResolverPaginationInvalidError{.operation = caller_};
    }

    void not_found(ResolverScope scope = ResolverScope::ActiveDialogs) {
        error_ = ResolverNotFoundError{.selector = selector_,
                                       .scope = scope == ResolverScope::LocalMaterialized
                                                    ? std::optional<ResolverScope>{scope}
                                                    : std::nullopt};
    }

    void usage(std::string_view /*message*/, std::string_view reason,
               std::string argument = "selector") {
        ResolverUsageReason typed_reason = ResolverUsageReason::InvalidArgument;
        if (reason == "unsupported_chat_type") {
            typed_reason = ResolverUsageReason::UnsupportedChatType;
        } else if (reason == "unsupported_link_type") {
            typed_reason = ResolverUsageReason::UnsupportedLinkType;
        }
        error_ = ResolverUsageError{.argument = std::move(argument), .reason = typed_reason};
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

    std::optional<ChatIdentity> identity(const core::TdChat& chat, bool reject_secret = true) {
        last_private_user_id_.reset();
        last_private_user_presence_.reset();
        const auto materialized = materialize_chat_identity(
            client_, chat, [&](const auto& start) { return read(start); });
        switch (materialized.status) {
        case ChatIdentityStatus::Success:
            last_private_user_id_ = materialized.private_user_id;
            last_private_user_presence_ = materialized.private_user_presence;
            return materialized.identity;
        case ChatIdentityStatus::Secret:
            if (reject_secret) {
                usage("secret chats are not supported", "unsupported_chat_type");
            }
            return std::nullopt;
        case ChatIdentityStatus::TdError:
            if (materialized.error) {
                td_error(*materialized.error);
            } else {
                internal_error();
            }
            return std::nullopt;
        case ChatIdentityStatus::Invalid:
            internal_error();
            return std::nullopt;
        case ChatIdentityStatus::ReadStopped:
            return std::nullopt;
        }
        internal_error();
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_numeric(std::int64_t chat_id) {
        auto chat = get_chat(chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt, std::nullopt,
                             std::nullopt, std::move(chat));
    }

    std::optional<ResolveResult> resolve_public_username(const std::string& username,
                                                         std::optional<ResolvedLinkType> link_type,
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
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt, link_type,
                             std::nullopt, *chat);
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
        error_ = ResolverAmbiguousError{.selector = selector_,
                                        .scope = scope,
                                        .candidates = std::move(candidates),
                                        .truncated = matches > kMaximumAmbiguousCandidates};
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_title(const std::string& title, ResolverScope scope) {
        return resolve_materialized(
            [&](const core::TdChat& chat) { return chat.title.find(title) != std::string::npos; },
            scope);
    }

  public:
    ResolverOutcome reject_write_title(std::string selector, std::string argument) {
        caller_ = M2Operation::Resolve;
        error_.reset();
        selector_ = std::move(selector);
        if (!principal_) {
            internal_error();
            return take_resolve_error_or_stop();
        }
        const auto ids = active_dialog_ids(ResolverScope::ActiveDialogs);
        if (!ids) {
            return take_resolve_error_or_stop();
        }
        std::vector<ChatIdentity> candidates;
        std::size_t matches = 0;
        for (const auto id : *ids) {
            const auto chat = get_chat(id, true);
            if (!chat) {
                return take_resolve_error_or_stop();
            }
            if (chat->kind == core::TdChatKind::Secret ||
                chat->title.find(selector_) == std::string::npos) {
                continue;
            }
            ++matches;
            if (candidates.size() < kMaximumAmbiguousCandidates) {
                auto candidate = shallow_candidate_identity(*chat);
                if (!candidate) {
                    internal_error();
                    return take_resolve_error_or_stop();
                }
                candidates.push_back(std::move(*candidate));
            }
        }
        return ResolverError{
            ResolverAmbiguousError{.selector = std::move(selector_),
                                   .scope = ResolverScope::ActiveDialogs,
                                   .candidates = std::move(candidates),
                                   .truncated = matches > kMaximumAmbiguousCandidates,
                                   .argument = std::move(argument)}};
    }

  private:
    std::optional<ResolveResult>
    resolve_local_username(const std::string& username,
                           std::optional<ResolvedLinkType> link_type = std::nullopt,
                           bool bot_only = false) {
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
                    materialized->usernames.end() &&
                (!bot_only || (materialized->type == "private" && materialized->is_bot))) {
                matches.push_back(std::move(*materialized));
            }
        }
        if (matches.empty()) {
            not_found(ResolverScope::LocalMaterialized);
            return std::nullopt;
        }
        if (matches.size() == 1) {
            return ResolveResult(std::move(matches.front()), std::nullopt, std::nullopt, link_type);
        }
        const bool truncated = matches.size() > kMaximumAmbiguousCandidates;
        if (truncated) {
            matches.resize(kMaximumAmbiguousCandidates);
        }
        error_ = ResolverAmbiguousError{.selector = selector_,
                                        .scope = ResolverScope::LocalMaterialized,
                                        .candidates = std::move(matches),
                                        .truncated = truncated};
        return std::nullopt;
    }

    std::optional<ResolveResult> resolve_link(const std::string& link_value) {
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
        switch (link->kind) {
        case core::TdInternalLinkKind::PublicChat:
            return resolve_public_username(link->username, ResolvedLinkType::PublicChat, false);
        case core::TdInternalLinkKind::BotStart:
            return resolve_public_username(link->username, ResolvedLinkType::BotStart, true);
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
        auto chat = get_chat(info->chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), info->message_id, info->topic,
                             ResolvedLinkType::Message, info->is_public, std::move(chat));
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
        auto chat = get_chat(info->chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt,
                             ResolvedLinkType::ChatInvite, info->is_public, std::move(chat));
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
        auto chat = get_chat(full->direct_messages_chat_id);
        if (!chat) {
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt,
                             ResolvedLinkType::DirectMessagesChat, std::nullopt, std::move(chat));
    }

    std::optional<ResolveResult>
    resolve_saved_messages_link(bool require_principal_binding = false) {
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
        if (chat == nullptr || !valid_int53(chat->id) ||
            (require_principal_binding &&
             (chat->kind != core::TdChatKind::Private || chat->related_id != me_.id))) {
            internal_error();
            return std::nullopt;
        }
        auto materialized = identity(*chat);
        if (!materialized) {
            return std::nullopt;
        }
        if (require_principal_binding &&
            (materialized->type != "private" || materialized->is_bot ||
             last_private_user_id_ != std::optional<std::int64_t>{me_.id})) {
            internal_error();
            return std::nullopt;
        }
        saved_messages_chat_ = *chat;
        return ResolveResult(std::move(*materialized), std::nullopt, std::nullopt,
                             ResolvedLinkType::SavedMessages, std::nullopt, *chat);
    }

    core::TdClient& client_;
    std::string account_;
    RequestSession& session_;
    std::string selector_;
    ReadyReadSession reads_;
    std::shared_ptr<const AuthStateSnapshot> snapshot_;
    core::TdUserSummary me_;
    ResolverCaller caller_ = M2Operation::Resolve;
    std::optional<ResolverPrincipal> principal_;
    std::optional<core::TdChat> saved_messages_chat_;
    std::optional<ResolverError> error_;
    std::optional<std::int64_t> last_private_user_id_;
    std::optional<core::TdUserPresence> last_private_user_presence_;
    bool bound_ = false;
};

} // namespace

class ResolverConsumer::Impl {
  public:
    Impl(core::TdClient& client, std::string_view account, RequestSession& session)
        : run_(client, account, session) {}

    ResolverPrincipalOutcome bind_principal(ResolverCaller caller) {
        return run_.bind_principal(caller);
    }

    ResolverOutcome resolve_chat(std::string selector, ResolverScope scope) {
        return run_.resolve_chat(std::move(selector), scope);
    }

    ResolverOutcome resolve_exact_chat(std::string selector, std::string argument) {
        const auto classification = classify_exact_write_selector(selector);
        if (argument != "chat" && argument != "from" && argument != "to") {
            return ResolverError{ResolverUsageError{
                .argument = std::move(argument), .reason = ResolverUsageReason::InvalidArgument}};
        }
        if (classification == ExactWriteSelectorStatus::Title) {
            return run_.reject_write_title(std::move(selector), std::move(argument));
        }
        if (classification != ExactWriteSelectorStatus::Exact) {
            const auto reason = classification == ExactWriteSelectorStatus::UnsupportedLink
                                    ? ResolverUsageReason::UnsupportedLinkType
                                    : ResolverUsageReason::InvalidArgument;
            return ResolverError{
                ResolverUsageError{.argument = std::move(argument), .reason = reason}};
        }
        auto result = run_.resolve_chat(std::move(selector), ResolverScope::ActiveDialogs);
        if (auto* target = std::get_if<ResolvedChatTarget>(&result);
            target != nullptr && !persistable_chat_identity(target->chat)) {
            return ResolverError{ResolverInternalError{.operation = M2Operation::Resolve}};
        }
        return result;
    }

    ResolverOutcome resolve_saved_messages() {
        return run_.resolve_saved_messages();
    }

    ResolverOutcome resolve_saved_messages_for_write() {
        return run_.resolve_saved_messages_for_write();
    }

    UserResolverOutcome resolve_user(std::string selector,
                                     const std::optional<core::TdChat>& domain) {
        return run_.resolve_user(std::move(selector), domain);
    }

    [[nodiscard]] std::optional<core::TdChat> cached_saved_messages_chat() const {
        return run_.cached_saved_messages_chat();
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> bound_authorization() const {
        return run_.bound_authorization();
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> first_non_ready_after_bound() const {
        return run_.first_non_ready_after_bound();
    }

    ReadyReadResult read_target(const ReadyReadStart& start) {
        return run_.read_target(start);
    }

  private:
    ResolverRun run_;
};

std::string_view m2_operation_name(M2Operation operation) {
    switch (operation) {
    case M2Operation::Chats:
        return "chats";
    case M2Operation::Read:
        return "read";
    case M2Operation::MsgGet:
        return "msg_get";
    case M2Operation::MsgLink:
        return "msg_link";
    case M2Operation::Search:
        return "search";
    case M2Operation::Unread:
        return "unread";
    case M2Operation::Fetch:
        return "fetch";
    case M2Operation::Resolve:
        return "resolve";
    case M2Operation::ChatInfo:
        return "chat_info";
    case M2Operation::ChatMembers:
        return "chat_members";
    case M2Operation::Listen:
        return "listen";
    case M2Operation::WaitFor:
        return "wait_for";
    }
    return "resolve";
}

std::string_view resolver_caller_name(const ResolverCaller& caller) {
    if (const auto* operation = std::get_if<M2Operation>(&caller)) {
        return m2_operation_name(*operation);
    }
    if (const auto* operation = std::get_if<proto::M3Operation>(&caller)) {
        const auto* identity = proto::m3_operation_identity(*operation);
        return identity == nullptr ? std::string_view{} : identity->canonical_name;
    }
    const auto* identity = proto::m6_operation_identity(std::get<proto::M6Operation>(caller));
    return identity == nullptr ? std::string_view{} : identity->canonical_name;
}

ResolverConsumer::ResolverConsumer(core::TdClient& client, std::string_view account,
                                   RequestSession& session)
    : impl_(std::make_unique<Impl>(client, account, session)) {}

ResolverConsumer::~ResolverConsumer() = default;

ResolverPrincipalOutcome ResolverConsumer::bind_principal(ResolverCaller caller) {
    return impl_->bind_principal(caller);
}

ResolverOutcome ResolverConsumer::resolve_chat(std::string selector, ResolverScope scope) {
    return impl_->resolve_chat(std::move(selector), scope);
}

ResolverOutcome ResolverConsumer::resolve_exact_chat(std::string selector, std::string argument) {
    return impl_->resolve_exact_chat(std::move(selector), std::move(argument));
}

ResolverOutcome ResolverConsumer::resolve_saved_messages() {
    return impl_->resolve_saved_messages();
}

ResolverOutcome ResolverConsumer::resolve_saved_messages_for_write() {
    return impl_->resolve_saved_messages_for_write();
}

UserResolverOutcome ResolverConsumer::resolve_user(std::string selector,
                                                   const std::optional<core::TdChat>& domain) {
    return impl_->resolve_user(std::move(selector), domain);
}

std::shared_ptr<const core::AuthStateSnapshot> ResolverConsumer::bound_authorization() const {
    return impl_->bound_authorization();
}

std::shared_ptr<const core::AuthStateSnapshot>
ResolverConsumer::first_non_ready_after_bound() const {
    return impl_->first_non_ready_after_bound();
}

std::optional<core::TdChat> ResolverConsumer::cached_saved_messages_chat() const {
    return impl_->cached_saved_messages_chat();
}

ReadyReadResult ResolverConsumer::read_target(const ReadyReadStart& start) {
    return impl_->read_target(start);
}

namespace {

std::string_view resolver_error_subject(const ResolverCaller& caller) {
    const auto name = resolver_caller_name(caller);
    return name == "resolve" ? std::string_view("resolver") : name;
}

json terminal(std::string code, std::string message, json details, int exit_code) {
    return {{"kind", "error"},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"details", std::move(details)},
            {"exit_code", exit_code}};
}

struct ResolverErrorMapper {
    json operator()(const ResolverUsageError& error) const {
        std::string_view reason = "invalid_argument";
        std::string_view message = "resolve selector must be non-empty valid UTF-8";
        if (error.reason == ResolverUsageReason::UnsupportedChatType) {
            reason = "unsupported_chat_type";
            message = "secret chats are not supported";
        } else if (error.reason == ResolverUsageReason::UnsupportedLinkType) {
            reason = "unsupported_link_type";
            message = "unsupported t.me link type";
        }
        return terminal("USAGE", std::string(message),
                        {{"argument", error.argument}, {"reason", reason}}, kUsage);
    }

    json operator()(const ResolverNotAuthenticatedError& error) const {
        const std::string_view reason = error.reason == ResolverNotAuthedReason::AuthorizationLost
                                            ? "authorization_lost"
                                            : "not_ready";
        return terminal("NOT_AUTHED",
                        std::string(m2_operation_name(owning_operation)) +
                            " requires an authenticated account",
                        {{"account", error.account},
                         {"state", core::auth_state_name(error.state)},
                         {"reason", reason}},
                        kNotAuthed);
    }

    json operator()(const ResolverBotUnsupportedError& error) const {
        return terminal("BOT_UNSUPPORTED", "this resolver branch requires a user account",
                        {{"operation", resolver_caller_name(error.operation)}}, kUsage);
    }

    json operator()(const ResolverNotFoundError& error) const {
        json details{{"selector", error.selector}};
        if (error.scope == ResolverScope::LocalMaterialized) {
            details["scope"] = "local_materialized";
        }
        return terminal("NOT_FOUND", "no chat or link target matches the selector",
                        std::move(details), kNotFound);
    }

    json operator()(const ResolverAmbiguousError& error) const {
        json candidates = json::array();
        for (const auto& candidate : error.candidates) {
            candidates.push_back(chat_identity_json(candidate));
        }
        return terminal(
            "AMBIGUOUS", "multiple chats match the selector",
            {{"selector", error.selector},
             {"scope", error.scope == ResolverScope::ActiveDialogs ? "active_dialogs"
                                                                   : "local_materialized"},
             {"candidates", std::move(candidates)},
             {"truncated", error.truncated}},
            kUsage);
    }

    json operator()(const ResolverRateLimitedError& error) const {
        return terminal("RATE_LIMITED", "Telegram rate limit",
                        {{"operation", resolver_caller_name(error.operation)},
                         {"tdlib_code", 429},
                         {"retry_after", error.retry_after}},
                        kRateLimited);
    }

    json operator()(const ResolverTdlibError& error) const {
        return terminal("TDLIB_ERROR",
                        std::string(resolver_error_subject(error.operation)) +
                            " TDLib request failed",
                        {{"operation", resolver_caller_name(error.operation)},
                         {"tdlib_code", error.tdlib_code}},
                        kGeneric);
    }

    json operator()(const ResolverTimeoutError& error) const {
        return terminal(
            "TIMEOUT", std::string(resolver_error_subject(error.operation)) + " request timed out",
            {{"operation", resolver_caller_name(error.operation)},
             {"state", error.state ? json(core::auth_state_name(*error.state)) : json(nullptr)}},
            kTimeout);
    }

    json operator()(const ResolverInternalError& error) const {
        return terminal(
            "INTERNAL",
            std::string(resolver_error_subject(error.operation)) + " returned an unexpected object",
            {{"operation", resolver_caller_name(error.operation)}, {"reason", "internal_error"}},
            kGeneric);
    }

    json operator()(const ResolverUserAmbiguousError& error) const {
        json candidates = json::array();
        for (const auto& candidate : error.candidates) {
            candidates.push_back({{"id", candidate.id},
                                  {"display_name", candidate.display_name},
                                  {"usernames", candidate.usernames},
                                  {"is_bot", candidate.is_bot}});
        }
        return terminal("AMBIGUOUS", "multiple users match the selector",
                        {{"selector", error.selector},
                         {"candidates", std::move(candidates)},
                         {"truncated", error.truncated}},
                        kUsage);
    }

    json operator()(const ResolverPaginationInvalidError& error) const {
        return terminal("PAGINATION_INVALID", "resolver pagination did not advance",
                        {{"operation", resolver_caller_name(error.operation)},
                         {"reason", "non_advancing_upstream"}},
                        kGeneric);
    }

    M2Operation owning_operation;
};

} // namespace

void emit_resolver_error(const ResolverError& error, RequestSession& session,
                         M2Operation owning_operation) {
    auto mapped = resolver_error_terminal(error, owning_operation);
    session.error(mapped["code"].get<std::string>(), mapped["message"].get<std::string>(),
                  std::move(mapped["details"]), mapped["exit_code"].get<int>());
}

json resolver_error_terminal(const ResolverError& error, M2Operation owning_operation) {
    return std::visit(ResolverErrorMapper{owning_operation}, error);
}

bool valid_resolve_selector(std::string_view selector) {
    return !selector.empty() && common::valid_utf8(selector) && classify_selector(selector);
}

ExactWriteSelectorStatus classify_exact_write_selector(std::string_view selector) {
    const auto classified = classify_local_selector(selector);
    if (!classified || classified->kind == LocalSelectorKind::InvalidLink) {
        return ExactWriteSelectorStatus::Invalid;
    }
    if (classified->kind == LocalSelectorKind::Title) {
        return ExactWriteSelectorStatus::Title;
    }
    if (classified->kind == LocalSelectorKind::UnsupportedLink) {
        return ExactWriteSelectorStatus::UnsupportedLink;
    }
    return ExactWriteSelectorStatus::Exact;
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
    if (scope == ResolverScope::LocalMaterialized) {
        const auto classified = classify_local_selector(selector);
        std::optional<ResolverUsageReason> reason;
        if (!classified || classified->kind == LocalSelectorKind::InvalidLink) {
            reason = ResolverUsageReason::InvalidArgument;
        } else if (classified->kind == LocalSelectorKind::UnsupportedLink) {
            reason = ResolverUsageReason::UnsupportedLinkType;
        }
        if (reason) {
            emit_resolver_error(
                ResolverError{ResolverUsageError{.argument = "selector", .reason = *reason}},
                session);
            return;
        }
    }
    ResolverConsumer consumer(client_.get(), account_, session);
    const auto principal = consumer.bind_principal(M2Operation::Resolve);
    if (const auto* error = std::get_if<ResolverError>(&principal)) {
        emit_resolver_error(*error, session);
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal)) {
        return;
    }
    const auto outcome = consumer.resolve_chat(std::move(selector), scope);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session);
        return;
    }
    if (const auto* result = std::get_if<ResolvedChatTarget>(&outcome)) {
        session.result(result_json(*result));
    }
}

void register_resolve_command(Dispatcher& dispatcher, ResolveCoordinator& coordinator) {
    dispatcher.register_command(
        "resolve",
        {Tier::Read, [&coordinator](const proto::Request& request, RequestSession& session) {
             coordinator.resolve(request, session);
         }});
}

} // namespace tgcli::daemon
