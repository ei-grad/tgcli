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
      JSON-conversion headers required by selected-B raw activation
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

- [x] **M1.1 config/bootstrap:** strict loader, 1 MiB bound, one-second idle
      watcher/two-second publish-or-reject deadline and immutable last-good
      snapshots; invalid-reload standing-grant deny and config-global current-
      file reads; cross-process `config.lock`, snapshot-identity CAS and
      symlink-safe atomic 0600 mutation. Pin query-1 `getAuthorizationState`
      response-first/update-first sequence behavior, every one of the 14
      `setTdlibParameters` fields,
      exact phone/QR/registration settings, deterministic implicit-main
      materialization, bounded/redacted per-field `*_cmd` fallback, and fully
      isolated/propagated `TGCLI_TEST_DC=1` roots and parameter identity.
- [x] **M1.2 auth core:** source-aware `(client_id, query_id)` correlation,
      immutable auth snapshots, exhaustive 13-state pinned FSM including
      `waitPremiumPurchase`, repeated QR updates, ready-loss termination with
      generic reason, and non-shutdown `Closed` replacement. Lifecycle-owned
      login/logout/removal/close waiters accept every response/update ordering,
      resolve their waiter before the unrelated-generation sweep, and preserve
      exactly one terminal; credential and unknown 400/401/429/5xx mapping is
      closed and tested.
- [x] **M1.3 challenge/login:** challenge identity binds connection/request,
      client generation, auth sequence, nonce and sequence; same-state updates
      supersede old input; answer/deadline acceptance is atomic. Cover pre-send
      disconnect, serialized/orphaned in-flight auth queries, cancellation and
      one-deadline/one-terminal behavior; phone/code/email/2FA/database-key and
      registration retry/resume; QR progress and bot hook/no-echo login; reject
      and redact legacy `--bot-token` plus every env/plain-config token path.
- [x] **M1.4 identity/safety:** curated `login`/`me`/`doctor` results; every
      TDLib send crosses the descriptor chokepoint, with AuthBootstrap's closed
      function/state allowlist and all other writes still denied. Implement the
      M1 destructive kernel for `logout`/`account remove` only: authority-source
      precedence, confirmation/`--yes`, dry-run, exact durable intent/outcome
      records, correlated logout completion and fail-closed remote uncertainty.
      Set process-global TDLib logging to ERROR before client creation, keep it
      below INFO for life, and make `-v` affect tgcli diagnostics only.
- [x] **M1.5 accounts/removal:** `account add|list|show|use|remove`, empty-config
      results, exact duplicate/missing/default-reassignment behavior, target-
      daemon routing and config/tdlib/state/socket isolation. Default removal
      versus `--keep-session` uses a global non-deletable audit/tombstone,
      ordered crash-safe remote/config/data/state/outcome checkpoints, fresh-
      approval recovery, root identity/CAS validation, and mount/device-
      boundary refusal; no local deletion precedes remote proof.
- [x] **M1.6 daemon/results:** exact auto-spawn/no-spawn matrix and
      `daemon status|stop|restart|run` absent/running behavior; preserve M0
      `USAGE {}` for lifecycle `--no-daemon`; one-second config observation and
      request/challenge/subscription `idle_exit` accounting. Add result-only
      manifest entries and strict Draft 2020-12 schemas for every exact M1
      success/error/detail/audit shape, uint64 request IDs, audit checkpoint
      arrays and `none|possible|confirmed` mutation state while preserving M0
      objects; keep raw rejected until atomic M7 activation and `--full`
      rejected throughout v1.
- [x] **M1.7 fake-boundary acceptance:** drive all 13 auth states and the closed
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
- [x] **M1.8 test-DC and real-TD sentinel harness:** `TGCLI_TEST_DC=1` creates
      and propagates isolated roots/parameters, refuses production state, and
      is wired into the nightly workflow. The deterministic harness covers the
      add/phone/fixed-code registration, `me`, explicitly granted/confirmed
      logout and correlated closed/re-login flows; QR/bot require their named
      fixtures. Concurrent `-v` auth sentinel scans cover stderr, active
      `tdlib.log` and every rotation without permitting TDLib INFO request
      serialization.
- [x] **M1.9 explicit E2E gaps:** every pinned state not deterministically
      forceable in the test DC has M1.7 coverage and a closed skip reason. Every
      run publishes `<build-dir>/test-results/tgcli-test-dc-skips.json`, even
      when empty, with exact sorted entries and only
      `fixture_missing:qr_approver`, `fixture_missing:bot_token_cmd`, or
      `test_dc_state_not_forceable:<state>`; a missing artifact or silent/pass-
      equivalent skip fails the milestone gate.
- [ ] Run the current HEAD M1 live TestDC smoke and sentinel scan with external
      credentials/network; local fake-boundary and workflow contracts cannot
      substitute for this evidence.
- [x] Review gate: M1 diff vs DESIGN.md

## M2 — Read path

- [x] Resolver: exact id/@username/t.me classification and link/bot matrix;
      arbitrary title substring over fully loaded active Main+Archive, local
      materialized-prefix scope, strict ambiguity candidates, and exact
      username `NOT_FOUND` normalization; the finalized `ResolverConsumer`
      must provide the no-send local link classification required by `read
      --local`, never route it through terminal resolve/`getInternalLinkType`
- [x] Shared M2 DTOs and parsing: lossless `TopicRef`, `MessageSummary`,
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
- [x] Output layer: equivalent human/JSON rendering, exact result/error shapes,
      stderr discipline, and self-contained untrusted cursors without a MAC or
      daemon state; reject bad scope and non-advancing upstream markers
- [x] Keep strict Draft 2020-12 result schemas and manifest entries for the
      active `chats`, `read`, `msg get`, `msg link`, `unread`, `fetch`, and
      `resolve` commands; keep `history` schema-less as the canonical `read`
      alias, and add exact command-local error schemas/catalog mappings for
      active chats/unread/read/msg-get/msg-link/fetch
- [x] Freeze and materialize uncataloged future result/error schemas plus exact
      cleaner, pagination, source, identity and sender contracts for `search`,
      `chat info`, and `chat members`; preserve user/chat member senders and
      keep every path unreachable until its handler and catalog activation
- [x] Implement and atomically activate `search`, `chat info`, and `chat
      members` from those dormant assets, including typed TD factories,
      converters, authorization/source rows, coordinators and catalog entries
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
      dispatcher/fake/native coverage, exact command-local error schemas and
      error-catalog mappings, and no new TestDC skip
- [x] `tgcli saved tags|search`: user-account-only reaction-tag discovery plus
      tag-only/tag+text search; canonical emoji/custom selector round-trip,
      label/count/order output, account/scope/filter-bound cursor pagination,
      and contract coverage for NOT_AUTHED/BOT_UNSUPPORTED preflight, malformed
      selectors/cursors, cursor/filter mismatches, paid/unknown variants, empty
      matches, and reaction tags vs emoji text
- [x] `tgcli unread`: fully load and deduplicate Main then Archive, apply the
      shared unread predicate, skip secret chats, and return `next:null`
- [x] `tgcli fetch <chat>`: default/finite/since/all targets, continuous local
      prefix plus one live-fill transition, exact progress and phase-aware timeout
      details, strict result schema/runtime invariants, recovery ordering, and the
      public-TDLib `tdlib_idle` limitation without false EOF/completeness claims
- [x] Landed Saved test-DC flow: after auth smoke, run `tgcli saved tags`,
      validate the unpaginated tag-list result, and retain the exact sorted
      test-DC skip artifact contract
- [x] General M2 read test-DC flow: after auth smoke, run
      `tgcli chats -n 1 --json` and require exit 0 plus a schema-valid empty or
      non-empty list without a pre-created fixture
- [x] Review gate: M2 diff vs DESIGN.md

## M3 — Safety & write path

- [x] Implement the frozen protocol-v3 precursor: strict nine-field context
      with `idempotency_key`, Hello-first parsing/writing, v1/v2↔v3 frozen
      control replacement, exact status/stop/restart/autospawn behavior, and
      auth-bound M3/M4 dry-run read allowlist/effect fixtures, including
      type- and id-bound `getUser`/`getSupergroup` `ChatIdentity` enrichment
      with no direct/raw/title-candidate admission or non-identity field
      retention; exact `resolve`-attributed enrichment failures and same-Ready
      arbitration; and zero config/audit/idempotency/spool/prior-group-
      reconciliation or other tgcli persistence mutation across all seventeen
      planner dry-runs
- [x] Reuse exact M2 resolver/principal DTOs, add static operation tiers and
      the user/bot/schedule admission matrix, and extend the single daemon-side
      safety chokepoint to every M3 Write/Destructive descriptor
- [x] Add neutral TD request/update DTOs, strict M3 results/errors/plans, and
      direct-response/auth-update/deadline arbitration without exposing
      `td_api.h` outside daemon implementation translation units
- [x] Add strict mixed-v1/v2 per-account audit schemas, factories, streaming
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
- [x] Align session/M3 AUDIT_UNAVAILABLE schemas with the complete accepted
      durability_reason enum; reject rotate_failed in v2 while retaining the
      separate v1 audit_reason, and generator-check every runtime audit record.
- [x] Add canonical JSON, domain-separated hashes and complete per-operation
      fingerprints with exact canonical byte/golden vectors and raw-key/invite
      sentinel gates.
- [x] Extend the accepted audit API with open-group generation, the exact
      immutable streamed completed-group view, one-pass tuple pin validation,
      move-only append permits and receipt-bound audit spool holds/releases;
      retain M6 `AbsentByPolicy` as zero store/temp I/O and hole-only rotation.
- [x] Add the strict canonical-byte idempotency store and reconciliation:
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
- [x] Add shared direct and single-message coordinators with immutable plans,
      schedule ceiling/boundary rechecks, strict timeout oneOf, exact terminal
      ordering, and no post-dispatch cancellation claim
- [x] Implement `tgcli send` text/Markdown/HTML/reply/forum-topic/silent/
      schedule paths, wait for authoritative final send state, and return the
      exact `MessageWriteResult`
- [x] Implement `tgcli msg edit|react|pin|unpin` with exact property
      validation, reaction availability, plan, audit, timeout and idempotency
      behavior
- [x] Implement destructive `tgcli msg delete`, including confirmation of the
      immutable plan on new invocation and completed replay, plus
      `completed_unchanged` replay-confirmation timeout
- [x] Implement destructive `tgcli chat leave` with the same immutable-plan,
      confirmation, replay, timeout, audit, and idempotency guarantees
- [x] Implement the ordered `tgcli msg forward` vector coordinator with one
      strict `ForwardItem` across success/error/timeout/audit/store, partial
      outcomes, deleted-before-confirmation handling, 429 aggregation, and the
      legal initial plus 100 terminal-transition progress sequence
- [x] Implement `tgcli chat mark-read|mute|unmute|pin|unpin|archive|unarchive|join`
      with exact direct-call state machines, invite secrecy and
      notification-setting plans
- [x] Add the complete fake-boundary/fault/cutpoint/canonicalization/sentinel
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
- [x] Add the mandatory no-skip Saved text-send TestDC flow with authoritative
      final-id validation, immediate cleanup registration, get/replay/conflict
      verification, cleanup on every exit, and post-delete absence proof
- [x] Review gate: M3 diff vs DESIGN.md (safety chokepoint gets extra scrutiny)

## M4 — Files & media

- [x] Freeze the dormant `download` source/destination/progress/crash contract:
      curated media constructors, observer-before-send, advisory progress,
      descriptor-walked stable source identity, same-directory 0600 temp,
      exclusive no-replace Linux/macOS publication, cleanup precedence, exact
      schemas and explicit named-temp orphan non-guarantee
- [x] Implement and atomically activate `tgcli download` with stderr progress,
      unlimited default, opt-in timeout, curated descriptor/observer path and
      result/error catalog mappings; raw `downloadFile` remains denied
- [x] Add the dormant two-pass file snapshot/spool foundation: exact
      no-follow frozen-locator replay and post-pass entry/FD revalidation,
      strict source errors, pass-1 full identity/digest, winner-only
      post-intent pass 2, total private-root classification, fsynced one-file
      spool, audit-first keyed SpoolRef publication, durable cleanup/ref
      clearing, byte-safe orphan diagnostics plus the effect-based v2 gate,
      prior-group/current-invocation persistence distinctions, Linux/macOS
      portability, and all filesystem/deadline/crash cutpoints; do not register
      `saved attach` or mark the adapter complete in this slice
- [x] Implement `tgcli saved attach <message-id> <PATH> [--caption TEXT]` as a
      user-only, single-file Saved reply adapter preserving the original;
      enforce saved/null topic inheritance and the shared plan/audit/timeout/
      idempotency contract, and return the exact `MessageWriteResult`
- [x] Add `saved attach` result-only manifest/schema and contract coverage for
      final id, NOT_AUTHED/BOT_UNSUPPORTED, NOT_FOUND/USAGE, gate/dry-run,
      two-pass source races, quota/CAS cutpoints, audit, timeout and replay
- [x] `TGCLI_MEDIA_DIR` handling
- [x] Add a supported M4 media flow to the test-DC E2E milestone gate
- [x] Review gate: M4 diff vs DESIGN.md

## M5 — Streaming

- [x] Add strict `listen` item, `wait-for` result, and stream error schemas plus
      the separate exact stream catalog/package bijection and schema-provable
      checked-sum byte-capacity diagnostics
- [x] Land the shared M2 resolver, MessageSummary/ChatIdentity/ChatSummary DTOs,
      sender matching, int53 handling, and local-history components M5 reuses
- [x] Integrate the measured google/re2 `2022-12-01` full-commit archive as a
      static runtime dependency; lock its archive/tree hashes, BSD and embedded
      Lucent notices, offline staging, and no-ICU/PCRE/Abseil provenance checks
- [x] Add tagged unlimited/finite deadlines and stop-aware wait helpers without
      a maximum-time sentinel
- [x] Add generation-scoped metadata bootstrap, the bounded ordered-normalization
      barrier, curated update/reaction/chat normalization, and explicit metadata
      capacity failures
- [x] Add the fixed 32-slot sequentially consistent ingress registry, bounded
      per-subscription SPSC queues, polling workers, overflow causes, and proven
      deferred reclamation
- [x] Add RequestSession item/terminal ordering, complete-write counting,
      transactional activity promotion, teardown, and first-cause arbitration
- [x] Implement exact `listen`/`wait-for` parsing, setup precedence, filters,
      retained `--after` scan/deduplication, bot behavior, and command handlers
- [x] Add silent internal `listen` Result handling, checked per-item stdout
      write/flush, output-failure cancellation, and daemon/no-daemon parity
- [x] Add schema, fake/native, TSan, integration, release-provenance, and no-skip
      Saved TestDC coverage for the complete M5 acceptance matrix
- [x] Review gate: M5 diff vs DESIGN.md

## M6 — Long tail

- [x] Freeze the normative §4.9 curated long-tail contract for all 30 remaining
      verbs: exact grammar/args and identifiers, TDLib a17f87c4 boundary,
      generation binding, strict DTOs/errors, tier/bot/secret matrices,
      22-of-24 idempotency allowlist, two-epoch audit/recovery, redaction,
      canonical TD-cleaned mutation strings, disjoint folder RMW, complete
      rights/transition and structural-sum invariants, no-reread results,
      closed family-local absence schemas, structural-before-progress topic
      cursor precedence with exact server-MessageId validation,
      daemon/no-daemon parity, schemas and acceptance gates
- [x] Add the complete dormant typed TD boundary and conversion foundation for
      contacts, folder cache/snapshots, paged topics, chat admin/member/photo/
      invite-link calls and storage statistics/default optimization; pin every
      constructor, field, variant, bound and forbidden call to a17f87c4
- [x] Add strict shared M6 CLI/frame parsing, canonical numeric/string ids,
      exact-only mutation selectors, closed folder/topic/right/permission/file-
      type enums and the common 60-second admission/generation lifecycle
- [x] Extend WriteKernel/audit/recovery atomically with all 24 mutation
      operations, the exact 22-operation idempotency allowlist, auth-bound
      dry-run planning, redaction, photo spool proof and no-post-mutation-read
      enforcement
- [x] Implement the five domain coordinators bottom-up: contact typed-user
      hydration; generation-scoped folder update cache and full-snapshot RMW;
      bounded all-page topics; member/right/photo/invite administration; and
      Ready-bound storage defaults
- [x] Add all 30 strict result schemas and five family error schemas together
      with result/error catalogs, generated embedded/package assets and the
      24-operation audit schema union; keep source/catalog/runtime bijections
      exact and expose no partially implemented command
- [x] Atomically activate/register the 30-command CLI/frame/safety surface only
      after handlers, native/fake boundaries, WriteKernel, schemas, human
      pretty-JSON fallback and focused tests are complete and green
- [x] Add exhaustive parser/native/fake/generation/pagination/member/spool/
      redaction/crash/TSan/schema/package tests for the §4.9 matrix, plus the
      non-mutating `m6.storage.stats` TestDC evidence flow; run no live
      mutation and keep macOS runtime evidence in CI
- [x] Freeze the §4.7 session-only grammar/frame args, full signed-int64 string
      identity including zero, 17-value device enum, strict DTO/error/human
      output, bot/current/business-bot semantics, deadlines, TDLib acceptance
      meaning and no-idempotency decision
- [x] Add dormant typed Session DTO/conversion, `getActiveSessions` and
      `terminateSession` runtime factories/descriptors/native matchers,
      scripted-fake seams and unregistered safety-policy descriptors at pinned
      TDLib 1.8.65 / a17f87c4cff7b90b278d12b91ba0614383aaee82
- [x] Extend the dormant audit-v2 contract with session_terminate,
      idempotency_key_hash:null and pre-Ready/step-6 prior-group inspection;
      enforce AbsentByPolicy store access, block keyed incomplete groups, and
      use only non-evicting capacity without pin knowledge; expose no command
      and perform no terminate dispatch yet.
- [x] Add the one-of real/dry-run terminate result schema, list result schema,
      manifest entries and deterministic human renderers; keep the public
      `schema` CLI deferred M7
- [x] Add the exact self-contained session error schema with all common,
      session-specific and account-global spool branches
- [x] Implement dormant complete list and terminate handlers with exact
      Ready/getMe/bot/deadline ordering; wire terminate dispatch only through
      the already-integrated destructive authority/confirmation/config-CAS/
      audit-intent/dispatch/proof/outcome path, never around it
- [x] Add exhaustive dormant handler/CLI-frame/schema/human/fake/native/crash/
      auth-loss/deadline tests, including zero and int64-outside-int53 ids, all
      device variants, current-session refusal, public-Ok semantics and
      business-bot exclusion
- [x] Atomically activate/register the complete `session list|terminate`
      CLI/frame surface only after both handlers, safety policy, audit v2,
      schemas/renderers and focused tests are present and passing
- [x] Add the no-skip non-mutating Saved TestDC `m6.session.list` flow with
      verified daemon stop/lock release before `--no-daemon`, plus release-
      provenance checks; do not terminate a live/TestDC session
- [x] Review gate: session-only M6 semantic diff vs DESIGN.md §4.7 and pinned
      TDLib generated API/AccountManager.cpp/Requests.cpp
- [x] Review gate: complete M6 implementation diff vs DESIGN.md §§4.7 and 4.9,
      pinned TDLib generated/source contracts and the one-shot activation
      boundary

## M7 — Polish & release

- [x] Freeze selected raw Option B: stdin-only grammar, parse-once typed TD
      canonicalization/hash/pre-transfer wipe and explicit TDLib ownership
      boundary, Ready/principal ordering, table-owned Read/Write/Destructive
      policy, no idempotency, closed live/dry schemas, and audit-v3
      no-body/no-resend recovery and activation order
- [x] Add pin-derived exhaustive 1001-function inventory/policy foundations for
      clean TDLib a17f87c4: exact tl/header bijection, committed count/digests,
      `doxygen-normalized-v2` evidence that removes only pinned standalone
      Doxygen blocks/lines while preserving every other generated-header byte,
      an exhaustive reviewed dormant candidate with 54 admitted typed rows and
      947 frozen whole-function denies, pinned principal evidence, compiled
      exact direct/nested chat-selector preflight plans, request/response
      sensitivity metadata, whole-function denial of indirect-result dialogs,
      honest allowlist evidence and an independent-acceptance activation
      blocker; count is drift evidence, not a timeless constant
- [x] Add dormant duplicate-rejecting raw parsing, typed canonical value/hash
      vectors, the complete 3118-constructor pinned type graph, one actual
      native `td_api::Function` holder, declared-result response validation,
      and audit-v3 schema/validator/scanner/recovery foundations; no TD raw
      send, parser registration, dispatcher admission or catalog mapping
- [x] Independently accept the exhaustive raw candidate, then atomically
      activate raw parser/handler/dispatcher/audit/result+error catalogs;
      unknown/null/unmatched variants remain denied
- [x] Freeze and prove raw request ownership Option 1: recursively wipe every
      tgcli-owned request exit, move the sole native Function once into pinned
      TDLib with no retained alias, make no post-transfer TDLib-memory
      zeroization claim, and retain recursive wipe for returned native responses
- [x] Implement local `tgcli schema <command-token>... [--all]`: exact
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
- [x] Add the deterministic selected-B public-command registry generator and
      checked-in byte-stable bash/zsh/fish completion assets; retain future
      activation bits so assets cannot expose unimplemented handlers
- [x] Activate `completion bash|zsh|fish` as a client-local meta-command and
      package byte-identical assets, registry and `tgcli(1)` page through one
      generated CMake/Linux/macOS release manifest with unpacked archive checks
- [x] docs/schemas/ — freeze curated JSON schemas per command; keep persistence
      schemas out of command catalogs unless a later reviewed mapping explicitly
      adds them, and byte-preserve semantic markers only for explicitly
      cataloged marked command schemas
- [ ] Validate the complete current-HEAD M1–M6 test-DC E2E suite at the M7
      gate, including explicit skip reasons for states the test DC or available
      account capabilities cannot exercise. The 49-case local fake/workflow
      contract is green, but external TestDC credentials and network execution
      are unavailable in this local gate and cannot be reported as live proof.
- [x] Implement the hermetic static-musl Linux and macOS-universal release jobs,
      including source/tag/version binding, provenance, SBOM, schema/support
      assets, signing separation and deterministic offline contract tests.
- [ ] Execute those jobs on the current HEAD and inspect real Linux static and
      macOS universal artifacts; local Linux builds and source/layout simulation
      are not current-head macOS runtime evidence.
- [x] Prepare fail-closed v1.0.0 AUR and Homebrew definitions plus the hardened
      systemd user-service template, with one generated CMake/Linux/macOS asset
      manifest and deterministic offline install/layout verification.
- [x] Review gate: complete M7 implementation diff vs DESIGN.md, including the
      raw/security activation and version/package/systemd/status closure.
- [ ] Tag, sign and publish v1.0.0, replace package checksum placeholders with
      verified artifact digests, publish the external package definitions, and
      validate the public release artifacts; no local gate can complete this.

## Post-1.0 ideas

- [ ] Decide and version `--full`; v1 rejects it for every command
- [ ] Account/profile/privacy commands (not hidden M6 requirements)
- [ ] MCP server mode (`tgcli mcp` over stdio)
- [ ] Offline search: client-side filtering over prefetched local history
- [ ] Secret chats
- [ ] General `tgcli send --file` media autodetection, captions, albums and
      spoilers
- [ ] Scheduled-message management
- [ ] Message translation
