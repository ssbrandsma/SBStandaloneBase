# Jive native performance source audit

Investigation only, 2026-10-24. No production code or device configuration changed.
Companion to [the runtime investigation](jive-performance-investigation.md).
Follow-up: [isolated Now Playing rendering investigation](jive-now-playing-rendering-investigation.md)
distinguishes automatic label animation from manual-menu layout costs, reviews
CPU-clock accuracy and documents the approval-gated next test.

## Leading finding: Now Playing scrolling amplifies framebuffer work

The user reports CPU rising specifically while Now Playing text scrolls. This
is valuable state-correlated evidence, but not yet a controlled per-thread A/B
measurement. It makes UI rendering the leading explanation for the *additional*
load, not for all audio-playback CPU.

Source chain in `ralph-irving/squeezeos-squeezeplay`:

1. `src/squeezeplay/src/ui/jive.h:23` sets the framework rate to 22 FPS.
   `src/squeezeplay/src/ui/jive_label.c:12` requests half that rate for label
   scrolling: nominally 11 scroll updates/second. Actual scheduling is subject
   to frame timing and load. `share/jive/ui/Widget.lua:484` converts this into
   an every-other-framework-frame callback.
2. `jive_label.c:296` advances the scroll offset and calls `reDraw()` when the
   text is wider than its label. Pause/initial-delay steps do not all redraw.
   `jive_widget.c:355` marks the visible widget's bounds dirty.
3. `jive_framework.c:~540` unions the current and previous dirty rectangles,
   clips the rendering surface, redraws background and invokes the screen's
   draw method. This is not proof that every widget's pixels are repainted:
   clipping limits pixel writes, although drawing/traversal work still occurs.
4. `jive_framework.c:~650` flips the surface whenever drawing occurred.
5. `jive_surface.c:1192`, under `SCREEN_ROTATION_ENABLED`, walks **every pixel
   of the surface**, copying it to a rotated position in the real framebuffer,
   then calls `SDL_Flip(real_sdl)`. It does not receive or use the dirty rectangle.
   Both 16-bit and 32-bit paths have nested full-surface loops with strided
   destination writes.
6. The Radio (`baby`) build recipe explicitly enables this path:
   `poky/meta-squeezeos/packages/squeezeplay/squeezeplay_svn.bb:41` in the local
   squeezeos tree adds `--enable-screen-rotation`.

Consequently, a moving label can cause approximately eleven full-frame rotation
copies each second while scrolling, in addition to clipped compositing and
SDL/backend flip work. For a 320x240 surface this is about 845,000 pixel copies
per second at 11 flips/s, before other animations or backend work. This is an
operation-count illustration, **not a measured CPU cost**. The strided memory
access and framebuffer characteristics matter; raw bytes alone do not predict
CPU usage. The recipe and reviewed source establish intended build behavior,
not disassembly-level proof of the installed binary's exact build options.

### What the scrolling path does not do

`jive_label.c:138` prepares text surfaces during layout. Scrolling changes the
offset and blits clipped sections of those existing surfaces (`:391`), including
an optional shadow and a second wrapped copy near the end. It does not call the
font renderer or allocate a fresh text surface on every scroll step.

`Label.lua` also checks whether the value changed before requesting relayout,
and native widget layout is guarded by invalidation origins. Repeated identical
label values are therefore already deduplicated. Actual text/layout changes do
free and recreate cached line surfaces, using `TTF_RenderUTF8_Blended` through
`jive_font_draw_text`; that is a different path from steady scrolling.

Now Playing already has normal, scroll-once and no-scroll preferences
(`share/applets/NowPlaying/NowPlayingApplet.lua:318`). Its title/artist animation
handoff and pauses are consistent with CPU rising and falling as text moves.
No preference was changed during this audit.

## Other reviewed native paths

| Finding | Activation and significance |
| --- | --- |
| Decoder greedily fills available output FIFO | `audio/decode/decode.c:~360–450` selects zero-delay decoding while input and output capacity permit. This offers a plausible startup/buffering spike mechanism, separate from recurring scrolling spikes. Steady codec work is not automatically unnecessary work. |
| Decoder callback result ignored by outer loop | Same loop calls the callback without using its return value. A no-progress busy loop is a conditional concern only if running state, sufficient input and output space all persist. No such loop has been demonstrated on the Radio. |
| ALSA scalar sample conversion/gain | `decode_alsa_backend.c:~280–487` performs per-frame conversion and gain multiplication in the output path; stereo 48 kHz means 96,000 sample operations/s. Unity-gain specialization is worth benchmarking, but conversion and normal volume handling remain necessary. No CPU saving is established. |
| Capture buffer capacity not updated | In `decode_alsa_backend.c:~498`, `capture_read` frees/reallocates `cbuf` when `cbuf_size` is too small, but the file never assigns the new capacity to `cbuf_size`. This is a concrete missing-capacity-update defect: repeated positive-length capture calls can churn the allocator. It is a capture/line-in path, not established as active during network-radio playback. Old buffers are freed; this is not by itself an accumulating leak. |
| HTTP response header reallocations | `audio/streambuf.c:~614` grows the response-header buffer with `realloc` as fragments arrive, with no hard header-size cap in that path. Large/fragmented headers can cause unnecessary copying and allocation. After headers complete, normal audio reception feeds the existing compressed FIFO; it does not repeatedly grow this header buffer for every audio packet. Disconnect frees the header storage. |
| Large shared audio allocation | The output FIFO is sized for ten seconds at stereo 44.1 kHz using 32-bit samples, roughly 3.36 MiB, alongside effect/state storage. The observed shared mapping in Jive and jive_alsa must not be counted twice as private application memory. A fixed buffer is not evidence of a leak. |

## Best next validation and optimization order

### Subsequent user tests and diagnostic preparation

The user subsequently paused playback and scrolled menus: CPU fell near idle
after pause, then approached 100% while navigating. A screenshot showed Jive
55.5%, jive_alsa 37.7%. Disabling navigation feedback sounds left jive_alsa near
10% while Jive still rose during menu scrolling (35.2% in a later screenshot).
These are separate observed snapshots, not matched interval averages. They
support distinct UI and sound-feedback costs, without proving rotation dominates
the UI work. Browser-unfocused intervals are missing measurements: straight
graph connections across them are not measured trends.

At the user's request, a bounded rendering-timing patch has been prepared in an
isolated pre-firmware source worktree. Host and emulated ARM tests pass, and all
changed C units compile to ARMv5 objects. **No complete Radio-compatible Jive
binary is built yet:** a matching legacy userspace SDK/sysroot is missing.
This missing-build state was subsequently resolved: the isolated workspace at
`C:\Projects\squeeze_userbuild` now contains linked baseline and diagnostic
executables. Both pass headless ARM emulation with eager symbol resolution;
comparison with saved Radio Jive shows identical ARM attributes/loader, no new
direct library or version requirements and no missing original dynamic exports.
At that stage, actual device-library and hardware rendering/audio checks remained pending.
Subsequently, user-approved RAM-only testing passed native-library/headless
startup on the Radio; the user confirmed baseline menu responsiveness. The
diagnostic measured substantial layout, background, drawing and full-frame
rotation costs during manual navigation, with zero flips in stationary intervals.
Accepted active intervals averaged about 17.9 ms CPU per rotation; layout was
the largest CPU bucket. One impossible CPU-versus-wall interval was excluded,
so percentages remain approximate. Original `/usr/bin/jive` was restored and
its checksum verified unchanged; no system reboot occurred. Full evidence and
limitations are in `C:\Projects\squeeze_userbuild\device-results.md`.
The earlier preparation changed nothing on the Radio; the later RAM-only test
restarted the UI temporarily but made no persistent executable replacement. See the
[patch, tests, build gate and proposed test procedure](../tools/jive-render-diag/README.md).

Keep the stream, volume, artwork, browser polling and other state unchanged.
Have the user alternate existing Now Playing no-scroll and normal scrolling
preferences, allowing stable intervals for each, and sample main-thread,
decoder-thread and jive_alsa CPU separately. Repeat the order to avoid startup
buffering or metadata changes masquerading as a scrolling effect. This requires
no rebuilt kernel or UART access. It has not yet been performed here.

If scrolling mainly raises the main thread while decoder/audio stay stable,
measure animation, drawing and flip/rotation separately in a future approved
instrumented build. Existing screen timing in `jive_framework.c` reports layout,
animation, background and drawing but is taken **before the later screen flip**;
it can therefore miss the expensive rotation suspected here. Instrumentation
must include `jive_surface_flip`, not just `_draw_screen`.

Candidate changes, **not implemented**, in order:

1. Reduce unnecessary flips during text animation, or offer a lower-rate/scroll-
   once policy as a usability tradeoff. Current preferences permit an immediate
   user-controlled experiment without modifying software.
2. Investigate rotating/updating only damaged regions, with correct coordinate
   transformation and backend/double-buffer synchronization. Existing union with
   previous damage is important; simply copying one current label rectangle
   could leave stale pixels. Benchmark the actual SDL framebuffer backend.
3. Benchmark a cache-friendly tiled full-frame rotation if partial updates are
   incompatible with the backend. Do not assume a faster loop without measuring.
4. Fix and test capture capacity bookkeeping independently if capture is used.
5. Investigate allocation/decoder concerns only against evidence that those paths
   are active and costly; do not conflate them with the scrolling observation.

Source reviewed at public/9.0 commit
`766b6a8b3c3c7722d586125b068b47facf9d4a77`. The closest inspected pre-firmware
candidate is `031ea8c9d75d2230355df5c3317088ea85874611`; differences under the
reviewed native and core Jive Lua trees are limited to a PCM sample-rate lookup
change. Exact installed native-binary provenance remains unproven. References
are relative to `src/squeezeplay/` unless a full repository-relative path is
given. Approximate line references are marked with `~`.

Public source: [native Jive/SqueezePlay](https://github.com/ralph-irving/squeezeos-squeezeplay/tree/766b6a8b3c3c7722d586125b068b47facf9d4a77/src/squeezeplay),
[Radio build recipe](https://github.com/ralph-irving/squeezeos/blob/master/poky/meta-squeezeos/packages/squeezeplay/squeezeplay_svn.bb).
