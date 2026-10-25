#!/bin/sh
# Fail-open early boot activation of the existing, validated sbdata volume.
# This script never creates, formats, resizes, renames, or removes a UBI volume.

ROOT=${SB_STORAGE_ROOT:-}
test -n "$ROOT" || PATH=/sbin:/bin:/usr/sbin:/usr/bin
MODULE=$ROOT/etc/standalonebase/sbubifs-authorized.ko
HELPER=$ROOT/etc/standalonebase/sb-storage-helper
test -f "$MODULE" || MODULE=$ROOT/mnt/storage/sbubifs-authorized.ko
test -x "$HELPER" || HELPER=$ROOT/usr/share/jive/applets/StandaloneBase/sb-storage-helper
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

log "activation started: checking existing volume and boot prerequisites"
test -r "$VOLUME/name" || fail_open "UBI0 volume ID 5 is absent"
test "$(cat "$VOLUME/name" 2>/dev/null)" = sbdata || fail_open "UBI0 volume ID 5 is not named sbdata"
test "$(cat "$VOLUME/type" 2>/dev/null)" = dynamic || fail_open "sbdata is not dynamic"
test "$(cat "$VOLUME/corrupted" 2>/dev/null)" = 0 || fail_open "sbdata is corrupt or unreadable"
test "$(cat "$VOLUME/upd_marker" 2>/dev/null)" = 0 || fail_open "sbdata update marker is set"

volume_mount() {
    awk -v target="$1" -v kind="$2" '
        $2 == target && ($1 == "ubi0:sbdata" || $1 == "/dev/ubi0_5") && $3 == kind { ok=1 }
        END { exit !ok }' "$MOUNTS" 2>/dev/null
}

# ubifs is present on legacy kernels too. The per-volume BDI name is evidence
# of the multi-mount fix; a verified existing second native mount also proves it.
if volume_mount "$MOUNT" sbubifs; then
    KIND=sbubifs
elif volume_mount "$MOUNT" ubifs || {
    test -d "$ROOT/sys/class/bdi/ubifs_0_2" &&
    grep -q '^[[:space:]]*nodev[[:space:]]*ubifs$' "$FILESYSTEMS";
}; then
    KIND=ubifs
else
    KIND=sbubifs
fi
if mounted_at "$MOUNT" && ! volume_mount "$MOUNT" "$KIND"; then
    fail_open "mountpoint occupied by a different filesystem or volume"
fi
if ! mounted_at "$MOUNT" && awk -v target="$MOUNT" '
    ($1 == "ubi0:sbdata" || $1 == "/dev/ubi0_5") && $2 != target {found=1}
    END {exit !found}' "$MOUNTS"; then
    fail_open "sbdata is already mounted elsewhere"
fi
if test "$KIND" = sbubifs && ! volume_mount "$MOUNT" sbubifs; then
    # Never try this old module on an unknown firmware/kernel ABI. Native
    # capability takes precedence, and a native mount failure never falls back.
    case $(sed -n '1p' "$ROOT/etc/squeezeos.version" 2>/dev/null) in
        '7.7.2 r9663'|'7.7.3 r16676'|'9.0.1 r17084'|'9.0.2 r17146') ;;
        *) fail_open "legacy driver compatibility is not established for this firmware" ;;
    esac
    test "$(cat "$ROOT/proc/sys/kernel/osrelease" 2>/dev/null)" = 2.6.26.8-rt16 || fail_open "legacy kernel is incompatible"
    grep -Eq 'Hardware[[:space:]]*:[[:space:]]*(Baby|Logitech MX25 Baby Board)' "$ROOT/proc/cpuinfo" || fail_open "legacy hardware is incompatible"
    test -x "$HELPER" || fail_open "module verifier is missing"
    test -f "$MODULE" || fail_open "authorized module is missing"
    "$HELPER" verify-module "$MODULE" >/dev/null 2>&1 || fail_open "authorized module identity mismatch"
    if ! grep -q '^[[:space:]]*nodev[[:space:]]*sbubifs$' "$FILESYSTEMS" 2>/dev/null; then
        log "loading authorized legacy storage driver"
        insmod "$MODULE" >/dev/null 2>&1 || fail_open "sbubifs module load failed"
        MODULE_LOADED=1
    fi
    grep -q '^[[:space:]]*nodev[[:space:]]*sbubifs$' "$FILESYSTEMS" || fail_open "sbubifs did not register"
fi

if ! mounted_at "$MOUNT"; then
    log "mounting existing sbdata volume"
    mkdir -p "$MOUNT" || fail_open "cannot create mount point"
    mount -t "$KIND" ubi0:sbdata "$MOUNT" || fail_open "sbdata mount failed ($KIND); no format attempted"
    MOUNT_CREATED=1
fi
volume_mount "$MOUNT" "$KIND" || fail_open "sbdata mount verification failed"

test -d "$SOURCE" || fail_open "migrated applet tree is absent"
test -d "$SOURCE/AboutJive" -o -d "$SOURCE/SetupAppletInstaller" || fail_open "core applet sentinel is absent"
test -d "$SOURCE/StandaloneBase" || fail_open "StandaloneBase sentinel is absent"
test -d "$TARGET" || fail_open "stock applet tree is absent"

if ! mounted_at "$TARGET"; then
    log "activating extended applet tree with bind mount"
    mount -o bind "$SOURCE" "$TARGET" >/dev/null 2>&1 || fail_open "applet bind mount failed"
    BIND_CREATED=1
fi
volume_mount "$TARGET" "$KIND" || fail_open "applet bind verification failed"
if test -z "$ROOT"; then
    test "$SOURCE" -ef "$TARGET" || fail_open "applet bind points at a different directory"
fi
test -d "$TARGET/StandaloneBase" || fail_open "post-bind StandaloneBase check failed"

rm -f "$REBOOT_MARKER" 2>/dev/null || true
log "activation complete"
exit 0
