# Lightweight HLS audio: implementation and validation report

Status: implemented in source and built for ARMv5; **not deployed or tested on
physical Radio hardware**. Measurements below are from the ARM build, ARM
emulation, or workstation checks, as explicitly identified.

## 1. Architecture and decisions

The existing `sbproxy` already has libcurl/wolfSSL, loopback-only listening,
bounded worker threads, redirect handling, and opt-in public-address socket
validation. HLS belongs in a small internal module of that process, not in a
second daemon or in Jive's main loop. No new transport/TLS library, transcoder,
decoder, multimedia framework, segment cache or persistent database is added.

```text
Any Radio directory / saved station
              |
    shared RadioFeeds playlist resolver
       | ordinary M3U/PLS -> existing continuous-stream path
       | HLS markers -> preserve original station + hls=true
       v
    UrlTransport.hlsUrl
       |
       v
  existing loopback sbproxy worker
       | HEAD: inspect first valid frame, report actual codec
       | GET: bounded manifest + sequential streaming downloads
       |       raw ADTS/MP3 OR small MPEG-TS -> PES -> audio extractor
       v
  complete, configuration-checked AAC/MP3 frames over ordinary HTTP
       |
       v
  existing SqueezePlay decoder -> unchanged audio output / Now Playing
```

Radio Browser HLS flags, filename-detected playlists and MIME-only playlist
responses converge on the same Lua resolver. `.m3u8` is **not** treated as proof
of HLS: ordinary M3U8 files still resolve to their direct stream. The native
module parses HLS structure; Lua never tries to concatenate segment URIs.

The design follows the playlist/client model in the primary specification,
[RFC 8216](https://www.rfc-editor.org/rfc/rfc8216), while deliberately rejecting
the unsupported features listed below.

## 2. Local interface and transport

```text
GET  http://127.0.0.1:8765/hls/http/<host>/<manifest-path-and-query>
GET  http://127.0.0.1:8765/hls/https/<host>/<manifest-path-and-query>
HEAD same URL
```

HEAD is a bounded **upstream GET probe**, not an assumption about a filename or
directory codec. It parses enough actual audio to return `audio/aac` or
`audio/mpeg`, then aborts further input. Radio selects Jive's ADTS AAC or MP3
decoder accordingly. GET returns a close-delimited continuous stream with no
transcoding or whole-segment buffering. HEAD and GET independently validate the
current stream. Radio supplies `X-SB-HLS-Codec: aac` or `mp3` on GET after probing;
the native relay rejects an intervening codec change before emitting audio. The
optional lock header is not forwarded upstream; HEAD always detects the codec.

No browser session is needed: this is an internal loopback interface. It is not
exposed on the management webserver's LAN port. All HLS source and redirect
connections use the existing actual-socket public-IPv4/ports-80-or-443 guard.
URL validation rejects credentials, fragments, unsafe schemes and oversized
references. The separate test executable alone permits explicit fixture ports.

Curl performs HTTP/HTTPS and cross-scheme redirects, with a three-redirect limit
per transfer. Effective playlist URLs become URI bases, using curl's URL API for
relative paths, query strings and dot segments. HLS does not forward caller
Authorization, Cookie or arbitrary headers. TLS compatibility settings are
unchanged: the existing proxy does not authenticate certificates/hostnames.
This limitation must not be mistaken for authenticated secure streaming.

`/health` advertises `X-SB-HLS: aac-mp3-ts`. Older Base versions return an error
for the new route; Radio then fails the probe without passing raw HLS data to a
decoder. Base and Radio changes must be installed together for this feature.

## 3. Supported playlist and audio features

- Media and master playlists; EXTINF, TARGETDURATION, MEDIA-SEQUENCE and ENDLIST.
- Absolute and relative HTTP/HTTPS playlist/segment URIs and playlist redirects.
- Deterministic master selection: compatible audio, AAC preferred to MP3, then
  lowest advertised bandwidth, then input order. No adaptive switching.
- EXT-X-MEDIA AUDIO renditions matched to a variant's AUDIO group; DEFAULT=YES
  preferred, otherwise first listed. Rendition-only masters are also accepted.
- Live refresh at half target duration, minimum 500 ms, only while requested.
  Initial live playback starts three listed segments behind the edge; VOD starts
  from the first segment. Sequence numbers prevent duplicate segment playback.
- Sliding-window expiry skips missing live material and resets incomplete framing
  before continuing. A backward sequence reset terminates rather than replaying
  old audio. Stale/stalled playlists terminate with diagnostics.
- Sequential, incremental segment downloads. An interrupted download retries at
  the exact received byte offset; HTTP 206 and matching Content-Range are required
  before resumed data is accepted. If Range is ignored, the stream stops rather
  than duplicating already-emitted audio. Segment URIs must identify immutable
  content, as HLS normally requires; no strong-ETag identity check is implemented.
- ADTS AAC and MPEG Layer III (MP3) frames, detected from bytes rather than names.
  Complete frame boundaries are preserved across transport chunks and segments.
  Visible codec configuration changes terminate the current response.
- MPEG-TS 188-byte packets: validated PAT/PMT CRCs, bounded section/PES assembly,
  audio PID selection, adaptation/continuity checks, and elementary extraction.
  Non-audio PIDs are ignored. TS packets are never sent to the AAC decoder.
- Leading/in-band ID3v2 tags at elementary frame boundaries are safely skipped,
  including tags larger than the framing buffer. Embedded artwork is not stored.

## 4. Unsupported features and limitations

- Encryption/DRM, AES-128 and SAMPLE-AES; keys are never downloaded.
- fMP4/CMAF initialization maps, `.m4s` containers, byte-range media, explicit
  discontinuities, gaps and I-frame-only playlists.
- Video playback, adaptive bitrate switching, alternate-audio language selection,
  full low-latency HLS, multi-section program tables and general MPEG demuxing.
- AAC LATM, channel-config-zero/PCE ADTS, multi-raw-block ADTS frames, free-format
  MP3, MPEG Layer I/II, and unsupported elementary codecs. Header-visible AAC/MP3
  configuration is checked; implicit payload-only codec changes are not decoded
  or interpreted by this relay.
- A segment lost after partial output cannot always be recovered. If a safe byte
  resume is unavailable, close/reconnect is preferable to corrupt/duplicate audio.
- Unsupported input before audio begins returns an informative 415 response and
  `X-SB-HLS-Error`. Fetch/allocation/deadline failures use 502/503/504 where
  appropriate. After HTTP 200 has begun, errors close the stream and are logged;
  HTTP status cannot be changed midway through a response.
- A new HEAD probe can return 503 if both bounded HLS slots are occupied. Existing
  playback is not preempted. Client disconnects release those slots promptly.

No claim is made that every public HLS station or every firmware AAC decoder is
compatible. The implemented subset covers the inspected BBC MPEG-TS/HE-AAC case
without a station-specific branch.

## 5. Timed metadata assessment

Timed ID3 now has a bounded native parser, per-stream loopback event/artwork
endpoints and a generation-aware Lua consumer. Lua retains all Now Playing
ownership. HEAD never publishes events; audio still excludes ID3 bytes.
See [HLS_TIMED_METADATA.md](HLS_TIMED_METADATA.md) for the current resource
budget, timing, transport support, tests and limitations. The original baseline
memory figures elsewhere in this report exclude this extension.

## 6. Limits, cleanup and security

| Resource | Limit |
| --- | --- |
| Source/resolved URL | 4,095 bytes |
| Manifest | 65,536 bytes; binary/NUL input rejected |
| Segments per manifest | 128 |
| Variants / rendition entries | 32 each |
| Master nesting | 4 levels; cycles rejected |
| Segment size | 16 MiB streamed, not accumulated |
| Target/segment duration | At most 120 seconds; target consistency checked |
| Curl transfer buffer | 8 KiB default; `SB_HLS_BUFFER_BYTES` 1–16 KiB |
| Elementary frame buffer | 8 KiB |
| TS packet / PSI / PES header | 188 / 1,024 / 264 bytes |
| Skipped ID3 tag | At most 1 MiB; body not buffered |
| Active HLS requests | 2, within the existing 8 proxy workers |
| Connect / manifest transfer | 3 / 8 seconds |
| Segment transfer | max(20 seconds, declared duration + 15 seconds) |
| Manifest attempts / segment attempts | 2 / 3, with bounded backoff |
| Startup deadline | 20 seconds without a valid first frame |
| Audio starvation | max(30 seconds, 3 × target duration) |
| Downstream stalled write | 15 seconds without progress |

Incomplete-frame and demux buffers are released at termination. All curl
transfers run synchronously inside their existing worker, with downstream-aware
progress cancellation. Waiting for a live refresh uses cancellation-aware poll,
not a permanent timer. Slow downstream writes use nonblocking send/poll with a
deadline, not an unbounded blocking write. No work occurs without a client.

Native logs deliberately omit HLS source URLs, including signed CDN paths.
For temporary variant-selection troubleshooting, start the proxy with
`SB_HLS_DIAGNOSTICS=1`. Bounded `[hls-diag]` entries classify CODECS, parsed
bandwidth and rejection/selection decisions without logging raw attributes or
URIs. Disabled by default; no per-packet diagnostics or persistent setting.
Temporary Lua request-query diagnostics used during device troubleshooting have
been removed. An unchanged media playlist is normal at half-target polling
cadence and no longer produces a speculative underrun warning. Actual stalled
playlist and audio-idle deadlines remain enforced and reported.
Initialization, selected variant depth/codec, fetch errors, live expiry, stalls
and termination are logged without per-packet noise. Termination reports
segments, frames, output bytes, retries, skipped segments and elementary ID3 tags.

## 7. Footprint, memory and CPU

Measured with the existing ARMv5 musl SDK, `-Os`, static linking and stripping:

| sbproxy | File bytes |
| --- | ---: |
| Before HLS, 0.4.1 development baseline | 837,772 |
| With HLS | 854,156 |
| Increase | **16,384 (16 KiB)** |

The smaller-than-target increase reuses the curl/TLS code already linked into
the executable. No new shared library or helper executable is needed.

ARM-layout measurements reported by the emulated binary:

- Fixed relay heap, including the maximum manifest: **76,497 bytes/session**.
- Media playlist view: 5,144 bytes, on the stack and included in the next figure.
- SDK `-fstack-usage`: `run` 33,888 bytes, `hls_parse` 9,000 bytes, relay entry
  360 bytes. The principal module call-chain bound is approximately **43 KiB**,
  excluding pre-existing proxy/curl stack use.
- Add the configured 8 KiB curl read buffer: roughly **125 KiB** of fixed module
  heap/buffer plus principal stack working storage per active session. This is
  an accounting estimate from compiled layouts, **not physical RSS**.

The existing 256 KiB worker stack reservation, wolfSSL handshake state, curl
connection/DNS allocations, socket buffers and Jive decoder buffers are not part
of that fixed-module figure. Two simultaneous requests can temporarily overlap
during station probing/switching. Actual Radio peak RSS and OOM margin must still
be measured while audio is playing.

CPU work is copying, frame/header parsing, small PSI CRC checks and existing TLS;
there is no audio decode/encode in this module. No representative on-device CPU
percentage is available. In one workstation ARM-emulated BBC smoke test, first
headers took about 0.22 seconds for HEAD and 0.13 seconds for GET. These include
network/emulation and are **not hardware CPU or latency guarantees**.

## 8. Tests and build results

Passed:

- Native parser/framing tests on ARM emulation and sanitizer-backed host builds.
  Live/master parsing, audio rendition selection, relative URLs, format rejection,
  manifest limits, malformed tags, sequence overflow, ID3 stripping, per-byte
  framing, codec configuration changes, plus a 2,000-case deterministic malformed
  audio/TS input sweep.
- Host AddressSanitizer/UndefinedBehaviorSanitizer HTTP integration and ARM-QEMU
  HTTP integration. Synthetic AAC/MP3 output is byte-exact; tests do not pretend
  synthetic frame payloads validate an audio decoder.
- MPEG-TS PAT/PMT/PES extraction, split PES headers, bad adaptation bounds and CRC;
  frame splits across segment boundaries; HTTP/HTTPS redirects and cross-scheme
  segment fetches; ID3; exact range resume; ignored-range nonduplication; 403/404,
  transient failures and slow transfers; live sliding windows and expiry; backward
  sequences; stalled streams; incompatible config; unsupported containers;
  cancellation/repeated probes and concurrent-slot exhaustion/recovery.
- Shipped-proxy private-address rejection for HLS and existing guarded HTTP,
  HTTPS and artwork interfaces. Test-origin allowances are not in production.
- Existing artwork streaming, SVG renderer/image HTTP, remote-control HTTP, pairing,
  station-management HTTP, artwork patch, native Now Playing API/Comet tests.
- Lua routing, playlist preservation, MIME discovery, actual-codec probing,
  unsupported/timeout probe rejection, Radio Browser cache HLS flag preservation,
  plus existing URL transport, logo, Now Playing and user-station tests.
- `scripts/build-arm.sh` built all six static ARM EABI5 binaries. The proxy was
  rebuilt again after final HLS edits. The existing NanoSVG signedness warning is
  unrelated; the new HLS modules compile without warnings.

A read-only **external-network, ARM-emulated** smoke test used the real BBC Radio
nan Gàidheal MPEG-TS manifest. The relay returned ADTS AAC, not TS packets. An
optional existing workstation probe identified HE-AAC, 48 kHz, stereo; a
workstation decode of the complete-frame sample succeeded. That diagnostic tool
is not included in the product or required by its build or automated tests.

Full CTest was not run: CMake is unavailable in the WSL test environment. Relevant
tests were run directly; the committed CMake registration supports configured
host builds. No physical Radio deployment, restart, listening test, long-duration
audio stability test or device RAM/CPU measurement was performed.

## 9. Files changed for HLS

Standalone Base:

- `native/sbproxy/hls.h`: bounded internal interfaces and state.
- `hls_playlist.c`: manifests, deterministic selection and URI resolution.
- `hls_audio.c`: elementary framing, ID3 skipping and TS audio demux.
- `hls_relay.c`: lifecycle, transfer/resume, live refresh and local HTTP output.
- `native/sbproxy/sbproxy.c`: guarded loopback route and capability header.
- `native/sbproxy/CMakeLists.txt`, `scripts/build-arm.sh`: build integration.
- `tests/images/test_hls.c`, `test_hls_http.py`, `test_proxy_guard.py`,
  `tests/CMakeLists.txt`: new tests and security regression coverage.

Standalone Radio:

- `RadioFeedsClient.lua`: distinguish HLS from ordinary playlists, retain source
  URL/metadata, mark HLS instead of rejecting it; shared forced MIME resolution.
- `UrlTransport.lua`: shared idempotent local HLS URL builder.
- `StreamPlayer.lua`: local transport, codec/MIME HEAD probing, fail-safe errors.
- `StandaloneRadioApplet.lua`: all directories share playlist handling; do not
  recursively resolve an already identified HLS station.
- `RadioBrowser.lua`: supported HLS audio entries and versioned cache flag.
- `PresetStore.lua`: persist HLS identity with the existing station data.
- `strings.txt`, README, playlist documentation and Lua tests.

Earlier uncommitted artwork, logo-import and playlist-count fixes remain intact;
they are not additional HLS dependencies or newly introduced services.

## 10. Reproduction and physical-device acceptance

Firmware 7.7.3 `RequestHttp:t_getRequestString()` appends query/params only
for GET, dropping them from HEAD probes. Standalone Radio overrides serialization
on its individual HEAD probe instances to retain the original encoded target
(excluding fragments). The shared firmware remains unchanged. A regression test
models the legacy serializer and verifies BBC station/bitrate queries, percent
escapes, path parameters and fragment exclusion at the serialized request boundary.

Initial playlist discovery in Lua also uses streaming reads capped at 64 KiB,
rejects oversized Content-Length values, and cancels obsolete playlist requests
when stations change. The bounded Lua chunks/concatenation are additional to the
native relay memory figures; ordinary directory/search requests are unchanged.
Regression tests cover oversized bodies, oversized headers and late callbacks
after cancellation.

Build normally: `sh scripts/build-arm.sh`. In a configured host CMake build, run
CTest targets `hls_native`, `hls_streaming`, `image_proxy_guard` and the existing
regressions. The HTTP fixture test uses the separate `sbproxy_test` executable;
its two explicitly permitted local origin ports are compile-time test-only.
For ARM emulation pass `qemu-arm <test-binary>` to the Python test, and run the
ARM-compiled native test under qemu-arm. Lua tests use Lua 5.1 and Jive's existing
LuaSocket URI resolver.

After separately approving a deployment of both projects:

1. Confirm the proxy health capability and both HLS route methods. Try a direct
   supported AAC HLS source, a MP3 packed-audio source and an ordinary M3U8 playlist.
2. Use fresh RadioFeeds BBC entries, including Radio nan Gàidheal AAC(HLS), Radio
   Scotland FM and MW. One example used in the workstation smoke test is:
   `https://as-hls-ww-live.akamaized.net/pool_01935182/live/ww/bbc_radio_nan_gaidheal/bbc_radio_nan_gaidheal.isml/bbc_radio_nan_gaidheal-audio%3d96000.norewind.m3u8`.
   Provider paths/availability may change; prefer the current directory response.
3. Confirm Jive receives `audio/aac`/ADTS or `audio/mpeg`, correct decoder selection,
   uninterrupted listening, unchanged station names/logos and functioning controls.
4. Switch repeatedly between HLS, direct MP3/AAC and another HLS station. Test
   cancellation during lookup, TLS connect, download, refresh wait and playback.
5. Test network loss/recovery, expired segments and unsupported encrypted/fMP4
   input. Check fixed diagnostics, bounded retries and absence of corrupt audio,
   leaked workers, persistent downloads or unintended reconnect storms.
6. Record module counters, `/proc/meminfo`, meaningful process RSS and CPU while
   listening for at least 30 minutes. Save diagnostics on the workstation rather
   than adding periodic flash logging. Record any firmware RSS-reporting limits.
7. Monitor OOM, audio underruns, power behavior and Jive restarts. Measure overlap
   during HEAD/GET switching, not just steady state. No hardware-success claim
   should be made until this procedure actually passes.
