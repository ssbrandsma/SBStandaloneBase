# Jive scrolling: end-to-end source review

24 October 2026. Source analysis and host-only experiments; no Radio access,
service changes, executable rebuild/deployment or firmware changes in this review.

## Outcome

There are concrete opportunities to reduce work without slowing scrolling:
make tiled painting clip-aware, improve rotation memory access, and eventually
carry damage through to a page-aware partial-rotation path. Avoiding needless
text-surface regeneration is a separate improvement for real layout changes,
especially menu movement and metadata updates. It is not the ordinary scroll tick.

The biggest proven operation-count amplification is full-frame rotation after
small changes. It is **not yet proven to be the largest CPU cost of visible Now
Playing scrolling**. The controlled title fixture failed visual validation;
its CPU contrasts cannot establish that attribution or predict savings.

## Source and evidence

Native and bundled SDL source is pinned to public SqueezePlay revision
`031ea8c9d75d2230355df5c3317088ea85874611`. It is the closest inspected
pre-firmware source, not proven identical to the installed executable.
The installed Label, Widget-related UI and NowPlaying source copies and saved
device logs supplement it. No current GitHub HEAD assumptions are needed.

The case-sensitive WSL environment was previously unavailable. This review uses
a separate Windows cache of selected pinned files, not a checkout of the whole
repository: `C:/Projects/squeeze_userbuild/reference/scroll-audit-031ea8c/`.
No WSL reset or Radio intervention was needed.

Source references below are under `src/squeezeplay/`, except SDL paths under
`src/SDL-1.2.13/`. Key primary sources:
[label](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/031ea8c9d75d2230355df5c3317088ea85874611/src/squeezeplay/src/ui/jive_label.c),
[framework](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/031ea8c9d75d2230355df5c3317088ea85874611/src/squeezeplay/src/ui/jive_framework.c),
[surface/presentation](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/031ea8c9d75d2230355df5c3317088ea85874611/src/squeezeplay/src/ui/jive_surface.c),
[widget invalidation](https://github.com/ralph-irving/squeezeos-squeezeplay/blob/031ea8c9d75d2230355df5c3317088ea85874611/src/squeezeplay/src/ui/jive_widget.c).

## End-to-end path and avoidable work

```text
NowPlaying metadata/handoff timer -> Label:animate(true)
  -> register animation, reset offset/start pause
Framework frame -> guarded layout check -> animation countdown
  -> advance offset -> redraw visible label bounds
  -> union current/previous damage -> clip logical software surface
  -> paint background + traverse window/groups + cached text alpha blits
  -> lock surfaces -> rotate ENTIRE logical image into writable display page
  -> unlock -> SDL_Flip -> framebuffer page pan / next writable page
```

### 1. Scheduling and offset changes

`share/jive/ui/Framework.lua:304` schedules nominal 22 Hz frames (45 ms after
flooring). `share/jive/ui/Widget.lua:484` registers an animation handle once;
`src/ui/jive_framework.c:485` decrements its frame counter. The label requests
half the framework rate, so one callback normally runs every two updates.
Frame drops reduce actual cadence; callbacks do not imply pixels changed.

`src/ui/jive_label.c:295` advances a font-sized step, handles start/repeat pauses
and wrap, then invokes `reDraw`. This path does not allocate text surfaces or
call the font renderer. A normal moving label needs that redraw: shifting only
the newly exposed strip would leave the other glyph pixels at their old positions.

Two small lifecycle inefficiencies deserve separate fixes:

- `animate(true)` resets `scroll_offset` **before** checking the existing handle
  (`:346`). Repeated enables restart the pause even without creating a handler.
  `_updateTrack` (`NowPlayingApplet.lua:897`) enables animation after every
  update, even though equal text values are already deduplicated by Label.
  Deduplicate unchanged metadata/handoff operations at the caller; do not change
  restart semantics globally without auditing callers that deliberately restart.
- Fitting text invokes its stop callback but does not itself unregister animation.
  The normal NowPlaying handoff may stop it; labels without a stop callback can
  keep receiving no-op callbacks. Optimizing registration must preserve callback
  timing, visibility lifecycle and restart when bounds/text later become wider.
  This is a small dispatch saving, not evidence of the main CPU problem.

Do not lower the global framework rate as the first fix: it changes menu motion,
timers, responsiveness and visible scrolling speed/smoothness.

### 2. Layout and text allocation

`src/ui/jive_widget.c:424` guards layout by invalidation origins. A redraw does
not request layout. `share/jive/ui/Label.lua:144` also avoids relayout on equal
values. Thus repeated rasterization is **not inherent to steady scrolling**.

However, every actual `jiveL_label_layout` (`jive_label.c:258`) calls `prepare`
unconditionally. `prepare:138` frees all cached lines, allocates/grows the line
array with `realloc` per line, and renders foreground plus optional shadow again.
`jive_font.c:233` uses `TTF_RenderUTF8_Blended`. Geometry-only changes can therefore
regenerate unchanged text. Menu scrolling can move widgets through `setBounds`,
which calls `reLayout` on changed geometry. How often this happens in a specific
menu still depends on its implementation.

Improvement: separate text/style preparation from alignment/geometry layout.
Keep existing per-label surfaces until rendered text or relevant style changes.
The cache key must cover rendered string (not just value-object identity),
font/style generation, per-line format, foreground/shadow and line metrics.
Recompute alignment, available width and scroll eligibility on bounds changes.
Language/theme changes and mutable string-token objects must invalidate it.
Retain a per-label bounded cache rather than an unbounded global title cache.

Other allocation observations:

- Text surfaces can be much wider than the visible label; caching every scroll
  position or duplicating a precomposed full title would worsen memory pressure.
- Line arrays grow per line, and `alloca(strlen(text)+1)` reserves the full input
  on the stack. `MAX_CHARS` limits a rendered chunk, not the entire input. A bounded
  input policy and one-pass capacity allocation are useful hardening, not measured
  explanations of ordinary single-line scrolling load.
- Group layout allocates width/border arrays and sometimes an ordered Lua table
  (`jive_group.c:101`); these are layout costs, not per-tick text allocations.
- Window/group drawing creates captured C closures through `lua_pushcclosure`
  (`jive_window.c:260`, `jive_group.c:301`), adding transient Lua objects whenever
  those containers draw. Consider a shared static visitor plus context arguments
  if profiling justifies it; a single global context would break nested draws.

There is also a concrete stale-state case: `prepare` frees lines then returns
early for an empty string without resetting `text_w/text_h/scroll_offset`.
`jive_label_gc_lines` resets line count/storage, not those dimensions. Clearing
a formerly long label can leave stale preferred dimensions and scroll eligibility
even though no text remains. Reset these fields on the empty path; add a
long-to-empty-to-short regression test. This is source-established, not evidence
that the Radio's visible title was empty during earlier spikes.

### 3. Redraw and dirty-area amplification

`jive_widget.c:355` marks the whole visible widget bounds. `jive_framework.c:664`
stores one bounding union. Distant clock/progress/title updates can inflate that
union. `_draw_screen:427` paints the union of current and previous damage.
Transparent windows and shadows need correct underlying pixels restored.

**Do not blindly remove unchanged-bounds redraws.** `setBounds:37` redraws old
bounds before its equality check. That is repeated work, but Label value changes
only request layout, and layout/parent bounds-setting can supply the repaint.
A no-op-bounds early return can therefore suppress required content updates.
First make content/geometry repaint obligations explicit and test unchanged-size
text changes, skin changes, layout roots, hiding and moving widgets.

### 4. Painting: clipping does not eliminate all CPU work

Window/group drawing visits children before SDL can reject their pixel writes.
Background tiles can be resolved and images loaded before a later clipped blit.
Text is blitted from cached surfaces, with foreground/shadow and optional wrapped
copies; SDL handles source/destination clipping and alpha blending.

The concrete loop issue is `jive_surface.c:645`, `blit_area`. It walks the entire
requested tiled area, repeatedly calling SDL even for tiles wholly outside the
destination clip. `SDL_surface.c:444`, `SDL_UpperBlit`, rejects them, but only
after each call/setup. The visible output is clipped; the tile enumeration is not.

Improvement: intersect the destination area with the clip and enumerate only
intersecting tiles. Preserve the **original tile origin**, last partial tile,
source dimensions and destination offsets. Starting a fresh tile pattern at the
clip edge would visibly shift the background. Keep SDL's own clipping for edge
tiles. Zero-size source images need safe rejection rather than an infinite loop.

Next, consider conservative off-clip early-outs for drawing-only native leaf
paths, preserving side effects, shadows/overhang, offsets and transparent layers.
Generic subtree culling is riskier: parent bounds need not contain everything a
custom child draws. Avoid assumptions that all draw methods are side-effect free.

### 5. Rotation and actual presentation

`jive_surface_flip:1192` ignores damage and the source clip. It copies 76,800
pixels for a 320x240 screen on every presented frame. In the current loop, source
reads are contiguous but successive destination stores are one physical pitch
apart (480 bytes for the captured 16-bit mode). This is poor write locality;
on framebuffer memory it is a credible optimization target, not a demonstrated
hardware bottleneck.

Two complementary options:

1. **Keep full-frame rotation, improve traversal.** Process small blocks or
   contiguous destination rows, trading cached source reads against contiguous
   framebuffer stores. Try 8/16/32-pixel blocks and retain a scalar fallback.
   This preserves full-page initialization and avoids dirty-history complexity.
   Benchmark on the actual ARMv5/framebuffer before choosing a winner; a desktop
   memory-copy benchmark cannot predict it. Preserve locks, pitch, mapping,
   format checks and presentation order. ARMv5 has no NEON assumption to exploit.
2. **Rotate only required page repair regions.** Pass damage from framework to a
   main-screen-specific presenter. Keep separate pending damage per destination
   page; union each frame's modifications into every page's pending state, then
   repair/clear only the writable page. Unknown pages, mode changes, transitions,
   format changes, lost history and failures require full fallback. A conservative
   bounded union is a reasonable first implementation; disjoint rectangles can
   follow if union inflation is actually costly. Retain existing paint behavior
   initially rather than combining two correctness-sensitive changes.

The successful-but-visually-invalid diagnostic run still provides backend facts:
destination flags **3221225505 = 0xC0000021**, including `SDL_HWSURFACE`,
`SDL_DOUBLEBUF`, `SDL_FULLSCREEN` and `SDL_NOFRAME`; writable pointers changed.
Double buffering is therefore observed, not merely inferred from virtual height.
Current-title-only repair without page history is unsafe.

A 270x31 rectangle covers 10.9% of the logical frame. That is an illustration
using captured label bounds, **not proof of the visible title's actual repair
area or a prediction of 89.1% CPU savings**. Current/previous paint damage,
other widgets, page repair, locks, alpha painting and fixed overhead remain.

Bundled fbcon `FB_FlipHWSurface:1318` calls `wait_vbl` then `FBIOPAN_DISPLAY`
and changes `pixels`. `FB_WaitVBL` attempts `MXCFB_WAIT_FOR_VSYNC`. Thus the
source does attempt a vsync wait; its success/timing on the device was not
measured. Retain it. Vsync wait is wall time, not automatically CPU consumption.
`SDL_Flip` may also copy a shadow surface on other configurations; do not apply
Radio-specific assumptions to every SDL backend.

## Offline validation performed

These tests compile extracted original functions with Windows GCC and mocks;
they do not build or run Jive and do not contact the Radio.

| Test | Result | What it establishes |
| --- | --- | --- |
| Original `blit_area` + actual bundled `SDL_UpperBlit` clipping vs clip-aware enumeration | 4,003 cases pass | Same destination pixels and effective-blit counts under a deterministic lower-blit model, including negative origins, empty clips, tile edges and random cases |
| Synthetic 320x240 tiled background, 8x8 tiles, 270x31 clip | SDL calls 1,200 -> 170; same 170 effective blits | Avoidable dispatch work exists; not an estimate of the installed skin's tile sizes or CPU saving |
| Original `jive_surface_flip` loop vs 10 blocked/store-order alternatives | 48 geometry/format/pitch cases pass | Exact bytes including pitch padding match an independent inverse-coordinate reference for 16/32-bit host buffers |

Scripts: `C:/Projects/squeeze_userbuild/test-scroll-source.py` and
`test-scroll-rotation.py`. Results: `logs/scroll-source-tests.json` and
`logs/scroll-rotation-tests.json`. Tile test records source SHA256s.
These are not full SDL integration, ARM performance, tearing, alpha-blitter,
watchdog or page-history validation. Existing earlier page-history model tests
are documented separately in the investigation report.

## Recommended implementation order

1. Fix empty-label stale state with a narrow regression test; independent of
   performance claims.
2. Implement clip-aware tile enumeration as an isolated native patch, preserving
   existing pixels, scrolling cadence, Lua interfaces and full-frame presentation.
3. Separately implement/test full-frame rotation store-order alternatives; choose
   using an approved hardware comparison, not desktop timing.
4. Separate text preparation from geometry layout, with explicit cache invalidation
   and visual tests. This targets allocation/layout churn rather than assuming
   that every scroll tick rasterizes fonts.
5. Add page-aware partial rotation after full fallback and synthetic/history tests.
   It has the greatest small-damage operation-count opportunity and more risk.

Do not bundle these into one unexplained performance patch. Do not globally stop
animations, drop background restoration, remove required layout redraws or slow
scrolling to make the CPU graph smaller. A future device test needs a confirmed
visible target and off-device logs, but source fixes and offline tests do not
depend on repeating the failed title fixture first.

## Selected follow-up: user-requested 3 fps combination

The user subsequently requested combining coarser text animation (target 3 fps)
with the rotation memory-access improvement. An isolated source candidate is at
`C:/Projects/squeeze_userbuild/candidates/scroll-3fps-v1/`; its README records
controls, tests and limitations. It uses integer phase accumulation to avoid the
framework's fractional scheduling truncation and carries fractional pixels to
preserve nominal scrolling speed. The framework itself remains at 22 Hz; visible
text intentionally moves in larger, less frequent jumps.

The candidate combines this with scalar 16x16 blocked **full-frame** rotation,
not page-aware partial rotation. Source-function host tests pass for 3/11-fps
cadence and both blocked/legacy rotation branches. This is source preparation,
not a complete ARM build or deployment, and no CPU benefit is measured. A
73% reduction in sustained scrolling redraw requests (3 vs 11) must not be
reported as a 73% reduction in whole-device CPU use.

The user subsequently approved the ARM build and RAM-only hardware test. The
build, ABI comparison, ARM-emulated source-function tests and actual-library
startup check passed. Run `20261024T181358Z-2e8886` completed and restored stock
Jive with matching original binary checksums and continuous uptime. The user
confirmed normal playback/controls, but found no long enough text to observe
scrolling. Therefore visual cadence/readability and CPU gains remain unverified.
The candidate's `arm-results.md` records details and the saved binary checksum.
