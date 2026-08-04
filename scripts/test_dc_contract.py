#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

MAX_CONTRACT_FILE_BYTES = 1024 * 1024
MAX_SENTINEL_BYTES = 64 * 1024
PINNED_AUTH_STATES = frozenset(
    {
        "wait_tdlib_parameters",
        "wait_phone_number",
        "wait_premium_purchase",
        "wait_email_address",
        "wait_email_code",
        "wait_code",
        "wait_other_device_confirmation",
        "wait_registration",
        "wait_password",
        "ready",
        "logging_out",
        "closing",
        "closed",
    }
)
FIXTURE_REASONS = frozenset(
    {"fixture_missing:qr_approver", "fixture_missing:bot_token_cmd"}
)


class ContractError(RuntimeError):
    pass


@dataclass(frozen=True, order=True)
class SkipEntry:
    test: str
    reason: str


@dataclass(frozen=True, repr=False)
class FrozenSentinel:
    source: Path
    value: bytes
    device: int
    inode: int
    size: int
    ctime_ns: int


def validate_skip(entry: SkipEntry) -> None:
    if not entry.test:
        raise ContractError("skip test name must be non-empty")
    if entry.reason in FIXTURE_REASONS:
        return
    prefix = "test_dc_state_not_forceable:"
    if not entry.reason.startswith(prefix):
        raise ContractError("skip reason is outside the M1 closed enum")
    state = entry.reason[len(prefix) :]
    if state not in PINNED_AUTH_STATES:
        raise ContractError("skip reason names an unknown authorization state")


def canonical_skips(entries: Iterable[SkipEntry]) -> list[SkipEntry]:
    result = list(entries)
    for entry in result:
        validate_skip(entry)
    result.sort()
    if len(set(result)) != len(result):
        raise ContractError("duplicate skip entries are not permitted")
    return result


def _open_private_replacement(target: Path) -> tuple[int, Path]:
    target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.{os.getpid()}.tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(temporary, flags, 0o600)
    except OSError as error:
        raise ContractError(
            f"cannot create artifact replacement: {error.strerror}"
        ) from error
    return descriptor, temporary


def write_skip_artifact(target: Path, entries: Iterable[SkipEntry]) -> None:
    skips = canonical_skips(entries)
    document = {
        "skips": [{"test": entry.test, "reason": entry.reason} for entry in skips]
    }
    payload = (
        json.dumps(document, ensure_ascii=False, separators=(",", ":")) + "\n"
    ).encode()
    descriptor, temporary = _open_private_replacement(target)
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, target)
        directory = os.open(target.parent, os.O_RDONLY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise ContractError(
            f"cannot publish skip artifact: {error.strerror}"
        ) from error


def _open_regular(source: Path, label: str) -> tuple[int, os.stat_result]:
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(source, flags)
        status = os.fstat(descriptor)
    except OSError as error:
        if "descriptor" in locals():
            os.close(descriptor)
        raise ContractError(f"{label} is unavailable: {source.name}") from error
    if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
        os.close(descriptor)
        raise ContractError(
            f"{label} must be one regular non-symlink file: {source.name}"
        )
    return descriptor, status


def _identity(status: os.stat_result) -> tuple[int, int, int, int]:
    return (status.st_dev, status.st_ino, status.st_size, status.st_ctime_ns)


def load_skip_artifact(source: Path) -> list[SkipEntry]:
    descriptor, status = _open_regular(source, "skip artifact")
    if status.st_size > MAX_CONTRACT_FILE_BYTES:
        os.close(descriptor)
        raise ContractError("skip artifact exceeds one MiB")
    try:
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            payload = stream.read(MAX_CONTRACT_FILE_BYTES + 1)
    except OSError as error:
        raise ContractError("skip artifact is not valid UTF-8 JSON") from error
    if len(payload) > MAX_CONTRACT_FILE_BYTES:
        raise ContractError("skip artifact exceeds one MiB")
    try:
        document = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ContractError("skip artifact is not valid UTF-8 JSON") from error
    if not isinstance(document, dict) or set(document) != {"skips"}:
        raise ContractError("skip artifact must contain exactly the skips field")
    if not isinstance(document["skips"], list):
        raise ContractError("skip artifact skips field must be an array")
    entries: list[SkipEntry] = []
    for item in document["skips"]:
        if not isinstance(item, dict) or set(item) != {"test", "reason"}:
            raise ContractError("each skip must contain exactly test and reason")
        if not isinstance(item["test"], str) or not isinstance(item["reason"], str):
            raise ContractError("skip test and reason must be strings")
        entries.append(SkipEntry(item["test"], item["reason"]))
    canonical = canonical_skips(entries)
    if entries != canonical:
        raise ContractError("skip entries must be sorted by test then reason")
    return entries


def _read_secret_fixture_with_status(source: Path) -> tuple[bytes, os.stat_result]:
    descriptor, status = _open_regular(source, "fixture")
    if status.st_uid != os.getuid() or status.st_mode & 0o077:
        os.close(descriptor)
        raise ContractError(f"fixture permissions are not private: {source.name}")
    if status.st_size == 0 or status.st_size > MAX_SENTINEL_BYTES:
        os.close(descriptor)
        raise ContractError(f"fixture size is invalid: {source.name}")
    try:
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            value = stream.read(MAX_SENTINEL_BYTES + 1)
    except OSError as error:
        raise ContractError(f"cannot read fixture: {source.name}") from error
    if len(value) > MAX_SENTINEL_BYTES:
        raise ContractError(f"fixture size is invalid: {source.name}")
    if value.endswith(b"\r\n"):
        value = value[:-2]
    elif value.endswith(b"\n"):
        value = value[:-1]
    if not value or b"\x00" in value or b"\n" in value or b"\r" in value:
        raise ContractError(
            f"fixture must contain exactly one non-empty value: {source.name}"
        )
    return value, status


def read_secret_fixture(source: Path) -> bytes:
    value, _ = _read_secret_fixture_with_status(source)
    return value


def freeze_auth_sentinels(sentinel_files: Sequence[Path]) -> list[FrozenSentinel]:
    if not sentinel_files:
        raise ContractError("at least one sentinel fixture is required")
    frozen: list[FrozenSentinel] = []
    for source in sentinel_files:
        value, status = _read_secret_fixture_with_status(source)
        frozen.append(FrozenSentinel(source, value, *_identity(status)))
    if len({sentinel.value for sentinel in frozen}) != len(frozen):
        raise ContractError("sentinel fixtures must have distinct byte strings")
    return frozen


def _verify_frozen_sentinels(sentinels: Sequence[FrozenSentinel]) -> None:
    for sentinel in sentinels:
        value, status = _read_secret_fixture_with_status(sentinel.source)
        if _identity(status) != (
            sentinel.device,
            sentinel.inode,
            sentinel.size,
            sentinel.ctime_ns,
        ):
            raise ContractError(
                f"sentinel fixture identity changed: {sentinel.source.name}"
            )
        if value != sentinel.value:
            raise ContractError(
                f"sentinel fixture content changed: {sentinel.source.name}"
            )


def _regular_scan_source(source: Path) -> None:
    descriptor, _ = _open_regular(source, "scan source")
    os.close(descriptor)


def tdlib_log_sources(log_directory: Path) -> list[Path]:
    active = log_directory / "tdlib.log"
    _regular_scan_source(active)
    try:
        entries = list(log_directory.iterdir())
    except OSError as error:
        raise ContractError("cannot enumerate the TDLib log directory") from error
    sources = [
        entry
        for entry in entries
        if entry.name == "tdlib.log" or entry.name.startswith("tdlib.log.")
    ]
    sources.sort(key=lambda entry: os.fsencode(entry.name))
    for source in sources:
        _regular_scan_source(source)
    return sources


def _contains_any(source: Path, sentinels: Sequence[bytes]) -> bool:
    overlap = max(len(value) for value in sentinels) - 1
    previous = b""
    descriptor, _ = _open_regular(source, "scan source")
    try:
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            while chunk := stream.read(64 * 1024):
                window = previous + chunk
                if any(value in window for value in sentinels):
                    return True
                previous = window[-overlap:] if overlap > 0 else b""
    except OSError as error:
        raise ContractError(f"cannot scan source: {source}") from error
    return False


def scan_frozen_auth_sentinels(
    sentinels: Sequence[FrozenSentinel],
    stderr_files: Sequence[Path],
    log_directories: Sequence[Path],
) -> None:
    if not sentinels:
        raise ContractError("at least one sentinel fixture is required")
    _verify_frozen_sentinels(sentinels)
    values = [sentinel.value for sentinel in sentinels]
    sources: list[Path] = []
    for source in stderr_files:
        _regular_scan_source(source)
        sources.append(source)
    if not sources:
        raise ContractError("at least one captured stderr file is required")
    if not log_directories:
        raise ContractError("at least one TDLib log directory is required")
    for directory in log_directories:
        sources.extend(tdlib_log_sources(directory))
    for source in sources:
        if _contains_any(source, values):
            raise ContractError(f"authentication sentinel found in {source.name}")


def scan_auth_sentinels(
    sentinel_files: Sequence[Path],
    stderr_files: Sequence[Path],
    log_directories: Sequence[Path],
) -> None:
    scan_frozen_auth_sentinels(
        freeze_auth_sentinels(sentinel_files), stderr_files, log_directories
    )


def parse_skip_argument(value: str) -> SkipEntry:
    if "=" not in value:
        raise ContractError("--skip must use TEST=REASON")
    test, reason = value.split("=", 1)
    return SkipEntry(test, reason)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate the tgcli test-DC acceptance contract"
    )
    commands = parser.add_subparsers(dest="command", required=True)

    write = commands.add_parser("write-skips")
    write.add_argument("--output", type=Path, required=True)
    write.add_argument("--skip", action="append", default=[])

    validate = commands.add_parser("validate-skips")
    validate.add_argument("artifact", type=Path)

    scan = commands.add_parser("scan-sentinels")
    scan.add_argument("--sentinel-file", action="append", type=Path, required=True)
    scan.add_argument("--stderr-file", action="append", type=Path, required=True)
    scan.add_argument("--tdlib-log-dir", action="append", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        if arguments.command == "write-skips":
            write_skip_artifact(
                arguments.output,
                [parse_skip_argument(value) for value in arguments.skip],
            )
        elif arguments.command == "validate-skips":
            load_skip_artifact(arguments.artifact)
        elif arguments.command == "scan-sentinels":
            scan_auth_sentinels(
                arguments.sentinel_file, arguments.stderr_file, arguments.tdlib_log_dir
            )
        else:
            raise ContractError("unknown test-DC contract command")
    except ContractError as error:
        print(f"test-DC contract failure: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
