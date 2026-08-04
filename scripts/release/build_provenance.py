#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys


class ProvenanceError(RuntimeError):
    pass


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
        document["argv"] == ["ctest", "--test-dir", "app", "--output-on-failure"]
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
        "release_contract_sha256": contract_sha256,
        "resolved_dependencies": resolved_dependencies,
        "runtime_selection": runtime,
        "schema_version": 2,
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
    write.add_argument("--output", type=pathlib.Path, required=True)
    write.add_argument("--image", required=True)
    write.add_argument("--cmake-version", required=True)
    write.add_argument("--compiler-version", required=True)
    write.add_argument("--ninja-version", required=True)
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
        else:
            write_provenance(args)
    except (ProvenanceError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"release provenance verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
