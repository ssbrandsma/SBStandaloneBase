# Standalone Base 0.6.3

Supplies numeric repeat/shuffle fields for stopped and inactive player status,
preventing stock NowPlaying nil-mode errors. The companion Radio 0.9.15 release
fixes premature LMS handoff on background reconnect and isolates stale LMS gain
and decoder commands. See [status fix](NOWPLAYING_STOPPED_STATUS_FIX.md).

- ZIP: `StandaloneBase-0.6.3.zip`, 42 entries, 2,586,047 bytes.
- SHA-1: `945e8b457201901c0d50ad26f4c5bfd49807c97c`.
- SHA-256: `e1ebde311bc5ba9332acb8c16d5c0f87eaa24aa7c86cadb474e6a4b74f70847b`.
- Minimum firmware: 7.7.2.

The embedded catalog contains Radio 0.9.15 and the preceding Base release to
avoid a self-checksum cycle. The updated root catalog advertises Base 0.6.3.
The bootstrap container remains Base-only. Existing storage recovery and
watchdog-stability qualifications in the recovery report still apply.
