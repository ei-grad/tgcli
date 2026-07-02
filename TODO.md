# TODO

Roadmap milestones from [DESIGN.md](DESIGN.md) §14. Work top-down; each item
should land with tests and keep the build green.

## M0 — Scaffold & process model

- [ ] CMake project (≥3.24) with presets: debug, release, release-static
- [ ] Pin tdlib source revision; FetchContent build + `-DTGCLI_SYSTEM_TDLIB=ON` option
- [ ] Vendor deps via FetchContent: CLI11, nlohmann/json, fmt, tomlplusplus, Catch2
- [ ] `TdClient` core: ClientManager thread, request/response correlation, update bus
- [ ] Daemon skeleton: `tgcli daemon run`, unix socket ($XDG_RUNTIME_DIR/tgcli/<name>.sock,
      0600, SO_PEERCRED check), JSONL frame protocol (result/error/item/progress/challenge)
- [ ] Auto-spawn: fork + re-exec on missing socket, readiness handshake,
      flock-settled spawn races
- [ ] `--no-daemon` in-process debug mode (same dispatch path, refuses if daemon holds lock)
- [ ] `tgcli version` / `tgcli doctor` round-trip through the daemon (tdlib
      version via `getOption("version")`)
- [ ] clang-format + clang-tidy configs
- [ ] GitHub Actions: Linux + macOS build/test matrix, ccache + tdlib build cache
- [ ] Choose license (MIT vs Apache-2.0; must be fine linking BSL-1.0 tdlib)

## M1 — Auth & accounts

- [ ] Authorization FSM (`updateAuthorizationState` handling, daemon-side)
- [ ] Challenge/response frames for interactive prompts (phone, code, 2FA password)
- [ ] Config loading (config.toml, XDG paths), `*_cmd` secret hooks
- [ ] `tgcli login` (phone/code/2FA via challenges), `--qr`, `--bot-token`;
      prompts for api_id/api_hash on first run and persists them to config
- [ ] `tgcli logout` (destructive gate), `tgcli me`, full `tgcli doctor`
- [ ] `tgcli account add|list|show|use|remove`; per-account state isolation
- [ ] `tgcli daemon status|stop|restart`; `idle_exit` config

## M2 — Read path

- [ ] Resolver: @username / id / t.me link / title substring; `candidates` on ambiguity
- [ ] Output layer: human renderers, `--json`, exit-code mapping, stderr discipline
- [ ] `tgcli chats` (folders, archived, unread filters, pagination)
- [ ] `tgcli read` (limits, --before, --since/--until, --topic, --local)
- [ ] `tgcli msg get`, `tgcli msg link`, `tgcli resolve`
- [ ] `tgcli search` (per-chat, --global, filters, --local)
- [ ] `tgcli unread`
- [ ] `tgcli fetch <chat>` (--limit/--all/--since, resumable, progress frames)
- [ ] `tgcli chat info`, `tgcli chat members`

## M3 — Safety & write path

- [ ] Safety chokepoint: Read/Write/Destructive tiers in static command descriptors,
      enforced daemon-side
- [ ] Write gate, default deny: `--allow-write` / `TGCLI_ALLOW_WRITE` /
      per-account `allow_write` (daemon-enforced, exit 6 without a grant)
- [ ] Two-phase destructive confirmation: challenge frame with resolved target →
      TTY prompt client-side; `--yes` for scripts; fails closed without a TTY
- [ ] `--dry-run` planner
- [ ] Audit log (JSONL per account)
- [ ] `--idempotency-key` store + replay
- [ ] `tgcli send` (text, --md/--html, --reply-to, --silent, --schedule)
- [ ] `tgcli msg edit|delete|forward|react|pin|unpin`
- [ ] `tgcli chat mark-read|mute|unmute|pin|unpin|archive|unarchive|join|leave`

## M4 — Files & media

- [ ] `tgcli download` with progress frames (stderr bar / NDJSON), `--timeout`
- [ ] `tgcli send --file` uploads (photos/video/voice/documents autodetect), --caption, albums
- [ ] `TGCLI_MEDIA_DIR` handling

## M5 — Streaming

- [ ] Update-bus subscriptions with filters, multiplexed to any number of clients
- [ ] `tgcli listen` (--chat, --types, --count, --timeout; NDJSON)
- [ ] `tgcli wait-for` (--chat, --from, --regex, --timeout → exit 7)

## M6 — Long tail

- [ ] `tgcli contact list|search|add|remove|block|unblock`
- [ ] `tgcli folder list|show|create|edit|delete|add-chat|remove-chat`
- [ ] `tgcli topic list|create|edit|close|reopen`
- [ ] `tgcli chat set-title|set-photo|set-description|invite-link|promote|demote|ban|unban|kick|set-permissions`
- [ ] `tgcli session list|terminate`

## M7 — Polish & release

- [ ] `tgcli raw` passthrough (td_api_json), read-only allowlist
- [ ] Shell completions (bash/zsh/fish), man pages
- [ ] docs/schemas/ — freeze curated JSON schemas per command
- [ ] Integration test suite against Telegram test DC (`TGCLI_TEST_DC=1`)
- [ ] Static musl Linux binary + macOS universal binary release job
- [ ] Packaging: AUR, Homebrew; systemd user unit example for `tgcli daemon run`
- [ ] v1.0

## Post-1.0 ideas

- [ ] MCP server mode (`tgcli mcp` over stdio)
- [ ] Secret chats
- [ ] Scheduled-message management
- [ ] Message translation
