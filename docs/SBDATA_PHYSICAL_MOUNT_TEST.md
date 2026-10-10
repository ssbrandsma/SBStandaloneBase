# SBData physical mount test

## Result

The authorized Milestone 4B experiment started on 2026-10-04 and stopped at
the first unexpected diagnostic result, before `ubiupdatevol`. The `sbdata`
volume was successfully created, but it contains no filesystem image and was
never mounted. No operation targeted any production volume.

Repository HEAD at the start was
`50ff45cb8cf1f2d7c6c40983a9701384ec6cc3da`, with the existing uncommitted
Milestone 3/4 and unrelated working-tree changes preserved.

## Pre-write baseline

COM5 at 115200 8N1 provided the running Linux root console. The baseline
matched the authorization exactly:

```text
kernel: Linux 2.6.26.8-rt16, armv5tejl
firmware: 7.7.3 r16676
LEB size / minimum I/O: 129024 / 2048 bytes
available LEBs: 660
volumes: 0:kernel_bak:23, 1:cramfs_bak:106, 2:ubifs:82,
         3:kernel:23, 4:cramfs:106
all production volumes: dynamic, OK
sbdata and ubifs_test: absent
production mount: ubi0:ubifs on /mnt/storage, read-write
grep ubifs /proc/mtd: exactly mtd4 "ubifs"
```

`rcS.local` and `/mnt/sbdata` were absent. The normal applet directory and
`SetupAppletInstaller` were accessible.

## Volume creation

The only executed UBI-writing command was:

```sh
/usr/sbin/ubimkvol /dev/ubi0 -S 521 -N sbdata -t dynamic
```

It completed with exit status zero:

```text
Volume ID 5
name: sbdata
type: dynamic
size: 521 LEBs / 67,221,504 bytes
LEB size: 129024
alignment: 1
```

Independent sysfs and `ubinfo -a` checks confirmed ID 5, the exact name,
dynamic type, 521 reserved LEBs, 129024-byte usable LEBs, corruption state 0,
and `/dev/ubi0_5` major/minor 253:6. Available LEBs became exactly 139. The
original volume IDs, names, sizes and states remained unchanged. `/proc/mtd`
added only `mtd7`, named `sbdata`; the unsafe `grep ubifs` expression still
returned only production `mtd4`.

## Native image generation

The transferred formatter matched MD5
`4cd405cf61fab20557976446d64e5ef4` and reported version 1.3. It is the approved
136472-byte artifact whose SHA-256 is
`f1ec2fb29c0a9e40a300283c191092905e0d6da40fcef32184dbfb5677653af3`.

Executed command:

```sh
/tmp/sb-mkfs-test/mkfs.ubifs \
  -r /tmp/sb-mkfs-test/root -m 2048 -e 129024 \
  -c 521 -x lzo -o /tmp/sb-mkfs-test/image.ubifs
```

The formatter exited successfully. Superblock inspection reported:

```text
magic: 0x06101831
valid: true
format: w4/r0
minimum I/O: 2048
LEB size: 129024
leb_cnt: 14
max_leb_cnt: 521
log/LPT/orphan LEBs: 5/2/1
compressor: LZO
```

`ls -l` reported the expected 1,806,336-byte file. Reading the file over the
verified SSH connection also returned exactly 1,806,336 bytes and SHA-256
`9adce70fa4213b4c999e10f4130d5be363c6fe3f886bb3de24078214c788757e`.
The historical image contains variable metadata, so this image digest is not
expected to equal a previous generated image.

## Stop condition

The Radio has no standalone `stat` command and its BusyBox build has no `stat`
applet. As a fallback, `wc -c` unexpectedly reported 1,806,307 bytes, 29 fewer
than both inode size and a complete SSH read:

```text
ls -l:        1806336
SSH cat read: 1806336
BusyBox wc:   1806307
```

This appears to be a target `wc` diagnostic defect rather than an image-size
or superblock defect, but the discrepancy was unexpected. Milestone 4B requires
stopping on any unexpected result and forbids writing an image whose size check
differs. Therefore no attempt was made to resolve it by regenerating the image,
padding it, retrying, or proceeding with NAND writing.

The following were **not executed**:

- `ubiupdatevol`;
- mounting `sbdata`;
- creating `/mnt/sbdata`;
- filesystem growth or persistence writes;
- `ubirmvol`, resize, raw MTD, reboot, boot integration or applet migration.

## Preserved diagnostic state

The new empty volume was deliberately not deleted:

```text
ubi0_5 name/type/LEBs: sbdata / dynamic / 521
corrupted: 0
update marker: 0
available LEBs: 139
mounted: no
```

The generated image and tools remain under `/tmp` for inspection:

```text
/tmp/sb-mkfs-test/image.ubifs
/tmp/sb-mkfs-test/mkfs.ubifs
/tmp/sb-storage-helper
```

The production `ubi0:ubifs` volume remains mounted read-write. All five
production volumes are unchanged and healthy. There were no new UBI/UBIFS
kernel warnings, the original applet directory remains accessible, no boot
script was changed, and the Radio remains responsive.

## Answers and next step

1. `sbdata` creation: **yes**, ID 5, healthy, empty.
2. `ubiupdatevol`: **not attempted** due to the stop condition.
3. Vendor-kernel mount: **not tested**.
4. UBIFS growth: **not tested**.
5. Usable filesystem storage: **not measured**.
6. Persistence test: **not run**.
7. Production volumes: **unchanged**.

Before resuming, explain or formally waive the BusyBox `wc -c` discrepancy and
explicitly authorize continuation from the existing empty `sbdata` volume.
The image's actual inode size and complete SSH byte count match the intended
14-LEB size, but this run will not infer permission to continue after its
mandatory stop.

## Milestone 4C continuation

The owner subsequently authorized continuation from the preserved volume and
required independent investigation of the `wc` discrepancy before writing.

### Image-integrity resolution

Two non-PTY SSH checks and filesystem metadata established that the image is
complete:

```text
ls inode size:             1,806,336 bytes
non-PTY SSH stream length: 1,806,336 bytes
stream SHA-256: 9adce70fa4213b4c999e10f4130d5be363c6fe3f886bb3de24078214c788757e
stream MD5:    53c2ff45ba19c7d796eaa9502bfcd7de
Radio MD5:     53c2ff45ba19c7d796eaa9502bfcd7de
BusyBox wc:    1,806,307 bytes
```

The non-PTY `wc` invocation produced the same incorrect count as COM5, while a
non-PTY binary stream read every expected byte and hashed consistently. The
filesystem inode size, complete stream, formatter success and decoded
superblock all agree. The best-supported explanation is therefore a defect in
this old BusyBox `wc` implementation or its libc read/count path, isolated from
the file and transport. No binary data was passed through COM5. The image was
freshly generated by the pinned verified formatter and was not modified or
regenerated for this continuation.

Immediately before writing, the image again decoded as valid `w4/r0`, 2048-byte
minimum I/O, 129024-byte LEB, `leb_cnt=14`, `max_leb_cnt=521`, and LZO. The
target again verified as exactly `ubi0_5`, name `sbdata`, dynamic, 521 LEBs,
healthy, update marker zero, unmounted, with 139 free LEBs and unchanged
production volumes.

### Volume update

Executed once, without retry:

```sh
/usr/sbin/ubiupdatevol /dev/ubi0_5 /tmp/sb-mkfs-test/image.ubifs
```

Start and completion times were 10:30:17 and 10:30:18 CEST. The command emitted
no stdout or stderr. The serial command sequence completed normally; subsequent
checks showed `upd_marker=0`, `corrupted=0`, unchanged name/ID/size, and a valid
on-volume superblock identical to the source image. The maximum UBI erase count
changed from 71 to 73, consistent with the update. All protected volumes stayed
healthy and unchanged.

### Mount failure and mandatory stop

The mount point was created as authorized, then the only mount attempt was:

```sh
mount -t ubifs ubi0:sbdata /mnt/sbdata
```

It failed with `File exists`. The kernel emitted a stack trace beginning:

```text
kobject_add_internal failed for ubifs with -EEXIST,
don't try to register things with the same name in the same directory.
...
bdi_register
ubifs_get_sb
vfs_kern_mount
```

`/sys/class/bdi/ubifs` already exists for the mounted production filesystem.
The trace shows the vendor UBIFS backport registering every UBIFS mount with the
fixed backing-device name `ubifs`; the second concurrent UBIFS mount collides
with that existing kobject before the new filesystem is mounted. This is a
kernel/backport limitation, not rejection of the `w4/r0` superblock. No UBIFS
filesystem-size or media-format mount message was emitted for `sbdata`, so
growth cannot be inferred.

As required for a mount failure, there was no alternate syntax, mount retry,
production unmount, image rewrite, repair or persistence write. The filesystem
growth and file persistence tests were not run.

### Final physical state

```text
production IDs 0-4: unchanged names, sizes, dynamic type and OK state
production ubi0:ubifs: still mounted read-write at /mnt/storage
sbdata: ID 5, dynamic, 521 LEBs, OK, corrupted=0, upd_marker=0
sbdata filesystem image: written, valid 14/521 w4/r0 superblock
sbdata mount: absent
available LEBs: 139
/mnt/sbdata: empty mount-point directory remains in the production overlay
rcS.local: absent
original applets: accessible
Radio: responsive; no reboot performed
```

### Updated conclusions

1. Image discrepancy: **resolved as an isolated, unreliable BusyBox `wc`
   result**; independent size and digest checks passed.
2. `ubiupdatevol`: **succeeded** and the image reads back from `ubi0_5`.
3. Vendor-kernel mount: **failed before UBIFS initialization** because a second
   UBIFS backing-device kobject named `ubifs` cannot be registered.
4. Growth from 14 toward 521 LEBs: **not tested**.
5. Usable mounted capacity: **not available**.
6. Persistence test: **not run**.
7. Production volumes: **unchanged and healthy**.

The proposed architecture cannot use two simultaneous UBIFS mounts on this
vendor kernel without addressing the backing-device naming defect or choosing
a different filesystem/storage architecture. Testing by unmounting the live
production UBIFS would disrupt the active UnionFS root and is outside this
authorization. No automatic boot mounting, bind mount or migration should be
implemented until this kernel limitation has a separately reviewed solution.
