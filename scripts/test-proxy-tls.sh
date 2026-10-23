#!/bin/sh
# Deterministic tests use host loopback TLS servers, never external stations.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk=${SBHTTPSPROXY_ROOT:-/mnt/c/Projects/SBHttpsProxy}
toolchain=${ARM_TOOLCHAIN_DIR:-$sdk/build/toolchains/armv5-eabi--musl--stable-2020.02-2}
curl_prefix=${ARM_TLS_PREFIX:-$sdk/build/arm/prefix}
wolf_prefix=${ARM_WOLFSSL_PREFIX:-$root/build-proxy-tls/prefix}
cc="$toolchain/bin/arm-buildroot-linux-musleabi-gcc"
out="$root/build-proxy-tls/tests"
test "$(tr -d '\r\n' < "$wolf_prefix/sbproxy-rsa-capacity")" = 4096
mkdir -p "$out"
flags='-static -Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -pthread'
for name in probe diagnostics; do
    if [ "$name" = probe ]; then
        source="$root/tools/sbproxy_tls_probe.c"
    else
        source="$root/tests/tls/test_tls_diagnostics.c"
    fi
    "$cc" $flags -I"$wolf_prefix/include" -I"$curl_prefix/include" "$source" \
        "$curl_prefix/lib/libcurl.a" "$wolf_prefix/lib/libwolfssl.a" -o "$out/$name"
done
qemu-arm -cpu arm926 "$out/diagnostics"
python3 "$root/tests/tls/test_rsa_capacity.py" "$out/probe"
