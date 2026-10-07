#!/bin/sh
set -eu
setup=$1
boot=$2
helper=$3
module=$4
image=${5:-artifacts/ubi/sbdata-empty.ubifs}
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT

make_base() {
    rm -rf "$fixture"/*
    mkdir -p "$fixture/proc/sys/kernel" "$fixture/sys/class/ubi/ubi0" "$fixture/etc/init.d" \
        "$fixture/usr/share/jive/applets/SetupAppletInstaller" \
        "$fixture/usr/share/jive/applets/StandaloneBase" "$fixture/mnt/storage" \
        "$fixture/tmp" "$fixture/bin" "$fixture/dev" "$fixture/usr/sbin"
    printf 'Hardware : Baby\n' >"$fixture/proc/cpuinfo"
    printf '2.6.26.8-rt16\n' >"$fixture/proc/sys/kernel/osrelease"
    printf '7.7.3 r16676\n' >"$fixture/etc/squeezeos.version"
    printf 'dev: size erasesize name\n' >"$fixture/proc/mtd"
    printf 'ubi0:ubifs %s/mnt/storage ubifs rw 0 0\n' "$fixture" >"$fixture/proc/mounts"
    printf 'nodev\tubifs\n' >"$fixture/proc/filesystems"
    printf '#!/bin/sh\ntest -x /etc/init.d/rcS.local && /etc/init.d/rcS.local\n' >"$fixture/etc/init.d/rcS"
    printf '100\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
    printf '2048\n' >"$fixture/sys/class/ubi/ubi0/min_io_size"
    touch "$fixture/usr/sbin/ubiupdatevol"
    chmod 755 "$fixture/usr/sbin/ubiupdatevol"
    cp "$helper" "$fixture/usr/share/jive/applets/StandaloneBase/sb-storage-helper"
    cp "$setup" "$boot" "$module" "$image" "$fixture/usr/share/jive/applets/StandaloneBase/"
    test "$(basename "$image")" = sbdata-empty.ubifs || \
        mv "$fixture/usr/share/jive/applets/StandaloneBase/$(basename "$image")" "$fixture/usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs"
    chmod 755 "$fixture/usr/share/jive/applets/StandaloneBase/sb-storage-helper" \
        "$fixture/usr/share/jive/applets/StandaloneBase/storage-setup.sh" \
        "$fixture/usr/share/jive/applets/StandaloneBase/storage-boot.sh"
}

add_volume() {
    mkdir -p "$fixture/sys/class/ubi/ubi0_5"
    printf '%s\n' "${1:-sbdata}" >"$fixture/sys/class/ubi/ubi0_5/name"
    printf 'dynamic\n' >"$fixture/sys/class/ubi/ubi0_5/type"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/corrupted"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/upd_marker"
    printf '521\n' >"$fixture/sys/class/ubi/ubi0_5/reserved_ebs"
    printf '67221504\n' >"$fixture/sys/class/ubi/ubi0_5/data_bytes"
    printf '129024\n' >"$fixture/sys/class/ubi/ubi0_5/usable_eb_size"
    touch "$fixture/dev/ubi0_5"
}

check() {
    SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
        sh "$setup" check
}

expect_status() { check | grep -q "^STATUS=$1$"; }

# no UBI
make_base; rm -rf "$fixture/sys/class/ubi/ubi0"; expect_status UNSUPPORTED
# no sbdata
make_base; expect_status UNAVAILABLE
# valid inactive sbdata
make_base; add_volume; expect_status AVAILABLE
# active sbdata
mkdir -p "$fixture/mnt/sbdata/applets"
printf 'nodev\tsbubifs\n' >>"$fixture/proc/filesystems"
printf 'ubi0:sbdata %s/mnt/sbdata sbubifs rw 0 0\nubi0:sbdata %s/usr/share/jive/applets sbubifs rw 0 0\n' "$fixture" "$fixture" >>"$fixture/proc/mounts"
expect_status ACTIVE
# wrong volume name
make_base; add_volume wrongname; expect_status ERROR
# missing module and factory-reset-like state
make_base; add_volume; rm "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko"
expect_status AVAILABLE; check | grep -q '^INITIALIZATION_SUPPORTED=0$'
# incorrect module hash
make_base; add_volume; printf x >>"$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko"
check | grep -q '^BUNDLED_MODULE_VALID=0$'

# invalid image identity is visible and blocks initialize before the gate
make_base; add_volume; printf x >>"$fixture/usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs"
check | grep -q '^SBDATA_IMAGE_VALID=0$'
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 23
grep -q '^ERROR=sbdata_image_identity_mismatch$' "$fixture/tmp/sbstorage.status"

# Destructive preflight rechecks exact geometry, node identity and mount state.
for field_value in 'reserved_ebs 520' 'data_bytes 1' 'usable_eb_size 1'; do
    make_base; add_volume; set -- $field_value; printf '%s\n' "$2" >"$fixture/sys/class/ubi/ubi0_5/$1"
    set +e
    SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
        sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
    rc=$?
    set -e
    test "$rc" = 20
done
make_base; add_volume; rm "$fixture/dev/ubi0_5"
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 21
make_base; add_volume
printf 'ubi0:sbdata %s/mnt/elsewhere sbubifs rw 0 0\n' "$fixture" >>"$fixture/proc/mounts"
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 20

# Real hardware path deliberately stops before destructive initialization.
make_base; add_volume
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 23
grep -q '^EXIT_CODE=23$' "$fixture/tmp/sbstorage.status"

# Fixture-only continuation exercises the post-format transaction.
printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/test_initialize_result"
cat >"$fixture/bin/insmod" <<'EOF'
#!/bin/sh
test "${SB_TEST_FAIL_INSMOD:-0}" = 1 && exit 1
printf 'nodev\tsbubifs\n' >>"$SB_STORAGE_ROOT/proc/filesystems"
exit 0
EOF
cat >"$fixture/bin/mount" <<'EOF'
#!/bin/sh
test "${SB_TEST_FAIL_MOUNT:-0}" = 1 && exit 1
if test "$1" = -t; then printf 'ubi0:sbdata %s/mnt/sbdata sbubifs rw 0 0\n' "$SB_STORAGE_ROOT" >>"$SB_STORAGE_ROOT/proc/mounts"
else printf 'ubi0:sbdata %s/usr/share/jive/applets sbubifs rw 0 0\n' "$SB_STORAGE_ROOT" >>"$SB_STORAGE_ROOT/proc/mounts"; fi
exit 0
EOF
cat >"$fixture/bin/cp" <<'EOF'
#!/bin/sh
case "$*" in
  *'/usr/share/jive/applets/.'*) test "${SB_TEST_FAIL_MIGRATION:-0}" = 1 && exit 1 ;;
  *'/mnt/storage/sbubifs-authorized.ko') test "${SB_TEST_FAIL_BOOT_INSTALL:-0}" = 1 && exit 1 ;;
esac
/bin/cp "$@" || { echo "cp failed: $*" >&2; exit 1; }
case "$*" in
  *'/usr/share/jive/applets/.'*) test "${SB_TEST_FAIL_VALIDATION:-0}" = 1 && rm -rf "$SB_STORAGE_ROOT/mnt/sbdata/applets/StandaloneBase" ;;
esac
exit 0
EOF
cat >"$fixture/bin/sync" <<'EOF'
#!/bin/sh
exit 0
EOF
cat >"$fixture/bin/rmmod" <<'EOF'
#!/bin/sh
exit 0
EOF
cat >"$fixture/bin/umount" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 755 "$fixture/bin/"*
export PATH="$fixture/bin:$PATH"

run_init() {
    SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
        sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
}

# module load failure
printf 'nodev\tubifs\n' >"$fixture/proc/filesystems"
set +e; SB_TEST_FAIL_INSMOD=1 run_init; rc=$?; set -e; test "$rc" = 22
unset SB_TEST_FAIL_INSMOD
# mount failure
printf 'nodev\tubifs\nnodev\tsbubifs\n' >"$fixture/proc/filesystems"; : >"$fixture/proc/mounts"
set +e; SB_TEST_FAIL_MOUNT=1 run_init; rc=$?; set -e; test "$rc" = 24
unset SB_TEST_FAIL_MOUNT
# migration failure
: >"$fixture/proc/mounts"
set +e; SB_TEST_FAIL_MIGRATION=1 run_init; rc=$?; set -e; test "$rc" = 25
unset SB_TEST_FAIL_MIGRATION
# validation failure
rm -rf "$fixture/mnt/sbdata"; : >"$fixture/proc/mounts"
set +e; SB_TEST_FAIL_VALIDATION=1 run_init; rc=$?; set -e; test "$rc" = 26
unset SB_TEST_FAIL_VALIDATION
# boot installation failure
rm -rf "$fixture/mnt/sbdata"; : >"$fixture/proc/mounts"
set +e; SB_TEST_FAIL_BOOT_INSTALL=1 run_init; rc=$?; set -e
test "$rc" = 27 || { cat "$fixture/tmp/sbstorage.log"; exit 1; }
unset SB_TEST_FAIL_BOOT_INSTALL
# successful fixture transaction -> reboot required
rm -rf "$fixture/mnt/sbdata" "$fixture/mnt/storage/standalonebase"; : >"$fixture/proc/mounts"
printf 'ubi0:ubifs %s/mnt/storage ubifs rw 0 0\n' "$fixture" >"$fixture/proc/mounts"
run_init
grep -q '^REBOOT_REQUIRED=1$' "$fixture/tmp/sbstorage.status"
expect_status REBOOT_REQUIRED
test -x "$fixture/etc/init.d/rcS.local"
grep -q 'STANDALONEBASE-EXTENDED-STORAGE' "$fixture/etc/init.d/rcS.local"

# Boot activation is idempotent and fail-open; it activates the bind without
# changing the production mount.
SB_STORAGE_ROOT="$fixture" sh "$boot"
expect_status ACTIVE
test ! -e "$fixture/mnt/storage/standalonebase/extended-storage-reboot-required"
SB_STORAGE_ROOT="$fixture" sh "$boot"
printf 'wrongname\n' >"$fixture/sys/class/ubi/ubi0_5/name"
mounts_before=$(cat "$fixture/proc/mounts")
SB_STORAGE_ROOT="$fixture" sh "$boot"
test "$mounts_before" = "$(cat "$fixture/proc/mounts")"

echo extended-storage-tests-ok
