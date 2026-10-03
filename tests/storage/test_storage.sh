#!/bin/sh
set -eu
helper=$1
runner=${2:-}
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/proc/sys/kernel" "$fixture/sys/class/ubi/ubi0/ubi0_2" "$fixture/etc"
for spec in '0 kernel_bak' '1 cramfs_bak' '3 kernel' '4 cramfs'; do
    set -- $spec
    mkdir -p "$fixture/sys/class/ubi/ubi0/ubi0_$1"
    printf '%s\n' "$2" >"$fixture/sys/class/ubi/ubi0/ubi0_$1/name"
done
printf 'Hardware : Baby\n' >"$fixture/proc/cpuinfo"
printf 'armv5tejl\n' >"$fixture/proc/sys/kernel/architecture"
printf '2.6.26.8-rt16\n' >"$fixture/proc/sys/kernel/osrelease"
printf 'dev: size erasesize name\nmtd1: 08000000 00020000 \"root\"\n' >"$fixture/proc/mtd"
printf 'ubi0:ubifs /mnt/storage ubifs rw,noatime 0 0\n' >"$fixture/proc/mounts"
printf '7.7.3 r16676\n' >"$fixture/etc/squeezeos.version"
printf '1014\n' >"$fixture/sys/class/ubi/ubi0/total_eraseblocks"
printf '660\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
printf '10\n' >"$fixture/sys/class/ubi/ubi0/reserved_for_bad"
printf '0\n' >"$fixture/sys/class/ubi/ubi0/bad_peb_count"
printf '129024\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/usable_eb_size"
printf '82\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/reserved_ebs"
printf 'dynamic\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/type"
printf 'ubifs\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/name"
printf '0\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/corrupted"
printf '8178893\n' >"$fixture/sys/class/ubi/ubi0/test_fs_total_bytes"
printf '5347738\n' >"$fixture/sys/class/ubi/ubi0/test_fs_free_bytes"
run(){ SB_STORAGE_ROOT="$fixture" $runner "$helper" "$@"; }
out=$(run check)
echo "$out" | grep -q '^compatible=true$'
echo "$out" | grep -q '^target_lebs=521$'
echo "$out" | grep -q '^flash_bytes=134217728$'
printf '9.0.1 r17084\n' >"$fixture/etc/squeezeos.version"
out=$(run check)
echo "$out" | grep -q '^compatible=true$'
printf '9.0.2 r99999\n' >"$fixture/etc/squeezeos.version"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=unsupported_firmware$'
printf '7.7.3 r16676\n' >"$fixture/etc/squeezeos.version"
printf '1\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/corrupted"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=volume_corrupted_or_unknown$'
printf '0\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/corrupted"
printf 'x86_64\n' >"$fixture/proc/sys/kernel/architecture"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=unsupported_architecture$'
printf 'armv5tejl\n' >"$fixture/proc/sys/kernel/architecture"
printf '1\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=insufficient_capacity$'
printf '660\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
printf 'Hardware : Unknown Board\n' >"$fixture/proc/cpuinfo"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=unsupported_model$'
printf 'Hardware : Baby\n' >"$fixture/proc/cpuinfo"
mkdir "$fixture/sys/class/ubi/ubi0/ubi0_6"
printf 'mystery\n' >"$fixture/sys/class/ubi/ubi0/ubi0_6/name"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=unexpected_volume_layout$'
printf 'ubifs\n' >"$fixture/sys/class/ubi/ubi0/ubi0_6/name"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=ambiguous_volume$'
rm -rf "$fixture/sys/class/ubi/ubi0/ubi0_6"
mv "$fixture/sys/class/ubi/ubi0" "$fixture/sys/class/ubi/ubi0.missing"
set +e; out=$(run check); status=$?; set -e
test "$status" = 2; echo "$out" | grep -q '^reason=unexpected_ubi_device_count$'
set +e; run expand >/dev/null 2>&1; status=$?; set -e
test "$status" = 78
set +e; run resize-test-volume 8388608 WRONG_TOKEN >/dev/null 2>&1; status=$?; set -e
test "$status" = 77
set +e; run resize-test-volume 8388608 I_ACCEPT_TEST_VOLUME_FLASH_WRITE >/dev/null 2>&1; status=$?; set -e
test "$status" = 78
echo storage-tests-ok
