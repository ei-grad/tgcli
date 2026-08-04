#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import stat
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

ENVIRONMENT_FIXTURES = {
    "TGCLI_TEST_DC_PHONE_NUMBER": ("phone_number", True),
    "TGCLI_TEST_DC_AUTHENTICATION_CODE": ("authentication_code", True),
    "TGCLI_TEST_DC_REGISTRATION_FIRST_NAME": ("registration_first_name", False),
    "TGCLI_TEST_DC_REGISTRATION_LAST_NAME": ("registration_last_name", False),
    "TGCLI_TEST_DC_EMAIL_ADDRESS": ("email_address", False),
    "TGCLI_TEST_DC_EMAIL_CODE": ("email_code", False),
    "TGCLI_TEST_DC_PASSWORD": ("password", False),
    "TGCLI_TEST_DC_DATABASE_KEY": ("database_key", True),
    "TGCLI_TEST_DC_BOT_TOKEN": ("bot_token", False),
}


class FixtureError(RuntimeError):
    pass


def _validate_value(name: str, value: str) -> bytes:
    encoded = value.encode()
    if not encoded or b"\x00" in encoded or b"\r" in encoded or b"\n" in encoded:
        raise FixtureError(f"fixture {name} must contain exactly one non-empty value")
    if len(encoded) > 64 * 1024:
        raise FixtureError(f"fixture {name} exceeds 64 KiB")
    return encoded


def _open_private_directory(directory: Path) -> int:
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(directory, flags)
        status = os.fstat(descriptor)
    except OSError as error:
        if "descriptor" in locals():
            os.close(descriptor)
        raise FixtureError("cannot open fixture directory") from error
    if (
        not stat.S_ISDIR(status.st_mode)
        or status.st_uid != os.getuid()
        or status.st_mode & 0o077
    ):
        os.close(descriptor)
        raise FixtureError("fixture directory must be current-uid mode 0700")
    return descriptor


def materialize(output: Path, environment: Mapping[str, str]) -> list[str]:
    fixtures: list[tuple[str, bytes]] = []
    for variable, (filename, required) in ENVIRONMENT_FIXTURES.items():
        value = environment.get(variable)
        if value is None or value == "":
            if required:
                raise FixtureError(f"required fixture is unavailable: {filename}")
            continue
        fixtures.append((filename, _validate_value(filename, value)))

    directory = _open_private_directory(output)
    written: list[str] = []
    try:
        if os.listdir(directory):
            raise FixtureError("fixture directory must be empty")
        for filename, payload in fixtures:
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            try:
                descriptor = os.open(filename, flags, 0o600, dir_fd=directory)
                with os.fdopen(descriptor, "wb", closefd=True) as fixture:
                    fixture.write(payload)
                    fixture.flush()
                    os.fsync(fixture.fileno())
            except OSError as error:
                raise FixtureError(f"cannot create fixture file: {filename}") from error
            written.append(filename)
        os.fsync(directory)
    finally:
        os.close(directory)
    return written


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Materialize private test-DC fixtures without printing their values"
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        materialize(arguments.output, os.environ)
    except FixtureError as error:
        print(f"test-DC fixture failure: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
