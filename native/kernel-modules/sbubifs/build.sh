#!/bin/sh
set -eu

: "${KERNEL_SRC:?set KERNEL_SRC to the fully built Logitech kernel tree}"
: "${CROSS_COMPILE:=arm-none-linux-gnueabi-}"
: "${MAKE:=make}"

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${BUILD_DIR:-"${TMPDIR:-/tmp}/sbubifs-build"}
out=${OUTPUT_DIR:-"$here/out"}
src="$work/src"

test -f "$KERNEL_SRC/Module.symvers" || {
	echo "missing exact vendor Module.symvers" >&2
	exit 2
}
grep -q '^CONFIG_UBIFS_FS=y$' "$KERNEL_SRC/.config"
grep -q '^CONFIG_MODVERSIONS=y$' "$KERNEL_SRC/.config"
grep -q '^# CONFIG_UBIFS_FS_DEBUG is not set$' "$KERNEL_SRC/.config"
grep -q '^# CONFIG_UBIFS_FS_XATTR is not set$' "$KERNEL_SRC/.config"

rm -rf "$src"
mkdir -p "$src" "$out"
cp "$KERNEL_SRC"/fs/ubifs/*.[ch] "$src/"
cp "$here/Makefile" "$src/Makefile"

while IFS= read -r patch_name; do
	case "$patch_name" in ''|'#'*) continue ;; esac
	patch -d "$src" -p1 --batch --forward < "$here/patches/$patch_name"
done < "$here/patches/series"

"$MAKE" -C "$KERNEL_SRC" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
	M="$src" clean modules
cp "$src/sbubifs.ko" "$out/sbubifs.ko"
"${CROSS_COMPILE}readelf" -h -r "$out/sbubifs.ko" > "$out/readelf.txt"
"${CROSS_COMPILE}nm" -n "$out/sbubifs.ko" > "$out/defined-and-undefined.txt"
"${CROSS_COMPILE}strings" "$out/sbubifs.ko" | \
	grep -E '^(vermagic|depends|license|description)=' > "$out/metadata.txt"
sha256sum "$out/sbubifs.ko" > "$out/SHA256SUMS"
echo "built $out/sbubifs.ko"

