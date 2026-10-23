# Now Playing metadata compatibility

## Protocol research (verified from upstream source)

This implementation targets the existing LMS commands, not a new Jive protocol.
Reference LMS revision: `c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7`
([source](https://github.com/LMS-Community/slimserver/tree/c85a3a8ba8a6cb81cbe6e4d9df0d3757d8468ae7)).

* `Slim/Control/Queries.pm`, `statusQuery`: ordinary status returns
  `playlist_loop`; `menu:menu` returns Jive `item_loop`. Playlist defaults are
  `gald`; menu mode selects `aAlKNcxJ`. Source metadata belongs to the track,
  whereas top-level `type` (`o`), `bitrate` (`b`), `samplerate` (`T`) and
  `samplesize` (`I`) describe the delivered playback stream.
* The same file's `%tagMap` / `_songData`: `o` type, `r` bitrate (formatted
  string), `T` samplerate (Hz), `I` samplesize (bits), `H` channels, `Q` lossless
  (0/1), `Y` replay_gain (dB), `d` duration (seconds), `a` artist, `l` album,
  `g` genre, `y` year, `t` tracknum, `i` disc, `u` url, `K` artwork_url,
  `N` remote_title, `L` info_link, `z` subtitle, `k` comment, `x` remote.
  In particular **track tag `b` is work, not bitrate**.
* `songinfoQuery`: selects the requested current track by `track_id` or `url`;
  `songinfo_loop` is a list of single-field objects, paginated over fields.
* Original Jive `SlimBrowserApplet.lua`, `showTrackOne` / `showTrack`: Now
  Playing More Info invokes `contextmenu` with `menu:track`,
  `context:playlist`, `playlist_index:0`. LMS `contextMenuQuery` delegates to
  `trackinfo items`. `Slim/Menu/TrackInfo.pm::cliQuery` supplies an XMLBrowser
  menu; `Slim/Control/XMLBrowser.pm::cliQuery` serializes non-actionable text
  as `{type:"text",text:"Label: value",style:"itemNoAction",action:"none"}`
  within `item_loop`, with `count` and `offset` for menu pagination.
* [Dark Flat Skin](https://github.com/AF-1/jivelite-darkflatskin/tree/30cd4c9f7be49c280edb5d6ac009c3c2759c8e66),
  `NowPlaying/baby/NowPlayingApplet.lua`: requests `songinfo` for library
  tracks and `status - 1 tags:IkoQrRTuyw` for remote tracks, reading source
  quality from `playlist_loop[1]`. It expects numeric negative remote IDs.

Station name is `remote_title` / current live `current_title`; current artist
and title remain track fields. A station description is **not** a track comment.
Fields with no matching standard track semantics must not be invented in LMS
responses. No UI or skin changes are required.

## Verification boundaries

The findings above are source-verified, not device-verified. Automated tests and
implementation details are recorded below as the implementation is completed.
Physical Radio tests must cover stock More Info, remote metadata transitions,
and optional Dark Flat Skin behavior; a host test cannot prove UI compatibility.

## Internal provider model

The existing claim/update/heartbeat/release API, ownership token and lease are
unchanged. The following are optional additions to `/api/nowplaying/update`.
`GET /api/nowplaying` exposes their normalized values, never the owner token.

| Location | Fields and units |
| --- | --- |
| `track.audio` | Source `codec`, integer `bitrate_kbps`, `bitrate_mode` (`CBR`/`VBR`), `sample_rate_hz`, `bit_depth`, `channels`, boolean `lossless`, numeric `replaygain_db` |
| `track.transport` | Same schema, describing audio delivered to Jive rather than its original source |
| `track` | Existing identity/title/artist/album/artwork/duration fields; optional `genre`, `comment`, `subtitle`, `url`, `info_link`, `year`, `track_number`, `disc_number` |
| root `station` | `id`, `name`, `description`, `homepage`, `country`, `language`, `genre`, `logo`, `stream_url`, optional `audio` (declared directory quality) |

Missing objects retain their previous value for a partial metadata update.
A supplied audio/station object is a **complete snapshot of that object**:
omitted attributes become unknown; `{}` or `null` clears it. Numeric zero means
unknown; `lossless:false` explicitly means lossy, whereas absence means unknown.
No codec-based lossless inference or PCM-derived lossless bitrate is performed.
ReplayGain is stored with millidecibel precision. Duration remains the existing
`track.duration_ms` and is exposed in seconds only when non-live and known.

Strings are UTF-8, bounded by the existing model: track text 255 bytes, IDs
191, URLs/artwork 383, codec 31, country/language 95. Oversized/type-invalid
updates fail atomically. An audio object accepts at most 100000 kbps,
768000 Hz, 64 bits and 32 channels; these are validation ceilings, not claims
that the Radio can decode every such format. Unknown values should be omitted.
No new decoder capabilities are introduced.

Example update (existing owner session required):

```json
{
  "session_id": "<existing-owner-session>",
  "state": "playing",
  "live": false,
  "track": {
    "id": "provider:track:123",
    "title": "Example Track",
    "artist": "Example Artist",
    "duration_ms": 180000,
    "audio": {
      "codec": "flc",
      "sample_rate_hz": 96000,
      "bit_depth": 24,
      "channels": 2,
      "lossless": true
    }
  }
}
```

## LMS responses and commands

Commands use the **existing Comet `/slim/request` command arrays** on sbbase's
HTTP listener, not a new HTTP endpoint or protocol. This change does not add a
TCP CLI listener or `/jsonrpc.js` endpoint.

* `status - 1 tags:orTIHQ` returns one `playlist_loop` item with `id:-1`,
  `title`, and only known requested source fields. Defaults are `gald`.
* `status - 10 menu:menu useContextMenu:1` retains the existing Now Playing
  `item_loop`, count, offset, artist/album, icon and playlist revisions. Its
  More action points to `contextmenu`, with the current playlist index.
* `songinfo 0 100 track_id:-1 tags:orTIHQ` returns `songinfo_loop`, one
  singleton object per field. The current opaque provider ID or known source
  URL can also select the current track. There is no synthetic music library.
  The source-verified upstream default tag set is used when tags are omitted;
  `track_id` or `url` is required. Unknown selectors return no fields.
* `contextmenu 0 100 menu:track context:playlist playlist_index:0` and
  `trackinfo items 0 100 playlist_index:0` provide the current track's bounded,
  paginated, non-actionable information rows. `track_id` and `url` selectors
  are also accepted. This is a current-track subset, not the full LMS library,
  playlist editing, rating or plugin menu system.

Example source response (standard status):

```json
{
  "playlist_tracks": 1,
  "count": 1,
  "offset": 0,
  "playlist_loop": [{
    "playlist index": 0,
    "id": -1,
    "title": "Example Track",
    "type": "flc",
    "samplerate": 96000,
    "samplesize": 24,
    "channels": 2,
    "lossless": 1
  }]
}
```

The actual response also includes ordinary player state, time and duration.
`bitrate` is a string such as `128kbps CBR` or `192kbps VBR`, not a JSON
bit counter. Its track tag is **r**; top-level playback bitrate uses **b**.
Unknown quality fields are absent. Numeric fields remain JSON numbers.
`id` and known `title` accompany track selection independently of tags.
Library-only fields (ratings, lyrics, work/composer, etc.) are not fabricated.

## Station information and More Info

Track `title`/`artist` describe current audio metadata. Station name maps to
`remote_title` (`N`), the current live title and the existing legacy
`track.station` display fallback. Station genre can supply `genre` when the
track has none; homepage supplies the optional `info_link` URL. Artwork uses
the current track image first, then a station image where appropriate.

More Info uses upstream XMLBrowser text-row structure, for example:

```json
{
  "count": 1,
  "offset": 0,
  "window": {"text": "More Info"},
  "item_loop": [{
    "type": "text",
    "style": "itemNoAction",
    "action": "none",
    "text": "Sample size (bits): 24"
  }]
}
```

Station description/country/language are additional ordinary text rows, not
invented LMS track tags. Declared station codec/bitrate are explicitly labeled
as declared, rather than presented as observed source quality. Station
description is never copied into `comment` or the track title. Information
links are displayed as text; opening a browser on the Radio is not implemented.
Labels are currently English.

## Provider integration

Changes also live in the adjacent provider repositories and must be packaged
with their respective applets; a Base-only update does not update those files.

* **Standalone Radio:** `StandaloneBaseNowPlaying.lua` sends bounded directory
  station properties independently of observed audio. `RadioBrowser.lua`
  preserves homepage/country/language/tags in its existing cached station
  records (old records remain readable). `StreamPlayer.lua` reuses successful
  HTTP/ICY headers for content type and `icy-br`; no probes or extra requests
  are added. MIME types identify a format/container, not sample rate or bit
  depth. Directory bitrate is nominal, not measured. Missing ICY titles and
  timed-ID3 album values explicitly clear obsolete values on the next update.
* **Spotify Connect:** `SpotifyStandaloneNowPlaying.lua` forwards optional
  authoritative `audio`/`transport` objects. Current metadata events provide
  neither source codec nor quality, so these remain unknown. The bridge's
  output is **not** reported as Spotify source format. Equal quality objects
  do not create duplicate revisions just because their Lua tables differ.
* **Qobuz Connect:** `playback.rs` retains source quality from the existing
  URL-resolution response and passes it in the existing playback command.
  No new API fetch is performed. FLAC/MP3 identification uses returned MIME
  type/format ID, and sample rate/bit depth are included only when supplied.
  Legacy kHz and newer Hz response ranges are normalized to Hz; unrecognized
  rates are omitted. No constant FLAC bitrate or unknown channel count is
  invented. `QobuzNowPlaying.lua` publishes through the same generic Base API,
  driven by the applet's existing IPC timer, with a 45-second lease heartbeat,
  bounded retries and ownership-loss handling. Qobuz's current playback command
  lacks title/artist/album/artwork/duration, so those remain unknown; this task
  deliberately does not fetch them separately. Its package manifest includes
  the new Lua module. The Qobuz daemon/app must be updated together.

## Lifecycle and notifications

Changing track ID clears previous track and station attributes before applying
the new snapshot; resend station information with a new identity. A changed
structured station ID also clears previous track quality, even if the provider
accidentally reuses its track ID. Providers should clear obsolete strings with
`""` and send complete quality objects on reconnect. Pause/resume preserves
metadata. Stop clears track/station metadata but retains the owner's lease;
release, expiry and takeover clear all owner metadata.

Metadata/artwork revisions use the existing change flags. Actual metadata
changes publish one existing playerstatus update, not serverstatus/ownership
announcements. Exact duplicate snapshots and heartbeats do not publish updates.
No additional Comet periodic traffic or metadata polling was introduced.

## Resource use

No database, extra daemon, cache, artwork download or metadata-network lookup is
added. Metadata is a single fixed-size snapshot, with a bounded stack candidate
for atomic updates. `sizeof(np_status)` is **6312 bytes on the host** (automated
measurement); its layout is also bounded below 8 KiB by a regression assertion.
The serializer uses a fixed field list and existing response capacity. The
playerstatus push scratch buffer grows by 2 KiB to safely accommodate escaped
maximum-length fields and the More action; response overflow fails with JSON
instead of sending a truncated document. ARMv5 sizes and timings are not
equivalent to host RSS or CPU measurements. Actual playback/OOM/underrun testing
is still required on hardware.

## Automated validation and remaining device checks

Verified by automated tests:

* 13 metadata unit cases cover unknown/MP3/AAC/FLAC/VBR quality, lossless false,
  source versus transport, tag filtering, numeric types, songinfo pagination,
  More Info rows, station/track separation, station switching with a reused ID,
  track transitions, takeover, stop/release, duplicate notifications, escaping,
  malformed/oversized fields and bounded output errors.
* Existing native ownership/lease-expiry tests and Comet lifecycle tests;
  real HTTP Comet tests exercise status, songinfo and contextmenu routing.
* Base host CTest: 38 of 41 passed directly. The three shell tests blocked by
  this Windows checkout's CRLF endings (storage, extended storage, repository)
  also pass when run from temporary LF-normalized copies. No unrelated source
  files were changed to achieve this.
* Spotify: 24 Python/Lua integration/lifecycle tests. Radio: Now Playing,
  navigation, HLS, artwork, handoff and station tests. Qobuz: seven Rust tests,
  Lua publisher tests and existing lifecycle policy tests.
* Existing static ARMv5 Base build; real Now Playing HTTP/Comet lifecycle and
  metadata commands also pass under QEMU's ARM926 CPU. Qobuz ARMv5 build and
  QEMU help smoke test pass.

Not verified on a physical device: stock Jive More Info display/navigation,
Dark Flat Skin display, real ICY headers, Qobuz quality values and playback
transitions, idle CPU/RSS, and audio underruns. Dark Flat's query shapes are
source-verified and tested; the skin has not been installed or changed.

Before release, pair a browser as usual and test MP3/AAC radio, HLS timed-ID3,
Qobuz FLAC and Spotify in turn; open More Info, switch sources/stations, pause,
resume, stop and let a lease expire. Check that unknown fields disappear,
station descriptions never become track titles/comments, and unchanged audio
does not produce extra metadata notifications. A targeted pre-release build was
deployed to a firmware 7.7.2 Radio: Jive restarted, the Radio applet registered,
and native health/status passed. That startup check does not validate playback,
More Info rendering or third-party skin behavior on the physical device.
