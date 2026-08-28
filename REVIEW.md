# Review Rules

A PR that satisfies every rule below is acceptable — merge it; no further
approval layers exist. A PR that violates any rule is returned with the rule
cited. These rules bind human and agent reviewers equally.

How to review: read the DESIGN.md sections the PR touches, then the diff,
then check the rules in order. Findings must cite a rule or a DESIGN.md
section — "I would have done it differently" is not a finding.

## 1. Scope

- The PR does one coherent thing and maps to TODO.md item(s), a filed
  fix-task, or an agreed spec change. No drive-by refactors, reformatting of
  untouched code, or opportunistic features.
- The diff is reviewable as a unit. If it can't be understood in one sitting,
  it should have been split.

## 2. Spec conformance

- Observable behavior matches DESIGN.md: command surface and flags, output
  contract and schemas, exit codes, safety tiers, protocol frames, selector
  semantics, config/env keys.
- Every user-visible behavior the PR introduces is covered by DESIGN.md —
  either it already was, or the PR extends DESIGN.md in the same change (see
  §7 for which changes may do this). Undocumented behavior is silent drift
  and is rejected.
- CLAUDE.md invariants hold: single safety chokepoint, td_api.h only in
  daemon-side implementation TUs (core/ and command .cpp files — never in
  public headers or client-side code), stdout carries data only, no bespoke
  message store, secrets never via argv or written to disk.
- If code and DESIGN.md disagree, DESIGN.md wins. Either fix the code or
  change the spec through §7 — a PR must never leave the two in
  contradiction.

## 3. Tests

- New behavior has contract coverage per the testing policy (CLAUDE.md):
  driven through the real dispatch path, asserting observables only.
- Bug fixes come with a test that fails without the fix.
- No mock-verification tests; no re-testing of centrally-tested shared
  semantics per command; no tests asserting implementation structure.
- Test deletions/weakenings are justified in the PR description in terms of
  the policy (redundant, implementation-mirroring). Weakening a test so CI
  passes is rejected.
- Golden-file regeneration is explained: what changed in the rendering and
  why that is intended.

## 4. Mechanical gate

All required CI green: build matrix, unit + contract + golden tests,
clang-format, clang-tidy, sanitizers (ASan/UBSan; TSan over the
fake-boundary suite). A reviewer does not re-litigate what CI proves; a
reviewer also never waives a red required check.

The E2E test-DC suite is deliberately **not** a per-PR required check — it
depends on an external, rate-limited, periodically-wiped service. M0 is
expressly exempt because it has no authentication. M1 must establish the
harness and nightly job with an auth smoke flow; each M2–M6 milestone gate
must add a flow for a supported feature. M7 validates the already-complete
accumulated suite rather than introducing E2E. A required state unavailable
in the test DC, including a Premium-only state, must have fake-boundary
contract coverage and an explicit E2E skip reason. At an applicable milestone
gate, a reproducible E2E regression blocks the milestone; a documented
infrastructure flake gets a rerun.

## 5. Code

- Matches surrounding idiom, naming, and structure; new dependencies require
  explicit justification in the PR description.
- Comments state non-obvious invariants only — no narration of edits, no
  dialog residue, no commented-out code.
- Error paths produce structured errors with the correct exit code; no
  swallowed tdlib errors; no `catch (...)`-and-continue without a stated
  invariant.
- No TODO/FIXME without a corresponding TODO.md entry or filed task.

## 6. Safety-sensitive changes

A diff touching the safety chokepoint, write gate, confirmation flow, audit
log, idempotency store, auth FSM, or socket permissions gets an adversarial
pass: the reviewer actively tries to construct an input or sequence that
performs a Telegram-side write without a grant, a destructive action without
confirmation, or a secret reaching argv/disk/logs. Fail-closed behavior must
be demonstrated by tests, not argued in prose.

Raw is always security-sensitive and audit-schema-changing. Its review owns
the complete pin-derived Function inventory/policy bijection, every admitted
row and compiled body-validator variant, request and response sensitivity,
duplicate/unknown-field/numeric/int64/bytes/double canonical hash vectors,
secret-chat provenance, tier monotonicity, wipe paths, audit-v3 crash cuts and
the parser/descriptor/audit/schema/catalog activation order. A global default
deny can be reviewed as a dormant decision; it is not evidence that a denied
row is safe to admit later.

Download review covers source and destination parent/final symlinks, stable
descriptor identity and count, Linux/macOS exclusive rename, publish versus
deadline/auth races, cleanup failure precedence and the explicit named-temp
orphan crash limitation. Search review covers cleaner byte-equality for both
the query and untrusted cursor offsets plus page/marker/raw-order progress.
Completion review proves registry/generated/checked-in/package/runtime byte
equality and that future registry rows are not runtime-active.

## 7. Changing the spec (DESIGN.md, docs/schemas/)

Two classes of spec change, with different processes:

- **Additive or clarifying** — a new command/flag following existing
  patterns, a new schema for a new command, documenting behavior the spec
  left unspecified, wording fixes. Before the M7 schema freeze, changes to
  existing docs/schemas/ fields are also additive-class; after the freeze
  they are contract-class. Allowed in the same PR as the
  implementation. The PR description must carry a `Spec delta:` section
  listing every DESIGN.md/schema change so it is reviewed as a spec change,
  not skimmed as prose.
- **Contract changes** — altering the exit-code table, changing or removing
  existing schema fields, changing frame-protocol or selector semantics, the
  safety model, or the meaning of existing config/env keys; bumping the
  pinned tdlib revision belongs here too (td_api churn can move the typed
  surface and curated schemas). Spec-first: a
  separate PR that changes only DESIGN.md (+ docs/schemas/) with the
  rationale and migration notes, reviewed and merged before any
  implementation PR. An implementation PR that silently alters a contract is
  rejected regardless of code quality.

After v1.0, contract changes additionally require a versioning decision
(major bump + documented migration); frozen schemas are append-only.

## 8. Automatic rejections

Any of the following ends the review immediately, whatever else the PR does
well:

- silent contract drift (behavior differs from DESIGN.md with no spec delta);
- a write path that bypasses the safety chokepoint;
- weakened or deleted tests without a policy-based justification;
- unexplained golden-file regeneration;
- secrets accepted via argv, or written to disk/logs;
- out-of-scope changes bundled with the reviewed change.
- raw JSON accepted from argv, a second raw parse/`from_json`, unknown fields
  accepted at a typed boundary, a caller-selected or lowered raw tier, an
  unknown/unclassified function fallback, a raw request/response body or TD
  message in audit/error/log output, name-only secret-chat proof, or raw
  handler/catalog activation before audit-v3 recovery;
- download publication that follows a symlink, overwrites an existing leaf,
  omits stable source revalidation/exclusive rename, or claims automatic
  cleanup of crash-orphaned named temps;
- completion bytes not derived from the canonical public registry, or a future
  registry row becoming invocable before its atomic handler activation.
