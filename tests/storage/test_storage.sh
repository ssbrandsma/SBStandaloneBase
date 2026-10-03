#!/bin/sh
set -eu
helper=$1
runner=${2:-}
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/proc/sys/kernel" "$fixture/sys/class/ubi/ubi0" "$fixture/etc/init.d" \
 "$fixture/dev" "$fixture/usr/sbin" "$fixture/usr/share/jive/applets/SetupAppletInstaller" \
 "$fixture/usr/share/jive/applets/StandaloneBase" "$fixture/mnt/storage/standalonebase" "$fixture/tmp"
for spec in '0 kernel_bak' '1 cramfs_bak' '2 ubifs' '3 kernel' '4 cramfs'; do
 set -- $spec; d="$fixture/sys/class/ubi/ubi0/ubi0_$1"; mkdir "$d"
 printf '%s\n' "$2" >"$d/name"; printf 'dynamic\n' >"$d/type"; printf '0\n' >"$d/corrupted"
done
printf '129024\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/usable_eb_size"
printf '82\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/reserved_ebs"
printf 'Hardware : Baby\n' >"$fixture/proc/cpuinfo"
printf 'armv5tejl\n' >"$fixture/proc/sys/kernel/architecture"
printf '2.6.26.8-rt16\n' >"$fixture/proc/sys/kernel/osrelease"
printf 'nodev\tubifs\n' >"$fixture/proc/filesystems"
printf 'dev: size erasesize name\nmtd1: 08000000 00020000 "root"\n' >"$fixture/proc/mtd"
printf 'ubi0:ubifs /mnt/storage ubifs rw,noatime 0 0\n' >"$fixture/proc/mounts"
printf '7.7.3 r16676\n' >"$fixture/etc/squeezeos.version"
printf '1014\n' >"$fixture/sys/class/ubi/ubi0/total_eraseblocks"
printf '660\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
printf '10\n' >"$fixture/sys/class/ubi/ubi0/reserved_for_bad"
printf '0\n' >"$fixture/sys/class/ubi/ubi0/bad_peb_count"
printf '0\n' >"$fixture/sys/class/ubi/ubi0/read_only"
printf '8178893\n' >"$fixture/sys/class/ubi/ubi0/test_fs_total_bytes"
printf '5347738\n' >"$fixture/sys/class/ubi/ubi0/test_fs_free_bytes"
touch "$fixture/dev/ubi0" "$fixture/usr/sbin/ubimkvol" "$fixture/mnt/storage/standalonebase/mkfs.ubifs"
run(){ SB_STORAGE_ROOT="$fixture" $runner "$helper" "$@"; }
reason(){ echo "$1" | grep -q "^prepare_reason=$2$"; }

# Existing production layout without sbdata.
out=$(run storage-status); echo "$out" | grep -q '^installation_state=absent$'
out=$(run check); echo "$out" | grep -q '^compatible=true$'
out=$(run prepare); echo "$out" | grep -q '^prepare_ready=true$'; echo "$out" | grep -q '^planned_volume=sbdata$'
set +e; run install >/dev/null 2>&1; s=$?; set -e; test "$s" = 78
set +e; run install >/dev/null 2>&1; s=$?; set -e; test "$s" = 78
set +e; run resize-test-volume 8388608 token >/dev/null 2>&1; s=$?; set -e; test "$s" = 64
set +e; run expand >/dev/null 2>&1; s=$?; set -e; test "$s" = 78

# Unknown extra volume.
mkdir "$fixture/sys/class/ubi/ubi0/ubi0_6"; printf 'mystery\n' >"$fixture/sys/class/ubi/ubi0/ubi0_6/name"; printf '0\n' >"$fixture/sys/class/ubi/ubi0/ubi0_6/corrupted"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" unexpected_volume_layout
rm -rf "$fixture/sys/class/ubi/ubi0/ubi0_6"

# Historically dangerous ubifs_test name is explicitly forbidden.
mkdir "$fixture/sys/class/ubi/ubi0/ubi0_6"; printf 'ubifs_test\n' >"$fixture/sys/class/ubi/ubi0/ubi0_6/name"; printf '0\n' >"$fixture/sys/class/ubi/ubi0/ubi0_6/corrupted"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" forbidden_ubifs_like_volume
rm -rf "$fixture/sys/class/ubi/ubi0/ubi0_6"

# Capacity reserve, unexpected production ID, and corruption.
printf '584\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" insufficient_capacity_with_reserve
printf '660\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
mv "$fixture/sys/class/ubi/ubi0/ubi0_3" "$fixture/sys/class/ubi/ubi0/ubi0_7"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" unexpected_volume_id
mv "$fixture/sys/class/ubi/ubi0/ubi0_7" "$fixture/sys/class/ubi/ubi0/ubi0_3"
printf '1\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/corrupted"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" volume_corrupted_or_unknown
printf '0\n' >"$fixture/sys/class/ubi/ubi0/ubi0_2/corrupted"

# Required nodes, boot merge review, and applet visibility.
rm "$fixture/dev/ubi0"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" missing_ubi_device_node
touch "$fixture/dev/ubi0"
printf '# existing user hook\n' >"$fixture/etc/init.d/rcS.local"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" existing_rcs_local_requires_review
printf '# STANDALONEBASE-SBDATA\n' >"$fixture/etc/init.d/rcS.local"
rm -rf "$fixture/usr/share/jive/applets/SetupAppletInstaller"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" required_applets_missing
mkdir "$fixture/usr/share/jive/applets/SetupAppletInstaller"

# Correct sbdata, incomplete install, and then complete verified state.
d="$fixture/sys/class/ubi/ubi0/ubi0_5"; mkdir "$d"; printf 'sbdata\n' >"$d/name"; printf 'dynamic\n' >"$d/type"; printf '0\n' >"$d/corrupted"; printf '521\n' >"$d/reserved_ebs"; printf '129024\n' >"$d/usable_eb_size"; touch "$fixture/dev/ubi0_5"
printf '100\n' >"$fixture/sys/class/ubi/ubi0/avail_eraseblocks"
out=$(run prepare); echo "$out" | grep -q '^prepare_ready=true$'
rm "$fixture/dev/ubi0_5"
set +e; out=$(run prepare); s=$?; set -e; test "$s" = 2; reason "$out" missing_sbdata_device_node
touch "$fixture/dev/ubi0_5"
out=$(run storage-status); echo "$out" | grep -q '^installation_state=partial$'
set +e; out=$(run verify); s=$?; set -e; test "$s" = 3; echo "$out" | grep -q '^verify_reason=sbdata_not_mounted$'
mkdir -p "$fixture/mnt/sbdata/applets/SetupAppletInstaller" "$fixture/mnt/sbdata/applets/StandaloneBase"
printf 'ubi0:sbdata /mnt/sbdata ubifs rw 0 0\n/mnt/sbdata/applets /usr/share/jive/applets none rw,bind 0 0\n' >>"$fixture/proc/mounts"
set +e; out=$(run verify); s=$?; set -e; test "$s" = 3; echo "$out" | grep -q '^verify_reason=boot_integration_incomplete$'
touch "$fixture/mnt/storage/standalonebase/sbdata-boot.sh"
out=$(run verify); echo "$out" | grep -q '^verified=true$'; echo "$out" | grep -q '^installation_state=complete$'
# Repeated read-only checks are idempotent.
test "$(run verify | sha256sum)" = "$(run verify | sha256sum)"
echo storage-tests-ok
