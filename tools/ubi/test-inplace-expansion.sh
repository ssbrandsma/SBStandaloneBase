#!/bin/sh
# Disposable structural experiments only. Never accepts a device path.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source_image=${1:-/mnt/c/SqueezeboxBackup/ubifs-online.img}
case "$source_image" in /dev/*) echo "refusing device input" >&2; exit 64;; esac
test -f "$source_image"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$root/tools/ubi/patch-ubifs-superblock.py" \
  "$source_image" "$tmp/production-max221.img" 221
"$root/build-host/sb-storage-helper" inspect-superblock "$tmp/production-max221.img"

for target in 82 100 128 139 160 256 400 500 600 638; do
  mkdir "$tmp/root-$target"
  "$root/artifacts/ubi/mkfs.ubifs-20090605-x86_64" \
    -r "$tmp/root-$target" -m 2048 -e 129024 -c "$target" -x lzo \
    -o "$tmp/$target.img"
  printf 'target=%s bytes=%s ' "$target" "$(stat -c %s "$tmp/$target.img")"
  "$root/build-host/sb-storage-helper" inspect-superblock "$tmp/$target.img" |
    grep -E '^(leb_cnt|max_leb_cnt|log_lebs|lpt_lebs|format)=' |
    tr '\n' ' '
  echo
done

python3 "$root/tools/ubi/ubifs-lpt-geometry.py" 82 100 128 139 160 256 400 500 600 638
echo inplace-structural-tests-ok
