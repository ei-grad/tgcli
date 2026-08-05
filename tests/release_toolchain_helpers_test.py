#!/usr/bin/env python3

import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
from unittest import mock

sys.dont_write_bytecode = True

REPO_ROOT = pathlib.Path(sys.argv[1]).resolve()
ARCHIVE_TOOL = REPO_ROOT / "scripts/release/archive_tool.py"
INSPECTOR = REPO_ROOT / "scripts/release/inspect_linux_artifact.py"
PROVENANCE = REPO_ROOT / "scripts/release/build_provenance.py"
RE2_BUILD_VERIFIER = REPO_ROOT / "scripts/release/verify_re2_build.py"
LOCAL_CACHE_ROOTS = (
    REPO_ROOT / "scripts/__pycache__",
    REPO_ROOT / "scripts/release/__pycache__",
    REPO_ROOT / "tests/__pycache__",
)


def local_cache_entries() -> set[str]:
    return {
        str(entry.relative_to(REPO_ROOT))
        for root in LOCAL_CACHE_ROOTS
        if root.exists()
        for entry in (root, *root.rglob("*"))
    }


INITIAL_LOCAL_CACHE_ENTRIES = local_cache_entries()


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
    (root / ".gitignore").write_text(".ruff_cache/\nbuild/\n", encoding="utf-8")
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

    for generated_root in (source / ".ruff_cache", source / "build"):
        generated_root.mkdir()
        (generated_root / "generated.cache").write_text("cache\n", encoding="utf-8")
    run(command)

    exclude_file = source / ".git/release-test-excludes"
    exclude_file.write_text("concealed/\n", encoding="utf-8")
    run(
        [
            "git",
            "-C",
            str(source),
            "config",
            "core.excludesFile",
            str(exclude_file),
        ]
    )
    concealed = source / "concealed/generated.pyc"
    concealed.parent.mkdir()
    concealed.write_bytes(b"concealed")
    if run(
        ["git", "-C", str(source), "ls-files", "--others", "--exclude-standard"]
    ).stdout:
        raise AssertionError("test exclude rule did not conceal the untracked fixture")
    expect_failure(command, "untracked files")
    concealed.unlink()
    concealed.parent.rmdir()
    run(command)

    prefix_lookalike = source / "build-output/forged"
    prefix_lookalike.parent.mkdir()
    prefix_lookalike.write_text("forged\n", encoding="utf-8")
    expect_failure(command, "untracked files")
    prefix_lookalike.unlink()
    prefix_lookalike.parent.rmdir()

    nested_lookalike = source / "src/build/forged"
    nested_lookalike.parent.mkdir(parents=True)
    nested_lookalike.write_text("forged\n", encoding="utf-8")
    expect_failure(command, "untracked files")
    nested_lookalike.unlink()
    nested_lookalike.parent.rmdir()
    nested_lookalike.parent.parent.rmdir()
    run(command)

    unsafe_source = base / "unsafe-generated-root"
    unsafe_source.mkdir()
    unsafe_commit = initialize_source_repo(unsafe_source)
    external_build = base / "external-build"
    external_build.mkdir()
    (unsafe_source / "build").symlink_to(external_build, target_is_directory=True)
    expect_failure(
        [
            sys.executable,
            str(PROVENANCE),
            "source-identity",
            "--repo-root",
            str(unsafe_source),
            "--expected-commit",
            unsafe_commit,
        ],
        "untracked files",
    )

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


def re2_build_verifier_tests(base: pathlib.Path) -> None:
    verifier = import_script("verify_re2_build", RE2_BUILD_VERIFIER)

    def expect_re2_failure(action, diagnostic: str) -> None:
        try:
            action()
        except verifier.Re2VerificationError as error:
            if diagnostic not in str(error):
                raise AssertionError(
                    f"unexpected RE2 verifier failure: {error}"
                ) from error
        else:
            raise AssertionError(f"RE2 verifier unexpectedly accepted {diagnostic}")

    valid_cache = "".join(
        f"{option}:BOOL=ON\n"
        for option in ("BUILD_SHARED_LIBS", "RE2_BUILD_TESTING", "USEPCRE")
    )
    verifier.validate_cache(valid_cache)
    expect_re2_failure(
        lambda: verifier.validate_cache(valid_cache + "RE2_USE_ICU:BOOL=OFF\n"),
        "nonexistent RE2_USE_ICU",
    )

    compile_commands = [
        {"command": f"c++ -c /source/{source}", "file": f"/source/{source}"}
        for source in sorted(verifier.RE2_RUNTIME_SOURCES)
    ]
    verifier.validate_compile_commands(compile_commands)
    invalid_commands = list(compile_commands)
    invalid_commands[0] = dict(invalid_commands[0])
    invalid_commands[0]["command"] += " -DRE2_USE_ICU"
    expect_re2_failure(
        lambda: verifier.validate_compile_commands(invalid_commands),
        "forbidden RE2 compile definition",
    )
    invalid_commands[0]["command"] = 'c++ "-DRE2_USE_ICU=1" -c /source/re2.cc'
    expect_re2_failure(
        lambda: verifier.validate_compile_commands(invalid_commands),
        "forbidden RE2 compile definition",
    )
    test_commands = list(compile_commands)
    test_commands.append(
        {"command": "c++ -c /source/util/pcre.cc", "file": "/source/util/pcre.cc"}
    )
    expect_re2_failure(
        lambda: verifier.validate_compile_commands(test_commands),
        "test or benchmark source",
    )

    build = base / "re2-build-contract"
    re2_build = build / "_deps/re2-build"
    re2_build.mkdir(parents=True)
    archive = re2_build / "libre2.a"
    archive.write_bytes(b"static re2")
    verified_archive = verifier.validate_build_artifacts(build)
    if verified_archive != archive.resolve():
        raise AssertionError("RE2 artifact verifier returned a non-canonical archive")
    verifier.validate_link_commands(
        "c++ _deps/re2-build/libre2.a -o tgcli\n", build, archive
    )
    verifier.validate_link_map("LOAD _deps/re2-build/libre2.a\n", build, archive)

    absolute_decoy = pathlib.Path("/untrusted/decoy/libre2.a")
    relative_decoy = "untrusted/decoy/libre2.a"
    for decoy in (str(absolute_decoy), relative_decoy):
        expect_re2_failure(
            lambda decoy=decoy: verifier.validate_link_commands(
                f"c++ {decoy} -o tgcli\n", build, archive
            ),
            "canonical RE2 archive",
        )
        expect_re2_failure(
            lambda decoy=decoy: verifier.validate_link_map(
                f"LOAD {decoy}\n", build, archive
            ),
            "canonical RE2 archive",
        )
        expect_re2_failure(
            lambda decoy=decoy: verifier.validate_link_commands(
                f"c++ _deps/re2-build/libre2.a {decoy} -o tgcli\n",
                build,
                archive,
            ),
            "canonical RE2 archive",
        )
        expect_re2_failure(
            lambda decoy=decoy: verifier.validate_link_map(
                f"LOAD _deps/re2-build/libre2.a\nLOAD {decoy}\n", build, archive
            ),
            "canonical RE2 archive",
        )
    expect_re2_failure(
        lambda: verifier.validate_link_commands(
            "c++ /untrusted/libre2.a.backup -o tgcli\n", build, archive
        ),
        "canonical RE2 archive",
    )
    for library in ("icuuc", "pcre", "absl_strings"):
        expect_re2_failure(
            lambda library=library: verifier.validate_link_commands(
                f"c++ _deps/re2-build/libre2.a -l{library} -o tgcli\n",
                build,
                archive,
            ),
            "forbidden runtime library",
        )

    shared_build = base / "re2-shared-contract"
    shared_re2_build = shared_build / "_deps/re2-build"
    shared_re2_build.mkdir(parents=True)
    (shared_re2_build / "libre2.a").write_bytes(b"static re2")
    (shared_re2_build / "libre2.so").write_bytes(b"shared re2")
    expect_re2_failure(
        lambda: verifier.validate_build_artifacts(shared_build),
        "shared or unexpected RE2 artifact",
    )


def re2_evidence_document() -> dict:
    verifier = import_script("verify_re2_build_evidence", RE2_BUILD_VERIFIER)
    return {
        "archive": {
            "path": "_deps/re2-build/libre2.a",
            "sha256": "a" * 64,
            "size": 1234,
        },
        "checks": {
            "abseil_absent": True,
            "benchmarks_absent": True,
            "icu_absent": True,
            "pcre_absent": True,
            "shared_re2_absent": True,
            "tests_absent": True,
        },
        "compile_sources": sorted(verifier.RE2_RUNTIME_SOURCES),
        "configured_options": {
            "BUILD_SHARED_LIBS": False,
            "RE2_BUILD_TESTING": False,
            "USEPCRE": False,
        },
        "final_link": {
            "archive": "_deps/re2-build/libre2.a",
            "command_sha256": "b" * 64,
        },
        "link_map": None,
        "runtime_libraries": ["Threads::Threads"],
        "schema_version": 1,
        "target": "re2::re2",
        "target_type": "STATIC_LIBRARY",
    }


def sbom_tests(base: pathlib.Path) -> None:
    package = base / "sbom-package"
    (package / "release").mkdir(parents=True)
    shutil.copy2(
        REPO_ROOT / "release/dependencies.lock.json",
        package / "release/dependencies.lock.json",
    )
    shutil.copytree(REPO_ROOT / "release/licenses", package / "release/licenses")
    lock = package / "release/dependencies.lock.json"

    def write_platform_inputs(platform: str) -> tuple[pathlib.Path, pathlib.Path]:
        artifact = package / f"{platform}/tgcli"
        artifact.parent.mkdir(parents=True)
        artifact.write_bytes(f"{platform} artifact\n".encode())
        provenance = artifact.parent / "PROVENANCE.json"
        write_json(
            provenance,
            {
                "artifact": {
                    "path": "tgcli",
                    "sha256": sha256(artifact),
                    "size": artifact.stat().st_size,
                },
                "platform": platform,
                "re2_build": re2_evidence_document(),
            },
        )
        return artifact, provenance

    def write_sbom(
        platform: str,
        artifact: pathlib.Path,
        provenance: pathlib.Path,
        output: pathlib.Path,
        slices: list[str] | None = None,
    ) -> None:
        command = [
            sys.executable,
            str(PROVENANCE),
            "write-sbom",
            "--artifact",
            str(artifact),
            "--lock",
            str(lock),
            "--provenance",
            str(provenance),
            "--platform",
            platform,
            "--output",
            str(output),
        ]
        for item in slices or []:
            command.extend(["--slice-sbom", item])
        run(command)

    def verify_sbom(
        platform: str,
        artifact: pathlib.Path,
        provenance: pathlib.Path,
        sbom: pathlib.Path,
        slices: list[str] | None = None,
        expected: int = 0,
    ) -> subprocess.CompletedProcess:
        command = [
            sys.executable,
            str(PROVENANCE),
            "verify-sbom",
            "--artifact",
            str(artifact),
            "--lock",
            str(lock),
            "--provenance",
            str(provenance),
            "--platform",
            platform,
            "--sbom",
            str(sbom),
        ]
        for item in slices or []:
            command.extend(["--slice-sbom", item])
        return run(command, expected=expected)

    linux_artifact, linux_provenance = write_platform_inputs("linux-x86_64-musl")
    linux_sbom = linux_artifact.parent / "SBOM.json"
    duplicate_sbom = linux_artifact.parent / "SBOM-duplicate.json"
    write_sbom("linux-x86_64-musl", linux_artifact, linux_provenance, linux_sbom)
    write_sbom("linux-x86_64-musl", linux_artifact, linux_provenance, duplicate_sbom)
    if linux_sbom.read_bytes() != duplicate_sbom.read_bytes():
        raise AssertionError("identical SBOM inputs produced different bytes")
    verify_sbom("linux-x86_64-musl", linux_artifact, linux_provenance, linux_sbom)

    document = json.loads(linux_sbom.read_text(encoding="utf-8"))
    re2 = next(item for item in document["components"] if item["id"] == "re2")
    if (
        re2["license_expression"] != "BSD-3-Clause"
        or re2["source_tree_sha256"]
        != "6d3942bcd96377f18ec60a7b190d1b217d037ff0132ff6ae8dc463347c067046"
        or re2["embedded_components"]
        != [
            {
                "id": "re2-plan9-utf",
                "license_expression": "LicenseRef-RE2-Lucent-2002",
                "license_files": [
                    {
                        "path": "release/licenses/RE2-Lucent-UTF.txt",
                        "sha256": "8af3194d846fcddce0f5e8d4ae6c404744d9b7922a24f23415bd15a9cfe5e6ee",
                    }
                ],
                "name": "Plan 9 UTF routines",
                "source_path": "util/rune.cc and util/utf.h",
                "version": "2002",
            }
        ]
    ):
        raise AssertionError("SBOM lost locked RE2 or Lucent license evidence")
    if "re2/mimics_pcre.cc" not in document["builds"][0]["re2"]["compile_sources"]:
        raise AssertionError("valid RE2 mimics_pcre runtime source was rejected")

    tampered_sbom = linux_artifact.parent / "SBOM-tampered.json"
    tampered_document = json.loads(linux_sbom.read_text(encoding="utf-8"))
    tampered_document["components"].append(
        {"id": "abseil", "license_expression": "Apache-2.0"}
    )
    write_json(tampered_sbom, tampered_document)
    verify_sbom(
        "linux-x86_64-musl",
        linux_artifact,
        linux_provenance,
        tampered_sbom,
        expected=1,
    )

    for index, mutate in enumerate(
        (
            lambda sbom, component: sbom["artifact"].__setitem__("sha256", "0" * 64),
            lambda sbom, component: component.__setitem__("archive_sha256", "0" * 64),
            lambda sbom, component: component.__setitem__(
                "source_tree_sha256", "0" * 64
            ),
            lambda sbom, component: component["license_files"][0].__setitem__(
                "sha256", "0" * 64
            ),
        )
    ):
        identity_document = json.loads(linux_sbom.read_text(encoding="utf-8"))
        identity_re2 = next(
            item for item in identity_document["components"] if item["id"] == "re2"
        )
        mutate(identity_document, identity_re2)
        identity_sbom = linux_artifact.parent / f"SBOM-identity-tamper-{index}.json"
        write_json(identity_sbom, identity_document)
        verify_sbom(
            "linux-x86_64-musl",
            linux_artifact,
            linux_provenance,
            identity_sbom,
            expected=1,
        )

    license_file = package / "release/licenses/RE2-Lucent-UTF.txt"
    license_bytes = license_file.read_bytes()
    license_file.write_bytes(b"tampered Lucent notice\n")
    failure = verify_sbom(
        "linux-x86_64-musl",
        linux_artifact,
        linux_provenance,
        linux_sbom,
        expected=1,
    )
    if "license digest differs" not in failure.stderr:
        raise AssertionError("packaged license tamper lacked the expected diagnostic")
    license_file.write_bytes(license_bytes)

    bad_evidence_cases = []
    for check in ("abseil_absent", "icu_absent", "pcre_absent"):
        evidence = re2_evidence_document()
        evidence["checks"][check] = False
        bad_evidence_cases.append(evidence)
    shared = re2_evidence_document()
    shared["target_type"] = "SHARED_LIBRARY"
    bad_evidence_cases.append(shared)
    tests = re2_evidence_document()
    tests["compile_sources"].append("util/test.cc")
    bad_evidence_cases.append(tests)
    benchmarks = re2_evidence_document()
    benchmarks["compile_sources"].append("util/benchmark.cc")
    bad_evidence_cases.append(benchmarks)
    pcre_option = re2_evidence_document()
    pcre_option["configured_options"]["USEPCRE"] = True
    bad_evidence_cases.append(pcre_option)
    for index, evidence in enumerate(bad_evidence_cases):
        bad_provenance = linux_artifact.parent / f"bad-provenance-{index}.json"
        bad_document = json.loads(linux_provenance.read_text(encoding="utf-8"))
        bad_document["re2_build"] = evidence
        write_json(bad_provenance, bad_document)
        failure = run(
            [
                sys.executable,
                str(PROVENANCE),
                "write-sbom",
                "--artifact",
                str(linux_artifact),
                "--lock",
                str(lock),
                "--provenance",
                str(bad_provenance),
                "--platform",
                "linux-x86_64-musl",
                "--output",
                str(linux_artifact.parent / f"bad-sbom-{index}.json"),
            ],
            expected=1,
        )
        if "required static runtime closure" not in failure.stderr:
            raise AssertionError("forbidden RE2 evidence lacked fail-closed diagnostic")

    slices: dict[str, tuple[pathlib.Path, pathlib.Path, pathlib.Path]] = {}
    for platform in ("macos-arm64", "macos-x86_64"):
        artifact, provenance = write_platform_inputs(platform)
        sbom = artifact.parent / "SBOM.json"
        write_sbom(platform, artifact, provenance, sbom)
        verify_sbom(platform, artifact, provenance, sbom)
        slices[platform] = artifact, provenance, sbom
    universal_artifact = package / "macos-universal/tgcli"
    universal_artifact.parent.mkdir(parents=True)
    universal_artifact.write_bytes(b"macOS universal artifact\n")
    universal_provenance = universal_artifact.parent / "PROVENANCE.json"
    write_json(
        universal_provenance,
        {
            "artifact": {
                "path": "tgcli",
                "sha256": sha256(universal_artifact),
                "size": universal_artifact.stat().st_size,
            },
            "platform": "macos-universal",
            "slices": {
                platform.removeprefix("macos-"): {"sbom_sha256": sha256(values[2])}
                for platform, values in slices.items()
            },
        },
    )
    slice_arguments = [
        f"{platform}={values[2]}" for platform, values in sorted(slices.items())
    ]
    universal_sbom = universal_artifact.parent / "SBOM.json"
    write_sbom(
        "macos-universal",
        universal_artifact,
        universal_provenance,
        universal_sbom,
        slice_arguments,
    )
    verify_sbom(
        "macos-universal",
        universal_artifact,
        universal_provenance,
        universal_sbom,
        slice_arguments,
    )
    slices["macos-arm64"][2].write_text(
        slices["macos-arm64"][2].read_text(encoding="utf-8") + "\n",
        encoding="utf-8",
    )
    failure = verify_sbom(
        "macos-universal",
        universal_artifact,
        universal_provenance,
        universal_sbom,
        slice_arguments,
        expected=1,
    )
    if "does not bind an architecture SBOM" not in failure.stderr:
        raise AssertionError("tampered architecture SBOM lacked binding diagnostic")


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
    re2_evidence = build / "re2-build-evidence.json"
    write_json(re2_evidence, re2_evidence_document())
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
            "--re2-build-evidence",
            str(re2_evidence),
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
    if os.environ.get("PYTHONDONTWRITEBYTECODE") != "1":
        raise AssertionError("release helper requires bytecode writes to be disabled")
    temporary_parent = REPO_ROOT / "build"
    temporary_parent.mkdir(exist_ok=True)
    if temporary_parent.is_symlink() or not temporary_parent.is_dir():
        raise AssertionError("release helper temporary parent is unsafe")
    with tempfile.TemporaryDirectory(
        prefix="tgcli-release-toolchain-test-", dir=temporary_parent
    ) as temporary:
        base = pathlib.Path(temporary)
        source_identity_tests(base)
        inspection_failure_tests(base)
        archive_failure_tests(base)
        partial_cleanup_test(base)
        runtime_decoy_test(base)
        re2_build_verifier_tests(base)
        sbom_tests(base)
        swapped_artifact_test(base)
    final_cache_entries = local_cache_entries()
    if final_cache_entries != INITIAL_LOCAL_CACHE_ENTRIES:
        created = sorted(final_cache_entries - INITIAL_LOCAL_CACHE_ENTRIES)
        raise AssertionError(f"release helper created local bytecode caches: {created}")
    print("release toolchain helper tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
