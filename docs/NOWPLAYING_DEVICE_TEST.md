# Phase 2 physical validation procedure

This procedure is documented but has not been executed. Deploy only after a
separate approval. Keep the previous binary as `sbbase.pre-phase2`; do not alter
firmware, UBI volumes, other applets, or the audio engine.

## Preparation and rollback

Stop only the supervised sbbase process, atomically install the candidate in
both applet paths, then start it normally (without `--test-nowplaying`). Keep
SqueezePlay running. Verify with `netstat -lnt` that 3483 and 9000 listen only
on `127.0.0.1`. Roll back by atomically restoring the saved binary and restarting
sbbase; no reboot should be required.

Use a PC-side client through SSH port forwarding, or BusyBox wget when it
supports headers/post data. Never expose port 9000 on Ethernet/Wi-Fi.

## A. Music

Claim `spotify-test`; publish a known-duration track at position 10 seconds.
Open native Now Playing and record title/artist/album/progress. Wait 10 seconds,
publish paused, wait again, publish playing, then publish a different track ID.
Expected: progress advances only while playing and track metadata changes once.

## B. Live radio

Release music; claim `radio-test`; publish `live:true`, stable station ID/name,
no duration, and title/artist. Change only ICY title/artist. Expected: station
identity remains stable, metadata refreshes, and no fake duration is displayed.

## C. Takeover

While radio owns the session, claim `qobuz-test`. Send a radio update using its
old token; expect HTTP 409 and unchanged Qobuz metadata. Release Qobuz; expect
the native state to become stopped/empty rather than resuming radio.

## D. Native controls

Claim with play/pause/stop/next/previous. Press each physical/native control,
fetch `/commands`, and record the request. Confirm it from the client with an
update, then acknowledge its ID. Expected: no UI state transition is published
before confirmation. Seek and volume are observation-only in this milestone;
capture CometD/SlimProto logs without changing hardware volume behavior.

## E. Artwork

Publish a station-logo HTTP URL, then replace it with an album-art URL without
changing the station ID. Restore the station logo and finally publish an empty
URL. Verify each CometD snapshot contains the exact value in
`item_loop[0].icon`, while `playlist_timestamp` stays fixed and no serverstatus
appears. For SBHttpsProxy use
`http://127.0.0.1:8765/https/<host>/<path>` and inspect:

```sh
tail -f /tmp/standalonebase/sbproxy.log
```

Absence of a proxy request after successful CometD delivery indicates the
loopback compatibility patch is absent, inactive, or rejected. Check:

```sh
cat /tmp/standalonebase/artwork-patch.log
grep -n 'StandaloneBase loopback artwork' /usr/share/jive/jive/slim/SlimServer.lua
ls -l /usr/share/jive/jive/slim/SlimServer.lua.pre-standalonebase
```

The request must then appear in `sbproxy.log` without a request to
`baby.squeezenetwork.com/public/imageproxy`. CometD delivery alone does not
prove image decoding or rendering.

## F. Reliability

Reconnect SqueezePlay, restart sbbase, then reclaim and republish. Let one test
session expire. Run for at least 30 minutes and inspect logs. Expected: no
repeating “Connecting to Standalone Base”, no client-slot exhaustion, stopped
state after restart/expiry, and normal discovery/HELO/keepalive operation.

Record firmware, binary SHA-256, player MAC, timestamps, API responses, relevant
logs, and screenshots. A feature is hardware-validated only after those results
are reviewed.

## Station-update reliability matrix

Keep Logitech's native Now Playing screen open. Test A to B, rapid A to B to C,
same-station ICY changes, a station with no artist/title, and
playing-buffering-playing. Repeat preset switching at least 25 times, force a
CometD reconnect, restart only StandaloneBase, and then play continuously for
at least 30 minutes. Expected: the latest station always wins, same-station ICY
updates do not replace station identity, and ordinary updates do not cause a
server reconnect or a full serverstatus publication.

For each discrepancy, capture all four views at the same timestamp:

1. `GET http://127.0.0.1:9000/api/nowplaying` through an SSH local forward.
2. A `/slim/request` status response for the Radio MAC, including
   `playlist_timestamp`, `current_title`, and `item_loop`.
3. The matching `/slim/playerstatus/<mac>` streaming CometD event and the
   diagnostic `NOWPLAYING UPDATE`/`DELIVERY` lines from a controlled run.
4. A photograph or screenshot of the actual native Now Playing screen.

Record whether the update was accepted, its generation (never its token), ID,
change flags, playlist revision, subscriber ID, and delivery count. This
separates API rejection, state comparison, publication, transport delivery,
and SqueezePlay rendering failures. This procedure remains unexecuted until a
separate deployment and physical-test approval is given.

For the radio cases, verify the native three-line layout reads title, artist,
and station respectively. Switch to a station whose initial update omits
title, artist, and artwork; the old values must disappear. Confirm playback
does not pause and the Player is not rebuilt while logo/album art alternates.
