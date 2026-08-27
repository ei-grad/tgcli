#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const ref = (name) => ({ $ref: `#/$defs/${name}` });
const object = (properties, required = Object.keys(properties)) => ({
  type: "object",
  additionalProperties: false,
  required,
  properties,
});
const array = (items, maximum, minimum = 0, unique = false) => ({
  type: "array",
  minItems: minimum,
  maxItems: maximum,
  ...(unique ? { uniqueItems: true } : {}),
  items,
});
const nullable = (schema) => ({ oneOf: [{ type: "null" }, schema] });
const boundedString = (minimum, maximum) => ({ type: "string", minLength: minimum, maxLength: maximum });

const folderIcons = [
  "all", "unread", "unmuted", "bots", "channels", "groups", "private", "custom",
  "setup", "cat", "crown", "favorite", "flower", "game", "home", "love", "mask",
  "party", "sport", "study", "trade", "travel", "work", "airplane", "book", "light",
  "like", "money", "note", "palette",
];
const topicColors = ["blue", "yellow", "purple", "green", "pink", "red"];
const adminRights = [
  "change-info", "post-messages", "edit-messages", "delete-messages", "invite-users",
  "restrict-members", "pin-messages", "manage-topics", "promote-members",
  "manage-video-chats", "post-stories", "edit-stories", "delete-stories",
  "manage-direct-messages", "manage-tags", "anonymous",
];
const permissions = [
  "send-basic-messages", "send-audios", "send-documents", "send-photos", "send-videos",
  "send-video-notes", "send-voice-notes", "send-polls", "send-other-messages",
  "add-link-previews", "react-to-messages", "edit-tag", "change-info", "invite-users",
  "pin-messages", "create-topics",
];
const storageTypes = [
  "none", "animation", "audio", "document", "live-photo-video", "notification-sound",
  "photo", "photo-story", "profile-photo", "secret", "secret-thumbnail", "secure",
  "self-destructing-live-photo-video", "self-destructing-photo", "self-destructing-video",
  "self-destructing-video-note", "self-destructing-voice-note", "sticker", "thumbnail",
  "unknown", "video", "video-note", "video-story", "voice-note", "wallpaper",
];

const definitions = {
  account: { type: "string", pattern: "^[A-Za-z0-9_-]{1,32}$" },
  int53: { type: "integer", minimum: -9007199254740991, maximum: 9007199254740991, not: { const: 0 } },
  positiveInt53: { type: "integer", minimum: 1, maximum: 9007199254740991 },
  positiveInt32: { type: "integer", minimum: 1, maximum: 2147483647 },
  nonnegativeInt32: { type: "integer", minimum: 0, maximum: 2147483647 },
  int32: { type: "integer", minimum: -2147483648, maximum: 2147483647 },
  int64String: { type: "string", pattern: "^(?:0|-?[1-9][0-9]{0,18})$" },
  hash: { type: "string", pattern: "^sha256:[0-9a-f]{64}$" },
  timestamp: { type: "string", pattern: "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$" },
  chat: object({
    id: ref("int53"),
    title: { type: "string" },
    type: { enum: ["private", "basic_group", "supergroup", "channel"] },
    is_bot: { type: "boolean" },
    usernames: array({ type: "string", pattern: "^[A-Za-z0-9_]{1,32}$" }, 100, 0, true),
  }),
  user: object({
    id: ref("positiveInt53"),
    display_name: { type: "string" },
    usernames: array({ type: "string", minLength: 1 }, 100, 0, true),
    is_bot: { type: "boolean" },
  }),
  folderName: object({
    text: boundedString(1, 12),
    animate_custom_emoji: { type: "boolean" },
    custom_emoji_entities: array(object({
      offset: ref("nonnegativeInt32"),
      length: ref("positiveInt32"),
      custom_emoji_id: ref("int64String"),
    }), 12),
  }),
  folderSummary: object({
    id: ref("positiveInt32"),
    name: ref("folderName"),
    icon: { enum: folderIcons },
    color_id: { type: "integer", minimum: -1, maximum: 6 },
    is_shareable: { type: "boolean" },
    has_my_invite_links: { type: "boolean" },
  }),
  folderSnapshot: object({
    id: ref("positiveInt32"),
    name: ref("folderName"),
    icon: nullable({ enum: folderIcons }),
    color_id: { type: "integer", minimum: -1, maximum: 6 },
    is_shareable: { type: "boolean" },
    has_my_invite_links: { type: "boolean" },
    pinned_chat_ids: array(ref("int53"), 100, 0, true),
    included_chat_ids: array(ref("int53"), 100, 0, true),
    excluded_chat_ids: array(ref("int53"), 100, 0, true),
    exclude_muted: { type: "boolean" },
    exclude_read: { type: "boolean" },
    exclude_archived: { type: "boolean" },
    include_contacts: { type: "boolean" },
    include_non_contacts: { type: "boolean" },
    include_bots: { type: "boolean" },
    include_groups: { type: "boolean" },
    include_channels: { type: "boolean" },
  }),
  sender: {
    oneOf: [
      object({ type: { const: "user" }, id: ref("positiveInt53") }),
      object({ type: { const: "chat" }, id: ref("int53") }),
    ],
  },
  topicIcon: object({ color: { enum: topicColors }, custom_emoji_id: ref("int64String") }),
  topicInfo: object({
    chat_id: ref("int53"),
    id: ref("positiveInt32"),
    name: boundedString(1, 128),
    icon: ref("topicIcon"),
    creation_date: ref("timestamp"),
    creator: ref("sender"),
    is_general: { type: "boolean" },
    is_outgoing: { type: "boolean" },
    is_closed: { type: "boolean" },
    is_hidden: { type: "boolean" },
    is_name_implicit: { type: "boolean" },
  }),
  topicRow: object({
    chat_id: ref("int53"), id: ref("positiveInt32"), name: boundedString(1, 128),
    icon: ref("topicIcon"), creation_date: ref("timestamp"), creator: ref("sender"),
    is_general: { type: "boolean" }, is_outgoing: { type: "boolean" },
    is_closed: { type: "boolean" }, is_hidden: { type: "boolean" },
    is_name_implicit: { type: "boolean" }, is_pinned: { type: "boolean" },
    unread_count: ref("nonnegativeInt32"), unread_mention_count: ref("nonnegativeInt32"),
    unread_reaction_count: ref("nonnegativeInt32"), unread_poll_vote_count: ref("nonnegativeInt32"),
  }),
  fileSnapshot: object({
    path: boundedString(1, 4096), name: boundedString(1, 255),
    size: { type: "integer", minimum: 1 }, sha256: ref("hash"),
    device: { type: "integer", minimum: 0 }, inode: { type: "integer", minimum: 0 },
    mtime_ns: { type: "integer" }, ctime_ns: { type: "integer" },
  }),
  memberStatus: {
    oneOf: [
      object({ kind: { const: "creator" }, is_anonymous: { type: "boolean" }, is_member: { type: "boolean" } }),
      object({ kind: { const: "administrator" }, can_be_edited: { type: "boolean" },
               can_manage_chat: { const: true }, rights: array({ enum: adminRights }, 16, 0, true) }),
      object({ kind: { const: "member" }, member_until_date: ref("int32") }),
      object({ kind: { const: "restricted" }, is_member: { type: "boolean" },
               restricted_until_date: ref("int32"), permissions: array({ enum: permissions }, 16, 0, true) }),
      object({ kind: { const: "left" } }),
      object({ kind: { const: "banned" }, banned_until_date: ref("int32") }),
    ],
  },
  storageType: object({ file_type: { enum: storageTypes }, size: { type: "integer", minimum: 0, maximum: 9007199254740991 }, count: ref("nonnegativeInt32") }),
  storageChat: object({ chat_id: { type: "integer", minimum: -9007199254740991, maximum: 9007199254740991 }, size: { type: "integer", minimum: 0, maximum: 9007199254740991 }, count: ref("nonnegativeInt32"), by_file_type: array(ref("storageType"), 25, 0, true) }),
  storage: object({ size: { type: "integer", minimum: 0, maximum: 9007199254740991 }, count: ref("nonnegativeInt32"), by_chat: array(ref("storageChat"), 101) }),
};

const operations = [
  ["contact-list", "contact list", "contact_list", false], ["contact-search", "contact search", "contact_search", false],
  ["contact-add", "contact add", "contact_add", true], ["contact-remove", "contact remove", "contact_remove", true],
  ["contact-block", "contact block", "contact_block", true], ["contact-unblock", "contact unblock", "contact_unblock", true],
  ["folder-list", "folder list", "folder_list", false], ["folder-show", "folder show", "folder_show", false],
  ["folder-create", "folder create", "folder_create", true], ["folder-edit", "folder edit", "folder_edit", true],
  ["folder-delete", "folder delete", "folder_delete", true], ["folder-add-chat", "folder add-chat", "folder_add_chat", true],
  ["folder-remove-chat", "folder remove-chat", "folder_remove_chat", true], ["topic-list", "topic list", "topic_list", false],
  ["topic-create", "topic create", "topic_create", true], ["topic-edit", "topic edit", "topic_edit", true],
  ["topic-close", "topic close", "topic_close", true], ["topic-reopen", "topic reopen", "topic_reopen", true],
  ["chat-set-title", "chat set-title", "chat_set_title", true], ["chat-set-photo", "chat set-photo", "chat_set_photo", true],
  ["chat-set-description", "chat set-description", "chat_set_description", true], ["chat-invite-link", "chat invite-link", "chat_invite_link", true],
  ["chat-promote", "chat promote", "chat_promote", true], ["chat-demote", "chat demote", "chat_demote", true],
  ["chat-ban", "chat ban", "chat_ban", true], ["chat-unban", "chat unban", "chat_unban", true],
  ["chat-kick", "chat kick", "chat_kick", true], ["chat-set-permissions", "chat set-permissions", "chat_set_permissions", true],
  ["storage-stats", "storage stats", "storage_stats", false], ["storage-optimize", "storage optimize", "storage_optimize", true],
];

const resultFor = (operation) => {
  if (operation === "contact_list" || operation === "contact_search") return object({ items: array(ref("user"), operation === "contact_search" ? 100 : 131072), next: { type: "null" } });
  if (operation === "contact_add" || operation === "contact_remove") return object({ user: ref("user"), is_contact: { const: operation === "contact_add" } });
  if (operation === "contact_block" || operation === "contact_unblock") return object({ user: ref("user"), blocked: { const: operation === "contact_block" } });
  if (operation === "folder_list") return object({ items: array(ref("folderSummary"), 100), next: { type: "null" } });
  if (operation === "folder_show") return object({ folder: ref("folderSnapshot") });
  if (operation === "folder_create" || operation === "folder_edit") return object({ folder: ref("folderSnapshot") });
  if (operation === "folder_delete") return object({ folder_id: ref("positiveInt32"), deleted: { const: true } });
  if (operation === "folder_add_chat" || operation === "folder_remove_chat") return object({ folder: ref("folderSnapshot"), chat: ref("chat"), included: { const: operation === "folder_add_chat" } });
  if (operation === "topic_list") return object({ items: array(ref("topicRow"), 4096), next: { type: "null" } });
  if (operation === "topic_create") return object({ topic: ref("topicInfo") });
  if (operation === "topic_edit") return object({ chat: ref("chat"), topic_id: ref("positiveInt32"), name: boundedString(1, 128) });
  if (operation === "topic_close" || operation === "topic_reopen") return object({ chat: ref("chat"), topic_id: ref("positiveInt32"), closed: { const: operation === "topic_close" } });
  if (operation === "chat_set_title") return object({ chat: ref("chat"), title: boundedString(1, 128) });
  if (operation === "chat_set_photo") return object({ chat: ref("chat"), photo: { enum: ["set", "deleted"] } });
  if (operation === "chat_set_description") return object({ chat: ref("chat"), description: boundedString(0, 255) });
  if (operation === "chat_invite_link") return object({ chat: ref("chat"), action: { enum: ["create", "revoke"] }, invite_link: nullable(boundedString(1, 4096)) });
  if (operation === "chat_promote") return object({ chat: ref("chat"), user: ref("user"), status: { const: "administrator" }, can_manage_chat: { const: true }, rights: array({ enum: adminRights }, 16, 1, true) });
  if (["chat_demote", "chat_ban", "chat_unban", "chat_kick"].includes(operation)) return object({ chat: ref("chat"), user: ref("user"), status: { const: operation === "chat_demote" ? "member" : operation === "chat_ban" ? "banned" : "left" } });
  if (operation === "chat_set_permissions") return object({ chat: ref("chat"), permissions: array({ enum: permissions }, 16, 0, true) });
  if (operation === "storage_stats") return ref("storage");
  return object({ optimized: { const: true }, statistics: ref("storage") });
};

const planFor = (operation) => {
  const common = { operation: { const: operation }, account: ref("account") };
  const request = (name, rest) => object({ ...common, tdlib_request: name, ...rest });
  if (operation === "contact_add") return request({ const: "addContact" }, { user: ref("user"), first_name: boundedString(1, 64), last_name: boundedString(0, 64), phone_number_sha256: ref("hash"), share_phone_number: { const: false } });
  if (operation === "contact_remove") return request({ const: "removeContacts" }, { user: ref("user"), is_contact: { const: false } });
  if (operation === "contact_block" || operation === "contact_unblock") return request({ const: "setMessageSenderBlockList" }, { user: ref("user"), blocked: { const: operation === "contact_block" } });
  if (operation === "folder_create") return request({ const: "createChatFolder" }, { name: ref("folderName"), icon: nullable({ enum: folderIcons }), color_id: { type: "integer", minimum: -1, maximum: 6 }, chat_ids: array(ref("int53"), 100, 1, true) });
  if (operation === "folder_edit") return request({ const: "editChatFolder" }, { folder_id: ref("positiveInt32"), before: ref("folderSnapshot"), after: ref("folderSnapshot") });
  if (operation === "folder_delete") return request({ const: "deleteChatFolder" }, { folder: ref("folderSnapshot"), leave_chat_ids: { const: [] } });
  if (operation === "folder_add_chat" || operation === "folder_remove_chat") return request({ const: "editChatFolder" }, { folder_id: ref("positiveInt32"), chat: ref("chat"), before: ref("folderSnapshot"), after: ref("folderSnapshot") });
  if (operation === "topic_create") return request({ const: "createForumTopic" }, { chat: ref("chat"), name: boundedString(1, 128), icon: { enum: topicColors } });
  if (operation === "topic_edit") return request({ const: "editForumTopic" }, { chat: ref("chat"), before: ref("topicInfo"), name: boundedString(1, 128) });
  if (operation === "topic_close" || operation === "topic_reopen") return request({ const: "toggleForumTopicIsClosed" }, { chat: ref("chat"), before: ref("topicInfo"), closed: { const: operation === "topic_close" } });
  if (operation === "chat_set_title") return request({ const: "setChatTitle" }, { chat: ref("chat"), title: boundedString(1, 128) });
  if (operation === "chat_set_photo") return request({ const: "setChatPhoto" }, { chat: ref("chat"), delete: { type: "boolean" }, file: nullable(ref("fileSnapshot")) });
  if (operation === "chat_set_description") return request({ const: "setChatDescription" }, { chat: ref("chat"), description: boundedString(0, 255) });
  if (operation === "chat_invite_link") return request({ enum: ["createChatInviteLink", "revokeChatInviteLink"] }, { chat: ref("chat"), action: { enum: ["create", "revoke"] }, invite_link_sha256: nullable(ref("hash")) });
  if (operation === "chat_promote") return request({ const: "setChatMemberStatus" }, { chat: ref("chat"), user: ref("user"), before: ref("memberStatus"), can_manage_chat: { const: true }, rights: array({ enum: adminRights }, 16, 1, true) });
  if (["chat_demote", "chat_ban", "chat_unban", "chat_kick"].includes(operation)) return request({ const: "setChatMemberStatus" }, { chat: ref("chat"), user: ref("user"), before: ref("memberStatus"), after: { const: operation === "chat_demote" ? "member" : operation === "chat_ban" ? "banned" : "left" } });
  if (operation === "chat_set_permissions") return request({ const: "setChatPermissions" }, { chat: ref("chat"), permissions: array({ enum: permissions }, 16, 0, true) });
  return request({ const: "optimizeStorage" }, { size: { const: -1 }, ttl: { const: -1 }, count: { const: -1 }, immunity_delay: { const: -1 }, file_types: { const: [] }, chat_ids: { const: [] }, exclude_chat_ids: { const: [] }, return_deleted_file_statistics: { const: false }, chat_limit: { const: 100 } });
};

const write = (filename, document) => fs.writeFileSync(path.join(directory, filename), `${JSON.stringify(document, null, 2)}\n`);

for (const [stem, command, operation, mutation] of operations) {
  const document = {
    $schema: "https://json-schema.org/draft/2020-12/schema",
    title: `tgcli ${command} result`,
    $defs: definitions,
  };
  const real = resultFor(operation);
  document.oneOf = mutation
    ? [real, object({ dry_run: { const: true }, plan: planFor(operation) })]
    : [real];
  write(`${stem}.result.schema.json`, document);
}

const state = { enum: ["unknown", "wait_tdlib_parameters", "wait_phone_number", "wait_premium_purchase", "wait_email_address", "wait_email_code", "wait_code", "wait_other_device_confirmation", "wait_registration", "wait_password", "ready", "logging_out", "closing", "closed"] };
const errorEnvelope = (code, details) => object({ code: { const: code }, message: { type: "string" }, details });
const familyOperations = {
  "contact.error.schema.json": ["contact_list", "contact_search", "contact_add", "contact_remove", "contact_block", "contact_unblock"],
  "folder.error.schema.json": ["folder_list", "folder_show", "folder_create", "folder_edit", "folder_delete", "folder_add_chat", "folder_remove_chat"],
  "topic.error.schema.json": ["topic_list", "topic_create", "topic_edit", "topic_close", "topic_reopen"],
  "chat-admin.error.schema.json": ["chat_set_title", "chat_set_photo", "chat_set_description", "chat_invite_link", "chat_promote", "chat_demote", "chat_ban", "chat_unban", "chat_kick", "chat_set_permissions"],
  "storage.error.schema.json": ["storage_stats", "storage_optimize"],
};
for (const [filename, family] of Object.entries(familyOperations)) {
  const operation = { enum: family };
  const mutations = family.filter((name) => operations.find((entry) => entry[2] === name)?.[3]);
  const idempotent = mutations.filter((name) => !["chat_invite_link", "storage_optimize"].includes(name));
  const destructive = mutations.filter((name) => ["folder_delete", "chat_invite_link", "chat_ban", "chat_kick", "storage_optimize"].includes(name));
  const detailsOperation = (extra = {}) => object({ operation, ...extra });
  const branches = [
    errorEnvelope("USAGE", object({ argument: nullable({ type: "string" }), reason: { enum: ["invalid_argument", "missing_argument", "mutually_exclusive", "unsupported_mode", "unsupported_chat_type"] } })),
    errorEnvelope("ACCOUNT_NOT_FOUND", object({ account: ref("account") })),
    errorEnvelope("ACCOUNT_MISMATCH", object({ requested_account: ref("account"), daemon_account: ref("account") })),
    errorEnvelope("CONFIG_INVALID", object({ path: { type: "string" }, reason: { enum: ["path_invalid", "wrong_owner", "wrong_type", "wrong_mode", "wrong_link_count", "too_large", "parse_error", "type_error", "invalid_default", "conflicting_credentials", "io_error", "sync_error"] } })),
    errorEnvelope("CONFIG_CONFLICT", object({ path: { type: "string" }, expected: { type: "string" }, current: { type: "string" } })),
    errorEnvelope("HOOK_FAILED", object({ hook: { enum: ["api_id_cmd", "api_hash_cmd", "db_key_cmd", "password_cmd", "bot_token_cmd"] }, reason: { enum: ["spawn", "exit", "signal", "timeout", "stdout_empty", "stdout_invalid", "stdout_too_large", "stderr_too_large"] }, status: nullable(ref("int32")) })),
    errorEnvelope("BOT_UNSUPPORTED", detailsOperation()),
    errorEnvelope("TDLIB_ERROR", detailsOperation({ tdlib_code: ref("int32") })),
    errorEnvelope("RATE_LIMITED", detailsOperation({ tdlib_code: { const: 429 }, retry_after: ref("nonnegativeInt32") })),
    errorEnvelope("NOT_AUTHED", object({ account: ref("account"), state, reason: { enum: ["not_ready", "authorization_lost", "login_required"] } })),
    errorEnvelope("INTERNAL", {
      oneOf: [
        detailsOperation({ reason: { enum: ["internal_error", "malformed_tdlib_response"] } }),
        detailsOperation({ reason: { const: "capacity_exhausted" }, resource: { enum: ["users", "topics", "bytes", "item_bytes"] }, limit: { type: "integer", minimum: 0 } }),
      ],
    }),
    errorEnvelope("TIMEOUT", {
      oneOf: [
        detailsOperation({ state: nullable(state) }),
        detailsOperation({ phase: { enum: ["preflight", "confirmation", "dispatch"] }, state: nullable(state), outcome: { enum: ["not_started", "unknown"] }, idempotency: { enum: ["not_created", "not_requested", "removed", "pending"] } }),
      ],
    }),
    errorEnvelope("DAEMON_SHUTDOWN", object({ reason: { const: "daemon_shutdown" } })),
    errorEnvelope("PROTOCOL_ANSWER_INVALID", object({ request_id: { type: "integer", minimum: 0, maximum: 18446744073709551615 }, reason: { enum: ["future_sequence", "nonce_mismatch", "generation_mismatch", "malformed", "unknown_request"] } })),
    errorEnvelope("DAEMON_NOT_RUNNING", object({ account: ref("account"), socket: { type: "string" } })),
    errorEnvelope("AUDIT_UNAVAILABLE", object({ account: ref("account"), path: { type: "string" }, reason: { type: "string" } })),
    errorEnvelope("AUDIT_INCOMPLETE", object({ account: ref("account"), path: {}, mutation_state: { enum: ["none", "possible", "confirmed"] }, completed_stages: array({ type: "string" }, 6) })),
    errorEnvelope("SPOOL_UNAVAILABLE", detailsOperation({ path: { type: "string" }, reason: { type: "string" } })),
  ];
  if (mutations.length) {
    branches.push(errorEnvelope("WRITE_DENIED", object({ account: ref("account"), reason: { enum: ["missing_authority", "config_denied", "explicit_denial"] } })));
    if (destructive.length) {
      const confirmationPlans = destructive.map((name) => planFor(name));
      branches.push(errorEnvelope("CONFIRMATION_REQUIRED", object({ account: ref("account"), action: { enum: destructive }, target: { oneOf: confirmationPlans } })));
    }
    if (idempotent.length) {
      const idempotentOperation = { enum: idempotent };
      branches.push(errorEnvelope("IDEMPOTENCY_CONFLICT", object({ operation: idempotentOperation, key_hash: ref("hash"), expected_fingerprint: ref("hash"), actual_fingerprint: ref("hash") })));
      branches.push(errorEnvelope("IDEMPOTENCY_PENDING", object({ operation: idempotentOperation, key_hash: ref("hash"), fingerprint: ref("hash"), invocation_id: { type: "string", pattern: "^[0-9a-f]{32}$" }, temporary_message_ids: { type: "array" } })));
      branches.push(errorEnvelope("IDEMPOTENCY_UNAVAILABLE", object({ account: ref("account"), path: { type: "string" }, reason: { type: "string" } })));
    }
  }
  const resolverNotFound = [
    object({ selector: { type: "string" } }),
    object({ selector: { type: "string" }, scope: { const: "local_materialized" } }),
  ];
  const notFound = filename === "storage.error.schema.json" ? [] : [...resolverNotFound];
  if (filename === "folder.error.schema.json") {
    notFound.push(object({ operation: { enum: ["folder_show", "folder_edit", "folder_delete", "folder_add_chat", "folder_remove_chat"] }, folder_id: ref("positiveInt32") }));
  } else if (filename === "topic.error.schema.json") {
    notFound.push(object({ operation: { enum: ["topic_edit", "topic_close", "topic_reopen"] }, chat_id: ref("int53"), topic_id: ref("positiveInt32") }));
  } else if (filename === "chat-admin.error.schema.json") {
    notFound.push(object({ operation, chat_id: ref("int53"), user_id: ref("positiveInt53") }));
    notFound.push(object({ operation: { const: "chat_set_photo" }, path: { type: "string" }, reason: { enum: ["missing", "symlink", "wrong_type", "empty", "unreadable"] } }));
  }
  if (notFound.length) branches.push(errorEnvelope("NOT_FOUND", { oneOf: notFound }));
  if (filename !== "storage.error.schema.json") {
    branches.push(errorEnvelope("AMBIGUOUS", { oneOf: [
      object({ selector: { type: "string" }, scope: { enum: ["active_dialogs", "local_materialized"] }, candidates: array(ref("chat"), 20), truncated: { type: "boolean" } }),
      object({ selector: { type: "string" }, candidates: array(ref("user"), 20), truncated: { type: "boolean" } }),
    ] }));
  }
  if (filename === "folder.error.schema.json") {
    branches.push(errorEnvelope("PRECONDITION_FAILED", object({ operation: { enum: ["folder_edit", "folder_add_chat", "folder_remove_chat"] }, folder_id: ref("positiveInt32"), chat_id: nullable(ref("int53")), reason: { enum: ["no_change", "already_in_folder", "not_in_folder", "folder_capacity"] } })));
  } else if (filename === "topic.error.schema.json") {
    branches.push(errorEnvelope("PRECONDITION_FAILED", object({ operation: { enum: ["topic_create", "topic_edit", "topic_close", "topic_reopen"] }, chat_id: ref("int53"), topic_id: nullable(ref("positiveInt32")), reason: { enum: ["missing_right", "no_change", "already_closed", "already_open"] } })));
    branches.push(errorEnvelope("PAGINATION_INVALID", object({ operation: { const: "topic_list" }, reason: { const: "non_advancing_upstream" } })));
  } else if (filename === "chat-admin.error.schema.json") {
    branches.push(errorEnvelope("PRECONDITION_FAILED", { oneOf: [
      object({ operation, chat_id: ref("int53"), reason: { const: "missing_right" }, right: { enum: adminRights } }),
      object({ operation, chat_id: ref("int53"), user_id: ref("positiveInt53"), reason: { enum: ["self_target", "creator", "noneditable_administrator", "wrong_member_state"] } }),
    ] }));
    branches.push(errorEnvelope("INPUT_CHANGED", object({ operation: { const: "chat_set_photo" }, path: { type: "string" } })));
  }
  write(filename, {
    $schema: "https://json-schema.org/draft/2020-12/schema",
    title: `tgcli ${filename.replace(".error.schema.json", "")} family error`,
    $defs: definitions,
    type: "object",
    additionalProperties: false,
    required: ["error"],
    properties: { error: { oneOf: branches } },
  });
}

const resultManifestPath = path.join(directory, "manifest.json");
const resultManifest = JSON.parse(fs.readFileSync(resultManifestPath, "utf8"));
for (const [stem, command] of operations) {
  resultManifest.commands[command] = { result: `${stem}.result.schema.json` };
}
const byKey = ([left], [right]) => left < right ? -1 : left > right ? 1 : 0;
resultManifest.commands = Object.fromEntries(Object.entries(resultManifest.commands).sort(byKey));
write("manifest.json", resultManifest);

const errorManifestPath = path.join(directory, "error-manifest.json");
const errorManifest = JSON.parse(fs.readFileSync(errorManifestPath, "utf8"));
for (const [filename, family] of Object.entries(familyOperations)) {
  for (const operation of family) {
    const identity = operations.find((entry) => entry[2] === operation);
    if (!identity) throw new Error(`missing command identity for ${operation}`);
    errorManifest.commands[identity[1]] = { error: filename };
  }
}
errorManifest.commands = Object.fromEntries(Object.entries(errorManifest.commands).sort(byKey));
write("error-manifest.json", errorManifest);
