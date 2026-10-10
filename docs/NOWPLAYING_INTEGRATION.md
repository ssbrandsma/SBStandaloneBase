# Now Playing integration

An application should claim only when it becomes the foreground playback
source, retain the returned token in RAM, update on track/state/seek changes,
heartbeat at roughly 30–60 seconds, consume commands, and release on clean
shutdown. After a failed heartbeat/update or an sbbase restart, reclaim and
republish the application's actual state.

## Lifecycle

1. Playback activation: claim with source, display name and capabilities.
2. Playback/new track: update state, position and complete track metadata.
3. ICY metadata: update title/artist with the same live station ID.
4. Pause/resume/seek: report confirmed state and authoritative position.
5. Stop: report stopped; release when the application relinquishes ownership.
6. Commands: long-polling is planned; version 1 currently returns immediately.
   Execute in ID order, acknowledge, then publish confirmed state.

## BusyBox example

The Radio may lack curl. A future app normally uses its native HTTP client.
For manual tests, `wget` can send JSON where its build supports `--post-data`:

```sh
wget -qO- --header='Content-Type: application/json' \
  --post-data='{"source":"radio","name":"Standalone Radio"}' \
  http://127.0.0.1:9000/api/nowplaying/claim
```

Insert the returned token in subsequent calls. Do not store it on flash.

## Native C

Open a TCP socket to `127.0.0.1:9000`, send HTTP/1.1 with an exact
`Content-Length`, `Content-Type: application/json`, and `Connection: close`.
Bound responses, check the HTTP status before parsing, and reconnect for each
state-changing call. This is intentionally dependency-free and works with the
same poll/socket APIs already used by embedded applications.

## Lua/Jive

Use the existing Jive networking task/http facilities asynchronously. Never
block the UI task waiting for commands or artwork. Treat `409` as lost ownership
and claim again only if the app is truly still the active playback source.

## Source examples

- Spotify/Qobuz: stable service track ID, `live:false`, duration and position;
  claim capabilities that the playback SDK can actually execute.
- Internet radio: stable station ID/name, `live:true`, no duration/seek; change
  title/artist for ICY updates without reclaiming.
- Unexpected disconnect: heartbeats cease, the 120-second lease expires, and
  StandaloneBase publishes stopped/empty. On restart, claim a fresh generation.

Commands are requests, never state confirmation. In particular, receiving
`pause` must not make the app assume the native UI already displays paused; the
app must execute pause and publish the confirmed result.

## Native update semantics

`playlist_timestamp` is a process-local, strictly increasing revision. It
changes only when `track.id` changes; it does not depend on wall-clock
resolution. A same-ID ICY title/artist update and buffering/play transition
publish a complete playerstatus snapshot while retaining the revision. An
exact duplicate effective state refreshes the ownership lease without sending
an unnecessary notification.

When a supplied ID differs from the current ID, StandaloneBase clears the old
track structure before applying the new fields. Thus an update for a new
station may omit title and artist without accidentally displaying values from
the previous station. Omitted fields on a same-ID update remain patch-style and
retain their values.

With `--test-nowplaying`, stderr includes update generation, change flags,
track ID, playlist revision, subscriber identity, and delivery counts. Tokens
are deliberately excluded. This mode is for controlled diagnostics only.
