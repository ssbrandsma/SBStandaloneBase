# Firmware update architecture: Phase 1 investigation

Date: 25 October 2026. **Research and design only; nothing implemented.**

## A. Executive summary

The feature is feasible without another daemon or a Standalone Base NAND
installer. Reuse SqueezePlay's `SetupFirmwareUpgrade` applet and its existing
UBI updater. Base should validate and stage a candidate, enforce the physical
device opt-in, and expose an immutable local download. The stock updater must
remain responsible for flashing and rebooting after the user's confirmation.

Important corrections to the initial assumptions:

1. **The Radio discovers firmware through the HTTP/Comet `firmwareupgrade`
   command, not `UREQ`, `upda` or `updn`.** Those SlimProto messages belong to
   the older player firmware transfer path. Base already returns a disabled
   placeholder for the correct command.
2. SourceForge's `baby_9.0.2_r17157.zip` is a **distribution wrapper**. It
   contains a `.bin`, two SHA-1 sidecars and `baby.version`. The `.bin` is
   itself the ZIP container that the native updater consumes. Do not hand the
   wrapper to the updater or merely rename it.
3. PR #37 is confirmed merged. The inspected r17157 artifact reports that merge
   as its base build revision. The patch fixes ordinary UBIFS backing-device
   naming; it does **not** add the `sbubifs` filesystem name that Base currently
   requires. Current Base will not automatically stop loading its module.
4. The native updater is not a download-then-flash installer: it removes old
   backup volumes before downloading, then streams into new UBI volumes. Its
   internal MD5 checks happen after writes. Prevalidation/staging therefore
   materially improves safety.
5. Update UI behavior differs by firmware. Stock 7.7 trusts the server's
   `firmwareUpgrade` flag. Community 9.0 recomputes its force flag from a
   versioned filename, even if the server sends zero. A new versioned URL can
   open a non-dismissable prompt, although installation still requires Begin
   Update. Optional notifications need an explicit compatibility strategy.
6. The existing updater accepts HTTP or `file:`; it does not implement HTTPS.
   Base's compatibility proxy can transport HTTPS, but disables certificate
   verification. It is **not sufficient to authenticate firmware** by itself.

Recommendation: begin with a trusted metadata feed and/or a directly supplied
native `.bin`, a pinned SHA-256 obtained through a trusted channel, bounded
staging and full preflight. Add SourceForge-wrapper import later. Use the
existing Comet command and native optional update UI, not new SlimProto flash
messages. Do not offer an update if authenticity, compatibility, staging space
or UBI headroom cannot be established.

The laptop LMS is not needed to establish this architecture. It would be useful
later as a controlled reference-server test; do not connect a real Radio to an
advertised candidate merely to capture a query.

## Evidence, scope and reproducibility

Only this document was created. No application source, device files, firmware,
configuration, storage or kernel modules were changed. No Radio was contacted,
no firmware was downloaded onto a Radio, and nothing was flashed, deployed,
rebooted or released. A SourceForge archive was read **in laptop process RAM**
and inspected with Python `zipfile`/`hashlib`; no archive or extraction files
were created. Source downloads were inspected in memory as well.

Source snapshots:

| ID | Repository / revision | Use |
| --- | --- | --- |
| LMS | `LMS-Community/slimserver`, `c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7` | Existing local `.local/upstream-lms` snapshot; missing worktree files read from Git/source URLs |
| SP9 | `ralph-irving/squeezeos-squeezeplay`, public/9.0, `766b6a8b3c3c7722d586125b068b47facf9d4a77` | Community embedded updater and protocol source, fetched read-only |
| SP7 | `LMS-Community/squeezeplay`, public/7.7, `cca1bba4abeb3fef2b7c4a48a4e4da8fc255666d` | Stock-branch update flag comparison |
| OS | `ralph-irving/squeezeos`, PR #37 merge `13014bea90f52b3d14ad90edfae180009b40c213` | UBIFS patch; release build provenance |
| Plugin | `ralph-irving/plugin-CommunityFirmware`, `73975827726552a90da73141f3bc1d39864fdd12` | Stable/beta repository selection |
| Base | `70fa144a7a94d0c07acc9c61dc265efb51614c2a` | Current local Standalone Base architecture |

An additional local checkout at `C:/Projects/Squeezify/upstream/squeezeplay`
(`9bad3c1f55f45575d985c37d73c4f1513c5a64d7`) was useful for navigation. Its
updater and `UpgradeUBI.lua` match SP9, but its `SlimServer:isMoreRecent()`
contains a lower-component check absent from SP9. It must not be silently
treated as the exact r17157 source. `C:/Projects/squeezeos2` is a locally modified
fork; the merged public patch and release metadata were checked independently.

### Source reference index

Line numbers below refer to these pinned snapshots, not an unversioned master.

- [LMS Firmware.pm](https://github.com/LMS-Community/slimserver/blob/c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7/Slim/Utils/Firmware.pm):
  `init_firmware_download` 93, custom pair 100–114, `init_version_done` 145,
  `init_fw_done` 211, `url` 288, `need_upgrade` 319, `downloadAsync` 360,
  SHA validation 433, retry/error handling 486.
- [LMS Jive.pm](https://github.com/LMS-Community/slimserver/blob/c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7/Slim/Control/Jive.pm#L2195):
  `firmwareUpgradeQuery_filter` 2195; `firmwareUpgradeQuery` 2208.
- [LMS Slimproto.pm](https://github.com/LMS-Community/slimserver/blob/c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7/Slim/Networking/Slimproto.pm):
  `UREQ` dispatch 69, `_update_request_handler` 886, HELO 964,
  legacy `ureq` send 1231.
- [LMS Squeezebox.pm](https://github.com/LMS-Community/slimserver/blob/c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7/Slim/Player/Squeezebox.pm):
  `upgradeFirmware_SDK5` 395, `upda` 456, `updn` 488, `sendFrame` 1140.
- [LMS SqueezePlay.pm](https://github.com/LMS-Community/slimserver/blob/c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7/Slim/Player/SqueezePlay.pm#L123):
  firmware accessor/revision 123–132, empty `needsUpgrade` override 144.
- [SP9 SlimServer.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay/share/jive/slim/SlimServer.lua):
  migration guard immediately before `_upgradeSink` 359; `_upgradeSink`
  359–426; firmware subscription 549–562; `isMoreRecent` 1314–1325;
  `getUpgradeUrl` near 1415.
- [SP7 SlimServer.lua](https://github.com/LMS-Community/squeezeplay/blob/cca1bba4abeb3fef2b7c4a48a4e4da8fc255666d/src/squeezeplay/share/jive/slim/SlimServer.lua#L354):
  `_upgradeSink` 354–392, server flag assignment 385.
- [SP9 SetupFirmwareUpgradeApplet.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay_squeezeos/share/applets/SetupFirmwareUpgrade/SetupFirmwareUpgradeApplet.lua):
  `notify_firmwareAvailable` 72, `_firmwareVersion` 88,
  `_versionCompare` 102, `_findServerUpgrade` 118, `_findUpgrades` 138,
  `_upgradeWindowSingle` 240, `firmwareUpgrade` 369,
  `showFirmwareUpgradeMenu` 400, `_checkBattery` 417, `_t_upgrade` 532,
  `_upgrade` 554, `_upgradeFailed` 629.
- [SP9 SetupFirmwareUpgradeMeta.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay_squeezeos/share/applets/SetupFirmwareUpgrade/SetupFirmwareUpgradeMeta.lua):
  services/menu 25–35; `notify_playerCurrent` 54–65.
- [SP9 Upgrade.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay_squeezeos/share/applets/SetupFirmwareUpgrade/Upgrade.lua):
  `start` 25, `parseMtd` 56; UBI/MTD backend selection.
- [SP9 UpgradeUBI.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay_squeezeos/share/applets/SetupFirmwareUpgrade/UpgradeUBI.lua):
  `_upgrade` 58–154, `parseCmdLine` 158, `parseCpuInfo` 175,
  `verifyPlatformRevision` 200, `parseUbi` 238, `upgradeSink` 289,
  `download` 395, `updatevol` 459, `renamevol` 498, `rmvol` 526,
  `checksum` 547. Function names are the authoritative lookup if line offsets
  differ in another firmware.
- [SP9 UpgradeMTD.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay_squeezeos/share/applets/SetupFirmwareUpgrade/UpgradeMTD.lua):
  erase/write/verify/switch flow 48–130; `upgradeSink` compatibility before
  `nandwrite` 213–249; `parseMtd` 312; `fw_setenv` 368.
- [SP9 RequestHttp.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay/share/jive/net/RequestHttp.lua#L339):
  streaming responses and redirects 339–409.
- [SP9 SlimProto.lua](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay/share/jive/net/SlimProto.lua):
  HELO 110–164, incoming server frame 490 onward, outbound packet 725–738.
- [SP9 system.c](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay/src/ui/system.c):
  `system_get_version` 30, `system_lua_get_machine` 72;
  [jive.c](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay/src/jive.c)
  exports compiled `JIVE_VERSION` to Lua.
- [CommunityFirmware Plugin.pm](https://github.com/ralph-irving/plugin-CommunityFirmware/blob/73975827726552a90da73141f3bc1d39864fdd12/Plugin.pm#L70):
  repository and beta selection 74–90.
- [SqueezeOS packaging class](https://github.com/ralph-irving/squeezeos/blob/13014bea90f52b3d14ad90edfae180009b40c213/poky/meta-squeezeos/classes/squeezeos-upgrade-image.bbclass):
  `do_squeezeos_image`, `squeezeos_version`.
- [Merged PR #37](https://github.com/ralph-irving/squeezeos/pull/37) and its
  [UBIFS patch](https://github.com/ralph-irving/squeezeos/blob/13014bea90f52b3d14ad90edfae180009b40c213/src/imx25/patches/ubifs/0228-UBIFS-allow-multiple-volume-mounts.patch).

## B. Existing LMS firmware architecture

### Download, catalog and advertisement

`Firmware.pm` obtains the updates directory from OSDetect, not a hardcoded
cross-platform path. The custom override is the pair
`custom.baby.version` plus `custom.baby.bin` in that directory. Both must be
readable. The custom metadata first line is parsed as `VERSION rREVISION`.
The image is registered as a raw HTTP download. This override does not run
the normal remote download/SHA verification path; it trusts the administrator's
local files. A custom filename also hides its version from filename-based Jive
UI parsing. It is not a signature or proof of compatibility.

Otherwise, LMS obtains `baby.version` from its configured repository and
version folder, then obtains `baby_<version>_r<revision>.bin` if needed. Normal
downloads go to a `.tmp` file and are renamed only after their `.sha` matches.
The sidecar parser expects **40 hex digits (SHA-1)**. Download errors start at
600 seconds and back off to at most one day; 403/404 in the LMS-version folder
can fall back to `latest`. Normal successful metadata checks recur daily in
core, while CommunityFirmware overrides the interval from LMS settings.

The community plugin reuses this machinery. Its stable and beta roots are:

```text
https://ralph_irving.gitlab.io/lms-community-firmware/update/firmware/
https://ralph_irving.gitlab.io/lms-community-firmware-beta/update/firmware/
```

These are update repositories, distinct from the SourceForge distribution
ZIP directory. This investigation's verified Python HTTPS request to the stable
`latest/baby.version` failed hostname certificate validation. The beta endpoint
was not subsequently validated. Do not infer current channel contents or
weaken TLS verification from that failure. The directly inspected SourceForge
candidate is evidence of availability outside the channel, not proof that the
beta feed already advertises it.

`firmwareUpgradeQuery` takes `firmwareVersion`, `machine` and optional
`subscribe`. It returns a URL whenever a model's firmware is available, even
when it is not newer, to support the manual update menu. It uses
`relativeFirmwareUrl` for capable clients when LMS hosts the image, otherwise
`firmwareUrl`. `firmwareUpgrade` is 0 or 1. A model-matched `fwdownloaded`
notification re-executes subscribed queries.

Do not copy LMS's broad `need_upgrade` rule into a safety gate. It returns
true for **any differing release version**, or a larger revision in the same
release. It is not a robust no-downgrade policy.

## C. Protocol details and actual negotiation

### Radio update discovery is Comet/HTTP

When a `SlimServer` is created, hardware SqueezePlay subscribes to firmware
status through its existing Comet connection. Desktop `machine=squeezeplay`
is excluded. The application command is:

```json
["firmwareupgrade", "firmwareVersion:9.0.1 r17084", "inSetup:0", "machine:baby", "subscribe:0"]
```

The logical response subscription is `/slim/firmwarestatus`, with the ordinary
Comet client-specific response routing. Transport is LMS HTTP (normally port
9000), not binary SlimProto on 3483. For example, the result's data can contain:

```json
{"relativeFirmwareUrl":"/firmware/baby_9.0.2_r17157.bin","firmwareUpgrade":1}
```

The URL is a JSON string, not a binary firmware-version field. Relative URLs
are prefixed by `http://<server-ip>:<server-http-port>`. An absolute
`firmwareUrl` can point elsewhere. The `firmwareupgrade` command name and
the `firmwareUpgrade` result field have different case; Base's current
lowercase placeholder is not a full implementation of this result contract.

`_upgradeSink` caches URL/force state and emits Jive's local
`firmwareAvailable` notification when it changes. The updater applet reacts
for the current player's server. Returning no URL can clear a previously
advertised candidate when the response reaches the client; server-side
revocation must also reject a stale download URL. There is no firmware ACK
or retry opcode to add for this path. Reconnection restores Comet
subscriptions. Download retry is a native UI action, separate from LMS's
repository retry timers.

```mermaid
sequenceDiagram
    participant J as SqueezePlay SlimServer
    participant L as LMS HTTP/Comet :9000
    participant R as Repository
    participant U as SetupFirmwareUpgrade
    J->>L: Subscribe firmwareupgrade (machine, current version)
    L->>R: Obtain .version, image and SHA sidecars if missing
    R-->>L: Metadata and image
    L-->>J: firmwareUrl/relativeFirmwareUrl and firmwareUpgrade
    Note over L,J: fwdownloaded re-executes matching subscriptions
    J->>J: _upgradeSink; update URL/force; firmwareAvailable
    J-->>U: Native notification/current-server change
    U-->>U: Show update UI (optional or forced by firmware rules)
    Note over U: User chooses Begin Update
    U->>L: HTTP GET native .bin
    L-->>U: Stream ZIP-container image
    U->>U: Board check, UBI write, MD5 verification, rename, reboot
```

### Legacy SlimProto messages: documented, not recommended for Radio

Server-to-client frames are `BE16(length of opcode + payload)`, then four
opcode bytes, then payload. Client-to-server frames are four opcode bytes,
`BE32(payload length)`, then payload. ASCII opcodes are case-sensitive.

| Opcode | Direction | Payload and behavior |
| --- | --- | --- |
| `ureq` | Server → old client | Empty payload; length prefix is 4. LMS asks an older player to enter its update path when `needsUpgrade` is true. |
| `UREQ` | Old client → server | Server handler ignores payload content; no version or URL is parsed. It unblocks the player, removes its heartbeat entry and calls `upgradeFirmware`. The exact client-produced payload is not established by this server source. |
| `upda` | Server → old client | Raw firmware bytes, up to 1024 bytes per send in `upgradeFirmware_SDK5`; no URL, offset or revision field. A full chunk has BE16 length 1028. |
| `updn` | Server → old client | One space byte (`0x20`), BE16 length 5; marks end of the old transfer. |

The inspected server transfer loop has no per-chunk acknowledgement,
offset-based resume or explicit retry negotiation. That is not evidence of
every old device's internal failure policy. SqueezePlay's player class has
an empty `needsUpgrade` override, and its inspected Lua SlimProto path has no
firmware handlers for these opcodes. Firmware/version information is available
through SqueezePlay HELO capabilities and the Comet request, not an `upda`
metadata packet. `player_needs_upgrade` in player status should not be abused
to emulate this older transfer path.

## D. Native SqueezePlay updater and UI

`SetupFirmwareUpgradeMeta` registers `firmwareUpgrade`,
`showFirmwareUpgradeMenu`, `wasFirmwareUpgraded` and media-discovery services.
The existing Advanced Settings software-update item calls
`showFirmwareUpgradeMenu`. A local applet can call these services: binary
SlimProto is not required to open the UI.

- `showFirmwareUpgradeMenu()` finds the current server URL and local media,
  and opens an **optional** native choice window.
- `firmwareUpgrade(server)` obtains `server:getUpgradeUrl()`, uses the
  supplied force flag, and discovers that server's candidate. Its code also
  reads `server:getName()` for display.
- A force notification opens a screen that suppresses Back/Home and
  screensavers. **It still does not flash until Begin Update is selected.**
  This nevertheless fails a requirement to let users postpone freely.
- Optional windows allow normal departure; a multiple-choice optional screen
  also adds Cancel. Once installation begins all input is consumed; there is
  no supported mid-flash Cancel.
- `_upgrade` checks battery capability, shows progress, prevents powersaving,
  and disconnects the player from LMS. Touch additionally stops its built-in
  server/file sharing. The Radio path does not stop Base's external processes.
- `_t_upgrade` invokes `Upgrade:start`; `Upgrade:parseMtd()` selects UBI if
  `/proc/mtd` has a partition named `ubi`, otherwise the legacy MTD updater.
- Failure returns to `_upgradeFailed`, re-enables input, reconnects the player
  and offers a user-initiated retry. Do not automatically repeat a firmware
  download/flash after error.

There is no unattended installer in the inspected normal applet path. Do not
call private `_upgrade` from Base, bypass the confirmation window, or promise
that a native confirmation reached via injected Virtual Radio controls is a
physical confirmation. Firmware settings and handoff screens need the same
trusted-input boundary as Web Access, strengthened/tested for queued input.

### UBI installation and recovery semantics

The actual installer is Lua plus existing `ubimkvol`, `ubiupdatevol`,
`ubirmvol`, `ubirename`, udev helpers and native ZIP/MD5 modules. It is not a
separate Base flashing executable.

1. Read CPU board identity and boot command line.
2. On a normal boot, remove old `kernel_bak`/`cramfs_bak`. If booted from
   `cramfs_bak`, instead remove `kernel`/`cramfs`; the source explicitly warns
   normal boot is then unavailable until completion.
3. Remove stale `kernel_upg`/`cramfs_upg`, trigger/settle udev.
4. Stream ZIP entries. `board.version` must arrive before image data.
   Check platform/revision before creating/updating each image volume.
5. Create `kernel_upg` and `cramfs_upg` sized from ZIP uncompressed lengths;
   feed decompressed bytes into `ubiupdatevol`.
6. Read the resulting volume devices and compare their MD5 with
   `upgrade.md5`. Missing volumes/digests and mismatches fail.
7. Use one `ubirename` operation to switch kernel/root names and retain the
   previous normal pair as backups; verify resulting IDs.
8. Delay roughly two seconds, then invoke the existing reboot service.

Only the **name switch** is intended to be atomic. The entire operation is
not atomic: backup removal and writes precede it, shell command exits are not
uniformly checked, and power loss can leave incomplete volumes. A normal
running image is generally left intact before the switch, but the backup-boot
path is explicitly less recoverable. No automatic successful-boot rollback
verification was established here. Bootloader/recovery implementation and
power-cut behavior require separate controlled validation. Reject backup-root
updates in Base's first supported workflow.

The non-UBI branch erases/writes alternate MTD partitions with `nandwrite`,
checks MD5 and changes boot environment using `fw_setenv`. It must not be
treated as equivalent to Radio UBI support or enabled without separate work.

## E. Firmware package analysis

### Directly inspected r17157 release

Source: [SourceForge distribution](https://sourceforge.net/projects/lmsclients/files/squeezeos/baby_9.0.2_r17157.zip/).
The download endpoint followed a redirect to a SourceForge mirror. The mirror
returned `application/octet-stream`, Content-Length 16,033,189, and advertised
byte ranges. Transient mirror query tokens must not be stored as configuration.

| Outer ZIP entry | Uncompressed bytes |
| --- | ---: |
| `baby_9.0.2_r17157.bin` | 16,032,273 |
| `baby_9.0.2_r17157.bin.sha` | 64 |
| `baby.version` | 115 |
| `baby.version.sha` | 55 |

The outer archive stores these entries without compression. Its observed
SHA-256 is `a3e026733b4e9a814b8e6ae08b11fcb24df49a424dbef80483d16f5fb89c955d`.
The image SHA-1 is `9ab354c46ae98027d5a3164c2ae13166875d2c87`, matching its
sidecar. These observations identify inspected bytes; they are not a signed
publisher attestation or a permanent trust policy.

`baby.version` and the inner `jive.version` both contain:

```text
9.0.2 r17157
root@poky Thu Oct 22 18:01:38 EDT 2026
Base build revision:  13014bea90f52b3d14ad90edfae180009b40c213
```

The `.bin` begins with ZIP records, not a custom signed firmware header. Its
ordered, deflated entries are:

| Inner image entry | Uncompressed bytes | Compressed bytes |
| --- | ---: | ---: |
| `jive.version` | 115 | 107 |
| `board.version` | 102 | 42 |
| `upgrade.md5` | 87 | 78 |
| `zImage` | 3,191,832 | 1,533,927 |
| `root.cramfs` | 14,598,144 | 14,497,351 |

`board.version` permits `baby:0001` through `baby:0009` and `baby:25000`.
The native parser converts these revisions to numbers and compares against
CPU hardware/revision. Inner ZIP CRC testing passed; independently recomputed
MD5 values matched `upgrade.md5`:

```text
0a1dcd59d22df06a518c279a9a4d92e6  zImage
7bdc48a8c5869c7e4e888fee2951e144  root.cramfs
```

The build class creates this precise inner layout and symlinks a generic
`baby.bin` to its versioned image. Generic names and `custom.baby.bin` are
aliases, not different image formats. `.version` is textual metadata, not
the firmware itself. No cryptographic signature enforcement was found in
the inspected UBI installer: MD5/CRC/internal sidecars detect corruption,
not a malicious publisher replacing image and checksums together.

### Input choices

| Input | Complexity and compatibility | Resource/security implications |
| --- | --- | --- |
| A: Direct native `.bin` | Smallest one-shot import. Read internal version/board/checksum metadata; no outer extraction. Require the real updater ZIP image, not an arbitrary raw binary. | One bounded image staging file; incremental hash/decompression validation. Require independent trusted digest/signature policy. |
| B: SourceForge release ZIP | Useful UX but not directly usable. Select exactly one baby image and matching version/SHA sidecars, then stage the inner image without changing its bytes/order. | Nested ZIP parsing, duplicate/path/expansion guards and redirect checks. Stream extraction to avoid retaining outer and inner copies in RAM or doubling persistent storage unnecessarily. |
| C: Version/update feed | Best recurring update model. Support a documented descriptor with model, release, numeric revision, image URL, length and trusted digest; an LMS-style `.version` base is another explicit adapter. | Tiny metadata checks; download only a new candidate. Standard LMS SHA-1 feeds alone are not authenticated. Signed/pinned metadata is preferable. |

Start with A plus a trusted expected SHA-256, and a simple explicit C descriptor
for automatic newer-version discovery. A single freely editable URL with no
trusted hash/signature is not enough to claim safe firmware authenticity when
using the existing unverified proxy. Avoid silently inferring an image URL by
replacing arbitrary extensions. Wrapper support is a subsequent bounded adapter.
Do not repack the `.bin`; ordering matters to the streaming native parser.

## F. Version and hardware detection

### Authoritative current identity

For the **running SqueezePlay client**, use `jive.JIVE_VERSION`; that exact
compiled value is sent in `firmwareVersion:`. `System:getMachine()` returns
machine and hardware revision. Cross-check `/etc/squeezeos.version` for the
installed OS version and `/proc/cpuinfo` Hardware/Revision for flashing. A
mismatch between running-client and installed-image identity is a stop/error,
not permission to select whichever looks older. `uname` identifies the kernel,
not the SqueezePlay release. The HELO legacy one-byte revision is not a substitute
for the full `Firmware=` capability/string. Base currently only extracts MAC
from HELO; it does not yet retain a trusted complete firmware identity.

### Existing comparison rules are not one consistent algorithm

| Component | Confirmed behavior |
| --- | --- |
| LMS `need_upgrade` | Parse first-line release and numeric `r` digits; any different release, or numerically higher revision, requests upgrade. |
| SP7 `_upgradeSink` | Use numeric `firmwareUpgrade == 1` from server. |
| SP9 `_upgradeSink` | Extract version/revision from `/baby_<release>_r<revision>.bin`; ignore flag for force decision. Equality suppresses; release checks plus revision comparison decide. Revisions are captured as strings and compared with Lua `>`. |
| SP9 `isMoreRecent` | Splits dot components and returns true for any numerically larger matching component; it does not stop at an earlier smaller component. It is not a proper lexicographic semantic-version comparison. |
| Updater `_versionCompare` | For the same release, subtract numeric-looking revision strings; missing/different release returns `math.huge`. This mostly chooses help text, not image compatibility. |

Five-digit string ordering happens to put `17157` after `17084`; that is not a
general rule. For example, string `100000 > 99999` is false while the numeric
comparison is true. The SP9 release-component algorithm can also regard a
lower major but higher later component as newer. Do not inherit either flaw.

Base should parse strict bounded numeric `(major, minor, patch, revision)`
tuples and compare release components in order, then revision within the same
declared release/channel policy. Reject malformed, negative, overflowing,
missing or ambiguous versions. Explicitly supported two-component releases
may normalize a missing patch to zero; arbitrary suffixes need an explicit
adapter, not guessing. Apply a no-downgrade rule independently of Jive.

`r17157` alone cannot establish compatibility or release precedence. The
inspected target is **9.0.2 r17157**. If the installed identity really is
9.0.1 r17084, both release and revision advance. If a different release or
channel is installed, its complete identity must be evaluated. A revision is
a build identifier influenced by the SqueezeOS/SqueezePlay build process,
not a universal cross-fork semantic version or beta-channel identifier.

After reboot, compare the running full identity again and suppress equal/older
candidates. Bind pending notices and consent to an immutable image digest,
not merely the URL. Reset them on URL/digest/opt-in changes. Persist dismissal
or use bounded in-memory notification suppression as a UX policy, without
turning it into an installation authorization. Unknown versions fail closed.

### Checks before advertisement

Require all of the following:

- Runtime machine exactly `baby`; firmware metadata also explicitly `baby`.
- Board revision supported by the image's `board.version`, using native-compatible
  numeric interpretation and matching runtime/CPU identity.
- Valid inner container, expected entry order, unique safe filenames, supported
  compression, bounded image sizes, matching metadata and all image hashes.
- Full target version is newer by Base's supported release/channel policy.
- Normal UBI root boot, required volume names/layout, compatible kernel/root
  image type and enough UBI space after only the native updater's permitted
  backup reclamation. No alteration of `sbdata` to make space.
- Local permission and current physical input authorization; no conflicting
  update/storage operation and sufficient power.
- Authenticated immutable staged bytes available for the entire native fetch.

The native board check remains defense in depth, not the first time Base learns
an image is for another model. A filename containing `baby` is not proof: the
updater's server discovery check is only a substring check.

## G. Standalone Base integration design

### Existing attachment points

| Component | Existing code / location | Natural extension, not implemented |
| --- | --- | --- |
| SlimProto / HTTP service | `native/sbbase/src/main.c`: `client`, `close_client`, `comet_data`, `handle_slim` near 1330; HELO 1369; `firmwareupgrade` placeholder 918 | Correct Comet result and firmware subscription lifecycle; retain typed per-client identity, never add legacy flash opcodes. |
| Protocol/config | `native/sbbase/src/protocol.c`, `native/sbbase/include/sbbase.h` | Bounded candidate descriptor, explicit eligibility, safe config reload. |
| Web routing | `native/sbwebserver/src/http_server.c`, shared auth in `auth.c` | Authenticated firmware configuration/status, CSRF/origin/read-only controls; special narrowly scoped native download route. |
| Persistence | `native/sbwebserver/src/settings.c`: `settings_load`/`settings_save`; `settings.conf` in existing config directory | Atomic server URL/candidate settings, no raw image embedded in JSON. |
| Background work | `catalog_sync.c`: async events, `catalog_sync_tick`; `image_render.c`: bounded child job | Reuse patterns, not artwork limits/cache itself; one firmware preparation job, no HTTP-loop blocking. |
| Applet / lifecycle | `applet/StandaloneBaseMeta.lua`: `defaultSettings`; `StandaloneBaseApplet.lua`: `init`, `supervise` (15 s), menu | Persistent device opt-in, runtime identity, local notice, optional native UI handoff and supervisor coordination. |
| Internal auth | `applet/RemoteBridge.lua`: `webAccess`, `isInjecting`; capability file and shared native auth | Separate authenticated internal firmware permission/status channel; never browser cookies or browser-writable permission. |
| Change notification | Base Comet streams/subscriptions; existing SIGHUP catalog reload | Candidate generation changes invalidate stale state; add firmware-specific subscription storage and change delivery. |

The existing `firmwareupgrade` response is a hardcoded all-zero object with no
URL. HELO currently records player MAC but not firmware/model capabilities.
The current Comet player-status subscription implementation is not already
a firmware-status subscription: an initial placeholder response is not enough
for later asynchronous updates.

### Proposed separation of responsibilities

**sbwebserver:** owns authenticated URL/configuration writes, validation jobs,
stage files, progress/error reporting and a narrow download handler. Candidate
preparation must never flash or reboot. Use temporary-file + atomic-rename
publication, a bounded streaming parser/hash worker, and cancellation on any
config generation change. Reuse existing HTTP fetching/`sbproxy` transport,
without another TLS library. Do not use the image renderer/cache: its file
limits, transformations and timeouts are inappropriate for firmware.

**sbbase:** owns the LMS-compatible `firmwareupgrade` response and subscription
delivery, using only a validated, immutable candidate descriptor. Runtime
permission starts false until authenticated local applet synchronization.
Do not cache an enabled bit indefinitely after the applet disappears. A lost
device permission lease clears advertisements. Do not trust an arbitrary
remote Comet query's claimed `machine`/`firmwareVersion` as authorization.

**Lua applet:** owns the physical checkbox and running identity. Add a separate
`customFirmwareUpdates=false` Jive setting, using `getSettings()` and
`storeSettings()`; do not overload Base's existing enabled setting. Synchronize
the current opt-in through the existing internal-capability domain at startup
and on changes, with bounded retry. Reinstallation or missing settings defaults
to disabled. Browser configuration cannot turn it on. A pending preflight
without permission may show status, but cannot publish or open the updater.

Every checkbox/notification/launch callback must reject Virtual Radio input.
The current `RemoteBridge:isInjecting()` uses a synchronous marker plus a
one-second guard for queued actions. That is a useful existing pattern, **not
a formal proof** for a firmware-security boundary. Phase 2 must validate queued
and delayed actions or introduce an explicit trusted hardware confirmation
with remote injection blocked while that screen is active. Do not claim
physical-only security from the timer alone. Protect native Begin Update too.

After Begin Update, the native updater owns the operation. There is no safe
general cancel guarantee once it removes/writes volumes. Disabling permission
before confirmation must revoke the advertisement and URL; during installation
the setting must be inaccessible, not abort a write midway.

### Optional UI compatibility strategy

Do not simply advertise canonical `baby_9.0.2_r17157.bin` and rely on
`firmwareUpgrade:0`: SP9 can force its window anyway. Do not ship a global
monkey-patch of `SlimServer:_upgradeSink` as the first solution.

A small compatibility design using existing facilities is:

1. Keep full verified version/board/digest in Base's candidate descriptor.
2. Expose those exact image bytes through a **versionless native alias**, e.g.
   `http://127.0.0.1:<web-port>/firmware/<digest>/custom.baby.bin`. The URL still
   passes native `baby` source discovery but does not match the canonical
   filename version parser. Never rename/repack the image internally.
3. Return this absolute `firmwareUrl` with `firmwareUpgrade:0` only when all
   gates pass. SP7 respects zero; inspected SP9 stores the URL but cannot infer
   an automatic force decision from that alias.
4. The Base applet displays a dismissable local availability item including
   actual target version, source and safety information, then calls the native
   optional UI service following physical user selection. Prefer
   `showFirmwareUpgradeMenu` when the current server is Base. If the current
   server is not Base, do not hijack its update state: defer, or use a narrowly
   scoped adapter with `getUpgradeUrl()` returning `(url,false)` and `getName()`
   for the existing `firmwareUpgrade` service, validated on each target firmware.
5. The native screen may lack a parsed target-version label for the alias;
   Base's preceding local screen must show the verified version. Installation
   still requires native Begin Update. Do not call private installer methods.

This is a **source-supported proposal, not a device-validated guarantee**.
The alias, optional departure behavior and input isolation are acceptance-test
gates. It does not exactly reproduce an automatic canonical LMS forced prompt;
that tradeoff is intentional to preserve postponement. If an entirely native
automatic optional notification with a visible version is mandatory, a small
upstream API extension/adapter is needed and must be agreed before coding.

### Candidate API, persistence and event model (proposed only)

```text
GET/PUT /api/firmware/config       paired browser; URL/trusted digest/channel
POST    /api/firmware/check        paired browser; start bounded preparation
GET     /api/firmware/status       paired browser; no authority to enable/install
POST    /api/internal/firmware/... local capability; identity/permission/lease
GET     /firmware/<digest>/custom.baby.bin  exact native download only
```

`check` is a state change: no GET-triggered downloads. Use existing write/CSRF
checks. Store configuration in Base's local settings, not the auto-merged public
applet catalog. Store staged images outside applet repository upload paths,
and never as executable shell commands. No browser `/install` or `/reboot`
endpoint is required. Suggested states: disabled, unconfigured, checking,
blocked, ready, dismissed, handed-off. Do not mark installed merely because
an HTTP GET finished; re-observe actual firmware after reboot.

Native SqueezePlay has no browser cookie, so its download route cannot use the
ordinary browser session requirement. Limit it to the exact active immutable
image on loopback, with a short-lived, device-authorized handoff state, strict
Host/peer/path validation, no directory serving and no arbitrary upstream URL.
A content digest is an identifier, **not a secret credential**. Internal
permission-control APIs require the capability even on loopback. Serving
already-validated bytes must never grant permission to install, and no
external browser should be able to invoke this route through a general proxy.

## H. Proposed update sequence

```mermaid
sequenceDiagram
    actor User
    participant W as Base web interface
    participant N as Base native services
    participant A as Base Lua applet
    participant U as SqueezePlay firmware updater
    participant R as Remote firmware server
    User->>W: Configure source and trusted digest/feed
    W->>N: Authenticated configuration/check request
    N->>R: Bounded metadata/image fetch via existing transport
    R-->>N: Candidate metadata and bytes
    N->>N: Stage; authenticate; check version, board, layout and space
    Note over N: No publication while local permission is absent
    User->>A: Physically enable custom updates (default off)
    A->>N: Capability-authenticated identity and permission lease
    N-->>A: Candidate status; Comet URL availability, force=0
    A-->>User: Dismissable notice with verified version
    User->>A: Physically choose Review Update
    A->>N: Recheck same candidate/permission/space; authorize handoff
    A->>U: Existing optional update UI service
    U-->>User: Native Begin Update confirmation
    User->>U: Explicitly confirm installation
    U->>N: GET immutable local native image
    N-->>U: Exact ZIP-container .bin; known Content-Length
    U->>U: Board checks; native UBI write and read-back MD5
    U->>U: Native volume switch and reboot
    Note over N,A: On next boot, re-observe identity; suppress installed candidate
```

Network retrieval is separated from flashing. Playback is not interrupted by
merely enabling the feature or seeing a notice. Once the user confirms, stop
playback through the normal services. Coordinate Base's supervisor so it does
not reconnect the player or restart intentionally quiesced workers while the
updater runs. Keep the local file-serving service alive until the native fetch
has finished; never stop the server hosting the update as a generic cleanup.

## I. Network, security, storage and recovery

### Transport choices

| Approach | Assessment |
| --- | --- |
| Advertise original remote URL | HTTP can work; HTTPS is rejected by `UpgradeUBI:download`. SourceForge wrapper/HTML links are wrong payloads. Changes/redirects during flashing remain a risk. |
| Advertise ordinary sbproxy URL | Native sees HTTP and can stream it, but current proxy has unverified TLS and generic streaming limits. No candidate authentication, immutable staging or complete-download assurance. Not recommended for initial safe workflow. |
| Serve local validated file | Recommended. Known bytes/length, no WAN dependency once native writes begin; existing server needs a scoped firmware route independent of browser cookies. |
| Stage before advertising | Strongest reliability improvement, provided both filesystem staging capacity and UBI update headroom are proven. If unavailable, block and explain; do not silently fall back to risky streaming. |

`UpgradeUBI:download` supports `file:` reads and streamed HTTP GET. A local
`file:` path could remove the HTTP hop, but `_findServerUpgrade` expects a
URL containing the machine, and permissions/path consistency across mounts
need testing; local HTTP is the closest LMS-compatible first implementation.

`RequestHttp` handles GET redirects 301/302/307, up to five, but recreates a
plain `SocketHttp`; it provides no demonstrated TLS redirect support. The
inspected implementation does not establish correct general relative-redirect
handling. Do not promise 303/308 or arbitrary HTTPS redirects. Serve the final
local image without any redirect. HTTP 200 is the straightforward success
contract; `Content-Length` feeds the progress display. The updater adds no
Range header or resume offset, and its body handling is designed around 200,
not a firmware Range/resume protocol. Mirror byte-range support does not mean
the Radio can resume a partially flashed update.

`sbproxy` is bounded to eight workers and streams rather than retaining the
whole response. Ordinary `configure_curl` follows eight redirects and has a
15-second connect timeout, but no firmware-specific total-size/deadline or
authenticated-manifest enforcement. The opt-in artwork/public path has
different, much shorter limits. Reuse transport only behind a firmware worker
with explicit size, destination, redirect, idle/total-time and authenticity
checks; never pass firmware through image transforms. Preserve established
proxy TLS policy; any verified-fetch policy needed for firmware must be an
explicit scoped design decision, not a stealth global change.

### Capacity and RAM

This candidate needs about 15.29 MiB for the inner staged file; keeping the
outer archive too adds another 15.29 MiB. A normal 8 MiB writable root cannot
hold it. `/tmp` is RAM on this platform and 16+ MiB staging is not automatically
safe on a 64 MiB Radio running Jive; do not choose tmpfs merely because it is
writable. Require sufficient actual staging filesystem capacity and a margin,
preferably the existing Extended Storage, or explain that preparation requires
an external trusted local image host. Do not create another UBI volume.

Native uncompressed kernel plus root require 17,789,976 bytes. At 126,976-byte
LEBs their separately rounded payloads need at least 26 + 115 = **141 LEBs**.
That is a payload lower bound, not a new initialization threshold: UBI reserve,
bad-block handling, existing volume geometry and updater allocation semantics
must be included. Before handoff, inspect free LEBs and reclaimable native
backup volumes read-only; do not delete anything in a preflight. Reject unknown
layouts, backup-root boot, insufficient headroom and destructive plans that
would require removing/shrinking `sbdata`.

Use one bounded job, small fixed I/O buffers, incremental hashing/ZIP validation
and bounded metadata. Never load both compressed firmware and decompressed
root/kernel into Radio RAM. Reuse staging across identical checks, avoid
continuous image redownloads/flash writes, and throttle preparation so audio
continues normally. CPU/RSS/time estimates require device measurement; none
are claimed here. Memory on the laptop during archive inspection is not a
Radio resource measurement.

Initial proposed policy limits for this Radio-only adapter are a 32 MiB outer
wrapper, 24 MiB native image, eight entries per archive, 64 KiB total text
metadata, 8 MiB kernel and 32 MiB root payload (40 MiB combined expanded image
limit), one staging worker and fixed-size I/O buffers. These are design caps,
not measurements or existing updater guarantees. Enforce them incrementally,
including when lengths are missing; revise only with explicit image/hardware
validation. Keep redirect count small and total/idle deadlines configurable
and finite. Do not use a generic unbounded unzip or allocate from ZIP lengths.

### Authentication and integrity

- Browser configuration is paired-session authenticated, same-origin/CSRF
  protected and subject to read-only mode. A configured URL is not permission.
- The persistent checkbox is physically controlled and cannot be changed by a
  browser, remote-injected action, applet-catalog merge or remote Comet query.
- Validate HTTP(S) URLs and all redirect targets. No credentials, file schemes,
  shell expansion, loopback/link-local/private SSRF, multicast or ambiguous IP
  forms for external sources. A trusted LAN firmware host requires a separate
  explicit allowlist policy, not a global private-network bypass.
- Enforce actual streamed length even if Content-Length is absent or false;
  cap expansion, nesting, entry count and metadata sizes; reject duplicate or
  traversing filenames, encryption and unsupported ZIP methods. Inspect both
  local-file order and central directory, not just a misleading filename.
- Compare outer `.version`, inner `jive.version`, requested model and expected
  digest. Freeze one candidate generation through notification and handoff.
- SHA-1 sidecars and MD5 contained in a downloaded image are corruption checks,
  not authenticity. Current sbproxy's disabled peer/hostname verification means
  encryption alone cannot establish publisher identity. Require a pinned
  independently trusted SHA-256 or verified signed manifest before publication.
- Do not log full signed/query-bearing URLs, credentials or cookies. Log model,
  versions, candidate digest, validation result, size/space and reason codes.
- Recheck eligibility just before handoff. A stale downloaded URL must not
  revive a revoked permission or install a different image under the same path.

### Failure and recovery boundaries

| Failure | Existing protection | Additional Base safeguard |
| --- | --- | --- |
| Wrong model/board | `board.version` checked before image-volume write | Validate before publication and native backup deletion; exact baby identity, not substring only |
| Corrupted image | ZIP processing and post-write MD5 | Complete staged validation and trusted digest before user sees Ready |
| Interrupted download | Native error/retry screen; normal current volumes generally retained | Stage first; no automatic flash retry; preserve diagnostic reason |
| Power loss | Atomic-name-switch design, previous image on normal path | Require mains/adequate power; do not claim whole-update atomicity; validate recovery separately |
| Backup-root boot | Source explicitly warns normal boot may be lost during update | Block initial implementation |
| Not enough UBI/filesystem space | Creation/write/assert failures can surface late | Preflight both capacities, no automatic storage erasure |
| Repeated/older offer | Some Jive/LMS version checks | Strict Base no-downgrade comparison, digest-bound notification state, reread version after reboot |
| URL changes or opt-out | No Base-specific protection in native updater | Invalidate generation, subscriptions and unconsumed handoff; never interrupt an already-started flash |
| Base applet disappears after upgrade | Existing storage recovery on reinstall | Document reinstall/recovery; don't promise Base or boot hooks survive firmware installation |
| Concurrent storage/app update | Not coordinated by native firmware updater | Serialize with storage initialization, recovery, erase and applet updates; suspend interfering supervisor behavior |

Factory reset is not a guaranteed recovery method for bad firmware. Official
custom-firmware guidance describes booting the previous image using the
rewind button, but physical recovery and current bootloader behavior must be
verified for the target before an experimental install. See
[Lyrion custom firmware guidance](https://lyrion.org/reference/custom-firmware/).

## UBIFS patch and Extended Storage compatibility

PR #37, merged 22 October 2026, adds patch 0228 to both RT and non-RT i.MX25
series. The change replaces the fixed backing-device name `ubifs` with
`ubifs_<ubi-number>_<volume-id>`, preventing registration collisions for
multiple ordinary UBIFS mounts. It does not change the on-disk filesystem,
volume format, mountpoint or Base's policy. The r17157 metadata's base commit
matches the merge. Actual concurrent mounts on a booted r17157 Radio were
**not** tested in this phase.

Base's separate module intentionally registers filesystem **`sbubifs`**, uses
`sbubifs_<ubi-number>_<volume-id>` and restricts mounting to its authorized
`sbdata` volume. See `native/kernel-modules/sbubifs/patches/0001-separate-identity-and-sbdata-allowlist.patch`
around lines 52–55. Current `storage-setup.sh` checks for `sbubifs` at line 72,
loads it near 543/594/639 and mounts it explicitly. `storage-boot.sh` lines
50–75 also requires it. `radio_stations.c` and `ExtendedStorageState.lua`
recognize the existing mount type. Merely skipping `insmod` would break these
checks; this needs a coherent backend selection, not a one-line module bypass.

Proposed future capability selection:

1. Inspect current mount identities, CPU/kernel/build identity and sysfs BDI
   names without changing mounts. A registered `ubifs_0_<root-id>` BDI is
   stronger feature evidence than a firmware revision string or `uname`.
2. `/proc/filesystems` containing `ubifs` is **not** a capability test: the
   broken old kernel already provides it. A version string alone is also not
   sufficient for custom/backported kernels.
3. Recognize a verified build/capability combination. If uncertain, retain the
   existing module path or mark unsupported; never trial-mount during a status
   check. A controlled read-only mount of an already validated existing sbdata
   volume is a later explicitly authorized validation step, not Phase 1.
4. In the supported native backend, mount `ubi0:sbdata` as `ubifs` at the same
   `/mnt/sbdata` and keep existing exact-volume/device/geometry safeguards.
   Accept the two known mount types only when backing-volume identity matches.
5. Update initialization, early boot, recovery, state reporting and all
   applet/storage consumers together. Retain old `sbubifs` module support for
   old kernels. Do not swap the type of a live mount or unload an active module.

Boot hooks/module ABI can change after firmware installation. Preserve and
reinstall recovery support deliberately. Never assume the PR removes storage
layout, free-block, applet bind-mount or post-upgrade recovery requirements.

## J. Recommended implementation plan and acceptance tests

### 1. Contract fixtures and source/runtime verification

- Capture reference LMS HTTP/Comet query/result/notification envelopes with a
  test client, not by permitting a real update. Confirm literal field casing,
  `subscribe:0` behavior and candidate removal.
- Exercise stock 7.7.2/7.7.3 and community 9.x Lua fixtures with canonical and
  versionless URLs: no candidate, older/equal/newer, missing/malformed version,
  different release, revision digit-boundary and downgrade cases.
- Verify `showFirmwareUpgradeMenu`/adapter behavior, voluntary Back/Cancel,
  target information and no installation without Begin Update. Use mocked
  installer services for automated tests.

### 2. Pure bounded candidate inspection

- Implement numeric identity comparison, board metadata interpretation and
  no-downgrade policy, independently of network/UI.
- Test the inspected package and wrong-model, wrong-board, mismatched version,
  duplicate/traversal/nested ZIP, incorrect size/CRC/hash, truncated image,
  encryption and unsupported compression fixtures. All failures mean no offer.
- Add wrapper import only after direct-native-image validation is sound.

### 3. Authenticated configuration, opt-in and staging

- Add server-side source/status settings and physical default-off Jive checkbox.
- Test no browser or remote-input route can enable permission or launch native
  Begin Update; cover delayed queued actions, not just synchronous dispatch.
- Implement one streaming preparation job with strict limits, atomic staging,
  trusted digest/signature policy, cancellation and resource reporting.
- Test wrong Origin, unauthenticated writes, read-only mode, permission loss,
  applet/service restart, changed URL/digest and stale handoff requests.
- Test low space in both storage domains, backup-root boot, malformed UBI layout
  and concurrent storage operation: no flash, erase, partition resize or offer.

### 4. Existing-protocol advertisement and native UI handoff

- Replace the placeholder with exact `firmwareupgrade` results; maintain bounded
  firmware subscriptions and publish/clear changes on the correct Comet stream.
- Use the optional alias strategy or agree an upstream optional-prompt adapter;
  do not send `ureq`, `upda` or `updn` to a Radio.
- Restrict downloads to the active verified file, native loopback access and
  handoff state; browser cookies are not required or exposed to native updater.
- Test no external LMS state is overwritten and no running playback is stopped
  merely by checking availability.

### 5. Controlled native updater validation, separately authorized

- Before any real flash, record firmware, board, boot source, UBI geometry,
  free space, Base recovery plan and a proven previous-image recovery method.
- Verify complete local native download with known Content-Length, no redirects
  and no full-image allocation, then test the confirmation boundary without
  pressing Begin Update.
- With explicit later authorization, test one recoverable Radio on mains. Check
  native progress, board verification, reboot, new runtime identity, Base
  reinstall/storage recovery and disappearance of the update offer.
- Record CPU/RSS, worker count, audio behavior during preparation and UBI space.
  Power-cut/failure testing belongs on expendable hardware with serial/recovery
  access, not an ordinary user's Radio.
- Separately validate the native UBIFS backend before retiring any module load
  on that kernel. Do not bundle a storage-backend change into untested update UI.

## K. Open questions and explicit limitations

1. No live LMS packet capture or Radio updater UI exercise was done. The laptop
   LMS can later validate wire envelopes safely. Its version/plugins/cache and
   candidate must be recorded; avoid enabling a forced offer to all real clients.
2. The exact installed firmware, board revision, boot source, free UBI blocks
   and patched-kernel behavior of a future test Radio must be read before use.
   Source snapshots do not prove the current device's installed Lua matches.
3. r17157's artifact and hashes were inspected; the new kernel was not booted,
   its full root filesystem/updater binaries were not extracted, and no mount
   test was performed. Metadata/source provenance is not complete reproducible
   binary attestation.
4. Stable feed HTTPS failed hostname validation here; beta publication and
   repository trust configuration remain unverified. Do not silently accept
   a certificate failure or assume the beta channel includes r17157.
5. A publisher authentication policy is required. Decide pinned expected hash,
   signed feed or a separately scoped verified-fetch mechanism before coding.
   The existing general proxy's TLS policy must not be globally redesigned.
6. The versionless optional-UI strategy needs fixtures and stock/community
   device validation. Native visible version text is absent for that alias;
   Base must show verified details beforehand. An all-native automatic optional
   prompt may require a small agreed upstream integration.
7. Native install-progress hooks, input provenance and supervisor coordination
   require a verified design. Do not infer "completed installation" from HTTP
   completion or rely on a one-second remote-input timer as a proof.
8. Bootloader previous-image selection, automatic rollback, command-failure
   behavior and power-loss recovery were not fully traced/tested. No guarantee
   against bricking is made.
9. Storage must coexist with temporary native image volumes. The 141-LEB payload
   calculation is not a universal free-block requirement or permission to erase
   backups/sbdata during preflight. Exact platform headroom must be validated.
10. No source implementation, deployable build or release is part of Phase 1.

## Final decision summary

Phase 2 development status and actual test results are tracked separately in
[`FIRMWARE_UPDATE_IMPLEMENTATION.md`](FIRMWARE_UPDATE_IMPLEMENTATION.md). The
Phase 1 observations in this document are not evidence that installation or
physical input security has been validated on hardware.

| Question | Finding |
| --- | --- |
| Advertise through existing SlimProto implementation? | Extend Base's existing HTTP/Comet `firmwareupgrade` support; Radio does not use binary SlimProto update transfer. |
| Required binary opcodes? | None for this feature. `UREQ`/`ureq`, `upda`, `updn` are older-player mechanisms. |
| Reuse native UI? | Yes, registered services exist. Optional/forced behavior and physical input must be handled explicitly. |
| Direct SourceForge ZIP? | No for this inspected release; extract/validate its native inner `.bin` first. |
| Compare versions? | Full validated numeric release tuple plus revision/channel policy; not bare `r` values or generic string comparison. |
| Hardware compatibility? | Exact `baby`, native-compatible board revision checks, inner metadata and UBI/boot/layout/headroom preflight. |
| HTTPS? | Native updater only HTTP/file; stage through existing transport with independent authenticity, then serve locally. |
| Checkbox? | Persistent default-off Jive applet setting, physically controlled and capability-synchronized; not browser-writable. |
| Expose updates? | Verified immutable candidate, Comet subscription, optional local notification and native user-confirmed handoff. |
| Implementation priority? | Protocol/UI fixtures → validator/trust policy → opt-in/staging/preflight → advertisement/handoff → separately authorized hardware validation. |
