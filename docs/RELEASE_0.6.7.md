# Standalone Base 0.6.7

Fix HTTPS streaming from legacy RSA/AES-GCM broadcasters, including the
Teveo Radio Enciclopedia endpoint. The rebuilt wolfSSL library enables static
RSA support; sbproxy's explicit cipher list permits only the two added RSA
AES-GCM suites, retaining the previous ephemeral suites and TLS 1.3 support.
Static RSA lacks forward secrecy: this is a deliberate compatibility tradeoff.
Certificate verification policy, protocol versions, destination guards and
HTTP behavior are unchanged. No HTTP downgrade or new daemon is introduced.

See [diagnosis and validation](SBPROXY_LEGACY_RSA.md). The 0.6.6 device-target
filtering fix is retained. Bootstrap remains Base-only; shared XML preserves
Radio entries. The ZIP embeds the preceding Base catalog metadata to avoid
a self-checksum cycle. This release does not restart a physical Radio.

- ZIP: `StandaloneBase-0.6.7.zip`, 44 flat entries, 2,613,177 bytes.
- SHA-1: `fd40ca82416c9d0a0dcb7eeea7bd8be564a2736f`.
- SHA-256: `9fa6c32b1e644ffcd13b924502332f74c8228caaada40b37a5e5bb42ebd57963`.
- Minimum firmware: 7.7.2.

The static ARMv5 build, ARM926 self-test/version checks, controlled TLS tests
and live Teveo request through the rebuilt ARM926 proxy passed. The live proxy
returned HTTP 200, `audio/mpeg`, and a bounded 32,768-byte stream sample.
All 56 host regression tests passed (193.56 seconds).
