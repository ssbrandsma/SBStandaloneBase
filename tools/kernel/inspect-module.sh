#!/bin/sh
set -eu

test "$#" -eq 1 || { echo "usage: $0 MODULE.ko" >&2; exit 2; }
module=$1
prefix=${CROSS_COMPILE:-arm-linux-gnueabi-}

"${prefix}readelf" -h "$module"
echo "--- module metadata ---"
"${prefix}strings" "$module" | grep -E '^(vermagic|depends|license|description)=' || true
echo "--- undefined/imported symbols ---"
"${prefix}readelf" -Ws "$module" | awk '$7 == "UND" && $8 != "" { print $8 }' | sort -u
echo "--- modversion CRC entries ---"
"${prefix}readelf" -x __versions "$module" 2>/dev/null || echo "no __versions section"

