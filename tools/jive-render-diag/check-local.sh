#!/bin/sh
# Local checks only: does not contact a Radio or claim vendor ABI compatibility.
set -eu
source_root=${1:?usage: check-local.sh /path/to/instrumented/source}
base=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ui="$source_root/src/squeezeplay/src/ui"
output=$(mktemp -d /tmp/jive-render-check.XXXXXX)
echo "Check artifacts: $output"
gcc -std=gnu89 -Wall -Wextra -Werror -I"$ui" \
 "$base/tools/jive-render-diag/test-timing.c" -o "$output/test-host"
for mode in disabled enabled clock-failure; do "$output/test-host" "$mode"; done
arm-linux-gnueabi-gcc -std=gnu89 -march=armv5te -mfloat-abi=soft \
 -Wall -Wextra -Werror -I"$ui" "$base/tools/jive-render-diag/test-timing.c" \
 -lrt -o "$output/test-arm"
for mode in disabled enabled clock-failure; do
 qemu-arm -L /usr/arm-linux-gnueabi "$output/test-arm" "$mode"
done
cd "$source_root"
for unit in jive_render_diag jive_framework jive_surface; do
 arm-linux-gnueabi-gcc -std=gnu89 -march=armv5te -mfloat-abi=soft \
  -D_GNU_SOURCE -include time.h -I"$base/tools/jive-render-diag/compile-headers" \
  -Isrc/SDL-1.2.13/include -Isrc/SDL_image-1.2.5 -Isrc/SDL_ttf-2.0.8 \
  -Isrc/SDL_gfx-2.0.15 -Isrc/lua-5.1.1/src \
  -Isrc/tolua++-1.0.92/include -Isrc/squeezeplay/src \
  -Isrc/squeezeplay/src/ui -c "src/squeezeplay/src/ui/$unit.c" \
  -o "$output/$unit-arm.o"
done
file "$output/"*-arm.o
git diff --check
