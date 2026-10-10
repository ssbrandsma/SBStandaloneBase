# Storage expansion testing

## Automated coverage

`tests/storage/test_updater.sh` validates:

- capacity calculation for the observed geometry;
- disabled destructive execution (exit 78);
- parsing valid and invalid journals;
- every ordered state transition;
- rejection of skipped and stale transitions;
- rejection of state mutation outside explicit simulation mode.

Existing tests continue to cover UBI identity/geometry rejection, dangerous
volume names, corrupt volumes, formatter identity, RAM/tmp capacity, UBIFS
superblock corruption/truncation and compact `w4/r0` image geometry. QEMU ARM926
runs both static ARM storage binaries.

These are host/QEMU simulations. They do not model UBI wear leveling, atomic
volume-table updates, power loss or the vendor UBIFS mount path.

## Backup/restore test requirements

Before enabling preparation, construct fixtures containing regular files,
empty directories, symlinks, hard links, FIFOs, device nodes, modes, multiple
UID/GID values, nanosecond/coarse timestamps and UnionFS whiteouts. Compare a
canonical metadata manifest before and after archive restoration. Repeat with a
corrupted payload, truncated header, wrong digest and interrupted extraction.

The existing BusyBox tar is not accepted until a physical fixture proves all
required metadata. Prefer a static archive implementation with an explicit
format and manifest.

## Required physical sequence

The next physical test is **not** production migration. Build a reviewable
early-boot/recovery image and boot it without changing RedBoot configuration or
mounting production UBIFS. In that environment:

1. confirm production remains unmounted;
2. mount the existing `sbdata` image as the only UBIFS filesystem;
3. verify the vendor kernel accepts `w4/r0` and grows 14 LEBs toward 521;
4. perform write, sync, unmount and remount persistence checks;
5. leave all production volumes unchanged and return to the normal boot path.

This isolates format/growth validation from the proven concurrent-mount kernel
bug. It requires separate physical authorization.

## Results for this milestone

- Read-only boot/storage inspection: passed on `192.168.1.222`.
- Capacity calculation: automated test passed.
- State-machine sequencing and safety lock: automated host test passed.
- ARM/QEMU build and execution: passed. `sb-storage-updater` is a stripped,
  statically linked ARM EABI5 soft-float executable; QEMU ARM926 passed its
  planner, ordered transitions, invalid-state and exit-78 safety tests.
- Physical migration, resize, deletion, restore and power cuts: not performed.
