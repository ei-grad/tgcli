#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pty
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
    SkipEntry,
    auth_state_test_id,
    freeze_auth_sentinels,
    read_secret_fixture,
    scan_frozen_auth_sentinels,
    validate_m1_coverage,
    write_skip_artifact,
)

USER_ACCOUNT = "test-user"
BOT_ACCOUNT = "test-bot"
QR_ACCOUNT = "test-qr"
REQUIRED_SURFACE = ("logout", "--allow-write", "--yes", "--verbose", "-v")
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


class AcceptanceError(RuntimeError):
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
        self.stderr_files: list[Path] = []
        self._sequence = 0
        self._lock = threading.Lock()

    def allocate(self, label: str) -> tuple[Path, Path]:
        with self._lock:
            sequence = self._sequence
            self._sequence += 1
            stdout = self.directory / f"{sequence:03d}-{label}.stdout"
            stderr = self.directory / f"{sequence:03d}-{label}.stderr"
            self.stderr_files.append(stderr)
        return stdout, stderr


@dataclass
class CommandResult:
    document: object
    observed_prompts: frozenset[str]


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
        self.qr_approvals = 0

    def _argv(self, arguments: Sequence[str]) -> list[str]:
        return [str(self.binary), *arguments]

    def run_json(self, label: str, arguments: Sequence[str]) -> CommandResult:
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
        if completed.returncode != 0:
            raise AcceptanceError(f"command returned non-zero: {label}")
        return CommandResult(_load_json(stdout_path, label), frozenset())

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


def _load_json(source: Path, label: str) -> object:
    try:
        return json.loads(source.read_bytes())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AcceptanceError(
            f"command did not return one JSON document: {label}"
        ) from error


def _surface_preflight(
    binary: Path, environment: Mapping[str, str], timeout: float
) -> None:
    try:
        status = binary.lstat()
    except OSError as error:
        raise AcceptanceError("tgcli binary is unavailable") from error
    if not stat.S_ISREG(status.st_mode) or not os.access(binary, os.X_OK):
        raise AcceptanceError("tgcli binary must be an executable regular file")
    try:
        completed = subprocess.run(
            [str(binary), "--help"],
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
    missing = [token for token in REQUIRED_SURFACE[1:] if token not in option_tokens]
    if REQUIRED_SURFACE[0] not in subcommands:
        missing.append(REQUIRED_SURFACE[0])
    if completed.returncode != 0 or missing:
        raise AcceptanceError("tgcli is missing the M1 test-DC runtime surface")


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


def _smoke(
    runner: Runner,
    environment: Mapping[str, str],
    fixtures: Mapping[str, bytes],
    sources: Mapping[str, Path],
    qr_approver: Path | None,
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
        description="Run the tgcli M1 acceptance flow on test DC"
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
    entries: list[SkipEntry] = []
    failure: BaseException | None = None
    runner: Runner | None = None
    traps: dict[Path, bytes] = {}
    scan_inputs: tuple[list[FrozenSentinel], list[Path]] | None = None
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
            )
            validate_m1_coverage(executed, entries)
    except (AcceptanceError, ContractError, OSError) as error:
        failure = error
    finally:
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
        if traps:
            try:
                _verify_production_traps(traps)
            except AcceptanceError as error:
                failure = error
        try:
            write_skip_artifact(artifact, entries)
        except ContractError as error:
            failure = error
    if failure is not None:
        print(f"test-DC acceptance failure: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
