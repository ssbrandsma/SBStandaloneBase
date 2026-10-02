#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$root" -B "$root/build-host" -DBUILD_SBPROXY=OFF
cmake --build "$root/build-host"
ctest --test-dir "$root/build-host" -C Debug --output-on-failure
