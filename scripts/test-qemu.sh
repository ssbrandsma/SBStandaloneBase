#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
qemu-arm "$root/build-arm/sbbase" --help >/dev/null 2>&1 || rc=$?
test "${rc:-0}" -eq 0 -o "${rc:-0}" -eq 1
file "$root/build-arm/sbbase"
readelf -h "$root/build-arm/sbbase"
