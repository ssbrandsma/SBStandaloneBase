# Dedicated UnionFS applet overlay validation

Date: 2026-10-04

Target profile: Squeezebox Radio, SqueezeOS 7.7.3 r16676, Linux
2.6.26.8-rt16, UnionFS 2.5.1.

## Verdict

**FAIL.** A second independent UnionFS mount is supported, but Logitech's exact
UnionFS implementation explicitly rejects a UnionFS filesystem as one of its
branches. The proposed lower branch, the currently visible
`/usr/share/jive/applets`, belongs to the existing root UnionFS. The intended
two-branch architecture therefore fails the driver's mount-time validation
with `EINVAL` before it can provide an applet view.

The kernel comments warn that overlapping or incoherent branches can cause
inconsistencies and even kernel oopses. In accordance with the task's static
stop condition, no physical UnionFS mount or test-data write was attempted.

The recommended architecture remains a migrated complete applet-tree snapshot
on `sbdata`, activated with a bind mount. A future investigation should also
consider adding an `sbdata` applet directory to Jive's search path before
discovery and explicitly directing Applet Installer there.

## Sources inspected

- Exact prepared Logitech kernel:
  `/tmp/sb-kernel-exact/linux-2.6.26/fs/unionfs`
- `fs/unionfs/main.c`, especially `check_branch()`,
  `parse_dirs_option()`, and `is_branch_overlap()`
- `fs/unionfs/inode.c`, `copyup.c`, `rename.c`, `unlink.c`,
  `lookup.c`, and `dirfops.c`
- `Documentation/filesystems/unionfs/rename.txt`
- [LMS-Community SetupAppletInstaller source](https://github.com/LMS-Community/squeezeplay/blob/master/src/squeezeplay/share/applets/SetupAppletInstaller/SetupAppletInstallerApplet.lua)
- [LMS-Community AppletManager source](https://github.com/LMS-Community/squeezeplay/blob/master/src/squeezeplay/share/jive/AppletManager.lua)
- Existing SBUBIFS feasibility and physical-validation reports

## Existing architecture

The firmware constructs the root view before entering its chroot:

```text
existing root UnionFS
  branch 0: /mnt/storage (production UBIFS, rw)
  branch 1: /            (firmware CramFS, ro)
  result:   running /
```

The relevant running paths are:

```text
/usr/share/jive/applets
  combined existing UnionFS view

/mnt/storage/usr/share/jive/applets
  production UBIFS branch only

/mnt/storage/mnt/storage/usr/share/jive/applets
  unrelated legacy nested directory; out of scope
```

The requested design was:

```text
new dedicated UnionFS
  branch 0: sbdata applets (rw)
  branch 1: /usr/share/jive/applets (ro, existing combined view)
  mountpoint: /usr/share/jive/applets
```

The second branch resolves to a UnionFS superblock and is rejected.

## UnionFS source findings

### Independent mounts

UnionFS state is allocated per superblock. The source contains no singleton
registration or fixed-name resource that would prevent a second independent
mount. A second mount is therefore supported when every branch is an acceptable
non-UnionFS directory and the branches do not overlap.

### Stacking is explicitly prohibited

`check_branch()` documents and enforces:

```c
/* we're not trying to stack unionfs on top of unionfs */
if (!strcmp(nd->path.dentry->d_sb->s_type->name, UNIONFS_NAME))
    return -EINVAL;
```

`parse_dirs_option()` resolves each branch with `path_lookup(...,
LOOKUP_FOLLOW, ...)` and passes it to `check_branch()`. Symlinking or bind
mounting the existing merged applet directory under another name does not
avoid the check: path resolution still reaches the UnionFS superblock.

This is not merely a conservative recommendation. It is a hard mount-time
failure in the tested kernel source.

### Branch ordering and requirements

- Branch zero is the highest-priority branch.
- The leftmost branch must be writable unless the complete UnionFS mount is
  read-only.
- New entries are created in the first suitable writable branch.
- Opening a lower read-only file for writing triggers delayed copy-up.
- Directory enumeration merges branch contents and suppresses whiteouts.
- Unlinking a lower-only object creates a whiteout in branch zero when the
  lower object cannot be removed because its branch is read-only.
- Copy-up reproduces file data, permissions and, where enabled, extended
  attributes.

These mechanics match the desired applet behavior, but only after branch
validation succeeds.

### Overlap and recursion

The driver walks parent dentries to reject ancestor/descendant overlap between
branches. Its source explains that Linux lacked the cache-coherency support
needed for overlapping stackable branches and that allowing overlap could
produce inconsistent deletion visibility and kernel oopses.

A UnionFS mount must not use a path that it subsequently obscures as one of its
branches. Doing so would create a recursive dependency even if the explicit
UnionFS-on-UnionFS check were removed.

The destination itself may be a directory currently visible through the root
UnionFS; VFS mountpoints are not rejected based on their containing
filesystem. Safety depends on all branch paths resolving independently of that
destination. The proposed lower branch fails that requirement.

### Rename limitations

The bundled rename documentation shows that non-empty directory renames can
return `EXDEV`, particularly when copy-up would be required. Empty-directory
renames and ordinary file replacements are supported in more cases. Applet
Installer does not rely on atomic whole-directory rename, but any future
staging-and-rename design would need to account for these restrictions.

### Unmount behavior

Normal unmount releases per-superblock branch references after open users and
the mountpoint are quiescent. This does not resolve a recursive branch
dependency; such a topology is rejected before mount. A production mount over
the active applet directory would also remain busy while Jive holds files, so
startup-before-Jive and shutdown ordering would still be required.

## BusyBox mount syntax

The Radio's existing root mount demonstrates the accepted syntax:

```sh
mount -t unionfs -o dirs=UPPER=rw:LOWER=ro none MOUNTPOINT
```

Branch order is left to right, highest priority first. The syntax is not the
blocker; resolved branch filesystem identity is.

## Alternative non-recursive UnionFS layout

In principle, this three-branch layout avoids UnionFS-on-UnionFS:

```text
sbdata applets                              rw
/mnt/storage/usr/share/jive/applets         ro
raw CramFS /usr/share/jive/applets          ro
```

It would present new applets, production-installed applets, and firmware
applets in one view. It is not available safely in the running system:

- the normal `/usr/share/jive/applets` path is already the root UnionFS view;
- the raw CramFS applet path is not exposed as a separate runtime path inside
  the chroot;
- exposing or remounting the firmware branch would require changes to early
  boot or additional low-level mounts outside this authorization;
- firmware and production branches would have to remain independently stable
  for the lifetime of the new union.

Snapshotting the combined view into `sbdata` and bind mounting it is simpler
and does not need a second stackable filesystem.

## Physical-test status

### Phase 2 through Phase 6

**BLOCKED / NOT TESTED by design.**

No directory, mountpoint, module, branch, whiteout, or payload was created for
this task. The previously validated SBUBIFS module was not loaded. `sbdata`
was not mounted. The Radio was not contacted, because Phase 1 established the
mandatory stop condition before physical testing.

Consequently Tests A through G are **NOT TESTED** for the proposed topology.
A temporary UnionFS using a RAM lower branch might demonstrate generic UnionFS
operations, but it could not validate the architecture that is blocked by
`check_branch()`; performing it would add physical risk without answering
the critical question.

### Mount tables

No before/after device mount tables were collected because physical execution
was prohibited after the static failure. The last completed physical
validation recorded:

```text
ubi0:ubifs /mnt/storage ubifs rw,noatime
none / unionfs rw,dirs=/mnt/storage=rw:/=ro
sbdata unmounted
sbubifs unloaded
kernel taint 0
```

This task made no device-side change, so there is no cleanup delta.

## Applet Installer compatibility

The installer finds its destination by scanning `package.path`, appending
`applets`, and selecting the first existing directory. On the Radio this is
the ordinary merged path `/usr/share/jive/applets`.

Installation and update use normal path-based operations:

1. recursively enumerate and remove the existing applet directory;
2. call `lfs.rmdir()` on it;
3. recreate it with `lfs.mkdir()` if absent;
4. stream ZIP entries directly to `io.open(..., "w")`;
5. persist version settings and request the reboot service.

The code does not canonicalize the destination, compare device or inode
identity, use `/mnt/storage` directly, stage an atomic directory replacement,
or understand external-storage failure. It follows the VFS namespace and would
therefore normally work through a transparent bind or valid UnionFS mount.

Removal is recursive and suppresses individual `os.remove` and `lfs.rmdir`
errors with `pcall`. On a UnionFS lower-only applet, this would rely on
whiteout behavior. Updates are remove-then-extract and are non-atomic; a power
loss or storage loss can leave a partial applet.

AppletManager also enumerates applet directories from `package.path`. It
records the first occurrence of each applet name and constructs module paths
from that discovery. Any storage mount or search-path change must therefore be
active before its discovery pass. The existing `rcS.local` hook is early
enough in principle, but this task did not create or modify it.

Because the proposed UnionFS cannot be mounted, the existing installer cannot
remain unchanged *with that architecture*. Its ordinary filesystem behavior is
compatible with the recommended bind-mounted complete snapshot, subject to a
later isolated end-to-end installer test.

## Alternative comparison

| Design | Built-ins preserved | Installer compatibility | Failure behavior | Complexity and risk |
|---|---|---|---|---|
| Dedicated nested UnionFS | Intended yes | Ordinary operations would fit | Cannot mount; hard `EINVAL` | **Rejected** by exact kernel |
| Per-applet symlinks | Yes | Fragile recursive removal; dangling-target failures | Individual applets can disappear | Low code, high operational edge cases |
| Complete snapshot + bind mount | Yes, from migration snapshot | Path stays an ordinary directory | Skip bind to retain original view | Moderate migration/reconciliation work; lowest kernel risk |
| Additional Jive search path | Yes | Installer still needs explicit destination | Original applets remain if storage fails | Clean namespace model; requires early Jive configuration/source work |
| Installer destination change alone | Yes | Explicit `sbdata` writes | Does not make new applets discoverable alone | Must be paired with search-path support |

### Recommended production architecture

For the next controlled milestone:

1. Mount verified `sbdata` with the exact SBUBIFS module before SqueezePlay.
2. Transactionally copy the complete currently visible applet tree to
   `sbdata/applets`, preserving modes, ownership and symlinks.
3. Verify required sentinels, including firmware applets and StandaloneBase.
4. Bind mount `sbdata/applets` over `/usr/share/jive/applets`.
5. If any prerequisite fails, do not activate the bind and leave the original
   root UnionFS view intact.
6. Run a separately authorized isolated Applet Installer install/update/remove
   test before production adoption.
7. Define firmware-update reconciliation because a migrated snapshot masks
   later firmware applet changes.

Longer term, investigate an early additional `package.path` applet directory
plus a small installer destination change. That avoids duplicating firmware
applets and avoids another UnionFS layer, but it requires Jive-level changes
and discovery/precedence validation.

## Direct answers

1. **Does a second UnionFS mount work?** The driver supports independent
   mounts, but not with the existing root UnionFS view as a branch.
2. **Can three logical applet sources be presented as requested?** Not through
   the proposed topology. A theoretical three-branch layout needs an
   independently exposed raw CramFS path that is unavailable at runtime.
3. **Do new files land on `sbdata`?** NOT TESTED; the proposed mount is
   statically rejected.
4. **Do updates, deletions and whiteouts work?** Generic source paths implement
   them, but the proposed physical topology was not mounted or tested.
5. **Can Applet Installer remain unchanged?** Not with the rejected topology.
   It is likely compatible with a full-tree bind after separate validation.
6. **Was the original filesystem affected?** No device-side action occurred.
7. **Was the device returned to its original state?** It was never changed.
8. **Next implementation step?** Validate complete-tree migration plus a bind
   mount in an isolated, reversible physical milestone; do not implement
   production boot integration yet.

**Final verdict: FAIL — Logitech UnionFS 2.5.1 prohibits the required nested
branch topology. Use a verified complete-tree bind design or investigate an
early Jive search path plus installer destination change.**
