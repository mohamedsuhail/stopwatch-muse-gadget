# Shared by scripts/build.sh, scripts/flash.sh and scripts/save-patch.sh.
# Sourced, not run. Expects REPO_ROOT and die() to be defined first.

SDK_REPO_URL="https://github.com/facebookincubator/muse-gadget-sdk.git"
SDK_COMMIT="fa073045a5572a8e3367fae142c1b7a7c554f222"

# The SDK checkout and its build folder live outside ~/Documents, which iCloud
# syncs: iCloud offloaded SDK files mid-build and left conflict copies of
# dependencies.lock. SDK_DIR=<repo>/.sdk/muse-gadget-sdk still works.
SDK_DIR="${SDK_DIR:-$HOME/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk}"

# --- One build or flash at a time per SDK checkout ------------------------------
#
# board.sh deletes managed_components/ and dependencies.lock at the start and end
# of every build, so two builds in one checkout break each other, and a flash
# during a build writes a half-built image. mkdir is atomic, and macOS has no
# flock. The lock holds a note of who took it; a lock whose process is gone is
# taken over.
#
#   LOCK_WAIT  seconds to wait for a held lock before giving up (default: 0)

SDK_LOCK="$SDK_DIR.lock"

lock_holder() {
    cat "$SDK_LOCK/owner" 2>/dev/null || echo "unknown (no owner file)"
}

release_sdk_lock() {
    rm -rf "$SDK_LOCK"
}

take_sdk_lock() {
    local what="$1" waited=0 wait="${LOCK_WAIT:-0}" pid
    mkdir -p "$(dirname "$SDK_LOCK")"
    while ! mkdir "$SDK_LOCK" 2>/dev/null; do
        pid="$(sed -n 's/^pid=//p' "$SDK_LOCK/owner" 2>/dev/null || true)"
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            printf 'warning: taking over a stale lock (process %s is gone):\n%s\n' "$pid" "$(lock_holder)" >&2
            rm -rf "$SDK_LOCK"
            continue
        fi
        if [ "$waited" -ge "$wait" ]; then
            die "$SDK_DIR is in use:
$(lock_holder)
Wait for it to finish, or set LOCK_WAIT=<seconds> to wait. If no such process is running, remove $SDK_LOCK."
        fi
        [ "$waited" -eq 0 ] && printf '==> waiting up to %ss for the lock held by:\n%s\n' "$wait" "$(lock_holder)"
        sleep 5
        waited=$((waited + 5))
    done
    printf 'pid=%s\nwhat=%s\nrepo=%s\nstarted=%s\n' "$$" "$what" "$REPO_ROOT" "$(date '+%Y-%m-%d %H:%M:%S')" \
        >"$SDK_LOCK/owner"
    trap release_sdk_lock EXIT
}
