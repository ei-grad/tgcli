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

    def test_m5_result_artifact_is_exact_private_and_round_trips(self) -> None:
        artifact = self.tree.root / "results" / "tgcli-test-dc-m5.json"
        record = contract.M5ResultRecord(
            account="test-user",
            chat_id=42,
            anchor_message_id=101,
            target_message_id=102,
            target_prefix="tgcli-m5-target-0123456789abcdef0123456789abcdef",
        )
        contract.write_m5_result_artifact(artifact, record)
        self.assertEqual(
            artifact.read_bytes(),
            b'{"schema_version":1,"account":"test-user","chat_id":42,'
            b'"anchor_message_id":101,"target_message_id":102,'
            b'"target_prefix":"tgcli-m5-target-0123456789abcdef0123456789abcdef"}\n',
        )
        self.assertEqual(contract.load_m5_result_artifact(artifact), record)
        self.assertEqual(stat.S_IMODE(artifact.stat().st_mode), 0o600)

    def test_m5_result_artifact_rejects_malformed_and_symlink_inputs(self) -> None:
        valid = contract.M5ResultRecord(
            account="test-user",
            chat_id=42,
            anchor_message_id=101,
            target_message_id=102,
            target_prefix="tgcli-m5-target-0123456789abcdef0123456789abcdef",
        )
        for field, value in (
            ("account", "Bad Account"),
            ("chat_id", 0),
            ("anchor_message_id", 0),
            ("target_message_id", 101),
            ("target_prefix", "tgcli-m5-target-not-hex"),
        ):
            with self.subTest(field=field):
                values = dict(valid.__dict__)
                values[field] = value
                with self.assertRaises(contract.ContractError):
                    contract.validate_m5_result(contract.M5ResultRecord(**values))
        target = self.tree.file("m5-target.json", b"{}\n")
        link = self.tree.root / "m5-link.json"
        link.symlink_to(target)
        with self.assertRaises(contract.ContractError):
            contract.load_m5_result_artifact(link)


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
  --idempotency-key TEXT key
SUBCOMMANDS:
  logout log out
  chats list chats
  send send text
  msg message commands
  delete delete messages
  saved Saved Messages operations
  resolve resolve a selector
  listen stream updates
  wait-for wait for a message
  contact contact operations
  folder folder operations
  topic topic operations
  chat chat administration
  storage storage operations
  session session operations
  attach attach a file
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

    def test_clean_json_runner_rejects_any_stderr_for_stream_acceptance(self) -> None:
        noisy = self.tree.file(
            "noisy-tgcli",
            b"#!/bin/sh\nprintf '%s\\n' '{\"id\":1}'\nprintf '%s\\n' warning >&2\n",
            0o700,
        )
        runner = acceptance.Runner(
            noisy,
            {"PATH": os.environ.get("PATH", "/usr/bin:/bin")},
            {},
            acceptance.Captures(self.tree.directory("clean-json-captures")),
            None,
            5,
        )
        with self.assertRaisesRegex(acceptance.AcceptanceError, "unexpected stderr"):
            runner.run_json_clean("m5-wait-for", [])

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
            with (
                self.subTest(document=document),
                self.assertRaises(acceptance.AcceptanceError),
            ):
                acceptance._assert_saved_tags(document)

    def test_chats_milestone_flow_requires_a_strict_zero_or_one_item_result(
        self,
    ) -> None:
        acceptance._assert_chats({"items": [], "next": None})
        item = {
            "id": -1001,
            "title": "Project",
            "type": "supergroup",
            "is_bot": False,
            "usernames": ["project"],
            "is_archived": False,
            "folder_ids": [2],
            "is_marked_unread": False,
            "unread_count": 3,
            "unread_mention_count": 1,
            "unread_reaction_count": 0,
            "unread_poll_vote_count": 0,
            "last_message": {
                "id": 123,
                "chat_id": -1001,
                "date": "2026-08-05T10:00:00Z",
                "sender": {"type": "user", "id": 42},
                "is_outgoing": False,
                "topic": {"kind": "forum", "id": 7},
                "type": "text",
                "text": "experiment result",
            },
        }
        acceptance._assert_chats({"items": [item], "next": "cursor"})
        acceptance._assert_chats(
            {
                "items": [
                    {
                        **item,
                        "id": 42,
                        "type": "private",
                        "is_bot": True,
                        "last_message": {**item["last_message"], "chat_id": 42},
                    }
                ],
                "next": None,
            }
        )
        invalid = (
            {"items": [], "next": "cursor"},
            {"items": [item, item], "next": None},
            {"items": [{**item, "is_bot": True}], "next": None},
            {"items": [{**item, "folder_ids": [3, 2]}], "next": None},
            {"items": [{**item, "unread_count": True}], "next": None},
            {
                "items": [
                    {
                        **item,
                        "last_message": {**item["last_message"], "chat_id": -1002},
                    }
                ],
                "next": None,
            },
            {
                "items": [
                    {
                        **item,
                        "last_message": {
                            **item["last_message"],
                            "topic": {"kind": "forum", "id": 2_147_483_648},
                        },
                    }
                ],
                "next": None,
            },
        )
        for document in invalid:
            with (
                self.subTest(document=document),
                self.assertRaises(acceptance.AcceptanceError),
            ):
                acceptance._assert_chats(document)

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

    def test_m3_flow_registers_cleanup_before_reads_and_runs_it_on_success(
        self,
    ) -> None:
        sent = {
            "id": 101,
            "chat_id": 42,
            "date": "2026-08-21T12:00:00Z",
            "sender": {"type": "user", "id": 42},
            "is_outgoing": True,
            "topic": None,
            "type": "text",
            "text": "",
            "scheduled": False,
        }
        attached = {
            "id": 102,
            "chat_id": 42,
            "date": "2026-08-21T12:00:01Z",
            "sender": {"type": "user", "id": 42},
            "is_outgoing": True,
            "topic": None,
            "type": "doc",
            "text": "tgcli M4 attachment",
            "scheduled": False,
        }

        class FlowRunner:
            def __init__(self, captures: acceptance.Captures) -> None:
                self.captures = captures
                self.calls: list[str] = []

            def run_json(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                self.calls.append(label)
                if label in {"m3-send", "m3-send-replay"}:
                    document = dict(sent)
                    document["text"] = arguments[-1]
                    if label == "m3-send-replay":
                        document["text"] = sent["text"]
                    else:
                        sent["text"] = arguments[-1]
                        document["text"] = arguments[-1]
                    return acceptance.CommandResult(document, frozenset())
                if label == "m3-get":
                    return acceptance.CommandResult(
                        {
                            "items": [
                                {
                                    name: value
                                    for name, value in sent.items()
                                    if name != "scheduled"
                                }
                            ],
                            "next": None,
                        },
                        frozenset(),
                    )
                if label in {"m4-saved-attach", "m4-saved-attach-replay"}:
                    return acceptance.CommandResult(dict(attached), frozenset())
                if label == "m4-saved-attach-get":
                    return acceptance.CommandResult(
                        {
                            "items": [
                                {
                                    name: value
                                    for name, value in attached.items()
                                    if name != "scheduled"
                                }
                            ],
                            "next": None,
                        },
                        frozenset(),
                    )
                raise AssertionError(label)

            def run_json_error(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                del arguments
                self.calls.append(label)
                return acceptance.CommandResult(
                    {
                        "error": {
                            "code": "IDEMPOTENCY_CONFLICT",
                            "message": "conflict",
                            "details": {},
                        }
                    },
                    frozenset(),
                )

            def register_message_cleanup(
                self, chat_id: int, message_id: int
            ) -> acceptance.MessageCleanup:
                self.calls.append("register-cleanup")
                return acceptance.MessageCleanup(
                    acceptance.USER_ACCOUNT, chat_id, message_id
                )

            def cleanup_message(self, cleanup: acceptance.MessageCleanup) -> None:
                self.calls.append("cleanup")
                cleanup.completed = True

        runner = FlowRunner(acceptance.Captures(self.tree.directory("m3-captures")))
        acceptance._m3_write_flow(runner)
        self.assertEqual(
            runner.calls,
            [
                "m3-send",
                "register-cleanup",
                "m3-get",
                "m3-send-replay",
                "m3-send-conflict",
                "m4-saved-attach",
                "register-cleanup",
                "m4-saved-attach-get",
                "m4-saved-attach-replay",
                "m4-saved-attach-conflict",
                "cleanup",
                "cleanup",
            ],
        )

    def test_m3_cleanup_failure_wins_over_an_earlier_flow_failure(self) -> None:
        class FailedFlowRunner:
            def __init__(self, captures: acceptance.Captures) -> None:
                self.captures = captures
                self.cleaned = False

            def run_json(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                if label == "m3-send":
                    return acceptance.CommandResult(
                        {
                            "id": 101,
                            "chat_id": 42,
                            "date": "2026-08-21T12:00:00Z",
                            "sender": {"type": "user", "id": 42},
                            "is_outgoing": True,
                            "topic": None,
                            "type": "text",
                            "text": arguments[-1],
                            "scheduled": False,
                        },
                        frozenset(),
                    )
                raise acceptance.AcceptanceError("earlier assertion")

            def register_message_cleanup(
                self, chat_id: int, message_id: int
            ) -> acceptance.MessageCleanup:
                return acceptance.MessageCleanup(
                    acceptance.USER_ACCOUNT, chat_id, message_id
                )

            def cleanup_message(self, cleanup: acceptance.MessageCleanup) -> None:
                del cleanup
                self.cleaned = True
                raise acceptance.AcceptanceError("delete failed")

        runner = FailedFlowRunner(
            acceptance.Captures(self.tree.directory("failed-m3-captures"))
        )
        with self.assertRaisesRegex(acceptance.AcceptanceError, "M3/M4 cleanup failed"):
            acceptance._m3_write_flow(runner)
        self.assertTrue(runner.cleaned)

    def test_m5_flow_constructs_exact_commands_records_result_and_never_cleans_up(
        self,
    ) -> None:
        token = "0123456789abcdef0123456789abcdef"
        anchor_text = f"tgcli-m5-anchor-{token}"
        target_text = f"tgcli-m5-target-{token}"
        anchor = {
            "id": 101,
            "chat_id": 42,
            "date": "2026-08-26T12:00:00Z",
            "sender": {"type": "user", "id": 42},
            "is_outgoing": True,
            "topic": None,
            "type": "text",
            "text": anchor_text,
            "scheduled": False,
        }
        target = {
            "id": 102,
            "chat_id": 42,
            "date": "2026-08-26T12:00:01Z",
            "sender": {"type": "user", "id": 42},
            "is_outgoing": True,
            "topic": None,
            "type": "text",
            "text": target_text,
            "scheduled": False,
        }

        class FlowRunner:
            def __init__(self) -> None:
                self.calls: list[tuple[str, list[str]]] = []

            def run_json(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                self.calls.append((label, arguments))
                if label == "m5-resolve-saved":
                    return acceptance.CommandResult(
                        {
                            "kind": "chat",
                            "chat": {
                                "id": 42,
                                "title": "Saved Messages",
                                "type": "private",
                                "is_bot": False,
                                "usernames": [],
                            },
                            "message_id": None,
                            "topic": None,
                            "link_type": "saved_messages",
                            "is_public": None,
                        },
                        frozenset(),
                    )
                if label == "m5-send-anchor":
                    return acceptance.CommandResult(anchor, frozenset())
                if label == "m5-send-target":
                    return acceptance.CommandResult(target, frozenset())
                raise AssertionError(label)

            def run_json_clean(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                self.calls.append((label, arguments))
                if label != "m5-wait-for":
                    raise AssertionError(label)
                return acceptance.CommandResult(
                    {
                        name: value
                        for name, value in target.items()
                        if name != "scheduled"
                    },
                    frozenset(),
                )

            def register_message_cleanup(self, chat_id: int, message_id: int) -> None:
                raise AssertionError(f"unexpected cleanup: {chat_id}/{message_id}")

        runner = FlowRunner()
        artifact = self.tree.root / "results" / "tgcli-test-dc-m5.json"
        with mock.patch.object(acceptance.secrets, "token_hex", return_value=token):
            acceptance._m5_stream_flow(runner, {"id": 42}, artifact)

        self.assertEqual(
            runner.calls,
            [
                (
                    "m5-resolve-saved",
                    [
                        "--json",
                        "--account",
                        acceptance.USER_ACCOUNT,
                        "resolve",
                        "t.me/saved",
                    ],
                ),
                (
                    "m5-send-anchor",
                    [
                        "--json",
                        "--allow-write",
                        "--account",
                        acceptance.USER_ACCOUNT,
                        "send",
                        "42",
                        anchor_text,
                    ],
                ),
                (
                    "m5-send-target",
                    [
                        "--json",
                        "--allow-write",
                        "--account",
                        acceptance.USER_ACCOUNT,
                        "send",
                        "42",
                        target_text,
                    ],
                ),
                (
                    "m5-wait-for",
                    [
                        "--json",
                        "--timeout",
                        "30",
                        "--account",
                        acceptance.USER_ACCOUNT,
                        "wait-for",
                        "--chat",
                        "42",
                        "--from",
                        "42",
                        "--after",
                        "101",
                        "--regex",
                        f"^{target_text}$",
                    ],
                ),
            ],
        )
        self.assertEqual(
            contract.load_m5_result_artifact(artifact),
            contract.M5ResultRecord(
                account=acceptance.USER_ACCOUNT,
                chat_id=42,
                anchor_message_id=101,
                target_message_id=102,
                target_prefix=target_text,
            ),
        )

    def test_m5_flow_rejects_wrong_wait_output_without_record_or_cleanup(self) -> None:
        class FailedRunner:
            def run_json(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                del arguments
                if label == "m5-resolve-saved":
                    return acceptance.CommandResult(
                        {
                            "kind": "chat",
                            "chat": {
                                "id": 42,
                                "title": "Saved Messages",
                                "type": "private",
                                "is_bot": False,
                                "usernames": [],
                            },
                            "message_id": None,
                            "topic": None,
                            "link_type": "saved_messages",
                            "is_public": None,
                        },
                        frozenset(),
                    )
                message_id = 101 if label == "m5-send-anchor" else 102
                text = (
                    "tgcli-m5-anchor-0123456789abcdef0123456789abcdef"
                    if label == "m5-send-anchor"
                    else "tgcli-m5-target-0123456789abcdef0123456789abcdef"
                )
                return acceptance.CommandResult(
                    {
                        "id": message_id,
                        "chat_id": 42,
                        "date": "2026-08-26T12:00:00Z",
                        "sender": {"type": "user", "id": 42},
                        "is_outgoing": True,
                        "topic": None,
                        "type": "text",
                        "text": text,
                        "scheduled": False,
                    },
                    frozenset(),
                )

            def run_json_clean(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                del label, arguments
                return acceptance.CommandResult({"id": 999}, frozenset())

            def register_message_cleanup(self, chat_id: int, message_id: int) -> None:
                raise AssertionError(f"unexpected cleanup: {chat_id}/{message_id}")

        artifact = self.tree.root / "results" / "tgcli-test-dc-m5.json"
        with (
            mock.patch.object(
                acceptance.secrets,
                "token_hex",
                return_value="0123456789abcdef0123456789abcdef",
            ),
            self.assertRaisesRegex(acceptance.AcceptanceError, "wrong target"),
        ):
            acceptance._m5_stream_flow(FailedRunner(), {"id": 42}, artifact)
        self.assertFalse(artifact.exists())

    def test_m6_storage_flow_constructs_exact_commands_and_registers_no_cleanup(
        self,
    ) -> None:
        result = {
            "size": 7,
            "count": 3,
            "by_chat": [
                {
                    "chat_id": -1001,
                    "size": 7,
                    "count": 3,
                    "by_file_type": [
                        {"file_type": "photo", "size": 5, "count": 1},
                        {"file_type": "video", "size": 2, "count": 2},
                    ],
                }
            ],
        }

        class FlowRunner:
            def __init__(self) -> None:
                self.calls: list[tuple[str, list[str]]] = []

            def run_json_clean(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                self.calls.append((label, arguments))
                return acceptance.CommandResult(result, frozenset())

            def register_message_cleanup(self, chat_id: int, message_id: int) -> None:
                raise AssertionError(f"unexpected cleanup: {chat_id}/{message_id}")

        runner = FlowRunner()
        acceptance._m6_storage_stats_flow(runner)
        self.assertEqual(
            runner.calls,
            [
                (
                    "m6-storage-stats-json",
                    [
                        "--json",
                        "--account",
                        acceptance.USER_ACCOUNT,
                        "storage",
                        "stats",
                    ],
                ),
                (
                    "m6-storage-stats-human",
                    ["--account", acceptance.USER_ACCOUNT, "storage", "stats"],
                ),
            ],
        )

    def test_m6_storage_evidence_rejects_unknown_types_and_inconsistent_sums(
        self,
    ) -> None:
        valid = {
            "size": 1,
            "count": 1,
            "by_chat": [
                {
                    "chat_id": -1001,
                    "size": 1,
                    "count": 1,
                    "by_file_type": [{"file_type": "photo", "size": 1, "count": 1}],
                }
            ],
        }
        acceptance._assert_m6_storage_stats(valid)
        for mutate in (
            lambda value: value["by_chat"][0]["by_file_type"][0].update(
                {"file_type": "future"}
            ),
            lambda value: value["by_chat"][0].update({"size": 2}),
            lambda value: value.update({"count": 2}),
        ):
            candidate = json.loads(json.dumps(valid))
            mutate(candidate)
            with self.assertRaises(acceptance.AcceptanceError):
                acceptance._assert_m6_storage_stats(candidate)

    def test_m6_session_flow_orders_daemon_stop_lock_proof_and_no_daemon(self) -> None:
        current = {
            "id": "0",
            "is_current": True,
            "is_password_pending": False,
            "is_unconfirmed": False,
            "can_accept_secret_chats": True,
            "can_accept_calls": True,
            "device_type": "linux",
            "api_id": 1,
            "application_name": "tgcli",
            "application_version": "1",
            "is_official_application": False,
            "device_model": "Test",
            "platform": "Linux",
            "system_version": "1",
            "log_in_date": None,
            "last_active_date": "2026-08-27T12:00:00Z",
            "ip_address": "192.0.2.1",
            "location": "TestDC",
        }
        other = dict(current)
        other.update({"id": "-9223372036854775808", "is_current": False})
        result = {
            "items": [current, other],
            "inactive_session_ttl_days": 30,
            "next": None,
        }

        class FlowRunner:
            def __init__(self) -> None:
                self.calls: list[tuple[str, list[str]]] = []

            def run_json_clean(
                self, label: str, arguments: list[str]
            ) -> acceptance.CommandResult:
                self.calls.append((label, arguments))
                return acceptance.CommandResult(result, frozenset())

            def stop_daemon_verified(self, account: str) -> None:
                self.calls.append(("stop-verified", [account]))

            def register_message_cleanup(self, chat_id: int, message_id: int) -> None:
                raise AssertionError(f"unexpected cleanup: {chat_id}/{message_id}")

        runner = FlowRunner()
        environment = {"HOME": str(self.tree.root), "TGCLI_TEST_DC": "1"}
        acceptance._m6_session_list_flow(runner, environment)
        common = [
            "--json",
            "--account",
            acceptance.USER_ACCOUNT,
            "session",
            "list",
        ]
        self.assertEqual(
            runner.calls,
            [
                ("m6-session-list-daemon-1", common),
                ("m6-session-list-daemon-2", common),
                ("stop-verified", [acceptance.USER_ACCOUNT]),
                ("m6-session-list-no-daemon", ["--no-daemon", *common]),
            ],
        )

    def test_m6_session_evidence_rejects_duplicate_or_missing_current_rows(
        self,
    ) -> None:
        row = {
            "id": "7",
            "is_current": True,
            "is_password_pending": False,
            "is_unconfirmed": False,
            "can_accept_secret_chats": True,
            "can_accept_calls": True,
            "device_type": "linux",
            "api_id": 1,
            "application_name": "tgcli",
            "application_version": "1",
            "is_official_application": False,
            "device_model": "Test",
            "platform": "Linux",
            "system_version": "1",
            "log_in_date": None,
            "last_active_date": None,
            "ip_address": "",
            "location": "",
        }
        document = {
            "items": [row],
            "inactive_session_ttl_days": 30,
            "next": None,
        }
        self.assertEqual(acceptance._assert_m6_session_list(document), ["7"])
        duplicate = json.loads(json.dumps(document))
        duplicate["items"].append(dict(row))
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._assert_m6_session_list(duplicate)
        missing = json.loads(json.dumps(document))
        missing["items"][0]["is_current"] = False
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._assert_m6_session_list(missing)

    def test_registered_cleanup_accepts_delete_success_and_verifies_absence(
        self,
    ) -> None:
        runner = acceptance.Runner.__new__(acceptance.Runner)
        calls: list[tuple[str, list[str]]] = []

        def run_status(label: str, arguments: list[str]) -> acceptance.JsonExecution:
            calls.append((label, arguments))
            return acceptance.JsonExecution(
                {
                    "chat_id": 42,
                    "message_ids": [101],
                    "for_all": False,
                    "deleted": True,
                },
                0,
            )

        def run_error(label: str, arguments: list[str]) -> acceptance.CommandResult:
            calls.append((label, arguments))
            return acceptance.CommandResult(
                {
                    "error": {
                        "code": "NOT_FOUND",
                        "message": "missing",
                        "details": {"chat_id": 42, "missing_ids": [101]},
                    }
                },
                frozenset(),
            )

        runner.run_json_status = run_status
        runner.run_json_error = run_error
        cleanup = acceptance.MessageCleanup(acceptance.USER_ACCOUNT, 42, 101)
        acceptance.Runner.cleanup_message(runner, cleanup)
        self.assertTrue(cleanup.completed)
        self.assertEqual(
            [label for label, _ in calls],
            ["m3-cleanup-delete-101", "m3-cleanup-absence-101"],
        )
        self.assertIn("--yes", calls[0][1])
        self.assertEqual(calls[0][1][-4:], ["msg", "delete", "42", "101"])

    def test_m3_key_scan_covers_every_private_artifact_class_and_earlier_daemon_capture(
        self,
    ) -> None:
        run_root = self.tree.directory("m3-private-artifacts")
        captures = acceptance.Captures(run_root / "captures")
        daemon_stdout, daemon_stderr = captures.allocate("daemon-before-m3")
        command_stdout, command_stderr = captures.allocate("m3-command")
        artifacts = [daemon_stdout, daemon_stderr, command_stdout, command_stderr]
        for relative in (
            "state/tgcli-test/accounts/test-user/audit.log",
            "state/tgcli-test/accounts/test-user/audit.log.1",
            "state/tgcli-test/accounts/test-user/idempotency.db",
            "state/tgcli-test/accounts/test-user/.idempotency.db.tmp",
            "state/tgcli-test/accounts/test-user/tdlib.log",
            "state/tgcli-test/accounts/test-user/tdlib.log.1",
            "state/tgcli-test/accounts/test-user/crash-diagnostic.json",
            "data/tgcli-test/accounts/test-user/td.binlog",
            "config/tgcli-test/config.toml",
            "runtime/tgcli-test/core-disabled.artifact",
        ):
            artifact = run_root / relative
            artifact.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            artifact.write_bytes(b"safe\n")
            artifact.chmod(0o600)
            artifacts.append(artifact)

        sentinel = b"raw-m3-key-sentinel"
        for artifact in artifacts:
            with self.subTest(artifact=artifact.relative_to(run_root)):
                artifact.write_bytes(b"prefix-" + sentinel + b"-suffix")
                with self.assertRaises(acceptance.AcceptanceError) as raised:
                    acceptance._scan_m3_key_artifacts(run_root, captures, [sentinel])
                self.assertNotIn(sentinel.decode(), str(raised.exception))
                artifact.write_bytes(b"safe\n")

    def test_m3_key_scan_accepts_a_complete_clean_private_tree(self) -> None:
        run_root = self.tree.directory("m3-clean-artifacts")
        captures = acceptance.Captures(run_root / "captures")
        stdout, stderr = captures.allocate("daemon-before-m3")
        stdout.write_bytes(b"safe stdout\n")
        stderr.write_bytes(b"safe stderr\n")
        for relative in (
            "state/tgcli-test/accounts/test-user/audit.log",
            "state/tgcli-test/accounts/test-user/idempotency.db",
            "state/tgcli-test/accounts/test-user/tdlib.log",
            "data/tgcli-test/accounts/test-user/td.binlog",
        ):
            artifact = run_root / relative
            artifact.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            artifact.write_bytes(b"safe\n")
            artifact.chmod(0o600)
        acceptance._scan_m3_key_artifacts(run_root, captures, [b"raw-m3-key-sentinel"])

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
        smoke = workflow.split(
            "name: Run M1 authentication and M2 chats smoke", maxsplit=1
        )[1]
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
