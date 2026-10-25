# Phase 2B preflight — NOT READY, 2026-10-25

Target: 192.168.1.228, Baby revision 5, installed 9.0.2 r17146.
No firmware installation, UBI volume changes or storage/mount changes authorized.

## Cleanup plan recorded before deletion

Active `applets`, `config`, operational `cache`, and all UBI volumes are retained.
`firmware-backup-Y8i1jt` is retained: a live interactive shell has its working
directory in its `previous-applets` tree. Its name is not grounds for deletion.
Other unclassified backups are retained for now.

| Exact path under `/mnt/sbdata/` | Measured KiB (`du -sk`) | Classification / reason | Recovery before deletion |
| --- | ---: | --- | --- |
| `release099.NL0xk7` | 8284 | Historical extraction with Base 0.5.7 / Radio 0.9.9 catalog, not active applet path | Off-device verified archive required |
| `release099-backup-20261021-173300` | 5932 | Historical release rollback tree, not active applet path | Off-device verified archive required |
| `jive-cpu-prof-20261024T202004Z-923b8e` | 1360 | Completed profiling capture/maps/samples and separate old Jive binary | Off-device verified archive required |
| `jive-cpu-prof-20261024T202449Z-e6431d` | 1368 | Completed profiling capture/maps/samples and separate old Jive binary | Off-device verified archive required |

The `/proc` executable/cwd/fd snapshot shows none of these four paths in use.
Current Base Lua and current operational catalog have no `release099` reference.
No symlinks were reported by `find /mnt/sbdata -type l`. Confirm paths once more
immediately before removal; do not touch the active bind-mounted applet tree.
Archive current Base and its configuration separately on the laptop first.
Local archives/evidence are private under ignored `.local/phase2b/`.

Baseline free filesystem space: 22,292 KiB. After verified archives and fresh
fd/cwd/executable/canonical-path checks, all four listed paths were removed.
Remaining free: 34,980 KiB; reclaimed: 12,688 KiB (12.39 MiB). `du` totals are not
physical UBIFS accounting, so this result uses the actual `df` delta.

Verified archives retained on laptop:

- `current-application-and-config.tar.gz`: 2,569,397 bytes, 48 members.
- `historical-release-and-profiles.tar.gz`: 12,178,209 bytes, 108 members.

Full gzip CRC and tar member reads passed before deletion; SHA-256 and exact
archive member roots recorded in private `.local/phase2b/archives.json`.
Deleted files are recoverable from those archives. No active application data
or native firmware backup volume was deleted.

## Real image evidence

TLS-verified workstation download from:
`https://downloads.sourceforge.net/project/lmsclients/squeezeos/baby_9.0.2_r17157.zip`.
Extracted only the exact native member, without repacking:

- Bytes: 16,032,273.
- SHA-256: `dc783d525e0ea17736f36ff51d1424e6d8909cc5a9ba51f0cfdcacef56d65d6d`.
- SHA-1: `9ab354c46ae98027d5a3164c2ae13166875d2c87`, matching bundled sidecar.
- Version: 9.0.2 r17157; board list includes Baby 5.
- Kernel: 3,191,832 bytes; root: 14,598,144 bytes.
- CRC, internal MD5 and corrected host native inspector: PASS.

The computed SHA-256 pins these downloaded bytes but is NOT an independently
authenticated publisher digest/signature. Same-archive SHA-1/MD5 and matching
source revision are not independent authenticity evidence. Installation remains
NOT READY until that trust issue is resolved explicitly.

## Native UBI headroom: capacity PASS (not installation authorization)

Actual LEB size 129,024. Free 129; old backup reservations 23 + 106 = 129.
After native reclamation: 258. Candidate rounded allocations: 25 + 114 = 139.
Remaining allocation margin: 119 LEBs (111 after Base's eight-block cushion).

The installed `SetupFirmwareUpgrade/UpgradeUBI.lua` was read, not executed.
`_upgrade` removes `kernel_bak`/`cramfs_bak` on normal boot, then any stale
`kernel_upg`/`cramfs_upg`, before starting download. `updatevol` requests each
upgrade volume's uncompressed byte size from `ubimkvol`. It does not implement
a separate explicit free-LEB acceptance check; allocation utility success is
required and failures can surface later. Then it streams data, reads back MD5,
and atomically renames the current main pair to backups and upgrades to main.
Neither `sbdata` nor `ubifs` is referenced for removal or resizing.

Current boot command line selects normal `cramfs`, supported by this flow.
Failure after old-backup removal leaves the current main pair before rename,
but loses the old fallback pair and may leave partial upgrade volumes. This
is not a power-loss guarantee; no failure/flash experiment has been performed.

## Validation issue fixed

The real image initially hit a false `Malformed deflate stream` failure. When
inflate exhausted its input exactly while filling output, the drain call with
no input could return `Z_BUF_ERROR`. That means refill, not corruption. The
validator now accepts only this no-input/no-output-progress case; stream-end,
length, CRC, SHA and MD5 checks remain mandatory. Larger low-compression fixture
coverage was added. No native flashing code changed.

## Staging policy

Fresh unknown-length downloads still reserve 24 MiB (enforced maximum image)
plus 4 MiB filesystem/concurrent-activity margin: 28 MiB. Only one temporary
download is written and renamed in place on success; validation does not make
a second image copy. Old digest-named candidates are cleaned before the check.
If space runs out mid-download, the existing write failure propagates, partial
temporary data is removed, and no candidate is advertised.

A pre-existing digest-named regular image within the 24 MiB bound is validated
in place, so it now needs only the 4 MiB reserve, not another 24 MiB allocation.
Its bytes are still fully hashed and inspected. Host tests simulate 5 MiB free:
fresh download rejected; cached validation accepted. This is not a reduction
of unknown-download safeguards or a bypass of image validation.

## Physical permission and authentication

User confirmed the physical checkbox visibly toggles and was asked to leave it
disabled. Read-only loopback packet summary confirmed permission POSTs contain
118 body bytes and receive HTTP 200 status bodies. The pairing PIN generated
physically by the user established a normal browser session; its cookie is held
only in the host test process RAM. Authenticated status reports installed
9.0.2 r17146 and `radioPermission:false`. No synthetic device permission was sent.

## Real-device transfer and reliability failure

The exact laptop-inspected image was being pre-staged by SCP directly into the
private directory on existing Extended Storage, not RAM and not a UBI device.
This was intentionally not represented as a production HTTP-fetch test, and no
LAN URL allowlist or SSRF bypass was added. Device opt-in stayed disabled.

During transfer the device became unreachable, then returned with fresh uptime.
Its boot log starts at 12:28:20 local Radio time. Only 10,854,400 of 16,032,273
bytes were present. No native installer, flash command, reboot command or
Jive restart had been invoked during this transfer. The cause is UNKNOWN;
watchdog, OOM, kernel/storage failure and network/power causes have not been
distinguished. The old `/var/log/messages` was RAM-backed and did not survive.
It would be incorrect to label this a proven OOM or proven UBIFS bug.

The stalled laptop sender was stopped. Fresh fd inspection found no reader of
the partial image, runtime firmware state was `0`, and that exact incomplete
test file was removed. It is recoverable from the unchanged laptop image.
No automatic retry was made. After removing the partial file, free space was
35,216 KiB (UBIFS accounting/GC can change slightly between snapshots).
Final verification after the webserver-only restart: uptime continuous at
four minutes in this boot, `sbwebserver` running, runtime firmware state `0`,
129 UBI LEBs still free, and filesystem free space 35,312 KiB (34.48 MiB).

No real-image inspector result on the physical Radio was obtained: the
read-only inspector was scheduled only after complete transfer and never ran.
No real image was advertised and no native firmware UI was opened.

## Integration results

| Area | Result / limitations |
| --- | --- |
| Web authentication | PASS: user-generated physical PIN established normal cookie session; cookie kept only in host RAM. Restart invalidated it as designed. |
| Installed identity | PASS: authenticated API reports 9.0.2 r17146, permission false. |
| Invalid configuration | PASS on device: `file:///etc/passwd` rejected HTTP 400, no configuration change. Host tests cover additional invalid/missing digest, Origin, unauthorized and read-only cases. |
| Valid source save/reload and Check | Host PASS; real-device workflow incomplete because transfer/reset and independent authenticity blockers. No misleading SourceForge wrapper URL configured. |
| Checkbox | User confirmed visible physical toggle. Later confirmed previously. Persistence/revocation/timeout on this physical device remain incomplete. |
| Heartbeat | Read-only wire summary confirms permission POST/HTTP 200. Runtime identity is present. Lua response/status-display behavior still needs end-to-end validation. |
| Virtual input isolation | Host callback/bridge tests pass; no simulated action can toggle the host fixture. Full queued/slow real-device acceptance is incomplete. |
| Comet | Host PASS: identity, expiry, availability, revocation, reconnect replay, dedup and unsubscribe. Real candidate advertisement/reconnect pending. |
| Native optional UI | Source-supported service/alias design; real-image Review/Back/optional screen NOT tested in this phase. Begin Update never selected. |
| Safe local HTTP delivery | Host synthetic exact-byte/length and access checks pass; real-image device delivery not reached. Slow-reader retention and transport-timeout cases remain pending. |

The corrected validator/staging webserver was subsequently deployed by atomic
file replacement and restarting **only sbwebserver through the existing applet
supervisor**, with firmware runtime state disabled. No Jive/watchdog/init script
or storage/UBIFS code was changed. No additional large transfer attempted.

## Builds and automated tests

- Complete final host suite: **46/46 passed**, 140.95 seconds, serial execution.
- New coverage: large low-compression deflate buffer boundaries; 5 MiB simulated
  free-space cached/fresh distinction; actual 45-second permission expiry;
  Comet reconnect/dedup/revocation/unsubscribe; earlier module-scope and checkbox
  callback/style regressions remain included.
- Static ARMv5 build: PASS. Existing image-render path-truncation and third-party
  NanoSVG signedness warnings remain; no new firmware compilation warning.
- Corrected ARM validator under QEMU on the unchanged real image: PASS,
  `9.0.2 r17157 3191832 14598144`.
- An intermediate HTTP test failed because the new insufficient-space test
  correctly created the private directory before refusing the download. Fixed
  test setup to allow that existing directory; final full suite passed.

## Resource measurements and remaining risks

| Measurement | Physical-device result |
| --- | --- |
| Cleanup/free space | Measured: 12,688 KiB reclaimed; 34,980 KiB immediately after cleanup; 35,216 KiB after aborted test cleanup. |
| Download/preparation duration | Not completed. SCP is not the production download path. |
| Peak preparation RSS/CPU | Not measured: inspector never ran. Post-reboot free memory is not a peak metric. |
| Local firmware HTTP throughput | Not measured on real image. |
| UBI headroom | Read-only arithmetic PASS, 119 LEB payload margin before Base cushion. |
| Jive/audio during preparation | Not validated; user confirmed responsive after device returned. No playback change requested by this test. |
| Unexpected reset | CONFIRMED during SCP; root cause unknown. Old boot logs unavailable. |
| OOM | Not proven or excluded; previous-boot evidence lost. |
| Flash writes/temp leaks | No firmware-volume writes. Normal filesystem archives/read checks and partial image staging only; incomplete image removed. |

Before another large transfer, collect continuous boot-ID/uptime, process RSS,
memory/dirty/writeback, CPU and live kernel/syslog evidence on the laptop. Use
a bounded/throttled transfer with a channel timeout; do not disable watchdog,
change kernel/UBIFS behavior or blindly repeat an operation that reset the Radio.
Do not attempt installation as a diagnostic test.

## Installation readiness decision: NOT READY

Blockers:

1. Unexplained reset during a non-installing image transfer.
2. No independently authenticated publisher digest/signature established.
3. No complete real-device preparation/inspection/delivery result.
4. Real-device native optional UI, persistence/revoke/timeout and comprehensive
   remote-input boundary tests incomplete.
5. Peak resource usage, audio impact, slow-reader and transport-timeout tests
   incomplete.

Storage capacity alone is now sufficient: fresh worst-case staging PASS and
read-only UBI allocation headroom PASS. These do not remove the blockers above.
Do not select Begin Update or authorize installation based on this report.

## Explicitly requested reproduction retry

One additional SCP attempt was authorized by the user and performed with live
syslog and two-second resource snapshots streamed to the laptop, so evidence
survived the reboot. Same source bytes, destination filesystem and unthrottled
SCP workload; sender used bounded 32 KiB application chunks and a 30-second
channel timeout, with no automatic retry. Monitoring adds some diagnostic load.
Firmware remained disabled; inspector and installer were never invoked.

**Reproduced: watchdog-triggered shutdown during transfer.** Captured timeline
(Radio local time, 2026-10-25):

- 12:40:13: preflight uptime 12 minutes, boot ID
  `70046068-fb1c-4d38-8efc-d36d2fa688de`, free storage 35,312 KiB.
- 12:41:08: `watchdog[707]: semaphore 819:0 was not changed.` PID 819 was
  the running Jive process, still listed in the watchdog's process dump.
- 12:41:10: `repair binary returned 2`, then
  `shutting down the system because of error 2`.
- 12:41:14: Jive logs `SIGQUIT`; SCP connection subsequently closed.
- 12:42:21: Radio reachable at .228 with fresh uptime and boot ID
  `93002fdd-9f55-4b72-831b-f96f189b20da`.

Device file after reboot: 12,230,656 bytes versus expected 16,032,273.
The incomplete file had no open reader, firmware runtime state was `0`, and
that exact partial file was removed; original complete source remains on laptop.
No third transfer attempted. No reboot command or storage/UBI change executed.

Observed minimum `MemFree` across 22 successful samples: 1,484 KiB, maximum
9,892 KiB. These are free pages, not available/reclaimable memory or proof of
OOM. No OOM-kill/kernel panic/storage error appeared in captured live messages.
The proven immediate failure is the unchanged Jive watchdog semaphore, not a
firmware-container or native-installation error. Why Jive stopped advancing its
heartbeat remains unproven (CPU starvation, blocking work or another problem).
Concurrent external/local LMS activity and existing player-notification errors
are also present; correlation with transfer does not isolate those factors.

Private evidence: `.local/phase2b/retry-20261025T114012Z/` contains
`messages.log`, `samples.jsonl`, and `result.json`. Raw diagnostic evidence is
ignored by Git. Further work should trace Jive heartbeat progress and blocked
stacks under transfer/write load; do not disable the watchdog as a workaround.
