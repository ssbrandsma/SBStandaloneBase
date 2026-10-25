# Standalone Base 0.6.2

## Changes

- Add physically authorized custom firmware preparation and native updater integration.
- Support paced browser firmware uploads, SHA-1 and SHA-256 validation, and local staging.
- Retain startup integration under `/etc/standalonebase` across compatible firmware updates.
- Add non-destructive repair for healthy existing Extended Storage, including native multi-UBIFS firmware.
- Preserve station configuration and existing applets during recovery.
- Include firmware regression tests, recovery procedures and Jive diagnostic documentation/tools.

## Validation and limitations

- ZIP: `StandaloneBase-0.6.2.zip`, 42 flat entries, 2,585,986 bytes.
- SHA-1: `8b55e02be102c63c76980ea9b863d3870d6faf38`.
- SHA-256: `9fc110f46dc365f8bf5817f26bd6741f1ad6008e9e16d71970692f1d58ee9053`.
- Minimum Radio firmware: 7.7.2.

Validation passed: static ARMv5 build, ARM926 QEMU self-test and version checks,
all 49 host regression tests, package validation and browser firmware UI tests.
Physical recovery and NPO Radio 2 playback were confirmed on firmware 9.0.2 r17157.
An unexpected watchdog reset occurred during post-recovery diagnostics; its cause
remains unresolved. This release does not claim full watchdog-stability acceptance
or universal recovery across future firmware versions. See
[the recovery report](POST_FIRMWARE_RECOVERY.md).

The remote bootstrap catalog advertises Standalone Base only. The public GitHub
catalog retains Base, Radio, Spotify Connect and Qobuz Connect. The packaged
catalog may reference the previous Base release to avoid a self-checksum cycle.
