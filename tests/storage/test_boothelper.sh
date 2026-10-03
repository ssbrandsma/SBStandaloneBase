#!/bin/sh
set -eu
boot=$1
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/sys/class/ubi/ubi0/ubi0_5" "$fixture/dev" "$fixture/proc" \
 "$fixture/mnt/sbdata/applets/SetupApplet" "$fixture/mnt/sbdata/applets/StandaloneBase" \
 "$fixture/usr/share/jive/applets/SetupApplet" "$fixture/usr/share/jive/applets/StandaloneBase" "$fixture/bin"
printf 'sbdata\n' >"$fixture/sys/class/ubi/ubi0/ubi0_5/name"
printf 'dynamic\n' >"$fixture/sys/class/ubi/ubi0/ubi0_5/type"
printf '0\n' >"$fixture/sys/class/ubi/ubi0/ubi0_5/corrupted"
touch "$fixture/dev/ubi0_5" "$fixture/proc/mounts" "$fixture/actions"
cat >"$fixture/bin/mount" <<'EOF'
#!/bin/sh
echo "mount $*" >>"$SB_STORAGE_ROOT/actions"
case "${FAIL_MODE:-}:$*" in volume:*sbdata*) exit 1;; bind:*bind*) exit 1;; esac
exit 0
EOF
cat >"$fixture/bin/umount" <<'EOF'
#!/bin/sh
echo "umount $*" >>"$SB_STORAGE_ROOT/actions"
exit 0
EOF
cat >"$fixture/bin/logger" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 755 "$fixture/bin/mount" "$fixture/bin/umount" "$fixture/bin/logger"
run(){ SB_STORAGE_ROOT="$fixture" PATH="$fixture/bin:$PATH" FAIL_MODE=${1:-} sh "$boot" 2>&1; }

# Mount failure leaves the original applet directory visible.
out=$(run volume); echo "$out" | grep -q 'fallback: mount failed'; test -d "$fixture/usr/share/jive/applets/StandaloneBase"

# Bind failure also leaves the original directory visible.
printf 'ubi0:sbdata %s/mnt/sbdata ubifs rw 0 0\n' "$fixture" >"$fixture/proc/mounts"
out=$(run bind); echo "$out" | grep -q 'fallback: bind mount failed'; test -d "$fixture/usr/share/jive/applets/StandaloneBase"

# A post-bind visibility failure invokes the fallback unmount.
rm -rf "$fixture/usr/share/jive/applets/StandaloneBase"
out=$(run none); echo "$out" | grep -q 'fallback: post-bind visibility check failed'
grep -q '^umount ' "$fixture/actions"
echo boothelper-tests-ok
