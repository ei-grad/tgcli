import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = dirname(fileURLToPath(import.meta.url));
const check = process.argv.includes('--check');
const dialect = 'https://json-schema.org/draft/2020-12/schema';
const uint64Maximum = 18446744073709551615n;
const uint64Marker = '__TGCLI_UINT64_MAXIMUM__';
const int64Minimum = -9223372036854775808n;
const int64Maximum = 9223372036854775807n;
const int64MinimumMarker = '__TGCLI_INT64_MINIMUM__';
const int64MaximumMarker = '__TGCLI_INT64_MAXIMUM__';

const object = (required, properties) => ({
  type: 'object',
  additionalProperties: false,
  required,
  properties,
});
const nullable = (schema) => ({ oneOf: [schema, { type: 'null' }] });
const relation = (fields, constrained) =>
  object(
    fields,
    Object.fromEntries(fields.map((field) => [field, constrained[field] ?? true])),
  );
const constrained = (base, branches) => ({ allOf: [base, { oneOf: branches }] });
const reference = (name) => ({ $ref: `#/$defs/${name}` });
const utf8String = ({ minScalars = 0, maxScalars, maxBytes, pattern } = {}) => ({
  type: 'string',
  ...(minScalars > 0 ? { minLength: minScalars } : {}),
  ...(maxScalars !== undefined ? { maxLength: maxScalars } : {}),
  ...(maxBytes !== undefined ? { 'x-tgcli-maxUtf8Bytes': maxBytes } : {}),
  ...(pattern !== undefined ? { pattern } : {}),
});
const noNulString = (limits = {}) => utf8String({ ...limits, pattern: '^[^\\u0000]*$' });
const nonemptyNoNulString = (limits = {}) =>
  utf8String({ minScalars: 1, ...limits, pattern: '^[^\\u0000]+$' });

const stateNames = [
  'unknown',
  'wait_tdlib_parameters',
  'wait_phone_number',
  'wait_premium_purchase',
  'wait_email_address',
  'wait_email_code',
  'wait_code',
  'wait_other_device_confirmation',
  'wait_registration',
  'wait_password',
  'ready',
  'logging_out',
  'closing',
  'closed',
];
const auditStages = [
  'planned',
  'intent_synced',
  'logout_send_started',
  'logout_closed_confirmed',
  'remote_logout_send_started',
  'remote_confirmed',
  'remote_not_present',
  'remote_kept',
  'client_close_started',
  'client_closed',
  'config_remove_started',
  'config_removed',
  'data_remove_started',
  'data_removed',
  'state_remove_started',
  'state_removed',
  'outcome_synced',
];
const v2Operations = [
  'send',
  'msg_edit',
  'msg_delete',
  'msg_forward',
  'msg_react',
  'msg_pin',
  'msg_unpin',
  'chat_mark_read',
  'chat_mute',
  'chat_unmute',
  'chat_pin',
  'chat_unpin',
  'chat_archive',
  'chat_unarchive',
  'chat_join',
  'chat_leave',
  'saved_attach',
  'session_terminate',
];
const v2Stages = [
  'idempotency_pending',
  'spool_ready',
  'dispatch_started',
  'temporary_ids_observed',
  'forward_progress',
  'mutation_confirmed',
];
const logoutStages = [
  ['intent_synced'],
  ['intent_synced', 'logout_send_started'],
  ['intent_synced', 'logout_send_started', 'logout_closed_confirmed'],
];
const removalBranches = [
  ['planned', 'intent_synced', 'remote_logout_send_started', 'remote_confirmed',
    'client_close_started', 'client_closed', 'config_remove_started', 'config_removed',
    'data_remove_started', 'data_removed', 'state_remove_started', 'state_removed'],
  ['planned', 'intent_synced', 'remote_logout_send_started', 'remote_not_present',
    'client_close_started', 'client_closed', 'config_remove_started', 'config_removed',
    'data_remove_started', 'data_removed', 'state_remove_started', 'state_removed'],
  ['planned', 'intent_synced', 'remote_not_present', 'client_close_started', 'client_closed',
    'config_remove_started', 'config_removed', 'data_remove_started', 'data_removed',
    'state_remove_started', 'state_removed'],
  ['planned', 'intent_synced', 'remote_kept', 'client_close_started', 'client_closed',
    'config_remove_started', 'config_removed', 'data_remove_started', 'data_removed',
    'state_remove_started', 'state_removed'],
];

const mutationState = (stages) => {
  const pairs = [
    ['logout_send_started', 'logout_closed_confirmed'],
    ['remote_logout_send_started', 'remote_confirmed'],
    ['config_remove_started', 'config_removed'],
    ['data_remove_started', 'data_removed'],
    ['state_remove_started', 'state_removed'],
  ];
  if (pairs.some(([started, completed]) => stages.includes(started) && !stages.includes(completed))) {
    return 'possible';
  }
  const confirmed = [
    'logout_closed_confirmed',
    'remote_confirmed',
    'config_removed',
    'data_removed',
    'state_removed',
  ];
  return confirmed.some((stage) => stages.includes(stage)) ? 'confirmed' : 'none';
};

const uniquePrefixes = (branches, minimum = 1) => {
  const prefixes = new Map();
  for (const branch of branches) {
    for (let length = minimum; length <= branch.length; ++length) {
      const prefix = branch.slice(0, length);
      prefixes.set(JSON.stringify(prefix), prefix);
    }
  }
  return [...prefixes.values()];
};

const snapshotPattern =
  '^sha256:[0-9a-f]{64};dev:(0|[1-9][0-9]*);ino:(0|[1-9][0-9]*);' +
  'size:(0|[1-9][0-9]*);ctime_ns:(0|[1-9][0-9]*)$';
const definitions = () => {
  const removalPlan = (keepSession) =>
    object(
      [
        'operation',
        'account',
        'remote_logout',
        'keep_session',
        'delete_paths',
        'config_path',
        'config_snapshot',
        'data_root',
        'state_root',
        'reassign_default',
      ],
      {
        operation: { const: 'account_remove' },
        account: reference('account'),
        remote_logout: { const: !keepSession },
        keep_session: { const: keepSession },
        delete_paths: {
          type: 'array',
          minItems: 2,
          maxItems: 2,
          items: { type: 'string', pattern: '^/' },
        },
        config_path: { type: 'string', pattern: '^/' },
        config_snapshot: reference('configSnapshot'),
        data_root: nullable(reference('rootIdentity')),
        state_root: nullable(reference('rootIdentity')),
        reassign_default: nullable(reference('account')),
      },
    );
  return {
    account: { type: 'string', pattern: '^[A-Za-z0-9_-]{1,32}$' },
    uint64: { type: 'integer', minimum: 0, maximum: uint64Maximum },
    state: { enum: stateNames },
    nullableState: nullable(reference('state')),
    auditStage: { enum: auditStages },
    invocationId: { type: 'string', pattern: '^[0-9a-f]{32}$' },
    timestamp: {
      type: 'string',
      pattern:
        '^(?!0000)[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])' +
        'T([01][0-9]|2[0-3]):[0-5][0-9]:([0-5][0-9]|60)(\\.[0-9]+)?Z$',
    },
    configSnapshot: { type: 'string', pattern: snapshotPattern },
    configSnapshotOrMissing: {
      type: 'string',
      pattern: `^(missing|${snapshotPattern.slice(1, -1)})$`,
    },
    rootIdentity: object(['path', 'device', 'inode', 'owner'], {
      path: { type: 'string', pattern: '^/' },
      device: reference('uint64'),
      inode: reference('uint64'),
      owner: reference('uint64'),
    }),
    logoutPlan: object(['operation', 'account', 'remote_logout', 'tdlib_request'], {
      operation: { const: 'logout' },
      account: reference('account'),
      remote_logout: { const: true },
      tdlib_request: { const: 'logOut' },
    }),
    remoteLogoutPlan: removalPlan(false),
    keepSessionPlan: removalPlan(true),
  };
};

const sessionIdMagnitude =
  '(?:[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[01][0-9]{17}|92[01][0-9]{16}|' +
  '922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|' +
  '922337[01][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|' +
  '9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|' +
  '922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|' +
  '92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|' +
  '9223372036854775[0-7][0-9]{2}|922337203685477580[0-7])';
const sessionIdNegativeMagnitude = sessionIdMagnitude.replace(
  '922337203685477580[0-7]',
  '922337203685477580[0-8]',
);
const v2Definitions = () => ({
  sha256: { type: 'string', pattern: '^sha256:[0-9a-f]{64}$' },
  hex32: { type: 'string', pattern: '^[0-9a-f]{32}$' },
  int53: {
    type: 'integer',
    minimum: -9007199254740991,
    maximum: 9007199254740991,
  },
  chatId: { allOf: [reference('int53'), { not: { const: 0 } }] },
  positiveInt53: { type: 'integer', minimum: 1, maximum: 9007199254740991 },
  int32: { type: 'integer', minimum: -2147483648, maximum: 2147483647 },
  positiveInt32: { type: 'integer', minimum: 1, maximum: 2147483647 },
  uint32: { type: 'integer', minimum: 0, maximum: 4294967295 },
  positiveUint32: { type: 'integer', minimum: 1, maximum: 4294967295 },
  int64: {
    type: 'integer',
    minimum: int64Minimum,
    maximum: int64Maximum,
  },
  unixSeconds: { type: 'integer', minimum: 0, maximum: 253402300799 },
  v2Timestamp: {
    type: 'string',
    pattern:
      '^(19[7-9][0-9]|[2-9][0-9]{3})-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])' +
      'T([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z$',
  },
  v2ConfigSnapshot: { type: 'string', pattern: snapshotPattern },
  v2Operation: { enum: v2Operations },
  v2Stage: { enum: v2Stages },
  chatIdentity: constrained(
    object(['id', 'title', 'type', 'is_bot', 'usernames'], {
      id: { allOf: [reference('int53'), { not: { const: 0 } }] },
      title: utf8String({ maxScalars: 1048576, maxBytes: 1048576 }),
      type: { enum: ['private', 'basic_group', 'supergroup', 'channel'] },
      is_bot: { type: 'boolean' },
      usernames: {
        type: 'array',
        maxItems: 100,
        items: {
          type: 'string', minLength: 1, maxLength: 32,
          'x-tgcli-maxUtf8Bytes': 32, pattern: '^[A-Za-z0-9_]+$',
        },
      },
    }),
    [
      relation(['id', 'title', 'type', 'is_bot', 'usernames'], {
        is_bot: { const: false },
      }),
      relation(['id', 'title', 'type', 'is_bot', 'usernames'], {
        is_bot: { const: true }, type: { const: 'private' },
      }),
    ],
  ),
  forumTopic: object(['kind', 'id'], {
    kind: { const: 'forum' },
    id: reference('positiveInt32'),
  }),
  savedTopic: object(['kind', 'id'], {
    kind: { const: 'saved' },
    id: reference('positiveInt53'),
  }),
  topicRef: {
    oneOf: [
      object(['kind', 'id'], { kind: { const: 'forum' }, id: reference('positiveInt32') }),
      ...['thread', 'direct', 'saved'].map((kind) =>
        object(['kind', 'id'], { kind: { const: kind }, id: reference('positiveInt53') }),
      ),
    ],
  },
  messageSender: {
    oneOf: [
      object(['type', 'id'], { type: { const: 'user' }, id: reference('positiveInt53') }),
      object(['type', 'id'], { type: { const: 'chat' }, id: reference('chatId') }),
    ],
  },
  messageWriteResult: object(
    ['id', 'chat_id', 'date', 'sender', 'is_outgoing', 'topic', 'type', 'text', 'scheduled'],
    {
      id: reference('positiveInt53'),
      chat_id: reference('chatId'),
      date: nullable(reference('v2Timestamp')),
      sender: reference('messageSender'),
      is_outgoing: { type: 'boolean' },
      topic: nullable(reference('topicRef')),
      type: { enum: ['text', 'photo', 'video', 'doc', 'voice', 'other'] },
      text: noNulString({ maxScalars: 4096, maxBytes: 16384 }),
      scheduled: { type: 'boolean' },
    },
  ),
  schedule: {
    oneOf: [
      object(['kind'], { kind: { const: 'online' } }),
      object(['kind', 'send_date'], {
        kind: { const: 'at' },
        send_date: reference('positiveInt32'),
      }),
    ],
  },
  fileSnapshot: object(
    ['path', 'name', 'size', 'sha256', 'device', 'inode', 'mtime_ns', 'ctime_ns'],
    {
      path: nonemptyNoNulString({ maxScalars: 16842751, maxBytes: 16842751 }),
      name: {
        ...utf8String({
          minScalars: 1, maxScalars: 255, maxBytes: 255,
          pattern: '^(?!\\.{1,2}$)[^/]+$',
        }),
        'x-tgcli-forbidControlScalars': true,
      },
      size: reference('uint64'),
      sha256: reference('sha256'),
      device: reference('uint64'),
      inode: reference('uint64'),
      mtime_ns: reference('int64'),
      ctime_ns: reference('int64'),
    },
  ),
  sessionId: {
    oneOf: [
      { const: '0' },
      { type: 'string', pattern: `^${sessionIdMagnitude}$` },
      { type: 'string', pattern: `^-${sessionIdNegativeMagnitude}$` },
    ],
  },
  sessionTarget: object(
    [
      'id',
      'is_current',
      'is_password_pending',
      'is_unconfirmed',
      'device_type',
      'application_name',
      'application_version',
      'device_model',
      'platform',
      'system_version',
      'last_active_date',
    ],
    {
      id: reference('sessionId'),
      is_current: { const: false },
      is_password_pending: { type: 'boolean' },
      is_unconfirmed: { type: 'boolean' },
      device_type: {
        enum: [
          'android', 'apple', 'brave', 'chrome', 'edge', 'firefox', 'ipad', 'iphone',
          'linux', 'mac', 'opera', 'safari', 'ubuntu', 'unknown', 'vivaldi', 'windows',
          'xbox',
        ],
      },
      application_name: utf8String({ maxScalars: 1048576, maxBytes: 1048576 }),
      application_version: utf8String({ maxScalars: 1048576, maxBytes: 1048576 }),
      device_model: utf8String({ maxScalars: 1048576, maxBytes: 1048576 }),
      platform: utf8String({ maxScalars: 1048576, maxBytes: 1048576 }),
      system_version: utf8String({ maxScalars: 1048576, maxBytes: 1048576 }),
      last_active_date: nullable(reference('v2Timestamp')),
    },
  ),
  forwardItem: {
    oneOf: [
      object(['source_id', 'status', 'temporary_message_id'], {
        source_id: reference('positiveInt53'),
        status: { const: 'pending' },
        temporary_message_id: reference('int53'),
      }),
      object(['source_id', 'status', 'message'], {
        source_id: reference('positiveInt53'),
        status: { const: 'sent' },
        message: reference('messageWriteResult'),
      }),
      ...['upstream_null', 'deleted_before_confirmation'].map((failureReason) =>
        object(['source_id', 'status', 'failure_reason', 'tdlib_code', 'retry_after'], {
          source_id: reference('positiveInt53'),
          status: { const: 'failed' },
          failure_reason: { const: failureReason },
          tdlib_code: { type: 'null' },
          retry_after: { type: 'null' },
        })),
      object(['source_id', 'status', 'failure_reason', 'tdlib_code', 'retry_after'], {
        source_id: reference('positiveInt53'),
        status: { const: 'failed' },
        failure_reason: { const: 'tdlib_error' },
        tdlib_code: { allOf: [reference('int32'), { not: { const: 429 } }] },
        retry_after: { type: 'null' },
      }),
      object(['source_id', 'status', 'failure_reason', 'tdlib_code', 'retry_after'], {
        source_id: reference('positiveInt53'),
        status: { const: 'failed' },
        failure_reason: { const: 'tdlib_error' },
        tdlib_code: { const: 429 },
        retry_after: reference('positiveInt32'),
      }),
    ],
  },
});
const auditDefinitions = () => ({ ...definitions(), ...v2Definitions() });

const int53Array = (minimum = 1) => ({
  type: 'array',
  minItems: minimum,
  maxItems: 100,
  uniqueItems: true,
  items: reference('int53'),
});
const positiveInt53Array = () => ({
  type: 'array',
  minItems: 1,
  maxItems: 100,
  uniqueItems: true,
  'x-tgcli-strictlyIncreasing': true,
  items: reference('positiveInt53'),
});
const v2Arguments = (operation) => {
  const string = nonemptyNoNulString({ maxScalars: 16842751, maxBytes: 16842751 });
  const byOperation = {
    send: object(['chat', 'text', 'parse_mode', 'reply_to', 'topic', 'silent', 'schedule'], {
      chat: string,
      text: nonemptyNoNulString({ maxScalars: 4096, maxBytes: 16384 }),
      parse_mode: { enum: ['plain', 'markdown_v2', 'html'] },
      reply_to: nullable(reference('positiveInt53')),
      topic: nullable(reference('forumTopic')),
      silent: { type: 'boolean' },
      schedule: nullable(reference('schedule')),
    }),
    msg_edit: object(['chat', 'message_id', 'text'], {
      chat: string,
      message_id: reference('positiveInt53'),
      text: nonemptyNoNulString({ maxScalars: 4096, maxBytes: 16384 }),
    }),
    msg_delete: object(['chat', 'message_ids', 'for_all'], {
      chat: string,
      message_ids: positiveInt53Array(),
      for_all: { type: 'boolean' },
    }),
    msg_forward: object(['from', 'to', 'message_ids', 'drop_author'], {
      from: string,
      to: string,
      message_ids: positiveInt53Array(),
      drop_author: { type: 'boolean' },
    }),
    msg_react: constrained(
      object(['chat', 'message_id', 'reaction', 'remove', 'big'], {
        chat: string,
        message_id: reference('positiveInt53'),
        reaction: nonemptyNoNulString({ maxScalars: 64, maxBytes: 64 }),
        remove: { type: 'boolean' },
        big: { type: 'boolean' },
      }),
      [
        relation(['chat', 'message_id', 'reaction', 'remove', 'big'], {
          remove: { const: false },
        }),
        relation(['chat', 'message_id', 'reaction', 'remove', 'big'], {
          remove: { const: true }, big: { const: false },
        }),
      ],
    ),
    msg_pin: object(['chat', 'message_id'], { chat: string, message_id: reference('positiveInt53') }),
    msg_unpin: object(['chat', 'message_id'], { chat: string, message_id: reference('positiveInt53') }),
    chat_mark_read: object(['chat'], { chat: string }),
    chat_mute: object(['chat', 'duration_seconds'], {
      chat: string, duration_seconds: { type: 'integer', minimum: 1, maximum: 31622400 },
    }),
    chat_unmute: object(['chat', 'duration_seconds'], {
      chat: string, duration_seconds: { const: 0 },
    }),
    chat_pin: object(['chat'], { chat: string }),
    chat_unpin: object(['chat'], { chat: string }),
    chat_archive: object(['chat'], { chat: string }),
    chat_unarchive: object(['chat'], { chat: string }),
    chat_join: {
      oneOf: [
        object(['source', 'username'], {
          source: { const: 'username' },
          username: string,
        }),
        object(['source', 'invite_link_sha256'], {
          source: { const: 'invite_link' },
          invite_link_sha256: reference('sha256'),
        }),
      ],
    },
    chat_leave: object(['chat'], { chat: string }),
    saved_attach: object(['message_id', 'path', 'caption'], {
      message_id: reference('positiveInt53'),
      path: string,
      caption: noNulString({ maxScalars: 1024, maxBytes: 4096 }),
    }),
    session_terminate: object(['session_id'], { session_id: reference('sessionId') }),
  };
  return byOperation[operation];
};

const v2Plan = (operation) => {
  const common = {
    operation: { const: operation },
    account: reference('account'),
  };
  const plan = (fields, properties) => object(
    ['operation', 'account', 'tdlib_request', ...fields],
    { ...common, ...properties },
  );
  const planRelation = (fields, properties) =>
    relation(['operation', 'account', 'tdlib_request', ...fields], properties);
  const chat = reference('chatIdentity');
  const byOperation = {
    send: constrained(
      plan(
        ['chat', 'text', 'parse_mode', 'reply_to', 'requested_topic', 'effective_topic', 'silent',
          'schedule', 'observed_server_unix_time'],
        {
        tdlib_request: { const: 'sendMessage' }, chat,
        text: nonemptyNoNulString({ maxScalars: 4096, maxBytes: 16384 }),
        parse_mode: { enum: ['plain', 'markdown_v2', 'html'] },
        reply_to: nullable(reference('positiveInt53')),
        requested_topic: nullable(reference('forumTopic')),
        effective_topic: nullable(reference('forumTopic')),
        silent: { type: 'boolean' }, schedule: nullable(reference('schedule')),
        observed_server_unix_time: nullable(reference('int64')),
        },
      ),
      [
        planRelation(
          ['chat', 'text', 'parse_mode', 'reply_to', 'requested_topic', 'effective_topic', 'silent',
            'schedule', 'observed_server_unix_time'],
          { schedule: { type: 'null' }, observed_server_unix_time: { type: 'null' } },
        ),
        planRelation(
          ['chat', 'text', 'parse_mode', 'reply_to', 'requested_topic', 'effective_topic', 'silent',
            'schedule', 'observed_server_unix_time'],
          {
            schedule: object(['kind'], { kind: { const: 'online' } }),
            observed_server_unix_time: { type: 'null' },
          },
        ),
        {
          ...planRelation(
            ['chat', 'text', 'parse_mode', 'reply_to', 'requested_topic', 'effective_topic', 'silent',
              'schedule', 'observed_server_unix_time'],
            {
            schedule: object(['kind', 'send_date'], {
              kind: { const: 'at' }, send_date: reference('positiveInt32'),
            }),
            observed_server_unix_time: reference('int64'),
            },
          ),
          'x-tgcli-serverWindow': { minimumLeadSeconds: 11, maximumLeadSeconds: 31622400 },
        },
      ],
    ),
    msg_edit: plan(['chat', 'message_id', 'text'], {
      tdlib_request: { const: 'editMessageText' }, chat,
      message_id: reference('positiveInt53'),
      text: nonemptyNoNulString({ maxScalars: 4096, maxBytes: 16384 }),
    }),
    msg_delete: plan(['chat', 'message_ids', 'requested_for_all', 'effective_for_all'], {
      tdlib_request: { const: 'deleteMessages' }, chat, message_ids: positiveInt53Array(),
      requested_for_all: { type: 'boolean' }, effective_for_all: { type: 'boolean' },
    }),
    msg_forward: plan(['from', 'to', 'message_ids', 'drop_author'], {
      tdlib_request: { const: 'forwardMessages' }, from: chat, to: chat,
      message_ids: positiveInt53Array(), drop_author: { type: 'boolean' },
    }),
    msg_react: constrained(
      plan(['chat', 'message_id', 'reaction', 'remove', 'big'], {
        tdlib_request: { enum: ['addMessageReaction', 'removeMessageReaction'] }, chat,
        message_id: reference('positiveInt53'),
        reaction: nonemptyNoNulString({ maxScalars: 64, maxBytes: 64 }),
        remove: { type: 'boolean' }, big: { type: 'boolean' },
      }),
      [
        planRelation(['chat', 'message_id', 'reaction', 'remove', 'big'], {
            remove: { const: false }, tdlib_request: { const: 'addMessageReaction' },
          }),
        planRelation(['chat', 'message_id', 'reaction', 'remove', 'big'], {
            remove: { const: true }, big: { const: false },
            tdlib_request: { const: 'removeMessageReaction' },
          }),
      ],
    ),
    msg_pin: plan(['chat', 'message_id', 'pinned'], {
      tdlib_request: { const: 'pinChatMessage' }, chat,
      message_id: reference('positiveInt53'), pinned: { const: true },
    }),
    msg_unpin: plan(['chat', 'message_id', 'pinned'], {
      tdlib_request: { const: 'unpinChatMessage' }, chat,
      message_id: reference('positiveInt53'), pinned: { const: false },
    }),
    chat_mark_read: constrained(
      plan(['chat', 'last_message_id'], {
        tdlib_request: { oneOf: [{ const: 'viewMessages' }, { type: 'null' }] }, chat,
        last_message_id: nullable(reference('positiveInt53')),
      }),
      [
        planRelation(['chat', 'last_message_id'], {
          tdlib_request: { type: 'null' }, last_message_id: { type: 'null' },
        }),
        planRelation(['chat', 'last_message_id'], {
            tdlib_request: { const: 'viewMessages' },
            last_message_id: reference('positiveInt53'),
          }),
      ],
    ),
    chat_mute: plan(['chat', 'muted', 'duration_seconds'], {
      tdlib_request: { const: 'setChatNotificationSettings' }, chat,
      muted: { const: true }, duration_seconds: { type: 'integer', minimum: 1, maximum: 31622400 },
    }),
    chat_unmute: plan(['chat', 'muted', 'duration_seconds'], {
      tdlib_request: { const: 'setChatNotificationSettings' }, chat,
      muted: { const: false }, duration_seconds: { const: 0 },
    }),
    chat_pin: plan(['chat', 'chat_list', 'pinned'], {
      tdlib_request: { const: 'toggleChatIsPinned' }, chat,
      chat_list: { enum: ['main', 'archive'] }, pinned: { const: true },
    }),
    chat_unpin: plan(['chat', 'chat_list', 'pinned'], {
      tdlib_request: { const: 'toggleChatIsPinned' }, chat,
      chat_list: { enum: ['main', 'archive'] }, pinned: { const: false },
    }),
    chat_archive: plan(['chat', 'archived'], {
      tdlib_request: { const: 'addChatToList' }, chat, archived: { const: true },
    }),
    chat_unarchive: plan(['chat', 'archived'], {
      tdlib_request: { const: 'addChatToList' }, chat, archived: { const: false },
    }),
    chat_join: constrained(
      plan(['source', 'chat', 'invite_link_sha256'], {
        tdlib_request: { enum: ['joinChat', 'joinChatByInviteLink'] },
        source: { enum: ['username', 'invite_link'] }, chat: nullable(chat),
        invite_link_sha256: nullable(reference('sha256')),
      }),
      [
        planRelation(['source', 'chat', 'invite_link_sha256'], {
            source: { const: 'username' }, tdlib_request: { const: 'joinChat' },
            chat, invite_link_sha256: { type: 'null' },
          }),
        planRelation(['source', 'chat', 'invite_link_sha256'], {
            source: { const: 'invite_link' }, tdlib_request: { const: 'joinChatByInviteLink' },
            chat: nullable(chat), invite_link_sha256: reference('sha256'),
          }),
      ],
    ),
    chat_leave: plan(['chat'], {
      tdlib_request: { const: 'leaveChat' },
      chat: { allOf: [chat, relation(['id', 'title', 'type', 'is_bot', 'usernames'], {
        type: { enum: ['basic_group', 'supergroup', 'channel'] },
      })] },
    }),
    saved_attach: plan(['chat', 'message_id', 'effective_topic', 'caption', 'file'], {
      tdlib_request: { const: 'sendMessage' }, chat,
      message_id: reference('positiveInt53'), effective_topic: nullable(reference('savedTopic')),
      caption: noNulString({ maxScalars: 1024, maxBytes: 4096 }),
      file: reference('fileSnapshot'),
    }),
    session_terminate: plan(['session'], {
      tdlib_request: { const: 'terminateSession' }, session: reference('sessionTarget'),
    }),
  };
  return byOperation[operation];
};

const v2ResultData = (operation) => {
  const byOperation = {
    send: reference('messageWriteResult'),
    msg_edit: reference('messageWriteResult'),
    saved_attach: reference('messageWriteResult'),
    msg_delete: object(['chat_id', 'message_ids', 'for_all', 'deleted'], {
      chat_id: reference('chatId'),
      message_ids: positiveInt53Array(),
      for_all: { type: 'boolean' },
      deleted: { const: true },
    }),
    msg_forward: object(['from_chat_id', 'to_chat_id', 'items'], {
      from_chat_id: reference('chatId'),
      to_chat_id: reference('chatId'),
      items: {
        type: 'array', minItems: 1, maxItems: 100,
        'x-tgcli-strictlyIncreasingField': 'source_id',
        items: {
          allOf: [
            reference('forwardItem'),
            object(['source_id', 'status', 'message'], {
              source_id: reference('positiveInt53'),
              status: { const: 'sent' },
              message: reference('messageWriteResult'),
            }),
          ],
        },
      },
    }),
    msg_react: object(['chat_id', 'message_id', 'reaction', 'removed', 'big'], {
      chat_id: reference('chatId'), message_id: reference('positiveInt53'),
      reaction: nonemptyNoNulString({ maxScalars: 64, maxBytes: 64 }),
      removed: { type: 'boolean' }, big: { type: 'boolean' },
    }),
    msg_pin: object(['chat_id', 'message_id', 'pinned'], {
      chat_id: reference('chatId'), message_id: reference('positiveInt53'),
      pinned: { const: true },
    }),
    msg_unpin: object(['chat_id', 'message_id', 'pinned'], {
      chat_id: reference('chatId'), message_id: reference('positiveInt53'),
      pinned: { const: false },
    }),
    chat_mark_read: object(['chat_id', 'last_read_message_id', 'marked_read'], {
      chat_id: reference('chatId'), last_read_message_id: nullable(reference('positiveInt53')),
      marked_read: { const: true },
    }),
    chat_mute: object(['chat_id', 'muted', 'duration_seconds'], {
      chat_id: reference('chatId'), muted: { const: true },
      duration_seconds: { type: 'integer', minimum: 1, maximum: 31622400 },
    }),
    chat_unmute: object(['chat_id', 'muted', 'duration_seconds'], {
      chat_id: reference('chatId'), muted: { const: false }, duration_seconds: { const: 0 },
    }),
    chat_pin: object(['chat_id', 'chat_list', 'pinned'], {
      chat_id: reference('chatId'), chat_list: { enum: ['main', 'archive'] },
      pinned: { const: true },
    }),
    chat_unpin: object(['chat_id', 'chat_list', 'pinned'], {
      chat_id: reference('chatId'), chat_list: { enum: ['main', 'archive'] },
      pinned: { const: false },
    }),
    chat_archive: object(['chat_id', 'archived'], {
      chat_id: reference('chatId'), archived: { const: true },
    }),
    chat_unarchive: object(['chat_id', 'archived'], {
      chat_id: reference('chatId'), archived: { const: false },
    }),
    chat_join: {
      oneOf: [
        object(['status', 'chat_id'], {
          status: { const: 'joined' }, chat_id: reference('chatId'),
        }),
        object(['status', 'chat_id'], {
          status: { const: 'request_sent' }, chat_id: nullable(reference('chatId')),
        }),
      ],
    },
    chat_leave: object(['chat_id', 'left'], {
      chat_id: reference('chatId'), left: { const: true },
    }),
    session_terminate: object(['session_id', 'terminated'], {
      session_id: reference('sessionId'), terminated: { const: true },
    }),
  };
  return byOperation[operation];
};

const v2ResultTerminal = (operation) =>
  object(['kind', 'data'], { kind: { const: 'result' }, data: v2ResultData(operation) });
const storedMessages = {
  AUDIT_INCOMPLETE: ['a prior audited invocation did not reach a terminal proof'],
  TIMEOUT: ['request timed out'],
  DAEMON_SHUTDOWN: ['daemon is shutting down'],
  NOT_AUTHED: ['authorization was lost'],
  TDLIB_ERROR: ['Telegram request failed'],
  RATE_LIMITED: ['Telegram rate limit exceeded'],
  INTERNAL: ['internal error', 'TDLib returned data outside the supported persistence bounds',
    'TDLib returned malformed session data'],
  SEND_FAILED: ['message was deleted before confirmation'],
  FORWARD_FAILED: ['messages could not be forwarded'],
  FORWARD_PARTIAL: ['some messages could not be forwarded'],
  JOIN_APPROVAL_REQUIRED: ['join request requires approval'],
  JOIN_DECLINED: ['join request was declined'],
  INPUT_CHANGED: ['input file changed while being read'],
  SPOOL_UNAVAILABLE: ['attachment spool is unavailable'],
  PRECONDITION_FAILED: ['operation precondition failed'],
};
const storedError = (code, details, exitCode, message) => ({
  code,
  schema: object(['kind', 'code', 'message', 'details', 'exit_code'], {
    kind: { const: 'error' }, code: { const: code },
    message: message ?? { enum: storedMessages[code] }, details,
    exit_code: { const: exitCode },
  }),
});
const operationDetails = (operation, fields, properties) =>
  object(['operation', ...fields], { operation: { const: operation }, ...properties });
const storedForwardItems = (minimum = 1) => ({
  type: 'array', minItems: minimum, maxItems: 100,
  'x-tgcli-strictlyIncreasingField': 'source_id', items: reference('forwardItem'),
});
const storedTimeoutDetails = (operation) => {
  if (operation === 'session_terminate') {
    return {
      oneOf: ['preflight', 'dispatch'].map((phase) =>
        operationDetails(operation, ['phase', 'state', 'outcome', 'idempotency'], {
          phase: { const: phase }, state: reference('nullableState'),
          outcome: { const: phase === 'preflight' ? 'not_started' : 'unknown' },
          idempotency: { const: 'not_requested' },
        })),
    };
  }
  const branches = [
    operationDetails(operation, ['phase', 'state', 'outcome', 'idempotency'], {
      phase: { const: 'preflight' }, state: reference('nullableState'),
      outcome: { const: 'not_started' },
      idempotency: { enum: ['not_requested', 'not_created', 'removed'] },
    }),
  ];
  if (['msg_delete', 'chat_leave'].includes(operation)) {
    branches.push(operationDetails(operation, ['phase', 'state', 'outcome', 'idempotency'], {
      phase: { const: 'replay_confirmation' }, state: reference('nullableState'),
      outcome: { const: 'not_started' }, idempotency: { const: 'completed_unchanged' },
    }));
  }
  if (!['send', 'saved_attach', 'msg_forward'].includes(operation)) {
    branches.push(operationDetails(operation, ['phase', 'state', 'outcome', 'idempotency'], {
      phase: { const: 'dispatch' }, state: reference('nullableState'),
      outcome: { const: 'unknown' }, idempotency: { enum: ['not_requested', 'pending'] },
    }));
  } else if (operation === 'msg_forward') {
    branches.push(operationDetails(
      operation, ['phase', 'state', 'outcome', 'idempotency', 'items'], {
        phase: { const: 'confirmation' }, state: reference('nullableState'),
        outcome: { const: 'unknown' }, idempotency: { enum: ['not_requested', 'pending'] },
        items: storedForwardItems(0),
      },
    ));
  } else {
    branches.push(operationDetails(
      operation, ['phase', 'state', 'outcome', 'idempotency', 'temporary_message_id'], {
        phase: { const: 'confirmation' }, state: reference('nullableState'),
        outcome: { const: 'unknown' }, idempotency: { enum: ['not_requested', 'pending'] },
        temporary_message_id: nullable(reference('int53')),
      },
    ));
  }
  return { oneOf: branches };
};
const preconditionReasons = {
  send: ['not_replyable', 'wrong_topic', 'online_schedule_unsupported',
    'schedule_window_elapsed', 'schedule_too_far'],
  msg_edit: ['not_editable', 'wrong_content_type', 'reply_markup_preservation_unsupported'],
  msg_delete: ['not_deletable_for_self', 'not_deletable_for_all'],
  msg_forward: ['not_forwardable', 'not_copyable'],
  msg_react: ['reaction_unavailable'],
  msg_pin: ['not_pinnable'],
  chat_mute: ['saved_notifications_unsupported'],
  chat_pin: ['chat_not_listed'],
  chat_unpin: ['chat_not_listed'],
  chat_leave: ['wrong_chat_type'],
  saved_attach: ['wrong_content_type', 'wrong_topic'],
};
const v2StoredErrorBranches = (operation) => {
  const branches = [
    storedError(
      'AUDIT_INCOMPLETE',
      object(['account', 'path', 'mutation_state', 'completed_stages'], {
        account: reference('account'), path: nonemptyNoNulString({ maxBytes: 16842751 }),
        mutation_state: { enum: ['none', 'possible', 'confirmed'] },
        completed_stages: {
          type: 'array', maxItems: 6, uniqueItems: true, items: reference('v2Stage'),
          'x-tgcli-legalStagePrefixFor': operation,
        },
      }),
      1,
      { const: 'a prior audited invocation did not reach a terminal proof' },
    ),
    storedError('TIMEOUT', storedTimeoutDetails(operation), 7),
    storedError('DAEMON_SHUTDOWN', object(['reason'], { reason: { const: 'daemon_shutdown' } }), 1),
    storedError('NOT_AUTHED', object(['account', 'state', 'reason'], {
      account: reference('account'), state: reference('state'),
      reason: { const: 'authorization_lost' },
    }), 3),
    storedError('TDLIB_ERROR', operationDetails(operation, ['tdlib_code'], {
      tdlib_code: reference('int32'),
    }), 1),
    storedError('INTERNAL', operation === 'session_terminate'
      ? operationDetails(operation, ['reason', 'tdlib_type_id'], {
        reason: { const: 'malformed_tdlib_response' },
        tdlib_type_id: nullable(reference('int32')),
      })
      : operationDetails(operation, ['reason'], { reason: { const: 'internal_error' } }), 1),
  ];
  if (operation === 'msg_forward') {
    const failed429 = object(
      ['source_id', 'status', 'failure_reason', 'tdlib_code', 'retry_after'],
      {
        source_id: reference('positiveInt53'), status: { const: 'failed' },
        failure_reason: { const: 'tdlib_error' }, tdlib_code: { const: 429 },
        retry_after: reference('positiveInt32'),
      },
    );
    branches.push(storedError('RATE_LIMITED', operationDetails(
      operation, ['tdlib_code', 'retry_after', 'items'], {
        tdlib_code: { const: 429 }, retry_after: reference('positiveInt32'),
        items: {
          type: 'array', minItems: 0, maxItems: 100,
          'x-tgcli-strictlyIncreasingField': 'source_id',
          'x-tgcli-retryAfterEqualsMaximum': true, items: failed429,
        },
      },
    ), 5));
    for (const code of ['FORWARD_FAILED', 'FORWARD_PARTIAL']) {
      branches.push(storedError(code, operationDetails(
        operation, ['from_chat_id', 'to_chat_id', 'items'], {
          from_chat_id: reference('chatId'), to_chat_id: reference('chatId'),
          items: { ...storedForwardItems(), 'x-tgcli-terminalClass': code },
        },
      ), 1));
    }
  } else {
    branches.push(storedError('RATE_LIMITED', operationDetails(
      operation, ['tdlib_code', 'retry_after'], {
        tdlib_code: { const: 429 },
        retry_after: operation === 'session_terminate'
          ? { type: 'integer', minimum: 0, maximum: 2147483647 }
          : reference('positiveInt32'),
      },
    ), 5));
  }
  if (['send', 'saved_attach'].includes(operation)) {
    branches.push(storedError('SEND_FAILED', operationDetails(
      operation, ['chat_id', 'temporary_message_id', 'reason'], {
        chat_id: reference('chatId'), temporary_message_id: reference('int53'),
        reason: { const: 'deleted_before_confirmation' },
      },
    ), 1));
  }
  if (operation === 'chat_join') {
    branches.push(storedError('JOIN_APPROVAL_REQUIRED', operationDetails(
      operation, ['bot_user_id', 'query_id'], {
        bot_user_id: reference('positiveInt53'), query_id: reference('positiveInt53'),
      },
    ), 1));
    branches.push(storedError('JOIN_DECLINED', operationDetails(operation, [], {}), 1));
  }
  if (operation === 'saved_attach') {
    branches.push(storedError('INPUT_CHANGED', operationDetails(operation, ['path'], {
      path: nonemptyNoNulString({ maxScalars: 4096, maxBytes: 4096 }),
    }), 1));
  }
  branches.push(storedError('SPOOL_UNAVAILABLE', operationDetails(
    operation, ['path', 'reason'], {
      path: nonemptyNoNulString({ maxScalars: 4096, maxBytes: 4096 }),
      reason: { enum: [
        'path_invalid', 'wrong_owner', 'wrong_type', 'wrong_mode', 'wrong_link_count',
        'too_large', 'capacity_exhausted', 'open_failed', 'lock_failed', 'read_failed',
        'write_failed', 'sync_failed', 'rename_failed', 'directory_sync_failed',
        'parse_error', 'schema_error', 'contradiction',
      ] },
    },
  ), 1));
  if (preconditionReasons[operation]) {
    branches.push(storedError('PRECONDITION_FAILED', operationDetails(
      operation, ['chat_id', 'message_id', 'reason'], {
        chat_id: nullable(reference('chatId')), message_id: nullable(reference('positiveInt53')),
        reason: { enum: preconditionReasons[operation] },
      },
    ), 1));
  }
  return branches;
};
const v2StoredErrorTerminal = (operation, allowedCodes) => ({
  oneOf: v2StoredErrorBranches(operation)
    .filter(({ code }) => allowedCodes === undefined || allowedCodes.includes(code))
    .map(({ schema: branch }) => branch),
});
const v2StoredTerminal = (operation) => ({
  oneOf: [v2ResultTerminal(operation), ...v2StoredErrorTerminal(operation).oneOf],
});
const v2MutationTerminal = (operation) => {
  const allowed = operation === 'msg_forward'
    ? ['FORWARD_PARTIAL', 'INTERNAL']
    : ['send', 'msg_edit', 'saved_attach'].includes(operation) ? ['INTERNAL'] : [];
  return {
    oneOf: [v2ResultTerminal(operation), ...v2StoredErrorTerminal(operation, allowed).oneOf],
  };
};

const detail = object;
const structuredError = (code, details) =>
  object(['code', 'details'], { code: { const: code }, details });
const errorEnvelope = (code, details) =>
  object(['error'], {
    error: object(['code', 'message', 'details'], {
      code: { const: code },
      message: { type: 'string' },
      details,
    }),
  });
const schema = (title, defs, body) => ({ $schema: dialect, title, $defs: defs, ...body });

const usageDetails = detail(['argument', 'reason'], {
  argument: { type: ['string', 'null'] },
  reason: {
    enum: [
      'missing_argument',
      'invalid_argument',
      'mutually_exclusive',
      'unknown_command',
      'invalid_environment',
      'unsupported_mode',
    ],
  },
});
const configDetails = detail(['path', 'reason'], {
  path: { type: 'string' },
  reason: {
    enum: [
      'path_invalid',
      'wrong_owner',
      'wrong_type',
      'wrong_mode',
      'wrong_link_count',
      'too_large',
      'parse_error',
      'type_error',
      'invalid_default',
      'conflicting_credentials',
      'io_error',
      'sync_error',
    ],
  },
});
const protocolDetails = detail(['request_id', 'reason'], {
  request_id: reference('uint64'),
  reason: {
    enum: [
      'future_sequence',
      'nonce_mismatch',
      'generation_mismatch',
      'malformed',
      'unknown_request',
    ],
  },
});
const writeDetails = detail(['account', 'reason'], {
  account: reference('account'),
  reason: { enum: ['explicit_deny', 'no_grant', 'invalid_config_grant'] },
});
const auditUnavailableDetails = detail(['account', 'path', 'reason'], {
  account: reference('account'),
  path: { type: 'string' },
  reason: { enum: ['path_invalid', 'open_failed', 'write_failed', 'sync_failed', 'rotate_failed'] },
});
const remoteUnconfirmedDetails = detail(['account', 'state', 'reason'], {
  account: reference('account'),
  state: reference('state'),
  reason: {
    enum: ['tdlib_error', 'timeout', 'generation_lost', 'transport_lost', 'state_unproven'],
  },
});
const shutdownDetails = detail(['reason'], { reason: { const: 'daemon_shutdown' } });
const logoutConfirmationDetails = detail(['account', 'action', 'target'], {
  account: reference('account'),
  action: { const: 'logout' },
  target: reference('logoutPlan'),
});
const removalConfirmationDetails = detail(['account', 'action', 'target'], {
  account: reference('account'),
  action: { const: 'account_remove' },
  target: { oneOf: [reference('remoteLogoutPlan'), reference('keepSessionPlan')] },
});

const auditIncompleteFields = ['account', 'path', 'mutation_state', 'completed_stages'];
const auditIncompleteDetails = constrained(
  detail(auditIncompleteFields, {
    account: reference('account'),
    path: { type: 'string' },
    mutation_state: { enum: ['none', 'possible', 'confirmed'] },
    completed_stages: { enum: logoutStages },
  }),
  logoutStages.map((stages) =>
    relation(auditIncompleteFields, {
      mutation_state: { const: mutationState(stages) },
      completed_stages: { const: stages },
    }),
  ),
);

const removalIncompleteFields = [
  'account',
  'path',
  'invocation_id',
  'stage',
  'completed_stages',
  'reason',
];
const removalPrefixes = uniquePrefixes(removalBranches);
const prefixesByStage = new Map();
for (const stages of removalPrefixes) {
  const stage = stages.at(-1);
  const grouped = prefixesByStage.get(stage) ?? [];
  grouped.push(stages);
  prefixesByStage.set(stage, grouped);
}
const removalIncompleteDetails = constrained(
  detail(removalIncompleteFields, {
    account: reference('account'),
    path: { type: 'string' },
    invocation_id: reference('invocationId'),
    stage: reference('auditStage'),
    completed_stages: {
      type: 'array',
      minItems: 1,
      items: reference('auditStage'),
    },
    reason: { enum: ['prior_crash', 'identity_ambiguous', 'outcome_missing'] },
  }),
  [...prefixesByStage].map(([stage, prefixes]) =>
    relation(removalIncompleteFields, {
      stage: { const: stage },
      completed_stages: { enum: prefixes },
    }),
  ),
);

const logoutErrors = [
  ['USAGE', usageDetails],
  ['ACCOUNT_NOT_FOUND', detail(['account'], { account: reference('account') })],
  ['CONFIG_INVALID', configDetails],
  ['WRITE_DENIED', writeDetails],
  [
    'NOT_AUTHED',
    detail(['account', 'state', 'reason'], {
      account: reference('account'),
      state: reference('state'),
      reason: { enum: ['not_ready', 'authorization_lost', 'login_required'] },
    }),
  ],
  ['CONFIRMATION_REQUIRED', logoutConfirmationDetails],
  ['PROTOCOL_ANSWER_INVALID', protocolDetails],
  ['AUDIT_UNAVAILABLE', auditUnavailableDetails],
  ['AUDIT_INCOMPLETE', auditIncompleteDetails],
  ['REMOTE_LOGOUT_UNCONFIRMED', remoteUnconfirmedDetails],
  [
    'AUTH_FUNCTION_DENIED',
    detail(['account', 'state', 'function'], {
      account: reference('account'),
      state: reference('state'),
      function: { const: 'logOut' },
    }),
  ],
  [
    'RATE_LIMITED',
    detail(['operation', 'tdlib_code', 'retry_after'], {
      operation: { const: 'logout' },
      tdlib_code: { const: 429 },
      retry_after: { type: 'integer' },
    }),
  ],
  [
    'TDLIB_ERROR',
    detail(['operation', 'tdlib_code'], {
      operation: { const: 'logout' },
      tdlib_code: { type: 'integer' },
    }),
  ],
  [
    'TIMEOUT',
    detail(['operation', 'state'], {
      operation: { enum: ['logout', 'audit'] },
      state: reference('nullableState'),
    }),
  ],
  ['DAEMON_SHUTDOWN', shutdownDetails],
  [
    'INTERNAL',
    detail(['operation', 'reason'], {
      operation: { enum: ['logout', 'audit'] },
      reason: { const: 'internal_error' },
    }),
  ],
];

const removalErrors = [
  ['USAGE', usageDetails],
  ['ACCOUNT_NOT_FOUND', detail(['account'], { account: reference('account') })],
  [
    'ACCOUNT_MISMATCH',
    detail(['requested_account', 'daemon_account'], {
      requested_account: reference('account'),
      daemon_account: reference('account'),
    }),
  ],
  [
    'DEFAULT_REASSIGNMENT_REQUIRED',
    detail(['account', 'candidates'], {
      account: reference('account'),
      candidates: { type: 'array', uniqueItems: true, items: reference('account') },
    }),
  ],
  ['CONFIG_INVALID', configDetails],
  [
    'CONFIG_CONFLICT',
    detail(['path', 'expected', 'current'], {
      path: { type: 'string' },
      expected: reference('configSnapshot'),
      current: reference('configSnapshotOrMissing'),
    }),
  ],
  ['WRITE_DENIED', writeDetails],
  ['CONFIRMATION_REQUIRED', removalConfirmationDetails],
  ['PROTOCOL_ANSWER_INVALID', protocolDetails],
  ['AUDIT_UNAVAILABLE', auditUnavailableDetails],
  ['REMOVAL_INCOMPLETE', removalIncompleteDetails],
  ['REMOTE_LOGOUT_UNCONFIRMED', remoteUnconfirmedDetails],
  [
    'LOCAL_CLEANUP_FAILED',
    detail(['account', 'reason', 'removed', 'retained'], {
      account: reference('account'),
      reason: { enum: ['path_changed', 'path_invalid', 'mount_boundary', 'io_error', 'sync_error'] },
      removed: { type: 'array', uniqueItems: true, items: { type: 'string' } },
      retained: { type: 'array', uniqueItems: true, items: { type: 'string' } },
    }),
  ],
  [
    'AUTH_FUNCTION_DENIED',
    detail(['account', 'state', 'function'], {
      account: reference('account'),
      state: reference('state'),
      function: { enum: ['logOut', 'close'] },
    }),
  ],
  [
    'DAEMON_CONTROL_FAILED',
    detail(['account', 'operation', 'reason'], {
      account: reference('account'),
      operation: { const: 'stop' },
      reason: {
        enum: [
          'surface_invalid',
          'identity_changed',
          'handshake_failed',
          'shutdown_failed',
          'replacement_failed',
        ],
      },
    }),
  ],
  [
    'RATE_LIMITED',
    detail(['operation', 'tdlib_code', 'retry_after'], {
      operation: { const: 'account_remove' },
      tdlib_code: { const: 429 },
      retry_after: { type: 'integer' },
    }),
  ],
  [
    'TDLIB_ERROR',
    detail(['operation', 'tdlib_code'], {
      operation: { const: 'account_remove' },
      tdlib_code: { type: 'integer' },
    }),
  ],
  [
    'TIMEOUT',
    detail(['operation', 'state'], {
      operation: { const: 'account_remove' },
      state: reference('nullableState'),
    }),
  ],
  ['DAEMON_SHUTDOWN', shutdownDetails],
  [
    'INTERNAL',
    detail(['operation', 'reason'], {
      operation: { const: 'account_remove' },
      reason: { const: 'internal_error' },
    }),
  ],
];

const logoutPostIntentCodes = new Set([
  'AUTH_FUNCTION_DENIED',
  'REMOTE_LOGOUT_UNCONFIRMED',
  'RATE_LIMITED',
  'TDLIB_ERROR',
  'TIMEOUT',
  'DAEMON_SHUTDOWN',
  'INTERNAL',
]);
const removalPostIntentCodes = new Set([
  'AUTH_FUNCTION_DENIED',
  'CONFIG_INVALID',
  'CONFIG_CONFLICT',
  'REMOTE_LOGOUT_UNCONFIRMED',
  'LOCAL_CLEANUP_FAILED',
  'DAEMON_CONTROL_FAILED',
  'RATE_LIMITED',
  'TDLIB_ERROR',
  'TIMEOUT',
  'DAEMON_SHUTDOWN',
  'INTERNAL',
]);
const postIntentErrors = (errors, codes, command, policy = 'any') => ({
  oneOf: errors
    .filter(([code]) => codes.has(code))
    .map(([code, details]) => {
      if (code === 'TIMEOUT') {
        details = detail(['operation', 'state'], {
          operation: { const: command },
          state: reference('nullableState'),
        });
      }
      if (code === 'INTERNAL') {
        details = detail(['operation', 'reason'], {
          operation: { const: command },
          reason: { const: 'internal_error' },
        });
      }
      if (code === 'AUTH_FUNCTION_DENIED' && policy === 'keep') {
        details = detail(['account', 'state', 'function'], {
          account: reference('account'),
          state: reference('state'),
          function: { const: 'close' },
        });
      }
      return structuredError(code, details);
    }),
});

const logoutErrorSchema = schema('tgcli logout error', definitions(), {
  oneOf: logoutErrors.map(([code, details]) => errorEnvelope(code, details)),
});
const removalErrorSchema = schema('tgcli account remove error', definitions(), {
  oneOf: removalErrors.map(([code, details]) => errorEnvelope(code, details)),
});

const intentFields = [
  'schema_version',
  'phase',
  'invocation_id',
  'timestamp',
  'account',
  'command',
  'arguments',
  'plan',
  'config_snapshot',
  'authority_source',
  'confirmation_source',
];
const intentBase = {
  schema_version: { const: 1 },
  phase: { const: 'intent' },
  invocation_id: reference('invocationId'),
  timestamp: reference('timestamp'),
  account: reference('account'),
  authority_source: { enum: ['request', 'config'] },
  confirmation_source: { enum: ['yes', 'tty'] },
};
const v2IntentFields = [
  'schema_version',
  'phase',
  'invocation_id',
  'timestamp',
  'account',
  'command',
  'arguments',
  'plan',
  'request_fingerprint',
  'config_snapshot',
  'authority_source',
  'confirmation_source',
  'idempotency_key_hash',
];
const v2IntentBranches = v2Operations.map((operation) =>
  object(v2IntentFields, {
    schema_version: { const: 2 },
    phase: { const: 'intent' },
    invocation_id: reference('hex32'),
    timestamp: reference('v2Timestamp'),
    account: reference('account'),
    command: { const: operation },
    arguments: v2Arguments(operation),
    plan: v2Plan(operation),
    request_fingerprint: reference('sha256'),
    config_snapshot: reference('v2ConfigSnapshot'),
    authority_source: { enum: ['request', 'config'] },
    confirmation_source: ['msg_delete', 'chat_leave', 'session_terminate'].includes(operation)
      ? { enum: ['yes', 'tty'] }
      : { type: 'null' },
    idempotency_key_hash:
      operation === 'session_terminate' ? { type: 'null' } : nullable(reference('sha256')),
  }),
);
const intentSchema = schema('tgcli destructive audit intent', auditDefinitions(), {
  oneOf: [
    object(intentFields, {
      ...intentBase,
      command: { const: 'logout' },
      arguments: object([], {}),
      plan: reference('logoutPlan'),
      config_snapshot: reference('configSnapshotOrMissing'),
    }),
    object(intentFields, {
      ...intentBase,
      command: { const: 'account_remove' },
      arguments: object(['keep_session', 'reassign_default'], {
        keep_session: { const: false },
        reassign_default: nullable(reference('account')),
      }),
      plan: reference('remoteLogoutPlan'),
      config_snapshot: reference('configSnapshot'),
    }),
    object(intentFields, {
      ...intentBase,
      command: { const: 'account_remove' },
      arguments: object(['keep_session', 'reassign_default'], {
        keep_session: { const: true },
        reassign_default: nullable(reference('account')),
      }),
      plan: reference('keepSessionPlan'),
      config_snapshot: reference('configSnapshot'),
    }),
    ...v2IntentBranches,
  ],
});

const v2CheckpointFields = [
  'schema_version',
  'phase',
  'invocation_id',
  'timestamp',
  'account',
  'command',
  'checkpoint_sequence',
  'stage',
  'data',
];
const dispatchFunctions = {
  send: ['sendMessage'],
  msg_edit: ['editMessageText'],
  msg_delete: ['deleteMessages'],
  msg_forward: ['forwardMessages'],
  msg_react: ['addMessageReaction', 'removeMessageReaction'],
  msg_pin: ['pinChatMessage'],
  msg_unpin: ['unpinChatMessage'],
  chat_mark_read: ['viewMessages'],
  chat_mute: ['setChatNotificationSettings'],
  chat_unmute: ['setChatNotificationSettings'],
  chat_pin: ['toggleChatIsPinned'],
  chat_unpin: ['toggleChatIsPinned'],
  chat_archive: ['addChatToList'],
  chat_unarchive: ['addChatToList'],
  chat_join: ['joinChat', 'joinChatByInviteLink'],
  chat_leave: ['leaveChat'],
  saved_attach: ['sendMessage'],
  session_terminate: ['terminateSession'],
};
const v2CheckpointBranch = (operation, stage, data) =>
  object(v2CheckpointFields, {
    schema_version: { const: 2 },
    phase: { const: 'checkpoint' },
    invocation_id: reference('hex32'),
    timestamp: reference('v2Timestamp'),
    account: reference('account'),
    command: { const: operation },
    checkpoint_sequence: reference('positiveUint32'),
    stage: { const: stage },
    data,
  });
const v2CheckpointBranches = [];
for (const operation of v2Operations) {
  if (operation !== 'session_terminate') {
    v2CheckpointBranches.push(
      v2CheckpointBranch(
        operation,
        'idempotency_pending',
        object(['key_hash', 'request_fingerprint', 'expires_at', 'reserved_terminal_bytes'], {
          key_hash: reference('sha256'),
          request_fingerprint: reference('sha256'),
          expires_at: reference('unixSeconds'),
          reserved_terminal_bytes: reference('uint32'),
        }),
      ),
    );
  }
  if (operation === 'saved_attach') {
    v2CheckpointBranches.push(
      v2CheckpointBranch(
        operation,
        'spool_ready',
        object(['file', 'relative_path'], {
          file: reference('fileSnapshot'),
          relative_path: {
            type: 'string',
            pattern: '^spool/[0-9a-f]{32}/[^/]+$',
          },
        }),
      ),
    );
  }
  v2CheckpointBranches.push(
    v2CheckpointBranch(
      operation,
      'dispatch_started',
      object(['tdlib_function', 'dispatch_token', 'client_generation'], {
        tdlib_function: { enum: dispatchFunctions[operation] },
        dispatch_token: reference('hex32'),
        client_generation: reference('uint64'),
      }),
    ),
  );
  if (['send', 'msg_forward', 'saved_attach'].includes(operation)) {
    v2CheckpointBranches.push(
      v2CheckpointBranch(
        operation,
        'temporary_ids_observed',
        object(['temporary_message_ids'], { temporary_message_ids: int53Array() }),
      ),
    );
  }
  if (operation === 'msg_forward') {
    v2CheckpointBranches.push(
      v2CheckpointBranch(
        operation,
        'forward_progress',
        object(['items'], {
          items: {
            type: 'array',
            minItems: 1,
            maxItems: 100,
            'x-tgcli-strictlyIncreasingField': 'source_id',
            items: reference('forwardItem'),
          },
        }),
      ),
    );
  }
  v2CheckpointBranches.push(
    v2CheckpointBranch(
      operation,
      'mutation_confirmed',
      object(['terminal'], { terminal: v2MutationTerminal(operation) }),
    ),
  );
}
const checkpointSchema = schema(
  'tgcli logout audit checkpoint',
  auditDefinitions(),
  {
    oneOf: [
      object(
        ['schema_version', 'phase', 'invocation_id', 'timestamp', 'account', 'command', 'stage'],
        {
          schema_version: { const: 1 },
          phase: { const: 'checkpoint' },
          invocation_id: reference('invocationId'),
          timestamp: reference('timestamp'),
          account: reference('account'),
          command: { const: 'logout' },
          stage: { enum: ['logout_send_started', 'logout_closed_confirmed'] },
        },
      ),
      ...v2CheckpointBranches,
    ],
  },
);

const logoutSuccessResult = object(['account', 'logged_out'], {
  account: reference('account'),
  logged_out: { const: true },
});
const removalSuccessResult = (remote) =>
  object(['account', 'removed', 'remote_logout', 'default_account'], {
    account: reference('account'),
    removed: { const: true },
    remote_logout: { const: remote },
    default_account: nullable(reference('account')),
  });
const outcomeFields = [
  'schema_version',
  'phase',
  'invocation_id',
  'timestamp',
  'account',
  'command',
  'success',
  'mutation_state',
  'completed_stages',
  'result',
  'error',
];
const outcomeDefinitions = {
  ...definitions(),
  logoutStructuredError: postIntentErrors(logoutErrors, logoutPostIntentCodes, 'logout'),
  removalStructuredError: {
    anyOf: [
      reference('removalRemoteStructuredError'),
      reference('removalKeepStructuredError'),
    ],
  },
  removalRemoteStructuredError: postIntentErrors(
    removalErrors,
    removalPostIntentCodes,
    'account_remove',
    'remote',
  ),
  removalKeepStructuredError: postIntentErrors(
    removalErrors,
    new Set([...removalPostIntentCodes].filter((code) => code !== 'REMOTE_LOGOUT_UNCONFIRMED')),
    'account_remove',
    'keep',
  ),
  logoutSuccessResult,
  removalConfirmedResult: removalSuccessResult('confirmed'),
  removalNotPresentResult: removalSuccessResult('not_present'),
  removalKeptResult: removalSuccessResult('kept'),
};
const v2CompletedStagePrefixes = (operation) => {
  const prefixes = new Map([[JSON.stringify([]), []]]);
  for (const keyed of [false, true]) {
    for (const temporary of [false, true]) {
      for (const progress of [false, true]) {
        for (const mutation of [false, true]) {
          const complete = [];
          if (keyed && operation !== 'session_terminate') complete.push('idempotency_pending');
          if (operation === 'saved_attach') complete.push('spool_ready');
          complete.push('dispatch_started');
          if (temporary && ['send', 'msg_forward', 'saved_attach'].includes(operation)) {
            complete.push('temporary_ids_observed');
          }
          if (progress && operation === 'msg_forward') complete.push('forward_progress');
          if (mutation) complete.push('mutation_confirmed');
          for (let length = 1; length <= complete.length; ++length) {
            const prefix = complete.slice(0, length);
            prefixes.set(JSON.stringify(prefix), prefix);
          }
        }
      }
    }
  }
  return [...prefixes.values()];
};
const v2OutcomeFields = [
  'schema_version',
  'phase',
  'invocation_id',
  'timestamp',
  'account',
  'command',
  'success',
  'mutation_state',
  'completed_stages',
  'terminal',
];
const v2OutcomeBranch = (operation, success, mutation, prefixes, terminal) =>
  object(v2OutcomeFields, {
    schema_version: { const: 2 },
    phase: { const: 'outcome' },
    invocation_id: reference('hex32'),
    timestamp: reference('v2Timestamp'),
    account: reference('account'),
    command: { const: operation },
    success: { const: success },
    mutation_state: { const: mutation },
    completed_stages: { enum: prefixes },
    terminal,
  });
const v2OutcomeBranches = v2Operations.flatMap((operation) => {
  const prefixes = v2CompletedStagePrefixes(operation);
  const undispatched = prefixes.filter((value) => !value.includes('dispatch_started'));
  const dispatched = prefixes.filter((value) => value.includes('dispatch_started'));
  const ambiguous = dispatched.filter((value) => !value.includes('mutation_confirmed'));
  const confirmed = dispatched.filter(
    (value) => value.includes('mutation_confirmed') || value.includes('forward_progress'),
  );
  const none = [...undispatched,
    ...dispatched.filter((value) => value.includes('forward_progress') &&
      !value.includes('mutation_confirmed'))];
  const successful = confirmed.filter((value) => value.at(-1) === 'mutation_confirmed');
  return [
    v2OutcomeBranch(operation, true, 'confirmed', successful, v2ResultTerminal(operation)),
    v2OutcomeBranch(operation, false, 'none', none, v2StoredErrorTerminal(operation)),
    v2OutcomeBranch(operation, false, 'possible', ambiguous, v2StoredErrorTerminal(operation)),
    v2OutcomeBranch(operation, false, 'confirmed', confirmed, v2StoredErrorTerminal(operation)),
  ];
});
const outcomeBase = object(outcomeFields, {
  schema_version: { const: 1 },
  phase: { const: 'outcome' },
  invocation_id: reference('invocationId'),
  timestamp: reference('timestamp'),
  account: reference('account'),
  command: { enum: ['logout', 'account_remove'] },
  success: { type: 'boolean' },
  mutation_state: { enum: ['none', 'possible', 'confirmed'] },
  completed_stages: { type: 'array', minItems: 1, items: reference('auditStage') },
  result: {
    oneOf: [
      reference('logoutSuccessResult'),
      reference('removalConfirmedResult'),
      reference('removalNotPresentResult'),
      reference('removalKeptResult'),
      { type: 'null' },
    ],
  },
  error: {
    anyOf: [
      reference('logoutStructuredError'),
      reference('removalStructuredError'),
      { type: 'null' },
    ],
  },
});
const outcomeRelations = [
  relation(outcomeFields, {
    command: { const: 'logout' },
    success: { const: true },
    mutation_state: { const: 'confirmed' },
    completed_stages: { const: logoutStages[2] },
    result: reference('logoutSuccessResult'),
    error: { type: 'null' },
  }),
  ...logoutStages.map((stages) =>
    relation(outcomeFields, {
      command: { const: 'logout' },
      success: { const: false },
      mutation_state: { const: mutationState(stages) },
      completed_stages: { const: stages },
      result: { type: 'null' },
      error: reference('logoutStructuredError'),
    }),
  ),
];
for (const [remote, stages, resultDefinition] of [
  ['confirmed', removalBranches[0], 'removalConfirmedResult'],
  ['not_present', removalBranches[1], 'removalNotPresentResult'],
  ['not_present', removalBranches[2], 'removalNotPresentResult'],
  ['kept', removalBranches[3], 'removalKeptResult'],
]) {
  outcomeRelations.push(
    relation(outcomeFields, {
      command: { const: 'account_remove' },
      success: { const: true },
      mutation_state: { const: mutationState(stages) },
      completed_stages: { const: stages },
      result: reference(resultDefinition),
      error: { type: 'null' },
    }),
  );
}
for (const state of ['none', 'possible', 'confirmed']) {
  for (const policy of ['shared', 'remote', 'keep']) {
    const prefixes = uniquePrefixes(removalBranches, 2).filter((stages) => {
      const actualPolicy =
        stages.length === 2 ? 'shared' : stages.includes('remote_kept') ? 'keep' : 'remote';
      return mutationState(stages) === state && actualPolicy === policy;
    });
    if (prefixes.length === 0) continue;
    const errorDefinition =
      policy === 'shared'
        ? 'removalStructuredError'
        : policy === 'keep'
          ? 'removalKeepStructuredError'
          : 'removalRemoteStructuredError';
    outcomeRelations.push(
      relation(outcomeFields, {
        command: { const: 'account_remove' },
        success: { const: false },
        mutation_state: { const: state },
        completed_stages: { enum: prefixes },
        result: { type: 'null' },
        error: reference(errorDefinition),
      }),
    );
  }
}
const outcomeSchema = schema(
  'tgcli destructive audit outcome',
  { ...outcomeDefinitions, ...v2Definitions() },
  {
    oneOf: [
      constrained(outcomeBase, outcomeRelations),
      ...v2OutcomeBranches,
    ],
  },
);

const nextStage = (stages, keepSession) => {
  const stage = stages.at(-1);
  if (stage === 'planned') return 'intent_synced';
  if (stage === 'intent_synced') return keepSession ? 'remote_kept' : null;
  if (stage === 'remote_logout_send_started') return null;
  return {
    remote_confirmed: 'client_close_started',
    remote_not_present: 'client_close_started',
    remote_kept: 'client_close_started',
    client_close_started: 'client_closed',
    client_closed: 'config_remove_started',
    config_remove_started: 'config_removed',
    config_removed: 'data_remove_started',
    data_remove_started: 'data_removed',
    data_removed: 'state_remove_started',
    state_remove_started: 'state_removed',
    state_removed: 'outcome_synced',
    outcome_synced: null,
  }[stage];
};
const tombstoneFields = [
  'schema_version',
  'invocation_id',
  'account',
  'stage',
  'completed_stages',
  'next_stage',
  'plan',
  'config_snapshot',
  'data_root',
  'state_root',
];
const tombstoneBase = object(tombstoneFields, {
  schema_version: { const: 1 },
  invocation_id: reference('invocationId'),
  account: reference('account'),
  stage: reference('auditStage'),
  completed_stages: { type: 'array', minItems: 1, items: reference('auditStage') },
  next_stage: nullable(reference('auditStage')),
  plan: { oneOf: [reference('remoteLogoutPlan'), reference('keepSessionPlan')] },
  config_snapshot: reference('configSnapshot'),
  data_root: nullable(reference('rootIdentity')),
  state_root: nullable(reference('rootIdentity')),
});
const tombstoneEntries = new Map();
const addTombstone = (stages, policy) =>
  tombstoneEntries.set(JSON.stringify([stages, policy]), { stages, policy });
addTombstone(['planned'], 'any');
addTombstone(['planned', 'intent_synced'], 'remote');
addTombstone(['planned', 'intent_synced'], 'keep');
for (let index = 0; index < removalBranches.length; ++index) {
  const branch = removalBranches[index];
  const policy = index === 3 ? 'keep' : 'remote';
  for (let length = 3; length <= branch.length; ++length) {
    addTombstone(branch.slice(0, length), policy);
  }
}
for (const { stages, policy } of [...tombstoneEntries.values()]) {
  if (stages.length >= 2) addTombstone([...stages, 'outcome_synced'], policy);
}
const tombstonesByRelation = new Map();
for (const { stages, policy } of tombstoneEntries.values()) {
  const next = nextStage(stages, policy === 'keep');
  const key = JSON.stringify([stages.at(-1), next, policy]);
  const grouped = tombstonesByRelation.get(key) ?? {
    stage: stages.at(-1),
    next,
    policy,
    prefixes: [],
  };
  grouped.prefixes.push(stages);
  tombstonesByRelation.set(key, grouped);
}
const tombstoneRelations = [...tombstonesByRelation.values()].map(
  ({ stage, next, policy, prefixes }) => {
    const plan =
      policy === 'any'
        ? { oneOf: [reference('remoteLogoutPlan'), reference('keepSessionPlan')] }
        : reference(policy === 'keep' ? 'keepSessionPlan' : 'remoteLogoutPlan');
  return relation(tombstoneFields, {
    stage: { const: stage },
    completed_stages: { enum: prefixes },
    next_stage: next === null ? { type: 'null' } : { const: next },
    plan,
  });
  },
);
const tombstoneSchema = schema(
  'tgcli account removal tombstone',
  definitions(),
  constrained(tombstoneBase, tombstoneRelations),
);

const documents = new Map([
  ['logout.error.schema.json', logoutErrorSchema],
  ['account-remove.error.schema.json', removalErrorSchema],
  ['audit-intent.schema.json', intentSchema],
  ['audit-checkpoint.schema.json', checkpointSchema],
  ['audit-outcome.schema.json', outcomeSchema],
  ['removal-tombstone.schema.json', tombstoneSchema],
]);

const render = (document) =>
  (JSON.stringify(
    document,
    (_key, value) => {
      if (value === uint64Maximum) return uint64Marker;
      if (value === int64Minimum) return int64MinimumMarker;
      if (value === int64Maximum) return int64MaximumMarker;
      return value;
    },
    2,
  ) + '\n')
    .replaceAll(`"${uint64Marker}"`, uint64Maximum.toString())
    .replaceAll(`"${int64MinimumMarker}"`, int64Minimum.toString())
    .replaceAll(`"${int64MaximumMarker}"`, int64Maximum.toString());

let mismatch = false;
for (const [filename, document] of documents) {
  const destination = join(directory, filename);
  const expected = render(document);
  if (check) {
    if (readFileSync(destination, 'utf8') !== expected) {
      console.error(`${filename} is not generated from generate-destructive-schemas.mjs`);
      mismatch = true;
    }
  } else {
    writeFileSync(destination, expected);
  }
}
if (mismatch) process.exitCode = 1;
