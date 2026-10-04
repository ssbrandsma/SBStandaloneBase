# SBUBIFS feasibility and offline validation

## Verdict

**CONDITIONAL.** Logitech's exact UBIFS implementation builds as an independent
`sbubifs.ko`. It has a separate filesystem identity, unique named resources,
only exported kernel dependencies, no exports of its own, and the unchanged
UBIFS on-media format. Important live-kernel tests remain, so compilation alone
is not classified as GO.

Nothing was deployed. No module was loaded, no volume was mounted, no NAND was
written, and neither Radio was rebooted. `192.168.1.141` was not contacted.

## Reproducible build environment

| Input | Verified identity |
|---|---|
| SqueezeOS | commit `bad080aecfec8226a4c1699b29d32cbba4ba396b` |
| kernel | Linux 2.6.26 plus 2.6.26.8, RT16, Freescale and Logitech patches |
| configuration | matches live `/proc/config.gz` except its timestamp |
| compiler | CodeSourcery 2010q1-202, GCC 4.4.1 |
| compiler archive | 82,460,512 bytes, SHA-256 `60791a1f...42a1d8` |
| make | GNU make 3.81, SHA-256 `16b77de9...215a2` |

The [Logitech kernel recipe](https://github.com/LMS-Community/squeezeos/blob/bad080aecfec8226a4c1699b29d32cbba4ba396b/poky/meta-squeezeos/packages/linux/linux-imx25_svn.bb)
and [toolchain selection](https://github.com/LMS-Community/squeezeos/blob/bad080aecfec8226a4c1699b29d32cbba4ba396b/poky/meta-squeezeos/conf/distro/include/poky-external-csl2010q1.inc)
are pinned. The toolchain is fetched from Buildroot's historical source mirror
and checksum-verified.

The compiler is a 32-bit Linux program. The staging script extracts, but does
not install, Ubuntu's i386 runtime. GNU make 3.81 is built locally. A one-line
host compatibility patch replaces Perl's removed `defined(@array)` syntax in
`kernel/timeconst.pl`; it does not change target-kernel behavior.

```sh
cd /mnt/c/Projects/SBStandaloneBase/StandaloneBase
sh tools/kernel/stage-legacy-toolchain.sh /tmp/sb-legacy-tools
sh tools/kernel/build-vendor-kernel.sh \
  /tmp/sb-legacy-tools /tmp/sb-vendor-kernel
. /tmp/sb-legacy-tools/env.sh

KERNEL_SRC=/tmp/sb-vendor-kernel/linux-2.6.26 \
BUILD_DIR=/tmp/sbdiag-build OUTPUT_DIR=/tmp/sbdiag-out \
sh tools/kernel/build-sbdiag.sh

KERNEL_SRC=/tmp/sb-vendor-kernel/linux-2.6.26 \
BUILD_DIR=/tmp/sbubifs-build OUTPUT_DIR=/tmp/sbubifs-out \
sh native/kernel-modules/sbubifs/build.sh
```

WSL-native build/output directories are required because the legacy 32-bit
compiler cannot stat DrvFS's large inode numbers.

The complete kernel build succeeded and generated a 4.0 MiB `vmlinux`,
`System.map`, and a 3,031-entry `Module.symvers`. A full build is required:
`modules_prepare` does not generate the necessary modversion CRC table.

`sbdiag.ko` built with exact vermagic. Its `printk` and `struct_module` CRCs
match Logitech's installed `ar6000.ko`. SBUBIFS has 149 versioned imports; 29
overlap with `ar6000.ko`, all 29 match, and none mismatch. Every SBUBIFS import
also matches reconstructed `Module.symvers`. This is strong ABI evidence, but
only insertion into the running kernel proves loader acceptance.

## Source changes

The build copies vendor `fs/ubifs/*.[ch]` into a disposable directory and
applies one small patch. Every C/header file except `super.c` and `ubifs.h`
remains byte-identical. `ubifs.h` changes only the background-thread name;
`super.c` contains identity, registration and allowlist changes.

The complete original implementation remains recognizable: superblock and
inode lifecycle, journal, recovery, TNC, LPT, budgeting, garbage collection,
orphans, UBI KAPI I/O, compression, commit thread and shrinker. The build
refuses configurations enabling UBIFS debug or xattrs because the target has
both disabled and those profiles were not reviewed.

## Registration conflicts

| Resource | Built-in UBIFS | SBUBIFS |
|---|---|---|
| filesystem | `ubifs` | `sbubifs` |
| BDI | `ubifs` | `sbubifs_<ubi>_<volume>` |
| inode slab | `ubifs_inode_slab` | `sbubifs_inode_slab` |
| background thread | `ubifs_bgt...` | `sbubifs_bgt...` |
| shrinker | module-private registration | separate module-private registration |
| mount list and lock | built-in storage | module storage |
| debugfs | disabled | not compiled |
| proc/sys/notifiers/workqueues | none | none |

Compression initialization allocates existing crypto transformations and does
not register duplicate algorithms. Failure paths unwind compressor, shrinker,
slab and filesystem registration in reverse order.

## Symbol report

The unstripped module is 198,944 bytes:

```text
text 149,787; data 628; bss 48
loadable sections reported by size -A: 151,626 bytes
relocations: 589 R_ARM_ABS32 and 1,779 R_ARM_CALL
```

| Check | Result |
|---|---:|
| global defined symbols | 175 |
| names also in built-in UBIFS/System.map | 172 |
| module-only definitions | `init_module`, `cleanup_module`, `__this_module` |
| undefined ELF symbols | 148 |
| versioned imports | 149 |
| exported module symbols | 0 |
| unresolved after modpost | 0 |
| vendor-module CRC overlap/mismatch | 29 / 0 |

The 172 duplicate implementation names are harmless: none appears in
`__ksymtab`, so the loader does not publish them or replace built-in functions.
Blindly renaming all functions would add divergence without resolving a kernel
namespace conflict.

Notable imports include VFS registration, BDI, slab, shrinker, kthread, crypto,
and the complete required UBI KAPI. All are exported with matching CRCs. UBI
interfaces are GPL-only exports and the module declares GPL licensing.
`inspect.sh` generates the complete defined/undefined/export report for each
build rather than checking generated output into Git.

## UBI volume isolation

The module resolves every supported mount-source syntax through vendor
`open_ubi()`, then requires all of the following before continuing:

```text
ubi_num == 0
vol_id == 5
name == "sbdata" with exact length
type == UBI_DYNAMIC_VOLUME
corrupted == 0
update marker == 0
```

It validates again immediately before opening read-write. Consequently
`ubi0:ubifs`, `ubi0_2`, renamed/recreated volumes, static volumes and damaged or
interrupted volumes are rejected. The fixed ID is deliberately conservative;
another verified layout requires another explicit compatibility profile.

VFS duplicate-mount detection is scoped by filesystem type and cannot alone
prevent cross-driver access. UBI provides a second guard: its read-write open
returns `-EBUSY` when `vol->writers > 0`. A volume already mounted by either
driver therefore rejects the other's writer. The explicit allowlist is still
essential because writer exclusion would not prevent SBUBIFS from becoming the
first writer of production storage.

## On-media compatibility

`ubifs-media.h`, magic values, node structures, CRCs, keys, superblock, master,
journal, LPT, compression identifiers and compatibility rules are unchanged.
Filesystem, BDI, slab and thread names are not serialized. The existing
historical `w4/r0` image is therefore theoretically mountable without
conversion or reformatting. A physical mount has not proven this yet.

## Lifecycle and memory

Load order is filesystem registration, unique slab creation, shrinker
registration and compressor initialization. Partial failures unwind completed
steps. The filesystem has `.owner = THIS_MODULE`, so a mounted superblock holds
a module reference and normal `rmmod` must fail busy. Clean unload releases
compressors, shrinker, slab and filesystem registration and asserts the
module-owned mount list is empty.

The module itself needs about 150 KiB loadable text/data/BSS. A mount adds the
UBIFS instance, one kernel thread/stack, inode objects, write buffers, journal,
LPT/TNC and compressor state. A preliminary light-use estimate is 0.5-2 MiB,
with higher transient use during replay, commit, GC and metadata-heavy work.
This is not a measured upper bound; 64 MiB hardware testing is mandatory.

## Offline test results

Passed:

- exact compiler and complete vendor-kernel build;
- diagnostic-module build and vendor CRC comparison;
- SBUBIFS compilation and modpost;
- ELF32 ARM EABI5, ARMv5 vermagic and relocation inspection;
- all imports and CRCs against `Module.symvers`;
- 29 shared CRCs against installed `ar6000.ko`;
- zero exports and unique registration strings;
- resolved-volume allowlist source checks;
- byte equality of every untouched UBIFS source/header.

```text
PASS: offline SBUBIFS checks; size=198944, imports=149,
vendor_crc_overlap=29
```

Not run: insertion, live filesystem registration, mounting, I/O, dual mounts,
unload behavior, corruption/out-of-space/power-cut cases and memory pressure.
Standard QEMU has no faithful i.MX25 `baby` board and NAND/UBI topology; a
modern ARM kernel would also have the wrong ABI and UBIFS implementation.

## Integration feasibility

If physical validation succeeds, StandaloneBase can verify kernel/config/UBI
identity, hash-check and load the module, mount only `sbdata`, validate storage,
migrate applets, then activate a bind over `/usr/share/jive/applets`. A minimal
bootstrap must remain on production storage because applet discovery precedes
StandaloneBase initialization. Restarting SqueezePlay is required for reliable
discovery; a reboot is not inherently required.

Every failure must leave the original applet directory visible. Mount failure
must never cause formatting, repair, reboot or a retry loop. No integration or
startup hook is implemented in this milestone.

## Licensing

SBUBIFS is derived from GPL-2.0 UBIFS and uses GPL-only UBI exports. Binary
distribution requires GPLv2 compliance and complete corresponding source: the
pinned kernel, Logitech/Freescale/RT patches, config, SBUBIFS patch and build
scripts. The CodeSourcery archive has separate redistribution terms and should
be fetched rather than bundled until those terms are reviewed.

## Next physical experiment

Separate authorization is required. First load and immediately unload only
`sbdiag.ko` from `/tmp` while COM5 is interactive, checking hash, modules,
taint and `dmesg`. Stop on any warning or rejection.

Only after that succeeds should another authorization allow loading
`sbubifs.ko` **without mounting anything**, checking `/proc/filesystems`, named
resources, taint and clean unload. A `sbdata` mount is a third, separately
reviewed experiment with backup and recovery prerequisites.

## Direct answers

1. Exact independent build: **yes**.
2. Registers as `sbubifs`: **compiled and statically verified; not run**.
3. Avoids BDI collision: **yes**, by per-volume `sbubifs` name.
4. Required exports: **all available; zero unresolved**.
5. CRC compatibility: **complete reconstructed table; 29/29 vendor checks**.
6. Driver coexistence: **no static blocker; live proof pending**.
7. Restricted to `sbdata`: **yes**, using resolved identity and health.
8. Existing image without reformat: **format-compatible in source; mount pending**.
9. Memory: **about 150 KiB module plus estimated 0.5-2 MiB light mount use**.
10. Safe unload: **correct by lifecycle inspection; live proof pending**.
11. Distribution readiness: **not yet; runtime and recovery tests block it**.
12. Next test: **authorized RAM-only `sbdiag.ko` load/unload under COM5**.

**Final verdict: CONDITIONAL.**

