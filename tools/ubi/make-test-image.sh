#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkfs=${MKFS_UBIFS:-$root/artifacts/ubi/mkfs.ubifs-20090605-x86_64}
out=${1:-$root/artifacts/ubi/ubifs-test-w4-r0.img}
mkdir -p "$(dirname "$out")"
"$mkfs" -r "$root/tools/ubi/test-root" -m 2048 -e 129024 -c 521 -x lzo -o "$out"
sha256sum "$out"
"$root/build-host/sb-storage-helper" inspect-superblock "$out" 2>/dev/null || true
