# Extended Storage after a firmware upgrade

Firmware installation can remove Standalone Base and its boot files while
leaving UBI volume 5 (`sbdata`) intact. Reinstall Standalone Base on the normal
storage partition. Its first applet startup performs a read-only storage check
and schedules a separate asynchronous recovery process if an existing inactive
volume is eligible. It does not depend on an old startup hook surviving.

`storage-setup.sh check` remains read-only. The explicit `recover` operation is
also used by the startup integration. Neither normal status requests nor browser
polls perform recovery. The existing `initialize` operation is not recovery:
it can overwrite the filesystem and must not be used for preserving old data.
The Radio menu offers recovery guidance instead of initialization for an
eligible existing volume.

## Recovery transaction

1. Validate the Baby platform, supported kernel release, exact existing volume
   geometry and clean update/corruption flags, original storage mount, and
   bundled driver's pinned identity. Free unallocated LEBs are not required.
2. Acquire a single-flight lock and revalidate. Refuse active/foreign mounts or
   an unsupported startup-script layout. Load the driver normally: failure is
   a compatibility blocker, never bypassed with forced module loading.
3. Mount only `sbdata` read-only. Check the original applet sentinels, available
   space, and custom tree safety before remounting it read-write.
4. Build a new tree beside the original. Known stock 7.7.3/9.0 applet names are
   excluded from the old tree; core applets come from the running firmware.
   Other applet directories and caches are preserved. Current reinstalled
   applets overwrite their old files. All copied current regular files are
   compared against their sources before promotion.
5. Flush staging, retain the entire original tree, and promote the replacement.
   Restore the authorized module and boot script with temporary files and
   renames. Preserve `rcS.local` custom commands. If `rcS` no longer invokes
   it, insert a call before the single recognized SqueezePlay start command,
   preserving the original `rcS` in the backup. Unknown boot layouts stop for
   review instead of replacing firmware scripts wholesale.
6. Set the existing reboot-required marker, flush, and unmount. Do not bind
   over a running Jive or reboot automatically. Open Standalone Base → Extended
   Storage and restart when it reports ready.

The routine never creates, formats, resizes, or deletes a UBI volume and never
invokes `ubimkvol` or `ubiupdatevol`. Other `sbdata` directories and production
settings are left in place. Temporary state/logs use `/tmp/sbstorage.status`
and `/tmp/sbstorage.log`. Successful backups remain at
`/mnt/sbdata/firmware-backup-*/previous-applets` after activation; they are not
automatically deleted. Retaining backups costs space deliberately.

## Failure and compatibility boundaries

- Unsupported volume geometry, damaged flags, incompatible modules, low space,
  custom symlinks, and unknown startup layouts require manual inspection.
- There is no assumption that an identical kernel release implies ABI
  compatibility: the module must load and the filesystem must mount normally.
- There is no destructive fallback if recovery fails.
- A power interruption can leave `.firmware-recovery.*` staging/journal
  directories. Automatic recovery refuses these, retaining both data and any
  prior tree for review. In particular the two directory renames are not a
  power-loss-atomic exchange. Do not initialize the volume to clear this state.
- The firmware applet name list covers the known stock trees. Future firmware
  adding/removing stock names should extend that list; recovery is not a
  general firmware-independent package manager.
- If reboot fails to activate storage, retain the backup and inspect the boot
  log and mount layout. Do not force a filesystem driver or remount old stock
  applets over a newer firmware.

## Validation

`tests/storage/test_firmware_recovery.py` uses fake mounts and module tools. It
checks preservation, stock refresh, reboot gating, repeat no-op behavior,
read-only checks, custom hooks, incompatible modules, corrupt flags, low space,
copy failure, unknown boot layouts, and unsafe custom symlinks. Destructive
tool stubs fail if called. Existing storage tests remain separate.

The recovery transaction was physically exercised on a Radio upgraded from
7.7.3 to 9.0.2 r17146: the driver loaded, the existing volume mounted, the new
tree verified, and boot support installation completed with exit code zero.
Final activation after reboot and power-loss behavior have not been verified.
Copy verification uses the native helper's bounded `verify-copy` operation;
the target firmware does not provide a `cmp` executable. Release 0.5.2 also
prioritizes live Active/Reboot Required status over a cached UI failure flag.
