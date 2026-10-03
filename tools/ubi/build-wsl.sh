#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
src=${MTD_SOURCE:-/tmp/mtd-utils-20090605}
prefix=${UBIFS_DEPS_PREFIX:-/usr}
commit=bb56df1e1a84304ec4a14169e4cdc41116ed4256
rm -rf "$src"
git clone -q https://git.kernel.org/pub/scm/linux/kernel/git/rw/mtd-utils.git "$src"
git -C "$src" checkout -q "$commit"
patch -d "$src" -p1 <"$root/tools/ubi/historical-build.patch"
mkdir -p "$src/out-ubi" "$src/out-mkfs" "$root/artifacts/ubi"
make BUILDDIR="$src/out-ubi" -C "$src/ubi-utils" "$src/out-ubi/libubi.a" CFLAGS="-O2 -std=gnu89 -I$prefix/include"
make BUILDDIR="$src/out-mkfs" -C "$src/mkfs.ubifs" "$src/out-mkfs/mkfs.ubifs" \
  CFLAGS="-O2 -std=gnu89 -I$prefix/include" LDFLAGS="-L$src/out-ubi -L$prefix/lib/x86_64-linux-gnu"
strip "$src/out-mkfs/mkfs.ubifs"
cp "$src/out-mkfs/mkfs.ubifs" "$root/artifacts/ubi/mkfs.ubifs-20090605-x86_64"
sha256sum "$root/artifacts/ubi/mkfs.ubifs-20090605-x86_64"
