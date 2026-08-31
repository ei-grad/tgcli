# tgcli

A fast, single-binary Telegram CLI built in C++ on [tdlib/td](https://github.com/tdlib/td) —
for humans at the shell and for AI agents alike.

```bash
tgcli chats -n 20
tgcli read @somechannel -n 50
tgcli send @user "hi there"
tgcli search "release notes" --global --json
tgcli wait-for --chat @buildbot --regex 'deploy (succeeded|failed)' --timeout 600
tgcli completion bash > ~/.local/share/bash-completion/completions/tgcli
```

**Status: v1.0.0 release candidate.** The local v1 contract and build version
are frozen, but no v1.0.0 tag or public package has been published.
[DESIGN.md](DESIGN.md) defines the contract; [TODO.md](TODO.md) records the
remaining external release evidence. Prepared AUR, Homebrew, and systemd assets
are documented in [docs/packaging.md](docs/packaging.md).

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
- **Reviewed raw subset** — selected-B `tgcli raw -` is stdin-only; its
  independently accepted pinned policy admits 54 typed functions and denies
  the other 947. It is a deliberately narrow reviewed subset, not general
  access to the full TDLib API.
- **Daemon-first** — a per-account daemon (auto-spawned, zero setup) owns the
  tdlib client: ~instant CLI startup, safe concurrent commands, and a local
  DB that stays continuously warm.
- **Multi-account**, fully isolated state per account.

## License

[MIT](LICENSE).
