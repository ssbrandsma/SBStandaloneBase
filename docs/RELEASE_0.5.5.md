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

## Published artifact

- File: `StandaloneBase-0.5.5.zip` (37 flat entries, 2,510,531 bytes).
- SHA-1: `d818619e6654e9e15991baf905ba0cba17f0c2b3`.
- SHA-256: `fbd318a2ddd7682c08591103e8677b74b6dfa93c1c9d36831b9281d78e15720c`.

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

See [firmware recovery](EXTENDED_STORAGE_FIRMWARE_RECOVERY.md) and
[Mongoose local changes](../native/sbwebserver/third_party/LOCAL_CHANGES.md)
for behavior, safety boundaries and vendor update notes.
