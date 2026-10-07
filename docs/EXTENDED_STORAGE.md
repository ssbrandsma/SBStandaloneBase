# Extended Storage integration

## Scope and safety boundary

StandaloneBase integrates the already validated secondary filesystem driver and
applet-tree bind architecture. It does not change the kernel, bootloader,
`/linuxrc`, production `ubifs`, or any kernel/CramFS volume. The stock writable
filesystem remains mounted at `/mnt/storage`.

The authoritative driver is `artifacts/sbubifs-authorized.ko`:

- size: `199119` bytes
- SHA-256: `63652ce67df06a78abb84a4986253bdab02fbd7b7c000779c60b3d393ba9566b`
- filesystem name: `sbubifs`
- allowlist: UBI device 0, volume ID 5, exact name `sbdata`

The module must not be rebuilt or substituted. Packaging fails if either its
size or digest differs. `sb-storage-helper verify-module FILE` provides the
same runtime identity check without depending on a Radio-side `sha256sum`.

The module, mount, read/write behavior, and bind-mount mechanism were physically
validated separately. See `SBUBIFS_PHYSICAL_VALIDATION.md` and
`APPLET_STORAGE_BIND_MOUNT_VALIDATION.md`. The integration described here is
host-tested only.

## Runtime architecture

At boot, `/etc/init.d/rcS.local` invokes:

```sh
/bin/sh /mnt/storage/standalonebase/storage-boot.sh || true
```

The boot helper verifies the module and exact sysfs identity at
`/sys/class/ubi/ubi0_5`, loads the module, verifies `sbubifs` in
`/proc/filesystems`, mounts `ubi0:sbdata` on `/mnt/sbdata`, validates the
migrated tree, and bind mounts `/mnt/sbdata/applets` on
`/usr/share/jive/applets` before SqueezePlay starts.

Every failure is fail-open. The script logs and exits successfully so normal
boot continues with the stock applet tree. It removes only a bind, mount, or
module that it created during that invocation. It never formats or repairs
storage at boot.

## Shell interface

Read-only inspection:

```sh
/bin/sh /usr/share/jive/applets/StandaloneBase/storage-setup.sh check
```

The command does not create files, directories, mounts, modules, or volumes.
It emits `KEY=value` records including:

```text
STATUS=AVAILABLE
VOLUME_PRESENT=1
VOLUME_ID=5
VOLUME_NAME=sbdata
DRIVER_LOADED=0
SBDATA_MOUNTED=0
APPLET_BIND_ACTIVE=0
TOTAL_KB=0
USED_KB=0
FREE_KB=0
INITIALIZATION_SUPPORTED=1
INITIALIZATION_REASON=validated_packaged_image
```

Status is one of `UNSUPPORTED`, `UNAVAILABLE`, `AVAILABLE`, `ACTIVE`, `ERROR`,
or `REBOOT_REQUIRED`. Capacity is read from `df` only while `sbdata` is mounted;
it is never hardcoded. A factory reset that leaves `ubi0_5` but removes the
module and boot hook is reported as `AVAILABLE`, not as an unexplained error.

The initialization interface is:

```sh
/bin/sh /usr/share/jive/applets/StandaloneBase/storage-setup.sh \
  initialize \
  /usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko
```

It reports stages through `/tmp/sbstorage.status` and diagnostics through
`/tmp/sbstorage.log`. Stage values are `CHECKING_SYSTEM`, `PREPARING_STORAGE`,
`CREATING_VOLUME` when needed, `INITIALIZING_FILESYSTEM`, `LOADING_DRIVER`, `MOUNTING`, `COPYING_APPLETS`,
`VERIFYING_APPLETS`, `INSTALLING_BOOT_SUPPORT`, `FINISHING`, and `COMPLETE`.

## Validated explicit initialization

The complete initialization preflight validates the exact UBI identity,
geometry, health, device node, unmounted state, module, legacy-generated image,
image size and image SHA-256. After the exact packaged image passed physical
write, mount, read/write and persistence validation, the explicitly confirmed
`initialize` operation was enabled to run `ubiupdatevol` on `/dev/ubi0_5`.

No other operation calls `ubiupdatevol`: checks, mounting, boot, installation,
upgrades, startup, UI navigation and mount failures remain non-destructive.
Host tests replace `ubiupdatevol` with a mock and never write UBI or NAND.

## Virgin 7.7.3 bootstrap

The physically measured factory-reset baseline is firmware `7.7.3 r16676`,
kernel `2.6.26.8-rt16`, UBI0 with 1,014 total LEBs, 660 available LEBs,
129,024-byte LEBs, 2,048-byte minimum I/O, zero bad PEBs and ten PEBs reserved
for bad blocks. It contains exactly these five healthy dynamic volumes:

| ID | Name | Reserved LEBs | Data bytes |
|---:|---|---:|---:|
| 0 | `kernel_bak` | 23 | 2,967,552 |
| 1 | `cramfs_bak` | 106 | 13,676,544 |
| 2 | `ubifs` | 82 | 10,579,968 |
| 3 | `kernel` | 23 | 2,967,552 |
| 4 | `cramfs` | 106 | 13,676,544 |

Only an exact match, with no volume or device node for ID 5 and no additional
user volumes, enables creation. The explicitly confirmed initializer runs:

```sh
/usr/sbin/ubimkvol /dev/ubi0 -n 5 -N sbdata -S 521 -t dynamic
```

It never uses maximum-available sizing. It then requires ID 5 to be the exact
healthy, dynamic, alignment-1 `sbdata` volume with 521 reserved LEBs,
67,221,504 data bytes and a 129,024-byte usable LEB, plus exactly 139 remaining
free LEBs. Only then does execution converge into the physically validated
0.2.5 image-write, mount, migration and boot-support transaction.

The factory layout measurements and the existing-sbdata transaction are
physically validated. The new virgin creation/bootstrap branch is validated
with mocks only until a separately approved physical test is performed.
Creation failure aborts immediately. A post-creation mismatch is left in place
for manual inspection—there is no automatic `ubirmvol`, resize, rename, repair
or rollback. A later explicit attempt can reuse it only if it exactly matches
the existing-sbdata profile. SqueezePlay remains running because stopping it
triggers the Radio watchdog; initialization never stops it or bypasses the
watchdog.

## Transaction after filesystem initialization

The implemented post-format transaction:

1. loads the exact driver and verifies filesystem registration;
2. mounts `ubi0:sbdata` as `sbubifs`;
3. copies `/usr/share/jive/applets/.` with `cp -a`;
4. compares regular-file and directory counts without iterating over
   whitespace-delimited filenames;
5. checks a core applet and StandaloneBase when present;
6. verifies the destination mount is `sbubifs`;
7. installs the module and boot helper under `/mnt/storage`;
8. installs `/etc/init.d/rcS.local` through the merged UnionFS path, last;
9. writes a reboot-required marker and syncs;
10. never stops SqueezePlay and never reboots automatically.

The hook preserves an existing `rcS.local` and appends one marked invocation.

Exit codes are: 0 success, 10 already active, 20 unsupported layout, 21
`sbdata` unavailable, 22 driver failure, 23 filesystem initialization failure,
24 mount failure, 25 migration failure, 26 validation failure, and 27 boot
installation failure. Virgin-bootstrap-specific codes are 28 unsupported
virgin fingerprint, 29 insufficient verified UBI capacity, 30 missing
`ubimkvol`, 31 volume creation failure and 32 post-creation validation failure.

## Jive UI

The menu path is:

```text
Standalone Base
  Extended Storage
```

The status screen shows `Not available`, `Available`, `Active`, `Error`, or
`Reboot required`. Active state includes measured total, used, and free space.
The complete confirmation, progress, error mapping, and `Restart Now`/`Later`
flow is implemented. Long work uses `jive.net.Process`; a 500 ms Jive timer
polls `/tmp/sbstorage.status`, so the UI event loop remains responsive. Restart
uses the platform `reboot` service rather than stopping SqueezePlay.

## Host tests

`tests/storage/test_extended_storage.sh` covers the full exact virgin
fingerprint, every creation gate, mocked creation and post-creation failures,
the complete successful mocked bootstrap, the existing exact-sbdata path,
module/mount/migration/validation/boot-install failures, reboot-required state,
boot idempotence, and fail-open boot behavior. `test_extended_storage_state.lua`
covers UI state, stage, capacity, and error mappings.

These are host/fixture tests. They do not claim physical validation of the new
integration.

## Future physical validation

The virgin bootstrap must not be described as physically validated yet. Its
first physical execution requires separate approval, serial recovery access
and a fresh verification of the complete fingerprint. Before approval:

1. Install the package, but do not initialize.
2. Open `Standalone Base -> Extended Storage` and record the status.
3. Over SSH run:

   ```sh
   cd /usr/share/jive/applets/StandaloneBase
   /bin/sh ./storage-setup.sh check
   ./sb-storage-helper verify-module ./sbubifs-authorized.ko
   ./sb-storage-helper verify-sbdata-image ./sbdata-empty.ubifs
   cat /sys/class/ubi/ubi0/{total_eraseblocks,avail_eraseblocks,eraseblock_size,min_io_size,bad_peb_count,reserved_for_bad}
   for v in /sys/class/ubi/ubi0_[0-4]; do cat "$v/name" "$v/type" "$v/reserved_ebs" "$v/data_bytes" "$v/corrupted" "$v/upd_marker"; done
   cat /proc/filesystems
   cat /proc/mounts
   ```

4. Confirm shell and UI status agree. Stop here.

After separate approval, use this second stage:

1. Start initialization from the UI and observe every displayed stage.
2. In SSH inspect `cat /tmp/sbstorage.status` and
   `cat /tmp/sbstorage.log`.
3. Verify `grep sbubifs /proc/filesystems`.
4. Verify `grep ' /mnt/sbdata sbubifs ' /proc/mounts`.
5. Compare safe counts:

   ```sh
   find /usr/share/jive/applets -type f -print | wc -l
   find /mnt/sbdata/applets -type f -print | wc -l
   find /usr/share/jive/applets -type d -print | wc -l
   find /mnt/sbdata/applets -type d -print | wc -l
   ```

6. Inspect `/etc/init.d/rcS.local`, the persistent module, and its identity.
7. Do not stop SqueezePlay. Reboot normally through `Restart Now`.
8. After reboot verify:

   ```sh
   grep sbubifs /proc/filesystems
   grep '^sbubifs ' /proc/modules
   grep ' /mnt/sbdata sbubifs ' /proc/mounts
   grep ' /usr/share/jive/applets sbubifs ' /proc/mounts
   /bin/sh /usr/share/jive/applets/StandaloneBase/storage-setup.sh check
   ```

9. Verify StandaloneBase, StandaloneRadio when installed, the Applet Installer,
   and other installed applets. Only with explicit approval proceed to repeated
   initialization, injected failures, or factory-reset testing.
