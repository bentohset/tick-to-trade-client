#!/usr/bin/env bash
#
# Configure and build the project. Only builds: run tests and benchmarks yourself.
#
# Usage:
#   ./build.sh [options]
#
# Options:
#   --release   Release build in build-release/ (default: Debug build in build/).
#   --test      Also build the unit tests.
#   --bench     Also build the benchmarks (numbers only mean something in --release).
#   -h, --help  Show this help.
#
# Without --test/--bench those targets are left out, and so are their files in
# compile_commands.json. For full clangd coverage use: ./build.sh --test --bench
#
# Examples:
#   ./build.sh                            # Debug: libraries and apps
#   ./build.sh --test --bench             # Debug: everything
#   ./build.sh --release --bench          # Release: libraries, apps, benchmarks
#   ./build.sh --release --test --bench   # Release: everything
#
# Then, for example:
#   ctest --test-dir build --output-on-failure
#   build-release/bench/bench_itch_parse data/itch/12302019.NASDAQ_ITCH50

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RELEASE=0
TEST=0
BENCH=0

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*" >&2; }
usage() { sed -n '3,/^$/s/^# \{0,1\}//p' "$0"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) RELEASE=1; shift ;;
        --test)    TEST=1; shift ;;
        --bench)   BENCH=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *)         die "unknown option: $1 (see --help)" ;;
    esac
done

if [[ $RELEASE -eq 1 ]]; then
    BUILD_DIR="$REPO_ROOT/build-release"
    BUILD_TYPE=Release
else
    BUILD_DIR="$REPO_ROOT/build"
    BUILD_TYPE=Debug
fi

# --- configure ------------------------------------------------------------------------

on_off() { [[ $1 -eq 1 ]] && echo ON || echo OFF; }
CMAKE_ARGS=(
    -S "$REPO_ROOT" -B "$BUILD_DIR"
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DBUILD_TESTING=$(on_off "$TEST")"
    "-DBUILD_BENCHMARKS=$(on_off "$BENCH")"
)
# The generator can only be chosen when a build dir is first created
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]] && command -v ninja >/dev/null; then
    CMAKE_ARGS+=(-G Ninja)
fi

# On macOS, build for the machine's real CPU. Without this, an x86_64 cmake
# (e.g. Intel Homebrew in /usr/local/bin) running under Rosetta builds x86_64
# binaries on an Apple Silicon Mac. hw.optional.arm64 reports the hardware even
# from a Rosetta process, unlike `uname -m`. Linux compilers already target the host.
MAC_ARCH=""
if [[ "$(uname -s)" == "Darwin" ]]; then
    if [[ "$(sysctl -n hw.optional.arm64 2>/dev/null)" == "1" ]]; then
        MAC_ARCH=arm64
    else
        MAC_ARCH=x86_64
    fi
    CMAKE_ARGS+=("-DCMAKE_OSX_ARCHITECTURES=$MAC_ARCH")
fi

info "Configuring $BUILD_TYPE in ${BUILD_DIR#"$REPO_ROOT"/} (${MAC_ARCH:-native}, tests $(on_off "$TEST"), benchmarks $(on_off "$BENCH"))"
cmake "${CMAKE_ARGS[@]}" >/dev/null

# --- build ----------------------------------------------------------------------------

info "Building"
cmake --build "$BUILD_DIR" --parallel
info "Done: ${BUILD_DIR#"$REPO_ROOT"/}"
