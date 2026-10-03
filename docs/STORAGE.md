# Storage management assessment

## Milestone scope

StandaloneBase now provides read-only storage discovery through `sb-storage-helper` and the `StorageManager` Lua service. No resize ioctl, formatter, volume removal, early-boot hook, or enabled expansion action is present. `sb-storage-helper expand` always exits 78.

The applet is loaded at boot, launches flat packaged executables from its own directory, and exposes services through AppletManager. The new services are `getStandaloneStorageInfo` and `getStandaloneStorageCompatibility`. The Storage screen reports measured filesystem capacity and a disabled compatibility result. Other applets should call these services instead of parsing UBI themselves.

## Detection and compatibility

The helper reads `/proc/cpuinfo`, `/proc/mtd`, `/proc/mounts`, `/proc/sys/kernel/osrelease`, `/etc/squeezeos.version`, `/sys/class/ubi`, and `statvfs(3)`. It identifies the writable volume from the `ubifs` mount at `/mnt/storage`, then matches the UBI volume by name. It never assumes volume ID 2.

Compatibility is fail-closed. The initial allow-list requires the Baby/Radio model, ARM, firmware 7.7.3 r16676, kernel 2.6.26, exactly one UBI device, the expected firmware volume names (plus the explicitly allowed `ubifs_test`), `ubi0:ubifs` mounted as UBIFS, one unambiguous dynamic `ubifs` volume, no corruption marker, no UBI read-only marker, and enough free PEBs to reach 64 MiB. A passing result means “eligible for isolated validation,” not “safe to resize.”

Target LEB count is `ceil(64 MiB / usable_eb_size)`. At 129024 bytes this is 521 LEBs. UBI volume capacity and the smaller usable UBIFS capacity are deliberately reported separately.

## Source-verified facts

- Linux UBI exposes volume resize as `UBI_IOCRSVOL`; the v2.6.26 UBI control path validates the request and calls `ubi_resize_volume`.
- Mainline UBIFS v2.6.27 reads the current UBI volume size at mount and has auto-growth logic, but growth is capped by the UBIFS superblock `max_leb_cnt`. A writable mount updates the superblock when growth succeeds.
- UBI’s `corrupted` attribute represents an interrupted volume-update marker. It is not a general UBIFS integrity check.
- UBIFS uses journal replay for power-cut recovery, but this does not prove that a particular vendor backport or resize sequence is safe.

Primary references:

- <https://github.com/torvalds/linux/blob/v2.6.26/include/mtd/ubi-user.h>
- <https://github.com/torvalds/linux/blob/v2.6.26/drivers/mtd/ubi/cdev.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/sb.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/super.c>
- <https://www.kernel.org/doc/html/v6.15/filesystems/ubifs.html>

## Unverified and risky

The Radio’s `2.6.26.8-rt16` UBIFS is a vendor backport, so its exact source and patches remain required. The current production filesystem’s `max_leb_cnt`, factory-reset target mapping, behavior while mounted beneath UnionFS, and firmware-upgrade cleanup behavior remain unverified. A resize interrupted by power loss may leave UBI allocation changed while UBIFS has not committed its enlarged geometry. Never remove and recreate the production volume.

Production resizing remains prohibited until the test-volume procedure passes, an off-device backup is restored successfully, vendor kernel behavior is identified, and an offline/early-boot design is reviewed.

## Build

`scripts/build-arm.sh` uses the existing Bootlin ARMv5 musl toolchain and produces a stripped, statically linked EABI5 soft-float helper. It has no shared-library dependency or daemon. CMake builds a host version where a host compiler is available. Mock tests redirect reads using `SB_STORAGE_ROOT`; this variable affects only read-only commands.
