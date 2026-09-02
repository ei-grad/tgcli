#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

from verify_re2_build import Re2VerificationError, validate_build_evidence


class ProvenanceError(RuntimeError):
    pass


SBOM_FORMAT = "tgcli-release-sbom"
SBOM_PLATFORMS = {
    "linux-x86_64-musl",
    "macos-arm64",
    "macos-universal",
    "macos-x86_64",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ProvenanceError(message)


def run_git(
    repo_root: pathlib.Path, arguments: list[str]
) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            ["git", "-C", str(repo_root), *arguments],
            capture_output=True,
            check=False,
        )
    except OSError as error:
        raise ProvenanceError(f"cannot execute Git: {error}") from error


def git_text(repo_root: pathlib.Path, arguments: list[str], owner: str) -> str:
    completed = run_git(repo_root, arguments)
    require(completed.returncode == 0, f"cannot resolve {owner} from Git")
    try:
        return completed.stdout.decode("utf-8", errors="strict").strip()
    except UnicodeError as error:
        raise ProvenanceError(f"Git {owner} is not valid UTF-8") from error


def source_identity(repo_root: pathlib.Path, expected_commit: str) -> dict:
    repo_root = repo_root.resolve()
    git_metadata = repo_root / ".git"
    require(
        git_metadata.exists() and not git_metadata.is_symlink(),
        "release source has no trustworthy Git identity",
    )
    require(
        git_text(repo_root, ["rev-parse", "--is-inside-work-tree"], "worktree state")
        == "true",
        "release source is not a Git worktree",
    )
    top_level = pathlib.Path(
        git_text(repo_root, ["rev-parse", "--show-toplevel"], "worktree root")
    ).resolve()
    require(top_level == repo_root, "release source is not the Git worktree root")
    commit = git_text(repo_root, ["rev-parse", "HEAD^{commit}"], "source commit")
    require(commit == expected_commit, "expected source commit differs from Git HEAD")

    staged = run_git(
        repo_root, ["diff", "--cached", "--quiet", "--ignore-submodules=none", "--"]
    )
    require(staged.returncode in {0, 1}, "cannot inspect staged source changes")
    require(staged.returncode == 0, "release source has staged changes")
    tracked = run_git(repo_root, ["diff", "--quiet", "--ignore-submodules=none", "--"])
    require(tracked.returncode in {0, 1}, "cannot inspect tracked source changes")
    require(tracked.returncode == 0, "release source has tracked worktree changes")

    untracked = run_git(
        repo_root,
        ["ls-files", "--others", "--directory", "--no-empty-directory", "-z"],
    )
    require(untracked.returncode == 0, "cannot inspect untracked source files")
    untracked_entries = [
        entry.decode("utf-8", errors="strict")
        for entry in untracked.stdout.split(b"\0")
        if entry
    ]
    allowed_generated_roots = {".ruff_cache/", "build/"}
    unsafe_untracked = []
    for entry in untracked_entries:
        generated_root = repo_root / entry.removesuffix("/")
        if (
            entry not in allowed_generated_roots
            or not generated_root.is_dir()
            or generated_root.is_symlink()
        ):
            unsafe_untracked.append(entry)
    require(
        not unsafe_untracked,
        "release source has untracked files outside build/cache roots: "
        f"{unsafe_untracked[:3]}",
    )

    tree = git_text(repo_root, ["rev-parse", "HEAD^{tree}"], "source tree")
    require(len(tree) == 40, "Git source tree identity is invalid")
    return {"commit": commit, "tree": tree, "type": "git"}


def load_json(file: pathlib.Path, owner: str) -> dict:
    require(file.is_file(), f"{owner} is missing: {file}")
    require(not file.is_symlink(), f"{owner} cannot be a symlink: {file}")
    try:
        document = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProvenanceError(f"cannot parse {owner}: {error}") from error
    require(isinstance(document, dict), f"{owner} must be a JSON object")
    return document


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with file.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise ProvenanceError(f"cannot read {file}: {error}") from error
    return digest.hexdigest()


def file_identity(file: pathlib.Path) -> dict:
    require(file.is_file(), f"identity input is missing: {file}")
    require(not file.is_symlink(), f"identity input cannot be a symlink: {file}")
    return {"sha256": sha256_file(file), "size": file.stat().st_size}


def artifact_identity(artifact: pathlib.Path) -> dict:
    identity = file_identity(artifact)
    return {"path": artifact.name, **identity}


def validate_re2_evidence(document: object) -> dict:
    try:
        return validate_build_evidence(document)
    except Re2VerificationError as error:
        raise ProvenanceError(str(error)) from error


def validate_license_evidence(repo_root: pathlib.Path, evidence: object) -> list[dict]:
    require(
        isinstance(evidence, list) and evidence,
        "SBOM component license evidence is missing",
    )
    validated: list[dict] = []
    for item in evidence:
        require(
            isinstance(item, dict) and set(item) == {"path", "sha256"},
            "SBOM component license evidence is invalid",
        )
        relative = pathlib.PurePosixPath(item["path"])
        require(
            not relative.is_absolute()
            and ".." not in relative.parts
            and item["path"].startswith("release/licenses/"),
            "SBOM component license path is unsafe",
        )
        license_file = repo_root.joinpath(*relative.parts)
        require(
            license_file.is_file() and not license_file.is_symlink(),
            f"SBOM component license file is missing or unsafe: {item['path']}",
        )
        require(
            sha256_file(license_file) == item["sha256"],
            f"SBOM component license digest differs: {item['path']}",
        )
        validated.append({"path": item["path"], "sha256": item["sha256"]})
    return sorted(validated, key=lambda item: item["path"])


def embedded_component_record(repo_root: pathlib.Path, component: object) -> dict:
    require(isinstance(component, dict), "embedded SBOM component is invalid")
    required = {
        "id",
        "license_expression",
        "license_files",
        "name",
        "source_path",
        "version",
    }
    require(set(component) == required, "embedded SBOM component schema differs")
    return {
        "id": component["id"],
        "license_expression": component["license_expression"],
        "license_files": validate_license_evidence(
            repo_root, component["license_files"]
        ),
        "name": component["name"],
        "source_path": component["source_path"],
        "version": component["version"],
    }


def component_record(repo_root: pathlib.Path, component: object) -> dict:
    require(isinstance(component, dict), "SBOM component is invalid")
    required = {
        "archive_sha256",
        "archive_size",
        "embedded_components",
        "id",
        "immutable_ref",
        "license_expression",
        "license_files",
        "name",
        "scope",
        "source_archive",
        "source_repository",
        "version",
    }
    require(required <= set(component), "SBOM component lock fields are incomplete")
    embedded = component["embedded_components"]
    require(isinstance(embedded, list), "embedded SBOM component list is invalid")
    return {
        "archive_sha256": component["archive_sha256"],
        "archive_size": component["archive_size"],
        "embedded_components": sorted(
            (embedded_component_record(repo_root, item) for item in embedded),
            key=lambda item: item["id"],
        ),
        "generated_source_tree_sha256": component.get("generated_source_tree_sha256"),
        "id": component["id"],
        "immutable_ref": component["immutable_ref"],
        "license_expression": component["license_expression"],
        "license_files": validate_license_evidence(
            repo_root, component["license_files"]
        ),
        "name": component["name"],
        "scope": component["scope"],
        "source_archive": component["source_archive"],
        "source_repository": component["source_repository"],
        "source_tree_sha256": component.get("source_tree_sha256"),
        "version": component["version"],
    }


def resolved_sbom_components(
    lock: dict, repo_root: pathlib.Path, platform: str
) -> list[dict]:
    components = lock.get("components")
    require(isinstance(components, list), "dependency lock components are missing")
    if platform == "linux-x86_64-musl":
        selected = [
            item
            for item in components
            if isinstance(item, dict)
            and item.get("scope") in {"runtime", "release-runtime"}
        ]
    else:
        selected = [
            item
            for item in components
            if isinstance(item, dict)
            and (item.get("scope") == "runtime" or item.get("id") == "openssl")
        ]
    records = sorted(
        (component_record(repo_root, item) for item in selected),
        key=lambda item: item["id"],
    )
    ids = [item["id"] for item in records]
    require(len(ids) == len(set(ids)), "SBOM component IDs are not unique")
    require("re2" in ids, "SBOM runtime component inventory omits RE2")
    return records


def validate_sbom_shape(document: object, lock: dict, repo_root: pathlib.Path) -> dict:
    require(isinstance(document, dict), "SBOM must contain a JSON object")
    require(
        set(document)
        == {
            "artifact",
            "builds",
            "components",
            "dependency_lock",
            "format",
            "platform",
            "provenance",
            "schema_version",
            "slice_sboms",
        }
        and document["format"] == SBOM_FORMAT
        and document["schema_version"] == 1
        and document["platform"] in SBOM_PLATFORMS,
        "SBOM schema is invalid",
    )
    artifact = document["artifact"]
    require(
        isinstance(artifact, dict)
        and set(artifact) == {"path", "sha256", "size"}
        and isinstance(artifact["path"], str)
        and artifact["path"]
        and pathlib.PurePosixPath(artifact["path"]).name == artifact["path"]
        and isinstance(artifact["sha256"], str)
        and re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]) is not None
        and type(artifact["size"]) is int
        and artifact["size"] > 0,
        "SBOM artifact identity is invalid",
    )
    dependency_lock = document["dependency_lock"]
    require(
        isinstance(dependency_lock, dict)
        and set(dependency_lock) == {"path", "sha256"}
        and dependency_lock["path"] == "release/dependencies.lock.json"
        and dependency_lock["sha256"]
        == sha256_file(repo_root / "release/dependencies.lock.json"),
        "SBOM dependency lock identity is invalid",
    )
    provenance = document["provenance"]
    require(
        isinstance(provenance, dict)
        and set(provenance) == {"sha256", "size"}
        and isinstance(provenance["sha256"], str)
        and re.fullmatch(r"[0-9a-f]{64}", provenance["sha256"]) is not None
        and type(provenance["size"]) is int
        and provenance["size"] > 0,
        "SBOM provenance identity is invalid",
    )
    expected_components = resolved_sbom_components(
        lock, repo_root, document["platform"]
    )
    require(
        document["components"] == expected_components,
        "SBOM component inventory differs from the dependency lock",
    )
    builds = document["builds"]
    require(isinstance(builds, list) and builds, "SBOM build inventory is missing")
    observed_platforms: list[str] = []
    for build in builds:
        require(
            isinstance(build, dict) and set(build) == {"platform", "re2"},
            "SBOM build record schema is invalid",
        )
        require(
            build["platform"] in {"linux-x86_64-musl", "macos-arm64", "macos-x86_64"},
            "SBOM build record platform is invalid",
        )
        validate_re2_evidence(build["re2"])
        observed_platforms.append(build["platform"])
    require(
        observed_platforms == sorted(set(observed_platforms)),
        "SBOM build records must be unique and sorted",
    )
    expected_build_platforms = (
        ["macos-arm64", "macos-x86_64"]
        if document["platform"] == "macos-universal"
        else [document["platform"]]
    )
    require(
        observed_platforms == expected_build_platforms,
        "SBOM build inventory differs from its platform",
    )
    slice_sboms = document["slice_sboms"]
    require(isinstance(slice_sboms, list), "SBOM slice inventory is invalid")
    if document["platform"] == "macos-universal":
        require(
            len(slice_sboms) == 2
            and [item.get("platform") for item in slice_sboms]
            == ["macos-arm64", "macos-x86_64"],
            "universal SBOM slice inventory is invalid",
        )
        for item in slice_sboms:
            require(
                isinstance(item, dict)
                and set(item) == {"platform", "sha256", "size"}
                and isinstance(item["sha256"], str)
                and re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) is not None
                and type(item["size"]) is int
                and item["size"] > 0,
                "universal SBOM slice identity is invalid",
            )
    else:
        require(not slice_sboms, "architecture SBOM cannot contain slice identities")
    return document


def build_sbom_document(
    artifact: pathlib.Path,
    lock_file: pathlib.Path,
    provenance_file: pathlib.Path,
    platform: str,
    slice_sboms: dict[str, pathlib.Path],
) -> dict:
    require(platform in SBOM_PLATFORMS, "SBOM platform is unsupported")
    lock = load_json(lock_file, "dependency lock")
    provenance = load_json(provenance_file, "build provenance")
    repo_root = lock_file.resolve().parent.parent
    artifact_record = artifact_identity(artifact)
    require(
        provenance.get("artifact") == artifact_record,
        "SBOM provenance does not identify the artifact",
    )
    if platform.startswith("macos-"):
        require(
            provenance.get("platform") == platform,
            "SBOM provenance has an unexpected macOS platform",
        )

    slice_records: list[dict] = []
    if platform == "macos-universal":
        require(
            set(slice_sboms) == {"macos-arm64", "macos-x86_64"},
            "universal SBOM requires both architecture SBOMs",
        )
        provenance_slices = provenance.get("slices")
        require(
            isinstance(provenance_slices, dict),
            "universal provenance slices are missing",
        )
        builds: list[dict] = []
        for slice_platform in sorted(slice_sboms):
            slice_file = slice_sboms[slice_platform]
            slice_document = validate_sbom_shape(
                load_json(slice_file, f"{slice_platform} SBOM"), lock, repo_root
            )
            require(
                slice_document["platform"] == slice_platform,
                "architecture SBOM has an unexpected platform",
            )
            arch = slice_platform.removeprefix("macos-")
            slice_identity = file_identity(slice_file)
            require(
                isinstance(provenance_slices.get(arch), dict)
                and provenance_slices[arch].get("sbom_sha256")
                == slice_identity["sha256"],
                "universal provenance does not bind an architecture SBOM",
            )
            builds.extend(slice_document["builds"])
            slice_records.append({"platform": slice_platform, **slice_identity})
        builds.sort(key=lambda item: item["platform"])
    else:
        require(not slice_sboms, "architecture SBOM cannot contain slice inputs")
        re2_build = provenance.get("re2_build")
        validate_re2_evidence(re2_build)
        builds = [{"platform": platform, "re2": re2_build}]

    document = {
        "artifact": artifact_record,
        "builds": builds,
        "components": resolved_sbom_components(lock, repo_root, platform),
        "dependency_lock": {
            "path": "release/dependencies.lock.json",
            "sha256": sha256_file(lock_file),
        },
        "format": SBOM_FORMAT,
        "platform": platform,
        "provenance": file_identity(provenance_file),
        "schema_version": 1,
        "slice_sboms": slice_records,
    }
    return validate_sbom_shape(document, lock, repo_root)


def parse_slice_sboms(values: list[str]) -> dict[str, pathlib.Path]:
    parsed: dict[str, pathlib.Path] = {}
    for value in values:
        platform, separator, filename = value.partition("=")
        require(
            separator == "="
            and platform in {"macos-arm64", "macos-x86_64"}
            and filename
            and platform not in parsed,
            "slice SBOM argument is invalid",
        )
        parsed[platform] = pathlib.Path(filename).resolve()
    return parsed


def write_json_new(output: pathlib.Path, document: dict, owner: str) -> None:
    require(
        not output.exists() and not output.is_symlink(),
        f"{owner} output already exists: {output}",
    )
    require(output.parent.is_dir(), f"{owner} output parent is missing")
    try:
        with output.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(document, indent=2, sort_keys=True) + "\n")
    except OSError as error:
        raise ProvenanceError(f"cannot write {owner}: {error}") from error


def write_sbom(args: argparse.Namespace) -> None:
    document = build_sbom_document(
        args.artifact.resolve(),
        args.lock.resolve(),
        args.provenance.resolve(),
        args.platform,
        parse_slice_sboms(args.slice_sbom),
    )
    write_json_new(args.output.resolve(), document, "SBOM")


def verify_sbom(args: argparse.Namespace) -> None:
    expected = build_sbom_document(
        args.artifact.resolve(),
        args.lock.resolve(),
        args.provenance.resolve(),
        args.platform,
        parse_slice_sboms(args.slice_sbom),
    )
    actual = load_json(args.sbom.resolve(), "SBOM")
    require(actual == expected, "SBOM differs from its artifact, lock, or provenance")


def validate_artifact_evidence(
    artifact: pathlib.Path, inspection: dict
) -> tuple[str, int]:
    require(artifact.is_file(), f"release artifact is missing: {artifact}")
    require(not artifact.is_symlink(), "release artifact cannot be a symlink")
    require(
        set(inspection) == {"artifact", "checks", "commands", "schema_version"}
        and inspection["schema_version"] == 1,
        "artifact inspection evidence has an invalid schema",
    )
    artifact_evidence = inspection["artifact"]
    require(
        isinstance(artifact_evidence, dict)
        and set(artifact_evidence) == {"path", "sha256", "size"},
        "artifact inspection identity is invalid",
    )
    actual_sha256 = sha256_file(artifact)
    actual_size = artifact.stat().st_size
    require(
        artifact_evidence["path"] == artifact.name
        and artifact_evidence["sha256"] == actual_sha256
        and artifact_evidence["size"] == actual_size,
        "release artifact differs from inspected artifact",
    )
    checks = inspection["checks"]
    require(
        isinstance(checks, dict)
        and checks
        and all(value is True for value in checks.values()),
        "artifact inspection checks did not all pass",
    )
    commands = inspection["commands"]
    require(
        isinstance(commands, list)
        and len(commands) == 4
        and all(isinstance(command, dict) for command in commands),
        "artifact inspection command evidence is incomplete",
    )
    for command in commands:
        require(
            set(command) == {"argv", "stdout", "stdout_sha256", "stdout_size"},
            "artifact inspection command evidence is invalid",
        )
        stdout = command["stdout"]
        require(isinstance(stdout, str), "artifact inspection stdout is invalid")
        encoded = stdout.encode("utf-8")
        require(
            command["stdout_size"] == len(encoded)
            and command["stdout_sha256"] == hashlib.sha256(encoded).hexdigest(),
            "artifact inspection output digest differs",
        )
    return actual_sha256, actual_size


def validate_test_evidence(document: dict) -> None:
    require(
        set(document) == {"argv", "binary", "passed", "working_directory"},
        "test evidence has an invalid schema",
    )
    require(
        document["argv"]
        == [
            "ctest",
            "--test-dir",
            "app",
            "--output-on-failure",
            "--exclude-regex",
            "^command-registry-completion-zsh$",
        ]
        and document["working_directory"] == ".tgcli-build"
        and document["binary"] == ".tgcli-build/app/tgcli_unit_tests"
        and document["passed"] is True,
        "test evidence does not describe the required release test invocation",
    )


def validate_runtime_evidence(document: dict) -> None:
    require(
        set(document)
        == {"compiler_driver", "link_map", "schema_version", "selected_runtime_files"}
        and document["schema_version"] == 1,
        "runtime selection evidence has an invalid schema",
    )
    selected = document["selected_runtime_files"]
    require(
        isinstance(selected, list) and len(selected) == 12,
        "runtime selection evidence is incomplete",
    )


def write_provenance(args: argparse.Namespace) -> None:
    repo_root = args.repo_root.resolve()
    artifact = args.artifact.resolve()
    output = args.output.resolve()
    require(
        not output.exists() and not output.is_symlink(),
        f"build provenance output already exists: {output}",
    )
    identity = source_identity(repo_root, args.expected_commit)
    lock_file = args.lock.resolve()
    contract_file = args.contract.resolve()
    recipe_file = args.recipe.resolve()
    lock = load_json(lock_file, "dependency lock")
    contract = load_json(contract_file, "release contract")
    inspection = load_json(args.inspection.resolve(), "artifact inspection evidence")
    runtime = load_json(args.runtime_selection.resolve(), "runtime selection evidence")
    tests = load_json(args.test_evidence.resolve(), "release test evidence")
    re2_build = load_json(
        args.re2_build_evidence.resolve(), "RE2 build verification evidence"
    )

    lock_sha256 = sha256_file(lock_file)
    contract_sha256 = sha256_file(contract_file)
    recipe_sha256 = sha256_file(recipe_file)
    require(
        contract.get("dependency_lock_sha256") == lock_sha256,
        "release contract does not bind the dependency lock",
    )
    require(
        isinstance(contract.get("recipe"), dict)
        and contract["recipe"].get("sha256") == recipe_sha256,
        "release contract does not bind the release recipe",
    )
    artifact_sha256, artifact_size = validate_artifact_evidence(artifact, inspection)
    validate_runtime_evidence(runtime)
    validate_test_evidence(tests)
    validate_re2_evidence(re2_build)

    components = lock.get("components")
    require(isinstance(components, list), "dependency lock components are missing")
    resolved_dependencies = sorted(
        (
            {
                "archive_sha256": component["archive_sha256"],
                "archive_size": component["archive_size"],
                "id": component["id"],
                "immutable_ref": component["immutable_ref"],
                "source_repository": component["source_repository"],
            }
            for component in components
            if component.get("scope") in {"runtime", "release-runtime"}
        ),
        key=lambda component: component["id"],
    )
    provenance = {
        "artifact": {
            "path": artifact.name,
            "sha256": artifact_sha256,
            "size": artifact_size,
        },
        "dependency_lock_sha256": lock_sha256,
        "inspection": inspection,
        "recipe_sha256": recipe_sha256,
        "re2_build": re2_build,
        "release_contract_sha256": contract_sha256,
        "resolved_dependencies": resolved_dependencies,
        "runtime_selection": runtime,
        "schema_version": 3,
        "source": identity,
        "source_sha": identity["commit"],
        "tests": tests,
        "tool_versions": {
            "cmake": args.cmake_version,
            "compiler": args.compiler_version,
            "ninja": args.ninja_version,
        },
        "toolchain_image": args.image,
    }
    output.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify and record release provenance")
    subparsers = parser.add_subparsers(dest="command", required=True)

    identity = subparsers.add_parser("source-identity")
    identity.add_argument("--repo-root", type=pathlib.Path, required=True)
    identity.add_argument("--expected-commit", required=True)

    write = subparsers.add_parser("write")
    write.add_argument("--repo-root", type=pathlib.Path, required=True)
    write.add_argument("--expected-commit", required=True)
    write.add_argument("--artifact", type=pathlib.Path, required=True)
    write.add_argument("--lock", type=pathlib.Path, required=True)
    write.add_argument("--contract", type=pathlib.Path, required=True)
    write.add_argument("--recipe", type=pathlib.Path, required=True)
    write.add_argument("--inspection", type=pathlib.Path, required=True)
    write.add_argument("--runtime-selection", type=pathlib.Path, required=True)
    write.add_argument("--test-evidence", type=pathlib.Path, required=True)
    write.add_argument("--re2-build-evidence", type=pathlib.Path, required=True)
    write.add_argument("--output", type=pathlib.Path, required=True)
    write.add_argument("--image", required=True)
    write.add_argument("--cmake-version", required=True)
    write.add_argument("--compiler-version", required=True)
    write.add_argument("--ninja-version", required=True)

    for command in ("write-sbom", "verify-sbom"):
        sbom = subparsers.add_parser(command)
        sbom.add_argument("--artifact", type=pathlib.Path, required=True)
        sbom.add_argument("--lock", type=pathlib.Path, required=True)
        sbom.add_argument("--provenance", type=pathlib.Path, required=True)
        sbom.add_argument("--platform", choices=sorted(SBOM_PLATFORMS), required=True)
        sbom.add_argument("--slice-sbom", action="append", default=[])
        if command == "write-sbom":
            sbom.add_argument("--output", type=pathlib.Path, required=True)
        else:
            sbom.add_argument("--sbom", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "source-identity":
            print(
                json.dumps(
                    source_identity(args.repo_root, args.expected_commit),
                    sort_keys=True,
                )
            )
        elif args.command == "write":
            write_provenance(args)
        elif args.command == "write-sbom":
            write_sbom(args)
        else:
            verify_sbom(args)
    except (ProvenanceError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"release provenance verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
