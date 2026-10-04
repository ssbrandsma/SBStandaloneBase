# SBUBIFS physical validation

Date: 2026-10-04

Target: Logitech Squeezebox Radio, MAC `00:04:20:29:16:7f`

Address: `192.168.1.222`

Firmware: SqueezeOS 7.7.3 r16676

Kernel: `2.6.26.8-rt16 #1 PREEMPT RT`, ARMv5TEJ

## Verdict

**CONDITIONAL.** The module loaded and unloaded cleanly, mounted `sbdata`
alongside production UBIFS both read-only and read-write, and completed bounded
small-file and metadata operations. Both drivers had distinct filesystem, BDI
and slab identities, and no kernel warning or storage error occurred.

Stage 3 stopped after the Radio reported that its BusyBox installation has no
`readlink` utility. The symlink was subsequently verified non-destructively
with `ls -l`, but the stop policy prevented continuing to the 1 MiB payload.
Persistence and throughput therefore remain untested.

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

## Stage 3 — PARTIAL PASS / STOPPED

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

`ls -la` confirmed the 14-byte overwritten file, eight-character symlink
target, and `drwxr-x---` directory. The Radio then returned status 127 because
`readlink` is not installed. Although this was a tooling absence rather than a
filesystem error, no further writes were issued after the nonzero result. The
planned 1 MiB payload, checksum, and sync measurement were not performed.

At the stop point `MemFree` was 6,460 kB, `Dirty` was 4 kB, `Writeback` was
zero, taint was zero, and the kernel log contained only the expected SBUBIFS
mount messages. Production UBIFS remained mounted and readable.

## Stages 4 and 5 — NOT TESTED

Persistence/remount and payload performance were not tested because Stage 3
stopped before the payload and Stage 4. Stage 1 and 2 timing resolution was one
second: module load was at most approximately one second, while mount and
unload completed within the same one-second uptime tick.

## Stage 6 — PASS for executed state

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

## Required answers

1. Did `sbubifs.ko` load successfully? **Yes.**
2. Did it unload successfully? **Yes.**
3. Were both filesystem types registered simultaneously? **Yes.**
4. Could `sbdata` mount alongside production UBIFS? **Yes, read-only.**
5. Were there BDI or slab conflicts? **No.**
6. Did read/write operations succeed? **Partially: bounded small-file and
   metadata operations succeeded; the 1 MiB payload was not attempted.**
7. Did data survive unmount/remount? **NOT TESTED.**
8. Additional memory? **About 588 kB loaded; mounted observation was about
   1,040 kB below the immediate pre-load sample. Most memory returned.**
9. Kernel warnings or errors? **None attributable to SBUBIFS or storage.**
10. Did production UBIFS remain operational? **Yes.**
11. Was temporary test state cleaned? **Yes.**
12. Suitable for the next milestone? **Not yet; payload, checksum, persistence,
    and performance validation remain required.**

The next approved run must restart at Stage 0 and may proceed to Stage 3 only
after repeating the prerequisites. It must not infer current state from this
run.

**Final verdict: CONDITIONAL — coexistence and basic read/write metadata
operations passed; payload and persistence remain untested.**
