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
  every upgrade.
  tgcli keeps no message store of its own: TDLib owns and migrates the Telegram
  database. tgcli's own persistent state is limited to config.toml, append-only
  audit, the versioned idempotency store, removal tombstones, rotated logs, and
  the private crash-recoverable outbound attachment spool from §4.5.12. The
  spool contains temporary bytes for one audited send and is not a Telegram
  message cache.
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
- **TTY-adaptive**: human tables on a TTY; `--json` (or non-TTY
  auto-detection is *not* used — output format never silently changes) for
  machines. No v1 renderer emits ANSI. `--no-color` and a nonempty `NO_COLOR`
  are accepted byte-preserving no-ops for commands, help, schemas and shell
  completion output.
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
--full                 reserved and rejected through v1; post-1.0 only
--allow-write          per-call write grant; §6 covers env/config grants and
                       the deny override
--dry-run              resolve and validate, print the plan without a
                       Telegram mutation; M3/M4 dry-runs are auth-bound (§6)
--yes                  non-interactive approval of destructive actions
--timeout <sec>        per-command deadline (default 60; streams, file
                       transfers and fetch: unlimited unless set)
--cursor <token>       resume pagination from a `next` token (§5)
--idempotency-key <k>  idempotent M3/M4 writes and exact replay (§4.5.7)
--verbose / -v         diagnostics to stderr
--no-daemon            debugging: run in-process without the daemon (§10)
--no-color             accepted v1 byte-preserving no-op; output has no ANSI
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
tgcli chats --cursor <token>
tgcli read <chat> [-n N] [--before <msg-id>] [--since TS] [--until TS]
              [--topic <kind:id>] [--local]      (alias: history)
tgcli read --cursor <token>
tgcli history --cursor <token>
tgcli send <chat> <TEXT|-> [--md | --html] [--reply-to ID]
              [--topic <bare-int|forum:int>] [--silent]
              [--schedule <RFC3339|online>]
tgcli search <query> [--chat <c> | --global] [--from <user>]
              [--type any|text|photo|video|doc|link|voice] [-n N]
              always server-side — tdlib has no local search (§8)
tgcli search --cursor <token>
tgcli unread                                      per-chat unread counters
tgcli fetch <chat> [--limit N | --all] [--since TS]
              deliberately warm the local DB with history for one chat
              (pages getChatHistory; enables later --local / offline work);
              progress on stderr, resumable, per-chat and per-depth targeting
tgcli download <chat> <msg-id> [-O <dir|path>]    progress on stderr
tgcli resolve <id | t.me-link | @username | title>
                                                  → ids, type, metadata

tgcli listen [--chat <c>]... [--types message,edit,delete,reaction,chat]
              [--timeout S] [--count N]           NDJSON stream, one update per line;
                                                  planned expiry (--timeout/--count) exits 0
tgcli wait-for [--chat <c>] [--from <user>] [--regex <re>]
              [--after <msg-id>] [--timeout S]
              blocks until one matching message arrives, prints it, exits 0;
              exits 7 (TIMEOUT) otherwise. --after <id> also matches messages
              that arrived after that id but before the call (checked in the
              continuous local prefix behind the already-published subscription).
              This closes the retained scan/live race for one client generation,
              not daemon downtime, an unknown local gap, or absolute Telegram
              delivery (§4.6.8).
              --after requires --chat (message ids are ordered per chat;
              exit 2 without it)

tgcli raw - [--timeout S]                         M7 stdin-only policy-classified escape hatch (§4.2)
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
tgcli chat members --cursor <token>
tgcli chat join <invite-link | @username>
tgcli chat leave <chat>                           destructive
tgcli chat mark-read <chat>
tgcli chat mute|unmute <chat> [--for <duration>]
tgcli chat pin|unpin|archive|unarchive <chat>
tgcli chat set-title <chat> <title>
tgcli chat set-photo <chat> <PATH>
tgcli chat set-photo <chat> --delete
tgcli chat set-description <chat> <description>
tgcli chat invite-link <chat> [--revoke <invite-link>] destructive
tgcli chat promote <chat> <user> --rights <right[,right...]>
tgcli chat demote <chat> <user>
tgcli chat ban|unban|kick <chat> <user>           ban/kick destructive
tgcli chat set-permissions <chat> --permissions <permission[,permission...]|none>

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
tgcli storage stats|optimize                      tdlib file-store usage; optimize is destructive
tgcli schema <command-token>... [--all]           print the primary curated success schema;
                                                   --all adds every cataloged payload kind (§4.8)
tgcli completion <shell>
tgcli version
```

### 4.1 Selectors

For every command other than `read --local`, `<chat>` classification remains
deterministic and ordered: a signed decimal is a tdlib chat id and is never
interpreted as a title; `@username` is an exact public username; an exact
lowercase `https://t.me/...` or bare `t.me/...` value is classified with
`getInternalLinkType`; every other non-empty value is a case-sensitive UTF-8 byte
substring over `chat.title`. Syntactically valid numeric and username selectors
never fall back to title matching. No local-read rule below changes that baseline.

`read --local` uses this separate exact ordered preclassification before title
fallback:

| order | byte class | selector class |
|---:|---|---|
| 1 | syntactically valid signed decimal | numeric chat id |
| 2 | `@` plus a nonempty valid-UTF-8 suffix | exact username |
| 3 | exact lowercase `https://t.me/` or bare `t.me/` prefix | local t.me grammar below |
| 4 | URL-like by the predicate below | `InvalidLink` |
| 5 | every other nonempty valid-UTF-8 value | title substring |

The URL-like predicate is true exactly when either (a) the bytes begin with an
ASCII scheme matching `^[A-Za-z][A-Za-z0-9+.-]*://`, or (b) under ASCII-only
case-insensitive comparison they begin with `t.me` or `www.t.me` and the next byte
is end-of-input, `/`, `:`, `?`, or `#`. Step 3 wins only for its two exact
case-sensitive prefixes. Consequently `http://t.me/x`, `HTTPS://t.me/x`,
`https://T.me/x`, `https://www.t.me/x`, `https://user@t.me/x`,
`https://t.me:443/x`, `https://example.com/x`, `T.ME/x`, `www.t.me/x`, and
`t.me:443/x` are all `InvalidLink`; none is a title or reaches TDLib.
`InvalidLink` is pre-Ready `USAGE/invalid_argument` with `argument:"selector"`.
A value such as `example.com/x`, `notes/http`, or `Development` matches neither
URL-like predicate and remains a title candidate. This preclassification performs
no decoding, case folding outside the predicate, resolver call or TDLib call.

Title matching scans active, non-secret chats in **fully loaded Main and
Archive lists**. Main order precedes Archive order. `searchChats` may optimize
candidate discovery, but it never establishes absence or completeness because
the pinned tdlib implementation indexes normalized word prefixes rather than
arbitrary substrings. The resolver grows each list's materialized prefix with
`getChats` and `loadChats(list, 100)` until tdlib reports the documented
end/404. If the request deadline wins first, resolution is `TIMEOUT`, never a
partial-domain result. It then materializes each chat with `getChat`, excludes
secret chats, and applies the exact byte-substring predicate. Zero
matches is `NOT_FOUND`; one resolves; more than one is `AMBIGUOUS`. The latter
has exact details:

```json
{"selector":"dev","scope":"active_dialogs","candidates":[{"id":-1001,"title":"Development","type":"supergroup","is_bot":false,"usernames":["dev"]}],"truncated":false}
```

At most 20 candidates are returned in Main-then-Archive order and `truncated`
states whether more exact matches existed. A short successful `loadChats` is
not end-of-list. Under `read --local`, no `loadChats` or other network-capable
lookup is allowed: the title domain is the Main+Archive prefix materialized at
request start, and a miss is `NOT_FOUND` with exact details
`{"selector":string,"scope":"local_materialized"}`.

All other `read --local` selector branches are equally offline: numeric ids use
local `getChat`; usernames and PublicChat/BotStart links match only active
usernames already present in the materialized Main+Archive prefix; Saved
Messages may use local `createPrivateChat(me.id, false)`.

Local t.me classification is a closed ASCII byte grammar and performs no TDLib
call. The only prefixes are exact lowercase `https://t.me/` and bare `t.me/`;
`http`, uppercase or mixed-case scheme/host, `www`, userinfo, a port and every
other host are invalid link input and never fall back to title matching. A local
username has 1 through 32 bytes, begins with `[A-Za-z]`, continues with
`[A-Za-z0-9_]`, contains no `__`, and does not end in `_`; matching preserves its
exact bytes. Percent escapes, non-ASCII bytes, fragments, an empty/consecutive path
component and a trailing slash are always malformed. After the prefix, the general
structural envelope is one or more nonempty `[A-Za-z0-9_+-]+` path components
separated by one slash and optionally `?key[=value](&key[=value])*`, where key is
nonempty and key/value use `[A-Za-z0-9_-]*`. Exact recognized forms below take
precedence; an envelope-valid remainder not matching one is `unsupported_link_type`.
The only accepted/query-classified forms are:

| exact bytes after either prefix | local class | result |
|---|---|---|
| `<username>` | `PublicChat` | match the exact active username |
| `<username>?start=<parameter>` | `BotStart` | match an exact active private-bot username; never execute the parameter |
| `<username>/<positive-decimal>` or `c/<positive-decimal>/<positive-decimal>` | `Message` | `NOT_FOUND/local_materialized` |
| `+<base64url>` or `joinchat/<base64url>` | `ChatInvite` | `NOT_FOUND/local_materialized` |
| `<username>?direct` | `DirectMessagesChat` | `NOT_FOUND/local_materialized` |

`positive-decimal` is canonical `[1-9][0-9]*`. `base64url` is one or more
`[A-Za-z0-9_-]` bytes. A BotStart parameter is zero or more of those same bytes
and has no independent limit below the existing argv/frame bound. Its query is
exactly one case-sensitive `start` pair: no duplicate, separator, extra key,
missing `=`, or percent decoding is accepted; a username query beginning with
`start` but not equal to that form is malformed, not a generic unsupported query.
An exact prefix followed by an
empty/invalid component or malformed path/query is `USAGE/invalid_argument`;
another structurally valid ASCII path/query outside the table is
`USAGE/unsupported_link_type`. None of these branches calls
`getInternalLinkType`, `searchPublicChat`, `getMessageLinkInfo`,
`checkChatInviteLink`, full-info lookup or another network-capable function. The
existing terminal `resolve` adapter is not a permissible local-read shortcut.
An unseen PublicChat/BotStart username returns the same
`NOT_FOUND/local_materialized` miss.

`<user>` accepts a positive tdlib user id, exact `@username`/public profile
link, or a title substring over the display name. The global title domain is
the complete returned `getContacts` set; a per-chat domain is the basic-group
full member vector or the complete pages returned by
`getSupergroupMembers(..., supergroupMembersFilterSearch)`. Ambiguous user
details contain `selector`, no more than 20
`{"id", "display_name", "usernames", "is_bot"}` candidates in tdlib order,
and `truncated`; there is no `scope` field in a user ambiguity object.

Title-substring resolution applies to read-tier commands only. Write- and
destructive-tier commands require an exact selector (`@username`, id, or
t.me link); a title substring fails with exit 2 and the `candidates` list.
This prevents a changing local dialog set from redirecting a mutation.

The supported `t.me` classes and resolver actions are closed:

| `InternalLinkType` | user session | bot session |
|---|---|---|
| `PublicChat` | `searchPublicChat` | allowed |
| `BotStart` | resolve the bot chat; never execute the start parameter | allowed |
| `Message` | `getMessageLinkInfo`; `/c/` links are supported | allowed |
| `ChatInvite` | `checkChatInviteLink`, require nonzero `chat_id`, never join | `BOT_UNSUPPORTED` before `checkChatInviteLink` |
| `DirectMessagesChat` | resolve the public channel, then `getSupergroupFullInfo.direct_messages_chat_id`; zero is `NOT_FOUND` | `BOT_UNSUPPORTED` before the user-only lookup |
| `SavedMessages` | `getMe`, then local `createPrivateChat(me.id, false)` | `BOT_UNSUPPORTED` |
| any other class | `USAGE`, reason `unsupported_link_type` | same |

`getInternalLinkType` is allowed to classify a bot-session link. For a bot
invite, the observable sequence is Ready, `getMe`, `getInternalLinkType`, then
`BOT_UNSUPPORTED`; no `checkChatInviteLink` request is sent. Saved Messages
materialization is not a Telegram-side write. Link resolution never joins a
chat and never executes a bot-start parameter.

`--since`/`--until` accept exactly one of these ASCII forms:

```text
YYYY-MM-DD
YYYY-MM-DDTHH:MM:SS[.DIGITS](Z|[+-]HH:MM)
[1-9][0-9]*(m|h|d)
```

All date/time digits are ASCII. The calendar form has a four-digit year from
`0001` through `9999` and a real proleptic-Gregorian month/day, including the
normal Gregorian leap-year rule. Datetime hour is `00` through `23`, minute and
second are `00` through `59`, uppercase `T` and `Z` are literal, and a numeric
offset has hour `00` through `23` and minute `00` through `59`. Both signs on a
zero offset are accepted and denote UTC. Basic ISO forms, lowercase `t`/`z`, a
space separator, a missing zone, offsets without the colon, leap second `60`,
`24:00`, and trailing or surrounding bytes are invalid.

If the decimal point is present, `DIGITS` contains one or more ASCII digits.
There is no smaller fraction-length limit than the existing argv/protocol-frame
size limit: parsing is linear, accumulates no unbounded integer, and records only
whether any fractional digit is nonzero. Let `base` be the signed integral Unix
second after applying the zone and let `fraction` be the parsed value in `[0,1)`.
The inclusive `since` second is `base` when the fraction is absent or all zeroes
and `base + 1` otherwise. The inclusive `until` second is always `base`. These are
mathematical ceiling and floor even when `base` is negative.

Date-only `since` is exact UTC day start. Date-only `until` is the last
representable whole second before the next UTC day. Relative syntax remains
exactly `^[1-9][0-9]*(m|h|d)$`; it denotes the request-start wall-clock instant
minus that many whole minutes, hours, or 24-hour days for either flag and is
rounded by the same lower/upper rule. The request-start wall clock is sampled
once, and multiplication, calendar, zone and rounding arithmetic use a checked
wide intermediate.

If the rounded `since` exceeds `until`, or either rounded endpoint is outside
tdlib's signed 32-bit seconds field, the command fails with pre-dispatch
`USAGE`/`invalid_argument`. No rejected timestamp reaches Ready/getMe, resolution,
or TDLib.

Message ids are **tdlib message ids** everywhere (tdlib's message-id space
differs from Telegram's server ids). `msg link` / `resolve` convert to/from
public t.me references, so nobody needs to know about the id-space
difference.

### 4.2 `raw` escape hatch

`raw` remains reserved and rejected with `USAGE/unsupported_mode` until its
parser, policy, audit-v3 recovery, handler and result/error catalog mappings
activate atomically. `--full` does not activate with it: `--full` remains
reserved and rejected throughout v1 and is a post-1.0 contract decision.

The selected v1 raw policy is **Option B**, a table-classified Read/Write/
Destructive escape hatch. Its only grammar is:

```text
tgcli raw - [--timeout S]
```

The JSON request is read only from stdin. Any positional JSON or positional
value other than the literal `-` is rejected before account, session, Ready or
audit work. Physical stdin, including whitespace, and the canonical typed
request are each bounded at 1,048,576 bytes. The canonical public TD response
is bounded at 16,777,216 bytes; an oversized response becomes
`INTERNAL/result_too_large` before any partial stdout frame.

One duplicate-rejecting parser builds an AST and processes numeric tokens once
without a second JSON parse. It requires one top-level object and one top-level
string `@type`, rejects
duplicate keys at every depth, unknown generated fields at every typed object
boundary, and top-level `@extra` or `@client_id`. Generated missing/default/
null rules apply exactly. User keys inside a typed `jsonValueObject` remain
data rather than transport metadata. The AST is converted exactly once into
one owned typed Function value. Classification, canonical hashing and future
native dispatch all retain that same object; a second parse or second
`from_json` is forbidden.

Canonical serialization is TD-schema-aware: Bool is a JSON boolean; int32 and
int53 are exact JSON integers with int53 restricted to
`[-9007199254740991,9007199254740991]`; int64 is a canonical signed-decimal
JSON string over the complete signed domain; bytes are padded RFC 4648 base64;
strings are exact valid UTF-8; finite doubles use RFC 8785 shortest round-trip
spelling and NaN/Inf are rejected. Objects serialize `@type` followed by
generated declaration order, vectors preserve order, and omitted/default
fields serialize as their typed values. Accepted source spellings that convert
to one typed value therefore hash identically.

The request digest is:

```text
SHA256("tgcli.raw.request.v1\0" || pinned_td_sha_ascii || "\0" ||
       function_name_ascii || "\0" || effective_tier_ascii || "\0" ||
       uint64_be(canonical_request_bytes) || canonical_request)
```

The response digest uses domain `tgcli.raw.response.v1\0`, the function name,
the uint64 big-endian canonical response length and canonical response bytes.
`request_bytes` and `response_bytes` count only canonical typed JSON, excluding
the hash domain, protocol envelope and LF. Raw stdin, AST string/byte storage,
canonical request storage and every recursively reachable native typed request
string/byte field are wiped by the pin-generated visitor after ownership
transfer or on rejection. Response hashing consumes one owned
`object_ptr<td_api::Object>`; an RAII guard recursively wipes every native
response string/byte field and canonical response staging on null, metadata,
type, canonicalization, oversize, TD-error and success exits before releasing
the object. No caller retains an unwiped response alias.

Two checked-in pin-owned assets govern classification:

```text
docs/raw/td-functions.a17f87c4cff7b90b278d12b91ba0614383aaee82.json
docs/raw/td-types.a17f87c4cff7b90b278d12b91ba0614383aaee82.json
docs/raw/raw-policy.a17f87c4cff7b90b278d12b91ba0614383aaee82.json
```

The inventory is derived from the complete pinned function set: at the current
pin both `td_api.tl` and generated `td_api.h` contain exactly 1001 functions.
The invariant is equality with both complete sources; 1001 and a committed
digest are drift evidence, not a timeless API constant. Each inventory row is
`name,constructor_id,result_type,fields_sha256`. The type graph contains all
3118 concrete pinned object/function constructors, every generated field in TD
declaration order, and each abstract result-type membership; its count and
digest are likewise pin-specific drift evidence. The duplicate-rejecting AST
is validated and default-materialized through this graph, then converted once
directly to the actual owned `td_api::Function`. The stored native object ID,
function row and canonical bytes remain one holder for classification, hashing
and eventual move-only dispatch. Native TD responses are revalidated against
the same graph and the holder's declared result base (with `td_api::error` as
the sole alternate) before canonical hashing. Policy is an exact
name/constructor/result/fields-digest bijection and records principal
(`user|bot|both`) with pinned TL/Requests evidence, admission
(`read|write|destructive|denied`), body validator, exact direct/nested target
fields, request/response sensitivity, evidence category, review decision and
row rationale.

The committed dormant candidate reviews all 1001 rows: exactly 32 are Read, 19
Write, 6 Destructive and 944 denied whole for v1. Eight admitted rows are
account-independent synchronous typed transforms. Of the other 49, 45 have one
direct `chat_id:int53`; three also have a required `member_id:MessageSender` and
one has an optional `sender_id:MessageSender`. The generated typed planner
collects the direct chat plus every `messageSenderChat.chat_id`; a
`messageSenderUser` adds no chat target, null is accepted only for the optional
sender, and an unknown/null required selector denies. Every collected target
requires a generation-bound curated non-secret `getChat` preflight. The
principal distribution is 893 user, 80 bot and 28 both; `both` is used only for
functions present in the pinned account-independent synchronous dispatcher.

The candidate remains `activation_ready:false` with the sole exact blocker
`independent_policy_acceptance`; `unfinished_functions` is empty. Row reasoning
and compiled mechanics are complete, but raw is not publicly accepted or
reachable. Dormant validation fails on
pin/source/count/bijection/digest/evidence mismatch. Activation additionally
requires the blocker to be removed and `activation_ready:true` in the
independently accepted asset. The generated symbol table contains exact sorted
unique `{name,nonnull typed_validator_fn,nonnull typed_preflight_fn}` rows and
the generated 1001-row runtime policy table references those same symbols.
Missing implementations are compile failures; unknown functions, symbols,
constructors and table versions deny.

A typed body validator returns exactly one closed decision:
`Deny|Preserve|RaiseWrite|RaiseDestructive`. `deny` returns `Deny`; `none`
returns `Preserve`. The runtime evaluator takes the row's closed static
admission tier (`Denied|Read|Write|Destructive`) and the same owned native
Function used for classification and hashing, invokes the callable from the
generated table, and returns a typed effective tier or denial. A static
`Denied` remains denied under every decision. `Preserve` retains a Read, Write
or Destructive static tier; `RaiseWrite` maps Read to Write and retains Write
or Destructive; `RaiseDestructive` maps every valid tier to Destructive. No
decision can lower a tier. An invalid static tier, decision, symbol,
missing/null callable, or unknown/unmatched nested variant is `Deny`. Functions
that change
authorization, TD parameters, lifecycle or logging, accept authentication,
credential, payment or proxy secrets, or can expose secret-chat/private data
without preprovable provenance are denied whole. Admitted chat selectors use a
generated exact Function-ID switch that extracts the direct `chat_id` plus the
known required/optional `MessageSender` variants into a fixed-capacity preflight
plan. Every admitted nested variant is enumerated; unexpected/null-required
selectors deny. Other multiple, indirect or unknown-provenance selectors are
denied whole. Curated same-generation `getChat` preflights reject Secret,
Unknown, errors and stale generations. Raw `getChat` itself and file-id-only
`downloadFile` are denied because executing them cannot first prove non-secret
provenance.
Curated download is a separate M4 path. No function is admitted on its name
alone.

Admission order is local parse/table lookup, one admission-relative Default60
absolute deadline, generation-scoped Ready, one correlated `getMe`, principal
check, typed validators and curated preflights, then dry-run or authority/
confirmation/audit/dispatch. No stage retries across generation replacement.
Write requires the normal write grant. Destructive requires the grant and
confirmation target `raw <function> sha256:<request-hash>`. The caller cannot
choose or lower tier, and raw never accepts an idempotency key. Dry-run executes
through principal/body validation but performs no authority, confirmation,
audit or dispatch.

A correlated response is either typed `td_api::error` or a non-null object
compatible with the policy row's declared result type. Null, Update, unknown
constructor or wrong result type is `INTERNAL/unexpected_response`. Code 429 is
`RATE_LIMITED`; other TD errors are `TDLIB_ERROR`. TD error messages and raw
request/response bodies never enter stdout errors, stderr, logs or audit. After
durable dispatch a TD error conservatively records mutation `possible`; typed
success records `confirmed`.

Option B uses a separate dormant audit schema version 3 rather than widening
v2 `StoredTerminal`. Beyond the common schema/version/record/invocation
bookkeeping, intent retains only function, tier, pin, request hash and request
bytes. Legal checkpoints are `raw_dispatch_started` and
`raw_response_received`; both bind one dispatch token and client generation,
and response data retains only result/error kind, response type, nullable TD
code, response hash and response bytes. Outcome stores a result digest or
closed error summary, never a body. Recovery never resends: intent without
dispatch closes `none`; a valid durable dispatch without a valid response
closes `RAW_OUTCOME_UNCONFIRMED`, exit 1, message
`raw request outcome is unconfirmed`, and exact hash-only details
`{"operation":"raw","function":function,"request_sha256":"sha256:<64-lower-hex>","mutation_state":"possible"}`.
The same possible-mutation outcome is persisted on a sealable live
post-dispatch loss/timeout/auth/disconnect and on recovery. It is never emitted
before dispatch or after a proven response. `AUDIT_INCOMPLETE` is reserved for
unreadable/schema-invalid records and ordering/token/generation/identity
contradictions, including inability to inspect or repair. A proven response
without outcome repairs the corresponding confirmed/possible digest outcome.
These dormant
schemas, validators, scanner and recovery rules must be present before raw is
registered.

Live success in human and JSON modes is the same compact canonical TD object
plus LF. The strict result schema is a one-of between that live object (one
string `@type`, no `@extra` or `@client_id`) and the exact auth-bound dry-run
plan containing `operation:"raw"`, function, `write|destructive` tier, pinned
TD SHA, request SHA-256 and request bytes. Structured errors are closed and
never contain a body, TD message, credential, selector preflight result or
internal query identifier.

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
For this Saved-only command, omitted `-n` means 20 and an explicit value must
be an integer from 1 through 100 inclusive. This Saved rule remains exact;
§4.4 independently adopts the same numeric default and range for the remaining
M2 commands.
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

The saved-search item is the Saved-only exact summary
`{"id":integer,"chat_id":integer,"date":string,"text":string}`. `id` and
`chat_id` are tdlib's returned message and chat ids. `date` is tdlib's Unix
`message.date` rendered in UTC as `YYYY-MM-DDTHH:MM:SSZ`. `text` is the exact
`formattedText.text` for `messageText`; it is the empty string for every other
message-content variant. This deliberately does not establish the eventual
global M2 `MessageSummary` or add speculative sender, topic, or media fields.

Saved failures use the standard error envelope. `BOT_UNSUPPORTED` has exact
empty details. TDLib RPC errors use `TDLIB_ERROR` with
`{"operation":"saved_tags"|"saved_search","tdlib_code":integer}` except 429,
which uses the standard `RATE_LIMITED` shape. A paid, unknown, null, invalid
UTF-8 emoji, or otherwise unusable returned tag uses `TDLIB_ERROR` with
`{"operation":"saved_tags","tdlib_type_id":integer}`; a non-positive custom
emoji id additionally includes `custom_emoji_id`.

`saved tags` and `saved search` are read-tier. The pinned
`searchSavedMessages` API is Premium-only; an unavailable Premium capability
surfaces through the existing `GENERIC` exit 1 with tdlib error details rather
than defining a new exit code.

### 4.4 M2 read contract

All M2 result objects are curated strict objects; their schemas reject unknown
properties. Chat and message ids are nonzero tdlib `int53` values and user ids
are positive tdlib `int53` values. Unless a command says otherwise, `-n`
defaults to 20 and accepts integers from 1 through 100. `chat members` instead
defaults to 50 and accepts 1 through 200. Syntactically invalid ids, limits,
topic references, timestamps and flag combinations fail before the selected
target request.

Every M2 command covered by this section first requires the selected account's
authorization snapshot to be Ready, then calls `getMe` for identity/cursor
binding and bot preflight. The already implemented Saved namespace retains its
exact §4.3 shapes. `NOT_AUTHED` therefore precedes `BOT_UNSUPPORTED`. For an
authenticated bot:

| command | M2 behavior |
|---|---|
| `chats`, `read`/`history`, `search`, `unread`, `fetch` | `BOT_UNSUPPORTED` before any user-only tdlib request |
| `msg get`, `msg link`, `chat info`, `chat members` | allowed |
| `resolve` | the selector-specific matrix in §4.1 |

#### Shared M2 objects

A `TopicRef` is the lossless tagged representation of tdlib's four message
topic variants:

```json
{"kind":"forum","id":123}
```

`kind` is `forum`, `thread`, `direct`, or `saved`. A forum id is a positive
`int32`; every other id is a positive `int53`. CLI selectors accept
`forum:123`, `thread:456`, `direct:789`, and `saved:321`. A bare positive
decimal is exactly an alias for `forum:<id>`. The lexical grammar is
`^(forum|thread|direct|saved):[1-9][0-9]*$` or bare `^[1-9][0-9]*$`, followed by
the kind-specific numeric range check. A plus sign, minus sign, zero, leading
zero, empty id, non-ASCII digit, whitespace or trailing byte is
`USAGE`/`invalid_argument`; tgcli never guesses topic kind from chat type.

Live topic-history dispatch is exact:

| kind | tdlib function |
|---|---|
| `forum` | `getForumTopicHistory(chat_id, id, ...)` |
| `thread` | `getMessageThreadHistory(chat_id, id, ...)` |
| `direct` | `getDirectMessagesChatTopicHistory(chat_id, id, ...)` |
| `saved` | `getSavedMessagesTopicHistory(id, ...)`, only for the current user's Saved Messages chat |

`--local` never calls those network-capable functions. It uses
`getChatHistory(..., only_local=true)` and compares the complete tagged topic
on each returned message for every admitted local topic; the channel/thread
cross-chat case has the explicit `unsupported_mode` branch in `read` below.

A `saved:<id>` operand or cursor state performs an exact ownership check after
principal binding and chat resolution and before any date/history request. Through
the same `ResolverConsumer` and absolute deadline, tgcli obtains the local result of
exactly `createPrivateChat(principal.id, false)`. A request-scoped result already
obtained while resolving a Saved Messages link is reused instead of sending it
twice. The returned chat must be a structurally valid private chat for the bound
non-bot principal, and its chat id must equal the resolved target chat id. A
different target is `USAGE`/`invalid_argument` with `argument:"--topic"`; a TDLib
error or malformed response uses the normal `read` target-phase error. This call is
the existing allowed local Saved Messages materialization, not a Telegram-side
write, and is also allowed under `--local`. No other topic kind performs it.

The shared neutral `TdMessages` representation preserves tdlib's nonnegative
`total_count`, the response vector including null positions, and each neutral
`TdMessageSummary`. `MessageSummary` materialization and JSON conversion remain the
single shared implementation used by `chats`, `read`, `msg get`, and later M2/M5
consumers; read does not create a parallel message DTO.

The typed boundary also exposes `TdMessageThreadInfo` with exactly
`history_chat_id`, `history_thread_id`, and the returned vector of neutral starting
messages from pinned `messageThreadInfo.chat_id`, `message_thread_id`, and
`messages`. Reply metadata, unread count and draft are not retained. This is the
only metadata seam for mapping a `thread:<id>` operand to its actual history chat.
`getMessageThread` is a `DescriptorKind::Read` function admitted only through this
user-read seam after bot preflight; pinned `Requests.cpp` applies `CHECK_IS_USER`.

A `MessageSenderRef` is exactly `{"type":"user","id":42}` or
`{"type":"chat","id":-1002}`. `MessageSummary` is:

```json
{
  "id": 123,
  "chat_id": -1001,
  "date": "2026-08-05T10:00:00Z",
  "sender": {"type":"user","id":42},
  "is_outgoing": false,
  "topic": {"kind":"forum","id":7},
  "type": "text",
  "text": "message or caption"
}
```

`date` is UTC RFC 3339 at second precision and is null when tdlib `date` is
zero. `topic` is `TopicRef` or null. `type` is exactly `text`, `photo`,
`video`, `doc`, `voice`, or `other`; `messageAnimatedEmoji` is `text`.
`text` is the documented formatted text or caption for the content variant,
otherwise the empty string. A search filter such as `link` never changes the
message's actual content `type`.

`ChatIdentity` is:

```json
{"id":-1001,"title":"Project","type":"supergroup","is_bot":false,"usernames":["project"]}
```

`type` is `private`, `basic_group`, `supergroup`, or `channel`. `usernames`
contains active tdlib usernames in returned order. `is_bot` can be true only
for a private bot chat. `ChatSummary` contains every `ChatIdentity` field and:

```json
{
  "is_archived": false,
  "folder_ids": [2],
  "is_marked_unread": false,
  "unread_count": 3,
  "unread_mention_count": 1,
  "unread_reaction_count": 0,
  "unread_poll_vote_count": 0,
  "last_message": null
}
```

`last_message` is `MessageSummary` or null. `chat.chat_lists`, not positions,
determines list membership, `is_archived`, and ascending `folder_ids`.
`chat.positions` supplies only the order key in the selected list. Membership
without a matching nonzero position does not put the chat in that page.

Secret chats are post-1.0. `chats` and `unread` skip them and continue scanning;
global search uses only `searchMessages` and never merges
`searchSecretMessages`. A secret target for `read`, per-chat `search`, `fetch`,
`msg get`, `msg link`, `resolve`, `chat info`, or `chat members` fails before
the target operation with `USAGE`, reason `unsupported_chat_type`.

#### `chats`

The scope is Main by default, Archive for `--archived`, or the positive numeric
folder id from `--folder`; archive and folder are mutually exclusive. The
`--unread` predicate is:

```text
is_marked_unread || unread_count > 0 || unread_mention_count > 0 ||
unread_reaction_count > 0 || unread_poll_vote_count > 0
```

Results are ordered by the selected `chatPosition` key `(order, chat.id)`
descending and have exact shape
`{"items":[<ChatSummary>],"next":<cursor|null>}`. Pagination materializes a
growing prefix:

1. Request an initial prefix of 100 with `getChats(list, prefix_limit)` and
   materialize its chats and selected positions.
2. Scan only keys strictly below the cursor anchor; the anchor need not still
   exist. Record the last raw scanned key even when `--unread` filters it out.
3. If fewer than `-n` matches are available, call `loadChats(list, 100)`, grow
   the requested prefix by at least 100, and reread the prefix.
4. A short successful load is not EOF. Continue until the prefix grows, tdlib
   reports documented end/404, or the absolute deadline wins. There is no raw
   scan cap.

Equal `order` values use chat id descending. A stateless continuation rebuilds
the growing prefix after daemon restart. This is live-view keyset pagination,
not snapshot isolation: a chat moved above the anchor can be missed, one moved
below it can repeat, and removal of the anchor is harmless.

#### `read` and `history`

`history` is a parser alias canonicalized to `read` before the protocol frame,
schema lookup, cursor operation and error details. `--before` is exclusive;
`--since` and `--until` are inclusive and can be combined with it. Output order
is decreasing tdlib message id. Contextual message/topic metadata carried by a
resolved message or topic link is ignored. Only the explicit `--before` and
`--topic` operands, or their normalized cursor state, govern the read.

The result is exactly:

```json
{"items":[],"next":null,"boundary":"local_boundary"}
```

Every non-empty `items` element is a `MessageSummary`.

First-page admission and dispatch order is exact:

1. Parse and normalize every argument using the closed grammar above. Invalid
   syntax stops before Ready/getMe.
2. Bind Ready and one correlated `getMe` through one `ResolverConsumer` and the
   request's already-computed absolute deadline.
3. Reject a bot with `BOT_UNSUPPORTED`/`{"operation":"read"}` before selector or
   history work.
4. Resolve the explicit chat under `ActiveDialogs`, or `LocalMaterialized` for
   `--local`; reject a secret target before a date/history call.
5. Derive `history_chat_id`. It equals the resolved chat id for no topic, forum,
   direct and saved topics; saved first performs the exact ownership check above.
   A live `thread:<id>` instead makes exactly one
   `getMessageThread(resolved_chat_id, id)` metadata call. The original resolved
   chat/id remain the arguments later passed to `getMessageThreadHistory`, while
   the metadata response supplies `history_chat_id`. For local thread reads, no
   metadata call is allowed: a resolved `supergroup` uses the same chat id; a
   resolved `channel` is `USAGE/unsupported_mode` because public TDLib exposes no
   offline cross-chat discussion mapping; private/basic-group targets are
   `USAGE/invalid_argument`.
6. For a live first page with `--until`, call
   `getChatMessageByDate(history_chat_id, until_floor)`. A 404 returns
   `empty_before_until`. A successful response must be the exact message type,
   materialize as a valid `MessageSummary`, have `chat_id == history_chat_id`, and
   have nonzero tdlib date `<= until_floor`; wrong chat, zero/too-new date, null,
   unknown or malformed response is `INTERNAL` for `read`. It is the inclusive
   start anchor only when `--before` is absent.
7. For a live first page with `--since`, call
   `getChatMessageByDate(history_chat_id, since_ceil - 1)` after the until probe.
   A successful response has the identical structural/history-chat/nonzero-date
   checks and its date must be `<= since_ceil - 1`; this is the strongest exact
   relation promised by pinned `getChatMessageByDate`. Wrong chat, zero/too-new
   date, null, unknown or malformed response is `INTERNAL` for `read`. Its id is
   the exclusive lower cutoff anchor. A 404 leaves the cutoff absent and does not
   prove exhaustion. If `since_ceil == INT32_MIN`, no earlier tdlib second exists:
   skip this call, leave the cutoff absent, and never synthesize `time_anchor`.
8. Dispatch `getChatHistory` or the exact topic-history function and scan. A
   thread call is exactly `getMessageThreadHistory(resolved_chat_id, topic.id,
   from_message_id, 0, limit)`; every returned message is validated against
   `history_chat_id`, not necessarily the original resolved chat.

A live `getMessageThread` response is structurally valid only when both ids are
nonzero int53, it contains at least one non-null valid starting `MessageSummary`,
all starting messages have `chat_id == history_chat_id` and strictly decreasing
unique ids, and the last starting-message id equals `history_thread_id`. If
`history_chat_id == resolved_chat_id`, `history_thread_id` must equal the requested
`thread:<id>`. If the chat ids differ, the resolved chat must be a `channel`; the
history thread id may differ from the channel message id because pinned TDLib maps
comments to the linked supergroup. Any wrong response type, null entry, id/type/chat
inconsistency or malformed starting message is `INTERNAL` for `read` before a date
or history call.
Only that validated live channel/thread mapping may have
`history_chat_id != resolved_chat_id`; every other admitted branch requires exact
equality. From this point through page scanning, anchor removal, topic/time
filtering, cutoff matching and result integrity, the expected message chat is
always `history_chat_id`.

When both `--before` and `--until` are present, the until call remains the required
existence probe, but its returned id is not compared with `--before` and is not the
history start. History starts from explicit `before` and removes it when returned;
every new raw message is independently required to satisfy `date <= until_floor`
before output. This implements the exact intersection without assuming any
relationship between message-id and date order. Without `--before`, a successful
until probe supplies an inclusive first history anchor and that first message is not
removed. Without either operand, the first history anchor is zero.

A continuation performs argument/cursor validation, Ready/getMe and bot preflight,
resolves the cursor's numeric chat id, repeats the Saved ownership check when
applicable, and starts exclusively after the cursor's raw anchor. A live thread
continuation repeats exactly one `getMessageThread` metadata call and requires its
validated `history_chat_id` to equal the cursor field; a mismatch is
`USAGE/cursor_scope_mismatch`. It does not repeat first-page until/since date probes.
A syntactically valid same-scope modified cursor remains a new explicit read request
under §5.2 and carries no proof that tgcli emitted it.

The exact relationship between `boundary` and `next` is:

| `boundary` | condition in this invocation | `next` |
|---|---|---|
| `page` | at least one new raw tdlib message was consumed and `time_anchor` was not reached | a cursor strictly after the last consumed raw message |
| `time_anchor` | the exact inclusive `--since` cutoff anchor was reached | null |
| `empty_before_until` | the initial live `getChatMessageByDate` for `--until` returned 404 | null |
| `local_boundary` | a local invocation consumed no new raw message from its input anchor | null |
| `tdlib_idle` | one structurally valid live history response produced zero new raw messages after optional inclusive-anchor removal | null |

`next` is never the input cursor and is emitted only after strict raw progress.
Consequently a filtered page may be
`{"items":[],"next":<advanced cursor>,"boundary":"page"}`. If a local
boundary or live idle condition is encountered after raw progress, that
invocation still returns `page` and its advancing cursor; an unchanged next
invocation exposes the terminal boundary with null `next`. This preserves the
resume anchor without allowing a same-state cursor loop. `time_anchor` is
terminal even when the invocation made raw progress. There is no live idle timer,
poll, retry, or short-page inference: one valid zero-progress response is
`tdlib_idle`, while every valid short response with progress continues unless the
output limit or a terminal time anchor was reached.

Live reads use `getChatHistory` or the exact topic-history function above.
Every returned message is checked by timestamp. A zero tdlib date, represented as
`date:null` in `MessageSummary`, satisfies neither a since nor an until predicate
and is consumed but filtered when either bound is present. The scan terminates at
the exact non-null cutoff anchor, before emitting it, never merely because a
message has `date < since`. With the `INT32_MIN` or 404 no-cutoff branch, live
termination is `tdlib_idle` unless another exact terminal wins.

Local reads call only `getChatHistory(..., only_local=true)`, apply topic/time
filters while scanning the entire available continuous prefix, and terminate
at `local_boundary`; they never call `getChatMessageByDate`. tdlib's public
local iterator stops where continuity is unknown and does not distinguish a
gap from the true oldest message. In particular, crossing or filtering below a
local `since` timestamp does not produce `time_anchor` and does not stop the scan;
only a live invocation with the exact date-derived cutoff id can do so.

An unanchored or inclusive-until request asks for at most `remaining`; an
exclusive before/cursor request asks for at most `remaining + 1`. Offset is always
zero. Remove exactly one first message whose id equals the exclusive input anchor;
if that anchor disappeared, accept the first strictly older new id and consume at
most `remaining` entries so no unreturned raw entry is skipped. The scanner records
the last consumed raw message before applying topic/time filters and has no bounded
raw cap.

A history response is structurally valid only when `total_count` is nonnegative and
not smaller than the response vector length. `total_count` is informational and no
value of it claims continuation: pinned plain-chat history reports a page-derived
count while topic history may report the whole topic total. Pagination is therefore
driven only by concrete non-null message ids. An empty vector, a null-only vector,
or an anchor-only vector is valid zero raw progress regardless of `total_count` and
produces `local_boundary` or `tdlib_idle` as above. A null mixed with a non-null new
message is malformed `INTERNAL` because its ordered position cannot be validated.

After optional anchor removal, every non-null raw message must have
`chat_id == history_chat_id`, a valid shared `MessageSummary`, and a unique id
strictly below the preceding
new id and, for an exclusive input anchor, strictly below that anchor. A repeated,
equal, increasing or otherwise non-advancing raw id is `PAGINATION_INVALID` with
`{"operation":"read","reason":"non_advancing_upstream"}`. Invalid count,
mixed-null, `chat_id != history_chat_id` or invalid-DTO responses are `INTERNAL`
with the normal
`read` details. Neither failure emits a result or partial stdout.

The implementation dependency order is:

1. land the shared `TopicRef`/`MessageSummary` conversion, `TdMessages` including
   `total_count` and null positions, strict `TdMessageThreadInfo`, and typed
   non-terminal `ResolverConsumer`;
2. add pure timestamp/topic parsing, the exact read cursor and raw-page scanner;
3. add the seven typed TD boundary calls (`getChatHistory`, `getChatMessageByDate`,
   `getMessageThread` and four topic-history calls) and fake/native conversion
   coverage;
4. add the one `read` coordinator and canonical `history` parser alias; and
5. add strict result schema/manifest validation, equivalent human goldens and the
   complete fake/native/gate matrix. No step registers a partially implemented
   public command.

#### `msg get`, `msg link`, and `resolve`

The shared chat resolver exposes one request-scoped, typed, non-terminal
consumer. It does not redefine §5.2's authoritative ten-value `M2Operation`.
Caller attribution instead uses this distinct extensible type:

```cpp
using ResolverCaller = std::variant<M2Operation, proto::M3Operation>;

enum class ResolverStop { Cancelled };

struct ResolverUsageError {
    std::string argument;
    UsageReason reason;
};
struct ResolverNotAuthenticatedError {
    std::string account;
    AuthState state;
    NotAuthedReason reason;
};
struct ResolverBotUnsupportedError { ResolverCaller operation; };
struct ResolverNotFoundError {
    std::string selector;
    std::optional<ResolverScope> scope;
};
struct ResolverAmbiguousError {
    std::string selector;
    ResolverScope scope;
    std::vector<ChatIdentity> candidates;
    bool truncated;
};
struct ResolverRateLimitedError {
    ResolverCaller operation;
    std::int32_t retry_after;
};
struct ResolverTdlibError {
    ResolverCaller operation;
    std::int32_t tdlib_code;
};
struct ResolverTimeoutError {
    ResolverCaller operation;
    std::optional<AuthState> state;
};
struct ResolverInternalError { ResolverCaller operation; };

using ResolverError =
    std::variant<ResolverUsageError, ResolverNotAuthenticatedError,
                 ResolverBotUnsupportedError, ResolverNotFoundError,
                 ResolverAmbiguousError, ResolverRateLimitedError,
                 ResolverTdlibError, ResolverTimeoutError,
                 ResolverInternalError>;

struct ResolverPrincipal {
    std::int64_t id;
    bool is_bot;
};

struct ResolvedChatTarget {
    ResolverPrincipal principal;
    ChatIdentity chat;
    std::optional<std::int64_t> contextual_message_id;
    std::optional<TopicRef> contextual_topic;
    std::optional<ResolvedLinkType> link_type;
    std::optional<bool> is_public;
};

using ResolverPrincipalOutcome =
    std::variant<ResolverPrincipal, ResolverError, ResolverStop>;
using ResolverOutcome =
    std::variant<ResolvedChatTarget, ResolverError, ResolverStop>;

class ResolverConsumer {
  public:
    ResolverPrincipalOutcome bind_principal(ResolverCaller caller);
    ResolverOutcome resolve_chat(std::string selector, ResolverScope scope);
    ReadyReadResult read_target(const ReadyReadStart& start);
};
```

`M2Operation` retains exactly `chats`, `read`, `msg_get`, `msg_link`,
`search`, `unread`, `fetch`, `resolve`, `chat_info`, and `chat_members` from
§5.2. `proto::M3Operation` retains the frozen seventeen M3/M4 canonical
operation identities from §4.5.1, including `saved_attach`; the resolver does
not create a parallel mutation-operation enum. `ResolverCaller` serializes
through the canonical identity of its held operation. A selector or identity-
enrichment failure is represented with
`ResolverCaller{M2Operation::Resolve}` even for an M3/M4 consumer.

`ResolvedLinkType` is the closed §4.4 `link_type` enum. `ResolverError` is a
typed tagged union, not a terminal frame or free-form JSON value. Its variants
carry exactly the closed payload already defined in §§4.1 and 5.2. Rendering
that error to the standard envelope is the owning command adapter's
responsibility.

One `ResolverConsumer` owns one `ReadyReadSession`, its advancing immutable
authorization snapshot, and the request's already-computed absolute deadline.
`bind_principal` is called exactly once and performs Ready then one correlated
`getMe`. Neither `bind_principal`, `resolve_chat`, nor `read_target` calls
`RequestSession::result` or `RequestSession::error`.

`bind_principal` and `resolve_chat` map an existing
`ReadyReadStatus::Cancelled` to `ResolverStop::Cancelled`; their caller emits
no terminal for that stop. `read_target` does not return `ResolverStop`: it
returns the existing `ReadyReadResult` unchanged, including
`ReadyReadStatus::Cancelled`. The outer message command owns the exact mapping
of Response, AuthorizationLost, TimedOut, Failed, and Cancelled; Cancelled
emits no terminal, while the other non-response states use the existing M2
error rules. Disconnect and shutdown reach this same non-terminal Cancelled
path through `RequestSession` cancellation. The public `resolve` handler is an
adapter over the typed bind/resolve outcomes and remains responsible for its
one terminal result or error.

A Ready failure or `getMe` failure before selector resolution begins is
attributed to the caller passed to `bind_principal`: `msg_get` or `msg_link`
here, and the exact outer M2/M3/M4 canonical identity for later consumers.
Once `resolve_chat` begins, every selector and identity-enrichment failure
retains `operation:"resolve"`, including timeout, 429, other tdlib errors, and
internal integrity failures. After a successful resolution, a `read_target`
failure from `getMessages` or `getMessageLink` is attributed to `msg_get` or
`msg_link`, respectively. All phases reuse the one absolute request deadline;
none resets it or performs a second `getMe`.

`ResolvedChatTarget` always retains message/topic/link metadata supplied by a
message or topic selector. `msg get` and `msg link` use only its immutable
`chat` identity. Their explicit positional message ids always govern the
target request: contextual `message_id` or `topic` neither overrides nor
validates a positional id.

The implementation dependency order for this slice is exact: (1) shared M2
DTOs plus neutral/native message conversion; (2) `ResolverConsumer` while the
public `resolve` adapter remains green; (3) the `msg get`/`msg link` consumers,
including their result schemas, manifest entries, human goldens, and focused
tests. TODO.md carries the same order and assigns each deliverable once.

`msg get` accepts 1 through 100 message ids and always returns
`{"items":[<MessageSummary>],"next":null}`. A single id still uses the list
shape. `getMessages` result order follows argv order and duplicate ids remain
duplicated. It makes exactly one `getMessages(chat_id, message_ids)` target
call after resolution.

The complete response is validated before classifying missing positions. A
response vector length different from the input length, or any non-null
position whose message has a wrong `chat_id`, a wrong positional `id`, or an
invalid `MessageSummary`, is `INTERNAL` with
`{"operation":"msg_get","reason":"internal_error"}`. This integrity failure
wins even when another position is null. Only a structurally valid response
with one or more null positions is `NOT_FOUND`; the command is atomic and
emits no result or partial stdout. `NOT_FOUND.details` is exactly
`{"chat_id":integer,"missing_ids":[integer...]}`, with missing ids unique in
first-occurrence order.

`msg link` calls exactly
`getMessageLink(chat_id, message_id, 0, 0, "", false, false)` and returns:

```json
{"chat_id":-1001,"message_id":123,"link":"https://t.me/example/7","is_public":true}
```

The returned `messageLink.link` must be non-empty valid UTF-8. An invalid value
is `INTERNAL` with `{"operation":"msg_link","reason":"internal_error"}`.
No URL scheme, host, path, or t.me pattern is imposed. Accordingly,
`msg-link.result.schema.json` uses only `{"type":"string","minLength":1}`
for `link`. A documented 404 is `NOT_FOUND`; an eligibility or permission
error is `TDLIB_ERROR`.

Both commands are Read-tier and allow authenticated user and bot principals.
The selector-specific §4.1 matrix is unchanged: user-only title, invite,
direct-message, and Saved Messages branches still return the existing
`BOT_UNSUPPORTED` resolver error at their specified point. A secret resolved
chat returns `USAGE/unsupported_chat_type` before either target call.
Neither command accepts a cursor; `msg get`'s `next:null` is terminal and not a
continuation token.

The exact human rendering is frozen by `tests/golden/msg-get.txt` and
`tests/golden/msg-link.txt`. `msg get` prints this tab-separated form, using
compact JSON for every non-integer value so untrusted strings remain escaped;
each `\t` below denotes one literal U+0009 tab byte:

```text
id\tchat_id\tdate\tsender\tis_outgoing\ttopic\ttype\ttext
123\t-1001\t"2026-08-05T10:00:00Z"\t{"type":"user","id":42}\tfalse\t{"kind":"forum","id":7}\t"text"\t"message or caption"
next\tnull
```

`msg link` prints:

```text
chat_id\t-1001
message_id\t123
link\t"https://t.me/example/7"
is_public\ttrue
```

`resolve` applies §4.1 and returns:

```json
{
  "kind":"message",
  "chat":{"id":-1001,"title":"Project","type":"supergroup","is_bot":false,"usernames":["project"]},
  "message_id":123,
  "topic":{"kind":"forum","id":7},
  "link_type":"message",
  "is_public":true
}
```

`kind` is `chat`, `message`, or `topic`; message takes precedence over topic,
which takes precedence over chat. `message_id` and `topic` are nullable.
`link_type` is null or `public_chat`, `bot_start`, `message`, `chat_invite`,
`direct_messages_chat`, or `saved_messages`. `is_public` is nullable and comes
only from tdlib link/invite metadata, never from a username inference.

#### `search`

Without `--chat`, search is global; `--global` is an explicit equivalent and
is mutually exclusive with `--chat`. Query bytes are exact, valid UTF-8 and
non-empty. Before the first TD call, tgcli applies the pinned
`clean_input_string` algorithm to a copy and accepts only byte-identical,
nonempty output. This rejects controls, CR removal, the pinned length cleanup
and every other source normalization before normalized args, cursor or TD work.
Every global continuation `next_offset` decoded from an untrusted cursor is
subject to the same equality/nonempty check. Per-chat search uses
`searchChatMessages`; global search uses
`searchMessages(chat_list=null)`, covering Main and Archive but not secret
chats. Results are `{"items":[<MessageSummary>],"next":<cursor|null>}`.

`--type` defaults to the literal `any`, which uses an empty tdlib filter and no
content postfilter. `photo`, `video`, `doc`, `link`, and `voice` map respectively to tdlib's Photo,
Video, Document, Url, and VoiceNote search filters. `text` uses an empty tdlib
filter followed by an exact content-class check for `messageText` or
`messageAnimatedEmoji`. Per-chat `--from` is the tdlib `sender_id`; global
`--from` is an exact client-side `message.sender_id` filter after server
search.

The scanner fills `-n` for both scopes, including text postfiltering, until it
has enough matches, upstream exhaustion or deadline. Global `--from` remains a
postfilter. Before postfiltering every response must be the exact expected
FoundChatMessages/FoundMessages, have `total_count == -1` or
`0..INT32_MAX`, contain no more than the requested limit (at most 100), contain
no null or malformed summaries, match the resolved scope/filter, and introduce
no duplicate `(chat_id,message_id)` in the invocation. Per-chat raw ids are
strictly decreasing. Global raw `(date,chat_id,message_id)` keys are strictly
lexicographically decreasing, including across cursor invocations.

One search invocation scans at most 4,096 raw message items and retains at
most 1,048,576 cumulative bytes across unique opaque global cursor markers.
The current cursor input marker is included in that byte charge. Each page is
capacity-checked before any raw item is inserted, and each clean, advancing,
previously unseen marker is capacity-checked before insertion or byte
addition. Limit+1 is atomically `RESOURCE_LIMIT` with exact details
`{"operation":"search","resource":"raw_scanned_items","limit":4096}` or
`{"operation":"search","resource":"cursor_marker_bytes","limit":1048576}`;
no partial result is emitted. Duplicate/cyclic marker classification still
precedes a byte charge because a seen marker is never inserted.

Each request asks for `min(100, remaining result slots)`. A per-chat first-page
marker of zero may return a positive continuation. A continuation accepts only
zero exhaustion or `0 < next < input`; a positive next must also be below the
last raw message id in that page. Equality/increase is
`PAGINATION_INVALID/marker_not_advancing`. A global marker is opaque but must
be clean, nonempty and different from its input, and an invocation rejects any
repeated/cyclic offset. The cursor carries the exact query, resolved sender,
scope, filter, page size and upstream state plus `last_raw_message_id` for chat
scope or `last_raw_order:{date,chat_id,message_id}` for global scope; the
inactive raw-order field is null. Search never calls local history APIs.

The first per-chat request is exactly
`searchChatMessages(chat_id,null,query,sender,0,0,page_limit,filter)`; every
continuation replaces only `from_message_id` with the validated upstream
marker. The first global request is exactly
`searchMessages(null,query,"",page_limit,filter,chat_type_filter=null,0,0)`;
continuations replace only the opaque offset. `sender` is null without
`--from`; a global resolved sender remains a local postfilter and is not sent
to TDLib. `page_limit` is `min(100, remaining)` before postfiltering.

The version-1 search cursor is exactly:

```json
{"version":1,"operation":"search","account":"main","user_id":42,"limit":20,"query":"needle","scope":"chat","chat_id":-1001,"sender_user_id":7,"type":"any","next_offset_message_id":123,"next_offset":null,"last_raw_message_id":124,"last_raw_order":null}
```

For `scope:"chat"`, `chat_id`, positive `next_offset_message_id`, and nonzero
`last_raw_message_id` are required, with the strict relation
`0 < next_offset_message_id < last_raw_message_id`, while `next_offset` and
`last_raw_order` are null. For `scope:"global"`, `chat_id`,
`next_offset_message_id`, and
`last_raw_message_id` are null; clean nonempty `next_offset` and
`last_raw_order:{date:int32_nonnegative,chat_id:nonzero_int53,message_id:nonzero_int53}`
are required. `sender_user_id` is null or the resolved positive user id.
`query`, `type`, limit, account and current user are authoritative token state;
any first-page selector spelling is absent. The decoder enforces the exact key
set, scalar ranges, cross-field rules, current account/user, clean query and
offset equality, declared scope/filter and strict raw-order continuation before
the next TD call.

#### `unread`

`unread` completely loads Main and then Archive with the same growing-prefix
and documented-end rules as `chats`, excludes secret chats, deduplicates a chat
across those lists and does not add folder lists. It applies the shared unread
predicate and preserves Main-then-Archive tdlib order. The exact result is
`{"items":[<UnreadSummary>],"next":null}`, where `UnreadSummary` is:

```json
{
  "id":-1001,
  "title":"Project",
  "type":"supergroup",
  "is_bot":false,
  "is_archived":false,
  "is_marked_unread":false,
  "unread_count":3,
  "unread_mention_count":1,
  "unread_reaction_count":0,
  "unread_poll_vote_count":0
}
```

#### `chat info` and `chat members`

`chat info` returns exactly the following strict object:

```json
{
  "id":-1001,
  "title":"Project",
  "type":"supergroup",
  "is_bot":false,
  "usernames":["project"],
  "description":"project room",
  "member_count":42,
  "is_forum":true,
  "linked_chat_id":null,
  "is_archived":false,
  "folder_ids":[2],
  "is_marked_unread":false,
  "unread_count":3,
  "unread_mention_count":1,
  "unread_reaction_count":0,
  "unread_poll_vote_count":0
}
```

The type-specific sources are closed:

| chat type | `description` | `member_count` | `is_forum` | `linked_chat_id` |
|---|---|---|---|---|
| private | `userFullInfo.bio.text` | null | false | null |
| basic group | `basicGroupFullInfo.description` | required non-null `basicGroup.member_count` | false | null |
| supergroup | `supergroupFullInfo.description` | required non-null nonnegative `supergroupFullInfo.member_count` | `supergroup.is_forum` | full-info zero becomes null |
| channel | `supergroupFullInfo.description` | required non-null nonnegative `supergroupFullInfo.member_count` | false | full-info zero becomes null |

List/unread fields come from the already observed resolver `chat`; active
usernames retain tdlib order and no second `getChat` is sent. Private branches
use exact `getUserFullInfo`, basic groups use exact `getBasicGroup` plus
`getBasicGroupFullInfo`, and supergroups/channels use the observed supergroup
plus exact `getSupergroupFullInfo`. Every returned type/id and nested optional
is validated before any result field is published. Secret targets fail
`USAGE/unsupported_chat_type`; authenticated bots are allowed.
An observed channel with `supergroup.is_forum:true` is a malformed TDLib
response and fails atomically as `INTERNAL/malformed_tdlib_response` before
`getSupergroupFullInfo` or result publication.

Private and secret targets are unsupported for `chat members`. Basic groups
use `basicGroupFullInfo.members` and local filtering/pagination. Supergroups
and channels use `getSupergroupMembers` with Recent by default,
Administrators for `--admins`, Bots for `--bots`, and Search for `--query`;
the three filters are mutually exclusive. A supergroup cursor contains the
tdlib offset. Exactly one raw `getSupergroupMembers` page is read per command
invocation. An empty page returns `next:null`; every nonempty page returns
immediately with a continuation cursor whose offset advances by the raw page
length, including a short page or a page filtered to zero output rows. There
is no internal empty probe, and approximate `total_count` never proves
exhaustion. A continuation that receives an empty page proves exhaustion. A
basic-group cursor uses the exact vector offset and vector length for exhaustion.

For the basic-group vector, `--admins` retains exactly `creator` and
`administrator` statuses. `--bots` retains only user senders whose exact
`getUser` identity has `is_bot:true`; chat senders never match. `--query Q`
uses a byte-exact case-sensitive substring over the derived `display_name` or
any active username. The selected predicate is applied to the fully validated
and identity-enriched source vector before pagination; slicing first is
forbidden. Default `recent` retains the full vector in TD order.

Default filtering is literal `recent` with null query; `--admins` selects
`administrators`, `--bots` selects `bots`, and `--query Q` selects `search`
with a byte-identical valid UTF-8 `Q` of 1..256 bytes after pinned input
cleaning. A first page has offset zero. Supergroup/channel responses require
`total_count>=0`, `total_count>=members.size()`, at most the requested page
size, and no null member. A nonempty page advances raw offset by its full
unfiltered vector length, even if later identity enrichment fails; a short page
does not prove exhaustion. Exactly one empty request at the next raw offset
proves exhaustion. Basic-group processing first atomically validates and
identity-enriches the entire full-info vector, then applies the selected
`recent|admins|bots|search` predicate to that whole vector. Only then may it
paginate: `source_count` is the exact filtered-vector length stored in the
first-page cursor, and offset slices the filtered vector rather than the
unfiltered TD vector.
Continuation repeats full-vector validation, enrichment and filtering, then
must compare its current filtered length to cursor `source_count` before
slicing. A mismatch is `PAGINATION_INVALID` with
`{"operation":"chat_members","reason":"source_changed"}` and returns no
partial rows. Therefore an unchanged full-vector length does not hide a
status, `getUser.is_bot`, `display_name` or active-username change that changes
the selected count. An unchanged filtered count does not freeze order or
member identity: v1 accepts that continuation as a fresh live view, so
reordering or equal-count membership replacement can shift rows across page
boundaries; the cursor is not a snapshot.

The exact cursor is:

```json
{"version":1,"operation":"chat_members","account":"main","user_id":42,"limit":50,"chat_id":-1001,"chat_type":"supergroup","source_id":55,"filter":"recent","query":null,"offset":50,"source_count":null}
```

`chat_type` is `basic_group|supergroup|channel`; `source_id` is the positive
observed basic-group or supergroup id backing `chat_id`. Continuation resolves
the live chat again and requires the same `chat_id`, `chat_type`, and
`source_id` before any member read. Basic-group cursors require a nonnegative
`source_count` equal to the newly validated and selected filtered-vector
length;
supergroup/channel cursors require it null. `query` is nonempty only with
`filter:"search"`, and every other filter requires null. For `basic_group`,
`offset` is a nonnegative int32 index into the fully validated,
identity-enriched and filtered vector. For `supergroup` and `channel`, `offset`
is the raw TD `getSupergroupMembers` offset advanced by the raw returned page
length, not by the number of enriched or emitted rows; a continuation
invocation's single empty page at the next raw offset is the only exhaustion
proof. Account, current
user, chat id/type, limit, filter/query, source identity/count relation and
canonical re-encoding are all validated before any member RPC. A live type or
backing-id change is `source_changed`.

A cursor query for `filter:"search"` is 1..256 bytes, valid UTF-8, and
byte-identical after the pinned cleaner. Those bounds are enforced again while
decoding the untrusted cursor before any RPC.

Member page validation is owner-aware. Creator status permits either
`is_member` value; administrator/member require true; left/banned require
false. Restricted is valid only for a supergroup owner. Basic groups reject
chat senders. Supergroup/channel chat senders are valid only with left or
banned status, and identity enrichment must resolve the referenced sender to
a supergroup or channel. The entire raw page passes status/sender structural
validation before any identity hydration or output; referenced chat kind is a
second atomic validation after hydration.

The result is `{"items":[<MemberSummary>],"next":<cursor|null>}`.
`MemberSummary` is:

```json
{
  "sender":{"type":"chat","id":-1002},
  "display_name":"Linked channel",
  "usernames":["linked"],
  "is_bot":false,
  "status":"banned",
  "tag":"",
  "joined_at":null
}
```

`status` is `creator`, `administrator`, `member`, `restricted`, `left`, or
`banned`; zero join time becomes null, otherwise it is UTC RFC 3339 seconds.
For a user sender, display name is `first_name` plus one space and `last_name`
only when `last_name` is non-empty, usernames are active in tdlib order, and
bot status comes from the user type. For a chat sender, display name is `chat.title`,
usernames come from chat-type metadata, and `is_bot` is false. `tag` is the
literal pinned `chatMember.tag` for every status, including member,
restricted, left and banned; tgcli does not synthesize it from a custom title
or clear it for a status. `joined_at` comes from tdlib's joined-chat date.

The chat-sender branch is normative: pinned `chatMember` permits other chats
as Left/Banned senders in supergroups and channels. tgcli enriches user senders
through exact `getUser` and chat senders through exact `getChat`, preserves
input order, and validates the whole page atomically. It never rejects a
structurally valid chat sender merely because it is not a user.

#### `fetch`

With none of `--limit`, `--all`, or `--since`, `fetch` targets a depth of 100.
`--limit` accepts 1 through 1000000 and is mutually exclusive with `--all`;
`--since` may accompany either. `--since` without a limit means all available
history down to its inclusive anchor. Fetch has no default deadline; an
explicit `--timeout` is one finite absolute deadline from daemon request
admission through config admission, RequestSession construction, removal and
logout recovery, Ready/getMe, resolution, the optional since probe, local scan,
and network fill. With no explicit timeout the fetch deadline is tagged
unlimited; cancellation, disconnect, daemon shutdown and authorization loss
remain active.

The public tdlib local-history seam exposes only the continuous prefix
reachable from newest history; it does not label the boundary as a gap or true
oldest message. Only after §6's removal then logout recovery preflights complete,
Ready/getMe user-only admission succeeds, and an ActiveDialogs non-secret chat
is resolved does fetch perform this exact history sequence:

1. Enter `local_scan`. If `--since` is present and its rounded second is not
   `INT32_MIN`, make exactly one
   `getChatMessageByDate(chat_id, since - 1)` call before any `getChatHistory`
   call. A valid message is the exclusive cutoff anchor. A 404 leaves the
   anchor absent; other errors use normal M2 mapping. The response must be an
   exact valid shared `MessageSummary` for `chat_id`, with nonzero date no later
   than the requested second. Wrong type/chat, zero or too-new date, null or
   malformed response is `INTERNAL` for `fetch`. At `INT32_MIN` the call is
   skipped and the cutoff remains absent. The probe is never counted directly.
2. Scan the continuous local prefix from newest with
   `getChatHistory(chat_id, from_message_id, 0, 100, true)`. The first call uses
   zero; every later call uses the oldest counted id and removes one returned
   inclusive duplicate. After each complete advancing response latch whether
   the numeric limit and exact since cutoff have been observed. Latches are
   monotonic request-local facts and emit no terminal yet. Continue through all
   short advancing responses until one structurally valid response has zero new
   messages. A deadline may win before that local boundary even after either
   target condition was latched.
3. At the sealed local boundary, an observed since latch wins over a numeric
   latch and returns `since_anchor_reached`; otherwise a numeric latch returns
   `target_reached`. Only when neither latch is set does fetch switch once to
   `network_fill` and call
   `getChatHistory(chat_id, from_message_id, 0, 100, false)` from the local
   boundary, again removing one inclusive duplicate.
4. In `network_fill`, incorporate an entire advancing response, update both
   latches, emit its one progress frame, and immediately evaluate the latches;
   since wins a same-response tie. If neither is set, continue through every
   short advancing page. One valid zero-progress response is `tdlib_idle`.
   There is no retry, poll, short-page inference or remote-EOF claim.

The cutoff construction and per-message timestamp classification are exactly
the live-`read` rules: date zero satisfies no since predicate, a date below
`since` does not itself stop an id-ordered scan, and only the exact non-null
cutoff id sets the since latch. A missing cutoff never proves the objective.
Every raw message returned by TDLib is counted in the invocation-observed cached
prefix, including page-level overfetch; fetch cannot undo TDLib caching.

Fetch inherits the live/local `read` history-page contract in full. A response
has nonnegative `total_count` no smaller than its vector length; the count is
informational only. After optional removal of exactly one leading inclusive
anchor, every non-null message is a valid shared `MessageSummary` for `chat_id`,
ids are unique and strictly decreasing, and the first new id is strictly below
the input anchor when present. A missing anchor is accepted if the first new id
is strictly older. Empty, null-only and anchor-only responses are valid zero
progress. A null mixed with a new non-null message, invalid count, wrong chat or
malformed DTO is `INTERNAL`; repeated, equal or increasing ids are
`PAGINATION_INVALID` with
`{"operation":"fetch","reason":"non_advancing_upstream"}`. No partial result
is emitted.

The prefix is an invocation snapshot, not a transaction over concurrent TDLib
updates. Its top is the newest message in the first advancing local response,
or, when local is empty, in the first advancing live response. Subsequent
anchored pages extend only that prefix downward. Messages arriving above that
top afterward are excluded; there is no final rescan or terminal-time snapshot
claim. Anchor deletion is harmless under the strictly-older rule. A later
invocation starts from newest. Disconnected cached islands below a zero-progress
local boundary are never selected as anchors. No tgcli cursor, resume file or
daemon-side fetch state is persisted.

`cached_count` is a checked JSON uint64 count of messages in this snapshot.
Overflow is `INTERNAL` for `fetch` and emits neither progress for the overflowing
page nor a result. When count is zero, both boundary ids are null. When positive,
both fields are serialized from the same `std::int64_t` nonzero-int53 boundary
value and are therefore numerically equal.

Success is exactly:

```json
{
  "chat_id":-1001,
  "cached_count":250,
  "oldest_message_id":123,
  "target":{"limit":1000,"all":false,"since":null},
  "target_reached":false,
  "stop_reason":"tdlib_idle",
  "resume_from_message_id":123
}
```

For an empty prefix, `oldest_message_id` and `resume_from_message_id` are null.
Within `target`, `limit` is the numeric depth or null, `all` records whether
`--all` was supplied, and `since` is the rounded UTC RFC 3339 boundary or null;
the only valid target shapes are:

| invocation | exact `target` |
|---|---|
| no target flag | `{"limit":100,"all":false,"since":null}` |
| `--limit N` with optional since | `{"limit":N,"all":false,"since":<time-or-null>}` |
| `--since TS` only | `{"limit":null,"all":false,"since":<time>}` |
| `--all` with optional since | `{"limit":null,"all":true,"since":<time-or-null>}` |

`{"limit":null,"all":false,"since":null}` and a non-null limit with
`all:true` are invalid. A target is finite exactly when limit or since is
non-null. Bare `--all` is the sole unbounded target.

Terminal target fields have this exact runtime truth table:

| condition | `stop_reason` | `target_reached` |
|---|---|---|
| local boundary reached with since latch set | `since_anchor_reached` | `true` |
| local boundary reached with only numeric latch set | `target_reached` | `true` |
| live advancing page sets since latch | `since_anchor_reached` | `true` |
| live advancing page sets only numeric latch | `target_reached` | `true` |
| live zero progress with finite target unmet | `tdlib_idle` | `false` |
| live zero progress for bare `--all` | `tdlib_idle` | `null` |

A since latch wins over a numeric latch regardless of the pages on which they
were set. No local success exists before the zero-progress local boundary.
A page is incorporated completely before latch evaluation. A response observed
strictly before a finite deadline may be incorporated; deadline equality or a
later response makes timeout win. `--all` is not a certificate that remote
history is complete. Public tdlib exposes no remote-EOF proof, so neither
`complete` nor `history_end` exists.

The standard Draft 2020-12 result schema asserts every expressible structural
and cross-property relation:

- exact result/target fields, individual uint64/int53/scalar ranges and real
  signed-int32 UTC timestamp syntax;
- the four target shapes above;
- `cached_count:0` requires both boundary ids null, while
  `cached_count >= 1` requires both non-null int53 values;
- `target_reached` and `since_anchor_reached` stop reasons require
  `target_reached:true`;
- `tdlib_idle` requires `target_reached:false` for a structurally finite target
  and null only for bare `--all`;
- `since_anchor_reached` requires a non-null target since; and
- `target_reached` stop reason requires a non-null numeric target limit.

The schema cannot compare two properties numerically or inspect coordinator
history. The exact additional runtime-only semantic rules are therefore:

- the two non-null boundary ids are numerically equal because both are emitted
  from the same coordinator integer;
- numeric latch/success requires the committed count to be at least the parsed
  numeric limit;
- since latch/success requires observation of the exact probe-derived cutoff id;
  and
- local-boundary sealing, phase transitions, latch timing and page incorporation
  match the coordinator history described above.

Each standard-expressible branch has ordinary-schema positive/negative tests;
each runtime-only rule has paired runtime accept/reject tests whose base values
remain schema-valid. Every runtime-accepted output also passes the ordinary
schema, and runtime never makes a schema-invalid value valid.

Progress is emitted exactly once after each structurally valid local or live
history response that contributes at least one new raw message, after the whole
response is incorporated and before any boundary/target terminal. The since
probe and zero-progress responses emit none. There is no separate initial or
final progress frame. The protocol progress payload is exactly:

```json
{"operation":"fetch","chat_id":-1001,"cached":250,"target":1000,"oldest_message_id":123}
```

`target` is null when no numeric depth target exists and `oldest_message_id`
is null iff `cached` is zero; otherwise it is serialized from the same current
numeric boundary value as the eventual result fields. In `--json` mode the
existing client wrapper writes these exact compact bytes plus LF:

```text
{"progress":{"cached":250,"chat_id":-1001,"oldest_message_id":123,"operation":"fetch","target":1000}}
```

Human mode writes these exact compact bytes plus LF:

```text
progress: {"cached":250,"chat_id":-1001,"oldest_message_id":123,"operation":"fetch","target":1000}
```

There is no TTY-specific alternate rendering in M2. Timeout preserves warmed
TDLib state and uses §5.2's phase-dependent details; it never emits success.

### 4.5 M3 write and minimal M4 Saved attachment contract

This subsection is the normative additive contract for M3 and the minimal M4
`saved attach` slice. It preserves the canonical M0–M2 contracts except for
the protocol/dry-run delta already frozen in §§6 and 10. All JSON objects
defined here are strict and use `additionalProperties:false`.

#### 4.5.1 Базовые типы, tier и bot matrix

Переиспользуются exact M2 `ChatIdentity`, `TopicRef`, `MessageSenderRef`,
`MessageSummary`. `int53` — JSON integer в safe signed range; chat id nonzero,
message/user/topic ids положительные там, где это требует M2.

`MessageWriteResult` имеет ровно поля `MessageSummary` плюс `scheduled:boolean`.
`date` null тогда и только тогда, когда возвращённое TDLib Message имеет
non-null `scheduling_state`; `scheduled` вычисляется из фактического Message,
не из request.

Closed operations:

```text
send, msg_edit, msg_delete, msg_forward, msg_react, msg_pin, msg_unpin,
chat_mark_read, chat_mute, chat_unmute, chat_pin, chat_unpin,
chat_archive, chat_unarchive, chat_join, chat_leave, saved_attach
```

| Operation | Tier | Bot |
|---|---|---|
| `send` without schedule | Write | allowed |
| `send` with any schedule | Write | `BOT_UNSUPPORTED` preflight |
| `msg_edit`, `msg_delete`, `msg_forward`, `msg_pin`, `msg_unpin` | Write except delete Destructive | allowed |
| `msg_react` | Write | `BOT_UNSUPPORTED` preflight |
| `chat_leave` | Destructive | allowed |
| all other `chat_*` | Write | `BOT_UNSUPPORTED` preflight |
| `saved_attach` | Write | `BOT_UNSUPPORTED` namespace preflight |

`msg_delete` и `chat_leave` расширяют `destructive_action`. Ready, then `getMe`,
then bot classification precede bot decision. `msg_react` rejects a bot before
`getMessageAvailableReactions`, `getMessageProperties`, `addMessageReaction`
или `removeMessageReaction`. Bot scheduled send rejects before resolver,
`getOption("unix_time")` и `sendMessage`.

#### 4.5.2 Admission и immutable plan

Real new invocation order:

1. strict CLI/frame/config parse;
2. frozen account match, Ready snapshot;
3. correlated `getMe`, principal binding, bot classification;
4. bot preflight;
5. write authority;
6. acquire the initial outer-account-mutex epoch; perform the complete
   account-global spool gate, known-pin audit/store reconciliation and expiry;
7. while that epoch remains held, parse/hash caller-controlled inputs, compute
   the request fingerprint and perform the locked lookup; `saved_attach` also
   completes pass 1 and its source SHA-256 before this lookup;
8. completed replay/pending/conflict handling occurs under that epoch;
   destructive completed replay freshly confirms its exact stored plan;
9. on a miss release the initial epoch, then perform exact-only M2 target
   resolution, read-only property validation and immutable proposed-plan
   planning-result construction, including whether the proposed plan would
   require confirmation, but do not prompt;
10. acquire the commit epoch, repeat only the nine-step core gate/expiry, and
    perform a fresh protected lookup;
11. treat that repeated lookup as authoritative before any current intent:
    completed same-fingerprint adopts the exact incumbent plan, freshly
    confirms it if destructive and replays; pending same-fingerprint returns
    `IDEMPOTENCY_PENDING`; different fingerprint returns
    `IDEMPOTENCY_CONFLICT`; each incumbent branch creates no current group, and
    pending/conflict never prompts even without a TTY or `--yes`;
12. only on a repeated miss, select the proposed plan and confirm it now while
    the epoch remains held when the new mutation is destructive; any
    decline/no-TTY/cancel/deadline exits with no intent;
13. perform fresh config-grant CAS after confirmation when authority source was
    config; request authority skips it;
14. compute audit append/rotation permit from exact now-known intent inputs;
15. append/fsync intent and obtain `audit_generation`;
16. run insert-if-absent: first revalidate the protected miss; an unexpected
    incumbent takes only the invariant-fatal mutation-none INTERNAL path below
    without insertion capacity, otherwise compute exact entry-count/byte/
    headroom quota using that generation, insert and fsync the winner, then
    append/fsync `idempotency_pending`; unkeyed continues without this step;
17. `saved_attach` winner/unkeyed pass 2, SHA-256, spool publication and
    `spool_ready` occur while the commit epoch remains held;
18. schedule boundary recheck, if applicable;
19. durable `dispatch_started`;
20. mutating TDLib call and complete wait/arbitration while the commit epoch is
    held;
21. durable progress/mutation checkpoints and matching store updates;
22. durable audit outcome;
23. durable store complete/remove/retain transition;
24. eligible spool cleanup and receipt-bound audit-hold release;
25. release the commit epoch immediately before exactly one terminal frame.

A deadline or failure before step 19 is `not_started`. Никакой step не
re-resolve-ит immutable plan.
Prior reconciliation precedes M4 pass 1 and may durably repair only prior
groups before a pass-1 error; a clean preflight plus pass-1 error has absolute
zero persistence. Dry-run performs no reconciliation. Pass-2 hashing and every
mutating TD dispatch are required inside the continuous commit epoch.

Write selectors — exact id, `@username` или supported t.me link. Title
substring никогда не становится target; resolver может вернуть только
`AMBIGUOUS` candidates.

Exact M3/M4 target resolution reuses M2 `ChatIdentity` materialization,
including its entity metadata reads. The chat object comes from the selected
exact resolver branch (`getChat`, `searchPublicChat`, a supported link lookup,
or the restricted Saved `createPrivateChat`). A private chat is enriched by
the matching `getUser`; a supergroup/channel is enriched by the matching
`getSupergroup`; a basic group needs no enrichment. The immutable plan
captures the returned title, type, active usernames and private-user bot bit
without re-resolution. This is an observed, receive-ordered identity, not a
claim of an atomic Telegram snapshot across separate read calls.

Nothing in this addition applies the two metadata reads to title-substring
resolution or `AMBIGUOUS.candidates`. A title-like M3/M4 target is rejected by
the existing exact-write-selector rule before either metadata read. Candidate
list construction and M2 title resolution remain outside this closure.

#### 4.5.3 Strict results, plans и errors

Success DTO:

| Operation | Exact result |
|---|---|
| `send`, `msg_edit`, `saved_attach` | `MessageWriteResult` |
| `msg_delete` | `{"chat_id":int53,"message_ids":int53[],"for_all":boolean,"deleted":true}` |
| `msg_forward` | `{"from_chat_id":int53,"to_chat_id":int53,"items":ForwardItem[]}`; every item is the sent branch |
| `msg_react` | `{"chat_id":int53,"message_id":int53,"reaction":string,"removed":boolean,"big":boolean}` |
| `msg_pin/unpin` | `{"chat_id":int53,"message_id":int53,"pinned":boolean}` |
| `chat_mark_read` | `{"chat_id":int53,"last_read_message_id":int53\|null,"marked_read":true}` |
| `chat_mute/unmute` | `{"chat_id":int53,"muted":boolean,"duration_seconds":integer}` |
| `chat_pin/unpin` | `{"chat_id":int53,"chat_list":"main"\|"archive","pinned":boolean}` |
| `chat_archive/unarchive` | `{"chat_id":int53,"archived":boolean}` |
| join success/request | `{"status":"joined","chat_id":int53}` or `{"status":"request_sent","chat_id":int53\|null}` |
| `chat_leave` | `{"chat_id":int53,"left":true}` |

Every plan has exact common fields `operation`, `account`, `tdlib_request`.
Per-operation remainder:

```text
send:
  chat:ChatIdentity, text:string,
  parse_mode:"plain"|"markdown_v2"|"html",
  reply_to:int53|null, requested_topic:ForumTopicRef|null,
  effective_topic:ForumTopicRef|null, silent:boolean,
  schedule:Schedule|null, observed_server_unix_time:int64|null
msg_edit:
  chat:ChatIdentity, message_id:int53, text:string
msg_delete:
  chat:ChatIdentity, message_ids:int53[1..100],
  requested_for_all:boolean, effective_for_all:boolean
msg_forward:
  from:ChatIdentity, to:ChatIdentity, message_ids:int53[1..100],
  drop_author:boolean
msg_react:
  chat:ChatIdentity, message_id:int53, reaction:string,
  remove:boolean, big:boolean
msg_pin/msg_unpin:
  chat:ChatIdentity, message_id:int53, pinned:boolean
chat_mark_read:
  chat:ChatIdentity, last_message_id:int53|null,
  tdlib_request:"viewMessages"|null
chat_mute/chat_unmute:
  chat:ChatIdentity, muted:boolean, duration_seconds:int32
chat_pin/chat_unpin:
  chat:ChatIdentity, chat_list:"main"|"archive", pinned:boolean
chat_archive/chat_unarchive:
  chat:ChatIdentity, archived:boolean
chat_join:
  source:"username"|"invite_link", chat:ChatIdentity|null,
  invite_link_sha256:sha256|null
chat_leave:
  chat:ChatIdentity
saved_attach:
  chat:ChatIdentity, message_id:int53,
  effective_topic:SavedTopicRef|null, caption:string, file:FileSnapshot
```

Every named field above is required; no other field is permitted. A
`ForumTopicRef` is exactly `{"kind":"forum","id":positive_int32}`; a
`SavedTopicRef` is exactly `{"kind":"saved","id":positive_int53}`.
Nullable fields are explicit JSON null. `Schedule` is null,
`{"kind":"online"}`, либо
`{"kind":"at","send_date":positive_int32}`. Raw invite отсутствует.
`observed_server_unix_time` is null exactly when `schedule` is null or
online.

`tdlib_request` exact mapping:

```text
send,saved_attach=sendMessage
msg_edit=editMessageText
msg_delete=deleteMessages
msg_forward=forwardMessages
msg_react=addMessageReaction|removeMessageReaction
msg_pin=pinChatMessage; msg_unpin=unpinChatMessage
chat_mark_read=viewMessages|null
chat_mute,chat_unmute=setChatNotificationSettings
chat_pin,chat_unpin=toggleChatIsPinned
chat_archive,chat_unarchive=addChatToList
chat_join=joinChat|joinChatByInviteLink
chat_leave=leaveChat
```
`FileSnapshot` exact:

```json
{"path":"/absolute/input.csv","name":"input.csv","size":1234,
 "sha256":"sha256:<64 lowercase hex>","device":1,"inode":2,
 "mtime_ns":3,"ctime_ns":4}
```

`path/name` are strings; `size/device/inode` are uint64;
`mtime_ns/ctime_ns` are signed int64. All eight fields are required.

Common new strict errors:

```text
BOT_UNSUPPORTED/2:
  {"operation":operation}
PRECONDITION_FAILED/1:
  {"operation":operation,"chat_id":int53|null,"message_id":int53|null,
   "reason":precondition_reason}
IDEMPOTENCY_CONFLICT/2:
  {"operation":operation,"key_hash":sha256,
   "expected_fingerprint":sha256,"actual_fingerprint":sha256}
IDEMPOTENCY_PENDING/1:
  {"operation":operation,"key_hash":sha256,"fingerprint":sha256,
   "invocation_id":hex32,"temporary_message_ids":int53[]}
IDEMPOTENCY_UNAVAILABLE/6:
  {"account":string,"path":string,"reason":durability_reason}
SEND_FAILED/1:
  {"operation":"send"|"saved_attach","chat_id":int53,
   "temporary_message_id":int53,"reason":"deleted_before_confirmation"}
FORWARD_FAILED/1 or FORWARD_PARTIAL/1:
  {"operation":"msg_forward","from_chat_id":int53,"to_chat_id":int53,
   "items":ForwardItem[]}
JOIN_APPROVAL_REQUIRED/1:
  {"operation":"chat_join","bot_user_id":int53,"query_id":int53}
JOIN_DECLINED/1:
  {"operation":"chat_join"}
INPUT_CHANGED/1:
  {"operation":"saved_attach","path":string}
SPOOL_UNAVAILABLE/1:
  {"operation":"saved_attach","path":string,"reason":durability_reason}
```

The `saved_attach` source-file `NOT_FOUND` branch is exit 4 with stable
message `input file is unavailable` and exact details
`{"operation":"saved_attach","path":string,"reason":source_file_reason}`.
`source_file_reason` is exactly
`missing|symlink|wrong_type|empty|unreadable`. Its `path`, and the `path` in
`INPUT_CHANGED` and per-invocation `SPOOL_UNAVAILABLE`, is the canonical
absolute source display path from §4.5.12. `INPUT_CHANGED` has stable message
`input file changed while being read`; `SPOOL_UNAVAILABLE` has stable message
`attachment spool is unavailable`. Per-invocation spool errors never report
an account-state or spool path. An account-global spool-root failure instead
has exact details
`{"operation":v2_gate_operation,"path":"spool/","reason":durability_reason}`;
the literal `spool/` is a redacted token, not a filesystem path.

`FilesystemDiagnosticPath` is exactly
`{"kind":"bytes_hex","value":lowercase_even_hex}`. It encodes every complete
absolute pathname as two lowercase hex digits per raw byte, including paths
whose bytes happen to be valid UTF-8. The value has even length at least 4,
begins `2f`, and contains no `00` byte pair.
The spool-contradiction `AUDIT_INCOMPLETE` branch has exact details
`{"account":string,"path":FilesystemDiagnosticPath,
"mutation_state":"none","completed_stages":[]}`. Existing audit-record
incomplete errors retain their string path branch.

`precondition_reason` exact:

```text
not_editable, not_deletable_for_self, not_deletable_for_all,
not_forwardable, not_copyable, not_pinnable, not_replyable,
wrong_content_type, wrong_chat_type, wrong_topic, chat_not_listed,
saved_notifications_unsupported, online_schedule_unsupported,
schedule_window_elapsed, schedule_too_far, reply_markup_preservation_unsupported,
reaction_unavailable
```

`durability_reason` exact:

```text
path_invalid, wrong_owner, wrong_type, wrong_mode, wrong_link_count,
too_large, capacity_exhausted, open_failed, lock_failed, read_failed,
write_failed, sync_failed, rename_failed, directory_sync_failed,
parse_error, schema_error, contradiction
```

M2 `NOT_FOUND`, `RATE_LIMITED` и `TDLIB_ERROR` shapes сохраняются, кроме
forward-specific 429:

```json
{"operation":"msg_forward","tdlib_code":429,"retry_after":12,"items":[]}
```

`items` обязателен и содержит full all-failed vector, если vector существовал,
иначе `[]`.

Entity metadata enrichment is part of the embedded exact M2 resolver and uses
`operation:"resolve"`, not the outer write operation. A `getUser` or
`getSupergroup` TD error 429 is exactly `RATE_LIMITED` / exit 5 with
`{"operation":"resolve","tdlib_code":429,"retry_after":<nonnegative integer>}`;
retry-after uses the existing ceiling parser. Any other TD error is exactly
`TDLIB_ERROR` / exit 1 with
`{"operation":"resolve","tdlib_code":<integer>}`. A null or unknown response,
invalid related entity id, returned identifier mismatch, returned object/type
mismatch, supergroup/channel-bit mismatch, or malformed returned active
username is exactly `INTERNAL` / exit 1 with
`{"operation":"resolve","reason":"internal_error"}`. These branches emit no
plan. The `msg_forward` all-failed-vector 429 exception applies only after a
forward operation owns a forward vector; pre-plan identity enrichment
therefore uses the common `operation:"resolve"` branch and never fabricates
`items`.

Existing primary M2 resolver mappings remain exact and precede this enrichment
mapping: malformed selector syntax is `USAGE` before a metadata call;
documented numeric 400/404 and username `USERNAME_NOT_OCCUPIED` or
`USERNAME_INVALID` misses use the contextual `NOT_FOUND` shape; other primary
resolver TD errors retain their existing `operation:"resolve"`
`RATE_LIMITED`/`TDLIB_ERROR` shapes. An invalid id obtained from a returned
chat type is an integrity failure (`INTERNAL`), not caller `USAGE` or
`NOT_FOUND`. No branch includes a TDLib message.

Each strict M3/M4 command error schema must admit those three exact embedded
resolver branches, with `additionalProperties:false`, alongside its
operation-owned branches. The `msg_forward` schema must keep the embedded
`operation:"resolve"` RATE_LIMITED branch distinct from its existing
`operation:"msg_forward"` branch with required `items`.

#### 4.5.4 Grammar, topics и command defaults

`send`:

```text
tgcli send <chat> <TEXT|-> [--md|--html] [--reply-to ID]
           [--topic <bare-int|forum:int>] [--silent]
           [--schedule <RFC3339|online>]
```

Stdin ≤1 MiB, valid UTF-8, без NUL. Parsed text 1–4096 Unicode scalars.
Markdown — TDLib Markdown v2; HTML — TDLib HTML; plain имеет empty entities.
External replies не поддерживаются. General M3 `--topic` принимает только
bare positive int32 или `forum:<positive-int32>`. `thread:`, `direct:` и
`saved:` — pre-dispatch `USAGE/unsupported_topic_kind`.

Reply проверяется `getMessage` + `getMessageProperties.can_be_replied`.
Без explicit topic reply может наследовать только `TopicRef(kind=forum)`.
Explicit forum обязан совпасть. Reply с thread/direct/saved topic в general
send отклоняется `PRECONDITION_FAILED/wrong_topic`. Это правило отражается:
fingerprint содержит caller `reply_to` и requested forum/null; plan/audit
дополнительно содержит effective inherited forum/null.

M4 `saved_attach` — единственное исключение: original обязан находиться в
current user's Saved chat; inherited topic допускается только
`TopicRef(kind=saved)` либо null. Любой forum/thread/direct в этом adapter —
`PRECONDITION_FAILED/wrong_topic`. Caller topic flag отсутствует. Fingerprint
содержит original message id и marker `"topic":"inherit_saved"`; derived
saved topic не является caller-controlled и фиксируется в plan/audit.

Omitted `--caption` normalizes to `""`; normalized args always contain the
string field. Caption is plain valid Unicode-scalar UTF-8, contains no NUL,
and is at most 4096 UTF-8 bytes and 1024 Unicode scalars. Empty is valid,
normalization is absent, and TDLib receives `formattedText(caption,[])`.
Formatting flags or caller entities are unsupported.

`messageSendOptions` имеет все поля false/0/null кроме silent, schedule и
random nonzero int32 `sending_id`; `only_preview=false`, `clear_draft=false`,
reply markup/link preview null.

`msg edit`: plain text rules send; formatting flags unsupported. Требуются
text content и `can_be_edited`. Non-null bot reply markup отклоняется, чтобы
его не потерять.

`msg delete`: 1–100 unique ascending ids. Private/basic uses
`revoke=--for-all`. Supergroup/channel/secret фактически revoke; без explicit
`--for-all` fail до confirmation. Все properties проверяются.

`msg forward`: 1–100 unique ids уже strictly increasing; input order не
сортируется. `--drop-author` ставит `send_copy=true,remove_caption=false` и
требует copy capability. Один `forwardMessages` call.

`msg react`: exact nonempty valid UTF-8 ≤64 bytes, без normalization; только
`reactionTypeEmoji`. Add uses `update_recent_reactions=true`; remove uses
`removeMessageReaction`; `--remove --big` mutually exclusive.

Message pin: `pinChatMessage(disable_notification=false,only_for_self=false)`
либо `unpinChatMessage`; pin требует `can_be_pinned`.

Mark read: current `chat.last_message.id`; `viewMessages([id],null,true)`.
Пустой chat — audited no-op, без mutating call.

Mute: no duration = `INT32_MAX`; grammar
`^[1-9][0-9]*(s|m|h|d|w)$`, итог 1–31,622,400. Unmute duration запрещён.
Полный observed settings object копируется; меняются только
`use_default_mute_for=false,mute_for=<n|0>`. Saved Messages mutation запрещена.

Chat list pin выбирает Archive при membership, иначе Main; ни одного —
`chat_not_listed`. Archive/unarchive uses `addChatToList`.

Join принимает exact `@username` или invite. Invite plan/audit/diagnostics
содержат только domain-separated SHA-256. Guard URL никогда не открывается и
не выводится. Success/RequestSent — success; Guard — approval error; Declined
— declined error. Leave разрешён только group/supergroup/channel и destructive.

#### 4.5.5 Exact schedule contract

RFC3339 принимает timezone-required instant и finite fractional seconds.
Conversion в Unix seconds — mathematical ceiling. Результат обязан быть
1…2147483647. `server_now` берётся только из
`getOption("unix_time")`/`optionValueInteger`.

При planning:

```text
send_date > server_now + 10
send_date - server_now <= 367 * 86400
```

Равенство `+10` отклоняется `schedule_window_elapsed`; равенство `+367d`
разрешено. Непомещающийся int32 — `USAGE/invalid_argument`.

После durable intent/pending, непосредственно перед `dispatch_started`, tgcli
повторяет `getOption("unix_time")`. Если первая inequality уже false —
durable no-mutation outcome, pending удаляется, `schedule_window_elapsed`.
Если second false — `schedule_too_far`. Никакой `sendMessage` не вызывается.

Между последним read и внутренним TDLib
`send_date <= G()->unix_time()+10` остаётся неустранимая race. Tgcli не
заявляет atomic schedule admission. Если TDLib coerces request to immediate,
фактический returned/succeeded Message имеет null scheduling state и success
возвращает `scheduled:false` с обычной date. Это confirmed mutation, store
completed. Если scheduling state non-null, `scheduled:true,date:null`.
Acceptance покрывает `+10`, `+11`, `+367d`, `+367d+1`, second-boundary crossing
и authoritative immediate result.

`online` user-only, private regular-user, not self, с exact online/offline
visibility; иначе `online_schedule_unsupported`. Bot с `online` или date
получает `BOT_UNSUPPORTED`.

#### 4.5.6 Audit schema version 2

M1 version-1 records и schemas не меняются. Один `audit.log` принимает strict
top-level oneOf:

```text
existing exact schema_version=1 M1 intent/checkpoint/outcome
new exact schema_version=2 M3 intent/checkpoint/outcome
```

The mixed audit schemas use §5's two-layer boundary only for
`schema_version:2`: ordinary self-contained Draft 2020-12 validation is
necessary, and the exact filename-owned runtime rules in the top-level
documentation-only marker are additionally necessary. Runtime acceptance
never makes a schema-invalid record valid. V1 branches are unchanged and the
marker is explicitly inapplicable to them. Every standard-expressible object,
scalar, finite relation, terminal class, stage-prefix, calendar, basename and
lexical-path restriction remains an ordinary schema assertion. Field/nested
`x-tgcli-*` pseudo-assertions are forbidden.

Before positive v2 recognition, unknown/absent/duplicate/non-integer schema
version follows the exact `path_invalid`/`parse_error` precedence below. After
positive recognition, unknown phase/field and a later bounded unsupported
version are contradictions. Group key — `(schema_version,invocation_id)`;
reuse одного invocation id в двух versions или commands — contradiction.

v2 common scalar rules: `hex32`/`invocation_id` = 32 lowercase hex;
`uint32` = 0…4294967295, positive uint32 excludes zero; `uint64` =
0…18446744073709551615; `int64` is signed 64-bit; `unix_seconds` =
integer 0…253402300799. Audit timestamp = UTC RFC3339 seconds
`YYYY-MM-DDTHH:MM:SSZ`, year 1970…9999. Account/operation/tdlib_function
use their closed enums; hashes are `sha256:<64 lowercase hex>`.

The ordinary schemas assert real Gregorian 1970…9999 calendar/leap validity,
finite legal stage-prefix and outcome mutation/success/proof branches,
scheduled/date branches, exact forward terminal classes, basename C0/C1
restrictions and lexical canonical `FileSnapshot.path`. Runtime-only checks are
limited to aggregate serialized bytes, contextual normalization, same-record
and cross-record equality/derivation, projected uniqueness, strict numeric
ordering and UTF-8 byte ceilings. `audit.arguments.path` preserves caller
spelling under §4.5.12 and is not incorrectly constrained to the canonical
snapshot path.

Each schema carries this exact top-level documentation-only marker adjacent to
`$schema`:

```json
"$comment": "For schema_version 2, full tgcli contract validation also requires the documentation-only x-tgcli-semanticValidation rules; an ordinary Draft 2020-12 validator ignores that annotation.",
"x-tgcli-semanticValidation": {
  "annotationOnly": true,
  "ordinaryDraft202012ValidationIsInsufficient": true,
  "schemaVersion": 2,
  "validator": "tgcli-runtime-v1",
  "rules": ["<exact sorted filename-owned rules>"]
}
```

The exact sorted filename-to-rules matrix is:

```json
{
  "audit-checkpoint.schema.json": [
    "aggregate_serialized_bytes",
    "cross_record_equality_and_derivation",
    "projected_uniqueness",
    "same_record_equality_and_derivation",
    "strict_numeric_order",
    "utf8_byte_limits"
  ],
  "audit-intent.schema.json": [
    "aggregate_serialized_bytes",
    "contextual_normalization",
    "same_record_equality_and_derivation",
    "strict_numeric_order",
    "utf8_byte_limits"
  ],
  "audit-outcome.schema.json": [
    "aggregate_serialized_bytes",
    "cross_record_equality_and_derivation",
    "projected_uniqueness",
    "same_record_equality_and_derivation",
    "strict_numeric_order",
    "utf8_byte_limits"
  ]
}
```

The generator enforces this exact matrix and `schemaVersion:2`. No unknown
keyword is treated as an assertion. The runtime taxonomy covers
`arguments↔plan`; group version/account/command/invocation identity;
idempotency hash/fingerprint; spool file/relative path; terminal↔plan/latest
vector; outcome stages/mutation/success/proof/terminal↔durable history; and
terminal/record/group/segment aggregate byte ceilings.

Intent exact keys:

```text
schema_version=2, phase="intent", invocation_id, timestamp, account,
command, arguments, plan, request_fingerprint, config_snapshot,
authority_source, confirmation_source, idempotency_key_hash
```

`authority_source` = `request|config`. `request` means the grant came
from v3's already folded `RequestContext.write_authority`; tgcli does not
claim whether the client folded a CLI flag or environment value. Adding that
distinction would require a separately reviewed protocol field and is not M3.
`confirmation_source` = null for Write, `yes|tty` for new Destructive.
`request_fingerprint` and non-null `idempotency_key_hash` are sha256;
`idempotency_key_hash` is sha256|null. `config_snapshot` is the non-null
frozen config-file identity:

```text
sha256:<64 lowercase hex>;dev:<device>;ino:<inode>;size:<size>;ctime_ns:<ctime>
```

The `device,inode,size,ctime` fields are all minimal unsigned decimal uint64.
There is no plus or leading zero except literal `0`. Hash covers the complete
file bytes; stat
identity is captured from the same opened non-symlink current-uid config FD
before/after reading and must be unchanged. A Ready M3 account without this
actual frozen config snapshot fails config admission before intent. The fresh
config-grant recheck compares the complete identity string, not only hash.
Raw key/invite отсутствуют.

`arguments` strict oneOf keyed by command:

```text
send:
  chat:string, text:string, parse_mode:"plain"|"markdown_v2"|"html",
  reply_to:int53|null, topic:ForumTopicRef|null, silent:boolean,
  schedule:Schedule|null
msg_edit:
  chat:string, message_id:int53, text:string
msg_delete:
  chat:string, message_ids:int53[1..100], for_all:boolean
msg_forward:
  from:string, to:string, message_ids:int53[1..100], drop_author:boolean
msg_react:
  chat:string, message_id:int53, reaction:string,
  remove:boolean, big:boolean
msg_pin/msg_unpin:
  chat:string, message_id:int53
chat_mark_read:
  chat:string
chat_mute/chat_unmute:
  chat:string, duration_seconds:int32
chat_pin/chat_unpin:
  chat:string
chat_archive/chat_unarchive:
  chat:string
chat_join username:
  source:"username", username:string
chat_join invite:
  source:"invite_link", invite_link_sha256:sha256
chat_leave:
  chat:string
saved_attach:
  message_id:int53, path:string, caption:string
```

Каждая branch содержит ровно перечисленные required fields. `chat/from/to` здесь —
validated original selector string; invite raw исключён. `plan` — strict oneOf
из §4.5.3.

Checkpoint exact keys:

```text
schema_version=2, phase="checkpoint", invocation_id, timestamp, account,
command, checkpoint_sequence, stage, data
```

`checkpoint_sequence` positive uint32, строго растёт. Stage/data oneOf:

```text
idempotency_pending:
  key_hash:sha256, request_fingerprint:sha256,
  expires_at:unix_seconds, reserved_terminal_bytes:uint32
spool_ready:
  file:FileSnapshot, relative_path:string
dispatch_started:
  tdlib_function:tdlib_function, dispatch_token:hex32,
  client_generation:uint64
temporary_ids_observed:
  temporary_message_ids:int53[1..100]
forward_progress:
  items:ForwardItem[1..100]
mutation_confirmed:
  terminal:StoredTerminal
```

`dispatch_token` = 32 lowercase hex; это tgcli correlation, не TD query id.
`relative_path` имеет exact form
`spool/<invocation_id>/<safe-basename>` и не absolute.

Legal first-occurrence stage orders:

```text
direct:       [idempotency_pending?], dispatch_started, mutation_confirmed?
single-send:  [idempotency_pending?], dispatch_started,
              temporary_ids_observed?, mutation_confirmed?
saved-attach: [idempotency_pending?], spool_ready, dispatch_started,
              temporary_ids_observed?, mutation_confirmed?
forward:      [idempotency_pending?], dispatch_started,
              temporary_ids_observed?, forward_progress*, mutation_confirmed?
no-op:        [idempotency_pending?]
```

Only `forward_progress` may repeat; каждый record — full input-order vector и
может менять item только `pending→sent|failed`. Temporary ids никогда не
меняются и не переиспользуются. `mutation_confirmed.terminal` — exact recovery
payload; partial-forward error допустим. Stage после `mutation_confirmed`
запрещён.

Outcome exact keys:

```text
schema_version=2, phase="outcome", invocation_id, timestamp, account,
command, success, mutation_state, completed_stages, terminal
```

`terminal` — `StoredTerminal`; `success` true iff kind=result.
`success` is boolean; `mutation_state` is
`none|possible|confirmed`; `completed_stages` — ordered unique
first-occurrence stage array, exact legal prefix.
M3 mutation state:

- `confirmed`: durable success proof или хотя бы один sent forward item;
- `possible`: dispatch durable, но success/failure proof отсутствует;
- `none`: dispatch отсутствует либо every dispatched item имеет explicit
  TDLib failure/upstream-null proof and no deletion ambiguity.

`StoredTerminal` oneOf, без transport id:

```json
{"kind":"result","data":{}}
```

```json
{"kind":"error","code":"TIMEOUT","message":"stable contract text",
 "details":{},"exit_code":7}
```

Intent и every checkpoint fsync before следующего mutation-relevant step.
Outcome fsync precedes store transition и terminal frame. После intent failure
append закрывает connection без terminal и включает audit-fatal shutdown.

Recovery:

- intent без dispatch → mutation none with the exact spool/outcome/store order
  below;
- dispatch без complete proof → outcome `AUDIT_INCOMPLETE`, mutation possible
  without a sent item or confirmed with any durable sent item; key остаётся
  pending;
- `mutation_confirmed` без outcome → reconstruct exact outcome из terminal;
- full terminal forward_progress без outcome → derive exact success/failure,
  при success item сначала append mutation_confirmed;
- outcome + stale pending store → repair store до terminal/removal/pending;
- corrupt sequence, identity mismatch, stage regression, changed immutable
  field, two different terminal payloads, store/audit fingerprint mismatch,
  missing pinned generation или impossible success/state combination —
  contradiction, `AUDIT_INCOMPLETE`, no resend.

Recovery-created v2 failure outcomes use one exact StoredTerminal:
`kind:"error"`, `code:"AUDIT_INCOMPLETE"`, message
`"a prior audited invocation did not reach a terminal proof"`, exit code 1,
and the standard exact details containing the group account, canonical
per-account `audit.log` path, mutation state, and ordered unique completed
stages. Intent without `dispatch_started` records `none`; after its durable
outcome and required store/spool cleanup, the inspecting request may continue
and does not emit the prior terminal. `dispatch_started` without complete
proof records `possible` when no durable forward item is sent, but records
`confirmed` when any durable forward_progress contains a sent item, even if
the vector remains incomplete. After syncing either outcome the inspecting
request emits the same AUDIT_INCOMPLETE and stops. Neither branch resends.
Durable proof reconstruction retains the existing rules. Contradiction
appends nothing and stops with the last trustworthy prefix; an empty prefix
uses the routed account, canonical audit path, mutation none, and `[]` stages.

Intent-without-dispatch recovery has only this order: when an incomplete or
`spool_ready` spool exists, delete the invocation spool idempotently and fsync
the spool root; append and fsync the none/AUDIT_INCOMPLETE outcome; then
remove/transition and fsync the matching pending store reference. Without a
spool it begins at outcome. A crash after unlink but before spool-root fsync
retries missing-is-success deletion and root fsync; after root fsync it starts
at outcome; after outcome fsync it skips outcome and repairs only store; after
store fsync it continues. Spool failure before outcome returns current-request
SPOOL_UNAVAILABLE/1, outcome failure returns current-request
AUDIT_INCOMPLETE/1, and post-outcome store failure returns current-request
IDEMPOTENCY_UNAVAILABLE/6. No case creates a current group or emits the prior
stored terminal on successful repair.

Durable-proof recovery appends/fsyncs a missing mutation proof when required,
appends/fsyncs the exact outcome only when absent, completes/removes and
fsyncs the store transition, then performs eligible idempotent spool deletion
and spool-root fsync. Outcome plus stale store skips proof/outcome; outcome
plus terminal store skips proof/outcome/store. Cleanup failure after durable
outcome/store returns current-request SPOOL_UNAVAILABLE/1, preserves those
bytes, and restart retries cleanup only. A crash at every boundary has the
same skip rule. Failure to append/fsync a missing proof or outcome returns
current-request AUDIT_INCOMPLETE/1 from the resulting durable prefix; retry
reconstructs from that prefix. Store-transition failure returns current-
request IDEMPOTENCY_UNAVAILABLE/6 and retry skips the durable outcome. These
are preflight terminals before current intent, not the
post-intent no-terminal audit-fatal path. Dispatch-ambiguous recovery retains
store and spool.

Per-account audit mutation occurs only while the existing verified
`<account-state>/daemon.lock` lifetime lease is held. Exactly one deadline-aware
outer per-account operation mutex is shared by audit, idempotency and spool
coordination; helpers take no inner audit or store mutex and there is no second
audit/store lock file. A real M3/M4 request uses §4.5.2's exact two epochs. The
initial epoch performs the complete gate, then caller hashing and lookup; M4
pass 1 is after prior reconciliation but before lookup in this epoch. A hit
remains held through selection and any destructive stored-plan confirmation. A
miss releases the epoch for resolver/property planning. The commit epoch
repeats only the core gate and authoritative lookup. Completed replay adopts
and, when destructive, freshly confirms the exact stored plan; pending and
conflict return directly. Every incumbent branch creates no current group.
Only a repeated miss confirms the proposed plan when required, then performs
config CAS, append-permit calculation, intent, generation-bound quota and
insert-if-absent. An unexpected insert loss is invariant-fatal, not an
incumbent terminal branch. No unconfirmed intent is durable. A winner holds the
epoch through pass 2,
checkpoints/store transitions, mutating TDLib dispatch/wait, outcome and spool
cleanup, and releases before terminal. Thus two callers may observe an initial
miss while audit groups remain contiguous. This
contract requires M4 pass-2 hashing and mutating TD dispatch inside the commit
epoch and makes no blanket assertion that network/file hashing is outside the
mutex.
A group is contiguous and never spans segments. A nonempty active file rotates
exactly once before intent only when
`active_size + intent_line_size > 33,554,432`; equality does not rotate, and a
missing/empty active file is used directly. The inode of the synced intent
segment, losslessly represented as
uint64, is its opaque `audit_generation`. Same-directory rename preserves it.
Store pins validate `(audit_generation,invocation_id,request_fingerprint,
operation)`, not the integer alone. Rotation first chooses the smallest
missing numbered slot and deletes nothing. Only when every numbered slot is
occupied does it unlink the largest-suffix proven-unpinned slot. It then
exclusively renames newer slots toward the selected hole and active to `.1`.
Pinned inodes may move but are never deleted or overwritten. No unpinned slot means
`AUDIT_UNAVAILABLE/capacity_exhausted` before mutation. A dangling or
mismatched pin is contradiction. Complete v1 groups retain v1 interpretation
and retention.

Across all 16 four-slot occupancy masks, a non-full starting mask has
`q=1..4` missing numbered slots. Choosing
the smallest missing suffix performs no unlink. Every exclusive numbered
rename fills one hole and opens one, so it preserves `q`; active-to-`.1`
reduces the completed mask to `q-1` holes. Other holes remain. For the full
mask, the one selected unlink creates one transient hole, numbered renames
preserve one, and active-to-`.1` returns to zero. A completed rotation never
increases the starting hole count; there is no at-most-one-hole invariant.
A crash before active-to-`.1` leaves active present and the same non-full `q`,
or the full-start transient one; restart validates the observed mask, uses its
smallest hole, and performs no second unlink. A crash after active-to-`.1`
but before fresh intent append leaves active absent. No
invocation is durable or pending for that vanished request. Restart, list,
and dry-run validate/inspect but leave active absent and cannot recover or
resume it. Only the next separately authorized real invocation creates active
and appends its own fresh intent without rotation or numbered-slot deletion.

These are newly reviewed behavioral limits, not non-narrowing claims and not
Telegram/TDLib producer guarantees. The shared compact serialized-frame budget
excluding LF is `P=16,842,751` bytes, derived from the current 16,777,216-byte
pre-read threshold plus the 65,536-byte chunk minus one. It is the maximum
whole Request, Result, Error, Item, Progress, Challenge or Answer frame in both
daemon and no-daemon mode; LF is appended only after the complete frame passes
the bound. Every otherwise-unbounded caller UTF-8 string is at most `P` decoded
bytes and all caller fields together fit one compact Request of at most `P`.
Existing tighter command limits still apply. Above `P`, a Request has no
command terminal and an outbound frame has no partial observable bytes: the
socket path closes the connection and the in-process path performs the same
transport-failure delivery before exposing the oversized value.

For compact `Result`, the exact whole-frame equation is
`bytes(data.dump()) + 31 + decimal_digits(request_id) <= P`; `31` is every
fixed byte of `{"type":"result","id":<id>,"data":<data>}`. Thus the exact
data ceiling is `P - 31 - decimal_digits(request_id)`: 16,842,719 bytes for
request id 0 and 16,842,700 bytes for the largest uint64 id. Producers that
cannot retain the request id use the conservative 16,842,700-byte ceiling.
Schema validity never overrides this serialization admission.

Before confirmation, intent, or mutation, TD conversion is all-or-nothing
under these exact limits: ChatIdentity title at most 1,048,576 valid UTF-8
bytes; at most 100 usernames, each 1..32 ASCII bytes; at most 4,096 sessions;
each session `application_name`, `application_version`, `device_model`,
`platform`, `system_version`, `ip_address`, and `location` at most 1,048,576
valid UTF-8 bytes; complete compact SessionListResult data at most 16,842,700
bytes; MessageSummary text/caption at most 4,096 Unicode scalars and 16,384
UTF-8 bytes; at most 100 forward items; canonical ForwardItem vector at most
4,194,304 bytes. SessionId remains the full canonical signed-int64 ASCII
decimal domain, including INT64_MIN, zero, and INT64_MAX, and occupies at most
20 bytes; it is not uint64. Terminal payload ceilings are 4,194,304 bytes for
`msg_forward`, 65,536 for `send`, `msg_edit`, and `saved_attach`, and 32,768
for every other M3 operation and `session_terminate`.

M3 pre-dispatch TD-limit failure returns the current Error terminal
INTERNAL/1 with exact
message `"TDLib returned data outside the supported persistence bounds"` and
exact details `{"operation":<the requested operation>,
"reason":"internal_error"}`; it creates no intent. Session conversion failure
retains INTERNAL/1 with exact accepted details
`{"operation":"session_list"|"session_terminate",
"reason":"malformed_tdlib_response","tdlib_type_id":integer|null}` and no
partial result/intent. A correlated oversized post-dispatch success can occur
only for `send`, `msg_edit`, `saved_attach`, or `msg_forward`; append/fsync a
mutation_confirmed StoredTerminal INTERNAL/1 with the same exact message and
details `{"operation":"send"|"msg_edit"|"saved_attach"|"msg_forward",
"reason":"internal_error"}`, then append/fsync a failure outcome with mutation
confirmed and perform the required durable store transition. Only then emit
that same INTERNAL/1 Error as the current terminal. No raw oversized value
reaches audit, store, diagnostics, or the terminal frame. Failure to persist
the checkpoint, outcome, or required store transition follows the existing
no-terminal audit-fatal rule.
Here the pre-dispatch operation is exactly one of `send`, `msg_edit`,
`msg_delete`, `msg_forward`, `msg_react`, `msg_pin`, `msg_unpin`,
`chat_mark_read`, `chat_mute`, `chat_unmute`, `chat_pin`, `chat_unpin`,
`chat_archive`, `chat_unarchive`, `chat_join`, `chat_leave`, or
`saved_attach`; no generic/unknown operation string is admitted.

Caller JSON control escapes expand canonically by at most 3x and caller
material appears at most twice, giving `6P`. One ChatIdentity is bounded by
`6*1,048,576 + 100*32 + 200 + 99 + 2 + 512 = 6,295,469` bytes. An enforced
`P` budget covers all other M3 keys, delimiters, hashes, FileSnapshot scalars,
integers, enums, and envelope bytes. Therefore the worst intent is
`7P + 2*6,295,469 = 130,490,195 < 134,217,728` JSON bytes. Session intent has
five persisted descriptive strings and is bounded by
`5*6*1,048,576 + P = 48,300,031` bytes. These admission checks make the
arithmetic true; they are not inferred producer bounds.

Exact record ceilings excluding LF are: intent 134,217,728; non-vector
checkpoint 65,536; terminal/vector payload 4,194,304; fixed canonical
proof/outcome envelope 4,096; vector/proof/outcome record 4,198,400. Including
LF they are 134,217,729, 65,537, and 4,198,401. Forward progress first persists
the immediate full vector, then persists every terminal transition before a
later TD event. A 100-item all-pending vector can therefore advance 100 times
and uses at most 101 progress records. Thus the exact maximal group is
`134,217,729 + 3*65,537 + 103*4,198,401 = 566,849,643` bytes. Its tail is
`432,631,914`; a nonrotating segment is at most
`33,554,432 + 432,631,914 = 466,186,346`; a fresh maximal group is
566,849,643. The exact segment ceiling is therefore 566,849,643 and five
segments are 2,834,248,215 bytes. Oversized prospective intent is
AUDIT_UNAVAILABLE/too_large. Only unreachable internal factory overflow after
intent is audit-fatal schema_error.

This monotone v2 widening does not change the schema or store version. A binary
that enforces the former 100-progress-record ceiling cannot read a group using
the legal 101st record, so downgrading to such a binary is unsupported.

The limits are intentionally high relative to ordinary Telegram DTOs but
finite. They preserve valid UTF-8 and existing public DTO/error shapes, use
the transport's actual request ceiling, and convert an extreme producer value
to a stable bounded terminal rather than allocation-dependent failure. This is
the safety/interoperability justification; producer compliance is not assumed.

A stored segment above 566,849,643 or a first line above the intent ceiling is
preclassification path_invalid for every version. A bounded complete JSON
object is positively v2-recognized as soon as it has exactly one top-level
integer `schema_version:2`, before phase, extra-field, intent, or group
validation. Checkpoint-before-intent, unknown phase, extra field, and invalid
v2 intent therefore return empty-prefix AUDIT_INCOMPLETE even above 64 MiB.
Before positive v2 recognition, unknown/absent/duplicate/non-integer version,
non-object JSON, and an overall-ceiling line without LF remain unrecognized
path_invalid; invalid JSON is
parse_error at/below 64 MiB, while legacy path_invalid wins above 64 MiB until
positive v2 recognition. After recognition, a later oversized v2 record is
too_large or the open prefix's AUDIT_INCOMPLETE. After recognition, any later
bounded complete object with unsupported integer schema_version is first a
contradiction and returns AUDIT_INCOMPLETE from the last trustworthy open v2
prefix (or the exact empty prefix when none is open), never path_invalid;
version classification precedes phase-size and sequence validation. A v1-only
segment above
64 MiB retains its existing path_invalid behavior. Inspection streams
65,536-byte chunks and never loads a segment as one string.

Invocation reuse detection retains no unbounded set. At every intent it
deterministically rescans all prior segments/bytes and rejects the same
invocation id in any version or command. The rescan uses the same bounds and
absolute deadline.

The dormant M3/M6 `AUDIT_UNAVAILABLE` reason enum is exactly:

```text
path_invalid, wrong_owner, wrong_type, wrong_mode, wrong_link_count,
too_large, capacity_exhausted, open_failed, lock_failed, read_failed,
write_failed, sync_failed, rename_failed, directory_sync_failed,
parse_error, schema_error, contradiction
```

`rotate_failed` is not a v2 durability reason and is rejected by the M3/M6
schemas and generator checks. Existing v1 logout/removal `audit_reason` and its
adapter retain `rotate_failed`; the generator defines the v1 and v2 enums
separately. V2 unlink/rename failures map to `rename_failed`, and directory
fsync failures map to `directory_sync_failed`.

Account-audit history contradiction always maps to `AUDIT_INCOMPLETE`, including
an empty trustworthy prefix; it does not use the `AUDIT_UNAVAILABLE` reason
branch. The full durability enum remains shared with store/spool failures that
can report `contradiction` without an audit-group history.

#### 4.5.7 Idempotency store и exact transitions

`idempotency.db` — strict JSON snapshot:

```json
{"schema_version":1,"entries":[]}
```

The exact `IDEMPOTENCY_UNAVAILABLE.details.path` is the frozen absolute,
lexically canonical per-account state directory plus `/idempotency.db`. It is
never a relative token, temp name, raw environment spelling or resolved symlink
target. The authoritative file is current-uid, regular non-symlink, exact
`0600`, link count 1, at most 16 MiB and at most 10,000 entries. It contains
exactly one canonical JSON serialization with no BOM, leading/trailing
whitespace or trailing LF. Duplicate-safe parsing, the strict schema and
relations, and byte-identical canonical reserialization are all necessary.
Missing is the empty snapshot; an empty or noncanonical present file is an
error. Entries are sorted by `key_hash`.

The only rewrite temp basename is `.idempotency.db.tmp`. The verified lifetime
lease and outer mutex make a PID/random suffix unnecessary. A present temp is
never parsed, replayed, promoted or merged. After the spool/audit/store
contradiction gate, a current-uid regular non-symlink exact-`0600`, link-count-1
temp is unlinked and the account-state directory fsynced; unsafe metadata fails
closed and is retained. Rewrite uses same-directory `O_CREAT|O_EXCL|O_NOFOLLOW`
mode `0600`, exact canonical bytes, temp-file fsync, descriptor/name identity
revalidation, atomic rename over the already-validated canonical entry,
final-name/descriptor revalidation and directory fsync. The old canonical name
is never separately unlinked. Unlink/rename failure is `rename_failed`, file
fsync failure is `sync_failed`, and directory fsync failure is
`directory_sync_failed`.

If rename succeeds but directory fsync fails, tgcli does not guess which
snapshot is crash-durable. The current operation follows its existing
pre-intent or post-intent failure rule. At restart the canonical final name is
the sole store authority, any temp is discarded, and audit reconciliation
repairs whichever complete canonical snapshot survived. A temp alone never
proves a transition.

Every store failure uses exit 6, stable message
`idempotency store is unavailable`, exact routed account, and the same canonical
absolute final store path even when the temp failed. Exact reason mapping:

| Failure | `durability_reason` |
|---|---|
| invalid/nonabsolute/noncanonical frozen path, symlink, named-entry/descriptor or inode mismatch/replacement | `path_invalid` |
| non-directory account state or non-regular final/temp | `wrong_type` |
| wrong uid | `wrong_owner` |
| account-state not exact `0700` or final/temp not exact `0600` | `wrong_mode` |
| final/temp link count not one | `wrong_link_count` |
| final exceeds 16 MiB | `too_large` |
| invalid/missing/replaced lease or lock | `lock_failed` |
| lstat/fstatat/open/initial-fstat failure before a stable descriptor | `open_failed` |
| read failure, premature EOF, or descriptor size/identity change during read | `read_failed` |
| invalid JSON/UTF-8, duplicate key, empty present file, trailing junk | `parse_error` |
| noncanonical bytes; standalone schema/version/field/type/range/order/uniqueness/state/expiry/quota/plan/terminal invariant; unrepresentable clock/factory | `schema_error` |
| otherwise-valid store disagrees with audit/spool, pin, progress, terminal, permit or release receipt | `contradiction` |
| post-expiry entry-count/byte/headroom admission failure | `capacity_exhausted` |
| short/failed write | `write_failed` |
| temp file fsync | `sync_failed` |
| stale-temp unlink or final rename | `rename_failed` |
| directory fsync after unlink/rename | `directory_sync_failed` |

Precedence is exact: deadline/cancellation while waiting to acquire the outer
epoch first, through its own terminal and with no gate observation; only after
acquisition, account-global spool failure/contradiction; then lease/frozen path/account-state;
authoritative final metadata/open/read/size then parse/duplicates then schema/
canonical validation; temp metadata; one-pass audit relation contradiction;
safe-temp unlink/fsync; prior recovery; expiry cleanup. Initial lookup performs
no append-permit or store-capacity decision. Commit repeats authoritative
lookup; an incumbent terminal returns before capacity and creates no group.
Only a miss confirms the proposed plan when required, performs config CAS and
computes exact append/rotation permit before intent. After intent/generation,
the insert primitive revalidates absence: unexpected incumbent takes its fatal
path without capacity, otherwise exact prospective-winner capacity precedes
insertion. Current rewrite failures retain their actual boundary. Final failure
wins over simultaneous temp failure. Parse
failure/duplicate keys win before schema/canonical comparison. Standalone
invalidity is `schema_error`; disagreement between otherwise valid durable
components is `contradiction`. Within metadata, symlink/name mismatch is
`path_invalid`, then wrong type, owner, mode, link count and size; a syscall
preventing classification uses `open_failed`/`read_failed`. No later failure
replaces an earlier selected one and no internal/temp/raw-key detail is public.

Entry exact:

```text
key_hash:sha256, request_fingerprint:sha256, operation:operation,
state:"pending"|"completed", invocation_id:hex32,
audit_generation:uint64, created_at:unix_seconds, expires_at:unix_seconds,
reserved_terminal_bytes:uint32, plan:Plan,
temporary_message_ids:int53[], forward_progress:ForwardItem[],
spool:SpoolRef|null, terminal:StoredTerminal|null
```

`SpoolRef` имеет ровно
`{"relative_path":string,"file":FileSnapshot}`. Pending terminal null;
completed terminal `StoredTerminal`. A transition to completed atomically sets
`reserved_terminal_bytes=0`, clears `temporary_message_ids` and
`forward_progress` to empty arrays, and retains `spool` unchanged until cleanup.
A pending/unknown entry retains its exact observed temporary ids and latest full
forward vector. Mutation-none removes the entry.
`created_at/expires_at` are integer Unix seconds 0…253402300799;
`expires_at=created_at+604800` exactly. `audit_generation` pins a real segment.
No transport request id/raw key/raw invite.

`Entry.spool` changes from null only after matching durable audit
`spool_ready`; it is durable before `dispatch_started`. The update obeys the
exact 16 MiB actual-snapshot and remaining-headroom inequalities below. No file
bytes or separate spool quota are reserved. Spool growth is outside the mutable
payload reservation and must preserve the global inequality before dispatch. A
completed entry retains the non-null reference through filesystem cleanup and
changes it to null only after cleanup plus spool-root fsync.

Before pending insert quota reserves maximum terminal bytes:

```text
msg_forward: 4,194,304
send, msg_edit, saved_attach: 65,536
all other operations: 32,768
```

The reservation is one reusable mutable-payload budget. For an entry `E`, let
`mutable(E)` be the exact canonical-byte growth of
`{"temporary_message_ids":E.temporary_message_ids,
"forward_progress":E.forward_progress,"terminal":E.terminal}` relative to the
same object with `[],[],null`. Pending entries require
`0 <= mutable(E) <= E.reserved_terminal_bytes`; define
`remaining(E)=E.reserved_terminal_bytes-mutable(E)`. Every snapshot and
prospective rewrite requires:

```text
canonical_snapshot_bytes
  + sum(remaining(E) for every pending E) <= 16 MiB
```

Insertion also requires fewer than 10,000 existing entries and includes the
exact prospective entry, array delimiters and comma growth. At insertion the
new mutable charge is zero and its whole reservation contributes to remaining
headroom. Temporary-id and latest-forward-vector rewrites consume that
headroom; completion clears both arrays and consumes the same budget with the
terminal. Every legal post-dispatch update therefore preserves the inequality
and cannot discover an unreserved quota shortage. `forward_progress` stores
only the latest full vector, never checkpoint history. The existing
terminal/vector byte ceilings are measured by these canonical growth
expressions, including member/delimiter bytes.

Strict curated terminal больше reservation — `schema_error`, durability-fatal;
dispatch до такой невозможности не допускается.

The durable order is exact:

- initial winner: store insert and directory fsync, then
  `idempotency_pending` checkpoint and audit fsync;
- `spool_ready`, `temporary_ids_observed`, and every `forward_progress`:
  checkpoint and audit fsync, then matching pending-entry rewrite and directory
  fsync before any later TD event/checkpoint/outcome;
- terminal proof when required, then audit outcome and fsync, then the exact
  completed/remove/retain store rewrite and directory fsync, then terminal.

Initial insertion is the sole store-before-checkpoint exception. A matching own
pending entry without `idempotency_pending` is its legal crash window and is
closed none then removed. Another invocation under the key after this intent is
an unexpected incumbent: intent-without-dispatch recovery closes the crashed
group none with exact `AUDIT_INCOMPLETE`, never removes that incumbent, and
validates the incumbent's own audit relation independently. A pending
checkpoint without its matching store entry is contradiction. After a durable
temporary/progress checkpoint, store lag may be repaired only forward from the
audit prefix; store-ahead or conflicting values are contradiction. A post-
dispatch required store-update failure is no-terminal durability-fatal.

Lookup и later insertion оба под lock. Miss не является reservation. После
durable intent `insert-if-absent`:

- the protected repeated miss is revalidated inside the insert primitive;
- if still absent, exact generation-bound quota is evaluated and the caller
  atomically becomes winner;
- an unexpected incumbent is an invariant failure, not replay, pending or
  conflict, and takes only the exact fatal closure below;
- inability to close that outcome means no terminal and audit-fatal.

At the repeated commit gate, the lookup is authoritative before any current
intent. Completed same-fingerprint adopts the exact incumbent stored immutable
plan, including after a username/link retarget, and replays it. Pending same-
fingerprint returns `IDEMPOTENCY_PENDING`; different fingerprint returns
`IDEMPOTENCY_CONFLICT`. Every incumbent branch returns without config CAS,
append permit, insertion capacity, current intent/outcome or current group.
Pending/conflict never prompts: no-TTY without `--yes` still returns the lookup
terminal because no destructive action or replay plan is admitted.

A destructive confirmation of the proposed plan does not authorize replay of a
different incumbent plan. Before CAS/intent, completed incumbent replay freshly
confirms its exact stored plan while the epoch protects it: `--yes` confirms it;
TTY renders/challenges it; decline/EOF/explicit cancel/non-TTY without `--yes`
returns canonical `CONFIRMATION_REQUIRED`; deadline equality returns exact
`replay_confirmation` TIMEOUT with `completed_unchanged`. No proposed-plan
prompt occurs on that branch. An absent destructive winner confirms the
proposed effective plan at the same pre-CAS position using ordinary new-mutation
branches. Every failed confirmation creates no intent. A confirmed completed
incumbent replays directly and creates no current group; only a confirmed miss
proceeds to config CAS, permit and intent. Thus every destructive current intent
has its ordinary exact `yes` or TTY `confirmation_source`; incumbent returns
have no current confirmation source.

The repeated lookup, insert and rewrite are continuously protected by the same
outer epoch and verified lease, so post-intent insert loss is impossible in
normal execution. If a fault injection or invariant violation nevertheless
returns an incumbent, never replace or remove it and do not classify the result
as replay/pending/conflict. Before dispatch, append/fsync mutation-none outcome
with `success:false`, empty `completed_stages`, and exact terminal
`{"kind":"error","code":"INTERNAL","message":"internal error","details":{"operation":"<operation>","reason":"internal_error"},"exit_code":1}`.
After that outcome is durable, emit it once and enter durability-fatal shutdown.
Outcome append/fsync failure emits no terminal, closes the connection and is
audit-fatal. Crash after intent but before outcome gives the crashed connection
no terminal; next inspection applies intent-without-dispatch recovery with exact
mutation-none `AUDIT_INCOMPLETE`, never mutates the incumbent, then uses the
existing continue rule. Crash after INTERNAL outcome but before its frame gives
that connection no terminal; retry uses ordinary authoritative lookup. There is
no confirmation after intent.

Completed replay проходит Ready/getMe/bot/authority. `msg_delete`/`chat_leave`
дополнительно требуют fresh confirmation над exact stored `plan`; cancel/no-TTY
возвращает canonical `CONFIRMATION_REQUIRED`, store не меняется, нового audit
group нет. Если monotonic deadline выигрывает во время replay confirmation,
возвращается exact §4.5.11 `replay_confirmation` TIMEOUT; completed entry и prior
audit остаются byte-unchanged. Replay строит Result/Error с текущим Request.id.

Per-cutpoint transitions:

| Cutpoint/outcome | audit | keyed store | terminal |
|---|---|---|---|
| before intent | none | none | ordinary error |
| intent, expected-winner quota/write failed | none outcome required | none | exact idempotency error only after outcome |
| intent, unexpected incumbent | none INTERNAL outcome required | incumbent unchanged | exact INTERNAL after outcome, then durability-fatal |
| pending, before dispatch (including changed file/schedule recheck) | none outcome | remove pending | exact error |
| dispatch, no proof; timeout/auth/generation/shutdown/top-level uncertain TD error | possible outcome | keep pending | exact error if durability succeeds |
| explicit single send failed state/update | none outcome | remove pending | TDLIB/RATE error |
| temporary message deleted before confirmation | possible outcome | keep pending | SEND_FAILED |
| direct correlated success/result | confirmed outcome | completed result | result |
| direct correlated error after dispatch | possible outcome | keep pending | TDLIB/RATE error |
| join Guard/Declined explicit no-join result | none outcome | remove pending | join error |
| empty mark-read audited no-op | none outcome | completed result | result |
| forward all success | confirmed outcome | completed result | result |
| forward partial success | confirmed outcome | completed FORWARD_PARTIAL | error |
| forward all explicit failed/null | none outcome | remove pending | FORWARD_FAILED/RATE |
| forward any deleted ambiguity and no success | possible outcome | keep pending | FORWARD_FAILED |
| forward deadline with any pending | confirmed if a sent item else possible | keep pending | TIMEOUT |

После Telegram success порядок фиксирован:
mutation checkpoint → audit outcome → store completion → terminal. Если audit
outcome durable, но store completion fail, daemon не выдаёт ни success, ни
replacement error; connection closes, daemon enters durability-fatal, startup
repair completes store from audit. Segment остаётся pinned. Аналогично failure
store transition обязан стать durable до error frame.

Every outer epoch samples wall-clock Unix seconds once after acquiring the
mutex. An entry is expired iff `sampled_now >= expires_at`; equality is
expired. A new winner takes one fresh integer wall sample immediately before
constructing its entry and computes `expires_at` by checked addition of exactly
604800. Wall clock and the monotonic request deadline are independent.

Clock rollback is conservative: it delays expiry, is not a contradiction, and
does not use a persisted or process-local high-water mark. `created_at` need not
be monotonic between entries. An unrepresentable sample or expiry addition
fails before insertion as `IDEMPOTENCY_UNAVAILABLE/schema_error`. Entries are
never expired early or automatically resent.

Reconciliation always precedes expiry, and even already-expired entries remain
pins until their audit/store/spool relation is proven. Sweep visits canonical
key-hash order and removes all expired pending/completed entries in one rewrite
when possible. Terminal/completed cleanup is retried before sweep. An expired
pending unknown entry is store-removed and fsynced before its newly eligible
spool is deleted and the spool root fsynced, all before rotation/current intent.
Cleanup failure stops the request and retains an audit-derived generation hold.
After equality expiry lookup sees absence and a later same-key mutation may
dispatch; exactly-once is no longer claimed.

The reusable known-pin core gate is exactly steps 1–9:

1. classify and enumerate the spool root without mutation;
2. read the canonical store and inspect, but do not promote, the fixed temp;
3. derive pins from every entry, including expired entries;
4. scan all audit segments once for group facts and pin validation;
5. select spool contradictions before any recovery/temp-cleanup write;
6. remove and directory-fsync a safe stale temp;
7. reconcile the open group and every store-pinned completed group;
8. retry eligible terminal/completed spool cleanup;
9. sample/sweep expiry and clean newly eligible spools;

Initial epoch stops here, then hashes/looks up: no permit, rotation-capacity or
store insertion-capacity calculation. Commit repeats core gate and authoritative
lookup; an incumbent terminal returns before confirmation/CAS/permit/capacity
and creates no group. Only a miss confirms the proposed plan if required,
performs config CAS, then computes append permit from exact intent bytes. After
durable intent returns generation, insert-if-absent revalidates absence: an
unexpected incumbent takes the invariant-fatal path without capacity;
otherwise exact prospective-winner quota precedes insertion.

The scanner assigns the intent segment inode to every open group as
`audit_generation` and streams each fully validated completed v2 group in audit
chronological order to a non-mutating consumer. The immutable typed view has
exactly: generation; invocation/account/operation/fingerprint; nullable key
hash; immutable plan; exact intent timestamp and validated Unix seconds;
nullable exact `idempotency_pending` payload; nullable exact `SpoolRef`;
durable temporary ids or `[]`; latest durable forward vector or `[]`; nullable
mutation proof; exact completed stages; and nullable exact outcome including
success/mutation/terminal. No process-memory or later store value is folded in.
It is streamed rather than accumulated across five maximum-sized segments.

For `KnownPins`, the audit layer builds one bounded tuple index and validates
all `(audit_generation,invocation_id,request_fingerprint,operation)` relations
while each segment is already streamed. Unmatched, duplicate, dangling or
mismatched pins are contradiction. Pin count never creates a per-pin segment
rescan; the separately required invocation-id rescans remain unchanged. A
successful scan yields a move-only append/rotation permit containing validated
segment identities and pin relations. Each uncleaned spool mints a move-only
hold bound to exact `(generation,invocation_id,SpoolRef,permit)`. Cleanup accepts
that hold and only missing-is-success deletion plus root fsync returns a typed
move-only release receipt. Rotation drops a hold only by consuming its exact
receipt; strings/booleans/wrong or duplicate receipts cannot release it. After
expiry the caller may narrow store pins only to a validated subset. Rotation
metadata-revalidates the permit, protects that subset plus every unreleased
hold, and does not rescan pins.

`AbsentByPolicy` is distinct from known-empty pins. It performs no canonical or
temp idempotency-store open, cleanup, quota, expiry or mutation. It may
reconcile only the already accepted audit-only groups; an incomplete keyed
group still stops with `AUDIT_INCOMPLETE`. Its rotation permit may consume a
missing numbered hole but never delete an occupied numbered slot. Completed
keyed groups do not by themselves block M6. Session paths retain their existing
account-global spool gate.

Unkeyed dispatch-unknown spool expiry is checked addition
`intent_unix_seconds+604800`; the one gate wall sample makes it eligible at
`now >= expiry`. Equality is eligible and rollback delays cleanup without
contradiction/high-water mark. Known-terminal unkeyed is immediately eligible.

For expired keyed pending, store removal+directory fsync occur before cleanup,
but the typed hold remains. Crash after store removal reconstructs it from the
completed view and spool inventory. Crash after deletion/root-fsync but before
receipt consumption repeats missing-is-success cleanup/root-fsync and remints
the receipt. Only receipt consumption makes the generation capacity-evictable;
cleanup failure stops before rotation/current intent.

#### 4.5.8 Canonical JSON, hashes, fingerprints и secrecy

Canonical JSON domain содержит только null, booleans, strings, integers,
arrays, objects:

- input strings valid Unicode scalar UTF-8; normalization отсутствует;
- object keys unique и сортируются по unsigned UTF-8 bytes;
- arrays сохраняют order;
- integers — minimal base-10 ASCII, minus only for negative, zero exactly `0`,
  no plus/leading zero/fraction/exponent;
- strings quoted; `"` и `\` escaped; U+0000…U+001F всегда `\u00xx` с lowercase
  hex; остальные scalars raw UTF-8;
- whitespace отсутствует.

```text
key_hash =
 SHA256("tgcli-idempotency-key-v1\0" || exact_key_bytes)
fingerprint =
 SHA256("tgcli-idempotency-request-v1\0" || canonical_json_bytes)
invite_hash =
 SHA256("tgcli-invite-link-v1\0" || exact_invite_bytes)
```

Hash strings render `sha256:` + lowercase hex.

Fingerprint root exact:

```json
{"version":1,"account":"main","principal":{"id":42,"is_bot":false},
 "operation":"send","payload":{}}
```

Payload exact per operation:

```text
send:
  chat_selector,text,parse_mode,reply_to,requested_topic,silent,schedule
msg_edit:
  chat_selector,message_id,text
msg_delete:
  chat_selector,message_ids,for_all
msg_forward:
  from_selector,to_selector,message_ids,drop_author
msg_react:
  chat_selector,message_id,reaction,remove,big
msg_pin/msg_unpin:
  chat_selector,message_id
chat_mark_read:
  chat_selector
chat_mute/chat_unmute:
  chat_selector,duration_seconds
chat_pin/chat_unpin:
  chat_selector
chat_archive/chat_unarchive:
  chat_selector
chat_join username:
  source,username
chat_join invite:
  source,invite_link_sha256
chat_leave:
  chat_selector
saved_attach:
  message_id,topic="inherit_saved",name,size,sha256,caption
```

`chat_selector/from_selector/to_selector`, `username`, text, reaction,
caption and name are strings; ids and arrays use the ranges in §4.5.3; flags are
booleans; hashes/FileSnapshot fields use their named strict types.
`requested_topic` is ForumTopicRef/null. Schedule is null,
`{"kind":"online"}` либо `{"kind":"at","send_date":int32}`. Exact selectors
после syntactic canonicalization: decimal IDs minimal; username и non-invite
public links сохраняют exact validated input bytes; invite заменён hash.
Fingerprint не включает timeout/tty/json/yes/authority/cwd
directory/server_now/effective resolved ids/transport id.

Raw idempotency key никогда не передаётся TDLib. Raw invite живёт только до
`checkChatInviteLink/joinChatByInviteLink`; tgcli diagnostics/errors/audit/store
используют hash. TDLib log callback обязан иметь concurrent sensitive-value
registry и до sink заменять exact invite bytes на
`<redacted:invite-link>` до конца correlated request; raw value затем wiped.

Tests создают distinct random byte sentinels для key/invite, провоцируют
success, parse error, TD error, timeout, audit/store error и log rotation, затем
byte-scan-ят stdout/stderr, every audit/store generation, tdlib.log
generations, crash diagnostics и core-disabled test artifacts. Ни один raw
sentinel не допускается; hash обязан присутствовать в соответствующем
audit/store record.

#### 4.5.9 Direct RPC and single-message arbitration

Every TD response/update/auth transition получает monotonic
`receive_event_sequence`. Event eligible только если observed monotonic time
`< deadline`; equality принадлежит deadline. Old generation discarded.
One-shot CAS выбирает terminal.

Для direct RPC earliest eligible correlated response против first non-Ready
auth event wins:

- earlier success/ok confirms desired state;
- earlier TD error returns TD error; post-dispatch mutation остаётся possible;
- earlier non-Ready returns `NOT_AUTHED/authorization_lost`, mutation possible;
- deadline before both returns TIMEOUT, mutation possible.

Late events только освобождают correlation; store state не меняют. Cancellation
до durable dispatch — not_started; после — unknown/pending.

Single send subscribes before `sendMessage`. Callback only queues. Immediate:

- `messageSendingStatePending` → durable temp id, wait;
- `messageSendingStateFailed` → explicit failure;
- null sending state → authoritative stable Message;
- unknown variant → INTERNAL possible/pending.

`updateMessageSendSucceeded.old_message_id` maps temp→final Message.
`updateMessageSendFailed` explicit failure. `updateDeleteMessages` с temp id
до success — deletion ambiguity. Response/update arbitrary order буферизуется.
Final id только из success Message. Scheduled success — server acceptance, не
future delivery.

#### 4.5.10 Forward exact vector contract

`ForwardItem` strict oneOf:

```json
{"source_id":1,"status":"pending","temporary_message_id":-1}
```

```json
{"source_id":1,"status":"sent",
 "message":{"id":101,"chat_id":-1001,"date":"2026-08-05T10:00:00Z",
            "sender":{"type":"user","id":42},"is_outgoing":true,
            "topic":null,"type":"text","text":"forwarded","scheduled":false}}
```

```json
{"source_id":1,"status":"failed",
 "failure_reason":"upstream_null|tdlib_error|deleted_before_confirmation",
 "tdlib_code":null,"retry_after":null}
```

`message` in the sent branch is the exact §4.5.1 `MessageWriteResult`, never an
arbitrary object or summary subtype. Each branch has exactly the shown fields;
the pending branch alone has `temporary_message_id`, the sent branch alone
has `message`, and the failed branch alone has
`failure_reason,tdlib_code,retry_after`.

`tdlib_code` non-null only for `tdlib_error`; `retry_after` non-null only for
code 429 and equals mathematical ceiling of positive TDLib seconds. Null
immediate vector element is `upstream_null`; temp deletion has no invented
code and is `deleted_before_confirmation`.

Immediate vector length must equal input. Every item follows single-send
correlation. Terminal aggregation:

- all sent → ordered success result;
- at least one sent and all terminal → `FORWARD_PARTIAL`, completed error;
- none sent, all terminal, every failed code 429 → forward RATE_LIMITED,
  `retry_after=max(item.retry_after)`;
- none sent, all terminal otherwise → `FORWARD_FAILED`;
- any pending at deadline → forward TIMEOUT with full vector.

The same `ForwardItem` schema is referenced by success result, both forward
errors, TIMEOUT, every `forward_progress` checkpoint and store entry. Success
contains only sent branches; PARTIAL contains at least one sent and one failed
and no pending; FAILED/RATE contains only failed; TIMEOUT may contain any
branch and may use `[]` only before the immediate vector exists.

All-failed mutation state none only when every reason is upstream-null or
explicit TDLib send failure. Any deletion ambiguity makes possible. Any sent
item makes confirmed. Top-level `forwardMessages` error before vector is
possible after dispatch and keeps key pending. Multi-message atomicity и
automatic subset retry не заявляются.

#### 4.5.11 TIMEOUT strict oneOf

`state` is null or one exact §4.5.7 auth state. Every M3 TIMEOUT details matches
exactly one branch:

```json
{"operation":"<any>","phase":"preflight","state":"ready",
 "outcome":"not_started",
 "idempotency":"not_requested|not_created|removed"}
```

```json
{"operation":"msg_delete|chat_leave","phase":"replay_confirmation",
 "state":"ready","outcome":"not_started",
 "idempotency":"completed_unchanged"}
```

```json
{"operation":"<direct-op>","phase":"dispatch","state":"ready",
 "outcome":"unknown","idempotency":"not_requested|pending"}
```

```json
{"operation":"send|saved_attach","phase":"confirmation","state":"ready",
 "outcome":"unknown","idempotency":"not_requested|pending",
 "temporary_message_id":null}
```

```json
{"operation":"msg_forward","phase":"confirmation","state":"ready",
 "outcome":"unknown","idempotency":"not_requested|pending",
 "items":[]|ForwardItem[1..100 containing at least one pending]}
```

`<any>` means the closed §4.5.1 operation enum. `direct-op` is exactly
`msg_edit,msg_delete,msg_react,msg_pin,msg_unpin,chat_mark_read,chat_mute,
chat_unmute,chat_pin,chat_unpin,chat_archive,chat_unarchive,chat_join,
chat_leave`.
Single temp is null или one int53. Forward items are either empty before the
immediate vector exists or a nonempty input-order vector containing at least
one pending item.
Post-dispatch keyed timeout всегда `pending`; pre-dispatch keyed pending,
если уже создан, удаляется durable и reports `removed`; keyed request до
insert reports `not_created`; no key = `not_requested`.
`completed_unchanged` допускается только для deadline в confirmation
completed destructive replay: no new intent, no store write, no dispatch.
TIMEOUT никогда не claims not_started после `dispatch_started`.

#### 4.5.12 File snapshot/spool and Saved attach

Minimal M4 sends any one file as document:

```text
inputDocument(inputFileLocal(spool_path), thumbnail=null,
              disable_content_type_detection=true)
inputMessageDocument(document, caption)
```

No autodetection/albums/spoiler in v1.

PATH is valid Unicode-scalar UTF-8, 1…4096 caller bytes, without NUL. A
trailing slash is invalid. Relative PATH resolves only against the frozen
client cwd. That cwd must be valid UTF-8, non-NUL, absolute, 1…4096 bytes and
already lexically canonical; otherwise the relative PATH is
`USAGE/{"argument":"PATH","reason":"invalid_argument"}`. Absolute PATH
ignores cwd. No fallback directory is used.

The canonical display path is the lexical normalization of canonical cwd plus
PATH, or `/` plus absolute PATH: empty and `.` components are omitted, `..`
pops one component without moving above `/`, and the result has one leading
slash, one separator, no dot components and at most 4096 UTF-8 bytes. It is
exactly `FileSnapshot.path` and the path in source/spool errors; only
`audit.arguments.path` preserves caller spelling. The display path is never
used to open the source.

Before pass 1 tgcli freezes an in-memory locator containing the exact original
PATH component sequence—including empty repeated-separator components, `.`
and `..`—and, for a relative PATH, one retained descriptor for the validated
client cwd. Each pass replays that exact sequence independently: absolute
starts from `/`; relative starts from a `dup` of the same cwd FD. Empty
components are explicit no-ops, `.` and `..` are processed in sequence, and
every named component is opened no-follow. No lexically cancelled component
is omitted. `/symlink/../file` is rejected while its canonical error path is
`/file`; renaming/replacing the cwd pathname between passes does not retarget a
relative source.

Each pass retains its directory-edge FDs. After pass-1 read and pass-2 copy it
revalidates every raw named edge with `fstatat(...,AT_SYMLINK_NOFOLLOW)` against
the retained child FD, and revalidates the basename entry against the source
FD. Disappearance, replacement, symlink, type or device/inode mismatch is
`INPUT_CHANGED`. Empty components have no entry; the frozen cwd root is
validated by FD, never reopened by its former pathname.

The final basename is the exact `FileSnapshot.name`: valid UTF-8, 1…255
bytes, no slash, C0/C1 control scalar, `.` or `..`. Other leading-dot names
are allowed. No escaping, hashing, normalization or case folding occurs. The
destination `_PC_NAME_MAX` must also admit it. The target is a readable,
nonempty regular file. Source owner, mode and link count are not restricted;
hard links are accepted. There is no local file-size or spool-byte quota.

Pass 1 before idempotency lookup opens the source once. It records
dev/inode/size/mtime_ns/ctime_ns before reading, hashes exactly `size` bytes,
probes EOF, records the identity after reading, and requires identical
identities and byte count. Initial missing/symlink/wrong-type/empty/unreadable
conditions use §4.5.3's source-file `NOT_FOUND`; malformed path is `USAGE`;
other I/O/representation failures are `SPOOL_UNAVAILABLE`. Any instability
after a regular target was observed is `INPUT_CHANGED`. Pass-1 failure creates
no current-invocation intent, idempotency entry or spool. A real request's
earlier v2 preflight may already have durably reconciled prior groups; absolute
zero persistence requires a clean preflight. All seventeen M3/M4 dry-runs
perform no reconciliation and retain their absolute zero-persistence rule.
Dry-run stops after pass 1 and its ordinary read-only planner work.

Pass 2 is permitted only after durable intent, and for a keyed invocation
only after durable insert-if-absent victory. It replays the frozen locator,
opens the source no-follow once, requires the before identity to equal pass 1,
copies and SHA-256 hashes in one streaming pass, then performs the full
directory-edge/basename revalidation and requires exact byte count, EOF,
after identity and digest equality with pass 1. Every source discrepancy is
`INPUT_CHANGED`; genuine I/O is `SPOOL_UNAVAILABLE`. Both are pre-dispatch
mutation `none` outcomes, with keyed pending removal and eligible cleanup.

Pass 2 creates `<account-state>/spool/<invocation_id>/<FileSnapshot.name>`.
Account-state, spool root and invocation directories are current-uid,
non-symlink exact-0700 directories; the exact-0600 `O_EXCL` file is a
current-uid non-symlink regular file with link count 1. Creation fsyncs
account-state after a new spool root and the spool root after a new invocation
directory, then fsyncs the completed file and invocation directory. The
persisted path is exactly `spool/<invocation_id>/<FileSnapshot.name>`.

After file and invocation-directory sync, tgcli appends and fsyncs
`spool_ready`. A keyed winner then durably changes its exact pending entry
from `spool:null` to the matching `SpoolRef` under the shared outer account
epoch. There is no separate inner store mutex or lock file. Only that
store update, or `spool_ready` itself for unkeyed execution, admits
`dispatch_started`. The store never references a spool without durable
`spool_ready`. Pass 1 hashes in the initial outer epoch; pass 2 hashes/copies in
the commit outer epoch; audit-first checkpoint then store rewrite remain ordered
inside that epoch.
Only the spool
path enters TDLib. Recovery/cleanup:

| Crash cutpoint | Action |
|---|---|
| during create/copy, no `spool_ready` | no dispatch possible; delete incomplete invocation dir, fsync spool root, close outcome none; remove pending only when keyed |
| `spool_ready`, no dispatch | delete spool, close outcome none; remove pending only when keyed |
| keyed dispatch, no terminal proof | retain spool and audit/store reference through pending expiry |
| keyed mutation checkpoint/outcome, store not completed | retain until store repair |
| keyed completed/removal store durable | cleanup may run; missing-after-delete is success; fsync spool root |
| unkeyed durable terminal outcome | cleanup may run; missing-after-delete is success; fsync spool root |
| unkeyed unknown | retain by audit invocation until terminal proof or intent timestamp +604800 |

Cleanup failure не меняет уже confirmed Telegram result; reference остаётся
durable и startup повторяет cleanup. Startup никогда удаляет spool по age
без reconciled audit/store relation.

Keyed completion is first durable with non-null `Entry.spool`. Successful
cleanup fsyncs the spool root and then canonically rewrites the completed
entry with `spool:null`. Failure to clear that reference does not replace a
confirmed terminal; startup accepts missing-after-delete and retries the
clear. Keyed mutation-none removal is durable before cleanup. Unkeyed cleanup
is related by audit. Cleanup failure never replaces the selected terminal.

Spool-root classification is total after no-follow validation of
`<account-state>`; an unsafe account-state maps through the same reason table
without exposing or repairing that path. Root absence is clean and creates nothing.
Safe means a current-uid, non-symlink exact-0700 directory whose opened FD
matches the entry. Symlink/replacement is `SPOOL_UNAVAILABLE/path_invalid`, a
file is `wrong_type`, wrong uid is `wrong_owner`, wrong mode is `wrong_mode`,
open/fstat I/O is `open_failed`, and enumeration I/O is `read_failed`. The
account-global error uses the requested `v2_gate_operation` and literal
redacted `path:"spool/"`. Unsafe roots are retained and never repaired.

After safe-root enumeration, raw entry names are sorted lexicographically by
unsigned bytes, shorter equal-prefix first. An orphan/invalid entry or
audit/store mismatch is a retained contradiction. Its complete absolute raw
path is rendered as canonical `FilesystemDiagnosticPath` with
`kind:"bytes_hex"` and two lowercase hex digits per raw byte, regardless of
UTF-8 validity. No filesystem entry bytes are placed directly in a JSON
string.

The account-global v2 gate blocks every real M3/M4 operation and M6
`session list` plus dry/real `session terminate`. It permits truly
persistence-free reads, v1-only M1 behavior and all seventeen M3/M4 dry-runs,
which never reconcile v2. Root classification precedes contradiction
selection, which precedes prior-group reconciliation/capacity preflight. For
session list/dry terminate this is §4.7.5 step 2; real terminate and real
M3/M4 retain their auth/bot/authority-before-step-6 ordering. Root failure is
account-global `SPOOL_UNAVAILABLE`; contradiction is the object-path
`AUDIT_INCOMPLETE` with mutation none and empty stages. No resend occurs.

`v2_gate_operation` is exactly the seventeen §4.5.1 operation names plus
`session_list` and `session_terminate`.

`saved_attach` exact order:

1. Ready → `getMe` → user-only preflight → write authority.
2. Acquire the initial epoch and perform complete prior spool/audit/store
   reconciliation and expiry before source parsing/hash.
3. Still under that epoch, parse message id/path/caption, perform pass 1, then
   fingerprint only caller facts:
   `message_id,topic="inherit_saved",name,size,sha256,caption`.
4. Perform keyed lookup in that epoch. Completed replay therefore
   does not require Saved materialization or the original message to exist.
5. Only unkeyed/new-miss execution materializes self chat, gets
   original/properties, validates reply, accepts derived saved/null topic and
   builds the proposed planning result without prompting.
6. Acquire commit epoch; repeat core gate/expiry and authoritative lookup.
   Incumbent completed replay/pending/conflict returns with no current group;
   completed destructive replay confirms the exact incumbent plan. Only a miss
   performs config CAS and computes append permit before intent.
7. Durable intent returns generation; keyed insert revalidates absence, then
   checks exact prospective-winner quota and must win; an unexpected incumbent
   takes the invariant-fatal INTERNAL path without capacity. Unkeyed continues.
8. Only winner/unkeyed runs pass 2, `spool_ready`, dispatch and coordinator,
   all while commit epoch remains held.

Derived self chat id, original properties and effective saved/null topic are
plan/audit facts, never fingerprint inputs. The original is not changed.
Prior reconciliation may write before pass-1 failure, but no current artifact
may. Clean preflight failure and every dry-run retain absolute zero persistence.

#### 4.5.13 Acceptance and TestDC

Required fake-boundary/contract gates:

- v3 exact frame/context, v1/v2↔v3 frozen replacement, status/stop/restart,
  absent/mismatch M3 dry-run autospawn;
- named dry-run allowlist and observed TDLib cache effects, including
  type-bound private `getUser` and supergroup/channel `getSupergroup`
  `ChatIdentity` enrichment; reject an unrelated entity id, mismatched
  channel bit, direct/raw use of either function, and every non-allowlisted TD
  function before the fake boundary; basic-group, title-like, and rejected
  secret/unknown branches issue neither enrichment call; assert the exact
  §4.5.3 `resolve`-attributed 429, other TD error, null/unknown, invalid-id,
  mismatch, and malformed-returned-username branches; zero config, audit,
  idempotency-store, spool, prior-group-reconciliation, or other tgcli
  persistence writes and zero mutating TD call for every one of the seventeen
  planners;
- bot immediate send positive; bot date/online/reaction negative before
  user-only calls;
- folded request grant audits `authority_source=request`; config grant audits
  `config`; hash/stat replacement at every config snapshot capture/recheck
  rejects the complete canonical identity;
- schedule rounding/int32 and every boundary/race in §4.5.5;
- barriers force two initial same-key misses: the first commit lookup misses,
  creates the sole current group, wins and dispatches; the second commit lookup
  observes pending/completed and returns with no confirmation, current group,
  capacity check or dispatch; assert exactly one current group;
  same fingerprint with username/link retarget adopts incumbent only for
  completed replay, freshly confirms exact incumbent across yes/TTY/decline/
  EOF/cancel/no-TTY/deadline, never dispatches new target, and creates no
  current group; pending/conflict returns without prompting even for a
  destructive proposed plan and no-TTY/no-yes;
- quota tests include the exact prospective pending-entry insertion bytes plus
  terminal growth and reject before dispatch;
  initial lookup invokes no capacity/permit, commit append permit precedes
  intent, repeated incumbent returns before capacity, and expected-winner quota
  follows generation; fault-injected unexpected insert loss skips capacity,
  never touches the incumbent, durably closes exact mutation-none INTERNAL,
  emits once then is durability-fatal; outcome failure emits nothing; crash
  before outcome recovers exact AUDIT_INCOMPLETE;
- every exact `IDEMPOTENCY_UNAVAILABLE` reason and precedence pair, canonical
  absolute final path, stable message/details and no temp/internal/raw detail;
  mutex-wait deadline/cancel wins before spool/root, which wins only after lock;
- every audit/store/spool fsync cutpoint, including success→outcome→store
  failure with no terminal and startup repair;
- mixed v1/v2 audit, every exact completed-view field, one-pass pins, typed
  hold/release misuse, store-removal/cleanup crashes, unkeyed timestamp expiry,
  contradictions and pinned rotation/capacity release;
- exact canonical bytes/golden hash vectors and raw sentinel scans;
- destructive completed replay prompts stored plan and never dispatches;
  confirmation deadline yields `completed_unchanged` with byte-identical
  store/audit;
- `saved_attach` proves prior reconciliation before pass 1, pass-1 SHA and
  fingerprint before lookup, repeated core gate/authoritative lookup returns an
  incumbent without a current group or, only on miss, proceeds CAS/permit before
  intent; pass-2 hash/spool/dispatch remains under the commit epoch; dirty
  pass-1 failure permits only prior recovery writes, clean failure/dry-run
  absolute zero;
- all direct response/auth/deadline permutations;
- all single-send response/update/delete/generation permutations;
- every forward vector/429/deletion/timeout aggregation;
- both passes replay one frozen cwd FD and the exact original component
  sequence, including repeated separators, dot and lexically cancelled
  components; after-read/copy parent-edge and basename-entry replacement,
  disappearance and symlink races; keyed-winner and unkeyed-post-intent
  hard-link-positive, wrong-type, empty, unreadable, same-size rewrite,
  truncate/append and identity/digest mismatch; 4096-byte path,
  safe-basename/control/255-byte and caption byte/scalar boundaries; total
  absent/safe/unsafe root classification; raw-byte orphan sorting and exact
  `bytes_hex` diagnostic encoding; effect-based gate coverage; every
  create/copy/file-fsync/directory-fsync/audit-ready/keyed-spool-ref/cleanup/
  ref-clear cutpoint; no local file-size/spool quota and no raw file bytes in
  any output, diagnostic, log, audit or store artifact; pass-1 failure creates
  no current-invocation artifact, real dirty-preflight fixtures permit only
  prior-group recovery records, and clean plus all seventeen dry-run fixtures
  prove absolute zero persistence;
- strict actual result/error/audit/store schema validation.

TestDC M3 flow is mandatory after canonical user auth smoke:

1. Send unique text to Saved with unique key.
2. Immediately after final id, install a scope guard that always executes
   `msg delete <saved-chat> <final-id> --yes` with write authority.
3. Verify `msg get` final id.
4. Repeat same key/payload: current-id result byte-equivalent in data, no second
   Telegram message.
5. Same key/different text → IDEMPOTENCY_CONFLICT.
6. Run registered cleanup on success and every assertion/command failure.
7. Verify deleted message absent.

Cleanup failure fails the test and preserves private stdout/stderr/audit
artifacts with the final id; it is never swallowed by an earlier failure.
Registration itself occurs before step 3.

M3 flow has no skip branch. Missing required user phone/code/API fixture,
login failure, Saved capability error, arbitrary TDLib error или cleanup error
fails job. Только существующие canonical optional auth skips
`fixture_missing:qr_approver`, `fixture_missing:bot_token_cmd` и exact
`test_dc_state_not_forceable:<state>` остаются допустимыми для их existing
M1 tests; они не могут быть assigned M3 flow.

#### 4.5.14 Implementation dependency slices

0. Separate reviewed DESIGN-only protocol/dry-run precursor.
1. Protocol v3 parser/writer/schemas and v1/v2↔v3 lifecycle fixtures.
2. M2 exact resolver/principal reuse and bot matrix.
3. Neutral TD request/update DTOs and direct arbitration.
4. Audit v2 schemas/writer/recovery/rotation.
5. Canonical JSON, idempotency store/quota/CAS/repair.
6. Shared direct and single-message coordinators.
7. Send text/schedule/topics.
8. Edit/react/message pin.
9. Delete/leave confirmation and replay confirmation.
10. Forward vector coordinator.
11. Chat writes/join.
12. Two-pass spool and cleanup.
13. Saved attach adapter.
14. Full fake-boundary, fault, sentinel and TestDC gates.

#### 4.5.15 Explicitly impossible/non-guaranteed

v1 не заявляет:

- atomic multi-message forward;
- exactly-once без key, после pending expiry или после unknown outcome;
- final message id recovery, если ни TDLib, ни durable checkpoint не оставили
  correlation proof;
- cancellation после dispatch;
- preservation concurrent notification-setting writes как CAS;
- coherent malicious in-place source version; second pass гарантирует только
  совпадение полной observed identity и прочитанного digest;
- atomic schedule boundary между `getOption` и TDLib internal clock;
- future delivery time for scheduled/online message;
- adding attachment into already-existing message;
- undo delete/leave.

При unknown outcome tgcli не пытается «догадаться» или автоматически повторить
mutation.

### 4.6 M5 streaming contract

This section closes the M5 `listen` / `wait-for` contract only. It does not activate
M6 or M7 syntax.

#### 4.6.1 Verified current constraints

- The existing update bus invokes every handler synchronously on TDLib's receive
  thread while holding its bus mutex (`src/core/update_bus.hpp:11-38`). A handler
  cannot block, query TDLib, subscribe, unsubscribe, or write a socket.
- Connection writes are synchronous and may wait five seconds
  (`src/daemon/server.cpp:43-55`). Current item delivery ignores the boolean write
  result (`src/daemon/server.cpp:76-80`).
- RequestSession gates Result/Error but not Item (`src/daemon/request_session.cpp:695-754`).
  M5 therefore requires a new item/terminal ordering seam.
- An absent timeout currently becomes 60 seconds (`src/proto/frame.cpp:601-618`),
  contrary to the documented unlimited stream default.
- The client prints every Result and does not flush each item
  (`src/cli/client.cpp:83-102`). `listen` needs a silent internal terminal and
  checked per-item flush.
- Request activity can be promoted to subscription activity, but hub registration and
  promotion are not one transaction (`src/daemon/request_session.cpp:559-562`).
- TDLib drops old client-generation events before publication and stamps accepted
  events with a process-local receive sequence (`src/core/td_client.cpp:745-804`).
  That sequence is ordering metadata only, never a public cursor.
- M2 `MessageSummary`, `ChatIdentity`, `ChatSummary`, resolver, sender, int53, and
  local-history components are specified but not yet implemented. M5 depends on
  those shared implementations and must not duplicate them.

Pinned TDLib source establishes the following exact update surface:

- `updateNewMessage` contains incoming or outgoing messages
  (`td_api.tl:10119-10120`).
- `updateMessageContent` and `updateMessageEdited` are separate updates
  (`td_api.tl:10140-10148`).
- User accounts receive `updateMessageInteractionInfo`, whose reactions are a
  nullable current snapshot (`td_api.tl:10153-10154`, `2894-2914`).
- Bots instead receive `updateMessageReaction`, a public-reaction per-actor old/new
  delta, and `updateMessageReactions`, an anonymous-reaction count snapshot
  (`td_api.tl:10904-10918`). These are not interchangeable with
  `updateMessageInteractionInfo`.
- Chat counters also arrive on `updateMessageMentionRead`,
  `updateMessageUnreadReactions`, and `updateMessageContainsUnreadPollVotes`, not
  only on their `updateChat*Count` counterparts (`td_api.tl:10159-10174`).
- Deletion is a batch (`td_api.tl:10404-10409`).
- `getChatHistory` is user-only (`td/telegram/Requests.cpp:3568-3571`), returns
  decreasing message IDs, accepts `limit <= 100`, may return fewer than requested,
  and is offline for `only_local=true` (`td_api.tl:11492-11500`).

These are source facts, not claims about delivery by Telegram in every deployment.

#### 4.6.2 CLI grammar, defaults, and validation

```text
tgcli listen
  [--chat <chat>]...
  [--types <comma-list>]
  [--count <N>]
  [--timeout <S>]

tgcli wait-for
  [--chat <chat>]
  [--from <user>]
  [--regex <pattern>]
  [--after <message-id>]
  [--timeout <S>]
```

Exact rules:

- `listen --chat` accepts zero through 64 occurrences. All selectors resolve before
  activation. Resolved duplicate chat IDs are collapsed. Any failure is atomic and
  produces no stdout.
- `wait-for --chat` accepts zero or one occurrence.
- `--types` occurs at most once and contains a nonempty comma-separated subset of
  exactly `message`, `edit`, `delete`, `reaction`, `chat`. Empty tokens, whitespace,
  duplicates, and unknown tokens are `USAGE/invalid_argument`. Default is all five.
- `--count` is `listen`-only and accepts canonical decimal integers
  `1..1000000`.
- `--after` accepts a positive TDLib int53, `1..9007199254740991`, and requires
  `--chat`.
- Explicit `--timeout` accepts finite seconds in `0.001..31536000` inclusive.
  Fractional values round upward to the next monotonic-clock tick.
- No timeout means unlimited for both commands. `listen` without count or timeout is
  indefinite.
- `listen` count or active-stream deadline expiry is planned exit 0.
- `wait-for` deadline expiry is `TIMEOUT`, exit 7.
- `--cursor`, `--full`, `--dry-run`, `--idempotency-key`, `--local`, and write-tier
  flags are unsupported on these read commands.
- `listen` always emits compact NDJSON, one item per line, in human and JSON modes.

CLI syntax and pure local validation happen before the protocol request. Daemon setup
precedence is exact:

1. account/config admission;
2. selected-account Ready check;
3. `getMe` and account-kind classification;
4. bot `wait-for --after` rejection;
5. chat resolution using the accepted M2 resolver;
6. sender resolution using the accepted M2 resolver;
7. subscription activation;
8. local scan, if requested, then live processing.

Authorization loss at any stage wins over a later RPC result. A bot `--after` trace is
Ready, `getMe`, `BOT_UNSUPPORTED`; it performs no chat-history request. Local selector
syntax is already validated, but remote selector calls are not made after that bot
rejection.

The command owns one absolute deadline beginning at daemon request admission. A
deadline during M2 resolution remains the accepted resolver `TIMEOUT` with
`operation:"resolve"`; it is not rewritten as a stream error or planned listen
expiry. Once activation succeeds, listen expiry is planned success and wait-for
expiry is `TIMEOUT` with `operation:"wait_for"`.

#### 4.6.3 Finite and unlimited deadlines

Unlimited is a tag, never `steady_clock::time_point::max()`:

```cpp
struct RequestDeadline {
    std::optional<Clock::time_point> expires_at; // nullopt is unlimited
};
```

`DeadlineDefault` is the closed enum `Default60` or `Unlimited`.
`request_deadline(timeout, policy, now)` returns invalid conversion or one tag.
An explicit timeout has identical meaning under both policies: finite, positive,
representable, and rounded upward to the next monotonic-clock tick. With no
timeout, Default60 yields `now + 60s`; Unlimited yields null expiry.

The exact Unlimited command set is `fetch`, `download`, `listen`, and
`wait-for`; recognizing an unregistered path does not activate it. All other
commands use Default60. The Request frame remains protocol v3 with the same
number-or-null `context.timeout` and no field/version change.

CLI parsing validates an explicitly supplied timeout before routing or framing.
No-daemon admission performs the same validation before local dispatch. Failure
is `USAGE`/exit 2 with exact details
`{"argument":"--timeout","reason":"invalid_argument"}`. Direct protocol input
retains the existing stricter framing behavior: a non-finite, non-positive or
unrepresentable non-null timeout makes the whole Request malformed, produces no
Request object, and the server sends connection-scoped `USAGE` with id 0 and
exact `{}` before EOF. It does not become a request-id command terminal.

After strict frame and routed-account acceptance, the server computes one tag at
request admission. The exact fetch handoff is:

1. config admission receives that tag;
2. successful config admission constructs `RequestSession` with the same tag;
3. Dispatcher removal recovery uses the session tag;
4. successful removal recovery is followed by logout recovery with that tag;
5. only after both succeed does the fetch handler run.

No layer recomputes or replaces the tag. No-daemon uses the same logical
sequence with its one locally computed tag. `RequestSession` stores/exposes
`RequestDeadline`, not an unconditional point. Wait helpers reached by an
unlimited command accept the tag plus a stop token. Expired means exactly
`expires_at && now >= *expires_at`; response/update events are eligible only
when observed strictly before finite expiry. Unlimited waits remain stop-aware
for disconnect, cancellation, shutdown, authorization loss and generation
replacement. APIs requiring a concrete point are extended; no max-time
sentinel, repeated long finite wait or polling substitute is allowed.

Config-admission expiry is the sole pre-RequestSession timeout for fetch and
every other routed command and has exact common details
`{"operation":"config_admission","state":null}`. After RequestSession
construction, recovery owns terminals until both preflights succeed:

- incomplete removal is `REMOVAL_INCOMPLETE`;
- invalid or unreadable removal journal/state is `AUDIT_UNAVAILABLE`;
- incomplete or unresolved logout recovery is `AUDIT_INCOMPLETE`;
- invalid or unreadable logout audit with no recognized incomplete group is
  `AUDIT_UNAVAILABLE`; and
- deadline during prior logout state observation is `AUDIT_INCOMPLETE`.

Only then do handler timeout branches apply. Before target resolution they use
the common fetch shape; after resolution phase is `local_scan`; `network_fill`
begins only after a sealed local boundary with neither latch. Cancellation never
becomes timeout and a disconnected owner receives no terminal.

#### 4.6.4 Closed public item union

`event` is the discriminator. No raw TDLib object, type ID, receive sequence, client
generation, or resume metadata is exposed.

##### 4.6.4.1 Message and edits

```json
{"event":"message","message":<MessageSummary>}
```

`MessageSummary` is exactly the shared M2 DTO.

```json
{
  "event":"edit_content",
  "chat_id":-1001,
  "message_id":123,
  "content":{"type":"text","text":"replacement"}
}
```

Content `type` and `text` use the exact M2 projection.

```json
{
  "event":"edit_metadata",
  "chat_id":-1001,
  "message_id":123,
  "edit_date":"2026-08-05T10:00:00Z",
  "has_reply_markup":false
}
```

`edit_date` is RFC 3339 UTC at second precision or null for TDLib zero. Content and
metadata edits are never merged or inferred.

##### 4.6.4.2 User-account reaction snapshot

```json
{
  "event":"reaction_snapshot",
  "chat_id":-1001,
  "message_id":123,
  "reactions":{
    "items":[{
      "reaction":{"type":"emoji","emoji":"🧪"},
      "total_count":3,
      "is_chosen":true,
      "used_sender":{"type":"user","id":42},
      "recent_senders":[{"type":"user","id":42}]
    }],
    "are_tags":false,
    "can_get_added_reactions":true
  }
}
```

`reactions` is null when interaction info or its reactions are null. This is the
current list snapshot, not a delta and not the complete interaction-info object.

##### 4.6.4.3 Bot public-reaction delta

```json
{
  "event":"bot_reaction_change",
  "chat_id":-1001,
  "message_id":123,
  "actor":{"type":"user","id":42},
  "date":"2026-08-05T10:00:00Z",
  "old_reactions":[{"type":"emoji","emoji":"👍"}],
  "new_reactions":[{"type":"emoji","emoji":"🧪"}]
}
```

This maps one `updateMessageReaction`. Arrays preserve TDLib order and are not a
whole-message count snapshot.

##### 4.6.4.4 Bot anonymous-reaction snapshot

```json
{
  "event":"bot_reaction_snapshot",
  "chat_id":-1001,
  "message_id":123,
  "date":"2026-08-05T10:00:00Z",
  "reactions":[
    {"reaction":{"type":"emoji","emoji":"🧪"},"total_count":3}
  ]
}
```

This maps one `updateMessageReactions`. It is an anonymous count snapshot and has no
actor, chosen flag, tag flag, or added-reaction capability.

All reaction items use the closed ReactionRef union:

```json
{"type":"emoji","emoji":"🧪"}
{"type":"custom","custom_emoji_id":"123456789"}
{"type":"paid"}
```

Custom IDs are canonical positive int64 decimal strings. Sender values reuse
`MessageSenderRef`. Reaction arrays preserve TDLib order. `--types reaction` selects
all three account-appropriate reaction event variants. No default bot selection is
silently weakened.

##### 4.6.4.5 Delete batch

```json
{
  "event":"delete_batch",
  "chat_id":-1001,
  "message_ids":[123,124],
  "is_permanent":true,
  "from_cache":false
}
```

One TDLib deletion update is one item and counts once. Deleted content is not
reconstructed.

##### 4.6.4.6 Closed chat-change union

```json
{"event":"chat_change","change":"new","chat":<ChatSummary>}
{"event":"chat_change","change":"identity","chat":<ChatIdentity>}
{"event":"chat_change","change":"title","chat_id":-1001,"title":"New title"}
{"event":"chat_change","change":"last_message","chat_id":-1001,"last_message":<MessageSummary|null>}
{"event":"chat_change","change":"list_added","chat_id":-1001,"list":<ChatListRef>}
{"event":"chat_change","change":"list_removed","chat_id":-1001,"list":<ChatListRef>}
{"event":"chat_change","change":"read_inbox","chat_id":-1001,"last_read_inbox_message_id":123,"unread_count":2}
{"event":"chat_change","change":"unread_mention_count","chat_id":-1001,"unread_mention_count":1}
{"event":"chat_change","change":"unread_reaction_count","chat_id":-1001,"unread_reaction_count":1}
{"event":"chat_change","change":"unread_poll_vote_count","chat_id":-1001,"unread_poll_vote_count":1}
{"event":"chat_change","change":"marked_unread","chat_id":-1001,"is_marked_unread":true}
```

ChatListRef is exactly:

```json
{"type":"main"}
{"type":"archive"}
{"type":"folder","folder_id":2}
```

Counter mapping is source-independent:

- `updateMessageMentionRead` and `updateChatUnreadMentionCount` both produce
  `unread_mention_count`.
- `updateMessageUnreadReactions` and `updateChatUnreadReactionCount` both produce
  `unread_reaction_count`.
- `updateMessageContainsUnreadPollVotes` and `updateChatUnreadPollVoteCount` both
  produce `unread_poll_vote_count`.

Each accepted raw update remains an item even when two consecutive snapshots contain
the same count. There is no undocumented coalescing. The message-level message ID is
not exposed in a chat-count item because the public item represents the resulting
ChatSummary counter.

`identity` is emitted only when folding an entity update changes the exact derived
ChatIdentity for a known non-secret chat. `updateChatTitle` uses the explicit `title`
variant and does not also emit `identity`. Photo, permissions, position, read-outbox,
draft, theme, notification, action-bar, and all other TDLib chat changes are excluded.
Secret-chat items are excluded through M6; selecting one is
`USAGE/unsupported_chat_type`.

Known malformed supported payloads terminate affected streams with `INTERNAL` before
schema-invalid stdout. Unsupported update variants are ignored because the union is
closed.

#### 4.6.5 Generation-scoped metadata bootstrap

M5 needs ChatIdentity/ChatSummary data without callback-time TDLib requests.

Each TdClient generation owns a new empty `StreamGenerationState`. Before that
generation accepts a subscription:

1. Install the metadata fold and raw-event buffer before public update publication.
2. Immediately after core-owned query id 1 `getAuthorizationState`, issue
   exactly one core-owned query id 2 `getCurrentState` for every generation,
   even when no stream observer is installed; never issue it from an update
   callback or expose it through a request-owner API. Its response is consumed
   only by that generation. A stale response from a replaced generation can
   settle only that generation's query and cannot publish into the current one.
3. While it is outstanding, normalize identity/chat deltas into a preallocated
   bootstrap buffer.
4. At the response receive-sequence barrier, fold the returned current-state updates
   as the base, then fold buffered deltas strictly after that barrier in receive order.
5. Mark the generation stream-ready and only then admit M5 subscriptions.
6. For every later receive event, fold metadata before making that same event visible
   to subscription slots.

The barrier, state-ready transition, and slot publication share the hub's generation
state machine. A subscriber receives one immutable metadata snapshot and then every
later eligible receive event; there is no gap between its installed snapshot and live
publication. Bootstrap updates themselves are state restoration, not historical
`listen` items.

Entity-to-chat mappings consume `updateUser`, `updateBasicGroup`,
`updateSupergroup`, and `updateNewChat`. Unknown entity order is tolerated by storing
the entity and chat halves until both exist. `updateNewChat` seeds ChatSummary.
Derived identity is recomputed after either half changes. State is destroyed on
client-generation replacement; no old identity can leak into a new generation.

After stream-ready, publication uses one generation-wide ordered-normalization FIFO.
Every supported update that can produce a public item is assigned its receive
sequence before any later update is considered. A complete candidate is materialized
as immutable compact JSON using the metadata state immediately after its own fold. An
`updateNewChat` whose private/basic-group/supergroup entity half is absent instead
stores the frozen chat half plus the exact missing entity key; it must not emit a
schema-incomplete `new` item.

The first incomplete candidate opens the ordering barrier. It and every later
public-capable candidate, including otherwise complete candidates for unrelated
chats, are appended in receive order. Metadata-only updates continue to fold. When a
missing entity arrives, it completes the earlier frozen `new` candidate; an
`identity` candidate caused by that entity update remains at the entity update's own
later sequence. The receive thread drains complete candidates only from the FIFO
head, offering each to subscription slots before the next. It stops at the first
still-incomplete head. Thus chat-before-entity produces `new` before the later
`identity`, and no later public item overtakes either one. Later metadata changes
never rewrite an already materialized or frozen earlier candidate.

No subscription slot is published while this FIFO is nonempty. Admission waits off
the receive thread until the barrier drains, the request deadline/authorization wins,
or the generation fails. At the first receive-loop boundary after drain, the sole
receive owner copies the current immutable metadata snapshot into the dormant slot
and `memory_order_seq_cst`-publishes that slot before accepting the next receive event. This prevents
a newly admitted subscriber from receiving a candidate whose receive sequence
predates its activation snapshot or missing the first later event.

For this internal activation step, "metadata snapshot" means the immutable activation
projection `{client_id,generation,activation_receive_sequence}`, the resolved sorted
chat-ID filter of at most 64 entries, and the staged type mask, mode, and operation. It
does not physically copy mutable `ChatSummary` title, username, or unread fields into
each slot. Workers do not consume those fields, live items are self-contained, and the
projection remains the complete observable state needed to enforce activation and
routing semantics. Its physical representation is exactly 552 bytes, including
explicit reserved padding; this is a size contract, not merely an upper bound.

The bootstrap delta buffer has exact bounds of 4,096 deltas and 16,777,216 logical
bytes. Persistent metadata has exact bounds of 65,536 chats, 131,072 entities, and a
67,108,864-byte string arena. A delta's logical byte charge is 64 bytes plus the UTF-8
length of every string copied into it; each persistent string consumes its UTF-8
length plus one terminator byte, without an alignment charge. Storage is allocated
before publication. Bootstrap delta exhaustion occurs only before stream-ready and
rejects the waiting admission. Persistent map/arena exhaustion during bootstrap does
the same; after stream-ready it marks the generation Failed, terminates active
subscriptions, and rejects later admissions with explicit `STREAM_CAPACITY`. No
metadata delta is skipped while a stream continues.

The ordered FIFO is separately preallocated for exactly 4,096 candidates and
16,777,216 candidate bytes; one candidate may consume at most 262,144 bytes. A
complete candidate is charged its compact public JSON length plus one newline. An
incomplete `new` reserves exactly 262,144 bytes until completion, then changes to the
actual compact-JSON-plus-newline charge without increasing. A completed item larger
than that reservation fails as `metadata_item_bytes`.
Crossing any of these three bounds atomically marks the generation Failed. Every
active subscription claims `STREAM_CAPACITY` with its own operation and the exact
resource/measurements from §4.6.9; if none is active, the cause is retained.
Queued but unstarted items are discarded under the ordinary terminal rule, and all
later admissions fail with that retained cause. The rejected update is therefore not
silently skipped in a continuing stream. No callback performs a TDLib call, heap
allocation, mutex acquisition, syscall, notification, or wait.

The gap-free claim is only relative to updates accepted by this one TDLib generation.
It is not a claim about Telegram delivery, daemon downtime, or earlier generations.

#### 4.6.6 Nonblocking multiplexing and bounded storage

One hub serves one TdClient generation. It has exactly 32 simultaneous M5 subscription
slots across `listen` and `wait-for`.

The receive path uses:

- a fixed array of 32 lock-free atomic raw-pointer slots;
- one receive-thread producer;
- one preallocated 256 KiB normalization scratch buffer;
- a preallocated SPSC descriptor ring and byte ring per subscription;
- lock-free fixed-width indices with compile-time and startup lock-free checks;
- one lock-free `std::atomic<uint32_t>` publisher count for deferred reclamation.

Registration reserves and initializes an unpublished slot off the receive thread.
Reservation returns one closed per-call result: an owned reservation, the exact
admission failure observed by that call, or invalid request. There is no shared
last-failure side channel between concurrent callers. Before activity promotion, a
separate lock-free activation state arbitrates promotion against terminal claim. A
terminal that wins this CAS prevents promotion. Once promotion wins, publication is
unconditional and noexcept even if a terminal cause is claimed immediately afterward;
that cause then drives ordinary detach and teardown.

The receive-loop owner fills every immutable filter and metadata-snapshot field before
publishing the slot with `slot.store(pointer, memory_order_seq_cst)` at the §4.6.5
activation boundary. No slot registry operation uses a weaker memory order.
Installation may transiently store a pointer to one stable, valid per-index marker
object in the published registry. The marker is neither a tagged pointer nor slot
storage and is never dereferenced as a published subscription. A scan that loads it
performs no enqueue. Cancellation may exchange the marker to null; installation then
rechecks the marker, terminal, generation, readiness, and state and must fail its
marker-to-slot publication after that exchange. The marker object exists for the
whole hub lifetime, so its load is safe under the same publisher guard. Tests force
marker load, marker exchange, failed publication after exchange, and successful
marker-to-slot publication.
Immediately before its first slot load, every receive callback executes
`publisher_count.fetch_add(1, memory_order_seq_cst)`, whose acquire half precedes the
load. It then scans all 32 slots with `slot.load(memory_order_seq_cst)`, applies cheap
immutable type/chat prefilters, and performs a bounded SPSC copy. An RAII guard executes
`publisher_count.fetch_sub(1, memory_order_seq_cst)`, whose release half follows the
final pointer use, including the final enqueue attempt; every exit path crosses that
guard. The pinned
TdClient receive loop has one non-reentrant publisher, so the count is exactly zero or
one. The scan has fixed maximum work; it uses no mutex, condition wait, syscall,
notification, heap, regex, TDLib request, shared_ptr refcount, or callback-time
teardown.
The same single, non-nested receive-owner guard spans generation-terminal owner loads,
dormant/published registry loads, and the final per-slot terminal write. Authorization
loss and metadata failure additionally retain one generation-wide first cause in fixed
lock-free scalar storage before that scan. Admission commit, under its control mutex,
snapshots that retained cause before arming and rechecks it after dormant installation;
a retained cause claims and removes the slot instead of publishing it. Control and
shutdown threads never enter the receive-owner guard.

The slot pointer, publisher count, descriptor producer/consumer indices, byte
producer/consumer indices, and terminal-cause atomics must each satisfy both
`is_always_lock_free` at compile time and `is_lock_free()` during hub construction.
The actual state, current-client, and current-generation atomic instances are checked
too, not substitute representative objects. Slot state is stored in the already
checked `uint32_t` representation; an invalid raw value fails closed and is removed
without introducing a callback lock or teardown.
The first failing atomic kind is reported by the exact `lock_free_ingress` schema
branch in §4.6.9. M5 fails closed at admission; it must not substitute a mutex or
lossy `try_lock`. Therefore callback lock contention is impossible in the conforming
implementation. A try-lock miss or silent callback skip is forbidden.

Each subscription queue has exact limits:

- 1,024 descriptors;
- 8,388,608 serialized data bytes;
- 262,144 bytes for one normalized item.

Byte accounting is compact item JSON plus its stdout newline. Enqueue succeeds only
when all three bounds remain valid. Producer counters use checked monotonic arithmetic;
counter exhaustion is an overflow cause.

Each published subscription has one worker and an absolute monotonic 2 ms poll
schedule. After an empty poll with no terminal, the worker advances `next_poll` by
2 ms, skips rather than replays already elapsed ticks, and calls
`std::this_thread::sleep_until(next_poll)`; a finite request deadline replaces
`next_poll` when earlier. It does not spin and has no condition-variable notifier.
Every duration conversion, skipped-tick multiplication, and time-point addition is
checked before arithmetic and saturates at the clock maximum; a schedule already at
maximum remains there without wrapping or spinning.
When not inside an already-started sink write, it reads terminal state and queue
indices on every scheduled poll. This permits at most 500 empty state probes per
second per subscription and 16,000 across 32 subscriptions; nonempty work is bounded
by admitted items. Scheduling delay is not represented as a real-time delivery
guarantee, but a runnable non-writing worker observes a claim at its first scheduled
poll after the claim. Callback code never wakes or notifies the worker.

On queue overflow, the receive thread performs one compare-exchange from Open to the
exact overflow cause and closes admission. It does not notify, unsubscribe, clear,
wait, or send. On its next poll, the worker discards unsent queued items and exchanges
its registry slot to null with `slot.exchange(nullptr, memory_order_seq_cst)`. It then polls
`publisher_count.load(memory_order_seq_cst)` off the receive path at the same 2 ms
period. Only after observing zero may it emit the overflow terminal and reclaim slot,
queue, filter, and metadata-snapshot storage. The slot exchange is also the point after
which no new callback can acquire that pointer; a callback guarded before the exchange
keeps the count nonzero until its last use. That slot cannot be reserved again
before reclamation. The control terminal has separate reserved storage and cannot be
crowded out by data.

If a reservation is destroyed, unwound, or disconnected before reclamation, ownership
of `{slot,epoch}` transfers to a fixed hub-owned retired set under the off-callback
control mutex before the reservation loses ownership. Control polling and later
reservation attempts retry reclamation. Capacity returns only after registry exchange,
publisher quiescence, borrow invalidation, poisoning, and epoch-checked reset. Hub
shutdown may synchronously sweep only after its receive producer has stopped; it never
forces a slot Free before the zero-count observation.

A worker observes a queue front only through a noncopyable, nonmovable scoped cursor.
Its dynamic-extent byte access remains behind epoch-checking borrowed-span methods; no
raw span is returned. The cursor invalidates the borrow on Keep, Consume, or exception
before advancing indices. Detach invalidates the epoch, reclamation rejects an active
borrow, and a retained cursor from an earlier epoch cannot become valid after slot
reuse.

The reclamation proof uses the single C++ sequentially consistent order. Let `P` be
slot publication, `I` the publisher-count increment, `L` a slot load, `D` the
publisher-count decrement after the reader's final pointer use, `X` the exchange to
null, and `Z` a reclaimer count load that observes zero. Program order requires
`I < L < D` for a callback and `X < Z` for a reclaimer; all six operations participate
in the same `seq_cst` order.

- Initialization of the slot object and immutable metadata snapshot is sequenced
  before `P`. If `L` reads the pointer stored by `P`, the release/acquire semantics of
  the `seq_cst` store/load publish every initialized field to the reader.
- A load of the stable Installing marker is not `P` and cannot enqueue. If `X`
  exchanges that marker to null, the later marker-to-slot compare-exchange must fail
  in the same `seq_cst` order. If it succeeds, that compare-exchange is `P`; the
  marker's stable lifetime covers every concurrent marker load.
- If `L < X`, then `I < L < X`. Because `D` is after the reader's final use, a `Z`
  ordered before `D` cannot observe a zero count contributed by that reader. The first
  admissible `Z` that observes zero is after `D`, so reclamation follows the last use.
- If `X < L`, sequential consistency makes `L` observe null: no `P` for that slot is
  allowed between `X` and reclamation. The subcase `I < X < L` merely delays
  reclamation until its null-reading callback executes `D`.

Therefore no callback can obtain the removed pointer after `X`, and every callback
that obtained it before `X` completes its final pointer and metadata-snapshot use
before `Z` permits reclamation. Reuse/publication of that slot is forbidden until
after reclamation, excluding ABA.

No silent drop, oldest-drop, sampling, coalescing, spill, or retry exists. Subscribers
have independent queues and failures. A slow subscriber cannot block another or the
TDLib receive thread.

#### 4.6.7 Filtering, ordering, and count

The logical order is:

1. normalize a supported update;
2. fold generation metadata;
3. exclude secret chats;
4. apply `--types`;
5. apply resolved chat IDs;
6. enqueue without waiting;
7. on the subscription worker, apply resolved sender;
8. apply regex;
9. check terminal/deadline state;
10. completely send and flush the item frame;
11. increment count after complete delivery;
12. after the Nth complete item, claim planned success.

Live FIFO order is TDLib receive order for one generation. Delete batches count once;
both edit variants select as `edit`; all three reaction variants select as `reaction`.
No ordering is claimed across processes or generations.

`--from` resolves once through M2. It matches only
`MessageSenderRef{"type":"user","id":resolved_id}`; chat senders do not match.

Regex is RE2 UTF-8, case-sensitive, unanchored `RE2::PartialMatch`. Pattern bytes are
valid UTF-8 and `1..4096`. Options are `log_errors=false`, `max_mem=1048576`, no ICU,
and no capture extraction. Matching uses exactly `MessageSummary.text`, including
captions and the empty string for other content. Invalid syntax or compile resource
failure is pre-activation `USAGE/invalid_argument`.

#### 4.6.8 `wait-for --after` local scan

After all setup and live-slot publication, a user-account `--after` scan uses only
these exact tuples:

```text
getChatHistory(chat_id, 0,            0, 100, true)  # first page
getChatHistory(chat_id, last_raw_id,  0, 100, true)  # following pages
```

Each following response includes its anchor when available. Remove exactly one first
element equal to `last_raw_id`; record the last newly consumed raw ID before filters.
New raw IDs must be unique and strictly decreasing. An out-of-order, duplicate, or
otherwise malformed advancing response is `PAGINATION_INVALID` with
`operation:"wait_for"`. An anchor-only, empty, or null-only local response after
anchor removal is the accepted `local_boundary`. A short successful page is not EOF;
continue from its last new raw ID. Stop after consuming the first ID `<= after`; it is
not eligible. There is no raw scan cap; deadline and explicit memory bounds remain.

Concurrent live message events enter the same bounded 1,024-item / 8 MiB wait queue.
Local candidates also count against those bounds. History scan keeps only the current
smallest eligible matching candidate plus bounded live overlap metadata; it does not
materialize an unbounded page set.

Deduplication precedes sender/regex filtering for an overlap key
`(chat_id,message_id)`. When history and buffered live DTOs differ, the history DTO
wins because it was returned by the later local query and is the current retained
snapshot; filters are then evaluated on that selected DTO. A history record checks
and marks any matching buffered live key before it can be emitted. A later live event
for an already-consumed history key is suppressed. The bounded overlap index uses the
same queue limits; exhaustion is `STREAM_OVERFLOW`, never duplicate or loss.

After the scan:

- return the smallest matching retained message ID greater than `after`, if any;
- otherwise drain unmatched/undeduplicated buffered live messages in receive order;
- then continue live until match, timeout, or another terminal.

Without `--after`, no history request occurs and eligibility begins at successful slot
publication.

The retained catch-up guarantee applies only to a matching message available in
TDLib's continuous local prefix or received in the subscription's live window. It
does not cover deleted/expired messages, an unknown local gap, daemon downtime,
messages TDLib never observed, or a prior generation. A durable local journal might
change some coverage but is neither claimed necessary nor sufficient for an absolute
Telegram-delivery guarantee.

#### 4.6.9 Errors and strict schemas

Command setup preserves accepted M1/M2 errors and their original operations. In
particular, resolver errors retain `operation:"resolve"`; M5 does not rewrite them to
`listen` or `wait_for`.

M5 adds:

```json
{
  "error":{
    "code":"STREAM_OVERFLOW",
    "message":"stream buffer capacity was exceeded",
    "details":{
      "operation":"wait_for",
      "cause":"queue_bytes",
      "limit_items":1024,
      "limit_bytes":8388608,
      "queued_items":800,
      "queued_bytes":8380000,
      "incoming_bytes":12000
    }
  }
}
```

`cause` is exactly `queue_items`, `queue_bytes`, `item_bytes`,
`history_overlap`, or `counter_exhausted`. `operation` is `listen` or `wait_for`.
All numeric values are nonnegative integers; limits are the constants above. This
error is exit 1.

```json
{
  "error":{
    "code":"STREAM_CAPACITY",
    "message":"stream service capacity is unavailable",
    "details":{
      "operation":"listen",
      "phase":"admission",
      "resource":"subscriber_slots",
      "limit":32
    }
  }
}
```

`stream.error.schema.json` defines `STREAM_CAPACITY.details` as a strict `oneOf`.
Every branch has `additionalProperties:false`, requires `operation` exactly
`listen|wait_for`, and then requires exactly the following fields and constants:

| `resource` | `phase` | exact remaining fields |
|---|---|---|
| `subscriber_slots` | `admission` | `limit:32` |
| `lock_free_ingress` | `admission` | `atomic`, exactly `slot_pointer`, `publisher_count`, `descriptor_index`, `byte_index`, or `terminal_cause`; no numeric field |
| `metadata_bootstrap_items` | `bootstrap` | `limit_items:4096`, `used_items:4096`, `incoming_items:1` |
| `metadata_bootstrap_bytes` | `bootstrap` | `limit_bytes:16777216`, `would_use_bytes` with minimum 16777217 |
| `metadata_chats` | `bootstrap\|active` | `limit:65536`, `used:65536`, `incoming:1` |
| `metadata_entities` | `bootstrap\|active` | `limit:131072`, `used:131072`, `incoming:1` |
| `metadata_bytes` | `bootstrap\|active` | `limit_bytes:67108864`, `would_use_bytes` with minimum 67108865 |
| `metadata_order_items` | `active` | `limit_items:4096`, `used_items:4096`, `incoming_items:1` |
| `metadata_order_bytes` | `active` | `limit_bytes:16777216`, `would_use_bytes` with minimum 16777217 |
| `metadata_item_bytes` | `active` | `limit_bytes:262144`, `incoming_bytes` with minimum 262145 |

For every byte/item/map occupancy branch, the reported values must prove that the
rejected insertion exceeds the stated constant. `would_use_bytes` is the authoritative
overflow-checked sum of the prior occupancy and the incoming logical byte charge; the
runtime must compute it with checked addition and must never publish a wrapped value.
`bootstrap` happens before any slot can be active and rejects the admission
that is waiting for stream-ready. `subscriber_slots` and `lock_free_ingress` reject
only the current admission. An `active` metadata failure is generation-wide: it
claims the same retained cause for every open subscription using that subscription's
operation, and a later admission receives the retained measurements with its requested
operation. First terminal cause still wins independently for a stream. This error is
exit 1.

An invalid challenge Answer received while a stream subscription is active claims the
same per-slot first-cause terminal with this exact error:

```json
{
  "error":{
    "code":"PROTOCOL_ANSWER_INVALID",
    "message":"invalid challenge answer",
    "details":{"request_id":18446744073709551615,"reason":"future_sequence"}
  }
}
```

`request_id` is an unsigned 64-bit integer. `reason` is exactly `malformed`,
`unknown_request`, `future_sequence`, `generation_mismatch`, or `nonce_mismatch`.
This cause is fixed-width and subscription-local; it is never retained as a generation
terminal for later admissions. It competes with timeout, authorization loss, shutdown,
overflow, and planned success under the same first-cause rule. The stream worker alone
serializes the winning error, with exit 2. Direct Result/Error calls cannot bypass the
stream first cause or its single terminal writer. Without an active stream, the same
invalid Answer retains its existing observable error, rejection disposition, protocol
state transition, challenge resolution, and secret-payload wipe.

Other exact branches are inherited rather than narrowed:

- `USAGE` and existing selector ambiguity/not-found shapes;
- `NOT_AUTHED` with account/state/reason;
- `BOT_UNSUPPORTED` with the operation that actually rejected the call;
- `TDLIB_ERROR`, `RATE_LIMITED`, `TIMEOUT`, and `PAGINATION_INVALID` with
  `resolve` where M2 resolution emitted them and `listen|wait_for` where the stream
  itself emitted them;
- `DAEMON_SHUTDOWN` and `INTERNAL`.

Add these self-contained strict Draft 2020-12 files:

- `listen.item.schema.json`, an exact `oneOf` over all eight event discriminators and
  the closed chat-change variants;
- `wait-for.result.schema.json`, exact M2 MessageSummary;
- `stream.error.schema.json`, including inherited valid branches and the two new
  capacity branches;
- `stream-manifest.json`, separate from the result-only manifest:

```json
{
  "schemaDialect":"https://json-schema.org/draft/2020-12/schema",
  "commands":{
    "listen":{"item":"listen.item.schema.json","error":"stream.error.schema.json"},
    "wait-for":{"result":"wait-for.result.schema.json","error":"stream.error.schema.json"}
  }
}
```

`wait-for.result.schema.json` also appears under `wait-for` in the existing
result-only manifest. `listen` has no result schema and no result-manifest entry.
Packaging and schema tests require exact stream-manifest/file bijection. The later
`schema` command must consult both catalogs; its M7 CLI presentation is intentionally
not activated here.

#### 4.6.10 Terminal, delivery, stdout, and lifecycle

Terminal handling has two levels:

1. an atomic nonblocking cause claim closes event admission;
2. one writer serializes already-started frame completion and the claimed terminal.

A cause callback only compare-exchanges state; it performs no wake or notification.
The worker observes the claim on its §4.6.6 poll schedule. The callback never waits
for the socket lock. After a terminal claim, queued frames that have not begun are
discarded. A frame whose write already began is allowed to finish before the terminal.
Beginning a frame is the writer's atomic observation that the cause is still Open; it
does not place writer progress in the terminal-cause atomic. Delivered count remains
writer-owned and may advance after an external payload is published. After a Complete
Nth delivery the writer increments count and then attempts Open-to-planned-success;
failure preserves the external winner.

Item delivery succeeds only after the complete protocol frame, including newline, is
written. Only then does count increment. A failed or partial write changes the session
to disconnected, shuts down that exact connection, cancels/unsubscribes, releases
activity, sends no terminal, and does not increment count. Terminal write failure is
also disconnect without retry.

If an external terminal is claimed while an item write is in progress, a successful
item may precede that terminal; its count increments, but the already-claimed cause
wins over count completion. If count completion claims first, later timeout/auth-loss
cannot replace its planned success. First atomic cause claim wins exactly once.

Planned `listen` completion sends internal `Result{}` for protocol/activity closure.
The client consumes it without stdout. `wait-for` success prints its MessageSummary
Result. Errors print no stdout.

For every Item in daemon and `--no-daemon` modes, the client writes the complete JSON
line, checks the write result, calls `fflush(stdout)`, and checks `ferror(stdout)`.
Failure closes/cancels the request and exits 1 with a client-local output diagnostic;
it does not fabricate a daemon terminal. Tests cover full buffering, EPIPE, short
write, and flush failure. SIGPIPE remains ignored. The in-process ResponseSink returns
delivery status so the same cancellation path applies.

Disconnect emits no observable terminal. Daemon shutdown emits one
`DAEMON_SHUTDOWN` unless another cause won. Ready-to-nonready emits one `NOT_AUTHED`
with `authorization_lost` and the first non-ready state. Unexpected TDLib Closed ends
that generation's streams; replacement does not resume them. Late old-generation,
queued, and post-terminal events are discarded.

Subscription activation is transactional:

1. reserve and initialize a dormant hub slot while the existing request activity
   token still prevents idle exit;
2. under the RequestSession lifecycle gate, verify Open and atomically arbitrate the
   prepared activation against terminal claim while promoting the request token to
   Subscription;
3. install the move-only lease in RequestSession-owned storage and unconditionally
   publish the already-initialized slot with the same lifecycle decision;
4. if Open was lost before promotion, release the dormant slot and leave normal
   terminal handling to release the request token;
5. no fallible operation, rollback, or demotion occurs after promotion and before
   publication.

Terminal/disconnect teardown removes the slot and releases the promoted token exactly
once after the final send attempt. Unlimited active subscriptions therefore keep
`subscriptions > 0` and prevent idle exit. Promotion failure, registry-capacity
failure, and setup error each have tests proving zero leaked or double-released
activity. A post-promotion terminal has separate coverage proving unconditional
publication followed by exact-once Subscription release.

There is no reconnect, automatic resubscribe, replay, resume token, public sequence,
gapless claim, or delivery acknowledgment after socket failure.

#### 4.6.11 Bot behavior

- Ready bots support `listen` and live-only `wait-for` for updates Telegram delivers
  to them.
- `--types reaction` maps bot updates to `bot_reaction_change` and
  `bot_reaction_snapshot`; it never waits for suppressed user-account interaction
  snapshots.
- `wait-for --after` is rejected before `getChatHistory` because the pinned function
  carries `CHECK_IS_USER`.
- Exact numeric and bot-capable public resolver branches remain allowed. Branches
  requiring user-only dialog lists, contacts, invite checks, Saved Messages, or other
  `CHECK_IS_USER` functions retain M2 `BOT_UNSUPPORTED` ordering.
- A global display-name `--from` that requires contacts is unsupported for bots;
  exact numeric and permitted public username/profile forms remain usable.
- No contract claims receipt of messages or reactions Telegram does not send to the
  bot.

#### 4.6.12 Persistence prohibition

M5 creates no event store, cursor/checkpoint store, wait registry, queue spill, resume
journal, filter persistence, or stream replay state. Queue, bootstrap, regex, and
overlap state are memory-only and generation-scoped. TDLib's database remains the only
message persistence. Standard test-harness evidence files are outside the product
runtime and do not alter this rule.

#### 4.6.13 RE2 dependency gate

The selected upstream is google/re2 tag `2022-12-01`, full commit
`4be240789d5b322df9f02b7e19c8651f3ccbf205`. The local official-origin checkout, the
official GitHub tag-ref/tag-object/commit API responses, and the extracted codeload
tree agree exactly: annotated tag object
`1834cd0cb196b1c6f7225df97c0550cf00f7f8e2` points to that full commit, whose Git tree
is `6cabea768fe9e69c1b7d4d410a2fdaa57057d881`. Tag and commit are unsigned; a tag/full
SHA mismatch would reject the candidate, but none exists here.

The exact runtime lock entry must use:

- source archive
  `https://codeload.github.com/google/re2/tar.gz/4be240789d5b322df9f02b7e19c8651f3ccbf205`;
- HTTP 200 `application/x-gzip`, 382,881 bytes, archive SHA-256
  `da5c23ecdb9a55c82d6802ee55812dfb99a035a4838287c0b7c0051bd0fdb9fc`;
- top-level directory `re2-4be240789d5b322df9f02b7e19c8651f3ccbf205`;
- repository-normalized extracted tree SHA-256
  `6d3942bcd96377f18ec60a7b190d1b217d037ff0132ff6ae8dc463347c067046`;
- `version:"2022-12-01"`, the full immutable ref above, `scope:"runtime"`, and
  `integration:"fetchcontent"`.

Extraction used the repository archive policy: at most 100,000 members, 256 MiB per
member, and 512 MiB expanded. It produced 128 regular files, no symlinks, no
`.gitmodules`, and no Git mode-160000 entries. Its normalized tree hash equals an
independent `git archive` export of the pinned commit, and `diff -qr` is empty.

The upstream `LICENSE` is exactly 1,558 bytes/27 lines, Git blob
`09e5ec1c74c187adc8fde6c74308c3492ef31f77`, SHA-256
`6040cda75d90b1738292a631d89934c411ef7ffd543c4d6a1b7edfc8edf29449`, SPDX
`BSD-3-Clause`. The exact bytes are the pinned archive's top-level `LICENSE` and must be copied unchanged to `release/licenses/RE2.txt`.

There is also one runtime embedded component: the Plan 9 UTF routines in
`util/rune.cc` and `util/utf.h`, by Rob Pike and Ken Thompson, Copyright (c) 2002
Lucent Technologies. `util/rune.cc` is in `RE2_SOURCES`; this is not test-only. The
notice is not asserted to match a standard SPDX identifier and must be locked as
`LicenseRef-RE2-Lucent-2002`. `release/licenses/RE2-Lucent-UTF.txt` must contain the
first 14 lines of the pinned `util/rune.cc` byte-for-byte, including comment markers
and final newline: 752 bytes, SHA-256
`8af3194d846fcddce0f5e8d4ae6c404744d9b7922a24f23415bd15a9cfe5e6ee`. The RE2 lock
entry records an embedded component with those two source paths and that notice.

The pinned CMake project has exactly `BUILD_SHARED_LIBS`, `USEPCRE`, and
`RE2_BUILD_TESTING` options; it has no `RE2_USE_ICU` CMake option. Passing
`-DRE2_USE_ICU=OFF` is therefore forbidden because CMake can ignore it while creating
false assurance. Configure with exactly `BUILD_SHARED_LIBS=OFF`,
`RE2_BUILD_TESTING=OFF`, and `USEPCRE=OFF`. The release verifier must additionally
reject `RE2_USE_ICU` in compile definitions/commands and reject ICU, PCRE, Abseil,
test, benchmark, or shared RE2 artifacts in the resolved target, link commands,
provenance, SBOM, and package. `re2/mimics_pcre.cc` is an internal RE2 implementation
source and does not link PCRE; `util/pcre.cc` is excluded with testing off.

Measured configure/build/install with those three options succeeded and produced
static `libre2.a`. CMake and the installed target expose only platform
`Threads::Threads`; no vendored/submodule dependency exists and no other runtime
transitive was found.

The external evidence gate is closed. Merge/release remains fail-closed until the
measured lock fields, both notices, `TGCLI_RE2_REV`/resolved-revision assertions,
static `re2::re2` integration, staged offline archive, and dependency/source-tree/
provenance/SBOM/notices/Linux/macOS/artifact verifiers are implemented and prove a
network-disabled configure/build/test. These are implementation prerequisites, not an
open behavioral-contract decision.

#### 4.6.14 Acceptance matrix

Fake/native tests must cover:

- parser defaults and every bound, including the 65th chat, count, timeout, after,
  types, and regex limits;
- exact Ready/getMe/bot/selector/deadline precedence and TD request traces;
- bot `--after` sends no history call;
- every strict item branch, unknown-property rejection, and malformed supported
  update termination;
- native conversion of user interaction snapshots, bot public deltas, bot anonymous
  snapshots, and all ReactionRef/sender variants;
- both message-level and chat-level sources for all three unread counters;
- generation bootstrap, entity-before-chat, chat-before-entity, frozen-candidate
  semantics, global later-item retention, head-only drain, admission during a live
  barrier, derived identity changes, generation reset, and every exact metadata
  capacity branch in bootstrap and active phases;
- fixed subscriber cap; compile-time/startup lock-free rejection for every named
  atomic; publisher acquire-before-slot/release-after-final-use; null-then-zero
  reclamation; 2 ms worker polling bounds; and no mutex/heap/syscall/notification/TD
  call from callbacks;
- queue item/byte/single-item/counter overflow for both operations, atomic first
  overflow, reserved control delivery, deferred teardown, and no silent loss;
- multi-subscriber isolation and a blocked sink not delaying receive publication;
- exact filter order, incoming/outgoing messages, batch count, edit mapping, and all
  reaction mappings;
- first and subsequent exact getChatHistory tuples, inclusive anchor removal,
  decreasing progress, short-page continuation, anchor-only/local boundary,
  malformed progress, deadline, and no network-capable history call;
- history/live dedup with different DTOs, history-wins filtering, retained ordering,
  and bounded overlap overflow;
- item-versus-count/timeout/auth-loss/shutdown/overflow races under the two-level state;
- count only after complete send, and no count/terminal after partial or failed send;
- silent listen Result in daemon/no-daemon paths;
- per-item stdout flush, EPIPE/short-write/flush failure cancellation, and no fabricated
  terminal;
- transactional activity promotion, pre-promotion cancellation, unconditional
  post-promotion publication, unlimited idle suppression, and exact-once release;
- old-generation and post-terminal rejection, no reconnect/resume claim;
- stream/result/error schema validation, strict stream catalog bijection, and packaged
  discoverability;
- strict `STREAM_CAPACITY` oneOf validation for every resource, operation, phase,
  required constant/measurement, unknown-field rejection, and retained active cause;
- RE2 tag/full-SHA equivalence, archive/tree corruption rejection, both license
  notices, offline staged build, absent ICU compile definition/linkage, absent
  PCRE/Abseil/tests/benchmarks/shared artifact, and Threads-only runtime closure;
- no product persistence or spill.

TSan fake coverage is required for slot publication, queue indices, terminal claim,
publisher reclamation, activity promotion, and generation bootstrap.
The slot test must force each `P/I/L/D/X/Z` ordering case above for every slot, assert
that no post-`X` load returns the removed pointer, poison freed slot/snapshot storage,
and stress at least 1,000,000 publish/scan/remove/reuse cycles under TSan without a
use-after-free, data race, ABA observation, or premature zero-count reclamation.

#### 4.6.15 TestDC M5 flow

Prerequisites are the isolated `TGCLI_TEST_DC=1` account, Ready user auth, implemented
M2 resolve/DTOs, implemented M3 send, and an explicit write grant. Missing M3/write
surface is a failed milestone prerequisite, not a silent skip.

The flow is:

1. Resolve the test user's Saved Messages numeric chat ID and current user ID.
2. Generate 32 lowercase hexadecimal characters from the harness CSPRNG. Define exact
   ASCII strings `tgcli-m5-anchor-<hex>` and `tgcli-m5-target-<hex>`. They contain no
   RE2 metacharacters.
3. Run `tgcli --json --allow-write --account <user> send <saved-id> <anchor>` and
   record final ID `B`.
4. Run `tgcli --json --allow-write --account <user> send <saved-id> <target>` and
   record final ID `T`; require `T > B`.
5. Run:

```text
tgcli --json --timeout 30 --account <user> wait-for \
  --chat <saved-id> \
  --from <self-user-id> \
  --after <B> \
  --regex '^tgcli-m5-target-<exact-hex>$'
```

6. Require exit 0, empty stderr, exactly one stdout object matching the result schema,
   and exact `id == T`, chat, sender, and text.

This exercises the retained subscribe-before-scan path without a readiness sleep.
Fake tests cover the exact concurrent live interleaving and listen activation.

M5 has no supported delete command, so the harness must not pretend to clean these two
messages. They remain only in the isolated, periodically wiped Telegram TestDC and its
isolated TDLib database. The harness writes a non-secret test result record containing
the account alias, chat ID, B, T, and target prefix so later cleanup tooling can locate
them once a supported delete surface exists. Test failure does not invoke raw or a
production account.

#### 4.6.16 Implementation dependency slices

Bottom-up order:

1. Add strict schemas/catalog, including retained-prefix and
   subscriber-cap corrections.
2. Land shared M2 resolver/DTO/history components.
3. Close the RE2 archive integrity gate and release dependency integration.
4. Add tagged deadlines and wait helpers.
5. Add generation metadata bootstrap and curated update normalization.
6. Add fixed-slot ingress, bounded queues, and capacity errors.
7. Add RequestSession two-level output/terminal and transactional activity activation.
8. Implement parser and handlers.
9. Add client silent Result, checked item write/flush, and cancellation propagation.
10. Add schema, fake/native, TSan, integration, TestDC, and release-provenance tests.

#### 4.6.17 Review ledger closure

| finding | closure | status |
|---|---|---|
| H1 bot reactions | Separate bot delta/snapshot event variants, default reaction selection, and native fake matrix are exact. | closed in contract |
| H2 counters/identity | A bounded generation-wide ordered-normalization FIFO freezes incomplete `new`, retains every later public candidate, drains only a complete head, blocks activation while nonempty, and fails the generation explicitly on capacity. | closed in contract |
| H3 no-block ingress | The publisher count acquires before the first slot load and releases after final use; every guard atomic is checked lock-free; null-then-zero reclamation and fixed 2 ms worker polling require no callback wake/syscall. | closed in contract |
| H4 wait buffer | Wait live, history candidate, and overlap state use the same explicit item/byte limits and `wait_for` overflow schema. | closed in contract |
| H5 setup errors | Ready/getMe/bot/resolver ordering, `resolve` error preservation, deadline precedence, and forbidden-history trace are exact. | closed in contract |
| M6 after pagination | Exact tuples, anchor removal, progress, short-page behavior, boundary, and history-wins dedup/filter order are exact. | closed in contract |
| M7 absolute guarantee | Contract is limited to retained continuous prefix plus live window and makes no journal necessity/sufficiency claim. | closed in contract |
| M8 delivery/terminal | Atomic cause plus ordered writer, complete-send counting, and disconnect on partial failure are exact. | closed in contract |
| M9 stdout flush | Per-item checked write/flush and local cancellation are required in both modes. | closed in contract |
| M10 activity | Dormant reservation, gated promotion/publication, rollback, idle suppression, and exact-once release are exact. | closed in contract |
| M11 RE2 | Official tag/full-SHA and immutable archive/tree hashes are measured; the absent ICU option, Threads-only link closure, and Lucent runtime notice are explicit. Repository lock/integration/verifier work remains a fail-closed implementation prerequisite. | evidence gate closed |
| M12 TestDC | Write prerequisites, RE2-safe nonce, exact assertions, retained-data record, and no fake cleanup are exact. | closed in contract |
| L13 schema discovery | Separate strict stream catalog is packaged/tested; no fake listen result; later schema CLI presentation remains M7. | closed in contract |

No further user-level product decision is required under the accepted no-persistence
direction. No external evidence gate remains; M5 must not be declared complete until
the fail-closed RE2 repository integration and the rest of this contract are
implemented and verified.

### 4.7 M6 regular-device session contract

This section is an additive session-only M6 contract. It preserves M0-M5 and
the M3 safety/audit semantics except for the explicit closed-enum extensions
and explicit absence of idempotency below. Every object defined here is strict:
all shown fields are required, no undeclared field is allowed, and JSON Schema
uses Draft 2020-12 with `additionalProperties:false` at every object boundary.

#### 4.7.1 Grammar, frame args and option matrix

Exact command grammar:

```text
tgcli session list
tgcli session terminate <session-id>
```

`<session-id>` is a canonical signed TDLib `int64` rendered as an ASCII decimal
string. It must:

- match `^(?:0|-?[1-9][0-9]*)$`;
- parse into `[-9223372036854775808, 9223372036854775807]`;
- contain no plus sign, whitespace or leading zero;
- reject `-0` while accepting canonical `0`.

The CLI, request frame, returned session rows, plan, confirmation target, error
details, audit arguments and human output all retain the canonical decimal
**string**. No layer converts a session id to a JSON number. This avoids
IEEE-754/int53 loss and prevents a different spelling from changing identity
or fingerprint. Zero receives no inferred meaning: it is parsed and matched
against the returned snapshot exactly like every other int64 value.

After normal v3 top-level/context validation, the closed command/args pairs are
exactly:

```text
command=["session","list"]
args={}

command=["session","terminate"]
args={"session_id":SessionId}
```

No alias, original argv spelling, numeric session id or additional args field
is accepted. Context retains the existing exact nine fields; the session id is
never duplicated in context.

Both commands accept the existing globals `--account`, `--json`, `-v`,
`--timeout` and `--no-daemon`. The ordinary-command deadline is the accepted
`request_deadline(timeout, Default60)`: absent `--timeout` means one absolute
60-second deadline beginning at daemon request admission.

Option rules are closed:

| option | `session list` | `session terminate` |
|---|---|---|
| `--allow-write` / folded request write grant | unsupported CLI mode for list | accepted for real execution; unnecessary for dry-run |
| `--yes` | `USAGE/unsupported_mode` | accepted; supplies destructive confirmation for real execution |
| `--dry-run` | `USAGE/unsupported_mode` | accepted; auth-bound read-only plan |
| `--idempotency-key` | `USAGE/unsupported_mode` | `USAGE/unsupported_mode` |
| `--cursor`, `-n`, `--full` | `USAGE/unsupported_mode` | `USAGE/unsupported_mode` |

`--yes` and a write grant may be present with `--dry-run`; they do not change
the plan and are not evaluated. Unknown positional arguments, a missing or
noncanonical id, duplicate singleton options and every unlisted command-local
option use the inherited exact `USAGE` envelope with exit 2. Invalid id uses
`{"argument":"session-id","reason":"invalid_argument"}`; unsupported flags
use the offending flag as `argument` and `reason:"unsupported_mode"`.

The request operation enum adds exactly `session_list` and
`session_terminate`. The safety function enum adds exactly
`getActiveSessions` and `terminateSession`. `session_list` declares `Read`;
`session_terminate` declares `Destructive`. There is no `session` fallback or
prefix dispatch.

#### 4.7.2 TDLib boundary and device enum

The native typed boundary adds exactly:

```text
TdFunctionKind::GetActiveSessions -> td_api::getActiveSessions
TdFunctionKind::TerminateSession  -> td_api::terminateSession(session_id:int64)
```

The test descriptor for `TerminateSession` contains one field named
`session_id` with signed int64 value. The list function has no fields. The
factory, native matcher, type-erased response converter, scripted fake and
daemon safety allowlist must agree on both kinds; a missing arm fails build or
test rather than falling through to `other`.

The curated `SessionDeviceType` enum is exactly these 17 lowercase values,
mapped one-to-one from the pinned generated variants:

```text
android, apple, brave, chrome, edge, firefox, ipad, iphone, linux, mac,
opera, safari, ubuntu, unknown, vivaldi, windows, xbox
```

`sessionDeviceTypeUnknown` is a supported value and maps to `"unknown"`.
An unrecognized future variant is not silently folded to `"unknown"`; it is
an incompatible TDLib boundary response and returns `INTERNAL` with
`reason:"malformed_tdlib_response"` and the observed `tdlib_type_id`.

The converter requires a non-null `td_api::sessions`, every element non-null,
`inactive_session_ttl_days` in 1..366, an int64 session id, a non-null known
device type, and dates in the allowed representation below. Zero is a valid
int64 row id and is not rejected or rewritten. A boundary violation emits no
partial result. No raw TD object or TD error text crosses the boundary.

#### 4.7.3 Strict DTOs

`Session` has exactly:

```text
id: SessionId
is_current: boolean
is_password_pending: boolean
is_unconfirmed: boolean
can_accept_secret_chats: boolean
can_accept_calls: boolean
device_type: SessionDeviceType
api_id: signed int32 JSON integer
application_name: string
application_version: string
is_official_application: boolean
device_model: string
platform: string
system_version: string
log_in_date: TimestampOrNull
last_active_date: TimestampOrNull
ip_address: string
location: string
```

`SessionId` is the canonical decimal string from §4.7.1, including `"0"`.
`TimestampOrNull` is either an exact UTC RFC3339 second string
`YYYY-MM-DDTHH:MM:SSZ` for a positive TD Unix timestamp in 1..2147483647, or
JSON null when the TD value is zero. A negative timestamp is a malformed TDLib
response. Strings are copied byte-for-byte after the project's normal UTF-8
JSON validation; empty TD strings remain empty and are never inferred.

`SessionListResult` is exactly:

```text
{
  "items": Session[],
  "inactive_session_ttl_days": integer 1..366,
  "next": null
}
```

Items remain in the exact TDLib-returned order. tgcli does not sort, group,
deduplicate or move the current session. Duplicate ids are a malformed TDLib
response rather than a lossy deduplication opportunity. This list is finite
and unpaginated; `next` is always null solely to retain the general list-result
shape.

`SessionTerminateTarget` has exactly:

```text
{
  "id": SessionId,
  "is_current": false,
  "is_password_pending": boolean,
  "is_unconfirmed": boolean,
  "device_type": SessionDeviceType,
  "application_name": string,
  "application_version": string,
  "device_model": string,
  "platform": string,
  "system_version": string,
  "last_active_date": TimestampOrNull
}
```

The immutable destructive plan is exactly:

```text
{
  "operation": "session_terminate",
  "account": string,
  "tdlib_request": "terminateSession",
  "session": SessionTerminateTarget
}
```

The plan intentionally excludes `api_id`, IP address, location, login time,
call/secret-chat capability and official-app status. They are returned by
`session list` but are not needed to identify, confirm, audit or recover a
termination. Confirmation and audit use the same immutable plan, with no
second resolution and no additional personal-location data.

Dry-run success is exactly:

```text
{"dry_run":true,"plan":<the exact SessionTerminatePlan>}
```

Real termination success is exactly:

```text
{"session_id":SessionId,"terminated":true}
```

A correlated public TDLib `Ok` maps to this result and to audit mutation state
`confirmed` under this contract. Here `confirmed` means durable proof of the
public TDLib acceptance terminal, not separately observed deletion. The pinned
implementation maps the server boolean false case to public `Ok`; therefore
the result does not prove that the target existed at dispatch, that this
invocation caused its disappearance, that it remains absent, that a device
received a push, or that local processes/cache closed. No post-dispatch
`getActiveSessions` reread is made. Such a reread could race and would not
recover causation or idempotence.

#### 4.7.4 User, bot, connected-bot and target semantics

Both operations require a user principal. Their common preflight is Ready,
then correlated `getMe`, then principal binding and bot classification.
A bot principal returns:

```text
BOT_UNSUPPORTED / exit 2
details = {"operation":"session_list"|"session_terminate"}
message = "session commands require a user account"
```

This error occurs before `getActiveSessions` or `terminateSession`, preserving
the existing rule that tgcli never calls a TDLib `CHECK_IS_USER` request for a
bot.

`getActiveSessions` returns regular device sessions. tgcli does not call
`getBusinessConnectedBot`, does not synthesize a connected-bot row, and does
not accept a bot user id as a session id. A business connected bot is outside
scope rather than an `unknown` device.

`session terminate` resolves the canonical id, including `"0"`, against one
exact `getActiveSessions` snapshot. Zero matches returns:

```text
NOT_FOUND / exit 4
details = {"operation":"session_terminate","session_id":SessionId}
message = "session not found"
```

More than one match is a malformed TDLib response. If the single matched row
has `is_current:true`, the command returns before confirmation/audit/dispatch:

```text
PRECONDITION_FAILED / exit 1
details = {
  "operation":"session_terminate",
  "session_id":SessionId,
  "reason":"current_session"
}
message = "the current session cannot be terminated; use tgcli logout"
```

No rule equates id zero, or any other numeric value, with the current session;
only the pinned returned row's `is_current` flag decides this precondition.
tgcli never translates current-session termination into `logout`, never calls
`terminateSession` for it and never creates a new auth-closing lifecycle.
`is_unconfirmed` and `is_password_pending` do not prevent termination of a
non-current session; the flags are shown in the target so the user can make an
informed confirmation. The command does not confirm a session.

After the immutable snapshot, the target can disappear or change concurrently.
tgcli neither re-resolves before dispatch nor rereads afterward. A correlated
public `Ok`, including the pinned server-false mapping, follows the acceptance
semantics in §4.7.3; it is not an observable claim of non-racing causation or
idempotence. A correlated public TD error remains an error and is never guessed
to mean an already-absent success.

#### 4.7.5 Admission, recovery preflight, dry-run and destructive order

`session list` and `session terminate --dry-run` inspect and reconcile prior
incomplete v2 audit groups under the same absolute deadline before their Ready
admission. A real new `session terminate` follows the accepted M3 position
instead: Ready/getMe/bot/authority steps precede inspection/reconciliation at
its listed step 6, before target resolution or confirmation. If destructive
write authority denies at step 5, the request returns without reconciliation
and cannot append a prior-group record. Reconciliation on an eligible path may
append/sync recovery records for prior invocations; those records belong to the
prior invocation. List and dry-run create no audit group for their current
invocation.

Session paths pass an explicit `AbsentByPolicy` audit-pin source; this is not
an empty pin set and the audit layer never opens `idempotency.db` itself. They
may reconcile only audit-only groups with null idempotency hash, no pending
stage, and no spool stage. An incomplete keyed group returns
AUDIT_INCOMPLETE without append or store/spool I/O. List and dry-run perform no
capacity operation. A real terminate that does not need rotation may append;
when rotation is needed it may use a missing numbered slot without deletion,
but it returns `AUDIT_UNAVAILABLE/capacity_exhausted` rather than delete an
occupied slot without pin knowledge. Thus no session path reads or writes the
idempotency store, evicts a possibly pinned M3 segment, or resends.

Every §4.7.5 reconciliation point first applies §4.5.12's account-global v2
gate. `session list` and `session terminate --dry-run` apply it at step 2
before Ready; real `session terminate` applies it at step 6 after
Ready/getMe/bot/authority. An unsafe/unreadable spool root returns the exact
account-global `SPOOL_UNAVAILABLE`; a retained spool contradiction returns
the object-path `AUDIT_INCOMPLETE`. Neither branch appends a current or prior
record. Session list and terminate dry-run are blocked because they can
reconcile v2; they are not members of the seventeen persistence-free §4.5
planner dry-runs.

`session list` runs this exact logical order:

1. strict CLI/frame/config parse and frozen account match;
2. inspect/reconcile prior incomplete v2 audit groups;
3. Ready admission;
4. correlated `getMe`, principal binding and bot classification;
5. bot preflight;
6. correlated `getActiveSessions` under the one request deadline;
7. all-or-nothing strict conversion;
8. one result terminal.

`session terminate --dry-run` is the accepted M3/M4 auth-bound dry-run: an
absent daemon may be spawned, while `--no-daemon` creates an isolated
in-process client. Its exact order is:

1. strict parse, frozen account match and id canonicalization;
2. inspect/reconcile prior incomplete v2 audit groups;
3. Ready, correlated `getMe`, principal binding and bot preflight;
4. correlated `getActiveSessions` and exact unique target resolution;
5. current-session rejection and immutable plan construction;
6. return the dry-run result.

Dry-run needs no write grant or confirmation, creates no current-invocation
general audit group or idempotency entry, and calls no destructive TD function.
Prior-group reconciliation is the sole permitted audit write on this path. It
adds `getActiveSessions` to the closed M3/M4 dry-run read allowlist only for the
`session_terminate` planner and performs no config mutation.

A real new `session terminate` invocation extends the accepted M3 admission
order as follows:

1. strict CLI/frame/config parse and canonical session id;
2. frozen account match and Ready snapshot;
3. correlated `getMe`, principal binding and bot classification;
4. bot preflight;
5. destructive write authority;
6. inspect/reconcile prior incomplete v2 audit groups and run the
   capacity-readable preflight;
7. M3 caller-input canonicalization and request fingerprint;
8. correlated `getActiveSessions` and exact unique target resolution;
9. reject a target whose matched row is current; build immutable strict plan;
10. obtain destructive confirmation;
11. fresh config-grant CAS if authority source is config;
12. durably append and sync current v2 audit intent;
13. durably append and sync `dispatch_started`;
14. send exactly one `terminateSession(plan.session.id)`;
15. if correlated public `Ok` wins, durably append and sync
    `mutation_confirmed` with exact success terminal;
16. durably append and sync audit outcome;
17. emit exactly one terminal frame.

There is no post-dispatch session reread and no idempotency lookup, insert,
replay, reservation, store transition, spool or cleanup step. Before
`dispatch_started`, mutation state is `none` and the outcome is `not_started`.
After durable dispatch and before a correlated public terminal, mutation state
is `possible`; tgcli never resends during recovery. Public `Ok` is recorded as
the contract's `confirmed` acceptance terminal with the limits in §4.7.3.

The confirmation action enum adds `session_terminate`. The challenge target is
the exact immutable plan. Without a TTY, `--yes` is required. With a TTY, the
exact prompt is:

```text
terminate session <id>: application=<JSON string> version=<JSON string> device=<device_type> model=<JSON string> platform=<JSON string> last_active=<JSON TimestampOrNull>? [y/N]
```

Only the accepted exact affirmative answer confirms. Cancellation, empty or
malformed input, disconnect and deadline do not. The inherited
`CONFIRMATION_REQUIRED` error contains action `session_terminate` and the exact
plan as target. Authority denial returns at step 5 before reconciliation, so
it creates no current-invocation intent and cannot append a prior-group recovery
record. Resolution failure, current-session rejection, failed CAS and
unconfirmed requests occur after step 6: prior-group reconciliation may already
have appended its required records, but these failures create no
current-invocation audit intent.

#### 4.7.6 Audit v2 extension, without idempotency

The M3 closed `operation` and v2 `command` enums add
`session_terminate`; the v2 `tdlib_function` enum adds
`terminateSession`. The v2 intent is unchanged structurally and has:

```text
command = "session_terminate"
arguments = {"session_id":SessionId}
plan = SessionTerminatePlan
confirmation_source = "yes"|"tty"
idempotency_key_hash = null
```

`arguments.session_id` accepts the full canonical signed-int64 string domain,
including `"0"`. `request_fingerprint` uses the accepted M3 domain-separated
canonical serialization with operation `session_terminate` and exact
fingerprint input:

```text
session_terminate:
  session_id: SessionId
```

It excludes timeout, JSON/human mode, tty state, `--yes`, authority source,
resolved descriptive fields and transport ids. The fingerprint remains in
the audit intent for integrity/correlation even though no idempotency feature
is exposed.

The only legal stage sequences are:

```text
no dispatch: []
direct:      dispatch_started, mutation_confirmed?
```

There is no `idempotency_pending`. `dispatch_started.data` is the accepted M3
object with `tdlib_function:"terminateSession"`; `mutation_confirmed.terminal`
is the exact stored success terminal. An `Ok` terminal is the accepted TDLib
acceptance evidence defined in §4.7.3, not an independently reread deletion
proof. Outcome and `StoredTerminal` retain the accepted v2 shapes.

Recovery is fail-closed:

- intent without `dispatch_started` gets a durable failure outcome with
  mutation `none`;
- `mutation_confirmed` without outcome reconstructs the byte-equivalent
  success outcome;
- dispatch without proof gets `AUDIT_INCOMPLETE`, mutation `possible`, and no
  automatic resend;
- contradiction, changed plan/fingerprint, illegal stage or conflicting
  terminal remains `AUDIT_INCOMPLETE`.

Audit outcome sync precedes the terminal frame. Failure to append/sync intent
returns `AUDIT_UNAVAILABLE` before dispatch. Failure after intent closes the
connection without an unaudited terminal and enters the accepted audit-fatal
path. Per-account audit inspection includes `session list` and
`session terminate`; each path performs the exact prior-group reconciliation
ordered in §4.7.5.

#### 4.7.7 Deadline, auth-loss and TD response arbitration

All TD requests use the accepted `(client_id,query_id)` correlation and one
absolute request deadline. Event eligibility and terminal arbitration are
identical to §4.5.9: an event is eligible only when observed monotonic time is
strictly `< deadline`; equality belongs to the deadline; events from an old
client generation are discarded.

For `getMe`, `getActiveSessions` and the direct `terminateSession` RPC, a
one-shot terminal claim chooses the earliest eligible correlated response,
first non-Ready authorization event or deadline:

- earlier correlated success continues or yields the public acceptance
  terminal described in §4.7.3;
- earlier TD error returns the mapped TD error;
- earlier non-Ready returns `NOT_AUTHED` with
  `reason:"authorization_lost"` and that event's exact auth snapshot;
- deadline before either returns `TIMEOUT`;
- late events only release correlation and never rewrite terminal/audit state.

For list and terminate pre-dispatch, auth loss or deadline makes no Telegram
mutation. After durable termination dispatch, TD error, auth loss and deadline
all leave mutation `possible` unless a correlated public `Ok` won first. A
public `Ok` produces the contract's `confirmed` acceptance terminal; no
post-dispatch reread occurs.

`session list` TIMEOUT details are exactly the accepted read shape:

```text
{"operation":"session_list","state":AuthStateOrNull}
```

`session terminate` TIMEOUT details add exactly these two direct branches to
the accepted M3 strict oneOf:

```text
preflight:
{"operation":"session_terminate","phase":"preflight",
 "state":AuthStateOrNull,"outcome":"not_started",
 "idempotency":"not_requested"}

dispatch:
{"operation":"session_terminate","phase":"dispatch",
 "state":AuthStateOrNull,"outcome":"unknown",
 "idempotency":"not_requested"}
```

The dispatch branch is impossible before synced `dispatch_started`; the
preflight branch is impossible afterward. No branch permits `pending`,
`not_created`, `removed` or `completed_unchanged` because the command has no
idempotency contract.

#### 4.7.8 Strict error surface

Every failure uses the standard single stderr envelope and one LF. Messages
are explanatory only; callers branch on code, exit and strict details.
No error includes a raw TD message, request object, IP, location or raw argv.

The session-specific exact detail shapes are:

| code / exit | exact details |
|---|---|
| `BOT_UNSUPPORTED` / 2 | `{"operation":"session_list"|"session_terminate"}` |
| `NOT_FOUND` / 4 | `{"operation":"session_terminate","session_id":SessionId}` |
| `PRECONDITION_FAILED` / 1 | `{"operation":"session_terminate","session_id":SessionId,"reason":"current_session"}` |
| `TDLIB_ERROR` / 1 | `{"operation":"session_list"|"session_terminate","tdlib_code":integer}` |
| `RATE_LIMITED` / 5 | `{"operation":"session_list"|"session_terminate","tdlib_code":429,"retry_after":nonnegative_integer}` |
| `INTERNAL` / 1 | `{"operation":"session_list"|"session_terminate","reason":"malformed_tdlib_response","tdlib_type_id":integer|null}` |

The account-global v2 spool gate adds exactly two M6-reachable branches.

`SPOOL_UNAVAILABLE`, exit 1, has stable message
`attachment spool is unavailable` and exact details:

```json
{"operation":"session_list","path":"spool/","reason":"wrong_mode"}
```

`operation` is exactly `session_list|session_terminate`; `path` is the literal
redacted token `spool/`, not a filesystem pathname; `reason` is exactly
`path_invalid|wrong_type|wrong_owner|wrong_mode|open_failed|read_failed`.

The spool-contradiction `AUDIT_INCOMPLETE`, exit 1, has stable message
`attachment spool recovery is incomplete` and exact details:

```json
{"account":"main",
 "path":{"kind":"bytes_hex","value":"2f73746174652f73706f6f6c2fff"},
 "mutation_state":"none","completed_stages":[]}
```

The path object has exactly `kind` and `value`; `kind` is `bytes_hex` and
`value` is the complete absolute retained pathname encoded as two lowercase
hexadecimal digits per raw non-NUL byte. It has even length at least 4,
begins `2f`, and contains no `00` byte pair. Every spool contradiction uses
this object even when its raw bytes are valid UTF-8. Existing audit-record
`AUDIT_INCOMPLETE` branches keep their absolute string `path`, existing
messages and legal history vectors.

Root classification precedes contradiction selection. Missing `spool` is
safe and creates nothing. Account-state/root symlink, entry/FD replacement or
disappearance maps to `SPOOL_UNAVAILABLE/path_invalid`; a non-directory root
maps to `wrong_type`; wrong uid to `wrong_owner`; non-0700 mode to
`wrong_mode`; root/account-state open, reopen or fstat I/O to `open_failed`;
root enumeration or entry-metadata I/O to `read_failed`. These cases retain
and never repair the unsafe object and cannot safely select an orphan.

Only after complete safe-root enumeration do an invalid raw invocation name,
unexpected entry type/owner/mode, an invocation without a matching valid v2
`saved_attach` intent, an unexpected invocation child, or an audit/store
filename mismatch become object-path `AUDIT_INCOMPLETE`. Raw entry names are
sorted lexicographically by unsigned bytes, shorter equal-prefix first, before
the first contradiction's complete path is encoded. A missing spool required
by an existing audit/store relation has no retained object and therefore uses
the existing string audit-path `AUDIT_INCOMPLETE`, not the object branch.

Neither new error appends/reconciles a record or creates current-invocation
state. For `session list` and `session terminate --dry-run`, the gate runs at
§4.7.5 step 2 before Ready. For real `session terminate`, it runs at step 6
after Ready/getMe/bot/authority. Root failure wins before object contradiction;
object contradiction wins before prior-group reconciliation. The exact
effect-based admitted/blocked operation set is §4.5.12's v2 gate. No M6
command can emit either branch with an M3/M4 operation name or an uncataloged
details shape.

Code 429 uses the accepted positive flood-wait parser and mathematical ceiling
to seconds; if the TD error has no valid positive wait, `retry_after` is 0.
Every other TD error remains `TDLIB_ERROR`; in particular, terminate errors
are not guessed to mean already absent and are not mapped to success. Standard
M1/M3 `USAGE`, `ACCOUNT_NOT_FOUND`, `ACCOUNT_MISMATCH`, `CONFIG_INVALID`,
`CONFIG_CONFLICT`, `HOOK_FAILED`, `NOT_AUTHED`, `WRITE_DENIED`,
`CONFIRMATION_REQUIRED`, `AUDIT_UNAVAILABLE`, `AUDIT_INCOMPLETE`,
`PROTOCOL_ANSWER_INVALID`, daemon and shutdown errors retain their existing
detail shapes and closed nested enums.

#### 4.7.9 Self-contained JSON Schemas and catalogs

M6 adds these packaged Draft 2020-12 schemas:

```text
docs/schemas/session-list.result.schema.json
docs/schemas/session-terminate.result.schema.json
docs/schemas/session.error.schema.json
```

`docs/schemas/manifest.json` remains result-only and adds exactly:

```json
{
  "session list": {
    "result": "session-list.result.schema.json"
  },
  "session terminate": {
    "result": "session-terminate.result.schema.json"
  }
}
```

as entries within the existing `commands` map, preserving that manifest's
repository-required ordering. The entries are a result catalog, not activation
of the M7 `schema` CLI. `session.error.schema.json` is packaged and tested but
has no entry in the result-only manifest.

Each of the three files is self-contained: it has no external `$ref`, `$id` or
`format`, defines every reused type under its own local `$defs`, and carries
all range/calendar restrictions itself. Passing a separate C++ semantic check
does not make a value schema-valid.

`session.error.schema.json` must add these self-contained definitions under
`$defs`; every existing definition remains unchanged:

```json
"filesystemDiagnosticPath": {
  "type": "object",
  "additionalProperties": false,
  "required": ["kind", "value"],
  "properties": {
    "kind": { "const": "bytes_hex" },
    "value": {
      "type": "string",
      "pattern": "^2f(?:0[1-9a-f]|[1-9a-f][0-9a-f])+$"
    }
  }
},
"spoolUnavailableError": {
  "type": "object",
  "additionalProperties": false,
  "required": ["code", "message", "details"],
  "properties": {
    "code": { "const": "SPOOL_UNAVAILABLE" },
    "message": { "const": "attachment spool is unavailable" },
    "details": {
      "type": "object",
      "additionalProperties": false,
      "required": ["operation", "path", "reason"],
      "properties": {
        "operation": { "enum": ["session_list", "session_terminate"] },
        "path": { "const": "spool/" },
        "reason": {
          "enum": [
            "path_invalid",
            "wrong_type",
            "wrong_owner",
            "wrong_mode",
            "open_failed",
            "read_failed"
          ]
        }
      }
    }
  }
},
"spoolAuditIncompleteError": {
  "type": "object",
  "additionalProperties": false,
  "required": ["code", "message", "details"],
  "properties": {
    "code": { "const": "AUDIT_INCOMPLETE" },
    "message": { "const": "attachment spool recovery is incomplete" },
    "details": {
      "type": "object",
      "additionalProperties": false,
      "required": ["account", "path", "mutation_state", "completed_stages"],
      "properties": {
        "account": { "$ref": "#/$defs/account" },
        "path": { "$ref": "#/$defs/filesystemDiagnosticPath" },
        "mutation_state": { "const": "none" },
        "completed_stages": { "const": [] }
      }
    }
  }
}
```

The two new error references are adjacent to the existing audit definitions
and are exactly:

```json
{ "$ref": "#/$defs/spoolUnavailableError" },
{ "$ref": "#/$defs/spoolAuditIncompleteError" }
```

The resulting top-level `error.oneOf` is authoritative and has exactly these
24 references in this order:

```text
#/$defs/usageError
#/$defs/accountNotFoundError
#/$defs/accountMismatchError
#/$defs/configInvalidError
#/$defs/configConflictError
#/$defs/hookFailedError
#/$defs/notAuthedError
#/$defs/writeDeniedError
#/$defs/confirmationRequiredError
#/$defs/auditUnavailableError
#/$defs/spoolUnavailableError
#/$defs/auditIncompleteError
#/$defs/spoolAuditIncompleteError
#/$defs/protocolAnswerInvalidError
#/$defs/daemonNotRunningError
#/$defs/daemonControlFailedError
#/$defs/daemonShutdownError
#/$defs/botUnsupportedError
#/$defs/notFoundError
#/$defs/preconditionFailedError
#/$defs/tdlibError
#/$defs/rateLimitedError
#/$defs/internalError
#/$defs/timeoutError
```

This authoritative common-branch list preserves every existing common/session
branch and adds only `spoolUnavailableError` and
`spoolAuditIncompleteError`. `SPOOL_UNAVAILABLE` is represented only by
`spoolUnavailableError`. `AUDIT_INCOMPLETE` intentionally has two disjoint
schema definitions: `auditIncompleteError` retains every legacy/current audit
branch whose `details.path` is a string and whose history vector is one of its
existing six strict `oneOf` cases; `spoolAuditIncompleteError` is only the
retained-spool contradiction branch whose `details.path` is the strict
reversible `FilesystemDiagnosticPath` object, `mutation_state` is `none`, and
`completed_stages` is empty. Neither definition widens the other.

Neither error definition is added to the result-only manifest. The error
schema remains self-contained Draft 2020-12, has
`additionalProperties: false` at every new object boundary and keeps every
existing session error branch. No M6 command may return an uncataloged error
branch.

Schema tests must accept every cross-product of the two permitted operations
and six `SPOOL_UNAVAILABLE` reasons, an object-path contradiction containing an
invalid-UTF-8 byte such as
`{"kind":"bytes_hex","value":"2f73746174652f73706f6f6c2fff"}`,
and the existing legacy string-path `AUDIT_INCOMPLETE` histories. They must
assert the exact 24 top-level references and order, the exact diagnostic-path
pattern above, and the unchanged six-case
`auditIncompleteError.details.oneOf`.

Negative schema tests must reject an unknown or M3/M4 operation, any
nonliteral, absolute-string or typed-object `SPOOL_UNAVAILABLE` path, unknown
and non-root reasons including `sync_failed`, `capacity_exhausted` and
`contradiction`, wrong messages, missing or extra fields, and unknown error
codes. For the object-path `AUDIT_INCOMPLETE` branch they must also reject a
kind other than `bytes_hex`, missing or extra object fields, uppercase, odd,
non-hex, non-absolute or NUL-containing encodings, any mutation state other
than `none`, and any nonempty completed-stage list.

The local `sessionId` definition in every applicable schema is exactly this
full signed-int64 string language:

```json
{
  "oneOf": [
    { "const": "0" },
    {
      "type": "string",
      "pattern": "^(?:[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[01][0-9]{17}|92[01][0-9]{16}|922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|922337[01][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|9223372036854775[0-7][0-9]{2}|922337203685477580[0-7])$"
    },
    {
      "type": "string",
      "pattern": "^-(?:[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[01][0-9]{17}|92[01][0-9]{16}|922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|922337[01][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|9223372036854775[0-7][0-9]{2}|922337203685477580[0-8])$"
    }
  ]
}
```

This accepts `-9223372036854775808`, `0` and `9223372036854775807`; it rejects
both adjacent overflows, `-0`, plus signs and leading zeros without an external
range checker.

The local `timestampOrNull` definition is exactly null for TD timestamp zero
or a calendar-valid UTC second in Unix range 1..2147483647. Its string branch
is self-contained as follows (`T` and `Z` are literal):

```json
{
  "oneOf": [
    { "type": "null" },
    {
      "type": "string",
      "not": { "const": "1970-01-01T00:00:00Z" },
      "oneOf": [
        {
          "pattern": "^(?:19(?:7[0-9]|8[0-9]|9[0-9])|20(?:0[0-9]|1[0-9]|2[0-9]|3[0-7]))-(?:(?:0[13578]|1[02])-(?:0[1-9]|[12][0-9]|3[01])|(?:0[469]|11)-(?:0[1-9]|[12][0-9]|30)|02-(?:0[1-9]|1[0-9]|2[0-8]))T(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z$"
        },
        {
          "pattern": "^(?:19(?:7[26]|8[048]|9[26])|20(?:0[048]|1[26]|2[048]|3[26]))-02-29T(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z$"
        },
        {
          "pattern": "^(?:2038-01-(?:0[1-9]|1[0-8])T(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]|2038-01-19T(?:0[0-2]:[0-5][0-9]:[0-5][0-9]|03:(?:(?:0[0-9]|1[0-3]):[0-5][0-9]|14:0[0-7])))Z$"
        }
      ]
    }
  ]
}
```

`session-list.result.schema.json` has an exact strict root requiring only
`items,inactive_session_ttl_days,next`; `items` contains strict Session objects
with all 18 fields in §4.7.3, local `sessionId`, the exact 17-value device enum,
signed-int32 `api_id`, booleans/strings as specified, local `timestampOrNull`,
TTL 1..366 and `next` const null.

There is one cataloged `session-terminate.result.schema.json`. Its strict root
is an exact `oneOf` with no wrapper widening:

```text
real branch:
  required=[session_id,terminated], additionalProperties=false,
  session_id=local sessionId, terminated=const true

dry-run branch:
  required=[dry_run,plan], additionalProperties=false,
  dry_run=const true, plan=exact strict SessionTerminatePlan
```

The local plan/target definitions include every field and constraint in
§4.7.3, including local sessionId and timestampOrNull. There is no second
dry-run schema or uncataloged arbitrary-object escape hatch.

`session.error.schema.json` is the exact named self-contained envelope for
both session commands. Its root requires only `error`; `error` is a strict
`oneOf` of:

1. locally copied common branches reachable by this namespace:
   `USAGE`, `ACCOUNT_NOT_FOUND`, `ACCOUNT_MISMATCH`, `CONFIG_INVALID`,
   `CONFIG_CONFLICT`, `HOOK_FAILED`, `NOT_AUTHED`, `WRITE_DENIED`,
   `CONFIRMATION_REQUIRED`, `AUDIT_UNAVAILABLE`, `AUDIT_INCOMPLETE`,
   `PROTOCOL_ANSWER_INVALID`, `DAEMON_NOT_RUNNING`, `DAEMON_CONTROL_FAILED`
   and `DAEMON_SHUTDOWN`; each branch copies the accepted exact detail shape,
   closed enum and exit-independent envelope constraints into local `$defs`;
2. the six session-specific branches in §4.7.8, using local `sessionId` and
   exact operation enums;
3. exactly these TIMEOUT detail branches:

```text
session_list:
  {"operation":"session_list","state":AuthStateOrNull}
session_terminate preflight:
  {"operation":"session_terminate","phase":"preflight",
   "state":AuthStateOrNull,"outcome":"not_started",
   "idempotency":"not_requested"}
session_terminate dispatch:
  {"operation":"session_terminate","phase":"dispatch",
   "state":AuthStateOrNull,"outcome":"unknown",
   "idempotency":"not_requested"}
```

Every error branch requires exactly `code,message,details`; code is const,
message is a string except where §4.7.4 fixes a const message, and details is
the exact strict branch object. `CONFIRMATION_REQUIRED.target` locally embeds
the exact SessionTerminatePlan. `AuthStateOrNull`, audit stages, account names,
reason enums and all other referenced types are local definitions; the schema
has no reference to another packaged file and, because its current rules are
fully asserted by the standard schema, has no semantic-validation marker.

Manifest/package/schema tests require file/entry bijection for results,
installed presence of all three files and direct Draft 2020-12 rejection of
undeclared/missing fields, JSON-number ids, `-0`, both int64 overflows, unknown
device types, 1970 zero-string, non-leap February 29, invalid month/day/time and
timestamps after `2038-01-19T03:14:07Z`. They accept all three int64 boundary
values and both timestamp boundaries (`1970-01-01T00:00:01Z`,
`2038-01-19T03:14:07Z`) using the packaged schema alone.

#### 4.7.10 Exact human output

Human output is derived only from the curated DTO and preserves the same
information as JSON. It is deterministic and ends with one LF.

`session list` emits one TSV header, one row per item in TD order, then the two
footer rows:

```text
id\tcurrent\tpassword_pending\tunconfirmed\tdevice\tapi_id\tapplication\tapplication_version\tofficial\tdevice_model\tplatform\tsystem_version\tlogin\tlast_active\tip\tlocation\taccept_secret_chats\taccept_calls
<one row per Session>
inactive_session_ttl_days\t<N>
next\tnull
```

Id and integer cells are minimal decimal, including literal `0`. Boolean cells
are `true|false`. Every string cell is a compact JSON string token, including
quotes and JSON escaping, so tabs/newlines cannot create extra columns.
Timestamp cells are a compact JSON string token or literal `null`. An empty
list still prints the header and both footer rows.

Real `session terminate` success is exactly:

```text
session_id\t<SessionId>
terminated\ttrue
```

Dry-run human success is exactly:

```text
dry_run\ttrue
plan\t<canonical compact JSON SessionTerminatePlan>
```

Canonical compact JSON preserves the plan key order shown in §4.7.3 and uses
the project's JSON escaping. stdout contains no warning or progress line;
stderr is empty on success. `--json` prints one compact exact result object
and LF; errors print no stdout.

#### 4.7.11 Tests and seams

Implementation is not complete until the following focused coverage exists.

Core/native tests:

- both new `TdFunctionKind` names, factory descriptors and native matchers;
- exact `terminateSession` signed-int64 argument, including INT64_MIN, zero and
  INT64_MAX;
- all 17 generated device variants converted to the exact enum value;
- supported `sessionDeviceTypeUnknown` versus an unrecognized type id;
- null result/session/device, duplicate ids, bad TTL and bad dates fail
  all-or-nothing without partial DTO; zero id remains a normal row;
- native and scripted-fake parity for returned order and every field;
- pinned `ResetAuthorizationQuery` server-false/public-Ok behavior is captured
  by a provenance fixture or exact source assertion.

Command/fake tests:

- strict grammar, frame args and canonical id boundaries, including zero;
  every forbidden option;
- Ready -> getMe -> user/bot ordering; bot traces contain neither
  `getActiveSessions` nor `terminateSession`;
- list/dry-run traces reconcile prior groups before Ready; real terminate
  traces perform Ready/getMe/bot/authority first and reconcile at step 6 before
  target resolution; authority-denied traces contain no reconciliation or
  prior-group append; list/dry-run create no current invocation group;
- total spool-root failure and byte-safe orphan contradiction at list,
  terminate-dry and real-terminate admission; exact step-2 versus step-6
  precedence; all affected paths emit no new record or TD request, while
  persistence-free reads and the seventeen §4.5 dry-runs remain admitted;
- list preserves order and makes exactly one `getActiveSessions` request;
- terminate grant denial occurs before target lookup in a real invocation;
- dry-run needs no grant/confirmation, calls only the admitted reads and
  creates no current audit/store/spool/config/mutation artifact;
- zero unmatched returns NOT_FOUND; zero matched non-current plans normally;
  zero or nonzero matched current returns PRECONDITION_FAILED/current_session;
- unknown id, duplicate id and current-session failure send no destructive
  request and create no current intent;
- immutable target is used by prompt, plan, audit and dispatch without a
  second resolution or post-dispatch reread;
- explicit deny beats all grants; request grant beats config source; config
  grant gets the accepted fresh CAS;
- non-TTY without `--yes`, TTY yes/no/malformed/disconnect/deadline;
- audit intent and `dispatch_started` are synced before the TD call; public
  `Ok`, including the server-false mapping, adds the acceptance
  `mutation_confirmed`, outcome and then terminal without a deletion claim;
- audit/store failure and recovery crash points at every durable boundary;
- no idempotency DB read/write, no pending stage and no replay; an
  `--idempotency-key` is rejected before Ready;
- every correlated TD success/error/429/auth-loss/deadline ordering, including
  `< deadline`, equality-to-deadline and old-generation events;
- current session can still be ended only by the existing `logout` path;
- business connected bots are not fetched or returned;
- all three packaged schemas validate directly, plus exact human golden output
  for empty/multi-row results, strings containing tabs/newlines, zero and
  int64 ids outside int53.

Daemon/no-daemon integration tests exercise the same handler and safety
descriptor in both modes. CLI-to-frame tests prove that session ids remain
strings and that neither parser nor JSON transport rounds them.

The no-skip Saved TestDC suite adds one non-mutating flow named
`m6.session.list` after authenticated user bootstrap:

1. invoke `tgcli --json session list` through the daemon;
2. validate stdout against `session-list.result.schema.json`;
3. require `inactive_session_ttl_days` in 1..366, `next:null`, at least one
   session, exactly one `is_current:true`, unique canonical ids, a known
   17-value device enum and returned order preserved by a second immediate
   call when TDLib returns the same id vector;
4. require empty stderr and no audit/idempotency/spool/config mutation for the
   current list invocation; the fixture begins with no incomplete audit group;
5. issue the normal daemon stop command, verify the daemon PID exited, then
   wait until the account socket disappears and the verified account lock can
   be acquired/released by the harness under the existing bounded deadline;
6. only after that release, repeat `tgcli --no-daemon --json session list` and
   validate the same schema/invariants without requiring byte-identical
   volatile dates or activity metadata.

The no-mutation assertion excludes expected daemon lifecycle artifacts such as
socket, PID/lock and lifecycle log/state changes, plus TDLib's permitted
internal cache/database effects. It does not exclude tgcli config, current-
invocation audit, idempotency or spool writes; those remain forbidden. TestDC
does not terminate a real session. Destructive success, failure,
current-session refusal, crash recovery and arbitration remain deterministic
fake/native-boundary tests. This still satisfies the canonical M6 requirement
to add one supported long-tail TestDC flow without creating an unsafe live
side effect.

### 4.8 M7 schema discovery

`tgcli schema <command-token>... [--all]` is an unconditional client-local
introspection command. It runs after CLI parsing and the global insecure-secret
precheck but before write-authority folding, account/config/removal routing,
request-context construction, runtime/socket access, daemon handling or TDLib
construction. `--no-daemon` is therefore an accepted byte-identical no-op. Invalid
config and `TGCLI_ACCOUNT`, `TGCLI_ALLOW_WRITE` or `TGCLI_TEST_DC` values cannot affect
it.

The positional tokens are flattened by splitting only ASCII space, tab, LF, CR, FF
and VT, discarding empty components and joining with one ASCII space. Matching is
case-sensitive and performs no Unicode or punctuation normalization. The exact
`history` key canonicalizes to `read`; there are no other aliases. Both `schema
account list` and `schema "account list"` therefore select `account list`.

Every catalog key is nonempty lowercase ASCII matching
`^[a-z0-9][a-z0-9-]*( [a-z0-9][a-z0-9-]*)*$`, equals its own target normalization,
and is unchanged by alias canonicalization. Catalog validation rejects whitespace
artifacts, doubled/edge spaces, uppercase/non-ASCII keys, quoted-command artifacts,
the alias key `history`, and collisions where different raw spellings across catalogs
normalize or alias to one target. Identical canonical keys are legal across catalogs:
the same `(command,kind,filename)` deduplicates, equal `(command,kind)` with different
filenames fails, and different kinds coexist. The bytewise schema-target accessor
contains only merged canonical keys plus a public alias whose destination is cataloged.
It supports only completion after `schema`; later general shell completion requires a
separate full command/alias registry, including uncataloged commands and aliases such
as `schema`, `daemon run`, and `history`.

Schema discovery consults three strict Draft 2020-12 catalogs:

- the existing result-only `docs/schemas/manifest.json`;
- the existing `docs/schemas/stream-manifest.json`;
- `docs/schemas/error-manifest.json`, the sole non-stream command-to-error-schema
  authority.

The error catalog maps account add/list/show/use to `account.error.schema.json`,
account remove to `account-remove.error.schema.json`, daemon restart/status/stop to
`daemon.error.schema.json`, login/me to `auth.error.schema.json`, logout to
`logout.error.schema.json`, resolve to `resolve.error.schema.json`, saved search/tags
to `saved.error.schema.json`, and session list/terminate to
`session.error.schema.json`. Stream errors remain solely in the stream catalog.
Audit/checkpoint/tombstone schemas are not command payload schemas and are not in these
catalogs.

The mixed audit schemas' documentation-only markers are verified by their
owning audit generator/source tests, not by command lookup. They are not
returned by `tgcli schema` or `--all` and are not silently added to a catalog.
If a future reviewed catalog entry names a marked command schema, byte-exact
schema discovery preserves that schema's marker; only then does the
catalog/embedded-asset test acquire a marker-visibility case.

Catalog merge keys are `(canonical command, result|item|error)`. Identical canonical
command keys may recur across catalogs and different kinds coexist. An identical
`(command,kind,filename)`, currently the `wait-for` result, is deduplicated; equal
`(command,kind)` with different filenames fails. Duplicate JSON keys, unsafe/non-leaf
references, missing/symlinked files, a wrong dialect, invalid JSON, or a referenced
file without exactly one final LF also fail catalog validation. The build generator
and release verifier each receive an explicit intended source root and, before
reading, lstat the nonsymlink directory chain `<root>/docs/schemas`, all three regular
nonsymlink catalogs, and every regular nonsymlink referenced leaf; strict resolution
of every checked component must remain inside the resolved intended source root.
Commands enumerate bytewise; kinds always enumerate in `result`, `item`, `error`
order.

Without `--all`, `schema` prints the cataloged result, or the item when no result
exists. Output is the embedded file byte-for-byte including its final LF. Thus
`schema listen` prints `listen.item.schema.json`; `listen` gains no result schema or
result-manifest entry. `schema wait-for` prints its one deduplicated result schema.

With `--all`, stdout is one JSON object containing every present kind in fixed
`result`, `item`, `error` order. Each value is the schema document itself. Construction
copies each schema byte-for-byte except its final LF into the member value and adds
exactly one final LF after the wrapper. There is no command/file metadata or success
envelope. Human and `--json` bytes are identical.

A missing or whitespace-only target is the existing exit-2
`USAGE/missing_argument` object. An unknown or uncataloged normalized target is exit 2
with exactly `{"error":{"code":"USAGE","message":"no curated schema is available
for command","details":{"argument":<normalized-key>,"reason":"unknown_command"}}}`
on stderr and no stdout.

The total CLI precedence is: global legacy-secret rejection; structural option parsing
and value validation; valid nested help; fixed unsupported-option precedence; target
normalization and missing detection; aliasing and lookup; success-only verbose
diagnostic; success output. A syntax/value failure is not hidden by a later help token,
but structurally valid help wins over missing/unknown targets, semantic unsupported
options and verbose. Unsupported options win over missing/unknown targets. Help is
exit 0 with empty stderr, no diagnostic or local state access, and these exact bytes:

    Print curated JSON schemas

    Usage:
      tgcli schema [OPTIONS] command...

    Positionals:
      command TEXT ... REQUIRED    command path (for example: account list)

    Options:
      -h,--help                    Print this help message and exit
      --all                        include every cataloged result, item, and error schema

`--json`, `--no-daemon`, `--no-color` and `--all` are accepted. `--no-color` is an
explicit byte-preserving no-op on help, success and failures. Verbose emits the exact
local-transport diagnostic only after a successful lookup, before schema output; help
and every failure emit no diagnostic. `--account`, `--full`, `--allow-write`, `--yes`,
`--dry-run`, `--timeout`, `--cursor` and `--idempotency-key` are
`USAGE/unsupported_mode` for `schema`, in that precedence after parser errors.

The build embeds the exact catalogs, every distinct referenced schema, and the merged
lookup table. Embedded bytes are the sole runtime authority: the command performs no
runtime filesystem lookup or schema-path override. Release packages still ship the
three catalogs and the exact unique set of referenced schemas. Package verification
derives that set from the catalogs, independently applies the same trusted-source-root
lstat/type/containment checks, rejects source or package symlinks/escape/tampering, and
proves a packaged binary's `schema version` and `schema listen` bytes equal the
packaged files.

`schema` is explicitly an introspection meta-command rather than a curated application
result DTO. It has no result/error schema and no catalog entry; `schema schema` is
therefore uncataloged. This sole exception avoids recursive arbitrary-schema
self-description and does not weaken strict schemas for ordinary command DTOs.

### 4.9 M6 curated long-tail contract

This section freezes the remaining M6 surface. Together with the two commands
in §4.7 it makes the curated M6 set exactly 32 commands. It is specification
only until the atomic activation described in §4.9.8: none of these 30 command
paths is registered, admitted by the safety dispatcher, discoverable through
`schema`, or present in the audit operation enum before its handler, typed
TDLib boundary, schemas, safety policy and tests land together. Section 4.7 is
unchanged.

The pinned authority is TDLib commit
`a17f87c4cff7b90b278d12b91ba0614383aaee82`. The generated declarations in
`td/generate/scheme/td_api.tl`, the folder clamp in the authenticated source
tree and the native generated C++ types are the boundary contract; a future
TDLib variant or field is malformed input until reviewed. Every JSON object in
this section is strict Draft 2020-12, has every shown field required and has
`additionalProperties:false` at every object boundary.

#### 4.9.1 Common grammar, identities and admission

The exact command grammar is:

```text
tgcli contact list
tgcli contact search <query>
tgcli contact add|remove|block|unblock <user>

tgcli folder list
tgcli folder show <folder-id>
tgcli folder create <name> --chat <chat> [--chat <chat> ...]
             [--icon <folder-icon>] [--color <-1..6>]
tgcli folder edit <folder-id> [--name <name>]
             [--icon <folder-icon|default>] [--color <-1..6>]
tgcli folder delete <folder-id>
tgcli folder add-chat|remove-chat <folder-id> <chat>

tgcli topic list <chat>
tgcli topic create <chat> <name> [--icon <topic-color>]
tgcli topic edit <chat> <topic-id> <name>
tgcli topic close|reopen <chat> <topic-id>

tgcli chat set-title <chat> <title>
tgcli chat set-photo <chat> <PATH>
tgcli chat set-photo <chat> --delete
tgcli chat set-description <chat> <description>
tgcli chat invite-link <chat> [--revoke <invite-link>]
tgcli chat promote <chat> <user> --rights <right[,right...]>
tgcli chat demote|ban|unban|kick <chat> <user>
tgcli chat set-permissions <chat> --permissions <permission[,permission...]|none>

tgcli storage stats
tgcli storage optimize
```

The normalized request command and args pairs are exactly:

```text
contact list                       {}
contact search                     {"query":string}
contact add|remove|block|unblock   {"user":string}

folder list                        {}
folder show|delete                 {"folder_id":int32}
folder create                      {"name":string,"chats":string[],
                                    "icon":folder_icon|null,"color_id":int32}
folder edit                        {"folder_id":int32,"name":string|null,
                                    "icon":folder_icon|null,
                                    "use_default_icon":boolean,
                                    "color_id":int32|null}
folder add-chat|remove-chat        {"folder_id":int32,"chat":string}

topic list                         {"chat":string}
topic create                       {"chat":string,"name":string,
                                    "icon":topic_color}
topic edit                         {"chat":string,"topic_id":int32,
                                    "name":string}
topic close|reopen                 {"chat":string,"topic_id":int32}

chat set-title                     {"chat":string,"title":string}
chat set-photo PATH                {"chat":string,"path":string}
chat set-photo --delete            {"chat":string,"delete":true}
chat set-description               {"chat":string,"description":string}
chat invite-link create            {"chat":string,"revoke":null}
chat invite-link revoke            {"chat":string,"revoke":string}
chat promote                       {"chat":string,"user":string,
                                    "rights":admin_right[]}
chat demote|ban|unban|kick         {"chat":string,"user":string}
chat set-permissions               {"chat":string,
                                    "permissions":chat_permission[]}

storage stats|optimize             {}
```

CLI order is not retained. Repeated `--chat` is the sole repeatable option;
every other duplicate singleton option is `USAGE/invalid_argument` before
routing. `folder edit` requires at least one of `--name`, `--icon` or
`--color`; `--icon default` sets `use_default_icon:true` and `icon:null`, while
an absent `--icon` sets both fields false/null. `chat set-photo` PATH and
`--delete` are mutually exclusive and one is required. `--rights` is required
and nonempty. `--permissions none` normalizes to an empty array; `none` cannot
be combined with another value.

All strings are valid Unicode-scalar UTF-8 without NUL. Lengths below are
Unicode scalar counts unless explicitly called bytes. No NFC/NFKC, case fold
or confusable rewrite occurs:

- contact search query is 1..256 UTF-8 bytes;
- folder name is 1..12 scalars without CR or LF and contains plain text only;
- topic name is 1..128 scalars;
- title is 1..128 scalars and description is 0..255 scalars;
- invite link is 1..4096 UTF-8 bytes without a C0/C1 control scalar;
- PATH and its frozen cwd/file rules are exactly §4.5.12.

Every M6 Result remains subject to the shared whole-frame equation in §4.5:
its compact data must fit `P - 31 - decimal_digits(request_id)`, with the
16,842,700-byte conservative ceiling used by request-id-independent
accumulators. Family-local 16,777,216-byte accumulator bounds are tighter and
remain unchanged. The future atomic schema/registry activation cannot admit a
payload merely because it satisfies its result schema.

Folder names, topic names, chat titles and chat descriptions use strict
canonical caller input. Before normalized args or a request frame exists, the
client applies the exact pinned cleaner below to a copy and requires byte
equality with the caller input. The daemon handler independently repeats the
same check before fingerprint, plan, confirmation or TD dispatch; a forged
noncanonical frame is `USAGE/invalid_argument`. The bytes are never silently
rewritten:

| field | exact pinned local pipeline |
|---|---|
| caller folder name | empty-entity `fix_formatted_text(...,skip_trim=false)` (including `clean_input_string`), then `clean_name(...,12)` |
| topic create/edit name | `clean_input_string`, then `clean_name(...,128)` |
| chat title | `clean_input_string`, then `clean_name(...,128)` |
| chat description | `clean_input_string`, then `strip_empty_characters(...,255,false)` |

The source authority is pinned `td/telegram/Requests.cpp`'s
`CLEAN_INPUT_STRING`, `td/telegram/misc.cpp`'s `clean_input_string`,
`strip_empty_characters` and `clean_name`,
`td/telegram/MessageEntity.cpp`'s `fix_formatted_text`,
`td/telegram/DialogFilter.cpp::create_dialog_filter`,
`ForumTopicManager.cpp::create_forum_topic/edit_forum_topic`,
`DialogManager.cpp::set_dialog_title/set_dialog_description`, and
`ChatManager.cpp::set_chat_description/set_channel_description`, all at the
§4.9 pin. This includes control-to-space conversion, CR removal, removal of
the pinned U+2028..U+202E and combining-line byte sequences, replacement of
the pinned Unicode space set, UTF-8 scalar truncation, edge trimming and—for
`clean_name`—collapse of ASCII space, LF and NBSP runs to one ASCII space.

Consequently leading/trailing/repeated ASCII space, LF and NBSP are rejected
for folder/topic/title fields whenever the cleaner changes them. For a
description, leading/trailing space or LF, NBSP and removed Unicode/control
characters are rejected, while repeated internal ASCII spaces and internal LF
are accepted because the pinned description pipeline preserves them. Tests
pin these distinctions, including U+2028 removal; a successful TD mutation
can therefore not normalize to bytes different from the plan/result.

`int53` is a JSON integer in `[-9007199254740991,9007199254740991]`.
Chat identifiers are nonzero int53 and user identifiers are positive int53.
`folder-id` and `topic-id` use canonical unsigned ASCII spelling and are JSON
integers in `1..2147483647`. Every pinned TDLib `int64` identifier is a
canonical decimal string matching §4.7's signed-int64 grammar; this includes
zero custom-emoji ids. Conversion to a JSON number is forbidden.

Mutation selectors are exact-only. A `<chat>` or `<user>` used by a mutation
must be a numeric id, exact `@username`, or exact supported public profile
link accepted by the shared M2 classifier. Display-name/title substring forms
fail locally with `USAGE/invalid_argument`; the resolver is not allowed to
expand the mutation domain. Read results may contain cached names but no M6
read command accepts a fuzzy target. Each resolver error retains
`operation:"resolve"` attribution. All selectors needed by one command are
resolved under one captured Ready/principal snapshot; partial resolution is
discarded, and the exact bound `{client_id,generation,auth_sequence,user_id}`
must still match before planning commit.

The existing globals `--account`, `--json`, `-v`, `--timeout` and
`--no-daemon` are accepted. Absent timeout means one admission-relative
absolute 60-second deadline. An explicit timeout retains the common finite
timeout grammar. The `raw` command prefix remains reserved through M6;
`--full`, `--cursor` and `--local` are unsupported for all 30 commands. A read
rejects `--allow-write`, `--yes`, `--dry-run` and
`--idempotency-key`. Every mutation accepts `--allow-write` and `--dry-run`.
Only the five Destructive commands accept `--yes`. The 22 allowlisted
mutations accept `--idempotency-key`; `chat invite-link` and
`storage optimize` reject it. Session commands retain §4.7's
`AbsentByPolicy` decision and are not added to this allowlist.

The closed tier matrix is:

| tier | commands |
|---|---|
| Read | `contact list`, `contact search`, `folder list`, `folder show`, `topic list`, `storage stats` |
| Write | `contact add/remove/block/unblock`, `folder create/edit/add-chat/remove-chat`, `topic create/edit/close/reopen`, `chat set-title/set-photo/set-description/promote/demote/unban/set-permissions` |
| Destructive | `folder delete`, `chat invite-link`, `chat ban`, `chat kick`, `storage optimize` |

`chat invite-link` has one static Destructive command descriptor for both
create and revoke. Request content never selects its tier. All 24 mutations
use the M3 WriteKernel; no handler may submit a mutating TD function directly.

Every command follows admission, the selected-account removal/logout preflight
applicable to ordinary authenticated work, Ready, correlated `getMe`,
principal binding, bot decision and command reads before its target call. A
dry-run retains §4.5's zero-current-persistence/no-prior-reconciliation rule;
a real mutation additionally enters the general v2 WriteKernel gate in
§4.9.7. Contacts and folders are user-account-only and return
`BOT_UNSUPPORTED` before their family-specific TD request. Topic, chat-admin
and storage commands admit a bot only where the pinned TD call and observed
chat/member permissions admit it; tgcli invents no bot privilege. Secret
chats are rejected for every M6 chat/folder/topic/admin operation.

Authorization loss, generation replacement, request cancellation, disconnect,
deadline and TD response compete through the existing RequestSession first
cause. A correlated response is usable only under its captured generation and
principal. Deadline equality belongs to the deadline. No setup retry crosses
an observed non-Ready state. Daemon and explicit no-daemon mode use the same
typed calls, resolver, WriteKernel, recovery, schemas and first-cause rules;
no-daemon never weakens write authority or durability.

#### 4.9.2 Contacts

The native boundary adds only `getContacts`, `searchContacts`, `addContact`,
`removeContacts` and `setMessageSenderBlockList`. The main block list is
`blockListMain`; null unblocks. `blockListStories`, `importContacts` and
`changeImportedContacts` are outside M6.

`contact list` performs `getContacts()`. `contact search` performs
`searchContacts(query,100)`. A `users` response must be non-null, have a
nonnegative `total_count`, contain no more ids than `total_count`, and contain
positive distinct ids; search additionally permits at most 100 ids. Every id
is converted through the accepted typed `getUser` path into exact
`UserIdentity`, in TD order. A null/mismatched/inaccessible user, duplicate id,
invalid username/name UTF-8, unknown user kind or partial conversion is
`INTERNAL/malformed_tdlib_response`; no partial list is emitted. These
unpaginated commands always return `next:null`. List admits at most 131,072
users, 16,777,216 charged serialized bytes and 262,144 bytes for one identity;
search admits at most 100 under the same byte/item bounds. A `+1` failure is
`INTERNAL/capacity_exhausted`, never truncation.

Contact mutations first classify the exact user selector, resolve a positive
non-bot-or-bot `UserIdentity` and obtain the matching typed user record in the
same generation. `contact add` uses exactly the returned `first_name`,
`last_name` and `phone_number` in
`importedContact(phone_number,first_name,last_name,note=null)` and always passes
`share_phone_number=false`. First name must be 1..64 scalars, last name 0..64;
the phone may be empty as TDLib permits. Caller-supplied replacement names,
notes, phone numbers and phone-sharing are not in this curated surface.
`contact remove` calls `removeContacts([user_id])`; no multi-user batch is
exposed. Block/unblock call
`setMessageSenderBlockList(messageSenderUser(user_id),blockListMain|null)`.

The raw phone is a request-local sensitive sidecar used only to construct the
single `addContact` call. It is never placed in a Result, error, frame,
confirmation, log, audit record, idempotency entry or durable spool. The plan
and audit arguments retain only
`phone_number_sha256`, the existing canonical `sha256:<64 lowercase hex>`
encoding of the digest of
`"tgcli.m6.contact.phone.v1\0" || raw_phone_bytes`, plus the resolved first and
last names. Even the empty phone is hashed; no raw-phone fallback exists.

The strict contact results are:

```text
ContactListResult   = {"items":UserIdentity[],"next":null}
ContactStateResult  = {"user":UserIdentity,"is_contact":boolean}
ContactBlockResult  = {"user":UserIdentity,"blocked":boolean}
```

List/search use `ContactListResult`; add/remove use `ContactStateResult` with
true/false; block/unblock use `ContactBlockResult` with true/false. Mutation
booleans reflect the accepted TD `ok` and planned action, not a post-mutation
reread.

#### 4.9.3 Folders and generation cache

The folder icon CLI/result enum is exactly:

```text
all, unread, unmuted, bots, channels, groups, private, custom, setup, cat,
crown, favorite, flower, game, home, love, mask, party, sport, study, trade,
travel, work, airplane, book, light, like, money, note, palette
```

The CLI/result spellings are case-sensitive. A fixed table maps them to the
pinned `chatFolderIcon.name` values `All`, `Unread`, `Unmuted`, `Bots`,
`Channels`, `Groups`, `Private`, `Custom`, `Setup`, `Cat`, `Crown`,
`Favorite`, `Flower`, `Game`, `Home`, `Love`, `Mask`, `Party`, `Sport`,
`Study`, `Trade`, `Travel`, `Work`, `Airplane`, `Book`, `Light`, `Like`,
`Money`, `Note`, and `Palette` in the same order; this is not a general title-
case conversion. Unknown returned names fail closed. `color_id` is -1..6.
`FolderName` has exactly `text`, `animate_custom_emoji`, and
`custom_emoji_entities`. Each entity has exact UTF-16 `offset`, positive
`length`, and canonical string `custom_emoji_id`; entities are sorted,
nonoverlapping, in bounds, and are the only permitted formatted-text kind.
`FolderSummary` has exactly `id`, `name:FolderName`, `icon`, `color_id`,
`is_shareable` and `has_my_invite_links`. `FolderSnapshot` has those fields plus
`pinned_chat_ids`, `included_chat_ids`, `excluded_chat_ids`, `exclude_muted`,
`exclude_read`, `exclude_archived`, `include_contacts`,
`include_non_contacts`, `include_bots`, `include_groups`, and
`include_channels`. `FolderSummary.icon` is the non-null effective lowercase
enum from `chatFolderInfo`; `FolderSnapshot.icon` is the configured lowercase
enum or null from `chatFolder`. Ids are nonzero int53.
Each id vector is duplicate-free, the three vectors are pairwise disjoint, and
TD order is preserved. The union of pinned and included ids and the excluded
vector each contain at most 100 entries. Any repeated id within a vector or
cross-vector duplicate in a TD snapshot is
`INTERNAL/malformed_tdlib_response` before a plan exists. The typed internal
`FolderRecord`, strict Result, plan, plan hash and audit snapshot all retain
the exact text, `animate_custom_emoji` boolean and validated CustomEmoji entity
ranges; dropping the boolean or hashing only visible text is forbidden.

Every folder command first binds the latest validated `updateChatFolders` for
the exact Ready generation; cached `chatFolderInfo` supplies the folder id and
`has_my_invite_links` field absent from `getChatFolder`. `folder list` is
served only from that update. The cache is cleared on generation start and on
the first non-Ready state. It validates at most 100 distinct positive folder
ids, one folder record per id and `main_chat_list_position` in
`0..chat_folders.size()`; `are_tags_enabled` must be a typed boolean. If the
current generation has not supplied the
update, the command waits under its deadline; it never reuses the previous
generation and does not synthesize a TD request. The result is
`{"items":FolderSummary[],"next":null}` in update order.

`folder show` calls `getChatFolder(folder_id)`, joins it to the cached info
with that id, and returns
`{"folder":FolderSnapshot}`. A successful top-level null `chatFolder` is the
exact pinned absence result and maps to
`NOT_FOUND/{"operation":folder_operation,"folder_id":folder_id}`, where
`folder_operation` is exactly `folder_show|folder_edit|folder_delete|`
`folder_add_chat|folder_remove_chat` for the call that returned null. A null
nested name/text/entity or null
effective `chatFolderInfo.icon`, malformed name/icon, invalid color, duplicate
chat id or unknown field variant is a whole-response `INTERNAL`; the configured
`chatFolder.icon` alone may be null. Null is not a general structural-to-
NOT_FOUND rule. TD errors remain `TDLIB_ERROR` after auth/429 precedence.
The same call-specific mapping applies to the edit/delete/add-chat/remove-chat
planner's `getChatFolder`; no mutation is planned from null.

Create resolves every repeated chat selector atomically, sorts/deduplicates
the resulting ids and requires 1..100 unique non-secret chats. It calls
`createChatFolder` with
`FolderName{text=canonical_name,animate_custom_emoji=false,
custom_emoji_entities=[]}`, selected/null icon, chosen color
(default -1), `is_shareable=false`, empty pinned/excluded ids, all resolved
ids as included ids, and every automatic filter boolean false. Its returned
`chatFolderInfo` supplies the new id and returned summary fields;
the immutable request supplies membership fields. Any disagreement with the
requested name/color/id is malformed. A configured null icon may legitimately
produce TDLib's non-null effective default icon in the info; a configured
non-null icon must match exactly. No `getChatFolder` follows creation.

Edit is metadata-only. It first reads and validates one full snapshot, replaces
only the explicitly supplied name text/icon/color and calls `editChatFolder`
with every other snapshot field byte-for-byte/logically unchanged. Without
`--name`, the complete FolderName is preserved. With `--name`, text is replaced
by the canonical caller bytes, the explicit caller-supplied plain-text entity
vector is empty, and the existing `animate_custom_emoji` value is preserved;
no hidden default changes true to false. At least one field must change; an
identical requested value is
`PRECONDITION_FAILED/no_change`. The returned info must match the planned
metadata and id, with the same configured-null/effective-default icon rule.
The result snapshot is composed from that response and the
pre-call preserved membership; no reread is allowed.
Here and below, “preserve FolderName” means exact preservation of text,
animation and entities whenever `--name` is absent; explicit `--name` is the
sole intentional text/entity replacement and still preserves the animation
boolean.

`folder add-chat` and `folder remove-chat` are full-snapshot read-modify-write
operations. They resolve the exact chat and read the folder before the commit
epoch. Add rejects an id already in pinned or included membership; otherwise
it removes the id from excluded membership when present and appends it to
`included_chat_ids`. Remove rejects an id absent from both pinned and included,
removes it from both vectors, and appends it to `excluded_chat_ids`. Both
require the resulting vectors to remain pairwise disjoint and the chosen union
to contain at most 100 unique ids. FolderName text/entities/animation,
unrelated excluded ids and every automatic filter are preserved. Both call
`editChatFolder` once, require matching returned info
and synthesize the final snapshot from the plan. They never use
`addChatToList`, never rewrite Main/Archive placement and never reread.
The strict plan's `before` and `after` snapshots make every excluded removal
or append, plus FolderName animation/entities, part of the plan hash and
audited transition.

Delete reads the full snapshot for confirmation/audit, then calls
`deleteChatFolder(folder_id,[])`; tgcli never leaves chats as a side effect.
The exact results are:

```text
FolderListResult       = {"items":FolderSummary[],"next":null}
FolderShowResult       = {"folder":FolderSnapshot}
FolderMutationResult   = {"folder":FolderSnapshot}
FolderMembershipResult = {"folder":FolderSnapshot,"chat":ChatIdentity,
                          "included":boolean}
FolderDeleteResult     = {"folder_id":int32,"deleted":true}
```

#### 4.9.4 Forum topics

Topics use only `getForumTopics`, `getForumTopic`, `createForumTopic`,
`editForumTopic` and `toggleForumTopicIsClosed`. List/create/edit accept an
observed forum supergroup or an observed private chat whose peer has pinned
`userTypeBot.has_topics=true`; create in the bot chat additionally requires
`allows_users_to_create_topics=true`. Close/reopen accept only a forum
supergroup, as required by the pinned function. A basic group, secret chat,
ordinary private chat, channel/non-forum supergroup or malformed bot type
fails before a topic TD call.

The create icon enum and TD color mapping are exactly:

```text
blue=0x6FB9F0, yellow=0xFFD67E, purple=0xCB86DB,
green=0x8EEE98, pink=0xFF93B2, red=0xFB6F5F
```

Omitted `--icon` normalizes to `blue`. Create passes
`is_name_implicit=false` and `forumTopicIcon(color,custom_emoji_id=0)`.
Custom-emoji input and implicit bot-topic names are outside M6. Edit changes
only the nonempty name and passes
`edit_icon_custom_emoji=false,icon_custom_emoji_id=0`; icon editing is not
inferred from the create enum.

For a forum supergroup, create requires the observed current member's
`can_create_topics` permission or administrator `can_manage_topics`; edit and
close/reopen require `can_manage_topics` unless the converted topic creator is
the current user. The bot-private-chat capabilities above replace, rather than
invent, a group-member right. A missing locally known capability is
`PRECONDITION_FAILED/missing_right` before the mutating TD call.

`TopicIcon` is exactly `{"color":topic_color,"custom_emoji_id":Int64String}`.
`TopicInfo` is exactly `chat_id`, `id`, `name`, `icon`, `creation_date`,
`creator`, `is_general`, `is_outgoing`, `is_closed`, `is_hidden`, and
`is_name_implicit`. `creation_date` is UTC RFC 3339 and `creator` is the exact
`MessageSenderRef`. `TopicRow` adds `is_pinned`, `unread_count`,
`unread_mention_count`, `unread_reaction_count`, and
`unread_poll_vote_count`. Counts are nonnegative int32. Null or malformed
topic/info/sender/icon values fail the whole command.

List has no public query or cursor. It issues the exact first tuple
`getForumTopics(chat_id,"",0,0,0,100)`, then repeats with the exact returned
`next_offset_date`, `next_offset_message_id` and
`next_offset_forum_topic_id`. It does not trust approximate `total_count` as
EOF. Every nonempty page must have
`total_count>=0`, `total_count>=topics.size()`, at most 100 topics, positive
distinct topic ids for the same chat, valid descending TD order, and a cursor
different from the request cursor unless the returned cursor is the all-zero
terminal sentinel. Before that comparison, all three returned cursor members
must be typed integers in their pinned wire ranges:
`next_offset_date` is `0..2147483647` and
`next_offset_forum_topic_id` is `0..2147483647`.
`next_offset_message_id=m` is structurally valid exactly when `m==0` or
`1048576<=m<=2251799812636672` and `(m & 1048575)==0`. This is the pinned
`MessageId::is_server()` representation: a positive `ServerMessageId` in
`1..2147483647`, shifted left by `SERVER_ID_SHIFT=20`. Merely fitting the
generated `int53` field is insufficient. A YetUnsent/local/scheduled/sponsored
or other internal `MessageId` representation is invalid even when
`MessageId::is_valid()`, `is_valid_scheduled()`, `is_valid_sponsored()` or the
outer int53 bound would accept it. These types and bounds come from the pinned
generated `forumTopics` declaration, `MessageId.h` and
`ForumTopicManager::get_forum_topics` zero-or-`is_server()` check.

The all-zero tuple is the terminal sentinel; any other tuple is a continuation
cursor. A negative, noninteger, out-of-range or non-server message-id component
is structural `INTERNAL/malformed_tdlib_response`, even if another component
or topic id would also make the page nonadvancing. This failure terminates the
request immediately: tgcli does not submit a next `getForumTopics`, relabel it
as `PAGINATION_INVALID`, or map it to `TDLIB_ERROR`. Order is the pinned signed
int64 field and is nonincreasing
across page boundaries. An empty page is valid only with the all-zero cursor
and then terminates; a nonempty page with the all-zero cursor also terminates.
Approximate `total_count` and a short page never do. A repeated topic or
nonterminal cursor that is equal, previously seen or otherwise nonadvancing is
`PAGINATION_INVALID/non_advancing_upstream`; this includes an empty page that
returns a structurally valid nonzero continuation cursor, a structurally valid
equal/seen/cyclic cursor, and duplicate-topic progression. A null top-level
page, negative or undersized `total_count`, oversized vector, null/malformed
topic/info/icon/sender, chat mismatch, or invalid topic scalar/date/count/order
is structural
`INTERNAL/{"operation":"topic_list","reason":"malformed_tdlib_response"}`.
Lifecycle/auth/deadline and an actual TD error arbitrate first; complete
page, nested-object, topic-id and returned-cursor structural validation then
precedes duplicate/cursor progress validation, and capacity admission is last.
Thus structural corruption cannot be relabeled as pagination merely because
the same page also repeats an id or cursor. The all-pages
accumulator admits at most 4096 topics, 16,777,216 charged serialized bytes
and 262,144 bytes for one topic; a `+1` failure is
`INTERNAL/capacity_exhausted`, never a partial list.

Create returns the converted `forumTopicInfo` directly. Edit/close/reopen first
call `getForumTopic` to validate the exact topic and precondition. A successful
top-level null `forumTopic` is the pinned absence result and maps to
`NOT_FOUND/{"operation":topic_operation,"chat_id":chat_id,"topic_id":topic_id}`,
where `topic_operation` is exactly `topic_edit|topic_close|topic_reopen` for
the call that returned null; a non-null topic with a
null `info` or other null/malformed nested object is `INTERNAL`. After the
mutation they do not reread. Close requires open, reopen requires closed, and
edit requires a different name. The strict results are:

```text
TopicListResult   = {"items":TopicRow[],"next":null}
TopicCreateResult = {"topic":TopicInfo}
TopicEditResult   = {"chat":ChatIdentity,"topic_id":int32,"name":string}
TopicStateResult  = {"chat":ChatIdentity,"topic_id":int32,"closed":boolean}
```

#### 4.9.5 Chat administration

All chat-admin commands require an exact non-secret basic group, supergroup or
channel target and a correlated `getChatMember(chat_id,messageSenderUser(me))`
preflight. The current principal must satisfy the explicit Creator/
Administrator/Member authority matrix below. A returned member/chat/sender
mismatch, unknown status,
invalid rights combination or inapplicable right is malformed; a locally
known missing right is `PRECONDITION_FAILED/missing_right`. TD error text is
never parsed to manufacture a privilege result.

`set-title`, `set-photo` and `set-description` require `can_change_info` and
call `setChatTitle`, `setChatPhoto` and `setChatDescription` respectively.
Photo PATH reuses the two-pass Saved-file stability and private spool protocol
in §4.5.12. In addition, its frozen basename ends in ASCII-case-insensitive
`.jpg` or `.jpeg`, pass 1 verifies SOI `ff d8` and a final EOI `ff d9` with no
trailing bytes, and TD receives only
`inputChatPhotoStatic(inputFileLocal(private_spool_path))`. Animated, previous
and sticker variants are forbidden. `--delete` passes null and creates no
file/spool. The same `spool_ready`, dispatch, recovery and cleanup ordering
applies; source bytes and private spool path never enter a terminal or audit.

`chat invite-link` requires `can_invite_users`. Create calls
`createChatInviteLink(chat_id,"",0,0,false)`. Revoke calls
`revokeChatInviteLink(chat_id,raw_link)`. The raw caller link is a secret
sidecar and may appear only in that TD request; it is absent from Results,
logs, errors, confirmation, plan, audit and store. A successful Result may
contain only a separately returned created or replacement link. Plan,
fingerprint and audit use only the canonical `sha256:<64 lowercase hex>`
encoding of the digest of
`"tgcli.m6.invite-link.v1\0" || raw_link_bytes`. Create/revoke outputs validate
every returned link record, but expose only the created link or, when a
primary link was revoked, the replacement primary link. The revoked raw link
is never echoed. Because Telegram creates a fresh secret or can revoke before
a correlated response, both modes share one Destructive descriptor and the
entire command rejects idempotency keys.

Every converted `chatInviteLink` requires a nonempty control-free link,
0..32-scalar name, positive creator user id, positive int32 creation `date`,
`edit_date==0 || edit_date>=date`, `expiration_date==0 ||
expiration_date>=date`, `member_limit` in 0..99999, and nonnegative member,
expired-member and pending-request counts. A positive member limit bounds
`member_count`; `creates_join_request:true` requires member_limit 0. The
optional `starSubscriptionPricing` is strict: period is exactly 60, 300, or
2592000 pinned seconds and `star_count` is positive int53. A subscription link
requires zero expiration/member limit, `creates_join_request:false`, and
`is_primary:false`. A primary record requires empty name, zero expiration/
member limit, `creates_join_request:false`, and null subscription pricing.
Null subscription pricing requires `expired_member_count==0`;
`creates_join_request:false` requires `pending_join_request_count==0`. Any
malformed present pricing, arithmetic or date/count relation failure is
structural `INTERNAL`; a null pricing pointer is valid only under the null
relationships above.

A create response is one non-null, non-primary, non-revoked record with the
bound principal as creator, empty name, zero edit/expiration/member limit and
all three counts zero, `creates_join_request:false`, and null subscription.
A revoke response requires `total_count==invite_links.size()` and exactly one
or two non-null records with distinct links. Exactly one record equals the
transient input and is revoked; pinned
`RevokeChatInviteLinkQuery::on_result` requires it at index 0. If it is
non-primary, it is the sole record. If it is primary, index 1 is required,
has the same creator, is a distinct active primary satisfying the primary
invariants, and is the only
replacement. No other ordering/cardinality or approximate-total
interpretation is accepted. The Result exposes the created/replacement active
link and uses null for a non-primary revoke; the revoked raw link is never
echoed.

Member commands resolve the target user exactly in the chat domain and call
`getChatMember(chat_id,messageSenderUser(user_id))` before mutation. The
target must not be the current principal or creator.

The member-absence classifier is call-specific and pinned to
`Requests.cpp::on_request(getChatMember)`,
`DialogParticipantManager::do_get_dialog_participant/finish_get_dialog_participant`
and `GetChannelParticipantQuery::on_error`. After lifecycle/auth/deadline wins
and 429 normalization, only TD error code 400 with exact case-sensitive message
`Member not found` maps to
`NOT_FOUND/{"operation":chat_admin_operation,"chat_id":chat_id,`
`"user_id":user_id}`, with the exact operation that issued this
`getChatMember`. Every other TD error,
including another 400 message, is `TDLIB_ERROR`. The upstream
`USER_NOT_PARTICIPANT` error for a supergroup/channel is converted by pinned
TDLib to a successful `chatMemberStatusLeft` and is a valid starting status,
not NOT_FOUND. A successful null `chatMember`, null status/rights/permissions,
sender mismatch or unknown status remains structural `INTERNAL`. Tests pin
auth/generation, 429, exact absence, other-400 and structural precedence.

Promote accepts this closed right enum in pinned field order:

```text
change-info, post-messages, edit-messages, delete-messages, invite-users,
restrict-members, pin-messages, manage-topics, promote-members,
manage-video-chats, post-stories, edit-stories, delete-stories,
manage-direct-messages, manage-tags, anonymous
```

The complete 17-bit applicability matrix is pinned to
`td_api.tl::chatAdministratorRights`; `yes` means the bit may be true for that
chat kind, not that the caller possesses it:

| pinned right | basic group | forum supergroup | nonforum supergroup | channel |
|---|:---:|:---:|:---:|:---:|
| `can_manage_chat` | yes | yes | yes | yes |
| `change-info` | yes | yes | yes | yes |
| `post-messages` | no | no | no | yes |
| `edit-messages` | no | no | no | yes |
| `delete-messages` | yes | yes | yes | yes |
| `invite-users` | yes | yes | yes | yes |
| `restrict-members` | yes | yes | yes | yes |
| `pin-messages` | yes | yes | yes | no |
| `manage-topics` | no | yes | no | no |
| `promote-members` | yes | yes | yes | yes |
| `manage-video-chats` | yes | yes | yes | yes |
| `post-stories` | no | yes | yes | yes |
| `edit-stories` | no | yes | yes | yes |
| `delete-stories` | no | yes | yes | yes |
| `manage-direct-messages` | no | no | no | yes |
| `manage-tags` | yes | yes | yes | no |
| `anonymous` | no | yes | yes | no |

A current Creator has every applicable delegable right in its column for local
preflight even though `chatMemberStatusCreator` contains no rights object. A
current Administrator has only the explicit true bits in its validated
snapshot; an inapplicable true bit is malformed. A current Member has no
administrator right. It may use only the pinned member permissions explicitly
named below: `can_change_info` for title/photo/description in a basic group or
supergroup, and `can_create_topics` for topic creation. Channel members have no
M6 admin mutation. Left/Restricted/Banned callers fail the authority
precondition.

This table combines the generated field comments with pinned
`DialogParticipant.cpp::AdministratorRights`: broadcast clears pin/topics/
tags/anonymous, megagroup clears post/edit/direct-message, Unknown/basic clears
topics, and every nonempty rights set implies `can_manage_chat`. The implied
bit is therefore structurally accepted for basic-group administrator snapshots
but remains unavailable as a caller-selected promotion flag.

The exact caller/target transition matrix is:

| operation | supported chat kinds | caller authority | target transition |
|---|---|---|---|
| set title/photo/description | basic, forum/nonforum supergroup, channel | Creator; Administrator with `change-info`; basic/supergroup Member with `can_change_info` | no member target |
| invite-link | basic, forum/nonforum supergroup, channel | Creator or Administrator with `invite-users`; ordinary Member is rejected | no member target |
| promote | forum/nonforum supergroup, channel; **basic group is `USAGE/unsupported_chat_type`** | Creator or Administrator with `promote-members` | Member/Restricted/editable Administrator → Administrator |
| demote | basic, forum/nonforum supergroup, channel | Creator or Administrator with `promote-members` | editable Administrator → Member |
| ban | basic, forum/nonforum supergroup, channel | Creator or Administrator with `restrict-members` | Member/Restricted/Left → Banned |
| unban | basic, forum/nonforum supergroup, channel | Creator or Administrator with `restrict-members` | Banned → Left |
| kick | basic, forum/nonforum supergroup, channel | Creator or Administrator with `restrict-members` | Member/Restricted → Left |
| set-permissions | basic, forum/nonforum supergroup | Creator or Administrator with `restrict-members` | no member target |

Basic-group promotion is intentionally absent because TDLib's basic-group
administrator status cannot represent the precise caller-selected rights
surface truthfully. It fails before target-member planning. For supported
promotion, requested rights must be applicable and a subset of the caller's
delegable rights; Creator uses the applicable column and Administrator uses
its explicit snapshot. `can_manage_chat` is never a caller flag and is passed
as true for the new administrator. Promote writes
`chatMemberStatusAdministrator(can_be_edited=true,rights)`; demote writes
`chatMemberStatusMember(0)`, ban writes `chatMemberStatusBanned(0)`, and
unban/kick write `chatMemberStatusLeft`.

Every target Creator, self target, noneditable Administrator or wrong starting
status is the exact closed `PRECONDITION_FAILED` branch. Chat-sender targets
are unsupported. There is no duration, revoke-messages, ownership transfer,
bulk member or chat-sender surface.

Set-permissions requires `can_restrict_members`, supports only basic groups
and supergroups, and maps the closed enum below one-to-one to the pinned
`chatPermissions` booleans; omitted flags are false:

```text
send-basic-messages, send-audios, send-documents, send-photos, send-videos,
send-video-notes, send-voice-notes, send-polls, send-other-messages,
add-link-previews, react-to-messages, edit-tag, change-info, invite-users,
pin-messages, create-topics
```

The complete array is canonicalized in this order. Duplicate, whitespace-
containing, empty or unknown comma members are local `USAGE`; `none` is the
only empty spelling. No implicit permission dependency is added by tgcli; a
combination rejected by TDLib remains a typed TD error.

Successful results are composed only from the immutable plan and the returned
`ok`/invite-link object:

```text
ChatTitleResult       = {"chat":ChatIdentity,"title":string}
ChatPhotoResult       = {"chat":ChatIdentity,"photo":"set"|"deleted"}
ChatDescriptionResult = {"chat":ChatIdentity,"description":string}
ChatInviteLinkResult  = {"chat":ChatIdentity,"action":"create"|"revoke",
                         "invite_link":string|null}
ChatPromoteResult     = {"chat":ChatIdentity,"user":UserIdentity,
                         "status":"administrator","can_manage_chat":true,
                         "rights":admin_right[]}
ChatMemberStateResult = {"chat":ChatIdentity,"user":UserIdentity,
                         "status":"member"|"banned"|"left"}
ChatPermissionsResult = {"chat":ChatIdentity,
                         "permissions":chat_permission[]}
```

Promote Result rights are exactly the canonical caller array and
`can_manage_chat:true` is exactly the TD input implied bit. Demote emits
`member`; ban emits `banned`; unban and kick emit `left`. Each value becomes
truth only after the correlated `ok`; it is never guessed before proof or
rewritten from an unavailable reread. No post-mutation `getChat`,
`getChatMember`, `getChatInviteLink` or other reread is allowed.

#### 4.9.6 Storage

Both storage commands require the selected account's exact Ready/principal
binding even though pinned TDLib permits some storage queries before
authorization. They add only `getStorageStatistics` and `optimizeStorage`.
Fast/database statistics are outside M6.

`storage stats` calls `getStorageStatistics(chat_limit=100)`. Optimize calls:

```text
optimizeStorage(size=-1, ttl=-1, count=-1, immunity_delay=-1,
                file_types=[], chat_ids=[], exclude_chat_ids=[],
                return_deleted_file_statistics=false, chat_limit=100)
```

Thus tgcli applies TDLib's deletion defaults and only fixes a bounded result
breakdown; it does not silently choose an age, size, count, file class or chat
filter. Optimize is Destructive, requires confirmation, and rejects
idempotency because a response cannot prove which already-missing local files
were deleted after a crash.

`StorageStatistics` has exactly `size`, `count`, and `by_chat`.
`StorageByChat` has `chat_id`, `size`, `count`, and `by_file_type`.
`StorageByFileType` has `file_type`, `size`, and `count`. Sizes are nonnegative
int53 and counts nonnegative int32. `chat_id` may be zero for the aggregate
bucket. `file_type` is the closed lowercase mapping of the 25 pinned variants:

```text
none, animation, audio, document, live-photo-video, notification-sound,
photo, photo-story, profile-photo, secret, secret-thumbnail, secure,
self-destructing-live-photo-video, self-destructing-photo,
self-destructing-video, self-destructing-video-note,
self-destructing-voice-note, sticker, thumbnail, unknown, video, video-note,
video-story, voice-note, wallpaper
```

Unknown variants, negative scalar fields, null entries, more than 101 chat
rows (100 plus aggregate), more than one zero aggregate row, duplicate nonzero
chat ids, more than 25 or duplicate file types in one row, or checked sum
overflow are malformed TD responses. For every chat row, parent `size` and
`count` must equal the overflow-checked sums of all file-type children. The
top-level `size` and `count` must equal the overflow-checked sums of every chat
row, including the zero aggregate row. Size sums remain within nonnegative
int53 and count sums within nonnegative int32; no wrap, saturation or omitted
bucket is accepted. TD order is preserved. Stats returns `StorageStatistics`
directly; optimize returns
`{"optimized":true,"statistics":StorageStatistics}`. No filesystem path,
TDLib database path or deleted-file name is exposed.

#### 4.9.7 WriteKernel, audit, idempotency and recovery

All 24 mutations use §4.5's two-epoch WriteKernel and durability rules. Their
closed future `AccountAuditOperation` additions are:

```text
contact_add, contact_remove, contact_block, contact_unblock,
folder_create, folder_edit, folder_delete, folder_add_chat,
folder_remove_chat,
topic_create, topic_edit, topic_close, topic_reopen,
chat_set_title, chat_set_photo, chat_set_description, chat_invite_link,
chat_promote, chat_demote, chat_ban, chat_unban, chat_kick,
chat_set_permissions,
storage_optimize
```

The first account epoch performs complete prior removal/audit/store/spool
reconciliation and initial idempotency lookup when policy allows one, then
releases before resolver, property/member/folder/topic/file planning. The
commit epoch repeats the core
gate and authoritative lookup, confirms the immutable plan for a Destructive
operation, performs config/principal/generation CAS, appends and syncs intent,
inserts the pending idempotency entry when allowed, publishes any photo spool,
then submits the mutation. It remains held through dispatch proof, outcome,
store transition and eligible spool cleanup. No mutating TD call occurs before
durable intent. No handler rereads Telegram state after a mutating call; the
result is the correlated returned object plus frozen plan. A disconnect does
not cancel durability completion.

Every non-photo M6 mutation uses §4.5's `direct` stage order
`[idempotency_pending?],dispatch_started,mutation_confirmed?`; photo set uses
the existing `saved-attach` order without temporary message ids:
`[idempotency_pending?],spool_ready,dispatch_started,mutation_confirmed?`.
Only the accepted six generic stage names exist; M6 adds no operation-specific
checkpoint spelling. `mutation_confirmed` stores the exact strict terminal
needed for recovery and idempotent replay.

Dry-run performs the same Ready/getMe/bot, exact resolution, folder/topic/member
reads, phone hash, invite-link hash and photo pass-1 stability/JPEG validation
needed to construct the plan. It makes no Write/Destructive TD call, no
confirmation and no current audit/idempotency/spool/config write. Prior-group
reconciliation is not invoked, exactly as for the existing M3/M4 planners; an
implementation may validate already-opened state read-only but cannot append,
sync, repair, expire or clean it. Its result is exactly
`{"dry_run":true,"plan":M6Plan}`. A grant or `--yes` on dry-run is ignored.

The closed §6 dry-run TD-read allowlist is extended for M6 by exactly
`getChatFolder`, `getForumTopic` and `getChatMember`, plus `getUser` only in
the exact user resolver needed by contact/member planning. Existing exact
chat/user public-link resolver calls retain their accepted restrictions.
Generation-cached `updateChatFolders` is an observer update, not a planner TD
call. `getContacts`, `searchContacts`, `getForumTopics`, storage functions and
every mutation remain forbidden in a dry-run.

At implementation activation the §4.5.12 `v2_gate_operation` set extends by
the 24 operation names above for real mutations only. The six M6 reads remain
persistence-free ordinary reads; they do not enter the general M3 mutation
spool/store gate. This additive rule supersedes §4.7's session-only M6 wording
only for §4.9 operations and does not change session `AbsentByPolicy`.

The 22 idempotent operations are every operation in the list above except
`chat_invite_link` and `storage_optimize`. Matching completed entries replay
the stored exact terminal without TD calls; Destructive replay reconfirms the
stored plan. A write without a key remains audited but has no automatic retry.
The accepted 604800-second expiry, quota, pin, crash and first-cause rules are
unchanged. Section 4.7 session operations remain `AbsentByPolicy`.

Every plan is a strict object beginning with its exact `operation` and resolved
identity. It includes only normalized mutation inputs and the minimum frozen
precondition state needed for replay/recovery. Folder RMW plans include the
complete before/after `FolderSnapshot`; member plans include the before status
and exact after status/rights; photo plans include the public `FileSnapshot`
identity and SHA-256 but no bytes or spool path; contact-add plans include the
domain-separated phone digest but no phone; revoke plans include only the
domain-separated link digest. Audit `arguments` use the same redacted forms.
The request fingerprint remains the canonical normalized caller request; its
caller-supplied revoke link is replaced by the domain-separated link digest,
while the server-derived phone digest belongs only to the plan/audit.
Confirmation targets are exactly these plans.

Every `M6Plan` branch has exact common fields `operation`, `account` and
`tdlib_request`. Its remaining required fields are:

```text
contact_add:
  user:UserIdentity, first_name:string, last_name:string,
  phone_number_sha256:sha256, share_phone_number:false
contact_remove:
  user:UserIdentity, is_contact:false
contact_block/contact_unblock:
  user:UserIdentity, blocked:boolean

folder_create:
  name:FolderName, icon:folder_icon|null, color_id:int32,
  chat_ids:nonzero_int53[1..100]
folder_edit:
  folder_id:int32, before:FolderSnapshot, after:FolderSnapshot
folder_delete:
  folder:FolderSnapshot, leave_chat_ids:[]
folder_add_chat/folder_remove_chat:
  folder_id:int32, chat:ChatIdentity,
  before:FolderSnapshot, after:FolderSnapshot

topic_create:
  chat:ChatIdentity, name:string, icon:topic_color
topic_edit:
  chat:ChatIdentity, before:TopicInfo, name:string
topic_close/topic_reopen:
  chat:ChatIdentity, before:TopicInfo, closed:boolean

chat_set_title:
  chat:ChatIdentity, title:string
chat_set_photo:
  chat:ChatIdentity, delete:boolean, file:FileSnapshot|null
chat_set_description:
  chat:ChatIdentity, description:string
chat_invite_link:
  chat:ChatIdentity, action:"create"|"revoke",
  invite_link_sha256:sha256|null
chat_promote:
  chat:ChatIdentity, user:UserIdentity,
  before:MemberStatusSnapshot, can_manage_chat:true,
  rights:admin_right[1..16]
chat_demote/chat_ban/chat_unban/chat_kick:
  chat:ChatIdentity, user:UserIdentity,
  before:MemberStatusSnapshot, after:"member"|"banned"|"left"
chat_set_permissions:
  chat:ChatIdentity, permissions:chat_permission[0..16]

storage_optimize:
  size:-1, ttl:-1, count:-1, immunity_delay:-1,
  file_types:[], chat_ids:[], exclude_chat_ids:[],
  return_deleted_file_statistics:false, chat_limit:100
```

`MemberStatusSnapshot` is a strict tagged union of the pinned six statuses:
creator has `kind`, `is_anonymous`, `is_member`; administrator has `kind`,
`can_be_edited`, `can_manage_chat`, `rights`; member has `kind`,
`member_until_date`; restricted
has `kind`, `is_member`, `restricted_until_date`, `permissions`; left has only
`kind`; banned has `kind`, `banned_until_date`. Dates are int32;
`can_manage_chat` preserves the pinned implied bit separately, while all other
rights/permissions use the canonical arrays above. `chat_set_photo.file` is
null iff delete is true. `chat_invite_link.invite_link_sha256` is null iff
action is create. Member `after` is fixed by the operation and cannot select
a different status.

The exact `tdlib_request` mapping is:

```text
contact_add=addContact
contact_remove=removeContacts
contact_block,contact_unblock=setMessageSenderBlockList
folder_create=createChatFolder
folder_edit,folder_add_chat,folder_remove_chat=editChatFolder
folder_delete=deleteChatFolder
topic_create=createForumTopic
topic_edit=editForumTopic
topic_close,topic_reopen=toggleForumTopicIsClosed
chat_set_title=setChatTitle
chat_set_photo=setChatPhoto
chat_set_description=setChatDescription
chat_invite_link=createChatInviteLink|revokeChatInviteLink
chat_promote,chat_demote,chat_ban,chat_unban,chat_kick=setChatMemberStatus
chat_set_permissions=setChatPermissions
storage_optimize=optimizeStorage
```

Mutation proof is the typed correlated TD response: `ok`, returned folder/topic
info, returned invite-link set, or returned storage statistics as applicable.
A malformed success response is not success. Dispatch without a correlated
proof is `mutation_state:"possible"`; recovery never resends automatically.
Invite-link and optimize recovery can close an unproven invocation only as an
ambiguous failure and never recreate/revoke/optimize again. Photo recovery
retains the private spool until the existing audit/store proof permits cleanup.

Raw phone, local file bytes, private spool path, TD error message, argv and
confirmation answers are prohibited from logs, diagnostics, audit,
idempotency, result/error schemas and crash records. The caller invite link is
necessarily present in the strict request-args schema and transient request
frame, but is prohibited from every other listed surface. Hashes are domain-
separated and are not presented as proof that the underlying secret is
unknowable. Audit files, store and spool retain the existing no-symlink/
current-uid/mode/link-count/fsync requirements. No M6 command creates new
persistence outside those foundations.

#### 4.9.8 Results, errors and schema activation

Each mutation result schema is a strict `oneOf` between its real result above
and the exact dry-run wrapper/plan. The six reads have only their real branch.
Human success uses the existing deterministic pretty-JSON fallback:
`data.dump(2) + "\n"`; M6 adds no ad-hoc table renderer. JSON mode emits the
same strict compact object plus one LF. Errors use the standard error envelope,
stderr and exit table; success stdout is empty on error.

The five closed family error operation enums are:

```text
contact.error:   contact_list, contact_search, contact_add, contact_remove,
                 contact_block, contact_unblock
folder.error:    folder_list, folder_show, folder_create, folder_edit,
                 folder_delete, folder_add_chat, folder_remove_chat
topic.error:     topic_list, topic_create, topic_edit, topic_close, topic_reopen
chat-admin.error: chat_set_title, chat_set_photo, chat_set_description,
                  chat_invite_link, chat_promote, chat_demote, chat_ban,
                  chat_unban, chat_kick, chat_set_permissions
storage.error:   storage_stats, storage_optimize
```

Each family schema enumerates only reachable strict branches from the existing
common contracts: `USAGE`, `CONFIG_INVALID`, `CONFIG_CONFLICT`,
`ACCOUNT_NOT_FOUND`, `ACCOUNT_MISMATCH`, `HOOK_FAILED`, `NOT_AUTHED`,
`BOT_UNSUPPORTED`, resolver `NOT_FOUND`/`AMBIGUOUS`, `TDLIB_ERROR`,
`RATE_LIMITED`, `TIMEOUT`, `DAEMON_SHUTDOWN`, `PROTOCOL_ANSWER_INVALID`,
`REMOVAL_INCOMPLETE`, prior-group `AUDIT_UNAVAILABLE`/`AUDIT_INCOMPLETE`/
`SPOOL_UNAVAILABLE`, `INTERNAL`, and for mutations `WRITE_DENIED`,
`CONFIRMATION_REQUIRED`, `AUDIT_UNAVAILABLE`, `AUDIT_INCOMPLETE`,
`IDEMPOTENCY_CONFLICT`, `IDEMPOTENCY_PENDING`,
`IDEMPOTENCY_UNAVAILABLE`, plus photo-only `NOT_FOUND`, `INPUT_CHANGED` and
`SPOOL_UNAVAILABLE`. Folder/topic/admin locally proven state conflicts use
`PRECONDITION_FAILED` with a closed family reason. Topic pagination uses
`PAGINATION_INVALID`. Invite-link/storage schemas omit idempotency branches.
Within each shared family schema, operation constants prevent a read operation
from validating current-mutation authority, confirmation, idempotency,
general-v2 recovery or outcome branches; ordinary selected-account preflight
errors retain their existing shapes. `TDLIB_ERROR`
exposes only operation and numeric TD code; 429 is always `RATE_LIMITED` with
canonical saturated `retry_after`. `INTERNAL` never exposes TD message text or
secrets.

The family-local strict details are closed as follows; every unlisted common
detail is inherited byte-for-byte from §§4.1, 4.5, 5.2 and 6 rather than
duplicated with a new shape:

| code | exact M6 details |
|---|---|
| `BOT_UNSUPPORTED` | `{"operation":m6_operation}` |
| `TDLIB_ERROR` | `{"operation":m6_operation,"tdlib_code":int32}` |
| `RATE_LIMITED` | `{"operation":m6_operation,"tdlib_code":429,"retry_after":nonnegative_int32}` |
| `TIMEOUT` | `{"operation":m6_operation,"state":nullable_auth_state}` |
| malformed `INTERNAL` | `{"operation":m6_operation,"reason":"malformed_tdlib_response"}` |
| capacity `INTERNAL` | `{"operation":"contact_list"|"contact_search"|"topic_list","reason":"capacity_exhausted","resource":"users"|"topics"|"bytes"|"item_bytes","limit":nonnegative_integer}` with the operation/resource pair enforced |
| `PAGINATION_INVALID` | `{"operation":"topic_list","reason":"non_advancing_upstream"}` |
| missing folder | `{"operation":"folder_show"|"folder_edit"|"folder_delete"|"folder_add_chat"|"folder_remove_chat","folder_id":positive_int32}` |
| missing topic | `{"operation":"topic_edit"|"topic_close"|"topic_reopen","chat_id":nonzero_int53,"topic_id":positive_int32}` |
| missing member | `{"operation":chat_admin_operation,"chat_id":nonzero_int53,"user_id":positive_int53}` |
| missing photo source | `{"operation":"chat_set_photo","path":string,"reason":source_file_reason}` |
| folder precondition | `{"operation":folder_operation,"folder_id":positive_int32,"chat_id":nonzero_int53_or_null,"reason":"no_change"|"already_in_folder"|"not_in_folder"|"folder_capacity"}` |
| topic precondition | `{"operation":topic_operation,"chat_id":nonzero_int53,"topic_id":positive_int32_or_null,"reason":"missing_right"|"no_change"|"already_closed"|"already_open"}` |
| admin-right precondition | `{"operation":chat_admin_operation,"chat_id":nonzero_int53,"reason":"missing_right","right":admin_right}` |
| member precondition | `{"operation":chat_admin_operation,"chat_id":nonzero_int53,"user_id":positive_int53,"reason":"self_target"|"creator"|"noneditable_administrator"|"wrong_member_state"}` |

The `chat_id` in folder preconditions is null only for `folder_edit/no_change`;
membership and `folder_capacity` branches retain the resolved chat id. The
topic id is
null only for create-time `missing_right`. Schema `oneOf` arms enforce those
relations instead of accepting the broad nullable forms. Resolver
NOT_FOUND/AMBIGUOUS retains its exact selector/candidates objects and
`operation:"resolve"` attribution. `NOT_AUTHED` retains exact account/state and
`reason:not_ready|authorization_lost|login_required`; it is never replaced by
a second authorization-loss code.

The five future family schemas have this exhaustive absence/ambiguity arm
inventory. `operation` constants shown here and in the table above are schema
constants or closed enums, never an unconstrained M6 operation string:

| family schema | exact `NOT_FOUND` / `AMBIGUOUS` arms |
|---|---|
| `contact.error.schema.json` | existing resolver arms with `operation:"resolve"`; runtime reachability is contact add/remove/block/unblock |
| `folder.error.schema.json` | existing resolver arms with `operation:"resolve"` for create/add-chat/remove-chat, plus the call-local missing-folder arm for show/edit/delete/add-chat/remove-chat |
| `topic.error.schema.json` | existing resolver arms with `operation:"resolve"` for all five topic operations, plus the call-local missing-topic arm for edit/close/reopen |
| `chat-admin.error.schema.json` | existing resolver arms with `operation:"resolve"` for all ten chat-admin operations, the call-local missing-member arm for exactly the ten-value `chat_admin_operation` enum, and the §4.5 source-file arm specialized to `operation:"chat_set_photo"` |
| `storage.error.schema.json` | no `NOT_FOUND` or `AMBIGUOUS` arm |

The specialized photo-source arm has stable message `input file is
unavailable`, the same closed `source_file_reason` and canonical display-path
rules as §4.5.3/§4.5.12, and is reachable only for non-delete
`chat set-photo`. No family accepts another family's operation value or a
folder/topic/member/photo absence arm not listed above. Resolver reachability
is additionally enforced by command tests because the inherited resolver
payload intentionally identifies its own operation as `resolve`; it does not
invent a parent-operation field.

The exact future result assets and result-manifest keys are:

| schema file | command key |
|---|---|
| `contact-list.result.schema.json` | `contact list` |
| `contact-search.result.schema.json` | `contact search` |
| `contact-add.result.schema.json` | `contact add` |
| `contact-remove.result.schema.json` | `contact remove` |
| `contact-block.result.schema.json` | `contact block` |
| `contact-unblock.result.schema.json` | `contact unblock` |
| `folder-list.result.schema.json` | `folder list` |
| `folder-show.result.schema.json` | `folder show` |
| `folder-create.result.schema.json` | `folder create` |
| `folder-edit.result.schema.json` | `folder edit` |
| `folder-delete.result.schema.json` | `folder delete` |
| `folder-add-chat.result.schema.json` | `folder add-chat` |
| `folder-remove-chat.result.schema.json` | `folder remove-chat` |
| `topic-list.result.schema.json` | `topic list` |
| `topic-create.result.schema.json` | `topic create` |
| `topic-edit.result.schema.json` | `topic edit` |
| `topic-close.result.schema.json` | `topic close` |
| `topic-reopen.result.schema.json` | `topic reopen` |
| `chat-set-title.result.schema.json` | `chat set-title` |
| `chat-set-photo.result.schema.json` | `chat set-photo` |
| `chat-set-description.result.schema.json` | `chat set-description` |
| `chat-invite-link.result.schema.json` | `chat invite-link` |
| `chat-promote.result.schema.json` | `chat promote` |
| `chat-demote.result.schema.json` | `chat demote` |
| `chat-ban.result.schema.json` | `chat ban` |
| `chat-unban.result.schema.json` | `chat unban` |
| `chat-kick.result.schema.json` | `chat kick` |
| `chat-set-permissions.result.schema.json` | `chat set-permissions` |
| `storage-stats.result.schema.json` | `storage stats` |
| `storage-optimize.result.schema.json` | `storage optimize` |

The exact future error assets are `contact.error.schema.json`,
`folder.error.schema.json`, `topic.error.schema.json`,
`chat-admin.error.schema.json`, and `storage.error.schema.json`; every command
in a family maps to that family file. The audit intent/outcome/checkpoint
schemas extend their operation/plan/result unions by the 24 names above.

These 35 command-schema files, manifest mappings and audit enum/schema arms are
one implementation activation transaction. The current source tests require
every top-level `*.result.schema.json` to have exactly one result-manifest
mapping and require embedded catalog/runtime tables to match. Adding dormant
files or manifest/audit arms before handlers would deliberately violate that
bijection and make `schema` advertise unimplemented commands. Therefore this
specification commit creates none of those files and does not change any
manifest, embedded asset, registry or audit enum. Implementation must add all
35 strict files, mappings, generated embedded bytes, 24 audit arms, native
factories, safety descriptors, handlers and public registrations atomically;
no partial public prefix is permitted.

#### 4.9.9 Verification, TestDC and platform gates

Implementation acceptance is foundation-oriented rather than 30 independent
micro-slices:

1. strict CLI/frame parsing covers every command/args pair, bounds, canonical
   numeric spelling, option matrix, repeated options and rejected global mode;
   folder/topic/title/description canonical-input fixtures cover leading,
   trailing and repeated ASCII space/LF, NBSP, U+2028 and every pinned removed
   sequence in both CLI and forged-frame paths;
2. typed native/fake conversion covers every pinned function and output
   variant, null/mismatch/unknown objects, exact TD field values and forbidden
   calls; source verification authenticates commit a17f87c4 and the inspected
   declarations/folder clamp without network access;
3. resolver/generation tests prove exact-only targets, atomic multi-chat
   folder resolution, bot and secret matrices, auth loss at every response
   gap, daemon/no-daemon byte/trace parity, top-level null folder/topic
   NOT_FOUND, nested-null INTERNAL, and exact `400/Member not found` versus
   Left/429/other-error precedence; schema tests cover every permitted
   folder/topic/member/photo absence operation and reject cross-family,
   unlisted-operation and storage-family NOT_FOUND/AMBIGUOUS arms;
4. folder-cache tests cover generation replacement, update validation,
   create 1/100/+1, every within/cross-vector duplicate, excluded-to-included
   add, pinned/included-to-excluded remove, `animate_custom_emoji` true/false,
   formatted-entity/plan-hash preservation, full-snapshot RMW and zero rereads;
5. topic tests cover every icon, all-page cursor progress, short pages, empty
   terminal and nonempty zero-cursor completion, duplicate/nonadvancing
   pagination, structurally valid equal/seen/cyclic cursor tuples, and
   duplicate-topic progression; negative, noninteger and one-past-maximum
   date/message/topic cursor components prove structural
   `INTERNAL/malformed_tdlib_response` precedence over every pagination
   conflict without another TD request. Message-id fixtures additionally pin
   `1048577` (low bit one), `1048578` (local), `12` (scheduled),
   `2251799812636673` (`MessageId::max().get()+1`) and aligned
   `2251799813685248` (`(INT32_MAX+1)<<20`) as structural failures, alongside
   total/null/nested/order corruption, 4096/+1 and byte-capacity cases;
6. admin tests cover static JPEG/path/spool cutpoints, delete mode, every
   one of 17 rights across Creator/Administrator/Member and all four chat-kind
   columns, basic-group promotion rejection, every target transition/member
   precondition, exact Result truth, invite create/revoke total/cardinality/
   creator/valid-and-invalid-date/count/null-or-present-subscription/primary
   relations, redaction and zero post-write reads;
7. storage tests prove the exact default tuple, null/duplicate/bounds
   conversion, overflow-safe child-to-parent and row-to-top size/count sums,
   destructive confirmation, no idempotency and no path/file leakage;
8. WriteKernel tests cover all 24 operations, both epochs, dry-run zero-current-
   persistence, grant/confirmation, 22-operation idempotency allowlist,
   crash/recovery/first-cause, audit grouping/redaction and proof/no-reread;
9. all 30 Results and five family errors validate positives and boundary/
   additional-property negatives; catalog/generator/package tests prove exact
   source↔manifest↔embedded↔installed bytes and the audit schema union;
10. Debug, ASan/UBSan, canonical wrapped non-TDLib TSan, Release, full tidy,
    format, dependency/source/release/mechanical gates run on the integrated
    candidate. Linux callback safety remains green; macOS compile/package and
    runtime evidence is required in CI because local macOS execution is not a
    substitute.

The one supported live TestDC milestone flow is non-mutating
`m6.storage.stats`: it uses the authenticated TestDC account lifecycle,
asserts strict result/schema/human output, records the pinned
binary/source/config evidence, and registers no cleanup because it creates no
Telegram object. Fake/native contract tests prove the exact TD tuple and cover
every mutation, destructive recovery and redaction. Live TestDC must never run
a contact,
folder, topic, admin, invite-link or optimize mutation, and the spec/generator
checks themselves require no network.

### 4.10 M2/M4 surfaces and dormant M7 freeze foundations

This section materializes byte-reviewable foundations before each command's
atomic activation. Search, chat-info, chat-members and download are now active
through their parser, handler, dispatcher and result/error mappings. Raw assets
remain uncataloged and unreachable. Existing commands keep their current
behavior.

#### 4.10.1 Curated download

`download` accepts exactly the primary animation, audio, document, photo,
sticker, video, video-note and voice-note media constructors. Album, paid
media, webpage, expired and other unsupported media produce exact
`PRECONDITION_FAILED` reasons `album_unsupported`, `paid_media_unsupported`,
`web_page_unsupported`, `expired_media`, or `unsupported_media`. A supported
constructor with null/malformed/nonpositive file id is
`INTERNAL/malformed_tdlib_response`.

After Ready/getMe and exact chat resolution, download performs one contextual
message read. A documented absent message is `NOT_FOUND` with exactly
`{"chat_id":nonzero_int53,"message_id":nonzero_int53}`; wrong/null response
types are malformed TD data. Direct animation, audio, document, sticker,
video, video-note and voice-note contents select only their primary file
field. Photo selects the first `photo.sizes` row having the greatest checked
`width*height`; dimensions must be nonnegative, every row and file is
structurally valid, and ties retain TD order. `messagePhoto.video` is never a
candidate and is neither a fallback nor an alternate media result. Album/
paid/webpage wrappers are classified before nested primary media and never
silently unwrapped.

Destination interpretation precedes suggested-name lookup. The invocation
freezes `context.cwd` before routing. A relative `-O` and a relative captured
`TGCLI_MEDIA_DIR` are each resolved against that same frozen cwd; neither may
observe a later process cwd or environment change. Without `-O`, the selected
media directory (captured environment, then frozen cwd) is directory mode. An
`-O` value naming an existing directory, or ending in a slash, is directory
mode; a trailing slash requires an existing safe directory and otherwise is
`OUTPUT_UNAVAILABLE/invalid_path`. Every other `-O` is exact-file mode. Exact-
file mode never calls TDLib for a name. Directory mode calls exactly
`getSuggestedFileName(file_id, absolute_directory)` and accepts only a
nonempty valid-UTF-8 safe leaf: not `.` or `..`, no slash, NUL, control byte,
or platform-invalid component. Empty, unsafe, null or malformed output is
`OUTPUT_UNAVAILABLE/invalid_path` with the resolved absolute destination in
details. There is no local fallback extension, media-derived name, or direct
media `file_name` precedence. A final-name collision is only `OUTPUT_EXISTS`.

The generation observer is installed before `downloadFile(file_id,16,0,0,
false)`. Its initial response may already be a completed local file. The
response File/error/malformed value and same-id `updateFile` events enter one
stamped sequence arbitration. The first structurally valid completed state is
the candidate. `local.can_be_downloaded` is advisory and may still be true on
that completed snapshot. Later incomplete snapshots, TD errors and malformed
non-file responses are stale after completion and cannot undo it; only a later
completed tuple with incompatible id/path/size/expected-size/completion state is
`INTERNAL/malformed_tdlib_response` and publishes nothing. Before completion,
the first stamped TD error or malformed response is terminal. A wrong-id
response is malformed; wrong-id updates are ignored. This curated operation
never shares a raw `downloadFile` descriptor and never calls TDLib's global
cancel-download operation.

Negative/out-of-int53 progress fields are INTERNAL. An advisory progress frame
is emitted only when `downloaded_size` is strictly greater than the last
advisory value; duplicates and regressions are suppressed, not errors. For
each advisory, `total_bytes` is `file.size` when positive, otherwise positive
`expected_size`, otherwise null. Once downloaded size exceeds the displayed
total, total is null for that and later advisory records; it is neither clamped
nor an integrity error.

Destination precedence is explicit `-O`, then `TGCLI_MEDIA_DIR`, then the
frozen client working directory. No tilde expansion occurs. Each parent is
walked descriptor-relative with no symlink traversal. An existing final leaf
of any type, including a symlink, is `OUTPUT_EXISTS`; a symlink or invalid
parent is `OUTPUT_UNAVAILABLE/invalid_path`. The same-directory temporary name
is `.<leaf>.tgcli-download.<32-lowercase-random-hex>.tmp`, created
`O_CREAT|O_EXCL|O_NOFOLLOW` mode 0600.

The TD local source path must be absolute with no empty, dot or dot-dot
component. Starting at `/`, every parent is opened directory/no-follow and the
leaf read-only/no-follow. The leaf must be regular, current-uid owned and no
larger than int53. Before copying, tgcli records device, inode, file type/mode,
size, mtime seconds/nanoseconds and ctime seconds/nanoseconds. Copy reads and
writes exactly that captured size: early EOF is `source_changed`, and one extra
source byte is read without writing to detect growth. Thus even continuous
growth cannot enlarge the private temp beyond captured `st_size`. It then
re-fstats the same descriptor and requires byte-identical metadata plus copied
count equal to both stable sizes. When TD `file.size` is positive it must also
equal the stable copied count; `expected_size` is never integrity proof.

Commit order is copy, source revalidation, temp fsync, serialized deadline/
authorization/cancellation arbitration, existing exclusive no-replace rename
(`renameat2(RENAME_NOREPLACE)` on Linux or `renameatx_np(RENAME_EXCL)` on
macOS), and final-directory fsync. After that fsync succeeds, tgcli always emits
exactly one final progress frame before stdout Result:

```json
{"operation":"download","file_id":7,"downloaded_bytes":4096,"total_bytes":4096}
```

Both byte fields equal the authoritative copied byte count. This record is
mandatory for zero-byte, already-complete and no-advisory downloads and is the
sole permitted duplicate or regression relative to advisory progress. Any
error emits no final record; already emitted advisory records remain visible.
Once exclusive publication and final-directory fsync win, deadline,
cancellation and disconnect cannot suppress the final progress or Result.
Rename `EEXIST` is
OUTPUT_EXISTS; other rename failure is `write_failed`. A normal pre-rename
failure unlinks the temp and fsyncs its directory; cleanup failure replaces the
filesystem error with `cleanup_failed` without exposing the temp name.

Crash guarantees are deliberately limited. A crash after temp creation and
before rename can leave a named 0600 temp, and v1 never enumerates or sweeps
such orphans. After rename but before directory fsync, final persistence after
reboot is unknown and success was not promised. After directory fsync the final
exists; a retry is OUTPUT_EXISTS. Result `bytes` is the authoritative stable
copy count, independent of advisory progress.

Publication uses one acknowledged receive-sequence lease. It briefly blocks
new TD receive-sequence assignment, waits until every earlier assigned event
has reached the nonblocking ordered download queue, drains those events and
then reserves one RequestSession terminal-batch owner. No TD callback is held
across copy, rename or fsync. An update, auth loss, deadline, cancellation or
disconnect ordered before the claim prevents rename; the same event ordered
after the claim loses. Filesystem failure after reservation completes the owned
error terminal. After directory fsync, the owner atomically emits final progress
then Result under one response-sink claim, so shutdown/disconnect cannot insert
an error or suppress either frame between them.

`download` is Read-tier but deliberately performs this local output side
effect. It rejects `--allow-write`, `--yes`, `--dry-run`, any non-unset daemon
write authority, and `--idempotency-key` before Ready or filesystem access.
The frozen cwd must be nonempty absolute valid UTF-8 and at most 4096 bytes.
Capture failure uses the reserved absolute diagnostic
`/.tgcli-cwd-unavailable`, which always produces
`OUTPUT_UNAVAILABLE/invalid_path`. Root `/`, `.`, existing-directory output and
terminal-slash directory intent are valid; terminal separators are removed only
after directory intent is captured. Human progress is exactly
`downloaded N/M bytes\n` with a known total or `downloaded N bytes\n` with a null total,
including the authoritative zero/final record. JSON keeps the normal
`{"progress":...}` wrapper.

#### 4.10.2 Canonical public registry and completions

One deterministic checked-in registry specifies the selected-B final command
tree and whether each path is currently active or future. Runtime CLI and
dispatcher activation may consume only active rows; changing a future row to
active is one atomic handler/dispatcher/schema/catalog change. No generator or
completion asset makes a future row invocable.

`completion bash|zsh|fish` is client-local static output and is a non-DTO meta
exception like `schema`: it reads no config, socket, account, cwd or network and
has no result/error schema. Bash, zsh and fish bytes are generated solely from
the registry, checked in as `completions/tgcli.bash`, `completions/_tgcli` and
`completions/tgcli.fish`, and must be byte-identical across generator output,
runtime output and packaged files. Assets use LF only, contain no timestamp,
version, cwd or environment-derived byte, have exactly one final LF, and never
execute tgcli. Assets contain active registry rows only, so dormant raw is
absent. Atomic raw activation will make its row active and regenerate the
assets to suggest literal `-` and only its accepted flags; `--full`,
`--bot-token`, raw cursor and raw idempotency remain absent.

Completion rejects `--account`, `--json`, `--full`, `--allow-write`, `--yes`,
`--dry-run`, `--timeout`, `--cursor` and `--idempotency-key` with exit 2 before
state access. `--verbose`/`-v`, `--no-daemon`, `--no-color` and nonempty
`NO_COLOR` are accepted byte-preserving no-ops. Missing, unknown or extra shell
arguments exit 2; success writes the exact asset to stdout and leaves stderr
empty.

Top-level final rows are `account`, `chat`, `chats`, `completion`, `contact`,
`daemon`, `doctor`, `download`, `fetch`, `folder`, `history`, `listen`, `login`,
`logout`, `me`, `msg`, `raw`, `read`, `resolve`, `saved`, `schema`, `search`,
`send`, `session`, `storage`, `topic`, `unread`, `version`, and `wait-for`.
Child rows are the exact command surface in §4; aliases remain explicit registry
rows rather than inferred completion text.

`--no-color` and nonempty `NO_COLOR` remain byte-preserving no-ops for these
assets and every v1 renderer because v1 emits no ANSI.

#### 4.10.3 Schemas and catalog activation

The strict self-contained future schemas are frozen under
`docs/schemas/future/`: `search.result.schema.json`,
`chat-info.result.schema.json`, `chat-members.result.schema.json`,
`download.result.schema.json`, `raw.result.schema.json`, and family errors
`search.error.schema.json`, `chat-read.error.schema.json`,
`download.error.schema.json`, `raw.error.schema.json`. The same directory holds
the dormant persistence-only `raw-audit-intent.v3.schema.json`,
`raw-audit-checkpoint.v3.schema.json` and `raw-audit-outcome.v3.schema.json`.
All twelve assets remain validated as Draft 2020-12 golden sources. The five
search/chat-read assets and two download assets are materialized byte-identically
at the schema root, cataloged, embedded and packaged with their active handlers.
Raw command assets remain absent from all three command catalogs, embedded
lookup bytes and packages; raw audit-v3 assets remain persistence-only and never
become command catalog entries.

Search result is exactly items (at most 100 exact shared MessageSummary) and
nonempty cursor-or-null. Chat-info has exactly the 16 fields in §4.4 and a
strict private/basic/supergroup/channel one-of. Chat-members has at most 200
strict rows, preserves the user/chat sender one-of, closed status enum, exact
tag and nullable timestamp. Download result has exact chat/message int53,
positive int32 file id, closed media type, a nonempty absolute path with no
invented schema-length cap, and exact JSON integer bytes in
`0..18446744073709551615`. Raw result is the live/dry one-of in §4.2.

Search errors close usage/auth/bot/resolver/rate/TD/timeout/internal/resource
and pagination reasons `invalid_cursor`, `scope_changed`, `source_changed`,
`marker_not_advancing`, `page_invalid`. Chat-read errors are restricted to
`chat_info|chat_members` and preserve contextual resolver, unsupported-type,
pagination/source-change and common lifecycle shapes. Chat-read and download
both admit the exact resolver terminal `BOT_UNSUPPORTED` with
`{"operation":"resolve"}`; they do not rewrite it to the owning command.
Download errors include
the contextual message `NOT_FOUND`, the closed precondition reasons above,
OUTPUT_EXISTS `{operation,path}`, and OUTPUT_UNAVAILABLE
`{operation:"download",path:absolute_string,reason:...}` with reasons
`invalid_path`, `open_failed`, `write_failed`, `sync_failed`,
`source_changed`, `cleanup_failed`. `cleanup_failed` is the documented normal
pre-rename cleanup/fsync replacement failure above, not a crash-orphan sweep.
Raw errors close denied,
confirmation, rate/TD/timeout/audit and internal reasons without request body,
response body, TD message, credential or private preflight output.

Already-active `chats`, `unread`, `read`, `msg get`, `msg link`, and `fetch`
have exact command-local error schemas and non-stream error-catalog mappings.
Each schema constrains its own operation and contextual detail shapes; sharing a
file must never let one command validate another command's operation. `history`
continues to canonicalize to `read` and has no catalog key. Stream error
authority remains solely `stream-manifest.json`.

Saved contract naming is exact: tags are not Premium-only; search is
Premium-only; the command is `saved search`, internal operation is
`saved_search`, and cursor operation is `saved.search`. `saved attach` maps the
existing M3 write error and `saved.error.schema.json` remains only tags/search.

## 5. Output contract

**No envelopes.** In `--json` mode a successful command prints the result
object itself to stdout:

```json
{"id": 123456, "chat_id": -1001234, "date": "2026-07-02T12:00:00Z", "text": "hi", ...}
```

List-returning commands print `{"items": [...], "next": <cursor|null>}`.
`next` is an opaque, self-contained, account-scoped token: pass it back via
`--cursor` to continue a paginated command; the daemon holds no per-cursor
state. With an unchanged tdlib view the continuation neither repeats nor skips
items, but §4.4's live-view caveats apply when Telegram state changes between
pages. `read` additionally accepts a plain `--before <msg-id>` as the
human-friendly first-page anchor.
Streams print one JSON object per line (NDJSON).

Failures print a single error object to **stderr** and set the exit code:

```json
{"error":{"code":"NOT_FOUND","message":"no chat matches 'dev'","details":{"selector":"dev"}}}
```

- Result schemas are **curated and stable** per command (documented in
  `docs/schemas/`), not raw td_api dumps. v1 rejects `--full`; a raw key in a
  curated result is a post-1.0 contract decision. Schemas use JSON Schema Draft 2020-12 and are listed
  by command in the result-only `docs/schemas/manifest.json`. The pre-freeze
  baseline is self-contained (no `$id`, external references, or `format`) and
  rejects undeclared properties at every object boundary. Commands without a
  result, such as `daemon run`, do not appear in the manifest.
  The M7 `schema` introspection meta-command is the sole result-manifest exception: its
  stdout is a selected schema asset or the fixed multi-kind object in §4.8, not a curated
  application DTO, and it is intentionally not recursively cataloged. `listen` remains
  absent because it produces items rather than a public result.
- Human output renders the same data — no information exists in one mode that
  the other lacks.
- Warnings go to stderr (prefixed `warning:` in human mode, NDJSON
  `{"warning":"<message>"}` objects in `--json` mode).

### Exit codes

| code | name | meaning |
|---|---|---|
| 0 | OK | success |
| 1 | GENERIC | unclassified error (tdlib error details on stderr) |
| 2 | USAGE | bad arguments, ambiguous selector, or unsupported selector type |
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
| `daemon status` (running) | `{"account":"main","running":true,"pid":123,"version":"0.1.0","protocol":3,"socket":"/…/main.sock"}` |
| `daemon status` (absent) | `{"account":"main","running":false,"socket":"/…/main.sock"}` |
| `daemon stop` | the existing M0 object `{"stopping":true}` |
| `daemon restart` | `{"account":"main","restarted":true,"pid":124,"version":"0.1.0","protocol":3,"socket":"/…/main.sock"}` |

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
retaining `unknown` before the first snapshot. The successful `daemon stop`
object is otherwise unchanged.

The `version` success data contains exactly the required `version`, `protocol`
and `tdlib` fields plus an optional `commit` string. When present, `commit` is
the abbreviated lowercase hexadecimal object name returned for `HEAD`, with a
minimum length of seven hexadecimal digits and a literal `-dirty` suffix iff
tracked index or work-tree state differs from `HEAD`. Untracked files alone do
not add the suffix. The key is absent, never null or empty, when a trustworthy
identity cannot be established.

A build has a trustworthy identity only when Git is available, the canonical
top-level work-tree path is the canonical CMake source path, `HEAD` resolves to
a commit with a valid abbreviation of at least seven lowercase hexadecimal
digits, and tracked status inspection succeeds. Failure of any check, including
status inspection, omits `commit`; a status failure is never interpreted as a
clean tree. This root equality excludes a source tree nested inside an unrelated
checkout. Exact tags do not suppress the revision: clean and dirty tagged
checkouts report the same abbreviated `HEAD` forms as untagged checkouts. A
detached shallow checkout with no tag refs is also reportable because neither
history nor tag discovery is required.

This field is diagnostic build identity, not release authority. Release
verification requires a present, clean abbreviation that is a prefix of its
independently established full source commit, while the full commit/tree
identity in release provenance remains authoritative. `commit` is not folded
into `version`, the binary version handshake, a protocol-envelope field or an
audit field, and it does not appear in `doctor`, `daemon status` or
`daemon restart` results.
Human output is exactly
`tgcli 0.1.0 (4d7ca6e, protocol 3, tdlib 1.8.65)` when present and
`tgcli 0.1.0 (protocol 3, tdlib 1.8.65)` when absent; a dirty value is rendered
unchanged, for example `4d7ca6e-dirty`.

Every M1 failure uses the single envelope
`{"error":{"code":<string>,"message":<string>,"details":<object>}}` on
stderr. `message` is explanatory text; branching uses `code`, exit status and
the exact detail shape below. Each `details` object contains exactly the shown
keys and no others. `string[]` arrays are bytewise sorted unless the shape says
otherwise. No secret, hook command, answer, raw argv, tdlib message or
unredacted tdlib request is permitted in any field.

`state` is `unknown` or one of the 13 tgcli state names in §8. For an M1
failure, `operation` is exactly `auth_bootstrap`, `login`, `logout`, `me`, `account_add`,
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

For M1, the auxiliary enums are closed: `usage_reason` is `missing_argument`,
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
the eleven AuthBootstrap functions listed in §6, `getMe`, `logOut`, `close`, or
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

### 5.2 M2 cursors, errors, and schemas

The cursor continuation forms are exactly:

```text
tgcli chats --cursor TOKEN
tgcli read --cursor TOKEN
tgcli history --cursor TOKEN
tgcli search --cursor TOKEN
tgcli chat members --cursor TOKEN
```

Except for the exact Saved Messages redundancy rule in §4.3, a continuation
accepts no selector, query, filter, `-n`, or `--before`; all read state comes
from the token. A cursor is canonical unpadded base64url JSON. It has no MAC,
authentication tag, signing key, or daemon-side state. It contains a version, canonical operation, routed account,
current `getMe.id`, page size, all normalized scopes/filters/resolved ids, and
the complete upstream continuation or keyset.

The version-1 `read` cursor contains exactly:

```json
{"version":1,"operation":"read","account":"main","user_id":42,"limit":20,"chat_id":-1001,"history_chat_id":-1001,"topic":null,"local":false,"since":null,"until":null,"since_cutoff_message_id":null,"from_message_id":123}
```

`user_id` is positive int53; `chat_id`, `history_chat_id`, and `from_message_id`
are nonzero int53; `limit` is 1 through 100; `topic` is exact `TopicRef` or null;
`local` is boolean;
`since` and `until` are the normalized signed-int32 whole seconds or null; and
`since_cutoff_message_id` is the exact nonzero int53 cutoff id or null. Local
cursors always have a null cutoff. A non-null cutoff requires non-null `since` and
`local:false`. `history_chat_id == chat_id` unless the topic kind is `thread`; a
local cursor always has equal chat ids. The raw `from_message_id` is the last raw
message consumed, not the last output match. Original selector spelling,
`--before`, resolved-link context, the metadata `history_thread_id`, and original
timestamp strings are absent.

An emitted cursor records the state after first-page metadata/date probes completed;
an accepted unsigned caller token proves no provenance and is only untrusted explicit
read input. Continuation does not repeat date probes; it applies the supplied
normalized filters and cutoff, while live thread metadata is independently
re-derived as above. The exact object field set, scalar ranges, cross-field rules,
canonical JSON serialization and canonical unpadded base64url re-encoding are
validated before target dispatch.
`history` accepts and emits this same `operation:"read"` token and never has an
alias-specific cursor.

The token is untrusted read input, not a security capability. A structurally
valid same-account token that a caller manually changes within the accepted
schema represents a new explicit read request and is accepted; tgcli does not
claim tamper detection. A new persisted signing key is not created. A malformed,
noncanonical, or schema-invalid token is `USAGE`/`invalid_cursor`; an operation,
account, or current-user mismatch is `USAGE`/`cursor_scope_mismatch`. A valid
upstream marker that repeats instead of advancing is `PAGINATION_INVALID`, not
an unchanged cursor or false exhaustion.

M2 failures other than the already specified Saved namespace use the standard
strict envelope
`{"error":{"code":string,"message":string,"details":object}}`. The closed
M2 `operation` enum is `chats`, `read`, `msg_get`, `msg_link`, `search`,
`unread`, `fetch`, `resolve`, `chat_info`, or `chat_members`; `history` is
always `read`. Exact common shapes are:

| error code | exit | exact `details` |
|---|---:|---|
| `BOT_UNSUPPORTED` | 2 | `{"operation":operation}` |
| `TDLIB_ERROR` | 1 | `{"operation":operation,"tdlib_code":integer}` |
| `RATE_LIMITED` | 5 | `{"operation":operation,"tdlib_code":429,"retry_after":integer}` |
| `TIMEOUT` | 7 | `{"operation":operation,"state":nullable_state}` |
| `PAGINATION_INVALID` | 1 | `{"operation":operation,"reason":"non_advancing_upstream"}` |

Before RequestSession construction, config-admission expiry for every M2
command, including fetch, is exactly
`{"operation":"config_admission","state":null}`. Daemon socket and no-daemon
paths emit the same terminal bytes. Fetch retains its separate
`{"operation":"fetch","state":nullable_state}` branch only after
RequestSession construction. Every M2 family additionally accepts the existing closed
account/config routing, daemon-shutdown, protocol-answer and prior removal/
audit recovery terminals without changing their detail shapes. Resolver work
continues to attribute selector/enrichment TD, rate and internal failures to
`operation:"resolve"`; the parent command operation is not substituted.

Fetch common TIMEOUT details are:

```json
{"operation":"fetch","state":"ready"}
```

`state` is nullable. Config-admission expiry uses this shape with null before
RequestSession exists. After recovery completes and before target resolution, a
fetch-only coordinator adapter rewrites only `ResolverTimeoutError` from
principal binding or resolution into this shape with the current state. Every
other ResolverError is emitted unchanged, including accepted `operation:"resolve"`
attribution on selector TDLib/rate-limit/internal failures.

Between RequestSession construction and successful completion of both recovery
preflights, fetch TIMEOUT is impossible. Recovery failure/expiry has the exact
taxonomy and ordering in §4.6.3/§6 and sends zero fetch Ready/getMe/resolver/
history calls.

Immediately after target resolution, timeout uses the extended form:

```json
{
  "operation":"fetch",
  "chat_id":-1001,
  "phase":"network_fill",
  "state":"ready",
  "cached_count":250,
  "oldest_message_id":123,
  "resume_from_message_id":123
}
```

`local_scan` begins before the optional since probe and remains through the
zero-progress response sealing the local boundary. A latched target does not
change phase or defeat a deadline before that boundary. `network_fill` begins
only before its first live history call. Count and boundary contain only pages
incorporated before the deadline event. Boundary ids use the same numeric
optional state as the result. A losing late response may still warm TDLib and is
rediscovered later, but is absent from these details. Authorization loss and
cancellation retain their own terminals/stop behavior.

M2 target-not-found details are contextual and closed:

| context | exact `NOT_FOUND.details` |
|---|---|
| ordinary chat/user/link resolver, including a numeric `getChat` context returning 400 | `{"selector":string}` |
| any local resolver miss | `{"selector":string,"scope":"local_materialized"}` |
| `msg get` null positions | `{"chat_id":integer,"missing_ids":integer[]}` |
| `msg link` message 404 | `{"chat_id":integer,"message_id":integer}` |
| missing topic target | `{"chat_id":integer,"topic":<TopicRef>}` |
| missing numeric folder scope | `{"folder_id":integer}` |

For username resolution, lifecycle/auth loss wins first and tdlib code 429 is
`RATE_LIMITED`. After those checks, a returned code 400 maps to `NOT_FOUND`
with the original selector only when its message is exactly
`USERNAME_NOT_OCCUPIED` or `USERNAME_INVALID`. This applies to
`searchPublicChat` and username-bearing link resolver branches; comparison is
case-sensitive equality with no substring or normalization. Any other actual
tdlib 400 is `TDLIB_ERROR` with `operation:"resolve"`. `USAGE` is reserved for
local parse/validation failure before tdlib dispatch. Documented null/404
results use the contextual table; tgcli does not otherwise parse tdlib message
text to invent error categories.

M2 extends `usage_reason` with `invalid_cursor`, `cursor_scope_mismatch`,
`unsupported_chat_type`, and `unsupported_link_type`. It uses the existing
`missing_argument`, `invalid_argument`, and `mutually_exclusive` reasons for
ordinary CLI validation. `AMBIGUOUS` is exit 2 and has the chat/user candidate
details defined in §4.1. A successful `local_boundary` or `tdlib_idle` is not
an error.

Implementation of the remaining M2 commands adds the following strict Draft
2020-12 result schemas and result-only manifest keys:

| schema file | manifest command key |
|---|---|
| `chats.result.schema.json` | `chats` |
| `read.result.schema.json` | `read` |
| `msg-get.result.schema.json` | `msg get` |
| `msg-link.result.schema.json` | `msg link` |
| `search.result.schema.json` | `search` |
| `unread.result.schema.json` | `unread` |
| `fetch.result.schema.json` | `fetch` |
| `resolve.result.schema.json` | `resolve` |
| `chat-info.result.schema.json` | `chat info` |
| `chat-members.result.schema.json` | `chat members` |

`history` has no manifest entry or schema because it canonicalizes to `read`.
`read.result` is self-contained and has exactly `items`, `next`, and `boundary`.
It includes the exact shared `MessageSummary`/tagged-topic definitions, permits zero
through 100 items, and uses a strict one-of: `boundary:"page"` requires a nonempty
cursor string, while `time_anchor`, `empty_before_until`, `local_boundary`, and
`tdlib_idle` require `next:null`. Unknown properties are rejected at every object
boundary.
`fetch.result` has nullable `target_reached`, `oldest_message_id`, and
`resume_from_message_id`, and has no `complete` or `history_end`.
`msg-get.result.schema.json` has one through 100 exact `MessageSummary` items
and `next` is exactly null. `msg-link.result.schema.json` has exactly
`chat_id`, `message_id`, `link`, and `is_public`; its ids are nonzero int53,
`link` is a string with `minLength:1` and no pattern, and `is_public` is
boolean. Human output renders the same fields and matches the exact goldens
above. Actual JSON data is validated against these strict schemas.
The active `chats`, `unread`, `read`, `msg get`, `msg link`, `search`,
`chat info`, `chat members`, and `fetch`
commands have exact command-local error schemas and non-stream error-manifest
mappings. `history` canonicalizes to `read` and therefore has no separate key.
The search and chat-read golden schemas remain under `future/` as deterministic
generator inputs and are required to match their active root copies byte for
byte.

M2 fake-boundary contract coverage must include:

- invalid ids, limits, topic kinds/ranges, timestamps/combinations and cursors
  before the selected target request; cross-command/account/user cursor scope;
- mid-word title matches absent from `searchChats`, full Main+Archive loading,
  local materialized-prefix misses, ambiguity truncation/order, and a deadline
  before domain completion producing no partial resolver result;
- bot preflight for every matrix row, including a bot invite trace with
  `getInternalLinkType` but no `checkChatInviteLink`, and user invite resolution
  with no join;
- exact username 400 normalization after auth/429 precedence and a nonmatching
  tdlib 400 remaining `TDLIB_ERROR`;
- sparse `chats --unread` beyond 100 raw chats, equal-order ties, continuation
  after restart and an anchor that moved or disappeared;
- exclusive `--before`, inclusive fractional/date-only time bounds, an
  out-of-order-date page that cannot terminate early, every topic kind, and
  local reads that issue no network-capable topic/date/link request;
- the closed timestamp and topic lexical grammars, arbitrary finite fraction
  lengths under the frame bound, mathematical rounding on both sides of epoch,
  checked zone/relative/calendar arithmetic, date-only edges, `since > until`,
  signed-int32 endpoints, and the no-subtraction `since == INT32_MIN` branch;
- the complete local t.me byte table: both exact prefixes, username/start/id/token
  boundaries, case, malformed scheme/host/port/userinfo, percent/non-ASCII,
  empty/extra segments, trailing slash, query duplication/extras and fragments;
  PublicChat/BotStart hits and misses; Message/Invite/Direct local misses; other
  structurally valid types as unsupported; zero TD calls for malformed/unsupported
  input; and zero `getInternalLinkType` or other network-capable calls for every
  locally classified link; ordered `InvalidLink` versus title examples at the
  initial classifier and byte-identical non-local baseline classification;
- exact until-then-since-then-history call order; `--before` plus `--until`
  existence-probe semantics without id/date comparison; inclusive-until versus
  exclusive-before/cursor anchor removal; both probes' exact response type,
  history-chat, nonzero-id/date and no-later-than-requested integrity; 404 probes;
  zero/wrong-chat/too-new/malformed failures; and ignored resolver context;
- one exact live `getMessageThread` call per thread invocation before date/history,
  native factory/matcher descriptor fields, strict neutral metadata/start-message
  conversion with unused reply/unread/draft fields discarded, same-chat identity,
  channel to linked-supergroup remapping with a different root id, original history
  call arguments,
  returned-message validation against `history_chat_id`, continuation revalidation,
  local supergroup same-chat behavior, and local channel `unsupported_mode` with no
  metadata/history call;
- shared scanner injection and every raw/anchor/filter/wrong-chat check using
  `history_chat_id`, with a cross-chat fixture that would fail if source `chat_id`
  were used and non-cross fixtures proving exact equality;
- emitted post-probe cursor state versus fabricated/modified unsigned input with no
  provenance claim, exact `history_chat_id` field/cross-field rules, live-thread
  scope mismatch, alias identity, and no repeated first-page date probes;
- exact Saved-topic ownership success, cached materialization reuse, wrong-chat
  refusal, malformed/TD error mapping, cursor revalidation, and no other topic
  making `createPrivateChat`;
- filtered read pages with raw progress and zero items returning an advancing
  `page` cursor, followed by a zero-progress terminal boundary with null
  `next`; one-response live idle; local since always ending at
  `local_boundary`; short-page continuation; count/vector integrity; empty,
  anchor-only and null-only zero progress; mixed-null integrity failure; and
  repeated/equal/increasing ids producing `PAGINATION_INVALID`;
- the typed `ResolverConsumer` bind/resolve success/error/stop variants without
  a terminal, `read_target` retaining `ReadyReadStatus::Cancelled`, one
  principal binding, Ready/getMe/selector/target attribution, reuse of the same
  absolute deadline, later M3/M4 caller attribution, and the public `resolve`
  adapter's one terminal;
- `msg get` argv order/duplicates, one exact `getMessages` call, atomic mixed
  found/missing behavior, unique first-occurrence missing ids, wrong vector
  length, wrong positional chat/id, invalid non-null DTO precedence over a
  simultaneous null, and no partial stdout; plus
  the exact `msg link` tdlib call and contextual errors;
- a non-empty valid-UTF-8 message link, empty and invalid-UTF-8 link integrity
  failures, no URL-pattern rejection, bot-positive numeric/public/message
  selectors, selector-specific bot-negative branches, and secret rejection
  before the target call;
- for each of `msg get` and `msg link`, real-Dispatcher recovery fixtures with
  both preflights installed: an unresolved removal stops at
  `REMOVAL_INCOMPLETE` before logout recovery and with zero Ready/getMe,
  resolver, target, or other tdlib calls; a clean removal followed by an
  unresolved logout stops at `AUDIT_INCOMPLETE` with the same zero-call proof;
  and a clean trace orders removal recovery, logout recovery, Ready, `getMe`,
  resolver, then the command target call;
- those Dispatcher fixtures validate the exact §5.1 terminal envelopes and
  exit 1: `REMOVAL_INCOMPLETE.details` has exactly `account`, `path`,
  `invocation_id`, `stage`, `completed_stages`, and `reason`, while
  `AUDIT_INCOMPLETE.details` has exactly `account`, `path`, `mutation_state`,
  and `completed_stages`; the removal branch proves the logout preflight was
  not entered, and neither failure emits result/stdout;
- exact result-schema validation and result-manifest entries in M2, with no msg
  error-schema or error-catalog expectation before the named M7 task;
- multi-page global sender/text search through matches/exhaustion, full marker
  preservation and repeated-marker rejection;
- Main/Archive unread deduplication, secret exclusion, exact chat-info source
  branches, member user/chat senders, and member empty-probe exhaustion without
  trusting approximate totals;
- fetch with a disconnected cached island below the public local boundary,
  every target shape and runtime truth-table row, since probe before history,
  `INT32_MIN`/404 cutoffs, the local latch model including deadline after latch
  but before boundary, since-over-limit precedence across different pages,
  empty-prefix nulls, page overfetch, missing anchors, concurrent messages above
  the frozen top, and live `tdlib_idle`; inherited read-page integrity failures;
  checked-count overflow; exact progress cadence/wrapper bytes; pre-target and
  both extended timeout phases; fetch config-admission timeout with null state
  while other commands retain config_admission; one tagged config → session →
  removal → logout → handler handoff with no recomputation; the complete
  REMOVAL_INCOMPLETE/AUDIT_UNAVAILABLE/AUDIT_INCOMPLETE recovery taxonomy and
  zero fetch calls; the fetch-only ResolverTimeoutError rewrite with every other
  resolver failure unchanged; direct malformed-timeout id-0 framing versus
  CLI/no-daemon validation; prior-outcome persistence permitted but zero current
  fetch audit group or fetch-owned cursor/store/spool/media artifacts; ordinary-
  schema negatives for every Draft-expressible count/null/stop/target branch and
  runtime pairs only for numeric id equality, count-vs-limit, observed cutoff
  and coordinator history; timeout followed by a repeated TDLib-only resume; no
  fetch error-catalog mapping; and no additional TestDC flow or skip beyond the
  existing M2 `chats` gate; and
- strict schema validation of actual result data plus human golden output with
  the same information.

## 6. Safety model

Every tdlib send is admitted by one daemon-side chokepoint (§7 `safety`).
Command handlers statically declare `Read`, `AuthBootstrap`, `Write`, or
`Destructive`; the user-authority tiers remain Read/Write/Destructive:

- **Reads** — always allowed, no grant needed.
- **AuthBootstrap** — grant-exempt but not unrestricted. It admits only
  `getAuthorizationState`, the generation-owned `getCurrentState`,
  `setTdlibParameters`,
  `setAuthenticationPhoneNumber`, `requestQrCodeAuthentication`,
  `checkAuthenticationBotToken`, `setAuthenticationEmailAddress`,
  `checkAuthenticationEmailCode`, `checkAuthenticationCode`, `registerUser`,
  and `checkAuthenticationPassword`. Each function must match the current
  pinned auth state, client generation, auth owner and secret-source rules in
  §8; anything else is `AUTH_FUNCTION_DENIED`. Each generation reserves query
  id 1 for `getAuthorizationState` and query id 2 for its sole
  `getCurrentState`; both use that generation's internal auth owner and
  Unknown snapshot. The current-state query has no public request-owner entry
  point. The initial state queries and configured-credential bootstrap use an
  internal auth owner, while
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
- **Destructive** (msg delete, chat leave/ban/kick/invite-link, session
  terminate, folder delete, storage optimize, logout, account remove) —
  requires a write grant *and*
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
`account list|add|use` do not inspect a per-account logout audit.

M2 extends, but does not otherwise broaden, that closed preflight surface.
The accepted selected-account reads `saved tags`, `saved search`, `resolve`,
`chats`, `msg get`, `msg link`, and `fetch` run both the global account-removal
tombstone preflight and the selected account's logout-audit preflight before
Ready/getMe or any command-specific tdlib request. Removal runs first:
recognized incomplete state is `REMOVAL_INCOMPLETE`, while invalid or unreadable
removal journal/state is `AUDIT_UNAVAILABLE`; either owns the terminal and logout
recovery is not entered. After clean removal, logout recovery runs: recognized
incomplete/unresolved state, including deadline during prior-logout observation,
is `AUDIT_INCOMPLETE`; invalid or unreadable logout audit with no recognized
incomplete group is `AUDIT_UNAVAILABLE`. Every branch uses its existing exact
§5.1 details/exit and sends zero fetch Ready/getMe/resolver/history calls.

Recovery may append/sync an outcome for a prior invocation; those bytes belong
to the prior group. Fetch creates no current audit group and no fetch-owned
persistence. Tests assert absence of fetch-owned cursor, audit group,
idempotency, spool and media artifacts, not zero audit access or recovery writes.
No other M2 command is admitted by this paragraph: canonical `read`, including
`history`, remains excluded from both lists. Central tests pin fetch membership,
read/history exclusion and removal-before-logout order.

For each
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

- **`--dry-run`** — performs Ready/getMe principal binding, bot preflight,
  caller-input parsing and hashing, exact resolution/property validation and
  immutable planning, then returns `{"dry_run":true,"plan":...}`. It needs
  no write grant or `--yes`, performs no confirmation, and writes no tgcli
  config, general audit, idempotency store or spool. It calls no
  Write/Destructive TD API and makes no Telegram-side mutation. The closed TD
  read allowlist is:

  ```text
  getMe, getChat, getUser, getSupergroup, searchPublicChat,
  getInternalLinkType, getMessageLinkInfo, checkChatInviteLink,
  getSupergroupFullInfo, createPrivateChat,
  getMessage, getMessages, getMessageProperties, getOption,
  getMessageAvailableReactions, parseTextEntities
  ```

  `createPrivateChat` is allowed only as
  `createPrivateChat(me.id,false)` for Saved materialization; `getOption` is
  allowed only for `"unix_time"`.

  `getUser` and `getSupergroup` are not general dry-run escape hatches. They
  are admitted only by the shared exact-only chat resolver while constructing
  an exact M2 `ChatIdentity` from a chat object already returned in the same
  request. For `chatTypePrivate(user_id)`, exactly `getUser(user_id)` may
  supply `usernames.active_usernames` and `is_bot`. For
  `chatTypeSupergroup(supergroup_id,is_channel)`, exactly
  `getSupergroup(supergroup_id)` may supply `usernames.active_usernames` and
  must confirm the same `is_channel` value. A basic-group identity performs
  neither call. A secret or unknown chat type fails before either call. A
  returned identifier mismatch, kind mismatch, malformed username, null
  object, or unknown variant is `INTERNAL`; tgcli never substitutes empty
  usernames or `is_bot:false`. Only the exact `ChatIdentity` fields are
  retained in the plan/audit/store. These functions may update only TDLib's
  cache/database and never authorize a Telegram-side mutation.

  Each admitted identity read uses §4.5.9's receive-event arbitration. A
  same-generation later Ready snapshot is not a terminal competitor and does
  not invalidate or retry an eligible correlated response. The response
  survives whether the later Ready event's receive sequence is before or after
  the response sequence; only an earlier eligible first non-Ready event wins.
  A response observed at or after the absolute deadline loses to the deadline
  and is never retried because Ready advanced. Retrying the identical function
  and entity id is permitted only when the lower descriptor/auth gate rejected
  the attempt with `GenerationMismatch`, `AuthSequenceMismatch`, or
  `GenerationClosed` before a TD request crossed the fake boundary, and the
  newest eligible snapshot is Ready and strictly supersedes the rejected
  descriptor. Such pre-boundary supersession may repeat only until the same
  deadline or cancellation. A TD response, TD error, null/unknown object,
  integrity failure, or generic exception is never retried under this rule.

  Each of the seventeen §4.5.1 M3/M4 planner dry-runs performs zero tgcli
  persistence mutation: no config, general audit, idempotency store, spool, or
  other tgcli write, and no invocation of prior-group reconciliation that can
  append or sync a recovery record. DESIGN §4.7.5's distinct M6 session
  prior-group-recovery paths (`session list` and
  `session terminate --dry-run`) are the sole explicit reconciliation
  exception; neither is one of these seventeen planners, and they do not
  weaken or generalize this rule.

  Local file open/stat/read/SHA-256 is allowed. These reads may access Telegram
  and mutate only TDLib's internal cache/database; any other TD function is a
  contract-test failure. M1
  `logout` and `account remove` dry-runs retain their client-local/no-spawn
  behavior. M3/M4 write dry-runs are auth-bound: an absent daemon is spawned,
  while `--no-daemon` creates an isolated in-process client.
- **Audit log** — M3 applies the intent/outcome record pair to its general
  write kernel. In M1 it applies only to actual `logout` and `account remove`
  execution. Normalized
  non-secret arguments and resolved targets are recorded; secret-bearing
  fields are always excluded. Files rotate by size (§9).
- **`--idempotency-key <k>`** — after durable audit intent, a locked
  insert-if-absent records only the domain-separated key hash, complete
  request fingerprint, immutable plan and quota reservation as pending before
  dispatch. A matching completed entry replays its stored terminal with the
  current transport id and no Telegram call; destructive replay first
  reconfirms the stored plan. A matching pending entry returns
  `IDEMPOTENCY_PENDING`; a different fingerprint returns
  `IDEMPOTENCY_CONFLICT`. The write gate precedes lookup and replay. Entries
  expire at exactly 604800 seconds without automatic resend. Exact quota,
  transition, crash-repair and audit-pinning rules are §4.5.7.

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
  requests, subscription multiplexing (up to 32 `listen`/`wait-for` clients
  over one update bus; §4.6.6). Every registered descriptor has a closed
  operation identity. A handler exception or a return without a terminal is
  contained as `INTERNAL/1` with exact
  `{"operation":<descriptor operation>,"reason":"internal_error"}`; raw
  exception text is never exposed. Current cataloged families use their exact
  strict error-schema operation. The local meta commands use exact `version`
  and `doctor` operations in their strict meta error schema. M3/M6 use their
  canonical operation tables, registered noncatalog descriptors use
  `dispatcher`, and an unregistered command remains `USAGE` without entering
  a handler.
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
conversion layer (`td_api_json`) for the selected-B `raw` parser/typed conversion.
That conversion layer is generated tdlib-internal code, not part of the
installed public interface, so tgcli supports exactly one tdlib provenance:
the pinned source revision — built via FetchContent, or preinstalled into a
prefix by `scripts/build-tdlib.sh`, which exports the extra headers
raw needs (§13). Arbitrary distro tdlib packages are not
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
  is `getAuthorizationState`. The sole pre-snapshot exception is the core-owned
  query id 2 `getCurrentState`, sent immediately after query 1 by the same
  generation-owned bootstrap path. Query 2 is private: its response may settle
  before query 1, but remains generation-local and may only populate internal
  update-derived state. It cannot authorize public work, publish authorization
  or stream readiness, or open the ordinary-query barrier. The query 1 response
  or the generation's first `updateAuthorizationState` remains that barrier;
  no other ordinary query is acted on before it. The generation begins at
  `auth_sequence=0` with no accepted update observed. In response-first order,
  the query 1 response installs its state and increments the sequence to 1 only
  if that condition still holds; the first later update, even if payload-equal,
  increments it to 2. In update-first order, the update installs sequence 1 and
  the later query 1 response is consumed for query correlation but ignored for
  state and sequence. Once any update has been accepted, no bootstrap response
  can install an authorization snapshot.
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
  arbitrary chat history, has no server-side global search and no contacts.
  The exact M2 per-command and per-link matrix is in §4.4 and §4.1. Ready and
  `getMe` precede bot classification; `BOT_UNSUPPORTED` is emitted before any
  tdlib function carrying `CHECK_IS_USER`. Bots remain supported for the
  narrow message/info/member reads and link classes named by that matrix.
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
  versions itself.
  tgcli never defines a Telegram message schema or message-cache migration.
  Its own persistent state is deliberately bounded to config.toml, append-only
  audit, the versioned idempotency store, removal tombstones, rotated logs, and
  the private crash-recoverable outbound attachment spool. The spool is
  temporary send staging governed by §4.5.12, not a message cache; TDLib's
  database remains the only Telegram cache.
  The
  per-account daemon (§10) keeps running by default, so the local DB absorbs
  updates continuously; `listen` is a live view over that stream, not the
  persistence mechanism.
- **Local-only reads**: `--local` on `read` sets the `only_local` flag on
  `getChatHistory` — offline mode for agents that must not hit the network.
  Topic-local reads use that same function plus exact tagged `TopicRef`
  filtering; they never call a topic-history or date-anchor function.
  There is deliberately no `--local` on `search`: tdlib exposes no
  local-only search for regular chats (`searchChatMessages`/`searchMessages`
  are server-side), and tgcli does not pretend otherwise; offline filtering
  over prefetched history is a post-1.0 idea. `tgcli fetch` is the
  deliberate warming path described exactly in §4.4. Public tdlib exposes the
  continuous local prefix from newest history, but does not label its boundary
  as a gap or the true oldest message and does not certify remote EOF. Fetch
  therefore resumes from that public boundary, ignores disconnected cached
  islands below it, and reports `tdlib_idle` rather than `complete` or
  `history_end` when live history stops advancing.
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
~/.local/state/tgcli/accounts/<name>/          audit.log, idempotency.db,
                                               spool/, tdlib.log
~/.local/state/tgcli/removals/                 removal audit + durable tombstones
~/.local/state/tgcli/removals/.<name>.lock     stable removal/daemon exclusion gate
$XDG_RUNTIME_DIR/tgcli/<name>.ctl              bootstrap stop socket
$XDG_RUNTIME_DIR/tgcli/<name>.sock             daemon socket
```

When `XDG_RUNTIME_DIR` is unset (macOS, most containers and CI sandboxes),
the socket falls back to `$TMPDIR/tgcli-<uid>/<name>.sock` (then
`/tmp/tgcli-<uid>/`), a `0700` directory whose ownership is verified before
use. Socket paths must fit `sun_path` (~104 bytes); account names are
length-validated accordingly. `audit.log` and `tdlib.log` rotate by size
(default 32 MiB, keep 4).

`spool/` is the private crash-recoverable outbound-attachment staging area
defined by §4.5.12. It is tgcli persistent state but not a message cache;
TDLib's database remains the only Telegram cache.

The `.ctl` endpoint and `<account-state-dir>/daemon.lock` form the bootstrap
compatibility surface described in §10. They are account-scoped even though
they are not part of the main JSONL protocol.

`daemon.lock` is also the cross-process lifetime lease for the per-account
audit and idempotency state. Those components share exactly one deadline-aware
outer account operation mutex inside the lease owner. A real M3/M4 request uses
the exact operation-specific initial and commit epochs in §§4.5.2, 4.5.7 and
4.5.12. M4 prior reconciliation, pass 1 and lookup are in the initial epoch. A
miss creates the only planning gap. Commit repeats the core gate/lookup, then
returns incumbent replay/pending/conflict before a current intent; only a miss
performs proposed-plan confirmation, config CAS, append permit, intent,
generation-bound quota and insert. An unexpected insert loss is invariant-
fatal, not an ordinary race. M4 pass 2 and every mutating TD dispatch/wait are
inside the continuous commit epoch through cleanup. Two callers may observe an
initial miss, but the second commit lookup observes the first caller's state and
creates no group. No audit groups interleave, and there is no inner store mutex.
The contract makes no blanket claim that network/file hashing is outside the
mutex. The typed lease revalidates its descriptor identity and
metadata. Waiting for the epoch shares the absolute monotonic deadline;
deadline/cancel wins before any acquired spool/root/store/audit observation.
Once acquired, scanning and pre-intent work use the same deadline and a begun
durability operation completes required fsync before deadline handling resumes.
No `.audit.lock`, store-lock path or other audit lock path is created. Audit
inspection/rotation/appends fail `lock_failed` without a valid lease, except
pure parsing tests over supplied bytes.

The hidden removal gate uses the same verified `0600` regular-file and
dual-lock mechanics as `daemon.lock`, but is not a control endpoint. A daemon
holds it for its lifetime; no-daemon execution and an actual local removal hold
it for the whole operation. It remains outside the deletable account roots and
is reused rather than removed. Dry-run neither creates nor acquires it.

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
DB open per command), safe concurrent ordinary invocations, up to 32 simultaneous
`listen`/`wait-for` subscribers, and a local DB that stays continuously warm
(§4.6.6).

- **Exact routing and auto-spawn.** Account client-local commands `login`,
  `logout`, `me`, the normal `doctor` probe, and M3/M4 write dry-runs route to
  the selected account daemon and auto-spawn it when absent; `doctor` uses its
  existing local result only if connection/spawn is unavailable. M1 `logout`
  and `account remove` dry-runs retain their client-local/no-spawn behavior.
  `account remove <name>` routes only to the positional target:
  default removal auto-spawns that target only when it must inspect/revoke a
  possible remote session; `--keep-session` never spawns and instead quiesces
  the target if it is already running. `account add|list|show|use`,
  `daemon status` and `daemon stop` are config/control-global and never
  auto-spawn an account daemon. `daemon restart` is the sole control operation
  that starts an absent daemon. `daemon run` stays in the
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
- **Protocol.** Main protocol version 3 is JSONL over a `SOCK_STREAM` unix
  socket. Every normal Request frame has exactly the six top-level fields
  `type`, `id`, `account`, `command`, `args`, and `context`; no extension
  fields are accepted. Client and daemon each send Hello as the first frame in
  their direction, with the frozen exact encoding:

  ```json
  {"type":"hello","binary_version":"0.1.0","protocol_version":3}
  ```

  No normal Request or Answer is sent before an exact v3 Hello match. A v3
  Request is, for example:

  ```json
  {"type":"request","id":42,"account":"work","command":["me"],"args":{},"context":{"tty":false,"json":true,"yes":false,"dry_run":false,"timeout":60,"cwd":"/srv/agent","media_dir":null,"write_authority":"unset","idempotency_key":null}}
  ```

  `account` is the already-resolved routed account name, as a top-level string;
  it is never supplied through `args` or `context`. It must satisfy §11's
  account-name grammar during strict frame parsing. A missing `account`, an
  unknown top-level field, a non-string `account`, or a syntactically invalid
  account makes the whole v3 Request a malformed protocol frame: the parser
  does not produce a Request, the server sends the existing connection-scoped
  `USAGE` error with frame id 0 and exact empty details, and the server closes
  the connection. This path never becomes `ACCOUNT_NOT_FOUND` or
  `ACCOUNT_MISMATCH`.

  Strict frame parsing does not establish connection-sequence eligibility. A
  syntactically valid Request received before the client has completed a
  matching v3 Hello is rejected by the existing Hello-first sequence gate with
  connection-scoped `USAGE`, frame id 0, exact empty details and connection
  close. That gate runs before account comparison, so even a pre-Hello Request
  whose valid `account` differs from the daemon is not `ACCOUNT_MISMATCH`.

  The remaining Request fields are the uint64 id, command path, normalized
  args, and a strict client context with exactly nine fields: TTY-ness,
  `--json`, `--yes`, `--dry-run`, timeout, the client's cwd,
  `TGCLI_MEDIA_DIR` (for output paths), a **tri-state write-authority field**
  (`grant` / `deny` / `unset`), and `idempotency_key`. The key is null or
  ASCII matching `^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$`; `--dry-run` with a key
  is `USAGE`/`mutually_exclusive`, and a non-null key outside the M3/M4
  idempotent allowlist is `USAGE`/`unsupported_mode`. The client folds
  `--allow-write` and `TGCLI_ALLOW_WRITE` into the write-authority field,
  because the daemon cannot see the invoking shell's environment, and the
  daemon combines it with the account config per §6 (explicit deny > any grant
  > default deny). Response frames are `result`, `error`, `item` (streams),
  `progress`, and `challenge`. Every request and response frame is
  compact-serialized and admitted under the shared `P=16,842,751` whole-frame
  bound from §4.5 before LF or any observable delivery; socket and in-process
  paths use the same predicate. Interactive flows
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
  startup. Only after a matching v3 Hello and the existing connection-sequence
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
  versions from both sides. Protocol v2 was the first main-protocol version
  whose Request carried the required routed `account`; v3 retains the same
  six top-level fields and adds only the ninth strict context field
  `idempotency_key`. The v1/v2 parsers do not accept a v3 Request, the v3
  parser does not accept a v1/v2 Request, and frames or curated schemas are
  never mixed across versions.

  Frozen `tgcli-lock-v1` and the authenticated datagram `.ctl` contract below
  are unchanged and mandatory for v1→v3, v2→v3, v3→v1 and v3→v2 replacement.
  A mismatched client sends no normal Request in the foreign dialect. After
  verifying the old owner, it stops that owner only through the frozen
  token-stop path. One monotonic deadline covers stop, disappearance of the
  old lock/main/.ctl identities, spawn, and exact replacement Hello.

  An ordinary command auto-spawns the current v3 daemon when absent. An
  ordinary command or M3/M4 dry-run that finds a verified v1/v2 daemon
  replaces it through frozen control, then retries the original v3 Request
  exactly once. `daemon status` and `daemon stop` never auto-spawn. Status
  never replaces a daemon: a verified v1/v2 surface returns `running:true`
  with its actual binary and protocol versions. Stop uses frozen control on a
  mismatch. `daemon restart` is the sole control command that starts an absent
  daemon; it stops a running v1/v2 daemon through frozen control and succeeds
  only after a v3 Hello. A same-protocol v3 binary mismatch may use a normal
  v3 stop only after exact account routing; frozen control remains the
  fail-closed fallback. A malformed, foreign, replaced or ambiguous surface
  remains `DAEMON_CONTROL_FAILED` and is never silently removed or restarted.
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
as an unconfirmed remote logout. Deletion holds the target's stable removal
gate in the global removals directory,
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

Before a durable remote proof, a retry may connect to or start TDLib to
re-evaluate the remote state. At `remote_confirmed`, `remote_not_present`,
`remote_kept`, or any later nonterminal stage, it first connects to an already
running target daemon without spawning; if none exists, it acquires the stable
removal gate and resumes locally without TDLib. `daemon run` rechecks the
tombstone after acquiring that gate and refuses to create the account state
root at those stages. This keeps a crash after `state_remove_started` or
`state_removed` from recreating the deleted root, and the still-linked global
gate prevents a competing daemon from replacing the unlinked in-root
`daemon.lock` during deletion.

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
  --timeout` turns "message someone and await a retained/local-or-live reply"
  into one blocking call with a deterministic timeout exit code; §4.6.8 states
  the local-prefix and generation limits.
- `listen --json --count N --timeout S` gives bounded streaming reads that
  exit 0 on planned expiry.
- `--idempotency-key` provides the bounded retry semantics in §4.5.7.
- Once M7 activates `raw`, every pinned function has an explicit policy row;
  denied rows fail closed instead of falling through to an unclassified
  capability.
- Stable curated schemas under `docs/schemas/`, also dumpable at runtime via
  `tgcli schema <command>` — no repo checkout needed inside a sandbox.
- **Schema/runtime boundary.** Ordinary Draft 2020-12 validity is always
  necessary. Schemas assert every restriction expressible in the project's
  standard self-contained subset. A schema_version-2 persistence record in one
  of the three explicitly marked audit files additionally requires the exact
  `tgcli-runtime-v1` rules in that file's documentation-only marker. Those
  rules are only aggregate serialized bytes, contextual normalization,
  same/cross-record equality and derivation, projected uniqueness, strict
  numeric order, and UTF-8 byte limits. A value rejected by the standard schema
  is contract-invalid regardless of C++; a value accepted by the standard
  schema may still fail a listed runtime rule. Unknown keywords never rescue or
  replace standard assertions.
- `tgcli doctor --json` is a one-call health/auth probe.

## 13. Build, dependencies, testing

- **Language/std**: C++20. **Build**: CMake ≥ 3.24 with presets.
- **tdlib**: pinned source revision (tdlib tags rarely; pin a commit hash),
  built via FetchContent by default; `-DTGCLI_SYSTEM_TDLIB=ON` accepts a
  prefix produced by `scripts/build-tdlib.sh` from the same pin — it exports
  the JSON-conversion headers raw needs; arbitrary distro tdlib
  packages are not supported. ccache strongly advised; CI caches the tdlib
  build keyed by the pinned hash. Bumping the pin is a
  contract-change-class PR (REVIEW.md §7): td_api churn can move the typed
  surface and the curated schemas.
- **Libraries** (FetchContent, permissively licensed): CLI11 (nested
  subcommands), nlohmann/json, fmt, tomlplusplus, Catch2; tests additionally
  use jsoncons 1.7.0 at the pinned release commit for Draft 2020-12 validation.
- **Schema assets**: M7 embeds the exact three command catalogs and every schema they
  reference into the binary; no runtime install-path lookup exists. The generator and
  release verifier independently lstat the explicit intended source root, `docs`,
  `docs/schemas`, all three catalogs and every referenced leaf, require exact
  nonsymlink directory/regular-file types, and prove strict resolved containment under
  that root before reads. Linux and macOS archives also carry the three catalogs plus
  their exact referenced-file union under `docs/schemas/`. The release verifier derives
  this set from the catalogs, rejects unsafe/symlinked/escaping or byte-different source
  and package components, and compares packaged-binary discovery output with the files.
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
  - Protocol-v3 routed-account and upgrade acceptance is one central frame/transport/
    routing suite, not repeated per command. It must prove all of the
    following exact cases:
    1. A Request round-trip preserves a valid top-level `account` and emits
       exactly the six v3 top-level fields and nine context fields, including
       a null or grammar-valid `idempotency_key`. Removing `account`, adding an
       unknown top-level field, changing `account` to a non-string, or using
       each invalid name class (empty, longer than 32 ASCII bytes, a character
       outside `[A-Za-z0-9_-]`, or non-ASCII) fails strict parsing without
       producing a Request. A missing/unknown context field, non-ASCII or
       grammar-invalid key, `--dry-run` plus key, or key outside the M3/M4
       allowlist is rejected with the specified `USAGE` reason. After a
       matching v3 Hello, the server classifies each malformed frame as
       `USAGE` with id 0 and `{}`, then EOF; none is
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
    4. After a matching v3 Hello, sending a syntactically valid Request for
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
    7. Upgrade fixtures cover v1→v3, v2→v3, v3→v1 and v3→v2 clients/daemons,
       with both equal and unequal binary-version strings. Every protocol
       mismatch verifies and uses the frozen `.ctl` stop surface, sends no
       normal Request in the mismatched dialect, waits for the old lock and
       both socket identities to disappear, completes the exact replacement
       Hello, and retries the original command exactly once. A same-protocol
       v3 binary mismatch proves that any normal stop Request uses the same
       account that selected its socket.
    8. Lifecycle fixtures prove ordinary and auth-bound M3/M4 dry-run
       autospawn/replacement, status reporting the actual verified v1/v2
       version without replacement, stop without autospawn, restart as the
       only absent-daemon-starting control command, one shared monotonic
       deadline, and `DAEMON_CONTROL_FAILED` for every unverified surface.
       Dry-run fakes admit only the closed read allowlist and local file hash
       operations. `getUser`/`getSupergroup` are accepted only through §6's
       type-bound `ChatIdentity` enrichment and expose no fields outside the
       exact identity. For every one of the seventeen §4.5.1 planner dry-runs,
       the fakes observe no Telegram mutation and no config, audit,
       idempotency-store, spool, prior-group-reconciliation, or other tgcli
       persistence mutation, while TDLib cache/database effects are permitted.
       The sole explicit reconciliation exception is §4.7.5's distinct M6
       session recovery for `session list` and
       `session terminate --dry-run`; those paths recover prior incomplete
       groups, create no current-invocation group, and do not weaken the
       seventeen-planner rule.
  - Golden files for human renderers.
  - E2E: a small opt-in suite (`TGCLI_TEST_DC=1`) against Telegram's **test
    DC** (`use_test_dc`), which provides synthetic phone numbers with fixed
    login codes. M0 is expressly exempt because it has no authentication.
    M1 bootstraps the harness and nightly job with an auth smoke flow; every
    M2–M6 milestone gate adds a flow for a feature that milestone supports.
    M7 validates the already-complete suite for release rather than
    introducing it. M1's required smoke is isolated account bootstrap, phone
    auth/fixed code (and registration when requested), `me`, correlated logout
    and re-login readiness. M2's required read flow runs
    `tgcli chats -n 1 --json` after that auth smoke and requires exit 0 plus a
    schema-valid list whether the account has zero or one returned chat; it
    needs no pre-created message or chat fixture. The already landed Saved
    slice separately runs `tgcli saved tags`; it does not mark the general
    `chats` flow implemented. QR and bot E2E run only with their explicit
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
- **M2** — read path: chats, read/history, msg get/link, search, Saved Messages
  reaction-tag discovery/search, unread, resolve, fetch, chat info/members;
  resolver, JSON/human output, exit codes; every new result-producing command
  lands with its manifest entry, strict schema, and contract validation.
- **M3** — extend the M1 safety kernel to the general write path:
  send/edit/delete/forward/react/mark-read/pin and the remaining destructive
  commands; add idempotency and general planners/audit integration.
- **M4** — files: download with progress frames and the single-file
  Saved Messages document-reply adapter that preserves the replied-to message.
- **M5** — streaming: multiplexed update subscriptions, listen, wait-for.
- **M6** — folders, topics, contacts, chat admin, sessions.
- **M7** — raw passthrough, shell completions, man pages, packaging (static
  binaries, AUR, Homebrew), docs/schemas freeze → v1.0.
- **E2E chronology** — M0 is exempt; M1 establishes auth smoke and nightly
  wiring; each M2–M6 gate adds a supported feature flow; M7 validates the
  complete accumulated suite.
- **Post-1.0 ideas**: MCP server mode (`tgcli mcp` over stdio), secret chats,
  general `send --file` media autodetection/captions/albums/spoilers,
  scheduled-message management, message translation.

## 15. Open questions

- ~~License (MIT vs Apache-2.0)~~ — resolved: MIT (see LICENSE). BSL-1.0 is
  permissive and copyleft-free, so linking tdlib is compatible.
- Whether `chat create` (new groups/channels) belongs in v1 scope.
- Windows support: tdlib supports it, but the process model assumes unix
  sockets and fork/exec; a Windows port needs named pipes and
  CreateProcess — deferred until asked.
