#!/bin/sh
# Early boot helper for a separately approved sbdata installation.
# It never creates, formats, resizes, renames, or removes a UBI volume.
test -n "${SB_STORAGE_ROOT:-}" || PATH=/sbin:/bin:/usr/sbin:/usr/bin
ROOT=${SB_STORAGE_ROOT:-}
SYS=$ROOT/sys/class/ubi/ubi0
MOUNT=$ROOT/mnt/sbdata
TARGET=$ROOT/usr/share/jive/applets
SOURCE=$MOUNT/applets
MOUNTS=$ROOT/proc/mounts
log(){ echo "StandaloneBase sbdata: $*" >&2; command -v logger >/dev/null 2>&1 && logger -t standalonebase-sbdata "$*" || true; }
fail(){ log "fallback: $*"; exit 0; }
found=
for name_file in "$SYS"/ubi0_*/name; do
 test -f "$name_file" || continue
 test "$(cat "$name_file")" = sbdata || continue
 test -z "$found" || fail "ambiguous sbdata volumes"
 found=${name_file%/name}
done
test -n "$found" || fail "sbdata is absent"
test "$(cat "$found/type" 2>/dev/null)" = dynamic || fail "sbdata is not dynamic"
test "$(cat "$found/corrupted" 2>/dev/null)" = 0 || fail "sbdata update marker is set"
id=${found##*_}
case "$id" in 0|1|2|3|4|*[!0-9]*|'') fail "refusing volume ID $id";; esac
if test -n "$ROOT"; then test -e "$ROOT/dev/ubi0_$id" || fail "device node ubi0_$id is missing"; else test -c "/dev/ubi0_$id" || fail "device node /dev/ubi0_$id is missing"; fi
mkdir -p "$MOUNT"
grep -q "^[^ ]* $MOUNT ubifs " "$MOUNTS" || mount -t ubifs ubi0:sbdata "$MOUNT" || fail "mount failed"
test -d "$SOURCE" || fail "sbdata applet directory is missing"
test -d "$SOURCE/StandaloneBase" || fail "StandaloneBase is missing from sbdata"
test -d "$SOURCE/SetupApplet" -o -d "$SOURCE/AppletInstaller" || fail "standard applets are missing from sbdata"
test -d "$TARGET" || fail "active applet directory is missing"
grep -q "^[^ ]* $TARGET " "$MOUNTS" || mount -o bind "$SOURCE" "$TARGET" || fail "bind mount failed"
if test ! -d "$TARGET/StandaloneBase" || { test ! -d "$TARGET/SetupApplet" && test ! -d "$TARGET/AppletInstaller"; }; then
 umount "$TARGET" 2>/dev/null || true
 fail "post-bind visibility check failed"
fi
log "sbdata applet bind active"
exit 0
