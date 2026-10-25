# Jive CPU and memory investigation

Date: 24 October 2026. Device: `192.168.1.223`. Investigation only.

## 1. Executive summary

**Playback follow-up:** after the initial investigation, the user started a
stream and reported a 100% peak in the System Performance page. New captures
found **34.33-40.61% Jive CPU**, with a substantial increase in both main-thread
and secondary-thread work, plus **13.4% jive_alsa**. The later screenshot shows
51.1% total, Jive 35.4%, and jive_alsa 14.0%, closely agreeing with steady playback
measurements. The initial near-100% transient was not captured at thread level;
it must not be dismissed as a proven accounting artifact. See the playback
follow-up below. The Radio rebooted between the earlier and playback captures.

The **initial unspecified-state captures** did not reproduce the reported
30.8% Jive CPU. A 66.15-second observation found Jive at **9.84%**, of which **9.40
percentage points were its main OS thread**. `jive_alsa` independently used
**7.15%**. Jive RSS stayed at **25,992 KiB (25.38 MiB)** across seven samples.
This narrows attribution to the main Lua/UI/event/network path, but does not
identify which callback or native call accounts for it. No call-stack profiler
was enabled. A subsequent lighter 61.76-second capture found Jive at **10.07%**
and jive_alsa at **6.93%**, supporting the same broad result with less collector
work. Playback intent and the visible screen were not independently
confirmed; do not label this an idle, stopped, playing, display-on or display-off
A/B result.

Important findings:

- The shared audio mapping occupies **3,532 KiB in both Jive and jive_alsa**.
  Adding their RSS double-counts this memory. Private heap usage remains material.
- The final Jive smaps census found **17,772 KiB resident in `[heap]`**, another
  **2,104 KiB in anonymous mappings**, plus shared audio, executable/library and
  other mappings. `[heap]` contains both native and Lua allocations: it is not a
  measurement of Lua heap size.
- Source confirms a periodic frame loop, explicit incremental Lua GC, and timers
  that continue in the background. It does **not** establish continuous full
  redraws: native drawing uses dirty regions and flips only if something drew.
- Radio backlight changes do not by themselves suspend the frame/timer loop.
  Physical darkness, a blank screensaver and disabled rendering are different
  conditions.
- Base's remote bridge wakes every 250 ms, but the `remote-active` marker was
  absent at inspection. Thus its heavy screenshot/request path is not supported
  as the explanation for this capture.
- Base globally enables HTTP tracing and wraps SocketHttp requests. Radio
  browsing/search, artwork conversion and native Now Playing updates are useful
  burst-work candidates. Their contribution to 30% CPU remains unmeasured.
- No memory leak is demonstrated by one minute of flat RSS. A longer run and
  in-process Lua heap/cache counters are necessary.

No production source, firmware, settings or services were changed; no packages
were installed on the Radio, and no service was restarted. Diagnostics were
streamed into SSH and saved on the workstation. Existing services may naturally
log SSH connections; this is not literally zero observer activity.

## 2. Firmware and source compatibility

### Installed system

The Radio reports:

```text
SqueezeOS/SqueezePlay: 9.0.2 r17146
Firmware build: root@poky Mon Jun 22 18:46:25 EDT 2026
Base build revision: 2e6b23e18165e824cc1d41e339583e456aa38f6a
Kernel: 2.6.26.8-rt16, PREEMPT RT, Jun 22 2026
BusyBox: 1.18.5, Jun 22 2026
Physical memory reported by Linux: 61,804 KiB; swap: none
```

The extracted Jive binary contains `Squeezeplay 9.0.2 r17146`. Its native binary
has not been reproduced from source, so this is not an exact native-build match.
The firmware's base build hash is not the SqueezePlay source hash.

### Repository verification and closest source

| Repository | Finding |
| --- | --- |
| `LMS-Community/squeezeos-squeezeplay` | Public URL returned 404; not usable as supplied. |
| [LMS-Community/squeezeplay](https://github.com/LMS-Community/squeezeplay) | Exists; public/7.8 default, with native `src/squeezeplay` sources. Useful lineage, not the installed 9.0 build identity. |
| [ralph-irving/squeezeos](https://github.com/ralph-irving/squeezeos) | Exists; firmware recipes, kernel and packaging. Its recipe selects the separate native repository below. |
| [ralph-irving/squeezeos-squeezeplay](https://github.com/ralph-irving/squeezeos-squeezeplay/tree/public/9.0) | Actual public/9.0 native/Lua source selected by the SqueezeOS recipe. |

Source inspected at upstream commit
`766b6a8b3c3c7722d586125b068b47facf9d4a77` (10 October 2026), in a separate
case-sensitive WSL checkout at `/home/sjoerd/jive-performance-source-20261024`.
The closest available date candidate preceding the installed firmware build is
`031ea8c9d75d2230355df5c3317088ea85874611` (22 June, 15:05 EDT). Between that
candidate and inspected HEAD, the only change under `src/squeezeplay/src` and
`src/squeezeplay/share/jive` is a PCM sample-rate lookup extension. This supports
the relevance of event-loop analysis, not an exact binary provenance claim.

Local application sources:

- Base: this repository, commit `70fa144a7a94d0c07acc9c61dc265efb51614c2a`.
- Radio: `C:/Projects/squeezebox`, commit
  `22c2dbcfdb514ca5a6a6dc75ec7b767d3a4d4c3a` (Radio 0.9.14).
- SqueezeOS recipe inspected at `C:/Projects/squeezeos2`, with existing unrelated
  dirty files retained. Its `poky/meta-squeezeos/conf/distro/squeezeos.conf:32`
  selects `ralph-irving/squeezeos-squeezeplay`, branch public/9.0, AUTOREV.

**36/36 installed Base and Radio Lua files match local bytes (MD5 comparison).**
Installed `Framework.lua`, `Timer.lua`, `Task.lua`, `JiveMain.lua`,
`NowPlayingApplet.lua`, and `SqueezeboxBabyApplet.lua` match inspected upstream
bytes. Installed SlimServer differs by the expected `StandaloneBase artwork
routing v2` adapter. Its sink delegates to `applets.StandaloneBase.Artwork`.
This difference was inspected, not discarded as a mismatch.

Disk-file matching does not prove what a long-running Lua process already loaded
before a file replacement. No restart was performed to resolve that residual
uncertainty. Runtime confirmation of loaded function origins belongs in approved
instrumentation.

### Evidence retained locally

Ignored directory `.local/jive-performance/` contains raw full and light
captures, installed-file hashes, a binary auxiliary vector, selected installed
Lua/binaries, and an offline analyzer. No diagnostics containing device details
are added to Git by this report. Capture names use the workstation investigation
date; device syslog time is not assumed to share its timezone.

Raw capture SHA-256 values:

```text
runtime-20261024.txt
c733416cc525a74593fe70b81f59a5c3c8c7cbfe7fee32743128703c0c9e9654
light-runtime-20261024.txt
80a5588dbac3d64045ccfd430d64550be6cd7e52d688f2bece710fab814bcd8f
```

## 3. Jive architecture relevant to performance

Paths in this section are relative to upstream `src/squeezeplay/`, unless noted.
They can be inspected at the pinned
[source commit](https://github.com/ralph-irving/squeezeos-squeezeplay/tree/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay).

### Main loop and task scheduling

`share/jive/ui/Framework.lua:286-367` implements the main loop in Lua. It runs
priority-ordered coroutine Tasks, services the network task with a zero timeout
when work remains or a deadline-based timeout when idle, then at a frame boundary:

```text
updateScreen -> collectgarbage("step") -> Timer:_runTimer -> processEvents
```

`Task.lua:24-90` has audio/high/low priority queues. These are cooperative Lua
coroutines, **not separate Linux threads**. `NetworkThread.lua:155-210` uses
LuaSocket select and coroutine yielding. A long-running callback can occupy the
main OS thread and delay UI/network service. Remaining runnable Tasks can cause
zero-timeout select calls; that is a mechanism to measure, not proof of a spin.

The native header `src/ui/jive.h:23` defines **22 frames/s**, giving a 45 ms
integer frame interval in this source. The binding specification's example
constant is 20; use the compiled native constant, not that example. The running
binary's actual exported value has not been queried in-process.

`Timer.lua:47,170-208` stores a sorted list, runs due callbacks through `pcall`,
and reschedules recurring timers. Late timers skip excessive catch-up instead
of replaying every missed interval. A nominal 10 ms Lua timer is therefore not
evidence of 100 callbacks/s; callbacks are serviced at frame boundaries. Queue
operations use array insertion/removal, but no large active-timer count has been
measured.

### Rendering, display off and redraws

`src/ui/jive_framework.c:427-656` performs layout and active animation work,
draws a transition or dirty region, and calls `jive_surface_flip()` only when a
draw occurred. `setUpdateScreen(false)` exits screen work early, after keeping
the UI watchdog alive. Merely calling updateScreen at every frame does not mean
every frame is fully repainted.

`src/ui/jive_surface.c:1237` delegates flipping to SDL. Exact framebuffer/video
mode and native costs still require runtime attribution. Per-pixel work can
become expensive during scrolling labels, meters, transitions or screenshots.

`Widget.lua:560,634-648` starts/stops **widget-owned** timers on show/hide.
Raw `Timer(...)` objects do not have this visibility behavior. Active animation
registration is also visibility-aware. Do not indiscriminately disable all
timers when a screen is hidden: audio/network ownership and recovery use them.

The matching Radio hardware applet is in
`src/squeezeplay_baby/share/applets/SqueezeboxBaby/SqueezeboxBabyApplet.lua`.
Its `setBrightness/_setBrightness` functions (498-540) write brightness and
bl_power; they do not call `setUpdateScreen(false)`. A separate 100 ms brightness
timer (113,200) may run automatic brightness work, with sensor-read divider 5.
The shared-source setUpdateScreen calls found are splash-screen handling in
JiveMain, not a general backlight-off scheduler switch. A Clock screensaver can
continue animating while the unit is considered powered off.

### Audio and memory ownership

`src/audio/decode/decode.c:1052` creates a native decoder thread. In
`decode_alsa.c:93-169,207-281`, Jive creates SysV shared audio storage, launches
`jive_alsa` through vfork/exec, and monitors it. `jive_alsa` is a separate process,
so its CPU is not included in Jive's own `utime+stime`.

`decode_alsa_backend.c:809-922` loops around PCM availability/state, waits for
periods and supplies output. PCM can remain running while supplying silence;
an ALSA RUNNING state is not proof of an audible stream. This capture found
44.1 kHz stereo, 441-frame periods and an 882-frame buffer: **10 ms periods**,
consistent with approximately 100 voluntary context switches/s in jive_alsa.
It does not establish that all output CPU is unavoidable.

`src/audio/streambuf.c:34` reserves a 3 MiB native stream buffer. Audio FIFO and
effects sizes are defined in `decode_priv.h:214-224`. Resident pages depend on
touch history; reserved virtual buffers must not all be counted as additional
resident memory.

### GC and existing diagnostics

Jive opens standard Lua libraries in `src/jive.c:479`; startup temporarily stops
GC, then restarts it. Framework subsequently performs a full collection, stops
automatic GC and requests an incremental step each frame. This deliberate GC
scheduling is relevant to allocation bursts and native resources freed by Lua
userdata. Blindly changing GC pause/step settings is not an established fix.

Existing facilities include:

- `Framework:perfwarn(...)`: native screen/layout/draw/event warning thresholds;
  task duration warnings above 20 ms also exist in the Lua loop.
- `jive.perfhook`: native call/return hook with optional **process-exit threshold**.
  It is not safe to enable casually, can conflict with hooks, and would add
  per-call overhead. Do not supply a nonzero kill threshold.
- `jive.heap()` and JiveMain's 60-second heap-debug timer (JiveMain:402): object
  census/history, gated by the heap logger's debug level. It is more expensive
  than `collectgarbage("count")`, and its diagnostic history itself uses memory.
- Optional LuaProfiler integration is commented out in JiveMain. The firmware
  recipe lists a Lua profiler dependency; runtime usability was not established.
- `configure.in:155-176` offers `--enable-profiling` / `-pg`; gprof needs an
  instrumented build, useful output termination, and introduces overhead.

A separate instrumented build is technically feasible using the existing
SqueezePlay recipe/SDL/Lua/audio dependency stack and Radio platform component.
It requires the appropriate ARM ABI, firmware-specific patches/libraries and
symbol preservation. The previously prepared kernel compiler is not proof that
this userspace dependency stack is ready. No Jive or firmware rebuild was done.

Community changes inspected include June's Radio elapsed-time/progress display
update and October's PCM rate lookup extension; neither is a demonstrated fix
for this CPU report. The Radio recipe also applies effects-resampler and
XRUN/bass-dropout audio fixes. Those are correctness/real-time changes that must
remain accounted for in any native comparison, not speculative performance
patches to remove. No verified community fix for the specific 30% Jive workload
was established in this investigation; this is not an exhaustive issue survey.

## 4. Timers, callbacks and background-work inventory

Base paths below are relative to this repository. `Radio/` means
`C:/Projects/squeezebox/applet/StandaloneRadio/`. Frequencies are configured
intervals, not measured callback counts. Event-driven callbacks have no fixed rate.

### All Base Lua timer sites

| Source | Interval/lifetime | Work and background behavior |
| --- | --- | --- |
| `applet/RemoteBridge.lua:55,106` | 250 ms recurring | Check RAM marker even without browser. With marker and no request pending: capability read, local HTTP request, heartbeat file. No display/playback gate. |
| `RemoteBridge.lua:76-104` | Request-driven; capture >=750 ms apart | Remote actions inject Framework events; requested screenshots allocate a surface, draw a window and save BMP in `/tmp`. Heavy path requires active marker/request. |
| `StandaloneBaseApplet.lua:182,198` | 15 s recurring | Service liveness via shell/grep for three processes, server migration checks, local-server connect. Continues stopped, hidden and display off. |
| `StandaloneBaseApplet.lua:208` | One-shot min(60 s, n*5 s), n<=5 | Failed-service restart backoff. Not normal steady-state polling. |
| `StandaloneBaseApplet.lua:185` | 2 s once after init | Storage check/recovery decision, potentially starting a separate asynchronous operation. Not a 2-second repeating storage scan. |
| `StandaloneBaseApplet.lua:249` | 1 s while pairing countdown | Menu text update until expiration/cancel. Timer closure captures menu/window-related objects; validate all close paths during instrumentation. |
| `StorageManager.lua:55` | 500 ms during storage operation | Read RAM progress status and invoke progress callback; stopped at completion/Manager stop. Not active simply because extended storage is mounted. |
| `TimeSync.lua:215` | 10 s startup once | Begin NTP resolution/synchronization. |
| `TimeSync.lua:65,189` | 5 s request timeout; 100 ms next-server continuation; retries 60/300/900/3600 s; daily resync | UDP/DNS/command callbacks; retry-related work continues in background. Not high-frequency continuous normal load. |
| `TimeSync.lua:99` | 10 s command guard, once per command | Timeout for asynchronous date/hwclock helper; no work when synchronization is dormant. |

`StandaloneBaseMeta.lua:15` loads Base unconditionally at registration; an
`enabled` default setting is not evidence of an implemented runtime disable
switch. `free()` stops Base-owned timers/services, but invoking it would change
device behavior and was not done.

### Radio timer sites and conditionals

| Source | Configured intervals | Activation/work |
| --- | --- | --- |
| `Radio/StreamPlayer.lua:24-29,160-195` | 1 s audio recovery; 30 s watchdog; 300 s power keepalive | Started by component construction at boot. Recovery quickly returns when inactive/stopped/paused; keepalive returns when inactive. Timer wakeups remain. |
| `StreamPlayer.lua:361` | One-shot 2/5/10/30 s reconnect | Failed desired stream only; generation guards. |
| `StandaloneBaseNowPlaying.lua:19-22,223,232,254` | 45 s heartbeat; 15 s request timeout; 2/5/10/30 s retry | Owned active session, in-flight request or failure only. Metadata/playback revisions suppress redundant publication; position publication is coarser than native decoder updates. |
| `NativeNowPlaying.lua:8,36` | One-shot retries 50/250/500/1000/2000 ms | Bounded asynchronous navigation; avoids reopening the already current Now Playing window. |
| `HlsMetadata.lua:55,91,160` | Continuation 1 ms; legacy idle fallback 5 s; failure backoff <=30 s; timeout wait+15 s | During an active HLS session. Supports one long poll up to 25 s by default, not fixed rapid polling. Pending decoder metadata uses 50-5000 ms scheduling, or 1 s if position unavailable. |
| `RadioBrowser.lua:292,367` | 10 ms recurring during search/popular scan | Batches 100/50 lines; stops/closes at completion/cancel. Frame-bound scheduling limits actual callback rate. |
| `OpmlClient.lua:150,189,220,267` | 15 s fetch timeout; RSS extends to 180 s; 1 ms deferred closes | Per-request guards/deferred cleanup, not always-running polling. |
| `StandaloneRadioApplet.lua:200` | Typically 1800 ms once | Transient message closure. |
| `StandaloneRadioApplet.lua:808` | 5 s startup-test once | Only when explicit `/tmp/standalone-radio-autotest` marker triggers scheduling. |

`StandaloneRadioMeta.lua:49` eagerly loads Radio and enables its physical
controls. `StandaloneRadioApplet.lua:176` returns false from `free()`, explicitly
retaining the applet. Hiding its menu therefore does not unload components or
remove the three background timers.

### Other callbacks, allocation and network consumers

| Component | Concrete work and source |
| --- | --- |
| Hardware events | Radio's `enableStandaloneMode` installs transport/preset handlers. Playback hooks in `StreamPlayer.lua` wrap SlimProto send, ICY metadata and stream-disconnect handling. Callback frequency follows input/stream events. |
| Native Now Playing | Radio state/metadata/selection callbacks in `StandaloneRadioApplet.lua:120-166` feed `StandaloneBaseNowPlaying`; HTTP publication reaches `native/sbbase/src/nowplaying.c` and Comet playerstatus in `main.c:340`. `main.c:1523` polls descriptors with a 1 s maximum timeout but publishes on state/subscription/expiry paths, not unconditionally every second. |
| Stock Now Playing | Upstream `NowPlayingApplet.lua:126,386-735` subscribes to player notifications and can update artwork, labels, playlist and mode. Widget timer at :1487 updates position at 1 s; other timer sites control scrolling, volume and navigation. Visibility and subscription behavior must be counted separately. |
| Global HTTP tracing | `applet/HttpTrace.lua:40-124` forces INFO and wraps SocketHttp fetch/close and each request's response methods. Allocates trace tables, wrapper closures, normalized headers and log strings for **all users of the shared layer**, not only Radio. Per-request, not per-audio-byte logging. Code presence is confirmed; current emission rate was not measured. |
| Artwork routing | `Artwork.lua:20-47`, `ImageRender.lua:33-65` normalize/wrap URLs and read web-port settings. The installed SlimServer adapter hands decode/cache/icon callbacks to `Artwork.sink`. It retains compressed bytes, decodes a native Surface and updates waiting icons. Stale-request tokens and cancellation guards already exist. |
| Jive artwork cache | Upstream `jive/slim/ArtworkCache.lua:23` has an 8 MiB compressed-byte LRU **per cache/server instance**; `SlimServer.lua:565` uses a weak decoded-image map. Live widgets can still retain native Surfaces; weak cache does not imply all image memory is free. Cache occupancy was not measured. |
| Radio logos/track art | `LogoCache.lua:23-25` bounds per-file input at 512 KiB and files at 100; persistence is not 50 MiB of resident Lua data. `TrackArtwork.lua:70-101` suppresses repeated artist/title lookup keys. Fetch/decode/network costs follow metadata or visible directory items. |
| Native image renderer | `native/sbwebserver/src/image_render.c:33-39` limits SVG/input/output and a 20 MiB **disk cache**, with asynchronous worker handling and capped waiters. Do not charge all its scratch buffers or disk cache to Jive RSS. Native decode/compressed response reception can still create Jive allocation spikes. |
| Directory/search retention | RadioBrowser reads JSON-lines, lowercases strings, decodes matching records and allocates result tables; explicit full GC occurs at scan completion and downloaded page boundaries, with step calls during iteration. OPML/RSS parsing builds items and closures. `OpmlNavigator.lua` retains at most 12 navigation contexts and clears popped menus. These are bounded candidates, not evidence of a leak. |
| Native services | Base launches `sbproxy` on 127.0.0.1:8765, `sbbase` for SlimProto/discovery/HTTP and `sbwebserver`. Socket/DNS/Process callbacks return into Jive; native service CPU is accounted separately. Supervision shell children are short-lived and can be missed between proc snapshots. |

Spotify/Qobuz are not in this device's installed-applet listing. Their local
sources exist (`C:/Projects/SBSpotifyConnect`, `C:/Projects/QoBuz`), including
Spotify's 250 ms track watcher and service timers, but they cannot explain this
capture as installed applets. Treat their tests as a later separately approved
configuration, not a current-device fact.

## 5. CPU measurements and profiling possibilities

### Completed read-only baseline

`runtime-20261024.txt` contains seven samples with six nominal 10-second sleeps.
Actual midpoint elapsed time is **66.15 s**, because full snapshots took
**0.94-1.04 s**. CPU calculations use measured elapsed uptime, not requested sleep.
The collector enumerated only selected processes and their threads, not the
whole system. A separate lighter control omits thread/status enumeration costs.

The initial `od -An -tu4` command was incompatible with this BusyBox build and
produced no auxiliary-vector output. This was corrected by streaming the raw
128-byte `/proc/820/auxv` to the host and decoding little-endian ARM pairs:
**AT_CLKTCK=100, AT_PAGESZ=4096**. Thus the denominator is verified for the
running process, not guessed from CONFIG_HZ. `getconf` is unavailable.

| Process/TID | Average CPU | User / system | Range of ~11 s interval averages | RSS first/last |
| --- | ---: | ---: | ---: | ---: |
| Jive PID 820, group | 9.84% | 7.33 / 2.51% | 7.99-14.17% | 25,992 / 25,992 KiB |
| Main TID 820 | 9.40% | 7.30 / 2.10% | 7.72-13.81% | Not a separate memory account |
| TID 840 | 0.00% | 0 / 0% | 0% | Shared process mm |
| TID 851 | 0.44% | 0.03 / 0.41% | 0.36-0.45% | Shared process mm |
| jive_alsa PID/TID 850 | 7.15% | 3.90 / 3.25% | 6.90-8.05% | 7,000 / 7,000 KiB |
| sbwebserver PID 892 | 0.47% | 0.17 / 0.30% | 0.45-0.54% | 428 / 428 KiB |
| sbproxy PID 875 | 0.00% | 0 / 0% | 0% | 900 / 900 KiB |

Intervals are not instantaneous peaks: shorter spikes may be missed. Jive task
names all say jive; no unsupported function-level identification of TIDs 840/851
is made. Parent Jive CPU equals its thread sum approximately; **do not add group
CPU and thread CPU together**. sbbase was omitted by the initial target-name
list and is included in the light control; these are not exhaustive task sums.

Jive main thread had ~43 voluntary and ~18 involuntary context switches/s.
jive_alsa had ~100 voluntary/s, SCHED_FIFO policy 1 and RT priority 45; Jive was
normal policy 0. Main-thread minor faults rose by 8,743, with no major faults.
Minor faults can reflect allocation/page touching or remapping/COW; they are not
disk reads or proof of a leak.

The conventional first-eight-field `/proc/stat` busy estimate was **14.55%**,
even below selected process CPU totals. This is an accounting/skew warning, not
an allocation of the difference to negative CPU. The existing
[CPU-accounting report](cpu-accounting-investigation.md) found independent idle
accounting discrepancies on `.222`; it is useful caution, not proof of the same
precise failure on this capture. The RT kernel appends overlapping user_rt and
system_rt fields: never sum all its columns as independent time categories.

### Lower-overhead control

`light-runtime-20261024.txt` used shell built-in proc reads and omitted task
enumeration and smaps. It spans **61.755 s**, with **0.20-0.32 s** snapshots:

| Process | Average CPU | ~10 s interval range | RSS first/last |
| --- | ---: | ---: | ---: |
| Jive | 10.07% | 8.72-14.11% | 25,992 / 25,992 KiB |
| jive_alsa | 6.93% | 6.83-7.00% | 7,000 / 7,000 KiB |
| sbbase | 0.049% | 0-0.10% | 280 / 280 KiB |
| sbwebserver | 0.453% | 0.39-0.49% | 428 / 428 KiB |
| sbproxy | 0.00% | 0% | 900 / 900 KiB |

This reduces observer work and broadly corroborates the full capture. The
captures are consecutive observations, not simultaneous or a controlled A/B
workload. Light capture omits context-switch fields; absence must not be reported
as zero switches. Its conventional aggregate busy reading is **7.71%**, versus
about 17.5% from selected process runtime deltas. Neither metric is independently
verified against hardware cycle/idle measurements here. This strengthens the
warning against interpreting this kernel's headline as precise capacity usage.

### User-confirmed playback and 100% graph follow-up

The user subsequently confirmed streaming, seeing 100% in Base's **System
Performance** web page, then a reduction toward 60%. Their screenshot later
shows total CPU **51.1%**, Jive **35.4%**, jive_alsa **14.0%**, and uptime five
minutes. Station/codec and visible Radio screen were not supplied.

The first playback-state read had uptime **266.82 s**, versus roughly 35,168 s
in the earlier investigation. Jive PID changed 820 -> 821. Thus the Radio was
rebooted outside this investigation; no stopped/playing same-boot A/B or memory
growth inference can be made by directly subtracting the earlier results.

A full process/thread and timer-list diagnostic, with a requested 30 s sleep,
spanned **32.675 s** between snapshot midpoints. Snapshot scans took 2.68 and
2.33 s and perturb the target. Using USER_HZ=100 verified on this firmware:

| Playback component | CPU ticks | Average wall-time CPU |
| --- | ---: | ---: |
| Jive PID 821, all threads | 1327 | 40.61% |
| Main TID 821 | 769 | 23.54% |
| Secondary Jive TID 852 | 555 | 16.99% |
| jive_alsa PID 851 | 439 | 13.44% |
| Matched processes, including kernel threads and collector | 2248 | 68.80% |

The main/secondary split rules out explaining all playback Jive CPU as Lua
timers or UI drawing on the main thread. TID 852 is consistent with the native
decoder: `decode.c:1018-1052` initializes ALSA/forks its output process, then
creates `decode_thread_execute`. Actual user-space stacks were not sampled,
so that thread-to-function mapping remains strongly supported rather than
directly observed. The main thread also handles audio/network coroutine Tasks,
not just UI; its 23.54% is not a measured redraw cost.

Aggregate tick-based CPU was **69.66% busy** (2294/3293 ticks), close to matched
process runtime totals in this window. Independently, `/proc/timer_list`
`idle_sleeptime` advanced **7.263 s**, and idle tick delta was 999. Both show
idle activity during the capture, so it was not continuously saturated for
the entire window. Idle-path timing is not a hardware cycle measurement;
do not infer an exact corrected busy percentage or equate its fraction with
the differently sampled idle tick fraction.

The subsequent lighter capture spans **63.48 s**, with 0.35-0.59 s snapshots:

| Process | Average CPU | ~10 s interval range | RSS first/last |
| --- | ---: | ---: | ---: |
| Jive | 34.33% | 33.55-35.07% | 25,164 / 25,164 KiB |
| jive_alsa | 13.39% | 13.23-13.63% | 7,000 / 7,000 KiB |
| sbbase | 0.047% | 0-0.10% | 280 / 280 KiB |
| sbwebserver | 3.92% | 0.48-5.22% | 464 / 476 KiB |
| sbproxy | 0.00% | 0% | 880 / 880 KiB |

Aggregate busy averaged **53.30%**, with ~10 s interval averages 48.95-55.66%.
This agrees with the later screenshot's 51.1% and the reported decline. These
averages do not rule out short genuine 100% peaks between samples or before
collection. No measurement captured the initial near-100% event itself.

PCM was RUNNING at **48 kHz**, stereo S16_LE, with 480-frame periods and a
960-frame buffer (10 ms / 20 ms). A 48 kHz output rate does not identify the
stream's encoded codec or input sample rate. Jive RSS was 23,200 KiB in the
initial state read, then 25,164 KiB at the first and last light snapshots. This
is some post-boot growth followed by a short plateau, not evidence proving or
excluding a long-run leak. The screenshot's application-memory estimate is
system-wide cache-subtracted usage, not Lua heap or private Jive memory.

The performance page defaults to **2 s polling** (`web/js/performance.js:8`),
uses first-eight-counter busy/total deltas (:104-119), and calculates process
CPU against wall time (:132-144). The backend `monitor.c:135-155` explicitly
excludes appended RT subtotals. Thus no RT-field double-counting bug was found
in this calculation. It still inherits kernel counter behavior. Its monitor
requests enumerate process data and allocate/serialize snapshots; the browser's
chart rendering runs on the client, not on the Radio. Webserver CPU rose during
the light capture while the user was viewing the monitor. Monitor overhead is
a plausible contributor, not an explanation for all Jive CPU or a proven cause
of the 100% transient. No polling interval was changed remotely.

Evidence retained in `.local/jive-performance/`:

```text
playing-accounting-20261024.txt SHA256
4badc0db7b000616a3d2b55e0af3ac3265604715a199c861c3c9f5a6091bc659
playing-light-20261024.txt SHA256
372eb628b739b9428fd693f089f5d68fa36ff720c1db9bf68fc945df3cede5dc
playing-state-20261024.txt: PCM, initial status and bounded existing service logs
```

Updated diagnostic priority: separate decoder-thread cost from main-thread
network/UI/GC work, then capture a reproducible startup/track-change spike.
Compare monitoring off or at a slower user-selected interval with the same
stream and screen, because observation itself adds work. Preserve the original
timers/GC/native profiling proposals; do not assume an idle-timer optimization
will eliminate native decode cost. No services, playback or settings were
changed during either follow-up capture.

### Available tools and limits

- Available: proc PID/task stat and status, maps and smaps, binary auxv, ALSA
  proc status/hw_params, standard shell, pidof, cat, grep, sleep, md5sum.
- `strace`, `gdb`, `gprof`, `perf`, `opcontrol`, `oprofiled`, `lua` and `luajit`
  executables were not found. No packages were installed.
- PID `io` and `schedstat` were unavailable. wchan paths were attempted but did
  not produce usable symbolic results; no sleeping-call attribution follows.
- CONFIG_PROFILING, CONFIG_OPROFILE and CONFIG_KALLSYMS are enabled. OProfile is
  a possible approved native sampling route, but userspace/setup/overhead are
  unverified; enabling it is not a read-only action. Modern perf is not an
  available solution for this 2.6.26 target.
- `strace` would expose syscall frequency, not CPU stacks, and ptrace can disrupt
  audio. GDB attachment stops execution. Neither is a first-line live-radio test.
- Proc-only sampling can attribute CPU to OS threads and scheduling domains. It
  **cannot distinguish Lua from SDL/font/image native calls on the same thread**.

Formula, for matching PID/TID and starttime:

```text
CPU% = 100 * delta(utime + stime) / (AT_CLKTCK * delta(uptime))
```

Use stat fields 14/15, not child fields 16/17. Locate the final `)` terminating
comm before splitting fields. Check starttime (22), restarts and disappearing
threads. Use process counters OR thread sums. Record bracket/midpoint timing
uncertainty; user/system splitting is less robust on this RT accounting path
than total task runtime. Bound measurement overhead with a lighter control.

## 6. Memory attribution and limits

Full-capture Jive: VmSize 31,268 KiB; VmData 20,368 KiB; VmStk 84 KiB; RSS
25,992 KiB; 3 threads. No swap exists. A missing VmSwap field is not a parser
error on this kernel. VmSize and VmStk are reservations, not resident amounts.

Final smaps breakdown:

| Jive mapping class | Virtual size KiB | Resident KiB | PSS KiB |
| --- | ---: | ---: | ---: |
| `[heap]` | 17,916 | 17,772 | 17,772 |
| Anonymous mappings | 2,444 | 2,104 | 2,104 |
| Shared SysV audio | 3,532 | 3,532 | 1,766 |
| Stack | 84 | 28 | 28 |
| Files/devices/other mappings combined | 6,492 | 2,828 | 1,658 |

The census was read after the timed snapshots; its RSS total need not equal the
earlier status field. Smaps walks are non-atomic and not free. The same deleted
`/SYSV0000de01` mapping appears in jive_alsa. Here `(deleted)` reflects SysV
shared-memory lifecycle, not automatically leaked memory. Shared library pages
are also counted in multiple RSS totals. PSS apportions sharing; USS approximates
Private_Clean+Private_Dirty, but neither distinguishes allocator object owners.

`Cached=21,328 KiB` and only 2,064 KiB free at the initial snapshot do not alone
show impending OOM; part of cache/slab is reclaimable. This kernel has no modern
MemAvailable estimate. No swap makes sustained private growth more consequential.

Lua heap is **not yet measured**. In-process `collectgarbage("count")` reports
Lua-managed KiB, including Lua objects/userdata storage but not arbitrary native
SDL/audio/image allocations referenced by userdata. A separate Lua executable
would measure its own heap, not Jive's. `[heap]` includes native and Lua users of
the allocator; native memory cannot be obtained by blindly subtracting Lua heap
from RSS. `VmData` growth can indicate reserved arena growth, not resident leaks.

Distinguish: cached live objects, unreachable Lua backlog, native live objects,
allocator-retained free arenas and fragmentation. Procfs alone cannot separate
all five. A falling Lua count with flat RSS is compatible with allocator reuse;
flat RSS over 66 s does not rule out a slow leak. Do not force GC or call
malloc_trim as an unapproved diagnostic intervention.

## 7. Controlled A/B tests (planned, not executed)

Use the same Radio, kernel and firmware. Record source versions, screen name,
brightness/power state, actual display behavior, source/codec/rate, volume,
network and browser clients. Keep a remote-control/performance browser closed
for the base measurement, then test it as a separate factor. Do not classify
stopped merely from ALSA RUNNING/STOPPED.

| Test | Audio | Display | Standalone | Restart requirement |
| --- | --- | --- | --- | --- |
| A | Stopped | On, fixed static screen | Enabled | No restart after setup |
| B | Stopped | Off/blank, explicitly identified | Enabled | Normally a UI transition; verify actual state |
| C | Playing fixed stream | On, fixed screen | Enabled | Normally no restart |
| D | Same playback | Off/blank | Enabled | Normally no restart; ensure transition does not stop audio |
| E | Stopped | On | Disabled | Clean approved configuration and Jive restart required |
| F | Same playback via supported non-Standalone path | On | Disabled | Same clean configuration; identical playback may require an external LMS |

E/F are not achieved by closing a menu, changing an unconsumed `enabled` setting,
or stopping just sbbase while its supervisor remains active. Base and Radio
eager-load, Radio refuses unload, and native hooks survive menu closure. Removing
their effects requires an approved clean startup/configuration. No such changes
are authorized by the current investigation. State differences in F (external
LMS versus Standalone source) are a confound, not a clean subtraction.

Additional factors, one at a time:

- Base only versus Base+Radio: separate approved startup compositions.
- Home versus static Now Playing versus scrolling text/meter/Clock screensaver.
- Browser remote-view inactive versus active; collect screenshot count.
- Stable metadata versus track change; artwork cache hit versus miss.
- Artwork on/off requires a diagnostic switch or existing supported control;
  none was invoked. Keep metadata equal when interpreting its delta.
- HLS versus equivalent codec transport: source equivalence is essential.
- Spotify and Qobuz: separate installations/test configurations, not currently
  present; do not install them during this investigation.

For each state: settle 60-120 s, observe 3-5 min at 10 s intervals, repeat three
times and alternate order (A/B/A, C/D/C). Add one higher-resolution short window
only if needed to resolve bursts; report peak **interval** CPU with its width.
Collect process/thread user+system, restart identity, context switches, RSS,
private/PSS memory, optional Lua heap, callbacks by origin, loop/frame/draw counts,
playerstatus notifications, requests and artwork cache hit/miss/decode counts.
Callbacks, Lua heap and redraw rates remain unavailable until instrumentation
is separately approved.

For leaks: 30-60 min constant-state run with status every 30-60 s and smaps at
start/end; then 4-8 h or overnight with sparse sampling. Also repeat a controlled
station/menu/artwork cycle and return to the same quiescent state. Compare Lua
count, private dirty and RSS slopes and post-quiescence plateaus. No full-GC
intervention without approval, and no leak conclusion from slope alone during
cache warm-up. Record restarts/OOMs and aborted runs.

## 8. Ranked hypotheses

| Rank | Hypothesis and confidence | CPU / memory impact | Easiest verification | Optimization risk |
| --- | --- | --- | --- | --- |
| 1 | **Confirmed domains:** earlier unspecified-state work was predominantly main-thread; playback adds substantial secondary/native work as well. Exact hot functions unknown. | Earlier 9.40 points main-thread CPU; playback 23.54 points main and 16.99 secondary in the full capture. | Split native decoder versus main Task/Timer/GC/network/UI timing. | Medium/high if changing the scheduler or decoder. |
| 2 | **Confirmed memory component:** native/Lua mixed heap plus shared audio and caches. A substantial footprint can exist without a leak. | 17.36 MiB resident heap; 3.45 MiB shared audio; exact cache/Lua ownership unknown. | Lua count + cache totals + smaps, longer identical-state run. | Medium; shrinking buffers/caches can hurt playback or downloads. |
| 3 | **Supported hypothesis:** always-running audio output, including silence, explains jive_alsa's load. | 7.15% observed, separate from Jive; shared buffer contribution confirmed. | Correlate logical stopped state with PCM state, periods and per-process CPU. | High: audio timing/underrun/power interactions. |
| 4 | **Supported possibility:** UI activity persists with backlight off; clock/scrollers/meters cause avoidable work in some states. | Could affect main thread; no on/off delta measured. | A/B display states plus actual redraw/animation counters. | Medium; don't disable watchdog/timers globally. |
| 5 | **Code-supported, unquantified:** metadata/artwork/HTTP trace allocations and notifications amplify bursts. | Main-thread CPU and transient Lua/native memory; no measured share. | Counts per request/notification, same-track cache hit/miss A/B. | Low/medium for dedup; medium for tracing/adapter changes. |
| 6 | **Conditional candidate, inactive marker in this capture:** remote-view screenshot path. | Surface/draw/BMP work up to about 1.33 eligible captures/s; browser-service CPU too. | Observe marker and capture counts with remote view on/off. | Low/medium; preserve remote-control responsiveness. |
| 7 | **Confirmed timer presence, weak explanation for large steady load:** 250 ms marker checks, 1 s guarded recovery, 15 s shell supervision. | Likely small per idle callback; no measured duration. | Aggregate callback self/inclusive time, not timer count alone. | Low/medium; preserve health/recovery behavior. |
| 8 | **Unproven:** slow leak, fragmentation or abnormal GC burden. | Potential long-run private growth/pauses; not demonstrated. | Longitudinal Lua/private/RSS/cache measurements. | High for blind GC/allocator tuning. |
| 9 | **Known accounting caveat:** system headline misrepresents capacity. | Can distort total CPU comparisons; does not by itself explain Jive RSS or per-task work. | Wall-time process deltas, independent idle diagnostics if needed. | High for speculative kernel changes. |

Playback now reproduces a comparable or greater Jive load. The exact cause of
the initial 100% graph peak remains unmeasured; UI, codec/startup, browser load,
and accounting effects are still alternatives to test. This report attributes measured work to domains and
specific source candidates, not falsely to an identified hot function.

## 9. Proposed lightweight Lua profiler architecture (not implemented)

An explicitly enabled temporary diagnostic applet can run inside the existing
Lua VM without replacing the native binary. Loading/enabling it is a behavior
change and requires approval. No new shell endpoint should evaluate arbitrary Lua.

1. **Passive counters first:** one sparse 5-10 s sampler for Lua count, existing
   cache totals and lifecycle counts; fixed-size numeric buckets for Timer,
   Task, HTTP, playerstatus and artwork events. Capture source+line once per
   callback registration, not on every call. Cap entries and keep an overflow
   bucket. No per-frame table/string construction.
2. **Reversible wrappers:** aggregate duration/count at Timer dispatch, Task
   resume, updateScreen and GC-step boundaries. Selected callback wrappers can
   identify hot applets. Existing Timer list is a private upvalue; enumeration
   would require version-checked debug introspection or a supported registration
   adapter, not an assumed public API. Preserve arguments, returns, errors,
   yielding and callback ownership. Restore methods/callbacks and stop sampler
   on disable. Avoid double-wrapping HTTP, which Base already wraps.
3. **Clock semantics:** Framework ticks provide millisecond elapsed duration,
   with wrap handling; callback elapsed time is not automatically CPU time.
   `os.clock()` measures process CPU with platform-dependent resolution and
   includes other native threads. Do not sum nested inclusive times or conflate
   blocking elapsed time with execution. Compare aggregates with proc CPU.
4. **Optional instruction hook:** brief low-duty `debug.sethook` count windows
   (for example initially every 50k-100k VM instructions) after overhead tests.
   This gives instruction-weighted Lua locations, not wall-clock sampling of C
   or time blocked in a native call. Existing coroutine Tasks require per-thread
   hook awareness; preserve prior hooks and do not assume the main hook covers
   all running coroutines. No call/return hook by default.
5. **Memory ownership:** bounded counters, weak associations without value-to-key
   retention cycles, no body/artwork buffers, no stored closures/stacks or full
   object graphs. A heap census is a separate, sparse, approved operation.
6. **Export:** bounded RAM ring buffer and manual retrieval, or an existing
   authenticated read-only diagnostics endpoint with rate/size limits. No
   continuous NAND logging, new capability bypass or high-rate browser polling.
7. **Overhead budget:** compare profiler off/passive/timed/hook modes in the same
   state. A provisional acceptance target is <1 CPU percentage point average and
   a small fixed RAM budget (e.g. 64 KiB), to be measured, not promised. Stop on
   audio glitches or excessive callback latency; schedule no forced full GC.

Native SDL/font/decode/allocator stacks remain outside a Lua-only solution.
If wrappers show most time inside a C call, consider approved OProfile sampling
or a separate symbolized/instrumented Jive build next. Keep the installed binary
and startup unchanged during the present task.

Framework captures `collectgarbage` in a local at module load, so replacing
`_G.collectgarbage` later would not time its existing GC steps. Measuring those
steps requires an approved, version-checked binding adapter or an instrumented
event-loop variant. Do not assume a global monkey-patch observes every call.

## 10. Optimization opportunities and defensible benefits

No optimization was implemented. Measure before changing any of these.

| Scope | Candidate | Supported benefit model / qualification |
| --- | --- | --- |
| Base applet | Back off inactive remote marker checks, or notify on activation. | Currently four checks/s; reducing that count is defensible, claiming a large CPU gain is not. |
| Base applet | Reduce no-op supervision work / synchronous shell spawning. | Three liveness shells per 15 s plus server checks; affects latency and bursts, not an established 30% sustained cost. Preserve restart detection. |
| Base applet | Make global HTTP tracing configurable or sampled after diagnosis. | Avoid per-request wrappers/log allocations where disabled. Benefit scales with measured request rate; no reduction estimate yet. |
| Radio applet | Suspend unnecessary raw timers when stopped, preserve needed health checks. | Removes guarded idle wakeups; active-playback recovery and hardware controls must remain correct. |
| Base/Radio/native sbbase | Suppress duplicate metadata/UI notifications and image fetches. | Existing revisions and generation guards already do some deduplication; quantify remaining duplicate callbacks before another layer. |
| Base/Radio | Cache URL/port normalization state, reuse bounded transient structures. | Only useful if measured hot; configuration invalidation and stale results must stay correct. |
| Base/Radio | Bound retained menus/results and improve image-cache hit rates. | Caches and context bounds already exist. Measure occupancy, decode frequency and navigation retention instead of labeling caches leaks. |
| Native Jive | Suppress unnecessary hidden-screen redraw/animation work. | CPU saving bounded by measured draw/animation time; timers/network/watchdog must keep running. Backlight-off alone is not sufficient. |
| Native Jive/audio | Revisit silence-output processing and buffer footprints. | Potentially relevant to measured audio CPU/shared RAM, but underrun risk is high. Needs controlled native profiling, not broad rate/priority changes. |
| Native/Lua | Tune GC only after allocation/collection timing is known. | May trade latency for memory. Frame-driven GC is intentional; no supported tuning value or benefit estimate yet. |

For measured callback cost `c` ms at rate `r`/s, its approximate one-core cost is
`r*c/10` CPU percentage points. Use exclusive/non-overlapping costs for totals.
This is a model, not a measured value for any callback listed above.

## 11. Safe commands and exact next steps

The following commands use facilities observed on this Radio. Run within an
already authenticated SSH session, or redirect their output to a workstation.
They do not restart services or change playback/configuration. Replace a
hard-coded PID after checking pidof, and avoid frequent smaps scans.

```sh
uname -a
cat /etc/squeezeos.version
pidof jive jive_alsa sbbase sbwebserver sbproxy
p=$(pidof jive)
cat /proc/$p/status
cat /proc/$p/maps
# A single memory census, not a tight loop:
cat /proc/$p/smaps
```

Two process snapshots with uptime and identity, for offline delta calculations:

```sh
p=$(pidof jive)
for n in 1 2; do
  echo SAMPLE
  cat /proc/uptime
  cat /proc/$p/stat
  for t in /proc/$p/task/[0-9]*; do
    cat "$t/stat"
    grep ctxt_switches "$t/status"
  done
  cat /proc/uptime
  [ "$n" = 1 ] && sleep 15
done
```

Also useful, read-only:

```sh
cat /proc/asound/card0/pcm0p/sub0/status
cat /proc/asound/card0/pcm0p/sub0/hw_params
cat /sys/class/backlight/mxc_lcdc_bl.0/brightness
cat /sys/class/backlight/mxc_lcdc_bl.0/bl_power
ls -l /tmp/standalonebase/remote-active
```

Missing remote-active is an expected inactive case. Brightness/bl_power are raw
hardware readings, not universal proof of a particular UI screen or mode.
BusyBox's od lacks `-A` and `-t`; to inspect auxv use `cat /proc/$p/auxv` over a
binary-safe SSH stream and decode it on the workstation. Do not print binary
auxv directly to a terminal. No getconf/readlink assumption is required.

Follow-up: [native source audit and scrolling-text findings](jive-native-source-audit.md).
The user's subsequent observation that CPU rises while Now Playing text scrolls
makes the redraw/full-frame rotation path the leading candidate for additional
UI load; it has not yet been isolated by a controlled scrolling/no-scroll test.

Next steps, in order:

1. Record the screen, logical playback state/source, browser activity and source
   revision at the moment **30% Jive CPU** recurs. Repeat the light capture in
   that state. This investigation did not manipulate those factors.
2. Run A-D with user-controlled state changes and stable sampling; keep E/F and
   applet removal/restart configurations pending explicit approval.
3. Review the proposed diagnostic-module design, then approve a bounded passive
   sampler/callback counter if desired. Do not deploy hooks on the strength of
   this report alone.
4. Resolve main-thread CPU into frame/GC/Timer/Task/network buckets; count and
   time the leading callback origins and actual redraws, not just timer intervals.
5. Collect sparse Lua heap/cache/private memory over matched-state short and
   overnight tests. Decide whether growth is retained cache, Lua backlog or
   native/allocator memory before proposing a memory fix.
6. Only if native time remains dominant, plan a symbolized Jive/OProfile test
   with a quantified overhead and audio-risk budget. No kernel change or flashing
   is justified by the current evidence.
