# Phase 2D — firmware checksum compatibility and web UI

This phase prepares and validates firmware only. It does not authorize or test
installation. The earlier watchdog/transfer risk remains a separate investigation.
No flashing code, UBI volume, UBIFS handling, kernel module, watchdog setting,
physical permission mechanism, Comet advertisement or native updater was changed.

## Checksum and candidate lifecycle

The expected checksum accepts exactly 40 SHA-1 or 64 SHA-256 hexadecimal
characters. ASCII uppercase hex is normalized to lowercase. Other lengths,
non-hex input, ambiguous upload headers and missing checksums are rejected.
The UI trims surrounding field whitespace; native validation is strict.

Upload and URL download workers calculate both SHA-1 and SHA-256 as network
entity bytes are streamed to the private file. Only the supplied algorithm is
selected for the external checksum comparison. The shared read-only inspector
then independently calculates both hashes over the staged file and retains all
existing ZIP structure, sizes, CRC, internal payload MD5, board and version checks.
Native UBI headroom requirements are unchanged.

Only calculated SHA-256 identifies a validated candidate:

```text
supplied SHA-1 or SHA-256
           |
bounded worker -> verify complete entity -> validate staged native container
           |
calculated SHA-256 -> atomic <sha256>.bin -> existing local firmware URL
           |
physical opt-in -> existing Review Update -> native confirmation (not invoked here)
```

SHA-1 values never become staging filenames or local URL identifiers. A cache
hit must match both the selected expected checksum and its existing SHA-256
identity; matching SHA-1 cannot change a cache key's SHA-256. Revalidation reads
the existing file, without downloading or allocating another firmware image.
Upload failures preserve the previous candidate; replacement and revocation
follow the existing lifecycle. Successful validation time is recorded separately
from the last attempted check.

SHA-1 is a compatibility option, **not equivalent in security to SHA-256**.
Either checksum establishes consistency with its supplied value, not independent
publisher authenticity. Neither checksum is automatically trusted from an
uploaded archive. There is no signature-verification claim.

## API and saved-configuration compatibility

- Upload: `X-Firmware-Checksum` accepts either length. The old
  `X-Firmware-SHA256` header still accepts 64 hex characters. Both header names
  together, or duplicate checksum headers, are rejected.
- URL configuration: `PUT /api/firmware/config` accepts `url` and `checksum`.
  The old `sha256` request field is accepted when `checksum` is absent.
- `POST /api/firmware/check` optionally accepts `{"checksum":"..."}` to
  revalidate the existing staged image using another supplied checksum.
  This is paired-browser/write-authorized preparation, not permission to install.
- Status adds `checksum`, `checksumAlgorithm`, `calculatedSha256`, `imageSize`,
  `urlChecksum`, and `lastValidated`. Existing fields remain available.
- Saved `sha256` remains the canonical candidate identifier. `checksum` stores
  the selected external pin. The historical `urlSha256` field is retained as a
  compatibility alias for the URL's expected checksum; it can now contain SHA-1.
  Consumers should use the new algorithm-neutral `urlChecksum` status field.
- Old configurations with only `url`/`sha256` are read without a migration write.
  An unchanged URL/pin save does not revoke an already validated candidate.
  Restart still requires validation and fresh physical permission; metadata is
  never treated as runtime authorization.

## Web interface

The firmware page uses existing cards, colors and typography with firmware-scoped
CSS only. Current and available versions, Baby revision, status, validation and
permission use an aligned definition list. Upload/URL selection shows only the
relevant fields, with explicit labels and the requested checksum helper text.

Progress appears only during an operation and shows percentage, bytes, average
browser-reported transfer speed and phase. Browser transfer speed describes bytes
handed to the network, not guaranteed device disk throughput. Cancel appears only
while the current upload request can be aborted. URL preparation retains its
existing bounded worker timeout, without an invented cancellation operation.
Busy controls are disabled. There are no automatic upload retries.

Polling remains at the existing three-second interval while the page is active.
Status updates change text nodes rather than rebuilding cards, preserving form
contents, focus and open details. Leaving the page stops polling and aborts its
active upload. Untrusted versions/errors use `textContent`; filenames remain in
the browser's native file input. No browser-side firmware installation or physical
permission controls were added.

Installation instructions are short. Checksum security help and technical values
are collapsed by default. The separate Revalidate Staged Image button can use a
checksum entered in the upload checksum field without submitting another file.

## Files changed for this phase

- `native/sbwebserver/src/firmware_image.c`, `.h`: normalization, dual file
  hashing, calculated digests and image size in validator results.
- `native/sbwebserver/src/firmware.c`: checksum-aware preparation, canonical
  publication/cache checks, legacy input/config compatibility and status fields.
- `native/sbwebserver/src/image_render.c`, `.h`: dual hashing inside the existing
  streaming firmware HTTP fetch path; image fetching behavior is unchanged.
- `native/sbwebserver/web/js/firmware.js`, `web/css/style.css`, `web/index.html`:
  redesigned page and asset cache-version changes.
- `tests/firmware/test_image.py`, `test_upload.py`, `test_http.py`: expanded
  validator/upload/cache/restart compatibility regressions.
- `tests/firmware/test_download.py`: real host fixture HTTP streaming, redirect,
  both algorithms/cases, canonical naming, cache hits and cache identity checks.
- `tests/firmware/test_ui.py`: optional Playwright browser regression/preview.
- `tests/CMakeLists.txt`: download test and existing fixture origin mapping in
  the host-only test executable. No test origin bypass enters the release binary.

## Host and build validation

The complete configured host suite passed **48/48** in 147.62 seconds. Focused
tests cover both algorithms/cases, bad values/lengths, truncation, source paths,
SHA-1 replacement, canonical naming/cache identity, revocation and compatibility
checks. The additional legacy-only saved-config/restart fixture also passed
separately (46.71 seconds).
The optional real-browser suite passed using installed Microsoft Edge through
Playwright, including safe error/version text, stable DOM nodes, source switching,
busy controls, transfer/validation states and 390-pixel mobile layout.

```powershell
wsl -- /mnt/c/Projects/SBHttpsProxy/build/toolchains/cmake-3.31.8-linux-x86_64/bin/cmake --build build-release-060 -j 4
wsl -- /mnt/c/Projects/SBHttpsProxy/build/toolchains/cmake-3.31.8-linux-x86_64/bin/ctest --test-dir build-release-060 --output-on-failure -j 1
$env:SB_TEST_BROWSER_CHANNEL='msedge' # Or use Playwright's installed Chromium.
python tests/firmware/test_ui.py . .local/phase2d/screenshots
wsl -- sh scripts/build-arm.sh
```

The ARMv5 static build passed. Existing image scratch-path truncation and
third-party NanoSVG signedness warnings remain. No new runtime library or daemon
was added. Hash contexts and digest strings are bounded; no image-sized RAM
allocation or additional firmware copy was introduced.

## Device validation

The release wrapper's actual `baby_9.0.2_r17157.bin.sha` was read on the laptop
and compared against the native file, not assumed from earlier notes:

- Native size: 16,032,273 bytes.
- SHA-1: `9ab354c46ae98027d5a3164c2ae13166875d2c87`.
- SHA-256: `dc783d525e0ea17736f36ff51d1424e6d8909cc5a9ba51f0cfdcacef56d65d6d`.

Four application files were backed up, replaced and hash-verified on
192.168.1.228: `sbwebserver`, `firmware.js`, `style.css`, `index.html`. Only the
webserver was restarted, retaining the earlier explicit 64 KiB/s upload test cap.
Physical permission was checked disabled before deployment. SqueezePlay was not
restarted. The existing staged image was retained. No large transfer was started.

Through normal physical PIN pairing, both SHA-1 and uppercase SHA-256 pins
successfully revalidated the existing image on the Radio. Both returned
`candidateReady: true`, the expected version and 16,032,273-byte size, and the
same canonical calculated SHA-256. Physical permission and `ready` stayed false.
The staged image was not uploaded or downloaded again. Native metadata confirms
the SHA-256 filename/identity and the final normalized SHA-256 pin.

The first live-browser automation clicked navigation before the existing
asynchronous navigation handler was initialized and timed out. The host browser
fixtures passed; the corrected live browser retest waited for initialization
before clicking and passed using a fresh physical pairing code. Both checksum
lengths passed the actual input validity check. The live page showed the validated
candidate with permission Disabled, no idle progress bar, and no JavaScript page
errors. Desktop and 390-pixel mobile screenshots were captured; the mobile page
had no horizontal overflow. The boot ID was unchanged, and permission remained
disabled afterward. No further image transfer or revalidation was performed for
this UI-only retest.

Live screenshots: `.local/phase2d/screenshots/device-firmware-desktop.png` and
`device-firmware-mobile.png`. Result: `.local/phase2d/device-validation.json`.
Private diagnostic helpers/evidence and screenshots are under ignored
`.local/phase2d/`; no session cookies or pairing PINs are written to those reports.

## Remaining limitations

Do not declare firmware installation ready based on this phase. The earlier
watchdog/transfer risk, unrestricted upload behavior, aligned CPU measurements and
actual firmware installation remain outside this task. Physical Review was
confirmed by the user before this phase; its code and native UI are unchanged.
