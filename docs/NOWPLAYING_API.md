# StandaloneBase Now Playing API

## Overview

Version 1 is a loopback-only JSON API on the existing StandaloneBase HTTP port
(normally `127.0.0.1:9000`). An audio application owns playback; StandaloneBase
owns only the state presented through LMS protocols. Runtime state and session
tokens are deliberately not persisted.

Only one source is active. A successful claim preempts the previous source,
clears its command queue and increments the generation. An old token receives
`409 Conflict`. Release and lease expiry return the LMS player to an empty,
stopped state. The default lease is 120 seconds; update and heartbeat renew it.

Tokens are 192 random bits read from `/dev/urandom`. They are ownership handles,
not network authentication. Security depends on all listeners remaining bound
to loopback.

## Endpoints

All responses include JSON. Request bodies are limited to 16 KiB by this API
and parsed as typed JSON rather than by substring matching.

### `POST /api/nowplaying/claim`

```json
{"source":"spotify","name":"Spotify Connect","capabilities":{"play":true,"pause":true,"stop":true,"next":true,"previous":true,"seek":true,"volume":false}}
```

Returns `201` with `session_id`, monotonically increasing `generation`, and
`active`. Claim is an explicit takeover; background metadata must never claim.

### `POST /api/nowplaying/update`

```json
{"session_id":"TOKEN","state":"playing","position_ms":43000,"live":false,"track":{"id":"spotify:track:123","title":"Money for Nothing","artist":"Dire Straits","album":"Brothers in Arms","duration_ms":506000,"artwork_url":"http://127.0.0.1:9000/api/nowplaying/artwork/example"}}
```

States are `playing`, `paused`, `stopped`, `buffering`, and `error`. Buffering
and error map to LMS `stop`; no unsupported LMS mode is invented. Position and
duration are milliseconds. StandaloneBase extrapolates a playing position with
`CLOCK_MONOTONIC`, freezes paused/stopped positions, and clamps to duration.

For live radio set `live:true`, omit duration, use a stable station ID and put
the station name in `track.station`. In playerstatus the native three-line
layout receives title as `track`, artist as `artist`, and—when no real album
was supplied—the station name as `album`. This is the same Internet Radio
special case used by LMS; it is not a custom field. ICY title/artist changes do not change the
playlist timestamp unless `track.id` changes. Live status uses LMS `remote:1`,
`current_title`, unknown duration, and the standard one-item loop. Music uses
`remote:0` and normal duration semantics.

Optional bounded UTF-8 strings are: `album_artist`, `content_type`, and
`artwork_url`. Invalid types and malformed JSON receive `400`.

### Heartbeat, release and status

```text
POST /api/nowplaying/heartbeat  {"session_id":"TOKEN"}
POST /api/nowplaying/release    {"session_id":"TOKEN"}
GET  /api/nowplaying
```

The public status never exposes the token. Applications must reclaim and
republish authoritative state after an sbbase restart.

### Commands

```text
GET  /api/nowplaying/commands?session_id=TOKEN
POST /api/nowplaying/commands/ack {"session_id":"TOKEN","id":12}
```

The in-memory FIFO is bounded to 32 entries. Acknowledging ID N removes commands
through N. A takeover discards old commands. Recognized CometD requests queue
play, pause, stop, next and previous without changing confirmed playback state.
The application performs the action and then confirms it with `update`.

Seek and volume fields exist in the capability/data model, but their exact
SqueezePlay command forms are hardware-unverified and are not routed yet.
Hardware volume behavior remains unchanged.

## LMS mapping and notifications

`playing` maps to `play`, `paused` to `pause`, and all other internal states to
`stop`. Initial queries and `/slim/subscribe` responses are generated from the
same central state. Changes are sent only to streams subscribed for the active
player. Reconnection, stream replacement and unsubscribe retain the physically
validated Phase 1 behavior. `serverstatus.isplaying` follows confirmed state.

## Artwork URLs

`track.artwork_url` accepts a UTF-8 string of at most 383 bytes. StandaloneBase
preserves the complete value, including query parameters and percent encoding,
and publishes it as `item_loop[0].icon`. That is the standard LMS field used
for remote artwork; stock Now Playing checks `icon-id` first and then `icon`.
An empty string removes artwork. Artwork-only changes publish a complete
playerstatus snapshot without changing track identity or `playlist_timestamp`;
an identical URL is suppressed as a duplicate.

StandaloneBase does not fetch, decode, resize, cache, or serve artwork.
SBHttpsProxy URLs have the form
`http://127.0.0.1:8765/https/<host>/<path>` and are not rewritten.

Stock SqueezePlay directly fetches only selected private IPv4 artwork URLs and
otherwise routes them through the retired mysqueezebox image-resizer. The
StandaloneBase installer therefore applies a source-verified compatibility
patch to `jive/slim/SlimServer.lua`: `127.0.0.1` and `localhost` join the
existing direct-fetch exceptions. It does not change public-URL handling.

The patch is idempotent and preserves the original once as
`SlimServer.lua.pre-standalonebase`. Native loopback support and firmware with
no historical `/public/imageproxy` path are recognized as no-op cases. Only the
complete known legacy `jnt:getSNHostname()` image-proxy structure is patched;
alternative or ambiguous proxy implementations are refused. Because this Lua module is
loaded early, SqueezePlay must restart after a newly applied patch. The package
deployment tool patches before its normal restart; an Applet Installer/factory
installation may require one additional SqueezePlay or Radio restart after the
first StandaloneBase initialization. Successful image decoding remains subject
to the stock firmware's supported image formats and memory limits.

## Resource limits and errors

There is one active session, 32 commands, fixed-size metadata, no worker thread,
and no runtime flash writes. Typical errors are `400` invalid input, `404`
unknown endpoint, `405` method, `409` inactive/obsolete token, `413` oversized
body and `503` unavailable entropy. API version is the integer `api_version:1`;
version 1 additions will remain backward compatible.
