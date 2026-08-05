from __future__ import annotations

import concurrent.futures
import contextlib
import io
import json
import os
import shlex
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

REPOSITORY = Path(__file__).resolve().parents[1]
SCRIPTS = REPOSITORY / "scripts"
BYTECODE_CACHE_ROOTS = (
    SCRIPTS / "__pycache__",
    SCRIPTS / "release/__pycache__",
    REPOSITORY / "tests/__pycache__",
)


def local_bytecode_cache_entries() -> frozenset[str]:
    return frozenset(
        str(entry.relative_to(REPOSITORY))
        for root in BYTECODE_CACHE_ROOTS
        if root.exists()
        for entry in (root, *root.rglob("*"))
    )


INITIAL_BYTECODE_CACHE_ENTRIES = local_bytecode_cache_entries()
sys.path.insert(0, str(SCRIPTS))

import materialize_test_dc_fixtures as materializer
import run_test_dc_e2e as acceptance
import test_dc_contract as contract


class BytecodeIsolationTests(unittest.TestCase):
    def test_suite_disables_source_tree_bytecode_writes(self) -> None:
        self.assertTrue(sys.dont_write_bytecode)
        self.assertEqual(local_bytecode_cache_entries(), INITIAL_BYTECODE_CACHE_ENTRIES)


class PrivateTree:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def close(self) -> None:
        self.temporary.cleanup()

    def directory(self, name: str) -> Path:
        directory = self.root / name
        directory.mkdir(mode=0o700)
        return directory

    def file(self, name: str, value: bytes, mode: int = 0o600) -> Path:
        source = self.root / name
        source.write_bytes(value)
        source.chmod(mode)
        return source


class SkipArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tree = PrivateTree()

    def tearDown(self) -> None:
        self.tree.close()

    def test_empty_artifact_has_exact_shape(self) -> None:
        artifact = self.tree.root / "results" / "tgcli-test-dc-skips.json"
        contract.write_skip_artifact(artifact, [])
        self.assertEqual(artifact.read_bytes(), b'{"skips":[]}\n')
        self.assertEqual(contract.load_skip_artifact(artifact), [])
        self.assertEqual(stat.S_IMODE(artifact.stat().st_mode), 0o600)

    def test_writer_sorts_closed_reasons(self) -> None:
        artifact = self.tree.root / "skips.json"
        contract.write_skip_artifact(
            artifact,
            [
                contract.SkipEntry(
                    contract.auth_state_test_id("wait_premium_purchase"),
                    "test_dc_state_not_forceable:wait_premium_purchase",
                ),
                contract.SkipEntry(contract.M1_QR_TEST, "fixture_missing:qr_approver"),
                contract.SkipEntry(
                    contract.M1_BOT_TEST, "fixture_missing:bot_token_cmd"
                ),
            ],
        )
        self.assertEqual(
            contract.load_skip_artifact(artifact),
            [
                contract.SkipEntry(
                    contract.M1_BOT_TEST, "fixture_missing:bot_token_cmd"
                ),
                contract.SkipEntry(contract.M1_QR_TEST, "fixture_missing:qr_approver"),
                contract.SkipEntry(
                    contract.auth_state_test_id("wait_premium_purchase"),
                    "test_dc_state_not_forceable:wait_premium_purchase",
                ),
            ],
        )

    def test_loader_rejects_silent_or_noncanonical_artifacts(self) -> None:
        cases = (
            {"skips": [], "extra": True},
            {
                "skips": [
                    {
                        "test": contract.M1_QR_TEST,
                        "reason": "fixture_missing:qr_approver",
                        "x": 1,
                    }
                ]
            },
            {"skips": [{"test": 1, "reason": "fixture_missing:qr_approver"}]},
            {
                "skips": [
                    {"test": contract.M1_QR_TEST, "reason": "fixture_missing:anything"}
                ]
            },
            {
                "skips": [
                    {
                        "test": contract.M1_QR_TEST,
                        "reason": "fixture_missing:qr_approver",
                    },
                    {
                        "test": contract.M1_BOT_TEST,
                        "reason": "fixture_missing:bot_token_cmd",
                    },
                ]
            },
        )
        for index, document in enumerate(cases):
            with self.subTest(index=index):
                artifact = self.tree.file(
                    f"bad-{index}.json", json.dumps(document).encode()
                )
                with self.assertRaises(contract.ContractError):
                    contract.load_skip_artifact(artifact)
        with self.assertRaises(contract.ContractError):
            contract.load_skip_artifact(self.tree.root / "missing.json")

    def test_duplicate_and_unknown_state_reasons_are_rejected(self) -> None:
        duplicate = contract.SkipEntry(
            contract.M1_QR_TEST, "fixture_missing:qr_approver"
        )
        with self.assertRaises(contract.ContractError):
            contract.canonical_skips([duplicate, duplicate])
        with self.assertRaises(contract.ContractError):
            contract.canonical_skips(
                [
                    contract.SkipEntry(
                        contract.auth_state_test_id("wait_password"),
                        "test_dc_state_not_forceable:future_state",
                    )
                ]
            )


class CoveragePartitionTests(unittest.TestCase):
    def test_expected_set_is_exactly_the_pinned_states_and_fixture_flows(self) -> None:
        self.assertEqual(
            contract.M1_EXPECTED_TESTS,
            {
                contract.M1_BOT_TEST,
                contract.M1_QR_TEST,
                *(
                    contract.auth_state_test_id(state)
                    for state in contract.PINNED_AUTH_STATES
                ),
            },
        )

    def test_executed_and_skipped_tests_form_a_complete_disjoint_partition(
        self,
    ) -> None:
        executed = {
            contract.M1_BOT_TEST,
            contract.auth_state_test_id("wait_tdlib_parameters"),
            contract.auth_state_test_id("wait_phone_number"),
            contract.auth_state_test_id("wait_code"),
            contract.auth_state_test_id("ready"),
            contract.auth_state_test_id("closed"),
        }
        skips = [
            contract.SkipEntry(contract.M1_QR_TEST, "fixture_missing:qr_approver"),
            *(
                contract.SkipEntry(
                    contract.auth_state_test_id(state),
                    f"test_dc_state_not_forceable:{state}",
                )
                for state in contract.PINNED_AUTH_STATES
                if contract.auth_state_test_id(state) not in executed
            ),
        ]
        contract.validate_m1_coverage(executed, skips)

    def test_missing_unknown_and_stale_coverage_are_rejected(self) -> None:
        complete = set(contract.M1_EXPECTED_TESTS)
        with self.assertRaisesRegex(
            contract.ContractError, "neither executed nor skipped"
        ):
            contract.validate_m1_coverage(complete - {contract.M1_QR_TEST}, [])
        with self.assertRaisesRegex(
            contract.ContractError, "outside the M1 closed set"
        ):
            contract.validate_m1_coverage(complete | {"m1.auth.future"}, [])
        with self.assertRaisesRegex(contract.ContractError, "stale skip"):
            contract.validate_m1_coverage(
                complete,
                [
                    contract.SkipEntry(
                        contract.M1_QR_TEST, "fixture_missing:qr_approver"
                    )
                ],
            )

    def test_skip_test_and_reason_are_exactly_bound(self) -> None:
        cases = [
            contract.SkipEntry("m1.auth.future", "fixture_missing:qr_approver"),
            contract.SkipEntry(contract.M1_BOT_TEST, "fixture_missing:qr_approver"),
            contract.SkipEntry(
                contract.auth_state_test_id("wait_code"),
                "test_dc_state_not_forceable:wait_password",
            ),
        ]
        for entry in cases:
            with self.subTest(entry=entry), self.assertRaises(contract.ContractError):
                contract.canonical_skips([entry])


class SentinelTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tree = PrivateTree()
        self.sentinel = self.tree.file("code", b"test-code-sentinel")
        self.stderr = self.tree.file("client.stderr", b"safe diagnostics\n")
        self.logs = self.tree.directory("logs")
        self.tree.file("logs/tdlib.log", b"safe active log\n")
        self.tree.file("logs/tdlib.log.1", b"safe rotated log\n")

    def tearDown(self) -> None:
        self.tree.close()

    def test_scans_active_and_every_rotated_log(self) -> None:
        contract.scan_auth_sentinels([self.sentinel], [self.stderr], [self.logs])
        for source in (self.stderr, self.logs / "tdlib.log", self.logs / "tdlib.log.1"):
            with self.subTest(source=source.name):
                source.write_bytes(b"prefix-test-code-sentinel-suffix")
                with self.assertRaises(contract.ContractError):
                    contract.scan_auth_sentinels(
                        [self.sentinel], [self.stderr], [self.logs]
                    )
                source.write_bytes(b"safe again")

    def test_detects_a_sentinel_across_the_scan_chunk_boundary(self) -> None:
        self.stderr.write_bytes(b"x" * (64 * 1024 - 5) + b"test-code-sentinel")
        with self.assertRaises(contract.ContractError):
            contract.scan_auth_sentinels([self.sentinel], [self.stderr], [self.logs])

    def test_requires_active_log_and_private_distinct_sentinels(self) -> None:
        missing_active = self.tree.directory("missing-active")
        self.tree.file("missing-active/tdlib.log.1", b"safe rotated log")
        with self.assertRaises(contract.ContractError):
            contract.scan_auth_sentinels(
                [self.sentinel], [self.stderr], [missing_active]
            )
        duplicate = self.tree.file("duplicate", b"test-code-sentinel")
        with self.assertRaises(contract.ContractError):
            contract.scan_auth_sentinels(
                [self.sentinel, duplicate], [self.stderr], [self.logs]
            )
        self.sentinel.chmod(0o644)
        with self.assertRaises(contract.ContractError):
            contract.scan_auth_sentinels([self.sentinel], [self.stderr], [self.logs])

    def test_rejects_symlink_scan_sources(self) -> None:
        target = self.tree.file("target", b"safe")
        link = self.logs / "tdlib.log.2"
        link.symlink_to(target)
        with self.assertRaises(contract.ContractError):
            contract.scan_auth_sentinels([self.sentinel], [self.stderr], [self.logs])

    def test_frozen_sentinel_rejects_post_capture_fixture_replacement(self) -> None:
        frozen = contract.freeze_auth_sentinels([self.sentinel])
        self.stderr.write_bytes(b"leaked test-code-sentinel")
        replacement = self.tree.file("replacement-code", b"changed-code-sentinel")
        os.replace(replacement, self.sentinel)
        with self.assertRaises(contract.ContractError):
            contract.scan_frozen_auth_sentinels(frozen, [self.stderr], [self.logs])


class MaterializerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tree = PrivateTree()

    def tearDown(self) -> None:
        self.tree.close()

    def test_requires_phone_code_and_database_key(self) -> None:
        for missing in (
            "TGCLI_TEST_DC_PHONE_NUMBER",
            "TGCLI_TEST_DC_AUTHENTICATION_CODE",
            "TGCLI_TEST_DC_DATABASE_KEY",
        ):
            with self.subTest(missing=missing):
                environment = {
                    "TGCLI_TEST_DC_PHONE_NUMBER": "9996620001",
                    "TGCLI_TEST_DC_AUTHENTICATION_CODE": "22222",
                    "TGCLI_TEST_DC_DATABASE_KEY": "db-key-sentinel",
                }
                del environment[missing]
                with self.assertRaises(materializer.FixtureError):
                    materializer.materialize(self.tree.directory(missing), environment)

    def test_materializes_private_files_without_printing_values(self) -> None:
        output = self.tree.directory("fixtures")
        environment = {
            "TGCLI_TEST_DC_PHONE_NUMBER": "9996620001",
            "TGCLI_TEST_DC_AUTHENTICATION_CODE": "22222",
            "TGCLI_TEST_DC_DATABASE_KEY": "db-key-sentinel",
            "TGCLI_TEST_DC_BOT_TOKEN": "bot-token-sentinel",
        }
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            written = materializer.materialize(output, environment)
        self.assertEqual(stream.getvalue(), "")
        self.assertEqual(
            written,
            ["phone_number", "authentication_code", "database_key", "bot_token"],
        )
        for filename in written:
            source = output / filename
            self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o600)
        self.assertNotIn("bot-token-sentinel", stream.getvalue())

    def test_rejects_multiline_values_and_nonprivate_directory(self) -> None:
        with self.assertRaises(materializer.FixtureError):
            materializer._validate_value("code", "one\ntwo")
        directory = self.tree.directory("public")
        directory.chmod(0o755)
        with self.assertRaises(materializer.FixtureError):
            materializer.materialize(
                directory,
                {
                    "TGCLI_TEST_DC_PHONE_NUMBER": "9996620001",
                    "TGCLI_TEST_DC_AUTHENTICATION_CODE": "22222",
                    "TGCLI_TEST_DC_DATABASE_KEY": "db-key-sentinel",
                },
            )


class HarnessPreflightTests(unittest.TestCase):
    VALID_HELP = """OPTIONS:
  -v, --verbose diagnostics
      --allow-write grant writes
      --yes confirm
SUBCOMMANDS:
  logout log out
"""

    def setUp(self) -> None:
        self.tree = PrivateTree()
        self.fixtures = self.tree.directory("fixtures")
        for name, value in (
            ("phone_number", b"9996620001"),
            ("authentication_code", b"code-sentinel"),
            ("database_key", b"database-key-sentinel"),
        ):
            self.tree.file(f"fixtures/{name}", value)
        self.build = self.tree.directory("build")

    def tearDown(self) -> None:
        self.tree.close()

    def _binary(self, name: str, help_text: str) -> Path:
        binary = self.tree.file(
            name,
            ("#!/bin/sh\nprintf '%s\\n' " + shlex.quote(help_text) + "\n").encode(),
            0o700,
        )
        return binary

    def _run(
        self,
        binary: Path,
        extra: list[str] | None = None,
        *,
        preflight_only: bool = True,
    ) -> int:
        arguments = [
            "--binary",
            str(binary),
            "--build-dir",
            str(self.build),
            "--fixture-dir",
            str(self.fixtures),
        ]
        if preflight_only:
            arguments.append("--preflight-only")
        arguments.extend(extra or [])
        with (
            mock.patch.dict(
                os.environ,
                {"TGCLI_API_ID": "1", "TGCLI_API_HASH": "non-secret-app-hash"},
                clear=False,
            ),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            return acceptance.main(arguments)

    def test_preflight_writes_sorted_fixture_and_state_skips(self) -> None:
        binary = self._binary("tgcli", self.VALID_HELP)
        result = self._run(binary, ["--unforceable-state", "wait_password"])
        self.assertEqual(result, 0)
        artifact = self.build / "test-results" / "tgcli-test-dc-skips.json"
        self.assertEqual(
            contract.load_skip_artifact(artifact),
            [
                contract.SkipEntry("m1.auth.bot", "fixture_missing:bot_token_cmd"),
                contract.SkipEntry("m1.auth.qr", "fixture_missing:qr_approver"),
                contract.SkipEntry(
                    "m1.auth.state.wait_password",
                    "test_dc_state_not_forceable:wait_password",
                ),
            ],
        )

    def test_missing_runtime_surface_fails_but_still_writes_artifact(self) -> None:
        binary = self._binary("incomplete", "login me")
        self.assertEqual(self._run(binary), 1)
        artifact = self.build / "test-results" / "tgcli-test-dc-skips.json"
        self.assertEqual(len(contract.load_skip_artifact(artifact)), 2)

    def test_unknown_state_fails_without_inventing_a_reason(self) -> None:
        binary = self._binary("tgcli", self.VALID_HELP)
        self.assertEqual(self._run(binary, ["--unforceable-state", "future"]), 1)
        artifact = self.build / "test-results" / "tgcli-test-dc-skips.json"
        self.assertEqual(contract.load_skip_artifact(artifact), [])

    def test_pty_responder_drives_tty_only_auth_challenges(self) -> None:
        binary = self.tree.file(
            "interactive-tgcli",
            b"""#!/usr/bin/env python3
import json
import os
import sys

if not os.isatty(0):
    raise SystemExit(9)
sys.stderr.write("Phone number: ")
sys.stderr.flush()
phone = sys.stdin.readline().strip()
sys.stderr.write("Authentication code: ")
sys.stderr.flush()
code = sys.stdin.readline().strip()
if phone != "9996620001" or code != "code-sentinel":
    raise SystemExit(8)
print(json.dumps({"account": "test-user", "auth_state": "ready", "user": {"id": 1}}))
""",
            0o700,
        )
        captures = acceptance.Captures(self.tree.directory("captures"))
        runner = acceptance.Runner(
            binary,
            {"PATH": os.environ.get("PATH", "/usr/bin:/bin")},
            {},
            captures,
            None,
            5,
        )
        result = runner.run_interactive(
            "login",
            [],
            {
                "phone_number": b"9996620001",
                "authentication_code": b"code-sentinel",
            },
        )
        self.assertEqual(
            result.observed_prompts,
            frozenset({"phone_number", "authentication_code"}),
        )
        self.assertEqual(result.document["auth_state"], "ready")

    def test_preflight_requires_exact_option_tokens_and_subcommand_rows(self) -> None:
        substring_only = """OPTIONS:
  --verbose diagnostics mention -v and --allow-write --yes
SUBCOMMANDS:
  logout-ish not logout
"""
        binary = self._binary("substring-only", substring_only)
        self.assertEqual(self._run(binary), 1)

    def test_full_run_accepts_a_complete_partition(self) -> None:
        binary = self._binary("tgcli-complete", self.VALID_HELP)
        executed = set(contract.M1_EXPECTED_TESTS) - {
            contract.M1_BOT_TEST,
            contract.M1_QR_TEST,
        }
        with (
            mock.patch.object(acceptance, "_smoke", return_value=executed),
            mock.patch.object(acceptance, "scan_frozen_auth_sentinels"),
        ):
            self.assertEqual(self._run(binary, preflight_only=False), 0)

    def test_full_run_rejects_an_incomplete_partition_and_writes_skips(self) -> None:
        binary = self._binary("tgcli-incomplete-coverage", self.VALID_HELP)
        with (
            mock.patch.object(acceptance, "_smoke", return_value=set()),
            mock.patch.object(acceptance, "scan_frozen_auth_sentinels"),
        ):
            self.assertEqual(self._run(binary, preflight_only=False), 1)
        artifact = self.build / "test-results" / "tgcli-test-dc-skips.json"
        self.assertEqual(
            contract.load_skip_artifact(artifact),
            [
                contract.SkipEntry(
                    contract.M1_BOT_TEST, "fixture_missing:bot_token_cmd"
                ),
                contract.SkipEntry(contract.M1_QR_TEST, "fixture_missing:qr_approver"),
            ],
        )


class HarnessInvariantTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tree = PrivateTree()

    def tearDown(self) -> None:
        self.tree.close()

    @staticmethod
    def user(*, bot: bool = False) -> dict[str, object]:
        return {
            "id": 1,
            "first_name": "Test",
            "last_name": "User",
            "usernames": [],
            "phone_number": "" if bot else "9996620001",
            "is_bot": bot,
            "is_premium": False,
        }

    def test_terminal_only_login_and_relogin_are_rejected(self) -> None:
        terminal_only = acceptance.CommandResult(
            {
                "account": acceptance.USER_ACCOUNT,
                "auth_state": "ready",
                "user": self.user(),
            },
            frozenset(),
        )
        for label in ("initial login", "re-login"):
            with (
                self.subTest(label=label),
                self.assertRaises(acceptance.AcceptanceError),
            ):
                acceptance._require_phone_code_flow(terminal_only, label)

    def test_bot_and_qr_terminal_only_results_are_rejected(self) -> None:
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._assert_bot_identity(self.user())
        acceptance._assert_bot_identity(self.user(bot=True))
        runner = mock.Mock(qr_approvals=0)
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._require_qr_approval(runner)
        runner.qr_approvals = 1
        acceptance._require_qr_approval(runner)

    def test_saved_tags_milestone_flow_requires_the_exact_result_shape(self) -> None:
        acceptance._assert_saved_tags(
            {
                "items": [
                    {"tag": "🧪", "label": "experiments", "count": 7},
                    {"tag": "custom:123456789", "label": "", "count": 2},
                ],
                "next": None,
            }
        )
        invalid = (
            {"items": [], "next": "cursor"},
            {"items": [{"tag": "", "label": "", "count": 0}], "next": None},
            {"items": [{"tag": "🧪", "label": "", "count": True}], "next": None},
            {"items": [{"tag": "🧪", "label": "", "count": -1}], "next": None},
        )
        for document in invalid:
            with self.subTest(document=document), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance._assert_saved_tags(document)

    def test_observed_prompts_map_only_to_their_pinned_auth_states(self) -> None:
        result = acceptance.CommandResult(
            {},
            frozenset(
                {
                    "phone_number",
                    "authentication_code",
                    "email_address",
                    "email_code",
                    "registration_first_name",
                    "password",
                    "database_key",
                }
            ),
        )
        self.assertEqual(
            acceptance._observed_auth_state_tests(result),
            {
                contract.auth_state_test_id("wait_phone_number"),
                contract.auth_state_test_id("wait_code"),
                contract.auth_state_test_id("wait_email_address"),
                contract.auth_state_test_id("wait_email_code"),
                contract.auth_state_test_id("wait_registration"),
                contract.auth_state_test_id("wait_password"),
            },
        )

    def test_noop_account_add_is_rejected_before_config_augmentation(self) -> None:
        runner = mock.Mock()
        runner.run_json.return_value = acceptance.CommandResult(
            {"items": [], "next": None}, frozenset()
        )
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._assert_account_persistence(
                runner,
                {
                    "XDG_DATA_HOME": str(self.tree.root / "data"),
                    "XDG_STATE_HOME": str(self.tree.root / "state"),
                    "XDG_RUNTIME_DIR": str(self.tree.root / "runtime"),
                },
                [acceptance.USER_ACCOUNT],
            )

    def test_selector_registration_failure_reaps_the_interactive_child(self) -> None:
        pid_file = self.tree.root / "child.pid"
        binary = self.tree.file(
            "blocking-child",
            b"""#!/usr/bin/env python3
import os
import pathlib
import time

pathlib.Path(os.environ["PID_FILE"]).write_text(str(os.getpid()))
time.sleep(5)
""",
            0o700,
        )
        runner = acceptance.Runner(
            binary,
            {
                "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                "PID_FILE": str(pid_file),
            },
            {},
            acceptance.Captures(self.tree.directory("selector-captures")),
            None,
            2,
        )

        class BrokenSelector:
            def register(self, fileobj: object, events: int) -> None:
                del fileobj, events
                deadline = time.monotonic() + 1
                while not pid_file.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                raise OSError("injected selector registration failure")

            def close(self) -> None:
                pass

        with (
            mock.patch.object(acceptance.selectors, "DefaultSelector", BrokenSelector),
            self.assertRaises(OSError),
        ):
            runner.run_interactive("selector-failure", [], {})
        child = int(pid_file.read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(child, 0)

    def test_cleanup_stops_a_replacement_after_the_tracked_pid_exited(self) -> None:
        class ExitedProcess:
            @staticmethod
            def poll() -> int:
                return 0

        class CleanupRunner:
            def __init__(self) -> None:
                self.daemons = [
                    acceptance.Daemon(
                        acceptance.USER_ACCOUNT,
                        ExitedProcess(),
                        io.BytesIO(),
                        io.BytesIO(),
                    )
                ]
                self.running = True
                self.calls: list[str] = []

            def run_json(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                del arguments
                self.calls.append(label)
                if label.startswith("stop-"):
                    self.running = False
                    return acceptance.CommandResult({"stopping": True}, frozenset())
                return acceptance.CommandResult({"running": self.running}, frozenset())

        runner = CleanupRunner()
        acceptance.Runner.stop_daemons(runner)
        self.assertIn("stop-test-user", runner.calls)
        self.assertIn("status-after-stop-test-user", runner.calls)
        self.assertFalse(runner.running)

    def test_concurrent_launch_proves_live_process_overlap(self) -> None:
        binary = self.tree.file(
            "overlap-child",
            b"""#!/usr/bin/env python3
import json
import time

time.sleep(0.2)
print(json.dumps({"done": True}))
""",
            0o700,
        )
        runner = acceptance.Runner(
            binary,
            {"PATH": os.environ.get("PATH", "/usr/bin:/bin")},
            {},
            acceptance.Captures(self.tree.directory("overlap-captures")),
            None,
            2,
        )
        launch = acceptance.ConcurrentLaunch(2, 1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures = [
                executor.submit(
                    runner.run_interactive,
                    f"login-{index}",
                    [],
                    {},
                    None,
                    launch,
                )
                for index in range(2)
            ]
            self.assertEqual(
                [future.result().document for future in futures], [{"done": True}] * 2
            )
        self.assertTrue(launch.overlap_proven)

    def test_concurrent_launch_rejects_terminal_only_processes(self) -> None:
        class ExitedProcess:
            @staticmethod
            def poll() -> int:
                return 0

        launch = acceptance.ConcurrentLaunch(2, 1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures = [
                executor.submit(launch.rendezvous, f"login-{index}", ExitedProcess())
                for index in range(2)
            ]
            for future in futures:
                with self.assertRaises(acceptance.AcceptanceError):
                    future.result()
        self.assertFalse(launch.overlap_proven)


class WorkflowContractTests(unittest.TestCase):
    def test_nightly_job_has_fail_closed_artifact_and_secret_boundaries(self) -> None:
        workflow = (REPOSITORY / ".github/workflows/test-dc.yml").read_text()
        self.assertIn("schedule:", workflow)
        self.assertIn("workflow_dispatch:", workflow)
        self.assertIn("scripts/run_test_dc_e2e.py", workflow)
        self.assertIn("scripts/materialize_test_dc_fixtures.py", workflow)
        self.assertGreaterEqual(workflow.count("if: always()"), 2)
        artifact = "build/debug/test-results/tgcli-test-dc-skips.json"
        self.assertGreaterEqual(workflow.count(artifact), 3)
        self.assertIn("if-no-files-found: error", workflow)
        upload = workflow.split("uses: actions/upload-artifact@v4", maxsplit=1)[1]
        self.assertNotIn("tdlib.log", upload)
        self.assertNotIn(".stderr", upload)
        self.assertNotIn("tgcli-test-dc-fixtures", upload)
        smoke = workflow.split("name: Run M1 authentication smoke", maxsplit=1)[1]
        self.assertNotIn("TGCLI_TEST_DC_PASSWORD:", smoke)
        self.assertNotIn("TGCLI_TEST_DC_BOT_TOKEN:", smoke)
        for state in (
            "wait_premium_purchase",
            "wait_email_address",
            "wait_email_code",
            "wait_registration",
            "wait_password",
            "logging_out",
            "closing",
        ):
            self.assertIn(f"--unforceable-state {state}", smoke)
        qr_else = smoke.split('if [[ -n "$TGCLI_TEST_DC_QR_APPROVER" ]]', maxsplit=1)[1]
        self.assertIn("else", qr_else)
        self.assertIn("--unforceable-state wait_other_device_confirmation", qr_else)


if __name__ == "__main__":
    unittest.main()
