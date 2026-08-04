#!/usr/bin/env python3

import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tarfile
import tempfile
from unittest import mock

REPO_ROOT = pathlib.Path(sys.argv[1]).resolve()
ARCHIVE_TOOL = REPO_ROOT / "scripts/release/archive_tool.py"
INSPECTOR = REPO_ROOT / "scripts/release/inspect_linux_artifact.py"
PROVENANCE = REPO_ROOT / "scripts/release/build_provenance.py"


def run(arguments: list[str], *, expected: int = 0) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        arguments,
        capture_output=True,
        check=False,
        text=True,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"unexpected exit {completed.returncode} for {arguments}:\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def expect_failure(arguments: list[str], diagnostic: str) -> None:
    completed = run(arguments, expected=1)
    if diagnostic not in completed.stderr:
        raise AssertionError(f"missing diagnostic {diagnostic!r}:\n{completed.stderr}")


def write_json(file: pathlib.Path, document: dict) -> None:
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def sha256(file: pathlib.Path) -> str:
    return hashlib.sha256(file.read_bytes()).hexdigest()


def initialize_source_repo(root: pathlib.Path) -> str:
    run(["git", "init", "--quiet", str(root)])
    run(["git", "-C", str(root), "config", "user.name", "tgcli test"])
    run(["git", "-C", str(root), "config", "user.email", "tgcli@example.invalid"])
    (root / ".gitignore").write_text("build/\n", encoding="utf-8")
    (root / "tracked.txt").write_text("locked\n", encoding="utf-8")
    run(["git", "-C", str(root), "add", ".gitignore", "tracked.txt"])
    run(["git", "-C", str(root), "commit", "--quiet", "-m", "fixture"])
    return run(["git", "-C", str(root), "rev-parse", "HEAD"]).stdout.strip()


def source_identity_tests(base: pathlib.Path) -> None:
    source = base / "source"
    source.mkdir()
    commit = initialize_source_repo(source)
    command = [
        sys.executable,
        str(PROVENANCE),
        "source-identity",
        "--repo-root",
        str(source),
        "--expected-commit",
        commit,
    ]
    run(command)

    (source / "tracked.txt").write_text("mutated\n", encoding="utf-8")
    expect_failure(command, "tracked worktree changes")
    (source / "tracked.txt").write_text("locked\n", encoding="utf-8")

    (source / "tracked.txt").write_text("staged\n", encoding="utf-8")
    run(["git", "-C", str(source), "add", "tracked.txt"])
    expect_failure(command, "staged changes")
    run(["git", "-C", str(source), "reset", "--quiet", "HEAD", "--", "tracked.txt"])
    (source / "tracked.txt").write_text("locked\n", encoding="utf-8")

    relevant = source / "src/untracked.cpp"
    relevant.parent.mkdir()
    relevant.write_text("int untracked;\n", encoding="utf-8")
    expect_failure(command, "untracked files")
    relevant.unlink()
    relevant.parent.rmdir()
    run(command)

    no_git = base / "no-git"
    no_git.mkdir()
    expect_failure(
        [
            sys.executable,
            str(PROVENANCE),
            "source-identity",
            "--repo-root",
            str(no_git),
            "--expected-commit",
            commit,
        ],
        "no trustworthy Git identity",
    )


def inspection_failure_tests(base: pathlib.Path) -> None:
    artifact = base / "artifact"
    artifact.write_bytes(b"not an ELF")
    expect_failure(
        [
            sys.executable,
            str(INSPECTOR),
            "--artifact",
            str(artifact),
            "--output",
            str(base / "missing-inspector.json"),
            "--file-command",
            str(base / "missing-file-command"),
        ],
        "required inspector is missing",
    )

    fake_file = base / "fake-file"
    fake_file.write_text(
        "#!/bin/sh\necho 'artifact: ELF statically linked'\n", encoding="utf-8"
    )
    fake_file.chmod(0o755)
    failing_readelf = base / "failing-readelf"
    failing_readelf.write_text("#!/bin/sh\nexit 7\n", encoding="utf-8")
    failing_readelf.chmod(0o755)
    expect_failure(
        [
            sys.executable,
            str(INSPECTOR),
            "--artifact",
            str(artifact),
            "--output",
            str(base / "failing-inspector.json"),
            "--file-command",
            str(fake_file),
            "--readelf-command",
            str(failing_readelf),
        ],
        "inspector failing-readelf failed with exit code 7",
    )


def add_regular(archive: tarfile.TarFile, name: str, content: bytes) -> None:
    member = tarfile.TarInfo(name)
    member.size = len(content)
    member.mode = 0o644
    archive.addfile(member, io.BytesIO(content))


def archive_failure_tests(base: pathlib.Path) -> None:
    cases: list[tuple[str, str, int, int, int]] = []

    traversal = base / "traversal.tar.gz"
    with tarfile.open(traversal, "w:gz") as archive:
        add_regular(archive, "../escape", b"escape")
    cases.append(("traversal", "unsafe archive member", 10, 100, 100))

    members = base / "members.tar.gz"
    with tarfile.open(members, "w:gz") as archive:
        add_regular(archive, "root/one", b"1")
        add_regular(archive, "root/two", b"2")
    cases.append(("members", "member count exceeds", 1, 100, 100))

    per_file = base / "per-file.tar.gz"
    with tarfile.open(per_file, "w:gz") as archive:
        add_regular(archive, "root/file", b"12345")
    cases.append(("per-file", "per-file safety cap", 10, 4, 100))

    cumulative = base / "cumulative.tar.gz"
    with tarfile.open(cumulative, "w:gz") as archive:
        add_regular(archive, "root/one", b"123")
        add_regular(archive, "root/two", b"456")
    cases.append(("cumulative", "expanded size exceeds", 10, 10, 5))

    special = base / "special.tar.gz"
    with tarfile.open(special, "w:gz") as archive:
        member = tarfile.TarInfo("root/fifo")
        member.type = tarfile.FIFOTYPE
        archive.addfile(member)
    cases.append(("special", "unsupported archive member type", 10, 100, 100))

    symlink = base / "symlink.tar.gz"
    with tarfile.open(symlink, "w:gz") as archive:
        member = tarfile.TarInfo("root/link")
        member.type = tarfile.SYMTYPE
        member.linkname = "../../escape"
        archive.addfile(member)
    cases.append(("symlink", "escaping archive link", 10, 100, 100))

    for name, diagnostic, max_members, max_member, max_expanded in cases:
        expect_failure(
            [
                sys.executable,
                str(ARCHIVE_TOOL),
                "extract",
                "--archive",
                str(base / f"{name}.tar.gz"),
                "--destination",
                str(base / f"{name}-output"),
                "--max-members",
                str(max_members),
                "--max-member-size",
                str(max_member),
                "--max-expanded-size",
                str(max_expanded),
            ],
            diagnostic,
        )
    if (base / "escape").exists():
        raise AssertionError("unsafe archive escaped the extraction root")


def import_script(name: str, file: pathlib.Path):
    specification = importlib.util.spec_from_file_location(name, file)
    if specification is None or specification.loader is None:
        raise AssertionError(f"cannot import {file}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def partial_cleanup_test(base: pathlib.Path) -> None:
    verifier = import_script(
        "verify_dependency_lock", REPO_ROOT / "scripts/verify_dependency_lock.py"
    )

    class Response:
        def __init__(self) -> None:
            self.headers: dict[str, str] = {}
            self.returned = False

        def __enter__(self):
            return self

        def __exit__(self, *_args) -> None:
            return None

        def read(self, _size: int) -> bytes:
            if self.returned:
                return b""
            self.returned = True
            return b"abcd"

    download = base / "download"
    component = {
        "archive_sha256": hashlib.sha256(b"abc").hexdigest(),
        "archive_size": 3,
        "id": "fixture",
        "source_archive": "https://example.invalid/fixture.tar.gz",
    }
    with mock.patch.object(verifier.urllib.request, "urlopen", return_value=Response()):
        try:
            verifier.verify_archives(
                {"fixture": component}, {"fixture"}, download_directory=download
            )
        except verifier.VerificationError as error:
            if "exceeds locked size" not in str(error):
                raise AssertionError(f"unexpected partial failure: {error}") from error
        else:
            raise AssertionError("oversized streamed archive unexpectedly succeeded")
    if list(download.glob("*.partial-*")) or list(download.glob(".*.partial-*")):
        raise AssertionError("failed download left a partial archive")


def runtime_decoy_test(base: pathlib.Path) -> None:
    runtime = import_script(
        "verify_toolchain_runtime",
        REPO_ROOT / "scripts/release/verify_toolchain_runtime.py",
    )
    toolchain = base / "toolchain"
    toolchain.mkdir()
    decoy = base / "decoy/libc.a"
    decoy.parent.mkdir()
    decoy.write_bytes(b"decoy")
    link_map = base / "decoy.map"
    link_map.write_text(f"LOAD {decoy}\n", encoding="utf-8")
    try:
        runtime.map_selection(link_map, toolchain)
    except runtime.RuntimeVerificationError as error:
        if "outside the pinned toolchain" not in str(error):
            raise AssertionError(f"unexpected decoy failure: {error}") from error
    else:
        raise AssertionError("same-basename runtime decoy unexpectedly succeeded")


def swapped_artifact_test(base: pathlib.Path) -> None:
    source = base / "provenance-source"
    source.mkdir()
    run(["git", "init", "--quiet", str(source)])
    run(["git", "-C", str(source), "config", "user.name", "tgcli test"])
    run(["git", "-C", str(source), "config", "user.email", "tgcli@example.invalid"])
    (source / ".gitignore").write_text("build/\n", encoding="utf-8")
    recipe = source / "scripts/release/build-linux-musl.sh"
    recipe.parent.mkdir(parents=True)
    recipe.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    lock = source / "release/dependencies.lock.json"
    write_json(lock, {"components": []})
    contract = source / "release/linux-musl-toolchain.json"
    write_json(
        contract,
        {
            "dependency_lock_sha256": sha256(lock),
            "recipe": {"sha256": sha256(recipe)},
        },
    )
    run(["git", "-C", str(source), "add", "."])
    run(["git", "-C", str(source), "commit", "--quiet", "-m", "fixture"])
    commit = run(["git", "-C", str(source), "rev-parse", "HEAD"]).stdout.strip()

    build = source / "build"
    build.mkdir()
    artifact = build / "tgcli"
    artifact.write_bytes(b"artifact A")
    empty_sha = hashlib.sha256(b"").hexdigest()
    inspection = build / "inspection.json"
    write_json(
        inspection,
        {
            "artifact": {
                "path": "tgcli",
                "sha256": sha256(artifact),
                "size": artifact.stat().st_size,
            },
            "checks": {"static_elf": True},
            "commands": [
                {
                    "argv": ["fixture"],
                    "stdout": "",
                    "stdout_sha256": empty_sha,
                    "stdout_size": 0,
                }
                for _ in range(4)
            ],
            "schema_version": 1,
        },
    )
    runtime = build / "runtime.json"
    write_json(
        runtime,
        {
            "compiler_driver": {},
            "link_map": {},
            "schema_version": 1,
            "selected_runtime_files": [{"path": str(index)} for index in range(12)],
        },
    )
    tests = build / "tests.json"
    write_json(
        tests,
        {
            "argv": ["ctest", "--test-dir", "app", "--output-on-failure"],
            "binary": ".tgcli-build/app/tgcli_unit_tests",
            "passed": True,
            "working_directory": ".tgcli-build",
        },
    )
    artifact.write_bytes(b"artifact B")
    expect_failure(
        [
            sys.executable,
            str(PROVENANCE),
            "write",
            "--repo-root",
            str(source),
            "--expected-commit",
            commit,
            "--artifact",
            str(artifact),
            "--lock",
            str(lock),
            "--contract",
            str(contract),
            "--recipe",
            str(recipe),
            "--inspection",
            str(inspection),
            "--runtime-selection",
            str(runtime),
            "--test-evidence",
            str(tests),
            "--output",
            str(build / "provenance.json"),
            "--image",
            "fixture@sha256:" + "0" * 64,
            "--cmake-version",
            "cmake fixture",
            "--compiler-version",
            "compiler fixture",
            "--ninja-version",
            "ninja fixture",
        ],
        "differs from inspected artifact",
    )


def main() -> int:
    with tempfile.TemporaryDirectory(
        prefix="tgcli-release-toolchain-test-"
    ) as temporary:
        base = pathlib.Path(temporary)
        source_identity_tests(base)
        inspection_failure_tests(base)
        archive_failure_tests(base)
        partial_cleanup_test(base)
        runtime_decoy_test(base)
        swapped_artifact_test(base)
    print("release toolchain helper tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
