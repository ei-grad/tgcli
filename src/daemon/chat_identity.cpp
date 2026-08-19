#include "daemon/chat_identity.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_user_id(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
}

bool valid_usernames(const std::vector<std::string>& usernames) {
    return std::ranges::all_of(usernames, [](const std::string& username) {
        return !username.empty() && common::valid_utf8(username);
    });
}

std::optional<std::string_view> chat_type_name(core::TdChatKind kind) {
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
    case core::TdChatKind::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

ChatIdentityResult stopped() {
    return {
        .status = ChatIdentityStatus::ReadStopped, .identity = std::nullopt, .error = std::nullopt};
}

ChatIdentityResult invalid() {
    return {.status = ChatIdentityStatus::Invalid, .identity = std::nullopt, .error = std::nullopt};
}

ChatIdentityResult td_error(const core::TdError& error) {
    return {.status = ChatIdentityStatus::TdError, .identity = std::nullopt, .error = error};
}

} // namespace

ChatIdentityResult materialize_chat_identity(core::TdClient& client, const core::TdChat& chat,
                                             const ChatIdentityRead& read) {
    if (!valid_int53(chat.id) || !common::valid_utf8(chat.title)) {
        return invalid();
    }
    if (chat.kind == core::TdChatKind::Secret) {
        return {
            .status = ChatIdentityStatus::Secret, .identity = std::nullopt, .error = std::nullopt};
    }
    const auto type = chat_type_name(chat.kind);
    if (!type) {
        return invalid();
    }
    ChatIdentity identity{.id = chat.id,
                          .title = chat.title,
                          .type = std::string(*type),
                          .is_bot = false,
                          .usernames = {}};
    if (chat.kind == core::TdChatKind::BasicGroup) {
        return {.status = ChatIdentityStatus::Success,
                .identity = std::move(identity),
                .error = std::nullopt};
    }
    if (!valid_user_id(chat.related_id)) {
        return invalid();
    }
    if (chat.kind == core::TdChatKind::Private) {
        auto response =
            read([&](const auto& current) { return client.get_user(current, chat.related_id); });
        if (!response) {
            return stopped();
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            return td_error(*error);
        }
        const auto* user = response->value.get_if<core::TdUserSummary>();
        if (user == nullptr || user->id != chat.related_id || !valid_usernames(user->usernames)) {
            return invalid();
        }
        identity.is_bot = user->is_bot;
        identity.usernames = user->usernames;
        return {.status = ChatIdentityStatus::Success,
                .identity = std::move(identity),
                .error = std::nullopt};
    }
    auto response =
        read([&](const auto& current) { return client.get_supergroup(current, chat.related_id); });
    if (!response) {
        return stopped();
    }
    if (const auto* error = response->value.get_if<core::TdError>()) {
        return td_error(*error);
    }
    const auto* supergroup = response->value.get_if<core::TdSupergroup>();
    const bool expected_channel = chat.kind == core::TdChatKind::Channel;
    if (supergroup == nullptr || supergroup->id != chat.related_id ||
        supergroup->is_channel != expected_channel || !valid_usernames(supergroup->usernames)) {
        return invalid();
    }
    identity.usernames = supergroup->usernames;
    return {.status = ChatIdentityStatus::Success,
            .identity = std::move(identity),
            .error = std::nullopt};
}

nlohmann::json chat_identity_json(const ChatIdentity& identity) {
    return {{"id", identity.id},
            {"title", identity.title},
            {"type", identity.type},
            {"is_bot", identity.is_bot},
            {"usernames", identity.usernames}};
}

} // namespace tgcli::daemon
