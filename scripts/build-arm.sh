#!/bin/sh
set -eu
: "${ARM_TOOLCHAIN_DIR:?Set ARM_TOOLCHAIN_DIR to the extracted Bootlin ARMv5 toolchain}"
cc="$ARM_TOOLCHAIN_DIR/bin/arm-buildroot-linux-musleabi-gcc"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/build-arm"
mkdir -p "$out"
"$cc" -Os -std=c99 -Wall -Wextra -Wpedantic -static -I"$root/native/sbbase/include" \
  "$root/native/sbbase/src/main.c" "$root/native/sbbase/src/protocol.c" -o "$out/sbbase"
"$cc" -Os -std=c99 -pthread -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
  -DMG_ENABLE_EPOLL=0 -DMG_ENABLE_POLL=1 -DMG_ENABLE_DIRLIST=0 -DMG_ENABLE_SSI=0 \
  -I"$root/native/sbwebserver/src" -I"$root/native/sbwebserver/third_party" \
  "$root/native/sbwebserver/src/"*.c "$root/native/sbwebserver/third_party/mongoose.c" \
  -static -o "$out/sbwebserver"
file "$out/sbbase" "$out/sbwebserver"
echo "Build sbproxy with the static ARM libcurl/wolfSSL bundle from the supplied SBHttpsProxy arm scripts."
