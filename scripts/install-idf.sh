#!/usr/bin/env bash
#
# Install ESP-IDF v6.0.1 (esp32s3 support) into ~/esp/esp-idf-v6.0.1.
#
# - Idempotent: safe to re-run; an existing correct checkout is reused.
# - Isolated: lives in its own directory and ignores any IDF_PATH from another
#   ESP-IDF install in your environment.
# - Verifies that `idf.py --version` reports v6.0.1.
#
# Overrides:
#   IDF_DIR         install location (default: ~/esp/esp-idf-v6.0.1)
#   IDF_TOOLS_PATH  standard ESP-IDF tools location (default: ~/.espressif);
#                   tools and the Python env there are versioned per IDF release,
#                   so they coexist with other installs.

set -euo pipefail

IDF_VERSION="v6.0.1"
IDF_REPO_URL="https://github.com/espressif/esp-idf.git"
IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v6.0.1}"
IDF_TARGETS="esp32s3"

log() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

version_matches() {
    # True if $1 contains v6.0.1 not followed by another digit (so v6.0.10 doesn't match).
    local re='v6\.0\.1([^0-9]|$)'
    [[ "$1" =~ $re ]]
}

# --- Prerequisites -----------------------------------------------------------

command -v git >/dev/null 2>&1 || die "git is required but not found."
command -v python3 >/dev/null 2>&1 || die "python3 is required but not found."

missing=""
for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    warn "not found on PATH:$missing"
    case "$(uname -s)" in
        Darwin) warn "install with: brew install cmake ninja dfu-util" ;;
        Linux) warn "install with (Debian/Ubuntu): sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0" ;;
    esac
    warn "continuing; ESP-IDF's installer may still provide what it needs."
fi

# --- Clone (or reuse) ESP-IDF ------------------------------------------------

if [ -d "$IDF_DIR/.git" ]; then
    current="$(git -C "$IDF_DIR" describe --tags --exact-match 2>/dev/null || true)"
    if [ "$current" != "$IDF_VERSION" ]; then
        die "$IDF_DIR exists but is at '${current:-an untagged commit}', not $IDF_VERSION. Move it aside, or set IDF_DIR to a different location, and re-run."
    fi
    log "ESP-IDF $IDF_VERSION already present at $IDF_DIR"
elif [ -e "$IDF_DIR" ]; then
    die "$IDF_DIR exists but is not a git checkout. Move it aside and re-run."
else
    partial="${IDF_DIR}.partial"
    if [ -e "$partial" ]; then
        log "removing leftover partial clone at $partial"
        rm -rf "$partial"
    fi
    mkdir -p "$(dirname "$IDF_DIR")"
    log "cloning ESP-IDF $IDF_VERSION into $IDF_DIR (this takes a while)"
    git clone -b "$IDF_VERSION" --recursive "$IDF_REPO_URL" "$partial"
    mv "$partial" "$IDF_DIR"
fi

log "syncing submodules"
git -C "$IDF_DIR" submodule update --init --recursive

# --- Install tools for esp32s3 -----------------------------------------------

log "installing ESP-IDF tools for: $IDF_TARGETS"
(
    # Don't let another ESP-IDF install's environment leak into this one.
    unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION
    "$IDF_DIR/install.sh" "$IDF_TARGETS"
)

# --- Verify -------------------------------------------------------------------

log "verifying idf.py version"
idf_version_output="$(
    unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION
    set +eu
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null 2>&1 || exit 1
    idf.py --version
)" || die "sourcing $IDF_DIR/export.sh or running idf.py failed. Try: . \"$IDF_DIR/export.sh\" to see the error."
if ! version_matches "$idf_version_output"; then
    die "expected $IDF_VERSION but idf.py reported: $idf_version_output"
fi
log "OK: $idf_version_output"

# --- Next steps ---------------------------------------------------------------

cat <<EOF
ESP-IDF $IDF_VERSION is installed at: $IDF_DIR
To use idf.py by hand, run this in each new shell:
    . "$IDF_DIR/export.sh"
Optional shell alias (add to ~/.zshrc or ~/.bashrc):
    alias get_idf601='. "$IDF_DIR/export.sh"'
scripts/build.sh and scripts/flash.sh find and source it automatically.
EOF
