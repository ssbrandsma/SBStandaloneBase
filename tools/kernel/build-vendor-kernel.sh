#!/bin/sh
set -eu

test "$#" -eq 2 || {
	echo "usage: $0 LEGACY_TOOLS_DIR KERNEL_WORK_DIR" >&2
	exit 2
}
tools=$1
work=$2
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

. "$tools/env.sh"
"$here/prepare-logitech-kernel.sh" "$work"
kernel="$work/linux-2.6.26"
patch -d "$kernel" -p1 --batch --forward < \
	"$here/patches/0001-modern-perl-timeconst.patch"

yes '' | "$MAKE" -C "$kernel" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" oldconfig
"$MAKE" -C "$kernel" -j2 ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
	vmlinux modules
test -s "$kernel/vmlinux"
test -s "$kernel/System.map"
test -s "$kernel/Module.symvers"
echo "built $kernel ($(wc -l < "$kernel/Module.symvers") symbol CRCs)"

