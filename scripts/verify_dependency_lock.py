#!/usr/bin/env python3

import argparse
import hashlib
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path


class VerificationError(RuntimeError):
    pass


DIRECT_FETCHCONTENT = {
    "tdlib": "TGCLI_TDLIB_REV",
    "cli11": "TGCLI_CLI11_REV",
    "nlohmann_json": "TGCLI_NLOHMANN_JSON_REV",
    "fmt": "TGCLI_FMT_REV",
    "tomlplusplus": "TGCLI_TOMLPLUSPLUS_REV",
    "catch2": "TGCLI_CATCH2_REV",
    "jsoncons": "TGCLI_JSONCONS_REV",
}
FETCHCONTENT_DECLARATIONS = {
    component_id: component_id for component_id in DIRECT_FETCHCONTENT
}
FETCHCONTENT_DECLARATIONS["tdlib"] = "td"

LOCKED_SCOPES = {"runtime", "release-runtime", "test-only"}
RUNTIME_SCOPES = {"runtime", "release-runtime"}
PLANNED_IDS = {"musl", "gcc-runtime"}
EXPECTED_LOCKED_INTEGRATIONS = {
    "runtime": "fetchcontent",
    "release-runtime": "release-toolchain-pending",
    "test-only": "test-fetchcontent",
}
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
LICENSE_PATH_PATTERN = re.compile(r"^release/licenses/[A-Za-z0-9.+_-]+\.txt$")

COMPONENT_KEYS = {
    "id",
    "name",
    "scope",
    "lock_state",
    "integration",
    "source_repository",
    "source_archive",
    "immutable_ref",
    "version",
    "archive_sha256",
    "license_expression",
    "license_files",
    "embedded_components",
}
LICENSE_FILE_KEYS = {"path", "sha256"}
EMBEDDED_KEYS = {
    "id",
    "name",
    "version",
    "source_path",
    "license_expression",
    "license_files",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def load_json(file: Path) -> dict:
    try:
        document = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot parse {file}: {error}") from error
    require(isinstance(document, dict), f"{file} must contain a JSON object")
    return document


def validate_license_file(entry: object, repo_root: Path, owner: str) -> str:
    require(isinstance(entry, dict), f"{owner}: license file entry must be an object")
    require(set(entry) == LICENSE_FILE_KEYS, f"{owner}: invalid license file keys")
    relative = entry["path"]
    checksum = entry["sha256"]
    require(
        isinstance(relative, str) and LICENSE_PATH_PATTERN.fullmatch(relative),
        f"{owner}: invalid license path",
    )
    require(
        isinstance(checksum, str) and SHA256_PATTERN.fullmatch(checksum),
        f"{owner}: invalid license SHA256",
    )
    license_file = repo_root / relative
    require(license_file.is_file(), f"{owner}: missing {relative}")
    require(not license_file.is_symlink(), f"{owner}: license file cannot be a symlink")
    actual = hashlib.sha256(license_file.read_bytes()).hexdigest()
    require(actual == checksum, f"{owner}: checksum mismatch for {relative}")
    return relative


def validate_embedded_component(
    entry: object, repo_root: Path, parent_id: str
) -> set[str]:
    require(
        isinstance(entry, dict), f"{parent_id}: embedded component must be an object"
    )
    require(
        set(entry) == EMBEDDED_KEYS, f"{parent_id}: invalid embedded component keys"
    )
    embedded_id = entry["id"]
    require(
        isinstance(embedded_id, str) and ID_PATTERN.fullmatch(embedded_id),
        f"{parent_id}: invalid embedded component id",
    )
    for field in ("name", "version", "source_path", "license_expression"):
        require(
            isinstance(entry[field], str) and entry[field],
            f"{parent_id}/{embedded_id}: invalid {field}",
        )
    license_files = entry["license_files"]
    require(
        isinstance(license_files, list) and license_files,
        f"{parent_id}/{embedded_id}: license_files must not be empty",
    )
    return {
        validate_license_file(item, repo_root, f"{parent_id}/{embedded_id}")
        for item in license_files
    }


def validate_lock_document(document: dict, repo_root: Path) -> dict[str, dict]:
    require(
        set(document) == {"$schema", "schema_version", "archive_policy", "components"},
        "dependency lock has unknown or missing top-level keys",
    )
    require(
        document["$schema"] == "./dependencies.lock.schema.json",
        "dependency lock references the wrong schema",
    )
    require(
        type(document["schema_version"]) is int and document["schema_version"] == 1,
        "unsupported dependency lock version",
    )
    policy = document["archive_policy"]
    require(isinstance(policy, dict), "archive_policy must be an object")
    require(
        set(policy) == {"algorithm", "github_outer_compression"},
        "archive_policy has unknown or missing keys",
    )
    require(policy["algorithm"] == "sha256", "only SHA256 archives are supported")
    require(
        isinstance(policy["github_outer_compression"], str)
        and policy["github_outer_compression"],
        "GitHub archive policy must be documented",
    )

    components = document["components"]
    require(isinstance(components, list) and components, "components must not be empty")
    by_id: dict[str, dict] = {}
    referenced_licenses: set[str] = set()
    archives: set[str] = set()

    for component in components:
        require(isinstance(component, dict), "component must be an object")
        require(
            set(component) == COMPONENT_KEYS, "component has unknown or missing keys"
        )
        component_id = component["id"]
        require(
            isinstance(component_id, str) and ID_PATTERN.fullmatch(component_id),
            "component has invalid id",
        )
        require(component_id not in by_id, f"duplicate component id: {component_id}")
        by_id[component_id] = component
        for field in ("name", "source_repository", "license_expression"):
            require(
                isinstance(component[field], str) and component[field],
                f"{component_id}: invalid {field}",
            )
        require(
            component["source_repository"].startswith("https://"),
            f"{component_id}: source_repository must use HTTPS",
        )
        require(
            isinstance(component["license_files"], list),
            f"{component_id}: license_files must be an array",
        )
        require(
            isinstance(component["embedded_components"], list),
            f"{component_id}: embedded_components must be an array",
        )

        if component["lock_state"] == "locked":
            require(
                component["scope"] in LOCKED_SCOPES,
                f"{component_id}: invalid locked scope",
            )
            expected_integration = EXPECTED_LOCKED_INTEGRATIONS[component["scope"]]
            if component_id == "tdlib":
                expected_integration = "fetchcontent-or-pinned-prefix"
            require(
                component["integration"] == expected_integration,
                f"{component_id}: integration differs from scope",
            )
            for field in (
                "source_archive",
                "immutable_ref",
                "version",
                "archive_sha256",
            ):
                require(
                    isinstance(component[field], str) and component[field],
                    f"{component_id}: locked {field} is required",
                )
            require(
                component["source_archive"].startswith("https://"),
                f"{component_id}: source archive must use HTTPS",
            )
            require(
                SHA256_PATTERN.fullmatch(component["archive_sha256"]),
                f"{component_id}: invalid archive SHA256",
            )
            require(
                component["source_archive"] not in archives,
                f"{component_id}: duplicate source archive",
            )
            archives.add(component["source_archive"])
            if component_id in DIRECT_FETCHCONTENT:
                require(
                    COMMIT_PATTERN.fullmatch(component["immutable_ref"]),
                    f"{component_id}: FetchContent ref must be a full commit",
                )
                require(
                    component["source_archive"].endswith(
                        f"/{component['immutable_ref']}.tar.gz"
                    ),
                    f"{component_id}: source archive is not bound to its commit",
                )
            if component["scope"] in RUNTIME_SCOPES:
                require(
                    component["license_files"],
                    f"{component_id}: runtime license files are required",
                )
            else:
                require(
                    not component["license_files"],
                    f"{component_id}: test-only licenses belong outside the runtime bundle",
                )
            for license_entry in component["license_files"]:
                referenced_licenses.add(
                    validate_license_file(license_entry, repo_root, component_id)
                )
            embedded_ids: set[str] = set()
            for embedded in component["embedded_components"]:
                require(
                    isinstance(embedded, dict),
                    f"{component_id}: embedded component must be an object",
                )
                embedded_id = embedded.get("id")
                require(
                    isinstance(embedded_id, str) and ID_PATTERN.fullmatch(embedded_id),
                    f"{component_id}: invalid embedded component id",
                )
                require(
                    embedded_id not in embedded_ids,
                    f"{component_id}: duplicate embedded component id",
                )
                embedded_ids.add(embedded_id)
                referenced_licenses.update(
                    validate_embedded_component(embedded, repo_root, component_id)
                )
        elif component["lock_state"] == "unresolved":
            require(
                component_id in PLANNED_IDS,
                f"{component_id}: unexpected unresolved component",
            )
            require(
                component["scope"] == "planned-runtime",
                f"{component_id}: unresolved component must be planned-runtime",
            )
            require(
                component["integration"] == "toolchain-unselected",
                f"{component_id}: unresolved toolchain integration is invalid",
            )
            for field in (
                "source_archive",
                "immutable_ref",
                "version",
                "archive_sha256",
            ):
                require(
                    component[field] is None,
                    f"{component_id}: unresolved {field} must be null",
                )
            require(
                not component["license_files"] and not component["embedded_components"],
                f"{component_id}: unresolved component cannot claim exact notices",
            )
        else:
            raise VerificationError(f"{component_id}: invalid lock_state")

    require(
        PLANNED_IDS
        == {item for item in by_id if by_id[item]["lock_state"] == "unresolved"},
        "planned runtime set is incomplete",
    )
    require(
        set(DIRECT_FETCHCONTENT) <= set(by_id),
        "FetchContent lock entries are incomplete",
    )
    tdlib_embedded = {
        embedded["id"]: embedded for embedded in by_id["tdlib"]["embedded_components"]
    }
    require(
        set(tdlib_embedded) == {"tdlib-sqlite-sqlcipher"},
        "TDLib embedded dependency inventory is incomplete",
    )
    tdlib_sqlite = tdlib_embedded["tdlib-sqlite-sqlcipher"]
    require(
        tdlib_sqlite["license_expression"] == "blessing AND BSD-3-Clause",
        "TDLib SQLite license expression is incomplete",
    )
    require(
        {item["path"] for item in tdlib_sqlite["license_files"]}
        == {
            "release/licenses/SQLite-blessing.txt",
            "release/licenses/TDLib-SQLCipher.txt",
        },
        "TDLib SQLite license texts are incomplete",
    )
    checked_in_licenses = {
        str(item.relative_to(repo_root))
        for item in (repo_root / "release/licenses").glob("*.txt")
    }
    require(
        checked_in_licenses == referenced_licenses,
        "checked-in runtime license set differs from dependency lock",
    )
    return by_id


def extract_fetchcontent_block(cmake: str, dependency: str) -> str:
    matches = list(
        re.finditer(
            rf"FetchContent_Declare\(\s*{re.escape(dependency)}\b(?P<body>.*?)\)",
            cmake,
            flags=re.DOTALL | re.IGNORECASE,
        )
    )
    require(
        len(matches) == 1,
        f"CMake must contain exactly one FetchContent declaration for {dependency}",
    )
    return matches[0].group("body")


def validate_repo_consistency(
    repo_root: Path,
    by_id: dict[str, dict],
    cmake_file: Path,
    build_script_file: Path,
    notices_file: Path,
) -> None:
    try:
        cmake = cmake_file.read_text(encoding="utf-8")
        build_script = build_script_file.read_text(encoding="utf-8")
        notices = notices_file.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise VerificationError(
            f"cannot read repository dependency metadata: {error}"
        ) from error

    declared_dependencies = {
        dependency.lower()
        for dependency in re.findall(
            r"^[ \t]*FetchContent_Declare\(\s*([A-Za-z0-9_+-]+)\b",
            cmake,
            flags=re.MULTILINE | re.IGNORECASE,
        )
    }
    expected_declarations = set(FETCHCONTENT_DECLARATIONS.values())
    require(
        declared_dependencies == expected_declarations,
        "CMake FetchContent inventory differs from the dependency lock: "
        f"expected {sorted(expected_declarations)}, got {sorted(declared_dependencies)}",
    )

    for component_id, variable in DIRECT_FETCHCONTENT.items():
        expected = by_id[component_id]["immutable_ref"]
        pin_matches = re.findall(
            rf"(?m)^[ \t]*set\({variable}[ \t]+([0-9a-f]{{40}})\)"
            r"[ \t]*(?:#[^\n]*)?$",
            cmake,
        )
        require(
            len(pin_matches) == 1,
            f"CMake must contain exactly one active pin for {component_id}",
        )
        require(pin_matches[0] == expected, f"CMake pin differs for {component_id}")
        block = extract_fetchcontent_block(
            cmake, FETCHCONTENT_DECLARATIONS[component_id]
        )
        repository_match = re.search(r"GIT_REPOSITORY\s+(\S+)", block)
        require(
            repository_match is not None
            and repository_match.group(1) == by_id[component_id]["source_repository"],
            f"CMake source repository differs for {component_id}",
        )
        require(
            re.search(rf"GIT_TAG\s+\$\{{{variable}\}}(?:\s|$)", block) is not None,
            f"CMake FetchContent tag is not lock-bound for {component_id}",
        )
        require(
            "GIT_SHALLOW TRUE" not in block,
            f"CMake cannot shallow-fetch the exact commit for {component_id}",
        )
        require(
            re.search(
                rf"tgcli_assert_dependency_lock\(\s*{component_id}\s+\$\{{{variable}\}}\s*\)",
                cmake,
            )
            is not None,
            f"CMake lock assertion missing for {component_id}",
        )
        require(
            re.search(
                rf"tgcli_assert_resolved_git_revision\(\s*{component_id}\b.*?\$\{{{variable}\}}\s*\)",
                cmake,
                flags=re.DOTALL,
            )
            is not None,
            f"CMake resolved revision assertion missing for {component_id}",
        )

    script_matches = re.findall(
        r"(?m)^[ \t]*TDLIB_REV=([^\s#]+)(?:[ \t]+#[^\n]*)?[ \t]*$",
        build_script,
    )
    require(
        len(script_matches) == 1,
        "build-tdlib.sh must contain exactly one active TDLib pin assignment",
    )
    require(
        COMMIT_PATTERN.fullmatch(script_matches[0]) is not None,
        "build-tdlib.sh TDLib pin must be a full commit",
    )
    require(
        script_matches[0] == by_id["tdlib"]["immutable_ref"],
        "build-tdlib.sh TDLib pin differs from lock",
    )
    repository_matches = re.findall(
        r"(?m)^[ \t]*TDLIB_REPOSITORY=([^\s#]+)(?:[ \t]+#[^\n]*)?[ \t]*$",
        build_script,
    )
    require(
        len(repository_matches) == 1,
        "build-tdlib.sh must contain exactly one active TDLib repository assignment",
    )
    require(
        repository_matches[0] == by_id["tdlib"]["source_repository"],
        "build-tdlib.sh TDLib repository differs from lock",
    )

    for component_id, component in by_id.items():
        if component["scope"] in RUNTIME_SCOPES:
            require(
                f"<!-- lock-id:{component_id} -->" in notices,
                f"runtime notice marker missing for {component_id}",
            )
        elif component["scope"] == "planned-runtime":
            require(
                f"<!-- planned-lock-id:{component_id} -->" in notices,
                f"planned runtime marker missing for {component_id}",
            )
        else:
            require(
                f"<!-- lock-id:{component_id} -->" not in notices,
                f"test-only component has a runtime notice marker: {component_id}",
            )
        for license_entry in component["license_files"]:
            require(
                f"]({license_entry['path']})" in notices,
                f"notice does not link {license_entry['path']}",
            )
        for embedded in component["embedded_components"]:
            for license_entry in embedded["license_files"]:
                require(
                    f"]({license_entry['path']})" in notices,
                    f"notice does not link {license_entry['path']}",
                )


def verify_archives(by_id: dict[str, dict], selected: set[str]) -> None:
    locked = {
        component_id: component
        for component_id, component in by_id.items()
        if component["lock_state"] == "locked"
    }
    unknown = selected - set(locked)
    require(
        not unknown,
        f"cannot download unlocked or unknown components: {sorted(unknown)}",
    )
    candidates = (
        locked.items()
        if not selected
        else ((component_id, locked[component_id]) for component_id in sorted(selected))
    )
    for component_id, component in candidates:
        request = urllib.request.Request(
            component["source_archive"],
            headers={"User-Agent": "tgcli-dependency-lock-verifier/1"},
        )
        digest = hashlib.sha256()
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                while chunk := response.read(1024 * 1024):
                    digest.update(chunk)
        except (OSError, urllib.error.URLError) as error:
            raise VerificationError(
                f"{component_id}: archive download failed: {error}"
            ) from error
        require(
            digest.hexdigest() == component["archive_sha256"],
            f"{component_id}: archive SHA256 mismatch",
        )


def parse_args() -> argparse.Namespace:
    default_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Validate tgcli's dependency source lock"
    )
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument(
        "--lock-file",
        type=Path,
        help="dependency lock override used by fail-closed tests",
    )
    parser.add_argument(
        "--cmake-file",
        type=Path,
        help="CMakeLists override used by fail-closed tests",
    )
    parser.add_argument(
        "--build-script-file",
        type=Path,
        help="build-tdlib.sh override used by fail-closed tests",
    )
    parser.add_argument(
        "--notices-file",
        type=Path,
        help="third-party notices override used by fail-closed tests",
    )
    parser.add_argument(
        "--network",
        action="store_true",
        help="explicitly download and verify locked source archives",
    )
    parser.add_argument(
        "--component",
        action="append",
        default=[],
        help="with --network, verify only this locked component (repeatable)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    lock_file = (
        args.lock_file or repo_root / "release/dependencies.lock.json"
    ).resolve()
    cmake_file = (args.cmake_file or repo_root / "CMakeLists.txt").resolve()
    build_script_file = (
        args.build_script_file or repo_root / "scripts/build-tdlib.sh"
    ).resolve()
    notices_file = (args.notices_file or repo_root / "THIRD_PARTY_NOTICES.md").resolve()
    try:
        document = load_json(lock_file)
        schema = load_json(repo_root / "release/dependencies.lock.schema.json")
        require(
            schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
            "dependency lock schema must use Draft 2020-12",
        )
        by_id = validate_lock_document(document, repo_root)
        validate_repo_consistency(
            repo_root, by_id, cmake_file, build_script_file, notices_file
        )
        require(
            args.network or not args.component,
            "--component requires explicit --network mode",
        )
        if args.network:
            verify_archives(by_id, set(args.component))
    except VerificationError as error:
        print(f"dependency lock verification failed: {error}", file=sys.stderr)
        return 1
    mode = "network" if args.network else "offline"
    print(f"dependency lock verified ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
