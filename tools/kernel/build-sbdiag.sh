#!/bin/sh
set -eu

# This script never contacts or changes a Radio. It expects a fully prepared
# Logitech kernel tree, including the Module.symvers produced by that build.
: "${KERNEL_SRC:?set KERNEL_SRC to the prepared Logitech kernel tree}"
: "${CROSS_COMPILE:=arm-none-linux-gnueabi-}"

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
module_dir="$repo/native/kernel-modules/sbdiag"
out="${OUTPUT_DIR:-$repo/artifacts/kernel-modules}"
build_dir=${BUILD_DIR:-$module_dir}

test -f "$KERNEL_SRC/.config" || { echo "missing $KERNEL_SRC/.config" >&2; exit 2; }
test -f "$KERNEL_SRC/Module.symvers" || {
	echo "missing Module.symvers: CONFIG_MODVERSIONS Radio modules require the exact vendor CRC table" >&2
	exit 2
}
grep -q '^CONFIG_MODVERSIONS=y$' "$KERNEL_SRC/.config" || {
	echo "refusing non-vendor configuration: CONFIG_MODVERSIONS is not enabled" >&2
	exit 2
}

release=$("${MAKE:-make}" -s -C "$KERNEL_SRC" ARCH=arm \
	CROSS_COMPILE="$CROSS_COMPILE" kernelrelease)
test "$release" = 2.6.26.8-rt16 || {
	echo "unexpected kernel release: $release" >&2
	exit 2
}

if test "$build_dir" != "$module_dir"; then
	rm -rf "$build_dir"
	mkdir -p "$build_dir"
	cp "$module_dir/Makefile" "$module_dir/sbdiag.c" "$build_dir/"
fi
"${MAKE:-make}" -C "$KERNEL_SRC" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
	M="$build_dir" clean modules
mkdir -p "$out"
cp "$build_dir/sbdiag.ko" "$out/sbdiag-2.6.26.8-rt16-armv5.ko"
"${CROSS_COMPILE}readelf" -h -r "$out/sbdiag-2.6.26.8-rt16-armv5.ko"
"${CROSS_COMPILE}strings" "$out/sbdiag-2.6.26.8-rt16-armv5.ko" | \
	grep -E '^(vermagic|depends|license)='
sha256sum "$out/sbdiag-2.6.26.8-rt16-armv5.ko" > "$out/SHA256SUMS"
