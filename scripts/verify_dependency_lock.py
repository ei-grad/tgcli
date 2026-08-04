#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path, PurePosixPath


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

LOCKED_SCOPES = {"runtime", "release-runtime", "test-only", "build-only"}
RUNTIME_SCOPES = {"runtime", "release-runtime"}
BUILD_ONLY_SCOPE = "build-only"
RELEASE_RUNTIME_IDS = {"openssl", "zlib", "musl", "gcc-runtime"}
BUILD_INPUT_IDS = {
    "tdlib",
    "cli11",
    "nlohmann_json",
    "fmt",
    "tomlplusplus",
    "openssl",
    "zlib",
    "catch2",
    "jsoncons",
    "gperf",
    "linux-musl-toolchain",
}
SOURCE_TREE_IDS = {
    "tdlib",
    "cli11",
    "nlohmann_json",
    "fmt",
    "tomlplusplus",
    "openssl",
    "zlib",
    "catch2",
    "jsoncons",
    "gperf",
}
EXPECTED_INTEGRATIONS = {
    "tdlib": "fetchcontent-or-pinned-prefix",
    "cli11": "fetchcontent",
    "nlohmann_json": "fetchcontent",
    "fmt": "fetchcontent",
    "tomlplusplus": "fetchcontent",
    "openssl": "release-source-build",
    "zlib": "release-source-build",
    "catch2": "test-fetchcontent",
    "jsoncons": "test-fetchcontent",
    "musl": "release-toolchain-runtime",
    "gcc-runtime": "release-toolchain-runtime",
    "gperf": "release-host-tool-source",
    "linux-musl-toolchain": "release-input-artifact",
    "linux-build-image": "pinned-build-image",
}
EXPECTED_COMPONENT_IDS = set(EXPECTED_INTEGRATIONS)
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
SHA512_PATTERN = re.compile(r"^[0-9a-f]{128}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
LICENSE_PATH_PATTERN = re.compile(r"^release/licenses/[A-Za-z0-9.+_-]+\.txt$")
IMAGE_PATTERN = re.compile(r"^[^\s@]+@sha256:[0-9a-f]{64}$")

RELEASE_TOOLCHAIN_KEYS = {
    "id",
    "target",
    "image",
    "image_component",
    "toolchain_component",
    "archive_root",
    "compiler",
    "producer_build_log",
    "source_evidence",
    "runtime_files",
}
COMPILER_KEYS = {"path", "target", "version"}
BUILD_LOG_KEYS = {"path", "sha256"}
SOURCE_EVIDENCE_KEYS = {"component_id", "archive_filename", "sha512"}
ARTIFACT_FILE_KEYS = {"component_id", "path", "sha256"}

COMPONENT_REQUIRED_KEYS = {
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
    "archive_size",
    "license_expression",
    "license_files",
    "embedded_components",
}
COMPONENT_OPTIONAL_KEYS = {"generated_source_tree_sha256", "source_tree_sha256"}
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


def validate_artifact_path(value: object, owner: str) -> str:
    require(isinstance(value, str) and value, f"{owner}: artifact path is required")
    candidate = PurePosixPath(value)
    require(
        not candidate.is_absolute()
        and ".." not in candidate.parts
        and "." not in candidate.parts
        and str(candidate) == value,
        f"{owner}: unsafe artifact path",
    )
    return value


def archive_basename(component: dict) -> str:
    name = PurePosixPath(urllib.parse.urlparse(component["source_archive"]).path).name
    require(name, f"{component['id']}: source archive has no filename")
    return name


def validate_release_toolchain(document: dict, by_id: dict[str, dict]) -> dict:
    toolchains = document["release_toolchains"]
    require(
        isinstance(toolchains, list) and len(toolchains) == 1,
        "exactly one release toolchain is required",
    )
    toolchain = toolchains[0]
    require(isinstance(toolchain, dict), "release toolchain must be an object")
    require(
        set(toolchain) == RELEASE_TOOLCHAIN_KEYS,
        "release toolchain has unknown or missing keys",
    )
    require(
        toolchain["id"] == "linux-musl-x86_64",
        "unexpected release toolchain id",
    )
    require(
        toolchain["target"] == "x86_64-unknown-linux-musl",
        "unexpected release toolchain target",
    )
    require(
        isinstance(toolchain["image"], str)
        and IMAGE_PATTERN.fullmatch(toolchain["image"]),
        "release toolchain image must be pinned by SHA256 digest",
    )
    require(
        toolchain["image_component"] == "linux-build-image",
        "release toolchain image component is invalid",
    )
    require(
        toolchain["toolchain_component"] == "linux-musl-toolchain",
        "release toolchain artifact component is invalid",
    )
    require(
        by_id[toolchain["image_component"]]["integration"] == "pinned-build-image",
        "release image is not tied to its source component",
    )
    require(
        by_id[toolchain["toolchain_component"]]["integration"]
        == "release-input-artifact",
        "release toolchain is not tied to its archive component",
    )
    require(
        toolchain["archive_root"] == "x86_64-unknown-linux-musl",
        "release toolchain archive root is invalid",
    )

    compiler = toolchain["compiler"]
    require(isinstance(compiler, dict), "release compiler must be an object")
    require(set(compiler) == COMPILER_KEYS, "release compiler keys are invalid")
    require(
        validate_artifact_path(compiler["path"], "release compiler")
        == "bin/x86_64-unknown-linux-musl-g++",
        "release compiler path is invalid",
    )
    require(
        compiler["target"] == toolchain["target"],
        "release compiler target differs from toolchain target",
    )
    require(
        compiler["version"] == by_id["gcc-runtime"]["version"],
        "release compiler version differs from GCC runtime source",
    )

    build_log = toolchain["producer_build_log"]
    require(isinstance(build_log, dict), "producer build log must be an object")
    require(set(build_log) == BUILD_LOG_KEYS, "producer build log keys are invalid")
    require(
        validate_artifact_path(build_log["path"], "producer build log")
        == "build.log.bz2",
        "producer build log path is invalid",
    )
    require(
        isinstance(build_log["sha256"], str)
        and SHA256_PATTERN.fullmatch(build_log["sha256"]),
        "producer build log SHA256 is invalid",
    )

    evidence = toolchain["source_evidence"]
    require(
        isinstance(evidence, list) and len(evidence) == 2,
        "toolchain source evidence must cover musl and GCC",
    )
    evidence_ids: set[str] = set()
    for entry in evidence:
        require(isinstance(entry, dict), "source evidence must be an object")
        require(set(entry) == SOURCE_EVIDENCE_KEYS, "source evidence keys are invalid")
        component_id = entry["component_id"]
        require(
            component_id in {"musl", "gcc-runtime"}
            and component_id not in evidence_ids,
            "toolchain source evidence component set is invalid",
        )
        evidence_ids.add(component_id)
        require(
            entry["archive_filename"] == archive_basename(by_id[component_id]),
            f"{component_id}: source evidence archive differs from source lock",
        )
        require(
            isinstance(entry["sha512"], str)
            and SHA512_PATTERN.fullmatch(entry["sha512"]),
            f"{component_id}: source evidence SHA512 is invalid",
        )
    require(
        evidence_ids == {"musl", "gcc-runtime"},
        "toolchain source evidence is incomplete",
    )

    runtime_files = toolchain["runtime_files"]
    require(
        isinstance(runtime_files, list) and len(runtime_files) == 12,
        "toolchain runtime file inventory is incomplete",
    )
    runtime_names: dict[str, set[str]] = {"musl": set(), "gcc-runtime": set()}
    runtime_paths: set[str] = set()
    for entry in runtime_files:
        require(isinstance(entry, dict), "runtime file entry must be an object")
        require(set(entry) == ARTIFACT_FILE_KEYS, "runtime file keys are invalid")
        component_id = entry["component_id"]
        require(
            component_id in runtime_names,
            "runtime file refers to a non-toolchain component",
        )
        relative = validate_artifact_path(entry["path"], f"{component_id} runtime file")
        require(relative not in runtime_paths, "duplicate toolchain runtime file path")
        runtime_paths.add(relative)
        runtime_names[component_id].add(PurePosixPath(relative).name)
        require(
            isinstance(entry["sha256"], str)
            and SHA256_PATTERN.fullmatch(entry["sha256"]),
            f"{component_id}: runtime file SHA256 is invalid",
        )
    require(
        runtime_names["musl"]
        == {"libc.a", "libdl.a", "libm.a", "crt1.o", "crti.o", "crtn.o"},
        "musl runtime input inventory is incomplete",
    )
    require(
        runtime_names["gcc-runtime"]
        == {
            "libatomic.a",
            "libgcc.a",
            "libgcc_eh.a",
            "libstdc++.a",
            "crtbeginT.o",
            "crtend.o",
        },
        "GCC runtime input inventory is incomplete",
    )
    return toolchain


def validate_lock_document(
    document: dict, repo_root: Path
) -> tuple[dict[str, dict], dict]:
    require(
        set(document)
        == {
            "$schema",
            "schema_version",
            "archive_policy",
            "release_toolchains",
            "components",
        },
        "dependency lock has unknown or missing top-level keys",
    )
    require(
        document["$schema"] == "./dependencies.lock.schema.json",
        "dependency lock references the wrong schema",
    )
    require(
        type(document["schema_version"]) is int and document["schema_version"] == 2,
        "unsupported dependency lock version",
    )
    policy = document["archive_policy"]
    require(isinstance(policy, dict), "archive_policy must be an object")
    require(
        set(policy)
        == {
            "algorithm",
            "github_outer_compression",
            "max_expanded_size",
            "max_member_count",
            "max_member_size",
            "release_asset_integrity",
        },
        "archive_policy has unknown or missing keys",
    )
    require(policy["algorithm"] == "sha256", "only SHA256 archives are supported")
    for field in ("github_outer_compression", "release_asset_integrity"):
        require(
            isinstance(policy[field], str) and policy[field],
            f"archive policy {field} must be documented",
        )
    for field in ("max_expanded_size", "max_member_count", "max_member_size"):
        require(
            type(policy[field]) is int and policy[field] > 0,
            f"archive policy {field} must be a positive integer",
        )
    require(
        policy["max_member_size"] <= policy["max_expanded_size"],
        "archive per-file safety cap exceeds cumulative cap",
    )

    components = document["components"]
    require(isinstance(components, list) and components, "components must not be empty")
    by_id: dict[str, dict] = {}
    referenced_licenses: set[str] = set()
    archives: set[str] = set()

    for component in components:
        require(isinstance(component, dict), "component must be an object")
        require(
            COMPONENT_REQUIRED_KEYS <= set(component)
            and set(component) <= COMPONENT_REQUIRED_KEYS | COMPONENT_OPTIONAL_KEYS,
            "component has unknown or missing keys",
        )
        component_id = component["id"]
        require(
            isinstance(component_id, str) and ID_PATTERN.fullmatch(component_id),
            "component has invalid id",
        )
        require(
            component_id in EXPECTED_COMPONENT_IDS,
            f"unexpected dependency component: {component_id}",
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
            component["lock_state"] == "locked",
            f"{component_id}: every release component must be locked",
        )
        require(
            component["scope"] in LOCKED_SCOPES,
            f"{component_id}: invalid locked scope",
        )
        require(
            component["integration"] == EXPECTED_INTEGRATIONS[component_id],
            f"{component_id}: integration differs from the release contract",
        )
        for field in ("source_archive", "immutable_ref", "version", "archive_sha256"):
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
            type(component["archive_size"]) is int and component["archive_size"] > 0,
            f"{component_id}: invalid archive size",
        )
        if component_id in SOURCE_TREE_IDS:
            require(
                isinstance(component.get("source_tree_sha256"), str)
                and SHA256_PATTERN.fullmatch(component["source_tree_sha256"]),
                f"{component_id}: invalid source tree SHA256",
            )
        else:
            require(
                "source_tree_sha256" not in component,
                f"{component_id}: source tree digest is not a release input",
            )
        if component_id == "tdlib":
            require(
                isinstance(component.get("generated_source_tree_sha256"), str)
                and SHA256_PATTERN.fullmatch(component["generated_source_tree_sha256"]),
                "tdlib: invalid generated source tree SHA256",
            )
        else:
            require(
                "generated_source_tree_sha256" not in component,
                f"{component_id}: unexpected generated source tree digest",
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
        if component_id == "linux-build-image":
            require(
                COMMIT_PATTERN.fullmatch(component["immutable_ref"]),
                "Linux build image source ref must be a full commit",
            )
            require(
                component["source_archive"].endswith(
                    f"/{component['immutable_ref']}.tar.gz"
                ),
                "Linux build image source archive is not bound to its commit",
            )
        require(
            isinstance(component["license_files"], list),
            f"{component_id}: license_files must be an array",
        )
        require(
            isinstance(component["embedded_components"], list),
            f"{component_id}: embedded_components must be an array",
        )
        if component["scope"] == "test-only":
            require(
                not component["license_files"],
                f"{component_id}: test-only licenses belong outside the release bundle",
            )
        else:
            require(
                component["license_files"],
                f"{component_id}: locked license files are required",
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

    require(
        set(by_id) == EXPECTED_COMPONENT_IDS,
        "dependency component inventory is incomplete",
    )
    require(
        {
            component_id
            for component_id, item in by_id.items()
            if item["scope"] == "release-runtime"
        }
        == RELEASE_RUNTIME_IDS,
        "release runtime component inventory is incomplete",
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
        "checked-in license set differs from dependency lock",
    )
    toolchain = validate_release_toolchain(document, by_id)
    return by_id, toolchain


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
            require(
                f"<!-- build-lock-id:{component_id} -->" not in notices,
                f"runtime component has a build-only marker: {component_id}",
            )
        elif component["scope"] == BUILD_ONLY_SCOPE:
            require(
                f"<!-- build-lock-id:{component_id} -->" in notices,
                f"build-only notice marker missing for {component_id}",
            )
            require(
                f"<!-- lock-id:{component_id} -->" not in notices,
                f"build-only component has a runtime marker: {component_id}",
            )
        else:
            require(
                f"<!-- lock-id:{component_id} -->" not in notices
                and f"<!-- build-lock-id:{component_id} -->" not in notices,
                f"test-only component has a release notice marker: {component_id}",
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


def sha256_file(file: Path) -> str:
    digest = hashlib.sha256()
    try:
        with file.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise VerificationError(f"cannot read {file}: {error}") from error
    return digest.hexdigest()


def require_regular_file(file: Path, owner: str) -> None:
    require(file.is_file(), f"{owner}: missing {file}")
    require(not file.is_symlink(), f"{owner}: file cannot be a symlink: {file}")


def validate_release_contract(
    repo_root: Path,
    lock_file: Path,
    by_id: dict[str, dict],
    release_toolchain: dict,
    contract_file: Path,
    release_build_script: Path,
) -> None:
    require_regular_file(contract_file, "Linux toolchain contract")
    require_regular_file(release_build_script, "Linux release build recipe")
    contract = load_json(contract_file)
    require(
        set(contract)
        == {
            "dependency_lock_sha256",
            "image",
            "openssl",
            "recipe",
            "schema_version",
            "target",
            "tdlib_revision",
            "zlib",
        },
        "Linux toolchain contract has unknown or missing keys",
    )
    require(
        type(contract["schema_version"]) is int and contract["schema_version"] == 1,
        "unsupported Linux toolchain contract version",
    )
    require(
        contract["dependency_lock_sha256"] == sha256_file(lock_file),
        "Linux toolchain contract dependency lock digest differs",
    )
    require(
        contract["image"] == release_toolchain["image"],
        "Linux toolchain contract image differs from dependency lock",
    )
    require(
        contract["target"] == "x86_64-linux-musl",
        "Linux toolchain contract target is invalid",
    )
    require(
        contract["tdlib_revision"] == by_id["tdlib"]["immutable_ref"],
        "Linux toolchain contract TDLib revision differs",
    )

    recipe = contract["recipe"]
    require(isinstance(recipe, dict), "Linux toolchain recipe must be an object")
    require(
        set(recipe) == {"path", "sha256"}, "Linux toolchain recipe keys are invalid"
    )
    require(
        recipe["path"] == "scripts/release/build-linux-musl.sh",
        "Linux toolchain recipe path is invalid",
    )
    expected_script = (repo_root / recipe["path"]).resolve()
    require(
        release_build_script == expected_script
        or release_build_script.name == expected_script.name,
        "Linux toolchain recipe override has an unexpected filename",
    )
    require(
        isinstance(recipe["sha256"], str)
        and SHA256_PATTERN.fullmatch(recipe["sha256"])
        and recipe["sha256"] == sha256_file(release_build_script),
        "Linux toolchain recipe digest differs",
    )

    for component_id in ("openssl", "zlib"):
        entry = contract[component_id]
        require(
            isinstance(entry, dict)
            and set(entry) == {"source_sha256", "source_url", "version"},
            f"Linux toolchain {component_id} entry is invalid",
        )
        component = by_id[component_id]
        require(
            entry["version"] == component["version"]
            and entry["source_url"] == component["source_archive"]
            and entry["source_sha256"] == component["archive_sha256"],
            f"Linux toolchain {component_id} entry differs from dependency lock",
        )


def staged_archive_name(component_id: str, component: dict) -> str:
    source_name = archive_basename(component)
    for suffix in (".tar.gz", ".tar.xz"):
        if source_name.endswith(suffix):
            return f"{component_id}{suffix}"
    raise VerificationError(
        f"{component_id}: source archive must be a .tar.gz or .tar.xz file"
    )


def verify_staged_archive(file: Path, component_id: str, component: dict) -> None:
    require_regular_file(file, component_id)
    require(
        file.stat().st_size == component["archive_size"],
        f"{component_id}: archive size mismatch",
    )
    require(
        sha256_file(file) == component["archive_sha256"],
        f"{component_id}: archive SHA256 mismatch",
    )


def stream_archive(
    component_id: str, component: dict, destination: Path | None
) -> None:
    request = urllib.request.Request(
        component["source_archive"],
        headers={"User-Agent": "tgcli-dependency-lock-verifier/2"},
    )
    digest = hashlib.sha256()
    output = None
    downloaded = 0
    try:
        if destination is not None:
            output = destination.open("xb")
        with urllib.request.urlopen(request, timeout=60) as response:
            content_length = response.headers.get("Content-Length")
            if content_length is not None:
                try:
                    parsed_length = int(content_length)
                except ValueError as error:
                    raise VerificationError(
                        f"{component_id}: invalid archive Content-Length"
                    ) from error
                require(
                    parsed_length == component["archive_size"],
                    f"{component_id}: archive Content-Length mismatch",
                )
            while chunk := response.read(1024 * 1024):
                downloaded += len(chunk)
                require(
                    downloaded <= component["archive_size"],
                    f"{component_id}: archive download exceeds locked size",
                )
                digest.update(chunk)
                if output is not None:
                    output.write(chunk)
    except (OSError, urllib.error.URLError) as error:
        raise VerificationError(
            f"{component_id}: archive download failed: {error}"
        ) from error
    finally:
        if output is not None:
            output.close()
    require(
        downloaded == component["archive_size"],
        f"{component_id}: archive size mismatch",
    )
    require(
        digest.hexdigest() == component["archive_sha256"],
        f"{component_id}: archive SHA256 mismatch",
    )


def verify_archives(
    by_id: dict[str, dict],
    selected: set[str],
    *,
    archive_directory: Path | None = None,
    download_directory: Path | None = None,
) -> None:
    unknown = selected - set(by_id)
    action = "verify" if archive_directory is not None else "download"
    require(
        not unknown,
        f"cannot {action} unknown components: {sorted(unknown)}",
    )
    candidates = (
        by_id.items()
        if not selected
        else ((component_id, by_id[component_id]) for component_id in sorted(selected))
    )
    if archive_directory is not None:
        require(
            archive_directory.is_dir() and not archive_directory.is_symlink(),
            f"archive directory is missing or unsafe: {archive_directory}",
        )
    if download_directory is not None:
        try:
            download_directory.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise VerificationError(
                f"cannot create archive download directory: {error}"
            ) from error
        require(
            download_directory.is_dir() and not download_directory.is_symlink(),
            f"archive download directory is unsafe: {download_directory}",
        )

    for component_id, component in candidates:
        filename = staged_archive_name(component_id, component)
        if archive_directory is not None:
            verify_staged_archive(archive_directory / filename, component_id, component)
            continue
        if download_directory is None:
            stream_archive(component_id, component, None)
            continue

        destination = download_directory / filename
        if destination.exists() or destination.is_symlink():
            verify_staged_archive(destination, component_id, component)
            continue
        partial = download_directory / f".{filename}.partial-{os.getpid()}"
        require(
            not partial.exists() and not partial.is_symlink(),
            f"{component_id}: partial archive path already exists",
        )
        try:
            stream_archive(component_id, component, partial)
            try:
                partial.replace(destination)
            except OSError as error:
                raise VerificationError(
                    f"{component_id}: cannot finalize archive download: {error}"
                ) from error
            verify_staged_archive(destination, component_id, component)
        except (OSError, VerificationError):
            if partial.is_file() and not partial.is_symlink():
                try:
                    partial.unlink()
                except OSError as error:
                    raise VerificationError(
                        f"{component_id}: cannot clean partial archive: {error}"
                    ) from error
            raise


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
        "--contract-file",
        type=Path,
        help="Linux toolchain contract override used by fail-closed tests",
    )
    parser.add_argument(
        "--release-build-script-file",
        type=Path,
        help="Linux release recipe override used by fail-closed tests",
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
        help="verify only this locked component (repeatable)",
    )
    parser.add_argument(
        "--build-inputs",
        action="store_true",
        help="verify the complete Linux offline-build input archive set",
    )
    parser.add_argument(
        "--archive-directory",
        type=Path,
        help="verify staged archives without network access",
    )
    parser.add_argument(
        "--download-directory",
        type=Path,
        help="with --network, retain verified archives in this directory",
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
    contract_file = (
        args.contract_file or repo_root / "release/linux-musl-toolchain.json"
    ).resolve()
    release_build_script = (
        args.release_build_script_file
        or repo_root / "scripts/release/build-linux-musl.sh"
    ).resolve()
    archive_directory = (
        args.archive_directory.absolute() if args.archive_directory else None
    )
    download_directory = (
        args.download_directory.absolute() if args.download_directory else None
    )
    try:
        document = load_json(lock_file)
        schema = load_json(repo_root / "release/dependencies.lock.schema.json")
        require(
            schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
            "dependency lock schema must use Draft 2020-12",
        )
        by_id, release_toolchain = validate_lock_document(document, repo_root)
        validate_repo_consistency(
            repo_root, by_id, cmake_file, build_script_file, notices_file
        )
        validate_release_contract(
            repo_root,
            lock_file,
            by_id,
            release_toolchain,
            contract_file,
            release_build_script,
        )
        require(
            not (args.network and archive_directory is not None),
            "--network and --archive-directory are mutually exclusive",
        )
        require(
            download_directory is None or args.network,
            "--download-directory requires explicit --network mode",
        )
        require(
            not (args.component and args.build_inputs),
            "--component and --build-inputs are mutually exclusive",
        )
        selected = BUILD_INPUT_IDS if args.build_inputs else set(args.component)
        require(
            args.network or archive_directory is not None or not selected,
            "archive selection requires --network or --archive-directory",
        )
        if args.network or archive_directory is not None:
            verify_archives(
                by_id,
                selected,
                archive_directory=archive_directory,
                download_directory=download_directory,
            )
    except VerificationError as error:
        print(f"dependency lock verification failed: {error}", file=sys.stderr)
        return 1
    if args.network:
        mode = "network+staged" if download_directory is not None else "network"
    elif archive_directory is not None:
        mode = "offline+staged"
    else:
        mode = "offline"
    print(f"dependency lock verified ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
