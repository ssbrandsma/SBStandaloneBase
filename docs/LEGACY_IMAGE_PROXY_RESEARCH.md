# Replacing the legacy Logitech artwork proxy

Research and proposed design, 2026-10-17. **No replacement is implemented by this report.**

## 1. Scope and evidence

Inspected the current Standalone Base and Standalone Radio sources, Spotify Connect and Qobuz Connect checkouts, local upstream SqueezePlay source, and read-only copies of the installed Radio's Jive Lua files. Installed-file references below are relative to `/usr/share/jive/`; Base references are relative to this repository. Companion references are relative to their respective repositories. Captured firmware files and diagnostic scripts remain in ignored `.local/`, not in the distribution.

The installed firmware is the authoritative source for the line numbers below. The upstream checkout differs: for example, its ImageViewer constructs an HTTPS Logitech proxy URL, whereas the installed version constructs HTTP. This is not a verification of every newer firmware variant.

Eight host-side assertions executed the **actual captured `fetchArtwork` function**, with stubbed caches, task scheduling and hostname lookup, without making network requests. They confirmed public PNG, public JPEG and HTTPS SVG rewriting; numeric-loopback and private-numeric bypass; the localhost defect; and unchanged LMS ID/relative-path handling. These are routing tests, not end-to-end decoder or resource measurements.

## 2. Confirmed root cause

`jive/slim/SlimServer.lua`, `fetchArtwork`, starts at **1096**. After both artwork caches miss, it classifies the source:

- **1154–1159:** hexadecimal/negative cover IDs become LMS `/music/<id>/cover_WxH_m[.format]` requests.
- **1162:** any source beginning `http` enters the external-URL branch, including HTTPS.
- **1166–1172:** certain numeric HTTP private addresses and numeric loopback bypass the proxy.
- **1175:** everything else becomes `http://<SN hostname>/public/imageproxy?w=...&h=...&f=...&u=<encoded original>`.
- **1178–1183:** relative image filenames retain LMS resize-suffix handling.

The source comment explicitly says the SN resizer is used on all remote URLs until SqueezePlay can resize with better quality. **There is no PNG/JPEG compatibility check, MIME check, or existing-dimensions check.** A perfectly usable public 320×320 PNG is proxied just like an SVG. The obsolete dependency therefore originates in the shared Jive framework, not in Standalone Radio's image downloader.

`applets/SqueezeboxBaby/SqueezeboxBabyMeta.lua:71` sets `jnt:setSNHostname("baby.squeezenetwork.com")`. `jive/net/NetworkThread.lua:63` otherwise defaults to `www.squeezenetwork.com`, and its getter at **380** returns that value. This explains the exact hostname in the reported request. The retired infrastructure's current availability and response codes were not independently measured in this investigation; the unconditional dependency itself is source-confirmed.

There is a **second independent producer**: `applets/ImageViewer/ImageSourceServer.lua`, `requestImage` at **170**, constructs the legacy URL at **214**. Its numeric-private bypass at **181–185** excludes loopback, and its `not ^http://` test at **188** incorrectly treats an original HTTPS URL as a server-relative image path. Patching only `SlimServer` will not repair that viewer path.

### Existing bypass and cache defects

`scripts/patch-squeezeplay-artwork.sh` already patches numeric loopback into `SlimServer`. It is not a general replacement. Its outer localhost condition is not matched by any inner localhost alternative, so `http://localhost/...` still goes to Logitech. The private-address patterns are historical heuristics, not complete subnet parsing or an SSRF boundary.

`SlimServer.lua:1190` stores an in-flight `true` marker before queuing. `_getArtworkThumbSink` (**935**, error branch **948–949**) logs transport failure but does not remove this marker; subsequent `fetchArtwork` calls return early at **1125–1134**. A failed request can consequently remain suppressed until cache reset/cancellation. Compressed bytes are also cached before decode validation at **955–958**. This is a source-confirmed failure path; its contribution to any particular observed missing image needs a matching runtime trace.

`_loadArtworkImage` (**896**) fully decodes before resizing (**898**). Its size mismatch condition uses **AND** (**916**) and its scale uses only the width ratio (**917**). Thus local scaling is not a reliable contain-within-W×H implementation. Exact-size transparent output from the proposed service avoids relying on this behavior.

## 3. Current request flow

```text
Radio / Spotify metadata                    LMS menus, popups, Now Playing
       | artwork_url                                   |
       v                                               |
Base nowplaying -> CometD item.icon --------------------+
                                                       v
                                            SlimServer.fetchArtwork
                                             | decoded weak cache hit -> icon
                                             | compressed RAM hit -> full decode -> icon
                                             | in-flight marker -> wait
                                             v cache miss
                       +---------------------+----------------------+
                       |                     |                      |
                LMS ID/relative       numeric local HTTP      public HTTP(S)
                       |                     |                      |
                LMS artwork pool       direct SocketHttp     Logitech imageproxy
                       +---------------------+----------------------+
                                             |
                                   RequestHttp redirect handling
                                             |
                                  compressed cache -> Surface decode
                                             |
                                     optional local scaling -> icon

ImageViewer.requestImage -> separate URL classification -> Logitech/direct/LMS
                                                      -> SocketHttp -> Surface

Radio LogoCache -> UrlTransport -> direct HTTP or sbproxy HTTPS
                              \-> Base /api/image/render for recognized SVG
                                    -> UBIFS PNG hit OR download + svgpngrenderer
                              -> PNG/JPEG local logo file and optional Surface
```

### Transport and formats

- `SlimServer.processArtworkQueue` (**973**) permits four in-flight artwork requests and uses a LIFO queue. Absolute HTTP-like URLs use `SocketHttp` (**988–995**); LMS-relative paths use the server's artwork pool (**996–998**).
- `SocketHttp.lua:97` uses `SocketTcp`. The inspected path does not negotiate TLS merely because a URL has an HTTPS scheme.
- `RequestHttp.lua:350` accepts 200; **362–401** follows GET redirects for 301/302/307, with a five-redirect limit. It creates another plain `SocketHttp`, not a call to Base's HTTPS transport. Parsing a relative Location uses generic defaults rather than resolving against the original URI. 303/308 are not handled by that redirect branch. HTTPS redirects and relative redirects must therefore not be delegated to this old client in the new design.
- Normal PNG/JPEG returned by the old proxy or a direct URL are decoded through `Surface:loadImageData`. No SVG renderer is invoked by `SlimServer` itself.
- Base's current SVG endpoint already handles secure fetching, redirects and conversion off the main event loop. A cached PNG avoids download and rendering. It does **not** implement general PNG/JPEG input resizing.
- The historical proxy URL expresses requested dimensions and output format, and the comment confirms a resizing-quality purpose. Its precise retired implementation, cropping rules and SVG support are **not established**. Do not promise byte-for-byte behavioral compatibility.

## 4. Affected artwork inventory

| Path | Confirmed source/call sites | Consequence |
| --- | --- | --- |
| Standard Now Playing and next-track preload | Installed `NowPlayingApplet.lua:102–117`, callers at 712/721 and 1862/1870 | Shared `fetchArtwork`; public URLs hit Logitech |
| Menus and server icons | Installed `SlimMenusApplet.lua:557,567` | Same shared pipeline |
| Browser item, title and large artwork | Installed `SlimBrowserApplet.lua:487,495,1009,1353` | Same shared pipeline |
| Notification popup artwork | Installed `jive/slim/Player.lua:1415` | Same shared pipeline |
| ImageViewer image | Installed `ImageSourceServer.lua:170,214,253–268` | Independent producer, decoder and error message |
| ImageViewer app icon | Installed `ImageSourceServer.lua:293` | Shared `fetchArtwork` |
| Standalone Radio native Now Playing | Radio `StandaloneBaseNowPlaying.lua`, station/track metadata URL selection; Base `native/sbbase/src/nowplaying.c:593,636,1082` | Base exports artwork as `item.icon`; it does not itself fetch it; stock Jive later rewrites it |
| Standalone Radio logo files | Radio `LogoCache.lua:_download,ensure`, `UrlTransport.lua:transportUrl,artworkUrl` | Separate download/file cache; its successful cached PNG does not guarantee stock Now Playing uses that file |
| Standalone Radio discovered track covers | Radio `TrackArtwork.lua:lookup`, `StandaloneBaseNowPlaying.lua:lookupTrackArtwork` | External cover URL enters metadata/shared Jive pipeline after transport normalization |
| Spotify native Now Playing | Spotify `SpotifyStandaloneNowPlaying.lua:97`, `SpotifyUrlTransport.lua:12` | HTTPS already becomes numeric-loopback sbproxy URL; existing patch bypasses Logitech, but does not provide bounded normalization/resizing |
| Spotify custom/legacy artwork UI | Spotify `SpotifyArtwork.lua:_download`, `SpotifyNowPlaying.lua:_setArtwork` | Own fetch and Surface resize; not automatically intercepted by `SlimServer` changes |
| Qobuz | Current Qobuz applet/native source search | No artwork producer found in inspected checkout. Its proxy use in `Playback.lua` is audio: do not redirect it. Future shared Now Playing artwork would inherit the Jive fix |
| Generic/future applets using native metadata or `fetchArtwork` | Base Now Playing interface and listed Jive callers | Automatically covered by central artwork fix, unlike direct SocketHttp/file loaders |

No `getArtworkThumb` function was found in these sources. Relevant actual symbols are `fetchArtwork`, `artworkThumbCached` and `_getArtworkThumbSink`. The two active legacy URL builders found are SlimServer and ImageViewer; other `squeezenetwork.com` uses include account/discovery behavior and are not artwork replacement targets.

Radio's optional `getStandaloneImageURL` service currently rewrites recognized SVG only (`applet/ImageRender.lua:16–24`). PNG/JPEG URLs remain unchanged there. Radio and Spotify can wrap HTTPS as `http://127.0.0.1:8765/https/...`. Any new helper must recognize these trusted local transport wrappers; blindly treating them as external sources would either fail SSRF validation or create nested proxying.

## 5. Recommended architecture and API

```text
SlimServer external-artwork adapter ---+
ImageViewer external-image adapter ---+-> shared Base Lua artwork-URL service
Other applets (optional adoption) -----+                  |
                                                         v
                                         existing sbwebserver endpoint
                                           | bounded PNG cache hit -> serve
                                           v miss / coalesced bounded queue
                                         one short-lived worker
                                           | public HTTP: existing fetch path
                                           | public HTTPS: existing sbproxy
                                           | validate each redirect/destination
                                           v bounded temporary source file
                                 +---------+-------------------------+
                                 | SVG                               | PNG/JPEG
                                 v                                   v
                           svgpngrenderer                  short-lived raster helper
                                 +----------------+------------------+
                                                  v
                                    validate result -> atomic UBIFS PNG
                                                  v
                                        Jive gets ordinary local HTTP PNG
```

Use the existing webserver and configured port. Proposed canonical route:

`GET /api/image/proxy?url=<encoded-source>&width=320&height=320&format=png&fit=contain`

Optional compatibility alias, using exactly the same validator/worker/cache:

`GET /public/imageproxy?w=320&h=320&f=&u=<encoded-source>`

Empty `f` and `png` mean PNG. Reject unsupported formats rather than pretending to emulate Logitech. JPEG output is unnecessary initially: PNG preserves alpha and existing Jive decodes it. Preserve `/api/image/render` and its current SVG callers; share infrastructure rather than replace that contract unexpectedly.

Default output: exact requested canvas dimensions, aspect-preserving contain, transparent padding. Explicitly define upscale behavior and include it in the cache revision. For already-correct PNG, a validated passthrough can avoid re-encoding. For JPEG, passthrough is possible if the canonical route explicitly allows original raster output; the PNG-only compatibility contract should convert it. **Never send a redirect to the external image** as an optimization: that bypasses resource/destination enforcement and reintroduces Jive's HTTPS/redirect problems.

### Authentication boundary

Artwork must work without browser PIN pairing. Reuse the existing image-render route's explicit local/internal authorization treatment and browser-session rules; do not expose internal credentials in generated artwork URLs. Confirm actual connection peer, not a forwarded header or Host alone. Restrict unauthenticated local artwork access to the intended loopback/internal path and keep remote management APIs protected. If LAN artwork consumers are needed later, specify an independent bounded authorization design first.

### Transport and SSRF policy

Reuse `image_render.c` URL parsing/public-address validation, pinned direct-HTTP connection and worker-side Mongoose fetching. HTTPS must use `sbproxy`'s existing `/https/<host>/<path>` interface with the opt-in `X-SB-Public-Fetch: 1` guard. Its current `/health` capability check must remain fail-closed when the guarded feature is missing.

The guard validates actual connection destinations, including redirects. Keep its public-IPv4/standard-port policy and reject credentials, malformed authority, control characters and unsafe destinations. Apply a **single global redirect budget** across HTTP/HTTPS transitions, supporting standards-based relative Location resolution within that policy. Do not forward browser cookies, Authorization or arbitrary caller headers to origins. Do not change proxy TLS compatibility settings: the current certificate-verification policy remains a documented limitation, not something this project silently fixes or weakens.

Only unwrap known local `/https/` and `/api/image/render` wrappers in the trusted Lua normalization layer, validating exact host, configured port, path and single decoding. Never allow an arbitrary LAN caller to turn a private URL into an allowed destination. Optional legacy-URL unwrapping must match known Logitech host/path, happen once and reject nesting/ambiguous duplicate parameters. Prevent renderer recursion.

### Cache and lifecycle

Reuse the existing extended-UBIFS cache and **shared** 20 MiB/256-entry budget, not another independent unbounded cache. Cache normalized PNG, keying canonical source URL, dimensions, fit, format, transformation version and orientation policy with a stable digest. Preserve significant signed query parameters; do not log them. Use current seven-day TTL initially, atomic same-filesystem rename, bounded startup/index work and bounded eviction. A valid hit must spawn no helper and contact no origin.

Keep temporary sources/results bounded and remove them on success, failure, timeout and cancellation. Extended storage unavailable: do not silently write persistent cache to firmware NAND. Decide explicitly between uncached volatile operation with a strict tmpfs budget or a clear unavailable placeholder; initial implementation should retain the current fail-closed storage behavior unless a separate fallback requirement is approved.

## 6. Raster helper, dependencies and resource budget

A **short-lived native local-file helper is appropriate**: decoder faults/limits stay outside the web event loop and process exit releases allocations. It must have no URLs, network, TLS or cache responsibility. Keep `svgpngrenderer` as-is; dispatch SVG to it and PNG/JPEG to a similarly small raster helper. Avoid making Lua decode/resize source files.

Recommended dependency experiment: PNG/JPEG-only pinned `stb_image`, existing vendored stb PNG writer, and a small bounded integer contain resampler. Use predecode image-info checks **and allocation hooks enforcing aggregate working memory**; metadata checks alone do not bound progressive JPEG or malformed decoder allocations. Disable unused formats/HDR/float paths. Compare quality and ARMv5 timing before settling on a resampler. Do not add libcurl, wolfSSL, ImageMagick or a daemon. An existing buildable libjpeg may be considered only if measurement establishes a worthwhile size/speed benefit; modern ARM NEON acceleration does not apply to ARMv5.

Upstream primary references: [stb decoder and limits](https://github.com/nothings/stb/blob/master/stb_image.h), [stb resizing implementation](https://github.com/nothings/stb/blob/master/stb_image_resize2.h), [libjpeg-turbo usage](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/doc/usage.txt), [SIMD coverage](https://libjpeg-turbo.org/About/SIMDCoverage). stb offers MIT/public-domain licensing; preserve the selected license and pinned provenance. Reusing Jive modifications also retains the project's existing applicable source/license obligations.

Initial proposed limits, subject to hardware tests:

| Resource | Proposed bound / rationale |
| --- | --- |
| Source download | 1 MiB, including decompressed transfer body; reject oversize before buffering more |
| Source raster | Axis ≤2048; total ≤1,048,576 pixels; validate before decode |
| Output | 1–512 pixels per axis; total ≤262,144 pixels; compatibility tests must cover small Jive icons |
| Pixel storage | Source RGBA at pixel cap = 4 MiB; 320² destination = 0.39 MiB, 512² = 1 MiB |
| Raster allocation budget | Start at 12 MiB total helper allocations, with graceful decode failure; measure before adoption |
| Process limits | Existing SVG 32 MiB address-space/5 CPU seconds; raster target 24–32 MiB address-space plus allocation cap; parent worker currently 40 MiB AS |
| Execution | One active normalization; bounded queue (e.g. eight descriptors), at least four coalesced same-key waiters; no parallel decoders |
| Time | Existing eight-second fetch deadline and 20-second overall worker deadline; enforce CPU/wall deadlines and reap children |
| Persistent cache | Shared 20 MiB, 256 entries, seven-day TTL |

These are **proposals, not measured safety guarantees**. `RLIMIT_AS` is virtual address space, not a reservation or a total-system RSS limit. The forked worker inherits the parent address space, PNG buffers can be duplicated across IPC/Mongoose send queues, and the proxy has its own memory. Low-memory admission control must account for the whole pipeline, not just the decoder. Old kernels may lack MemAvailable: verify conservative usable-memory accounting rather than assuming that field exists.

Expected additional stripped raster executable: roughly **100–250 KiB planning estimate**, to be measured with the actual ARMv5 toolchain. Ordinary helper working memory is expected in the low single-digit MiB, with worst-case admitted work nearer the allocation cap. No raster timing measurements exist yet. A sub-300 ms small-raster conversion is a performance target, not a prediction. Soft-float filtering may miss it substantially.

Earlier SVG validation (not performed as part of this research) measured a simple 64² conversion around 102 ms and a particular 320² external SVG miss around 1.44 s including download on the Radio. Host/QEMU numbers do not establish ARM hardware raster timing. The old kernel reported unusable RSS for that experiment, so no reliable hardware peak-memory claim is available.

### Principal OOM/audio risks

- `ArtworkCache.lua:23` already permits 8 MiB of compressed artwork in RAM. Decoded weak surfaces are not counted in that budget; live icons can retain them. Four full-size Jive decodes can greatly exceed the compressed byte count.
- A 4096² RGBA source needs 64 MiB for pixels alone. Download size is not a decoded-memory bound. Limit before allocating.
- Source plus decoded pixels plus destination plus encoder buffers, child/parent result copies, network queues and sbproxy can overlap. One worker is necessary but not sufficient.
- Progressive JPEG, malformed PNG/SVG complexity and allocator fragmentation require explicit failure tests. SVG dimensions alone do not bound path complexity.
- ARMv5 has limited CPU and no NEON. CPU-intensive filters, PNG compression and simultaneous audio decoding can cause underruns. Use inexpensive filtering/encoding, low-priority workers where supported and test during real playback. A timeout is damage containment, not an audio-quality guarantee.
- Cache writes consume NAND cycles; write only on successful bounded misses, not hits/polls. No persistent request history.

## 7. Smallest maintainable integration

1. Add one shared Base Lua artwork URL service, using the configured local web port and common normalization policy. It returns a short-lived-use local URL, not fetched data. Preserve the existing SVG-specific service contract.
2. Extend the existing source-verified atomic Jive patch for the external branch of `SlimServer.fetchArtwork`. Leave LMS cover IDs, relative paths and normal server ownership unchanged. Handle trusted prewrapped local artwork without nested proxying.
3. Add a similarly small verified adapter in `ImageViewer.requestImage` for original external HTTP **and HTTPS** sources, before its broken relative-path classification. Preserve viewer-specific rotation/crop semantics explicitly, or document unsupported transformations; contain is not a silent substitute for requested fullscreen crop/rotation.
4. Repair in-flight marker cleanup on transport/decode failure, discard invalid compressed data, and use bounded retry/backoff. Account for Base busy responses so one transient 503 does not permanently blank an icon.
5. Preserve backups, exact anchor checks, version markers and safe reapplication after firmware upgrades. Unknown source layouts fail safely and produce a local diagnostic, not a half-applied patch.

Do **not** change `jnt:getSNHostname()`, globally redirect DNS, or blindly rewrite every SocketHttp request. The hostname has unrelated account/discovery consumers. A generic RequestHttp interceptor alone also misses ImageViewer's HTTPS misclassification before the request exists. Two small artwork adapters sharing one service are narrower and more complete than per-applet patches or a global networking hook.

Central interception covers users of these Jive paths, **not all arbitrary applet downloads**. Direct legacy Spotify artwork/file loaders can adopt the common service later without being required for the initial fix. Existing PNG/JPEG playback and audio URLs must remain unaffected. Metadata artwork URL limits (Base's 384-byte field including terminator) are a separate compatibility concern: generate the expanded local proxy URL in Jive after metadata delivery, rather than inflating it before the field limit.

## 8. Implementation sequence and acceptance tests

1. Freeze firmware source fixtures and capture current routing/cache behavior; inventory small icon sizes, rotation/crop requirements and configured web ports. Test patch anchors against original, already-patched and newer firmware copies.
2. Prototype the local-file raster helper on host and ARMv5. Test PNG/JPEG baseline/progressive, alpha, grayscale, palette, orientation policy, aspect ratio, tiny dimensions, truncated/malformed inputs, decompression bombs, allocation failure and exact pixel/byte limits. Confirm no network/TLS dependency and meaningful exit statuses.
3. Refactor only enough of the existing image worker/cache to share it; add the generic route and optional alias. Test HTTP/HTTPS redirects including relative Location, cross-scheme transitions, redirect loops/global budget, DNS rebinding/private redirects and unsupported ports. Retain proxy guard capability checks and TLS policy.
4. Test cache hits with origin offline and renderer disabled; one miss must create one atomic result, coalesced requests one job. Test expiry, revision changes, signed query keys, size/count eviction, full/read-only UBIFS, no extended storage, worker crash, restart cleanup, slow origin/client disconnect, queue exhaustion and no zombies.
5. Implement the shared Lua service and two verified Jive adapters; test original public PNG/JPEG/SVG, already wrapped HTTPS/SVG routes, legacy wrapper unwrapping, encoded query characters, cover IDs and LMS-relative paths. No request may reach the retired proxy. Verify retry after a failed miss, without polling storms or permanently retained in-flight markers.
6. Regression-test Now Playing/current/next, menus, popup icons, ImageViewer, native Radio/Spotify metadata, browser image authorization and independent audio/proxy behavior. Unauthenticated remote requests must not bypass management protections or become an open relay.
7. Device acceptance, explicitly authorized separately: measure parent/proxy/helper peaks and end-to-end latency at supported sizes during playback; stress repeated unique misses, multiple clients, low memory and full cache. Monitor OOM, audio underruns, UI responsiveness and NAND activity. Confirm no child/decode on hits. Choose final budgets from these results, not desktop benchmarks.

Suggested effort: **4–7 engineering days plus device-validation time**, assuming decoder/toolchain compatibility and patch fixtures cooperate. This is a planning estimate; hardening image decoders and low-memory behavior can extend it.

## 9. Remaining uncertainties and decisions

- Exact transformation semantics of the retired Logitech service are unknown; support a deliberately documented subset.
- No current raster helper binary-size, ARMv5 CPU or trustworthy hardware peak-RSS measurements exist.
- Newer firmware patch layouts, decoder feature coverage and exact small-icon/crop/rotation requirements need fixtures/device validation.
- Initial no-UBIFS behavior needs an explicit product decision before adding a RAM-only fallback.
- Determine whether EXIF orientation is required. If supported, bound parsing and include orientation policy in the cache key; do not silently change behavior across cache versions.
- Artwork providers can return HTML, misleading MIME, expiring signed URLs and changing content. Use bounded byte sniffing, controlled errors and TTL, not extensions alone.
- Source-based transport/TLS limitations above are confirmed; no claim is made that physical OOM/audio safety or every service's artwork behavior has been validated on-device.

Only this documentation was added to tracked project content for this investigation. No replacement production code, firmware patch, deployment or service restart was performed.
