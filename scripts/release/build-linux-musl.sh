#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIRECTORY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIRECTORY
REPO_ROOT="$(cd "$SCRIPT_DIRECTORY/../.." && pwd)"
readonly REPO_ROOT
readonly LOCK_FILE="$REPO_ROOT/release/dependencies.lock.json"
readonly CONTRACT_FILE="$REPO_ROOT/release/linux-musl-toolchain.json"
readonly VERIFIER="$REPO_ROOT/scripts/verify_dependency_lock.py"
readonly ARCHIVE_TOOL="$REPO_ROOT/scripts/release/archive_tool.py"
readonly ARTIFACT_INSPECTOR="$REPO_ROOT/scripts/release/inspect_linux_artifact.py"
readonly PROVENANCE_TOOL="$REPO_ROOT/scripts/release/build_provenance.py"
readonly RE2_BUILD_VERIFIER="$REPO_ROOT/scripts/release/verify_re2_build.py"
readonly RUNTIME_VERIFIER="$REPO_ROOT/scripts/release/verify_toolchain_runtime.py"
readonly INPUT_DIRECTORY="${TGCLI_RELEASE_INPUTS_DIR:-$REPO_ROOT/build/release-inputs}"
readonly OUTPUT_DIRECTORY="${TGCLI_RELEASE_OUTPUT_DIR:-$REPO_ROOT/build/release-static}"
readonly WORK_DIRECTORY="${TGCLI_RELEASE_WORK_DIR:-$REPO_ROOT/build/release-linux-musl}"

fail() {
    printf '%s\n' "$1" >&2
    return 1
}

read_json() {
    python3 - "$1" "$2" <<'PY'
import json
import sys

document = json.load(open(sys.argv[1], encoding="utf-8"))
value = document
for key in sys.argv[2].split("."):
    if isinstance(value, list) and key.isdecimal():
        value = value[int(key)]
    else:
        value = value[key]
if not isinstance(value, (str, int)):
    raise SystemExit("requested JSON value is not scalar")
print(value)
PY
}

read_component_json() {
    python3 - "$LOCK_FILE" "$1" "$2" <<'PY'
import json
import sys

document = json.load(open(sys.argv[1], encoding="utf-8"))
matches = [item for item in document["components"] if item["id"] == sys.argv[2]]
if len(matches) != 1:
    raise SystemExit(f"cannot resolve locked component: {sys.argv[2]}")
value = matches[0][sys.argv[3]]
if not isinstance(value, (str, int)) or isinstance(value, bool):
    raise SystemExit("requested component value is not scalar")
print(value)
PY
}

extract_archive() {
    local archive_file="$1"
    local destination="$2"

    [[ ! -e "$destination" && ! -L "$destination" ]] || \
        fail "archive extraction destination already exists: $destination"
    python3 "$ARCHIVE_TOOL" extract \
        --archive "$archive_file" \
        --destination "$destination" \
        --max-members "$MAX_ARCHIVE_MEMBERS" \
        --max-member-size "$MAX_ARCHIVE_MEMBER_SIZE" \
        --max-expanded-size "$MAX_ARCHIVE_EXPANDED_SIZE"
}

single_source_root() {
    local extraction_directory="$1"
    local -a entries=()

    mapfile -d '' -t entries < <(
        find "$extraction_directory" -mindepth 1 -maxdepth 1 -print0
    )
    [[ "${#entries[@]}" -eq 1 && -d "${entries[0]}" && ! -L "${entries[0]}" ]] || \
        fail "archive does not contain exactly one source root: $extraction_directory"
    printf '%s\n' "${entries[0]}"
}

verify_source_tree() {
    local component_id="$1"
    local source_directory="$2"
    local digest_field="${3:-source_tree_sha256}"
    local actual
    local expected

    expected="$(read_component_json "$component_id" "$digest_field")"
    actual="$(python3 "$ARCHIVE_TOOL" tree-sha256 --root "$source_directory")"
    [[ "$actual" == "$expected" ]] || \
        fail "$component_id source tree differs from dependency lock"
}

normalize_source_timestamps() {
    local source_directory="$1"

    find "$source_directory" \
        -exec touch -h -d "@$SOURCE_DATE_EPOCH" -- {} +
}

verify_sha256() {
    local expected="$1"
    local checked_file="$2"
    local actual

    actual="$(sha256sum "$checked_file" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] || \
        fail "SHA256 mismatch for $checked_file"
}

prepare_sources() {
    local component_id
    local extraction_directory
    local source_directory
    local suffix

    for component_id in \
        tdlib re2 cli11 nlohmann_json fmt tomlplusplus openssl zlib catch2 jsoncons gperf; do
        suffix=.tar.gz
        extraction_directory="$WORK_DIRECTORY/sources/$component_id"
        extract_archive "$INPUT_DIRECTORY/$component_id$suffix" "$extraction_directory"
        source_directory="$(single_source_root "$extraction_directory")"
        verify_source_tree "$component_id" "$source_directory"
        normalize_source_timestamps "$source_directory"
        printf -v "SOURCE_${component_id^^}" '%s' "$source_directory"
    done
}

write_toolchain_file() {
    cat > "$WORK_DIRECTORY/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$CROSS_CC")
set(CMAKE_CXX_COMPILER "$CROSS_CXX")
set(CMAKE_AR "$CROSS_AR")
set(CMAKE_RANLIB "$CROSS_RANLIB")
set(CMAKE_STRIP "$CROSS_STRIP")
set(CMAKE_SYSROOT "$TOOLCHAIN_ROOT/x86_64-unknown-linux-musl/sysroot")
set(CMAKE_FIND_ROOT_PATH
    "$TOOLCHAIN_ROOT/x86_64-unknown-linux-musl/sysroot"
    "$TARGET_PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CROSSCOMPILING_EMULATOR "/usr/bin/env")
EOF
}

verify_toolchain() {
    local archive_root
    local compiler_relative
    local compiler_target
    local compiler_version
    local expected_build_log
    local source_evidence
    local runtime_files

    archive_root="$(read_json "$LOCK_FILE" release_toolchains.0.archive_root)"
    [[ "$archive_root" == x86_64-unknown-linux-musl ]] || \
        fail "unexpected Linux release toolchain archive root"
    TOOLCHAIN_ROOT="$WORK_DIRECTORY/toolchain/$archive_root"
    [[ -d "$TOOLCHAIN_ROOT" && ! -L "$TOOLCHAIN_ROOT" ]] || \
        fail "toolchain archive root is missing or unsafe"

    compiler_relative="$(read_json "$LOCK_FILE" release_toolchains.0.compiler.path)"
    compiler_target="$(read_json "$LOCK_FILE" release_toolchains.0.compiler.target)"
    compiler_version="$(read_json "$LOCK_FILE" release_toolchains.0.compiler.version)"
    CROSS_CXX="$TOOLCHAIN_ROOT/$compiler_relative"
    CROSS_CC="${CROSS_CXX%g++}gcc"
    CROSS_AR="${CROSS_CXX%g++}ar"
    CROSS_RANLIB="${CROSS_CXX%g++}ranlib"
    CROSS_STRIP="${CROSS_CXX%g++}strip"
    for tool in "$CROSS_CC" "$CROSS_CXX" "$CROSS_AR" "$CROSS_RANLIB" "$CROSS_STRIP"; do
        [[ -x "$tool" && ! -L "$tool" ]] || fail "missing pinned cross tool: $tool"
    done
    [[ "$("$CROSS_CXX" -dumpmachine)" == "$compiler_target" ]] || \
        fail "cross compiler target differs from dependency lock"
    [[ "$("$CROSS_CXX" -dumpfullversion -dumpversion)" == "$compiler_version" ]] || \
        fail "cross compiler version differs from dependency lock"

    expected_build_log="$(read_json "$LOCK_FILE" release_toolchains.0.producer_build_log.sha256)"
    verify_sha256 "$expected_build_log" "$TOOLCHAIN_ROOT/build.log.bz2"

    source_evidence="$WORK_DIRECTORY/toolchain-source-evidence.tsv"
    runtime_files="$WORK_DIRECTORY/toolchain-runtime-files.tsv"
    python3 - "$LOCK_FILE" "$source_evidence" "$runtime_files" <<'PY'
import json
import pathlib
import sys

lock = json.load(open(sys.argv[1], encoding="utf-8"))
toolchain = lock["release_toolchains"][0]
pathlib.Path(sys.argv[2]).write_text(
    "".join(
        f'{entry["archive_filename"]}\t{entry["sha512"]}\n'
        for entry in toolchain["source_evidence"]
    ),
    encoding="utf-8",
)
pathlib.Path(sys.argv[3]).write_text(
    "".join(
        f'{entry["path"]}\t{entry["sha256"]}\n'
        for entry in toolchain["runtime_files"]
    ),
    encoding="utf-8",
)
PY
    while IFS=$'\t' read -r archive_filename expected_sha512; do
        bzgrep -Fq \
            "Correct sha512 digest for $archive_filename: $expected_sha512" \
            "$TOOLCHAIN_ROOT/build.log.bz2" || \
            fail "toolchain build log lacks locked source evidence for $archive_filename"
    done < "$source_evidence"
    while IFS=$'\t' read -r runtime_file expected_sha256; do
        verify_sha256 "$expected_sha256" "$TOOLCHAIN_ROOT/$runtime_file"
    done < "$runtime_files"
}

write_build_provenance() {
    local cmake_version
    local compiler_version
    local ninja_version

    cmake_version="$(cmake --version | sed -n '1p')"
    compiler_version="$("$CROSS_CXX" --version | sed -n '1p')"
    ninja_version="$(ninja --version)"
    python3 "$PROVENANCE_TOOL" write \
        --repo-root "$REPO_ROOT" \
        --expected-commit "$TGCLI_SOURCE_SHA" \
        --artifact "$OUTPUT_DIRECTORY/tgcli" \
        --lock "$LOCK_FILE" \
        --contract "$CONTRACT_FILE" \
        --recipe "$SCRIPT_DIRECTORY/build-linux-musl.sh" \
        --inspection "$WORK_DIRECTORY/artifact-inspection.json" \
        --runtime-selection "$WORK_DIRECTORY/toolchain-runtime-selection.json" \
        --test-evidence "$WORK_DIRECTORY/test-evidence.json" \
        --re2-build-evidence "$WORK_DIRECTORY/re2-build-evidence.json" \
        --output "$OUTPUT_DIRECTORY/release-build-provenance.json" \
        --image "$PINNED_IMAGE" \
        --cmake-version "$cmake_version" \
        --compiler-version "$compiler_version" \
        --ninja-version "$ninja_version"
}

fetch_inputs() {
    exec python3 "$VERIFIER" \
        --repo-root "$REPO_ROOT" \
        --network \
        --download-directory "$INPUT_DIRECTORY" \
        --build-inputs
}

build_release() {
    local actual_home_identity
    local canonical_work_directory
    local expected_image
    local expected_home_identity
    local jobs
    local common_flags
    local private_home

    [[ "${SOURCE_DATE_EPOCH:-}" =~ ^[1-9][0-9]*$ ]] || \
        fail "SOURCE_DATE_EPOCH must be a positive integer"
    [[ "${TGCLI_SOURCE_SHA:-}" =~ ^[0-9a-f]{40}$ ]] || \
        fail "TGCLI_SOURCE_SHA must be an exact Git commit"
    private_home="$WORK_DIRECTORY/release-home"
    HOME="$private_home"
    export HOME
    canonical_work_directory="$(realpath -m -- "$WORK_DIRECTORY")"
    [[ "$canonical_work_directory" == "$WORK_DIRECTORY" ]] || \
        fail "release work directory must be an exact canonical path"
    [[ "$WORK_DIRECTORY" == "$REPO_ROOT/build/"* ]] || \
        fail "release work directory must be inside the repository build root"
    python3 "$PROVENANCE_TOOL" source-identity \
        --repo-root "$REPO_ROOT" \
        --expected-commit "$TGCLI_SOURCE_SHA" >/dev/null
    expected_image="$(read_json "$CONTRACT_FILE" image)"
    [[ -z "${TOOLCHAIN_IMAGE:-}" || "$TOOLCHAIN_IMAGE" == "$expected_image" ]] || \
        fail "TOOLCHAIN_IMAGE differs from the pinned build contract"
    PINNED_IMAGE="$expected_image"
    [[ ! -e "$WORK_DIRECTORY" && ! -L "$WORK_DIRECTORY" ]] || \
        fail "release work directory already exists: $WORK_DIRECTORY"
    [[ ! -e "$OUTPUT_DIRECTORY" && ! -L "$OUTPUT_DIRECTORY" ]] || \
        fail "release output directory already exists: $OUTPUT_DIRECTORY"
    mkdir -p -m 0700 "$private_home"
    [[ -d "$WORK_DIRECTORY" && ! -L "$WORK_DIRECTORY" ]] || \
        fail "release work directory is not a private directory"
    [[ "$(cd "$WORK_DIRECTORY" && pwd -P)" == "$WORK_DIRECTORY" ]] || \
        fail "release work directory identity changed during creation"
    [[ -d "$private_home" && ! -L "$private_home" ]] || \
        fail "release private HOME is not a directory"
    [[ "$(cd "$private_home" && pwd -P)" == "$private_home" ]] || \
        fail "release private HOME identity changed during creation"
    expected_home_identity="$(id -u):$(id -g):700"
    actual_home_identity="$(stat -c '%u:%g:%a' -- "$private_home")"
    [[ "$actual_home_identity" == "$expected_home_identity" ]] || \
        fail "release private HOME has unsafe owner or mode"

    python3 "$VERIFIER" \
        --repo-root "$REPO_ROOT" \
        --archive-directory "$INPUT_DIRECTORY" \
        --build-inputs

    MAX_ARCHIVE_MEMBERS="$(read_json "$LOCK_FILE" archive_policy.max_member_count)"
    MAX_ARCHIVE_MEMBER_SIZE="$(read_json "$LOCK_FILE" archive_policy.max_member_size)"
    MAX_ARCHIVE_EXPANDED_SIZE="$(read_json "$LOCK_FILE" archive_policy.max_expanded_size)"
    readonly MAX_ARCHIVE_MEMBERS MAX_ARCHIVE_MEMBER_SIZE MAX_ARCHIVE_EXPANDED_SIZE
    mkdir -p "$WORK_DIRECTORY/sources" "$OUTPUT_DIRECTORY"
    extract_archive \
        "$INPUT_DIRECTORY/linux-musl-toolchain.tar.xz" \
        "$WORK_DIRECTORY/toolchain"
    prepare_sources
    verify_toolchain

    readonly TARGET_ROOT="$WORK_DIRECTORY/target-root"
    readonly TARGET_PREFIX="$TARGET_ROOT/usr"
    readonly HOST_PREFIX="$WORK_DIRECTORY/host-prefix"
    mkdir -p "$TARGET_PREFIX" "$HOST_PREFIX"
    write_toolchain_file
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')"
    [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || jobs=1
    common_flags="-ffile-prefix-map=$REPO_ROOT=.tgcli-source -fdebug-prefix-map=$REPO_ROOT=.tgcli-source -fmacro-prefix-map=$REPO_ROOT=.tgcli-source -ffile-prefix-map=$WORK_DIRECTORY=.tgcli-build -fdebug-prefix-map=$WORK_DIRECTORY=.tgcli-build -fmacro-prefix-map=$WORK_DIRECTORY=.tgcli-build"

    (
        cd "$SOURCE_GPERF"
        ./configure --prefix="$HOST_PREFIX"
        make -j"$jobs"
        make install
    )
    export PATH="$TOOLCHAIN_ROOT/bin:$HOST_PREFIX/bin:$PATH"

    cmake -S "$SOURCE_TDLIB" -B "$WORK_DIRECTORY/tdlib-generator" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="$common_flags" \
        -DTD_ENABLE_DOTNET=OFF \
        -DTD_ENABLE_JNI=OFF \
        -DTD_GENERATE_SOURCE_FILES=ON
    cmake --build "$WORK_DIRECTORY/tdlib-generator" \
        --target prepare_cross_compiling \
        --parallel "$jobs"
    verify_source_tree \
        tdlib "$SOURCE_TDLIB" generated_source_tree_sha256

    cmake -S "$SOURCE_ZLIB" -B "$WORK_DIRECTORY/zlib" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$WORK_DIRECTORY/toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$TARGET_PREFIX" \
        -DCMAKE_C_FLAGS="$common_flags" \
        -DZLIB_BUILD_EXAMPLES=OFF \
        -DZLIB_BUILD_SHARED=OFF \
        -DZLIB_BUILD_STATIC=ON \
        -DZLIB_BUILD_TESTING=OFF
    cmake --build "$WORK_DIRECTORY/zlib" --parallel "$jobs"
    cmake --install "$WORK_DIRECTORY/zlib"

    (
        cd "$SOURCE_OPENSSL"
        env \
            AR=x86_64-unknown-linux-musl-ar \
            CC=x86_64-unknown-linux-musl-gcc \
            RANLIB=x86_64-unknown-linux-musl-ranlib \
            ./Configure linux-x86_64 \
                --prefix=/usr \
                --openssldir=/etc/ssl \
                --libdir=lib \
                no-apps no-docs no-module no-shared no-tests no-zlib no-pinshared
        make -j"$jobs"
        make install_sw DESTDIR="$TARGET_ROOT"
    )

    cmake -S "$REPO_ROOT" -B "$WORK_DIRECTORY/app" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$WORK_DIRECTORY/toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$TARGET_PREFIX" \
        -DCMAKE_C_FLAGS="$common_flags" \
        -DCMAKE_CXX_FLAGS="$common_flags" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--build-id=none -Wl,-Map,tgcli.link.map" \
        -DCMAKE_SHARED_LINKER_FLAGS="-static -Wl,--build-id=none" \
        -DBUILD_SHARED_LIBS=OFF \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DFETCHCONTENT_SOURCE_DIR_CATCH2="$SOURCE_CATCH2" \
        -DFETCHCONTENT_SOURCE_DIR_CLI11="$SOURCE_CLI11" \
        -DFETCHCONTENT_SOURCE_DIR_FMT="$SOURCE_FMT" \
        -DFETCHCONTENT_SOURCE_DIR_JSONCONS="$SOURCE_JSONCONS" \
        -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$SOURCE_NLOHMANN_JSON" \
        -DFETCHCONTENT_SOURCE_DIR_RE2="$SOURCE_RE2" \
        -DFETCHCONTENT_SOURCE_DIR_TD="$SOURCE_TDLIB" \
        -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS="$SOURCE_TOMLPLUSPLUS" \
        -DOPENSSL_ROOT_DIR="$TARGET_PREFIX" \
        -DOPENSSL_USE_STATIC_LIBS=TRUE \
        -DTGCLI_BUILD_TESTS=ON \
        -DTGCLI_RELEASE_ARCHIVE_MODE=ON \
        -DZLIB_ROOT="$TARGET_PREFIX" \
        -DZLIB_USE_STATIC_LIBS=TRUE
    cmake --build "$WORK_DIRECTORY/app" \
        --target tgcli_unit_tests \
        --parallel "$jobs"
    if ! (
        cd "$WORK_DIRECTORY"
        ctest --test-dir app --output-on-failure \
            --exclude-regex '^command-registry-completion-zsh$'
    ) >"$WORK_DIRECTORY/ctest.log" 2>&1; then
        sed -n '1,240p' "$WORK_DIRECTORY/ctest.log" >&2
        fail "release test invocation failed"
    fi
    sed -n '1,240p' "$WORK_DIRECTORY/ctest.log"
    python3 "$RE2_BUILD_VERIFIER" \
        --build-directory "$WORK_DIRECTORY/app" \
        --link-map "$WORK_DIRECTORY/app/tgcli.link.map" \
        --output "$WORK_DIRECTORY/re2-build-evidence.json"
    python3 "$PROVENANCE_TOOL" source-identity \
        --repo-root "$REPO_ROOT" \
        --expected-commit "$TGCLI_SOURCE_SHA" >/dev/null
    python3 - "$WORK_DIRECTORY/test-evidence.json" <<'PY'
import json
import pathlib
import sys

pathlib.Path(sys.argv[1]).write_text(
    json.dumps(
        {
            "argv": ["ctest", "--test-dir", "app", "--output-on-failure"],
            "binary": ".tgcli-build/app/tgcli_unit_tests",
            "passed": True,
            "working_directory": ".tgcli-build",
        },
        indent=2,
        sort_keys=True,
    )
    + "\n",
    encoding="utf-8",
)
PY
    python3 "$RUNTIME_VERIFIER" \
        --lock "$LOCK_FILE" \
        --toolchain-root "$TOOLCHAIN_ROOT" \
        --compiler "$CROSS_CXX" \
        --link-map "$WORK_DIRECTORY/app/tgcli.link.map" \
        --output "$WORK_DIRECTORY/toolchain-runtime-selection.json"

    install -m 0755 "$WORK_DIRECTORY/app/tgcli" "$OUTPUT_DIRECTORY/tgcli"
    "$CROSS_STRIP" --strip-debug "$OUTPUT_DIRECTORY/tgcli"
    python3 "$ARTIFACT_INSPECTOR" \
        --artifact "$OUTPUT_DIRECTORY/tgcli" \
        --output "$WORK_DIRECTORY/artifact-inspection.json"
    write_build_provenance
    python3 "$PROVENANCE_TOOL" write-sbom \
        --artifact "$OUTPUT_DIRECTORY/tgcli" \
        --lock "$LOCK_FILE" \
        --provenance "$OUTPUT_DIRECTORY/release-build-provenance.json" \
        --platform linux-x86_64-musl \
        --output "$OUTPUT_DIRECTORY/SBOM.json"
    touch -d "@$SOURCE_DATE_EPOCH" \
        "$OUTPUT_DIRECTORY/tgcli" \
        "$OUTPUT_DIRECTORY/release-build-provenance.json" \
        "$OUTPUT_DIRECTORY/SBOM.json"
}

export CCACHE_DISABLE=1
export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1
export TZ=UTC
export ZERO_AR_DATE=1
umask 022

case "${1:-build}" in
    fetch)
        [[ "$#" -eq 1 ]] || fail "fetch mode accepts no additional arguments"
        fetch_inputs
        ;;
    build)
        [[ "$#" -le 1 ]] || fail "build mode accepts no additional arguments"
        build_release
        ;;
    *)
        fail "usage: $0 [fetch|build]"
        ;;
esac
