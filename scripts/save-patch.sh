#!/usr/bin/env bash
#
# Save the SDK checkout's changes as patches/0001-stopwatch-customizations.patch.
#
# Usage: scripts/save-patch.sh
#
# - Marks every untracked file under esp32/components/muse (except tape/, which
#   build.sh copies in, and lock files), esp32/tests (host-test stand-ins such
#   as tests/host_include/) and esp32/partitions_stopwatch.csv with
#   `git add -N`, so the diff includes them, writes the diff, and unmarks them.
# - Checks the patch applies to a clean checkout of the pinned commit.
# - Scans it for keys (docs/token.md): the shapes of a Google or xAI key or an
#   SDK token, and the saved copies in ~/.config/stopwatch/ if there are any.
# - Replaces patches/0001-stopwatch-customizations.patch only if every check
#   passes.
#
# Overrides:
#   SDK_DIR     SDK checkout location (default: ~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk)
#   LOCK_WAIT   seconds to wait if a build or flash holds the checkout (default: 0)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH="$REPO_ROOT/patches/0001-stopwatch-customizations.patch"

log() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# shellcheck source=lib/common.sh
. "$SCRIPT_DIR/lib/common.sh"

[ -d "$SDK_DIR/.git" ] || die "SDK checkout not found at $SDK_DIR. Run scripts/build.sh first."
[ "$(git -C "$SDK_DIR" rev-parse HEAD)" = "$SDK_COMMIT" ] \
    || die "$SDK_DIR is not at the pinned commit ${SDK_COMMIT:0:12}; the patch must be made against it."

take_sdk_lock "scripts/save-patch.sh"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/save-patch.XXXXXX")"
cleanup() {
    rm -rf "$TMP"
    release_sdk_lock
}
trap cleanup EXIT

# --- Write the diff, new files included -----------------------------------------

new_files=()
while IFS= read -r f; do
    case "$f" in
        esp32/components/muse/tape/*|*.lock) continue ;;
    esac
    new_files+=("$f")
done < <(git -C "$SDK_DIR" ls-files --others --exclude-standard -- \
    esp32/components/muse esp32/tests esp32/partitions_stopwatch.csv)

if [ ${#new_files[@]} -gt 0 ]; then
    log "new files in the patch:"
    printf '    %s\n' "${new_files[@]}"
    git -C "$SDK_DIR" add -N -- "${new_files[@]}"
fi
rc=0
git -C "$SDK_DIR" diff >"$TMP/new.patch" || rc=$?
[ ${#new_files[@]} -gt 0 ] && git -C "$SDK_DIR" reset -q -- "${new_files[@]}"
[ "$rc" -eq 0 ] || die "git diff failed."
[ -s "$TMP/new.patch" ] || die "$SDK_DIR has no changes; refusing to write an empty patch."

# --- It applies to a clean checkout of the pinned commit ------------------------

# A throwaway index holding the pinned commit's tree stands in for the checkout.
GIT_INDEX_FILE="$TMP/clean.index" git -C "$SDK_DIR" read-tree "$SDK_COMMIT"
GIT_INDEX_FILE="$TMP/clean.index" git -C "$SDK_DIR" apply --cached --check "$TMP/new.patch" \
    || die "the patch doesn't apply to a clean checkout of ${SDK_COMMIT:0:12}."
log "applies cleanly to ${SDK_COMMIT:0:12}"

# --- No keys (docs/token.md) ------------------------------------------------------
#
# Counts only: printing a matching line would print the key.

leaks=0
n="$(grep -cE 'AIza[0-9A-Za-z_-]{30,}|mgst_[0-9A-Za-z]{8,}|xai-[0-9A-Za-z]{20,}' "$TMP/new.patch" || true)"
if [ "$n" -gt 0 ]; then
    warn "$n line(s) look like a Google key, an SDK token or an xAI key."
    leaks=1
fi
for k in "$HOME/.config/stopwatch/"*; do
    [ -s "$k" ] || continue
    if grep -qF -- "$(cat "$k")" "$TMP/new.patch"; then
        warn "the patch contains the key saved in $k."
        leaks=1
    fi
done
[ "$leaks" -eq 0 ] || die "not writing $PATCH. Remove the key from the SDK files and re-run (docs/token.md)."
log "no keys found"

# --- Save ---------------------------------------------------------------------------

if [ -f "$PATCH" ] && cmp -s "$TMP/new.patch" "$PATCH"; then
    log "unchanged: ${PATCH#$REPO_ROOT/}"
else
    cp "$TMP/new.patch" "$PATCH"
    log "wrote ${PATCH#$REPO_ROOT/} ($(grep -c '^diff --git' "$PATCH") files)"
fi
