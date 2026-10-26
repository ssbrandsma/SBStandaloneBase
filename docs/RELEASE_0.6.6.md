# Standalone Base 0.6.6

Fix duplicate Standalone Radio entries in the local Applet Installer. The
`jiveapplets` response now filters catalog entries by the request's `target:`
model, instead of returning the entire multi-device catalog. Radio (`baby`)
and Touch (`fab4`) variants remain in the public catalog for their respective
devices. Response counts reflect the filtered list. Each request is independent.

Missing target defaults to `baby` on this Radio service. Untargeted and `*`
entries remain shared; unknown targets receive only shared entries. This fix
does not change firmware-version eligibility rules.

The regression test exercises baby, fab4, unknown and missing targets, shared
entries, JSON strings containing braces and quotes, and alternating requests
to verify filtering never mutates the loaded catalog.

Bootstrap remains Base-only; public XML preserves the Radio entries. As usual,
the ZIP embeds the preceding Base catalog entry to avoid a self-checksum cycle.

- ZIP: `StandaloneBase-0.6.6.zip`, 44 flat entries, 2,612,744 bytes.
- SHA-1: `b95ad0bca6c5dde52bc43f583b8a84fa90196892`.
- SHA-256: `a2564c97933983da542d4265acb59e615ce40014bbf96da8f086ce60bde3e4bd`.
- Minimum firmware: 7.7.2.

The ARMv5 build, ARM926 QEMU self-test/version check and the actual installer
target-filtering regression against the ARM926 binary passed. No physical
Radio is restarted as part of this remote-server release.

All 56 host regression tests passed (153.50 seconds), including the new
Comet installer target-filtering test.
