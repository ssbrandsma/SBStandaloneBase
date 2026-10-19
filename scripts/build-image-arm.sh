#!/bin/sh
# Targeted build for the image service; does not modify other native services.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
toolchain=${ARM_TOOLCHAIN_DIR:-/mnt/c/Projects/SBHttpsProxy/build/toolchains/armv5-eabi--musl--stable-2020.02-2}
cc="$toolchain/bin/arm-buildroot-linux-musleabi-gcc"
strip="$toolchain/bin/arm-buildroot-linux-musleabi-strip"
prefix=${ARM_IMAGE_PREFIX:-$root/build-image-arm/prefix}
flags="-Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections"
mkdir -p "$root/build-arm"
"$cc" -static $flags -std=c99 -Wall -Wextra -Wl,--gc-sections \
  -I"$root/native/svgpngrenderer/third_party" -I"$prefix/include" \
  "$root/native/svgpngrenderer/main.c" "$root/native/svgpngrenderer/raster.c" \
  "$prefix/lib/libpng16.a" "$prefix/lib/libjpeg.a" "$prefix/lib/libz.a" -lm -o "$root/build-arm/svgpngrenderer"
if [ "${1:-}" = --renderer-only ]; then
  "$strip" --strip-all "$root/build-arm/svgpngrenderer"
  exit 0
fi
"$cc" -static $flags -std=c99 -Wall -Wextra -pthread -Wl,--gc-sections \
  -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DMG_ENABLE_EPOLL=0 -DMG_ENABLE_POLL=1 \
  -DMG_ENABLE_DIRLIST=0 -DMG_ENABLE_SSI=0 -I"$root/native/sbwebserver/src" \
  -I"$root/native/sbwebserver/third_party" "$root/native/sbwebserver/src/"*.c \
  "$root/native/sbwebserver/third_party/mongoose.c" -o "$root/build-arm/sbwebserver"
"$strip" --strip-all "$root/build-arm/svgpngrenderer" "$root/build-arm/sbwebserver"
file "$root/build-arm/svgpngrenderer" "$root/build-arm/sbwebserver"
