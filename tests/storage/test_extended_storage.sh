#!/bin/sh
set -eu
setup=$1
boot=$2
helper=$3
module=$4
image=${5:-artifacts/ubi/sbdata-empty.ubifs}
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT

# The boot path is permanently activation-only.
! grep -qE 'ubimkvol|ubiupdatevol' "$boot"

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
    printf '1014\n' >"$fixture/sys/class/ubi/ubi0/total_eraseblocks"
    printf '660\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
    printf '129024\n' >"$fixture/sys/class/ubi/ubi0/eraseblock_size"
    printf '2048\n' >"$fixture/sys/class/ubi/ubi0/min_io_size"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0/bad_peb_count"
    printf '10\n' >"$fixture/sys/class/ubi/ubi0/reserved_for_bad"
    touch "$fixture/dev/ubi0"
    add_stock_volume 0 kernel_bak 23 2967552
    add_stock_volume 1 cramfs_bak 106 13676544
    add_stock_volume 2 ubifs 82 10579968
    add_stock_volume 3 kernel 23 2967552
    add_stock_volume 4 cramfs 106 13676544
    cat >"$fixture/usr/sbin/ubimkvol" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$SB_STORAGE_ROOT/tmp/ubimkvol.calls"
test "${SB_TEST_FAIL_UBIMKVOL:-0}" = 1 && exit 9
volume="$SB_STORAGE_ROOT/sys/class/ubi/ubi0_5"
mkdir -p "$volume"
printf '%s\n' "${SB_TEST_POST_NAME:-sbdata}" >"$volume/name"
printf '%s\n' "${SB_TEST_POST_TYPE:-dynamic}" >"$volume/type"
printf '1\n' >"$volume/alignment"
printf '%s\n' "${SB_TEST_POST_RESERVED_EBS:-521}" >"$volume/reserved_ebs"
printf '%s\n' "${SB_TEST_POST_DATA_BYTES:-67221504}" >"$volume/data_bytes"
printf '%s\n' "${SB_TEST_POST_LEB_SIZE:-129024}" >"$volume/usable_eb_size"
printf '%s\n' "${SB_TEST_POST_CORRUPTED:-0}" >"$volume/corrupted"
printf '%s\n' "${SB_TEST_POST_UPD_MARKER:-0}" >"$volume/upd_marker"
touch "$SB_STORAGE_ROOT/dev/ubi0_5"
printf '%s\n' "${SB_TEST_POST_AVAILABLE_LEBS:-139}" >"$SB_STORAGE_ROOT/sys/class/ubi/ubi0/avail_eraseblocks"
exit 0
EOF
    cat >"$fixture/usr/sbin/ubiupdatevol" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$SB_STORAGE_ROOT/tmp/ubiupdatevol.calls"
test "$1" = "$SB_STORAGE_ROOT/dev/ubi0_5" || exit 91
test "$2" = "$SB_STORAGE_ROOT/usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs" || exit 92
test "${SB_TEST_FAIL_UBIUPDATEVOL:-0}" = 1 && exit 9
if test "${SB_TEST_FAIL_POST_WRITE_HEALTH:-0}" = 1; then
    printf '1\n' >"$SB_STORAGE_ROOT/sys/class/ubi/ubi0_5/corrupted"
fi
exit 0
EOF
    chmod 755 "$fixture/usr/sbin/ubimkvol" "$fixture/usr/sbin/ubiupdatevol"
    cp "$helper" "$fixture/usr/share/jive/applets/StandaloneBase/sb-storage-helper"
    cp "$setup" "$boot" "$module" "$image" "$fixture/usr/share/jive/applets/StandaloneBase/"
    test "$(basename "$image")" = sbdata-empty.ubifs || \
        mv "$fixture/usr/share/jive/applets/StandaloneBase/$(basename "$image")" "$fixture/usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs"
    chmod 755 "$fixture/usr/share/jive/applets/StandaloneBase/sb-storage-helper" \
        "$fixture/usr/share/jive/applets/StandaloneBase/storage-setup.sh" \
        "$fixture/usr/share/jive/applets/StandaloneBase/storage-boot.sh"
}

add_stock_volume() {
    stock="$fixture/sys/class/ubi/ubi0_$1"
    mkdir -p "$stock"
    printf '%s\n' "$2" >"$stock/name"
    printf 'dynamic\n' >"$stock/type"
    printf '%s\n' "$3" >"$stock/reserved_ebs"
    printf '%s\n' "$4" >"$stock/data_bytes"
    printf '0\n' >"$stock/corrupted"
    printf '0\n' >"$stock/upd_marker"
}

add_volume() {
    mkdir -p "$fixture/sys/class/ubi/ubi0_5"
    printf '%s\n' "${1:-sbdata}" >"$fixture/sys/class/ubi/ubi0_5/name"
    printf 'dynamic\n' >"$fixture/sys/class/ubi/ubi0_5/type"
    printf '1\n' >"$fixture/sys/class/ubi/ubi0_5/alignment"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/corrupted"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/upd_marker"
    printf '521\n' >"$fixture/sys/class/ubi/ubi0_5/reserved_ebs"
    printf '67221504\n' >"$fixture/sys/class/ubi/ubi0_5/data_bytes"
    printf '129024\n' >"$fixture/sys/class/ubi/ubi0_5/usable_eb_size"
    touch "$fixture/dev/ubi0_5"
    printf '139\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
}

check() {
    SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
        sh "$setup" check
}

expect_status() { check | grep -q "^STATUS=$1$"; }
assert_ubiupdatevol_not_called() { test ! -e "$fixture/tmp/ubiupdatevol.calls"; }
assert_ubimkvol_not_called() { test ! -e "$fixture/tmp/ubimkvol.calls"; }

run_init_raw() {
    SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
        sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
}

# no UBI
make_base; rm -rf "$fixture/sys/class/ubi/ubi0"; expect_status UNSUPPORTED
# incomplete stock layout without sbdata is not treated as virgin
make_base; rm -rf "$fixture/sys/class/ubi/ubi0_4"; expect_status UNAVAILABLE
# exact virgin layout is eligible for explicit initialization
make_base; expect_status AVAILABLE
check | grep -q '^INITIALIZATION_SUPPORTED=1$'
check | grep -q '^INITIALIZATION_REASON=validated_virgin_layout$'
# valid inactive sbdata
make_base; add_volume; expect_status AVAILABLE
check | grep -q '^INITIALIZATION_SUPPORTED=1$'
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

expect_precreate_failure() {
    expected=$1
    set +e; run_init_raw; rc=$?; set -e
    test "$rc" = "$expected"
    assert_ubimkvol_not_called
    assert_ubiupdatevol_not_called
}

# Every virgin-layout invariant is a hard gate before ubimkvol.
make_base; printf '9.0.1 r17084\n' >"$fixture/etc/squeezeos.version"; expect_precreate_failure 28
make_base; printf '1013\n' >"$fixture/sys/class/ubi/ubi0/total_eraseblocks"; expect_precreate_failure 28
make_base; printf '520\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"; expect_precreate_failure 29
make_base; printf '128000\n' >"$fixture/sys/class/ubi/ubi0/eraseblock_size"; expect_precreate_failure 28
make_base; printf '4096\n' >"$fixture/sys/class/ubi/ubi0/min_io_size"; expect_precreate_failure 20
make_base; rm -rf "$fixture/sys/class/ubi/ubi0_3"; expect_precreate_failure 28
make_base; add_stock_volume 6 unexpected 1 129024; expect_precreate_failure 28
make_base; printf 'wrong\n' >"$fixture/sys/class/ubi/ubi0_1/name"; expect_precreate_failure 28
make_base; printf '1\n' >"$fixture/sys/class/ubi/ubi0_2/data_bytes"; expect_precreate_failure 28
make_base; printf 'static\n' >"$fixture/sys/class/ubi/ubi0_4/type"; expect_precreate_failure 28
make_base; printf '1\n' >"$fixture/sys/class/ubi/ubi0_0/corrupted"; expect_precreate_failure 28
make_base; printf '1\n' >"$fixture/sys/class/ubi/ubi0_3/upd_marker"; expect_precreate_failure 28
make_base; add_volume wrongname; expect_precreate_failure 21
make_base; touch "$fixture/dev/ubi0_5"; expect_precreate_failure 21
make_base; printf x >>"$fixture/usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs"; expect_precreate_failure 23
make_base; printf x >>"$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko"; expect_precreate_failure 22
make_base; rm "$fixture/usr/sbin/ubimkvol"; expect_precreate_failure 30

# Creation failure and every post-create mismatch stop before ubiupdatevol.
make_base
set +e; SB_TEST_FAIL_UBIMKVOL=1 run_init_raw; rc=$?; set -e
test "$rc" = 31
test "$(wc -l <"$fixture/tmp/ubimkvol.calls" | tr -d ' ')" = 1
assert_ubiupdatevol_not_called
test ! -e "$fixture/mnt/storage/standalonebase"

for post_case in 'SB_TEST_POST_NAME wrong' 'SB_TEST_POST_DATA_BYTES 1' \
    'SB_TEST_POST_CORRUPTED 1' 'SB_TEST_POST_AVAILABLE_LEBS 140'; do
    make_base
    set -- $post_case
    export "$1=$2"
    set +e; run_init_raw; rc=$?; set -e
    unset "$1"
    test "$rc" = 32
    test -e "$fixture/tmp/ubimkvol.calls"
    assert_ubiupdatevol_not_called
done

# A valid creation uses the exact command and converges into image writing.
make_base
set +e; run_init_raw; rc=$?; set -e
test "$rc" = 22
test "$(cat "$fixture/tmp/ubimkvol.calls")" = "$fixture/dev/ubi0 -n 5 -N sbdata -S 521 -t dynamic"
test -e "$fixture/tmp/ubiupdatevol.calls"

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
assert_ubiupdatevol_not_called

# Wrong volume identity never reaches the destructive command.
make_base; add_volume wrongname
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 21
assert_ubiupdatevol_not_called

# Destructive preflight rechecks exact geometry, node identity and mount state.
for field_value in 'reserved_ebs 520' 'data_bytes 1' 'usable_eb_size 1'; do
    make_base; add_volume; set -- $field_value; printf '%s\n' "$2" >"$fixture/sys/class/ubi/ubi0_5/$1"
    set +e
    SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
        sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
    rc=$?
    set -e
    test "$rc" = 20
    assert_ubiupdatevol_not_called
done

# Minimum I/O geometry is independently mandatory.
make_base; add_volume; printf '4096\n' >"$fixture/sys/class/ubi/ubi0/min_io_size"
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 20
assert_ubiupdatevol_not_called
make_base; add_volume; rm "$fixture/dev/ubi0_5"
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 21
assert_ubiupdatevol_not_called
make_base; add_volume
printf 'ubi0:sbdata %s/mnt/elsewhere sbubifs rw 0 0\n' "$fixture" >>"$fixture/proc/mounts"
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" initialize "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
rc=$?
set -e
test "$rc" = 20
assert_ubiupdatevol_not_called

# Mocks exercise the destructive boundary and post-format transaction without
# touching UBI or NAND.
make_base; add_volume
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

reset_runtime() {
    rm -rf "$fixture/mnt/sbdata" "$fixture/mnt/storage/standalonebase"
    rm -f "$fixture/tmp/ubimkvol.calls" "$fixture/tmp/ubiupdatevol.calls" "$fixture/tmp/sbstorage.status" "$fixture/tmp/sbstorage.log"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/corrupted"
    printf '0\n' >"$fixture/sys/class/ubi/ubi0_5/upd_marker"
    printf 'nodev\tubifs\n' >"$fixture/proc/filesystems"
    printf 'ubi0:ubifs %s/mnt/storage ubifs rw 0 0\n' "$fixture" >"$fixture/proc/mounts"
}

reset_virgin() {
    reset_runtime
    rm -rf "$fixture/sys/class/ubi/ubi0_5"
    rm -f "$fixture/dev/ubi0_5"
    printf '660\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
}

# Read-only entry points and boot never invoke initialization.
reset_runtime
check >/dev/null
assert_ubiupdatevol_not_called
assert_ubimkvol_not_called
set +e
SB_STORAGE_ROOT="$fixture" SB_STORAGE_APPLET_DIR="$fixture/usr/share/jive/applets/StandaloneBase" \
    sh "$setup" mount "$fixture/usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko" >/dev/null
set -e
assert_ubiupdatevol_not_called
assert_ubimkvol_not_called
SB_STORAGE_ROOT="$fixture" sh "$boot"
assert_ubiupdatevol_not_called
assert_ubimkvol_not_called

# A valid explicit initialization reaches the exact mocked command, while a
# command failure aborts before driver loading, mounting, or migration.
reset_runtime
set +e; SB_TEST_FAIL_UBIUPDATEVOL=1 run_init; rc=$?; set -e
test "$rc" = 23
grep -q '^ERROR=ubiupdatevol_failure$' "$fixture/tmp/sbstorage.status"
test "$(wc -l <"$fixture/tmp/ubiupdatevol.calls" | tr -d ' ')" = 1
test ! -d "$fixture/mnt/sbdata"
unset SB_TEST_FAIL_UBIUPDATEVOL

# Post-write health failure stops before loading, mounting, and migration.
reset_runtime
set +e; SB_TEST_FAIL_POST_WRITE_HEALTH=1 run_init; rc=$?; set -e
test "$rc" = 23
grep -q '^ERROR=post_update_corruption_detected$' "$fixture/tmp/sbstorage.status"
test -e "$fixture/tmp/ubiupdatevol.calls"
test ! -d "$fixture/mnt/sbdata"
unset SB_TEST_FAIL_POST_WRITE_HEALTH

# Reset the successful fixture used by all post-write workflow tests.
reset_runtime

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
assert_ubimkvol_not_called
test -s "$fixture/tmp/ubiupdatevol.calls"
grep -q '^STAGE=COMPLETE$' "$fixture/tmp/sbstorage.status"
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

# Complete virgin bootstrap: create, validate, update and reuse the unchanged
# mount/migration/boot-support transaction.
reset_virgin
run_init
test "$(cat "$fixture/tmp/ubimkvol.calls")" = "$fixture/dev/ubi0 -n 5 -N sbdata -S 521 -t dynamic"
test -s "$fixture/tmp/ubiupdatevol.calls"
grep -q '^STAGE=COMPLETE$' "$fixture/tmp/sbstorage.status"
grep -q '^REBOOT_REQUIRED=1$' "$fixture/tmp/sbstorage.status"
test -d "$fixture/mnt/sbdata/applets/StandaloneBase"
test -x "$fixture/etc/init.d/rcS.local"

echo extended-storage-tests-ok
