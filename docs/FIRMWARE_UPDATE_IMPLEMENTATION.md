# Custom firmware integration — Phase 2 development status

This accompanies `FIRMWARE_UPDATE_ARCHITECTURE.md`, whose Phase 1 conclusions
remain the source investigation, not a claim of completed device validation.
Actual installation is explicitly excluded from Phase 2 testing.

Latest follow-up: [Phase 2B preflight report](FIRMWARE_PREFLIGHT_PHASE2B.md).
Storage cleanup and UBI capacity passed, but an unexplained reset during image
pre-staging blocks installation readiness. Final host suite 46/46 and ARM build
passed; corrected validator/staging webserver deployed without restarting Jive.
Real-device preparation and native optional UI validation remain incomplete.

## Components and flow

```text
Paired browser: source URL + trusted SHA-256
    -> sbwebserver firmware module
    -> bounded preparation worker -> existing HTTP/sbproxy transport
    -> streaming ZIP/checksum/board/version validation
    -> existing mounted Extended Storage: firmware/<sha256>.bin

Physical raw KEY_GO -> Jive persistent default-off checkbox
    -> existing internal capability -> expiring native permission
    -> private RAM descriptor -> sbbase firmwareupgrade/Comet subscriptions
    -> optional Review Update -> public SetupFirmwareUpgrade service
    -> versionless loopback HTTP image (no flashing implementation in Base)
```

The browser can save configuration and request preparation, but cannot enable
permission or install. SourceForge outer distribution ZIPs are unsupported;
configure a direct native image URL. A separately trusted SHA-256 is mandatory,
including for HTTPS: this does not change sbproxy's TLS compatibility policy.

## API and persistence

- `GET /api/firmware/status`: paired-browser status.
- `PUT /api/firmware/config`: URL and lowercase SHA-256; existing write/Origin
  authorization applies. Read-only mode denies changes and preparation.
- `POST /api/firmware/check`: start one asynchronous preparation job.
- `POST /api/internal/firmware/permission`: existing loopback internal capability,
  installed version, baby machine/board, physical-support flag and ordered
  permission sequence. No browser session authorizes this endpoint.
- `POST /api/internal/firmware/review`: same internal authentication, current
  eligibility and headroom; returns the optional native UI adapter URL.
- `GET /firmware/<sha256>/custom.baby.bin`: exact current candidate, loopback
  source and Host, no Origin, browser fetch metadata or Range. No redirect.

Configuration is private `firmware.json` under the existing webserver config
directory. Runtime permission/candidate validity are never restored as trusted
from disk. After a service restart, Jive must refresh identity/permission and a
check must revalidate any cached image. Jive's checkbox itself persists in its
normal applet settings.

Permission expires after 45 seconds without a heartbeat (normally every 10
seconds). Ordered requests prevent a delayed old enable from undoing disable.
The Virtual Radio input lock intentionally remains latched after heartbeat loss
until an authenticated device disable, protecting a potentially open native
update screen. Capture is not disabled by this feature.

## Resource and safety limits

One forked worker; maximum native image 24 MiB; download deadline 300 seconds;
worker deadline 360 seconds; 28 MiB staging free-space requirement. Streaming
inflate and checksum buffers are bounded; no image-wide RAM allocation.
Metadata is bounded to 8 KiB per entry; kernel/root expanded limits are 8/32 MiB.
Exactly five ordered entries and consistent central/local directory data are
required. ZIP64, encrypted files, descriptors, extra entries and trailing data
are rejected. Supported bounded extra-field TLVs allow ordinary Unix ZIP tools.

Only existing writable Extended Storage is used. No mount, module, initialization,
volume or recovery behavior is changed. Preflight reads current identity,
boot command line and native UBI available/reclaimable backup block counters.
Unknown identity/headroom fails closed. Its conservative eight-block margin is
not a guarantee of installation success and needs physical-layout validation.

Obsolete digest-named regular candidates are removed only from the private
firmware staging directory. Open file readers retain their original descriptor
on POSIX unlink; no path is overwritten with different bytes. Cached bytes are
fully revalidated before advertising. Config changes revoke advertisements.

The native downloader may fetch after opting in through its existing Advanced
menu, not only after Base's Review RPC: the read-only download URL itself grants
no installation authority. Blocking download until Review would be unsafe,
because the native updater can remove old backups before starting HTTP.

## Native UI and input boundary

Only raw hardware KEY_GO on the foreground Base menu can toggle permission or
select Review. Ordinary action callbacks cannot do either. The listener runs
before the stock key-to-action mapper; RemoteBridge injects action/scroll events,
not raw key presses. Both native queue delivery and Lua response injection are
blocked while enabled. Hardware behavior still requires device confirmation;
host stubs alone do not prove physical security.

Review uses public `firmwareUpgrade(server)` with `getUpgradeUrl()` returning
the versionless alias and false force flag. Stock service returns nil after
showing its window, so the controller observes the public window stack. It does
not call private flash methods or synthesize Begin Update. Base supervision is
suspended while its native review flow is active. Candidate invalidation closes
the review window only when native installation has not started.

## Verification status (development, 2026-10-25)

- Full 45-test host suite: passed, including the new firmware tests.
- New synthetic image validator, physical-key/permission sequencing, Comet
  identity/expiry and HTTP preparation/delivery tests: passed individually.
- HTTP restart tests also confirm configuration persists but prior sessions,
  permission and trusted candidate state do not. ARM/QEMU validator tests pass.
- Final static ARMv5 build: passed. Existing image-render/NanoSVG compiler
  warnings remain; firmware validator signedness warnings were corrected.
- Read-only inspection of 192.168.1.228: firmware 9.0.2 r17146, Baby revision 5,
  normal cramfs boot, existing sbubifs Extended Storage mounted writable.
- Existing Extended Storage has 22,468 KiB available, below the 28 MiB staging
  minimum. No files were deleted or storage layouts changed to work around it.
- Development application package deployed to .228 (not a public release).
  Initial device startup exposed a restricted Lua-module `require` lookup in
  RemoteBridge. Fixed by importing JSON before entering `module(...)`; a new
  real Lua 5.1 module-sandbox regression test passes.
- The device watchdog logged repair failure/shutdown after the SqueezePlay
  restart, and .228 became unreachable. No reboot command or native installer
  was invoked. The corrected RemoteBridge could not yet be uploaded; device
  state/IP confirmation is required before recovery and continued validation.
- Follow-up: user confirmed .228 remained its address. Corrected RemoteBridge
  was uploaded there. A direct init-script restart over SSH again triggered the
  watchdog shutdown; no further restarts were attempted. Device returned with
  fresh uptime, Jive and native services running and no applet-evaluation error
  in the new boot log. Physical menu confirmation and permission HTTP response
  validation remain pending. Do not repeat direct SSH init-script restarts;
  investigate the watchdog/session lifecycle before any further restart.
- Physical menu follow-up: Jive `Menu:getSelectedItem()` returns the rendered
  widget, not SimpleMenu entry data. Fixed hardware callback lookup to use
  `getItem(getSelectedIndex())`, with a regression stub matching the real API.
  Updated Lua deployed to .228. Restart detached with nohup and redirected
  standard streams succeeded; uptime remained continuous and Jive stayed alive.
  Earlier direct SSH restarts appear session-sensitive; no init/watchdog code
  was modified. User confirmation of checkbox and Later behavior remains pending.
- User confirmed Later works. The enable row lacked Jive's `item_choice` style,
  so the skin hid its existing checkbox widget. Added the standard choice-row
  style and host tests for its widget, physical toggling and ordinary-action
  rejection; deployed the Lua correction to .228.
- Verified pre-deployment applet/config backup is kept locally, ignored by Git,
  at `.local/phase2/radio-before-phase2.tar.gz`.
- Real source download, physical checkbox, native optional window, Later/Back,
  headroom eligibility and measured preparation memory/CPU: pending.
- Firmware installation, firmware UBI writes and firmware reboot: NOT tested,
  NOT authorized and NOT performed.

The supplied SourceForge `/download` URL is an outer wrapper. A direct inner
`.bin` URL and independently trusted SHA-256 are still needed for a real-source
preparation test. Do not authorize installation from synthetic fixtures.

## Build and safe device acceptance

Use the existing host CMake/CTest workflow (serial tests share fixed ports),
including `firmware_image`, `firmware_physical_input`, `firmware_comet`, and
`firmware_http`. Run `scripts/build-arm.sh` with the existing SDK/image/TLS
prefixes. Package development builds in a private output directory: do not
overwrite the published 0.6.1 release ZIP with development bytes.

Before deployment back up the current applet/config. Deploy only application
changes with the existing deployment script. Then check authenticated browser
configuration, disabled-by-default state, physical checkbox persistence,
Virtual Radio rejection, real-image validation and exact local delivery. Open
the native optional UI and test Later/Back only. STOP before Begin Update.

Further automated coverage is still needed for subscription reconnect/dedup,
permission timeout, active slow readers and transport
timeouts. Until these and physical tests pass, treat this as development code,
not an approved firmware-installation path.
