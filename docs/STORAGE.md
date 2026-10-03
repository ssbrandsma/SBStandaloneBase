# Storage management assessment

## Milestone scope

StandaloneBase now provides read-only storage discovery through `sb-storage-helper` and the `StorageManager` Lua service. No resize ioctl, formatter, volume removal, early-boot hook, or enabled expansion action is present. `sb-storage-helper expand` always exits 78.

The applet is loaded at boot, launches flat packaged executables from its own directory, and exposes services through AppletManager. The new services are `getStandaloneStorageInfo` and `getStandaloneStorageCompatibility`. The Storage screen reports measured filesystem capacity and a disabled compatibility result. Other applets should call these services instead of parsing UBI themselves.

## Detection and compatibility

The helper reads `/proc/cpuinfo`, `/proc/mtd`, `/proc/mounts`, `/proc/sys/kernel/osrelease`, `/etc/squeezeos.version`, `/sys/class/ubi`, and `statvfs(3)`. It identifies the writable volume from the `ubifs` mount at `/mnt/storage`, then matches the UBI volume by name. It never assumes volume ID 2.

Compatibility is fail-closed. The allow-list recognizes only firmware 7.7.3 r16676 and 9.0.1 r17084 on the Baby/Radio model and ARM Linux 2.6.26. Firmware identity alone never passes the check: it also requires exactly one UBI device, the expected firmware volume names (plus the explicitly allowed `ubifs_test`), `ubi0:ubifs` mounted as UBIFS, one unambiguous dynamic `ubifs` volume, no corruption marker, no UBI read-only marker, and enough free PEBs to reach 64 MiB. A passing result means “eligible for isolated validation,” not “safe to resize.”

Target LEB count is `ceil(64 MiB / usable_eb_size)`. At 129024 bytes this is 521 LEBs. UBI volume capacity and the smaller usable UBIFS capacity are deliberately reported separately.

## Source-verified facts

- Linux UBI exposes volume resize as `UBI_IOCRSVOL`; the v2.6.26 UBI control path validates the request and calls `ubi_resize_volume`.
- Mainline UBIFS v2.6.27 reads the current UBI volume size at mount and has auto-growth logic, but growth is capped by the UBIFS superblock `max_leb_cnt`. A writable mount updates the superblock when growth succeeds.
- UBI’s `corrupted` attribute represents an interrupted volume-update marker. It is not a general UBIFS integrity check.
- UBIFS uses journal replay for power-cut recovery, but this does not prove that a particular vendor backport or resize sequence is safe.

## Compatibility profiles

The prepared image uses the common geometry observed on the 9.0.1 Radio and reported for the 7.7.3 Radio: minimum I/O 2048, LEB size 129024, LZO, format `w4/r0`, and `max_leb_cnt=521`. This is the leading single-image profile, but support for both firmwares is not yet established: the 7.7.3 device must be inspected and must successfully complete the isolated test. Firmware strings are never used as a substitute for geometry and superblock checks.

Read-only inspection of the 9.0.1 r17084 production volume found `w4/r0`, `leb_cnt=82`, and `max_leb_cnt=82`. Enlarging only that UBI volume would not allow UBIFS to grow beyond 82 LEBs. This rules out an in-place production resize on that filesystem without a separately designed backup/recreate/restore process, which remains outside the approved scope.

The 7.7.3 r16676 device at `192.168.1.222` was offline during this milestone. Its actual production `max_leb_cnt`, test-volume identity, and vendor behavior remain unverified.

Primary references:

- <https://github.com/torvalds/linux/blob/v2.6.26/include/mtd/ubi-user.h>
- <https://github.com/torvalds/linux/blob/v2.6.26/drivers/mtd/ubi/cdev.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/sb.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/super.c>
- <https://www.kernel.org/doc/html/v6.15/filesystems/ubifs.html>

## Unverified and risky

The Radio’s `2.6.26.8-rt16` UBIFS is a vendor backport. Mainline did not contain UBIFS until Linux 2.6.27, and no matching Logitech vendor patch set was located, so exact source-level equivalence cannot be claimed. The 7.7.3 production filesystem’s `max_leb_cnt`, factory-reset target mapping, behavior while mounted beneath UnionFS, and firmware-upgrade cleanup behavior remain unverified. A resize interrupted by power loss may leave UBI allocation changed while UBIFS has not committed its enlarged geometry. Never remove and recreate the production volume.

Production resizing remains prohibited until the test-volume procedure passes, an off-device backup is restored successfully, vendor kernel behavior is identified, and an offline/early-boot design is reviewed.

## Build

`scripts/build-arm.sh` uses the existing Bootlin ARMv5 musl toolchain and produces a stripped, statically linked EABI5 soft-float helper. It has no shared-library dependency or daemon. CMake builds a host version where a host compiler is available. Mock tests redirect reads using `SB_STORAGE_ROOT`; this variable affects only read-only commands.

`tools/ubi/Dockerfile` pins historical mtd-utils commit `bb56df1e1a84304ec4a14169e4cdc41116ed4256` (2009-06-05, mkfs.ubifs 1.3). The patch only supplies modern glibc device-number macros; GNU89 mode preserves the historical inline semantics. `tools/ubi/build.ps1` extracts the resulting host builder. `tools/ubi/build-wsl.sh` is a developer fallback and uses an explicitly supplied dependency prefix.

Artifacts and hashes are in `artifacts/ubi`. The test image superblock is `w4/r0`, `leb_cnt=14`, `max_leb_cnt=521`, minimum I/O 2048, LEB size 129024, and LZO compression. In format v4 the reader version is not a separate superblock field; the inspector reports `r0` as the historical reader level associated with this format.

The source revision and build recipe are reproducible. A newly generated UBIFS image is not expected to be byte-identical because mkfs embeds time- and randomness-derived metadata; validate its decoded format and geometry rather than requiring the checked-in image hash from a fresh build. Docker Desktop was unavailable during this milestone, so the equivalent pinned WSL build produced and tested the checked-in binary. The Docker recipe remains the canonical clean build and still needs one successful run on a host with a working Docker daemon.
