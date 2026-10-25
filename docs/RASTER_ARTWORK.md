# Bounded PNG and JPEG artwork

`ImageRender.artworkUrl` now routes remote PNG/JPEG/SVG artwork through
`/api/image/render`. The display adapter explicitly marks extensionless URLs
as `image/*`. Shared transport calls for audio, playlists and JSON/XML remain
unchanged; the Radio's actual stream-routing behavior is regression-tested.
The existing URL validation, public-address
checks, redirect limit, isolated worker, seven-day cache and atomic publication
also apply to raster images. Cache keys include the source URL, requested size
and converter version. Local resources and already-local renderer URLs pass
through unchanged.

PNG/JPEG output fits within 320 by 240 pixels, preserving aspect ratio without
cropping or enlarging smaller images. A square image therefore becomes at most
240 by 240. Requested smaller sizes remain supported. Output is RGBA PNG.

Downloads stream into the private job file, including chunked and close-framed
HTTP responses. They do not accumulate a complete compressed image in Lua or
in Mongoose's receive buffer. The compressed input limit is 8 MiB, download
deadline eight seconds, image dimension limit 8192 per axis and decoded pixel
limit 16 megapixels. The worker has a twenty-second overall deadline. The
converter retains its 32 MiB address-space limit. The CPU limit is ten seconds
for raster input on ARM926 and five seconds for SVG.
Private job files use extended storage, keeping large compressed originals
off the Radio's ramfs. Without extended storage the input limit drops to
512 KiB and private jobs use `/tmp`. Only one conversion job runs at once.
SVG retains its separate 1 MiB parser limit and existing rendering behavior.

The libpng reader normalizes one scanline at a time. An integer area filter
uses two output-width accumulator rows and alpha-premultiplied color sums,
avoiding both a source-sized bitmap and a destination-sized accumulation grid.
Palette, grayscale, transparency and 16-bit PNG input are handled. Baseline
JPEG uses libjpeg-turbo's 1/2, 1/4 or 1/8 DCT scaling when possible, followed
by the same area filter. Progressive and other multi-scan Huffman JPEGs use
libjpeg-turbo's coefficient arrays, preflighted using MCU-rounded component
block dimensions and capped at 4 MiB. The library memory manager also has a
5 MiB limit (coefficients plus overhead), with no backing store. A progress
callback rejects more than 100 scans. Scaling reduces pixel-row storage but
cannot reduce progressive coefficient storage, so oversized coefficient
images are rejected before `jpeg_start_decompress`.

Interlaced PNG, arithmetic JPEG and CMYK JPEG remain explicitly unsupported.
JPEG RGB/YCbCr/grayscale and 4:4:4, 4:2:2, 4:2:0 subsampling are supported.
EXIF orientation is ignored, as before; output follows encoded pixel order.
Unsupported/corrupt images are never cached. No full-resolution RGB bitmap
is retained alongside the coefficient arrays.

Build dependencies reproducibly with `scripts/build-image-libs.sh`. It pins
the exact zlib, libpng and libjpeg-turbo source commits and builds static host
and ARM libraries. CMake host builds need their prefix in `CMAKE_PREFIX_PATH`.
`scripts/build-image-test.sh` builds the standalone host test executables;
`scripts/build-image-arm.sh` builds only the two affected ARM executables.
The full `scripts/build-arm.sh` also links these codecs. Dependency notices
are included in the existing packaged `SVG-THIRD-PARTY-NOTICES` file.

Validation commands (from the project root on Linux/WSL):

```sh
python3 tests/images/test_raster.py build-image-host/svgpngrenderer --marcher build-marcher.png
python3 tests/images/test_raster.py build-arm/svgpngrenderer --qemu /usr/bin/qemu-arm --marcher build-marcher.png
python3 tests/images/test_renderer.py build-image-host/svgpngrenderer
python3 tests/images/test_image_http.py build-image-host/sbwebserver-test build-image-host/svgpngrenderer native/sbwebserver/web --test-network
```

The raster tests cover aspect ratios, no enlargement, exact area averaging,
alpha edges, grayscale, palette transparency, 16-bit input, JPEG DCT scaling,
dimension bombs, corrupt/truncated data and explicit format rejections. HTTP
tests cover streaming inputs above 1 MiB, chunked and close-delimited bodies,
truncated responses, existing SSRF checks, cache behavior and worker isolation.

Marcher Vibe's original Squarespace PNG is 1806 by 1806 and 1,670,983 bytes.
The optimized conversion produced 240 by 240 with 19,704 bytes of explicit row
working storage and approximately 3.4 MiB peak host process RSS. On the Radio,
the Marcher PNG became 53,453 bytes; cold conversion took about 7.1 seconds,
and a subsequent cached HTTP request completed in about 0.4 seconds over SSH.
This firmware reports zero for `ru_maxrss`, so that field is unavailable on
the device rather than an actual zero-memory claim. Measurements appear in
`/tmp/standalonebase/sbwebserver.log`. Download sizes, source host, output
dimensions and renderer errors are logged without source URL query strings.

## Last.fm progressive JPEG fix (23 October 2026)

Reproduction URL:
`https://lastfm.freetls.fastly.net/i/u/654f6e791c2117e067c23a91679bb73c.jpg`.
The actual 70,094-byte file has JPEG SOI `FF D8` and progressive SOF2 `FF C2`,
550x545 YCbCr with 2x2/1x1/1x1 sampling. Upstream download was successful;
`decode_jpeg` then explicitly rejected `progressive_mode`, returning 65,
which `convert_file` mapped to HTTP 415. Missing codecs, MIME detection and
TLS were not the cause. Existing statically linked libjpeg-turbo 3.2.0 supports
progressive decoding; the pinned build disables SIMD/TurboJPEG API/shared
libraries, not progressive support. libpng and zlib still encode RGBA PNG.

Pipeline: applet artwork URL -> ImageRender.artworkUrl -> loopback
`/api/image/render` -> bounded download through sbproxy for HTTPS -> private
input file -> svgpngrenderer signature detection -> libjpeg/libpng -> scaled
scanlines and integer area fit -> libpng RGBA output -> atomic cache -> HTTP
image/png. File extensions and Content-Type do not select the decoder.

Renderer diagnostics now emit `HTTPTRACE event=image_decode` into syslog
(`/var/log/messages`) and include the image-render trace ID, format and
category. Categories distinguish unsupported_format (69), invalid_image (65),
decoder_failure (74), resource_limit (75), and encoding_failure (73).
Library decoder messages and sampling/memory details remain in renderer
stderr (`/tmp/standalonebase/sbwebserver.log`). The service keeps 415 for
unsupported/invalid/decoder failures and 503 for resource/output failures.
SVG rendering and unrelated network/artwork behavior are unchanged.

Before: host and Radio 192.168.1.225 returned exit 65 / HTTP 415.
After: the exact file decodes to 242x240; coefficients require 940,800 bytes,
row/filter buffers 13,684 bytes. Host peak RSS was 3,712 KiB, elapsed 35 ms.
On 192.168.1.225 the cold full HTTP request returned 200 and 124,383 bytes
of valid PNG in 4,416 ms, with rendering taking 2,984 ms. Retrieved PNG was
independently decoded to confirm dimensions and integrity. The two updated
binaries were backed up under `/mnt/storage/jpeg-render-backup-20261023-122236`.
Firmware ru_maxrss reports zero, so it cannot measure device peak RSS.

Regression fixtures are synthetic solid-color JPEGs generated with Pillow
(640x480 RGB 230/40/80, quality 85 for sampling variants; grayscale;
EXIF orientation 6; CMYK; 1600x1600 progressive 4:4:4 for memory rejection).
Pillow is not a runtime test or device dependency. Baseline/progressive, PNG,
corruption, limits, output failure, grayscale, EXIF policy and subsampling are
tested on host and ARM/QEMU. The HTTP network fixture suite exercises JPEG
200/415/503 results, fetching, caching and existing SVG/API regression checks.
The downloaded Last.fm artwork is kept outside tracked fixtures; pass
`--lastfm .local/jpeg-investigation/lastfm.jpg` to `test_raster.py` to validate
its byte count, SOF marker and output dimensions.

## Optional LMS Community fallback

The existing `/api/image/render` endpoint is local-first for every consumer
(Radio, Radio Browser, RadioFeeds, TuneIn, Spotify and Qobuz). A cache hit or
successful local JPEG/PNG/SVG conversion never invokes the external service.
No applet APIs, SVG decoder rules or Wikipedia requests change.

### Confirmed 413 cause and unchanged limits

On test Radio 192.168.1.225 the 23 October trace for Last.fm
`ee575a61705152eb978262ce56614958.png` declared **739,205 bytes** and returned
413 before any body bytes were accepted: the active cap was **524,288 bytes**
without a verified extended-storage cache. This was a compressed-download
size rejection, not a JPEG decoder allocation or dimension failure. The old
stderr line incorrectly always printed 8 MiB; it now prints the active cap.
Other 413 paths are declared/cumulative download bounds, bounded HTTP receive
buffers, or the existing 1 MiB local-upload request limit. A local upload has
no remote URL and cannot use fallback.

| Limit | Before and after |
| --- | --- |
| Original compressed download | 512 KiB without extended cache; 8 MiB with it |
| Original download deadline | 8 seconds |
| HTTP/upload body | Existing 1 MiB SVG upload rule |
| Source dimensions/pixels | 8192 per axis, 16 megapixels |
| JPEG multi-scan coefficients | 4 MiB, library budget 5 MiB, at most 100 scans |
| Raster helper | 32 MiB address space, 10 CPU seconds |
| Image worker | 40 MiB address space, 20 seconds overall |
| Output | Existing RGBA PNG, at most 320x240, 2 MiB encoded cap |
| Concurrency | One existing image worker, up to two coalesced waiters |
| Existing cache | Extended-storage cache only, 20 MiB, seven-day TTL |
| New fallback response | At most 512 KiB or a configured lower cap |
| New fallback download | Default 3 seconds; configurable 1?5, inside the same 20-second job deadline |

Decoder dimensions, coefficient allocation and renderer failures have separate
exit codes and diagnostics, generally mapped to 415 or 503, not 413. Output
encoding/publication failures remain 503 and do not trigger fallback.

### Eligibility and privacy

Remote original downloads rejected with 413 qualify, provided the upstream
status was 200/413 (a huge 401/403/404 body must not turn into fallback).
Rejected XML/SVG remains local to preserve the existing SVG feature/security
policy; it is not externally retried. Renderer invalid/unsupported/decoder
failures for other formats (65/69/74) and decoder resource
limits (75) qualify. Transport, DNS, timeout, renderer-unavailable, filesystem,
encoding, authorization and security failures do not. The original public
HTTP error is preserved if fallback also fails.

Before sharing the URL, the worker rechecks public DNS, HTTP(S) syntax, and
credentials. Loopback/private sources and userinfo are rejected; URLs on the
LMS host or configured fallback host cannot be wrapped again. Query keys are
percent-decoded and conservatively restricted to public image parameters:
`w,h,width,height,size,format,quality,fit,crop,auto,v,lang`. Known token, auth,
signature, session, secret and credential patterns are rejected. Unknown
queries stay local rather than risking disclosure. Complete permitted query
parameters are preserved by encoding the normalized source URL once (an
existing `%20` correctly becomes `%2520` in the service path).

Enabling the feature shares public artwork URL paths and permitted image
query parameters with an external service, which can observe the request and
source. Opaque credentials embedded in arbitrary URL paths cannot always be
distinguished from image identifiers: explicitly disable fallback for artwork
sources whose paths must not be shared. Source cookies, authentication
headers and local files are never forwarded. No complete payload is logged.

### Settings

Use the existing `settings.conf` in the webserver config directory (normally
`/mnt/storage/standalonebase/settings.conf`). Existing settings save operations
preserve these fields. They are file settings, without new browser UI:

```ini
artwork_external_fallback_enabled=1
artwork_external_fallback_base_url=https://api.lms-community.org/img/compatible/
artwork_external_fallback_timeout=3
artwork_external_fallback_max_bytes=524288
```

Fallback is enabled by default when the setting is absent, including existing
configuration files without that key. A saved `artwork_external_fallback_enabled=0`
remains disabled. This is a Standalone Base webserver setting, not a Radio applet
setting. Local processing remains first; external requests occur only on eligible
failures. The change requires an updated webserver binary.

The base URL must be a public credential-free HTTPS URL ending in `/`, with
no query. Invalid values disable the attempt; size and timeout settings are
clamped to the limits above. The width sent is `min(requested width, requested
height,240)`, followed by the usual local aspect-fit conversion. No
unconditional JPEG conversion is requested, so transparency can survive.
The endpoint is `/img/compatible/<encoded-source>?w=<width>`.

### Bounded operation and caching

A qualifying failure removes original input/output files before fetching the
replacement. The same worker streams to the same private file; no original
and fallback bitmaps or full compressed bodies are retained together. HTTPS
uses the existing public-fetch-guarded sbproxy, without new TLS libraries or
services. Redirects are revalidated and bounded (four worker hops, plus the
proxy's existing three-hop guard per HTTPS request), within one download
deadline. Disconnected clients terminate the whole worker group once no
coalesced waiter remains. The job's existing overall deadline also applies.

One fallback is attempted per original request. Returned bytes must have a
PNG/JPEG signature; Content-Type is diagnostic, never sufficient. The normal
local decoder, coefficient/dimension checks, area fit, PNG encoder and output
limit apply again, with no second fallback. Failures do not populate the
artwork cache. Successful fallback output uses the same original-URL and
requested-size cache key, TTL, atomic publication and eviction as local output.
A fixed 16-entry RAM ring suppresses repeated failed fallbacks for 60 seconds;
no negative cache or fallback history is written to flash. Restart clears the
backoff. Cache hits avoid both original and fallback downloads.

The unchanged overall 20-second deadline can leave little time for fallback
after an expensive local decode; recovery is best-effort and does not extend
local limits. Unknown query parameters and overlong encoded URLs are skipped.
LMS service reachability, supported input formats and availability are external
dependencies. There is no retry loop or alternate external provider.

### Diagnostics and validation

Failures eligible for fallback log trace-linked `artwork_local`,
`artwork_fallback`, `artwork_fallback_result` and final `image_result` events.
Replacement format, bytes, HTTP status and conversion result are distinguished.
The final source is `local`, `external_fallback` or `cache`. Normal local success
adds no extra fallback logging. The prior JPEG decoder diagnostics are retained.

Deterministic tests use a mock origin and mock existing HTTPS proxy, with real
workers/renderers. Run:

```sh
python3 tests/images/test_image_fallback.py build-image-host/sbwebserver-test build-image-host/svgpngrenderer native/sbwebserver/web
```

Coverage: local baseline/progressive JPEG, PNG and SVG without fallback;
unsupported input, download 413 and coefficient limits; replacement JPEG/PNG,
invalid bodies, size limits, 404, timeout, redirects and redirect security;
source encoding, credentials/private addresses/unknown queries, recursion,
cache reuse, failure backoff, coalescing/busy limits, client cancellation and
responsiveness afterward. Existing HTTP, SVG and host/ARM raster suites remain
required. The mock host mapping exists only in `SB_IMAGE_TESTING` builds.

Live LMS test on 23 October: the supplied `/img/compatible` Last.fm example
returned HTTP 200, image/jpeg, 28,713 bytes and 240x238 pixels, independently
decoded on the host. Local progressive JPEG remains the primary path.

Device validation (192.168.1.225): fallback is enabled in its existing
settings.conf for testing; the default was disabled at that time. It is now
enabled by default as described above, while explicit opt-outs remain respected.
Only sbwebserver was replaced; the progressive-capable renderer and other
services were retained. Previous binary/config were backed up under
`/mnt/storage/artwork-fallback-backup-20261023-124144`.
The 739,205-byte Last.fm PNG was rejected at headers, with zero original body
bytes buffered. LMS returned a 23,276-byte JPEG (240x235), which the unchanged
local renderer converted to a valid 109,953-byte PNG. Final HTTP status was
200, source=external_fallback, elapsed 6,276 ms. The earlier progressive JPEG
still returned local HTTP 200 and a valid 242x240 PNG without fallback.

Final checks: deterministic fallback and existing HTTP integration suites pass;
settings defaults/persistence/clamping test passes; host and ARM/QEMU raster
suites (including the exact progressive JPEG) and SVG renderer regressions
pass. The fallback tests include a genuine synthetic WebP input and verify
that PNG fallback preserves alpha. Error-response receive bounds are also
limited to the smaller of the existing receive cap and the fallback byte cap.

During an active fallback request on the device, host requests fetched the
web root, remote-control JavaScript and stylesheet successfully in roughly
0.12?0.14 seconds each. Jive's PID remained 756 and sbbase continued receiving
playback STAT frames. The webserver idle RSS was 368?384 KiB (about 8?24 KiB
over its pre-feature 360 KiB measurement), with no observed growing resident
allocation. This is a spot check, not a long-running memory soak or an audible
playback/interactive virtual-radio test; those manual checks remain advisable.
No flash-backed cache is available on this device with extended storage off,
so successful cache reuse is covered by the deterministic extended-cache test.
Repeated uncached requests may each use fallback; failed attempts have RAM
backoff. The new fallback adds no persistent history/cache writes outside the
existing optional cache and explicit configuration/binary deployment.

## Shared Lua artwork URL normalization

`applet/ArtworkUrlNormalizer.lua` is a pure, dependency-free ordered rule
component. `normalize(url)` returns the input unchanged when no rule matches,
including malformed/non-string/oversized input. It never fetches, decodes or
consults settings. The initial and only rule recognizes the exact hostname
`upload.wikimedia.org`, HTTP(S), a `/wikipedia/<project>/thumb/` path segment,
and a final filename beginning with decimal `<width>px-`. It replaces only
that width with `250px`, regardless of image extension. Explicit ordinary
ports are accepted; host lookalikes, userinfo, original image URLs and
non-thumbnail filenames are unchanged. Scheme/host spelling, directory,
filename, escapes, query and fragment bytes are retained. The rule is
idempotent. Future rules belong in this component's ordered rule list.

The integration point is `ImageRender.artworkUrl`, after its existing artwork
eligibility and validation checks but before encoding the source into the
renderer request. `getStandaloneImageURL` calls it directly;
`getStandaloneArtworkURL`, the shared Jive `Artwork.route` adapter and its
known loopback-HTTPS unwrap path reach it as well. Individual applets do not
change. Non-artwork transport (streams, OPML, JSON) and local resources retain
their existing routing. Artwork.route explicitly marks artwork with image/*,
so non-PNG/JPEG/SVG source formats are also normalized and can use fallback.

The central native cache hashes the normalized source URL and requested
render dimensions. Thus 250px, 640px and 1024px source variants generate the
same renderer request/cache identity when all other parameters match. Native
download, conversion and LMS fallback receive that same normalized source.
Different queries or render dimensions remain distinct. Cache expiry,
eviction, persistent write policy and decoding limits are unchanged.
Per-consumer icon/station caches retain their existing keys; Base does not
change station identity or rewrite unrelated applet cache policies.

There is no universal Lua hook for clients which call the native HTTP endpoint
directly. Such callers bypass Lua and remain unchanged; local SVG uploads
also have no URL to normalize. The smallest shared Lua integration covers
Base's artwork services rather than duplicating provider rules in C or
individual applets. URL fragments are preserved by the normalizer, but this
does not relax the native endpoint's existing fragment/security validation.

On rewrites, the helper retains the original URL and emits a debug diagnostic
with original and normalized URLs (queries/fragments redacted and text
bounded). No history or extra cache is retained. The native request trace
continues to identify the normalized source. No diagnostic is added for URLs
that pass through unchanged.

Run the Lua 5.1-compatible fixture:

```sh
lua tests/images/test_artwork_url_normalizer.lua .
```

Coverage includes all supplied SVG/PNG examples, JPEG/GIF, other Wikimedia
projects, query/fragment preservation, exact authority matching and lookalike
rejection, malformed/non-string inputs, idempotence, loopback-proxy routing,
normalized renderer/cache identity, distinct output dimensions, normalized
fallback source and redacted original-URL diagnostics. Existing artwork
adapter and image-helper regressions also pass. Packaging includes the new
module alongside ImageRender. No native renderer, fallback policy or public
artwork API change is required.
