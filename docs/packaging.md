# v1 packaging

The repository contains prepared package definitions for the v1.0.0 release:

- `packaging/aur/PKGBUILD` installs the statically linked Linux x86-64 release
  archive as the `tgcli-bin` package.
- `packaging/homebrew/tgcli.rb` installs the macOS universal release archive.
- `packaging/systemd/tgcli@.service` is a hardened systemd user-service
  template for the foreground daemon.

The AUR and Homebrew definitions intentionally contain all-zero SHA-256
placeholders. They must not be published or used as release metadata until the
v1.0.0 tag exists, the release workflow has produced and verified both archives,
and each placeholder has been replaced with the corresponding published
archive digest. `SKIP` and Homebrew's `:no_check` are not accepted substitutes.

## systemd user service

Install and configure tgcli normally before enabling an instance. For account
`main`:

```sh
systemctl --user enable --now 'tgcli@main.service'
```

The instance name is the exact tgcli account name; names containing characters
that systemd escapes should be passed through `systemd-escape`. The unit runs
`tgcli --account %i daemon run` in the foreground, waits for tgcli's native
`READY=1` notification, and relies on SIGTERM for the normal graceful shutdown
path. The executable name is resolved by systemd's fixed executable search
path, not by a shell or user-controlled `PATH` expansion.

The service runs as the current user and neither creates nor changes the account
configuration. With the default XDG locations, tgcli reads configuration from
`$XDG_CONFIG_HOME/tgcli/config.toml`, stores TDLib data below
`$XDG_DATA_HOME/tgcli/accounts/<account>`, stores tgcli state below
`$XDG_STATE_HOME/tgcli/accounts/<account>`, and places sockets below
`$XDG_RUNTIME_DIR/tgcli`. When an XDG home variable is absent, tgcli uses the
documented per-user fallback below `$HOME`; the user manager normally supplies
`XDG_RUNTIME_DIR`.

The unit applies process and filesystem hardening without making the user's XDG
data/state trees read-only. It permits only Unix, IPv4, and IPv6 sockets needed
for the local protocol and Telegram network access. `UMask=0077` complements
tgcli's own exact ownership and mode checks.
