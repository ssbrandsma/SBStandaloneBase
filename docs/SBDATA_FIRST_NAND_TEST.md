# SBData first NAND test: Phase A preflight

## Status

Phase A was completed read-only on 2026-10-04 against the SqueezeOS 7.7.3
Radio at `192.168.1.222`, with COM5 at 115200 8N1. **Phase B is not ready
for approval.** No UBI or MTD write, volume operation, mount change, boot-file
change, reboot, or RedBoot interaction was performed.

The blocker is recovery readiness: the serial Linux console works and the
installed `ubirmvol` can remove a volume by exact name, but a bootable RedBoot
recovery path, its exact commands, and the required complete recovery images
and tools have not been demonstrated for this Radio. The existing online UBIFS
dump is useful but is not an atomic or independently restored recovery image.

## Physical baseline

The observed identity and layout match the required baseline exactly:

```text
model: Logitech MX25 Baby Board
kernel: Linux 2.6.26.8-rt16, armv5tejl
firmware: 7.7.3 r16676
UBI device: ubi0
PEB/LEB/minimum I/O: 131072 / 129024 / 2048 bytes
total/available LEBs: 1014 / 660
bad/reserved-for-bad PEBs: 0 / 10
volumes: 0:kernel_bak:23, 1:cramfs_bak:106, 2:ubifs:82,
         3:kernel:23, 4:cramfs:106
volume type/state: all dynamic / OK
production mount: ubi0:ubifs on /mnt/storage, UBIFS read-write
sbdata: absent
ubifs_test: absent
```

The proposed 521-LEB allocation would leave 139 LEBs, 75 above the configured
64-LEB minimum reserve. Capacity does not imply approval.

The boot log reports production UBIFS `w4/r0`, LZO, mounted as UBI device 0,
volume 2. It also shows that recovery was performed on the production UBIFS at
the latest boot and completed successfully; that is not evidence that an
external recovery path works.

## Boot safety analysis

The complete `/linuxrc` and `/etc/init.d/rcS` were read. `rcS.local` is absent.
`/linuxrc` attaches the existing UBI device before userspace, mounts the
production volume by the exact UBI name `ubi0:ubifs`, and then constructs the
UnionFS overlay. It does not count user volumes and does not select a volume by
volume ID.

Its MTD discovery is unsafe:

```sh
UBIFS_DEV=`grep ubifs /proc/mtd | cut -c4`
```

The current single match is `mtd4`, named `ubifs`. A new UBI volume is expected
to receive another emulated MTD entry, but an exactly named `sbdata` entry does
not match `grep ubifs`; therefore it cannot introduce the historical second
match caused by `ubifs_test`. The test must reject every name except the exact
string `sbdata`. Existing MTD numbering and the single `ubifs` match must be
rechecked immediately after creation.

Factory reset occurs when the production mount fails, `.factoryreset` exists,
or `factoryreset` is on the kernel command line. Adding an unmounted `sbdata`
volume does not directly satisfy those conditions. Nevertheless, UBI metadata
failure or accidental interference with `ubifs` could make the production
mount fail and enter destructive factory-reset handling. This is why verified
offline recovery remains mandatory.

## Backup verification

`C:\SqueezeboxBackup\SHA256SUMS` matches both files:

```text
a5957089921392fdd9cec53df7f4da2b077b555716fe224626a4e668cedaf56d  ubifs-files.tar
98f115abe1e2c3b19635f003bdcb52536561343a89b00ec1480f867fe2e02898  ubifs-online.img
```

`ubifs-files.tar` is 3,528,192 bytes. It contains 89 regular files beneath the
top-level paths `etc`, `mnt`, `root`, `standalone-backups`, `standalonebase`,
and `usr`. It was extracted successfully into a temporary host directory (40
directories observed), then the temporary restore was removed. This proves
archive readability, not a metadata-perfect restore to UBIFS; ownership,
special files, xattrs and a full device restore remain unproved.

`ubifs-online.img` is 10,579,968 bytes, exactly 82 LEBs. Read-only superblock
inspection reports valid UBIFS, `w4/r0`, minimum I/O 2048, LEB size 129024,
`leb_cnt=82`, `max_leb_cnt=82`, and LZO. Because it was captured from the live
online production filesystem, it must not be treated as an atomic image.

Before Phase B, provide and rehearse a bootable recovery path and determine
whether an offline production-volume image plus external kernel/CramFS and UBI
layout material is required. Do not rely on factory reset as rollback.

## COM5, tools and recovery assessment

COM5 provides a working Linux root shell and SSH host-key verification succeeds.
RedBoot exists as `mtd0`, but Phase A deliberately did not enter or interrupt
it. No exact, tested RedBoot procedure was found in the project or backup set.
Consequently RedBoot availability as an effective recovery environment is
**unverified**.

Installed tools and their actual syntax:

```text
ubimkvol version 1.0:
  ubimkvol /dev/ubi0 -S <LEBs> -N <name> -t dynamic
ubiupdatevol version 1.1:
  ubiupdatevol /dev/ubi0_N <image-file>
ubirmvol version 1.0:
  ubirmvol /dev/ubi0 -N <exact-name>
```

Removal by exact name is available if Linux remains bootable. A recovery removal
would first require an unmounted volume, two independent exact-name/ID checks,
and separate authorization, followed by:

```sh
ubirmvol /dev/ubi0 -N sbdata
```

It must never remove a volume by an assumed ID and must never be run as an
automatic failure handler.

## Formatter and RAM-only image validation

The formatter transferred to `/tmp` matched MD5
`4cd405cf61fab20557976446d64e5ef4`, corresponding to the checked SHA-256
`f1ec2fb29c0a9e40a300283c191092905e0d6da40fcef32184dbfb5677653af3`.
It reported version 1.3 and is the previously verified static ARM EABI5,
soft-float executable.

The exact RAM-only command succeeded:

```sh
/tmp/sb-mkfs-test/mkfs.ubifs \
  -r /tmp/sb-mkfs-test/root -m 2048 -e 129024 \
  -c 521 -x lzo -o /tmp/sb-mkfs-test/image.ubifs
```

Result: exit 0, 0.31 seconds, 1,806,336 bytes (14 LEBs), valid magic and node,
`w4/r0`, minimum I/O 2048, LEB size 129024, `leb_cnt=14`,
`max_leb_cnt=521`, five log LEBs, two LPT LEBs, one orphan LEB and LZO.
The image length is exactly `14 * 129024`.

Before generation, free/buffer/cache/reclaimable memory was
9,020/7,784/20,116/2,660 KiB. Afterwards it was
7,244/7,784/21,880/2,660 KiB; anonymous memory stayed at 16,072 KiB. `/tmp`
is unbounded `ramfs`, so its effective limit is available RAM. BusyBox again
reported an unusable peak RSS of zero.

The formatter, helper, image and temporary directory were removed. Final UBI
state remained five volumes and 660 free LEBs; `/proc/mtd` and mounts were
unchanged.

## Proposed Phase B procedure — do not execute

The following is an execution plan only. It requires a new explicit approval
after the recovery blocker is resolved.

### 1. Recheck and capture baseline

```sh
uname -a
head -1 /etc/squeezeos.version
ubinfo -a
cat /proc/mtd
cat /proc/mounts
grep ubifs /proc/mtd
test "$(grep -c ubifs /proc/mtd)" = 1
test ! -e /sys/class/ubi/ubi0_5 || true
```

Independently enumerate every `/sys/class/ubi/ubi0_*` name, ID, type,
`reserved_ebs`, `usable_eb_size` and `corrupted`. Stop unless the original
five-volume baseline is exact and no `sbdata` exists.

### 2. Create exactly one volume

This is the first NAND-writing command:

```sh
/usr/sbin/ubimkvol /dev/ubi0 -S 521 -N sbdata -t dynamic
```

Do not specify or assume ID 5. Immediately rediscover the one sysfs object whose
`name` file is exactly `sbdata`. Stop unless there is exactly one, its ID is
greater than 4, its type is `dynamic`, `reserved_ebs` is 521,
`usable_eb_size` is 129024, `corrupted` is 0, and its `/dev/ubi0_N` node exists.
Re-run `ubinfo -a`, `/proc/mtd`, mounts and the single-match `grep` test. The
original five names, IDs, sizes and states must be unchanged and free LEBs must
be 139. Any deviation ends the test without an automatic retry or removal.

### 3. Generate and verify the compact image

Copy only the approved formatter and helper to `/tmp`, verify their hashes,
then run:

```sh
mkdir -p /tmp/sb-mkfs-test/root
/tmp/sb-mkfs-test/mkfs.ubifs \
  -r /tmp/sb-mkfs-test/root -m 2048 -e 129024 \
  -c 521 -x lzo -o /tmp/sb-mkfs-test/image.ubifs
/tmp/sb-storage-helper inspect-superblock /tmp/sb-mkfs-test/image.ubifs
test "$(stat -c %s /tmp/sb-mkfs-test/image.ubifs)" = 1806336
```

Stop unless all validated Phase A fields match exactly.

### 4. Resolve identity again and write only `sbdata`

Derive `N` from the unique sysfs entry whose name equals `sbdata`; do not paste
an assumed ID. Re-read its name immediately before the write:

```sh
test "$(cat /sys/class/ubi/ubi0_N/name)" = sbdata
test "$(cat /sys/class/ubi/ubi0_N/type)" = dynamic
test "$(cat /sys/class/ubi/ubi0_N/reserved_ebs)" = 521
test "$(cat /sys/class/ubi/ubi0_N/corrupted)" = 0
/usr/sbin/ubiupdatevol /dev/ubi0_N /tmp/sb-mkfs-test/image.ubifs
```

Here `N` is a verified discovered number substituted only after review. Never
target `/dev/ubi0`, `/dev/mtd*`, or IDs 0–4. On any error, stop; do not retry.

### 5. Mount and verify automatic growth

Creating `/mnt/sbdata` through the active UnionFS may itself create a directory
in the production writable overlay. This expected side effect must be explicitly
accepted with Phase B approval and the empty directory should be removed after
the final unmount.

```sh
mkdir /mnt/sbdata
mount -t ubifs ubi0:sbdata /mnt/sbdata
mount | grep 'ubi0:sbdata /mnt/sbdata '
df -k /mnt/sbdata
dmesg | tail -80
```

The mount must be read-write, clean, and report capacity corresponding to the
521-LEB volume rather than the initial 14-LEB image. Inspect `/dev/ubi0_N` with
the helper after a synchronized unmount to confirm the stored `leb_cnt` grew
without manual patching. Stop on any UBIFS warning, read-only mount, failure to
grow, or identity mismatch.

### 6. Persistence test

Only after the clean growth result:

```sh
printf '%s\n' 'sbdata-milestone4' > /mnt/sbdata/milestone4.txt
sync
test "$(cat /mnt/sbdata/milestone4.txt)" = sbdata-milestone4
umount /mnt/sbdata
mount -t ubifs ubi0:sbdata /mnt/sbdata
test "$(cat /mnt/sbdata/milestone4.txt)" = sbdata-milestone4
sync
umount /mnt/sbdata
rmdir /mnt/sbdata
```

Do not bind mount applets, edit boot configuration, migrate data, reboot, or
delete `sbdata`.

### 7. Final verification

Repeat `ubinfo -a`, `/proc/mtd`, `/proc/mounts`, sysfs identity/health checks,
the single `grep ubifs` assertion, and production read-write checks. Confirm
the original five volumes remain byte-for-byte identical in identity and size,
`sbdata` is the only addition, it has 521 LEBs and is unmounted, and no boot or
applet file changed. Remove RAM-only test files.

## Remaining risks and approval decision

- Power loss during volume-table update or `ubiupdatevol` has not been tested.
- The vendor 2.6.26 UBIFS backport has not mounted or grown this image yet.
- A failed production mount enters potentially destructive factory-reset logic.
- The production backup image was captured online and no offline restore was
  rehearsed.
- Exact RedBoot recovery commands and bootable recovery artifacts are missing.
- Creating `/mnt/sbdata` briefly changes the production overlay even though no
  production content is migrated.

**Decision: blocked, not ready for Phase B approval.** Resolve and rehearse the
offline recovery path first. A later explicit authorization must name Phase B;
it must not be inferred from approval of this document.
