#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
p="$root/build-image-host/prefix"
gcc -O2 -std=c99 -Wall -Wextra -I native/svgpngrenderer/third_party -I "$p/include" \
  native/svgpngrenderer/main.c native/svgpngrenderer/raster.c \
  "$p/lib/libpng16.a" "$p/lib/libjpeg.a" "$p/lib/libz.a" -lm -o build-image-host/svgpngrenderer
if [ "${1:-}" = --renderer-only ]; then exit 0; fi
gcc -O2 -std=c99 -Wall -Wextra -pthread -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
  -DMG_ENABLE_EPOLL=0 -DMG_ENABLE_POLL=1 -DMG_ENABLE_DIRLIST=0 -DMG_ENABLE_SSI=0 \
  -DSB_IMAGE_TESTING -I native/sbwebserver/src -I native/sbwebserver/third_party -I "$p/include" \
  native/sbwebserver/src/*.c native/sbwebserver/third_party/mongoose.c "$p/lib/libz.a" -o build-image-host/sbwebserver-test
