# Standalone Base 0.5.8

## Changes

- Include the managed AppletManager instance adapter for install, update and
  removal on Extended Storage. Stream ZIP extraction safely, including nested
  directories, directory end-of-stream and legacy backslash separators.
- Enable bounded, redacted HTTP transaction tracing in shared Jive networking
  and native proxy, HLS, artwork and GitHub catalog paths. Trace IDs correlate
  local requests without logging bodies, authentication headers or query values.
- Keep the clock fallback, storage syslog diagnostics and physically confirmed
  erase/reinitialization introduced by previous releases.
- Package `InstallerCompat.lua` and `HttpTrace.lua`; validate presence of the
  installer compatibility module when deploying 0.5.7 or newer.

See [installer compatibility](APPLET_INSTALLER_COMPAT.md),
[HTTP tracing](HTTP_TRACING.md) and [storage diagnostics](EXTENDED_STORAGE.md).

## Distribution

- ZIP: `StandaloneBase-0.5.8.zip`, 39 flat entries, 2,523,294 bytes.
- SHA-1: `f2ea54078b3d3352c6e4c0a33f4db56f4af02446`.
- SHA-256: `6f495e2394c40f9ff75fccf7769e94c89c50141706a23c9fe45af840f15dcea2`.
- Minimum advertised firmware remains 7.7.2.
- Base-only manifest: `/sbstandalonebase/extensions.xml`.
- Combined manifest: `http://www.bytestack.nl/sbstandalone/extensions.xml`;
  contains Base 0.5.8 for Radio and Radio 0.9.10 for Radio and Touch.
- Bootstrap and repository catalogs update the Base entry while preserving
  other existing entries and server configuration.

The package contains runtime version 0.5.8 and the preceding published Base
self-entry (0.5.7); a ZIP cannot contain its own final archive checksum. The
published GitHub and bootstrap catalogs contain the final 0.5.8 checksum and
are merged through the existing catalog refresh. Build and package using
`scripts/build-arm.sh` and `packaging/build-package.ps1`.

## Validation

- Full static ARMv5 build and distribution layout/version/module validation.
- Installer adapter, managed-instance lifecycle and safe ZIP extraction tests.
- Lua and native HTTP-trace tests, including token/query redaction, redirects,
  streaming, cancellation, sink errors and duplicate callbacks.
- Storage helper, extended-storage syslog, recovery and menu tests.
- Catalog-sync regression suite and ARM/QEMU clock fallback test.
- Proxy/HLS and renderer regressions; synthetic framing tests do not replace
  physical audio validation. No Radio is deployed, restarted or erased by this
  remote publication workflow.
