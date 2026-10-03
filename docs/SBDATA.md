# Separate `sbdata` storage design

## Status and evidence

The target Radio uses ARMv5TEJ, Linux 2.6.26.8-rt16, SqueezeOS 7.7.3 r16676, UBI LEB size 129024, and the fixed production volume map `0:kernel_bak`, `1:cramfs_bak`, `2:ubifs`, `3:kernel`, `4:cramfs`. The production UBIFS is the writable UnionFS layer at `/mnt/storage`. These facts and bind mounts over the live applet directory were verified on physical hardware. No `sbdata` volume has been created or mounted by this implementation.

The old in-place growth experiment is retired. Production `ubifs` remains untouched and `ubifs_test` is forbidden because `/linuxrc` selects its UBIFS MTD match with an unsafe substring search. The helper contains no UBI ioctl or resize command.

## Proposed architecture

A separately approved installation would create one dynamic volume named exactly `sbdata`, format it with the checked historical UBIFS `w4/r0` profile, mount it at `/mnt/sbdata`, and bind `/mnt/sbdata/applets` over `/usr/share/jive/applets` after UnionFS initialization and before SqueezePlay starts. It must first copy the currently visible merged applet tree, not merely the upper-layer directory. This preserves firmware applets and installed overlay applets at migration time.

Mounting `sbdata` on `/mnt/storage/usr/share/jive/applets` from `rcS.local` is not selected: UnionFS is already initialized, and physical testing showed that changes made only through the underlying upper path are not reliably reflected in the active `/usr/share/jive` view.

The Applet Installer writes to `/usr/share/jive/applets`; an active bind therefore directs new installations into `sbdata`. This follows normal VFS path resolution but has not yet been validated with a complete Applet Installer install on `sbdata`.

## Boot and fallback

`scripts/sbdata-boot.sh` is a non-interactive, bounded boot helper intended to be copied to `/mnt/storage/standalonebase`, where it remains reachable without `sbdata`. A future installer would merge a marked call into an existing `/etc/init.d/rcS.local`; it must never overwrite an existing hook. The helper discovers `sbdata` by name, rejects IDs 0-4, verifies type and corruption state, mounts without formatting, validates the staged applet tree, activates the bind, and checks visibility. A missing or failed volume leaves the original directory visible. A failed post-bind check unmounts the bind.

Updating StandaloneBase through Applet Installer while the bind is active writes the update into `sbdata`. The independent boot-helper copy must be refreshed transactionally by a future approved installer/update step. Firmware upgrades may add or change lower-layer applets that the migrated snapshot masks; reconciliation is unresolved and is a production blocker.

## Planned installation transaction (disabled)

`prepare` proposes 64 MiB (521 LEBs at the confirmed geometry) while reserving at least 64 free LEBs. A future implementation would re-run preflight, create `sbdata`, format with the historical compatible formatter, mount it, copy the visible applet tree with permissions/symlinks/timestamps preserved, validate it, install the persistent boot helper and a carefully merged `rcS.local` marker, then activate the bind. Rollback before activation removes only staged integration; runtime fallback unmounts only the bind. It must never modify the five production volumes.

`install` currently exits 78 unconditionally. This remains true with `SB_STORAGE_ROOT`; fixtures cannot unlock writes.

## Validation levels

- Physical evidence: base UBI geometry, UnionFS ordering, bind behavior, and original-directory restoration after unmount.
- Simulated evidence: discovery, preflight rejection paths, complete/partial/absent status, verification, boot mount/bind failures, and fallback unmount.
- Not tested: creating/formatting `sbdata`, old-kernel mount of its final image, full metadata migration, Applet Installer writes, reboot persistence, power loss, firmware upgrade, factory reset, and recovery from an interrupted transaction.

## Next safe hardware test

Copy only the newly built helper to `/tmp` and run `storage-status` and `prepare`. Review their output together with `/proc/mounts`, `/proc/filesystems`, `/sys/class/ubi`, device nodes, installed applets, available tools, and any existing `rcS.local`. Do not copy or invoke the boot helper, create a volume, format, mount, edit boot files, or reboot during that test.
