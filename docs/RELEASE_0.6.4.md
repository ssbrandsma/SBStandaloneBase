# Standalone Base 0.6.4

Enable the existing bounded LMS Community image compatibility fallback by
default when no setting is present. Local artwork processing and cache remain
first. Existing `artwork_external_fallback_enabled=0` settings remain disabled.
This is a native Base webserver change, not a Radio applet change.

Eligible failures may share public artwork URLs with the LMS Community service.
Existing URL/privacy restrictions, download limits, timeouts, decoder limits
and failure backoff remain unchanged. See [artwork documentation](RASTER_ARTWORK.md).

- ZIP: `StandaloneBase-0.6.4.zip`, 42 flat entries, 2,586,069 bytes.
- SHA-1: `ba68a3f098a982567c0ee75065a3c1471faa3a8b`.
- SHA-256: `80c294013b16384b603823fbd7ce1f1f31d106b858ebd4e1b249d15792c01f9c`.
- Minimum firmware: 7.7.2.

The remote bootstrap catalog remains Base-only. The public catalog retains
Radio 0.9.15 and the existing Spotify/Qobuz versions. The embedded Base catalog
entry references the previous release to avoid a self-checksum cycle.

No physical Radio deployment is part of this release request. Existing firmware
recovery and watchdog-stability limitations remain documented separately.

Validation includes the static ARMv5 build, ARM926 QEMU self-test/version and
settings tests, plus mock-origin artwork fallback tests covering bounds, privacy,
cache, error backoff, concurrency and cancellation. Default-setting tests also
preserve timeout/size clamping and explicit saved opt-outs.
All 50 configured host regression tests passed (152.83 seconds).
