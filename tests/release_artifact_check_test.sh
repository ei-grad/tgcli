#!/usr/bin/env bash
set -euo pipefail

if [[ "${TGCLI_TEST_FAKE_FILE:-}" == 1 ]]; then
    printf '%s: ELF 64-bit LSB executable, statically linked\n' "$1"
    exit 0
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
checker="$root/scripts/check-release-artifact.sh"

expect_failure_message() {
    local name="$1"
    local expected="$2"
    local output
    shift 2
    if output="$("$@" 2>&1)"; then
        printf '%s unexpectedly succeeded\n' "$name" >&2
        exit 1
    fi
    if [[ "$output" != *"$expected"* ]]; then
        printf '%s failed without the expected diagnostic\n' "$name" >&2
        exit 1
    fi
}

expect_failure_message dynamic-glibc 'not a statically linked ELF' \
    env \
    TGCLI_FILE_OUTPUT='ELF 64-bit LSB pie executable, dynamically linked' \
    TGCLI_PROGRAM_HEADERS='INTERP' \
    TGCLI_DYNAMIC_SECTION='(NEEDED) Shared library' \
    TGCLI_VERSION_INFO='GLIBC_2.34' \
    bash "$checker" classify-linux

expect_failure_message static-glibc 'glibc symbol-version reference' \
    env \
    TGCLI_FILE_OUTPUT='ELF 64-bit LSB executable, statically linked' \
    TGCLI_PROGRAM_HEADERS='Program Headers:' \
    TGCLI_DYNAMIC_SECTION='There is no dynamic section in this file.' \
    TGCLI_VERSION_INFO='Name: GLIBC_2.34' \
    bash "$checker" classify-linux

classification="$(
    TGCLI_FILE_OUTPUT='ELF 64-bit LSB executable, statically linked' \
    TGCLI_PROGRAM_HEADERS='Program Headers:' \
    TGCLI_DYNAMIC_SECTION='There is no dynamic section in this file.' \
    TGCLI_VERSION_INFO='No version information found in this file.' \
    bash "$checker" classify-linux
)"
if [[ "$classification" != 'static-elf-without-interpreter-or-dynamic-dependencies' ]]; then
    printf 'clean static classification changed unexpectedly\n' >&2
    exit 1
fi

static_pie_classification="$(
    TGCLI_FILE_OUTPUT='ELF 64-bit LSB pie executable, static-pie linked' \
    TGCLI_PROGRAM_HEADERS='Program Headers:' \
    TGCLI_DYNAMIC_SECTION='Dynamic section contains no NEEDED entries.' \
    TGCLI_VERSION_INFO='No version information found in this file.' \
    bash "$checker" classify-linux
)"
if [[ "$static_pie_classification" != \
      'static-elf-without-interpreter-or-dynamic-dependencies' ]]; then
    printf 'clean static PIE classification changed unexpectedly\n' >&2
    exit 1
fi

expect_failure_message file-inspector-failure 'file failed while inspecting' \
    env FILE_TOOL=/bin/false bash "$checker" inspect-linux /bin/true

expect_failure_message readelf-inspector-failure 'readelf failed while reading Linux program headers' \
    env \
    TGCLI_TEST_FAKE_FILE=1 \
    FILE_TOOL="$0" \
    READELF_TOOL=/bin/false \
    bash "$checker" inspect-linux /bin/true

expect_failure_message otool-inspector-failure 'otool failed while inspecting' \
    env OTOOL_TOOL=/bin/false bash "$checker" inspect-macos /bin/true

expect_failure_message lipo-inspector-failure 'lipo failed while inspecting' \
    env LIPO_TOOL=/bin/false bash "$checker" inspect-macos-universal /bin/true

TGCLI_OTOOL_OUTPUT=$'/tmp/tgcli (architecture x86_64):\n\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0)\n/tmp/tgcli (architecture arm64):\n\t/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation (compatibility version 150.0.0)' \
    bash "$checker" classify-macos

expect_failure_message macos-non-system-dependency 'non-system runtime dependency' \
    env \
    TGCLI_OTOOL_OUTPUT=$'/tmp/tgcli:\n\t/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib (compatibility version 3.0.0)' \
    bash "$checker" classify-macos

expect_failure_message macos-unparsed-output 'cannot parse the complete otool dependency report' \
    env \
    TGCLI_OTOOL_OUTPUT=$'/tmp/tgcli:\nthis line is not a dependency record' \
    bash "$checker" classify-macos

archive_name="$(
    bash "$checker" staged-archive-name \
        openssl \
        https://example.invalid/releases/openssl-3.5.7.tar.gz
)"
if [[ "$archive_name" != openssl.tar.gz ]]; then
    printf 'staged archive name differs from the canonical verifier contract\n' >&2
    exit 1
fi
codeload_archive_name="$(
    bash "$checker" staged-archive-name \
        re2 \
        https://codeload.github.com/google/re2/tar.gz/4be240789d5b322df9f02b7e19c8651f3ccbf205
)"
if [[ "$codeload_archive_name" != re2.tar.gz ]]; then
    printf 'codeload staged archive name differs from the canonical contract\n' >&2
    exit 1
fi
expect_failure_message unsafe-staged-archive-source 'unsupported source archive' \
    bash "$checker" staged-archive-name openssl https://example.invalid/openssl.zip

staging_contract_verifier="${TGCLI_STAGING_CONTRACT_VERIFIER:-$root/scripts/verify_dependency_lock.py}"
python3 - \
    "$staging_contract_verifier" \
    "${TGCLI_STAGING_CONTRACT_VERIFIER:+required}" <<'PY'
import importlib.util
import pathlib
import sys


verifier = pathlib.Path(sys.argv[1])
required = sys.argv[2] == "required"
spec = importlib.util.spec_from_file_location("tgcli_dependency_lock_verifier", verifier)
if spec is None or spec.loader is None:
    raise SystemExit("cannot load dependency lock verifier")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
if not hasattr(module, "staged_archive_name"):
    if required:
        raise SystemExit("dependency lock verifier has no staged archive contract")
    raise SystemExit(0)
component = {
    "id": "openssl",
    "source_archive": "https://example.invalid/releases/openssl-3.5.7.tar.gz",
}
if module.staged_archive_name("openssl", component) != "openssl.tar.gz":
    raise SystemExit("dependency lock verifier staging name changed")
codeload = {
    "id": "re2",
    "source_archive": "https://codeload.github.com/google/re2/tar.gz/"
    "4be240789d5b322df9f02b7e19c8651f3ccbf205",
}
if module.staged_archive_name("re2", codeload) != "re2.tar.gz":
    raise SystemExit("dependency lock verifier cannot stage codeload archives")
PY

fixture_root="$(mktemp -d)"
source_sha=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
source_tree=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
toolchain_image="docker.io/example/toolchain@sha256:$(printf 'c%.0s' {1..64})"

schema_package="$fixture_root/schema-package"
mkdir -p "$schema_package/docs/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" "$schema_package/docs/schemas/$schema_file"
done
bash "$checker" verify-schema-package "$schema_package" "$root/docs/schemas"

missing_schema_package="$fixture_root/missing-schema-package"
mkdir -p "$missing_schema_package/docs/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" "$missing_schema_package/docs/schemas/$schema_file"
done
expect_failure_message missing-packaged-stream-schema 'file set differs' \
    bash "$checker" verify-schema-package "$missing_schema_package" "$root/docs/schemas"

tampered_schema_package="$fixture_root/tampered-schema-package"
mkdir -p "$tampered_schema_package/docs/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" "$tampered_schema_package/docs/schemas/$schema_file"
done
printf '\n' >> "$tampered_schema_package/docs/schemas/listen.item.schema.json"
expect_failure_message tampered-packaged-stream-schema 'listen.item.schema.json differs' \
    bash "$checker" verify-schema-package "$tampered_schema_package" "$root/docs/schemas"

symlink_schema_package="$fixture_root/symlink-schema-package"
mkdir -p "$symlink_schema_package/docs/schemas"
for schema_file in \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" "$symlink_schema_package/docs/schemas/$schema_file"
done
ln -s "$root/docs/schemas/listen.item.schema.json" \
    "$symlink_schema_package/docs/schemas/listen.item.schema.json"
expect_failure_message symlink-packaged-stream-schema \
    'packaged docs/schemas/listen.item.schema.json cannot be a symlink' \
    bash "$checker" verify-schema-package "$symlink_schema_package" "$root/docs/schemas"

missing_session_schema_package="$fixture_root/missing-session-schema-package"
mkdir -p "$missing_session_schema_package/docs/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json; do
    cp "$root/docs/schemas/$schema_file" \
        "$missing_session_schema_package/docs/schemas/$schema_file"
done
expect_failure_message missing-packaged-session-schema 'file set differs' \
    bash "$checker" verify-schema-package "$missing_session_schema_package" "$root/docs/schemas"

tampered_session_schema_package="$fixture_root/tampered-session-schema-package"
mkdir -p "$tampered_session_schema_package/docs/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" \
        "$tampered_session_schema_package/docs/schemas/$schema_file"
done
printf '\n' >> \
    "$tampered_session_schema_package/docs/schemas/session-list.result.schema.json"
expect_failure_message tampered-packaged-session-schema \
    'session-list.result.schema.json differs' \
    bash "$checker" verify-schema-package "$tampered_session_schema_package" "$root/docs/schemas"

symlink_session_schema_package="$fixture_root/symlink-session-schema-package"
mkdir -p "$symlink_session_schema_package/docs/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json; do
    cp "$root/docs/schemas/$schema_file" \
        "$symlink_session_schema_package/docs/schemas/$schema_file"
done
ln -s "$root/docs/schemas/session.error.schema.json" \
    "$symlink_session_schema_package/docs/schemas/session.error.schema.json"
expect_failure_message symlink-packaged-session-schema \
    'packaged docs/schemas/session.error.schema.json cannot be a symlink' \
    bash "$checker" verify-schema-package "$symlink_session_schema_package" "$root/docs/schemas"

parent_symlink_target="$fixture_root/parent-symlink-target"
mkdir -p "$parent_symlink_target/schemas"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" "$parent_symlink_target/schemas/$schema_file"
done
parent_symlink_package="$fixture_root/parent-symlink-package"
mkdir -p "$parent_symlink_package"
ln -s "$parent_symlink_target" "$parent_symlink_package/docs"
expect_failure_message symlink-packaged-schema-parent 'packaged docs cannot be a symlink' \
    bash "$checker" verify-schema-package "$parent_symlink_package" "$root/docs/schemas"

directory_symlink_target="$fixture_root/directory-symlink-target"
mkdir -p "$directory_symlink_target"
for schema_file in \
    listen.item.schema.json \
    stream-manifest.json \
    stream.error.schema.json \
    wait-for.result.schema.json \
    session-list.result.schema.json \
    session-terminate.result.schema.json \
    session.error.schema.json; do
    cp "$root/docs/schemas/$schema_file" "$directory_symlink_target/$schema_file"
done
directory_symlink_package="$fixture_root/directory-symlink-package"
mkdir -p "$directory_symlink_package/docs"
ln -s "$directory_symlink_target" "$directory_symlink_package/docs/schemas"
expect_failure_message symlink-packaged-schema-directory \
    'packaged docs/schemas cannot be a symlink' \
    bash "$checker" verify-schema-package "$directory_symlink_package" "$root/docs/schemas"

non_directory_package="$fixture_root/non-directory-package"
mkdir -p "$non_directory_package"
printf 'not a directory\n' > "$non_directory_package/docs"
expect_failure_message non-directory-packaged-schema-parent 'packaged docs is not a directory' \
    bash "$checker" verify-schema-package "$non_directory_package" "$root/docs/schemas"

for unsafe_reference in '../listen.item.schema.json' '/listen.item.schema.json'; do
    if [[ "$unsafe_reference" == /* ]]; then
        unsafe_name=absolute
    else
        unsafe_name=traversal
    fi
    unsafe_source="$fixture_root/$unsafe_name-source-schemas"
    unsafe_package="$fixture_root/$unsafe_name-schema-package"
    mkdir -p "$unsafe_source" "$unsafe_package/docs/schemas"
    for schema_file in \
        listen.item.schema.json \
        stream-manifest.json \
        stream.error.schema.json \
        wait-for.result.schema.json \
        session-list.result.schema.json \
        session-terminate.result.schema.json \
        session.error.schema.json; do
        cp "$root/docs/schemas/$schema_file" "$unsafe_source/$schema_file"
        cp "$root/docs/schemas/$schema_file" "$unsafe_package/docs/schemas/$schema_file"
    done
    python3 - \
        "$unsafe_source/stream-manifest.json" \
        "$unsafe_package/docs/schemas/stream-manifest.json" \
        "$unsafe_reference" <<'PY'
import json
import pathlib
import sys


for filename in sys.argv[1:3]:
    manifest_file = pathlib.Path(filename)
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    manifest["commands"]["listen"]["item"] = sys.argv[3]
    manifest_file.write_text(json.dumps(manifest) + "\n", encoding="utf-8")
PY
    expect_failure_message "$unsafe_name-packaged-schema-reference" \
        'stream manifest contains unsafe schema reference' \
        bash "$checker" verify-schema-package "$unsafe_package" "$unsafe_source"
done

python3 - \
    "$fixture_root" \
    "$source_sha" \
    "$source_tree" \
    "$toolchain_image" \
    "$root" <<'PY'
import hashlib
import importlib
import json
import pathlib
import sys


def write_json(file: pathlib.Path, document: dict) -> None:
    file.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def sha256_file(file: pathlib.Path) -> str:
    return hashlib.sha256(file.read_bytes()).hexdigest()


root = pathlib.Path(sys.argv[1])
source_sha = sys.argv[2]
source_tree = sys.argv[3]
image = sys.argv[4]
repo_root = pathlib.Path(sys.argv[5])
sys.path.insert(0, str(repo_root / "scripts/release"))
provenance_tool = importlib.import_module("build_provenance")
re2_verifier = importlib.import_module("verify_re2_build")
(root / "release").mkdir(parents=True)
(root / "release/licenses").mkdir(parents=True)
(root / "scripts/release").mkdir(parents=True)
(root / "build").mkdir(parents=True)

artifact = root / "build/tgcli"
artifact.write_bytes(b"static release fixture\n")
artifact.chmod(0o755)
recipe = root / "scripts/release/build-linux-musl.sh"
recipe.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")

runtime = {
    "component_id": "libc",
    "path": "sysroot/usr/lib/libc.a",
    "sha256": "d" * 64,
}
for name, content in {
    "Fixture.txt": b"fixture license\n",
    "RE2.txt": b"fixture RE2 BSD license\n",
    "RE2-Lucent-UTF.txt": b"fixture Lucent license\n",
}.items():
    (root / "release/licenses" / name).write_bytes(content)


def license_evidence(name: str) -> list[dict]:
    file = root / "release/licenses" / name
    return [{"path": f"release/licenses/{name}", "sha256": sha256_file(file)}]


def component(
    component_id: str,
    archive_sha256: str,
    archive_size: int,
    immutable_ref: str,
    scope: str,
) -> dict:
    return {
        "archive_sha256": archive_sha256,
        "archive_size": archive_size,
        "embedded_components": [],
        "id": component_id,
        "immutable_ref": immutable_ref,
        "license_expression": "MIT",
        "license_files": license_evidence("Fixture.txt"),
        "name": component_id,
        "scope": scope,
        "source_archive": f"https://example.invalid/{component_id}.tar.gz",
        "source_repository": f"https://example.invalid/{component_id}",
        "source_tree_sha256": "f" * 64,
        "version": "1.0",
    }


components = [
    component("libc", "1" * 64, 11, "v1", "release-runtime"),
    component("tdlib", "2" * 64, 22, "e" * 40, "runtime"),
    component("openssl", "3" * 64, 33, "openssl-3.5.2", "runtime"),
    component("re2", "4" * 64, 44, "d" * 40, "runtime"),
]
components[2]["version"] = "3.5.2"
components[2]["source_archive"] = "https://example.invalid/openssl-3.5.2.tar.gz"
components[2]["source_tree_sha256"] = "5" * 64
components[3].update(
    {
        "embedded_components": [
            {
                "id": "re2-plan9-utf",
                "license_expression": "LicenseRef-RE2-Lucent-2002",
                "license_files": license_evidence("RE2-Lucent-UTF.txt"),
                "name": "Plan 9 UTF routines",
                "source_path": "util/rune.cc and util/utf.h",
                "version": "2002",
            }
        ],
        "license_expression": "BSD-3-Clause",
        "license_files": license_evidence("RE2.txt"),
        "source_tree_sha256": "6" * 64,
        "version": "2022-12-01",
    }
)
lock = {
    "components": components,
    "release_toolchains": [{"runtime_files": [runtime]}],
    "schema_version": 2,
}
lock_file = root / "release/dependencies.lock.json"
write_json(lock_file, lock)
contract = {
    "dependency_lock_sha256": sha256_file(lock_file),
    "image": image,
    "openssl": {},
    "recipe": {
        "path": "scripts/release/build-linux-musl.sh",
        "sha256": sha256_file(recipe),
    },
    "schema_version": 1,
    "target": "x86_64-linux-musl",
    "tdlib_revision": "e" * 40,
    "zlib": {},
}
contract_file = root / "release/linux-musl-toolchain.json"
write_json(contract_file, contract)
artifact_evidence = {
    "path": artifact.name,
    "sha256": sha256_file(artifact),
    "size": artifact.stat().st_size,
}
re2_build = {
    "archive": {
        "path": "_deps/re2-build/libre2.a",
        "sha256": "7" * 64,
        "size": 777,
    },
    "checks": {
        "abseil_absent": True,
        "benchmarks_absent": True,
        "icu_absent": True,
        "pcre_absent": True,
        "shared_re2_absent": True,
        "tests_absent": True,
    },
    "compile_sources": sorted(re2_verifier.RE2_RUNTIME_SOURCES),
    "configured_options": {
        "BUILD_SHARED_LIBS": False,
        "RE2_BUILD_TESTING": False,
        "USEPCRE": False,
    },
    "final_link": {
        "archive": "_deps/re2-build/libre2.a",
        "command_sha256": "8" * 64,
    },
    "link_map": None,
    "runtime_libraries": ["Threads::Threads"],
    "schema_version": 1,
    "target": "re2::re2",
    "target_type": "STATIC_LIBRARY",
}
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
    ),
    key=lambda component: component["id"],
)
provenance = {
    "artifact": artifact_evidence,
    "dependency_lock_sha256": sha256_file(lock_file),
    "inspection": {
        "artifact": artifact_evidence,
        "checks": {"static_elf": True},
        "commands": [{"argv": [str(index)]} for index in range(4)],
        "schema_version": 1,
    },
    "recipe_sha256": sha256_file(recipe),
    "re2_build": re2_build,
    "release_contract_sha256": sha256_file(contract_file),
    "resolved_dependencies": resolved_dependencies,
    "runtime_selection": {
        "schema_version": 1,
        "selected_runtime_files": [runtime],
    },
    "schema_version": 3,
    "source": {"commit": source_sha, "tree": source_tree, "type": "git"},
    "source_sha": source_sha,
    "tests": {"argv": ["ctest"], "passed": True},
    "tool_versions": {"cmake": "cmake 4", "compiler": "gcc 16", "ninja": "1.13"},
    "toolchain_image": image,
}
write_json(root / "build/provenance.json", provenance)
write_json(
    root / "build/SBOM.json",
    provenance_tool.build_sbom_document(
        artifact,
        lock_file,
        root / "build/provenance.json",
        "linux-x86_64-musl",
        {},
    ),
)

source = {"epoch": 1700000000, "sha": source_sha, "tree": source_tree}
dependency_lock = {
    "path": "release/dependencies.lock.json",
    "sha256": sha256_file(lock_file),
    "value": lock,
}
openssl = next(component for component in components if component["id"] == "openssl")
locked_openssl = {
    "archive_sha256": openssl["archive_sha256"],
    "archive_size": openssl["archive_size"],
    "id": "openssl",
    "source_tree_sha256": openssl["source_tree_sha256"],
    "source_url": openssl["source_archive"],
    "version": openssl["version"],
}
macos = {}
for arch in ("arm64", "x86_64"):
    binary = root / f"build/macos-{arch}"
    binary.write_bytes(f"macOS {arch} release fixture\n".encode())
    binary.chmod(0o755)
    architecture_provenance = root / f"build/macos-{arch}-provenance.json"
    write_json(
        architecture_provenance,
        {
            "artifact": {
                "path": binary.name,
                "sha256": sha256_file(binary),
                "size": binary.stat().st_size,
            },
            "dependency_lock": dependency_lock,
            "locked_openssl": locked_openssl,
            "platform": f"macos-{arch}",
            "re2_build": re2_build,
            "recipes": {},
            "resolved_dependencies": [],
            "runner": {},
            "schema_version": 2,
            "source": source,
            "tools": {},
        },
    )
    architecture_sbom = root / f"build/macos-{arch}-SBOM.json"
    write_json(
        architecture_sbom,
        provenance_tool.build_sbom_document(
            binary,
            lock_file,
            architecture_provenance,
            f"macos-{arch}",
            {},
        ),
    )
    macos[arch] = (binary, architecture_provenance, architecture_sbom)

universal = root / "build/macos-universal"
universal.write_bytes(b"macOS universal release fixture\n")
universal.chmod(0o755)
write_json(
    root / "build/macos-universal-provenance.json",
    {
        "artifact": {
            "path": universal.name,
            "sha256": sha256_file(universal),
            "size": universal.stat().st_size,
        },
        "dependency_lock": dependency_lock,
        "platform": "macos-universal",
        "runner": {},
        "schema_version": 1,
        "slices": {
            arch: {
                "binary_sha256": sha256_file(macos[arch][0]),
                "provenance_sha256": sha256_file(macos[arch][1]),
                "sbom_sha256": sha256_file(macos[arch][2]),
            }
            for arch in ("arm64", "x86_64")
        },
        "source": source,
        "tools": {},
    },
)
write_json(
    root / "build/macos-universal-SBOM.json",
    provenance_tool.build_sbom_document(
        universal,
        lock_file,
        root / "build/macos-universal-provenance.json",
        "macos-universal",
        {
            f"macos-{arch}": macos[arch][2]
            for arch in ("arm64", "x86_64")
        },
    ),
)
PY

bash "$checker" verify-linux-build \
    "$fixture_root/build/tgcli" \
    "$fixture_root/build/provenance.json" \
    "$fixture_root/build/SBOM.json" \
    "$source_sha" \
    "$source_tree" \
    "$toolchain_image" \
    "$fixture_root"

ln -s tgcli "$fixture_root/build/tgcli-link"
expect_failure_message symlink-linux-inspection 'not an executable regular file' \
    bash "$checker" inspect-linux "$fixture_root/build/tgcli-link"
expect_failure_message symlink-linux-artifact 'artifact cannot be a symlink' \
    bash "$checker" verify-linux-build \
        "$fixture_root/build/tgcli-link" \
        "$fixture_root/build/provenance.json" \
        "$fixture_root/build/SBOM.json" \
        "$source_sha" \
        "$source_tree" \
        "$toolchain_image" \
        "$fixture_root"
ln -s provenance.json "$fixture_root/build/provenance-link.json"
expect_failure_message symlink-linux-provenance 'Linux build provenance cannot be a symlink' \
    bash "$checker" verify-linux-build \
        "$fixture_root/build/tgcli" \
        "$fixture_root/build/provenance-link.json" \
        "$fixture_root/build/SBOM.json" \
        "$source_sha" \
        "$source_tree" \
        "$toolchain_image" \
        "$fixture_root"
ln -s SBOM.json "$fixture_root/build/SBOM-link.json"
expect_failure_message symlink-linux-sbom 'Linux SBOM cannot be a symlink' \
    bash "$checker" verify-linux-build \
        "$fixture_root/build/tgcli" \
        "$fixture_root/build/provenance.json" \
        "$fixture_root/build/SBOM-link.json" \
        "$source_sha" \
        "$source_tree" \
        "$toolchain_image" \
        "$fixture_root"

python3 - "$fixture_root/build/SBOM.json" "$fixture_root/build/SBOM-extra-component.json" <<'PY'
import json
import pathlib
import sys

document = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
document["components"].append({"id": "icu", "license_expression": "ICU"})
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY
expect_failure_message extra-linux-sbom-component \
    'SBOM differs from its artifact, lock, or provenance' \
    bash "$checker" verify-linux-build \
        "$fixture_root/build/tgcli" \
        "$fixture_root/build/provenance.json" \
        "$fixture_root/build/SBOM-extra-component.json" \
        "$source_sha" \
        "$source_tree" \
        "$toolchain_image" \
        "$fixture_root"

for arch in arm64 x86_64; do
    bash "$checker" verify-macos-build \
        "$fixture_root/build/macos-$arch" \
        "$fixture_root/build/macos-$arch-provenance.json" \
        "$fixture_root/build/macos-$arch-SBOM.json" \
        "macos-$arch" \
        "$source_sha" \
        "$source_tree" \
        "$fixture_root/release/dependencies.lock.json"
done
bash "$checker" verify-macos-build \
    "$fixture_root/build/macos-universal" \
    "$fixture_root/build/macos-universal-provenance.json" \
    "$fixture_root/build/macos-universal-SBOM.json" \
    macos-universal \
    "$source_sha" \
    "$source_tree" \
    "$fixture_root/release/dependencies.lock.json" \
    "$fixture_root/build/macos-arm64" \
    "$fixture_root/build/macos-arm64-provenance.json" \
    "$fixture_root/build/macos-arm64-SBOM.json" \
    "$fixture_root/build/macos-x86_64" \
    "$fixture_root/build/macos-x86_64-provenance.json" \
    "$fixture_root/build/macos-x86_64-SBOM.json"

package_root="$fixture_root/package/tgcli-fixture"
mkdir -p "$package_root/release"
cp "$fixture_root/build/macos-universal" "$package_root/tgcli"
cp "$fixture_root/build/macos-arm64-provenance.json" \
    "$package_root/PROVENANCE-arm64.json"
cp "$fixture_root/build/macos-x86_64-provenance.json" \
    "$package_root/PROVENANCE-x86_64.json"
cp "$fixture_root/build/macos-arm64-SBOM.json" "$package_root/SBOM-arm64.json"
cp "$fixture_root/build/macos-x86_64-SBOM.json" "$package_root/SBOM-x86_64.json"
cp "$fixture_root/release/dependencies.lock.json" \
    "$package_root/release/dependencies.lock.json"
cp -R "$fixture_root/release/licenses" "$package_root/release/licenses"
python3 - \
    "$root" \
    "$fixture_root/build/macos-universal-provenance.json" \
    "$package_root" <<'PY'
import importlib
import json
import pathlib
import sys

repo_root = pathlib.Path(sys.argv[1])
source_provenance = pathlib.Path(sys.argv[2])
package = pathlib.Path(sys.argv[3])
sys.path.insert(0, str(repo_root / "scripts/release"))
provenance_tool = importlib.import_module("build_provenance")

provenance = json.loads(source_provenance.read_text(encoding="utf-8"))
provenance["artifact"]["path"] = "tgcli"
(package / "PROVENANCE.json").write_text(
    json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
sbom = provenance_tool.build_sbom_document(
    package / "tgcli",
    package / "release/dependencies.lock.json",
    package / "PROVENANCE.json",
    "macos-universal",
    {
        "macos-arm64": package / "SBOM-arm64.json",
        "macos-x86_64": package / "SBOM-x86_64.json",
    },
)
(package / "SBOM.json").write_text(
    json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY

bash "$checker" verify-macos-build \
    "$package_root/tgcli" \
    "$package_root/PROVENANCE.json" \
    "$package_root/SBOM.json" \
    macos-universal \
    "$source_sha" \
    "$source_tree" \
    "$package_root/release/dependencies.lock.json" \
    "$package_root/PROVENANCE-arm64.json" \
    "$package_root/SBOM-arm64.json" \
    "$package_root/PROVENANCE-x86_64.json" \
    "$package_root/SBOM-x86_64.json"

missing_package="$fixture_root/package/missing-slice"
mkdir -p "$missing_package/release"
cp "$package_root/tgcli" "$package_root/PROVENANCE.json" "$package_root/SBOM.json" \
    "$package_root/PROVENANCE-arm64.json" "$package_root/SBOM-arm64.json" \
    "$package_root/PROVENANCE-x86_64.json" "$missing_package/"
cp "$package_root/release/dependencies.lock.json" \
    "$missing_package/release/dependencies.lock.json"
cp -R "$package_root/release/licenses" "$missing_package/release/licenses"
expect_failure_message missing-packaged-slice-sbom 'x86_64 packaged SBOM is missing' \
    bash "$checker" verify-macos-build \
        "$missing_package/tgcli" \
        "$missing_package/PROVENANCE.json" \
        "$missing_package/SBOM.json" \
        macos-universal \
        "$source_sha" \
        "$source_tree" \
        "$missing_package/release/dependencies.lock.json" \
        "$missing_package/PROVENANCE-arm64.json" \
        "$missing_package/SBOM-arm64.json" \
        "$missing_package/PROVENANCE-x86_64.json" \
        "$missing_package/SBOM-x86_64.json"

symlink_package="$fixture_root/package/symlink-slice"
mkdir -p "$symlink_package/release"
cp "$package_root/tgcli" "$package_root/PROVENANCE.json" "$package_root/SBOM.json" \
    "$package_root/PROVENANCE-arm64.json" "$package_root/PROVENANCE-x86_64.json" \
    "$package_root/SBOM-x86_64.json" "$symlink_package/"
ln -s "$package_root/SBOM-arm64.json" "$symlink_package/SBOM-arm64.json"
cp "$package_root/release/dependencies.lock.json" \
    "$symlink_package/release/dependencies.lock.json"
cp -R "$package_root/release/licenses" "$symlink_package/release/licenses"
expect_failure_message symlink-packaged-slice-sbom 'cannot be a symlink' \
    bash "$checker" verify-macos-build \
        "$symlink_package/tgcli" \
        "$symlink_package/PROVENANCE.json" \
        "$symlink_package/SBOM.json" \
        macos-universal \
        "$source_sha" \
        "$source_tree" \
        "$symlink_package/release/dependencies.lock.json" \
        "$symlink_package/PROVENANCE-arm64.json" \
        "$symlink_package/SBOM-arm64.json" \
        "$symlink_package/PROVENANCE-x86_64.json" \
        "$symlink_package/SBOM-x86_64.json"

tampered_package="$fixture_root/package/tampered-slice"
cp -R "$package_root" "$tampered_package"
printf '\n' >> "$tampered_package/SBOM-arm64.json"
expect_failure_message tampered-packaged-slice-sbom \
    'universal provenance does not bind both architecture slices' \
    bash "$checker" verify-macos-build \
        "$tampered_package/tgcli" \
        "$tampered_package/PROVENANCE.json" \
        "$tampered_package/SBOM.json" \
        macos-universal \
        "$source_sha" \
        "$source_tree" \
        "$tampered_package/release/dependencies.lock.json" \
        "$tampered_package/PROVENANCE-arm64.json" \
        "$tampered_package/SBOM-arm64.json" \
        "$tampered_package/PROVENANCE-x86_64.json" \
        "$tampered_package/SBOM-x86_64.json"

swapped_package="$fixture_root/package/swapped-slices"
mkdir -p "$swapped_package/release"
cp "$package_root/tgcli" "$package_root/PROVENANCE.json" "$package_root/SBOM.json" \
    "$package_root/PROVENANCE-arm64.json" "$package_root/PROVENANCE-x86_64.json" \
    "$swapped_package/"
cp "$package_root/SBOM-x86_64.json" "$swapped_package/SBOM-arm64.json"
cp "$package_root/SBOM-arm64.json" "$swapped_package/SBOM-x86_64.json"
cp "$package_root/release/dependencies.lock.json" \
    "$swapped_package/release/dependencies.lock.json"
cp -R "$package_root/release/licenses" "$swapped_package/release/licenses"
expect_failure_message swapped-packaged-slice-sboms \
    'packaged slice SBOM and provenance identities differ' \
    bash "$checker" verify-macos-build \
        "$swapped_package/tgcli" \
        "$swapped_package/PROVENANCE.json" \
        "$swapped_package/SBOM.json" \
        macos-universal \
        "$source_sha" \
        "$source_tree" \
        "$swapped_package/release/dependencies.lock.json" \
        "$swapped_package/PROVENANCE-arm64.json" \
        "$swapped_package/SBOM-arm64.json" \
        "$swapped_package/PROVENANCE-x86_64.json" \
        "$swapped_package/SBOM-x86_64.json"

ln -s macos-arm64 "$fixture_root/build/macos-arm64-link"
expect_failure_message symlink-macos-inspection 'not an executable regular file' \
    bash "$checker" inspect-macos "$fixture_root/build/macos-arm64-link"
ln -s macos-arm64-SBOM.json "$fixture_root/build/macos-arm64-SBOM-link.json"
expect_failure_message symlink-macos-sbom 'macOS SBOM cannot be a symlink' \
    bash "$checker" verify-macos-build \
        "$fixture_root/build/macos-arm64" \
        "$fixture_root/build/macos-arm64-provenance.json" \
        "$fixture_root/build/macos-arm64-SBOM-link.json" \
        macos-arm64 \
        "$source_sha" \
        "$source_tree" \
        "$fixture_root/release/dependencies.lock.json"

printf 'tampered slice\n' >> "$fixture_root/build/macos-arm64"
expect_failure_message tampered-macos-architecture \
    'provenance does not identify the macOS artifact' \
    bash "$checker" verify-macos-build \
        "$fixture_root/build/macos-arm64" \
        "$fixture_root/build/macos-arm64-provenance.json" \
        "$fixture_root/build/macos-arm64-SBOM.json" \
        macos-arm64 \
        "$source_sha" \
        "$source_tree" \
        "$fixture_root/release/dependencies.lock.json"
expect_failure_message tampered-macos-slice \
    'universal provenance does not bind both architecture slices' \
    bash "$checker" verify-macos-build \
        "$fixture_root/build/macos-universal" \
        "$fixture_root/build/macos-universal-provenance.json" \
        "$fixture_root/build/macos-universal-SBOM.json" \
        macos-universal \
        "$source_sha" \
        "$source_tree" \
        "$fixture_root/release/dependencies.lock.json" \
        "$fixture_root/build/macos-arm64" \
        "$fixture_root/build/macos-arm64-provenance.json" \
        "$fixture_root/build/macos-arm64-SBOM.json" \
        "$fixture_root/build/macos-x86_64" \
        "$fixture_root/build/macos-x86_64-provenance.json" \
        "$fixture_root/build/macos-x86_64-SBOM.json"

printf 'tampered universal\n' >> "$fixture_root/build/macos-universal"
expect_failure_message tampered-macos-universal \
    'provenance does not identify the macOS artifact' \
    bash "$checker" verify-macos-build \
        "$fixture_root/build/macos-universal" \
        "$fixture_root/build/macos-universal-provenance.json" \
        "$fixture_root/build/macos-universal-SBOM.json" \
        macos-universal \
        "$source_sha" \
        "$source_tree" \
        "$fixture_root/release/dependencies.lock.json" \
        "$fixture_root/build/macos-arm64" \
        "$fixture_root/build/macos-arm64-provenance.json" \
        "$fixture_root/build/macos-arm64-SBOM.json" \
        "$fixture_root/build/macos-x86_64" \
        "$fixture_root/build/macos-x86_64-provenance.json" \
        "$fixture_root/build/macos-x86_64-SBOM.json"

printf 'tampered\n' >> "$fixture_root/build/tgcli"
expect_failure_message tampered-linux-provenance 'does not identify the artifact' \
    bash "$checker" verify-linux-build \
        "$fixture_root/build/tgcli" \
        "$fixture_root/build/provenance.json" \
        "$fixture_root/build/SBOM.json" \
        "$source_sha" \
        "$source_tree" \
        "$toolchain_image" \
        "$fixture_root"

bundle_directory="$fixture_root/bundle"
mkdir "$bundle_directory"
printf 'linux archive\n' > "$bundle_directory/tgcli-1.2.3-linux-x86_64-musl.tar.gz"
printf 'macOS archive\n' > "$bundle_directory/tgcli-1.2.3-macos-universal.tar.gz"
(
    cd "$bundle_directory"
    sha256sum \
        tgcli-1.2.3-linux-x86_64-musl.tar.gz \
        tgcli-1.2.3-macos-universal.tar.gz \
        | LC_ALL=C sort -k2 > SHA256SUMS
)
bash "$checker" verify-release-bundle 1.2.3 "$bundle_directory"

printf '{"bundle":true}\n' > "$bundle_directory/SHA256SUMS.sigstore.json"
printf '{"bundle":true}\n' \
    > "$bundle_directory/tgcli-1.2.3-linux-x86_64-musl.tar.gz.sigstore.json"
printf '{"bundle":true}\n' \
    > "$bundle_directory/tgcli-1.2.3-macos-universal.tar.gz.sigstore.json"
bash "$checker" verify-signed-layout 1.2.3 "$bundle_directory"

python3 - "$bundle_directory" "$source_sha" <<'PY'
import hashlib
import json
import pathlib
import sys


directory = pathlib.Path(sys.argv[1])
source_sha = sys.argv[2]
assets = []
for file in sorted(directory.iterdir()):
    assets.append(
        {
            "digest": f"sha256:{hashlib.sha256(file.read_bytes()).hexdigest()}",
            "name": file.name,
            "size": file.stat().st_size,
            "state": "uploaded",
        }
    )
release = {
    "assets": assets,
    "draft": False,
    "name": "v1.2.3",
    "prerelease": False,
    "tag_name": "v1.2.3",
    "target_commitish": source_sha,
}
(directory.parent / "published-release.json").write_text(
    json.dumps(release) + "\n", encoding="utf-8"
)
release["draft"] = True
release["assets"] = assets[:-1]
(directory.parent / "partial-draft.json").write_text(
    json.dumps(release) + "\n", encoding="utf-8"
)
release["assets"][0]["digest"] = "sha256:" + "0" * 64
(directory.parent / "mismatched-draft.json").write_text(
    json.dumps(release) + "\n", encoding="utf-8"
)
PY

release_state="$(
    bash "$checker" classify-release-state \
        1.2.3 \
        "$bundle_directory" \
        v1.2.3 \
        "$source_sha" \
        "$fixture_root/published-release.json"
)"
if [[ "$release_state" != published ]]; then
    printf 'complete published release was not accepted\n' >&2
    exit 1
fi

release_state="$(
    bash "$checker" classify-release-state \
        1.2.3 \
        "$bundle_directory" \
        v1.2.3 \
        "$source_sha" \
        "$fixture_root/partial-draft.json"
)"
expected_missing="$(basename "$(find "$bundle_directory" -mindepth 1 -maxdepth 1 -type f | LC_ALL=C sort | tail -n1)")"
if [[ "$release_state" != $'draft\n'"$expected_missing" ]]; then
    printf 'partial draft did not report its exact missing asset\n' >&2
    exit 1
fi

expect_failure_message mismatched-release-asset \
    'existing release asset differs from local content' \
    bash "$checker" classify-release-state \
        1.2.3 \
        "$bundle_directory" \
        v1.2.3 \
        "$source_sha" \
        "$fixture_root/mismatched-draft.json"

tag_one="$(printf '1%.0s' {1..40})"
tag_two="$(printf '2%.0s' {1..40})"
mock_gh="$fixture_root/mock-gh"
cat > "$mock_gh" <<'MOCK_GH'
#!/usr/bin/env bash
set -euo pipefail

[[ "$1" == api ]]
case "$GH_MOCK_MODE:$2" in
    lightweight:repos/test/repo/git/ref/tags/v1.2.3)
        printf '{"object":{"sha":"%s","type":"commit"}}\n' "$GH_MOCK_SOURCE_SHA"
        ;;
    moved:repos/test/repo/git/ref/tags/v1.2.3)
        printf '{"object":{"sha":"%040d","type":"commit"}}\n' 9
        ;;
    annotated:repos/test/repo/git/ref/tags/v1.2.3)
        printf '{"object":{"sha":"%s","type":"tag"}}\n' "$GH_MOCK_TAG_ONE"
        ;;
    annotated:repos/test/repo/git/tags/"$GH_MOCK_TAG_ONE")
        printf '{"sha":"%s","object":{"sha":"%s","type":"tag"}}\n' \
            "$GH_MOCK_TAG_ONE" "$GH_MOCK_TAG_TWO"
        ;;
    annotated:repos/test/repo/git/tags/"$GH_MOCK_TAG_TWO")
        printf '{"sha":"%s","object":{"sha":"%s","type":"commit"}}\n' \
            "$GH_MOCK_TAG_TWO" "$GH_MOCK_SOURCE_SHA"
        ;;
    *)
        exit 1
        ;;
esac
MOCK_GH
chmod 0755 "$mock_gh"

GH_TOOL="$mock_gh" \
GH_MOCK_MODE=lightweight \
GH_MOCK_SOURCE_SHA="$source_sha" \
    bash "$checker" verify-remote-tag "$source_sha" test/repo v1.2.3
GH_TOOL="$mock_gh" \
GH_MOCK_MODE=annotated \
GH_MOCK_SOURCE_SHA="$source_sha" \
GH_MOCK_TAG_ONE="$tag_one" \
GH_MOCK_TAG_TWO="$tag_two" \
    bash "$checker" verify-remote-tag "$source_sha" test/repo v1.2.3
expect_failure_message moved-release-tag \
    'remote release tag no longer identifies the built source' \
    env \
        GH_TOOL="$mock_gh" \
        GH_MOCK_MODE=moved \
        GH_MOCK_SOURCE_SHA="$source_sha" \
        bash "$checker" verify-remote-tag "$source_sha" test/repo v1.2.3

printf 'unexpected\n' > "$bundle_directory/unexpected"
expect_failure_message unexpected-signed-file 'unexpected file set' \
    bash "$checker" verify-signed-layout 1.2.3 "$bundle_directory"

python3 - "$root/.github/workflows/release.yml" <<'PY'
import pathlib
import sys

workflow = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
required = [
    "push:\n    tags:\n      - 'v*'",
    "PYTHONDONTWRITEBYTECODE: '1'",
    'bash "$BUILD_RECIPE" fetch',
    "--init \\",
    "--network=none \\",
    "--pull=never \\",
    "--component openssl",
    "staged-archive-name",
    "tree-sha256 --root",
    "--libdir=lib",
    "no-shared",
    "-DOPENSSL_USE_STATIC_LIBS=TRUE",
    "verify-macos-build",
    "verify-schema-package",
    "verify_re2_build.py",
    "write-sbom",
    "verify-sbom",
    "SBOM-arm64.json",
    "SBOM-x86_64.json",
    "SBOM.json",
    "sbom_sha256",
    "verify-remote-tag",
    "classify-release-state",
    'gh release create "$TAG_NAME"',
    'gh release upload "$TAG_NAME" "${missing_assets[@]}"',
    'gh release edit "$TAG_NAME" --verify-tag --draft=false',
]
for snippet in required:
    if snippet not in workflow:
        raise SystemExit(f"release workflow is missing required contract: {snippet}")
for forbidden in ("workflow_dispatch:", "pull_request:", "branches:"):
    if forbidden in workflow:
        raise SystemExit(f"release workflow has a forbidden trigger: {forbidden}")
if workflow.index('bash "$BUILD_RECIPE" fetch') > workflow.index("docker run"):
    raise SystemExit("Linux dependency prefetch must precede the offline container")
if workflow.count("contents: write") != 1 or workflow.count("id-token: write") != 1:
    raise SystemExit("release privileges must be scoped to separate single jobs")
if "--env HOME=" in workflow or "--env TMPDIR=" in workflow:
    raise SystemExit("the workflow must not override recipe-owned HOME or TMPDIR")
metadata = workflow.split("\n  metadata:\n", 1)[1].split("\n  linux-musl:\n", 1)[0]
if metadata.index("Validate the tag source and version") > metadata.index(
    "Verify the release artifact inspector"
):
    raise SystemExit("tag source identity must be validated before repository code runs")

linux = workflow.split("\n  linux-musl:\n", 1)[1].split("\n  macos-arch:\n", 1)[0]
linux_package = linux.split("      - name: Package the Linux artifact\n", 1)[1]
for schema_file in (
    "docs/schemas/listen.item.schema.json",
    "docs/schemas/stream-manifest.json",
    "docs/schemas/stream.error.schema.json",
    "docs/schemas/wait-for.result.schema.json",
    "docs/schemas/session-list.result.schema.json",
    "docs/schemas/session-terminate.result.schema.json",
    "docs/schemas/session.error.schema.json",
):
    if schema_file not in linux_package:
        raise SystemExit(f"Linux package is missing packaged schema {schema_file}")
if "verify-schema-package" not in linux_package:
    raise SystemExit("Linux package does not verify its stream schema catalog")

macos = workflow.split("\n  macos-arch:\n", 1)[1].split(
    "\n  macos-universal:\n", 1
)[0]
if "brew install ccache coreutils gperf jq ninja openssl@3" in macos:
    raise SystemExit("macOS cannot use a floating Homebrew OpenSSL input")
if "install -D" in macos:
    raise SystemExit("macOS jobs cannot use GNU-only install -D")
if macos.index("ctest --test-dir") > macos.index(
    "Reverify source identity after the macOS build"
):
    raise SystemExit("macOS source identity must be rechecked after tests")

universal = workflow.split("\n  macos-universal:\n", 1)[1].split(
    "\n  sign:\n", 1
)[0]
if "install -D" in universal:
    raise SystemExit("macOS universal packaging cannot use GNU-only install -D")
for evidence in (
    "arm64_binary_sha256",
    "arm64_provenance_sha256",
    "arm64_sbom_sha256",
    "x86_64_binary_sha256",
    "x86_64_provenance_sha256",
    "x86_64_sbom_sha256",
):
    if evidence not in universal:
        raise SystemExit(f"universal provenance is missing {evidence}")
package_step = universal.split("      - name: Package the universal binary\n", 1)[1]
for schema_file in (
    "docs/schemas/listen.item.schema.json",
    "docs/schemas/stream-manifest.json",
    "docs/schemas/stream.error.schema.json",
    "docs/schemas/wait-for.result.schema.json",
    "docs/schemas/session-list.result.schema.json",
    "docs/schemas/session-terminate.result.schema.json",
    "docs/schemas/session.error.schema.json",
):
    if schema_file not in package_step:
        raise SystemExit(f"universal package is missing packaged schema {schema_file}")
if "verify-schema-package" not in package_step:
    raise SystemExit("universal package does not verify its stream schema catalog")
package_verification = package_step.split(
    "bash scripts/check-release-artifact.sh verify-macos-build \\\n", 1
)[1].split('          find "package/$package"', 1)[0]
if "components/" in package_verification:
    raise SystemExit("packaged universal verification cannot use external slice files")
for packaged_evidence in (
    '"package/$package/PROVENANCE-arm64.json"',
    '"package/$package/SBOM-arm64.json"',
    '"package/$package/PROVENANCE-x86_64.json"',
    '"package/$package/SBOM-x86_64.json"',
):
    if packaged_evidence not in package_verification:
        raise SystemExit(
            f"packaged universal verification is missing {packaged_evidence}"
        )

sign = workflow.split("\n  sign:\n", 1)[1].split("\n  release:\n", 1)[0]
release = workflow.split("\n  release:\n", 1)[1]
for snippet in ("contents: read", "id-token: write", "cosign sign-blob"):
    if snippet not in sign:
        raise SystemExit(f"signing job is missing {snippet}")
for forbidden in ("contents: write", "gh release"):
    if forbidden in sign:
        raise SystemExit(f"signing job has publication authority: {forbidden}")
for snippet in ("contents: write", "cosign verify-blob", "verify_remote_tag"):
    if snippet not in release:
        raise SystemExit(f"release job is missing {snippet}")
for forbidden in ("id-token: write", "cosign sign-blob"):
    if forbidden in release:
        raise SystemExit(f"release job has signing authority: {forbidden}")
PY

printf 'release artifact checks: ok\n'
