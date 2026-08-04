# tgcli — Design Document

A fast, single-binary Telegram CLI built in C++ on [tdlib/td](https://github.com/tdlib/td),
designed for two audiences at once:

1. **Humans** — a pleasant everyday terminal tool: read, search, send, manage
   your Telegram from the shell with minimal ceremony.
2. **AI agents** — a machine-first mode: plain JSON output, meaningful exit
   codes, a sandboxing switch, streaming primitives (`listen`, `wait-for`),
   and non-interactive auth paths.

Status: pre-release development. This document defines the target contract;
TODO.md is authoritative for current implementation status.

## 1. Goals and non-goals

### Goals

- Full-featured control of the user's own account (and optionally bot
  accounts): read, search, send, edit, media, folders, topics, chat
  administration, sessions.
- Fail-closed writes: anything that acts on Telegram on behalf of the account
  is gated behind an explicit grant — per call, per environment, or granted
  once per account — with an explicit deny that overrides any standing grant
  (§6). Reads are always free; with no grant anywhere an invocation is
  read-only, and `TGCLI_ALLOW_WRITE=0` forces read-only regardless of
  account configuration.
- Low-ceremony manual use: the account owner grants writes once in config
  (`allow_write = true`) and `tgcli send @user "hi"` just works — no cache
  warm-up, no confirmation tax on routine actions.
- First-class agent ergonomics: structured errors, deterministic exit codes,
  streaming/wait primitives, non-interactive auth.
- Single static binary, fast startup, no runtime dependencies — trivially
  deployable into agent sandboxes and containers.
- Multi-account with fully isolated state per account.

### Non-goals

- Not a TUI/chat client (no ncurses UI).
- Not an MTProto library — all protocol work is delegated to tdlib.
- No custom message cache: tdlib's own database *is* the cache.
- Bot-platform features (inline keyboards, payments, webhook-style serving)
  are out of scope for v1; basic bot-token login is in scope.

## 2. Design drivers: lessons from existing Telegram CLIs

Existing Telegram CLIs — typically Python tools built on a client library
with a bespoke local message cache — share a set of UX flaws this design
deliberately avoids:

- **Manual cache management** (explicit sync/backfill steps before any read)
  — the single biggest friction, made worse when backfill is a blunt sweep of
  the top-N active dialogs with no way to target a specific chat or depth. tdlib
  eliminates the ceremony: reads are transparently served from tdlib's local
  DB and fetched from the server on miss; for deliberate warming there is an
  explicit, precisely targeted `tgcli fetch <chat>` (§4).
- **Local-only reads**: every query ran against the local cache; there was no
  server-side search at all, so results silently depended on cache freshness.
  tgcli defaults to live server-side operations (per-chat and global search
  included), with `--local` on history reads as the explicit offline option.
- **Bespoke SQLite store with a hardcoded schema** — a migration liability on
  every upgrade. tgcli keeps *no* message store of its own: tdlib owns its
  database and migrates its schema across versions itself. tgcli's only
  persistent state is an append-only JSONL audit log and a tiny versioned
  idempotency store (§8).
- **`listen` streamed to stdout without persisting, and there was no
  wait-by-filter primitive.** In tgcli persistence is decoupled from
  streaming: tdlib writes everything it observes into its DB regardless (the
  daemon keeps it continuously warm), `listen` is just a live view, and
  `wait-for` covers "block until a matching message/event arrives".
- **Envelope tax**: every JSON response wrapped in `{"ok": …, "data": …}`
  forces `jq .data` on every call. tgcli prints the data itself.
- **Flat 60+-verb namespace** (`folders-list`, `chat-invite-link`, …) is hard
  to discover. tgcli uses grouped subcommands plus a small set of flat
  everyday verbs.

What tdlib brings over building on a raw MTProto client library, independent
of UX:
server-side full-text search, consistent local state maintained via updates,
flood-wait handling largely inside the library, secret-chat capability, and a
single static binary with ~instant startup.

## 3. UX principles

- **Everyday verbs are flat, the long tail is grouped.** `tgcli read @chat`,
  `tgcli send @chat "hi"`, `tgcli search foo` — no noun prefix for the
  operations that account for 95% of invocations. Administrative and rare
  operations live under nouns: `tgcli chat …`, `tgcli msg …`, `tgcli folder …`,
  discoverable via `tgcli <noun> --help`.
- **TTY-adaptive**: human tables/colors on a TTY; `--json` (or non-TTY
  auto-detection is *not* used — output format never silently changes) for
  machines. `NO_COLOR` and `--no-color` respected.
- **stdout is data, stderr is commentary.** Progress, warnings, and
  diagnostics never contaminate stdout in either mode.
- **No command requires a TTY except interactive `login`**; every prompt has a
  flag/env/config alternative.
- **Errors are data**: structured error objects with a machine-readable code,
  never just prose.
- **Fast by default**: no network round-trips for what the local tdlib DB can
  answer; `--local` forces offline-only reads.

## 4. Command surface

Global flags:

```
--account <name>       account selection (default from config / TGCLI_ACCOUNT)
--json                 machine output (plain JSON / NDJSON for streams)
--full                 M7: include underlying td_api object under a raw key
--allow-write          per-call write grant; §6 covers env/config grants and
                       the deny override
--dry-run              resolve and validate, print the plan, call nothing;
                       needs no write grant (§6)
--yes                  non-interactive approval of destructive actions
--timeout <sec>        per-command deadline (default 60; streams, file
                       transfers and fetch: unlimited unless set)
--cursor <token>       resume pagination from a `next` token (§5)
--idempotency-key <k>  idempotent writes: record-then-send, replay on repeat (§6)
--verbose / -v         diagnostics to stderr
--no-daemon            debugging: run in-process without the daemon (§10)
--no-color
```

### Everyday commands (flat)

```
tgcli login [--qr | --bot]
tgcli logout                                      destructive (§6)
tgcli me
tgcli doctor                                      auth state, tdlib version, daemon state;
                                                  degrades to local config/socket diagnostics
                                                  when the daemon is unreachable

tgcli chats [--folder <f>] [--archived] [--unread] [-n N]
tgcli read <chat> [-n N] [--before <msg-id>] [--since TS] [--until TS]
              [--topic <id>] [--local]           (alias: history)
tgcli send <chat> [TEXT | -] [--file PATH]... [--caption TEXT]
              [--md | --html] [--reply-to ID] [--topic ID]
              [--silent] [--schedule <ts|"online">] [--spoiler]
tgcli search <query> [--chat <c> | --global] [--from <user>]
              [--type text|photo|video|doc|link|voice] [-n N]
              always server-side — tdlib has no local search (§8)
tgcli unread                                      per-chat unread counters
tgcli fetch <chat> [--limit N | --all] [--since TS]
              deliberately warm the local DB with history for one chat
              (pages getChatHistory; enables later --local / offline work);
              progress on stderr, resumable, per-chat and per-depth targeting
tgcli download <chat> <msg-id> [-O <dir|path>]    progress on stderr
tgcli resolve <t.me-link | @username | title>     → ids, type, metadata

tgcli listen [--chat <c>]... [--types message,edit,delete,reaction,chat]
              [--timeout S] [--count N]           NDJSON stream, one update per line;
                                                  planned expiry (--timeout/--count) exits 0
tgcli wait-for [--chat <c>] [--from <user>] [--regex <re>]
              [--after <msg-id>] [--timeout S]
              blocks until one matching message arrives, prints it, exits 0;
              exits 7 (TIMEOUT) otherwise. --after <id> also matches messages
              that arrived after that id but before the call (checked in the
              local DB behind the subscription) — so `send`, then
              `wait-for --after <sent-id>`, is race-free by construction.
              --after requires --chat (message ids are ordered per chat;
              exit 2 without it)

tgcli raw '<td_api request JSON>' [--timeout S]   M7 full-API escape hatch (§4.2)
```

### Grouped commands (long tail)

```
tgcli msg get <chat> <id>...
tgcli msg edit <chat> <id> <TEXT | ->
tgcli msg delete <chat> <id>... [--for-all]       destructive
tgcli msg forward <from> <id>... <to> [--drop-author]
tgcli msg react <chat> <id> <emoji> [--remove] [--big]
tgcli msg pin|unpin <chat> <id>
tgcli msg link <chat> <id>                        → t.me permalink

tgcli chat info <chat>
tgcli chat members <chat> [--admins|--bots|--query <q>] [-n N]
tgcli chat join <invite-link | @username>
tgcli chat leave <chat>                           destructive
tgcli chat mark-read <chat>
tgcli chat mute|unmute <chat> [--for <duration>]
tgcli chat pin|unpin|archive|unarchive <chat>
tgcli chat set-title|set-photo|set-description <chat> <value>
tgcli chat invite-link <chat> [--revoke]
tgcli chat promote|demote <chat> <user> [--rights ...]
tgcli chat ban|unban|kick <chat> <user>           ban/kick destructive
tgcli chat set-permissions <chat> [--flags ...]

tgcli contact list|search <q>|add|remove|block|unblock
tgcli folder list|show <f>|create|edit|delete|add-chat|remove-chat
                                                  delete destructive
tgcli topic list <chat> | create|edit|close|reopen <chat> ...
tgcli session list | terminate <id>               terminate destructive
tgcli account add <name>
tgcli account list
tgcli account show|use <name>
tgcli account remove <name> [--keep-session]
                           [--reassign-default <name>]  destructive (§11)
tgcli saved tags                                  list reaction tags (§4.3)
tgcli saved search [<query>] --tag <selector> [-n N]
tgcli saved search [<query>] [--tag <selector>] --cursor <token>
tgcli saved attach <message-id> <PATH> [--caption TEXT]
tgcli daemon status|stop|restart|run              lifecycle (§10); status/stop do not auto-spawn,
                                                  `run` stays in the foreground
tgcli storage stats|optimize                      tdlib file-store usage / optimizeStorage cleanup
tgcli schema <command> [--all]                    print the command's curated result schema
tgcli completion <shell>
tgcli version
```

### 4.1 Selectors

`<chat>` accepts `@username`, a numeric tdlib chat id, a `t.me/...` link, or a
title substring. A selector that parses as an integer is always a chat id,
never a title. Title resolution uses tdlib `searchChats` (plus
`searchPublicChat` for unseen `@usernames`). An ambiguous title fails with
exit 2 and a `candidates` list in the error object, so a human or an agent can
retry with a precise id. `<user>` follows the same rules against
contacts/chat members.

Title-substring resolution applies to read-tier commands only. Write- and
destructive-tier commands require an exact selector (`@username`, id, or
t.me link); a title substring fails with exit 2 and the `candidates` list.
Rationale: `searchChats` matches only locally-known chats, so a fuzzy
selector resolves differently depending on how warm the DB is — tolerable
for a read, not for a send that could hit the wrong chat.

`--since`/`--until` timestamps are ISO-8601 (date or datetime, UTC unless an
offset is given) or relative (`30m`, `2h`, `7d`).

Message ids are **tdlib message ids** everywhere (tdlib's message-id space
differs from Telegram's server ids). `msg link` / `resolve` convert to/from
public t.me references, so nobody needs to know about the id-space
difference.

### 4.2 `raw` escape hatch

`raw` and `--full` are reserved syntax through M6. Before M7, invoking `raw`
or passing `--full` to any M0–M6 command is `USAGE` with reason
`unsupported_mode`; M1 curated results therefore have no optional `raw` key.
M7 activates both together and publishes the corresponding schema delta.

Direct passthrough to the td_api schema (`@type`-keyed JSON, the same
convention as td_json_client): guarantees full API coverage before a dedicated
subcommand exists. `raw` passes through the same write gate (§6): request
types outside a known-read-only allowlist count as writes and require a
write grant. Auth-flow request types (`setAuthenticationPhoneNumber`,
`checkAuthenticationPassword`, …) are refused outright — they would put
secrets on argv and into the audit log; authentication goes through `login`
challenges only. Audited `raw` records redact known secret-bearing fields.

### 4.3 Saved Messages namespace

`saved` is an additive, user-account-only command namespace, not a `<chat>`
selector. Its commands always operate on the selected account's own Saved
Messages; account isolation follows §11. The normal authorization check runs
first (`NOT_AUTHED` if the selected account is not ready). An authenticated bot
account is then rejected by namespace preflight, before the selected subcommand
can dispatch any Telegram read or write, with exit 2 and a structured error
such as:

```json
{"error":{"code":"BOT_UNSUPPORTED","message":"saved commands require a user account"}}
```

Telegram has no generic mutation that appends an attachment to an already-sent
text message. `tgcli saved attach <message-id> <PATH> [--caption TEXT]`
therefore sends exactly one new media message as a reply to that message in the
selected user's Saved Messages and preserves the original unchanged. For
example, after an idea note has tdlib message id `N`:

```
tgcli saved attach N result.csv --caption "experiment result"
```

`saved attach` delegates to the normal single-file `send` path: it is
write-tier and inherits the write gate, dry-run plan, audit record,
idempotency-key semantics, timeout/send-confirmation behavior, and successful
curated message result with its final tdlib id in `id`. A missing Saved
Messages message or input file fails with `NOT_FOUND`; malformed arguments or
more than one path fail with `USAGE`. Multiple files and albums are outside
this command. In-place media or caption replacement is also outside this
workflow and may be exposed later by a dedicated command or through `raw` when
the underlying td_api operation supports it.

Saved Messages tags are reaction tags, not emoji characters found in message
text. `tgcli saved tags` returns all tags across Saved Messages topics for the
selected account as the standard unpaginated list result (`next` is null); a
`--cursor` supplied to this command is `USAGE`. Each item has the canonical
reaction selector in `tag`, plus `label` and `count`:

```json
{"items":[{"tag":"🧪","label":"experiments","count":7},{"tag":"custom:123456789","label":"","count":2}],"next":null}
```

The same `tag` string can be passed to `saved search --tag` without
conversion. A regular-emoji selector is the exact, non-empty valid UTF-8
string from tdlib's `reactionTypeEmoji.emoji`; it can contain variation
selectors, skin-tone modifiers, or zero-width-joiner sequences and is one
reaction value rather than necessarily one Unicode scalar. tgcli performs no
Unicode normalization, variation-selector stripping, case folding, or grapheme
rewrite, so callers must preserve the returned string exactly. Empty or invalid
UTF-8 selectors fail with `USAGE`.

A custom-emoji selector has the canonical form `custom:<id>`, where `<id>` is
an unprefixed base-10 integer matching `[1-9][0-9]*` and no greater than
9223372036854775807. The JSON representation remains that string; the id is
never emitted as a JSON number. Zero, negative, signed, leading-zero,
overflowing, non-decimal, or otherwise malformed custom ids fail with `USAGE`.
At the pinned tdlib revision, `reactionTypePaid` denotes paid reactions in
channel chats and has no Saved Messages selector. No CLI spelling maps to it.
If `saved tags` receives a paid or unknown reaction variant, or a non-positive
custom id, it fails with `GENERIC` and tdlib details instead of silently
dropping the item or inventing a selector.

Items from `saved tags` preserve tdlib's returned order: at the pinned
revision this is non-increasing `count`; order among equal counts is opaque and
must not be treated as stable. `tgcli saved search --tag 🧪` searches by one
exact reaction tag; adding the optional positional query, for example
`tgcli saved search experiment --tag 🧪`, intersects the tag filter with
tdlib's Saved Messages text query. An emoji appearing only in message text does
not satisfy `--tag`. An unused but valid tag is a successful empty list, not
`NOT_FOUND`.

The first search page uses
`tgcli saved search [<query>] --tag <selector> [-n N]` without `--cursor`.
A continuation uses `tgcli saved search --cursor <token>` and may redundantly
repeat the same query and canonical `--tag` selector; `-n` is omitted because
the original page size is cursor-bound. The opaque cursor binds the
`saved.search` operation, selected account identity, all-topics Saved Messages
search scope (`saved_messages_topic_id = 0`), exact canonical tag selector,
exact UTF-8 query argument (empty when omitted), page size, and all tdlib
continuation state.
`next_from_message_id` is part of that state, not the complete cursor contract.
A cursor for another operation, account, or Saved Messages scope, a malformed
cursor, or a supplied query/tag that differs from the cursor fails with
`USAGE`. Results use the standard paginated message list in reverse message-id
order, with the next opaque cursor in `next` or null when exhausted.

`saved tags` and `saved search` are read-tier. The pinned
`searchSavedMessages` API is Premium-only; an unavailable Premium capability
surfaces through the existing `GENERIC` exit 1 with tdlib error details rather
than defining a new exit code.

## 5. Output contract

**No envelopes.** In `--json` mode a successful command prints the result
object itself to stdout:

```json
{"id": 123456, "chat_id": -1001234, "date": "2026-07-02T12:00:00Z", "text": "hi", ...}
```

List-returning commands print `{"items": [...], "next": <cursor|null>}`.
`next` is an opaque, self-contained, account-scoped token: pass it back via
`--cursor` (accepted by every paginated command) to fetch the next page
deterministically; the daemon holds no per-cursor state. `read` additionally
accepts a plain `--before <msg-id>` as the human-friendly equivalent.
Streams print one JSON object per line (NDJSON).

Failures print a single error object to **stderr** and set the exit code:

```json
{"error": {"code": "AMBIGUOUS", "message": "3 chats match 'dev'", "candidates": [...]}}
```

- Result schemas are **curated and stable** per command (documented in
  `docs/schemas/`), not raw td_api dumps; `--full` adds the underlying td_api
  object under a `raw` key. They use JSON Schema Draft 2020-12 and are listed
  by command in the result-only `docs/schemas/manifest.json`. The pre-freeze
  baseline is self-contained (no `$id`, external references, or `format`) and
  rejects undeclared properties at every object boundary. Commands without a
  result, such as `daemon run`, do not appear in the manifest.
- Human output renders the same data — no information exists in one mode that
  the other lacks.
- Warnings go to stderr (prefixed `warning:` in human mode, NDJSON
  `{"warning":"<message>"}` objects in `--json` mode).

### Exit codes

| code | name | meaning |
|---|---|---|
| 0 | OK | success |
| 1 | GENERIC | unclassified error (tdlib error details on stderr) |
| 2 | USAGE | bad arguments, unknown selector, ambiguous resolve |
| 3 | NOT_AUTHED | not logged in → `tgcli login` |
| 4 | NOT_FOUND | operational target/routing failure: entity absent or routed daemon account mismatch |
| 5 | RATE_LIMITED | Telegram flood wait surfaced; `retry_after` in error details |
| 6 | DENIED | write attempted without a grant, or destructive action without confirmation |
| 7 | TIMEOUT | `--timeout` elapsed without the awaited outcome (`wait-for`, transfers, raw); `listen` reaching `--timeout`/`--count` is a planned expiry and exits 0 |

### 5.1 M1 curated results and errors

The following are the exact M1 success objects; later Draft 2020-12 schemas
encode these shapes without adding td_api fields. A `UserSummary` is:

```json
{"id":123456,"first_name":"Ada","last_name":"Lovelace","usernames":["ada"],"phone_number":"12025550123","is_bot":false,"is_premium":false}
```

`usernames` contains tdlib's active usernames in returned order and
`phone_number` is the returned string (empty for a bot), never inferred.

| command/path | exact success data |
|---|---|
| `login` | `{"account":"main","auth_state":"ready","user":<UserSummary>}` |
| `logout` | `{"account":"main","logged_out":true}` after correlated `Closed`; dry-run is `{"dry_run":true,"plan":{"operation":"logout","account":"main","remote_logout":true,"tdlib_request":"logOut"}}` |
| `me` | `<UserSummary>` |
| `account add work` | `{"account":"work","created":true,"default":false}` (`default` is true only for the first configured account) |
| `account list` | `{"items":[{"name":"main","default":true},{"name":"work","default":false}],"next":null}` in bytewise name order |
| `account show main` | `{"account":"main","default":true,"allow_write":false,"idle_exit":null,"credentials":{"api_id":"value","api_hash":"value","db_key":"none","password":"interactive","bot_token":"interactive"},"paths":{"data":"/…/tgcli/accounts/main","state":"/…/tgcli/accounts/main","socket":"/…/main.sock"}}` |
| `account use work` | `{"default_account":"work","previous_default":"main"}`; `previous_default` is null only when none was configured |
| `account remove work` | `{"account":"work","removed":true,"remote_logout":"confirmed","default_account":"main"}`; `remote_logout` is exactly `confirmed`, `not_present`, or `kept`, and `default_account` is string or null |
| `daemon status` (running) | `{"account":"main","running":true,"pid":123,"version":"0.1.0","protocol":2,"socket":"/…/main.sock"}` |
| `daemon status` (absent) | `{"account":"main","running":false,"socket":"/…/main.sock"}` |
| `daemon stop` | the existing M0 object `{"stopping":true}` |
| `daemon restart` | `{"account":"main","restarted":true,"pid":124,"version":"0.1.0","protocol":2,"socket":"/…/main.sock"}` |

In `account show`, `api_id`/`api_hash` are exactly `value`, `command`, or
`missing`; `db_key` is `command` or `none`; and `password`/`bot_token` are
`command` or `interactive`. This reports persisted account configuration, not
temporary `TGCLI_API_ID`/`TGCLI_API_HASH` spawn overrides. Values and command
strings are never returned.
The `account remove` dry-run is exactly:

```json
{"dry_run":true,"plan":{"operation":"account_remove","account":"work","remote_logout":true,"keep_session":false,"delete_paths":["/…/data","/…/state"],"config_path":"/…/tgcli/config.toml","config_snapshot":"sha256:…","data_root":{"path":"/…/data","device":1,"inode":2,"owner":1000},"state_root":{"path":"/…/state","device":1,"inode":3,"owner":1000},"reassign_default":"main"}}
```

The booleans are opposites, `delete_paths` are absolute in data-then-state
order, `config_path` is absolute, `config_snapshot` is §9's identity, and
`reassign_default` is string or null. Each root identity is exactly
`{"path":string,"device":uint64,"inode":uint64,"owner":uint64}` or null
when that root is absent; its path still appears in `delete_paths`.
Dry-run performs the same name/default,
path and config validation as execution but does not start a daemon, execute a
hook, challenge, append audit, call tdlib, mutate config or touch either path;
when it cannot prove remote auth state without doing so, the plan still states
the requested `remote_logout` policy rather than predicting its outcome.

`doctor` deliberately retains the three existing M0 object shapes so M1 does
not redesign an M0 result: reachable daemon and in-process variants contain
exactly `account`, the existing `daemon` object, `tdlib:{"version":...}`, and
`auth:{"state":...}`; local fallback contains exactly `account`, the existing
absent-daemon object and `config:{"path":...,"exists":...}`. M1 widens the
reachable `auth.state` value from `unknown` to the state names in §8 while
retaining `unknown` before the first snapshot. `version` and the successful
`daemon stop` object are otherwise unchanged.

Every M1 failure uses the single envelope
`{"error":{"code":<string>,"message":<string>,"details":<object>}}` on
stderr. `message` is explanatory text; branching uses `code`, exit status and
the exact detail shape below. Each `details` object contains exactly the shown
keys and no others. `string[]` arrays are bytewise sorted unless the shape says
otherwise. No secret, hook command, answer, raw argv, tdlib message or
unredacted tdlib request is permitted in any field.

`state` is `unknown` or one of the 13 tgcli state names in §8. `operation` is
exactly `auth_bootstrap`, `login`, `logout`, `me`, `account_add`,
`account_list`, `account_show`, `account_use`, `account_remove`,
`doctor`, `daemon_status`, `daemon_stop`, `daemon_restart`, `daemon_run`,
`config_reload`, or `audit`.
`credential` is the closed enum in §8. `completed_stages` is an ordered list of
the exact durable `audit_stage` values reached by that invocation; it never
contains a stage that was merely attempted in memory.

| error code | exit | exact `details` object |
|---|---:|---|
| `USAGE` | 2 | `{"argument":nullable_string,"reason":usage_reason}`; the inherited M0 daemon-lifecycle `--no-daemon` case and a connection-scoped malformed-frame rejection remain exactly `{}` |
| `INSECURE_SECRET_INPUT` | 2 | `{"argument":"--bot-token","replacement":"--bot"}` |
| `ACCOUNT_EXISTS` | 2 | `{"account":string}` |
| `DEFAULT_REASSIGNMENT_REQUIRED` | 2 | `{"account":string,"candidates":string[]}` |
| `ACCOUNT_NOT_FOUND` | 4 | `{"account":string}` |
| `ACCOUNT_MISMATCH` | 4 | `{"requested_account":string,"daemon_account":string}` |
| `CONFIG_INVALID` | 1 | `{"path":string,"reason":config_reason}` |
| `CONFIG_CONFLICT` | 1 | `{"path":string,"expected":string,"current":string}` |
| `HOOK_FAILED` | 1 | `{"hook":hook_name,"reason":hook_reason,"status":nullable_integer}` |
| `AUTH_FLOW_IN_PROGRESS` | 3 | `{"account":string,"state":state}` |
| `NOT_AUTHED` | 3 | `{"account":string,"state":state,"reason":not_authed_reason}` |
| `AUTH_INPUT_REQUIRED` | 3 | `{"account":string,"state":state,"challenge":challenge_kind}` |
| `AUTH_CANCELLED` | 3 | `{"account":string,"state":state,"challenge":challenge_kind}` |
| `AUTH_CREDENTIAL_REJECTED` | 3 | `{"account":string,"state":state,"credential":credential,"tdlib_code":integer}` |
| `AUTH_PREMIUM_REQUIRED` | 3 | `{"account":string,"state":"wait_premium_purchase","store_product_id":string,"premium_day_count":integer,"support_email_address":string,"support_email_subject":string}` |
| `UNSUPPORTED_AUTH_STATE` | 1 | `{"account":string,"tdlib_type_id":integer}` |
| `AUTH_FUNCTION_DENIED` | 6 | `{"account":string,"state":state,"function":auth_function}` |
| `PROTOCOL_ANSWER_INVALID` | 2 | `{"request_id":uint64,"reason":protocol_reason}` |
| `WRITE_DENIED` | 6 | `{"account":string,"reason":write_reason}` |
| `CONFIRMATION_REQUIRED` | 6 | `{"account":string,"action":destructive_action,"target":destructive_plan}` |
| `AUDIT_UNAVAILABLE` | 6 | `{"account":string,"path":string,"reason":audit_reason}` |
| `AUDIT_INCOMPLETE` | 1 | `{"account":string,"path":string,"mutation_state":mutation_state,"completed_stages":audit_stage[]}` |
| `REMOTE_LOGOUT_UNCONFIRMED` | 1 | `{"account":string,"state":state,"reason":remote_reason}` |
| `LOCAL_CLEANUP_FAILED` | 1 | `{"account":string,"reason":cleanup_reason,"removed":string[],"retained":string[]}` |
| `REMOVAL_INCOMPLETE` | 1 | `{"account":string,"path":string,"invocation_id":string,"stage":audit_stage,"completed_stages":audit_stage[],"reason":removal_reason}` |
| `DAEMON_NOT_RUNNING` | 4 | `{"account":string,"socket":string}` |
| `DAEMON_CONTROL_FAILED` | 1 | `{"account":string,"operation":daemon_control_operation,"reason":daemon_reason}` |
| `RATE_LIMITED` | 5 | `{"operation":operation,"tdlib_code":429,"retry_after":integer}` |
| `TDLIB_ERROR` | 1 | `{"operation":operation,"tdlib_code":integer}` |
| `TIMEOUT` | 7 | `{"operation":operation,"state":nullable_state}` |
| `DAEMON_SHUTDOWN` | 1 | `{"reason":"daemon_shutdown"}` |
| `INTERNAL` | 1 | `{"operation":operation,"reason":"internal_error"}` |

The auxiliary enums are closed: `usage_reason` is `missing_argument`,
`invalid_argument`, `mutually_exclusive`, `unknown_command`,
`invalid_environment`, or `unsupported_mode`; `config_reason` is
`path_invalid`, `wrong_owner`, `wrong_type`, `wrong_mode`, `wrong_link_count`,
`too_large`, `parse_error`, `type_error`, `invalid_default`,
`conflicting_credentials`, `io_error`, or `sync_error`; `hook_name` is
`api_id_cmd`, `api_hash_cmd`, `db_key_cmd`, `password_cmd`, or
`bot_token_cmd`; `hook_reason` is `spawn`, `exit`, `signal`, `timeout`,
`stdout_empty`, `stdout_invalid`, `stdout_too_large`, or `stderr_too_large`;
`not_authed_reason` is `not_ready`, `authorization_lost`, or `login_required`;
`protocol_reason` is `future_sequence`, `nonce_mismatch`,
`generation_mismatch`, `malformed`, or `unknown_request`; `write_reason` is
`explicit_deny`, `no_grant`, or `invalid_config_grant`; `audit_reason` is
`path_invalid`, `open_failed`, `write_failed`, `sync_failed`, or
`rotate_failed`; `remote_reason` is `tdlib_error`, `timeout`,
`generation_lost`, `transport_lost`, or `state_unproven`; `cleanup_reason` is
`path_changed`, `path_invalid`, `mount_boundary`, `io_error`, or `sync_error`;
`removal_reason` is `prior_crash`, `identity_ambiguous`, or
`outcome_missing`; and `daemon_reason` is `surface_invalid`,
`identity_changed`, `handshake_failed`, `shutdown_failed`, or
`replacement_failed`. `destructive_action` is `logout` or `account_remove`;
`daemon_control_operation` is `status`, `stop`, or `restart`;
`challenge_kind` is one of the exact kinds in §10; `auth_function` is one of
the ten AuthBootstrap functions listed in §6, `getMe`, `logOut`, `close`, or
`other`; `destructive_plan` is the exact logout or removal plan from §5.1 and
must match `action`; `mutation_state` is `none`, `possible`, or `confirmed`;
and `audit_stage` is `planned`, `intent_synced`, `logout_send_started`,
`logout_closed_confirmed`, `remote_logout_send_started`, `remote_confirmed`,
`remote_not_present`, `remote_kept`, `client_close_started`, `client_closed`,
`config_remove_started`, `config_removed`, `data_remove_started`,
`data_removed`, `state_remove_started`, `state_removed`, or `outcome_synced`.
`uint64` is a JSON integer from 0 through 18446744073709551615; frame request
ids use that representation end to end and are never stringified.
`nullable_string`, `nullable_integer` and `nullable_state` mean exactly their
named scalar/state type or JSON null.

`mutation_state` is computed from durable checkpoints, not process memory.
The mutation-relevant started/completion pairs are
`logout_send_started`/`logout_closed_confirmed`,
`remote_logout_send_started`/`remote_confirmed`,
`config_remove_started`/`config_removed`,
`data_remove_started`/`data_removed`, and
`state_remove_started`/`state_removed`. It is `possible` if any such start
lacks its completion, even when an earlier mutation was confirmed; otherwise
it is `confirmed` if `logout_closed_confirmed`, `remote_confirmed`,
`config_removed`, `data_removed`, or `state_removed` is present; otherwise it
is `none`. `remote_not_present`, `remote_kept` and client-close stages are
proof/lifecycle stages and do not by themselves make it `confirmed`.

Logout `completed_stages` is exactly one of
`["intent_synced"]`,
`["intent_synced","logout_send_started"]`, or
`["intent_synced","logout_send_started","logout_closed_confirmed"]`.
Removal starts with `planned,intent_synced`, then takes exactly one remote
branch (`remote_logout_send_started,remote_confirmed`,
`remote_logout_send_started,remote_not_present`, `remote_not_present`, or
`remote_kept`), followed by the ordered
`client_close_started,client_closed,config_remove_started,config_removed,
data_remove_started,data_removed,state_remove_started,state_removed` suffix.
At any instant its array is an exact prefix of that selected sequence.
`outcome_synced` is appended only to the tombstone after the outcome record is
durable; the outcome cannot self-report that later checkpoint.

Known credential errors retry only under §8. Unknown 400/401, all 5xx and all
other tdlib failures are `TDLIB_ERROR`; 429 is `RATE_LIMITED`. Account removal
uses `TDLIB_ERROR` only for tdlib work, `LOCAL_CLEANUP_FAILED` for filesystem
work, and the config/audit codes for those respective boundaries.

## 6. Safety model

Every tdlib send is admitted by one daemon-side chokepoint (§7 `safety`).
Command handlers statically declare `Read`, `AuthBootstrap`, `Write`, or
`Destructive`; the user-authority tiers remain Read/Write/Destructive:

- **Reads** — always allowed, no grant needed.
- **AuthBootstrap** — grant-exempt but not unrestricted. It admits only
  `getAuthorizationState`, `setTdlibParameters`,
  `setAuthenticationPhoneNumber`, `requestQrCodeAuthentication`,
  `checkAuthenticationBotToken`, `setAuthenticationEmailAddress`,
  `checkAuthenticationEmailCode`, `checkAuthenticationCode`, `registerUser`,
  and `checkAuthenticationPassword`. Each function must match the current
  pinned auth state, client generation, auth owner and secret-source rules in
  §8; anything else is `AUTH_FUNCTION_DENIED`. The initial state query and
  configured-credential bootstrap use an internal auth owner, while
  interactive functions require the login lease. `getMe` remains Read;
  `logOut` remains Destructive; process-global log initialization and
  intentional `close` are lifecycle primitives, not an auth bypass.
- **Writes** — anything that acts on Telegram on behalf of the account (send,
  edit, react, mark-read, mute, folder edits, …) is **denied by default**
  (exit 6). It runs only under an explicit write grant, any of:
  - `--allow-write` — per call;
  - `TGCLI_ALLOW_WRITE=1` — per environment (a shell session, a CI job, an
    agent harness that is trusted to write);
  - `allow_write = true` in the account's config — granted once by the
    account owner for ceremony-free everyday use of their own account.

  **An explicit deny overrides every grant.** `TGCLI_ALLOW_WRITE=0` denies
  writes even when the config grants them and even when the invocation
  passes `--allow-write`. Precedence: explicit deny > any grant > default
  deny. This is the one-variable sandbox switch for a harness running an
  agent against an account whose owner keeps a standing config grant.
- **Destructive** (msg delete, chat leave/ban/kick, session terminate, folder
  delete, logout, account remove) — requires a write grant *and*
  confirmation: on a TTY an interactive prompt showing the resolved target
  (`leave chat "Dev Team" (-1001234)? [y/N]`); without a TTY an explicit
  `--yes`.

`login` and the auth flows are tier-exempt: the gate governs acting *as* the
account, not becoming it — requiring a write grant to create the very first
session would break bootstrap. `logout` remains destructive.

M1 implements the safety kernel only for its two destructive commands,
`logout` and `account remove`: the shared daemon-side tier chokepoint,
deny-overrides-grants authority calculation, resolved-target confirmation,
`--yes`, `--dry-run`, and the audit primitives below. Its order is fixed:
parse/config and target validation; build the plan; return a dry-run without
authority or confirmation; evaluate authority; validate the auth/account
precondition; obtain confirmation; for a config-changing plan acquire the
config lock and verify its captured identity; durably append audit intent;
then and only then make the first Telegram call or local deletion. An exact
affirmative TTY answer or request `--yes` confirms; no TTY, cancellation,
disconnect, false/empty/malformed answer, or expiry is no confirmation. The
general write-command gate, idempotency and M3 command planners remain M3;
moving this kernel does not make any other write-tier descriptor executable
in M1. For these commands, confirmation `action` is exactly `logout` or
`account_remove`, and `target` is the corresponding `plan` object from §5.1;
the prompt, error details and audit use that same object without re-resolution.

M1 logout audit entries are JSONL in the target account's `audit.log`, opened
without following symlinks in the verified `0700` state directory. Account
removal instead uses `$XDG_STATE_HOME/tgcli/removals/audit.log` and
`$XDG_STATE_HOME/tgcli/removals/<invocation_id>.json`, both outside every
deletable account root. The verified `removals` directory is current-uid mode
`0700`; its audit and tombstone files are current-uid, non-symlink regular
files with mode `0600` and link count 1. The global audit rotates under §9 but
never deletes a record still referenced by a nonterminal tombstone.

Each executing M1 destructive invocation has a random `invocation_id`, one
durable `phase:"intent"` record before the first mutation, zero or more exact
checkpoint records/tombstone transitions, and one durable `phase:"outcome"`
record before any terminal frame. Logout appends and syncs
`logout_send_started` before calling `ClientManager::send(logOut)` and appends
and syncs `logout_closed_confirmed` only after correlated `Closed`. Removal
uses the before-action/after-confirm tombstone transitions in §11. Raw argv,
challenge answers, hook values and secret-bearing fields are never recorded.
A dry-run, authority denial, failed CAS or unconfirmed request writes no intent
because it cannot mutate anything. If intent cannot be appended and synced,
the operation is `AUDIT_UNAVAILABLE` and does nothing. Once intent exists, no
terminal frame is emitted until its outcome record is appended and synced. If
that becomes impossible, the connection closes without a terminal, the daemon
enters audit-fatal shutdown, and the durable removal tombstone or unmatched
logout intent causes the next inspection to report `REMOVAL_INCOMPLETE` or
`AUDIT_INCOMPLETE`, respectively; an unaudited success or error is never
emitted. Size rotation preserves complete JSONL records and the full
intent/checkpoint/outcome group. M3 extends these records to its writes and
adds idempotency integration; AuthBootstrap and other M1 non-destructive work
do not create destructive audit records.

Audit timestamps are UTC RFC 3339 strings, `invocation_id` is 32 lowercase
hexadecimal characters, `authority_source` is `request` or `config` (the
tri-state request intentionally folds flag and environment), and
`confirmation_source` is `yes` or `tty`. Every record is one complete object
and one LF; rotation never splits a record or any invocation's complete
intent/checkpoint/outcome group.

The three M1 audit record objects have exactly these keys and types, with no
extension fields:

```text
intent = {"schema_version":1,"phase":"intent","invocation_id":string,
          "timestamp":string,"account":string,
          "command":"logout" or "account_remove","arguments":object,
          "plan":destructive_plan,"config_snapshot":string,
          "authority_source":"request" or "config",
          "confirmation_source":"yes" or "tty"}
checkpoint = {"schema_version":1,"phase":"checkpoint",
              "invocation_id":string,"timestamp":string,"account":string,
              "command":"logout","stage":"logout_send_started" or
              "logout_closed_confirmed"}
outcome = {"schema_version":1,"phase":"outcome","invocation_id":string,
           "timestamp":string,"account":string,
           "command":"logout" or "account_remove","success":boolean,
           "mutation_state":mutation_state,"completed_stages":audit_stage[],
           "result":command_result|null,"error":structured_error|null}
```

For logout, `arguments` is exactly `{}` and `plan` is the exact logout plan in
§5.1. For removal, `arguments` is exactly
`{"keep_session":boolean,"reassign_default":string|null}` and `plan` is its
exact §5.1 plan. `config_snapshot` is §9's identity string. On success,
`result` is the command's exact success object and `error` is null; on failure,
`result` is null and `error` is exactly `{"code":string,"details":object}`
using the closed table in §5.1. `command_result` is the exact success object for
the record's command and `structured_error` is that two-key error object;
neither has extension fields. `completed_stages` uses the ordered stage enum.
`mutation_state` uses the durable-checkpoint rule in §5.1; dispatch without a
correlated completion is `possible`, never `confirmed`.

Authority-source selection is deterministic: an explicit request deny stops
before intent; otherwise a request grant records `request`, even if config also
grants; only an unset request plus valid config grant records `config`.
Likewise `--yes` records `yes`; otherwise only an exact TTY confirmation
records `tty`. No other value reaches an intent.

Audit inspection is an exact M1 preflight for `login`, `logout`, `me`,
`doctor`, `account show <name>` and `account remove <name>`; daemon control and
`account list|add|use` do not inspect a per-account logout audit. For each
intent lacking a synced outcome, the reconciler does exactly this: intent with
no `logout_send_started` gets a failed `INTERNAL` outcome with mutation state
`none`; `logout_closed_confirmed` gets the successful logout outcome with
`confirmed`; and `logout_send_started` without that completion reopens the
database only to observe state, never to resend. It writes a failed
`REMOTE_LOGOUT_UNCONFIRMED` outcome with `possible` once the state is observed;
that error's `state` is the exact observed auth snapshot and its `reason` is
`generation_lost`. If observation cannot finish within the inspecting
command's deadline, the intent remains incomplete.

The preflight continues the requested command only after the reconciled outcome
is synced. Otherwise it returns `AUDIT_INCOMPLETE`; `details.path` is exactly
the absolute per-account `audit.log` containing the unmatched intent, and its
`mutation_state` and `completed_stages` come only from synced records. The
condition clears only when that invocation's outcome is synced;
intent/checkpoint records are never deleted to hide it. A removal tombstone is
resolved first and uses `REMOVAL_INCOMPLETE` instead: that error's `path` is
exactly the absolute `removals/<invocation_id>.json`, and it stops blocking
only after the same tombstone durably reaches `outcome_synced`.

The gate fails closed: an agent invoked with no grant — or under
`TGCLI_ALLOW_WRITE=0` when the account carries a standing grant — has a
read-only surface and receives a structured exit-6 error the moment it
strays. The gate guards against accidental and unauthorized-by-omission
writes, not against a hostile process running as the same uid (§10).

Supporting mechanisms:

- **`--dry-run`** — performs resolution and validation, prints the exact plan
  (resolved ids, message preview, td_api request type), calls nothing. It
  needs no write grant and no `--yes` — it exists precisely so plans can be
  made without authority. M1 supports it for `logout` and `account remove`;
  general command planners land in M3.
- **Audit log** — M3 applies the intent/outcome record pair to its general
  write kernel. In M1 it applies only to actual `logout` and `account remove`
  execution. Normalized
  non-secret arguments and resolved targets are recorded; secret-bearing
  fields are always excluded. Files rotate by size (§9).
- **`--idempotency-key <k>`** — record-then-send: the key is written to the
  store as *pending* (with a payload fingerprint) before the request is
  dispatched, and updated with the result on completion. Replay semantics: a
  *completed* key returns the recorded result without calling Telegram; a
  *pending* key (the prior attempt's outcome is unknown — daemon crash or
  client-side timeout mid-send) fails with exit 1 and error code
  `IDEMPOTENCY_PENDING` instead of re-sending, so the caller can inspect the
  chat or pick a new key; the same key with a different payload fingerprint
  is a usage error. Entries expire after 7 days. The gate is checked before
  the store: with no grant or under an explicit deny, replay of even a
  *completed* key exits 6 — a write-tier command never returns results in a
  denied context. Recording only successes would not prevent double-sends in
  the very scenario retries exist for; record-then-send does.

## 7. Architecture

One binary, two roles: every invocation is a thin **client**; the tdlib
client lives in a per-account **daemon** (auto-spawned, §10). The role is an
argv detail (`tgcli daemon run` is the daemon entrypoint), all code ships in
the same binary.

```mermaid
graph TD
    subgraph client ["client process (every invocation)"]
        CLI["cli: CLI11 parser<br/>argv → Command + GlobalOpts"]
        OUT["output: JSON writer, human renderers,<br/>NDJSON stream writer, exit-code mapping"]
        PROMPT["prompts: login challenges,<br/>destructive confirmations (TTY)"]
    end
    subgraph daemon ["daemon process (one per account)"]
        DISP["dispatch: frame protocol,<br/>request → handler, stream multiplexing"]
        CMD["commands: one handler per subcommand"]
        SAFE["safety: tier gates, confirm challenges,<br/>dry-run planner, audit, idempotency"]
        RES["resolver: chat/user/link resolution,<br/>ambiguity → candidates"]
        CORE["core: TdClient<br/>type-erased TdValue requests → futures,<br/>auth FSM, update bus, file transfers"]
        TD["tdlib ClientManager<br/>(own thread)"]
    end
    CLI -- "request frame<br/>(unix socket, JSONL)" --> DISP
    DISP -- "result / stream / progress frames" --> OUT
    DISP -. "challenge frames" .-> PROMPT
    DISP --> CMD
    CMD --> SAFE --> CORE
    CMD --> RES --> CORE
    CORE --> TD
```

Layer responsibilities:

- **client** — parses argv, opens (or spawns) the account daemon, sends one
  request frame, renders response frames, maps the terminal frame to an exit
  code. Owns the TTY: challenge frames (login secrets, destructive
  confirmations) are prompted client-side and answered back over the socket.
- **dispatch** — the daemon-side protocol endpoint: frames in/out, concurrent
  requests, subscription multiplexing (many `listen`/`wait-for` clients over
  one update bus).
- **core (`TdClient`)** — owns the tdlib `ClientManager` receive loop on a
  dedicated thread. Its public boundary is
  `send(TdValue) → future<TdValue>`: `TdValue` is move-only type erasure, so
  generated td_api types do not enter project headers. Daemon implementation
  translation units construct typed requests, box them for `send`, and unbox
  typed replies. The core also owns request-id correlation, the update bus
  used by `listen`/`wait-for`/file progress, and the authorization state
  machine. Command handlers never touch the ClientManager or receive loop.
- **resolver** — selector strings → ids, cached per request; produces the
  `candidates` payload on ambiguity.
- **safety** — the single chokepoint (§6), evaluated daemon-side. Handlers
  declare exactly one `DescriptorKind::Read|AuthBootstrap|Write|Destructive`
  in their static descriptor. Descriptor kind governs function admission;
  only Read/Write/Destructive are user authority tiers, so AuthBootstrap does
  not create a fourth grant level.
- **commands** — thin: validated args in, a few `TdClient` calls, curated
  result JSON out, plus a human renderer (rendering happens client-side from
  the same curated data).
- **output** — the only code that writes to the client's stdout.

### tdlib interface choice

tgcli uses tdlib's **native typed C++ interface** (`td::td_api`) for all
implemented commands — compile-time schema safety — and tdlib's JSON
conversion layer (`td_api_json`) for the `raw` command and `--full` dumps.
That conversion layer is generated tdlib-internal code, not part of the
installed public interface, so tgcli supports exactly one tdlib provenance:
the pinned source revision — built via FetchContent, or preinstalled into a
prefix by `scripts/build-tdlib.sh`, which exports the extra headers
`raw`/`--full` need (§13). Arbitrary distro tdlib packages are not
supported.

The huge `td_api.h` header is confined to daemon-side implementation
translation units — `core/` plus individual command `.cpp` files, which do
construct and box typed requests — behind a precompiled header. It never
appears in public headers or client-side code (cli, output, prompts). This
keeps incremental builds tolerable without hand-mirroring td_api while the
type-erased public boundary keeps generated types from leaking across layers.

## 8. tdlib integration details

- **Bootstrap ordering and parameters**: after installing the process-global
  logging cap, a new client generation reserves query id 1 and its first send
  is `getAuthorizationState`. No other query or auth update is acted on until
  that response or the generation's first `updateAuthorizationState` installs
  the initial snapshot. The generation begins at `auth_sequence=0` with no
  accepted update observed. In response-first order, the response installs its
  state and increments the sequence to 1 only if that condition still holds;
  the first later update, even if payload-equal, increments it to 2. In update-
  first order, the update installs sequence 1 and the later bootstrap response
  is consumed for query correlation but ignored for state and sequence. Once
  any update has been accepted, no bootstrap response can install a snapshot.
  `setTdlibParameters` is sent only from `wait_tdlib_parameters`, once its 14
  fields have been resolved as follows:

  | field | exact source/value |
  |---|---|
  | `use_test_dc` | `false`, except exact trusted harness opt-in `TGCLI_TEST_DC=1` described below |
  | `database_directory` | `<account-data-dir>/tdlib/db` |
  | `files_directory` | `<account-data-dir>/tdlib/files` |
  | `database_encryption_key` | `db_key_cmd`, interactive `database_key` fallback, or empty bytes when no key is configured/requested |
  | `use_file_database` | `true` |
  | `use_chat_info_database` | `true` |
  | `use_message_database` | `true` |
  | `use_secret_chats` | `false` (secret chats are post-1.0) |
  | `api_id` | `TGCLI_API_ID` > configured value > `api_id_cmd` > login challenge; canonical positive signed-32-bit integer |
  | `api_hash` | `TGCLI_API_HASH` > configured value > `api_hash_cmd` > login challenge |
  | `system_language_code` | literal `en` |
  | `device_model` | literal `tgcli` |
  | `system_version` | literal `Linux` or `macOS` for the compiled target |
  | `application_version` | the running tgcli binary version string |

  The directories are created as current-uid `0700` directories before the
  send. Test mode is not a normal credential/config override: the client must
  see exactly `TGCLI_TEST_DC=1`, select `tgcli-test` instead of `tgcli` in
  every config/data/state/runtime root, carry `test_dc:true` through daemon
  re-exec and bootstrap identity, and require the daemon's parameter snapshot
  to agree before sending credentials. Any other non-empty value is `USAGE`.
  A test-mode process refuses a production-namespaced socket/database/config;
  a production process refuses test-namespaced state. Thus the harness cannot
  authenticate against production even if a production daemon is running.

  `setAuthenticationPhoneNumber` always uses
  `phoneNumberAuthenticationSettings` with `allow_flash_call=false`,
  `allow_missed_call=false`, `is_current_phone_number=false`,
  `has_unknown_phone_number=false`, `allow_sms_retriever_api=false`, null
  Firebase settings and an empty authentication-token array.
  `requestQrCodeAuthentication` always sends an empty `other_user_ids` array.
  `registerUser` sends the challenged first/last names and
  `disable_notification=false`.
- **Auth state snapshot**: the receive loop is the sole writer. Before
  publishing each `updateAuthorizationState`, it atomically replaces an
  immutable snapshot containing `(client_generation, auth_sequence, state,
  state fields)`. It starts at 0/`unknown`; every accepted auth update
  increments it exactly once, including payload-equal repeated-state updates.
  The only non-update installation is the response-first bootstrap rule above.
  Readers either see the complete old snapshot or the complete new one. Login
  reads the current snapshot after acquiring the auth lease and therefore
  resumes the state tdlib is actually in instead of replaying earlier steps.
- **Auth FSM**: the pinned tdlib authorization variants are exhaustively
  handled as follows. An unknown variant is `UNSUPPORTED_AUTH_STATE` (exit 1),
  never treated as ready.

  | tdlib state | tgcli state | login action |
  |---|---|---|
  | `authorizationStateWaitTdlibParameters` | `wait_tdlib_parameters` | Resolve app credentials and the optional DB key, then call `setTdlibParameters`. Missing app credentials are requested from the login owner and persisted atomically (§9). |
  | `authorizationStateWaitPhoneNumber` | `wait_phone_number` | Plain `login` challenges for a phone number and calls `setAuthenticationPhoneNumber`; `--qr` calls `requestQrCodeAuthentication`; `--bot` resolves a bot token (§9) and calls `checkAuthenticationBotToken`. |
  | `authorizationStateWaitPremiumPurchase` | `wait_premium_purchase` | Do not initiate or emulate a purchase. Terminate with `AUTH_PREMIUM_REQUIRED` (exit 3) and all non-secret product/day-count/support fields from the state. |
  | `authorizationStateWaitEmailAddress` | `wait_email_address` | Challenge for an email address and call `setAuthenticationEmailAddress`; Apple-ID/Google-ID web-token login is outside v1. |
  | `authorizationStateWaitEmailCode` | `wait_email_code` | Challenge for the emailed code and call `checkAuthenticationEmailCode`; the prompt includes only the address pattern and expected length. |
  | `authorizationStateWaitCode` | `wait_code` | Challenge for the authentication code and call `checkAuthenticationCode`; the prompt includes tdlib's delivery type, expected length and resend timeout. |
  | `authorizationStateWaitOtherDeviceConfirmation` | `wait_other_device_confirmation` | Emit an `auth_qr` progress frame for the current link and wait. Every later update in this state emits a replacement frame with its new `auth_sequence`, even if an earlier QR is still displayed. QR frames are non-secret display/progress, not challenges, and need no `answer`. |
  | `authorizationStateWaitRegistration` | `wait_registration` | Challenge to accept the supplied terms (including text, minimum age and popup flag), then for non-empty first name and optional last name; call `registerUser`. Refusal cancels login and sends nothing. |
  | `authorizationStateWaitPassword` | `wait_password` | Try `password_cmd` once for this state occurrence, if configured; otherwise challenge without echo, including only tdlib's hint/recovery metadata, then call `checkAuthenticationPassword`. |
  | `authorizationStateReady` | `ready` | Fetch `getMe`, release the auth lease and return the login result. |
  | `authorizationStateLoggingOut` | `logging_out` | Wait for the next state under the request's existing deadline; do not prompt or declare success. |
  | `authorizationStateClosing` | `closing` | Wait for `Closed` under the existing deadline; do not prompt. |
  | `authorizationStateClosed` | `closed` | Complete/fail work for that client generation and apply the replacement rule below. |

  `--qr` and `--bot` are mutually exclusive (exit 2). They select only the
  transition out of `wait_phone_number`; after a disconnect, a later `login`
  invocation resumes any subsequent current state regardless of those flags.
  No mode switch rewinds tdlib. A credential error while tdlib remains in the
  same state emits non-secret `auth_retry` progress and creates a fresh
  challenge with a new nonce/sequence. A configured hook is not re-run for the
  same state occurrence after its value is rejected: a TTY falls back to an
  interactive challenge and a non-TTY invocation terminates with
  `AUTH_CREDENTIAL_REJECTED` (exit 3). Retries and state transitions never
  reset the one command deadline.

  Credential classification is closed. `credential` is exactly one of
  `app_credentials`, `database_key`, `phone_number`, `bot_token`,
  `email_address`, `email_code`, `authentication_code`, `registration_name`,
  or `password`. For pinned TDLib revision
  `a17f87c4cff7b90b278d12b91ba0614383aaee82`, only the following exact tuples
  are credential rejection. `message` is the complete case-sensitive raw
  `td_api::error.message_`; matching neither trims nor normalizes it. Local
  validation strings come from `Td::get_parameters`, `init_binlog` in
  `TdDb.cpp`, and
  `AuthManager`; RPC errors pass unchanged through
  `AuthManager::on_query_error` and `Td::send_error`.

  | originating td_api function | code | exact message | credential |
  |---|---:|---|---|
  | `setTdlibParameters` | 400 | `Valid api_id must be provided. Can be obtained at https://my.telegram.org` | `app_credentials` |
  | `setTdlibParameters` | 400 | `Valid api_hash must be provided. Can be obtained at https://my.telegram.org` | `app_credentials` |
  | `setTdlibParameters` | 401 | `Wrong database encryption key` | `database_key` |
  | `setAuthenticationPhoneNumber` | 400 | `Phone number must be non-empty` | `phone_number` |
  | `setAuthenticationPhoneNumber` | 406 | `PHONE_NUMBER_INVALID` | `phone_number` |
  | `setAuthenticationPhoneNumber` | 400 | `API_ID_INVALID` | `app_credentials` |
  | `requestQrCodeAuthentication` | 400 | `API_ID_INVALID` | `app_credentials` |
  | `checkAuthenticationBotToken` | 400 | `API_ID_INVALID` | `app_credentials` |
  | `checkAuthenticationBotToken` | 400 | `ACCESS_TOKEN_INVALID` | `bot_token` |
  | `checkAuthenticationBotToken` | 400 | `ACCESS_TOKEN_EXPIRED` | `bot_token` |
  | `setAuthenticationEmailAddress` | 400 | `Email address must be non-empty` | `email_address` |
  | `setAuthenticationEmailAddress` | 400 | `EMAIL_INVALID` | `email_address` |
  | `checkAuthenticationEmailCode` | 400 | `Code must be non-empty` | `email_code` |
  | `checkAuthenticationEmailCode` | 400 | `CODE_INVALID` | `email_code` |
  | `checkAuthenticationEmailCode` | 400 | `EMAIL_VERIFY_EXPIRED` | `email_code` |
  | `checkAuthenticationEmailCode` | 400 | `PHONE_CODE_EMPTY` | `email_code` |
  | `checkAuthenticationEmailCode` | 400 | `PHONE_CODE_INVALID` | `email_code` |
  | `checkAuthenticationEmailCode` | 400 | `PHONE_CODE_EXPIRED` | `email_code` |
  | `checkAuthenticationCode` | 400 | `PHONE_CODE_EMPTY` | `authentication_code` |
  | `checkAuthenticationCode` | 400 | `PHONE_CODE_INVALID` | `authentication_code` |
  | `checkAuthenticationCode` | 400 | `PHONE_CODE_EXPIRED` | `authentication_code` |
  | `registerUser` | 400 | `First name must be non-empty` | `registration_name` |
  | `registerUser` | 400 | `FIRSTNAME_INVALID` | `registration_name` |
  | `registerUser` | 400 | `LASTNAME_INVALID` | `registration_name` |
  | `checkAuthenticationPassword` | 400 | `PASSWORD_HASH_INVALID` | `password` |

  A listed rejection retries only while client generation, auth sequence and
  state occurrence still match and time remains. An intervening auth update is
  authoritative. A tuple absent from this table is never guessed to be a
  credential failure: code 429 is terminal `RATE_LIMITED`, and every other
  code/message pair is terminal `TDLIB_ERROR`.

  Hook fallback is per field and per state occurrence. `api_id_cmd`,
  `api_hash_cmd`, `db_key_cmd`, `password_cmd` and `bot_token_cmd` each run at
  most once for their applicable occurrence. Runner failure or invalid stdout
  falls back on a TTY to the corresponding `api_id`, `api_hash`,
  `database_key`, `password` or `bot_token` challenge; without a TTY it is
  `HOOK_FAILED`. A listed TDLib rejection of a hook/config value never reruns
  the hook: a TTY receives a fresh corresponding challenge, including
  `database_key` for an invalid DB encryption key, while non-TTY receives
  `AUTH_CREDENTIAL_REJECTED`. Rejected interactive phone/email/code/name
  values similarly receive a fresh challenge only on a TTY. No fallback
  persists real secrets or extends the request deadline.
  An `app_credentials` tuple received after parameters were accepted prompts
  for both app fields, intentionally closes the generation, and applies the
  replacement/bootstrap rules before retrying; it never resends a phone/QR/bot
  request with the rejected parameter snapshot.

  `--bot` obtains the token only from `[accounts.<name>].bot_token_cmd` or a
  no-echo `bot_token` challenge. There is no bot-token environment variable,
  plain config value or argv value. The parser recognizes legacy
  `--bot-token <value>` only to return `INSECURE_SECRET_INPUT` (exit 2): it
  consumes and redacts the value before diagnostics/frame/audit construction,
  never authenticates with or persists it, and directs the caller to `--bot`
  plus `bot_token_cmd` or the interactive prompt. A plain `bot_token` config
  key is likewise invalid and is never auto-migrated; users must remove it and
  rotate any token previously exposed through argv or plaintext config.
- **Bot-account caveat**: a bot session has no dialog list, cannot read
  arbitrary chat history, has no server-side search and no contacts — most
  of the read surface (`chats`, `read`, `search`, `unread`, `fetch`) is
  user-account-only. Bots suit narrow send/receive automation in chats they
  are already in; an agent that needs to read should run on a user account.
  A user-account-only command invoked on a bot session fails with exit 2 and
  error code `BOT_UNSUPPORTED`.
- **Every other command** requires the current snapshot to be `ready` at
  admission; anything else exits 3 with `account`, `state` and a generic
  `reason` in error details. If a ready generation loses authorization while
  an auth-bound request or subscription is active, each receives exactly one
  terminal `NOT_AUTHED` error unless already terminal. The reason is
  `authorization_lost`; `updateAuthorizationState` carries no source string,
  so tgcli never claims that another device, a particular session, or an
  operator caused the loss. Active terminals name the first non-ready state
  that caused termination; later commands and `doctor` name their current
  snapshot. The daemon stays up for re-authentication.
- **Client replacement and request correlation**:
  `ClientManager::send(client_id, query_id, fn)` is correlated by the full
  `(client_id, query_id)` pair. Query ids are non-zero and monotonic within a
  client generation; a new generation may restart them at 1. Updates have
  query id 0 and are associated with their source client id before entering
  the auth FSM/update bus. `Closed` fails unresolved queries and subscriptions
  from exactly that generation. Unless daemon shutdown intentionally requested
  the close, the daemon immediately allocates a replacement client id, advances
  `client_generation`, installs an `unknown` snapshot, and drives the new
  client from `wait_tdlib_parameters`; late responses/updates from an older
  client id are discarded and can neither fulfill a new promise nor advance
  its auth state. An intentional daemon close never creates a replacement.
  Request reservation and a generation's close transition remain ordered
  under one lifecycle gate: a send admitted first is resolved normally or
  failed during close, while a later send returns an immediately exceptional
  future. No request can be reserved after that generation's final pending
  sweep.
- **Lifecycle-owned transitions and terminal ordering**: login, logout,
  account removal and daemon shutdown register a lifecycle waiter containing
  exactly `(operation, client_id, client_generation, starting_auth_sequence,
  deadline)` before their first send. Ordinary login transitions are expected
  only while that login owns the auth lease. `logout` and default account
  removal expect `ready -> logging_out -> closing -> closed`, with either
  intermediate state permitted to be omitted; an intentional daemon close
  expects any current state to reach `closing -> closed`, again permitting the
  `closing` update to be omitted. No other request may claim those transitions.

  TDLib response/update ordering is deliberately not prescribed. If `ok`
  arrives before an expected state update, the lifecycle waiter remains
  pending. If an expected state update arrives first, that committed snapshot
  is authoritative and the later response is consumed only for correlation.
  If `closed` arrives first, the receive loop atomically resolves the matching
  lifecycle waiter **before** failing unrelated queries/subscriptions from the
  generation and before allocating a replacement client. A tdlib error wins
  only while the source snapshot is still the waiter's starting occurrence;
  an already committed expected transition wins over its late error response.
  Thus logout's own transition away from `ready` is never reported to its
  waiter as `NOT_AUTHED`, while unrelated auth-bound work still terminates
  once with `authorization_lost`. The waiter, deadline and close sweep share a
  one-shot terminal flag: every legal response/update permutation emits
  exactly one terminal frame, and replacement starts only after that decision.
- **Send lifecycle**: tdlib's `sendMessage` returns immediately with a
  message in `messageSendingStatePending` carrying a *temporary local id*;
  the permanent id arrives via `updateMessageSendSucceeded`/`Failed`.
  `tgcli send` waits for that update and returns the **final** tdlib message
  id — the id that is valid for `wait-for --after` and `msg get`. An
  idempotency key is marked *completed* only on send success, with the final
  id in the recorded result. If `--timeout` expires between dispatch and
  confirmation, the command exits 7 with the temporary id in the error
  details and the key stays *pending* — exactly the unknown-outcome state
  its replay semantics exist for (§6).
- **Logout lifecycle**: `logout` is admitted only from `ready`. After the M1
  destructive gate it calls `logOut` and waits for `Closed` from the same
  client generation; the immediate `ok`, `logging_out` and `closing` states
  are progress, not success. Timeout or generation loss is
  `REMOTE_LOGOUT_UNCONFIRMED`; the daemon does not claim success. Confirmed
  `Closed` returns the logout result, and the normal replacement rule creates
  a fresh unauthenticated client so the daemon remains usable for `login`.
  Account removal uses the same lifecycle waiter and ordering rules; it merely
  suppresses replacement while the target daemon is being quiesced.
- **Files**: `downloadFile` + `updateFile` progress events → progress bar on
  stderr (TTY) or NDJSON progress frames. Uploads via `inputFileLocal` inside
  send requests, same plumbing.
- **Flood waits**: tdlib absorbs some flood waits during its internal
  syncing, but user-initiated request bursts routinely surface error 429 —
  agents should expect exit 5 with `retry_after` and back off.
- **Local DB and persistence**: tdlib persists every chat, message and update
  it observes into its own database and migrates that schema across tdlib
  versions itself — tgcli never defines a message schema and has no
  migration story to maintain for messages. tgcli's own persistent state is
  deliberately trivial: an append-only JSONL audit log (no schema to migrate)
  and a small idempotency store carrying an explicit `schema_version`. The
  per-account daemon (§10) keeps running by default, so the local DB absorbs
  updates continuously; `listen` is a live view over that stream, not the
  persistence mechanism.
- **Local-only reads**: `--local` on `read` sets the `only_local` flag on
  `getChatHistory` — offline mode for agents that must not hit the network.
  There is deliberately no `--local` on `search`: tdlib exposes no
  local-only search for regular chats (`searchChatMessages`/`searchMessages`
  are server-side), and tgcli does not pretend otherwise; offline filtering
  over prefetched history is a post-1.0 idea. `tgcli fetch` is the
  deliberate warming path: it pages `getChatHistory` for one chat to a
  requested depth/date, with progress on stderr. Resume keeps no persisted
  state, but must be gap-aware: the local DB accretes disconnected islands
  of messages (from `search` results, `msg get`, live updates), so the
  resume anchor is the *first gap* found when paging `only_local` history
  down from the newest message — not the globally oldest cached message,
  which could sit below a hole and silently skip unfetched ranges.
- **Options at startup**: tdlib defaults are kept; tgcli uses no
  notification machinery and relies on background updates being processed —
  both already the default behavior.
- **tdlib logging and secret boundary**: before the first client id or tdlib
  request is created, tgcli sets tdlib's process-global verbosity to 1
  (`ERROR`) and never raises it for the daemon lifetime. Levels 3 and above
  include INFO request dumps from `to_string(function)` and can expose bot
  tokens, authentication/email codes, passwords and DB keys; level 2 is
  therefore also the absolute contractual ceiling if the pinned tdlib changes
  the default. `-v` increases only tgcli-owned diagnostic detail and never
  changes tdlib global/tag verbosity or mirrors tdlib INFO/debug output to
  stderr. TDLib ERROR output is routed to the account's rotating `tdlib.log`
  with the same secret-redaction boundary; stderr contains only tgcli-owned
  summaries.

## 9. Configuration, paths, secrets

XDG layout, one subtree per account:

```
~/.config/tgcli/config.toml                    global + per-account config
~/.local/share/tgcli/accounts/<name>/tdlib/    tdlib database + files
~/.local/state/tgcli/accounts/<name>/          audit.log, idempotency.db, tdlib.log
~/.local/state/tgcli/removals/                 removal audit + durable tombstones
$XDG_RUNTIME_DIR/tgcli/<name>.ctl              bootstrap stop socket
$XDG_RUNTIME_DIR/tgcli/<name>.sock             daemon socket
```

When `XDG_RUNTIME_DIR` is unset (macOS, most containers and CI sandboxes),
the socket falls back to `$TMPDIR/tgcli-<uid>/<name>.sock` (then
`/tmp/tgcli-<uid>/`), a `0700` directory whose ownership is verified before
use. Socket paths must fit `sun_path` (~104 bytes); account names are
length-validated accordingly. `audit.log` and `tdlib.log` rotate by size
(default 32 MiB, keep 4).

The `.ctl` endpoint and `<account-state-dir>/daemon.lock` form the bootstrap
compatibility surface described in §10. They are account-scoped even though
they are not part of the main JSONL protocol.

`config.toml`:

```toml
default_account = "main"

[accounts.main]
api_id       = 12345                             # plain values are the normal path;
api_hash     = "0123456789abcdef"                # `tgcli login` saves them on first run
# api_id_cmd   = "pass show tg-api-id"           # alternative: source app creds from a command
# api_hash_cmd = "pass show tg-api-hash"
db_key_cmd   = ""                                # optional tdlib database_encryption_key source
password_cmd = ""                                # optional 2FA password source
bot_token_cmd = ""                               # optional bot token source for `login --bot`
allow_write  = false                             # standing write grant for this account (§6);
                                                 # default deny — grant per call/env otherwise
# idle_exit  = 300                               # daemon exits after N idle seconds (default: stays up)
```

Environment (env beats config; for the write gate, §6's
deny-overrides-grants rule applies): `TGCLI_ACCOUNT`, `TGCLI_API_ID`,
`TGCLI_API_HASH`, `TGCLI_ALLOW_WRITE` (`1` grant / `0` hard deny),
`TGCLI_MEDIA_DIR` (default output directory for `download` when `-O` is
omitted; falls back to the current directory).

Each variable is read where it acts. `TGCLI_ACCOUNT`, `TGCLI_ALLOW_WRITE`,
`TGCLI_MEDIA_DIR` and the working directory are **client-side**: the client
resolves `TGCLI_ACCOUNT` into the request's top-level routed account and folds
the other values into the request context (§10), since the long-lived daemon
cannot see an invoking shell's environment. `TGCLI_API_ID`/`TGCLI_API_HASH` are
consumed at **daemon spawn** (the auto-spawned daemon inherits the spawning
client's environment); changing them takes effect on daemon restart.

Config reads and writes are fail-closed. The `tgcli` config directory is a
current-uid `0700` directory. An existing `config.toml` must be a current-uid,
non-symlink regular file with mode exactly `0600` and link count 1; a missing
file is created as `0600`. Every process that can mutate it first opens
`$XDG_CONFIG_HOME/tgcli/config.lock` as a current-uid, non-symlink regular
file with mode `0600` and link count 1 and holds an exclusive `flock`. While
holding that lock, mutation opens the directory and files relative to the
verified directory, writes a uniquely named `O_CREAT|O_EXCL` non-symlink
temporary file in the same directory, `fsync`s it, atomically renames it over
`config.toml`, then `fsync`s the directory. The mutation preserves every
parsed key/table outside the fields it changes; comments, whitespace and key
ordering are not contract data. Parse, ownership, permission, write or sync
failure leaves the old path in place and no partially written file is used.
Known fields are type-checked. Hook strings are absent when empty; a non-empty
`api_id_cmd`/`api_hash_cmd` is mutually exclusive with its plain counterpart.
`idle_exit`, when present, is a positive integer number of seconds.
`default_account` must name an existing account unless there are no configured
accounts. The file is at most 1 MiB. Unknown keys/tables are retained and do
not by themselves invalidate the file.

A parsed snapshot identity is the lowercase SHA-256 of the complete file bytes
plus the opened file's `(device, inode, size, ctime-nanoseconds)` tuple; absence
has the distinct identity `missing`. Its exact non-missing serialization is
`sha256:<hex>;dev:<device>;ino:<inode>;size:<size>;ctime_ns:<ctime>`, where
`hex` is 64 lowercase hexadecimal characters and each remaining value is
unsigned decimal without a leading zero except zero itself. It contains no
spaces. Every config-changing plan records that
identity. After confirmation, the executor acquires `config.lock`, reopens and
validates the file, and requires the identity, resolved target/default and
resolved data/state roots to equal the plan. Any difference is
`CONFIG_CONFLICT` before audit intent or mutation. It does not re-resolve the
target or silently merge against the newer snapshot; the caller must plan and
confirm again. The lock remains held through the atomic replacement, so
`account add|use|remove` and credential materialization cannot lose one
another's updates.

Implicit first-run `main` is virtual until app credentials are fully resolved.
Immediately before the first `setTdlibParameters`, the login executor acquires
`config.lock`, repeats its snapshot check, and atomically creates
`[accounts.main]` with `allow_write = false`; it also assigns
`default_account = "main"` only when there was no default. App credentials
entered through challenges are persisted together in that same mutation.
Environment- or hook-sourced credentials create the minimal account/default
entry but are never persisted as values. Cancellation, hook failure or
validation failure before this materialization does not create a previously
absent file and leaves an existing file unchanged: no account table or default
is added. Failures after materialization do not roll it back; persisted
prompted credentials remain until explicitly edited. A CAS conflict is
`CONFIG_CONFLICT`, sends no parameters, and requires a fresh invocation.

The daemon runs a monotonic config poll every second, including while it has
no connections or admitted requests, and also forces the same check before
each new admission. For a file at or below the size limit, an atomic
replacement is detected, fully parsed and either published or rejected within
two seconds of the rename unless the filesystem itself fails the read. The
daemon builds a complete immutable snapshot before publishing it. A request keeps
the snapshot with which it was admitted, including hook commands, write
authority and `idle_exit`; a successful reload is visible only to later
requests. On an invalid reload the daemon retains the last-good snapshot for
non-safety settings, reports `CONFIG_INVALID` diagnostics, and marks config
write authority invalid. In that condition a last-good `allow_write = true`
is never a grant: only a per-call/request grant can authorize an M1
destructive operation, and `TGCLI_ALLOW_WRITE=0` still overrides it. A command
that would mutate the invalid config fails instead of overwriting it. With no
last-good snapshot, commands needing config fail `CONFIG_INVALID`; local
`doctor` and `daemon status` remain available. `account show` is a
config-global read of the current on-disk file and returns `CONFIG_INVALID`
rather than the daemon's last-good snapshot. Gate and `idle_exit` changes
therefore apply to new requests without restart or traffic; while already
idle, the new deadline is recomputed from the last active-to-zero transition.
The watcher itself is not activity. tdlib parameters
(`api_id`/`api_hash`, DB key) apply to a replacement client generation or
daemon restart, never by mutating a live generation.

Credentials policy — two classes, treated differently:

- **App credentials (`api_id`/`api_hash`)** are *not* account secrets: they
  identify the application, grant no account access without the user's own
  auth, and are less sensitive than the tdlib directory sitting next to them
  (which holds the MTProto auth key — the real crown jewel). They live as
  plain values in `config.toml`; `tgcli login` prompts for them on first run
  and persists those prompted values. `TGCLI_API_ID`/`TGCLI_API_HASH` are
  ephemeral overrides. `api_id_cmd`/`api_hash_cmd` hooks remain available for
  those who prefer a secret store.
  tgcli does not embed shared app credentials in the binary: publishing an
  api_hash is against Telegram's guidance and one abusive user could get the
  shared app id rate-limited or banned for everyone.
- **Real secrets (2FA password, DB encryption key, bot token)** are accepted
  only via `*_cmd` config hooks or an applicable no-echo interactive prompt —
  never via argv or environment and never written to disk by the tool. Bot
  tokens additionally have no plain config form.

`*_cmd` values are trusted user-configured shell programs, not sandboxed
expressions. Each is executed as `/bin/sh -c <configured-value>` in a new
process group, with working directory `/`, stdin from `/dev/null`, captured
stdout/stderr, and only this inherited allowlist when set: `HOME`, `PATH`,
`LANG`, `LC_ALL`, `LC_CTYPE`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`,
`XDG_STATE_HOME`, `XDG_CACHE_HOME`, `XDG_RUNTIME_DIR`, `GNUPGHOME`,
`PASSWORD_STORE_DIR`, and `SSH_AUTH_SOCK`; all `TGCLI_*` variables and every
other variable are removed. If `PATH` was unset it is `/usr/bin:/bin`.
Execution gets the smaller of 10 seconds and the invoking request's remaining
deadline; startup hooks without a request get 10 seconds. Timeout kills the
whole process group. Stdout and stderr are each capped at 64 KiB; overflow,
signal termination, timeout or non-zero exit is `HOOK_FAILED`.

Successful stdout must contain exactly one non-empty value optionally followed
by one LF (or CRLF): embedded CR/LF/NUL and additional bytes are rejected, and
no whitespace is trimmed. `api_id_cmd` is additionally parsed as a canonical
positive decimal integer fitting tdlib's signed 32-bit field. The other hook
values are opaque bytes subject to their tdlib field's validation. Hook stdout,
stderr and the configured command text are never copied into frames,
diagnostics or the audit log; failures expose only the hook field, failure
class, and numeric exit/signal status when applicable. Secret buffers are
cleared after the corresponding tdlib request has taken ownership.

Protection of the account state rests on file permissions (config `0600`,
account dirs `0700`) and optionally on tdlib's at-rest encryption via
`db_key_cmd` — which covers the auth key and message DB, a strictly more
valuable target than the app credentials.

## 10. Process model: per-account daemon

Hard tdlib constraint: **one tdlib database directory — one client process**.
tgcli embraces it instead of working around it: the tdlib client lives in a
per-account daemon from day one, and every invocation is a thin client over
the account's unix socket. Consequences: ~zero CLI startup latency (no tdlib
DB open per command), safe concurrent invocations, any number of simultaneous
`listen`/`wait-for` subscribers, and a local DB that stays continuously warm.

- **Exact routing and auto-spawn.** Account client-local commands `login`,
  `logout`, `me` and the normal `doctor` probe route to the selected account
  daemon and auto-spawn it when absent; `doctor` uses its existing local result
  only if connection/spawn is unavailable. `account remove <name>` routes only
  to the positional target:
  default removal auto-spawns that target only when it must inspect/revoke a
  possible remote session; `--keep-session` never spawns and instead quiesces
  the target if it is already running. `account add|list|show|use`,
  `daemon status`, `daemon stop`, and every `--dry-run` are config/control-
  global and never auto-spawn an account daemon. `daemon restart` is the sole
  control operation that starts an absent daemon. `daemon run` stays in the
  foreground and is never client-dispatched. A command in the spawn set that
  finds no live socket forks and re-execs `tgcli daemon run --account <name>`,
  waits for a readiness handshake, and proceeds. Spawn races are settled by an
  `flock` on the target account dir: the loser connects to the winner. The
  client computes the §11 routing value once; that same immutable value selects
  the socket, populates `Request.account`, selects any spawn/control surfaces,
  and becomes the frozen account in `--no-daemon` mode. There is no separate
  frame-account option, argument, environment read, or caller override after
  routing.
- **Lifecycle.** The daemon keeps running by default — a continuously warm
  DB is a feature, not a leak; `idle_exit = <seconds>` in the account config
  opts into terminate-when-idle. `tgcli daemon status|stop|restart` manage
  it; `tgcli daemon run` stays in the foreground for systemd user units,
  containers, and debugging. `status` and `stop` never auto-spawn: status of
  an absent daemon is successful `running:false`, while stop of an absent
  daemon is `DAEMON_NOT_RUNNING` (exit 4). Restart starts an absent daemon or
  gracefully replaces a running one and succeeds only after the replacement
  Hello matches; old-daemon shutdown and replacement readiness share the one
  request deadline. `daemon status|stop|restart --no-daemon` retains M0's
  `USAGE` failure (exit 2, exact empty details object); `doctor --no-daemon`
  remains the in-process diagnostic path.
  Running status is reported only after the frozen lock/socket identity and
  current Hello are verified. A missing surface is absent; a malformed,
  foreign-owned, replaced or internally inconsistent surface is
  `DAEMON_CONTROL_FAILED`, never silently removed or reported as running.

  An admitted request counts as activity until its sole terminal frame; time
  spent waiting on a hook, tdlib response or challenge is therefore active.
  Each live subscription counts from admission through planned expiry,
  cancellation, disconnect or terminal error. A socket connection with no
  admitted request and background tdlib updates/auth waits without a login
  owner do not count. The idle clock starts from zero when all request and
  subscription counts become zero, and any new admission cancels it. A valid
  reload of `idle_exit` applies to this accounting for later admissions; when
  already idle, its new duration is measured from the most recent transition
  to zero, so shortening it may request immediate graceful shutdown. Missing
  `idle_exit` disables idle shutdown.
- **Protocol.** Main protocol version 2 is JSONL over a `SOCK_STREAM` unix
  socket. Every normal Request frame has exactly the six top-level fields
  `type`, `id`, `account`, `command`, `args`, and `context`; no extension
  fields are accepted. For example:

  ```json
  {"type":"request","id":42,"account":"work","command":["me"],"args":{},"context":{"tty":false,"json":true,"yes":false,"dry_run":false,"timeout":60,"cwd":"/srv/agent","media_dir":null,"write_authority":"unset"}}
  ```

  `account` is the already-resolved routed account name, as a top-level string;
  it is never supplied through `args` or `context`. It must satisfy §11's
  account-name grammar during strict frame parsing. A missing `account`, an
  unknown top-level field, a non-string `account`, or a syntactically invalid
  account makes the whole v2 Request a malformed protocol frame: the parser
  does not produce a Request, the server sends the existing connection-scoped
  `USAGE` error with frame id 0 and exact empty details, and the server closes
  the connection. This path never becomes `ACCOUNT_NOT_FOUND` or
  `ACCOUNT_MISMATCH`.

  Strict frame parsing does not establish connection-sequence eligibility. A
  syntactically valid Request received before the client has completed a
  matching v2 Hello is rejected by the existing Hello-first sequence gate with
  connection-scoped `USAGE`, frame id 0, exact empty details and connection
  close. That gate runs before account comparison, so even a pre-Hello Request
  whose valid `account` differs from the daemon is not `ACCOUNT_MISMATCH`.

  The remaining Request fields are the uint64 id, command path, normalized
  args, and client context — TTY-ness, `--json`, `--yes`, `--dry-run`, timeout,
  the client's cwd and `TGCLI_MEDIA_DIR` (for output paths), and a **tri-state
  write-authority field** (`grant` / `deny` / `unset`): the client folds
  `--allow-write` and `TGCLI_ALLOW_WRITE` into it, because the daemon cannot
  see the invoking shell's environment, and the daemon combines it with the
  account config per §6 (explicit deny > any grant > default deny). Response
  frames: `result`,
  `error`, `item` (streams), `progress`, and `challenge`. Interactive flows
  are challenge/response: the daemon asks (login values or destructive
  confirmation), the client prompts on *its* TTY and answers. A challenge
  payload always contains `kind`, a random 128-bit lowercase-hex `nonce`, a
  per-request `sequence` starting at 1, and `client_generation` plus
  `auth_sequence`; its answer repeats all four identity fields and contains
  exactly one of `value` or `cancelled: true`. The generation/sequence are
  integers for authentication and null only for a destructive confirmation
  that has no live auth client. Secret
  kinds are rendered without echo and never enter stdout. With no TTY the
  daemon emits no challenge: missing auth input is `AUTH_INPUT_REQUIRED`
  (exit 3), while missing destructive confirmation is
  `CONFIRMATION_REQUIRED` (exit 6). No empty or synthetic answer is created.
  Thus non-TTY `login --bot` without `bot_token_cmd` fails before any auth
  request. `auth_qr` is a progress payload
  `{"kind":"auth_qr","auth_sequence":N,"link":"tg://…"}`, rendered on
  stderr in human or JSON progress form; it is not a challenge. Multiple
  requests are served concurrently; a slow download never blocks a `send`
  from another shell.

  A daemon freezes its one valid account name from `daemon run --account` at
  startup. Only after a matching v2 Hello and the existing connection-sequence
  checks accept the parsed frame as the connection's next Request does the
  daemon compare `request.account` with that frozen name; the comparison is
  then the first request-specific action. It occurs before any request-caused
  config read or snapshot observation, hook execution, auth snapshot or
  TDLib-state observation, account data/state/audit/media path derivation or
  filesystem operation, activity admission, or dispatcher lookup. A mismatch
  emits exactly one terminal Error for that request id: `ACCOUNT_MISMATCH`,
  exit 4, with exact redacted details
  `{"requested_account":<request.account>,"daemon_account":<frozen-name>}`.
  It emits no other request frame and closes that connection immediately after
  the terminal write; the request is never admitted and cannot be followed by
  another Request on that connection. Other connections and account daemons
  are unaffected. The already-open transport and daemon startup surfaces are
  not request observations.

  Every challenge has exactly
  `{"kind":string,"nonce":string,"sequence":integer,
  "client_generation":integer|null,"auth_sequence":integer|null,
  "secret":boolean,"prompt":string,"details":object}`. String-answer kinds are `api_id`,
  `api_hash`, `database_key`, `phone_number`, `authentication_code`, `email_address`,
  `email_code`, `password`, `bot_token`, `registration_first_name`, and
  `registration_last_name`; boolean-answer kinds are `registration_terms`
  and `destructive_confirmation`. `api_hash`, `database_key`, both code kinds,
  `password` and `bot_token` set `secret:true`; all others are false. Details are exact:
  `authentication_code` has `delivery_type`, `expected_length` (integer or
  null), and `resend_timeout`; `email_code` has `address_pattern` and
  `expected_length`; `password` has `hint`, `has_recovery_email`,
  `has_passport_data`, and `recovery_email_pattern`; `registration_terms` has
  the plain UTF-8 `text`, `min_user_age`, and `show_popup`; and
  `destructive_confirmation` has `action` and the fully resolved `target`.
  Other details are empty. `delivery_type` is exactly `telegram_message`,
  `sms`, `sms_word`, `sms_phrase`, `call`, `flash_call`, `missed_call`,
  `fragment`, `firebase_android`, or `firebase_ios` for the pinned variants;
  an unknown nested type fails closed as `UNSUPPORTED_AUTH_STATE`. Answers do
  not repeat prompts/details and are exactly
  `{"nonce":string,"sequence":integer,"client_generation":integer|null,
  "auth_sequence":integer|null,"value":string|boolean}` or the same identity
  plus `"cancelled":true`. Retry progress is exactly
  `{"kind":"auth_retry","auth_sequence":N,"state":string,
  "credential":credential,"tdlib_code":integer}` and contains no submitted
  value or tdlib message.
- **Version handshake.** The connect handshake carries binary and protocol
  versions from both sides. Protocol v2 is the first main-protocol version
  whose Request carries the required routed `account`; v1 Request frames do
  not satisfy the v2 parser, and v2 frames are never sent after a v1 Hello.
  On any mismatch the client — freshly exec'd, so authoritative — asks the
  daemon to shut down gracefully and respawns it from the new binary; streams
  on the old daemon receive a terminal error frame. A v1↔v2 protocol mismatch,
  whether or not the binary versions also differ, is recovered only through
  the frozen lock plus authenticated `.ctl` token-stop path below, before any
  normal Request is sent. Thus an old daemon remains replaceable without
  accepting a v2 Request, and a v2 daemon remains replaceable by a v1 client
  that cannot construct one. A same-protocol v2 binary mismatch may use a
  normal `daemon stop` Request only when its `account` is the same routing
  value that selected the socket; the frozen control path remains valid.
  Frames and curated schemas are never mixed across versions.
- **Frozen bootstrap/control contract.** Main-protocol evolution must not be
  required to stop an older daemon. This contract starts at the v1
  pre-release baseline and remains readable and operational across all later
  main-protocol versions that can be encountered during an in-place upgrade:
  - the account lifetime file is exactly
    `<account-state-dir>/daemon.lock`, a non-symlink regular file owned by the
    current uid, mode `0600`, link count 1. Its one-line ASCII record is
    `tgcli-lock-v1 <pid> <process-start> <control-token>\n`, with no extra
    bytes. Every shown space is exactly one ASCII space, and the final byte is
    exactly one LF. `<pid>` is a canonical positive ASCII decimal PID including
    1: no sign or leading zero, and its value must fit `pid_t`.
    `<process-start>` is platform-specific. On Linux it is
    `linux:<proc-stat-field-22>`, where field 22 is a canonical positive ASCII
    decimal integer fitting `uint64_t`. On macOS it is
    `macos:<start-seconds>:<start-microseconds>`; seconds are a canonical
    positive ASCII decimal integer fitting `uint64_t`, and microseconds are a
    canonical non-negative ASCII decimal integer from 0 through 999999. A zero
    value is written only as `0`; every other numeric field has no leading zero.
    A platform rejects the other platform's process-start form.
    `<control-token>` is exactly 32 lowercase hexadecimal characters encoding
    128 random bits;
  - the daemon holds both a whole-file exclusive `flock` and a whole-file
    POSIX write lock for its lifetime. A client accepts the record only when
    the POSIX lock's kernel owner PID, the live process-start value and the
    record agree. The client keeps that exact lock-file inode open while
    observing shutdown. Numeric PIDs from this record are never signalled;
  - the control endpoint is exactly
    `$XDG_RUNTIME_DIR/tgcli/<account>.ctl` (with §9's runtime fallback), an
    `AF_UNIX`/`SOCK_DGRAM` filesystem socket in the verified `0700` runtime
    directory, owned by the current uid, mode `0600`, link count 1. The stop
    datagram is exactly the record's 32 ASCII token bytes, with no prefix,
    suffix or newline. A matching datagram requests graceful stop; malformed
    or non-matching datagrams are ignored and the endpoint sends no reply;
  - before using this control path, the client also verifies that its
    connected main-socket peer PID is the verified lock owner and that the
    main and control path identities have not changed. A missing, malformed,
    foreign, unlocked or ambiguous bootstrap surface fails closed. Once a
    target was verified, disappearance or replacement is treated as another
    client already performing the same restart;
  - shutdown observation, reconnect/spawn and the replacement Hello share one
    monotonic deadline. A replacement is usable only after the old lock and
    both old socket identities disappear and the new daemon completes the
    current main-protocol handshake.

  Future implementations may add a new bootstrap record/control version but
  must retain the `tgcli-lock-v1` parser and token-stop behavior until no
  supported direct upgrade can encounter a daemon that only implements this
  baseline. Renaming a main Hello or changing its encoding therefore does not
  remove the shutdown path. Unit coverage accepts and validates PID 1 in the
  frozen record and owner-matching helpers. An executable daemon-as-PID-1
  namespace test additionally requires the test host to permit user and PID
  namespace creation; restricted hosts cannot exercise that kernel path.
- **Challenge ownership and terminality.** A challenge belongs to the tuple
  `(connection, request_id, client_generation, auth_sequence, nonce,
  sequence)`, and a request has at most one
  current challenge. Only an exact answer on the owning connection is
  accepted, once, and acceptance consumes that challenge before processing
  its value. A duplicate of an already consumed exact answer and an answer
  for an older sequence are ignored; they never answer the current challenge.
  A future sequence, wrong nonce for the current sequence, malformed answer,
  or answer for an unknown request is `PROTOCOL_ANSWER_INVALID` (exit 2) for
  the sender and cancels only a request owned by that connection. An answer
  sent on a different connection cannot identify or affect the actual owner.
  The implementation retains bounded per-request consumed-challenge metadata
  until the request terminates so duplicate classification is deterministic.

  Every auth update, including a repeated tdlib state, advances
  `auth_sequence`, invalidates the old challenge and issues a new one if that
  state still needs input. A bootstrap response never supersedes an accepted
  update. An answer whose
  generation or auth sequence is old is ignored as stale and never reaches
  TDLib. Answer acceptance and deadline expiry serialize under the request
  owner's mutex. An answer is eligible only while `now < deadline`; equality
  or later makes timeout win. Acceptance atomically consumes the challenge and
  reserves its single TDLib auth query before releasing that mutex, so neither
  the deadline nor a second answer can send another function.

  Explicit `cancelled: true`, Ctrl-C/EOF at the prompt (best-effort
  cancellation followed by disconnect), the one monotonic request deadline,
  daemon shutdown, or owner disconnect invalidates the current challenge.
  Auth cancellation returns `AUTH_CANCELLED` (exit 3) when the connection is
  still writable; confirmation cancellation returns
  `CONFIRMATION_REQUIRED` (exit 6). No answer or cancellation can produce a
  second terminal frame. The deadline is computed once at request admission
  (60 seconds by default for M1 commands) and is shared by hooks, prompts,
  tdlib queries, retry prompts, state waits, logout confirmation and daemon
  lifecycle waits; progress and answers do not extend it.

  `login` additionally takes a per-account auth lease: one login flow at a
  time. A concurrent login gets `AUTH_FLOW_IN_PROGRESS`; other auth-required
  commands get `NOT_AUTHED` naming the current state. Owner cancellation or
  disconnect releases the lease immediately only when no auth query was sent.
  If a query is already in flight, ownership becomes `orphaned`: the lease
  remains held, no prompt/progress/terminal is emitted, and it is released only
  when that correlated response settles or a newer auth update supersedes it.
  If a connected request's deadline arrives first it emits its one `TIMEOUT`;
  in either connected or disconnected cases the daemon intentionally closes
  that generation in the background, waits for its `Closed`, replaces it, and
  only then releases the lease. It never sends a second auth function into an
  unknown state. A later login resumes the resulting current snapshot. A destructive-confirmation
  challenge dies with its request: cancellation/disconnect before the exact
  affirmative answer means no audit intent and no Telegram call or local
  deletion.
- **Shutdown.** On SIGTERM/SIGINT or `daemon stop`: stop accepting requests;
  finish every active request with one terminal `DAEMON_SHUTDOWN` error (exit
  1, details `{"reason":"daemon_shutdown"}`) unless it already emitted its
  terminal frame. The sole exception is an audit-fatal invocation whose
  post-intent outcome cannot be made durable: §6 requires EOF without an
  unaudited terminal. Then call tdlib `close()` and wait for
  `authorizationStateClosed` so the database is flushed cleanly; then exit.
  Admission and shutdown designation are atomic: once a `daemon stop`
  request is admitted, a racing signal or external stop cannot replace its
  sole successful `{"stopping":true}` result with the shutdown error, and
  teardown waits for that terminal before EOF. The systemd user unit uses
  readiness notification. Active `listen`/`wait-for` subscribers count as
  activity for `idle_exit`.
- **Security.** Socket at `$XDG_RUNTIME_DIR/tgcli/<name>.sock` (fallback per
  §9), mode 0600, peer-uid check (`SO_PEERCRED` on Linux,
  `getpeereid()`/`LOCAL_PEERCRED` on macOS). The write gate (§6) is evaluated
  in the daemon: a write-tier request runs only if the account config grants
  `allow_write` or the request itself carries a grant, and never under an
  explicit deny. Per-invocation grants are client declarations within the
  same uid trust domain — the gate is a guard against accidents, not against
  a hostile local process.
- **`--no-daemon`.** Debugging escape hatch: runs dispatch and handlers
  in-process over the same code path minus the socket; refuses to start if a
  daemon already holds the account lock. It creates one tdlib client
  generation, runs the command through its terminal frame, closes cleanly and
  never applies `idle_exit`.

## 11. Multi-account

`tgcli account add work && tgcli --account work login` — each account has an
isolated config section, tdlib dir, state dir, and socket. `account use` sets
`default_account`. Nothing is shared between accounts, including idempotency
stores and ordinary audit logs. The one explicit exception is §6/§11's global
removal journal and tombstone directory: it contains only removal metadata
needed to survive deletion of a target account and never shares TDLib data,
credentials, hooks, auth snapshots or command results between accounts.

Normal client-local command routing is exactly explicit `--account` >
`TGCLI_ACCOUNT` > `default_account` > the first-run implicit name `main`.
Names are 1–32 ASCII
characters from `[A-Za-z0-9_-]`. Except for first-run `main`, a routed name
must have an `[accounts.<name>]` table or the command returns
`ACCOUNT_NOT_FOUND`; implicit `main` is materialized at the precise §9
pre-parameter point. Before that materialization, implicit `main` is accepted
only by `login`, `doctor`, and daemon lifecycle inspection/control; another
routed command is `ACCOUNT_NOT_FOUND`. The selected name is one immutable
routing value. It determines the socket, control/lock surface and, for a
matching request, one config subsection, tdlib directory, state directory and
audit log. The client copies that exact value to the v2 Request's top-level
`account`; it cannot select one socket and encode another account. A daemon
receives only its frozen account identity and matching account snapshot. Even
if a misrouted or independently constructed client sends a valid v2 Request
for another account over that socket, §10's pre-admission
`ACCOUNT_MISMATCH` rejection prevents either account's config, hook, auth
state, paths, activity or dispatch from being observed through that request.
No result, hook, auth snapshot or filesystem path is shared across accounts.

`account list` is config-global and takes no name. `account add|show|use|remove`
take their target from the positional `<name>`; supplying global `--account`
to an `account` subcommand is `USAGE` rather than a silently ignored second
target. `TGCLI_ACCOUNT` and the current default do not change that positional
target. `account list` is bytewise name-sorted. `account add` creates only the
config table (`allow_write = false`) and makes it default iff there was no
configured account; an existing name is `ACCOUNT_EXISTS` (exit 2), with no
merge. `show`, `use` and `remove` of a missing name are `ACCOUNT_NOT_FOUND`
(exit 4). `use` changes only `default_account`; an explicit flag/environment
continues to override it and no daemon or session is moved.

An absent config and a valid config with no accounts are both an empty account
set: `account list` returns exactly `{"items":[],"next":null}`; `account add`
and implicit-main `login` may create the first account; `account show|use|remove`
return `ACCOUNT_NOT_FOUND`; and local `doctor` plus `daemon status` remain
usable. Config-global commands parse the current file themselves and never
select a daemon's last-good snapshot. In particular, `account show` during an
invalid reload returns `CONFIG_INVALID`, even if a target daemon is healthy.

Removing a non-default account rejects `--reassign-default`. Removing the
default while other accounts remain requires
`--reassign-default <existing-other-name>`; omission is
`DEFAULT_REASSIGNMENT_REQUIRED` (exit 2), and a missing/same target is
`ACCOUNT_NOT_FOUND`/`USAGE`. Removing the sole configured account clears
`default_account` and rejects reassignment. These checks and the target paths
are resolved before confirmation; no lexical or last-used implicit
reassignment occurs.

`account remove` is destructive (§6): it deletes the account's local tdlib,
state and config entry, including the MTProto auth key. By default it first
loads/contacts that account and, if `ready`, calls `logOut`; a returned `ok` is
not confirmation. Local deletion/config mutation begins only after
`authorizationStateClosed` for the same `(client_id, client_generation)` or
after a fresh client opens the existing database with `setTdlibParameters`
and reaches `wait_phone_number`, which proves there is no authorized session.
That second case produces `remote_logout:"not_present"`. Timeout,
transport/tdlib error, an intermediate `logging_out`/`closing` state at the
deadline, or loss/replacement of the correlated client is
`REMOTE_LOGOUT_UNCONFIRMED` and aborts all local deletion and reassignment.
Partially authenticated states (`wait_code`, `wait_email_address`,
`wait_email_code`, `wait_registration`, `wait_password`,
`wait_premium_purchase` and
`wait_other_device_confirmation`) are not proof that no server session
exists; default removal fails closed in them. Once either permitted proof is
recorded, removal gracefully stops the target daemon/any replacement client,
waits for its intentional `Closed`, and only then starts local deletion.

`account remove <name> --keep-session` deliberately skips `logOut`, cleanly
closes/stops the target client without revoking Telegram authorization, then
deletes local state. The flag belongs only to `account remove`; `tgcli logout`
always requests server-side logout and keeps the account config/local
directory for later login. Confirmation and dry-run state either
`remote_logout:true` or `keep_session:true` explicitly. After remote logout is
confirmed (or deliberately skipped), a later local deletion/config failure is
reported with the exact `removed`/`retained` path lists; it is never described
as an unconfirmed remote logout. Deletion holds the target account lock,
opens the verified data/state parent directories, refuses a target root that
is a symlink, foreign-owned or replaced since planning, and never follows a
symlink while traversing beneath either root. A mismatch is
`LOCAL_CLEANUP_FAILED`, not permission to delete a replacement path.

Removal is a checkpointed transaction, not an atomic claim. After confirmation
the executor holds `config.lock` and performs exactly: (1) revalidate the
planned config/root identities; (2) create and sync the global tombstone at
`planned`; (3) append and sync the global audit intent, then sync tombstone
stage `intent_synced`; (4) when `logOut` is needed, sync
`remote_logout_send_started` before the send, then sync `remote_confirmed`
after correlated `Closed`; otherwise sync `remote_not_present` after its proof
or `remote_kept` for `--keep-session`; (5) sync `client_close_started`, quiesce
the target, then sync `client_closed`; (6) sync `config_remove_started`,
atomically remove the config entry/default, then sync `config_removed`; (7)
sync `data_remove_started`, perform descriptor-relative deletion and parent
directory sync, then sync `data_removed`; (8) do the corresponding
`state_remove_started`/`state_removed` sequence; (9) append and sync the audit
outcome; and (10) sync the terminal `outcome_synced` tombstone before
returning. Every `*_started` checkpoint is durable before its potentially
mutating action; every completion checkpoint is durable only after the action
and required file/directory sync.
The tombstone contains exactly
`{"schema_version":1,"invocation_id":string,"account":string,
"stage":audit_stage,"completed_stages":audit_stage[],
"next_stage":audit_stage|null,"plan":account_remove_plan,"config_snapshot":string,
"data_root":root_identity|null,"state_root":root_identity|null}`, with no
extension fields. `account_remove_plan` is the exact removal plan in §5.1,
`root_identity` is that plan's exact four-key root object, stages use §5.1's
enum, and the two identities are those captured during planning. It contains
no credential or hook material.

A command that names or routes an account resolves a matching nonterminal
tombstone before consulting `[accounts.<name>]`, including `account add|show|use`
and client-local routing; thus a crash after `config_removed` never becomes
`ACCOUNT_NOT_FOUND` or permits a replacement account. Those commands return
`REMOVAL_INCOMPLETE`; only a repeated `account remove <name>` may resume.
Recovery never mutates automatically:
a repeated remove must obtain a fresh authority decision and confirmation,
hold `config.lock`, and satisfy this exact table. “Captured” means path,
device, inode and owner all equal the non-null `root_identity`; “planned
absent” means the plan stored null.

| latest durable stage | required config entry/default | required data path | required state path | deterministic next action |
|---|---|---|---|---|
| `planned`, `intent_synced` | target entry and default exactly match the captured config snapshot | captured, or absent only if planned absent | captured, or absent only if planned absent | find the global audit intent by `invocation_id`, append/sync it only if absent, record `intent_synced`, then re-evaluate remote state |
| `remote_logout_send_started` | same captured entry/default | captured/planned absent | captured/planned absent | reopen the captured DB: wait for an in-progress close; if `wait_phone_number`, record `remote_not_present`; if `ready`, issue a newly confirmed `logOut`; otherwise block |
| `remote_confirmed`, `remote_not_present`, `remote_kept`, `client_close_started`, `client_closed` | same captured entry/default | captured/planned absent | captured/planned absent | finish quiescing if needed, then start config removal |
| `config_remove_started` | either exact captured entry/default or target absent with the plan's post-removal default | captured/planned absent | captured/planned absent | retry the atomic config mutation in the former case; record `config_removed` in the latter |
| `config_removed`, `data_remove_started` | target absent and default equals `reassign_default` (including null) | captured, planned absent, or absent after `data_remove_started` | captured/planned absent | finish or recognize data deletion, then record `data_removed` |
| `data_removed`, `state_remove_started` | target absent and planned default | absent | captured, planned absent, or absent after `state_remove_started` | finish or recognize state deletion, then record `state_removed` |
| `state_removed` | target absent and planned default | absent | absent | find the global audit outcome by `invocation_id`, append/sync it only if absent, then record `outcome_synced` |
| `outcome_synced` | not consulted | not consulted | not consulted | terminal; no recovery |

A present root with a different path/device/inode/owner, an unplanned absence
before its `*_started` checkpoint, a resurrected account entry, or a default
different from the table blocks with `REMOVAL_INCOMPLETE`; recovery never
deletes or logs out that object. The `remote_logout_send_started` boundary is
explicitly uncertain: recovery observes TDLib state and never assumes the send
did or did not reach Telegram.

The captured data and state roots must be distinct descendants of their
verified parents, must not themselves be mount points, and must have the same
device id as their respective parent. Descriptor-relative traversal refuses
every entry whose device id differs from that root, so account removal never
crosses a mount/device boundary. These checks occur during planning and again
immediately before deletion; a difference is `LOCAL_CLEANUP_FAILED` without
traversing it.

## 12. Agent ergonomics checklist

What makes tgcli specifically LLM-agent-friendly:

- Exit codes are the control flow — agents branch without parsing prose.
- Writes fail closed: with no grant every mutating command exits 6 with a
  structured error, and `TGCLI_ALLOW_WRITE=0` force-sandboxes an agent even
  against an account with a standing config grant. A harness that sets
  `TGCLI_ALLOW_WRITE=1` (or lets the agent pass `--allow-write` per call)
  opts into writes deliberately.
- The per-account daemon gives ~zero per-call startup latency and makes
  parallel tool calls safe (no DB-lock failures), with the local DB
  continuously warm for `--local` reads.
- `--dry-run` lets an agent present a plan for human approval before writing.
- Destructive actions are non-interactive-safe: without a TTY they fail
  closed (exit 6) unless `--yes` is explicit in the call.
- `send` returns the sent message id; `wait-for --after <that-id> --regex
  --timeout` turns "message someone and await the reply" into one race-free
  blocking call with a deterministic timeout exit code.
- `listen --json --count N --timeout S` gives bounded streaming reads that
  exit 0 on planned expiry.
- `--idempotency-key` makes retries safe (record-then-send, §6).
- Once M7 activates `raw`, it guarantees no capability cliff: anything td_api
  can do is reachable.
- Stable curated schemas under `docs/schemas/`, also dumpable at runtime via
  `tgcli schema <command>` — no repo checkout needed inside a sandbox.
- `tgcli doctor --json` is a one-call health/auth probe.

## 13. Build, dependencies, testing

- **Language/std**: C++20. **Build**: CMake ≥ 3.24 with presets.
- **tdlib**: pinned source revision (tdlib tags rarely; pin a commit hash),
  built via FetchContent by default; `-DTGCLI_SYSTEM_TDLIB=ON` accepts a
  prefix produced by `scripts/build-tdlib.sh` from the same pin — it exports
  the JSON-conversion headers `raw`/`--full` need; arbitrary distro tdlib
  packages are not supported. ccache strongly advised; CI caches the tdlib
  build keyed by the pinned hash. Bumping the pin is a
  contract-change-class PR (REVIEW.md §7): td_api churn can move the typed
  surface and the curated schemas.
- **Libraries** (FetchContent, permissively licensed): CLI11 (nested
  subcommands), nlohmann/json, fmt, tomlplusplus, Catch2; tests additionally
  use jsoncons 1.7.0 at the pinned release commit for Draft 2020-12 validation.
- **Testing** (policy detailed in CLAUDE.md; tests pin the external
  contract, never the implementation):
  - Contract tests as the default: a command driven through the real
    dispatch path (`--no-daemon`) against one shared scripted fake of the
    td_api boundary, asserting emitted td_api requests as data, output JSON
    against docs/schemas/, exit codes, stderr discipline. Cross-command
    semantics (write gate, NOT_AUTHED, ambiguity, timeouts) are tested once
    centrally, not per command.
  - Unit tests only for real logic (resolver matching, tier evaluation,
    frame codec, cursors, idempotency replay); no mock-verification tests.
  - Protocol-v2 routed-account acceptance is one central frame/transport/
    routing suite, not repeated per command. It must prove all of the
    following exact cases:
    1. A Request round-trip preserves a valid top-level `account` and emits
       exactly the six v2 top-level fields. Removing `account`, adding an
       unknown top-level field, changing `account` to a non-string, or using
       each invalid name class (empty, longer than 32 ASCII bytes, a character
       outside `[A-Za-z0-9_-]`, or non-ASCII) fails strict parsing without
       producing a Request. After a matching v2 Hello, the server classifies
       each as malformed-frame `USAGE` with id 0 and `{}`, then EOF; none is
       `ACCOUNT_NOT_FOUND` or `ACCOUNT_MISMATCH`.
    2. A routing-capture test covers explicit `--account`, `TGCLI_ACCOUNT`,
       configured default and implicit `main`, and asserts that each resolved
       value is simultaneously the selected socket/spawn/control account and
       serialized `Request.account`. No supported option, environment value,
       argument or config read can override the frame account after that
       resolution. `--no-daemon` freezes the same value.
    3. Before sending the client Hello, a syntactically valid Request with
       `account:"work"` sent over `main.sock` yields connection-scoped `USAGE`
       with id 0 and exact `{}`, followed by EOF. It never yields
       `ACCOUNT_MISMATCH` and triggers none of the observations in case 5.
    4. After a matching v2 Hello, sending a syntactically valid Request for
       `work` over `main.sock` yields one and only one terminal for its id:
       `ACCOUNT_MISMATCH`, exit 4, exact details
       `{"requested_account":"work","daemon_account":"main"}`, followed
       immediately by connection EOF. No result, item, progress, challenge,
       or second error may precede or follow it.
    5. Guarded fakes reset after daemon startup and fail the test if that
       mismatched request accesses a config loader/snapshot, executes a hook,
       reads auth state or calls TDLib, derives or touches an account path,
       enters the activity tracker, or reaches dispatcher/handler lookup.
       The rejection itself must leave every guard at zero.
    6. Two configured daemons with distinct sockets, config/hook sentinels,
       TDLib/auth fakes, data/state roots and activity counters reject crossed
       Requests in both directions as above. Subsequent correctly routed
       Requests still reach only their own daemon and return only their own
       account data.
    7. Upgrade fixtures cover v2 client→v1 daemon and v1 client→v2 daemon,
       with both equal and unequal binary-version strings. Every protocol-
       mismatch case authenticates and uses the frozen `.ctl` stop surface,
       sends no normal Request in the mismatched dialect, waits for the old
       lock and both socket identities to disappear, completes the replacement
       Hello, and then successfully retries the original command. A binary-
       only v2 mismatch additionally proves that any normal stop Request uses
       the same account that selected its socket.
  - Golden files for human renderers.
  - E2E: a small opt-in suite (`TGCLI_TEST_DC=1`) against Telegram's **test
    DC** (`use_test_dc`), which provides synthetic phone numbers with fixed
    login codes. M0 is expressly exempt because it has no authentication.
    M1 bootstraps the harness and nightly job with an auth smoke flow; every
    M2–M6 milestone gate adds a flow for a feature that milestone supports.
    M7 validates the already-complete suite for release rather than
    introducing it. M1's required smoke is isolated account bootstrap, phone
    auth/fixed code (and registration when requested), `me`, correlated logout
    and re-login readiness. QR and bot E2E run only with their explicit
    approver/`bot_token_cmd` fixtures and otherwise report
    `fixture_missing:qr_approver`/`fixture_missing:bot_token_cmd`. Required
    states unavailable in the test DC, including Premium-only states, receive
    fake-boundary contract coverage and a machine-visible
    `test_dc_state_not_forceable:<state>` skip reason. Every test-DC run,
    including one with no skips, writes
    `<build-dir>/test-results/tgcli-test-dc-skips.json` with exactly
    `{"skips":[{"test":string,"reason":string}]}`. Entries are sorted by
    test then reason. A reason is exactly `fixture_missing:qr_approver`,
    `fixture_missing:bot_token_cmd`, or
    `test_dc_state_not_forceable:<state>`; absence of this published artifact
    fails the job, so a skip cannot be silent or pass-equivalent. The external
    service is rate-limited and periodically wiped, so E2E is not a per-PR
    merge blocker (REVIEW.md §4).
- **Sanitizers**: ASan/UBSan jobs run the full test suite (an uninstrumented
  tdlib is acceptable for those); TSan runs the unit/contract suite against
  the fake td boundary only — meaningful TSan across tdlib's own threads
  would require a second, TSan-instrumented tdlib build (optional nightly).
- **CI (GitHub Actions)**: Linux + macOS build/test matrix, clang-format and
  clang-tidy checks, sanitizer jobs as above, and a tdlib build cache. M7 is
  planned to add the release job producing a static (musl) Linux binary —
  OpenSSL/zlib linked statically from pinned sources in the musl toolchain
  image — and a macOS universal binary assembled from per-arch builds with
  `lipo`.

## 14. Roadmap

Milestones. TODO.md is normative for per-milestone contents; this list is a
summary:

- **M0** — scaffold + process model: CMake + presets, deps, CI, tdlib builds;
  auto-spawned daemon, socket protocol skeleton; `tgcli version` / `tgcli
  doctor` round-trip through the daemon reporting the tdlib version; initial
  result-schema manifest and strict schemas for the M0 result commands.
- **M1** — auth: challenge/response login over the protocol (phone/QR/bot),
  logout, me, accounts; auth FSM; daemon lifecycle commands
  (status/stop/restart/run, idle_exit); the minimal shared safety kernel for
  destructive logout/account removal (authority, confirmation, dry-run,
  audit).
- **M2** — read path: chats, read, msg get, search, Saved Messages reaction-tag
  discovery/search, unread, resolve, fetch; resolver, JSON/human output, exit
  codes; every new result-producing command lands with its manifest entry,
  strict schema, and contract validation.
- **M3** — extend the M1 safety kernel to the general write path:
  send/edit/delete/forward/react/mark-read/pin and the remaining destructive
  commands; add idempotency and general planners/audit integration.
- **M4** — files: download/upload with progress frames, media types, single-file
  Saved Messages attachment replies that preserve the replied-to message.
- **M5** — streaming: multiplexed update subscriptions, listen, wait-for.
- **M6** — folders, topics, contacts, chat admin, sessions.
- **M7** — raw passthrough, shell completions, man pages, packaging (static
  binaries, AUR, Homebrew), docs/schemas freeze → v1.0.
- **E2E chronology** — M0 is exempt; M1 establishes auth smoke and nightly
  wiring; each M2–M6 gate adds a supported feature flow; M7 validates the
  complete accumulated suite.
- **Post-1.0 ideas**: MCP server mode (`tgcli mcp` over stdio), secret chats,
  scheduled-message management, message translation.

## 15. Open questions

- ~~License (MIT vs Apache-2.0)~~ — resolved: MIT (see LICENSE). BSL-1.0 is
  permissive and copyleft-free, so linking tdlib is compatible.
- Whether `chat create` (new groups/channels) belongs in v1 scope.
- Windows support: tdlib supports it, but the process model assumes unix
  sockets and fork/exec; a Windows port needs named pipes and
  CreateProcess — deferred until asked.
