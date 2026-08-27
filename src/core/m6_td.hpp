#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tgcli::core {

enum class TdM6FolderIcon {
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

enum class TdM6TopicColor { Blue, Yellow, Purple, Green, Pink, Red };

enum class TdM6StorageFileType {
    None,
    Animation,
    Audio,
    Document,
    LivePhotoVideo,
    NotificationSound,
    Photo,
    PhotoStory,
    ProfilePhoto,
    Secret,
    SecretThumbnail,
    Secure,
    SelfDestructingLivePhotoVideo,
    SelfDestructingPhoto,
    SelfDestructingVideo,
    SelfDestructingVideoNote,
    SelfDestructingVoiceNote,
    Sticker,
    Thumbnail,
    Unknown,
    Video,
    VideoNote,
    VideoStory,
    VoiceNote,
    Wallpaper,
};

struct TdM6CustomEmojiEntity {
    std::int32_t offset = 0;
    std::int32_t length = 0;
    std::string custom_emoji_id;

    bool operator==(const TdM6CustomEmojiEntity&) const = default;
};

struct TdM6FolderName {
    std::string text;
    bool animate_custom_emoji = false;
    std::vector<TdM6CustomEmojiEntity> custom_emoji_entities;

    bool operator==(const TdM6FolderName&) const = default;
};

struct TdM6FolderInfo {
    std::int32_t id = 0;
    TdM6FolderName name;
    TdM6FolderIcon icon = TdM6FolderIcon::Custom;
    std::int32_t color_id = -1;
    bool is_shareable = false;
    bool has_my_invite_links = false;

    bool operator==(const TdM6FolderInfo&) const = default;
};

struct TdM6ChatFolder {
    TdM6FolderName name;
    std::optional<TdM6FolderIcon> icon;
    std::int32_t color_id = -1;
    bool is_shareable = false;
    std::vector<std::int64_t> pinned_chat_ids;
    std::vector<std::int64_t> included_chat_ids;
    std::vector<std::int64_t> excluded_chat_ids;
    bool exclude_muted = false;
    bool exclude_read = false;
    bool exclude_archived = false;
    bool include_contacts = false;
    bool include_non_contacts = false;
    bool include_bots = false;
    bool include_groups = false;
    bool include_channels = false;

    bool operator==(const TdM6ChatFolder&) const = default;
};

struct TdM6ChatFoldersUpdate {
    std::vector<TdM6FolderInfo> folders;
    std::int32_t main_chat_list_position = 0;
    bool are_tags_enabled = false;

    bool operator==(const TdM6ChatFoldersUpdate&) const = default;
};

struct TdM6TopicIcon {
    TdM6TopicColor color = TdM6TopicColor::Blue;
    std::string custom_emoji_id = "0";

    bool operator==(const TdM6TopicIcon&) const = default;
};

enum class TdM6SenderKind { User, Chat, Unknown };

struct TdM6Sender {
    TdM6SenderKind kind = TdM6SenderKind::Unknown;
    std::int64_t id = 0;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const TdM6Sender&) const = default;
};

struct TdM6ForumTopicInfo {
    std::int64_t chat_id = 0;
    std::int32_t id = 0;
    std::string name;
    TdM6TopicIcon icon;
    std::int32_t creation_date = 0;
    TdM6Sender creator;
    bool is_general = false;
    bool is_outgoing = false;
    bool is_closed = false;
    bool is_hidden = false;
    bool is_name_implicit = false;

    bool operator==(const TdM6ForumTopicInfo&) const = default;
};

struct TdM6ForumTopic {
    TdM6ForumTopicInfo info;
    std::int64_t order = 0;
    bool is_pinned = false;
    std::int32_t unread_count = 0;
    std::int32_t unread_mention_count = 0;
    std::int32_t unread_reaction_count = 0;
    std::int32_t unread_poll_vote_count = 0;

    bool operator==(const TdM6ForumTopic&) const = default;
};

struct TdM6ForumTopics {
    std::int32_t total_count = 0;
    std::vector<TdM6ForumTopic> topics;
    std::int32_t next_offset_date = 0;
    std::int64_t next_offset_message_id = 0;
    std::int32_t next_offset_forum_topic_id = 0;

    bool operator==(const TdM6ForumTopics&) const = default;
};

struct TdM6AdminRights {
    bool can_manage_chat = false;
    bool can_change_info = false;
    bool can_post_messages = false;
    bool can_edit_messages = false;
    bool can_delete_messages = false;
    bool can_invite_users = false;
    bool can_restrict_members = false;
    bool can_pin_messages = false;
    bool can_manage_topics = false;
    bool can_promote_members = false;
    bool can_manage_video_chats = false;
    bool can_post_stories = false;
    bool can_edit_stories = false;
    bool can_delete_stories = false;
    bool can_manage_direct_messages = false;
    bool can_manage_tags = false;
    bool is_anonymous = false;

    bool operator==(const TdM6AdminRights&) const = default;
};

struct TdM6ChatPermissions {
    bool can_send_basic_messages = false;
    bool can_send_audios = false;
    bool can_send_documents = false;
    bool can_send_photos = false;
    bool can_send_videos = false;
    bool can_send_video_notes = false;
    bool can_send_voice_notes = false;
    bool can_send_polls = false;
    bool can_send_other_messages = false;
    bool can_add_link_previews = false;
    bool can_react_to_messages = false;
    bool can_edit_tag = false;
    bool can_change_info = false;
    bool can_invite_users = false;
    bool can_pin_messages = false;
    bool can_create_topics = false;

    bool operator==(const TdM6ChatPermissions&) const = default;
};

enum class TdM6MemberStatusKind {
    Creator,
    Administrator,
    Member,
    Restricted,
    Left,
    Banned,
    Unknown,
};

struct TdM6MemberStatus {
    TdM6MemberStatusKind kind = TdM6MemberStatusKind::Unknown;
    bool is_anonymous = false;
    bool is_member = false;
    bool can_be_edited = false;
    std::int32_t member_until_date = 0;
    std::int32_t restricted_until_date = 0;
    std::int32_t banned_until_date = 0;
    TdM6AdminRights rights;
    TdM6ChatPermissions permissions;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const TdM6MemberStatus&) const = default;
};

struct TdM6ChatMember {
    TdM6Sender member;
    std::int64_t inviter_user_id = 0;
    std::int32_t joined_chat_date = 0;
    TdM6MemberStatus status;

    bool operator==(const TdM6ChatMember&) const = default;
};

struct TdM6StarSubscriptionPricing {
    std::int32_t period = 0;
    std::int64_t star_count = 0;

    bool operator==(const TdM6StarSubscriptionPricing&) const = default;
};

struct TdM6ChatInviteLink {
    std::string invite_link;
    std::string name;
    std::int64_t creator_user_id = 0;
    std::int32_t date = 0;
    std::int32_t edit_date = 0;
    std::int32_t expiration_date = 0;
    std::int32_t member_limit = 0;
    std::int32_t member_count = 0;
    std::int32_t expired_member_count = 0;
    std::int32_t pending_join_request_count = 0;
    bool creates_join_request = false;
    bool is_primary = false;
    bool is_revoked = false;
    std::optional<TdM6StarSubscriptionPricing> subscription_pricing;

    bool operator==(const TdM6ChatInviteLink&) const = default;
};

struct TdM6ChatInviteLinks {
    std::int32_t total_count = 0;
    std::vector<TdM6ChatInviteLink> invite_links;

    bool operator==(const TdM6ChatInviteLinks&) const = default;
};

struct TdM6StorageByFileType {
    TdM6StorageFileType file_type = TdM6StorageFileType::Unknown;
    std::int64_t size = 0;
    std::int32_t count = 0;

    bool operator==(const TdM6StorageByFileType&) const = default;
};

struct TdM6StorageByChat {
    std::int64_t chat_id = 0;
    std::int64_t size = 0;
    std::int32_t count = 0;
    std::vector<TdM6StorageByFileType> by_file_type;

    bool operator==(const TdM6StorageByChat&) const = default;
};

struct TdM6StorageStatistics {
    std::int64_t size = 0;
    std::int32_t count = 0;
    std::vector<TdM6StorageByChat> by_chat;

    bool operator==(const TdM6StorageStatistics&) const = default;
};

struct TdM6Ok {
    bool operator==(const TdM6Ok&) const = default;
};

struct TdM6Users {
    std::int32_t total_count = 0;
    std::vector<std::int64_t> user_ids;

    bool operator==(const TdM6Users&) const = default;
};

struct TdM6MaybeChatFolder {
    std::optional<TdM6ChatFolder> folder;

    bool operator==(const TdM6MaybeChatFolder&) const = default;
};

struct TdM6MaybeForumTopic {
    std::optional<TdM6ForumTopic> topic;

    bool operator==(const TdM6MaybeForumTopic&) const = default;
};

struct TdM6ConversionError {
    std::optional<std::int32_t> tdlib_type_id;

    bool operator==(const TdM6ConversionError&) const = default;
};

using TdM6Response =
    std::variant<TdM6Ok, TdM6Users, TdM6MaybeChatFolder, TdM6FolderInfo, TdM6ForumTopics,
                 TdM6MaybeForumTopic, TdM6ForumTopicInfo, TdM6ChatMember, TdM6ChatInviteLink,
                 TdM6ChatInviteLinks, TdM6StorageStatistics, TdM6ConversionError>;

bool valid_td_m6_chat_folders_update(const TdM6ChatFoldersUpdate& update) noexcept;

struct TdM6GetContactsRequest {
    bool operator==(const TdM6GetContactsRequest&) const = default;
};

struct TdM6SearchContactsRequest {
    std::string query;
    std::int32_t limit = 100;

    bool operator==(const TdM6SearchContactsRequest&) const = default;
};

struct TdM6AddContactRequest {
    std::int64_t user_id = 0;
    std::string phone_number;
    std::string first_name;
    std::string last_name;
    bool share_phone_number = false;

    bool operator==(const TdM6AddContactRequest&) const = default;
};

struct TdM6RemoveContactsRequest {
    std::vector<std::int64_t> user_ids;

    bool operator==(const TdM6RemoveContactsRequest&) const = default;
};

struct TdM6SetBlockRequest {
    std::int64_t user_id = 0;
    bool blocked = false;

    bool operator==(const TdM6SetBlockRequest&) const = default;
};

struct TdM6GetChatFolderRequest {
    std::int32_t folder_id = 0;

    bool operator==(const TdM6GetChatFolderRequest&) const = default;
};

struct TdM6CreateChatFolderRequest {
    TdM6ChatFolder folder;

    bool operator==(const TdM6CreateChatFolderRequest&) const = default;
};

struct TdM6EditChatFolderRequest {
    std::int32_t folder_id = 0;
    TdM6ChatFolder folder;

    bool operator==(const TdM6EditChatFolderRequest&) const = default;
};

struct TdM6DeleteChatFolderRequest {
    std::int32_t folder_id = 0;
    std::vector<std::int64_t> leave_chat_ids;

    bool operator==(const TdM6DeleteChatFolderRequest&) const = default;
};

struct TdM6GetForumTopicsRequest {
    std::int64_t chat_id = 0;
    std::string query;
    std::int32_t offset_date = 0;
    std::int64_t offset_message_id = 0;
    std::int32_t offset_forum_topic_id = 0;
    std::int32_t limit = 100;

    bool operator==(const TdM6GetForumTopicsRequest&) const = default;
};

struct TdM6GetForumTopicRequest {
    std::int64_t chat_id = 0;
    std::int32_t topic_id = 0;

    bool operator==(const TdM6GetForumTopicRequest&) const = default;
};

struct TdM6CreateForumTopicRequest {
    std::int64_t chat_id = 0;
    std::string name;
    TdM6TopicIcon icon;
    bool is_name_implicit = false;

    bool operator==(const TdM6CreateForumTopicRequest&) const = default;
};

struct TdM6EditForumTopicRequest {
    std::int64_t chat_id = 0;
    std::int32_t topic_id = 0;
    std::string name;
    bool edit_icon_custom_emoji = false;
    std::int64_t icon_custom_emoji_id = 0;

    bool operator==(const TdM6EditForumTopicRequest&) const = default;
};

struct TdM6ToggleForumTopicRequest {
    std::int64_t chat_id = 0;
    std::int32_t topic_id = 0;
    bool is_closed = false;

    bool operator==(const TdM6ToggleForumTopicRequest&) const = default;
};

struct TdM6GetChatMemberRequest {
    std::int64_t chat_id = 0;
    std::int64_t user_id = 0;

    bool operator==(const TdM6GetChatMemberRequest&) const = default;
};

struct TdM6SetChatTitleRequest {
    std::int64_t chat_id = 0;
    std::string title;

    bool operator==(const TdM6SetChatTitleRequest&) const = default;
};

struct TdM6SetChatPhotoRequest {
    std::int64_t chat_id = 0;
    std::optional<std::string> local_path;

    bool operator==(const TdM6SetChatPhotoRequest&) const = default;
};

struct TdM6SetChatDescriptionRequest {
    std::int64_t chat_id = 0;
    std::string description;

    bool operator==(const TdM6SetChatDescriptionRequest&) const = default;
};

struct TdM6CreateChatInviteLinkRequest {
    std::int64_t chat_id = 0;

    bool operator==(const TdM6CreateChatInviteLinkRequest&) const = default;
};

struct TdM6RevokeChatInviteLinkRequest {
    std::int64_t chat_id = 0;
    std::string invite_link;

    bool operator==(const TdM6RevokeChatInviteLinkRequest&) const = default;
};

struct TdM6SetChatMemberStatusRequest {
    std::int64_t chat_id = 0;
    std::int64_t user_id = 0;
    TdM6MemberStatus status;

    bool operator==(const TdM6SetChatMemberStatusRequest&) const = default;
};

struct TdM6SetChatPermissionsRequest {
    std::int64_t chat_id = 0;
    TdM6ChatPermissions permissions;

    bool operator==(const TdM6SetChatPermissionsRequest&) const = default;
};

struct TdM6GetStorageStatisticsRequest {
    std::int32_t chat_limit = 100;

    bool operator==(const TdM6GetStorageStatisticsRequest&) const = default;
};

struct TdM6OptimizeStorageRequest {
    std::int64_t size = -1;
    std::int32_t ttl = -1;
    std::int32_t count = -1;
    std::int32_t immunity_delay = -1;
    std::vector<TdM6StorageFileType> file_types;
    std::vector<std::int64_t> chat_ids;
    std::vector<std::int64_t> exclude_chat_ids;
    bool return_deleted_file_statistics = false;
    std::int32_t chat_limit = 100;

    bool operator==(const TdM6OptimizeStorageRequest&) const = default;
};

using TdM6Request =
    std::variant<TdM6GetContactsRequest, TdM6SearchContactsRequest, TdM6AddContactRequest,
                 TdM6RemoveContactsRequest, TdM6SetBlockRequest, TdM6GetChatFolderRequest,
                 TdM6CreateChatFolderRequest, TdM6EditChatFolderRequest,
                 TdM6DeleteChatFolderRequest, TdM6GetForumTopicsRequest, TdM6GetForumTopicRequest,
                 TdM6CreateForumTopicRequest, TdM6EditForumTopicRequest,
                 TdM6ToggleForumTopicRequest, TdM6GetChatMemberRequest, TdM6SetChatTitleRequest,
                 TdM6SetChatPhotoRequest, TdM6SetChatDescriptionRequest,
                 TdM6CreateChatInviteLinkRequest, TdM6RevokeChatInviteLinkRequest,
                 TdM6SetChatMemberStatusRequest, TdM6SetChatPermissionsRequest,
                 TdM6GetStorageStatisticsRequest, TdM6OptimizeStorageRequest>;

} // namespace tgcli::core
