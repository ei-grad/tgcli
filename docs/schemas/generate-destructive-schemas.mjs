import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = dirname(fileURLToPath(import.meta.url));
const check = process.argv.includes('--check');
const dialect = 'https://json-schema.org/draft/2020-12/schema';
const uint64Maximum = 18446744073709551615n;
const uint64Marker = '__TGCLI_UINT64_MAXIMUM__';

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
const intentSchema = schema('tgcli destructive audit intent', definitions(), {
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
  ],
});

const checkpointSchema = schema(
  'tgcli logout audit checkpoint',
  definitions(),
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
  outcomeDefinitions,
  constrained(outcomeBase, outcomeRelations),
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
    (_key, value) => (typeof value === 'bigint' ? uint64Marker : value),
    2,
  ) + '\n').replaceAll(`"${uint64Marker}"`, uint64Maximum.toString());

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
