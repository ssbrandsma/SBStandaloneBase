# HLS timed ID3 metadata and artwork

## Architecture and ownership

The working HLS relay remains the only downloader/demultiplexer. No decoder,
transcoder, TLS dependency, daemon, permanent thread or metadata provider was
added. ID3 is removed before encoded audio reaches SqueezePlay. The native relay
never claims or updates a generic Now Playing session.

```text
HTTP/HTTPS HLS (existing curl/TLS and public-destination policy)
    -> packed AAC/MP3 framing OR TS audio + metadata PID demux
    -> bounded ID3 parser -> per-stream RAM event queue / artwork
                                      |
                         sbproxy loopback GET endpoints
                                      |
                  HlsMetadata.lua + decode:status().elapsed
                                      |
            existing StandaloneBaseNowPlaying client / ownership
                                      |
                       existing Jive Now Playing artwork
```

Phase-0 inspection covered `hls_audio.c`, `hls_relay.c`, proxy request routing,
`StreamPlayer.lua` ICY hooks and decoder setup, the Lua Now Playing client,
`Artwork.lua`, `UrlTransport.lua`, and SqueezePlay's `decode.c` status binding.
The binding reports `elapsed` in milliseconds from output/elapsed samples, with
the firmware's existing synchronization adjustment. This is more useful than
download time. The existing Now Playing session token stays entirely in its
original owner/client.

## Parser and transport

`hls_id3.c` incrementally handles ID3v2.3/v2.4 tag/frame headers, padding,
version-specific big-endian versus synchsafe sizes, extended header lengths,
v2.3 tag unsynchronization and v2.4 frame/tag unsynchronization. Unknown frames
are skipped. Compressed, encrypted, grouped and data-length-indicated frames are
not decoded. Text is bounded to 255 UTF-8 bytes, ending at a scalar boundary, to
match the existing Now Playing API. Latin-1, UTF-8, BOM UTF-16 and UTF-16BE are
converted, including surrogate pairs and replacement characters for invalid text.

- `TIT2`, `TPE1`, `TALB`: structured title, artist and album.
- `APIC`: bounded PNG/JPEG cover or the standardized `-->` linked-image form.
- `PRIV`: Apple's 33-bit packed-audio transport timestamp, when present.
- `TXXX`/`WXXX`: recognized as non-cover frames and skipped. No arbitrary
  description/value is interpreted as an artwork API or provider-specific field.
- ID3v2.2 still strips safely through existing framing, without field extraction.

Packed ID3 works before or between complete elementary frames and across network
chunks/segments. Separate TS metadata uses PMT stream type `0x15`, or `0x06`
with an `ID3 ` registration descriptor. Metadata PES uses private stream `0xbd`;
its header can span TS packets. The parser can scan through a short metadata
access-unit prefix before ID3. Loss/invalid framing on the metadata PID discards
its partial tag without aborting valid audio. Existing audio PSI/PES/CRC and
continuity checks are preserved. Multi-PES assembly of one ID3 tag is not
supported: a new metadata PES start resets an unfinished tag.

Malformed individual ID3 frames are discarded without failing playback. The
existing elementary-stream guard still rejects invalid ID3 envelopes, impossible
synchsafe sizes and tags exceeding its 1 MiB limit. It cannot safely infer where
audio begins after a deliberately false tag length; it does not guess by scanning
arbitrary embedded image bytes for apparent AAC syncwords. This remains a strict
framing limitation, not a claim that every corrupt envelope is recoverable.

## Identity, endpoints and lifecycle

Every GET relay may allocate a random 128-bit session identity from `/dev/urandom`.
It is returned in `X-SB-HLS-Session`; HEAD creates no metadata session or parser.
Failure to allocate metadata resources does not prevent audio playback. The
existing loopback listener protects these read-only endpoints from LAN access;
there is no browser-management bypass and no Now Playing credential in them.

- `GET /hls-meta/<32-lowercase-hex-id>?after=<revision>`: next retained event.
  Includes session, revision, output position, present-field bits, title, artist,
  album, optional artwork URL, nullable source timestamp and dropped-event count.
- No new event: `{"pending":true}`. Natural EOF adds `"finished":true`.
- `GET /hls-meta/<id>/art/<revision>`: validated embedded JPEG/PNG bytes.
- Invalid/retired identities and expired artwork revisions return 404. Malformed
  identifiers/cursors return 400. Responses are `no-store` and `nosniff`.

Two slots match the existing two-HLS-worker limit. Each holds eight ordered events;
overflow evicts oldest events and reports the gap. Slow clients cannot accumulate
unbounded native or Lua history. Equal consecutive metadata/artwork is suppressed;
timestamp-only tags neither clear track fields nor consume the queue.

Cancellation/error immediately retires identity and artwork. Natural EOF retains
events for at most 120 monotonic seconds so buffered audio can catch up. Expiry
is checked on reads/new sessions, without a timer thread; new streams can reclaim
finished slots immediately. At idle, at most two bounded finished slots may
remain allocated until the next registry operation or process exit.

The original `HlsMetadata.lua` used 500 ms requests. This has been replaced by
bounded long polling and independent one-shot decoder scheduling; see
[HLS_METADATA_DELIVERY.md](HLS_METADATA_DELIVERY.md) for the current transport,
request counts, worker limits, timeout and backward-compatibility behavior.
Lua still holds one request and one pending event; responses are capped at 8 KiB.
Generation/session/revision checks reject late or duplicate callbacks. Stop,
switch and reconnect close sockets and timers; each reconnect acquires a new
native identity. Existing ICY parsing and non-HLS playback are unchanged.

## Timing

Packed metadata is stamped at the current output-audio sample offset, using
ADTS sample-rate/1024-sample frames or MP3 version/rate/frame sample counts.
This includes frames already sent into Jive's buffers, not elapsed wall time.
The original PRIV timestamp is retained when supplied; its absence is JSON null.

Separate TS events use metadata PTS relative to the latest audio PES PTS and that
PES's output sample position. The 33-bit difference is wrap-safe. Differences
over 120 seconds are treated as a discontinuous clock and fall back to demux
ordering/sample position rather than waiting indefinitely on an invalid clock.
If no matching audio/metadata PTS exists, the sample position at tag completion
is an approximation: TS interleaving can place it before/after the true point.

Lua applies events only when decoder `elapsed` reaches their output position.
The optimized consumer schedules a local one-shot timer from remaining decoder
time instead of coupling synchronization to HTTP polling. A slow network/UI can
still add delay. No decoder elapsed value means no speculative early application.
Decoder clock drift, missing PTS, live-window resynchronization, queue overflow,
and natural-EOF retention/reclaim limit exactness. This is not sample-accurate
synchronization. Long finite downloads whose final metadata is buffered beyond
120 seconds may lose that metadata; the audio behavior is unchanged.

## Artwork and Now Playing

Embedded art is limited to 32 KiB, 1024 per axis, and 262144 pixels total.
PNG signature/IHDR/chunk bounds/IDAT/IEND or JPEG SOI/SOF/dimensions/EOI are checked;
the relay never decompresses image pixels. These are structural checks, not a
complete image decoder or authenticity check. Oversized/invalid pictures are
ignored. Artwork has bounded parser/queue/HTTP copies, not repeated downloads or
flash writes. Normal Jive artwork caching/display remains responsible for decoding.

Linked APIC URLs are restricted to bounded ASCII HTTP/HTTPS, without credentials,
fragments or control characters, then wrapped in the existing public-only
`/artwork/320/320/80/...` pipeline. Its destination/redirect guards, raster
handling and SVG renderer remain authoritative. The current route assumes the
existing web service is on port 80 for SVG fallback; custom web-port discovery
for linked APIC is not implemented. Arbitrary WXXX/TXXX URLs are never fetched.

The Lua owner publishes structured fields without splitting artist/title around
hyphens. It preserves station identity/name/logo, suppresses unchanged updates,
rejects stale generations and removes obsolete track artwork on a changed track.
Timestamp-only/unrelated tags preserve valid current fields. Missing covers use
the station logo. No external track-art search runs for timed ID3 updates.

## Footprint and verification

Measured with the project's ARMv5 static musl/curl/wolfSSL toolchain, stripped:

| Item | Bytes |
|---|---:|
| sbproxy before timed metadata | 858252 |
| sbproxy after timed metadata | 866444 |
| Executable file growth | 8192 (8 KiB) |
| One event, ARM ABI | 33824 |
| One parser, ARM ABI | 67192 |
| One eight-event registry slot, ARM ABI | 270656 |
| Slot + two parsers per active GET | 405040 |
| Relay-context growth per GET | 384 |
| Total additional heap per active GET | 405424 |
| Maximum two active GETs, added heap | 810848 |
| One concurrent metadata/artwork HTTP response copy | 33824 |

These are object/allocation sizes, not RSS. The relay's existing `working_heap`
initialization diagnostic excludes separately allocated parsers/registry slots.
Six additional short-lived HTTP workers
could add 202944 response-copy bytes while two stream workers are active. Existing
worker stacks, curl/TLS/socket buffers, allocator overhead, Lua strings and Jive
decoded-image/cache memory are separate. Registry static storage increases by
approximately 32 bytes on this build. No RSS/CPU or audio-underrun measurement
for this extension has been performed on hardware. Metadata/embedded-art handling
adds no NAND writes; linked artwork retains the existing proxy/renderer cache policy.

Tests added/extended:

- `test_hls_id3.c`: v2.3/v2.4, byte-at-a-time parsing, Latin-1/UTF-8/UTF-16,
  surrogate pairs, padding, extended headers, unsync, malformed lengths,
  timestamp PRIV, JPEG APIC, linked APIC, invalid/oversized art, unknown frames,
  deterministic malformed-input sweep. Runs natively under ASan/UBSan and ARM/QEMU.
- `test_hls_http.py`: packed tags + PNG APIC, two track changes and sample timing,
  separate metadata PID/PTS, unchanged encoded audio, eight-event eviction,
  stale IDs/artwork, cancellation, HEAD isolation and concurrent session identities,
  plus the existing HTTP/HTTPS, retries, expiry, framing and capacity regressions.
- `hls-metadata-test.lua`: decoder timing, absent clock, no idle polling,
  duplicate events, generation changes, wrong sessions and cancellation.
- `nowplaying-client-test.lua`: structured fields, duplicate/timestamp suppression,
  stale-generation rejection, artwork replacement and station-logo fallback.
- Existing HLS probe, URL transport, LogoCache, RadioFeeds, navigation and station
  loader tests; complete Radio applet Lua syntax checks.

Build: `sh scripts/build-arm.sh`. Standard CMake/CTest adds `hls_id3`; run that
alongside `hls_native`, `hls_streaming` and `image_proxy_guard`. Python integration
accepts `qemu-arm <sbproxy_test>` for target emulation. Lua lifecycle fixtures use
Lua 5.1, matching Jive's module environments. No new dependency is required.

## Real streams, deployment and remaining validation

On 2026-10-17, workstation sampling of one current 48-kbit HLS segment each from
BBC Radio nan Gàidheal, BBC Radio Scotland FM and BBC World Service found no ID3
tags (42488 bytes each). This small sample does not prove their streams never
carry metadata. No real artist/title/APIC stream has been confirmed; synthetic
fixtures provide the field/artwork evidence. There is no station-specific API.

The earlier HLS audio implementation was listened to successfully on the Radio.
This timed-metadata extension has **not been deployed or tested on hardware**.
Install both projects together: new sbproxy and the Radio modules/package including
`HlsMetadata.lua`. Re-test HLS without metadata, ordinary ICY, rapid switching,
reconnect/stop, UTF-8 track changes, bounded artwork and audible timing. Measure
RSS/CPU and listen for underruns for at least 30 minutes before claiming hardware
success. No release ZIP, version bump, commit, push or deployment is implied here.

Source changes: Base `hls.h`, `hls_audio.c`, `hls_relay.c`, `sbproxy.c`, new
`hls_id3.c`, `hls_metadata.[ch]`, ARM/CMake wiring and image tests. Radio
`HlsMetadata.lua`, `StreamPlayer.lua`, `StandaloneRadioApplet.lua`,
`StandaloneBaseNowPlaying.lua`, package manifest and Lua tests/docs.

References: [ID3v2.3](https://id3.org/id3v2.3.0),
[ID3v2.4 structure](https://id3.org/id3v2.4.0-structure),
[HLS packed audio and timestamp conventions, RFC 8216](https://www.rfc-editor.org/rfc/rfc8216.html#section-3.4).
