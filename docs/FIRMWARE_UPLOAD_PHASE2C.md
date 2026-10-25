# Phase 2C — browser firmware upload (development)

## Scope and implementation

No firmware installation, native flash changes, UBI changes, storage layout,
mount, kernel module, watchdog or Jive scheduling changes are part of this work.
URL downloading remains available. Upload and URL jobs share the existing
firmware module, inspector, single worker slot, candidate and Comet handoff.

Changed application files: `native/sbwebserver/src/firmware.{c,h}`,
`http_server.c`, `web/js/firmware.js`, `web/index.html`; host coverage in
`tests/firmware/test_upload.py` and `tests/CMakeLists.txt`.

## Browser/API

Upload is the default source choice. File bytes are sent using XMLHttpRequest
for progress/cancel support, no browser-side image hashing or transformation.
The user must supply the expected SHA-256 separately. URL save/check remains in
the second source panel; URL digest is stored separately from upload digest.
The browser cannot enable Radio permission or install firmware.

The distribution's bundled `.bin.sha` contains SHA-1 (40 hexadecimal digits),
not SHA-256 (64 digits). Do not pad, truncate or hash that checksum text to turn
it into a different algorithm. On Windows use
`Get-FileHash -Algorithm SHA256 .\baby_9.0.2_r17157.bin` on the extracted native
image. This is a local integrity pin, not independent publisher authentication.

`POST /api/firmware/upload` accepts only:

- Existing paired-browser session, permitted writes and same-origin request.
- `Content-Type: application/octet-stream`.
- Exactly one decimal `Content-Length`, between 1 byte and 24 MiB.
- `X-Firmware-SHA256`: explicit lowercase 64-digit expected digest.

Missing Content-Length fails 411; chunked/ambiguous framing is rejected rather
than buffered. Absolute limits also apply to accepted bytes, not only headers.
No browser filename is used as a path. Known length + 4 MiB reserve is required;
existing candidates already count against actual free space. Unknown URL fetches
retain the existing 24 MiB + 4 MiB reservation.

## Streaming and candidate lifecycle

After header authentication, the module detaches Mongoose's whole-body parser.
A bounded receive queue feeds a nonblocking UNIX socket to one forked worker;
full queues pause socket reads and propagate TCP backpressure. The worker batches
32 KiB writes, handles short writes/EINTR, incrementally hashes and checks free
space. The complete image never resides in Radio RAM or `/tmp`.

Staging uses only the existing active Extended Storage mount:
`/mnt/sbdata/firmware/upload.tmp`, private mode 0600 under a 0700 directory.
No fsync occurs per chunk. One final fsync precedes the existing streaming ZIP,
CRC, MD5, board/version and SHA validator. Parent verifies current identity,
newer-version policy and UBI headroom, then atomically renames to `<sha256>.bin`.
The inode isn't repacked or copied. Existing open readers retain their bytes.

During preparation advertisements are suspended. Failed/cancelled uploads retain
the previous known-good candidate and restore its eligibility; a successful
replacement removes the obsolete path after publication. Permission is separate
and is never granted by upload. Parent-death handling kills the upload child;
startup removes the bounded stale temporary path. Cached images are untrusted
after restart and must be revalidated before advertisement.

Idle deadline 30 seconds; upload deadline 600 seconds; bounded worker deadline
660 seconds. Closing/aborting the browser cancels the job and clears its temporary
file after the child is reaped. Progress is RAM-only. Main-loop file metadata
operations remain small; image writes, fsync and full inspection run in worker.

Optional local process setting `SB_FIRMWARE_UPLOAD_RATE_KIB` (8–1024) provides
an explicit test rate cap through the event loop's token budget, not sleeps.
Default is uncapped with worker backpressure. The first physical test uses
64 KiB/s to reduce load relative to the failed roughly 200 KiB/s SCP transfers.
This is a controlled experiment, not an assertion that pacing fixes the cause.

The existing loopback alias/physical Review flow is retained. A compatibility
correction permits `Host: 127.0.0.1` on port 80 because Jive omits default ports;
nondefault ports still require the exact host and port. No arbitrary hosts,
browser-origin downloads or remote LAN firmware delivery are permitted.

## Review of Phase 2B evidence before another large transfer

Source: private `.local/phase2b/retry-20261025T114012Z/` files, read before test.
22 successful snapshots show:

| Metric | Observed |
| --- | --- |
| MemFree | 1,484–9,892 KiB |
| Cached | 20,584–29,424 KiB |
| Dirty | 0–3,608 KiB |
| Writeback | 0–4 KiB |
| Jive RSS peak | 15,272 KiB |
| Webserver RSS peak | 372 KiB |
| Jive sampled CPU peak | about 30.2% |
| Webserver sampled CPU peak | about 2.5% |

CPU estimates above use /proc uptime windows and 100-Hz process ticks, not
perfect physical CPU accounting. Sampling may miss short peaks. No MemAvailable
or swap is provided by this target. Free memory alone is not proof of OOM.

Last normal Jive HTTP activity was at 12:40:52; watchdog reported unchanged Jive
semaphore at 12:41:08 and shutdown at 12:41:10. Jive was still present. The log
also shows external/local LMS switching and player-free errors. No OOM kill,
kernel panic or filesystem error was captured. Underlying stall cause remains
unknown; do not assume HTTP removes this risk or blame a filesystem from timing.

## Controlled physical test plan

Run host suite and static ARM build before deploying only application files.
Back up current application files first; restart only the native webserver,
not Jive. Set explicit 64 KiB/s test cap and verify health/uptime/disabled opt-in.
Use user-generated physical pairing PIN and keep session token only in host RAM.
An automated HTTP client may exercise the identical octet-stream endpoint.

Monitor off-device: boot ID/uptime, Jive and parent/worker PID/state/CPU/RSS,
system CPU, /proc/meminfo, dirty/writeback and swap counters, upload progression,
live system/kernel messages. Read-only IPC snapshot uses IPC_STAT/GETALL on
existing key 1234; raw semaphore values and operation/change times are recorded
without altering watchdog state. These values alone do not prove exact heartbeat
age; retain the underlying records and avoid overstating their meaning.

Use the unchanged 16,032,273-byte r17157 native image and explicit integrity pin
`dc783d525e0ea17736f36ff51d1424e6d8909cc5a9ba51f0cfdcacef56d65d6d`.
This identifies the previously inspected file; it is not a publisher signature.
No SCP firmware payload, no automatic retry after a reset, no Begin Update.

## Build and host validation results

- Final full host suite: **47/47 passed**, 144.12 seconds, serial execution.
- Static ARMv5 build: PASS. Existing image-render path and third-party NanoSVG
  warnings remain; no new firmware compilation warning.
- Upload regression coverage includes authentication, Origin/read-only checks,
  missing/ambiguous/oversized lengths, streaming overflow, absent/bad digest,
  short/corrupt/wrong-board/equal/older images, concurrent operation rejection,
  cancel/disconnect, idle timeout, slow upload, reader retention across atomic
  replacement, startup temporary cleanup and uploaded-candidate revalidation.
- A test initially attempted to turn read-only mode off through an API that
  intentionally denies such writes. Test ordering was corrected, not the
  authorization policy; the final full suite passed.
- Firmware UI JavaScript syntax compilation: PASS.

## First physical HTTP upload: PASS at explicit 64 KiB/s cap

Application-only deployment and webserver-only restart preserved Jive uptime.
The running process environment was checked to confirm the 64 KiB/s test cap.
Initial expired pairing code was rejected before any upload; a fresh user-made
physical PIN established the real browser session. Session token remained in
host process RAM. One actual payload upload was made through the same paired,
Origin-protected octet-stream endpoint used by the browser. No SCP image payload
was used, and no retry was made.

| Measurement | Result |
| --- | --- |
| Image | Unchanged native baby_9.0.2_r17157.bin |
| Received bytes | 16,032,273 |
| Expected SHA-256 | dc783d525e0ea17736f36ff51d1424e6d8909cc5a9ba51f0cfdcacef56d65d6d |
| HTTP result | 200, state ready, candidateReady true |
| Installed / available | 9.0.2 r17146 / 9.0.2 r17157 |
| Hardware | Baby revision 5 accepted by shared inspector |
| Radio permission / native eligibility | false / false (no advertisement) |
| Upload plus validation duration | 261.14 seconds |
| End-to-end average | 61,393 bytes/s (59.95 KiB/s), including validation |
| Validation-only duration | Not independently timed; do not infer exact value from rate cap |
| Peak sampled parent RSS | 564 KiB, PID 2154 |
| Peak sampled worker RSS | 484 KiB, PID 9446 |
| Peak sampled Jive RSS | 15,208 KiB |
| MemFree | 1,436–9,560 KiB; Cached 20,772–31,060 KiB |
| Dirty / Writeback peaks | 2,240 KiB / 4 KiB |
| Swap / MemAvailable | No swap; MemAvailable absent on target kernel |
| System busy peak | 100% in sampled counter windows; not sustained utilization |
| Watchdog/reset | No watchdog error/reset captured; unchanged boot ID |
| Remaining storage | 19,720 KiB at post-upload verification |

There were 79 successful sampled snapshots. The same boot ID
`93002fdd-9f55-4b72-831b-f96f189b20da` was present before, during and after.
The complete mode-0600 digest-named candidate is retained on Extended Storage;
`upload.tmp` was atomically replaced. Runtime firmware state remained `0` while
disabled, despite successful validation. No native installer or firmware writes
were invoked. No continuous background telemetry was installed on the Radio.

**CPU measurement limitation:** sequential SSH snapshot windows combined with
this old kernel's accounting produced impossible per-process peak percentages
(over 100% on one core). These were explicitly marked unusable, not clamped or
reported as valid CPU peaks. Accurate aligned per-process CPU measurement is
still outstanding. RSS numbers are sampled resident pages, not guaranteed exact
microsecond peaks. Jive/audio responsiveness was not audibly confirmed by user.

Private evidence: `.local/phase2c/http-20261025T152827Z/` contains live logs,
samples, result and measurement summary. It is Git-ignored. The checksum-help
clarification was deployed as a UI-only file change after the controlled upload.

## Remaining native acceptance and readiness: NOT READY

The user confirmed the corrected Review Update item opens the native screen
offering Begin Update, then returned and disabled permission as requested.
No synthetic input has been used to grant permission or launch the native UI.
The disabled loopback download was tested through an SSH direct-tcpip channel
and correctly returned HTTP 403. After the user physically re-enabled permission,
the exact same endpoint returned HTTP 200 with Content-Length 16,032,273.
All 16,032,273 bytes were received and their SHA-256 matched
`dc783d525e0ea17736f36ff51d1424e6d8909cc5a9ba51f0cfdcacef56d65d6d`.
This read-only download was paced at 64 KiB/s on the diagnostic client, used
Host `127.0.0.1` (default port), and left the boot ID unchanged. It invoked no
updater and created no additional on-device firmware copy. Private evidence:
`.local/phase2c/delivery-enabled.json` and `delivery-disabled.json`.
Still outstanding:

- Physical enable reached the native service: its runtime state contained the
  validated candidate's optional loopback alias. Comet acceptance remains pending.
- Aligned CPU measurement, explicit audible/responsiveness checks.
- Uncapped operation is untested; this successful test does not prove HTTP alone
  fixes the earlier stalls or make unrestricted-speed upload safe.
- The supplied digest is an integrity pin, not an independently authenticated
  publisher signature; no silent authenticity claim is made.

Do not select Begin Update. No actual installation is authorized by these results.

### Menu refresh correction

Device testing found that Review Update was created only during initial menu
construction, before the asynchronous permission response. The open menu now
updates eligibility after each response (including removing Review on errors),
retains row objects on unchanged replies, and requests fresh status on opening.
Review still requires the physical key callback; the normal action is inert.
The Lua regression test covers late eligibility, no duplicate rows, revocation,
header updates, and action isolation. It passes.

After the user disabled permission, runtime state `0` was verified before and
after installing the single Lua file. A detached SqueezePlay restart loaded the
change; the operating-system boot ID was unchanged. No firmware installation,
storage operation or permission enable was invoked. The user subsequently
confirmed Review Update appears and opens the native confirmation screen.
