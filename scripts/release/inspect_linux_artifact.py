#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys


class InspectionError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InspectionError(message)


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_inspector(value: str, name: str) -> str:
    if "/" in value:
        candidate = pathlib.Path(value)
        require(
            candidate.is_file() and not candidate.is_symlink(),
            f"required inspector is missing or unsafe: {name}",
        )
        return str(candidate)
    resolved = shutil.which(value)
    require(resolved is not None, f"required inspector is missing: {name}")
    return resolved


def run_inspector(
    executable: str, arguments: list[str], working_directory: pathlib.Path
) -> dict:
    command = [executable, *arguments]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            cwd=working_directory,
            check=False,
        )
    except OSError as error:
        raise InspectionError(
            f"cannot execute inspector {arguments[0]}: {error}"
        ) from error
    require(
        completed.returncode == 0,
        f"inspector {pathlib.Path(executable).name} failed with exit code "
        f"{completed.returncode}",
    )
    try:
        stdout = completed.stdout.decode("utf-8", errors="strict")
        stderr = completed.stderr.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise InspectionError("inspector output is not valid UTF-8") from error
    require(not stderr, f"inspector {pathlib.Path(executable).name} wrote to stderr")
    return {
        "argv": [pathlib.Path(executable).name, *arguments],
        "stdout": stdout,
        "stdout_sha256": hashlib.sha256(completed.stdout).hexdigest(),
        "stdout_size": len(completed.stdout),
    }


def inspect(args: argparse.Namespace) -> None:
    artifact = args.artifact.resolve()
    output = args.output.resolve()
    require(artifact.is_file(), f"release artifact is missing: {artifact}")
    require(
        not artifact.is_symlink(), f"release artifact cannot be a symlink: {artifact}"
    )
    require(
        not output.exists() and not output.is_symlink(),
        f"inspection output already exists: {output}",
    )
    file_command = resolve_inspector(args.file_command, "file")
    readelf_command = resolve_inspector(args.readelf_command, "readelf")
    before_sha256 = sha256_file(artifact)
    before_size = artifact.stat().st_size
    working_directory = artifact.parent
    artifact_name = artifact.name

    file_result = run_inspector(file_command, [artifact_name], working_directory)
    program_headers = run_inspector(
        readelf_command, ["-lW", artifact_name], working_directory
    )
    dynamic_section = run_inspector(
        readelf_command, ["-dW", artifact_name], working_directory
    )
    version_info = run_inspector(
        readelf_command, ["--version-info", artifact_name], working_directory
    )

    require(
        re.search(r"ELF.*(statically linked|static-pie linked)", file_result["stdout"])
        is not None,
        "Linux release artifact is not a static ELF",
    )
    require(
        " INTERP " not in program_headers["stdout"],
        "Linux release artifact contains an ELF interpreter",
    )
    require(
        "(NEEDED)" not in dynamic_section["stdout"],
        "Linux release artifact contains a dynamic dependency",
    )
    require(
        re.search(r"GLIBC(_|XX_)", version_info["stdout"]) is None,
        "Linux release artifact contains a glibc symbol-version reference",
    )
    require(
        artifact.stat().st_size == before_size
        and sha256_file(artifact) == before_sha256,
        "release artifact changed during inspection",
    )

    document = {
        "artifact": {
            "path": artifact_name,
            "sha256": before_sha256,
            "size": before_size,
        },
        "checks": {
            "dynamic_dependencies_absent": True,
            "elf_interpreter_absent": True,
            "glibc_symbol_versions_absent": True,
            "static_elf": True,
        },
        "commands": [file_result, program_headers, dynamic_section, version_info],
        "schema_version": 1,
    }
    output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Inspect a Linux release artifact")
    parser.add_argument("--artifact", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--file-command", default="file")
    parser.add_argument("--readelf-command", default="readelf")
    return parser.parse_args()


def main() -> int:
    try:
        inspect(parse_args())
    except (InspectionError, OSError, json.JSONDecodeError) as error:
        print(f"artifact inspection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
