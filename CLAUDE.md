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
- Real secrets (2FA password, DB encryption key, bot token) are never accepted
  via argv or environment and never written to disk by the tool. Bot login is
  `login --bot` and obtains its token only from `bot_token_cmd` or a no-echo
  challenge; legacy `--bot-token` is consumed only for redacted rejection.
- `raw` and `--full` remain rejected reserved syntax through M6; M7 activates
  them with its explicit schema delta.

## Build & dev

- CMake ≥ 3.24 with presets; C++20; clang-format and clang-tidy clean.
- Dev loop uses a prebuilt tdlib (`-DTGCLI_SYSTEM_TDLIB=ON`) plus ccache —
  never rebuild tdlib from scratch per checkout. The prefix must be produced
  by `scripts/build-tdlib.sh` at the pinned revision (distro tdlib packages
  lack the JSON-conversion headers `raw`/`--full` need).
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
