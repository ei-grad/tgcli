from __future__ import annotations

import json
import os
import re
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
VERSION = "1.0.0"
ZERO_SHA256 = "0" * 64
SERVICE_SOURCE = Path("packaging/systemd/tgcli@.service")
SERVICE_PACKAGE = Path("lib/systemd/user/tgcli@.service")


class VersionContractTests(unittest.TestCase):
    def test_cmake_design_and_goldens_use_the_v1_identity(self) -> None:
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        versions = re.findall(
            r"^project\(tgcli VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$",
            cmake,
            flags=re.MULTILINE,
        )
        self.assertEqual(versions, [VERSION])

        version_sources = [
            REPOSITORY / "DESIGN.md",
            REPOSITORY / "tests/golden/version.txt",
            REPOSITORY / "tests/golden/version-commit.txt",
            REPOSITORY / "tests/golden/version-commit-dirty.txt",
            REPOSITORY / "tests/golden/daemon-status-running.txt",
            REPOSITORY / "tests/golden/daemon-restart.txt",
        ]
        for source in version_sources:
            text = source.read_text(encoding="utf-8")
            self.assertNotIn("0.1.0", text, source)
            self.assertIn(VERSION, text, source)

    def test_release_workflow_keeps_tag_to_cmake_version_binding(self) -> None:
        workflow = (REPOSITORY / ".github/workflows/release.yml").read_text(
            encoding="utf-8"
        )
        for required in (
            "^v([0-9]+\\.[0-9]+\\.[0-9]+)$",
            "project\\(tgcli VERSION ([0-9]+\\.[0-9]+\\.[0-9]+) LANGUAGES CXX\\)",
            '"${project_versions[0]}" != "$version"',
        ):
            self.assertIn(required, workflow)


class PackageAssetTests(unittest.TestCase):
    def test_release_manifest_installs_the_systemd_template(self) -> None:
        manifest = json.loads(
            (REPOSITORY / "docs/release/command-assets.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(len(manifest["assets"]), 6)
        self.assertIn(
            {
                "source": SERVICE_SOURCE.as_posix(),
                "package": SERVICE_PACKAGE.as_posix(),
                "shell": None,
            },
            manifest["assets"],
        )

    def test_systemd_template_matches_the_foreground_daemon_contract(self) -> None:
        service = (REPOSITORY / SERVICE_SOURCE).read_text(encoding="utf-8")
        self.assertEqual(
            service,
            """[Unit]
Description=tgcli daemon for account %i
Documentation=https://github.com/ei-grad/tgcli/blob/v1.0.0/docs/packaging.md
Wants=network-online.target
After=network-online.target

[Service]
Type=notify
NotifyAccess=main
ExecStart=tgcli --account %i daemon run
Restart=on-failure
RestartSec=2s
TimeoutStopSec=90s
UMask=0077
NoNewPrivileges=yes
PrivateDevices=yes
PrivateTmp=yes
ProtectControlGroups=yes
ProtectKernelModules=yes
ProtectKernelTunables=yes
ProtectSystem=full
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes

[Install]
WantedBy=default.target
""",
        )

    def test_aur_recipe_is_pretag_fail_closed_and_installs_exact_layout(self) -> None:
        recipe = REPOSITORY / "packaging/aur/PKGBUILD"
        text = recipe.read_text(encoding="utf-8")
        self.assertIn("pkgname=tgcli-bin", text)
        self.assertIn(f"pkgver={VERSION}", text)
        self.assertIn("pkgrel=1", text)
        self.assertIn("arch=('x86_64')", text)
        self.assertIn("options=('!strip')", text)
        self.assertIn(
            "https://github.com/ei-grad/tgcli/releases/download/v${pkgver}/tgcli-${pkgver}-linux-x86_64-musl.tar.gz",
            text,
        )
        self.assertIn(f"sha256sums_x86_64=('{ZERO_SHA256}')", text)
        self.assertNotIn("SKIP", text)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_root = root / "src" / f"tgcli-{VERSION}-linux-x86_64-musl"
            package_root = root / "pkg"
            sources = {
                Path("tgcli"): b"binary\n",
                Path("LICENSE"): (REPOSITORY / "LICENSE").read_bytes(),
                Path("README.md"): (REPOSITORY / "README.md").read_bytes(),
                Path("THIRD_PARTY_NOTICES.md"): (
                    REPOSITORY / "THIRD_PARTY_NOTICES.md"
                ).read_bytes(),
                Path("share/bash-completion/completions/tgcli"): (
                    REPOSITORY / "completions/tgcli.bash"
                ).read_bytes(),
                Path("share/fish/vendor_completions.d/tgcli.fish"): (
                    REPOSITORY / "completions/tgcli.fish"
                ).read_bytes(),
                Path("share/zsh/site-functions/_tgcli"): (
                    REPOSITORY / "completions/_tgcli"
                ).read_bytes(),
                Path("share/man/man1/tgcli.1"): (
                    REPOSITORY / "docs/man/tgcli.1"
                ).read_bytes(),
                Path("share/tgcli/public-command-registry.json"): (
                    REPOSITORY / "docs/commands/public-command-registry.json"
                ).read_bytes(),
                SERVICE_PACKAGE: (REPOSITORY / SERVICE_SOURCE).read_bytes(),
            }
            for relative, content in sources.items():
                destination = archive_root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(content)
            os.chmod(archive_root / "tgcli", 0o755)

            completed = subprocess.run(
                [
                    "bash",
                    "-c",
                    'set -euo pipefail; source "$1"; srcdir="$2"; pkgdir="$3"; package',
                    "bash",
                    str(recipe),
                    str(root / "src"),
                    str(package_root),
                ],
                capture_output=True,
                check=False,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            expected = {
                Path("usr/bin/tgcli"): Path("tgcli"),
                Path("usr/share/bash-completion/completions/tgcli"): Path(
                    "share/bash-completion/completions/tgcli"
                ),
                Path("usr/share/fish/vendor_completions.d/tgcli.fish"): Path(
                    "share/fish/vendor_completions.d/tgcli.fish"
                ),
                Path("usr/share/zsh/site-functions/_tgcli"): Path(
                    "share/zsh/site-functions/_tgcli"
                ),
                Path("usr/share/man/man1/tgcli.1"): Path("share/man/man1/tgcli.1"),
                Path("usr/share/tgcli/public-command-registry.json"): Path(
                    "share/tgcli/public-command-registry.json"
                ),
                Path("usr/lib/systemd/user/tgcli@.service"): SERVICE_PACKAGE,
            }
            self.assertEqual(
                {
                    candidate.relative_to(package_root)
                    for candidate in package_root.rglob("*")
                    if candidate.is_file()
                    and candidate.relative_to(package_root)
                    not in {
                        Path("usr/share/licenses/tgcli-bin/LICENSE"),
                        Path("usr/share/doc/tgcli/README.md"),
                        Path("usr/share/doc/tgcli/THIRD_PARTY_NOTICES.md"),
                    }
                },
                set(expected),
            )
            for installed, source in expected.items():
                self.assertEqual(
                    (package_root / installed).read_bytes(),
                    (archive_root / source).read_bytes(),
                )
            self.assertEqual(
                stat.S_IMODE((package_root / "usr/bin/tgcli").stat().st_mode), 0o755
            )

    def test_homebrew_formula_is_pretag_fail_closed_and_maps_archive_assets(
        self,
    ) -> None:
        formula = (REPOSITORY / "packaging/homebrew/tgcli.rb").read_text(
            encoding="utf-8"
        )
        required = (
            'version "1.0.0"',
            'url "https://github.com/ei-grad/tgcli/releases/download/v1.0.0/tgcli-1.0.0-macos-universal.tar.gz"',
            f'sha256 "{ZERO_SHA256}"',
            "depends_on :macos",
            'skip_clean "bin/tgcli"',
            'bin.install "tgcli"',
            'bash_completion.install "share/bash-completion/completions/tgcli"',
            'fish_completion.install "share/fish/vendor_completions.d/tgcli.fish"',
            'zsh_completion.install "share/zsh/site-functions/_tgcli"',
            'man1.install "share/man/man1/tgcli.1"',
            '(share/"tgcli").install "share/tgcli/public-command-registry.json"',
            'assert_match \'"version":"1.0.0"\'',
        )
        for snippet in required:
            self.assertIn(snippet, formula)
        self.assertNotIn("sha256 :no_check", formula)

    def test_packaging_documentation_keeps_external_release_steps_open(self) -> None:
        documentation = (REPOSITORY / "docs/packaging.md").read_text(encoding="utf-8")
        normalized = " ".join(documentation.split())
        for required in (
            "all-zero SHA-256 placeholders",
            "must not be published",
            "v1.0.0 tag",
            "systemctl --user enable --now 'tgcli@main.service'",
            "$XDG_CONFIG_HOME/tgcli/config.toml",
            "$XDG_DATA_HOME/tgcli/accounts/<account>",
            "$XDG_STATE_HOME/tgcli/accounts/<account>",
        ):
            self.assertIn(required, normalized)


if __name__ == "__main__":
    unittest.main()
