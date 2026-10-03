#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="${QEMU_ARM:-qemu-arm}"
for binary in sbbase sbwebserver sbproxy sb-storage-helper; do
  file "$root/build-arm/$binary"
  readelf -h "$root/build-arm/$binary" | grep -E 'Class:|Machine:|Flags:'
  readelf -l "$root/build-arm/$binary" | grep INTERP && exit 1 || true
  readelf -d "$root/build-arm/$binary" 2>/dev/null | grep NEEDED && exit 1 || true
done
"$runner" -cpu arm926 "$root/build-arm/sbbase" --self-test
"$runner" -cpu arm926 "$root/build-arm/sbbase" --version
"$runner" -cpu arm926 "$root/build-arm/sb-storage-helper" info >/dev/null
if test -f "$root/artifacts/ubi/ubifs-test-w4-r0.img"; then
  "$runner" -cpu arm926 "$root/build-arm/sb-storage-helper" inspect-superblock \
    "$root/artifacts/ubi/ubifs-test-w4-r0.img" | grep -q '^format=w4/r0$'
fi
set +e
"$runner" -cpu arm926 "$root/build-arm/sb-storage-helper" expand >/dev/null 2>&1
status=$?
set -e
test "$status" = 78
