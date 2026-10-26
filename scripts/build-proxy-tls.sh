#!/bin/sh
# Rebuild wolfSSL's bounded RSA capacity and legacy AES-GCM key exchange.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk=${SBHTTPSPROXY_ROOT:-/mnt/c/Projects/SBHttpsProxy}
toolchain=${ARM_TOOLCHAIN_DIR:-$sdk/build/toolchains/armv5-eabi--musl--stable-2020.02-2}
cmake=${CMAKE:-$sdk/build/toolchains/cmake-3.31.8-linux-x86_64/bin/cmake}
source=${WOLFSSL_SOURCE_DIR:-$sdk/build/vendor-arm/wolfssl-5.8.2-stable}
build=${SB_TLS_BUILD_DIR:-$root/build-proxy-tls/wolfssl}
prefix=${ARM_WOLFSSL_PREFIX:-$root/build-proxy-tls/prefix}
cc="$toolchain/bin/arm-buildroot-linux-musleabi-gcc"
test -x "$cc"
test -x "$cmake"
grep -q 'LIBWOLFSSL_VERSION_STRING "5.8.2"' "$source/wolfssl/version.h"
flags='-Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections -DWOLFSSL_NO_ASN_STRICT -DSP_INT_BITS=4096 -DWOLFSSL_STATIC_RSA'
"$cmake" -S "$source" -B "$build" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_C_FLAGS="$flags" -DBUILD_SHARED_LIBS=OFF \
    -DWOLFSSL_ALT_CERT_CHAINS=ON -DWOLFSSL_CURL=ON -DWOLFSSL_TLS13=ON \
    -DWOLFSSL_DTLS=OFF -DWOLFSSL_EXAMPLES=OFF -DWOLFSSL_CRYPT_TESTS=OFF
"$cmake" --build "$build" -j2
"$cmake" --install "$build"
# CMake's generated options.h omits CFLAGS defines. Export the capacity as
# part of the installed SDK so consumers see the same integer/RSA layouts.
"$cmake" -DSB_TLS_PREFIX="$prefix" -P "$root/scripts/export-proxy-tls.cmake"
