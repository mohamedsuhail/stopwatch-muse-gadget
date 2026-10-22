#!/usr/bin/env bash
#
# Flash Muse Gadget firmware onto the M5Stack StopWatch.
#
# Usage: scripts/flash.sh [PORT]
#
# - Works from the repo root; locates the SDK checkout like scripts/build.sh.
# - Ensures ESP-IDF v6.0.1's idf.py is available (auto-sources export.sh).
# - On the FIRST flash, backs up the 16 MB factory firmware to
#   backups/stopwatch-factory-<date>.bin (plus a .sha256 checksum) and verifies
#   it, then flashes. Later runs find the existing backup and flash directly.
# - Flashes with: tools/muse/board.sh flash stopwatch [PORT]
#
# Overrides:
#   SDK_DIR     SDK checkout location (default: <repo>/.sdk/muse-gadget-sdk)
#   IDF_EXPORT  path to an ESP-IDF v6.0.1 export.sh, or the directory containing it
#   BACKUP_DIR  where factory backups live (default: <repo>/backups)
#   BACKUP_DATE stamp used in the backup filename (default: current date+time)

set -euo pipefail

BOARD="stopwatch"
IDF_VERSION="v6.0.1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK_DIR="${SDK_DIR:-$REPO_ROOT/.sdk/muse-gadget-sdk}"
BACKUP_DIR="${BACKUP_DIR:-$REPO_ROOT/backups}"

log() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || die "git is required but not found."

# --- ESP-IDF v6.0.1 (same lookup as build.sh) ----------------------------------

version_matches() {
    local re='v6\.0\.1([^0-9]|$)'
    [[ "$1" =~ $re ]]
}

current_idf_version() {
    command -v idf.py >/dev/null 2>&1 || return 1
    idf.py --version 2>/dev/null || true
}

source_export() {
    set +eu
    # shellcheck disable=SC1090
    . "$1" >/dev/null 2>&1
    local rc=$?
    set -eu
    return "$rc"
}

ensure_idf() {
    local v=""
    v="$(current_idf_version || true)"
    if [ -n "$v" ] && version_matches "$v"; then
        log "using idf.py already on PATH: $v"
        return 0
    fi
    local candidates=()
    if [ -n "${IDF_EXPORT:-}" ]; then
        if [ -d "$IDF_EXPORT" ]; then
            candidates+=("$IDF_EXPORT/export.sh")
        else
            candidates+=("$IDF_EXPORT")
        fi
    fi
    candidates+=(
        "$HOME/.espressif/esp-idf-v6.0.1/export.sh"
        "$HOME/esp/esp-idf-v6.0.1/export.sh"
        "$HOME/esp/esp-idf-v6/export.sh"
        "$HOME/esp/esp-idf/export.sh"
    )
    local f
    for f in "${candidates[@]}"; do
        [ -f "$f" ] || continue
        log "sourcing $f"
        if ! source_export "$f"; then
            warn "sourcing $f failed; trying next location"
            continue
        fi
        v="$(current_idf_version || true)"
        if [ -n "$v" ] && version_matches "$v"; then
            log "using $v"
            return 0
        fi
        warn "$f provides '${v:-no idf.py}', not $IDF_VERSION; trying next location"
    done
    if [ -n "$v" ]; then
        die "found idf.py ($v) but not ESP-IDF $IDF_VERSION. Run scripts/install-idf.sh, or set IDF_EXPORT to a v6.0.1 export.sh."
    fi
    die "idf.py not found. Run scripts/install-idf.sh, or set IDF_EXPORT to a v6.0.1 export.sh."
}

# --- esptool -------------------------------------------------------------------

ensure_esptool() {
    if command -v esptool.py >/dev/null 2>&1; then
        ESPTOOL=( esptool.py )
    elif python3 -m esptool --version >/dev/null 2>&1; then
        ESPTOOL=( python3 -m esptool )
    else
        die "esptool not found. Install it with: pip install esptool"
    fi
}

# --- Serial port ---------------------------------------------------------------

resolve_port() {
    if [ $# -gt 0 ] && [ -n "${1:-}" ]; then
        echo "$1"
        return 0
    fi
    case "$(uname -s)" in
        Darwin)
            # First USB serial device, preferring cu.* (call-in) entries.
            port="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -n 1 || true)"
            ;;
        Linux)
            port="$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n 1 || true)"
            ;;
        *)
            port=""
            ;;
    esac
    if [ -z "$port" ]; then
        die "no serial port given and none auto-detected. Plug in the StopWatch and pass the port, e.g.: scripts/flash.sh /dev/cu.usbmodem101"
    fi
    echo "$port"
}

# --- Factory backup ------------------------------------------------------------

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1"
    else
        sha256sum "$1"
    fi
}

backup_factory() {
    local port="$1"
    local date_stamp="${BACKUP_DATE:-$(date +%Y%m%d-%H%M%S)}"
    local backup_path="$BACKUP_DIR/stopwatch-factory-$date_stamp.bin"

    existing_backup=""
    if [ -d "$BACKUP_DIR" ]; then
        for f in "$BACKUP_DIR"/stopwatch-factory-*.bin; do
            [ -f "$f" ] || continue
            existing_backup="$f"
            break
        done
    fi
    if [ -n "$existing_backup" ]; then
        log "factory backup already exists: $existing_backup (skipping)"
        return 0
    fi

    mkdir -p "$BACKUP_DIR"
    log "backing up factory firmware (16 MB) to $backup_path"
    "${ESPTOOL[@]}" --chip esp32s3 -p "$port" read-flash 0 0x1000000 "$backup_path"
    sha256_file "$backup_path" >"$backup_path.sha256"
    log "verifying backup checksum"
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -c "$backup_path.sha256"
    else
        sha256sum -c "$backup_path.sha256"
    fi
    log "backup OK: $backup_path"
    log "checksum: $backup_path.sha256"
    log "copy this backup somewhere safe OUTSIDE this repo before flashing."
}

# --- Main -----------------------------------------------------------------------

PORT="$(resolve_port "$@")"
log "using serial port: $PORT"

ensure_idf
ensure_esptool

[ -d "$SDK_DIR/.git" ] || die "SDK checkout not found at $SDK_DIR. Run scripts/build.sh first."
[ -f "$SDK_DIR/esp32/tools/muse/board.sh" ] || die "expected $SDK_DIR/esp32/tools/muse/board.sh but it is missing."

backup_factory "$PORT"

# The character's clips go in their own partition (partitions_stopwatch.csv,
# "anims" at 0x830000), written only when what's there differs. They go first,
# leaving the chip in its bootloader (--after no-reset): the firmware flash
# below then resets it once. Reset again by a later esptool run, a firmware
# that had just started booting could be cut off mid token refresh, after
# Muse's server had replaced the token but before the watch saved the new one,
# and the next boot is rejected and unpairs.
ANIMS="$REPO_ROOT/avatar/character/anims.pack"
ANIMS_OFFSET=0x830000
if [ -f "$ANIMS" ]; then
    if "${ESPTOOL[@]}" --chip esp32s3 -p "$PORT" --after no-reset verify-flash "$ANIMS_OFFSET" "$ANIMS" >/dev/null 2>&1; then
        log "animations already up to date"
    else
        log "writing animations ($(( $(wc -c < "$ANIMS") / 1024 )) KB) to $ANIMS_OFFSET"
        "${ESPTOOL[@]}" --chip esp32s3 -p "$PORT" -b 460800 --after no-reset write-flash "$ANIMS_OFFSET" "$ANIMS"
    fi
fi

cd "$SDK_DIR/esp32"
log "flashing: tools/muse/board.sh flash $BOARD $PORT"
if [ -x tools/muse/board.sh ]; then
    tools/muse/board.sh flash "$BOARD" "$PORT"
else
    bash tools/muse/board.sh flash "$BOARD" "$PORT"
fi

log "flash succeeded"
echo "If this was the first flash, the factory backup is in $BACKUP_DIR."
echo "Next: add the SDK token (docs/token.md), then pair from the Muse app."
