# Phase 2E — post-firmware recovery and retained bootstrap

## Scope and current result

Target: Squeezebox Radio Baby, 192.168.1.228, firmware 9.0.2 r17157.
The user performed the native firmware upgrade from r17146 before this task.
This task did **not** install firmware or change the native flashing mechanism.
No UBI volume was created, deleted, resized, overwritten or reformatted on the
Radio. No kernel module or watchdog configuration was modified.

The existing installation was restored. The user confirmed that Standalone Base
and Standalone Radio reappeared and that normal station playback worked. Native
UBIFS backs both production storage and Extended Storage; the legacy module was
not needed. The controlled test demonstrated retained startup recovery, but also
observed an additional watchdog reset. Full reboot-stability acceptance remains
open; see the qualified reboot result below.

## Confirmed root cause

The decisive device evidence is **`/init`**, not an assumption about the firmware
flasher. Before constructing the UnionFS root, it compares the complete firmware
version file with `/mnt/storage/etc/squeezeos.version`. On a version change it:

1. Moves selected modified system files aside according to
   `/etc/move-after-upgrade`.
2. Enumerates regular files under persistent storage and removes those not
   matching `/etc/keep-after-upgrade`.
3. Updates the persistent version marker, then creates the UnionFS root and
   enters it with `chroot`.

The observed r17157 retention policy preserves `settings.lua$`, `/etc/`, and
`/root/`. Its cleanup does not remove the separate `sbdata` filesystem.
The original bootstrap at `/mnt/storage/standalonebase/storage-boot.sh` did not
match the retained paths. Neither did Base's ordinary configuration directory
or its legacy module copy at `/mnt/storage/sbubifs-authorized.ko`.

The existing installer is responsible for those paths:
`scripts/storage-setup.sh` defines `PERSIST_DIR`, `PERSIST_BOOT` and
`PERSIST_MODULE`; `initialize()` and `recover_existing()` install them and
`install_rcs_hook()` writes the rcS.local invocation. Before this correction,
that invocation was `/bin/sh /mnt/storage/standalonebase/storage-boot.sh || true`.
The hook itself is under `/etc/`, so it survived while its target disappeared.
The old command's failure was ignored, without a dedicated syslog diagnostic.

Further corroboration:

- The old persistent version backup and current marker span the upgrade.
- Directories remain, but regular files outside retained paths are absent,
  matching the cleanup's `find ... -type f` behavior.
- The native updater's `UpgradeUBI.lua` deals with firmware volumes; it is not
  the observed regular-file filter. It was inspected, not changed.
- The original applet directory is discovered before Base can run. Without the
  early mount/bind, Base cannot launch its own automatic recovery code.

### UnionFS path behavior

`/init` enters a UnionFS chroot. The global mount table still names the outer
`/mnt/storage` mount, but paths below `/mnt/storage` inside that root can create
nested upper-layer directories. Both `/mnt/storage/standalonebase` and
`/mnt/storage/mnt/storage/standalonebase` were present but empty, and inode
inspection showed they were different directories. This is evidence of the path
hazard, not a claim that UnionFS spontaneously deleted files.

The correction writes retained state through the ordinary **`/etc/standalonebase`**
view. It does not try to guess or traverse a raw nested upper-layer path.

## Surviving versus missing components

| Component | Observed after upgrade, before restoration |
|---|---|
| `ubi0:sbdata`, volume ID 5, 521 LEBs | Intact; healthy dynamic volume |
| Production `ubi0:ubifs`, 82 LEBs | Mounted at production storage |
| Native UBIFS multi-mount | Confirmed by two live UBIFS mounts and `ubifs_0_2` / `ubifs_0_5` BDI entries |
| `/mnt/sbdata/applets` | Existing Base, Radio, binaries and original applet tree intact |
| `/mnt/sbdata/config/radio/stations.json` | Intact |
| `/mnt/sbdata/cache` and other Extended Storage data | Present; no cleanup performed |
| Staged r17157 native image | Present, 16,032,273 bytes |
| `/etc/squeezeplay/userpath` settings and Radio cache | Retained |
| `rcS.local` | Present and executable; referenced a missing file |
| `/mnt/storage/standalonebase` and nested counterpart | Empty |
| Legacy persistent module copy | Missing; native mounting makes it unnecessary on this kernel |
| Active applet bind | Missing; stock firmware tree visible instead |
| Base services | Not running |

The existing volume was already natively mounted when inspection began. No
initialization or format was needed. Inspection was read-only until evidence and
the correct bootstrap implementation had been established.

## Restoration performed

1. Captured device inventory, startup scripts, cleanup policy and logs on the
   laptop. Archived files before replacing them.
2. Recorded MD5 manifests of surviving station/user settings and the staged image.
   These hashes were preservation checks, not firmware authenticity checks.
3. Updated the existing `storage-boot.sh` implementation to recognize the native
   backend. It did not introduce a replacement daemon or a separate boot scheme.
4. Added the non-destructive `storage-setup.sh repair` operation. It installed
   retained support and the compatibility alias, without binding beneath running
   SqueezePlay or replacing the existing applet tree.
5. Restored the deleted catalog and internal capability from the prior laptop
   backup, only into absent files. Restored descriptive firmware configuration
   from the known Phase 2D candidate metadata. No browser session or physical
   firmware permission was restored by those files.
6. Verified the captured surviving-data manifests were byte-identical before
   reloading Jive.
7. Used the established detached stop/bootstrap/start procedure once. Jive had
   scanned the stock tree at boot, so a rescan was necessary. It was stopped
   before the applet bind was activated. The operating-system boot ID did not
   change during restoration.
8. Confirmed all three native services and HTTP reachability. The user confirmed
   both applets and audible station playback.

Not every deleted file can be reconstructed without a backup. This restoration
does not claim otherwise: surviving Extended Storage/userpath data was preserved,
and the specified missing Base files were recovered from known evidence. The
earlier catalog/capability backup is a snapshot, not proof of arbitrary settings
that may have been changed after it was taken.

## Retained startup architecture

```text
firmware /init cleanup
  keeps /etc/ under the observed policy
             |
stock /etc/init.d/rcS
             |
/etc/init.d/rcS.local (before squeezeplay start)
             |
/etc/standalonebase/storage-boot.sh
             |
validate existing sbdata -> select proven filesystem -> mount if needed
             |
validate existing applets -> bind /mnt/sbdata/applets if needed
             |
stock SqueezePlay startup -> Base Meta/initialization
             |
existing sbbase + sbwebserver + sbproxy supervision
```

Retained files are the bootstrap, the existing small verifier helper, and, only
for compatible legacy firmware, the unmodified authorized module. Existing
configuration consumers continue using `/mnt/storage/standalonebase`, now a
symlink to `/etc/standalonebase`. This avoids changing every internal client or
exposing internal credentials to browsers.

The migration verifies copied configuration before retaining the previous
directory as a backup and installing the alias. Conflicting retained/legacy
directories stop for review rather than being merged destructively. Existing
files are backed up before replacement, new support files and hooks are promoted
atomically, and the hook is published only after its dependencies exist.
Repeated successful repair leaves the hook and persistent file contents/list
unchanged. An interrupted support-file installation can be retried; ambiguous
configuration conflicts stop safely and preserve both copies.

The device lacks `readlink` and `cmp`. Alias/bind checks therefore use the verified
shell `test -ef`; file comparison uses the already bundled native helper's
`verify-copy`. No extra BusyBox applet, library, daemon or package was added.

## Native and legacy compatibility

- Seeing `ubifs` in `/proc/filesystems` is **not** sufficient: old kernels already
  list it. Native capability uses the fixed per-volume BDI registration
  `ubifs_0_2`, or an already verified native mount of the second volume.
- Prefer native `mount -t ubifs ubi0:sbdata /mnt/sbdata` when this capability is
  established. A native failure never falls back to loading the old module.
- A correct existing native or legacy mount is left in place. A foreign volume,
  filesystem or duplicate mount elsewhere is refused. The production applet bind
  additionally checks that source and target are the same directory.
- Legacy module loading is restricted to the established Baby hardware/kernel
  and known firmware releases (7.7.2 r9663, 7.7.3 r16676, 9.0.1 r17084,
  9.0.2 r17146), with the existing exact module identity verification. Unknown
  legacy firmware/kernel combinations are refused rather than trial-loaded.
- Healthy-volume identity, geometry, update/corruption markers and the existing
  applet sentinels remain mandatory. No missing volume is created by boot/repair.
- A fresh native installation without `sbdata` continues using ordinary storage;
  provisioning a new native volume is not implemented in this phase. Native
  initialization/reinitialization is refused, not used as a recovery fallback.

Firmware staging, artwork caching, repository storage, station configuration,
installer routing and native storage status now recognize the two known mount
types with the expected backing-volume identity. Merely changing a mount type
string would have left these consumers disabled or using fallback storage.

## Installation and repair behavior

`storage-setup.sh check` / `analyze` remain diagnostic: they do not mount or repair.
New fields include `NATIVE_MULTI_UBIFS`, `BOOT_SUPPORT_RETAINED`, and
`REPAIR_SUPPORTED`. Existing healthy data is not offered as fresh initialization.
The older explicitly confirmed legacy erase operation remains separate; it is
not called by repair, startup, or this task.

The Base applet chooses non-destructive repair when its supported repair flag is
set, otherwise retaining the established legacy recovery path. A menu action,
**Repair startup integration (preserve data)**, exposes the same operation.
Services remain on the normal Jive startup/supervision path. Repair itself does
not add a service registration, start duplicate processes, or switch an applet
tree beneath a live Jive instance.

The Firmware Updates page includes `bootstrapRetained` status and warns when the
retained hook/target is missing. Installed support is described as installed,
**not** as a guarantee of future firmware compatibility. The physical firmware
checkbox, heartbeat, Comet offer, Review action and native installer are unchanged.

## Safe shell recovery

After inspecting a healthy existing volume and installing the current Base
applet from a trusted package, run:

```sh
/bin/sh /usr/share/jive/applets/StandaloneBase/storage-setup.sh analyze
/bin/sh /usr/share/jive/applets/StandaloneBase/storage-setup.sh repair
```

If Base is only present on an already verified/mounted Extended Storage tree,
the source can be selected explicitly:

```sh
SB_STORAGE_APPLET_DIR=/mnt/sbdata/applets/StandaloneBase \
  /bin/sh /mnt/sbdata/applets/StandaloneBase/storage-setup.sh repair
```

Do not substitute initialize, erase, or volume-management commands. Inspect
`/tmp/sbstorage.log`, `/tmp/sbstorage-check.log` and `/var/log/messages` if repair
stops. Verify the hook chain, files, mount identity and backups before restarting.
If repair cannot establish compatibility, retain the data and seek review.

## Files changed

- `scripts/storage-boot.sh`: native capability selection, conservative legacy
  loading, exact mount/bind checks, no duplicate activation.
- `scripts/storage-setup.sh`: retained support/alias migration, atomic hook
  repair, native-aware status/mounting, repair-only entry point and safety gates.
- `applet/StorageManager.lua`, `StandaloneBaseApplet.lua`: repair operation,
  automatic selection and the non-destructive menu action.
- `applet/InstallerCompat.lua`: route native as well as legacy Extended Storage.
- `native/sbstorage/sb_storage_helper.c`: correct native/legacy mount and bind
  status, retaining the historical direct bind notation.
- `native/sbwebserver/src/firmware.c`, `radio_stations.c`, `repository.c`,
  `image_render.c`: recognize verified native storage; report retained boot support.
- `native/sbwebserver/web/js/firmware.js`, `web/index.html`: recovery warning and
  cache version, no installation control changes.
- Storage, installer and browser regression fixtures plus `tests/CMakeLists.txt`.

## Validation evidence

Host configured suite: **49/49 passed** (150.06 seconds), with the updated native
helper and both mount modes. The final focused storage suite passed **9/9**;
the native/repair fixture contains **11** cases covering absent/foreign storage,
idempotency, no-module native activation, interrupted repair, conflict preservation,
retention-policy simulation and refusal of native initialization. Existing legacy
recovery/initialization fixtures remain host-only mocks, not device operations.
Optional Playwright firmware-page tests passed, including the missing-bootstrap
warning. The ARMv5 static build passed; existing image-path and NanoSVG warnings
remain. No new native dependency was introduced.

On the actual Radio, the restoration data manifests matched, normal playback was
confirmed by the user, and two repair runs reported Active. The second repair
left its persistent file contents/list unchanged. Repeated bootstrap activation
reused both mounts and confirmed the real bind inode identity. Private evidence,
backups and diagnostic helpers are under ignored `.local/phase2e/`.

### Controlled reboot test

**Bootstrap persistence passed; reboot stability did not pass.** Exactly one
normal reboot was requested after
confirming the complete startup chain, idle preparation state, service counts,
mount/bind identities, retention policy and data hashes.

- Before boot ID: `079affe2-2529-42a7-9a10-ecc6a9b9e0ad`.
- Initial post-request boot ID: `c7a245a8-e171-417d-845e-64a0d71fc288`.
- Firmware remains 9.0.2 r17157; address remains 192.168.1.228.
- Storage status Active, native multi-UBIFS detected, retained support present,
  no pending repair or initialization/erasure offer.
- Both `/mnt/sbdata` and the applet bind are native UBIFS.
- One process each for `sbbase`, `sbproxy`, `sbwebserver`; HTTP returned 200.
- Stations, the staged firmware, the catalog and the internal-capability file
  have the same hashes before and after reboot.
- The module list contains no `sbubifs`; native mounting did not load it.

Those initial startup/data checks succeeded, but later review of the captured
log found `semaphore 819:0 was not changed`, repair error 2 and a watchdog
shutdown. A follow-up confirmed an additional reset, with boot ID
`316211f9-c1a3-4ac6-b437-a8ec710c1aa4`. No second reboot command was issued.
The retained bootstrap again restored both native mounts and all services
automatically. The subsequent boot remained up for several minutes, with normal
permission heartbeats and no further watchdog error in the observed log.

The snapshot shows a large `md5sum` process running during the watchdog event
(including `rt_spin_lock` as its sampled wait location). Instrumentation/flash
read load is therefore a confounder, **not a proven cause**. Do not attribute this
reset to the bootstrap, firmware flashing, or OOM without further evidence.
No more large firmware hashing or reboot experiments were performed afterward.
Small configuration hashes and the staged image's presence/size were rechecked.
Watchdog configuration, kernel behavior and the firmware updater were untouched.

The acceptance requirement of no unexpected reboot is explicitly **not met**.
This remains a separate stability investigation; functioning retained storage
must not be presented as full reboot-stability acceptance.

Private machine-readable result: `.local/phase2e/reboot-test.json` now records
`bootstrap_recovery_passed: true`, `stability_passed: false`, `passed: false`.
The first snapshot and full captured watchdog evidence are preserved alongside
it. No additional mount, bind or repair command was run after reboot to make
these checks pass.
The user confirmed after reboot that Standalone Base and Standalone Radio are
visible and that the shortcut plays NPO Radio 2. This confirms physical playback
and applet recovery, but does not resolve the watchdog-stability issue above.

## Limitations and recommendations

- The actual failure cause is established for this upgrade. A future firmware
  may change `/init`, retention policy, hook ordering, module ABI or applet APIs.
  Do not claim universal automatic recovery. Keep an off-device backup and
  verify compatibility before updating.
- This restoration preserves the existing full applet tree, including its stock
  applet copies. It does not silently rebase stock applets from a new firmware.
  A future firmware with changed stock applet APIs can require a reviewed rebase
  or reinstall/recovery from its stock tree. The legacy rebase transaction remains
  separate from native repair; it is not invoked against a live bound tree.
- A successful reboot on r17157 demonstrates this installed chain on r17157,
  not another cross-version upgrade or a factory-reset survival guarantee.
  Host retention simulation and source evidence are not another physical flash.
- No filesystem capability is inferred solely from a release number. Unknown
  native/legacy combinations fail safely and keep the data available for review.
- No actual firmware installation, kernel or watchdog work belongs to this phase.
  Transfer/watchdog risk investigation remains separate.

Before future firmware updates: back up userpath/settings, Base retained state
and Extended Storage off-device; check that retained boot support is installed;
review target firmware compatibility; then use only the existing physical native
update workflow. Afterward verify mounts, services and playback before deleting
any backups. Never initialize a surviving volume to recover missing startup files.
