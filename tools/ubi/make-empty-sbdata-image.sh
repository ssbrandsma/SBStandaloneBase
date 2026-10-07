#!/bin/sh
set -eu

root=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
mkfs=${MKFS_UBIFS:-$root/artifacts/ubi/mkfs.ubifs-20090605-x86_64}
output=${1:-$root/artifacts/ubi/sbdata-empty.ubifs}
empty=${TMPDIR:-/tmp}/sbdata-empty-root.$$

cleanup() {
    test -n "$empty" && test "$empty" != / && rm -rf "$empty"
}
trap cleanup EXIT HUP INT TERM

test -x "$mkfs" || { echo "missing legacy mkfs.ubifs: $mkfs" >&2; exit 1; }
version=$($mkfs -V 2>&1)
test "$version" = "Version 1.3" || { echo "unexpected mkfs.ubifs version: $version" >&2; exit 1; }

mkdir -p "$empty" "$(dirname "$output")"
chmod 0755 "$empty"
# Stabilize root inode metadata where supported. The historical tool still
# creates a fresh filesystem UUID, so rebuilt images need not be byte-identical.
touch -t 197001010000 "$empty" 2>/dev/null || true

"$mkfs" -r "$empty" -m 2048 -e 129024 -c 521 -x lzo -o "$output"
test "$(wc -c <"$output" | tr -d ' ')" = 1806336
sha256sum "$output"

if test -x "$root/build-host/sb-storage-helper"; then
    "$root/build-host/sb-storage-helper" inspect-superblock "$output"
fi
