#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Reuse the hardware-validated SDK and static TLS libraries from SBHttpsProxy.
# Override SBHTTPSPROXY_ROOT when that project is not beside SBStandaloneBase.
if [ -z "${SBHTTPSPROXY_ROOT:-}" ]; then
  if [ -d /mnt/c/Projects/SBHttpsProxy ]; then
    SBHTTPSPROXY_ROOT=/mnt/c/Projects/SBHttpsProxy
  else
    SBHTTPSPROXY_ROOT="$root/../../SBHttpsProxy"
  fi
fi
toolchain="${ARM_TOOLCHAIN_DIR:-$SBHTTPSPROXY_ROOT/build/toolchains/armv5-eabi--musl--stable-2020.02-2}"
target=arm-buildroot-linux-musleabi
cc="$toolchain/bin/$target-gcc"
strip="$toolchain/bin/$target-strip"
size="$toolchain/bin/$target-size"
tls_prefix="${ARM_TLS_PREFIX:-$SBHTTPSPROXY_ROOT/build/arm/prefix}"
out="$root/build-arm"
flags="-Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections"

test -x "$cc" || { echo "Missing ARM compiler: $cc" >&2; exit 1; }
test -f "$tls_prefix/lib/libcurl.a" || { echo "Missing static libcurl in $tls_prefix" >&2; exit 1; }
test -f "$tls_prefix/lib/libwolfssl.a" || { echo "Missing static wolfSSL in $tls_prefix" >&2; exit 1; }
mkdir -p "$out"

"$cc" -static $flags -std=c99 -Wall -Wextra -Wpedantic -Wl,--gc-sections \
  -I"$root/native/sbbase/include" "$root/native/sbbase/src/main.c" \
  "$root/native/sbbase/src/protocol.c" -o "$out/sbbase"

"$cc" -static $flags -std=c99 -Wall -Wextra -pthread -Wl,--gc-sections \
  -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DMG_ENABLE_EPOLL=0 -DMG_ENABLE_POLL=1 \
  -DMG_ENABLE_DIRLIST=0 -DMG_ENABLE_SSI=0 -I"$root/native/sbwebserver/src" \
  -I"$root/native/sbwebserver/third_party" "$root/native/sbwebserver/src/"*.c \
  "$root/native/sbwebserver/third_party/mongoose.c" -o "$out/sbwebserver"

"$cc" -static $flags -std=c99 -Wall -Wextra -Wpedantic -pthread -Wl,--gc-sections \
  -D_GNU_SOURCE -I"$tls_prefix/include" "$root/native/sbproxy/sbproxy.c" \
  "$tls_prefix/lib/libcurl.a" "$tls_prefix/lib/libwolfssl.a" -o "$out/sbproxy"

"$cc" -static $flags -std=c99 -Wall -Wextra -Wpedantic -Wl,--gc-sections \
  "$root/native/sbstorage/sb_storage_helper.c" -o "$out/sb-storage-helper"

"$strip" --strip-all "$out/sbbase" "$out/sbwebserver" "$out/sbproxy" "$out/sb-storage-helper"
file "$out/sbbase" "$out/sbwebserver" "$out/sbproxy" "$out/sb-storage-helper"
"$size" "$out/sbbase" "$out/sbwebserver" "$out/sbproxy" "$out/sb-storage-helper"
for binary in sbbase sbwebserver sbproxy sb-storage-helper; do
  if readelf -l "$out/$binary" | grep -q INTERP; then echo "$binary unexpectedly has an ELF interpreter" >&2; exit 1; fi
  if readelf -d "$out/$binary" 2>/dev/null | grep -q NEEDED; then echo "$binary unexpectedly has shared dependencies" >&2; exit 1; fi
done
