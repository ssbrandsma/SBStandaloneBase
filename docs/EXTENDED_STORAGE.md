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
`INITIALIZING_FILESYSTEM`, `LOADING_DRIVER`, `MOUNTING`, `COPYING_APPLETS`,
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
installation failure.

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

`tests/storage/test_extended_storage.sh` covers no UBI, missing `sbdata`, valid
inactive and active storage, wrong identity, missing/corrupt module, the real
exit-23 safety boundary, module/mount/migration/validation/boot-install
failures, reboot-required state, factory-reset-like state, boot idempotence,
and fail-open boot behavior. `test_extended_storage_state.lua` covers UI state,
stage, capacity, and error mappings.

These are host/fixture tests. They do not claim physical validation of the new
integration.

## Staged physical validation procedure

The UI initialization action is safe while the gate remains present: it runs
all current preflight checks and stops with status 23 before NAND writing. Do
not remove that gate until the separate physical procedure succeeds.

The currently safe first stage is:

1. Install the package, but do not initialize.
2. Open `Standalone Base -> Extended Storage` and record the status.
3. Over SSH run:

   ```sh
   cd /usr/share/jive/applets/StandaloneBase
   /bin/sh ./storage-setup.sh check
   ./sb-storage-helper verify-module ./sbubifs-authorized.ko
   wc -c ./sbubifs-authorized.ko
   cat /sys/class/ubi/ubi0_5/name
   cat /sys/class/ubi/ubi0_5/type
   cat /sys/class/ubi/ubi0_5/corrupted
   cat /sys/class/ubi/ubi0_5/upd_marker
   cat /proc/filesystems
   cat /proc/mounts
   ```

4. Confirm shell and UI status agree. Stop here.

After separate approval and implementation of the clean initializer, use this
second stage:

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
