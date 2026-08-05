# TODO

Roadmap milestones from [DESIGN.md](DESIGN.md) §14 (this file is normative
for per-milestone contents). Work top-down; each item lands with the tests
the testing policy (CLAUDE.md) calls for and none it forbids, and keeps the
build green.

Every milestone ends with a review gate: a deep review of the milestone's
full diff against DESIGN.md under the rules of REVIEW.md, with findings
independently verified before being accepted; accepted findings are fixed
before the milestone is closed.

## M0 — Scaffold & process model

- [x] CMake project (≥3.24) with presets: debug, release, release-static
- [x] Pin tdlib source revision; FetchContent build + `-DTGCLI_SYSTEM_TDLIB=ON` option
- [x] `scripts/build-tdlib.sh`: prebuilt pinned-tdlib prefix exporting the
      JSON-conversion headers (dev loop + `raw`/`--full` dependency)
- [x] Vendor deps via FetchContent: CLI11, nlohmann/json, fmt, tomlplusplus, Catch2
- [x] `TdClient` core: ClientManager thread, request/response correlation, update bus
- [x] Daemon skeleton: `tgcli daemon run`, unix socket ($XDG_RUNTIME_DIR/tgcli/<name>.sock;
      $TMPDIR/tgcli-<uid>/ fallback with 0700-dir ownership checks; sun_path length limits;
      0600, peer-uid check: SO_PEERCRED on Linux / getpeereid on macOS),
      JSONL frame protocol (result/error/item/progress/challenge; request
      frame carries tri-state write authority + client cwd/media-dir)
- [x] Version handshake: binary+protocol versions at connect; graceful daemon
      restart on mismatch, terminal frames to old streams
- [x] Auto-spawn: fork + re-exec on missing socket, readiness handshake,
      flock-settled spawn races
- [x] Graceful shutdown: SIGTERM/SIGINT → terminal frames → tdlib close() →
      wait authorizationStateClosed → exit; systemd readiness
- [x] `--no-daemon` in-process debug mode (same dispatch path, refuses if daemon holds lock)
- [x] `tgcli version` / `tgcli doctor` round-trip through the daemon (tdlib
      version via `getOption("version")`); doctor degrades to local
      diagnostics when the daemon is unreachable
- [x] Establish the Draft 2020-12 result-only `docs/schemas/manifest.json` and
      strict result schemas for every M0 result-producing command
- [x] clang-format + clang-tidy configs
- [x] macOS portability: peer-uid via getpeereid, accept4/SOCK_CLOEXEC and
      sigtimedwait replacements (Linux-only today; validated by the CI matrix)
- [x] GitHub Actions: Linux + macOS build/test matrix, ccache + tdlib build cache;
      sanitizer jobs (ASan/UBSan full suite; TSan fake-boundary suite only)
- [x] Choose license (MIT vs Apache-2.0; must be fine linking BSL-1.0 tdlib)
- [x] Protocol-mismatch restart: verify the daemon lifetime lock, authenticate a
      version-independent control socket, wait boundedly for old ownership to
      disappear, then spawn and complete a matching handshake
- [x] E2E gate exemption: M0 has no authentication; test-DC coverage starts in M1
- [x] Review gate: M0 diff vs DESIGN.md

## M1 — Auth & accounts

- [ ] **M1.1 config/bootstrap:** strict loader, 1 MiB bound, one-second idle
      watcher/two-second publish-or-reject deadline and immutable last-good
      snapshots; invalid-reload standing-grant deny and config-global current-
      file reads; cross-process `config.lock`, snapshot-identity CAS and
      symlink-safe atomic 0600 mutation. Pin query-1 `getAuthorizationState`
      response-first/update-first sequence behavior, every one of the 14
      `setTdlibParameters` fields,
      exact phone/QR/registration settings, deterministic implicit-main
      materialization, bounded/redacted per-field `*_cmd` fallback, and fully
      isolated/propagated `TGCLI_TEST_DC=1` roots and parameter identity.
- [ ] **M1.2 auth core:** source-aware `(client_id, query_id)` correlation,
      immutable auth snapshots, exhaustive 13-state pinned FSM including
      `waitPremiumPurchase`, repeated QR updates, ready-loss termination with
      generic reason, and non-shutdown `Closed` replacement. Lifecycle-owned
      login/logout/removal/close waiters accept every response/update ordering,
      resolve their waiter before the unrelated-generation sweep, and preserve
      exactly one terminal; credential and unknown 400/401/429/5xx mapping is
      closed and tested.
- [ ] **M1.3 challenge/login:** challenge identity binds connection/request,
      client generation, auth sequence, nonce and sequence; same-state updates
      supersede old input; answer/deadline acceptance is atomic. Cover pre-send
      disconnect, serialized/orphaned in-flight auth queries, cancellation and
      one-deadline/one-terminal behavior; phone/code/email/2FA/database-key and
      registration retry/resume; QR progress and bot hook/no-echo login; reject
      and redact legacy `--bot-token` plus every env/plain-config token path.
- [ ] **M1.4 identity/safety:** curated `login`/`me`/`doctor` results; every
      TDLib send crosses the descriptor chokepoint, with AuthBootstrap's closed
      function/state allowlist and all other writes still denied. Implement the
      M1 destructive kernel for `logout`/`account remove` only: authority-source
      precedence, confirmation/`--yes`, dry-run, exact durable intent/outcome
      records, correlated logout completion and fail-closed remote uncertainty.
      Set process-global TDLib logging to ERROR before client creation, keep it
      below INFO for life, and make `-v` affect tgcli diagnostics only.
- [ ] **M1.5 accounts/removal:** `account add|list|show|use|remove`, empty-config
      results, exact duplicate/missing/default-reassignment behavior, target-
      daemon routing and config/tdlib/state/socket isolation. Default removal
      versus `--keep-session` uses a global non-deletable audit/tombstone,
      ordered crash-safe remote/config/data/state/outcome checkpoints, fresh-
      approval recovery, root identity/CAS validation, and mount/device-
      boundary refusal; no local deletion precedes remote proof.
- [ ] **M1.6 daemon/results:** exact auto-spawn/no-spawn matrix and
      `daemon status|stop|restart|run` absent/running behavior; preserve M0
      `USAGE {}` for lifecycle `--no-daemon`; one-second config observation and
      request/challenge/subscription `idle_exit` accounting. Add result-only
      manifest entries and strict Draft 2020-12 schemas for every exact M1
      success/error/detail/audit shape, uint64 request IDs, audit checkpoint
      arrays and `none|possible|confirmed` mutation state while preserving M0
      objects; keep `raw`/`--full` rejected until M7.
- [ ] **M1.7 fake-boundary acceptance:** drive all 13 auth states and the closed
      exact `(function, code, raw message)` credential table; first-query/all-
      parameter bootstrap in response-first and update-first orders, repeated
      QR replacement, stale/wrong/duplicate answers, same-state supersession,
      disconnect/orphan/deadline races, legal lifecycle response/update orders,
      ready loss and old-generation late responses. Cover hook/config watcher/
      CAS races, invalid `account show`, destructive authority/audit order,
      every logout/removal before-send and after-confirm crash checkpoint,
      audit inspection/clear behavior, mount refusal, account
      isolation/routing/empty config and daemon spawn/idle boundaries through
      real dispatch.
- [ ] **M1.8 test-DC and real-TD sentinel:** `TGCLI_TEST_DC=1` creates and
      propagates isolated roots/parameters, refuses production state, and runs
      nightly. Smoke covers add/phone/fixed-code registration, `me`, explicitly
      granted/confirmed logout and correlated closed/re-login readiness. QR/bot
      require their named fixtures. Concurrent `-v` auth with sentinel token,
      codes, password and database key scans stderr, active `tdlib.log` and all
      rotated logs byte-for-byte and proves TDLib INFO request serialization is
      absent.
- [ ] **M1.9 explicit E2E gaps:** every pinned state not deterministically
      forceable in the test DC has M1.7 coverage and a closed skip reason. Every
      run publishes `<build-dir>/test-results/tgcli-test-dc-skips.json`, even
      when empty, with exact sorted entries and only
      `fixture_missing:qr_approver`, `fixture_missing:bot_token_cmd`, or
      `test_dc_state_not_forceable:<state>`; a missing artifact or silent/pass-
      equivalent skip fails the milestone gate.
- [ ] Review gate: M1 diff vs DESIGN.md

## M2 — Read path

- [ ] Resolver: exact id/@username/t.me classification and link/bot matrix;
      arbitrary title substring over fully loaded active Main+Archive, local
      materialized-prefix scope, strict ambiguity candidates, and exact
      username `NOT_FOUND` normalization
- [ ] Shared M2 DTOs and parsing: lossless `TopicRef`, `MessageSummary`,
      `ChatIdentity`/`ChatSummary`, member/user/chat sender variants, int53
      boundaries, inclusive rounded timestamps, and command-specific limits
- [ ] Output layer: equivalent human/JSON rendering, exact result/error shapes,
      stderr discipline, and self-contained untrusted cursors without a MAC or
      daemon state; reject bad scope and non-advancing upstream markers
- [ ] Add strict Draft 2020-12 result schemas and manifest entries for `chats`,
      `read`, `msg get`, `msg link`, `search`, `unread`, `fetch`, `resolve`,
      `chat info`, and `chat members`; validate actual result data and keep
      `history` schema-less as the canonical `read` alias
- [ ] `tgcli chats`: Main/Archive/numeric-folder growing-prefix scan, exact
      `(position.order, chat_id)` keyset, sparse unread pagination, and
      live-view continuation cases across restart/movement/removal/ties
- [ ] `tgcli read`/`history`: limits, exclusive `--before`, inclusive
      `--since`/`--until`, all four topic kinds, offline continuous-prefix
      behavior, exact `boundary`/`next` mapping, and advancing raw cursors
- [ ] `tgcli msg get`, `tgcli msg link`, `tgcli resolve`: atomic ordered batch
      reads, exact link call/result, resolver DTOs, and contextual targets
- [ ] `tgcli search`: per-chat/global server search, exact filter mappings,
      exhaustive sender/text post-filtering, full upstream cursors, and no
      secret/local-search merge
- [x] `tgcli saved tags|search`: user-account-only reaction-tag discovery plus
      tag-only/tag+text search; canonical emoji/custom selector round-trip,
      label/count/order output, account/scope/filter-bound cursor pagination,
      and contract coverage for NOT_AUTHED/BOT_UNSUPPORTED preflight, malformed
      selectors/cursors, cursor/filter mismatches, paid/unknown variants, empty
      matches, and reaction tags vs emoji text
- [ ] `tgcli unread`: fully load and deduplicate Main then Archive, apply the
      shared unread predicate, skip secret chats, and return `next:null`
- [ ] `tgcli fetch <chat>`: default/finite/since/all targets, continuous local
      prefix plus live fill, resumable progress/timeout details, and the public-
      TDLib `tdlib_idle` limitation without false EOF/completeness claims
- [ ] `tgcli chat info`, `tgcli chat members`: type-specific info sources,
      user/chat senders, exact filters, and empty-probe pagination independent
      of approximate member totals
- [x] Landed Saved test-DC flow: after auth smoke, run `tgcli saved tags`,
      validate the unpaginated tag-list result, and retain the exact sorted
      test-DC skip artifact contract
- [ ] General M2 read test-DC flow: after auth smoke, run
      `tgcli chats -n 1 --json` and require exit 0 plus a schema-valid empty or
      non-empty list without a pre-created fixture
- [ ] Review gate: M2 diff vs DESIGN.md

## M3 — Safety & write path

- [ ] Implement the frozen protocol-v3 precursor: strict nine-field context
      with `idempotency_key`, Hello-first parsing/writing, v1/v2↔v3 frozen
      control replacement, exact status/stop/restart/autospawn behavior, and
      auth-bound M3/M4 dry-run read allowlist/effect fixtures
- [ ] Reuse exact M2 resolver/principal DTOs, add static operation tiers and
      the user/bot/schedule admission matrix, and extend the single daemon-side
      safety chokepoint to every M3 Write/Destructive descriptor
- [ ] Add neutral TD request/update DTOs, strict M3 results/errors/plans, and
      direct-response/auth-update/deadline arbitration without exposing
      `td_api.h` outside daemon implementation translation units
- [ ] Add audit schema v2 common/per-command/checkpoint/recovery records,
      legal stage ordering, mixed-v1/v2 recovery, contradiction handling,
      rotation/retention, and durable intent/outcome enforcement
- [ ] Add canonical JSON and complete per-operation fingerprints plus the
      idempotency store: quota/reservations, locked insert-if-absent, exact
      pending/completed/conflict/replay states, crash-cutpoint repair, and
      raw-key/invite sentinel gates
- [ ] Add shared direct and single-message coordinators with immutable plans,
      schedule ceiling/boundary rechecks, strict timeout oneOf, exact terminal
      ordering, and no post-dispatch cancellation claim
- [ ] Implement `tgcli send` text/Markdown/HTML/reply/forum-topic/silent/
      schedule paths, wait for authoritative final send state, and return the
      exact `MessageWriteResult`
- [ ] Implement `tgcli msg edit|react|pin|unpin` with exact property
      validation, reaction availability, plan, audit, timeout and idempotency
      behavior
- [ ] Implement destructive `tgcli msg delete` and `tgcli chat leave`,
      including confirmation of the immutable plan on new invocation and
      completed replay, plus `completed_unchanged` replay-confirmation timeout
- [ ] Implement the ordered `tgcli msg forward` vector coordinator with one
      strict `ForwardItem` across success/error/timeout/audit/store, partial
      outcomes, deleted-before-confirmation handling and 429 aggregation
- [ ] Implement `tgcli chat mark-read|mute|unmute|pin|unpin|archive|unarchive|join`
      with exact direct-call state machines, invite secrecy and
      notification-setting plans
- [ ] Add the complete fake-boundary/fault/cutpoint/canonicalization/sentinel
      acceptance matrix and the no-skip Saved text-send TestDC flow with
      immediately registered confirmed cleanup
- [ ] Review gate: M3 diff vs DESIGN.md (safety chokepoint gets extra scrutiny)

## M4 — Files & media

- [ ] `tgcli download` with progress frames (stderr bar / NDJSON); transfers
      unlimited by default, `--timeout` opt-in
- [ ] Add the two-pass file snapshot/spool coordinator: pass-1 full identity
      and digest, winner-only post-intent pass 2, private fsynced spool,
      durable `spool_ready`, complete crash-cutpoint cleanup and startup repair
- [ ] Implement `tgcli saved attach <message-id> <PATH> [--caption TEXT]` as a
      user-only, single-file Saved reply adapter preserving the original;
      enforce saved/null topic inheritance and the shared plan/audit/timeout/
      idempotency contract, and return the exact `MessageWriteResult`
- [ ] Add `saved attach` result-only manifest/schema and contract coverage for
      final id, NOT_AUTHED/BOT_UNSUPPORTED, NOT_FOUND/USAGE, gate/dry-run,
      two-pass source races, quota/CAS cutpoints, audit, timeout and replay
- [ ] `TGCLI_MEDIA_DIR` handling
- [ ] Add a supported M4 media flow to the test-DC E2E milestone gate
- [ ] Review gate: M4 diff vs DESIGN.md

## M5 — Streaming

- [ ] Update-bus subscriptions with filters, multiplexed to any number of clients
- [ ] `tgcli listen` (--chat, --types, --count, --timeout; NDJSON; planned expiry → exit 0)
- [ ] `tgcli wait-for` (--chat, --from, --regex, --after for race-free
      send-then-wait — requires --chat, --timeout → exit 7)
- [ ] Add a supported M5 streaming flow to the test-DC E2E milestone gate
- [ ] Review gate: M5 diff vs DESIGN.md

## M6 — Long tail

- [ ] `tgcli contact list|search|add|remove|block|unblock`
- [ ] `tgcli folder list|show|create|edit|delete|add-chat|remove-chat`
- [ ] `tgcli topic list|create|edit|close|reopen`
- [ ] `tgcli chat set-title|set-photo|set-description|invite-link|promote|demote|ban|unban|kick|set-permissions`
- [ ] `tgcli session list|terminate`
- [ ] `tgcli storage stats|optimize` (tdlib file-store usage, optimizeStorage)
- [ ] Add a supported M6 long-tail flow to the test-DC E2E milestone gate
- [ ] Review gate: M6 diff vs DESIGN.md

## M7 — Polish & release

- [ ] `tgcli raw` passthrough (td_api_json), read-only allowlist, auth-type
      refusal + audit redaction
- [ ] `tgcli schema <command> [--all]` — runtime dump of curated schemas
- [ ] Shell completions (bash/zsh/fish), man pages
- [ ] docs/schemas/ — freeze curated JSON schemas per command
- [ ] Validate the complete M1–M6 test-DC E2E suite at the M7 gate, including
      fake-boundary coverage and explicit skip reasons for states the test DC
      or available account capabilities cannot exercise
- [ ] Static musl Linux binary + macOS universal binary release job
- [ ] Packaging: AUR, Homebrew; systemd user unit example for `tgcli daemon run`
- [ ] Review gate: M7 diff vs DESIGN.md
- [ ] v1.0

## Post-1.0 ideas

- [ ] MCP server mode (`tgcli mcp` over stdio)
- [ ] Offline search: client-side filtering over prefetched local history
- [ ] Secret chats
- [ ] General `tgcli send --file` media autodetection, captions, albums and
      spoilers
- [ ] Scheduled-message management
- [ ] Message translation
