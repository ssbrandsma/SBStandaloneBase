#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="${QEMU_ARM:-qemu-arm}"
for binary in sbbase sbwebserver sbproxy sb-storage-helper sb-storage-updater; do
  file "$root/build-arm/$binary"
  readelf -h "$root/build-arm/$binary" | grep -E 'Class:|Machine:|Flags:'
  readelf -l "$root/build-arm/$binary" | grep INTERP && exit 1 || true
  readelf -d "$root/build-arm/$binary" 2>/dev/null | grep NEEDED && exit 1 || true
done
"$runner" -cpu arm926 "$root/build-arm/sbbase" --self-test
"$runner" -cpu arm926 "$root/build-arm/sbbase" --version
catalog=$("$runner" -cpu arm926 "$root/build-arm/sbbase" --check-config "$root/config.json")
echo "$catalog" | grep -q '"count":2'
echo "$catalog" | grep -q '"title": "Standalone Radio"'
echo "$catalog" | grep -q '"title": "Standalone Spotify"'
"$runner" -cpu arm926 "$root/build-arm/sb-storage-helper" info >/dev/null
"$root/tests/storage/test_storage.sh" "$root/build-arm/sb-storage-helper" "$runner -cpu arm926"
"$root/tests/storage/test_updater.sh" "$root/build-arm/sb-storage-updater" "$runner -cpu arm926"
"$root/tests/storage/test_boothelper.sh" "$root/scripts/sbdata-boot.sh"
if test -f "$root/artifacts/ubi/sbdata-w4-r0.img"; then
  "$runner" -cpu arm926 "$root/build-arm/sb-storage-helper" inspect-superblock \
    "$root/artifacts/ubi/sbdata-w4-r0.img" | grep -q '^format=w4/r0$'
fi
set +e
"$runner" -cpu arm926 "$root/build-arm/sb-storage-helper" expand >/dev/null 2>&1
status=$?
set -e
test "$status" = 78
