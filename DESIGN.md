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
--full                 include the underlying td_api object under a raw key
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
tgcli login [--qr] [--bot-token <token>]
tgcli logout                                      destructive (§6)
tgcli me
tgcli doctor                                      auth state, tdlib version, paths, DB sizes, daemon state;
                                                  degrades to local diagnostics (paths, config,
                                                  socket state) when the daemon is unreachable

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

tgcli raw '<td_api request JSON>' [--timeout S]   full-API escape hatch (§4.2)
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
tgcli account add|list|show|use|remove <name>     remove destructive (§11)
tgcli daemon status|stop|restart|run              lifecycle (§10); auto-spawned otherwise,
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

Direct passthrough to the td_api schema (`@type`-keyed JSON, the same
convention as td_json_client): guarantees full API coverage before a dedicated
subcommand exists. `raw` passes through the same write gate (§6): request
types outside a known-read-only allowlist count as writes and require a
write grant. Auth-flow request types (`setAuthenticationPhoneNumber`,
`checkAuthenticationPassword`, …) are refused outright — they would put
secrets on argv and into the audit log; authentication goes through `login`
challenges only. Audited `raw` records redact known secret-bearing fields.

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
| 4 | NOT_FOUND | chat/message/user/file not found |
| 5 | RATE_LIMITED | Telegram flood wait surfaced; `retry_after` in error details |
| 6 | DENIED | write attempted without a grant, or destructive action without confirmation |
| 7 | TIMEOUT | `--timeout` elapsed without the awaited outcome (`wait-for`, transfers, raw); `listen` reaching `--timeout`/`--count` is a planned expiry and exits 0 |

## 6. Safety model

Three tiers, enforced in one chokepoint (§7 `safety` layer, evaluated
daemon-side); each command handler statically declares its tier so a gate
cannot be forgotten:

- **Reads** — always allowed, no grant needed.
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

The gate fails closed: an agent invoked with no grant — or under
`TGCLI_ALLOW_WRITE=0` when the account carries a standing grant — has a
read-only surface and receives a structured exit-6 error the moment it
strays. The gate guards against accidental and unauthorized-by-omission
writes, not against a hostile process running as the same uid (§10).

Supporting mechanisms:

- **`--dry-run`** — performs resolution and validation, prints the exact plan
  (resolved ids, message preview, td_api request type), calls nothing. It
  needs no write grant and no `--yes` — it exists precisely so plans can be
  made without authority.
- **Audit log** — every state-changing invocation appends a JSONL record
  (argv, resolved targets, outcome) to the account's `audit.log`. The record
  includes message bodies — it is the account owner's own log; secret-bearing
  fields in `raw` payloads are redacted. Files rotate by size (§9).
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
  *declare* `Tier::Read|Write|Destructive` in their static descriptor.
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

- **Auth FSM**: driven by `updateAuthorizationState`. The common path is
  `waitTdlibParameters → waitPhoneNumber → waitCode → [waitPassword] → ready`,
  but the FSM handles every authorization state tdlib can enter:
  `waitRegistration` (fresh numbers — the test-DC suite hits this
  constantly), `waitOtherDeviceConfirmation` (the state the `--qr` flow
  actually runs in), and the email states (`waitEmailAddress`,
  `waitEmailCode`) some accounts are forced through. Interactive `login`
  prompts reach the invoking client as challenge frames (§10) and are
  answered from its TTY (password without echo); `--qr` uses
  `requestQrCodeAuthentication` and renders the QR in the terminal;
  `--bot-token` uses `checkAuthenticationBotToken` (fully non-interactive).
  The 2FA password can come from `password_cmd` in config (e.g.
  `pass show …`) for scripted re-auth; secrets are never accepted via argv.
- **Bot-account caveat**: a bot session has no dialog list, cannot read
  arbitrary chat history, has no server-side search and no contacts — most
  of the read surface (`chats`, `read`, `search`, `unread`, `fetch`) is
  user-account-only. Bots suit narrow send/receive automation in chats they
  are already in; an agent that needs to read should run on a user account.
  A user-account-only command invoked on a bot session fails with exit 2 and
  error code `BOT_UNSUPPORTED`.
- **Every other command** requires `authorizationStateReady`; anything else →
  exit 3 with the current state named in the error details.
- **Remote session death**: a session revoked from another device surfaces
  as `authorizationStateLoggingOut`/`Closed` unprompted. Streaming
  subscribers get a terminal error frame, subsequent commands exit 3 with
  the reason, `doctor` reports the state, and the daemon stays up so
  `login` can re-authenticate without a respawn.
- **Request correlation**: `ClientManager::send(client_id, query_id, fn)`;
  `TdClient` hands out monotonic query ids and resolves the matching promise
  on receive. Updates (query_id 0) go to the update bus. Request reservation
  and the close transition are ordered under one lifecycle gate: a send
  admitted first is resolved normally or failed during close, while a send
  after close begins returns an immediately exceptional future. No request
  can be reserved after the final pending-query sweep.
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
- **tdlib logging**: to a file in the account state dir at verbosity 1 by
  default; `-v` raises verbosity and mirrors to stderr.

## 9. Configuration, paths, secrets

XDG layout, one subtree per account:

```
~/.config/tgcli/config.toml                    global + per-account config
~/.local/share/tgcli/accounts/<name>/tdlib/    tdlib database + files
~/.local/state/tgcli/accounts/<name>/          audit.log, idempotency.db, tdlib.log
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
folds them into the request frame (§10), since the long-lived daemon cannot
see an invoking shell's environment. `TGCLI_API_ID`/`TGCLI_API_HASH` are
consumed at **daemon spawn** (the auto-spawned daemon inherits the spawning
client's environment); changing them takes effect on daemon restart.

The daemon re-reads `config.toml` when its mtime changes, so gate and
`idle_exit` edits apply to new requests without a restart; tdlib parameters
(api_id/api_hash, db key) take effect on daemon restart.

Credentials policy — two classes, treated differently:

- **App credentials (`api_id`/`api_hash`)** are *not* account secrets: they
  identify the application, grant no account access without the user's own
  auth, and are less sensitive than the tdlib directory sitting next to them
  (which holds the MTProto auth key — the real crown jewel). They live as
  plain values in `config.toml`; `tgcli login` prompts for them on first run
  (or takes `TGCLI_API_ID`/`TGCLI_API_HASH`) and persists them. `api_id_cmd`/
  `api_hash_cmd` hooks remain available for those who prefer a secret store.
  tgcli does not embed shared app credentials in the binary: publishing an
  api_hash is against Telegram's guidance and one abusive user could get the
  shared app id rate-limited or banned for everyone.
- **Real secrets (2FA password, DB encryption key)** are accepted only via
  `*_cmd` config hooks or interactive prompt — never via argv (visible in
  `ps`) and never written to disk by the tool.

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

- **Auto-spawn, zero setup.** A command that finds no live socket forks the
  daemon (re-execs itself as `tgcli daemon run --account <name>`), waits for
  a readiness handshake, and proceeds. Spawn races are settled by an `flock`
  on the account dir: the loser just connects to the winner's socket.
- **Lifecycle.** The daemon keeps running by default — a continuously warm
  DB is a feature, not a leak; `idle_exit = <seconds>` in the account config
  opts into terminate-when-idle. `tgcli daemon status|stop|restart` manage
  it; `tgcli daemon run` stays in the foreground for systemd user units,
  containers, and debugging.
- **Protocol.** JSONL frames over a `SOCK_STREAM` unix socket. Request frame:
  id, command path, normalized args, client context — TTY-ness, `--json`,
  `--yes`, `--dry-run`, timeout, the client's cwd and `TGCLI_MEDIA_DIR` (for
  output paths), and a **tri-state write-authority field** (`grant` / `deny`
  / `unset`): the client folds `--allow-write` and `TGCLI_ALLOW_WRITE` into
  it, because the daemon cannot see the invoking shell's environment, and
  the daemon combines it with the account config per §6 (explicit deny >
  any grant > default deny). Response frames: `result`,
  `error`, `item` (streams), `progress`, and `challenge`. Interactive flows
  are challenge/response: the daemon asks (login code, 2FA password,
  destructive confirmation), the client prompts on *its* TTY and answers;
  with no TTY a challenge fails closed (exit 3 for auth, 6 for confirmation).
  Multiple requests are served concurrently; a slow download never blocks a
  `send` from another shell.
- **Version handshake.** The connect handshake carries binary and protocol
  versions from both sides. On any mismatch the client — freshly exec'd, so
  authoritative — asks the daemon to shut down gracefully and respawns it
  from the new binary; streams on the old daemon receive a terminal error
  frame. Frames and curated schemas are never mixed across versions.
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
- **Challenge ownership.** A challenge belongs to the request (and client
  connection) that triggered it. `login` additionally takes a per-account
  auth lease: one login flow at a time — a concurrent `login` gets a
  structured "auth flow in progress" error, and other commands exit 3
  naming the pending flow. If the owning client disconnects mid-challenge,
  the flow aborts and the lease is released; tdlib's auth FSM stays in its
  current wait state and the next `login` resumes from it. A
  destructive-confirmation challenge dies with its request: a disconnect
  before the answer means nothing is sent.
- **Shutdown.** On SIGTERM/SIGINT or `daemon stop`: stop accepting requests;
  finish every active request with one terminal `DAEMON_SHUTDOWN` error (exit
  1, details `{"reason":"daemon_shutdown"}`) unless it already emitted its
  terminal frame; call tdlib `close()` and wait for
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
  daemon already holds the account lock.

## 11. Multi-account

`tgcli account add work && tgcli --account work login` — each account has an
isolated config section, tdlib dir, state dir, and socket. `account use` sets
`default_account`. Nothing is shared between accounts, including idempotency
stores and audit logs.

`account remove` is destructive (§6): it deletes the account's tdlib
directory — the MTProto auth key — irreversibly. By default it first
performs a server-side logout so no orphaned session survives; `--keep-session`
skips that (explicitly leaving the session valid server-side, e.g. when the
same account is used elsewhere), and the confirmation prompt states which of
the two is about to happen.

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
- `raw` guarantees no capability cliff: anything td_api can do is reachable.
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
  - Golden files for human renderers.
  - E2E: a small opt-in suite (`TGCLI_TEST_DC=1`) against Telegram's **test
    DC** (`use_test_dc`), which provides synthetic phone numbers with fixed
    login codes. M0 is expressly exempt because it has no authentication.
    M1 bootstraps the harness and nightly job with an auth smoke flow; every
    M2–M6 milestone gate adds a flow for a feature that milestone supports.
    M7 validates the already-complete suite for release rather than
    introducing it. Required states unavailable in the test DC, including
    Premium-only states, receive fake-boundary contract coverage and an
    explicit E2E skip reason. The external service is rate-limited and
    periodically wiped, so E2E is not a per-PR merge blocker (REVIEW.md §4).
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
  (status/stop/restart/run, idle_exit).
- **M2** — read path: chats, read, msg get, search, unread, resolve, fetch;
  resolver, JSON/human output, exit codes; every new result-producing command
  lands with its manifest entry, strict schema, and contract validation.
- **M3** — safety layer + write path: send/edit/delete/forward/react/
  mark-read/pin, two-phase destructive confirmation over the protocol,
  audit log, idempotency, dry-run.
- **M4** — files: download/upload with progress frames, media types.
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
