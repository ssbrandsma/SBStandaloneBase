# Bind-mounted applet storage physical validation

Date: 2026-10-04

Target: Logitech Squeezebox Radio, MAC `00:04:20:29:16:7f`,
SqueezeOS 7.7.3 r16676, Linux 2.6.26.8-rt16, ARMv5TEJ.

## Verdict

**PASS.** A directory on `sbdata` was bind mounted over an isolated
destination genuinely located in the existing root UnionFS. Reads and writes
through the destination addressed the same SBUBIFS inodes, installer-like
create/update/replace/remove operations succeeded, and a known payload
persisted across bind unmount/remount.

Unmounting immediately restored the destination's original UnionFS contents,
which remained unchanged underneath the bind. Production UBIFS, the real
applet tree, and the legacy nested applet directory remained operational and
untouched except for creation and removal of the explicitly authorized unique
test destination in the production writable branch.

This validates the filesystem mechanism only. It does not deploy the
production applet bind, startup integration, migration, or a real Applet
Installer transaction.

## Source assumptions

The upstream SqueezePlay sources confirm:

- SetupAppletInstaller selects the first existing `applets` directory found
  through `package.path`.
- It recursively removes an old applet, recreates its directory, and streams
  ZIP members to ordinary `io.open(..., "w")` paths.
- It does not canonicalize the destination, compare filesystem identities, or
  access `/mnt/storage` directly.
- Updates are non-atomic remove-then-extract operations.
- AppletManager discovers directories from `package.path` and records the
  first occurrence of each applet before applet initialization.

Sources:

- [SetupAppletInstallerApplet.lua](https://github.com/LMS-Community/squeezeplay/blob/master/src/squeezeplay/share/applets/SetupAppletInstaller/SetupAppletInstallerApplet.lua)
- [AppletManager.lua](https://github.com/LMS-Community/squeezeplay/blob/master/src/squeezeplay/share/jive/AppletManager.lua)

These semantics predict that a transparent directory bind works without an
installer path change, provided the complete tree is present before
SqueezePlay starts. The physical tests below validate the relevant filesystem
operations with synthetic applets, not a real installer invocation.

## Safety and baseline

Only `192.168.1.222` was contacted. The second Radio was not contacted.
No boot file, startup script, production applet, volume definition, firmware,
or configuration was modified. SqueezePlay was not restarted and the Radio
was not rebooted.

The exact SBUBIFS artifact was verified locally, uploaded to RAM, read back
byte-for-byte, and hashed:

```text
sha256 63652ce67df06a78abb84a4986253bdab02fbd7b7c000779c60b3d393ba9566b
size   199119 bytes
```

Baseline mounts:

```text
/dev/root  /            cramfs  ro
ubi0:ubifs /mnt/storage ubifs   rw,noatime
none       /            unionfs rw,dirs=/mnt/storage=rw:/=ro
```

Baseline module state contained only `ar6000`; kernel taint was zero.
`sbdata` was unmounted and verified through sysfs as:

```text
UBI device:       0
volume ID:        5
name:             sbdata
type:             dynamic
reserved LEBs:    521
usable LEB size:  129024
corrupted:        0
update marker:    0
```

Both candidate paths were confirmed absent before creation:

```text
/usr/share/jive/sb-bind-validation-target-2026
/mnt/storage/usr/share/jive/sb-bind-validation-target-2026
```

The real `/usr/share/jive/applets`, its production branch, and
`/mnt/storage/mnt/storage/usr/share/jive/applets` were inspected but never
used as test targets.

## Test topology

SBUBIFS was mounted by this task:

```sh
mkdir /tmp/sbdata-bind-validation
mount -t sbubifs ubi0:sbdata /tmp/sbdata-bind-validation
```

The isolated paths were:

```text
source on sbdata:
  /tmp/sbdata-bind-validation/bind-validation-2026/source

destination in existing root UnionFS:
  /usr/share/jive/sb-bind-validation-target-2026

destination's production UBIFS branch:
  /mnt/storage/usr/share/jive/sb-bind-validation-target-2026
```

Creating `original.txt` through the unmounted destination caused it to appear
at the corresponding production branch path. `df` on the destination showed
the existing root filesystem rather than RAM-backed `/tmp`. This establishes
that the destination was representative of a directory inside the active root
UnionFS.

The source initially contained:

```text
TestApplet/
  TestAppletApplet.lua
  TestAppletMeta.lua
  manifest.txt
```

## Bind mount

The installed mount utility accepted:

```sh
mount -o bind \
  /tmp/sbdata-bind-validation/bind-validation-2026/source \
  /usr/share/jive/sb-bind-validation-target-2026
```

The resulting mount table entry was:

```text
ubi0:sbdata /usr/share/jive/sb-bind-validation-target-2026 sbubifs rw 0 0
```

After binding:

- `original.txt` was hidden;
- `TestApplet` was visible and readable;
- source and destination directories both reported inode 76;
- the Lua file through both paths reported inode 78;
- the real root UnionFS and production UBIFS mounts remained present;
- real applet sentinels remained accessible.

The Radio does not provide `stat` or `cmp`; inode identity, identical reads,
and the mount table supplied the equivalent physical-identity evidence.

## Installer-like operation results

### E1 — New installation: PASS

`NewTestApplet` and its Applet Lua, Meta Lua, and manifest were created
through the bind destination. The exact bytes appeared beneath the source on
`sbdata`. No corresponding directory appeared beneath the production UBIFS
branch path.

### E2 — Update: PASS

`NewTestAppletApplet.lua` was overwritten through the destination from
`new-applet-v1` to `new-applet-v2`. The new bytes were read directly from
the `sbdata` source; production UBIFS remained free of that applet.

### E3 — Replacement: PASS

Two patterns succeeded:

1. A populated replacement directory was created, the old directory renamed
   aside, and the replacement renamed into place.
2. The actual installer-style non-atomic sequence recursively removed files,
   removed the directory, recreated it, and wrote version-four files.

No `EXDEV`, `EBUSY`, or `EROFS` occurred. Both directories reside on the
same SBUBIFS filesystem, so rename stayed within one filesystem.

### E4 — Removal: PASS

All synthetic `NewTestApplet` files were removed through the destination and
its directory removed. It disappeared directly from `sbdata`.
`TestApplet` remained visible and unchanged.

### E5 — Permissions, symlinks and enumeration: PASS

`FeatureTest` was created with mode `0750`; its Lua file was renamed and
set to `0640`. `current.lua -> renamed.lua` resolved and read
`lua-readable`. Directory enumeration returned the expected entries.

### E6 — Persistence and original-view restoration: PASS

A 65,536-byte zero-filled payload was written through the bind and synced:

```text
MD5 fcd6bcb56c1689fcef28b57c22475bad
inode 97 through source and destination
```

It did not exist at the production branch destination. After unmounting the
bind:

- `original.txt` immediately reappeared with `original-view`;
- `TestApplet` disappeared from the destination;
- the payload remained directly readable on `sbdata` with the same digest.

After rebinding, the payload digest still matched, `TestApplet` reappeared,
and `original.txt` was hidden again.

### E7 — Repeated access: PASS

Twenty bounded iterations read the Lua file, enumerated the destination and
updated a small counter through the bind. The direct source held final value
`19`. No filesystem error, taint, or suspicious memory trend appeared.

## Mount tables

During the test:

```text
ubi0:ubifs  /mnt/storage ubifs   rw,noatime
none        /            unionfs rw,dirs=/mnt/storage=rw:/=ro
ubi0:sbdata /tmp/sbdata-bind-validation sbubifs rw
ubi0:sbdata /usr/share/jive/sb-bind-validation-target-2026 sbubifs rw
```

After cleanup:

```text
/dev/root  /            cramfs  ro
ubi0:ubifs /mnt/storage ubifs   rw,noatime
none       /            unionfs rw,dirs=/mnt/storage=rw:/=ro
```

There was no remaining SBUBIFS module or `sbdata` mount.

## Resource observations

| State | MemFree | Slab |
|---|---:|---:|
| Initial baseline | 10,840 kB | 5,564 kB |
| Immediately before module load | 10,632 kB | 5,572 kB |
| After module load | 10,056 kB | 5,652 kB |
| After SBUBIFS mount | 9,492 kB | 5,676 kB |
| After bind mount | 9,436 kB | 5,740 kB |
| After operations/sync | 9,316 kB | 5,744 kB |
| After complete cleanup/unload | 10,252 kB | 6,056 kB |

The bind itself changed the sample by about 56 kB, within normal cache/slab
variation. SBUBIFS remained the dominant added cost. Memory recovered after
cleanup, although reclaimable slab/cache distribution differed from baseline.

Measured bind unmount and remount times were 0.04 and 0.05 seconds. The initial
SBUBIFS mount completed within the host-observed 0.57-second command interval.

Kernel taint remained zero. COM5 independently observed the clean SBUBIFS
mount/unmount. UnionFS emitted `new lower inode mtime` revalidation notices
when the isolated upper-branch destination was created and removed. These were
expected cache-revalidation diagnostics, not errors; there was no warning,
oops, UBI error, UBIFS error, bind failure, or I/O error.

## Complete-tree migration feasibility

The currently visible applet tree measured:

```text
6070 KiB
613 regular files
```

`sbdata` had approximately 59,700 KiB available during the test, leaving
ample capacity for the current tree and additional applets.

BusyBox 1.18.2 supports:

```text
cp -a    equivalent to -dpR
cp -p    preserve attributes where possible
cp -d    preserve symlinks
```

Its `tar` is minimal and exposes no explicit owner, permission, xattr or ACL
flags. A future migration should prefer a staged `cp -a`, followed by an
independent manifest containing relative path, type, size, mode, ownership and
checksum. Neither ACL nor xattr preservation should be assumed without
separate tool support and inspection.

Do not copy only `/mnt/storage/usr/share/jive/applets`: that omits firmware
applets supplied by CramFS. The source must be the complete visible
`/usr/share/jive/applets` view before the bind is activated. StandaloneBase
and SetupAppletInstaller must be mandatory sentinels.

## Boot ordering and running processes

`/etc/init.d/rcS` runs an executable `/etc/init.d/rcS.local` at lines
116–118 and starts SqueezePlay later at lines 134–135. A future bootstrap can
therefore load SBUBIFS, mount/validate `sbdata`, and activate the bind before
AppletManager discovery. This task did not create or edit `rcS.local`.

Activating the production bind after SqueezePlay starts is unsuitable: Jive can
retain open files, loaded Lua modules, discovered paths and cached directory
state. Production integration must establish the final namespace before
SqueezePlay starts.

## Failure and upgrade design

A production bootstrap must fail closed:

- module hash/kernel/volume mismatch: do not mount or bind;
- `sbdata` mount failure: retain the original applet view;
- incomplete copied tree or missing sentinels: do not bind;
- bind failure: leave the original view visible;
- full storage: abort staging, retain the last verified active tree;
- interrupted applet install: report a partial applet; the stock installer is
  non-atomic and offers no rollback;
- corruption/update marker: do not repair or format automatically;
- future incompatible firmware: skip activation until reconciliation succeeds.

Migration should build a staging tree on `sbdata`, verify a manifest, then
rename it atomically within SBUBIFS to the active tree. A persistent
`READY` manifest should include firmware/build identity and hashes of
firmware-derived applets.

On firmware change, boot without the bind first, construct a fresh staged tree
from the new complete visible view, and then merge independently installed
applets using an explicit ownership manifest. Firmware-owned names must take
the new firmware copy unless a reviewed override policy says otherwise.
StandaloneBase and recovery tooling must remain available in the fallback
production view.

## Cleanup verification

Cleanup was file-by-file and directory-by-directory after verifying mount
ownership. No broad recursive deletion was used.

Verified final state:

- bind mount absent;
- `sbdata` unmounted;
- `sbubifs` unloaded;
- module binary removed from `/tmp`;
- source validation tree absent;
- logical and physical UnionFS target absent;
- original root UnionFS mounted;
- production UBIFS mounted read-write;
- real StandaloneBase and SetupAppletInstaller present;
- legacy nested applet directory present and untouched;
- `sbdata` still ID5/name `sbdata`/521 LEBs, corruption 0, update marker 0;
- kernel taint 0;
- dirty and writeback memory 0;
- no reboot, service restart, or persistent startup change.

## Architecture comparison

| Architecture | Result | Assessment |
|---|---|---|
| Dedicated UnionFS | **FAIL** | Exact UnionFS rejects a UnionFS-backed branch |
| Complete applet tree + bind | **PASS mechanism** | Transparent, reversible, lowest kernel complexity; migration and boot transaction still required |
| Installer destination change | Fallback | Must also add an early discovery path; more Jive-specific code |
| Per-applet symlinks | Fallback | Fragile with recursive removal, dangling targets and partial storage failure |

## Recommended next step

Prepare a separate, explicitly reviewed production-integration design and test:

1. package the exact SBUBIFS module with a checksum allowlist;
2. implement an early fail-closed bootstrap without replacing normal boot;
3. stage and verify a complete `cp -a` applet snapshot plus manifest;
4. atomically select the verified active tree on `sbdata`;
5. bind it before SqueezePlay starts;
6. leave the original applet view untouched whenever validation fails;
7. test a real synthetic catalog install/update/remove lifecycle;
8. test interrupted migration/install recovery and firmware-version
   reconciliation before general deployment.

Do not proceed to production integration without separate authorization.

**Final verdict: PASS — bind mounting an SBUBIFS-backed complete applet tree
over a directory within the existing root UnionFS is physically supported,
transparent, persistent across bind remount, and cleanly reversible.**
