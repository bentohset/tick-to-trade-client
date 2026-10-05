#!/usr/bin/env bash
#
# Fetch Nasdaq TotalView-ITCH 5.0 sample data from Nasdaq's public EMI server.
#
# Files are full trading days: 3.5–5.6 GB gzip'd, roughly 3x that uncompressed.
# The format is a stream of [2-byte big-endian length][ITCH message] records
# (see docs/protocols/itch.md).
#
# Usage:
#   scripts/fetch_itch_sample.sh [options]
#
# Options:
#   --list              List available full-day ITCH 5.0 files and exit.
#   --file NAME         File to fetch (default: 12302019.NASDAQ_ITCH50.gz, the smallest).
#   --dest DIR          Download directory (default: <repo>/data/itch).
#   --decompress        Also decompress the download (keeps the .gz).
#   --sample N          Write only the first N messages to a small uncompressed file,
#                       streaming from the local .gz if present, otherwise straight
#                       from the server (no full download). Good for tests/data.
#                       Note: the first ~240k messages are pre-market reference data
#                       (R/H/Y/L); order flow (A/E/X/D/U) starts after that, so use
#                       N >= 300000 for a sample that exercises the book
#                       (500000 messages is ~14 MB).
#   --sample-out PATH   Output path for --sample
#                       (default: <repo>/tests/data/<date>_first<N>.itch).
#   -h, --help          Show this help.
#
# Examples:
#   scripts/fetch_itch_sample.sh --list
#   scripts/fetch_itch_sample.sh                      # full download of default day
#   scripts/fetch_itch_sample.sh --decompress
#   scripts/fetch_itch_sample.sh --sample 500000      # first 500k messages into tests/data/
#
# Downloads resume if interrupted. Re-running is safe: a complete file is skipped.
# Note: the server's .md5sum links return an HTML page rather than a checksum, so
# integrity is checked against the server's Content-Length.

set -euo pipefail

BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FILE="12302019.NASDAQ_ITCH50.gz"
DEST="$REPO_ROOT/data/itch"
DECOMPRESS=0
LIST=0
SAMPLE_N=""
SAMPLE_OUT=""

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*" >&2; }

usage() { sed -n '2,/^$/s/^# \{0,1\}//p' "$0"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --list)        LIST=1; shift ;;
        --file)        [[ $# -ge 2 ]] || die "--file needs a value"; FILE="$2"; shift 2 ;;
        --dest)        [[ $# -ge 2 ]] || die "--dest needs a value"; DEST="$2"; shift 2 ;;
        --decompress)  DECOMPRESS=1; shift ;;
        --sample)      [[ $# -ge 2 ]] || die "--sample needs a value"; SAMPLE_N="$2"; shift 2 ;;
        --sample-out)  [[ $# -ge 2 ]] || die "--sample-out needs a value"; SAMPLE_OUT="$2"; shift 2 ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "unknown option: $1 (see --help)" ;;
    esac
done

command -v curl >/dev/null || die "curl is required"

# --- list ---------------------------------------------------------------------

if [[ $LIST -eq 1 ]]; then
    info "Available ITCH 5.0 day files at $BASE_URL/"
    curl -fsSL "$BASE_URL/" \
        | grep -oE '[0-9]+ <A HREF="[^"]+">[0-9]{8}\.NASDAQ_ITCH50\.gz</A>' \
        | sed -E 's/^([0-9]+) <A HREF="[^"]+">([^<]+)<\/A>$/\2 \1/' \
        | sort -k2 -n \
        | awk '{ printf "  %-28s %6.2f GB\n", $1, $2 / 1e9 }'
    exit 0
fi

[[ "$FILE" == *.gz ]] || die "--file should name a .gz file (see --list)"
URL="$BASE_URL/$FILE"
LOCAL_GZ="$DEST/$FILE"

# --- sample -------------------------------------------------------------------

# Copies the first N length-prefixed messages from stdin to stdout, stopping on
# a message boundary. Exits 0 if N messages were written, 3 if input ran out.
take_messages() {
    python3 -c '
import sys
n = int(sys.argv[1])
src, dst = sys.stdin.buffer, sys.stdout.buffer
written = 0
while written < n:
    hdr = src.read(2)
    if len(hdr) < 2:
        break
    length = int.from_bytes(hdr, "big")
    body = src.read(length)
    if len(body) < length:
        break
    dst.write(hdr + body)
    written += 1
dst.flush()
print(f"wrote {written} messages", file=sys.stderr)
sys.exit(0 if written == n else 3)
' "$1"
}

if [[ -n "$SAMPLE_N" ]]; then
    [[ "$SAMPLE_N" =~ ^[1-9][0-9]*$ ]] || die "--sample needs a positive integer"
    command -v python3 >/dev/null || die "python3 is required for --sample"

    date_part="${FILE%%.*}"
    [[ -n "$SAMPLE_OUT" ]] || SAMPLE_OUT="$REPO_ROOT/tests/data/${date_part}_first${SAMPLE_N}.itch"
    mkdir -p "$(dirname "$SAMPLE_OUT")"
    tmp_out="$SAMPLE_OUT.part"

    # Upstream curl/gzip get SIGPIPE once python has enough messages; that's
    # expected, so only python's exit status decides success.
    set +e +o pipefail
    if [[ -f "$LOCAL_GZ" ]]; then
        info "Sampling first $SAMPLE_N messages from local $LOCAL_GZ"
        gzip -dc "$LOCAL_GZ" 2>/dev/null | take_messages "$SAMPLE_N" > "$tmp_out"
        status=${PIPESTATUS[1]}
    else
        info "Sampling first $SAMPLE_N messages by streaming $URL"
        curl -fsSL "$URL" 2>/dev/null | gzip -dc 2>/dev/null | take_messages "$SAMPLE_N" > "$tmp_out"
        status=${PIPESTATUS[2]}
    fi
    set -e -o pipefail

    case "$status" in
        0) mv "$tmp_out" "$SAMPLE_OUT" ;;
        3) [[ -s "$tmp_out" ]] || { rm -f "$tmp_out"; die "no messages read (bad file name? see --list)"; }
           mv "$tmp_out" "$SAMPLE_OUT"; info "warning: input ended before $SAMPLE_N messages" ;;
        *) rm -f "$tmp_out"; die "sampling failed (status $status)" ;;
    esac
    info "Sample written: $SAMPLE_OUT ($(wc -c < "$SAMPLE_OUT" | tr -d ' ') bytes)"
    exit 0
fi

# --- full download ------------------------------------------------------------

mkdir -p "$DEST"

headers="$(curl -fsSLI "$URL" 2>/dev/null)" \
    || die "could not reach $URL (does it exist? try --list)"
remote_size="$(printf '%s\n' "$headers" | tr -d '\r' \
    | awk -F': ' 'tolower($1) == "content-length" { len = $2 } END { print len }')"
[[ "$remote_size" =~ ^[0-9]+$ ]] || die "could not get size of $URL (does it exist? try --list)"

local_size=0
[[ -f "$LOCAL_GZ" ]] && local_size="$(wc -c < "$LOCAL_GZ" | tr -d ' ')"

if [[ "$local_size" -eq "$remote_size" ]]; then
    info "Already downloaded: $LOCAL_GZ"
else
    if [[ "$local_size" -gt "$remote_size" ]]; then
        info "Local file is larger than remote; restarting download"
        rm -f "$LOCAL_GZ"
    elif [[ "$local_size" -gt 0 ]]; then
        info "Resuming download at $local_size / $remote_size bytes"
    fi
    info "Downloading $FILE ($(awk -v s="$remote_size" 'BEGIN { printf "%.2f GB", s / 1e9 }')) to $DEST"
    curl -fL --retry 5 --retry-delay 5 -C - --progress-bar -o "$LOCAL_GZ" "$URL"

    local_size="$(wc -c < "$LOCAL_GZ" | tr -d ' ')"
    [[ "$local_size" -eq "$remote_size" ]] \
        || die "size mismatch after download: got $local_size, expected $remote_size (re-run to resume)"
    info "Download complete: $LOCAL_GZ"
fi

# --- decompress ---------------------------------------------------------------

if [[ $DECOMPRESS -eq 1 ]]; then
    out="${LOCAL_GZ%.gz}"
    if [[ -f "$out" ]]; then
        info "Already decompressed: $out"
    else
        if command -v pigz >/dev/null; then unzip_cmd=(pigz -dc); else unzip_cmd=(gzip -dc); fi
        info "Decompressing with ${unzip_cmd[0]} to $out (expect ~3x the .gz size)"
        "${unzip_cmd[@]}" "$LOCAL_GZ" > "$out.part"
        mv "$out.part" "$out"
        info "Decompressed: $out ($(wc -c < "$out" | tr -d ' ') bytes)"
    fi
fi
