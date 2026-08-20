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
      username `NOT_FOUND` normalization; the finalized `ResolverConsumer`
      must provide the no-send local link classification required by `read
      --local`, never route it through terminal resolve/`getInternalLinkType`
- [ ] Shared M2 DTOs and parsing: lossless `TopicRef`, `MessageSummary`,
      `ChatIdentity`/`ChatSummary`, member/user/chat sender variants, int53
      boundaries, inclusive rounded timestamps, command-specific limits, and
      neutral/native top-level message/messages/message-link/thread-info conversion;
      `TdMessages` retains nonnegative `total_count`, null positions and shared
      summaries without treating count as continuation state; strict thread metadata
      retains history chat/thread ids and starting summaries only
- [x] Refactor the accepted resolver into the typed non-terminal
      `ResolverConsumer`: retain the ten-value M2 operation enum, use the
      separate M2-or-frozen-M3/M4 caller attribution, bind Ready/getMe once,
      return immutable contextual result and typed error/stop outcomes, retain
      `ReadyReadStatus::Cancelled` from target reads, and reuse one absolute
      deadline; expose cached-or-exact local Saved chat materialization for the
      read topic-ownership check
- [ ] Output layer: equivalent human/JSON rendering, exact result/error shapes,
      stderr discipline, and self-contained untrusted cursors without a MAC or
      daemon state; reject bad scope and non-advancing upstream markers
- [ ] Add strict Draft 2020-12 result schemas and manifest entries for `chats`,
      `read`, `search`, `unread`, `fetch`, `resolve`, `chat info`, and `chat
      members`; validate actual result data and keep `history` schema-less as
      the canonical `read` alias; `read` adds no error-catalog mapping
- [x] Freeze the `read`/`history` contract: closed ASCII timestamp/topic
      grammars, exact before/until and signed-int32 since-anchor branches,
      explicit-operand precedence over resolver context, Saved ownership, live
      thread history-chat metadata and safe local-thread limits, count/progress/
      idle/local-boundary rules, unsigned version-1 cursor state, preflight
      exclusion, schema relation and dependency/test order
- [x] `tgcli chats`: Main/Archive/numeric-folder growing-prefix scan, exact
      `(position.order, chat_id)` keyset, sparse unread pagination, and
      live-view continuation cases across restart/movement/removal/ties
- [x] `tgcli read`/`history`, after shared MessageSummary/TdMessages and
      ResolverConsumer land: pure timestamp/topic/cursor/scanner foundation;
      seven typed TD calls including `getMessageThread`; one canonical coordinator
      plus parser alias; limits,
      exclusive `--before`, inclusive `--since`/`--until`, all topic kinds,
      offline continuous-prefix behavior with the explicit local channel/thread
      refusal, exact `boundary`/`next` mapping and
      advancing raw cursors; result schema/manifest, human goldens, fake/native
      coverage and full mechanical gates; no new TestDC flow or skip
- [x] `tgcli msg get`, `tgcli msg link`: explicit positional ids govern after
      chat-only consumption of immutable resolver context; atomic ordered batch
      reads, malformed-response integrity precedence, exact link call/result,
      non-empty UTF-8 link validation, both result schemas and result-manifest
      entries, equivalent exact human goldens, real-dispatch recovery ordering,
      dispatcher/fake/native coverage, and no new TestDC skip; defer both error
      schemas and error-catalog mappings to the existing M7 schema task
- [ ] `tgcli search`: per-chat/global server search, exact filter mappings,
      exhaustive sender/text post-filtering, full upstream cursors, and no
      secret/local-search merge
- [x] `tgcli saved tags|search`: user-account-only reaction-tag discovery plus
      tag-only/tag+text search; canonical emoji/custom selector round-trip,
      label/count/order output, account/scope/filter-bound cursor pagination,
      and contract coverage for NOT_AUTHED/BOT_UNSUPPORTED preflight, malformed
      selectors/cursors, cursor/filter mismatches, paid/unknown variants, empty
      matches, and reaction tags vs emoji text
- [x] `tgcli unread`: fully load and deduplicate Main then Archive, apply the
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
- [x] General M2 read test-DC flow: after auth smoke, run
      `tgcli chats -n 1 --json` and require exit 0 plus a schema-valid empty or
      non-empty list without a pre-created fixture
- [ ] Review gate: M2 diff vs DESIGN.md

## M3 — Safety & write path

- [ ] Implement the frozen protocol-v3 precursor: strict nine-field context
      with `idempotency_key`, Hello-first parsing/writing, v1/v2↔v3 frozen
      control replacement, exact status/stop/restart/autospawn behavior, and
      auth-bound M3/M4 dry-run read allowlist/effect fixtures, including
      type- and id-bound `getUser`/`getSupergroup` `ChatIdentity` enrichment
      with no direct/raw/title-candidate admission or non-identity field
      retention; exact `resolve`-attributed enrichment failures and same-Ready
      arbitration; and zero config/audit/idempotency/spool/prior-group-
      reconciliation or other tgcli persistence mutation across all seventeen
      planner dry-runs
- [ ] Reuse exact M2 resolver/principal DTOs, add static operation tiers and
      the user/bot/schedule admission matrix, and extend the single daemon-side
      safety chokepoint to every M3 Write/Destructive descriptor
- [ ] Add neutral TD request/update DTOs, strict M3 results/errors/plans, and
      direct-response/auth-update/deadline arbitration without exposing
      `td_api.h` outside daemon implementation translation units
- [ ] Add strict mixed-v1/v2 per-account audit schemas, factories, streaming
      scanner, recovery and pin-aware fixed-slot rotation; preserve v1 APIs and
      its 64 MiB behavior; freeze AUDIT_INCOMPLETE recovery terminals,
      sent-forward confirmed recovery, cleanup/outcome/store ordering,
      inode-backed audit generations, hole-first rotation, bounded global
      invocation rescans, external data ceilings, exact v2 record/group/segment
      limits, oversize precedence, contradiction handling and crash points;
      publish the
      reviewed standard-schema/runtime boundary, retain all expressible
      calendar/stage/terminal/basename/path assertions, remove field-level
      pseudo-assertions, emit the exact schema-version-2 marker and
      filename/rules matrix, and test ordinary-schema acceptance separately
      from required runtime semantics.
- [ ] Align session/M3 AUDIT_UNAVAILABLE schemas with the complete accepted
      durability_reason enum; reject rotate_failed in v2 while retaining the
      separate v1 audit_reason, and generator-check every runtime audit record.
- [ ] Add canonical JSON, domain-separated hashes and complete per-operation
      fingerprints with exact canonical byte/golden vectors and raw-key/invite
      sentinel gates.
- [ ] Extend the accepted audit API with open-group generation, the exact
      immutable streamed completed-group view, one-pass tuple pin validation,
      move-only append permits and receipt-bound audit spool holds/releases;
      retain M6 `AbsentByPolicy` as zero store/temp I/O and hole-only rotation.
- [ ] Add the strict canonical-byte idempotency store and reconciliation:
      fixed stale-temp recovery, file-fsync→rename→directory-fsync replacement,
      exhaustive public reason/precedence and canonical absolute path, exact
      count/byte/mutable-headroom quota, keyed/unkeyed equality expiry with
      conservative clock rollback, completed clearing/pending retention,
      nine-step core gate without capacity, pre-intent append permit,
      authoritative incumbent return before a current group, miss-only
      confirmation/config-CAS/intent, post-generation prospective-winner quota,
      unexpected-insert-loss INTERNAL/AUDIT_INCOMPLETE fatal closure,
      checkpoint→store repair, exact M4 pass-1/pass-2 epoch placement, no inner
      store lock, crash recovery and safe spool/capacity release.
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
      acceptance matrix: same-key dual misses; expiry equality/rollback/order;
      canonical/noncanonical final bytes; fixed-temp and rename/directory-fsync
      crash images; mutable progress quota and completion clearing;
      audit-ahead/store-lag repair for temp ids and every forward vector;
      exact completed views/one-pass pins/receipt-bound holds across rotation;
      unkeyed timestamp expiry and store-removal-before-cleanup crashes;
      exhaustive idempotency reason/precedence and absolute final paths;
      same-fingerprint username retarget/completed-incumbent adoption and fresh
      confirmation, direct pending/conflict return with no prompt/group, exact
      miss-only pre-CAS/pre-intent proposed-plan confirmation, and zero
      unconfirmed artifacts;
      wait deadline/cancel before acquired spool/root precedence; initial core
      gate with zero capacity/permit, authoritative commit lookup before any
      current group, commit permit before miss-only intent, winner quota after
      generation, unexpected-loss fatal closure; M4 pass 1 after prior
      reconciliation and before lookup, repeated core gate/lookup before
      CAS/permit/intent, pass 2 and dispatch inside commit epoch with no inner
      store lock; and proof M6 `AbsentByPolicy` opens neither
      canonical nor temp store. Retain the no-skip Saved text-send TestDC flow
      with immediately registered confirmed cleanup.
- [ ] Review gate: M3 diff vs DESIGN.md (safety chokepoint gets extra scrutiny)

## M4 — Files & media

- [ ] `tgcli download` with progress frames (stderr bar / NDJSON); transfers
      unlimited by default, `--timeout` opt-in
- [ ] Add the dormant two-pass file snapshot/spool foundation: exact
      no-follow frozen-locator replay and post-pass entry/FD revalidation,
      strict source errors, pass-1 full identity/digest, winner-only
      post-intent pass 2, total private-root classification, fsynced one-file
      spool, audit-first keyed SpoolRef publication, durable cleanup/ref
      clearing, byte-safe orphan diagnostics plus the effect-based v2 gate,
      prior-group/current-invocation persistence distinctions, Linux/macOS
      portability, and all filesystem/deadline/crash cutpoints; do not register
      `saved attach` or mark the adapter complete in this slice
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

- [x] Add strict `listen` item, `wait-for` result, and stream error schemas plus
      the separate exact stream catalog/package bijection and schema-provable
      checked-sum byte-capacity diagnostics
- [ ] Land the shared M2 resolver, MessageSummary/ChatIdentity/ChatSummary DTOs,
      sender matching, int53 handling, and local-history components M5 reuses
- [ ] Integrate the measured google/re2 `2022-12-01` full-commit archive as a
      static runtime dependency; lock its archive/tree hashes, BSD and embedded
      Lucent notices, offline staging, and no-ICU/PCRE/Abseil provenance checks
- [ ] Add tagged unlimited/finite deadlines and stop-aware wait helpers without
      a maximum-time sentinel
- [ ] Add generation-scoped metadata bootstrap, the bounded ordered-normalization
      barrier, curated update/reaction/chat normalization, and explicit metadata
      capacity failures
- [ ] Add the fixed 32-slot sequentially consistent ingress registry, bounded
      per-subscription SPSC queues, polling workers, overflow causes, and proven
      deferred reclamation
- [ ] Add RequestSession item/terminal ordering, complete-write counting,
      transactional activity promotion, teardown, and first-cause arbitration
- [ ] Implement exact `listen`/`wait-for` parsing, setup precedence, filters,
      retained `--after` scan/deduplication, bot behavior, and command handlers
- [ ] Add silent internal `listen` Result handling, checked per-item stdout
      write/flush, output-failure cancellation, and daemon/no-daemon parity
- [ ] Add schema, fake/native, TSan, integration, release-provenance, and no-skip
      Saved TestDC coverage for the complete M5 acceptance matrix
- [ ] Review gate: M5 diff vs DESIGN.md

## M6 — Long tail

- [ ] `tgcli contact list|search|add|remove|block|unblock`
- [ ] `tgcli folder list|show|create|edit|delete|add-chat|remove-chat`
- [ ] `tgcli topic list|create|edit|close|reopen`
- [ ] `tgcli chat set-title|set-photo|set-description|invite-link|promote|demote|ban|unban|kick|set-permissions`
- [ ] Freeze the §4.7 session-only grammar/frame args, full signed-int64 string
      identity including zero, 17-value device enum, strict DTO/error/human
      output, bot/current/business-bot semantics, deadlines, TDLib acceptance
      meaning and no-idempotency decision
- [ ] Add dormant typed Session DTO/conversion, `getActiveSessions` and
      `terminateSession` runtime factories/descriptors/native matchers,
      scripted-fake seams and unregistered safety-policy descriptors at pinned
      TDLib 1.8.65 / a17f87c4cff7b90b278d12b91ba0614383aaee82
- [ ] Extend the dormant audit-v2 contract with session_terminate,
      idempotency_key_hash:null and pre-Ready/step-6 prior-group inspection;
      enforce AbsentByPolicy store access, block keyed incomplete groups, and
      use only non-evicting capacity without pin knowledge; expose no command
      and perform no terminate dispatch yet.
- [x] Add the one-of real/dry-run terminate result schema, list result schema,
      exact self-contained session error schema, manifest entries and
      deterministic human renderers; keep the public `schema` CLI deferred M7
- [ ] Implement dormant complete list and terminate handlers with exact
      Ready/getMe/bot/deadline ordering; wire terminate dispatch only through
      the already-integrated destructive authority/confirmation/config-CAS/
      audit-intent/dispatch/proof/outcome path, never around it
- [ ] Add exhaustive dormant handler/CLI-frame/schema/human/fake/native/crash/
      auth-loss/deadline tests, including zero and int64-outside-int53 ids, all
      device variants, current-session refusal, public-Ok semantics and
      business-bot exclusion
- [ ] Atomically activate/register the complete `session list|terminate`
      CLI/frame surface only after both handlers, safety policy, audit v2,
      schemas/renderers and focused tests are present and passing
- [ ] Add the no-skip non-mutating Saved TestDC `m6.session.list` flow with
      verified daemon stop/lock release before `--no-daemon`, plus release-
      provenance checks; do not terminate a live/TestDC session
- [ ] Review gate: session-only M6 semantic diff vs DESIGN.md §4.7 and pinned
      TDLib generated API/AccountManager.cpp/Requests.cpp
- [ ] `tgcli storage stats|optimize` (tdlib file-store usage, optimizeStorage)
- [ ] Add a supported M6 long-tail flow to the test-DC E2E milestone gate
- [ ] Review gate: M6 diff vs DESIGN.md

## M7 — Polish & release

- [ ] `tgcli raw` passthrough (td_api_json), read-only allowlist, auth-type
      refusal + audit redaction
- [ ] Implement local `tgcli schema <command-token>... [--all]`: exact
      result/item fallback, fixed result/item/error aggregation, history alias,
      embedded byte-authoritative catalogs, separate non-stream error manifest,
      including the deferred exact `msg get`/`msg link` error schemas and
      mappings, canonical-key/different-spelling collision gates with legal
      identical-key kind coexistence, trusted-source lstat/containment checks,
      frozen help/option/lookup precedence, uncataloged meta-command exception,
      deterministic errors/output, and complete Linux/macOS package derivation
      plus packaged-binary byte smoke; perform no config/socket/daemon/TDLib
      access
- [x] Add diagnostic `tgcli version` build revision reporting for every
      trustworthy checkout, including tags, linked worktrees, canonical
      symlink-spelled source roots, and shallow tagless release checkouts;
      reject unrelated parent-checkout leakage; tracked-tree dirtiness,
      fail-closed identity/status handling, no-op header regeneration,
      dependent rebuilds, schema and reviewed absent/clean/dirty human
      goldens, and release-provenance consistency tests
- [ ] Shell completions (bash/zsh/fish), man pages
- [ ] docs/schemas/ — freeze curated JSON schemas per command; keep persistence
      schemas out of command catalogs unless a later reviewed mapping explicitly
      adds them, and byte-preserve semantic markers only for explicitly
      cataloged marked command schemas
- [ ] Validate the complete M1–M6 test-DC E2E suite at the M7 gate, including
      fake-boundary coverage and explicit skip reasons for states the test DC
      or available account capabilities cannot exercise
- [ ] Static musl Linux binary + macOS universal binary release job
- [ ] Packaging: AUR, Homebrew; systemd user unit example for `tgcli daemon run`
- [ ] Review gate: M7 diff vs DESIGN.md
- [ ] v1.0

## Post-1.0 ideas

- [ ] Account/profile/privacy commands (not hidden M6 requirements)
- [ ] MCP server mode (`tgcli mcp` over stdio)
- [ ] Offline search: client-side filtering over prefetched local history
- [ ] Secret chats
- [ ] General `tgcli send --file` media autodetection, captions, albums and
      spoilers
- [ ] Scheduled-message management
- [ ] Message translation
