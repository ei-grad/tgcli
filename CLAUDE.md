# tgcli

Daemon-first Telegram CLI in C++20 on tdlib/td. Currently in design phase:
DESIGN.md is the authoritative spec, TODO.md is the roadmap (work milestones
top-down).

## Ground rules

- DESIGN.md governs the command surface, output contract, exit codes, safety
  tiers, and protocol frames. If implementation needs to deviate, update
  DESIGN.md in the same change — no silent drift. Spec changes follow the
  process in REVIEW.md §7 (additive deltas in-PR, contract changes
  spec-first).
- Changes are accepted against REVIEW.md — a PR satisfying every rule there
  is mergeable; one violating any rule is returned.
- Every task lands with the tests the testing policy below calls for — no
  more, no fewer — and keeps the build green.

## Invariants

- stdout carries data only; progress, warnings, and diagnostics go to stderr.
- The exit-code table (DESIGN.md §5) and the curated JSON schemas under
  docs/schemas/ are stable contracts; changing them is a design change.
- The write gate is fail-closed and evaluated daemon-side. Every
  Telegram-side mutation passes through the single safety chokepoint with a
  statically declared tier (Read/Write/Destructive); no handler bypasses it.
- td_api.h stays confined to core/ translation units; layers above core
  never touch tdlib types.
- No bespoke message store: tdlib's database is the cache. tgcli's only
  persistent state is the append-only audit log and the idempotency store.
- Real secrets (2FA password, DB encryption key) are never accepted via argv
  and never written to disk by the tool.

## Build & dev

- CMake ≥ 3.24 with presets; C++20; clang-format and clang-tidy clean.
- Dev loop uses a prebuilt tdlib (`-DTGCLI_SYSTEM_TDLIB=ON`) plus ccache —
  never rebuild tdlib from scratch per checkout.
- Sanitizers (ASan/UBSan, TSan for daemon code) must pass in CI.

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
  curated suite — roughly one flow per feature area. It proves the stack is
  real end-to-end; branch coverage belongs to contract tests.
