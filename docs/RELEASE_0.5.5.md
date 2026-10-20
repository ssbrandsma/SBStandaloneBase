# Standalone Base 0.5.5

## Changes

- Fall back from unsupported `CLOCK_MONOTONIC_RAW` to `CLOCK_MONOTONIC` at
  runtime. Older Radio kernels now advance Mongoose timers and refresh the
  GitHub applet catalog normally.
- Add **Erase and reinitialize Extended Storage** for validated, inactive
  `sbdata` volumes, including after recovery stops for insufficient space.
  Explicit physical confirmation is required. Cancel is the first choice;
  Virtual Radio injected input is rejected. Recovery never automatically erases.
- Retain exact volume/image/driver checks, shared operation locking and mounted
  volume protection. Successful reinitialization copies current applets and
  restores boot support, then requests a manual restart.
- Keep firmware 7.7.2 as the advertised minimum. The bootstrap catalog offers
  only Standalone Base; the GitHub catalog retains Radio, Spotify and Qobuz.
- Deployment tooling now requires an explicit release version and checksum,
  avoiding accidental publication using stale defaults.
- Add automatic RAM-only layout diagnostics at `/tmp/sbstorage-check.log` and
  an explicit non-destructive `storage-setup.sh analyze` command with expected
  and actual values for failed checks. Initialization/recovery logs are retained
  separately. See [Extended Storage diagnostics](EXTENDED_STORAGE.md).

## Published artifact

- File: `StandaloneBase-0.5.5.zip` (37 flat entries, 2,512,627 bytes).
- SHA-1: `ab1e8cef14d61f156fc642f587de43e4312b9d97`.
- SHA-256: `cc7db5dfa9da6d88d89620496070417861f2d1e5435f986c20e43918f9895a7b`.

This is a same-version republish adding storage diagnostics. All referencing
catalogs/manifests must use the checksum above. The original `0.5.5` Git tag
remains unchanged; the diagnostics revision is in the subsequent master commit.
An installed original 0.5.5 must be reinstalled explicitly to receive these
scripts: the stock installer compares version strings, not archive checksums.

The ZIP contains the preceding published self-entry (0.5.4) and the new runtime
version (0.5.5). A ZIP cannot embed its own final checksum. The repaired catalog
refresh merges the published GitHub self-entry after startup. The repository
catalog and bootstrap catalog advertise 0.5.5 with the final ZIP checksum.

Build using `scripts/build-arm.sh`, followed by `packaging/build-package.ps1`
with the public release base URL. See the README for prerequisites. Rebuilding
after the catalog update changes the archive contents/checksum; do not replace
an already published ZIP without updating all referencing manifests.

## Validation

- Full static ARMv5 build of all six executables.
- Storage helper and extended-storage fixture tests, including a complete
  mocked reinitialization and refusal of missing/wrong/mounted/pending volumes.
- Eleven non-destructive firmware recovery tests.
- Lua 5.1 menu, confirmation, input-rejection and operation-manager tests.
- Catalog-sync regression tests and actual ARM/QEMU clock-fallback test.
- ARM/QEMU storage-helper, version and raster-rendering checks.
- ZIP layout validation. No physical Radio erase is performed by these tests.
- Diagnostics revision: storage/recovery and Lua menu regressions rerun; report
  tests cover multiple failed comparisons, report bounds/permissions, forced
  refresh, preservation of operation logs and absence of destructive calls.
  The six native executables are byte-identical to the original 0.5.5 build.

See [firmware recovery](EXTENDED_STORAGE_FIRMWARE_RECOVERY.md) and
[Mongoose local changes](../native/sbwebserver/third_party/LOCAL_CHANGES.md)
for behavior, safety boundaries and vendor update notes.
