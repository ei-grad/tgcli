# tgcli

Daemon-first Telegram CLI in C++20 on tdlib/td, under pre-release
development. DESIGN.md is the authoritative target spec; TODO.md is the
authoritative implementation status and roadmap (work milestones top-down).

## Ground rules

- DESIGN.md governs the command surface, output contract, exit codes, safety
  tiers, and protocol frames. If implementation needs to deviate, update
  DESIGN.md in the same change — no silent drift. Spec changes follow the
  process in REVIEW.md §7 (additive deltas in-PR, contract changes
  spec-first).
- Changes are accepted against REVIEW.md — a PR satisfying every rule there
  is mergeable; one violating any rule is returned.
- Every task lands with the tests the testing policy below calls for and
  none it forbids, and keeps the build green.

## Invariants

- stdout carries data only; progress, warnings, and diagnostics go to stderr.
- The exit-code table (DESIGN.md §5) and curated JSON schemas under
  `docs/schemas/` are stable contracts; changing them is a design change (pre-M7-freeze
  schema edits are additive-class spec deltas, post-freeze contract-class — REVIEW.md
  §7). Result schemas use Draft 2020-12, reject undeclared object properties, and have
  an exact command/file bijection in the result-only `docs/schemas/manifest.json`;
  commands without results are absent. `stream-manifest.json` separately owns the M5
  item/result/error stream mappings, and `error-manifest.json` is the sole non-stream
  command-to-error-schema authority. Catalog references are safe leaf filenames and
  canonical command keys are unchanged by target normalization/aliasing; catalog
  merging rejects collisions only between different raw spellings, deduplicates equal
  `(command,kind,filename)`, rejects equal `(command,kind)` with different filenames,
  and lets different kinds for an identical command coexist. The generator and release
  verifier independently reject wrong-type, symlinked or
  escaping source-root/catalog/schema components before reading them. M7 embeds exact
  catalog/schema bytes as the runtime schema-discovery authority while release
  packages ship and byte-verify the same referenced set. The `schema` introspection
  meta-command is the sole explicit result-manifest exception and is not recursively
  cataloged; it does not permit a free-form result schema for ordinary commands.
  Ordinary schema validity is necessary. The three uncataloged mixed audit
  persistence schemas additionally carry one exact documentation-only marker
  applying only to schema version 2 and naming their frozen filename-owned
  `tgcli-runtime-v1` rules. Standard-expressible calendar, stage,
  terminal-class, basename and lexical-path constraints remain schema
  assertions; field/nested pseudo-assertion keywords are forbidden. Runtime
  acceptance never makes a schema-invalid value valid. Audit markers are
  verified by the audit generator/source tests and are not exposed through or
  added to M7 command catalogs.
- The write gate is fail-closed and evaluated daemon-side. Every
  Telegram-side mutation passes through the single safety chokepoint with a
  statically declared descriptor (Read/AuthBootstrap/Write/Destructive); no
  handler bypasses it.
  M1 activates the minimal authority/confirmation/dry-run/audit kernel only
  for destructive `logout` and `account remove`; M3 extends it to general
  writes.
- AuthBootstrap is a separate grant-exempt descriptor, not a write bypass. It
  admits only DESIGN.md §6's closed auth-function allowlist for the current
  client generation, auth state/sequence and owner; every TDLib send crosses
  that same chokepoint.
- TDLib process-global logging is set to ERROR before client creation and is
  never raised to INFO; `-v` changes only tgcli-owned diagnostics. Authentication
  sentinels must be absent from stderr, active `tdlib.log` and rotated logs.
- All config mutations hold the cross-process config lock and compare the
  planned snapshot identity before mutation. Account removal journals intent,
  ordered checkpoints and outcome in global removals state outside the account
  roots it can delete, and never crosses a mount/device boundary.
- td_api.h appears only in daemon-side implementation translation units
  (core/ and individual command .cpp files) — never in public headers or
  client-side code (cli, output, prompts).
- No bespoke message store: tdlib's database is the cache. tgcli's own
  persistent state is limited to the audit log, the idempotency store,
  removal tombstones, the private crash-recoverable outbound attachment
  spool from DESIGN.md §4.5.12, config.toml (which `login` updates with app
  credentials), and rotated logs. The spool is temporary send staging and
  never becomes a second Telegram message cache.
- Real M3/M4 operations use two deadline-aware outer account-mutex epochs: an
  initial reconciliation/lookup epoch and a revalidated commit epoch. A lookup
  miss releases the first epoch for resolver/property planning. Prior
  reconciliation, M4 pass 1 SHA/fingerprint and lookup occur in the initial
  epoch. Repeated core gate/lookup is authoritative before any current intent.
  Incumbent replay/pending/conflict creates no current group. Only a repeated
  miss proceeds through proposed-plan confirmation, config CAS, exact append
  permit, intent, returned generation, insertion quota and insert-if-absent.
  M4 pass 2 and mutating TD dispatch occur inside the commit epoch, which stays
  held through checkpoints, store transitions, outcome and cleanup. Audit
  groups never interleave.
- A completed same-fingerprint incumbent's stored plan replaces a newly
  resolved plan for replay. Destructive replay freshly confirms that plan while
  held; decline/no-TTY/cancel/deadline creates no intent. Pending/conflict never
  prompts. An unexpected post-intent insert loss closes mutation-none with
  exact INTERNAL then is durability-fatal; crash recovery uses
  AUDIT_INCOMPLETE and never removes the incumbent. No separate inner store
  lock exists.
- `idempotency.db` contains exact canonical JSON bytes and is replaced only
  through the fixed non-authoritative `.idempotency.db.tmp`, file fsync, rename
  and directory fsync protocol. Its public failure reason/precedence table and
  canonical absolute final path are exhaustive. Audit is recovery authority;
  completed clears temporary/progress, pending unknown retains them, and keyed
  plus unkeyed expiry are equality-exact and conservative under clock rollback.
- Session paths pass `AbsentByPolicy`; this is mechanically distinct from
  known-empty pins and performs zero idempotency-store or temp-file I/O. Audit
  rotation protects surviving pins and receipt-bound audit spool holds; only
  cleanup plus spool-root fsync mints the matching release.
- Raw idempotency keys are accepted only at the protocol boundary and are
  converted to the domain-separated hash before any persistence helper,
  filename, error or diagnostic. Store/audit APIs never accept the raw key.
- Real secrets (2FA password, DB encryption key, bot token) are never accepted
  via argv or environment and never written to disk by the tool. Bot login is
  `login --bot` and obtains its token only from `bot_token_cmd` or a no-echo
  challenge; legacy `--bot-token` is consumed only for redacted rejection.
- `raw` remains rejected until its selected-B parser, exhaustive pin policy,
  audit-v3 recovery, handler and catalogs activate atomically. It is stdin-only
  (`raw -`): argv JSON is always rejected. `--full` remains rejected throughout
  v1 and is post-1.0 only.
- Raw parsing is duplicate-rejecting and converts once. Classifier, TD-aware
  canonical hash serializer and future dispatch retain the same actual owned
  `td_api::Function`; a second parse/`from_json` is forbidden. The complete
  pinned function inventory, 3118-constructor type graph and policy are exact
  tl/generated-header bijections with committed count/digests and deny unknown
  drift. Generated missing/default/null and abstract-variant rules apply before
  the single native conversion. Native responses must match the holder's
  declared result base (or exact `td_api::error`) before canonical hashing and
  are consumed through an RAII recursive native wipe. Generated body-validator
  descriptors bind each name to one nonnull compiled decision callable;
  runtime lookup uses the same fail-closed table and derives a typed effective
  tier from the static row tier without permitting a decrease. The dormant
  candidate has explicit evidence for all 1001 rows and admits only 57 typed
  functions (8 local transforms, 45 direct-chat-target functions, 3 with an
  additional required member sender, and 1 with an optional sender); the other
  944 are frozen whole-function denies. Generated typed preflight planners
  collect every direct and nested chat selector. It remains activation-blocked
  on independent policy acceptance. A body validator can only retain/raise
  tier or deny, and unknown/null/unmatched nested variants deny.
- Raw request/response bodies, TD messages, credentials and curated preflight
  output never enter audit, errors, diagnostics or logs. Request/response
  staging is wiped on every terminal path. Dormant hash-only audit-v3 schemas,
  validator/scanner/recovery and no-resend crash cuts must land before any raw
  registration.
- Download source and destination paths use no-follow descriptor walks, stable
  source identity and exclusive no-replace publication. Normal failures clean
  and fsync the temp directory; v1 deliberately does not sweep a named 0600
  temp left by a process crash.
- Public shell completion bytes are generated only from the checked-in command
  registry and must equal checked-in/package/runtime bytes. Scripts never run
  tgcli or inspect config/network. No v1 output contains ANSI, so `--no-color`
  and nonempty `NO_COLOR` are byte-preserving no-ops.

## Build & dev

- CMake ≥ 3.24 with presets; C++20; clang-format and clang-tidy clean.
- Dev loop uses a prebuilt tdlib (`-DTGCLI_SYSTEM_TDLIB=ON`) plus ccache —
  never rebuild tdlib from scratch per checkout. The prefix must be produced
  by `scripts/build-tdlib.sh` at the pinned revision (distro tdlib packages
  lack the JSON-conversion headers selected-B raw activation needs).
- Sanitizers must pass in CI: ASan/UBSan over the full suite; TSan over the
  fake-boundary unit/contract suite only (tdlib is uninstrumented —
  full-program TSan would need a TSan-built tdlib).

## Testing policy

Tests pin the external contract; they never mirror the implementation.

- **Contract tests are the default.** Drive a command through the real
  dispatch path (in-process `--no-daemon` mode) against the shared scripted
  fake of the td_api boundary, and assert only observables: the td_api
  requests actually emitted (as data), output JSON against docs/schemas/,
  exit code, stderr discipline. One high-fidelity fake, maintained in one
  place — no ad-hoc per-test mocks.
  Marked audit schemas have separate ordinary-schema, runtime-only and
  conjunction coverage. Ordinary-validator negatives cover every expressible
  failure. Runtime-only pairs cover only the exact marker taxonomy. Every
  runtime-accepted generated record also passes the ordinary schema, and no
  test or prose claims that C++ makes a schema-invalid instance valid.
- **Shared semantics are tested once, centrally.** Write gate, NOT_AUTHED,
  ambiguous resolver, timeout mapping, confirmation challenges live in one
  cross-command suite. A per-command test covers only that command's
  distinctive behavior: typically one happy path plus its specific
  edge/error cases.
- **Unit tests only where real logic lives**: resolver matching, safety-tier
  evaluation, frame codec, pagination cursors, idempotency replay. Never
  assert "handler called method X" via mock verification — a test that
  breaks on refactoring without a contract change is a bad test and should
  be deleted, which is a valid change on its own. Coverage percentage is not
  a goal.
- **Golden files** cover human renderers; regenerating them is a deliberate,
  reviewed act, not a reflex on failure.
- **E2E against the Telegram test DC** (`TGCLI_TEST_DC=1`) is a small
  curated suite proving the stack end-to-end; branch coverage belongs to
  contract tests. M0 is exempt because it has no auth. M1 establishes the
  harness, nightly job, and auth smoke; each M2–M6 gate adds a supported
  feature flow; M7 validates the complete accumulated suite. A required
  test-DC or Premium-only state that cannot be induced gets fake-boundary
  coverage and an explicit E2E skip reason. Every test-DC run publishes the
  exact sorted `test-results/tgcli-test-dc-skips.json` artifact, even when it
  is empty. The real-TD M1 sentinel runs concurrent `-v` authentication and
  scans stderr, the active TDLib log and every rotated log for each credential
  byte string. E2E is not a per-PR blocker
  because the service is external, rate-limited, and periodically wiped
  (REVIEW.md §4).
