# Native historical `mkfs.ubifs` validation

## Result

On 2026-10-04, mtd-utils commit `bb56df1e1a84304ec4a14169e4cdc41116ed4256` (2009-06-05, `mkfs.ubifs` 1.3) successfully generated UBIFS images natively on the SqueezeOS 7.7.3 r16676 Radio through the COM5 Linux console. Both tests wrote only beneath `/tmp/sb-mkfs-test`; no UBI ioctl, volume creation, update, mount, persistent-file change, reboot, or service shutdown occurred.

The selected revision is close to the original UBIFS merge era and produces format `w4/r0`. The Radio kernel is a vendor UBIFS backport on Linux 2.6.26.8; its exact vendor source remains unavailable, so compatibility is based on the observed production `w4/r0` format, historical source review, decoded superblock geometry, and native generation—not firmware version alone. A future milestone must still mount a separately approved test volume before any production recommendation.

## Reproducible build

The ARM build is scripted by `tools/ubi/build-arm-mkfs.sh`. It pins:

- mtd-utils `bb56df1e1a84304ec4a14169e4cdc41116ed4256`;
- zlib 1.2.11 (`c3e5e9fd…`);
- LZO 2.10 (`c0f89294…`);
- util-linux/libuuid 2.34 (`743f9d0c…`);
- Bootlin armv5-eabi musl stable-2020.02-2, GCC 8.4.0 (`4734ebcd…` toolchain archive).

Compiler profile: `-Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections -static -Wl,--gc-sections`. The resulting executable is ELF32 ARM EABI5, soft-float, statically linked and stripped, with no interpreter or dynamic section.

```sh
docker build -f tools/ubi/Dockerfile.arm -t sb-mkfs-arm .
docker create --name sb-mkfs-out sb-mkfs-arm
docker cp sb-mkfs-out:/src/artifacts/ubi/mkfs.ubifs-20090605-armv5-static artifacts/ubi/
docker rm sb-mkfs-out
```

Docker Desktop was unavailable during this run, so the identical pinned script was executed under WSL with the already verified Bootlin toolchain. The Docker recipe is provided but was not falsely recorded as executed.

Artifact:

```text
file: artifacts/ubi/mkfs.ubifs-20090605-armv5-static
size: 136472 bytes
SHA-256: f1ec2fb29c0a9e40a300283c191092905e0d6da40fcef32184dbfb5677653af3
MD5 used for physical transfer verification: 4cd405cf61fab20557976446d64e5ef4
runtime libraries: none
```

QEMU ARM926 executed `-V` successfully and reported `Version 1.3`. QEMU generation produced a 1,677,312-byte 32-LEB-profile image and a compact 1,806,336-byte 521-LEB-profile image; both decoded as `w4/r0` with 2,048-byte minimum I/O and 129,024-byte LEBs.

## Physical preflight

COM5 opened at 115200 8N1 and presented the running Linux root shell. SSH was used only for transfer into `/tmp`; execution and diagnostics used COM5. Before generation:

```text
MemTotal: 62112 kB
MemFree: 11392 kB (small test), 11340 kB (521 test)
Buffers: 7728 kB
Cached: 18792/18816 kB
/tmp: ramfs (no independent capacity limit)
UBI available LEBs: 660
LEB size: 129024
minimum I/O: 2048
volumes: 0:kernel_bak,1:cramfs_bak,2:ubifs,3:kernel,4:cramfs
```

Because `/tmp` is ramfs, image pages compete directly with system RAM. The helper conservatively requires 8 MiB of free-plus-reclaimable memory and 3 MiB effective temporary capacity. The measured effective availability was about 40 MiB. SqueezePlay and all existing services remained running.

## Physical commands and results

Small image:

```sh
busybox time -v /tmp/sb-mkfs-test/mkfs.ubifs \
  -r /tmp/sb-mkfs-test/root -m 2048 -e 129024 -c 32 -x lzo \
  -o /tmp/sb-mkfs-test/image.ubifs
```

Result: exit 0, 0.26 s, 1,677,312 bytes, `leb_cnt=13`, `max_leb_cnt=32`, `log_lebs=4`, `lpt_lebs=2`, `orph_lebs=1`, format `w4/r0`, compressor LZO.

Intended compact profile:

```sh
busybox time -v /tmp/sb-mkfs-test/mkfs.ubifs \
  -r /tmp/sb-mkfs-test/root -m 2048 -e 129024 -c 521 -x lzo \
  -o /tmp/sb-mkfs-test/image.ubifs
```

Result: exit 0, 0.26 s, 1,806,336 bytes, `leb_cnt=14`, `max_leb_cnt=521`, `max_bud_bytes=8257536`, `log_lebs=5`, `lpt_lebs=2`, `orph_lebs=1`, `jhead_cnt=1`, `fanout=8`, `lsave_cnt=256`, format `w4/r0`, compressor LZO.

BusyBox `time -v` reported peak RSS as zero even for control commands, so this 2.6.26 kernel does not expose a trustworthy peak-RSS measurement through that interface. It recorded 112 minor faults for the 521 test. Anonymous memory was effectively unchanged (15,296 to 15,292 KiB); free memory fell by 1,764 KiB while cache rose by 1,768 KiB, matching the generated image. Peak formatter RSS is therefore **not measurable**, while the observed incremental RAM footprint was approximately the 1.8 MiB image cache plus a short-lived small working set.

`leb_cnt` is the number of LEBs materialized in the compact image; `max_leb_cnt` is the growth ceiling stored in the superblock. Thus the 14-LEB image is configured to grow normally when written to a 521-LEB dynamic volume. No superblock was patched.

## Helper and automated validation

`sb-storage-helper` schema 3 performs read-only checks for the formatter path, temporary versus persistent location, ELF32 ARM/static identity, exact SHA-256, mapped version 1.3, 8 MiB conservative RAM availability, 3 MiB temporary capacity, and exact UBI geometry. The exact physical formatter passed. `prepare` remains false with `migration_and_mount_safety_pending`; `install` and `expand` both still exit 78.

Host/QEMU tests pass for formatter detection, missing executable, wrong architecture/linkage, wrong checksum, unsupported version, checksum failure, insufficient RAM, insufficient temporary capacity, incorrect LEB/minimum-I/O geometry, compact geometry, corrupt/truncated images, and repeated read-only execution. The ARM formatter and helper both run under QEMU ARM926.

## Cleanup and unchanged flash state

`/tmp/sb-mkfs-test` was removed after recording results. Final memory returned to 11,536 KiB free and 18,596 KiB cached. `/proc/mounts` was unchanged, UBI still had 660 available LEBs, and the only volumes remained the original IDs 0–4. No `sbdata` or UBIFS-like test volume exists.

## Assessment and remaining risks

Native generation is practical: the formatter is 136 KiB, needs no runtime libraries, completes in roughly 0.26 seconds, and emits a compact 1.8 MiB image for a 521-LEB ceiling. This is preferable to distributing a prebuilt image because geometry and UUID metadata can be generated locally while retaining a small bundle.

Before any NAND write, separately approve and validate: `sbdata` creation identity, `ubiupdatevol` power-loss boundaries, old-kernel mounting and growth of this exact image, migration metadata preservation, boot-hook transaction/rollback, backup restoration, and Applet Installer behavior. Firmware version alone must never bypass geometry, format, volume identity, or kernel-capability checks.
