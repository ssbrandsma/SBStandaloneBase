# Standalone Base 0.6.5

Includes local alarm support, the Settings sleep timer, paired-browser
configuration backup and restore, and local-player selection recovery.
See ALARMS.md, SLEEP_TIMER.md, BACKUP_RESTORE.md and
LOCAL_PLAYER_SELECTION_RECOVERY.md for behavior and limitations.

Configuration backups exclude binaries, firmware, credentials and caches.
Restored alarms are disabled. Restore restarts SqueezePlay.

The remote bootstrap catalog remains Base-only. The public repository catalog
retains the current Radio, Spotify and Qobuz entries. The ZIP embeds the
preceding Base release metadata to avoid a self-checksum cycle.

Standalone Radio fixes are distributed separately in its own release ZIP;
this Base package does not embed or replace the Radio applet.

- ZIP: `StandaloneBase-0.6.5.zip`, 44 flat entries, 2,612,406 bytes.
- SHA-1: `e919b9bc4299bf527df5536be99120dd4fcd9dc0`.
- SHA-256: `eb6bbb7ae53c5533b1592a647a7d353b9724b2425c0c4e5b82c8be19b3de769d`.
- Minimum firmware: 7.7.2.

Validation: static ARMv5 build and ARM926 QEMU self-test/version checks passed.
All 55 host regression tests passed (155.72 seconds). The Edge browser
backup/restore UI checks passed. Physical alarm, sleep/standby and restore
validation remains pending; this release workflow does not restart a Radio.
