#!/bin/sh
# Fail-open early boot activation for the physically validated sbubifs layout.
# This script never creates, formats, resizes, renames, or removes a UBI volume.

ROOT=${SB_STORAGE_ROOT:-}
test -n "$ROOT" || PATH=/sbin:/bin:/usr/sbin:/usr/bin
MODULE=$ROOT/mnt/storage/sbubifs-authorized.ko
HELPER=$ROOT/usr/share/jive/applets/StandaloneBase/sb-storage-helper
VOLUME=$ROOT/sys/class/ubi/ubi0_5
MOUNT=$ROOT/mnt/sbdata
SOURCE=$MOUNT/applets
TARGET=$ROOT/usr/share/jive/applets
MOUNTS=$ROOT/proc/mounts
FILESYSTEMS=$ROOT/proc/filesystems
REBOOT_MARKER=$ROOT/mnt/storage/standalonebase/extended-storage-reboot-required

log() {
    echo "StandaloneBase Extended Storage: $*" >&2
    if test -z "$ROOT" && command -v logger >/dev/null 2>&1; then
        logger -t standalonebase-storage "$*" || true
    fi
}

mounted_at() {
    awk -v target="$1" '$2 == target { found=1 } END { exit !found }' "$MOUNTS" 2>/dev/null
}

cleanup() {
    test "${BIND_CREATED:-0}" = 1 && umount "$TARGET" 2>/dev/null || true
    test "${MOUNT_CREATED:-0}" = 1 && umount "$MOUNT" 2>/dev/null || true
    test "${MODULE_LOADED:-0}" = 1 && rmmod sbubifs 2>/dev/null || true
}

fail_open() {
    log "activation skipped: $*"
    cleanup
    exit 0
}

test -x "$HELPER" || fail_open "module verifier is missing"
test -f "$MODULE" || fail_open "authorized module is missing"
"$HELPER" verify-module "$MODULE" >/dev/null 2>&1 || fail_open "authorized module identity mismatch"
test -r "$VOLUME/name" || fail_open "UBI0 volume ID 5 is absent"
test "$(cat "$VOLUME/name" 2>/dev/null)" = sbdata || fail_open "UBI0 volume ID 5 is not named sbdata"
test "$(cat "$VOLUME/type" 2>/dev/null)" = dynamic || fail_open "sbdata is not dynamic"
test "$(cat "$VOLUME/corrupted" 2>/dev/null)" = 0 || fail_open "sbdata is corrupt or unreadable"
test "$(cat "$VOLUME/upd_marker" 2>/dev/null)" = 0 || fail_open "sbdata update marker is set"

if ! grep -q '^[[:space:]]*nodev[[:space:]]*sbubifs$' "$FILESYSTEMS" 2>/dev/null; then
    insmod "$MODULE" >/dev/null 2>&1 || fail_open "sbubifs module load failed"
    MODULE_LOADED=1
fi
grep -q '^[[:space:]]*nodev[[:space:]]*sbubifs$' "$FILESYSTEMS" 2>/dev/null || fail_open "sbubifs did not register"

if ! mounted_at "$MOUNT"; then
    mkdir -p "$MOUNT" || fail_open "cannot create mount point"
    mount -t sbubifs ubi0:sbdata "$MOUNT" >/dev/null 2>&1 || fail_open "sbdata mount failed"
    MOUNT_CREATED=1
fi
awk -v target="$MOUNT" '$2 == target && $3 == "sbubifs" { ok=1 } END { exit !ok }' "$MOUNTS" 2>/dev/null || fail_open "sbdata mount verification failed"

test -d "$SOURCE" || fail_open "migrated applet tree is absent"
test -d "$SOURCE/AboutJive" -o -d "$SOURCE/SetupAppletInstaller" || fail_open "core applet sentinel is absent"
test -d "$SOURCE/StandaloneBase" || fail_open "StandaloneBase sentinel is absent"
test -d "$TARGET" || fail_open "stock applet tree is absent"

if ! mounted_at "$TARGET"; then
    mount -o bind "$SOURCE" "$TARGET" >/dev/null 2>&1 || fail_open "applet bind mount failed"
    BIND_CREATED=1
fi
awk -v target="$TARGET" '$2 == target && $3 == "sbubifs" { ok=1 } END { exit !ok }' "$MOUNTS" 2>/dev/null || fail_open "applet bind verification failed"
test -d "$TARGET/StandaloneBase" || fail_open "post-bind StandaloneBase check failed"

rm -f "$REBOOT_MARKER" 2>/dev/null || true
log "activation complete"
exit 0
