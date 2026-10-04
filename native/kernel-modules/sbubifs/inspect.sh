#!/bin/sh
set -eu

module=${1:-"$(dirname -- "$0")/out/sbubifs.ko"}
prefix=${CROSS_COMPILE:-arm-none-linux-gnueabi-}
out=${2:-"$(dirname -- "$0")/out/symbol-report.txt"}

{
	echo "MODULE $module"
	"${prefix}readelf" -h "$module"
	echo "METADATA"
	"${prefix}strings" "$module" | grep -E '^(vermagic|depends|license|description)=' || true
	echo "DEFINED GLOBAL SYMBOLS"
	"${prefix}nm" -g --defined-only "$module" | sort
	echo "UNDEFINED SYMBOLS"
	"${prefix}nm" -u "$module" | sort
	echo "EXPORTED SYMBOLS (__ksymtab)"
	"${prefix}readelf" -S "$module" | grep __ksymtab || echo "none"
	echo "REGISTRATION STRINGS"
	"${prefix}strings" "$module" | grep -E 'sbubifs|ubifs_inode_slab|ubifs_bgt' | sort -u
} > "$out"
cat "$out"

