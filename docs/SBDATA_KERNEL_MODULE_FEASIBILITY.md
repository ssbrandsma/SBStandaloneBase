# `sbdata` kernel-module feasibility

## Verdict

**CONDITIONAL.** The stock Radio demonstrably supports ARM kernel modules, and
the exact public Logitech kernel source/configuration has now been identified.
The smallest credible storage route is a **JFFS2 module mounted on the existing
`gluebi` MTD view of `sbdata`**. This avoids the UBIFS defect and avoids a block
translation layer. It is not yet a production solution: the module must first
be reproduced with matching modversion CRCs, then loaded harmlessly, and JFFS2
must be tested on disposable storage for RAM, mount-time, power-loss and UBI
layering behavior.

A module cannot safely repair or replace the built-in UBIFS implementation.
Runtime instruction patching is the only plausible way to alter its fixed BDI
name without a new kernel, and that is explicitly unsuitable for distribution.

This milestone made no persistent change. No module was loaded; no NAND, UBI
volume, mount, applet directory, startup hook, firmware or boot state was
changed. `192.168.1.141` was not contacted.

## Physical read-only findings (192.168.1.222)

Observed on 2026-10-04 through host-key-verified SSH:

```text
Linux SqueezeboxRadio 2.6.26.8-rt16 #1 PREEMPT RT ... armv5tejl
gcc version 4.4.1 (Sourcery G++ Lite 2010q1-202)
/proc/modules: ar6000 142684 0 - Live 0xbf000000
/proc/sys/kernel/tainted: 0
```

The running `/proc/config.gz` says:

```text
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_MODVERSIONS=y
CONFIG_KALLSYMS=y
CONFIG_CPU_ARM926T=y
CONFIG_AEABI=y
CONFIG_PREEMPT_RT=y
CONFIG_MTD_BLOCK=y
CONFIG_MTD_UBI_GLUEBI=y
CONFIG_UBIFS_FS=y
```

`CONFIG_KPROBES` and `CONFIG_SECURITY` are disabled. There is no module-signing
facility in this kernel generation/configuration. BusyBox supplies `insmod`,
`rmmod` and `modprobe`; `/proc/modules`, `/sys/module` and `/lib/modules` exist.
The only installed `.ko` is `/lib/atheros/ar6000.ko`.

The installed wireless module is an unstripped ARM EABI5 relocatable object.
Its metadata is:

```text
vermagic=2.6.26.8-rt16 preempt mod_unload modversions ARMv5
depends=
license=GPL and additional rights
relocations: R_ARM_ABS32 and R_ARM_CALL
```

This proves that the loader handles the normal ARMv5 relocation set. Root has
the ordinary module-loading tools and there is no observed policy restriction.
It does **not** prove that an independently built module will load.

### ABI requirements

`CONFIG_MODVERSIONS=y` is decisive. A candidate needs all of the following:

1. the exact patched vendor source and configuration;
2. `ARCH=arm`, ARM EABI5 and ARMv5-compatible code generation;
3. release/vermagic `2.6.26.8-rt16 preempt mod_unload modversions ARMv5`;
4. the exact CRC for every imported symbol in its `__versions` section;
5. compatible structure layout and compiler assumptions from the vendor tree;
6. only symbols exported by that kernel/configuration.

Changing vermagic or forcing `insmod` cannot repair CRC or layout differences.
The live config was compared with the config produced by Logitech's patch
series: the only difference was the generated timestamp. That is strong source
provenance, but a candidate still has to be compared with the installed module
and tested on hardware.

## Reconstructed vendor build environment

The public [LMS-Community SqueezeOS tree](https://github.com/LMS-Community/squeezeos/tree/bad080aecfec8226a4c1699b29d32cbba4ba396b)
contains the build corresponding to the firmware date. Commit
`bad080aecfec8226a4c1699b29d32cbba4ba396b` is dated 2014-02-14 and contains:

- the `linux-imx25` recipe based on upstream Linux 2.6.26;
- the 2.6.26.8 and PREEMPT_RT16 patch sets;
- the large Freescale i.MX vendor patch stack;
- Logitech's Radio (`baby`) platform patches;
- the `.config` patch used by the Radio;
- the OpenEmbedded selection of CodeSourcery 2010q1-202.

The running kernel independently reports the same GCC release. The recipe is
visible in [linux-imx25_svn.bb](https://github.com/LMS-Community/squeezeos/blob/bad080aecfec8226a4c1699b29d32cbba4ba396b/poky/meta-squeezeos/packages/linux/linux-imx25_svn.bb),
and the toolchain pin in [poky-external-csl2010q1.inc](https://github.com/LMS-Community/squeezeos/blob/bad080aecfec8226a4c1699b29d32cbba4ba396b/poky/meta-squeezeos/conf/distro/include/poky-external-csl2010q1.inc).

[`tools/kernel/prepare-logitech-kernel.sh`](../tools/kernel/prepare-logitech-kernel.sh)
reconstructs this tree from pinned inputs and verifies the upstream tarball
SHA-256. Its patch application completed successfully in WSL. A complete build
was not claimed: the host's GNU make 4.x rejects syntax accepted by the old
build system, the installed GCC is 13.3 rather than 4.4.1, and Docker was not
running. The old CodeSourcery archive must be obtained from a trustworthy
archive and hashed before it becomes a release input. Generated
`Module.symvers` is mandatory; `modules_prepare` alone is insufficient when
modversions are enabled.

The four distinct validation stages remain:

1. **compile:** source produces an ARM relocatable object;
2. **link/modpost:** every dependency is exported and receives a CRC;
3. **load:** loader accepts ELF, relocations, vermagic and all CRCs;
4. **function:** the module behaves correctly with this vendor backport.

## Why a module cannot fix the second UBIFS mount

The vendor backport added:

```c
err = bdi_register(&c->bdi, NULL, "ubifs");
```

to each UBIFS mount. The historical follow-up patch, posted as
[“ubifs: allow more than one volume to be mounted”](https://lkml.iu.edu/0907.0/00828.html),
changed the name to `ubifs_<ubi-number>_<volume-id>`. It is a small source fix,
but it sits inside `ubifs_fill_super()`.

On this Radio `CONFIG_UBIFS_FS=y`, not `m`: UBIFS is linked into the kernel and
absent from `/proc/modules`. It cannot be unloaded. Registering another
filesystem called `ubifs` returns `-EBUSY`; registering a differently named
copy does not redirect ordinary `mount -t ubifs` and would duplicate a large,
stateful driver against the same UBI internals. More importantly, the vendor
UBIFS code uses internal/static functions and data that are not a supported
module interface.

A small helper can call exported `bdi_register`, UBI KAPI functions, or
`register_filesystem`, but none is an extension point into the already-running
UBIFS `fill_super` path. Creating a separate BDI does not cause UBIFS to use it.

There is no supported live-patch framework, no kprobes, and no ftrace-based
redirection configured. An ARM text patch would require locating an internal
instruction sequence, stopping concurrent execution, handling branch range and
literal pools, changing text permissions, and synchronizing instruction/data
caches. A firmware revision or compiler change could invalidate offsets. This
is kernel-memory patching and is rejected for production.

**Answer: simultaneous UBIFS mounts cannot be enabled safely by a dynamically
installed module without replacing the kernel.** The exact blocker is that the
bug is in a built-in driver's private mount function with no supported hook or
replacement mechanism.

## Alternative filesystem modules

| Candidate | Interface | Module/dependencies | Semantics | Assessment |
|---|---|---|---|---|
| JFFS2 | `/dev/mtd/7` (`sbdata` gluebi MTD) | `jffs2.ko`; CRC32 is already built in; optional zlib | files, modes, ownership, symlinks | **Most promising**; flash-aware and no block shim, but double GC/wear management, scan RAM/time and power loss need measurement |
| ext2 | `/dev/mtdblock:sbdata` | `ext2.ko`; minimal profile can avoid xattr/ACL modules | full required semantics | Technically mountable in principle, but `mtdblock` performs eraseblock read-modify-write and ext2 has no journal; high write amplification and poor sudden-power-loss behavior |
| ext3 | `/dev/mtdblock:sbdata` | `ext3.ko`, `jbd.ko` and CRC-matched dependencies | full semantics, journal | Journal adds RAM/writes; old mtdblock flush/barrier behavior and eraseblock RMW remain; not preferred |
| FAT/VFAT | block translation | FAT/VFAT modules plus NLS choices | no native Unix ownership/modes/symlinks | Fails mandatory semantics |
| SquashFS/CramFS | block/MTD | driver | read-only | Cannot support installer updates |
| UBIFS copy | UBI character device | large private driver dependency surface | ideal semantics | Cannot replace or coexist transparently with built-in `ubifs` type |

Linux 2.6.26 contains module-capable ext2/ext3/JFFS2 sources. The exact source
tree's [filesystem Kconfig](https://github.com/torvalds/linux/blob/v2.6.26/fs/Kconfig)
documents ext2 as a block filesystem and JFFS2 as an MTD filesystem. Actual
dependency and CRC lists must come from `modpost` against the reconstructed
vendor build, not from generic 2.6.26 binaries.

### UBI interfaces and safety

The device exposes three distinct layers:

- `/dev/ubi0_5`: UBI character volume, suitable for UBIFS only among these
  candidates;
- `/dev/mtd/7`: `gluebi` MTD emulation for the UBI volume;
- `/dev/mtdblock:sbdata`: generic mtdblock translation above gluebi.

`gluebi` is built into this kernel. Its source maps MTD reads, aligned writes,
erase and sync operations to the UBI EBA/KAPI, so UBI still owns physical PEB
mapping and bad-block handling. JFFS2 can consume that MTD contract directly.
The layering is nevertheless redundant: JFFS2 performs log-structured garbage
collection above UBI's own wear leveling. It must be validated rather than
declared safe by inspection.

`mtdblock` caches a whole logical eraseblock and performs read-modify-erase-write
for small block writes. An ordinary filesystem therefore amplifies metadata and
small-file writes. A power cut can lose the cached/rewritten LEB, while ext2
then needs filesystem checking. This is a worse fit for unattended applet
storage than JFFS2.

Modern `ubiblock` is not present in the tree; its normal use is a read-only block
view, so backporting it does not supply the required writable filesystem. A new
writable UBI block translator would be a custom FTL and is outside the safe,
maintainable scope.

The existing `sbdata` contents are UBIFS. Using JFFS2 or ext2 would require a
separately authorized destructive reformat of **only the positively identified
`sbdata` volume**. It cannot be mounted as JFFS2 as-is. No such operation was
performed.

## SBStandaloneBase integration

If disposable physical tests validate JFFS2, the smallest architecture is:

```text
normal boot and production UBIFS
  -> persistent StandaloneBase bootstrap remains on production storage
  -> verify exact model/kernel/config/UBI geometry/volume name+ID+type+health
  -> load exact, hashed jffs2.ko (failure is non-fatal)
  -> mount positively identified gluebi MTD at /mnt/sbdata (never format here)
  -> validate marker, free space and staged applet tree
  -> bind-mount /mnt/sbdata/applets over /usr/share/jive/applets
  -> start/restart SqueezePlay so normal discovery runs once
```

The existing Applet Installer writes directly into
`/usr/share/jive/applets`, recursively replacing files. A bind mount is more
transparent than symlinks and preserves the original directory underneath for
fallback. Individual symlinks expose installer edge cases; replacing the whole
directory with a persistent symlink risks a dangling path.

Applet discovery occurs before `StandaloneBaseApplet:init()`. Loading/mounting
from the applet is therefore too late for a normal boot discovery pass. A user
action can mount and then deliberately restart SqueezePlay; persistent automatic
use ultimately needs the already-designed minimal bootstrap/early helper while
leaving normal OS boot independent of it. No firmware boot-sequence replacement
is required, but installing any startup hook is a later, explicitly authorized
milestone.

A reboot is not intrinsically required to load a module or mount/bind storage.
Restarting SqueezePlay is required for reliable discovery. A reboot may be used
later to prove boot ordering and fallback, never as an automatic response to a
failure.

## Failure and recovery policy

- Verify a release allow-list plus live geometry and volume identity; firmware
  version alone is never sufficient.
- Stage module and helpers on production storage with hashes; do not place the
  bootstrap on optional storage.
- Module-load, mount and marker failures leave the original applet directory
  visible and must not reboot or format anything.
- Never format because mount returned an error. Formatting is a separate,
  confirmed action with backup and exact-device review.
- Activate the bind only after filesystem and copied-tree validation. Undo only
  the bind on post-activation failure.
- Keep StandaloneBase itself available in production storage for diagnostics and
  disable/recovery actions.
- Treat corruption as read-only/offline recovery; do not run an automatic repair.
- Applet installation is not atomic, so stage/copy/verify/rename support should
  be added around external-storage installs where feasible.
- An incompatible module yields a diagnostic and normal boot, never a retry or
  reboot loop.

The volume has 521 LEBs of 129,024 bytes: 67,221,504 bytes (about 64.1 MiB)
raw. Filesystem metadata, clean space and JFFS2 garbage-collection reserve reduce
usable capacity; the physical prototype must measure the real value rather than
promise 64 MiB.

## Offline prototype and test status

Added artifacts:

- `native/kernel-modules/sbdiag/sbdiag.c`: harmless load/unload log probe;
- `tools/kernel/prepare-logitech-kernel.sh`: pinned source reconstruction;
- `tools/kernel/build-sbdiag.sh`: fail-closed module build requiring
  `Module.symvers` and exact release;
- `tools/kernel/inspect-module.sh`: ELF, metadata, import and CRC inspection.

Completed tests:

- PASS: SSH identity, kernel, config, filesystems, mounts and module inventory;
- PASS: copied and inspected existing `ar6000.ko` read-only;
- PASS: public patched config matches live config except timestamp;
- PASS: pinned source reconstruction and complete patch-series application;
- PASS: no physical write/load/reboot occurred;
- BLOCKED: complete kernel/module build, because the exact legacy compiler is
  not yet staged and the local Docker daemon was unavailable;
- NOT RUN by design: module load and every storage/NAND operation.

QEMU can validate ARM EABI instruction execution and module ELF inspection, but
cannot validate insertion: that requires a kernel with the same build and CRC
table. It also cannot establish Radio-specific UBI/gluebi behavior unless that
entire vendor kernel and an emulated NAND topology are booted.

## Next physical experiment (requires separate approval)

The first experiment must test only module ABI, not storage:

1. Rebuild the complete vendor kernel using CodeSourcery 2010q1-202 and archive
   its inputs, logs and `Module.symvers`.
2. Build `sbdiag.ko`; compare ELF class, EABI, relocations, vermagic and the CRCs
   for shared imports (`struct_module`, `printk`, cleanup/init machinery) with
   `/lib/atheros/ar6000.ko` where overlap exists.
3. Keep COM5 open at 115200 and confirm an interactive root prompt.
4. Copy only the hash-verified probe to `/tmp/sbdiag.ko` (RAM).
5. Record `cat /proc/modules`, `cat /proc/sys/kernel/tainted` and `dmesg` tail.
6. Run `insmod /tmp/sbdiag.ko`; verify its single log line and `/proc/modules`.
7. Immediately run `rmmod sbdiag`; verify the unload line and restored module
   list/taint state; remove the temporary RAM file.
8. Stop on any warning, oops, hang, CRC/version error or serial anomaly. Do not
   mount, open or write `/dev/mtd*` or `/dev/ubi*`, and do not reboot.

After separately approving that test and copying the artifact to `/tmp`, the
exact Radio-side command sequence is:

```sh
set -eu
test "$(uname -r)" = "2.6.26.8-rt16"
test "$(cat /proc/sys/kernel/tainted)" = "0"
sha256sum /tmp/sbdiag.ko
grep -q '^sbdiag ' /proc/modules && exit 1 || true
dmesg | tail -n 30
insmod /tmp/sbdiag.ko
grep '^sbdiag ' /proc/modules
dmesg | tail -n 30
rmmod sbdiag
grep -q '^sbdiag ' /proc/modules && exit 1 || true
cat /proc/sys/kernel/tainted
dmesg | tail -n 30
rm -f /tmp/sbdiag.ko
```

The expected SHA-256 must be compared manually with the release manifest before
`insmod`; it is intentionally not guessed here because no validated binary has
yet been built.

Only after that succeeds should a separately reviewed milestone build a minimal
JFFS2 profile and test it on disposable storage. It must not begin with the
existing `sbdata` volume, whose current contents would have to be destroyed.

## Direct answers

1. **Does the stock kernel support modules?** Yes: load/unload/modversions are
   enabled and `ar6000.ko` is live.
2. **Can exact compatible modules be built?** Probably, because the exact
   source/config/toolchain identity is preserved; not yet demonstrated because
   a CRC-matching candidate has not been built and loaded.
3. **Is UBIFS modular?** No, it is built in (`CONFIG_UBIFS_FS=y`).
4. **Can a module fix the second UBIFS mount?** No, not through a supported and
   safe interface.
5. **Most promising alternative?** JFFS2 as a module on gluebi MTD.
6. **Can it safely use current `sbdata`?** Interface-wise it is plausible, but
   current UBIFS contents require destructive reformatting and operational
   safety is unproven. Therefore not yet.
7. **Can StandaloneBase install/activate it?** Technically yes after ABI and
   storage validation, with fail-closed checks and a production-resident
   bootstrap.
8. **Is reboot required?** No for load/mount; SqueezePlay needs restart for
   discovery. Reboot testing is later required for persistent integration.
9. **Can applets be transparent?** A validated bind mount is transparent to
   lookup and likely to the installer; full install/update/power-loss testing
   remains.
10. **What if it fails?** Skip the bind, retain original applets, report the
    error, and neither format nor reboot.
11. **How much space?** 67,221,504 raw bytes (64.1 MiB), less JFFS2 overhead and
    reserve.
12. **Next safe experiment?** Load and immediately unload only `sbdiag.ko` from
    `/tmp` with COM5 recovery, after CRC equivalence is established.

**Final verdict: CONDITIONAL.**
