#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import fcntl
import json
import os
import pty
import re
import secrets
import selectors
import shlex
import stat
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from test_dc_contract import (
    M1_BOT_TEST,
    M1_QR_TEST,
    PINNED_AUTH_STATES,
    ContractError,
    FrozenSentinel,
    M5ResultRecord,
    SkipEntry,
    auth_state_test_id,
    freeze_auth_sentinels,
    read_secret_fixture,
    scan_frozen_auth_sentinels,
    validate_m1_coverage,
    write_m5_result_artifact,
    write_skip_artifact,
)

USER_ACCOUNT = "test-user"
BOT_ACCOUNT = "test-bot"
QR_ACCOUNT = "test-qr"
REQUIRED_SUBCOMMANDS = (
    "logout",
    "chats",
    "send",
    "msg",
    "saved",
    "resolve",
    "listen",
    "wait-for",
    "contact",
    "folder",
    "topic",
    "chat",
    "storage",
    "session",
)
REQUIRED_OPTIONS = (
    "--allow-write",
    "--yes",
    "--idempotency-key",
    "--verbose",
    "-v",
)
USER_FIELDS = {
    "id",
    "first_name",
    "last_name",
    "usernames",
    "phone_number",
    "is_bot",
    "is_premium",
}
PROMPTS = {
    b"Phone number: ": "phone_number",
    b"Authentication code: ": "authentication_code",
    b"Email address: ": "email_address",
    b"Email code: ": "email_code",
    b"First name: ": "registration_first_name",
    b"Last name (optional): ": "registration_last_name",
    b"Accept Telegram terms? [y/N] ": "registration_terms",
    b"Database encryption key: ": "database_key",
    b"2FA password: ": "password",
    b"Bot token: ": "bot_token",
    b"Telegram api_id: ": "unexpected_app_credential",
    b"Telegram api_hash: ": "unexpected_app_credential",
}
PROMPT_AUTH_STATES = {
    "phone_number": "wait_phone_number",
    "authentication_code": "wait_code",
    "email_address": "wait_email_address",
    "email_code": "wait_email_code",
    "registration_terms": "wait_registration",
    "registration_first_name": "wait_registration",
    "registration_last_name": "wait_registration",
    "password": "wait_password",
}
M6_STORAGE_FILE_TYPES = frozenset(
    {
        "none",
        "animation",
        "audio",
        "document",
        "live-photo-video",
        "notification-sound",
        "photo",
        "photo-story",
        "profile-photo",
        "secret",
        "secret-thumbnail",
        "secure",
        "self-destructing-live-photo-video",
        "self-destructing-photo",
        "self-destructing-video",
        "self-destructing-video-note",
        "self-destructing-voice-note",
        "sticker",
        "thumbnail",
        "unknown",
        "video",
        "video-note",
        "video-story",
        "voice-note",
        "wallpaper",
    }
)
M6_SESSION_DEVICE_TYPES = frozenset(
    {
        "android",
        "apple",
        "brave",
        "chrome",
        "edge",
        "firefox",
        "ipad",
        "iphone",
        "linux",
        "mac",
        "opera",
        "safari",
        "ubuntu",
        "unknown",
        "vivaldi",
        "windows",
        "xbox",
    }
)


class AcceptanceError(RuntimeError):
    pass


class CleanupError(AcceptanceError):
    pass


def _private_directory(directory: Path) -> None:
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    status = directory.lstat()
    if (
        not stat.S_ISDIR(status.st_mode)
        or status.st_uid != os.getuid()
        or status.st_mode & 0o077
    ):
        raise AcceptanceError(f"private directory invariant failed: {directory.name}")


def _private_output(path: Path) -> object:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    return os.fdopen(descriptor, "wb", closefd=True)


class Captures:
    def __init__(self, directory: Path) -> None:
        _private_directory(directory)
        self.directory = directory
        self.stdout_files: list[Path] = []
        self.stderr_files: list[Path] = []
        self._sequence = 0
        self._lock = threading.Lock()

    def allocate(self, label: str) -> tuple[Path, Path]:
        with self._lock:
            sequence = self._sequence
            self._sequence += 1
            stdout = self.directory / f"{sequence:03d}-{label}.stdout"
            stderr = self.directory / f"{sequence:03d}-{label}.stderr"
            self.stdout_files.append(stdout)
            self.stderr_files.append(stderr)
        return stdout, stderr


@dataclass
class CommandResult:
    document: object
    observed_prompts: frozenset[str]


@dataclass
class JsonExecution:
    document: object
    returncode: int
    stderr: bytes = b""


@dataclass
class MessageCleanup:
    account: str
    chat_id: int
    message_id: int
    completed: bool = False


@dataclass
class Daemon:
    account: str
    process: subprocess.Popen[bytes]
    stdout: object
    stderr: object


class ConcurrentLaunch:
    def __init__(self, participants: int, timeout: float) -> None:
        self._processes: dict[str, subprocess.Popen[bytes]] = {}
        self._lock = threading.Lock()
        self._failure: AcceptanceError | None = None
        self._timeout = timeout
        self.overlap_proven = False
        self._barrier = threading.Barrier(participants, action=self._verify_overlap)

    def _verify_overlap(self) -> None:
        with self._lock:
            if any(process.poll() is not None for process in self._processes.values()):
                self._failure = AcceptanceError(
                    "concurrent authentication processes did not overlap"
                )
            else:
                self.overlap_proven = True

    def rendezvous(self, label: str, process: subprocess.Popen[bytes]) -> None:
        with self._lock:
            self._processes[label] = process
        try:
            self._barrier.wait(timeout=self._timeout)
        except threading.BrokenBarrierError as error:
            raise AcceptanceError("concurrent authentication launch failed") from error
        if self._failure is not None:
            raise self._failure


class Runner:
    def __init__(
        self,
        binary: Path,
        environment: Mapping[str, str],
        fixtures: Mapping[str, bytes],
        captures: Captures,
        qr_approver: Path | None,
        timeout: float,
    ) -> None:
        self.binary = binary
        self.environment = dict(environment)
        self.fixtures = fixtures
        self.captures = captures
        self.qr_approver = qr_approver
        self.timeout = timeout
        self.daemons: list[Daemon] = []
        self.message_cleanups: list[MessageCleanup] = []
        self.qr_approvals = 0

    def _argv(self, arguments: Sequence[str]) -> list[str]:
        return [str(self.binary), *arguments]

    def run_json_status(self, label: str, arguments: Sequence[str]) -> JsonExecution:
        stdout_path, stderr_path = self.captures.allocate(label)
        try:
            with (
                _private_output(stdout_path) as stdout,
                _private_output(stderr_path) as stderr,
            ):
                completed = subprocess.run(
                    self._argv(arguments),
                    stdin=subprocess.DEVNULL,
                    stdout=stdout,
                    stderr=stderr,
                    env=self.environment,
                    timeout=self.timeout,
                    check=False,
                )
        except (OSError, subprocess.SubprocessError) as error:
            raise AcceptanceError(f"command execution failed: {label}") from error
        source = stdout_path if completed.returncode == 0 else stderr_path
        try:
            stderr_bytes = stderr_path.read_bytes()
        except OSError as error:
            raise AcceptanceError(f"cannot read command stderr: {label}") from error
        return JsonExecution(
            _load_json(source, label), completed.returncode, stderr_bytes
        )

    def run_json(self, label: str, arguments: Sequence[str]) -> CommandResult:
        completed = self.run_json_status(label, arguments)
        if completed.returncode != 0:
            raise AcceptanceError(f"command returned non-zero: {label}")
        return CommandResult(completed.document, frozenset())

    def run_json_error(self, label: str, arguments: Sequence[str]) -> CommandResult:
        completed = self.run_json_status(label, arguments)
        if completed.returncode == 0:
            raise AcceptanceError(f"command unexpectedly succeeded: {label}")
        return CommandResult(completed.document, frozenset())

    def run_json_clean(self, label: str, arguments: Sequence[str]) -> CommandResult:
        completed = self.run_json_status(label, arguments)
        if completed.returncode != 0:
            raise AcceptanceError(f"command returned non-zero: {label}")
        if completed.stderr:
            raise AcceptanceError(f"command returned unexpected stderr: {label}")
        return CommandResult(completed.document, frozenset())

    def register_message_cleanup(self, chat_id: int, message_id: int) -> MessageCleanup:
        cleanup = MessageCleanup(USER_ACCOUNT, chat_id, message_id)
        self.message_cleanups.append(cleanup)
        return cleanup

    def cleanup_message(self, cleanup: MessageCleanup) -> None:
        if cleanup.completed:
            return
        deleted = self.run_json_status(
            f"m3-cleanup-delete-{cleanup.message_id}",
            [
                "--json",
                "--allow-write",
                "--yes",
                "--account",
                cleanup.account,
                "msg",
                "delete",
                str(cleanup.chat_id),
                str(cleanup.message_id),
            ],
        )
        expected = {
            "chat_id": cleanup.chat_id,
            "message_ids": [cleanup.message_id],
            "for_all": False,
            "deleted": True,
        }
        if deleted.returncode == 0:
            if deleted.document != expected:
                raise AcceptanceError("M3 cleanup returned an invalid delete result")
        elif _error_code(deleted.document) != "NOT_FOUND":
            raise AcceptanceError("M3 cleanup delete failed")
        absent = self.run_json_error(
            f"m3-cleanup-absence-{cleanup.message_id}",
            [
                "--json",
                "--account",
                cleanup.account,
                "msg",
                "get",
                str(cleanup.chat_id),
                str(cleanup.message_id),
            ],
        ).document
        if _error_code(absent) != "NOT_FOUND":
            raise AcceptanceError("M3 cleanup did not make the message absent")
        details = (
            absent.get("error", {}).get("details", {})
            if isinstance(absent, dict)
            else {}
        )
        if details != {"chat_id": cleanup.chat_id, "missing_ids": [cleanup.message_id]}:
            raise AcceptanceError("M3 cleanup absence result is invalid")
        cleanup.completed = True

    def cleanup_messages(self) -> None:
        failure: BaseException | None = None
        for cleanup in reversed(self.message_cleanups):
            try:
                self.cleanup_message(cleanup)
            except (AcceptanceError, OSError, subprocess.SubprocessError) as error:
                failure = error
        if failure is not None:
            raise CleanupError("M3 message cleanup failed") from failure

    def run_interactive(
        self,
        label: str,
        arguments: Sequence[str],
        responses: Mapping[str, bytes],
        progress: Callable[[dict[str, object]], None] | None = None,
        concurrent_launch: ConcurrentLaunch | None = None,
    ) -> CommandResult:
        stdout_path, stderr_path = self.captures.allocate(label)
        master, slave = pty.openpty()
        observed: set[str] = set()
        maximum_prompt = max(len(prompt) for prompt in PROMPTS)
        tail = b""
        line_buffer = b""
        try:
            stdout = _private_output(stdout_path)
            stderr = _private_output(stderr_path)
            try:
                process = subprocess.Popen(
                    self._argv(arguments),
                    stdin=slave,
                    stdout=stdout,
                    stderr=subprocess.PIPE,
                    env=self.environment,
                )
            except OSError as error:
                stdout.close()
                stderr.close()
                raise AcceptanceError(f"command execution failed: {label}") from error
            finally:
                os.close(slave)

            selector: selectors.BaseSelector | None = None
            try:
                if concurrent_launch is not None:
                    concurrent_launch.rendezvous(label, process)
                selector = selectors.DefaultSelector()
                assert process.stderr is not None
                selector.register(process.stderr, selectors.EVENT_READ)
                deadline = time.monotonic() + self.timeout
                while selector.get_map():
                    if time.monotonic() >= deadline:
                        raise AcceptanceError(f"interactive command timed out: {label}")
                    events = selector.select(0.1)
                    if not events and process.poll() is not None:
                        events = [
                            (key, selectors.EVENT_READ)
                            for key in selector.get_map().values()
                        ]
                    for key, _ in events:
                        chunk = os.read(key.fd, 64 * 1024)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        stderr.write(chunk)
                        stderr.flush()
                        window = tail + chunk
                        for prompt, fixture_name in PROMPTS.items():
                            start = 0
                            while (offset := window.find(prompt, start)) >= 0:
                                end = offset + len(prompt)
                                start = end
                                if end <= len(tail):
                                    continue
                                if fixture_name in observed:
                                    raise AcceptanceError(
                                        f"interactive prompt repeated: {fixture_name}"
                                    )
                                if fixture_name == "unexpected_app_credential":
                                    raise AcceptanceError(
                                        "app credentials were not accepted from the isolated environment"
                                    )
                                if fixture_name not in responses:
                                    raise AcceptanceError(
                                        f"required interactive fixture is unavailable: {fixture_name}"
                                    )
                                os.write(master, responses[fixture_name] + b"\n")
                                observed.add(fixture_name)
                        tail = window[-(maximum_prompt - 1) :]
                        if progress is not None:
                            line_buffer += chunk
                            while b"\n" in line_buffer:
                                line, line_buffer = line_buffer.split(b"\n", 1)
                                try:
                                    value = json.loads(line)
                                except (UnicodeDecodeError, json.JSONDecodeError):
                                    continue
                                if isinstance(value, dict):
                                    progress(value)
                return_code = process.wait(timeout=1)
            except BaseException:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
                raise
            finally:
                if selector is not None:
                    selector.close()
                process.stderr.close()
                stdout.close()
                stderr.close()
        finally:
            os.close(master)
        if return_code != 0:
            raise AcceptanceError(f"command returned non-zero: {label}")
        return CommandResult(_load_json(stdout_path, label), frozenset(observed))

    def start_daemon(self, account: str) -> None:
        stdout_path, stderr_path = self.captures.allocate(f"daemon-{account}")
        stdout = _private_output(stdout_path)
        stderr = _private_output(stderr_path)
        try:
            process = subprocess.Popen(
                self._argv(["--account", account, "daemon", "run"]),
                stdin=subprocess.DEVNULL,
                stdout=stdout,
                stderr=stderr,
                env=self.environment,
            )
        except OSError as error:
            stdout.close()
            stderr.close()
            raise AcceptanceError(f"cannot start daemon: {account}") from error
        daemon = Daemon(account, process, stdout, stderr)
        self.daemons.append(daemon)
        deadline = time.monotonic() + min(self.timeout, 15)
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AcceptanceError(f"daemon exited during startup: {account}")
            try:
                status = self.run_json(
                    f"status-{account}",
                    ["--json", "--account", account, "daemon", "status"],
                ).document
            except AcceptanceError:
                time.sleep(0.1)
                continue
            if (
                isinstance(status, dict)
                and status.get("running") is True
                and status.get("pid") == process.pid
            ):
                return
            time.sleep(0.1)
        raise AcceptanceError(f"daemon did not become ready: {account}")

    def approve_qr(self, progress_document: dict[str, object]) -> None:
        progress = progress_document.get("progress")
        if not isinstance(progress, dict) or progress.get("kind") != "auth_qr":
            return
        link = progress.get("link")
        if not isinstance(link, str) or not link or self.qr_approver is None:
            raise AcceptanceError("QR progress did not provide an approvable link")
        stdout_path, stderr_path = self.captures.allocate("qr-approver")
        try:
            with (
                _private_output(stdout_path) as stdout,
                _private_output(stderr_path) as stderr,
            ):
                completed = subprocess.run(
                    [str(self.qr_approver)],
                    input=(link + "\n").encode(),
                    stdout=stdout,
                    stderr=stderr,
                    env=self.environment,
                    timeout=self.timeout,
                    check=False,
                )
        except (OSError, subprocess.SubprocessError) as error:
            raise AcceptanceError("QR approver execution failed") from error
        if completed.returncode != 0:
            raise AcceptanceError("QR approver returned non-zero")
        self.qr_approvals += 1

    def stop_daemons(self) -> None:
        failure: BaseException | None = None
        for daemon in reversed(self.daemons):
            try:
                try:
                    self.run_json(
                        f"stop-{daemon.account}",
                        ["--json", "--account", daemon.account, "daemon", "stop"],
                    )
                except AcceptanceError:
                    if daemon.process.poll() is None:
                        daemon.process.terminate()
                if daemon.process.poll() is None:
                    try:
                        daemon.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        daemon.process.kill()
                        daemon.process.wait(timeout=2)
                status = self.run_json(
                    f"status-after-stop-{daemon.account}",
                    ["--json", "--account", daemon.account, "daemon", "status"],
                ).document
                if isinstance(status, dict) and status.get("running") is True:
                    self.run_json(
                        f"stop-replacement-{daemon.account}",
                        ["--json", "--account", daemon.account, "daemon", "stop"],
                    )
                    status = self.run_json(
                        f"status-after-replacement-stop-{daemon.account}",
                        ["--json", "--account", daemon.account, "daemon", "status"],
                    ).document
                if not isinstance(status, dict) or status.get("running") is not False:
                    raise AcceptanceError(
                        f"daemon remained after cleanup: {daemon.account}"
                    )
            except (AcceptanceError, OSError, subprocess.SubprocessError) as error:
                failure = failure or error
                try:
                    if daemon.process.poll() is None:
                        daemon.process.kill()
                        daemon.process.wait(timeout=2)
                except (OSError, subprocess.SubprocessError):
                    pass
            finally:
                daemon.stdout.close()
                daemon.stderr.close()
        if failure is not None:
            raise AcceptanceError("daemon cleanup failed") from failure

    def stop_daemon_verified(self, account: str) -> None:
        daemon = next((item for item in self.daemons if item.account == account), None)
        if daemon is None:
            raise AcceptanceError(f"tracked daemon is unavailable: {account}")
        self.run_json_clean(
            f"m6-stop-{account}", ["--json", "--account", account, "daemon", "stop"]
        )
        try:
            daemon.process.wait(timeout=min(self.timeout, 10))
        except subprocess.TimeoutExpired as error:
            raise AcceptanceError(f"daemon did not stop: {account}") from error
        if daemon.process.returncode != 0:
            raise AcceptanceError(f"daemon returned non-zero while stopping: {account}")

        namespace = (
            "tgcli-test" if self.environment.get("TGCLI_TEST_DC") == "1" else "tgcli"
        )
        runtime_base = self.environment.get("XDG_RUNTIME_DIR")
        if runtime_base is None:
            runtime_base = str(
                Path(self.environment.get("TMPDIR", "/tmp"))
                / f"{namespace}-{os.getuid()}"
            )
            runtime_directory = Path(runtime_base)
        else:
            runtime_directory = Path(runtime_base) / namespace
        socket = runtime_directory / f"{account}.sock"
        deadline = time.monotonic() + min(self.timeout, 10)
        while socket.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        if socket.exists():
            raise AcceptanceError(f"daemon socket remained after stop: {account}")

        state_base = self.environment.get("XDG_STATE_HOME")
        if state_base is None:
            home = self.environment.get("HOME")
            if home is None:
                raise AcceptanceError(
                    "HOME is unavailable for daemon lock verification"
                )
            state_base = str(Path(home) / ".local" / "state")
        lock = Path(state_base) / namespace / "accounts" / account / "daemon.lock"
        flags = os.O_RDWR | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        try:
            descriptor = os.open(lock, flags)
            status = os.fstat(descriptor)
            if (
                not stat.S_ISREG(status.st_mode)
                or status.st_uid != os.getuid()
                or status.st_mode & 0o077
            ):
                raise AcceptanceError("daemon lock identity is invalid after stop")
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        except OSError as error:
            raise AcceptanceError(f"daemon lock remained held: {account}") from error
        finally:
            if "descriptor" in locals():
                os.close(descriptor)
        daemon.stdout.close()
        daemon.stderr.close()
        self.daemons.remove(daemon)


def _load_json(source: Path, label: str) -> object:
    try:
        return json.loads(source.read_bytes())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AcceptanceError(
            f"command did not return one JSON document: {label}"
        ) from error


def _error_code(document: object) -> str | None:
    if not isinstance(document, dict) or set(document) != {"error"}:
        return None
    error = document["error"]
    if not isinstance(error, dict):
        return None
    code = error.get("code")
    return code if isinstance(code, str) else None


def _help_tokens(help_text: str) -> tuple[set[str], set[str]]:
    option_tokens: set[str] = set()
    subcommands: set[str] = set()
    section = ""
    for line in help_text.splitlines():
        stripped = line.strip()
        if stripped == "OPTIONS:":
            section = "options"
            continue
        if stripped == "SUBCOMMANDS:":
            section = "subcommands"
            continue
        fields = stripped.replace(",", " ").split()
        if section == "options" and fields and fields[0].startswith("-"):
            for field in fields:
                if not field.startswith("-"):
                    break
                option_tokens.add(field)
        elif section == "subcommands" and fields:
            subcommands.add(fields[0])
    return option_tokens, subcommands


def _surface_preflight(
    binary: Path, environment: Mapping[str, str], timeout: float
) -> None:
    try:
        status = binary.lstat()
    except OSError as error:
        raise AcceptanceError("tgcli binary is unavailable") from error
    if not stat.S_ISREG(status.st_mode) or not os.access(binary, os.X_OK):
        raise AcceptanceError("tgcli binary must be an executable regular file")

    def inspect(arguments: Sequence[str]) -> tuple[int, set[str], set[str]]:
        try:
            completed = subprocess.run(
                [str(binary), *arguments],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                env=environment,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise AcceptanceError("cannot inspect the tgcli CLI surface") from error
        try:
            help_text = (completed.stdout + completed.stderr).decode("utf-8")
        except UnicodeDecodeError as error:
            raise AcceptanceError("tgcli help is not valid UTF-8") from error
        options, subcommands = _help_tokens(help_text)
        return completed.returncode, options, subcommands

    root_code, option_tokens, subcommands = inspect(["--help"])
    msg_code, _, msg_subcommands = inspect(["msg", "--help"])
    saved_code, _, saved_subcommands = inspect(["saved", "--help"])
    missing = [token for token in REQUIRED_OPTIONS if token not in option_tokens]
    missing.extend(token for token in REQUIRED_SUBCOMMANDS if token not in subcommands)
    if "delete" not in msg_subcommands:
        missing.append("msg delete")
    if "attach" not in saved_subcommands:
        missing.append("saved attach")
    if root_code != 0 or msg_code != 0 or saved_code != 0 or missing:
        raise AcceptanceError("tgcli is missing the test-DC runtime surface")


def _approver_preflight(approver: Path | None) -> None:
    if approver is None:
        return
    try:
        status = approver.lstat()
    except OSError as error:
        raise AcceptanceError("QR approver is unavailable") from error
    if not stat.S_ISREG(status.st_mode) or not os.access(approver, os.X_OK):
        raise AcceptanceError("QR approver must be an executable regular file")


def _isolated_environment(run_root: Path) -> tuple[dict[str, str], dict[Path, bytes]]:
    values = {
        "home": run_root / "home",
        "config": run_root / "config",
        "data": run_root / "data",
        "state": run_root / "state",
        "runtime": run_root / "runtime",
        "cache": run_root / "cache",
        "tmp": run_root / "tmp",
    }
    for directory in values.values():
        _private_directory(directory)
    environment = {
        "HOME": str(values["home"]),
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "XDG_CONFIG_HOME": str(values["config"]),
        "XDG_DATA_HOME": str(values["data"]),
        "XDG_STATE_HOME": str(values["state"]),
        "XDG_RUNTIME_DIR": str(values["runtime"]),
        "XDG_CACHE_HOME": str(values["cache"]),
        "TMPDIR": str(values["tmp"]),
        "TGCLI_TEST_DC": "1",
    }
    for name in ("LANG", "LC_ALL", "LC_CTYPE"):
        if value := os.environ.get(name):
            environment[name] = value
    for name in ("TGCLI_API_ID", "TGCLI_API_HASH"):
        if value := os.environ.get(name):
            environment[name] = value
    if "TGCLI_API_ID" not in environment or "TGCLI_API_HASH" not in environment:
        raise AcceptanceError("test-DC app credentials are unavailable")

    traps: dict[Path, bytes] = {}
    for base in (values["config"], values["data"], values["state"], values["runtime"]):
        trap = base / "tgcli"
        payload = b"production namespace access is forbidden\n"
        with _private_output(trap) as output:
            output.write(payload)
        traps[trap] = payload
    return environment, traps


def _verify_production_traps(traps: Mapping[Path, bytes]) -> None:
    for trap, expected in traps.items():
        try:
            status = trap.lstat()
            actual = trap.read_bytes()
        except OSError as error:
            raise AcceptanceError("production namespace trap was modified") from error
        if (
            not stat.S_ISREG(status.st_mode)
            or status.st_nlink != 1
            or actual != expected
        ):
            raise AcceptanceError("production namespace trap was modified")


def _load_fixtures(directory: Path) -> tuple[dict[str, bytes], dict[str, Path]]:
    required = ("phone_number", "authentication_code", "database_key")
    optional = (
        "registration_first_name",
        "registration_last_name",
        "email_address",
        "email_code",
        "password",
        "bot_token",
    )
    fixtures: dict[str, bytes] = {}
    sources: dict[str, Path] = {}
    for name in required:
        source = directory / name
        fixtures[name] = read_secret_fixture(source)
        sources[name] = source
    for name in optional:
        source = directory / name
        if source.exists():
            fixtures[name] = read_secret_fixture(source)
            sources[name] = source
    fixtures.setdefault("registration_first_name", b"tgcli")
    fixtures.setdefault("registration_last_name", b"")
    fixtures["registration_terms"] = b"y"
    return fixtures, sources


def _hook(source: Path) -> str:
    return f"exec /bin/cat -- {shlex.quote(str(source))}"


def _write_config(
    environment: Mapping[str, str], sources: Mapping[str, Path], accounts: list[str]
) -> None:
    config_directory = Path(environment["XDG_CONFIG_HOME"]) / "tgcli-test"
    _private_directory(config_directory)
    lines = [f'default_account = "{USER_ACCOUNT}"', ""]
    for account in accounts:
        lines.extend([f"[accounts.{account}]", "allow_write = false"])
        lines.append(f"db_key_cmd = {json.dumps(_hook(sources['database_key']))}")
        if account == USER_ACCOUNT and "password" in sources:
            lines.append(f"password_cmd = {json.dumps(_hook(sources['password']))}")
        if account == BOT_ACCOUNT:
            lines.append(f"bot_token_cmd = {json.dumps(_hook(sources['bot_token']))}")
        lines.append("")
    target = config_directory / "config.toml"
    temporary = config_directory / f".config.toml.test-dc-{os.getpid()}"
    with _private_output(temporary) as output:
        output.write(("\n".join(lines) + "\n").encode())
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, target)


def _assert_account_add(document: object, account: str, expected_default: bool) -> None:
    if (
        not isinstance(document, dict)
        or set(document) != {"account", "created", "default"}
        or document.get("account") != account
    ):
        raise AcceptanceError(f"account add returned an invalid result: {account}")
    if (
        document.get("created") is not True
        or document.get("default") is not expected_default
    ):
        raise AcceptanceError(f"account add returned an invalid result: {account}")


def _assert_account_persistence(
    runner: Runner, environment: Mapping[str, str], accounts: Sequence[str]
) -> None:
    listed = runner.run_json("account-list-after-add", ["--json", "account", "list"])
    expected_items = [
        {"name": account, "default": account == USER_ACCOUNT}
        for account in sorted(accounts, key=os.fsencode)
    ]
    if listed.document != {"items": expected_items, "next": None}:
        raise AcceptanceError("account add did not persist the exact account set")
    for account in accounts:
        shown = runner.run_json(
            f"account-show-after-add-{account}",
            ["--json", "account", "show", account],
        ).document
        expected_paths = {
            "data": str(
                Path(environment["XDG_DATA_HOME"]) / "tgcli-test" / "accounts" / account
            ),
            "state": str(
                Path(environment["XDG_STATE_HOME"])
                / "tgcli-test"
                / "accounts"
                / account
            ),
            "socket": str(
                Path(environment["XDG_RUNTIME_DIR"]) / "tgcli-test" / f"{account}.sock"
            ),
        }
        if (
            not isinstance(shown, dict)
            or set(shown)
            != {
                "account",
                "default",
                "allow_write",
                "idle_exit",
                "credentials",
                "paths",
            }
            or shown["account"] != account
            or shown["default"] != (account == USER_ACCOUNT)
            or shown["allow_write"] is not False
            or shown["idle_exit"] is not None
            or shown["paths"] != expected_paths
        ):
            raise AcceptanceError(f"account add persistence is invalid: {account}")


def _assert_login(document: object, account: str) -> dict[str, object]:
    if not isinstance(document, dict) or set(document) != {
        "account",
        "auth_state",
        "user",
    }:
        raise AcceptanceError(f"login returned an invalid result: {account}")
    if document["account"] != account or document["auth_state"] != "ready":
        raise AcceptanceError(f"login did not reach ready: {account}")
    user = document["user"]
    if (
        not isinstance(user, dict)
        or set(user) != USER_FIELDS
        or not isinstance(user["id"], int)
        or isinstance(user["id"], bool)
        or not isinstance(user["first_name"], str)
        or not isinstance(user["last_name"], str)
        or not isinstance(user["usernames"], list)
        or not all(isinstance(value, str) for value in user["usernames"])
        or not isinstance(user["phone_number"], str)
        or not isinstance(user["is_bot"], bool)
        or not isinstance(user["is_premium"], bool)
    ):
        raise AcceptanceError(f"login returned an invalid identity: {account}")
    return user


def _require_phone_code_flow(result: CommandResult, label: str) -> None:
    required = {"phone_number", "authentication_code"}
    if not required.issubset(result.observed_prompts):
        raise AcceptanceError(
            f"{label} did not exercise phone and fixed-code challenges"
        )


def _assert_bot_identity(user: Mapping[str, object]) -> None:
    if user["is_bot"] is not True or user["phone_number"] != "":
        raise AcceptanceError("bot login returned a non-bot identity")


def _assert_saved_tags(document: object) -> None:
    if (
        not isinstance(document, dict)
        or set(document) != {"items", "next"}
        or document["next"] is not None
        or not isinstance(document["items"], list)
    ):
        raise AcceptanceError("saved tags returned an invalid result")
    for item in document["items"]:
        if (
            not isinstance(item, dict)
            or set(item) != {"tag", "label", "count"}
            or not isinstance(item["tag"], str)
            or not item["tag"]
            or not isinstance(item["label"], str)
            or not isinstance(item["count"], int)
            or isinstance(item["count"], bool)
            or item["count"] < 0
        ):
            raise AcceptanceError("saved tags returned an invalid item")


def _int(value: object, minimum: int, maximum: int) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and minimum <= value <= maximum
    )


def _assert_chats_topic(topic: object) -> None:
    if not isinstance(topic, dict) or set(topic) != {"kind", "id"}:
        raise AcceptanceError("chats returned an invalid message topic")
    kind = topic["kind"]
    maximum = 2_147_483_647 if kind == "forum" else 9_007_199_254_740_991
    if kind not in {"forum", "thread", "direct", "saved"} or not _int(
        topic["id"], 1, maximum
    ):
        raise AcceptanceError("chats returned an invalid message topic")


def _assert_chats_message(message: object, chat_id: int) -> None:
    fields = {
        "id",
        "chat_id",
        "date",
        "sender",
        "is_outgoing",
        "topic",
        "type",
        "text",
    }
    if not isinstance(message, dict) or set(message) != fields:
        raise AcceptanceError("chats returned an invalid last message")
    if (
        not _int(message["id"], -9_007_199_254_740_991, 9_007_199_254_740_991)
        or message["id"] == 0
        or message["chat_id"] != chat_id
        or not isinstance(message["is_outgoing"], bool)
        or message["type"] not in {"text", "photo", "video", "doc", "voice", "other"}
        or not isinstance(message["text"], str)
    ):
        raise AcceptanceError("chats returned an invalid last message")
    date = message["date"]
    if date is not None and (
        not isinstance(date, str)
        or re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", date)
        is None
    ):
        raise AcceptanceError("chats returned an invalid last message date")
    sender = message["sender"]
    if not isinstance(sender, dict) or set(sender) != {"type", "id"}:
        raise AcceptanceError("chats returned an invalid message sender")
    if sender["type"] == "user":
        valid_sender = _int(sender["id"], 1, 9_007_199_254_740_991)
    elif sender["type"] == "chat":
        valid_sender = (
            _int(sender["id"], -9_007_199_254_740_991, 9_007_199_254_740_991)
            and sender["id"] != 0
        )
    else:
        valid_sender = False
    if not valid_sender:
        raise AcceptanceError("chats returned an invalid message sender")
    if message["topic"] is not None:
        _assert_chats_topic(message["topic"])


def _assert_chats(document: object) -> None:
    if (
        not isinstance(document, dict)
        or set(document) != {"items", "next"}
        or not isinstance(document["items"], list)
        or len(document["items"]) > 1
        or (
            document["next"] is not None
            and (not isinstance(document["next"], str) or not document["next"])
        )
        or (not document["items"] and document["next"] is not None)
    ):
        raise AcceptanceError("chats returned an invalid result")
    for chat in document["items"]:
        fields = {
            "id",
            "title",
            "type",
            "is_bot",
            "usernames",
            "is_archived",
            "folder_ids",
            "is_marked_unread",
            "unread_count",
            "unread_mention_count",
            "unread_reaction_count",
            "unread_poll_vote_count",
            "last_message",
        }
        if not isinstance(chat, dict) or set(chat) != fields:
            raise AcceptanceError("chats returned an invalid item")
        chat_id = chat["id"]
        folder_ids = chat["folder_ids"]
        if (
            not _int(chat_id, -9_007_199_254_740_991, 9_007_199_254_740_991)
            or chat_id == 0
            or not isinstance(chat["title"], str)
            or chat["type"] not in {"private", "basic_group", "supergroup", "channel"}
            or not isinstance(chat["is_bot"], bool)
            or (chat["type"] != "private" and chat["is_bot"])
            or not isinstance(chat["usernames"], list)
            or not all(isinstance(value, str) and value for value in chat["usernames"])
            or not isinstance(chat["is_archived"], bool)
            or not isinstance(folder_ids, list)
            or not all(_int(value, 1, 2_147_483_647) for value in folder_ids)
            or folder_ids != sorted(set(folder_ids))
            or not isinstance(chat["is_marked_unread"], bool)
            or not all(
                _int(chat[field], 0, 2_147_483_647)
                for field in (
                    "unread_count",
                    "unread_mention_count",
                    "unread_reaction_count",
                    "unread_poll_vote_count",
                )
            )
        ):
            raise AcceptanceError("chats returned an invalid item")
        if chat["last_message"] is not None:
            _assert_chats_message(chat["last_message"], chat_id)


def _require_qr_approval(runner: Runner) -> None:
    if runner.qr_approvals < 1:
        raise AcceptanceError("QR login completed without an auth_qr approval")


def _observed_auth_state_tests(result: CommandResult) -> set[str]:
    return {
        auth_state_test_id(state)
        for prompt, state in PROMPT_AUTH_STATES.items()
        if prompt in result.observed_prompts
    }


def _state_log_directory(environment: Mapping[str, str], account: str) -> Path:
    return Path(environment["XDG_STATE_HOME"]) / "tgcli-test" / "accounts" / account


def _authoritative_send_ids(document: object) -> tuple[int, int]:
    if not isinstance(document, dict):
        raise AcceptanceError("M3 send returned a non-object result")
    message_id = document.get("id")
    chat_id = document.get("chat_id")
    if (
        not _int(message_id, -9_007_199_254_740_991, 9_007_199_254_740_991)
        or message_id == 0
        or not _int(chat_id, -9_007_199_254_740_991, 9_007_199_254_740_991)
        or chat_id == 0
    ):
        raise AcceptanceError("M3 send did not return authoritative final ids")
    return chat_id, message_id


def _assert_send_result(
    document: object, chat_id: int, message_id: int, text: str
) -> None:
    fields = {
        "id",
        "chat_id",
        "date",
        "sender",
        "is_outgoing",
        "topic",
        "type",
        "text",
        "scheduled",
    }
    if not isinstance(document, dict) or set(document) != fields:
        raise AcceptanceError("M3 send returned an invalid result")
    sender = document["sender"]
    if (
        document["id"] != message_id
        or document["chat_id"] != chat_id
        or not isinstance(document["date"], str)
        or re.fullmatch(
            r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z",
            document["date"],
        )
        is None
        or not isinstance(sender, dict)
        or set(sender) != {"type", "id"}
        or sender["type"] != "user"
        or not _int(sender["id"], 1, 9_007_199_254_740_991)
        or document["is_outgoing"] is not True
        or document["topic"] is not None
        or document["type"] != "text"
        or document["text"] != text
        or document["scheduled"] is not False
    ):
        raise AcceptanceError("M3 send returned invalid message facts")


def _assert_saved_attach_result(
    document: object, chat_id: int, message_id: int, caption: str
) -> None:
    fields = {
        "id",
        "chat_id",
        "date",
        "sender",
        "is_outgoing",
        "topic",
        "type",
        "text",
        "scheduled",
    }
    if not isinstance(document, dict) or set(document) != fields:
        raise AcceptanceError("M4 saved attach returned an invalid result")
    sender = document["sender"]
    topic = document["topic"]
    if (
        document["id"] != message_id
        or document["chat_id"] != chat_id
        or not isinstance(document["date"], str)
        or re.fullmatch(
            r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z",
            document["date"],
        )
        is None
        or not isinstance(sender, dict)
        or set(sender) != {"type", "id"}
        or sender["type"] != "user"
        or not _int(sender["id"], 1, 9_007_199_254_740_991)
        or document["is_outgoing"] is not True
        or (
            topic is not None
            and (
                not isinstance(topic, dict)
                or set(topic) != {"kind", "id"}
                or topic["kind"] != "saved"
                or not _int(topic["id"], 1, 9_007_199_254_740_991)
            )
        )
        or document["type"] != "doc"
        or document["text"] != caption
        or document["scheduled"] is not False
    ):
        raise AcceptanceError("M4 saved attach returned invalid message facts")


def _assert_get_matches_send(document: object, sent: Mapping[str, object]) -> None:
    expected = {name: value for name, value in sent.items() if name != "scheduled"}
    if document != {"items": [expected], "next": None}:
        raise AcceptanceError("msg get did not return the authoritative sent message")


def _assert_error(document: object, code: str) -> None:
    if _error_code(document) != code:
        raise AcceptanceError(f"command did not return {code}")


def _scan_m3_key_artifacts(
    run_root: Path, captures: Captures, sentinels: Sequence[bytes]
) -> None:
    if not sentinels or any(not sentinel for sentinel in sentinels):
        raise AcceptanceError("M3 idempotency scan inputs are incomplete")
    sources = set(captures.stdout_files + captures.stderr_files)
    try:
        for source in run_root.rglob("*"):
            status = source.lstat()
            if stat.S_ISLNK(status.st_mode):
                raise AcceptanceError("M3 private artifact scan encountered a symlink")
            if stat.S_ISREG(status.st_mode):
                sources.add(source)
        maximum = max(len(sentinel) for sentinel in sentinels)
        for source in sorted(sources, key=lambda item: os.fsencode(item)):
            status = source.lstat()
            if not stat.S_ISREG(status.st_mode) or stat.S_ISLNK(status.st_mode):
                raise AcceptanceError("M3 private artifact scan source is unsafe")
            tail = b""
            with source.open("rb") as stream:
                while chunk := stream.read(64 * 1024):
                    window = tail + chunk
                    if any(sentinel in window for sentinel in sentinels):
                        raise AcceptanceError(
                            "raw M3 idempotency key leaked into private acceptance artifacts"
                        )
                    tail = window[-(maximum - 1) :] if maximum > 1 else b""
    except OSError as error:
        raise AcceptanceError("cannot scan M3 private acceptance artifacts") from error


def _m3_write_flow(runner: Runner, key_sentinels: list[bytes] | None = None) -> None:
    raw_key = f"tgcli-test-dc-{secrets.token_hex(16)}"
    if key_sentinels is not None:
        key_sentinels.append(raw_key.encode())
    text = f"tgcli M3 acceptance {secrets.token_hex(16)}"
    arguments = [
        "--json",
        "--allow-write",
        "--account",
        USER_ACCOUNT,
        "--idempotency-key",
        raw_key,
        "send",
        "t.me/saved",
        text,
    ]
    sent = runner.run_json("m3-send", arguments).document
    chat_id, message_id = _authoritative_send_ids(sent)
    cleanups = [runner.register_message_cleanup(chat_id, message_id)]
    try:
        _assert_send_result(sent, chat_id, message_id, text)
        fetched = runner.run_json(
            "m3-get",
            [
                "--json",
                "--account",
                USER_ACCOUNT,
                "msg",
                "get",
                str(chat_id),
                str(message_id),
            ],
        ).document
        _assert_get_matches_send(fetched, sent)
        replayed = runner.run_json("m3-send-replay", arguments).document
        if replayed != sent:
            raise AcceptanceError("M3 send replay changed the stored result")
        conflict_arguments = [*arguments]
        conflict_arguments[-1] = text + " conflict"
        conflict = runner.run_json_error(
            "m3-send-conflict", conflict_arguments
        ).document
        _assert_error(conflict, "IDEMPOTENCY_CONFLICT")

        fixture_path = runner.captures.directory / "m4-saved-attachment.bin"
        with _private_output(fixture_path) as output:
            output.write(b"tgcli deterministic M4 attachment fixture\n")
            output.flush()
            os.fsync(output.fileno())
        m4_key = f"tgcli-test-dc-m4-{secrets.token_hex(16)}"
        if key_sentinels is not None:
            key_sentinels.append(m4_key.encode())
        caption = "tgcli M4 attachment"
        attach_arguments = [
            "--json",
            "--allow-write",
            "--account",
            USER_ACCOUNT,
            "--idempotency-key",
            m4_key,
            "saved",
            "attach",
            str(message_id),
            str(fixture_path),
            "--caption",
            caption,
        ]
        attached = runner.run_json("m4-saved-attach", attach_arguments).document
        attached_chat_id, attached_message_id = _authoritative_send_ids(attached)
        cleanups.append(
            runner.register_message_cleanup(attached_chat_id, attached_message_id)
        )
        _assert_saved_attach_result(
            attached, attached_chat_id, attached_message_id, caption
        )
        attached_get = runner.run_json(
            "m4-saved-attach-get",
            [
                "--json",
                "--account",
                USER_ACCOUNT,
                "msg",
                "get",
                str(attached_chat_id),
                str(attached_message_id),
            ],
        ).document
        _assert_get_matches_send(attached_get, attached)
        attach_replay = runner.run_json(
            "m4-saved-attach-replay", attach_arguments
        ).document
        if attach_replay != attached:
            raise AcceptanceError("M4 saved attach replay changed the stored result")
        attach_conflict_arguments = [*attach_arguments]
        attach_conflict_arguments[-1] = caption + " conflict"
        attach_conflict = runner.run_json_error(
            "m4-saved-attach-conflict", attach_conflict_arguments
        ).document
        _assert_error(attach_conflict, "IDEMPOTENCY_CONFLICT")
    finally:
        cleanup_failure: BaseException | None = None
        for cleanup in reversed(cleanups):
            try:
                runner.cleanup_message(cleanup)
            except (AcceptanceError, OSError, subprocess.SubprocessError) as error:
                cleanup_failure = error
        if cleanup_failure is not None:
            raise CleanupError("M3/M4 cleanup failed") from cleanup_failure


def _saved_messages_chat_id(document: object, self_user_id: int) -> int:
    fields = {"kind", "chat", "message_id", "topic", "link_type", "is_public"}
    if not isinstance(document, dict) or set(document) != fields:
        raise AcceptanceError("M5 Saved Messages resolve result is invalid")
    chat = document["chat"]
    if (
        document["kind"] != "chat"
        or document["message_id"] is not None
        or document["topic"] is not None
        or document["link_type"] != "saved_messages"
        or document["is_public"] is not None
        or not isinstance(chat, dict)
        or set(chat) != {"id", "title", "type", "is_bot", "usernames"}
        or chat["id"] != self_user_id
        or chat["type"] != "private"
        or chat["is_bot"] is not False
        or not isinstance(chat["title"], str)
        or not isinstance(chat["usernames"], list)
        or not all(isinstance(value, str) and value for value in chat["usernames"])
    ):
        raise AcceptanceError("M5 Saved Messages resolve identity is invalid")
    return self_user_id


def _m5_stream_flow(
    runner: Runner, user_identity: Mapping[str, object], result_artifact: Path
) -> None:
    self_user_id = user_identity.get("id")
    if not _int(self_user_id, 1, 9_007_199_254_740_991):
        raise AcceptanceError("M5 current user identity is invalid")
    resolved = runner.run_json(
        "m5-resolve-saved",
        ["--json", "--account", USER_ACCOUNT, "resolve", "t.me/saved"],
    ).document
    chat_id = _saved_messages_chat_id(resolved, self_user_id)

    token = secrets.token_hex(16)
    if re.fullmatch(r"[0-9a-f]{32}", token) is None:
        raise AcceptanceError("M5 CSPRNG token is invalid")
    anchor_text = f"tgcli-m5-anchor-{token}"
    target_text = f"tgcli-m5-target-{token}"
    send_prefix = [
        "--json",
        "--allow-write",
        "--account",
        USER_ACCOUNT,
        "send",
        str(chat_id),
    ]
    anchor = runner.run_json("m5-send-anchor", [*send_prefix, anchor_text]).document
    anchor_chat_id, anchor_message_id = _authoritative_send_ids(anchor)
    _assert_send_result(anchor, anchor_chat_id, anchor_message_id, anchor_text)
    if anchor_chat_id != chat_id or anchor_message_id <= 0:
        raise AcceptanceError("M5 anchor returned invalid Saved Messages ids")

    target = runner.run_json("m5-send-target", [*send_prefix, target_text]).document
    target_chat_id, target_message_id = _authoritative_send_ids(target)
    _assert_send_result(target, target_chat_id, target_message_id, target_text)
    if target_chat_id != chat_id or target_message_id <= anchor_message_id:
        raise AcceptanceError("M5 target did not follow its anchor")

    matched = runner.run_json_clean(
        "m5-wait-for",
        [
            "--json",
            "--timeout",
            "30",
            "--account",
            USER_ACCOUNT,
            "wait-for",
            "--chat",
            str(chat_id),
            "--from",
            str(self_user_id),
            "--after",
            str(anchor_message_id),
            "--regex",
            f"^{target_text}$",
        ],
    ).document
    expected_match = {
        name: value for name, value in target.items() if name != "scheduled"
    }
    if matched != expected_match:
        raise AcceptanceError("M5 wait-for returned the wrong target message")

    write_m5_result_artifact(
        result_artifact,
        M5ResultRecord(
            account=USER_ACCOUNT,
            chat_id=chat_id,
            anchor_message_id=anchor_message_id,
            target_message_id=target_message_id,
            target_prefix=target_text,
        ),
    )


def _assert_m6_storage_stats(document: object) -> None:
    if not isinstance(document, dict) or set(document) != {"size", "count", "by_chat"}:
        raise AcceptanceError("M6 storage stats result has an invalid envelope")
    size = document["size"]
    count = document["count"]
    chats = document["by_chat"]
    if (
        not isinstance(size, int)
        or isinstance(size, bool)
        or size < 0
        or not isinstance(count, int)
        or isinstance(count, bool)
        or not 0 <= count <= 2_147_483_647
        or not isinstance(chats, list)
        or len(chats) > 101
    ):
        raise AcceptanceError("M6 storage stats result has invalid bounds")
    chat_ids: set[int] = set()
    summed_size = 0
    summed_count = 0
    for chat in chats:
        if not isinstance(chat, dict) or set(chat) != {
            "chat_id",
            "size",
            "count",
            "by_file_type",
        }:
            raise AcceptanceError("M6 storage stats chat row is malformed")
        chat_id = chat["chat_id"]
        file_types = chat["by_file_type"]
        if (
            not isinstance(chat_id, int)
            or isinstance(chat_id, bool)
            or not -9_007_199_254_740_991 <= chat_id <= 9_007_199_254_740_991
            or chat_id in chat_ids
            or not isinstance(file_types, list)
            or len(file_types) > 25
        ):
            raise AcceptanceError("M6 storage stats chat identity is invalid")
        chat_ids.add(chat_id)
        child_size = 0
        child_count = 0
        file_type_names: set[str] = set()
        for item in file_types:
            if not isinstance(item, dict) or set(item) != {
                "file_type",
                "size",
                "count",
            }:
                raise AcceptanceError("M6 storage stats file-type row is malformed")
            file_type = item["file_type"]
            item_size = item["size"]
            item_count = item["count"]
            if (
                not isinstance(file_type, str)
                or file_type not in M6_STORAGE_FILE_TYPES
                or file_type in file_type_names
                or not isinstance(item_size, int)
                or isinstance(item_size, bool)
                or item_size < 0
                or not isinstance(item_count, int)
                or isinstance(item_count, bool)
                or not 0 <= item_count <= 2_147_483_647
            ):
                raise AcceptanceError("M6 storage stats file-type values are invalid")
            file_type_names.add(file_type)
            child_size += item_size
            child_count += item_count
        if chat["size"] != child_size or chat["count"] != child_count:
            raise AcceptanceError("M6 storage stats chat sums are inconsistent")
        summed_size += child_size
        summed_count += child_count
    if size != summed_size or count != summed_count:
        raise AcceptanceError("M6 storage stats top-level sums are inconsistent")


def _m6_storage_stats_flow(runner: Runner) -> None:
    arguments = ["--account", USER_ACCOUNT, "storage", "stats"]
    json_result = runner.run_json_clean("m6-storage-stats-json", ["--json", *arguments])
    _assert_m6_storage_stats(json_result.document)
    human_result = runner.run_json_clean("m6-storage-stats-human", arguments)
    _assert_m6_storage_stats(human_result.document)
    if human_result.document != json_result.document:
        raise AcceptanceError("M6 storage stats human and JSON results diverged")


def _m6_control_snapshot(
    environment: Mapping[str, str], account: str
) -> tuple[object, ...]:
    namespace = "tgcli-test" if environment.get("TGCLI_TEST_DC") == "1" else "tgcli"
    home = environment.get("HOME")
    config_base = environment.get("XDG_CONFIG_HOME")
    state_base = environment.get("XDG_STATE_HOME")
    if config_base is None:
        if home is None:
            raise AcceptanceError("HOME is unavailable for M6 control snapshot")
        config_base = str(Path(home) / ".config")
    if state_base is None:
        if home is None:
            raise AcceptanceError("HOME is unavailable for M6 control snapshot")
        state_base = str(Path(home) / ".local" / "state")
    state = Path(state_base) / namespace / "accounts" / account
    sources = [
        Path(config_base) / namespace / "config.toml",
        *(
            state / name
            for name in (
                "audit.log",
                "audit.log.1",
                "audit.log.2",
                "audit.log.3",
                "audit.log.4",
            )
        ),
        state / "idempotency.db",
        state / ".idempotency.db.tmp",
    ]

    def capture(source: Path) -> object:
        try:
            status = source.lstat()
        except FileNotFoundError:
            return None
        if not stat.S_ISREG(status.st_mode) or status.st_uid != os.getuid():
            raise AcceptanceError(f"M6 control file identity is invalid: {source.name}")
        try:
            return (stat.S_IMODE(status.st_mode), source.read_bytes())
        except OSError as error:
            raise AcceptanceError(
                f"M6 control file is unreadable: {source.name}"
            ) from error

    spool = state / "spool"
    spool_entries: list[tuple[str, object]] = []
    if spool.exists():
        if not spool.is_dir() or spool.is_symlink():
            raise AcceptanceError("M6 spool identity is invalid")
        for source in sorted(spool.rglob("*")):
            relative = str(source.relative_to(spool))
            status = source.lstat()
            if stat.S_ISDIR(status.st_mode):
                spool_entries.append(
                    (relative, ("directory", stat.S_IMODE(status.st_mode)))
                )
            elif stat.S_ISREG(status.st_mode) and status.st_uid == os.getuid():
                spool_entries.append(
                    (
                        relative,
                        ("file", stat.S_IMODE(status.st_mode), source.read_bytes()),
                    )
                )
            else:
                raise AcceptanceError("M6 spool contains an invalid entry")
    return (
        *((str(source), capture(source)) for source in sources),
        tuple(spool_entries),
    )


def _assert_m6_session_list(document: object) -> list[str]:
    if not isinstance(document, dict) or set(document) != {
        "items",
        "inactive_session_ttl_days",
        "next",
    }:
        raise AcceptanceError("M6 session list result has an invalid envelope")
    items = document["items"]
    ttl = document["inactive_session_ttl_days"]
    if (
        not isinstance(items, list)
        or not items
        or not isinstance(ttl, int)
        or isinstance(ttl, bool)
        or not 1 <= ttl <= 366
        or document["next"] is not None
    ):
        raise AcceptanceError("M6 session list result has invalid bounds")
    fields = {
        "id",
        "is_current",
        "is_password_pending",
        "is_unconfirmed",
        "can_accept_secret_chats",
        "can_accept_calls",
        "device_type",
        "api_id",
        "application_name",
        "application_version",
        "is_official_application",
        "device_model",
        "platform",
        "system_version",
        "log_in_date",
        "last_active_date",
        "ip_address",
        "location",
    }
    ids: list[str] = []
    current_count = 0
    for item in items:
        if not isinstance(item, dict) or set(item) != fields:
            raise AcceptanceError("M6 session list row is malformed")
        identifier = item["id"]
        if (
            not isinstance(identifier, str)
            or re.fullmatch(r"0|-?[1-9][0-9]{0,18}", identifier) is None
        ):
            raise AcceptanceError("M6 session id is noncanonical")
        parsed = int(identifier)
        if not -(1 << 63) <= parsed <= (1 << 63) - 1 or identifier in ids:
            raise AcceptanceError("M6 session id is out of range or duplicated")
        ids.append(identifier)
        boolean_fields = {
            "is_current",
            "is_password_pending",
            "is_unconfirmed",
            "can_accept_secret_chats",
            "can_accept_calls",
            "is_official_application",
        }
        if any(not isinstance(item[field], bool) for field in boolean_fields):
            raise AcceptanceError("M6 session boolean field is invalid")
        current_count += int(item["is_current"])
        if item["device_type"] not in M6_SESSION_DEVICE_TYPES:
            raise AcceptanceError("M6 session device type is invalid")
        if (
            not isinstance(item["api_id"], int)
            or isinstance(item["api_id"], bool)
            or not -(1 << 31) <= item["api_id"] <= (1 << 31) - 1
        ):
            raise AcceptanceError("M6 session api_id is invalid")
        for field in (
            "application_name",
            "application_version",
            "device_model",
            "platform",
            "system_version",
            "ip_address",
            "location",
        ):
            if not isinstance(item[field], str):
                raise AcceptanceError("M6 session text field is invalid")
        for field in ("log_in_date", "last_active_date"):
            value = item[field]
            if value is not None and (
                not isinstance(value, str)
                or re.fullmatch(
                    r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z",
                    value,
                )
                is None
            ):
                raise AcceptanceError("M6 session timestamp is invalid")
    if current_count != 1:
        raise AcceptanceError(
            "M6 session list must contain exactly one current session"
        )
    return ids


def _m6_session_list_flow(runner: Runner, environment: Mapping[str, str]) -> None:
    arguments = ["--json", "--account", USER_ACCOUNT, "session", "list"]
    before = _m6_control_snapshot(environment, USER_ACCOUNT)
    first = runner.run_json_clean("m6-session-list-daemon-1", arguments)
    first_ids = _assert_m6_session_list(first.document)
    second = runner.run_json_clean("m6-session-list-daemon-2", arguments)
    if _assert_m6_session_list(second.document) != first_ids:
        raise AcceptanceError(
            "M6 session list order changed across an unchanged vector"
        )
    if _m6_control_snapshot(environment, USER_ACCOUNT) != before:
        raise AcceptanceError(
            "M6 session list mutated config, audit, idempotency or spool state"
        )
    runner.stop_daemon_verified(USER_ACCOUNT)
    no_daemon = runner.run_json_clean(
        "m6-session-list-no-daemon", ["--no-daemon", *arguments]
    )
    _assert_m6_session_list(no_daemon.document)


def _smoke(
    runner: Runner,
    environment: Mapping[str, str],
    fixtures: Mapping[str, bytes],
    sources: Mapping[str, Path],
    qr_approver: Path | None,
    m3_key_sentinels: list[bytes],
    m5_result_artifact: Path,
) -> set[str]:
    executed: set[str] = set()
    accounts = [USER_ACCOUNT]
    if "bot_token" in sources:
        accounts.append(BOT_ACCOUNT)
    if qr_approver is not None:
        accounts.append(QR_ACCOUNT)
    for account in accounts:
        result = runner.run_json(
            f"account-add-{account}", ["--json", "account", "add", account]
        )
        _assert_account_add(result.document, account, account == USER_ACCOUNT)
    _assert_account_persistence(runner, environment, accounts)
    _write_config(environment, sources, accounts)
    for account in accounts:
        runner.start_daemon(account)

    user_arguments = ["--json", "-v", "--account", USER_ACCOUNT, "login"]
    work: dict[str, concurrent.futures.Future[CommandResult]] = {}
    concurrent_launch = (
        ConcurrentLaunch(2, min(runner.timeout, 10))
        if BOT_ACCOUNT in accounts
        else None
    )
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        work[USER_ACCOUNT] = executor.submit(
            runner.run_interactive,
            "login-user",
            user_arguments,
            fixtures,
            None,
            concurrent_launch,
        )
        if BOT_ACCOUNT in accounts:
            work[BOT_ACCOUNT] = executor.submit(
                runner.run_interactive,
                "login-bot",
                ["--json", "-v", "--account", BOT_ACCOUNT, "login", "--bot"],
                {},
                None,
                concurrent_launch,
            )
        results = {account: future.result() for account, future in work.items()}
    if concurrent_launch is not None and not concurrent_launch.overlap_proven:
        raise AcceptanceError("concurrent authentication overlap was not proven")

    _require_phone_code_flow(results[USER_ACCOUNT], "initial login")
    user_identity = _assert_login(results[USER_ACCOUNT].document, USER_ACCOUNT)
    executed.update(_observed_auth_state_tests(results[USER_ACCOUNT]))
    executed.add(auth_state_test_id("wait_tdlib_parameters"))
    executed.add(auth_state_test_id("ready"))
    if BOT_ACCOUNT in results:
        bot_identity = _assert_login(results[BOT_ACCOUNT].document, BOT_ACCOUNT)
        _assert_bot_identity(bot_identity)
        executed.add(M1_BOT_TEST)
    me = runner.run_json(
        "me-user", ["--json", "--account", USER_ACCOUNT, "me"]
    ).document
    if me != user_identity:
        raise AcceptanceError("me identity differs from login identity")
    _assert_saved_tags(
        runner.run_json(
            "saved-tags-user",
            ["--json", "--account", USER_ACCOUNT, "saved", "tags"],
        ).document
    )
    _assert_chats(
        runner.run_json(
            "chats-user",
            ["--json", "--account", USER_ACCOUNT, "chats", "-n", "1"],
        ).document
    )

    logout = runner.run_json(
        "logout-user",
        [
            "--json",
            "-v",
            "--allow-write",
            "--yes",
            "--account",
            USER_ACCOUNT,
            "logout",
        ],
    ).document
    if logout != {"account": USER_ACCOUNT, "logged_out": True}:
        raise AcceptanceError("logout did not report correlated Closed completion")
    executed.add(auth_state_test_id("closed"))
    relogin = runner.run_interactive("relogin-user", user_arguments, fixtures)
    _require_phone_code_flow(relogin, "re-login")
    relogin_identity = _assert_login(relogin.document, USER_ACCOUNT)
    executed.update(_observed_auth_state_tests(relogin))
    if relogin_identity.get("id") != user_identity.get("id"):
        raise AcceptanceError("re-login returned a different identity")
    if (
        runner.run_json(
            "me-user-after-relogin", ["--json", "--account", USER_ACCOUNT, "me"]
        ).document
        != relogin_identity
    ):
        raise AcceptanceError("me after re-login differs from login identity")

    _m3_write_flow(runner, m3_key_sentinels)
    _m5_stream_flow(runner, relogin_identity, m5_result_artifact)
    _m6_storage_stats_flow(runner)

    if QR_ACCOUNT in accounts:
        qr = runner.run_interactive(
            "login-qr",
            ["--json", "-v", "--account", QR_ACCOUNT, "login", "--qr"],
            {},
            runner.approve_qr,
        )
        _assert_login(qr.document, QR_ACCOUNT)
        _require_qr_approval(runner)
        executed.add(M1_QR_TEST)
        executed.add(auth_state_test_id("wait_other_device_confirmation"))
    _m6_session_list_flow(runner, environment)
    return executed


def _skip_entries(
    arguments: argparse.Namespace, sources: Mapping[str, Path]
) -> list[SkipEntry]:
    entries: list[SkipEntry] = []
    if arguments.qr_approver is None:
        entries.append(SkipEntry(M1_QR_TEST, "fixture_missing:qr_approver"))
    if "bot_token" not in sources:
        entries.append(SkipEntry(M1_BOT_TEST, "fixture_missing:bot_token_cmd"))
    for state in arguments.unforceable_state:
        if state not in PINNED_AUTH_STATES:
            raise AcceptanceError(f"unknown pinned authorization state: {state}")
        entries.append(
            SkipEntry(auth_state_test_id(state), f"test_dc_state_not_forceable:{state}")
        )
    return entries


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the tgcli M1-M5 mandatory acceptance flows on test DC"
    )
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--qr-approver", type=Path)
    parser.add_argument("--unforceable-state", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--preflight-only", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    os.umask(0o077)
    artifact = arguments.build_dir / "test-results" / "tgcli-test-dc-skips.json"
    m5_result_artifact = arguments.build_dir / "test-results" / "tgcli-test-dc-m5.json"
    entries: list[SkipEntry] = []
    failure: BaseException | None = None
    runner: Runner | None = None
    cleanup_failure: BaseException | None = None
    traps: dict[Path, bytes] = {}
    scan_inputs: tuple[list[FrozenSentinel], list[Path]] | None = None
    run_root: Path | None = None
    m3_key_sentinels: list[bytes] = []
    try:
        if arguments.timeout <= 0:
            raise AcceptanceError("timeout must be positive")
        fixtures, sources = _load_fixtures(arguments.fixture_dir)
        entries = _skip_entries(arguments, sources)
        run_parent = arguments.build_dir / "test-results" / "test-dc-private"
        _private_directory(run_parent)
        run_root = run_parent / f"run-{os.getpid()}"
        run_root.mkdir(mode=0o700)
        environment, traps = _isolated_environment(run_root)
        _surface_preflight(arguments.binary, environment, arguments.timeout)
        _approver_preflight(arguments.qr_approver)
        if not arguments.preflight_only:
            captures = Captures(run_root / "captures")
            runner = Runner(
                arguments.binary,
                environment,
                fixtures,
                captures,
                arguments.qr_approver,
                arguments.timeout,
            )
            accounts = [USER_ACCOUNT]
            if "bot_token" in sources:
                accounts.append(BOT_ACCOUNT)
            if arguments.qr_approver is not None:
                accounts.append(QR_ACCOUNT)
            sentinel_names = (
                "authentication_code",
                "email_code",
                "password",
                "database_key",
                "bot_token",
            )
            scan_inputs = (
                freeze_auth_sentinels(
                    [sources[name] for name in sentinel_names if name in sources]
                ),
                [_state_log_directory(environment, account) for account in accounts],
            )
            executed = _smoke(
                runner,
                environment,
                fixtures,
                sources,
                arguments.qr_approver,
                m3_key_sentinels,
                m5_result_artifact,
            )
            validate_m1_coverage(executed, entries)
    except (AcceptanceError, ContractError, OSError) as error:
        if isinstance(error, CleanupError):
            cleanup_failure = error
        failure = error
    finally:
        if runner is not None:
            try:
                runner.cleanup_messages()
            except (AcceptanceError, OSError) as error:
                cleanup_failure = error
                failure = error
        if runner is not None:
            try:
                runner.stop_daemons()
            except (AcceptanceError, OSError) as error:
                failure = failure or error
        if runner is not None and scan_inputs is not None:
            try:
                sentinels, log_directories = scan_inputs
                scan_frozen_auth_sentinels(
                    sentinels, runner.captures.stderr_files, log_directories
                )
            except ContractError as error:
                failure = error
        if runner is not None and run_root is not None and m3_key_sentinels:
            try:
                _scan_m3_key_artifacts(run_root, runner.captures, m3_key_sentinels)
            except AcceptanceError as error:
                failure = error
        if traps:
            try:
                _verify_production_traps(traps)
            except AcceptanceError as error:
                failure = error
        try:
            write_skip_artifact(artifact, entries)
        except ContractError as error:
            failure = error
        if cleanup_failure is not None:
            failure = cleanup_failure
    if failure is not None:
        print(f"test-DC acceptance failure: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
