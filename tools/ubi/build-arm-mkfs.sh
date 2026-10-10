#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${UBIFS_ARM_WORK:-/tmp/sb-mkfs-arm}
deps="$work/deps"
prefix="$work/prefix"
source="$work/mtd-utils"
toolchain=${ARM_TOOLCHAIN_DIR:-/mnt/c/Projects/SBHttpsProxy/build/toolchains/armv5-eabi--musl--stable-2020.02-2}
host=arm-buildroot-linux-musleabi
cc="$toolchain/bin/$host-gcc"
ar="$toolchain/bin/$host-ar"
ranlib="$toolchain/bin/$host-ranlib"
strip="$toolchain/bin/$host-strip"
flags="-Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections"

fetch() {
    file=$1 url=$2 checksum=$3
    test -f "$deps/$file" || curl -fL "$url" -o "$deps/$file"
    printf '%s  %s\n' "$checksum" "$deps/$file" | sha256sum -c -
}

test -x "$cc" || { echo "missing compiler: $cc" >&2; exit 1; }
rm -rf "$prefix" "$source" "$work/zlib-1.2.11" "$work/lzo-2.10" "$work/util-linux-2.34"
mkdir -p "$deps" "$prefix" "$root/artifacts/ubi"

fetch zlib-1.2.11.tar.gz https://zlib.net/fossils/zlib-1.2.11.tar.gz c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1
fetch lzo-2.10.tar.gz https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz c0f892943208266f9b6543b3ae308fab6284c5c90e627931446fb49b4221a072
fetch util-linux-2.34.tar.xz https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.34/util-linux-2.34.tar.xz 743f9d0c7252b6db246b659c1e1ce0bd45d8d4508b4dfa427bbb4a3e9b9f62b5

tar -C "$work" -xf "$deps/zlib-1.2.11.tar.gz"
(cd "$work/zlib-1.2.11" && CC="$cc" AR="$ar" RANLIB="$ranlib" CFLAGS="$flags" ./configure --static --prefix="$prefix" && make -j2 && make install)

tar -C "$work" -xf "$deps/lzo-2.10.tar.gz"
(cd "$work/lzo-2.10" && ./configure --host="$host" --prefix="$prefix" --enable-static --disable-shared CC="$cc" CFLAGS="$flags" && make -j2 && make install)

tar -C "$work" -xf "$deps/util-linux-2.34.tar.xz"
(cd "$work/util-linux-2.34" && ./configure --host="$host" --prefix="$prefix" --disable-all-programs --enable-libuuid --disable-shared --enable-static CC="$cc" CFLAGS="$flags" && make -j2 libuuid.la)
mkdir -p "$prefix/include/uuid" "$prefix/lib"
cp "$work/util-linux-2.34/libuuid/src/uuid.h" "$prefix/include/uuid/uuid.h"
cp "$work/util-linux-2.34/.libs/libuuid.a" "$prefix/lib/libuuid.a"

git clone -q https://git.kernel.org/pub/scm/linux/kernel/git/rw/mtd-utils.git "$source"
git -C "$source" checkout -q bb56df1e1a84304ec4a14169e4cdc41116ed4256
patch -d "$source" -p1 <"$root/tools/ubi/historical-build.patch"
mkdir -p "$source/out-ubi" "$source/out-mkfs"
make BUILDDIR="$source/out-ubi" -C "$source/ubi-utils" "$source/out-ubi/libubi.a" CC="$cc" AR="$ar" CFLAGS="$flags -std=gnu89 -I$prefix/include"
make BUILDDIR="$source/out-mkfs" -C "$source/mkfs.ubifs" "$source/out-mkfs/mkfs.ubifs" \
    CC="$cc" AR="$ar" CFLAGS="$flags -std=gnu89 -I$prefix/include" \
    LDFLAGS="-static -Wl,--gc-sections -L$source/out-ubi -L$prefix/lib" LDLIBS="-lz -llzo2 -lm -luuid -lubi"
"$strip" --strip-all "$source/out-mkfs/mkfs.ubifs"
cp "$source/out-mkfs/mkfs.ubifs" "$root/artifacts/ubi/mkfs.ubifs-20090605-armv5-static"
file "$root/artifacts/ubi/mkfs.ubifs-20090605-armv5-static"
readelf -h -l -d "$root/artifacts/ubi/mkfs.ubifs-20090605-armv5-static"
sha256sum "$root/artifacts/ubi/mkfs.ubifs-20090605-armv5-static"
