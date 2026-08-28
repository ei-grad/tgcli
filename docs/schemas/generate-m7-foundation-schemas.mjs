#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const futureDirectory = path.join(directory, "future");
const dialect = "https://json-schema.org/draft/2020-12/schema";
const ref = (name) => ({ $ref: `#/$defs/${name}` });
const object = (properties, required = Object.keys(properties), extra = {}) => ({
  type: "object",
  additionalProperties: false,
  required,
  properties,
  ...extra,
});
const array = (items, maximum, minimum = 0, unique = false) => ({
  type: "array",
  minItems: minimum,
  maxItems: maximum,
  ...(unique ? { uniqueItems: true } : {}),
  items,
});
const nullable = (schema) => ({ oneOf: [{ type: "null" }, schema] });
const exactInteger = (decimal) => ({ __tgcli_exact_integer__: decimal });
const error = (code, details, message = { type: "string" }) =>
  object({ code: { const: code }, message, details });
const serialize = (document) => {
  const markers = [];
  const encoded = JSON.stringify(
    document,
    (_key, value) => {
      if (
        value !== null &&
        typeof value === "object" &&
        Object.keys(value).length === 1 &&
        typeof value.__tgcli_exact_integer__ === "string"
      ) {
        const decimal = value.__tgcli_exact_integer__;
        if (!/^(?:0|[1-9][0-9]*)$/.test(decimal)) {
          throw new Error(`invalid exact integer: ${decimal}`);
        }
        const marker = `__TGCLI_EXACT_INTEGER_${markers.length}__`;
        markers.push({ marker, decimal });
        return marker;
      }
      return value;
    },
    2,
  );
  return markers.reduce(
    (output, { marker, decimal }) => output.replace(`"${marker}"`, decimal),
    `${encoded}\n`,
  );
};
const write = (filename, document, root = directory) =>
  fs.writeFileSync(path.join(root, filename), serialize(document));

fs.mkdirSync(futureDirectory, { recursive: true });

const readSchema = JSON.parse(fs.readFileSync(path.join(directory, "read.result.schema.json")));
const messageDefinitions = structuredClone(readSchema.$defs);
const state = {
  enum: [
    "unknown",
    "wait_tdlib_parameters",
    "wait_phone_number",
    "wait_premium_purchase",
    "wait_email_address",
    "wait_email_code",
    "wait_code",
    "wait_other_device_confirmation",
    "wait_registration",
    "wait_password",
    "ready",
    "logging_out",
    "closing",
    "closed",
  ],
};
const account = { type: "string", pattern: "^[A-Za-z0-9_-]{1,32}$" };
const int53 = {
  type: "integer",
  minimum: -9007199254740991,
  maximum: 9007199254740991,
  not: { const: 0 },
};
const positiveInt53 = { type: "integer", minimum: 1, maximum: 9007199254740991 };
const int32 = { type: "integer", minimum: -2147483648, maximum: 2147483647 };
const positiveInt32 = { type: "integer", minimum: 1, maximum: 2147483647 };
const nonnegativeInt32 = { type: "integer", minimum: 0, maximum: 2147483647 };
const timestamp = {
  type: "string",
  pattern: "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$",
};
const hash = { type: "string", pattern: "^sha256:[0-9a-f]{64}$" };
const tdIdentifier = { type: "string", pattern: "^[A-Za-z][A-Za-z0-9]{0,127}$" };
const requestId = {
  type: "integer",
  minimum: 0,
  maximum: exactInteger("18446744073709551615"),
};
const uint64 = {
  type: "integer",
  minimum: 0,
  maximum: exactInteger("18446744073709551615"),
};
const topic = structuredClone(messageDefinitions.topic);
const chatIdentity = object({
  id: int53,
  title: { type: "string" },
  type: { enum: ["private", "basic_group", "supergroup", "channel"] },
  is_bot: { type: "boolean" },
  usernames: array({ type: "string", minLength: 1 }, 100),
});
const userIdentity = object({
  id: positiveInt53,
  display_name: { type: "string" },
  usernames: array({ type: "string", minLength: 1 }, 100),
  is_bot: { type: "boolean" },
});

const usage = (reasons) =>
  error(
    "USAGE",
    object({ argument: nullable({ type: "string" }), reason: { enum: reasons } }),
  );
const notAuthed = error(
  "NOT_AUTHED",
  object({
    account,
    state,
    reason: { enum: ["not_ready", "authorization_lost", "login_required"] },
  }),
);
const operationSchema = (operation) =>
  typeof operation === "string" ? { const: operation } : structuredClone(operation);
const commonTransport = (operation, { includeBot = true } = {}) => [
  error("ACCOUNT_NOT_FOUND", object({ account })),
  error(
    "ACCOUNT_MISMATCH",
    object({ requested_account: account, daemon_account: account }),
  ),
  error(
    "CONFIG_INVALID",
    object({
      path: { type: "string" },
      reason: {
        enum: [
          "path_invalid",
          "wrong_owner",
          "wrong_type",
          "wrong_mode",
          "wrong_link_count",
          "too_large",
          "parse_error",
          "type_error",
          "invalid_default",
          "conflicting_credentials",
          "io_error",
          "sync_error",
        ],
      },
    }),
  ),
  ...(includeBot
    ? [error("BOT_UNSUPPORTED", object({ operation: operationSchema(operation) }))]
    : []),
  error(
    "TDLIB_ERROR",
    object({ operation: operationSchema(operation), tdlib_code: int32 }),
  ),
  error(
    "RATE_LIMITED",
    object({
      operation: operationSchema(operation),
      tdlib_code: { const: 429 },
      retry_after: nonnegativeInt32,
    }),
  ),
  error(
    "INTERNAL",
    object({
      operation: operationSchema(operation),
      reason: { enum: ["internal_error", "malformed_tdlib_response"] },
    }),
  ),
  error("DAEMON_SHUTDOWN", object({ reason: { const: "daemon_shutdown" } })),
  error(
    "PROTOCOL_ANSWER_INVALID",
    object({
      request_id: requestId,
      reason: {
        enum: [
          "future_sequence",
          "nonce_mismatch",
          "generation_mismatch",
          "malformed",
          "unknown_request",
        ],
      },
    }),
  ),
];
const resolverBranches = () => [
  error("NOT_FOUND", {
    oneOf: [
      object({ selector: { type: "string" } }),
      object({ selector: { type: "string" }, scope: { const: "local_materialized" } }),
    ],
  }),
  error(
    "AMBIGUOUS",
    object({
      selector: { type: "string" },
      scope: { enum: ["active_dialogs", "local_materialized"] },
      candidates: array(chatIdentity, 20),
      truncated: { type: "boolean" },
    }),
  ),
  error(
    "TDLIB_ERROR",
    object({ operation: { const: "resolve" }, tdlib_code: int32 }),
  ),
  error(
    "RATE_LIMITED",
    object({
      operation: { const: "resolve" },
      tdlib_code: { const: 429 },
      retry_after: nonnegativeInt32,
    }),
  ),
  error(
    "INTERNAL",
    object({ operation: { const: "resolve" }, reason: { const: "internal_error" } }),
  ),
];
const recoveryBranches = () => [
  error(
    "REMOVAL_INCOMPLETE",
    object({
      account,
      path: { type: "string" },
      invocation_id: { type: "string", pattern: "^[0-9a-f]{32}$" },
      stage: { type: "string", minLength: 1 },
      completed_stages: array({ type: "string", minLength: 1 }, 32, 0, true),
      reason: { type: "string", minLength: 1 },
    }),
  ),
  error(
    "AUDIT_UNAVAILABLE",
    object({ account, path: { type: "string" }, reason: { type: "string", minLength: 1 } }),
  ),
  error(
    "AUDIT_INCOMPLETE",
    object({
      account,
      path: { type: "string" },
      mutation_state: { enum: ["none", "possible", "confirmed"] },
      completed_stages: array({ type: "string", minLength: 1 }, 32, 0, true),
    }),
  ),
];
const readyTimeout = (operation) =>
  error("TIMEOUT", object({ operation: operationSchema(operation), state: nullable(state) }));
const configAdmissionTimeout = error(
  "TIMEOUT",
  object({ operation: { const: "config_admission" }, state: { type: "null" } }),
);

const currentSchemas = {
  "chats.error.schema.json": {
    operation: "chats",
    reasons: [
      "missing_argument",
      "invalid_argument",
      "mutually_exclusive",
      "unknown_command",
      "unsupported_mode",
      "invalid_cursor",
      "cursor_scope_mismatch",
    ],
    extra: [
      error("NOT_FOUND", object({ folder_id: positiveInt32 })),
      error(
        "PAGINATION_INVALID",
        object({ operation: { const: "chats" }, reason: { const: "non_advancing_upstream" } }),
      ),
    ],
    recovery: true,
  },
  "unread.error.schema.json": {
    operation: "unread",
    reasons: ["missing_argument", "invalid_argument", "unknown_command", "unsupported_mode"],
    extra: [],
    recovery: false,
  },
  "read.error.schema.json": {
    operation: "read",
    reasons: [
      "missing_argument",
      "invalid_argument",
      "mutually_exclusive",
      "unknown_command",
      "unsupported_mode",
      "invalid_cursor",
      "cursor_scope_mismatch",
      "unsupported_chat_type",
    ],
    extra: [
      ...resolverBranches(),
      error("NOT_FOUND", object({ chat_id: int53, topic })),
      error(
        "PAGINATION_INVALID",
        object({ operation: { const: "read" }, reason: { const: "non_advancing_upstream" } }),
      ),
    ],
    recovery: false,
  },
  "msg-get.error.schema.json": {
    operation: "msg_get",
    reasons: ["missing_argument", "invalid_argument", "unknown_command", "unsupported_mode"],
    extra: [
      ...resolverBranches(),
      error(
        "NOT_FOUND",
        object({ chat_id: int53, missing_ids: array(int53, 100, 1, true) }),
      ),
    ],
    recovery: true,
  },
  "msg-link.error.schema.json": {
    operation: "msg_link",
    reasons: ["missing_argument", "invalid_argument", "unknown_command", "unsupported_mode"],
    extra: [
      ...resolverBranches(),
      error("NOT_FOUND", object({ chat_id: int53, message_id: int53 })),
    ],
    recovery: true,
  },
  "fetch.error.schema.json": {
    operation: "fetch",
    reasons: [
      "missing_argument",
      "invalid_argument",
      "mutually_exclusive",
      "unknown_command",
      "unsupported_mode",
    ],
    extra: [
      ...resolverBranches(),
      error(
        "PAGINATION_INVALID",
        object({ operation: { const: "fetch" }, reason: { const: "non_advancing_upstream" } }),
      ),
      error(
        "TIMEOUT",
        object({
          operation: { const: "fetch" },
          chat_id: int53,
          phase: { enum: ["local_scan", "network_fill"] },
          state: nullable(state),
          cached_count: { type: "integer", minimum: 0, maximum: 1000000 },
          oldest_message_id: nullable(int53),
          resume_from_message_id: nullable(int53),
        }),
      ),
    ],
    recovery: true,
  },
};

for (const [filename, contract] of Object.entries(currentSchemas)) {
  const branches = [
    usage(contract.reasons),
    notAuthed,
    ...commonTransport(contract.operation),
    configAdmissionTimeout,
    readyTimeout(contract.operation),
    ...contract.extra,
    ...(contract.recovery ? recoveryBranches() : []),
  ];
  write(filename, {
    $schema: dialect,
    title: `tgcli ${contract.operation} error`,
    $defs: messageDefinitions,
    type: "object",
    additionalProperties: false,
    required: ["error"],
    properties: { error: { oneOf: branches } },
  });
}

const manifestPath = path.join(directory, "error-manifest.json");
const errorManifest = JSON.parse(fs.readFileSync(manifestPath));
for (const [command, filename] of [
  ["chats", "chats.error.schema.json"],
  ["unread", "unread.error.schema.json"],
  ["read", "read.error.schema.json"],
  ["msg get", "msg-get.error.schema.json"],
  ["msg link", "msg-link.error.schema.json"],
  ["fetch", "fetch.error.schema.json"],
]) {
  errorManifest.commands[command] = { error: filename };
}
errorManifest.commands = Object.fromEntries(
  Object.entries(errorManifest.commands).sort(([left], [right]) => left.localeCompare(right)),
);
write("error-manifest.json", errorManifest);

write(
  "search.result.schema.json",
  {
    $schema: dialect,
    title: "tgcli search result",
    $defs: messageDefinitions,
    ...object({
      items: array(ref("message"), 100),
      next: nullable({ type: "string", minLength: 1 }),
    }),
  },
  futureDirectory,
);

const chatInfoProperties = {
  id: int53,
  title: { type: "string" },
  type: { enum: ["private", "basic_group", "supergroup", "channel"] },
  is_bot: { type: "boolean" },
  usernames: array({ type: "string", minLength: 1 }, 100),
  description: { type: "string" },
  member_count: nullable(nonnegativeInt32),
  is_forum: { type: "boolean" },
  linked_chat_id: nullable(int53),
  is_archived: { type: "boolean" },
  folder_ids: array(positiveInt32, 100, 0, true),
  is_marked_unread: { type: "boolean" },
  unread_count: nonnegativeInt32,
  unread_mention_count: nonnegativeInt32,
  unread_reaction_count: nonnegativeInt32,
  unread_poll_vote_count: nonnegativeInt32,
};
const chatInfoBranch = (type, overrides) =>
  object({ ...structuredClone(chatInfoProperties), type: { const: type }, ...overrides });
write(
  "chat-info.result.schema.json",
  {
    $schema: dialect,
    title: "tgcli chat info result",
    oneOf: [
      chatInfoBranch("private", {
        member_count: { type: "null" },
        is_forum: { const: false },
        linked_chat_id: { type: "null" },
      }),
      chatInfoBranch("basic_group", {
        is_bot: { const: false },
        member_count: nonnegativeInt32,
        is_forum: { const: false },
        linked_chat_id: { type: "null" },
      }),
      chatInfoBranch("supergroup", { is_bot: { const: false } }),
      chatInfoBranch("channel", { is_bot: { const: false }, is_forum: { const: false } }),
    ],
  },
  futureDirectory,
);

const memberProperties = {
  display_name: { type: "string" },
  usernames: array({ type: "string", minLength: 1 }, 100),
  status: { enum: ["creator", "administrator", "member", "restricted", "left", "banned"] },
  tag: { type: "string" },
  joined_at: nullable(timestamp),
};
const member = {
  oneOf: [
    object({
      sender: object({ type: { const: "user" }, id: positiveInt53 }),
      is_bot: { type: "boolean" },
      ...memberProperties,
    }),
    object({
      sender: object({ type: { const: "chat" }, id: int53 }),
      is_bot: { const: false },
      ...memberProperties,
    }),
  ],
};
write(
  "chat-members.result.schema.json",
  {
    $schema: dialect,
    title: "tgcli chat members result",
    ...object({ items: array(member, 200), next: nullable({ type: "string", minLength: 1 }) }),
  },
  futureDirectory,
);

write(
  "download.result.schema.json",
  {
    $schema: dialect,
    title: "tgcli download result",
    ...object({
      chat_id: int53,
      message_id: int53,
      file_id: positiveInt32,
      media_type: {
        enum: ["animation", "audio", "document", "photo", "sticker", "video", "video_note", "voice_note"],
      },
      path: { type: "string", minLength: 1, pattern: "^/" },
      bytes: uint64,
    }),
  },
  futureDirectory,
);

const rawLive = {
  type: "object",
  required: ["@type"],
  properties: {
    "@type": tdIdentifier,
    "@extra": false,
    "@client_id": false,
  },
  additionalProperties: true,
};
const rawDry = object({
  dry_run: { const: true },
  plan: object({
    operation: { const: "raw" },
    function: tdIdentifier,
    tier: { enum: ["write", "destructive"] },
    tdlib_sha: { const: "a17f87c4cff7b90b278d12b91ba0614383aaee82" },
    request_sha256: hash,
    request_bytes: { type: "integer", minimum: 2, maximum: 1048576 },
  }),
});
write(
  "raw.result.schema.json",
  { $schema: dialect, title: "tgcli raw result", oneOf: [rawLive, rawDry] },
  futureDirectory,
);

const futureErrorDocument = (title, branches) => ({
  $schema: dialect,
  title,
  type: "object",
  additionalProperties: false,
  required: ["error"],
  properties: { error: { oneOf: branches } },
});
const futureCommon = (operation) => [
  usage([
    "missing_argument",
    "invalid_argument",
    "mutually_exclusive",
    "unknown_command",
    "unsupported_mode",
    "invalid_cursor",
    "cursor_scope_mismatch",
    "unsupported_chat_type",
  ]),
  notAuthed,
  ...commonTransport(operation),
  configAdmissionTimeout,
  readyTimeout(operation),
  ...recoveryBranches(),
];
write(
  "search.error.schema.json",
  futureErrorDocument("tgcli search error", [
    ...futureCommon("search"),
    ...resolverBranches(),
    error(
      "PAGINATION_INVALID",
      object({
        operation: { const: "search" },
        reason: {
          enum: ["invalid_cursor", "scope_changed", "source_changed", "marker_not_advancing", "page_invalid"],
        },
      }),
    ),
    error(
      "RESOURCE_LIMIT",
      object({
        operation: { const: "search" },
        resource: { enum: ["items", "bytes", "item_bytes"] },
        limit: { type: "integer", minimum: 1 },
      }),
    ),
  ]),
  futureDirectory,
);
const chatReadOperation = { enum: ["chat_info", "chat_members"] };
write(
  "chat-read.error.schema.json",
  futureErrorDocument("tgcli chat read error", [
    usage(["missing_argument", "invalid_argument", "mutually_exclusive", "unknown_command", "unsupported_mode", "invalid_cursor", "cursor_scope_mismatch", "unsupported_chat_type"]),
    notAuthed,
    ...commonTransport(chatReadOperation, { includeBot: false }),
    configAdmissionTimeout,
    ...recoveryBranches(),
    error("TIMEOUT", object({ operation: chatReadOperation, state: nullable(state) })),
    error("INTERNAL", object({ operation: chatReadOperation, reason: { const: "source_changed" } })),
    error("PAGINATION_INVALID", object({ operation: { const: "chat_members" }, reason: { enum: ["invalid_cursor", "scope_changed", "source_changed", "marker_not_advancing", "page_invalid"] } })),
    ...resolverBranches(),
  ]),
  futureDirectory,
);
write(
  "download.error.schema.json",
  futureErrorDocument("tgcli download error", [
    usage(["missing_argument", "invalid_argument", "mutually_exclusive", "unknown_command", "unsupported_mode"]),
    notAuthed,
    ...commonTransport("download", { includeBot: false }),
    configAdmissionTimeout,
    ...recoveryBranches(),
    error("TIMEOUT", object({ operation: { const: "download" }, state: nullable(state) })),
    error("NOT_FOUND", object({ chat_id: int53, message_id: int53 })),
    error("PRECONDITION_FAILED", object({ operation: { const: "download" }, chat_id: int53, message_id: int53, reason: { enum: ["album_unsupported", "paid_media_unsupported", "web_page_unsupported", "expired_media", "unsupported_media"] } })),
    error("OUTPUT_EXISTS", object({ operation: { const: "download" }, path: { type: "string", minLength: 1, pattern: "^/" } })),
    error("OUTPUT_UNAVAILABLE", object({ operation: { const: "download" }, path: { type: "string", minLength: 1, pattern: "^/" }, reason: { enum: ["invalid_path", "open_failed", "write_failed", "sync_failed", "source_changed", "cleanup_failed"] } })),
    ...resolverBranches(),
  ]),
  futureDirectory,
);
write(
  "raw.error.schema.json",
  futureErrorDocument("tgcli raw error", [
    usage(["missing_argument", "invalid_argument", "mutually_exclusive", "unknown_command", "unsupported_mode"]),
    notAuthed,
    ...commonTransport("raw", { includeBot: false }),
    configAdmissionTimeout,
    ...recoveryBranches(),
    error("DENIED", object({ operation: { const: "raw" }, function: { type: "string", minLength: 1 }, reason: { enum: ["function_denied", "principal_unsupported", "secret_chat_unsupported", "sensitive_input_unsupported", "write_grant_required"] } })),
    error("CONFIRMATION_REQUIRED", object({ account, action: { const: "raw" }, target: object({ function: { type: "string", minLength: 1 }, request_sha256: hash }) })),
    error("RATE_LIMITED", object({ operation: { const: "raw" }, function: { type: "string", minLength: 1 }, tdlib_code: { const: 429 }, retry_after: nonnegativeInt32 })),
    error("TDLIB_ERROR", object({ operation: { const: "raw" }, function: { type: "string", minLength: 1 }, tdlib_code: int32 })),
    error("TIMEOUT", object({ operation: { const: "raw" }, state: nullable(state) })),
    error(
      "RAW_OUTCOME_UNCONFIRMED",
      object({
        operation: { const: "raw" },
        function: tdIdentifier,
        request_sha256: hash,
        mutation_state: { const: "possible" },
      }),
      { const: "raw request outcome is unconfirmed" },
    ),
    error("INTERNAL", object({ operation: { const: "raw" }, reason: { enum: ["unexpected_response", "result_too_large", "canonicalization_failed", "policy_table_invalid"] } })),
  ]),
  futureDirectory,
);

const invocationId = { type: "string", pattern: "^[0-9a-f]{32}$" };
const tdlibSha = { type: "string", pattern: "^[0-9a-f]{40}$" };
const uint64String = {
  type: "string",
  pattern:
    "^([1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|184[0-3][0-9]{16}|1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|184467[0-3][0-9]{13}|1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|1844674407[0-2][0-9]{9}|18446744073[0-6][0-9]{8}|1844674407370[0-8][0-9]{6}|18446744073709[0-4][0-9]{5}|184467440737095[0-4][0-9]{4}|18446744073709550[0-9]{3}|18446744073709551[0-5][0-9]{2}|1844674407370955160[0-9]|1844674407370955161[0-5])$",
};
const auditCommon = (recordType) => ({
  schema_version: { const: 3 },
  record_type: { const: recordType },
  invocation_id: invocationId,
});
const responseType = tdIdentifier;

write(
  "raw-audit-intent.v3.schema.json",
  {
    $schema: dialect,
    title: "tgcli dormant raw audit v3 intent",
    ...object({
      ...auditCommon("raw_intent"),
      function: responseType,
      tier: { enum: ["write", "destructive"] },
      tdlib_sha: tdlibSha,
      request_sha256: hash,
      request_bytes: { type: "integer", minimum: 2, maximum: 1048576 },
    }),
  },
  futureDirectory,
);

const dispatchData = object({ dispatch_token: invocationId, generation: uint64String });
const responseData = (kind) =>
  object({
    dispatch_token: invocationId,
    generation: uint64String,
    kind: { const: kind },
    response_type:
      kind === "result"
        ? { allOf: [responseType, { not: { const: "error" } }] }
        : { const: "error" },
    td_error_code: kind === "result" ? { type: "null" } : int32,
    response_sha256: hash,
    response_bytes: { type: "integer", minimum: 2, maximum: 16777216 },
  });
const rawCheckpoint = (stage, data) =>
  object({ ...auditCommon("raw_checkpoint"), stage: { const: stage }, data });
write(
  "raw-audit-checkpoint.v3.schema.json",
  {
    $schema: dialect,
    title: "tgcli dormant raw audit v3 checkpoint",
    oneOf: [
      rawCheckpoint("raw_dispatch_started", dispatchData),
      rawCheckpoint("raw_response_received", {
        oneOf: [responseData("result"), responseData("error")],
      }),
    ],
  },
  futureDirectory,
);

const resultTerminal = object({
  kind: { const: "result_digest" },
  response_type: { allOf: [responseType, { not: { const: "error" } }] },
  response_sha256: hash,
  response_bytes: { type: "integer", minimum: 2, maximum: 16777216 },
});
const errorTerminal = {
  oneOf: [
    object({
      kind: { const: "error_summary" },
      code: { const: "RATE_LIMITED" },
      td_error_code: { const: 429 },
    }),
    object({
      kind: { const: "error_summary" },
      code: { const: "TDLIB_ERROR" },
      td_error_code: int32,
    }),
    object({
      kind: { const: "error_summary" },
      code: { const: "RAW_OUTCOME_UNCONFIRMED" },
      td_error_code: { type: "null" },
    }),
  ],
};
write(
  "raw-audit-outcome.v3.schema.json",
  {
    $schema: dialect,
    title: "tgcli dormant raw audit v3 outcome",
    oneOf: [
      object({
        ...auditCommon("raw_outcome"),
        mutation_state: { const: "confirmed" },
        terminal: resultTerminal,
      }),
      object({
        ...auditCommon("raw_outcome"),
        mutation_state: { const: "possible" },
        terminal: errorTerminal,
      }),
    ],
  },
  futureDirectory,
);
