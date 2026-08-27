#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace tgcli::daemon {

enum class M6FolderIcon {
    All,
    Unread,
    Unmuted,
    Bots,
    Channels,
    Groups,
    Private,
    Custom,
    Setup,
    Cat,
    Crown,
    Favorite,
    Flower,
    Game,
    Home,
    Love,
    Mask,
    Party,
    Sport,
    Study,
    Trade,
    Travel,
    Work,
    Airplane,
    Book,
    Light,
    Like,
    Money,
    Note,
    Palette,
};

enum class M6TopicColor { Blue, Yellow, Purple, Green, Pink, Red };

enum class M6AdminRight {
    ChangeInfo,
    PostMessages,
    EditMessages,
    DeleteMessages,
    InviteUsers,
    RestrictMembers,
    PinMessages,
    ManageTopics,
    PromoteMembers,
    ManageVideoChats,
    PostStories,
    EditStories,
    DeleteStories,
    ManageDirectMessages,
    ManageTags,
    Anonymous,
};

enum class M6ChatPermission {
    SendBasicMessages,
    SendAudios,
    SendDocuments,
    SendPhotos,
    SendVideos,
    SendVideoNotes,
    SendVoiceNotes,
    SendPolls,
    SendOtherMessages,
    AddLinkPreviews,
    ReactToMessages,
    EditTag,
    ChangeInfo,
    InviteUsers,
    PinMessages,
    CreateTopics,
};

enum class M6TextKind { FolderName, TopicName, ChatTitle, ChatDescription };

std::span<const std::string_view> m6_folder_icon_names() noexcept;
std::span<const std::string_view> m6_topic_color_names() noexcept;
std::span<const std::string_view> m6_admin_right_names() noexcept;
std::span<const std::string_view> m6_chat_permission_names() noexcept;

std::optional<M6FolderIcon> parse_m6_folder_icon(std::string_view value) noexcept;
std::optional<M6TopicColor> parse_m6_topic_color(std::string_view value) noexcept;
std::optional<M6AdminRight> parse_m6_admin_right(std::string_view value) noexcept;
std::optional<M6ChatPermission> parse_m6_chat_permission(std::string_view value) noexcept;

std::string_view m6_folder_icon_name(M6FolderIcon value) noexcept;
std::string_view m6_topic_color_name(M6TopicColor value) noexcept;
std::string_view m6_admin_right_name(M6AdminRight value) noexcept;
std::string_view m6_chat_permission_name(M6ChatPermission value) noexcept;

bool valid_m6_canonical_text(M6TextKind kind, std::string_view value);
bool valid_m6_exact_selector(std::string_view value);
std::optional<std::int32_t> parse_m6_positive_int32(std::string_view value) noexcept;
bool valid_m6_contact_query(std::string_view value) noexcept;
bool valid_m6_invite_link(std::string_view value) noexcept;

} // namespace tgcli::daemon
