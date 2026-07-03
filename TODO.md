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
- [x] clang-format + clang-tidy configs
- [ ] macOS portability: peer-uid via getpeereid, accept4/SOCK_CLOEXEC and
      sigtimedwait replacements (Linux-only today; validated by the CI matrix)
- [ ] GitHub Actions: Linux + macOS build/test matrix, ccache + tdlib build cache;
      sanitizer jobs (ASan/UBSan full suite; TSan fake-boundary suite only)
- [x] Choose license (MIT vs Apache-2.0; must be fine linking BSL-1.0 tdlib)
- [ ] Review gate: M0 diff vs DESIGN.md

## M1 — Auth & accounts

- [ ] Authorization FSM (`updateAuthorizationState` handling, daemon-side) —
      all states incl. waitRegistration, waitOtherDeviceConfirmation (QR),
      waitEmailAddress/waitEmailCode; remote-revocation handling
- [ ] Challenge/response frames for interactive prompts (phone, code, 2FA password);
      challenge ownership: per-account auth lease, abort on client disconnect
- [ ] Config loading (config.toml, XDG paths), `*_cmd` secret hooks,
      mtime-based reload in the daemon
- [ ] `tgcli login` (phone/code/2FA via challenges), `--qr`, `--bot-token`;
      prompts for api_id/api_hash on first run and persists them to config
- [ ] `tgcli logout` (destructive gate), `tgcli me`, full `tgcli doctor`
- [ ] `tgcli account add|list|show|use|remove`; per-account state isolation;
      remove is destructive with default server-side logout / `--keep-session`
- [ ] `tgcli daemon status|stop|restart`; `idle_exit` config
- [ ] Review gate: M1 diff vs DESIGN.md

## M2 — Read path

- [ ] Resolver: @username / id / t.me link / title substring; `candidates` on
      ambiguity; integer selectors are always ids; title matching read-tier-only
- [ ] Output layer: human renderers, `--json`, exit-code mapping, stderr
      discipline; opaque `--cursor` pagination tokens
- [ ] docs/schemas/ established (mutable until the M7 freeze) for every
      implemented command; contract tests assert against them from here on
- [ ] `tgcli chats` (folders, archived, unread filters, pagination)
- [ ] `tgcli read` (limits, --before, --since/--until, --topic, --local)
- [ ] `tgcli msg get`, `tgcli msg link`, `tgcli resolve`
- [ ] `tgcli search` (per-chat, --global, filters; server-side only)
- [ ] `tgcli unread`
- [ ] `tgcli fetch <chat>` (--limit/--all/--since, resumable, progress frames)
- [ ] `tgcli chat info`, `tgcli chat members`
- [ ] Review gate: M2 diff vs DESIGN.md

## M3 — Safety & write path

- [ ] Safety chokepoint: Read/Write/Destructive tiers in static command descriptors,
      enforced daemon-side
- [ ] Write gate, default deny: `--allow-write` / `TGCLI_ALLOW_WRITE` /
      per-account `allow_write` (daemon-enforced, exit 6 without a grant);
      `TGCLI_ALLOW_WRITE=0` explicit deny overrides all grants; `login`
      tier-exempt
- [ ] Two-phase destructive confirmation: challenge frame with resolved target →
      TTY prompt client-side; `--yes` for scripts; fails closed without a TTY
- [ ] `--dry-run` planner
- [ ] Audit log (JSONL per account, size-based rotation, raw-secret redaction)
- [ ] `--idempotency-key`: record-then-send store, pending/completed replay
      semantics (pending → exit 1 IDEMPOTENCY_PENDING), payload fingerprint
      check, 7-day expiry; gate checked before the store
- [ ] `tgcli send` (text, --md/--html, --reply-to, --silent, --schedule);
      waits for updateMessageSendSucceeded, returns the final message id
- [ ] `tgcli msg edit|delete|forward|react|pin|unpin`
- [ ] `tgcli chat mark-read|mute|unmute|pin|unpin|archive|unarchive|join|leave`
- [ ] Review gate: M3 diff vs DESIGN.md (safety chokepoint gets extra scrutiny)

## M4 — Files & media

- [ ] `tgcli download` with progress frames (stderr bar / NDJSON); transfers
      unlimited by default, `--timeout` opt-in
- [ ] `tgcli send --file` uploads (photos/video/voice/documents autodetect), --caption, albums
- [ ] `TGCLI_MEDIA_DIR` handling
- [ ] Review gate: M4 diff vs DESIGN.md

## M5 — Streaming

- [ ] Update-bus subscriptions with filters, multiplexed to any number of clients
- [ ] `tgcli listen` (--chat, --types, --count, --timeout; NDJSON; planned expiry → exit 0)
- [ ] `tgcli wait-for` (--chat, --from, --regex, --after for race-free
      send-then-wait — requires --chat, --timeout → exit 7)
- [ ] Review gate: M5 diff vs DESIGN.md

## M6 — Long tail

- [ ] `tgcli contact list|search|add|remove|block|unblock`
- [ ] `tgcli folder list|show|create|edit|delete|add-chat|remove-chat`
- [ ] `tgcli topic list|create|edit|close|reopen`
- [ ] `tgcli chat set-title|set-photo|set-description|invite-link|promote|demote|ban|unban|kick|set-permissions`
- [ ] `tgcli session list|terminate`
- [ ] `tgcli storage stats|optimize` (tdlib file-store usage, optimizeStorage)
- [ ] Review gate: M6 diff vs DESIGN.md

## M7 — Polish & release

- [ ] `tgcli raw` passthrough (td_api_json), read-only allowlist, auth-type
      refusal + audit redaction
- [ ] `tgcli schema <command> [--all]` — runtime dump of curated schemas
- [ ] Shell completions (bash/zsh/fish), man pages
- [ ] docs/schemas/ — freeze curated JSON schemas per command
- [ ] E2E suite against Telegram test DC (`TGCLI_TEST_DC=1`) wired as a
      nightly job + milestone-gate check (not a per-PR blocker)
- [ ] Static musl Linux binary + macOS universal binary release job
- [ ] Packaging: AUR, Homebrew; systemd user unit example for `tgcli daemon run`
- [ ] Review gate: M7 diff vs DESIGN.md
- [ ] v1.0

## Post-1.0 ideas

- [ ] MCP server mode (`tgcli mcp` over stdio)
- [ ] Offline search: client-side filtering over prefetched local history
- [ ] Secret chats
- [ ] Scheduled-message management
- [ ] Message translation
