# Native SqueezePlay Now Playing — Phase 1 proof of concept

## Scope and safety

This phase supplies fixed LMS-compatible metadata only. It does not send a
SlimProto playback command, create an audio stream, touch the playback engine,
replace Logitech's Now Playing applet, or integrate any audio application.
Normal StandaloneBase behavior is unchanged unless `--test-nowplaying` is
present. The test endpoint is disabled otherwise, and all sbbase listeners are
loopback-only.

## Verified LMS/SqueezePlay data flow

The source investigation used SqueezePlay commit
[`758bc110`](https://github.com/ralph-irving/squeezeplay/tree/758bc110e16de4a61905e19d012a0ad25d97859b)
and LMS commit
[`f0a77cdc`](https://github.com/LMS-Community/slimserver/tree/f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1).
These are newer than Radio firmware 7.7.3, so the physical firmware's exact Lua
revision remains a compatibility uncertainty.

Verified in upstream source:

1. `Player:onStage()` subscribes to `/slim/playerstatus/<player-id>` with the
   command `status - 10 menu:menu useContextMenu:1 subscribe:600`.
   [Player.lua](https://github.com/ralph-irving/squeezeplay/blob/758bc110e16de4a61905e19d012a0ad25d97859b/src/squeezeplay/share/jive/slim/Player.lua#L982-L1000)
2. `Comet:subscribe()` sends that as `/slim/subscribe`; its response channel is
   `/<clientId>/slim/playerstatus/<player-id>`. The client separately subscribes
   to `/<clientId>/**` and receives later events through the chunked streaming
   connection.
   [Comet.lua](https://github.com/ralph-irving/squeezeplay/blob/758bc110e16de4a61905e19d012a0ad25d97859b/src/squeezeplay/share/jive/jive/net/Comet.lua#L253-L307)
3. Player status updates `time`, `duration`, playlist size/index, player state,
   and volume. A changed `mode` generates `playerModeChange`; changed
   `item_loop[1].params.track_id` generates `playerTrackChange`; changed
   `playlist_timestamp` generates `playerPlaylistChange`.
   [Player.lua](https://github.com/ralph-irving/squeezeplay/blob/758bc110e16de4a61905e19d012a0ad25d97859b/src/squeezeplay/share/jive/jive/slim/Player.lua#L1175-L1303)
4. For normal local tracks, Now Playing reads `track`, `artist`, and `album`
   from the first `item_loop` entry. `remote:1` plus `current_title` is intended
   for remote streams, so this proof uses `remote:0` and a stable `track_id`.
   [NowPlayingApplet.lua](https://github.com/ralph-irving/squeezeplay/blob/758bc110e16de4a61905e19d012a0ad25d97859b/src/squeezeplay/share/applets/NowPlaying/NowPlayingApplet.lua#L1973-L1985)
5. Metadata notification does not create the Now Playing window when it does
   not already exist. The user normally opens the stock Now Playing screen;
   once open, notifications refresh it. A non-empty `playlist_tracks` is
   required or the applet defers back to the browser.
   [NowPlayingApplet.lua](https://github.com/ralph-irving/squeezeplay/blob/758bc110e16de4a61905e19d012a0ad25d97859b/src/squeezeplay/share/applets/NowPlaying/NowPlayingApplet.lua#L537-L578),
   [window entry](https://github.com/ralph-irving/squeezeplay/blob/758bc110e16de4a61905e19d012a0ad25d97859b/src/squeezeplay/share/applets/NowPlaying/NowPlayingApplet.lua#L1873-L1927)

LMS itself publishes zero-based `playlist_cur_index`, a playlist timestamp and
the playlist count, and uses `item_loop` for menu-mode status responses.
[Queries.pm](https://github.com/LMS-Community/slimserver/blob/f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1/Slim/Control/Queries.pm#L4217-L4227)

## Implementation

Start the server with:

```sh
sbbase --config /mnt/storage/standalonebase/config.json --test-nowplaying
```

The first fixed status is:

- Sultans of Swing — Dire Straits — *Dire Straits*
- duration 348 seconds, position 0, mode `play`
- one playlist item, index 0, `remote:0`, track ID `900001`

The second is Money for Nothing — Dire Straits — *Brothers in Arms*, duration
506 seconds and track ID `900002`. A loopback-only trigger switches tracks:

```sh
wget -qO- --post-data='' http://127.0.0.1:9000/test/next-track
```

The change increments `playlist_timestamp` and publishes a complete updated
status only to active streaming clients that explicitly subscribed for the
current player. Initial subscriptions always receive current state through the
request response. Stream replacement preserves subscription state;
re-subscription is idempotent; disconnect and unsubscribe remove delivery.
Normal mode continues to return `mode:stop`, an empty `item_loop`, and zero
playlist tracks. The existing serverstatus bootstrap, HELO registration,
keepalives, discovery, health endpoint, chunking and heartbeats are unchanged.

Fixed strings avoid dynamic metadata allocation. Player IDs must be valid MAC
addresses, response paths are length-checked, event buffers have bounded
sizes, and all writes continue through the existing bounded/nonblocking send
path. A general JSON parser was deliberately not added for this fixed proof.

## Automated validation

`tests/comet/test_nowplaying.py` uses simulated CometD clients and checks:

- production stop/empty status and disabled test endpoint;
- valid first-track JSON and consistent one-item playlist fields;
- exact initial `/slim/subscribe` response;
- targeted update after the trigger;
- no unsolicited update to an unrelated stream;
- replacement stream, re-subscription, current-state recovery and unsubscribe;
- unchanged serverstatus and health behavior.

Existing Comet connection, protocol, discovery/self-test and static ARMv5
build checks remain part of regression validation.

## Physical Radio validation procedure

This procedure has not yet been executed. It stops/restarts **sbbase only**;
never stop SqueezePlay, because the Radio watchdog may reboot the device.

1. Build and verify the ARM binary on the PC:

   ```sh
   wsl sh scripts/build-arm.sh
   wsl qemu-arm -cpu arm926 build-arm/sbbase --version
   wsl qemu-arm -cpu arm926 build-arm/sbbase --self-test
   ```

2. Install the new package or atomically copy the new `sbbase` binary into both
   StandaloneBase applet locations. Do not initialize storage or alter Logitech
   Lua files.
3. Over SSH, replace only the supervised sbbase process with demo mode while
   keeping its PID file correct so the applet supervisor does not start a
   duplicate:

   ```sh
   ROOT=/usr/share/jive/applets/StandaloneBase
   PIDFILE=/tmp/standalonebase/sbbase.pid
   test -r "$PIDFILE" && kill "$(cat "$PIDFILE")"
   "$ROOT/sbbase" --config /mnt/storage/standalonebase/config.json \
       --test-nowplaying >>/tmp/standalonebase/sbbase.log 2>&1 &
   echo $! >"$PIDFILE"
   ```

4. Verify `pidof sbbase`, `GET /health`, and the log line
   `NOWPLAYING TEST enabled`. Confirm the Radio remains connected to Standalone
   Base without repeated reconnect screens.
5. On the Radio open the original Logitech **Now Playing** item. Expected:
   Sultans of Swing, Dire Straits, album Dire Straits, play mode, one track.
   Metadata does not intentionally force the window open.
6. Trigger the second track over SSH with the loopback `wget` command above.
   Expected: the open native screen changes to Money for Nothing / Dire Straits
   / Brothers in Arms and the log records `NOWPLAYING TRACK changed` followed
   by `NOWPLAYING PUSH`.
7. Disable the proof by restarting sbbase normally, again updating its PID:

   ```sh
   kill "$(cat /tmp/standalonebase/sbbase.pid)"
   /usr/share/jive/applets/StandaloneBase/sbbase \
       --config /mnt/storage/standalonebase/config.json \
       >>/tmp/standalonebase/sbbase.log 2>&1 &
   echo $! >/tmp/standalonebase/sbbase.pid
   ```

8. Confirm `/health`, normal connection, and stopped/empty status.

If the UI does not respond, capture `/var/log/messages`, the sbbase log,
SqueezePlay `net.comet`, `squeezebox.player`, NowPlaying and SlimProto lines,
the player MAC, the exact `/slim/subscribe` request, returned playerstatus JSON,
and subsequent pushed event. If correct metadata reaches `Player.lua` but the
screen still does not appear or refresh, the likely remaining question is
whether firmware 7.7.3 gates presentation on real playback-engine state. The
smallest next experiment is to observe its normal status while playing a real
track and compare only missing state fields—not to modify Logitech UI code or
send playback commands in this phase.

## Unverified behavior and compatibility limits

- Native Radio firmware 7.7.3 has not yet displayed these simulated tracks.
- No claim is made that metadata-only `mode:play` is sufficient when the local
  playback engine is idle.
- The inspected upstream SqueezePlay source is newer than r16676; exact field
  handling and menu behavior must be confirmed from device logs.
- No audio, artwork, real application metadata or automatic Now Playing window
  transition is implemented.
