#!/usr/bin/env bash
#
# Format every .cpp and .hpp file in the project with clang-format.
#
# Style is configured in <repo>/.clang-format.
#
# Usage:
#   scripts/format.sh [options] [files...]
#
# Options:
#   --check      Don't modify files; list the ones that need formatting and
#                exit 1 if any do (for CI / pre-commit).
#   -h, --help   Show this help.
#
# With no files given, formats all tracked and untracked (not ignored) .cpp/.hpp
# files under src/, apps/, tests/ and bench/.
#
# clang-format is found via $CLANG_FORMAT, then PATH, then Homebrew's llvm.
# Install on macOS:  brew install clang-format
# Install on Linux:  apt install clang-format

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_DIRS=(src apps tests bench)

CHECK=0
FILES=()

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*" >&2; }
usage() { sed -n '3,/^$/s/^# \{0,1\}//p' "$0"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)   CHECK=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)        die "unknown option: $1 (see --help)" ;;
        *)         FILES+=("$1"); shift ;;
    esac
done

# --- find clang-format ----------------------------------------------------------

find_clang_format() {
    if [[ -n "${CLANG_FORMAT:-}" ]]; then echo "$CLANG_FORMAT"; return; fi
    if command -v clang-format >/dev/null; then command -v clang-format; return; fi
    local p
    for p in /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format; do
        if [[ -x "$p" ]]; then echo "$p"; return; fi
    done
}

FMT="$(find_clang_format)"
[[ -n "$FMT" ]] || die "clang-format not found. Install it (macOS: brew install clang-format; Linux: apt install clang-format) or set CLANG_FORMAT=/path/to/clang-format"

# --- file list ------------------------------------------------------------------

if [[ ${#FILES[@]} -eq 0 ]]; then
    cd "$REPO_ROOT"
    while IFS= read -r f; do
        FILES+=("$REPO_ROOT/$f")
    done < <(git ls-files --cached --others --exclude-standard -- \
                 "${SOURCE_DIRS[@]/%//*.cpp}" "${SOURCE_DIRS[@]/%//*.hpp}" | sort -u)
fi
[[ ${#FILES[@]} -gt 0 ]] || { info "No .cpp/.hpp files to format"; exit 0; }

# --- run ------------------------------------------------------------------------

if [[ $CHECK -eq 1 ]]; then
    unformatted=()
    for f in "${FILES[@]}"; do
        if ! "$FMT" --style=file --dry-run --Werror "$f" >/dev/null 2>&1; then
            unformatted+=("$f")
        fi
    done
    if [[ ${#unformatted[@]} -gt 0 ]]; then
        info "${#unformatted[@]} of ${#FILES[@]} files need formatting:"
        printf '  %s\n' "${unformatted[@]#"$REPO_ROOT"/}" >&2
        die "run scripts/format.sh to fix"
    fi
    info "$(basename "$FMT"): ${#FILES[@]} files already formatted"
else
    "$FMT" --style=file -i "${FILES[@]}"
    info "$(basename "$FMT"): formatted ${#FILES[@]} files"
fi
