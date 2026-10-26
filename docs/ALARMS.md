# Standalone Base alarms

## Why the menu was missing

SqueezePlay has a resident **AlarmSnooze** applet, but that applet is not an
alarm configuration service. LMS supplies the Settings → Alarm Clock menu,
stores alarms, selects the next occurrence and starts the alarm playlist.
AlarmSnooze supplies the popup, physical snooze/off controls, hardware wake-up
and built-in fallback sound. Base previously supplied neither the menu nor the
alarm fields in `playerstatus`.

Source inventory used for this implementation:

- LMS `Slim/Control/Jive.pm`: `alarmSettingsQuery`, `alarmUpdateMenu`,
  `alarmUpdateDays`, `jiveAlarmVolumeSlider`, and the `settingsAlarm` menu entry.
- LMS `Slim/Control/Commands.pm`: `alarmCommand`; `Jive.pm`: `jiveAlarmCommand`.
- LMS `Slim/Control/Queries.pm`: `alarmsQuery` and `alarmPlaylistsQuery`.
- LMS `Slim/Utils/Alarm.pm`: scheduling, repeat/one-shot, volume and fade behavior.
- SqueezePlay `applets/AlarmSnooze/AlarmSnoozeMeta.lua`: resident applet loading.
- `AlarmSnoozeApplet.lua`: `notify_playerAlarmState`, `rearmRTCAlarm`,
  `openAlarmWindow`, `_alarmSnooze`, `_alarmOff`, fallback audio and MCU wake-up.
- `jive/slim/Player.lua`: `_processPlayerstatus`, `snooze`, `stopAlarm`.
- `jive/slim/LocalPlayer.lua`: `playFileInLoop`, `volumeLocal`.

The inspected Jive checkout was `9bad3c1f55f45575d985c37d73c4f1513c5a64d7`.
The native scheduler is an independent implementation of the protocol, not a
port/copy of the GPL LMS scheduler. No LMS server or Perl runtime is installed.

## Architecture

```text
Stock Settings / SlimBrowser alarm widgets
                   │ existing local Comet requests
                   ▼
sbbase alarms.c ────┬──── atomic alarms.conf
 configuration     │     beside Base's active config.json
 bounded scheduler │
                   │ stock v2 alarm playerstatus + private audio event
           ┌───────┴──────────────────┐
           ▼                          ▼
 Stock AlarmSnooze             Base AlarmPlayback.lua
 RTC wake-up + popup           Radio preset/current station
 snooze/off + fallback         local tone + volume/fade
```

There is no new daemon, thread, browser polling loop, database or management
authentication path. The existing sbbase event loop advances the scheduler.
Only the real local player's SlimProto connection permits server-side firing.
The existing loopback-only listener and management authentication are unchanged.

## User operation

Connect the Radio to Standalone Base and open **Settings → Alarm Clock**.
Add an alarm using the stock time-entry widget. Configure enabled state, weekdays,
repeat/one-time behavior and playlist, or delete it with confirmation. All Alarms,
Alarm Volume and Fade In are available in the main menu.

The alarm popup, snooze/off buttons, speaker routing, hardware wake-up, alarm
icon and failed-stream fallback remain the device's stock AlarmSnooze behavior.
Defaults are nine minutes snooze, one hour timeout, volume 50 and fade disabled.
Fade In ramps over 20 seconds and yields immediately to manual volume changes.
Snooze/off restores the volume saved before the alarm. Dismiss/continue-audio
leaves playback and volume unchanged. Scheduled audio events are deduplicated.

Sources available without an external LMS library:

- Current Playlist: resume the Standalone Radio station, if available.
- Alarm Sound: the firmware's local `AlarmSnooze/alarm.mp3`.
- Preset 1–6: Standalone Radio's saved presets. A missing preset/applet falls
  back to the local tone. A failed network stream uses stock decoder fallback.

## Scheduling and persistence

- Maximum eight alarms; fixed bounded allocation.
- `alarms.conf` is alongside the active `--config` file, normally
  `/mnt/storage/standalonebase/alarms.conf`. This uses the existing retained
  configuration location and works without extended storage.
- Files are mode 0600; writes use same-directory temporary files, file fsync and
  atomic rename. Idle clock ticks and status reads do not write configuration.
  A sudden power loss can still lose the last update: directory fsync and a
  UPS/transactional flash guarantee are not provided.
- Configuration and transitions, including the last fired civil date, persist
  across sbbase restarts. Invalid/unreadable files fail closed, are not overwritten,
  and produce a diagnostic in the existing sbbase service log.
- Daily alarms use `localtime_r`/`mktime` and the Radio's `/etc/localtime`, not
  fixed UTC offsets or repeated 86400-second additions. DST uses libc's rules;
  a nonexistent spring-forward time is normalized forward. A repeated autumn
  hour does not fire the same alarm twice on one local date.
- Clock corrections replan future alarms. A known due occurrence at most 90
  seconds late can fire; old missed alarms are not replayed hours later.
- Active and snoozed durations use monotonic time. Wall-clock adjustments do
  not silently extend the alarm, and corrected expiration is saved for restart.
- One-time alarms disable after their first occurrence. When Base is disconnected,
  native audio does not take over from another LMS. Stock cached RTC fallback
  remains responsible for disconnected operation.
- Boot clocks earlier than 2020 are not trusted for scheduling. Set time/timezone
  correctly; restart Base after changing the timezone file while it is running.
- The ARMv5 target's 32-bit `time_t` retains its existing 2038 limitation.

## Wire compatibility

Supported LMS commands: `alarmsettings`, `jiveupdatealarm`,
`jiveupdatealarmdays`, `jivealarmvolume`, `alarms`, `alarm add/update/delete`,
`alarm enableall/disableall/defaultvolume/playlists`, and
`jivealarm fadein/snooze/stop` (including `continueAudio`).

Stock status fields: `alarm_state`, `alarm_version:2`, `alarm_next`,
`alarm_next2`, `alarm_repeat`, `alarm_days`, `alarm_snooze_seconds`,
`alarm_timeout_seconds`. Private `sb_alarm_*` fields carry only a bounded
source identifier, volume/fade and serial-numbered audio transition.

The bridge ignores other players and server UUIDs. It cancels fades on handoff
without issuing stop or volume changes against the newly selected LMS. It also
cancels pending Radio playlist resolution before snooze/off, so a late network
response cannot restart the alarm stream.
Serial-numbered audio events and bridge failures use the normal StandaloneBase
Jive logger (`/var/log/messages`); native scheduler/storage failures also appear
in `/tmp/standalonebase/sbbase.log`. Idle ticks do not produce log messages.

Two guarded instance-level adapters on the resident AlarmSnooze applet release
Radio's reconnect watchdog before stock fallback audio takes over, and send a
native snooze even after the stock tone changes local state to `active_fallback`.
They delegate unchanged for a real LMS. No installed firmware Lua file is patched.

## Validation

Host tests cover native command parsing, menu structure through real Comet HTTP,
wrong-player/source rejection, scheduling, repeat/one-shot behavior, snooze,
backward clock correction, DST scheduling, fixed capacity, atomic persistence,
restart, corrupt/unwritable storage, bounded output and unchanged failure state.
Lua tests cover duplicate events, current station/presets, missing-preset tone,
volume restoration, fade/manual override, dismiss, server handoff and recovery
of the stock popup after a Jive restart during an active alarm.

Run the normal CMake/CTest suite. Build ARM with `sh scripts/build-arm.sh`.
`test_alarms` can also be cross-compiled with the normal ARM toolchain and run
under `qemu-arm -cpu arm926`; neither test contacts a physical device.
Both packaging scripts include `AlarmPlayback.lua`.

Validation recorded on 2026-10-26: all 53 tests in the configured host suite
(`build-release-060`, `BUILD_SBPROXY=OFF`) passed. The alarm C tests also passed
with AddressSanitizer/UndefinedBehaviorSanitizer and as a statically linked
ARMv5 executable under QEMU's ARM926 CPU. The complete ARM build, including
sbproxy, passed; it retained existing image-render/NanoSVG compiler warnings.
Proxy-specific host TLS/HLS tests were not included in that host configuration.
These were host-only checks; no release ZIP was published.

Later on 2026-10-26, with user approval, the three alarm-related files (`sbbase`,
`StandaloneBaseApplet.lua`, `AlarmPlayback.lua`) were deployed to the test Radio
and SqueezePlay was restarted, without rebooting Linux or replacing unrelated
services/configuration. A rollback snapshot was retained in temporary RAM storage.
Live Comet checks confirmed the Settings alarm entry, normal alarm settings
response, zero configured alarms, `alarm_state:none`, and no next occurrence.
All Alarms was explicitly set to Off. No alarm was created or sounded. Physical
menu navigation, audible playback/snooze and RTC wake-up remain unverified.

### Required physical acceptance test before claiming LMS parity

The test build is deployed; physical alarm/UI acceptance is still pending.

1. With user approval, deploy to a test Radio; record its current volume, source,
   timezone and existing alarm settings. Do not overwrite a user's wake-up alarm.
2. Verify Settings → Alarm Clock, all stock widgets, back navigation, weekdays,
   enable/disable, add/edit/delete, global volume and Fade In on 7.7.x and 9.x.
3. Set an alarm a few minutes ahead at a safe audible volume. Test stopped,
   playing, standby/screensaver and repeating/one-time cases. Confirm the popup,
   alarm icon, speaker routing, snooze, off, dismiss, timeout and volume restoration.
4. Test a saved preset and a deliberately unavailable stream; verify stock
   fallback sound, then snooze/off while fallback is sounding.
5. Test sbbase/Jive restart before and during an alarm, Radio reboot, disconnected
   fallback and hardware RTC wake-up. Never rely on an untested alarm to wake up.
6. Test forward/backward clock changes and both DST transitions on a controlled
   host fixture; do not change the everyday Radio's clock to simulate them.
7. Switch to a real LMS while an alarm/fade is active. Confirm that Base does not
   seize the player or alter the new server's playback/volume. Switch back.
8. Remove test alarms and restore the original user settings.

## Remaining differences and limitations

Full LMS feature parity is not claimed before physical acceptance testing.
Menus currently use English text and 24-hour list labels; stock time-entry
widgets still use the device's normal input UI. Base has no external LMS music
library, playlists, favorites database or arbitrary alarm-URL resolver. Current
Playlist resumes Radio, not Spotify/Qobuz or a remote LMS music queue. Shuffle
settings are retained for protocol compatibility but have no effect on a single
live station/tone. The bounded `alarms` list returns all matching entries rather
than implementing LMS pagination; third-party controllers need separate tests.

Saved native state does not replace the firmware's RTC battery or guarantee
wake-up after a total loss of power. Firmware-specific RTC/DST fallback behavior,
audio underruns, standby power state and interactions with other audio applets
must be tested on hardware. Uninstallation/reinstallation and firmware recovery
retain alarms only insofar as the existing configuration storage is retained.

For recovery, stop the service, back up an invalid `alarms.conf` through local
shell access, then move that exact file aside and restart Base to create a new
empty configuration. Do not erase or reinitialize extended storage to repair an
alarm configuration file.
