# Standalone Base 0.6.1

Custom station management now supports OPML directories alongside radio
stations. The shared editor has a type selector, type-aware URL labels and
codec controls. The API validates directory URLs, persists entry types and
supports mixed station/directory ordering, editing and JSON/CSV import/export.
Existing entries without a type remain ordinary stations.

See [station management](RADIO_STATIONS_MANAGEMENT.md) for details.

## Artifact

- `StandaloneBase-0.6.1.zip`: 40 flat entries, 2,540,601 bytes
- SHA-1: `157b4d27105dcf059cd15a43c3dbec601ac2e8c4`
- SHA-256: `7f8d6cfa172402345031ae1d5073f4f3a70a91c9cbddab9e919de38e6828d988`
- Minimum Radio firmware: 7.7.2

The bootstrap catalog remains Base-only. The combined public extensions
manifest preserves the existing Radio entries. The root GitHub catalog
advertises the final release checksum; the embedded catalog retains the
preceding Base artifact to avoid a circular checksum.

Validation includes the host suite, OPML form behavior and API regression
tests, static ARMv5 build and ARM926 QEMU self-test/version checks.
