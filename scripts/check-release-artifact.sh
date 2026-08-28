#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf '%s\n' "$1" >&2
    return 1
}

classify_linux() {
    local file_output="$1"
    local program_headers="$2"
    local dynamic_section="$3"
    local version_info="$4"

    if [[ "$file_output" != *ELF* || \
          ("$file_output" != *'statically linked'* && \
           "$file_output" != *'static-pie linked'*) ]]; then
        fail "Linux release binary is not a statically linked ELF"
        return
    fi
    if grep -Fq ' INTERP ' <<< "$program_headers"; then
        fail "Linux release binary contains an ELF interpreter"
        return
    fi
    if grep -Fq '(NEEDED)' <<< "$dynamic_section"; then
        fail "Linux release binary contains a dynamic dependency"
        return
    fi
    if grep -Eq 'GLIBC(_|XX_)' <<< "$version_info"; then
        fail "Linux release binary contains a glibc symbol-version reference"
        return
    fi

    printf '%s\n' 'static-elf-without-interpreter-or-dynamic-dependencies'
}

inspect_linux() {
    local binary="$1"
    local file_tool="${FILE_TOOL:-file}"
    local readelf_tool="${READELF_TOOL:-readelf}"
    local file_output
    local program_headers
    local dynamic_section
    local version_info

    if [[ ! -f "$binary" || ! -x "$binary" || -L "$binary" ]]; then
        fail "Linux release artifact is not an executable regular file"
        return
    fi
    if ! file_output="$("$file_tool" "$binary" 2>&1)"; then
        fail "file failed while inspecting the Linux release artifact"
        return
    fi
    if ! program_headers="$("$readelf_tool" -lW "$binary" 2>&1)"; then
        fail "readelf failed while reading Linux program headers"
        return
    fi
    if ! dynamic_section="$("$readelf_tool" -dW "$binary" 2>&1)"; then
        fail "readelf failed while reading the Linux dynamic section"
        return
    fi
    if ! version_info="$("$readelf_tool" --version-info "$binary" 2>&1)"; then
        fail "readelf failed while reading Linux symbol versions"
        return
    fi

    classify_linux "$file_output" "$program_headers" "$dynamic_section" "$version_info"
}

classify_macos_dependencies() {
    local otool_output="$1"
    local unexpected

    if ! unexpected="$(
        awk '
            /^[^[:space:]].*:[[:space:]]*$/ {
                header_seen = 1
                next
            }
            /^[[:space:]]*$/ { next }
            /^[[:space:]]+/ {
                if (!header_seen || NF < 1) {
                    exit 2
                }
                dependency = $1
                if (dependency !~ "^/usr/lib/" && dependency !~ "^/System/Library/") {
                    print dependency
                }
                next
            }
            { exit 3 }
            END {
                if (!header_seen) {
                    exit 4
                }
            }
        ' <<< "$otool_output"
    )"; then
        printf '%s\n' "$otool_output" >&2
        fail "cannot parse the complete otool dependency report"
        return
    fi
    if [[ -n "$unexpected" ]]; then
        printf '%s\n' "$otool_output" >&2
        fail "macOS release binary has a non-system runtime dependency"
        return
    fi
}

inspect_macos() {
    local binary="$1"
    local otool_tool="${OTOOL_TOOL:-otool}"
    local otool_output

    if [[ ! -f "$binary" || ! -x "$binary" || -L "$binary" ]]; then
        fail "macOS release artifact is not an executable regular file"
        return
    fi
    if ! otool_output="$("$otool_tool" -L "$binary" 2>&1)"; then
        fail "otool failed while inspecting macOS runtime dependencies"
        return
    fi
    classify_macos_dependencies "$otool_output"
}

inspect_macos_universal() {
    local binary="$1"
    local lipo_tool="${LIPO_TOOL:-lipo}"
    local archs

    if [[ ! -f "$binary" || ! -x "$binary" || -L "$binary" ]]; then
        fail "macOS release artifact is not an executable regular file"
        return
    fi
    if ! archs="$("$lipo_tool" -archs "$binary" 2>&1)"; then
        fail "lipo failed while inspecting the universal binary"
        return
    fi
    if [[ " $archs " != *' arm64 '* || " $archs " != *' x86_64 '* ]]; then
        fail "universal binary does not contain both required architectures"
        return
    fi
    if [[ "$(wc -w <<< "$archs" | tr -d ' ')" != 2 ]]; then
        fail "universal binary contains an unexpected architecture"
        return
    fi
    inspect_macos "$binary"
}

verify_linux_build() {
    local binary="$1"
    local provenance="$2"
    local sbom="$3"
    local source_sha="$4"
    local source_tree="$5"
    local toolchain_image="$6"
    local repo_root="${7:-$(cd "$(dirname "$0")/.." && pwd)}"
    local sbom_tool

    sbom_tool="$(cd "$(dirname "$0")" && pwd)/release/build_provenance.py"

    python3 - \
        "$binary" \
        "$provenance" \
        "$sbom" \
        "$source_sha" \
        "$source_tree" \
        "$toolchain_image" \
        "$repo_root" <<'PY'
import hashlib
import json
import pathlib
import sys


def fail(message: str) -> None:
    raise SystemExit(f"Linux release build verification failed: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def load_json(file: pathlib.Path, owner: str) -> dict:
    require(file.is_file(), f"{owner} is missing")
    require(not file.is_symlink(), f"{owner} cannot be a symlink")
    try:
        document = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot parse {owner}: {error}")
    require(isinstance(document, dict), f"{owner} must be a JSON object")
    return document


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with file.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        fail(f"cannot hash {file}: {error}")
    return digest.hexdigest()


binary_input = pathlib.Path(sys.argv[1])
provenance_input = pathlib.Path(sys.argv[2])
sbom_input = pathlib.Path(sys.argv[3])
source_sha = sys.argv[4]
source_tree = sys.argv[5]
toolchain_image = sys.argv[6]
repo_root = pathlib.Path(sys.argv[7]).resolve()
lock_file = repo_root / "release/dependencies.lock.json"
contract_file = repo_root / "release/linux-musl-toolchain.json"
recipe_file = repo_root / "scripts/release/build-linux-musl.sh"

require(binary_input.is_file(), "artifact is missing")
require(not binary_input.is_symlink(), "artifact cannot be a symlink")
require(provenance_input.is_file(), "Linux build provenance is missing")
require(not provenance_input.is_symlink(), "Linux build provenance cannot be a symlink")
require(sbom_input.is_file(), "Linux SBOM is missing")
require(not sbom_input.is_symlink(), "Linux SBOM cannot be a symlink")
binary = binary_input.resolve()
provenance_file = provenance_input.resolve()
require(binary.stat().st_mode & 0o111 != 0, "artifact is not executable")
require(len(source_sha) == 40 and all(c in "0123456789abcdef" for c in source_sha),
        "source commit is not an exact Git object id")
require(len(source_tree) == 40 and all(c in "0123456789abcdef" for c in source_tree),
        "source tree is not an exact Git object id")
require(
    toolchain_image.count("@sha256:") == 1
    and len(toolchain_image.rsplit("@sha256:", 1)[1]) == 64
    and all(c in "0123456789abcdef" for c in toolchain_image.rsplit("@sha256:", 1)[1]),
    "toolchain image is not pinned by digest",
)

lock = load_json(lock_file, "dependency lock")
contract = load_json(contract_file, "Linux toolchain contract")
provenance = load_json(provenance_file, "Linux build provenance")
require(recipe_file.is_file() and not recipe_file.is_symlink(),
        "Linux build recipe is missing or unsafe")

lock_sha256 = sha256_file(lock_file)
contract_sha256 = sha256_file(contract_file)
recipe_sha256 = sha256_file(recipe_file)
artifact_sha256 = sha256_file(binary)
artifact_size = binary.stat().st_size

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
    }
    and contract["schema_version"] == 1,
    "Linux toolchain contract schema is invalid",
)
require(contract["dependency_lock_sha256"] == lock_sha256,
        "toolchain contract does not bind the dependency lock")
require(contract["image"] == toolchain_image,
        "toolchain image differs from the release contract")
require(contract["target"] == "x86_64-linux-musl",
        "toolchain contract has an unexpected target")
require(
    contract.get("recipe")
    == {"path": "scripts/release/build-linux-musl.sh", "sha256": recipe_sha256},
    "toolchain contract does not bind the release recipe",
)

components = lock.get("components")
require(isinstance(components, list), "dependency lock components are missing")
tdlib = [component for component in components if component.get("id") == "tdlib"]
require(len(tdlib) == 1 and tdlib[0].get("immutable_ref") == contract["tdlib_revision"],
        "toolchain contract does not bind the locked TDLib revision")

expected_keys = {
    "artifact",
    "dependency_lock_sha256",
    "inspection",
    "recipe_sha256",
    "re2_build",
    "release_contract_sha256",
    "resolved_dependencies",
    "runtime_selection",
    "schema_version",
    "source",
    "source_sha",
    "tests",
    "tool_versions",
    "toolchain_image",
}
require(set(provenance) == expected_keys and provenance["schema_version"] == 3,
        "build provenance schema is invalid")
expected_artifact = {
    "path": binary.name,
    "sha256": artifact_sha256,
    "size": artifact_size,
}
require(provenance["artifact"] == expected_artifact,
        "build provenance does not identify the artifact")
require(provenance["dependency_lock_sha256"] == lock_sha256,
        "build provenance does not bind the dependency lock")
require(provenance["release_contract_sha256"] == contract_sha256,
        "build provenance does not bind the toolchain contract")
require(provenance["recipe_sha256"] == recipe_sha256,
        "build provenance does not bind the release recipe")
require(provenance["toolchain_image"] == toolchain_image,
        "build provenance does not bind the toolchain image")
require(provenance["source_sha"] == source_sha,
        "build provenance has an unexpected source commit")
require(
    provenance["source"]
    == {"commit": source_sha, "tree": source_tree, "type": "git"},
    "build provenance has an unexpected source identity",
)

inspection = provenance["inspection"]
require(
    isinstance(inspection, dict)
    and set(inspection) == {"artifact", "checks", "commands", "schema_version"}
    and inspection["schema_version"] == 1
    and inspection["artifact"] == expected_artifact
    and isinstance(inspection["checks"], dict)
    and inspection["checks"]
    and all(value is True for value in inspection["checks"].values())
    and isinstance(inspection["commands"], list)
    and len(inspection["commands"]) == 4,
    "embedded artifact inspection evidence is invalid",
)

expected_dependencies = sorted(
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
require(provenance["resolved_dependencies"] == expected_dependencies,
        "build provenance dependency set differs from the lock")

toolchains = lock.get("release_toolchains")
require(isinstance(toolchains, list) and len(toolchains) == 1,
        "dependency lock must contain one release toolchain")
locked_runtime = toolchains[0].get("runtime_files")
require(isinstance(locked_runtime, list) and locked_runtime,
        "locked runtime inventory is missing")
expected_runtime = sorted(
    (
        {
            "component_id": item["component_id"],
            "path": item["path"],
            "sha256": item["sha256"],
        }
        for item in locked_runtime
    ),
    key=lambda item: pathlib.PurePosixPath(item["path"]).name,
)
runtime_selection = provenance["runtime_selection"]
require(
    isinstance(runtime_selection, dict)
    and runtime_selection.get("schema_version") == 1
    and runtime_selection.get("selected_runtime_files") == expected_runtime,
    "build provenance runtime selection differs from the lock",
)

tests = provenance["tests"]
require(
    isinstance(tests, dict)
    and tests.get("passed") is True
    and isinstance(tests.get("argv"), list)
    and tests["argv"]
    and all(isinstance(argument, str) and argument for argument in tests["argv"]),
    "build provenance test evidence is incomplete",
)
tool_versions = provenance["tool_versions"]
require(
    isinstance(tool_versions, dict)
    and set(tool_versions) == {"cmake", "compiler", "ninja"}
    and all(isinstance(value, str) and value for value in tool_versions.values()),
    "build provenance tool versions are incomplete",
)
PY
    python3 "$sbom_tool" verify-sbom \
        --artifact "$binary" \
        --lock "$repo_root/release/dependencies.lock.json" \
        --provenance "$provenance" \
        --platform linux-x86_64-musl \
        --sbom "$sbom"
}

verify_macos_build() {
    python3 - "$@" <<'PY'
import hashlib
import json
import pathlib
import sys


def fail(message: str) -> None:
    raise SystemExit(f"macOS release build verification failed: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def regular_file(raw: str, owner: str) -> pathlib.Path:
    candidate = pathlib.Path(raw)
    require(candidate.is_file(), f"{owner} is missing")
    require(not candidate.is_symlink(), f"{owner} cannot be a symlink")
    return candidate.resolve()


def load_json(file: pathlib.Path, owner: str) -> dict:
    try:
        document = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot parse {owner}: {error}")
    require(isinstance(document, dict), f"{owner} must be a JSON object")
    return document


require(len(sys.argv) in {8, 12, 14}, "invalid macOS build verification arguments")
artifact = regular_file(sys.argv[1], "macOS artifact")
provenance_file = regular_file(sys.argv[2], "macOS provenance")
sbom_file = regular_file(sys.argv[3], "macOS SBOM")
platform = sys.argv[4]
source_sha = sys.argv[5]
source_tree = sys.argv[6]
lock_file = regular_file(sys.argv[7], "dependency lock")
require(platform in {"macos-arm64", "macos-x86_64", "macos-universal"},
        "unexpected macOS platform")
require(len(source_sha) == 40 and all(c in "0123456789abcdef" for c in source_sha),
        "source commit is not an exact Git object id")
require(len(source_tree) == 40 and all(c in "0123456789abcdef" for c in source_tree),
        "source tree is not an exact Git object id")
require((platform == "macos-universal") == (len(sys.argv) in {12, 14}),
        "universal slice evidence arguments are incomplete")

lock = load_json(lock_file, "dependency lock")
provenance = load_json(provenance_file, "macOS provenance")
if platform == "macos-universal":
    expected_provenance_keys = {
        "artifact",
        "dependency_lock",
        "platform",
        "runner",
        "schema_version",
        "slices",
        "source",
        "tools",
    }
    expected_schema_version = 1
else:
    expected_provenance_keys = {
        "artifact",
        "dependency_lock",
        "locked_openssl",
        "platform",
        "re2_build",
        "recipes",
        "resolved_dependencies",
        "runner",
        "schema_version",
        "source",
        "tools",
    }
    expected_schema_version = 2
require(
    set(provenance) == expected_provenance_keys
    and provenance["schema_version"] == expected_schema_version,
    "macOS provenance schema is invalid",
)
artifact_identity = {
    "path": artifact.name,
    "sha256": sha256_file(artifact),
    "size": artifact.stat().st_size,
}
require(provenance.get("artifact") == artifact_identity,
        "provenance does not identify the macOS artifact")
source = provenance.get("source")
require(
    isinstance(source, dict)
    and source.get("sha") == source_sha
    and source.get("tree") == source_tree
    and type(source.get("epoch")) is int
    and source["epoch"] > 0,
    "provenance has an unexpected source identity",
)
require(provenance.get("platform") == platform,
        "provenance has an unexpected macOS platform")
dependency_lock = provenance.get("dependency_lock")
require(
    isinstance(dependency_lock, dict)
    and dependency_lock.get("sha256") == sha256_file(lock_file)
    and dependency_lock.get("value") == lock,
    "provenance does not bind the dependency lock",
)

if platform == "macos-universal":
    if len(sys.argv) == 14:
        arm64_binary = regular_file(sys.argv[8], "arm64 slice")
        arm64_provenance = regular_file(sys.argv[9], "arm64 slice provenance")
        arm64_sbom = regular_file(sys.argv[10], "arm64 slice SBOM")
        x86_64_binary = regular_file(sys.argv[11], "x86_64 slice")
        x86_64_provenance = regular_file(sys.argv[12], "x86_64 slice provenance")
        x86_64_sbom = regular_file(sys.argv[13], "x86_64 slice SBOM")
        expected_slices = {
            "arm64": {
                "binary_sha256": sha256_file(arm64_binary),
                "provenance_sha256": sha256_file(arm64_provenance),
                "sbom_sha256": sha256_file(arm64_sbom),
            },
            "x86_64": {
                "binary_sha256": sha256_file(x86_64_binary),
                "provenance_sha256": sha256_file(x86_64_provenance),
                "sbom_sha256": sha256_file(x86_64_sbom),
            },
        }
    else:
        package_root = artifact.parent
        require(
            artifact.name == "tgcli"
            and provenance_file == package_root / "PROVENANCE.json"
            and sbom_file == package_root / "SBOM.json"
            and lock_file == package_root / "release/dependencies.lock.json",
            "packaged universal verification requires canonical package-root files",
        )

        packaged_slices = {}
        packaged_arguments = {
            "arm64": (sys.argv[8], sys.argv[9]),
            "x86_64": (sys.argv[10], sys.argv[11]),
        }
        for arch, (provenance_argument, sbom_argument) in packaged_arguments.items():
            slice_provenance = regular_file(
                provenance_argument, f"{arch} packaged provenance"
            )
            slice_sbom_file = regular_file(sbom_argument, f"{arch} packaged SBOM")
            require(
                slice_provenance == package_root / f"PROVENANCE-{arch}.json"
                and slice_sbom_file == package_root / f"SBOM-{arch}.json",
                "packaged slice evidence must use canonical package-root files",
            )
            slice_provenance_document = load_json(
                slice_provenance, f"{arch} packaged provenance"
            )
            slice_sbom = load_json(slice_sbom_file, f"{arch} packaged SBOM")
            require(
                set(slice_provenance_document)
                == {
                    "artifact",
                    "dependency_lock",
                    "locked_openssl",
                    "platform",
                    "re2_build",
                    "recipes",
                    "resolved_dependencies",
                    "runner",
                    "schema_version",
                    "source",
                    "tools",
                }
                and slice_provenance_document["schema_version"] == 2
                and slice_provenance_document.get("platform") == f"macos-{arch}"
                and slice_provenance_document.get("artifact") == slice_sbom.get("artifact")
                and slice_sbom.get("platform") == f"macos-{arch}"
                and slice_sbom.get("provenance")
                == {
                    "sha256": sha256_file(slice_provenance),
                    "size": slice_provenance.stat().st_size,
                },
                "packaged slice SBOM and provenance identities differ",
            )
            slice_source = slice_provenance_document.get("source")
            slice_lock = slice_provenance_document.get("dependency_lock")
            require(
                isinstance(slice_source, dict)
                and slice_source.get("sha") == source_sha
                and slice_source.get("tree") == source_tree
                and isinstance(slice_lock, dict)
                and slice_lock.get("sha256") == sha256_file(lock_file)
                and slice_lock.get("value") == lock,
                "packaged slice provenance differs from the universal source or lock",
            )
            packaged_slices[arch] = {
                "binary_sha256": slice_sbom["artifact"]["sha256"],
                "provenance_sha256": sha256_file(slice_provenance),
                "sbom_sha256": sha256_file(slice_sbom_file),
            }
        expected_slices = packaged_slices
    require(provenance.get("slices") == expected_slices,
            "universal provenance does not bind both architecture slices")
else:
    openssl = [
        component
        for component in lock.get("components", [])
        if isinstance(component, dict) and component.get("id") == "openssl"
    ]
    require(len(openssl) == 1, "dependency lock has no unique OpenSSL component")
    locked = openssl[0]
    expected_openssl = {
        "archive_sha256": locked["archive_sha256"],
        "archive_size": locked["archive_size"],
        "id": "openssl",
        "source_tree_sha256": locked["source_tree_sha256"],
        "source_url": locked["source_archive"],
        "version": locked["version"],
    }
    require(provenance.get("locked_openssl") == expected_openssl,
            "architecture provenance does not bind locked OpenSSL")
PY
    local sbom_tool
    sbom_tool="$(cd "$(dirname "$0")" && pwd)/release/build_provenance.py"
    if [[ "$4" == macos-universal && "$#" -eq 13 ]]; then
        python3 "$sbom_tool" verify-sbom \
            --artifact "$1" \
            --lock "$7" \
            --provenance "$2" \
            --platform "$4" \
            --slice-sbom "macos-arm64=${10}" \
            --slice-sbom "macos-x86_64=${13}" \
            --sbom "$3"
    elif [[ "$4" == macos-universal ]]; then
        python3 "$sbom_tool" verify-sbom \
            --artifact "$1" \
            --lock "$7" \
            --provenance "$2" \
            --platform "$4" \
            --slice-sbom "macos-arm64=$9" \
            --slice-sbom "macos-x86_64=${11}" \
            --sbom "$3"
    else
        python3 "$sbom_tool" verify-sbom \
            --artifact "$1" \
            --lock "$7" \
            --provenance "$2" \
            --platform "$4" \
            --sbom "$3"
    fi
}

classify_release_state() {
    local version="$1"
    local directory="$2"
    local tag="$3"
    local source_sha="$4"
    local release_json="$5"

    python3 - "$version" "$directory" "$tag" "$source_sha" "$release_json" <<'PY'
import hashlib
import json
import pathlib
import sys


def fail(message: str) -> None:
    raise SystemExit(f"release state verification failed: {message}")


def sha256_file(file: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


version, directory_raw, tag, source_sha, release_json_raw = sys.argv[1:]
directory = pathlib.Path(directory_raw)
release_json = pathlib.Path(release_json_raw)
if release_json.is_symlink() or not release_json.is_file():
    fail("release state input is missing or unsafe")
try:
    release = json.loads(release_json.read_text(encoding="utf-8"))
except (OSError, UnicodeError, json.JSONDecodeError) as error:
    fail(f"cannot parse release state: {error}")
if not isinstance(release, dict):
    fail("release state must be an object")
if (
    release.get("tag_name") != tag
    or release.get("name") != tag
    or release.get("target_commitish") != source_sha
    or release.get("prerelease") is not False
    or type(release.get("draft")) is not bool
):
    fail("existing release metadata differs from this release")

expected_names = {
    "SHA256SUMS",
    "SHA256SUMS.sigstore.json",
    f"tgcli-{version}-linux-x86_64-musl.tar.gz",
    f"tgcli-{version}-linux-x86_64-musl.tar.gz.sigstore.json",
    f"tgcli-{version}-macos-universal.tar.gz",
    f"tgcli-{version}-macos-universal.tar.gz.sigstore.json",
}
expected = {}
for name in expected_names:
    file = directory / name
    if file.is_symlink() or not file.is_file():
        fail(f"local signed release asset is missing or unsafe: {name}")
    expected[name] = {
        "digest": f"sha256:{sha256_file(file)}",
        "size": file.stat().st_size,
    }

assets = release.get("assets")
if not isinstance(assets, list):
    fail("existing release asset list is invalid")
observed = {}
for asset in assets:
    if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
        fail("existing release contains an invalid asset")
    name = asset["name"]
    if name in observed or name not in expected:
        fail(f"existing release contains an unexpected asset: {name}")
    if (
        asset.get("state") != "uploaded"
        or asset.get("digest") != expected[name]["digest"]
        or asset.get("size") != expected[name]["size"]
    ):
        fail(f"existing release asset differs from local content: {name}")
    observed[name] = asset

missing = sorted(expected_names - set(observed))
if release["draft"] is False and missing:
    fail("published release is missing signed assets")
print("draft" if release["draft"] else "published")
if missing:
    print("\n".join(missing))
PY
}

verify_remote_tag() {
    local source_sha="$1"
    local repository="$2"
    local tag="$3"
    local gh_tool="${GH_TOOL:-gh}"
    local jq_tool="${JQ_TOOL:-jq}"
    local object
    local object_sha
    local object_type
    local tag_object_sha
    local depth=0

    if [[ ! "$source_sha" =~ ^[0-9a-f]{40}$ ]]; then
        fail "remote tag verification requires an exact source commit"
        return
    fi
    if [[ ! "$repository" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
        fail "remote tag verification received an unsafe repository name"
        return
    fi
    if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        fail "remote tag verification requires an exact release tag"
        return
    fi

    if ! object="$("$gh_tool" api "repos/$repository/git/ref/tags/$tag")"; then
        fail "cannot retrieve the remote release tag"
        return
    fi
    if ! object_sha="$("$jq_tool" -er '.object.sha' <<< "$object")" || \
       ! object_type="$("$jq_tool" -er '.object.type' <<< "$object")"; then
        fail "remote release tag response is invalid"
        return
    fi
    if [[ ! "$object_sha" =~ ^[0-9a-f]{40}$ || \
          ("$object_type" != tag && "$object_type" != commit) ]]; then
        fail "remote release tag target is invalid"
        return
    fi
    while [[ "$object_type" == tag ]]; do
        if [[ "$depth" -ge 8 ]]; then
            fail "remote release tag exceeds the supported annotation depth"
            return
        fi
        if ! object="$("$gh_tool" api "repos/$repository/git/tags/$object_sha")"; then
            fail "cannot peel the remote annotated release tag"
            return
        fi
        if ! tag_object_sha="$("$jq_tool" -er '.sha' <<< "$object")" || \
           [[ "$tag_object_sha" != "$object_sha" ]] || \
           ! object_sha="$("$jq_tool" -er '.object.sha' <<< "$object")" || \
           ! object_type="$("$jq_tool" -er '.object.type' <<< "$object")"; then
            fail "remote annotated release tag response is invalid"
            return
        fi
        if [[ ! "$object_sha" =~ ^[0-9a-f]{40}$ || \
              ("$object_type" != tag && "$object_type" != commit) ]]; then
            fail "remote annotated release tag target is invalid"
            return
        fi
        depth=$((depth + 1))
    done
    if [[ "$object_type" != commit || "$object_sha" != "$source_sha" ]]; then
        fail "remote release tag no longer identifies the built source"
        return
    fi
}

verify_schema_package() {
    local package_root="$1"
    local source_directory="$2"

    python3 - "$package_root" "$source_directory" <<'PY'
import json
import pathlib
import stat
import sys


def fail(message: str) -> None:
    raise SystemExit(f"schema package verification failed: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


package_root = pathlib.Path(sys.argv[1])
source_directory = pathlib.Path(sys.argv[2])
expected_catalog = {
    "schemaDialect": "https://json-schema.org/draft/2020-12/schema",
    "commands": {
        "listen": {
            "item": "listen.item.schema.json",
            "error": "stream.error.schema.json",
        },
        "wait-for": {
            "result": "wait-for.result.schema.json",
            "error": "stream.error.schema.json",
        },
    },
}
expected_stream_files = {
    "listen.item.schema.json",
    "stream-manifest.json",
    "stream.error.schema.json",
    "wait-for.result.schema.json",
}
expected_session_files = {
    "session-list.result.schema.json",
    "session-terminate.result.schema.json",
    "session.error.schema.json",
}
expected_files = expected_stream_files | expected_session_files

try:
    package_root_mode = package_root.lstat().st_mode
except OSError as error:
    fail(f"package root is missing or unsafe: {error}")
require(not stat.S_ISLNK(package_root_mode), "package root cannot be a symlink")
require(stat.S_ISDIR(package_root_mode), "package root is not a directory")
try:
    resolved_package_root = package_root.resolve(strict=True)
except OSError as error:
    fail(f"cannot resolve package root: {error}")


def packaged_component(relative_name: str, expected_type: str) -> pathlib.Path:
    relative = pathlib.PurePosixPath(relative_name)
    require(
        not relative.is_absolute()
        and relative.parts
        and all(part not in {"", ".", ".."} for part in relative.parts),
        f"unsafe packaged relative path: {relative_name}",
    )

    candidate = package_root
    for index, component in enumerate(relative.parts):
        candidate /= component
        owner = f"packaged {'/'.join(relative.parts[: index + 1])}"
        try:
            mode = candidate.lstat().st_mode
        except OSError as error:
            fail(f"{owner} is missing: {error}")
        require(not stat.S_ISLNK(mode), f"{owner} cannot be a symlink")

        is_leaf = index == len(relative.parts) - 1
        required_type = expected_type if is_leaf else "directory"
        if required_type == "directory":
            require(stat.S_ISDIR(mode), f"{owner} is not a directory")
        else:
            require(stat.S_ISREG(mode), f"{owner} is not a regular file")

        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(resolved_package_root)
        except (OSError, ValueError) as error:
            fail(f"{owner} escapes the package root: {error}")
    return candidate


packaged_component("docs", "directory")
package_directory = packaged_component("docs/schemas", "directory")
require(
    source_directory.is_dir() and not source_directory.is_symlink(),
    "source schema directory is missing or unsafe",
)

actual_entries = {entry.name for entry in package_directory.iterdir()}
require(actual_entries == expected_files, "packaged schema file set differs")

for filename in sorted(expected_files):
    packaged_file = packaged_component(f"docs/schemas/{filename}", "file")
    source_file = source_directory / filename
    require(
        source_file.is_file() and not source_file.is_symlink(),
        f"source {filename} is missing or unsafe",
    )
    require(packaged_file.read_bytes() == source_file.read_bytes(), f"packaged {filename} differs")

try:
    catalog = json.loads((package_directory / "stream-manifest.json").read_text(encoding="utf-8"))
except (OSError, UnicodeError, json.JSONDecodeError) as error:
    fail(f"stream manifest is invalid: {error}")

commands = catalog.get("commands") if isinstance(catalog, dict) else None
require(isinstance(commands, dict), "stream manifest commands are invalid")
referenced_files = []
for contracts in commands.values():
    require(isinstance(contracts, dict), "stream manifest command contract is invalid")
    for filename in contracts.values():
        require(isinstance(filename, str), "stream manifest schema reference is invalid")
        reference = pathlib.PurePosixPath(filename)
        require(
            not reference.is_absolute()
            and len(reference.parts) == 1
            and reference.parts[0] not in {"", ".", ".."},
            f"stream manifest contains unsafe schema reference: {filename}",
        )
        referenced_files.append(filename)

require(catalog == expected_catalog, "stream manifest contract differs")
referenced = set(referenced_files)
require(
    referenced == expected_stream_files - {"stream-manifest.json"},
    "stream manifest and packaged schemas are not bijective",
)
for filename in sorted(expected_files - {"stream-manifest.json"}):
    try:
        schema = json.loads((package_directory / filename).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"packaged {filename} is invalid: {error}")
    require(
        isinstance(schema, dict)
        and schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
        f"packaged {filename} has an invalid dialect",
    )
PY
}

verify_command_assets_package() {
    local package_root="$1"
    local source_root="$2"
    local binary="$3"
    local manifest="$4"

    python3 - "$package_root" "$source_root" "$binary" "$manifest" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys


def fail(message: str) -> None:
    raise SystemExit(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        require(key not in result, f"command asset manifest has duplicate key: {key}")
        result[key] = value
    return result


package_root = pathlib.Path(sys.argv[1])
source_root = pathlib.Path(sys.argv[2])
binary = pathlib.Path(sys.argv[3])
manifest_path = pathlib.Path(sys.argv[4])
for owner, candidate, required_type in (
    ("package root", package_root, "directory"),
    ("source root", source_root, "directory"),
    ("release binary", binary, "file"),
    ("command asset manifest", manifest_path, "file"),
):
    try:
        mode = candidate.lstat().st_mode
    except OSError as error:
        fail(f"{owner} is missing or unsafe: {error}")
    require(not stat.S_ISLNK(mode), f"{owner} cannot be a symlink")
    require(
        stat.S_ISDIR(mode) if required_type == "directory" else stat.S_ISREG(mode),
        f"{owner} has the wrong type",
    )

try:
    resolved_package_root = package_root.resolve(strict=True)
    resolved_source_root = source_root.resolve(strict=True)
    resolved_binary = binary.resolve(strict=True)
    resolved_binary.relative_to(resolved_package_root)
except (OSError, ValueError) as error:
    fail(f"release command asset roots are unsafe: {error}")
require(os.access(resolved_binary, os.X_OK), "release binary is not executable")

try:
    manifest = json.loads(
        manifest_path.read_text(encoding="utf-8"), object_pairs_hook=unique_object
    )
except (OSError, UnicodeError, json.JSONDecodeError) as error:
    fail(f"command asset manifest is invalid: {error}")
require(
    isinstance(manifest, dict)
    and set(manifest) == {"schema_version", "assets"}
    and manifest["schema_version"] == 1
    and isinstance(manifest["assets"], list),
    "command asset manifest root differs",
)
expected = {
    (
        "completions/tgcli.bash",
        "share/bash-completion/completions/tgcli",
        "bash",
    ),
    (
        "completions/tgcli.fish",
        "share/fish/vendor_completions.d/tgcli.fish",
        "fish",
    ),
    ("completions/_tgcli", "share/zsh/site-functions/_tgcli", "zsh"),
    (
        "docs/commands/public-command-registry.json",
        "share/tgcli/public-command-registry.json",
        None,
    ),
    ("docs/man/tgcli.1", "share/man/man1/tgcli.1", None),
}
actual: set[tuple[str, str, str | None]] = set()
runtime_sources: dict[str, pathlib.Path] = {}


def checked_file(root: pathlib.Path, relative_name: str, owner: str) -> pathlib.Path:
    relative = pathlib.PurePosixPath(relative_name)
    require(
        not relative.is_absolute()
        and relative.parts
        and all(part not in {"", ".", ".."} for part in relative.parts),
        f"unsafe {owner} path: {relative_name}",
    )
    candidate = root
    for index, component in enumerate(relative.parts):
        candidate /= component
        try:
            mode = candidate.lstat().st_mode
        except OSError as error:
            fail(f"{owner} is missing: {relative_name}: {error}")
        require(not stat.S_ISLNK(mode), f"{owner} cannot be a symlink: {relative_name}")
        leaf = index == len(relative.parts) - 1
        require(
            stat.S_ISREG(mode) if leaf else stat.S_ISDIR(mode),
            f"{owner} has the wrong type: {relative_name}",
        )
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root.resolve(strict=True))
    except (OSError, ValueError) as error:
        fail(f"{owner} escapes its root: {relative_name}: {error}")
    return candidate


for asset in manifest["assets"]:
    require(
        isinstance(asset, dict) and set(asset) == {"source", "package", "shell"},
        "command asset manifest row differs",
    )
    source_name = asset["source"]
    package_name = asset["package"]
    shell = asset["shell"]
    require(
        isinstance(source_name, str)
        and isinstance(package_name, str)
        and (shell is None or shell in {"bash", "zsh", "fish"}),
        "command asset manifest row types differ",
    )
    identity = (source_name, package_name, shell)
    require(identity not in actual, "duplicate command asset manifest row")
    actual.add(identity)
    source_file = checked_file(resolved_source_root, source_name, "source command asset")
    package_file = checked_file(resolved_package_root, package_name, "packaged command asset")
    source_bytes = source_file.read_bytes()
    package_bytes = package_file.read_bytes()
    require(
        hashlib.sha256(source_bytes).digest() == hashlib.sha256(package_bytes).digest()
        and source_bytes == package_bytes,
        f"packaged command asset differs: {package_name}",
    )
    if shell is not None:
        runtime_sources[shell] = source_file

require(actual == expected, "command asset manifest file set differs")
environment = os.environ.copy()
environment.update(
    {
        "HOME": "/tgcli-release-command-assets-missing-home",
        "NO_COLOR": "1",
        "TGCLI_ACCOUNT": "invalid account value",
        "TGCLI_ALLOW_WRITE": "invalid",
        "XDG_CONFIG_HOME": "/tgcli-release-command-assets-missing-config",
        "XDG_RUNTIME_DIR": "/tgcli-release-command-assets-missing-runtime",
    }
)
for shell, source_file in sorted(runtime_sources.items()):
    result = subprocess.run(
        [str(resolved_binary), "--no-color", "completion", shell],
        cwd=resolved_package_root,
        env=environment,
        check=False,
        capture_output=True,
    )
    require(
        result.returncode == 0 and not result.stderr,
        f"packaged runtime completion failed: {shell}",
    )
    require(
        result.stdout == source_file.read_bytes(),
        f"packaged runtime completion differs: {shell}",
    )
PY
}

staged_archive_name() {
    python3 - "$1" "$2" <<'PY'
import pathlib
import re
import sys
import urllib.parse


component_id, source_url = sys.argv[1:]
if re.fullmatch(r"[a-z0-9][a-z0-9_-]*", component_id) is None:
    raise SystemExit("staged archive naming failed: invalid component id")
parsed = urllib.parse.urlparse(source_url)
source_name = pathlib.PurePosixPath(parsed.path).name
for suffix in (".tar.gz", ".tar.xz"):
    if parsed.scheme == "https" and source_name.endswith(suffix):
        print(f"{component_id}{suffix}")
        break
else:
    parts = pathlib.PurePosixPath(parsed.path).parts
    if (
        parsed.scheme == "https"
        and len(parts) >= 2
        and parts[-2] == "tar.gz"
        and parts[-1]
    ):
        print(f"{component_id}.tar.gz")
    else:
        raise SystemExit("staged archive naming failed: unsupported source archive")
PY
}

verify_release_bundle() {
    local version="$1"
    local directory="$2"
    local linux="tgcli-$version-linux-x86_64-musl.tar.gz"
    local macos="tgcli-$version-macos-universal.tar.gz"
    local expected_listing
    local actual_listing
    local expected_checksums

    [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
        fail "release version is not an exact MAJOR.MINOR.PATCH value"
    [[ -d "$directory" && ! -L "$directory" ]] || \
        fail "release bundle directory is missing or unsafe"

    expected_listing="$(printf '%s\n' SHA256SUMS "$linux" "$macos" | LC_ALL=C sort)"
    actual_listing="$(find "$directory" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)"
    [[ "$actual_listing" == "$expected_listing" ]] || \
        fail "release bundle has an unexpected file set"
    for release_file in "$directory/SHA256SUMS" "$directory/$linux" "$directory/$macos"; do
        [[ -f "$release_file" && ! -L "$release_file" ]] || \
            fail "release bundle contains a non-regular artifact"
    done

    expected_checksums="$({
        sha256sum "$directory/$linux"
        sha256sum "$directory/$macos"
    } | sed "s|  $directory/|  |" | LC_ALL=C sort -k2)"
    [[ "$(cat "$directory/SHA256SUMS")" == "$expected_checksums" ]] || \
        fail "release checksum manifest is not canonical"
    (
        cd "$directory"
        sha256sum --check --strict SHA256SUMS >/dev/null
    )
}

verify_signed_layout() {
    local version="$1"
    local directory="$2"
    local unsigned_directory
    local release_file

    unsigned_directory="$(mktemp -d)"
    for release_file in \
        SHA256SUMS \
        "tgcli-$version-linux-x86_64-musl.tar.gz" \
        "tgcli-$version-macos-universal.tar.gz"; do
        [[ -f "$directory/$release_file" && ! -L "$directory/$release_file" ]] || \
            fail "signed release bundle is missing $release_file"
        [[ -f "$directory/$release_file.sigstore.json" && \
           ! -L "$directory/$release_file.sigstore.json" ]] || \
            fail "signed release bundle is missing $release_file.sigstore.json"
        cp "$directory/$release_file" "$unsigned_directory/$release_file"
    done
    [[ "$(find "$directory" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')" == 6 ]] || \
        fail "signed release bundle has an unexpected file set"
    python3 - "$directory" <<'PY'
import json
import pathlib
import sys

for bundle in pathlib.Path(sys.argv[1]).glob("*.sigstore.json"):
    try:
        document = json.loads(bundle.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"invalid Sigstore bundle {bundle.name}: {error}")
    if not isinstance(document, dict) or not document:
        raise SystemExit(f"invalid Sigstore bundle {bundle.name}: expected object")
PY
    verify_release_bundle "$version" "$unsigned_directory"
}

verify_version_revision() {
    local binary="$1"
    local source_sha="$2"
    local workspace
    local version_json=""
    local failure=""
    local validation_error=""

    [[ -f "$binary" && -x "$binary" && ! -L "$binary" ]] || {
        fail "release version artifact is not an executable regular file"
        return
    }
    [[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || {
        fail "release version source SHA is not canonical"
        return
    }

    workspace="$(mktemp -d)"
    mkdir -p \
        "$workspace/home" \
        "$workspace/config/tgcli" \
        "$workspace/data" \
        "$workspace/state" \
        "$workspace/runtime"
    chmod 0700 "$workspace/config/tgcli" "$workspace/runtime"
    printf '%s\n' \
        'default_account = "main"' \
        '[accounts.main]' \
        'allow_write = false' > "$workspace/config/tgcli/config.toml"
    chmod 0600 "$workspace/config/tgcli/config.toml"

    if ! version_json="$(
        unset TGCLI_ACCOUNT TGCLI_ALLOW_WRITE TGCLI_TEST_DC
        HOME="$workspace/home" \
        XDG_CONFIG_HOME="$workspace/config" \
        XDG_DATA_HOME="$workspace/data" \
        XDG_STATE_HOME="$workspace/state" \
        XDG_RUNTIME_DIR="$workspace/runtime" \
        "$binary" --no-daemon --json version 2>"$workspace/stderr"
    )"; then
        failure="release binary version command failed"
    elif [[ -s "$workspace/stderr" ]]; then
        failure="release binary version command wrote to stderr"
    elif ! validation_error="$(python3 - "$version_json" "$source_sha" 2>&1 <<'PY'
import json
import re
import sys

try:
    result = json.loads(sys.argv[1])
except json.JSONDecodeError as error:
    raise SystemExit(f"release version output is not one JSON value: {error}") from error

if not isinstance(result, dict) or set(result) != {"commit", "protocol", "tdlib", "version"}:
    raise SystemExit("release version result has unexpected keys")
if not isinstance(result["version"], str) or not isinstance(result["tdlib"], str):
    raise SystemExit("release version result has invalid string fields")
if (
    isinstance(result["protocol"], bool)
    or not isinstance(result["protocol"], int)
    or result["protocol"] < 1
):
    raise SystemExit("release version result has an invalid protocol")

revision = result["commit"]
if not isinstance(revision, str):
    raise SystemExit("release version result has no string commit")
if revision.endswith("-dirty"):
    raise SystemExit("release build revision is dirty")
if re.fullmatch(r"[0-9a-f]{7,}", revision) is None:
    raise SystemExit("release build revision is invalid")
if not sys.argv[2].startswith(revision):
    raise SystemExit("release build revision differs from full provenance")
PY
    )"; then
        failure="$validation_error"
    fi

    rm -rf -- "$workspace"
    if [[ -n "$failure" ]]; then
        fail "$failure"
        return
    fi
    printf '%s\n' 'release-version-revision-matches-full-provenance'
}

usage() {
    printf 'usage: %s inspect-linux|inspect-macos|inspect-macos-universal <artifact>\n' "$0" >&2
    printf '       %s verify-linux-build <artifact> <provenance> <sbom> <source-sha> <source-tree> <image> [repo-root]\n' "$0" >&2
    printf '       %s verify-macos-build <artifact> <provenance> <sbom> <platform> <source-sha> <source-tree> <lock> [<arm64> <arm64-provenance> <arm64-sbom> <x86_64> <x86_64-provenance> <x86_64-sbom> | <arm64-provenance> <arm64-sbom> <x86_64-provenance> <x86_64-sbom>]\n' "$0" >&2
    printf '       %s verify-release-bundle|verify-signed-layout <version> <directory>\n' "$0" >&2
    printf '       %s classify-release-state <version> <directory> <tag> <source-sha> <release-json>\n' "$0" >&2
    printf '       %s verify-remote-tag <source-sha> <owner/repository> <tag>\n' "$0" >&2
    printf '       %s verify-schema-package <package-root> <source-schema-directory>\n' "$0" >&2
    printf '       %s verify-command-assets-package <package-root> <source-root> <binary> <manifest>\n' "$0" >&2
    printf '       %s verify-version-revision <artifact> <source-sha>\n' "$0" >&2
    printf '       %s staged-archive-name <component-id> <source-url>\n' "$0" >&2
    printf '       %s classify-linux|classify-macos\n' "$0" >&2
    return 2
}

case "${1:-}" in
    inspect-linux)
        [[ "$#" -eq 2 ]] || usage
        inspect_linux "$2"
        ;;
    inspect-macos)
        [[ "$#" -eq 2 ]] || usage
        inspect_macos "$2"
        ;;
    inspect-macos-universal)
        [[ "$#" -eq 2 ]] || usage
        inspect_macos_universal "$2"
        ;;
    verify-linux-build)
        [[ "$#" -eq 7 || "$#" -eq 8 ]] || usage
        verify_linux_build "$2" "$3" "$4" "$5" "$6" "$7" "${8:-}"
        ;;
    verify-macos-build)
        [[ "$#" -eq 8 || "$#" -eq 12 || "$#" -eq 14 ]] || usage
        verify_macos_build "${@:2}"
        ;;
    verify-release-bundle)
        [[ "$#" -eq 3 ]] || usage
        verify_release_bundle "$2" "$3"
        ;;
    verify-signed-layout)
        [[ "$#" -eq 3 ]] || usage
        verify_signed_layout "$2" "$3"
        ;;
    classify-release-state)
        [[ "$#" -eq 6 ]] || usage
        classify_release_state "$2" "$3" "$4" "$5" "$6"
        ;;
    verify-remote-tag)
        [[ "$#" -eq 4 ]] || usage
        verify_remote_tag "$2" "$3" "$4"
        ;;
    verify-schema-package)
        [[ "$#" -eq 3 ]] || usage
        verify_schema_package "$2" "$3"
        ;;
    verify-command-assets-package)
        [[ "$#" -eq 5 ]] || usage
        verify_command_assets_package "$2" "$3" "$4" "$5"
        ;;
    verify-version-revision)
        [[ "$#" -eq 3 ]] || usage
        verify_version_revision "$2" "$3"
        ;;
    staged-archive-name)
        [[ "$#" -eq 3 ]] || usage
        staged_archive_name "$2" "$3"
        ;;
    classify-linux)
        [[ "$#" -eq 1 ]] || usage
        classify_linux \
            "${TGCLI_FILE_OUTPUT:-}" \
            "${TGCLI_PROGRAM_HEADERS:-}" \
            "${TGCLI_DYNAMIC_SECTION:-}" \
            "${TGCLI_VERSION_INFO:-}"
        ;;
    classify-macos)
        [[ "$#" -eq 1 ]] || usage
        classify_macos_dependencies "${TGCLI_OTOOL_OUTPUT:-}"
        ;;
    *)
        usage
        ;;
esac
