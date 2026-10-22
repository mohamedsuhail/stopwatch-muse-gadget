#!/usr/bin/env bash
#
# Build Muse Gadget firmware for the M5Stack StopWatch.
#
# - Clones the Muse Gadget SDK at the pinned commit into .sdk/muse-gadget-sdk
#   (override with SDK_DIR). If the checkout exists, fetches and checks out the
#   pinned commit.
# - Makes ESP-IDF v6.0.1's idf.py available (sources export.sh from known places).
# - Runs `tools/muse/board.sh build stopwatch` from the SDK's esp32/ directory.
# - Does NOT need the SDK token (the build warns about it; that's expected).
#
# Overrides:
#   SDK_DIR     SDK checkout location (default: <repo>/.sdk/muse-gadget-sdk)
#   IDF_EXPORT  path to an ESP-IDF v6.0.1 export.sh, or the directory containing it

set -euo pipefail

SDK_REPO_URL="https://github.com/facebookincubator/muse-gadget-sdk.git"
SDK_COMMIT="fa073045a5572a8e3367fae142c1b7a7c554f222"
IDF_VERSION="v6.0.1"
BOARD="stopwatch"
BUILD_DIR_NAME="build-muse-m5stack-stopwatch"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK_DIR="${SDK_DIR:-$REPO_ROOT/.sdk/muse-gadget-sdk}"

log() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || die "git is required but not found."

# --- SDK checkout at the pinned commit ----------------------------------------

sync_sdk() {
    if [ -d "$SDK_DIR/.git" ]; then
        local head
        head="$(git -C "$SDK_DIR" rev-parse HEAD)"
        if [ "$head" = "$SDK_COMMIT" ]; then
            log "SDK already at pinned commit ${SDK_COMMIT:0:12} ($SDK_DIR)"
        else
            if ! git -C "$SDK_DIR" diff --quiet || ! git -C "$SDK_DIR" diff --cached --quiet; then
                die "$SDK_DIR has uncommitted changes; refusing to switch commits. Stash/discard them, or delete $SDK_DIR and re-run."
            fi
            log "fetching SDK updates"
            git -C "$SDK_DIR" fetch --tags origin
            if ! git -C "$SDK_DIR" cat-file -e "${SDK_COMMIT}^{commit}" 2>/dev/null; then
                git -C "$SDK_DIR" fetch origin "$SDK_COMMIT"
            fi
            log "checking out pinned commit ${SDK_COMMIT:0:12}"
            git -C "$SDK_DIR" checkout --detach "$SDK_COMMIT"
        fi
    elif [ -e "$SDK_DIR" ]; then
        die "$SDK_DIR exists but is not a git checkout. Remove it or set SDK_DIR."
    else
        log "cloning Muse Gadget SDK into $SDK_DIR"
        mkdir -p "$(dirname "$SDK_DIR")"
        git clone "$SDK_REPO_URL" "$SDK_DIR"
        git -C "$SDK_DIR" checkout --detach "$SDK_COMMIT"
    fi
    git -C "$SDK_DIR" submodule update --init --recursive
    [ -f "$SDK_DIR/esp32/tools/muse/board.sh" ] \
        || die "expected $SDK_DIR/esp32/tools/muse/board.sh but it is missing."
}

# --- Local patches on top of the pinned SDK -----------------------------------

apply_patches() {
    local p
    for p in "$REPO_ROOT"/patches/*.patch; do
        [ -f "$p" ] || continue
        if git -C "$SDK_DIR" apply --reverse --check "$p" 2>/dev/null; then
            log "patch already applied: $(basename "$p")"
        elif git -C "$SDK_DIR" apply --check "$p" 2>/dev/null; then
            log "applying patch: $(basename "$p")"
            git -C "$SDK_DIR" apply "$p"
        else
            die "patch $(basename "$p") doesn't apply to $SDK_DIR. Discard local SDK changes (git -C $SDK_DIR checkout . && git -C $SDK_DIR clean -fd) and re-run."
        fi
    done
    # Our avatar (the SDK keeps custom avatars in a gitignored directory): the
    # character's video clips by default (avatar/character, written to the
    # "anims" partition by flash.sh), or AVATAR=pixel for the pixel astronaut.
    local avatar="$REPO_ROOT/avatar/character/muse_pixel.c"
    if [ "${AVATAR:-clips}" = "pixel" ] || [ ! -f "$avatar" ]; then
        avatar="$REPO_ROOT/avatar/muse_pixel.c"
    fi
    if [ -f "$avatar" ]; then
        mkdir -p "$SDK_DIR/esp32/components/muse/avatar"
        cp "$avatar" "$SDK_DIR/esp32/components/muse/avatar/muse_pixel.c"
        log "using ${avatar#$REPO_ROOT/}"
    fi
    # The MOBOT tape label (avatar/make_tape.py makes it).
    if [ -f "$REPO_ROOT/avatar/tape.rgb565a8" ]; then
        mkdir -p "$SDK_DIR/esp32/components/muse/tape"
        cp "$REPO_ROOT/avatar/tape.rgb565a8" "$SDK_DIR/esp32/components/muse/tape/tape.rgb565a8"
        log "using avatar/tape.rgb565a8"
    fi
}

# --- ESP-IDF v6.0.1 -----------------------------------------------------------

version_matches() {
    local re='v6\.0\.1([^0-9]|$)'
    [[ "$1" =~ $re ]]
}

current_idf_version() {
    command -v idf.py >/dev/null 2>&1 || return 1
    idf.py --version 2>/dev/null || true
}

source_export() {
    # export.sh isn't written for `set -eu`; relax it while sourcing.
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

run_board() {
    if [ -x tools/muse/board.sh ]; then
        tools/muse/board.sh "$@"
    else
        bash tools/muse/board.sh "$@"
    fi
}

# --- Main -----------------------------------------------------------------------

sync_sdk
apply_patches
ensure_idf

cd "$SDK_DIR/esp32"
log "building: tools/muse/board.sh build $BOARD"
run_board build "$BOARD"

BUILD_DIR="$SDK_DIR/esp32/$BUILD_DIR_NAME"
[ -d "$BUILD_DIR" ] || die "build finished but $BUILD_DIR does not exist."

app_bin=""
if [ -f "$BUILD_DIR/project_description.json" ]; then
    app_bin="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("app_bin",""))' \
        "$BUILD_DIR/project_description.json" 2>/dev/null || true)"
    if [ -n "$app_bin" ] && [ "${app_bin#/}" = "$app_bin" ]; then
        app_bin="$BUILD_DIR/$app_bin"
    fi
    if [ -z "$app_bin" ] || [ ! -f "$app_bin" ]; then
        app_bin=""
        for f in "$BUILD_DIR"/*.bin; do
            [ -f "$f" ] && { app_bin="$f"; break; }
        done
    fi
fi

echo
log "build succeeded"
echo "Build dir: $BUILD_DIR"
if [ -n "$app_bin" ]; then
    echo "Firmware: $app_bin"
else
    warn "could not identify the app .bin; look in $BUILD_DIR"
fi
[ -f "$BUILD_DIR/flash_args" ] && echo "Flash args: $BUILD_DIR/flash_args"
echo "A missing SDK token warning is expected. See docs/token.md."
echo "Next: scripts/flash.sh [PORT]   (factory backup happens automatically on first flash)"
