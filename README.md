# tgcli

A fast, single-binary Telegram CLI built in C++ on [tdlib/td](https://github.com/tdlib/td) —
for humans at the shell and for AI agents alike.

```bash
tgcli chats -n 20
tgcli read @somechannel -n 50
tgcli send @user "hi there"
tgcli search "release notes" --global --json
tgcli wait-for --chat @buildbot --regex 'deploy (succeeded|failed)' --timeout 600
```

**Status: pre-release development.** [DESIGN.md](DESIGN.md) defines the target
contract; [TODO.md](TODO.md) is the authoritative record of what is currently
implemented and what remains.

## Target v1 highlights

- **No cache ceremony** — tdlib's local database serves reads instantly and
  fetches from the server on miss; server-side full-text search included.
- **Fail-closed writes** — reads are free, but acting on behalf of the
  account requires an explicit grant: `--allow-write` per call,
  `TGCLI_ALLOW_WRITE=1` per environment, or `allow_write = true` granted once
  in the account config for ceremony-free everyday use. `TGCLI_ALLOW_WRITE=0`
  is a hard deny overriding any standing grant — the one-variable sandbox for
  agent harnesses. Destructive actions additionally require confirmation
  (`--yes` when scripted).
- **Agent-friendly** — plain JSON output (no envelopes), stable schemas,
  meaningful exit codes, `--dry-run`, idempotency keys.
- **Streaming primitives** — `listen` (NDJSON updates) and `wait-for`
  (block until a matching message arrives).
- **Full API reach** — `tgcli raw '<td_api JSON>'` escape hatch before a
  dedicated subcommand exists.
- **Daemon-first** — a per-account daemon (auto-spawned, zero setup) owns the
  tdlib client: ~instant CLI startup, safe concurrent commands, and a local
  DB that stays continuously warm.
- **Multi-account**, fully isolated state per account.

## License

[MIT](LICENSE).
