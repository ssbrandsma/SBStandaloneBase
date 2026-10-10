# Existing-UBIFS expansion research

## Verified boot sequence

RedBoot loads the selected kernel and CramFS firmware. The kernel exposes the
NAND as `mtd0` (`redboot`) and `mtd1` (`ubi`), attaches `mtd1` as `ubi0`, and
creates MTD emulation entries for its UBI volumes. `/linuxrc`, stored in the
read-only CramFS, runs before normal init. It searches `/proc/mtd` for `ubifs`,
mounts the exact UBI name `ubi0:ubifs` at `/mnt/storage`, and then mounts
UnionFS with `/mnt/storage` as its writable branch and CramFS `/` as its
read-only branch. Finally it chroots into the UnionFS root and executes init.

`/etc/init.d/rcS` runs only after that transition. Its `rcS.local` hook and
media `squeezeos-boot.sh` hooks therefore run too late to remove, recreate or
restore the production filesystem safely. The updater binary and its state
would also disappear if they lived only on `/mnt/storage` when that volume was
removed.

No existing pre-production-mount updater or recovery hook was found. The stock
firmware provides `ubimkvol`, `ubirmvol` and `ubiupdatevol`, but no `ubirsvol`,
SHA-256 utility or native `mkfs.ubifs`. Modifying CramFS `/linuxrc`, RedBoot or
firmware is outside this milestone. Consequently unattended Option 1 is not
deployable through the current applet alone: it first needs a separately
reviewed early-boot environment resident outside production UBIFS.

The fragile `grep ubifs /proc/mtd | cut -c4` lookup remains boot-critical. No
temporary or replacement volume may contain `ubifs` in its name. The final
production volume must be named exactly `ubifs`. UBI ID 2 is not inherently
required for mounting, but current scripts, recovery assumptions and safety
checks expect it. Deleting volume 2 does not guarantee automatic reuse of ID 2;
the migration must explicitly request ID 2 and verify it before continuing.

## Physical state and geometry

Read-only inspection of `192.168.1.222` confirmed:

```text
total UBI LEBs: 1014
internal/unavailable overhead: 14 LEBs
current user-volume budget: 1000 LEBs
fixed kernel/CramFS volumes: 258 LEBs
production ubifs: 82 LEBs
sbdata: 521 LEBs
free: 139 LEBs
LEB/minimum I/O: 129024/2048 bytes
```

The five protected production/firmware volumes remain healthy. `sbdata` ID 5
contains the validated compact filesystem image and remains unmounted after the
second-UBIFS mount failure. It was not changed during this research.

## Capacity budget

Keeping the 521-LEB `sbdata` allocation while deleting production volume 2
would permit only `139 + 82 - 64 = 157` replacement LEBs after a 64-LEB free
reserve. That is about 19.3 MB nominal and does not meet the objective.

The existing host backup is 3,528,192 bytes uncompressed and 1,643,797 bytes
when gzip-compressed. Forty LEBs provide 5,160,960 bytes. If a future verified
backup fits in a 40-LEB raw container, shrinking/recreating the backup container
to 40 LEBs changes the budget to:

```text
1000 - 258 fixed - 40 backup - 64 reserve = 638 target LEBs
638 * 129024 = 82,317,312 nominal bytes
```

A conservative 600-LEB production target is 77,414,400 nominal bytes and leaves
102 free LEBs while the 40-LEB backup remains. The target and backup sizes must
be calculated from current measurements; 40 and 600 are evidence-based examples,
not constants. UBI's 14-LEB internal overhead and bad-block reserve are already
excluded from the observed 1000-LEB user budget.

Because `ubirsvol` is absent, a future bundle needs a small audited ARMv5 UBI
resize utility or must copy the verified archive elsewhere, remove `sbdata`, and
recreate it smaller. The latter has a moment with no on-device backup and is not
acceptable without another verified copy.

## Filesystem format choice

A full-target image wastes temporary space and makes backup coexistence harder.
The historical formatter has already produced a compact 14-LEB `w4/r0` image
with a 521-LEB ceiling. For rebuilding production, strategy B is preferred:
format a compact image with `max_leb_cnt` equal to the calculated final volume,
write it after creating that final-sized UBI volume, and let the vendor kernel
grow `leb_cnt` on its first and only UBIFS mount. This preserves internally
consistent LPT geometry; editing the old superblock is rejected.

Physical evidence does not yet prove the first-mount growth path because the
kernel cannot mount a second UBIFS concurrently. It must be tested in an early
boot/recovery environment where production UBIFS is not already mounted.

## Metadata preservation

The backup must archive the underlying writable branch `/mnt/storage`, not the
merged UnionFS view. It must preserve mode, UID/GID, symlinks, timestamps,
device nodes, FIFOs and UnionFS whiteouts. BusyBox 1.18.2 tar has only basic
`c/x/t/z/v/O/h/f/C` options and offers no xattr or ACL switches. Current
read-only inspection found a symlink but no proof that all future whiteout or
special-node semantics round-trip.

Therefore the stock BusyBox archive alone is not yet an approved preservation
mechanism. A bundled static archiver with explicit metadata/whiteout support, a
manifest and a device-side restore test is required. The existing online UBIFS
image remains non-atomic and is not the migration backup.

## Conclusion

Option 1 is technically plausible without changing the kernel's UBIFS code,
but it is not safely deployable through SBStandaloneBase until a pre-UBIFS boot
environment and metadata-complete backup/restore path are implemented and
physically validated.
