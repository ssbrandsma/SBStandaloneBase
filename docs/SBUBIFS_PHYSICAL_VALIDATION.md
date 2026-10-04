# SBUBIFS physical validation

Date: 2026-10-04

Target: Logitech Squeezebox Radio, MAC `00:04:20:29:16:7f`

Requested address: `192.168.1.222`

Firmware: SqueezeOS 7.7.3 r16676

Kernel: `2.6.26.8-rt16 #1 PREEMPT RT`, ARMv5TEJ

## Verdict

**BLOCKED before Stage 1.** Stage 0 device, storage, module, and serial checks
passed, but the workstation could not reach TCP port 22 on the Radio. A
fallback upload to RAM over the serial console encountered a truncated command
and was stopped. The incomplete zero-byte `/tmp/sbubifs.ko.part` was removed.
The module was never loaded, `sbdata` was never mounted, and NAND-backed data
was not changed.

This is not evidence for or against SBUBIFS runtime compatibility.

## Module identity

The initially inspected file `/tmp/sbubifs-out/sbubifs.ko` had the expected
198,944-byte size but SHA-256 `cf3941d5...009a18`; it was rejected and never
sent to the Radio. The authorized artifact was then found at
`/tmp/sbubifs-work/src/sbubifs.ko` and verified as:

```text
size:    198944 bytes
sha256:  63652ce67df06a78abb84a4986253bdab02fbd7b7c000779c60b3d393ba9566b
format:  ELF 32-bit LSB relocatable, ARM, EABI5, not stripped
```

## Stage 0 — PASS

COM5 at 115200 8N1 returned an independently delimited command result after
the serial helper was changed to wait for the newly opened shell. It remained
usable after cleanup.

Observed baseline:

```text
/dev/root       /             cramfs  ro
ubi0:ubifs      /mnt/storage  ubifs   rw,noatime
sbdata          not mounted
/proc/sys/kernel/tainted: 0
MemTotal: 62112 kB
MemFree:  13656 kB
/mnt/storage available: 4980 KiB
```

Verified UBI identity by name as well as number:

| Volume | ID | Type | Reserved LEBs | LEB size | Corrupt | Update marker |
|---|---:|---|---:|---:|---:|---:|
| `ubifs` | 2 | dynamic | 82 | 129024 | 0 | 0 |
| `sbdata` | 5 | dynamic | 521 | 129024 | 0 | 0 |

The boot log showed production UBIFS recovery followed by a successful clean
mount and media format `w4/r0`. It contained no UBI/UBIFS corruption or I/O
error. The only later warning-like message was an unrelated SSI FIFO error.
Production storage was readable and remained mounted.

The Radio reported `192.168.1.222` on `eth1`, but workstation TCP/22 attempts
timed out and ICMP was unreachable. The Radio also could not ping the
workstation at `192.168.1.190`, consistent with client isolation.

## Transfer attempt and stop

Only the permitted RAM path `/tmp/sbubifs.ko.part` was targeted. The serial
fallback encoded binary bytes as shell `printf` input with acknowledged
chunks. The first command exceeded the console's reliable line handling and
left the shell at a continuation prompt. Following the required stop policy,
no retry or alternate transfer was attempted.

The prompt was restored by closing the incomplete quote. Cleanup verification:

```text
/tmp/sbubifs.ko.part: 0 bytes before removal
/tmp/sbubifs.ko:      absent
/proc/modules:        ar6000 only
sbdata mount:         absent
kernel taint:         0
```

## Stage results

| Stage | Result | Notes |
|---|---|---|
| 0 — preflight/baseline | **PASS** | Identity, UBI metadata, health, memory, module hash, logs, and COM5 verified |
| 1 — module load/unload | **BLOCKED / NOT TESTED** | Exact module could not be transferred reliably |
| 2 — read-only mount | **NOT TESTED** | Stage 1 prerequisite not met |
| 3 — read/write operations | **NOT TESTED** | No filesystem write occurred |
| 4 — persistence/remount | **NOT TESTED** | No mount occurred |
| 5 — memory/performance | **NOT TESTED** | Baseline memory only |
| 6 — cleanup | **PASS for attempted state** | Incomplete RAM file removed; module absent; `sbdata` unmounted; production UBIFS operational |

## Commands and actions used

Read-only Radio commands included `uname -a`, `/proc/filesystems`,
`/proc/mounts`, `/proc/meminfo`, `/proc/modules`, `/proc/mtd`, `df -k`, UBI
attributes below `/sys/class/ubi`, `ifconfig`, `route`, and `dmesg`. Cleanup
removed only `/tmp/sbubifs.ko` and `/tmp/sbubifs.ko.part`.

No volume was opened or mounted through SBUBIFS. No command addressed
`/dev/mtd*`, `/dev/ubi*`, `/mnt/storage`, volume creation/removal/resize,
formatting, boot configuration, or the second Radio. No reboot occurred.

## Answers required by the milestone

1. Module load: **NOT TESTED**.
2. Module unload: **NOT TESTED**.
3. Simultaneous filesystem registration: **NOT TESTED**.
4. `sbdata` mount alongside production: **NOT TESTED**.
5. BDI or slab conflicts: **NOT TESTED**.
6. Read/write operations: **NOT TESTED**.
7. Persistence: **NOT TESTED**.
8. Additional memory: **NOT MEASURED**; baseline `MemFree` was 13,656 kB.
9. Kernel warnings/errors: no storage warning or error in the baseline; one
   unrelated SSI FIFO message was present.
10. Production UBIFS operational: **YES** throughout the attempted work.
11. Temporary Radio state cleaned: **YES**.
12. Suitable for the next milestone: **NOT YET ESTABLISHED**.

## Required next step

Restore direct workstation-to-Radio connectivity (or provide a separately
validated binary-safe transfer route), then restart at Stage 0. Verify the
Radio-side SHA-256 before `insmod`. Do not resume at Stage 1 based on this run's
baseline because device state may have changed.

**Final verdict: CONDITIONAL — validation is blocked by transport; runtime was
not tested.**
