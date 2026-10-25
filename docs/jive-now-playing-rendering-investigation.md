# Jive Now Playing scrolling investigation

2026-10-24. Status: source review, evidence review and offline models completed.
Subsequent approved diagnostic pilots are recorded in sections 14-15, including
a full reboot and a failed visible-title comparison. **No optimization deployed.**
Stock executables were verified unchanged after recovery. Further live tests
require approval before UI stop/replacement. The follow-up
[end-to-end source review](jive-scrolling-end-to-end-review.md) records new
paint-loop/allocation findings, offline checks and implementation priorities.

## 1. Executive summary

Automatic label scrolling and manual menu navigation must be treated separately.
The ordinary label animation calls `reDraw`, not `reLayout`. Cached text surfaces
are reused. Consequently, the high layout cost previously measured while turning
the menu knob is not evidence that steady Now Playing scrolling incurs the same
layout work.

The source does establish a rendering amplification: a visible label invalidates
its bounds; painting is clipped to a union of current/previous damage; then Jive
rotates **every pixel** of the logical framebuffer, without receiving damage.
Nominal label animation is half the 22 Hz framework rate. Actual rotations per
second during automatic Now Playing scrolling have **not yet been measured**.
Pauses, short strings, frame drops and other UI updates prevent assuming a
constant eleven full-frame rotations each second.

Partial rotation is algorithmically feasible, but simply rotating one current
label rectangle is unsafe. Later diagnostic capture confirmed hardware double
buffering and writable-page pointer changes (section 15 and the follow-up review).
Repair damage accumulated
since each destination page was last updated, initialize every page, and retain
full-frame fallbacks. An offline model passes 12 synthetic tests. It is not an
SDL patch and demonstrates correctness only under its stated assumptions.

An offline /proc cross-check independently reproduces impossible CPU accounting
in the earlier capture. This strengthens the kernel/accounting hypothesis but
does not identify the faulty instruction or establish corrected CPU percentages.
No optimization benefit has been measured or claimed.

## 2. Reviewed sources and current-device verification

Main source: `ralph-irving/squeezeos-squeezeplay`, pinned commit
`031ea8c9d75d2230355df5c3317088ea85874611`, local case-sensitive tree
`/home/sjoerd/squeeze_userbuild/source`. Source references below use this revision;
diagnostic additions shift line numbers in instrumented trees. It is the closest
inspected pre-firmware source, not proven exact stock-binary provenance.

Reused artifacts:

- [Native source audit](jive-native-source-audit.md) and
  [runtime investigation](jive-performance-investigation.md).
- `C:\Projects\squeeze_userbuild\patches\render-timing.patch`,
  `analyze-device-timing.py`, timing-helper tests and `device-tests/` captures.
- `build.sh`, `verify.sh`, `device-test.py`, `device-session.sh`,
  `device-rollback.sh`, `device-failsafe.sh` and `device-observe.py`.
- `C:\Projects\squeeze_userbuild\device-results.md` records previous approved
  tests and restoration. No fresh infrastructure or SDK reconstruction was needed.

Read-only verification in `logs/now-playing-readonly-check.txt` confirms stock
`/usr/bin/jive`, PID 5816 at verification, uptime 5868.34 seconds, and unchanged
MD5s:

```
ee632b3141dbecb2f08467fa73923072  /usr/bin/jive
95fa00abb73ee78ae14a14f0bc06885b  /usr/bin/jive_alsa
```

### Exact path/function map

Paths in this table start under `src/squeezeplay/` unless specified otherwise.

| Responsibility | Source and entry point |
| --- | --- |
| Now Playing title/artist handoff | `share/applets/NowPlaying/NowPlayingApplet.lua:1399`, `textStopCallback` closures; `_addScrollSwitchTimer` at 1905; track animation activation at 929 |
| Frame rate | `src/ui/jive.h:23`, `JIVE_FRAME_RATE=22`; `share/jive/ui/Framework.lua:304`, event-loop frame scheduling |
| Animation registration | `share/jive/ui/Widget.lua:484`, `addAnimation`; `Framework.lua:1181`, `_addAnimationWidget` |
| Label enable/advance | `src/ui/jive_label.c:346`, `jiveL_label_animate`; at 295, `jiveL_label_do_animate` |
| Widget redraw vs relayout | `src/ui/jive_widget.c:355`, `jiveL_widget_redraw`; at 317, `jiveL_widget_relayout`; at 420, `jiveL_widget_check_layout` |
| Global invalidation union | `src/ui/jive_framework.c:664`, `jive_redraw` |
| Layout, animation, damage clipping, background and window draw | `src/ui/jive_framework.c:427`, `_draw_screen` |
| Window/group/widget traversal | `src/ui/jive_window.c:198`, `jiveL_window_draw`; `jive_group.c:276`, `jiveL_group_draw`, their `draw_closure` helpers |
| Cached text drawing | `src/ui/jive_label.c:391`, `jiveL_label_draw`; cache preparation in `prepare` at 138, called by `jiveL_label_layout` |
| Screen update and flip decision | `src/ui/jive_framework.c:621`, `jiveL_update_screen` |
| Screen allocation and rotation | `src/ui/jive_surface.c`, `jive_surface_set_video_mode` around 823; `jive_surface_flip` at 1192 |
| SDL final update | repository `src/SDL-1.2.13/src/video/SDL_video.c:1083`, `SDL_Flip`, plus `SDL_UpdateRects` at 1015 |
| Linux framebuffer backend | repository `src/SDL-1.2.13/src/video/fbcon/SDL_fbvideo.c:1318`, `FB_FlipHWSurface`; at 1340, `FB_DirectUpdate` |

## 3. Complete scrolling call flow

```text
NowPlaying: trackTitle:animate(true)
  -> jiveL_label_animate
     -> Widget:addAnimation(jiveL_label_do_animate, 22/2)
        -> register visible widget with Framework

Framework event loop (nominal 22 Hz; 45 ms frame period after flooring)
  -> Framework:updateScreen -> jiveL_update_screen -> _draw_screen
     -> top window:checkLayout [origin guarded; not automatically full layout]
     -> animation countdown [every second framework update for this label]
        -> jiveL_label_do_animate
           -> if text fits: textStopCallback; no moving-text redraw
           -> advance offset; if in initial/repeat pause: possibly return
           -> label:reDraw -> jiveL_widget_redraw -> jive_redraw(bounds)
     -> if damage exists:
        paint_clip = union(current_damage, last_damage)
        set logical surface clip
        paint global background; draw window/group/children
          -> jiveL_label_draw: clipped cached foreground/shadow blits
        last_damage = current_damage; clear current damage
        return drawn=true
  -> if drawn: jive_surface_flip
     -> lock logical surface and real SDL surface
     -> full logical-surface pixel loop, rotated strided writes
     -> unlock both
     -> SDL_Flip(real_sdl)
        -> hardware double buffer: FB_FlipHWSurface -> FBIOPAN_DISPLAY
        -> otherwise SDL_UpdateRect(full screen) -> backend UpdateRects
```

The three-second Now Playing timer chooses which title/artist label should
animate; it is **not** the per-scroll-step timer. The latter is a frame-count
animation scheduler. `SCROLL_PAD_START=-100`, `SCROLL_PAD_LEFT=-200`,
`SCROLL_PAD_RIGHT=40` and font-dependent pixel steps produce initial/repeat
pauses. A callback is not necessarily a visible offset change or a flip.
The framework may reschedule dropped frames rather than maintain exact wall-time
scroll velocity. Therefore count callbacks, offset changes, paints and flips
separately. Multiple animated labels share the frame and do not each necessarily
cause a separate flip.

## 4. Dirty regions, layout and painting

`jiveL_widget_redraw` marks visible widget bounds, with a menu smooth-scrolling
offset adjustment where applicable. It does not invalidate layout origins.
`jive_redraw` stores one bounding union, not an arbitrary list of rectangles.
Separated title, clock and progress updates can therefore expand damage across
unchanged pixels between them. The marked rectangle is the **whole label widget**,
not just the few pixels whose glyph coverage changed.

`_draw_screen` first performs guarded layout checks, then animation. A normal
label scroll callback requests redraw only, so it should not itself invoke text
layout/font rasterization. Actual title changes, skin/bounds changes or unrelated
widgets can still dirty layout. `Label.lua:144` `_setValue` compares values before
calling `reLayout`; native `prepare` frees/recreates text surfaces on real label
layout. A layout **check** count is not a layout **execution** count: previous
instrumentation counted the top-level check and all work beneath it.

Ordinary painting uses `union(current_damage,last_damage)` as a surface clip.
The global background, window backgrounds, group backgrounds and child draw
methods can all be invoked. These calls do **not** imply every screen pixel is
painted: SDL clipping restricts writes. Traversal, tile selection and setup can
still cost CPU outside the clipped pixel area. Transparent layers, alpha blending
and shadows require restoring underlying content; blindly dropping background
painting would leave trails. Window transitions intentionally use full-surface
painting. Standalone screenshots also have full-draw behavior and must be absent
from measurement intervals.

Damage survives through the ordinary paint clip, but is lost at the flip boundary:
`jive_surface_flip(JiveSurface *)` takes no damage argument and iterates full
width/height regardless of clipping. It is therefore incorrect to describe the
entire pipeline as already full-screen painting, or as already region-rotating.

## 5. Radio framebuffer, geometry and bandwidth

Confirmed read-only sysfs: framebuffer `DISP0 BG`, virtual size **240x640**,
16 bits/pixel, **480-byte stride**, kernel rotation 0. These are configuration
readings, not a direct inspection of live SDL surface flags. The landscape skin
requests logical 320x240 at 16 bits/pixel (`QVGAlandscapeSkinApplet.lua:110`).
Jive swaps dimensions before `SDL_SetVideoMode`, keeps the real SDL surface,
and creates a logical software surface with swapped-back dimensions. Source
and destination pitches must both be respected; never assume width*Bpp generally.

The CPU implements rotation in nested C loops. For logical source `(x,y)` of
size `W x H`, physical destination is:

```
dx = y
dy = W - 1 - x
```

This is a 90-degree counterclockwise image rotation in conventional screen
coordinates. A clipped half-open source rectangle `(x,y,w,h)` maps to
`(y, W-(x+w), h, w)`. Use signed/widened arithmetic and clip before conversion;
unsigned underflow at screen edges is unsafe. The code handles 16/32-bit pixels;
unsupported formats need fallback/rejection, not a silent partial copy.

One 320x240x16-bit frame is **153,600 bytes**. Full rotation reads that amount
and writes that amount: at least **307,200 bytes/frame**, or **3.38 MB/s at
11 rotations/s** (decimal units), excluding compositing, cache-line amplification,
write-buffer behavior, locks and scanout. This modest byte count is not a CPU
prediction: strided uncached/framebuffer writes can dominate. For an illustrative
280x24 label, area is 6,720 pixels (8.75% of frame); minimum rotation traffic
would be 26,880 bytes/flip if that were the entire effective repair area. Actual
label bounds/damage have not been measured and may be larger.

`SDL_Flip` swaps hardware pages if SDL_DOUBLEBUF is active; otherwise it requests
a full-screen SDL update, possibly copying a shadow surface first. In fbcon,
`FB_DirectUpdate` is a no-op because memory is already visible; double-buffer
`FB_FlipHWSurface` pans to a page and changes the writable `surface->pixels`
pointer. Virtual height 640 supports two 320-high pages but alone does not prove
the current SDL flags. Fullscreen vs windowed mode also changes requested flags.
Instrument actual driver name, flags, dimensions, pitches, pixel format, shadow
status and destination pointer/page identity once, then pointer changes per flip.

The previous rotation bucket included both surface locks and the loop. Its
roughly 18 ms CPU/flip cannot currently be divided into lock overhead vs copying.

## 6. Profiling accuracy assessment

Original helper uses CLOCK_MONOTONIC wall time and CLOCK_THREAD_CPUTIME_ID,
converts timespec values to double seconds, and accumulates six buckets on the
UI thread. Reporting/reset occurs after a completed screen update and final
flip. There is no normal reset in the middle of a synchronous phase. Double
conversion at these uptimes cannot plausibly create a multi-second error.
The helper does not validate negative deltas, raw timespec normalization,
thread identity or nested standalone draws; these are gaps to address offline.
Long initial frames are reported as long intervals, not five-second intervals.

The invalid interval at age 80.083 reported **4761.528 ms background CPU against
605.623 ms background wall**. The analyzer preserves and flags it and excludes
the entire interval from CPU summaries. It must not be repaired by arbitrary
subtraction. `review-clock-evidence.py` now cross-checks the saved /proc capture:
supervisor-loop samples 70..75 show **7.010 s Jive CPU in 5.690 s wall**, followed
by normal intervals. Loop labels are not exact seconds: samples ran roughly
5.5–5.9 s apart. This independent reader sees the anomaly too, making a
phase-reporting-only bug less likely. No baseline /proc sample was collected
with identical instrumentation/sampling, so overhead remains unquantified.

Important dependence: /proc task user/system totals in this kernel are derived
from `p->se.sum_exec_runtime` (`kernel/sched.c:4306`, `task_utime/task_stime`).
Thread CPU clock uses `task_sched_runtime` through `posix-cpu-timers.c`
CPUCLOCK_SCHED, which adds current execution to that accumulator. These are
different observation interfaces, **not independent underlying clocks**.
Platform sched_clock comes from `drivers/clocksource/mxc_epit_clksrc.c:300`
`mxc_epit_sched_clock`; its 32-bit EPIT cycle reader is extended to 64 bits under
a lock in `mxc_epit_get_cycles:136`. The prepared vendor kernel has EPIT clock
source enabled and HZ=100; exact running-kernel provenance still matters.

Subtracting 2^32 nanoseconds (4294.967296 ms) from the anomalous CPU result yields
466.560704 ms, a plausible value. That is a **wrap/carry clue, not proof**.
EPIT raw cycles wrap at a rate-dependent interval, not necessarily 4.295 seconds.
Do not equate a nanosecond word boundary with a hardware-counter wrap without
tracing conversion/accounting. The saved aggregates lack the individual raw
clock pairs needed to identify the exact failure.

Before accepting finer CPU decomposition, proposed instrumentation should retain
a small bounded anomaly ring with raw before/after wall and CPU timespecs, TID,
phase, frame sequence and process-clock samples. Record resolution from
clock_getres. Track negative/nonmonotonic values and CPU>wall with an explicit
sampling-skew tolerance. Flag aggregate CPU>interval separately. Ensure reentrant
draws have depth tracking so inclusive buckets are not summed twice. Preserve
wall measurements even when CPU data are invalid. Retain stdout/stderr logs and
clock-call counts; no per-frame flash logging.

Cross-check process/task deltas against monotonic elapsed time and the independent
idle-path timer (`/proc/timer_list` if affordable), plus /proc/stat conventional
fields only. RT subtotal fields overlap; never sum them again. A prior .222
accounting investigation is background evidence, not proof of the .223 cause.
Report clock anomalies rather than labeling unexplained residual CPU idle.

Overhead plan: same-source baseline and instrumented binary, same RAM metadata
harness, identical playback/settings and matched A/B sequences. Compare repeated
whole-process CPU and wall responsiveness, not just timed buckets. Include a
diagnostic-disabled run; the disabled helper still has function-call/branch
overhead. Microbenchmark clock reads locally on the Radio only in an approved
bounded RAM probe. QEMU tests validate behavior, not real ARM timing overhead.

## 7. Static versus scrolling test design — approval required

No new replacement/stopping test has been run for this task. Proposed first
approval is **diagnostics only**, not partial rotation or any optimization.

Keep one stable stream running and unchanged across static/scrolling sections:
same codec/bitrate/output rate, volume, feedback-sound setting, artwork, brightness,
screen style and browser polling. Do not reconnect the stream merely to change
the title. Exclude startup/transition and changed-metadata settling time. No
manual knob navigation or remote screenshots during accepted sections.

Deterministic metadata options reviewed:

- StandaloneBase `native/sbbase/src/nowplaying.c:1133` POST
  `/api/nowplaying/update` accepts `track` updates but requires the active session.
  `StandaloneRadio/StandaloneBaseNowPlaying.lua` owns session state and can
  overwrite an injected update on its next publish. Do not claim a new owner or
  assume a public GET exposes an update token. Publishing is a state change and
  belongs in the approved test, not source analysis.
- Lower-risk initial isolation: an explicitly disclosed **RAM-only test harness**
  obtains the existing NowPlaying instance (`AppletManager:getAppletInstance`),
  sets/overrides only its title label at phase boundaries, and restores the
  instance methods/values afterwards. No production Lua file is edited. Use a
  short title such as `Test` and a fixed long ASCII title that exceeds measured
  label width. Hold artist/album/artwork and actual playback state constant.
  This tests the real automatic label/rendering path but does not prove the
  complete upstream metadata-publication path; disclose that limitation. The
  fixture must first be implemented and checked offline, with a narrow
  instance-only hook, safe cleanup and no per-frame allocations.
- If that harness cannot safely attach to the active label, use natural metadata
  only as a documented less-controlled fallback, not equivalent evidence.

Tests A and B: A is static short-title Now Playing with active audio; B changes
only title length to enable automatic scrolling. Include stationary A before and
after each B. Repeat at least two A-B-A sequences per binary and report each
window, variability, observed scroll pauses and actual redraw count. The first
short approved pilot can use one 140-second baseline and one 140-second
diagnostic session with the existing 190-second failsafe; run only the number of
complete settled sequences that fit, and exclude incomplete ones. More windows
or extending bounds requires a reviewed plan, not silently lengthening timers.
The original 120-second profiler budget must be reconciled with warm-up time;
reset phase markers after readiness or fit shorter marked windows, not pretend
startup was a steady baseline. Confirm visual readiness before starting phases.

Collect whole-device conventional CPU, Jive and jive_alsa CPU, all Jive TIDs,
actual monotonic sample spans, callback/visible-step/frame/paint/flip counts,
layout checks vs actual layout executions, label prepare/font-render counts,
background/drawing/rotation/flip CPU+wall, lock/unlock separately, raw/effective
damage coordinates and areas, actual copy area, destination page identities,
and sparse Lua heap/RSS. Query Lua heap via collectgarbage('count') only, not a
forced collection. Keep shared audio RSS ownership caveats. Use fixed bounded
histograms/counters and a small exceptional-record ring, not unbounded logs.

Test C: partial rotation under the same conditions only after separate explicit
approval. It must use the same measurement harness; compare whole-process CPU,
frame cadence/jitter, audio underruns and visual correctness. No CPU-saving
claim from area ratios or rendering-bucket totals alone.

## 8. Existing measurements and their limits

The earlier approved manual-menu test found accepted active intervals around
16.6% layout, 10.2% rotation, 8.5% background and 6.0% drawing (shares of a core).
These are not additional-CPU attribution percentages for Now Playing scrolling.
No static-vs-scrolling Now Playing A/B, instrumentation-overhead A/B or partial
rotation device test has yet been completed. No improvement has been demonstrated.

The first new verification was read-only and confirmed stock checksums and
framebuffer configuration. It did not run the profiler or modify metadata.

## 9. Strategy comparison

| Strategy | Changes / possible benefit | Complexity, risks and compatibility | Required validation |
| --- | --- | --- | --- |
| A: partial rotation | Pass actual paint damage into presentation; copy only accumulated repair area for the destination page. Can remove work proportional to avoidable copied pixels, but fixed locks/flip costs remain; CPU saving unknown. | Medium/high. Page identity, mode changes, clipping, stale pixels and unsupported SDL backends require explicit fallbacks. Preserve public Lua Surface APIs; use an internal damaged-flip entry point and leave generic flips full-frame. | Synthetic coordinate/pitch/page tests, unknown-history fallback, transitions, screenshots, overlays, then approved full-process A/B and visual/audio checks. |
| B: damage propagation | Preserve raw/effective damage across flip and log where unions grow. Later consider several rectangles instead of one union. | Medium. Removing current/previous history breaks back-buffer correctness; separated rectangles can overlap and increase setup work. | Tiny/separate/overlap/full damage, repaint/copy area distributions, per-page history and steady frame cadence. |
| C: background/widget work | Measure clipped pixel work vs traversal; possible cached opaque backgrounds or clip-aware subtree culling. | Medium/high. Alpha, shadows, transparent windows and layer order make naive omission unsafe. Clipped calls are not automatically redundant pixels. | Compare reference pixels across skins, masks, artwork and transitions; count setup vs pixel work and cache memory. |
| D: label/layout | Keep existing cached surfaces; count real layout/prepares and allocations. Avoid unnecessary reLayout only if isolated scrolling actually causes it. | Low for counters; policy changes can be high risk. Menu findings cannot justify skipping geometry updates globally. No need to reduce scroll rate as the primary strategy. | Fixed-title scrolling, metadata changes, fonts, UTF-8, shadows, wrapping, resizing and scrolling handoff/pause behavior. |

## 10. Partial-rotation feasibility and offline model

Separate detached worktree:
`/home/sjoerd/jive-partial-rotation-offline-20261024`, base commit above.
`test/partial_rotation_model.py` is a workstation-only executable model. It is
**not a production patch, compiled Jive variant or performance benchmark**.

The model assumes a complete authoritative logical image and known destination
page identity. Each page starts needing a full repair. Every paint adds damage
to all pages' pending regions; presenting one page repairs its pending union
from the authoritative image, then clears only that page's pending region.
Unknown history forces a full repair. Production code must also invalidate
history on surface recreation, framebuffer mode/pointer changes, transition/full
draws, backend failure and external writes. Lock return/error handling must not
clear history before a successful copy/update.

All **12 tests pass**: mapping corners, one rectangle, multiple separated
rectangles, clipped/empty/edge damage, overlaps, scrolling then quiet swaps,
full-screen then small damage, unknown-history fallback, one/two/three page
histories, 16/32-bit values with padded pitches, 250 seeded randomized updates,
and actual 320x240 geometry. Each presented page is compared against an
independent inverse-coordinate full-rotation reference; padding must not change.

```sh
python3 /home/sjoerd/jive-partial-rotation-offline-20261024/test/partial_rotation_model.py
```

Passing the model does not establish SDL shadow-buffer, framebuffer driver,
cache-coherency, lock or real tearing correctness. The bundled fbcon path attempts
a vsync wait before page panning; preserve that behavior, without claiming its
success or elimination of tearing. Actual
smoothness and visual equivalence still require device A/B and user observation.

## 11. Recommended implementation approach

First complete accuracy guards, counters and the metadata fixture offline.
Then seek approval for an **unoptimized** Now Playing diagnostic A/B. Establish
actual damage/page behavior and extra whole-process CPU before choosing a patch.

If the data justify partial rotation, add an internal damaged-presentation path
for the main screen only. Start with a conservative bounding union and explicit
per-page accumulated history, not a complex rectangle list. Fall back to the
existing full-frame loop for first use, large/unknown damage, unknown pages,
mode changes, unsupported formats/backends and generic Surface flips. Always
retain full-frame behavior behind a simple runtime diagnostic toggle so both
variants share one binary/harness for future comparisons. Do not remove existing
paint history until its correctness role is independently replaced and tested.

## 12. Benefits and remaining uncertainties

Smaller repaired areas may greatly reduce rotation memory traffic while keeping
the existing pixel step and animation cadence. That is an operation-count
opportunity, not an established CPU benefit. Fixed overhead, broader paint unions,
other Now Playing timers, page repair history and layout may reduce gains.
Background/widget optimization should follow measured clipped work, not replace
correct layer composition with assumptions.

Success questions currently answered:

1. Eleven rotations/s during automatic scrolling? **Nominal mechanism only;
   actual rate unmeasured.**
2. Extra Now Playing CPU attribution? **Unmeasured.** Menu buckets are separate.
3. Dirty area smaller than frame? **Likely from label bounds, not yet measured.**
4. Restrict rotation safely? **Feasible under tested page-history assumptions;
   SDL integration remains to be validated.**
5. Whole-process CPU improvement? **Not measured.**
6. Preserve quality/smoothness? **Design preserves cadence/pixels in the model;
   hardware validation pending.**
7. Lowest risk? **Measure first, conservative damaged flip with per-page history
   and full fallback, no persistent install.**

## 13. Approval gate and next steps

Approval requested for the diagnostic-only pilot described in section 7,
**after** offline fixture/instrumentation checks, with unchanged firmware,
original audio binary, original Lua files and watchdog/MSP430 protection. The
operator must understand the two planned UI reloads per bounded session. Reuse
the verified backup, RAM staging, vendor stopwdog transition, separate failsafe,
automatic stock restoration and post-test checksum checks. Keep all test files
and logging off NAND where controllable; ordinary app startup may still write
its normal state. Remove completed RAM staging only after saving evidence.

No optimization deployment is covered by that diagnostic approval. Test C needs
another explicit approval after reviewing source changes and offline tests.

## 14. Diagnostic pilot progress (24 October 2026)

The user approved the diagnostic-only pilot. The first controlled run,
`20261024T100229Z-90f569`, completed its short/long/short/long/short title
sequence with NPO Radio 2 playing. This used the enhanced probe binary with
timing disabled, not an optimization. Title overrides were instance-local,
temporary, and restored on exit; artist/title handoff was held fixed to isolate
the native title scrolling engine. This is not an unrestricted natural-metadata
workload.

After excluding phase transitions and the first three seconds of each phase,
three valid process-accounting windows per title condition gave:

| Condition | Jive CPU | jive_alsa CPU | Accepted duration |
| --- | ---: | ---: | ---: |
| Short, fitting title | 35.4% | 13.9% | 17.56 s |
| Long, scrolling title | 53.0% | 14.1% | 17.91 s |

These point estimates initially suggested a scrolling-specific cost of about 17.6 percentage
points with effectively unchanged audio cost in this run, but the later visual
validation failure below makes that interpretation unconfirmed. They are not measured
optimization gains or precise estimates of natural station-title costs. One
additional long-title interval reported impossible Jive CPU (124.4% of one
core) and was rejected, not corrected. Kernel accounting remains suspect even
with probe timing disabled.

The original Jive was restored automatically, executable checksums matched, and
watchdog PID 709 remained active. Continuous system uptime confirms a UI
restart, not a device reboot. Evidence and analysis are retained under
`C:/Projects/squeeze_userbuild/device-tests/20261024T100229Z-90f569/`.
The matching timing-enabled run `20261024T170143Z-291958` passed both actual-device
headless startup checks and started the bounded RAM session (candidate PID
27250, supervisor PID 27222). A subsequent SSH connection timed out, and ping
reported destination unreachable with no ARP entry. Thus neither fixture
completion nor stock restoration has been verified for this run. The independent
rollback was launched by the supervisor, but its success must not be assumed
without recovered evidence. No further live test should start before checking
the running executable, watchdog, uptime and original checksums. Detailed phase
attribution, damage areas, actual scrolling cadence and probe overhead remain
pending; no measurements from this incomplete recovery are claimed.
Earlier sections describing these quantities as unmeasured reflect the
pre-pilot source investigation; this section records subsequent live evidence.

### Recovery after address change

The operator supplied the new DHCP address `192.168.1.228`. SSH authenticated
against the same previously verified Ed25519 host key (no trust-on-first-use
exception). Recovery showed uptime 292.56 seconds, stock Jive PID 823 with
`/proc/823/exe -> /usr/bin/jive`, audio PID 844 and active watchdog PID 711.
Both original binary MD5s still matched:

```
ee632b3141dbecb2f08467fa73923072  /usr/bin/jive
95fa00abb73ee78ae14a14f0bc06885b  /usr/bin/jive_alsa
```

This confirms a full device reboot, not merely the planned UI restart. The RAM
stage was absent, so the timing-enabled session's phase logs and supervisor
outcome cannot be recovered. Boot messages show a normal kernel startup and
UBIFS journal recovery, but do not establish the reset cause. The available
kernel log is from the new boot; absence of a panic there does not exclude a
previous-boot crash. Neither watchdog reset nor probe-induced failure is proven.
The stock binaries were never overwritten; the reboot returned to stock through
the normal startup path, not a verified execution of the test rollback.

No repeat diagnostic or optimization deployment was attempted. Before another
live attempt, arrange continuous workstation-side log capture (so a reset cannot
erase all evidence) and investigate the test's stability/heartbeat behavior
offline. Serial capture would provide stronger reset evidence. The completed
timing-disabled run lacks confirmation that its title changes were visible; the
timing-enabled attribution and overhead comparison remain incomplete.
Evidence: `recovered-status.txt` and `post-reboot-evidence.txt` in the timing-enabled
run's local directory. All recovery commands were read-only on the Radio.

## 15. Retest with workstation streaming: visual validation failed

The operator approved a retest with browser logging active. Run
`20261024T171424Z-95e2ab` used the same unoptimized timing-enabled executable and
title fixture. A new bounded SSH capture attached **before** stock Jive stopped;
candidate, supervisor and system logs were written continuously to the PC.
Both headless startup checks passed. The fixture completed all five phases,
automatic rollback completed, and recovery confirmed stock Jive PID 2355,
unchanged original binary checksums, watchdog PID 711 and continuous uptime
(527.69 seconds before the test, 738.37 at recovery). Thus the loading screens
in this retest were planned UI restarts, not a full reboot.

Crucially, the operator did **not** see either controlled title or long-title
scrolling: the visible screen retained NPO Radio 2 and its ordinary song title.
Reject this run as a visible-scrolling A/B regardless of its completed phase
markers. The first timing-disabled run used the same fixture without explicit
visual confirmation, so its 35.4%/53.0% contrast must also remain unconfirmed as
scrolling attribution. User observations of natural scrolling/menu load remain
separate evidence; no optimization benefit has been demonstrated.

Inspection of the actual installed `Framework.lua` identifies an insufficient
fixture guard: `isWindowInStack` means only membership, whereas
`isCurrentWindow` checks the top/current window. The former allows a covered
Now Playing window to pass readiness. This is a concrete harness defect, not yet
proof that it is the only cause of the visible-widget mismatch. The local entry
fixture now requires the current window at readiness and throughout the run;
that change has **not** been deployed. Active window/widget ownership and any
custom Now Playing implementation still need identification before a new test.

The saved native log independently confirms a 320x240 16-bit logical source
(pitch 640), 240x320 destination (pitch 480), destination flags 3221225505 and
page pointer changes. These are backend observations only, not title-scrolling
performance results. Later probe intervals often show only five paints per
five seconds despite title animation callbacks, consistent with the failed
visual validation; global counters cannot identify the visible label alone.

Raw evidence, original backups and descriptive parser output are retained in
`C:/Projects/squeeze_userbuild/device-tests/20261024T171424Z-95e2ab/`.
Parser summaries are descriptive only and must not override the failed visual
check. Installed UI source copies are in `reference/installed-ui/` under the
build-control workspace. No optimization or further live test was deployed.
