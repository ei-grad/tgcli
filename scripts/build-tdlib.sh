#!/usr/bin/env bash
# Build the pinned tdlib revision into a local prefix for -DTGCLI_SYSTEM_TDLIB=ON.
#
# tgcli supports exactly one tdlib provenance: the pinned revision below
# (DESIGN.md §8/§13). Distro tdlib packages are not supported because the
# JSON-conversion layer (td_api_json) that `raw` and `--full` need is
# generated tdlib-internal code and is not part of tdlib's public install;
# this script installs it into the prefix on top of the standard install.
#
# Usage: scripts/build-tdlib.sh [PREFIX]
#   PREFIX               install destination
#                        (default: $XDG_CACHE_HOME/tgcli-dev/tdlib-<rev12>)
# Environment:
#   TDLIB_SRC            existing tdlib checkout to reuse (cloned otherwise)
#   JOBS                 parallel build jobs (default: nproc)
set -euo pipefail

TDLIB_REV=a17f87c4cff7b90b278d12b91ba0614383aaee82  # v1.8.65, 2026-06-13
TDLIB_REPOSITORY=https://github.com/tdlib/td

CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/tgcli-dev"
SRC_DIR="${TDLIB_SRC:-$CACHE_DIR/td}"
PREFIX="${1:-$CACHE_DIR/tdlib-${TDLIB_REV:0:12}}"
BUILD_DIR="$CACHE_DIR/tdlib-build-${TDLIB_REV:0:12}"
if [ "$(uname -s)" = "Darwin" ]; then
    JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
else
    JOBS="${JOBS:-$(nproc)}"
fi

if [ ! -d "$SRC_DIR/.git" ]; then
    git clone "$TDLIB_REPOSITORY" "$SRC_DIR"
fi
if ! git -C "$SRC_DIR" cat-file -e "$TDLIB_REV^{commit}" 2>/dev/null; then
    git -C "$SRC_DIR" fetch origin "$TDLIB_REV"
fi
git -C "$SRC_DIR" -c advice.detachedHead=false checkout --quiet "$TDLIB_REV"
if [ "$(git -C "$SRC_DIR" rev-parse HEAD)" != "$TDLIB_REV" ]; then
    echo "tdlib checkout did not resolve to $TDLIB_REV" >&2
    exit 1
fi
if [ -n "$(git -C "$SRC_DIR" status --porcelain)" ]; then
    echo "tdlib source checkout must be clean: $SRC_DIR" >&2
    exit 1
fi

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DBUILD_TESTING=OFF
)
if command -v ninja >/dev/null; then
    CMAKE_ARGS+=(-G Ninja)
fi
if command -v ccache >/dev/null; then
    CMAKE_ARGS+=(
        -DCMAKE_C_COMPILER_LAUNCHER=ccache
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    )
fi
# macOS ships LibreSSL; tdlib needs OpenSSL, which brew installs keg-only.
if [ "$(uname -s)" = "Darwin" ] && command -v brew >/dev/null; then
    CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)")
fi

cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j "$JOBS" --target install

# The JSON-conversion layer: libtdjson_private.a is installed by the standard
# install rules, but its headers are not. td_api_json.h is generated into the
# source tree; tl_json.h pulls in tdutils headers, of which config.h is
# generated into the build tree.
install -Dm644 "$SRC_DIR/td/generate/auto/td/telegram/td_api_json.h" \
    "$PREFIX/include/td/telegram/td_api_json.h"
install -Dm644 "$SRC_DIR/td/tl/tl_json.h" "$PREFIX/include/td/tl/tl_json.h"
(cd "$SRC_DIR/tdutils" && find td/utils -name '*.h' -print0 \
    | xargs -0 cp --parents -t "$PREFIX/include")
install -Dm644 "$BUILD_DIR/tdutils/td/utils/config.h" \
    "$PREFIX/include/td/utils/config.h"

# Smoke check: the exported JSON-conversion header must compile standalone.
CHECK_TU="$BUILD_DIR/tgcli-prefix-check.cpp"
printf '#include <td/telegram/td_api_json.h>\nint main() { return 0; }\n' \
    > "$CHECK_TU"
c++ -std=c++20 -fsyntax-only -I"$PREFIX/include" "$CHECK_TU"

PROVENANCE_FILE="$BUILD_DIR/tgcli-tdlib-source.json"
printf '%s\n' \
    '{' \
    '  "schema_version": 1,' \
    "  \"source_repository\": \"$TDLIB_REPOSITORY\"," \
    "  \"immutable_ref\": \"$TDLIB_REV\"" \
    '}' > "$PROVENANCE_FILE"
install -Dm644 "$PROVENANCE_FILE" \
    "$PREFIX/share/tgcli/tdlib-source.json"

echo "tdlib $TDLIB_REV installed into: $PREFIX" >&2
echo "$PREFIX"
