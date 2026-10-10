# `sbdata` applet-storage feasibility

## Verdict

**NO — the existing `sbdata` volume cannot provide normal writable applet
storage within the stated requirements.**

The directory-redirection mechanisms themselves work, but neither symlinks nor
bind mounts provide storage. They require `sbdata` to be mounted first. The only
persistent writable filesystem supported by this kernel is UBIFS, and the
vendor kernel cannot mount a second UBIFS filesystem while production UBIFS is
mounted. No other compiled-in filesystem and device-interface combination can
turn `sbdata` into an ordinary writable filesystem.

No NAND, volume, production directory, boot configuration or firmware was
changed during this investigation. The known failing UBIFS mount was not
repeated.

## Kernel and device findings

The physical Radio's `/proc/filesystems` contains:

```text
sysfs, rootfs, bdev, proc, debugfs, sockfs, pipefs, anon_inodefs,
tmpfs, inotifyfs, devpts, cramfs, ramfs, unionfs, oprofilefs, ubifs
```

Only CramFS and UBIFS are persistent-storage filesystems. CramFS is read-only.
There is no ext2, ext3, JFFS2, squashfs, FAT or other writable conventional
filesystem. The only loaded module is the `ar6000` wireless driver and there are
no filesystem modules available under `/lib/modules`.

The device exposes three different interfaces that must not be conflated:

- `/dev/mtd/7` is a raw MTD character view of the UBI-created `sbdata` MTD
  emulation entry. Raw NAND/MTD semantics are not an ordinary block filesystem.
- `/dev/mtdblock:sbdata` is block major 31 and exposes an MTD block translation
  view. Its existence does not supply a writable filesystem driver; putting a
  non-UBI filesystem through this view would also bypass UBI's volume semantics.
- `/dev/ubi0_5` is UBI character major 253. UBIFS understands this interface,
  but ordinary block filesystems do not.

Eight loop block nodes and `/sbin/losetup` exist. A loop device can expose a
regular image file as a block device, but it cannot make the UBI character
volume into a supported writable filesystem, and there is no suitable writable
block-filesystem driver anyway. BusyBox mount accepts loop and bind options.

The installed format/mount tool set contains `mount`, `umount`, `losetup`,
`ubimkvol`, `ubirmvol` and `ubiupdatevol`. It has no `mkfs.ext2`, `mke2fs`,
`mkfs.ext3` or `mkfs.jffs2`. More importantly, installing a formatter would not
help because the corresponding kernel drivers are absent.

### Candidate assessment

| Candidate | Device/interface | Writable | Result |
|---|---|---:|---|
| UBIFS | `/dev/ubi0_5` / `ubi0:sbdata` | Yes | Correct interface, but second mount fails at fixed BDI name `ubifs` |
| CramFS | MTD block or loop image | No | Kernel support exists, but unsuitable for install/update/uninstall |
| ext2/ext3 | Requires block device | Yes in principle | Kernel driver and formatter absent; UBI character device is not block storage |
| JFFS2 | Raw MTD | Yes in principle | Kernel driver and formatter absent; using it on a UBI-emulated volume is unsupported layering |
| tmpfs/ramfs | Memory | Yes | Not persistent and consumes scarce RAM |
| UnionFS | Existing filesystems | Yes | An overlay, not backing storage; cannot make `sbdata` accessible |

The previous physical test already established the decisive UBIFS failure:

```text
kobject_add_internal failed for ubifs with -EEXIST
bdi_register -> ubifs_get_sb -> vfs_kern_mount
```

`/sys/class/bdi/ubifs` belongs to production UBIFS. This vendor backport uses a
fixed name for every UBIFS backing-device object, so the second mount fails
before its filesystem is initialized.

## Actual applet layout and discovery

The merged runtime applet directory is:

```text
/usr/share/jive/applets
```

Its writable UnionFS branch is:

```text
/mnt/storage/usr/share/jive/applets
```

Installed applets currently visible in the writable branch include
StandaloneBase, StandaloneRadio, StandaloneTime and ChooseMusicSource. Firmware
applets such as SetupAppletInstaller come from the CramFS branch but appear in
the merged runtime path.

`AppletManager:discover()` performs one `_findApplets()` pass over each
`package.path` applet directory before registering or configuring any applet
metadata. It stores each discovered applet's physical path in an internal
database. StandaloneBase itself is loaded only after this enumeration when its
Meta registers. Consequently mounting storage from `StandaloneBaseApplet:init()`
is too late to make previously absent applets participate in the normal startup
discovery pass. A full Jive reload might rediscover them, but would re-register
existing applets and is not a simple or demonstrated startup solution.

Keeping StandaloneBase in production storage would avoid a circular dependency,
but it does not solve the missing second filesystem mount.

## Applet Installer behavior

SetupAppletInstaller determines its destination by scanning `package.path` for
the first `applets` directory. On this Radio that resolves to the merged
`/usr/share/jive/applets` directory.

For installation/update it:

1. recursively removes the named applet directory's contents;
2. attempts to remove that directory;
3. creates the directory if `lfs.attributes()` says it is absent;
4. streams the ZIP directly into files opened beneath that directory;
5. updates its settings and invokes the reboot service.

There is no staging directory followed by an atomic directory rename. An
interrupted update can therefore leave a partially removed or extracted applet.

LuaFileSystem `attributes()` follows symlinks. An individual symlink pointing to
an available directory would generally be seen as a directory, and file opens
would follow it. The recursive remover would delete target contents; its final
`rmdir` on the symlink path is expected to fail, leaving the link and empty
target. This arrangement is plausible but is not an explicit installer contract.
If the target is unavailable, the link is dangling, directory creation at the
link path fails, and extraction cannot recover cleanly. The installer has no
special symlink, external-storage or rollback handling.

A bind mount is more transparent to AppletManager and Applet Installer because
the path remains an ordinary directory. It is preferable to a symlink if the
backing filesystem is already mounted. However, bind mounting cannot occur
until `sbdata` is accessible, which is the unsatisfied prerequisite.

## Temporary-path proof of concept

Without touching production paths, `/tmp` tests confirmed:

- a directory symlink is recognized as a directory;
- reads and writes through it reach its target;
- BusyBox `mount -o bind` works for directories;
- reads and writes through the bind reach the source;
- the temporary bind unmounted cleanly and all fixtures were removed.

This proves VFS redirection behavior only. It does not overcome the storage
mount limitation or prove Applet Installer update/uninstall recovery semantics.

## Startup and failure behavior

The normal operating system remains bootable while `sbdata` stays unmounted;
that is its current state. A hypothetical optional mount failure could be made
non-fatal if all original directories were left intact and no symlink/bind were
activated until after a successful mount.

In practice, SBStandaloneBase starts too late for initial applet discovery and
the UBIFS mount itself fails. A symlink installed persistently before storage is
available would create a dangling path during startup. A bind mount avoids that
failure mode but still requires a successful filesystem mount before SqueezePlay
discovers applets. The stock `rcS.local` hook runs before SqueezePlay and would
be early enough for discovery, but using it does not fix the kernel's second
UBIFS limitation and installing it is outside this investigation.

No automatic formatting or repair should ever occur when optional storage is
missing or corrupt.

## Direct answers

1. **Can existing `sbdata` be used as a normal writable filesystem without a
   kernel patch?** No, not concurrently with production UBIFS on this kernel.
2. **Which filesystem and device interface?** UBIFS on `ubi0:sbdata` is the only
   technically correct writable combination, and it is blocked by the fixed
   BDI-name kernel bug. No alternative combination is supported.
3. **Can symlinks or bind mounts redirect applet storage?** They redirect paths
   correctly once backing storage is mounted. Bind mounts are more transparent;
   neither supplies or mounts storage.
4. **Does Applet Installer support it?** Not explicitly. It likely follows an
   available individual symlink but recursively modifies its target and has no
   handling for dangling links or interrupted extraction. A bind is more
   compatible but cannot be activated here.
5. **Can SBStandaloneBase activate it automatically?** No. Its applet initializes
   after discovery, and the required second UBIFS mount fails regardless.
6. **Will the Radio remain bootable if `sbdata` fails?** Yes only if production
   paths remain untouched and optional redirection is never activated. A
   persistent dangling symlink would make affected applets unavailable.
7. **Smallest practical implementation?** None within the non-negotiable
   requirements. A simple mount-plus-bind design would otherwise be smallest,
   but the mount prerequisite is impossible on the existing kernel.
8. **Remaining risks?** Kernel BDI collision, startup ordering, installer
   non-atomic updates, symlink removal/dangling behavior, and loss of redirected
   applets when optional storage is unavailable.

The investigation stops here as requested. Making this concept work would
require relaxing at least one prohibited constraint—most directly a kernel fix,
a different firmware/kernel with another writable filesystem, or replacement
of production UBIFS—so no implementation milestone is recommended for the
current requirements.
