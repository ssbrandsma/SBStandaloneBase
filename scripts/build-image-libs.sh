#!/bin/sh
# Pinned static raster codecs, shared by native tests and the ARM renderer.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${SB_IMAGE_BUILD_DIR:-$HOME/.cache/sb-image-libs}
mkdir -p "$work"
fetch() {
  name=$1 repo=$2 tag=$3 commit=$4
  if [ ! -d "$work/$name/.git" ]; then
    git clone -q --depth 1 --branch "$tag" "$repo" "$work/$name"
  fi
  test "$(git -C "$work/$name" rev-parse HEAD)" = "$commit"
}
fetch zlib https://github.com/madler/zlib.git v1.3.2 da607da739fa6047df13e66a2af6b8bec7c2a498
fetch png https://github.com/pnggroup/libpng.git v1.6.59 cd952f49f95bb27154ae77dbb103032d95f6e580
fetch jpeg https://github.com/libjpeg-turbo/libjpeg-turbo.git 3.2.0 c85e6b905bf237038faa936dab160ebfc5da0344
for arch in host arm; do
  prefix="$root/build-image-$arch/prefix"
  compiler=$(command -v cc)
  extra=""
  if [ "$arch" = arm ]; then
    toolchain=${ARM_TOOLCHAIN_DIR:-/mnt/c/Projects/SBHttpsProxy/build/toolchains/armv5-eabi--musl--stable-2020.02-2}
    compiler="$toolchain/bin/arm-buildroot-linux-musleabi-gcc"
    extra="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm -DCMAKE_C_FLAGS=-Os"
  fi
  cmake -S "$work/zlib" -B "$work/zlib-$arch" $extra -DCMAKE_C_COMPILER="$compiler" -DCMAKE_INSTALL_PREFIX="$prefix" -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF
  cmake --build "$work/zlib-$arch" -j4
  cmake --install "$work/zlib-$arch"
  cmake -S "$work/png" -B "$work/png-$arch" $extra -DCMAKE_C_COMPILER="$compiler" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=off
  cmake --build "$work/png-$arch" -j4
  cmake --install "$work/png-$arch"
  cmake -S "$work/jpeg" -B "$work/jpeg-$arch" $extra -DCMAKE_C_COMPILER="$compiler" -DCMAKE_INSTALL_PREFIX="$prefix" -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF -DCMAKE_BUILD_TYPE=MinSizeRel
  cmake --build "$work/jpeg-$arch" --target jpeg-static -j4
  mkdir -p "$prefix/include" "$prefix/lib"
  cp "$work/jpeg-$arch/libjpeg.a" "$prefix/lib/"
  cp "$work/jpeg-$arch/jconfig.h" "$work/jpeg/src/jpeglib.h" "$work/jpeg/src/jmorecfg.h" "$prefix/include/"
done
