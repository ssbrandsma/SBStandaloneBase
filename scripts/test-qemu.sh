#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="${QEMU_ARM:-qemu-arm}"
for binary in sbbase sbwebserver sbproxy; do
  file "$root/build-arm/$binary"
  readelf -h "$root/build-arm/$binary" | grep -E 'Class:|Machine:|Flags:'
  readelf -l "$root/build-arm/$binary" | grep INTERP && exit 1 || true
  readelf -d "$root/build-arm/$binary" 2>/dev/null | grep NEEDED && exit 1 || true
done
"$runner" -cpu arm926 "$root/build-arm/sbbase" --self-test
"$runner" -cpu arm926 "$root/build-arm/sbbase" --version
