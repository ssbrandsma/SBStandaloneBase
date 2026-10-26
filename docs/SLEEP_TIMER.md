# Sleep timer

Open **Settings → Sleep** while connected to Standalone Base. Choose 15, 30,
45, 60 or 90 minutes, or Cancel sleep timer. Reopening the menu shows rounded-up
remaining minutes. The menu uses stock SlimBrowser actions, not a new applet.

`native/sbbase/src/sleep.c` implements LMS `sleepsettings` and `sleep <seconds>`
commands. Zero cancels, `?` queries remaining seconds, and explicit durations
must be decimal integers from 0 through 86400. Player IDs must match the real
local player. Playerstatus supplies `sleep`, `will_sleep_in` and a private
`sb_sleep_serial` expiry marker.

The existing sbbase loop uses monotonic milliseconds. Wall-clock/NTP/DST changes
do not affect an armed duration. There is no additional timer thread, background
HTTP polling or flash persistence. The timer is cancelled on local SlimProto
disconnect (including a library switch), service restart, and when an alarm is
active or snoozed. An alarm therefore cannot accidentally be stopped by an old
sleep countdown.

Expiry queues Stop for the active Now Playing owner using the existing control
integration. `AlarmPlayback.lua` receives the stock `playerSleepChange` event,
stops Radio/reconnect activity and local audio, and requests the Radio's normal
soft-power-off state. Serial deduplication and an initial baseline prevent old
expiry events stopping fresh playback after a Jive restart or library switch.
Events from another LMS/player are ignored.

Host coverage: synthetic monotonic expiry, single firing, cancellation,
disconnect, duration bounds, malformed commands, stock menu integration and Lua
expiry/deduplication/handoff behavior. ARMv5 compilation and ARM926 emulation
validate the native timer separately from audible hardware behavior.

Physical acceptance remains pending: test each menu entry, a short safe-volume
countdown while playing Radio, cancel, standby/clock display, alarm arbitration,
switching LMS and the other installed audio integrations. Do not treat host
tests as proof of Spotify/Qobuz remote-stop or firmware-specific standby behavior.
This implementation stops at expiry; LMS end-of-track sleep and gradual sleep
fade-out are not implemented.
