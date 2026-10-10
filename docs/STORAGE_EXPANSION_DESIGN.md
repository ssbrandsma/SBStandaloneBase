# Existing-UBIFS expansion design

## Architecture

The applet performs read-only discovery, explains eligibility and stages signed
artifacts. It never performs destructive UBI work in the running UnionFS system.
A future early-boot updater, resident in reviewed CramFS/recovery media rather
than production UBIFS, owns the migration. Normal boot resumes only after the
updater records and verifies `complete`.

The implemented `sb-storage-updater` is currently an offline planner and state
machine prototype. `execute` and `install` return 78. Physical state transitions
are accepted only when `SB_STORAGE_SIMULATION=1`, preventing accidental device
use. The applet reports the calculated maximum and the explicit blocker “early
boot environment required”; it does not offer an unsafe confirmation action.

## Proposed preparation transaction

1. Revalidate model, firmware, geometry, protected volume identities and health.
2. Measure the writable branch and enumerate every metadata type.
3. Create a compressed archive directly into a raw `sbdata` update stream while
   production remains mounted, with a separately stored manifest and SHA-256.
4. Read the complete raw backup back and verify its length, digest and archive
   listing. Do not trust BusyBox `wc` on this firmware.
5. Copy updater, formatter, archiver, manifest and state journal into the future
   early-boot environment and verify their signed digests.
6. Record `backup_verified`, then `updater_armed`; only now offer restart.

Step 3 requires a raw container format with redundant headers at both ends,
generation number, payload length, SHA-256, archive format/version and a commit
marker. Writing a filesystem into `sbdata` is unsuitable because the kernel
cannot mount it alongside production UBIFS.

## Early-boot migration

After booting without mounting production UBIFS:

1. Revalidate the UBI topology and journal.
2. Reduce/recreate the raw backup volume to its calculated size without losing
   the verified payload; this requires an audited resize/copy mechanism.
3. Record `destructive_started` redundantly outside the volume to be deleted.
4. Remove only the exact volume named `ubifs`, after verifying its ID and size.
5. Create exactly one dynamic volume named `ubifs`, explicitly requesting ID 2
   and the calculated target LEB count. Stop if UBI does not assign ID 2.
6. Generate the compact historical `w4/r0` image with `max_leb_cnt` equal to the
   target and write it only to the newly verified volume 2.
7. Mount it as the only UBIFS filesystem, confirm automatic growth and capacity.
8. Restore the archive to the empty filesystem using the metadata-capable tool.
9. Verify the restored manifest, whiteouts, configuration and free capacity.
10. Unmount cleanly, mark `complete`, and resume the stock `/linuxrc` path.

No temporary volume name may contain `ubifs`. A replacement should be created
at its final UBI size; only the compact filesystem image grows. This is simpler
and safer than resizing the production UBI volume after formatting.

## Eligibility and UI

The applet may show:

- current total/free storage;
- measured archive size and required backup LEBs;
- calculated target and remaining reserve;
- preparation state and last verified result;
- a detailed explanation that migration is unavailable until the early-boot
  component is installed through a separately authorized mechanism.

Eligibility requires exact hardware/firmware capabilities, not firmware version
alone: geometry, protected volume map, `w4/r0`, volume health, formatter identity,
backup-tool identity, sufficient capacity, external recovery readiness and an
approved early-boot path.

## Idempotence

Read-only discovery and artifact staging are repeatable. Every destructive step
requires the exact preceding durable state plus independent device identity.
Creation treats “already exists and exactly matches” as resumable; any partial,
ambiguous or corrupted object stops. The updater never guesses an ID, silently
formats, automatically deletes a failed volume or proceeds into normal boot with
an invalid filesystem.
