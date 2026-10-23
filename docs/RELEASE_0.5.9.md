# Standalone Base 0.5.9

## Changes

- Add optional source/transport audio quality and separate station metadata to
  the existing generic Now Playing owner API. Unknown values remain omitted.
- Provide tag-filtered LMS `status` / `playlist_loop`, current-track `songinfo`,
  and Jive `contextmenu` / `trackinfo items` text menus for More Info.
- Clear obsolete metadata on stop, station/track changes, takeover and lease
  expiry; preserve duplicate-notification suppression and existing playback.
- Keep the Applet Installer adapter attached after close/free/reopen. Wake
  asynchronous downloads on errors and prevent false successful-install
  bookkeeping, stuck waiting windows and reboot after failed extraction.
- Preserve existing firmware 7.7.2 support, storage safeguards and HTTP tracing.

See [metadata architecture and validation](now-playing-metadata.md) and
[installer compatibility](APPLET_INSTALLER_COMPAT.md).

## Artifacts and catalogs

- ZIP: `StandaloneBase-0.5.9.zip`, 39 flat entries, 2,529,730 bytes.
- SHA-1: `fe29ae5f40b0957d5654afe5d98d4964a6aee5ea`.
- SHA-256: `b9583f2802732c59dfb7212d82515a749a5fc7096b489ede364e1c3a2496c004`.
- Build: `scripts/build-arm.sh`; package: `packaging/build-package.ps1`.
- Runtime version is 0.5.9. The archive includes the preceding Base 0.5.8
  self-entry because an archive cannot contain its own final checksum. The
  published repository/bootstrap catalogs contain the final 0.5.9 entry and
  are merged through the existing catalog refresh.
  To reconstruct the embedded catalog when packaging this commit, retain the
  current root version and other applet entries, restore only the Base applet
  entry from `config.json` at preceding release commit `7abc97d`, and pass that
  catalog to the package script with `-ConfigPath`. Archive timestamps are not
  normalized, so rebuilt ZIP checksums need not match the published archive.
- Update the Base-only manifest and the combined manifest at
  `http://www.bytestack.nl/sbstandalone/extensions.xml`. Preserve other
  published entries and bootstrap server settings; restart its container.
- Repository catalog also retains the verified Radio 0.9.11 release, Spotify
  0.6.2 and Qobuz 0.1.2. Provider-specific Lua/native changes are packaged in
  their respective repositories, not inside the Base ZIP.

## Validation boundaries

Static ARMv5 build, 13 metadata unit cases, native ownership/expiry tests,
Comet lifecycle and real HTTP metadata queries pass. The installer adapter's
reopen lifecycle, ZIP handling and asynchronous-error tests pass. Base metadata
HTTP/Comet tests also pass under ARM926 QEMU. Existing image-render build
warnings remain unrelated to this release.

A targeted pre-release metadata build was installed on a firmware 7.7.2 Radio:
Jive restarted, the Radio applet registered and native health/status passed.
Actual playback, stock More Info rendering and third-party skin behavior still
need physical validation; do not interpret startup verification as UI testing.
No physical Radio is deployed or restarted as part of remote publication.

Keep hosting credentials, deployment configuration and server backup details in
local ignored files. They are not release sources.
