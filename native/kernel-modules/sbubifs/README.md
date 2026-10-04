# SBUBIFS offline prototype

This directory builds Logitech's patched UBIFS implementation as a second,
GPL-2.0 kernel module named `sbubifs.ko`. Vendor sources are copied from the
prepared SqueezeOS kernel tree at build time so that the small identity and
safety patch remains reviewable.

The prototype registers filesystem type `sbubifs`, uses unique BDI, slab-cache
and background-thread names, and accepts only the resolved dynamic UBI volume
`ubi0_5` named exactly `sbdata`. It does not alter UBIFS on-media structures.

It is an offline research artifact. Do not load it on a Radio without separate
authorization. In particular, do not mount production `ubi0:ubifs` through it.

Build after completing the exact vendor kernel build:

```sh
KERNEL_SRC=/path/to/linux-2.6.26 \
CROSS_COMPILE=/path/to/arm-none-linux-gnueabi- \
MAKE=/path/to/make-3.81 \
BUILD_DIR=/native-linux-filesystem/path/sbubifs-build \
OUTPUT_DIR=/native-linux-filesystem/path/output \
sh build.sh
```

The historical compiler cannot stat source files on WSL's `/mnt/c` filesystem,
so both build and output directories should be on WSL's native filesystem.

UBIFS is GPL-2.0-derived code. Redistributing the binary requires compliance
with GPLv2, including providing the complete corresponding source: the pinned
upstream kernel tarball, Logitech patch series, SBUBIFS patch, configuration,
build scripts and toolchain information.

