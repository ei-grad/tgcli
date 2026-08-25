import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFIER = REPO_ROOT / "scripts/verify_tdlib_stream_source_contract.py"
TDLIB_SOURCE = pathlib.Path(os.environ["TGCLI_TEST_TDLIB_SOURCE"])
PINNED_REVISION = "a17f87c4cff7b90b278d12b91ba0614383aaee82"
PINNED_FILES = (
    "td/telegram/DialogFilterManager.cpp",
    "td/telegram/DialogListId.cpp",
    "td/telegram/MessagesManager.cpp",
)


class StreamSourceContractTest(unittest.TestCase):
    def run_verifier(
        self,
        *arguments: str,
        repo_root: pathlib.Path = REPO_ROOT,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                os.fspath(VERIFIER),
                "--repo-root",
                os.fspath(repo_root),
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def make_checkout(self, destination: pathlib.Path) -> pathlib.Path:
        checkout = destination / "tdlib"
        subprocess.run(
            [
                "git",
                "clone",
                "--shared",
                "--no-checkout",
                os.fspath(TDLIB_SOURCE),
                os.fspath(checkout),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["git", "-C", os.fspath(checkout), "sparse-checkout", "set", *PINNED_FILES],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["git", "-C", os.fspath(checkout), "checkout", "--detach", PINNED_REVISION],
            check=True,
            capture_output=True,
            text=True,
        )
        return checkout

    def test_required_mode_rejects_missing_source(self) -> None:
        result = self.run_verifier()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--tdlib-source is required", result.stderr)

    def test_explicit_metadata_only_mode_does_not_claim_source(self) -> None:
        result = self.run_verifier("--metadata-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("metadata verified; source not inspected", result.stdout)

    def test_authenticated_source_accepts_the_real_pinned_checkout(self) -> None:
        result = self.run_verifier("--tdlib-source", os.fspath(TDLIB_SOURCE))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("authenticated source verified", result.stdout)

    def test_wrong_revision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            checkout = self.make_checkout(pathlib.Path(temporary))
            subprocess.run(
                [
                    "git",
                    "-C",
                    os.fspath(checkout),
                    "-c",
                    "user.name=tgcli test",
                    "-c",
                    "user.email=tgcli@example.invalid",
                    "commit",
                    "--allow-empty",
                    "-m",
                    "wrong revision",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = self.run_verifier("--tdlib-source", os.fspath(checkout))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("wrong revision", result.stderr)

    def test_dirty_and_hidden_corruption_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            checkout = self.make_checkout(pathlib.Path(temporary))
            source = checkout / PINNED_FILES[0]
            source.write_text(
                source.read_text(encoding="utf-8") + "\ncorrupt\n", encoding="utf-8"
            )
            dirty = self.run_verifier("--tdlib-source", os.fspath(checkout))
            self.assertNotEqual(dirty.returncode, 0)
            self.assertIn("tracked or untracked changes", dirty.stderr)
            subprocess.run(
                [
                    "git",
                    "-C",
                    os.fspath(checkout),
                    "update-index",
                    "--assume-unchanged",
                    PINNED_FILES[0],
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            corrupt = self.run_verifier("--tdlib-source", os.fspath(checkout))
            self.assertNotEqual(corrupt.returncode, 0)
            self.assertIn("digest differs", corrupt.stderr)

    def test_symlink_source_argument_is_rejected_before_canonicalization(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            alias = pathlib.Path(temporary) / "tdlib-link"
            alias.symlink_to(TDLIB_SOURCE, target_is_directory=True)
            result = self.run_verifier("--tdlib-source", os.fspath(alias))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source argument is a symlink", result.stderr)

    def test_symlink_parent_substitution_is_rejected_before_canonicalization(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            alias_parent = root / "substituted"
            alias_parent.symlink_to(TDLIB_SOURCE.parent, target_is_directory=True)
            result = self.run_verifier(
                "--tdlib-source", os.fspath(alias_parent / TDLIB_SOURCE.name)
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source argument is a symlink", result.stderr)

    def test_changed_structural_fragment_is_rejected_offline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            release = root / "release"
            release.mkdir()
            shutil.copy2(REPO_ROOT / "release/dependencies.lock.json", release)
            contract_source = REPO_ROOT / "release/tdlib-stream-source-contract.json"
            contract = json.loads(contract_source.read_text(encoding="utf-8"))
            contract["assertions"][0]["source_lines"][0] += " changed"
            (release / contract_source.name).write_text(
                json.dumps(contract, indent=2) + "\n", encoding="utf-8"
            )
            result = self.run_verifier("--metadata-only", repo_root=root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source assertion differs", result.stderr)


if __name__ == "__main__":
    unittest.main()
