#!/bin/sh
set -eu

# DESTRUCTIVE TO ubifs_test. Never run without separate approval.
test "${SB_ALLOW_UBI_MUTATION:-}" = isolated-test-only || {
    echo "Set SB_ALLOW_UBI_MUTATION=isolated-test-only after approval" >&2; exit 77;
}
image=${1:?path to reviewed UBIFS image required}
helper=${2:?path to sb-storage-helper required}
target_bytes=${3:-8388608}
test -f "$image" || { echo "image is not a regular file" >&2; exit 66; }
test -x "$helper" || { echo "helper is not executable" >&2; exit 66; }
sys=/sys/class/ubi/ubi0
found=
for name_file in "$sys"/ubi0_*/name; do
    test -f "$name_file" || continue
    test "$(cat "$name_file")" = ubifs_test || continue
    test -z "$found" || { echo "ambiguous ubifs_test volume" >&2; exit 78; }
    found=$(dirname "$name_file")
done
test -n "$found" || { echo "ubifs_test not found" >&2; exit 78; }
test "$(cat "$found/type")" = dynamic || { echo "test volume is not dynamic" >&2; exit 78; }
test "$(cat "$found/corrupted")" = 0 || { echo "test volume corruption marker set" >&2; exit 78; }
id=${found##*_}
device=/dev/ubi0_$id
test -c "$device" || { echo "$device is not a character device" >&2; exit 78; }
grep -q "ubi0:ubifs_test " /proc/mounts && { echo "ubifs_test already mounted" >&2; exit 78; }
case "$id" in 0|1|2|3|4) echo "refusing protected volume ID $id" >&2; exit 78;; esac

before=$(cat "$found/reserved_ebs")
mkdir -p /mnt/ubifs-test
mounted=false
cleanup() {
    test "$mounted" = false || umount /mnt/ubifs-test || true
}
trap cleanup EXIT HUP INT TERM
ubiupdatevol "$device" "$image"
mount -t ubifs ubi0:ubifs_test /mnt/ubifs-test
mounted=true
before_blocks=$(df -k /mnt/ubifs-test | awk 'NR==2 { print $2 }')
printf 'StandaloneBase isolated growth validation\n' >/mnt/ubifs-test/sentinel.txt
sha256sum /mnt/ubifs-test/sentinel.txt >/mnt/ubifs-test/sentinel.sha256
sync
umount /mnt/ubifs-test
mounted=false
"$helper" resize-test-volume "$target_bytes" I_ACCEPT_TEST_VOLUME_FLASH_WRITE
mount -t ubifs ubi0:ubifs_test /mnt/ubifs-test
mounted=true
sha256sum -c /mnt/ubifs-test/sentinel.sha256
after=$(cat "$found/reserved_ebs")
after_blocks=$(df -k /mnt/ubifs-test | awk 'NR==2 { print $2 }')
test "$after" -gt "$before" || { echo "UBI reserved LEB count did not grow" >&2; exit 1; }
test "$after_blocks" -gt "$before_blocks" || { echo "UBIFS capacity did not grow" >&2; exit 1; }
df -k /mnt/ubifs-test
"$helper" inspect-superblock "$device"
umount /mnt/ubifs-test
mounted=false
trap - EXIT HUP INT TERM
printf 'before_lebs=%s\nafter_lebs=%s\nbefore_blocks=%s\nafter_blocks=%s\n' \
    "$before" "$after" "$before_blocks" "$after_blocks"
