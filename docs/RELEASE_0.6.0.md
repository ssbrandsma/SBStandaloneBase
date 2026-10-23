# Standalone Base 0.6.0

## Changes

- Fix NPO Radio 2 and other RSA-4096 HTTPS endpoints by rebuilding the
  existing wolfSSL backend with sufficient fixed integer capacity.
- Add opt-in, sanitized TLS diagnostics and deterministic RSA capacity tests.
- Include the recent bounded artwork fallback, progressive JPEG decoding and
  artwork URL normalization work.
- Package `ArtworkUrlNormalizer.lua` consistently in both package builders.

See [TLS investigation and build instructions](SBPROXY_TLS_DIAGNOSTICS.md).
Existing TLS certificate verification policy is unchanged.

## Release artifact

- File: `StandaloneBase-0.6.0.zip` (40 flat entries, 2,539,998 bytes)
- SHA-1: `49b310ff9ad7d91340a042c5a503697552f4b933`
- SHA-256: `f6fb361ff6f7bfba49b1c5a605adb4b5a86aeb6d6ccfe71b1f95e95a8ece5305`
- Minimum Radio firmware: 7.7.2

The public root `config.json` advertises this artifact alongside the other
applets. The bootstrap server catalog remains Standalone Base only. Both the
Base-specific and combined public extensions manifests advertise 0.6.0.

As explained in [deployment](deployment.md), the packaged catalog retains the
previous Base artifact entry to avoid a circular checksum. The public catalog
supplies the final release checksum through normal catalog synchronization.

## Validation

Static ARMv5 build and ARM926 QEMU sbbase self-test/version checks passed.
The host test suite passed, including serial reruns of tests that share ports
and tests requiring LF shell-script line endings. RSA-2048/4096 TLS 1.2/1.3
and diagnostic redaction tests passed with the rebuilt backend.

Deployment to a Radio restarts SqueezePlay and native services, invalidating
in-memory browser sessions. Pair the browser again if required.
