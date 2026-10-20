# Standalone Base 0.5.6

## Storage logging

- Mirror detailed storage analysis to the existing syslog daemon under tag
  `standalonebase-storage`, visible in `/var/log/messages` and the web System
  Log viewer. Keep the existing RAM-only check/operation reports as well.
- Log the first check and status changes, including healthy/active status.
  Deduplicate unchanged polling and exclude capacity fluctuations from the
  deduplication key. Explicit `analyze` always emits a fresh complete report.
- Log initialization, explicitly confirmed erasure, applet copying,
  verification counts, boot support installation, recovery space calculations,
  promotion, completion and failure codes. Mirror invoked commands' stdout and
  stderr while preserving their actual exit codes, even if syslog fails.
- Add boot-activation progress messages for driver loading, mounting and applet
  bind activation. No new daemon or persistent logging database is introduced.
- Preserve all existing flash safety checks, physical confirmation and
  non-destructive recovery behavior. Logging does not trigger initialization.

## Collecting logs

Use the authenticated System Log viewer to reproduce and export the issue, or:

```sh
sh /usr/share/jive/applets/StandaloneBase/storage-setup.sh analyze
grep 'standalonebase-storage' /var/log/messages
```

Export promptly because normal syslog rotation still applies. The detailed
reports remain at `/tmp/sbstorage-check.log`, `/tmp/sbstorage.log` and
`/tmp/sbstorage.status`. See [Extended Storage](EXTENDED_STORAGE.md).

## Release artifact

- `StandaloneBase-0.5.6.zip`: 37 flat entries, 2,513,389 bytes.
- SHA-1: `6ecaf351d9460bd4b8623351b524bf24b66ce03c`.
- SHA-256: `e7db2ef6fbbddd53abe56d0cf6469d1fe1955302d0602652e3c6ea52aab3897d`.
- Minimum advertised firmware remains 7.7.2.
- The bootstrap catalog contains only Standalone Base. The GitHub catalog
  retains Standalone Radio, Spotify Connect and Qobuz Connect.

The packaged config contains runtime version 0.5.6 and the preceding published
self-entry (0.5.5), since a ZIP cannot embed its own final checksum. The final
repository and bootstrap entries advertise 0.5.6 with the checksum above; the
fixed catalog refresh merges GitHub's published entry after startup.

## Validation

- Full static ARMv5 build of all six executables.
- Extended-storage fixtures covering syslog duplication, stdout/stderr capture,
  forced analysis, quiet repeated polling, operation/error logging, logger
  failure and preservation of storage-command exit codes.
- Eleven firmware recovery fixtures, including preservation of original data
  and logging of insufficient-space details and copied applets.
- Lua menu and storage-operation manager regressions.
- Catalog-sync suite and ARM/QEMU Mongoose clock-fallback test.
- ARM/QEMU storage-helper, version and raster-rendering tests; ZIP validation.

No physical Radio is erased or restarted by release validation. Live device
syslog delivery depends on the Radio's existing logger/syslog configuration.
