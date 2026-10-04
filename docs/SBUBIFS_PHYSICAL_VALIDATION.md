# SBUBIFS physical validation

Date: 2026-10-04

Target: Logitech Squeezebox Radio, MAC `00:04:20:29:16:7f`

Address: `192.168.1.222`

Firmware: SqueezeOS 7.7.3 r16676

Kernel: `2.6.26.8-rt16 #1 PREEMPT RT`, ARMv5TEJ

## Verdict

**PASS.** The module loaded and unloaded cleanly, mounted `sbdata` alongside
production UBIFS read-only and read-write, completed bounded file and metadata
operations, and preserved all test data across unmount, module unload, reload,
and remount. Both drivers had distinct filesystem, BDI and slab identities.
No SBUBIFS, UBIFS, or UBI warning or error occurred.

## Module identity

The authorized artifact was `/tmp/sbubifs-work/src/sbubifs.ko`:

```text
sha256: 63652ce67df06a78abb84a4986253bdab02fbd7b7c000779c60b3d393ba9566b
size:   199119 bytes
format: ELF 32-bit LSB relocatable, ARM, EABI5, not stripped
```

An older output at `/tmp/sbubifs-out/sbubifs.ko` was 198,944 bytes but had a
different SHA-256 and was rejected. The Radio lacks `sha256sum`, so the
uploaded RAM file was read back byte-for-byte over host-key-verified SSH and
hashed locally. Its size and digest exactly matched the authorized artifact.

## Stage 0 — PASS

SSH used the existing known-host key with `RejectPolicy`. COM5 at 115200 8N1
was independently monitored and captured the SBUBIFS mount messages.

Baseline:

```text
/dev/root       /             cramfs  ro
ubi0:ubifs      /mnt/storage  ubifs   rw,noatime
sbdata          unmounted
/proc/sys/kernel/tainted: 0
MemTotal: 62112 kB
MemFree:  10424 kB
/mnt/storage available: 4984 KiB
```

UBI identity was resolved by sysfs path and exact volume name:

| Volume | ID | Type | Reserved LEBs | LEB size | Corrupt | Update marker |
|---|---:|---|---:|---:|---:|---:|
| `ubifs` | 2 | dynamic | 82 | 129024 | 0 | 0 |
| `sbdata` | 5 | dynamic | 521 | 129024 | 0 | 0 |

No baseline UBI/UBIFS corruption or I/O error was present. Production UBIFS
had completed its ordinary boot-time recovery and mounted as `w4/r0`.

## Stage 1 — PASS

`insmod /tmp/sbubifs.ko` returned zero in approximately one second. While
loaded:

```text
/proc/filesystems: ubifs and sbubifs
/proc/modules:     sbubifs 167600 0 - Live 0xbf024000
slabs:             ubifs_inode_slab and sbubifs_inode_slab
kernel taint:      0
```

Production `/mnt/storage` remained mounted and readable. No load-time kernel
message, BDI conflict, slab conflict, warning, or error occurred. `MemFree`
changed from 8,524 kB immediately before loading to 7,936 kB after loading,
an observed delta of about 588 kB which includes concurrent cache variation.

`rmmod sbubifs` returned zero. `sbubifs` disappeared from `/proc/filesystems`,
`/proc/modules`, and `/proc/slabinfo`; built-in `ubifs` remained registered and
production storage remained readable. `MemFree` recovered to 8,436 kB and
taint remained zero.

## Stage 2 — PASS

The module was reloaded and the existing filesystem was mounted with:

```sh
mount -t sbubifs -o ro ubi0:sbdata /tmp/sbubifs-test
```

Observed:

```text
ubi0:sbdata /tmp/sbubifs-test sbubifs ro
filesystem size: 65802240 bytes (510 LEBs)
journal size:    8902656 bytes (69 LEBs)
media format:    w4/r0
available:       59708 KiB
contents:        empty root directory
BDIs:            ubifs and sbubifs_0_5
```

Both inode slabs existed independently. Production UBIFS remained mounted and
readable. There was no recovery message, write attempt, warning, error, taint,
or unexpected UBI metadata change. `MemFree` while mounted was 7,484 kB.

The production-volume rejection was established by static inspection of the
module allowlist rather than opening volume ID 2 through SBUBIFS. The allowlist
requires UBI0, ID5, exact name `sbdata`, dynamic type, and clear corruption and
update markers.

The filesystem was cleanly unmounted and the module was cleanly unloaded.
`MemFree` was then 8,176 kB. The temporary mountpoint was removed.

## Stage 3 — PASS

Before Stage 3, the following prerequisites were confirmed:

- `sbdata` remained UBI0/ID5, named `sbdata`, 521 LEBs, healthy;
- the read-only root contained no user data;
- checked-in `artifacts/ubi/sbdata-w4-r0.img` and the pinned historical builder
  can recreate the 2048/129024/521 `w4/r0` profile;
- the module source still contained the exact `sbdata` allowlist;
- Stage 2 produced no unexpected kernel message.

Two oversized SSH exec requests were rejected before execution. Short SSH
commands worked, but one subsequent short channel also closed without an exit
status. Read-only inspection proved no Stage 3 command had run. Because COM5
was reliable and is an approved command path, Stage 3 was then executed there
as individually acknowledged commands.

The following operations physically succeeded inside the dedicated
`sbubifs-validation` directory:

- create and read a small file;
- overwrite it with `beta-overwrite` and read it back;
- create a directory and rename it;
- change the renamed directory mode to `0750`;
- create `note-link -> note.txt`;
- create and delete a temporary file.

`ls -la` confirmed the 14-byte overwritten file, `note-link -> note.txt`,
and `drwxr-x---` directory. A 1 MiB zero-filled payload was then written and
synced. Checksums before unmount were:

```text
68c5630dbd9d49f464103a6bb1fb54f4  note.txt
b6d81b360a5672d80c27430f39153e2c  payload.bin
```

The payload write itself took 0.14 seconds, approximately 7.1 MiB/s, before
the separately executed explicit sync. After sync, `Dirty` and `Writeback`
were both zero. Taint remained zero and production UBIFS stayed readable.

## Stage 4 — PASS

The filesystem was cleanly unmounted and the module unloaded. The module was
then reloaded and `sbdata` remounted without rebooting.

Both MD5 digests matched their pre-unmount values. The 14-byte file contents,
`0750` directory mode, symlink target, 1 MiB payload size, 510-LEB filesystem
size, and `w4/r0` geometry persisted unchanged. Production UBIFS remained
mounted read-write throughout.

Measured wall-clock times were:

| Operation | Time |
|---|---:|
| Module load | 0.15 s |
| Mount | 0.20 s |
| Unmount | 0.06 s |

## Stage 5 — PASS

Reading the 1 MiB payload to `/dev/null` took 0.15 seconds, approximately
6.7 MiB/s. This small cached measurement is an operational sanity check, not a
raw-NAND benchmark.

Observed memory samples during the final run:

| State | MemFree | Slab |
|---|---:|---:|
| Before load | 6,744 kB | 5,388 kB |
| After load | 6,164 kB | 5,468 kB |
| After mount | 5,612 kB | 5,492 kB |
| After write/remount and cached payload | 4,932 kB | 5,496 kB |
| After cleanup and unload | 7,144 kB | 5,492 kB |

The load-only free-memory delta was about 580 kB. The mounted/cached low point
was about 1.8 MiB below baseline. Free memory exceeded the baseline after
unload as cache state changed; therefore these values demonstrate recovery but
are not a precise unreclaimable allocation measurement.

## Stage 6 — PASS

Cleanup verification:

```text
/tmp/sbubifs.ko:   removed
/tmp/sbubifs-test: removed
sbubifs module:    absent
sbdata mount:      absent
production UBIFS:  mounted read-write and readable
original ubifs:    registered
kernel taint:      0
```

No startup configuration was changed. No volume was formatted, recreated,
resized, created, or removed. No direct MTD/UBI write or production-file change
occurred. No reboot occurred and the second Radio was not contacted.

The kernel log contained UART input-overrun messages from an interrupted,
high-rate serial upload attempt before the final Ethernet transfer. They
preceded SBUBIFS mounting and were not storage errors. The experimental serial
uploader was removed rather than retained as a supported tool.

## Required answers

1. Did `sbubifs.ko` load successfully? **Yes.**
2. Did it unload successfully? **Yes.**
3. Were both filesystem types registered simultaneously? **Yes.**
4. Could `sbdata` mount alongside production UBIFS? **Yes, read-only and
   read-write.**
5. Were there BDI or slab conflicts? **No.**
6. Did read/write operations succeed? **Yes, including the bounded 1 MiB
   payload and metadata operations.**
7. Did data survive unmount/remount? **Yes, including module unload/reload.**
8. Additional memory? **About 580 kB load-only and approximately 1.8 MiB at
   the mounted/cached low point; memory recovered after unload.**
9. Kernel warnings or errors? **None attributable to SBUBIFS or storage.**
10. Did production UBIFS remain operational? **Yes.**
11. Was temporary test state cleaned? **Yes.**
12. Suitable for the next milestone? **Yes, subject to retaining the same
    fail-closed identity, health, hash, and cleanup checks.**

**Final verdict: PASS — physical coexistence, bounded read/write, persistence,
memory, performance, and cleanup validation succeeded.**
