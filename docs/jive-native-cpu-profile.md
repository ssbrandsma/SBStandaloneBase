# Jive native profiling on the Radio

Investigation performed 24 October 2026; analysis/restoration continued after midnight on 25 October (Europe/Amsterdam). Device: 192.168.1.228, 400 MHz ARMv5, Linux 2.6.26.8-rt16.

## Result

Native statistical sampling worked using **per-thread CPU timers**, after OProfile's timer backend initialized but delivered zero samples. This gives actual interrupted instruction addresses, not just `/proc` CPU totals or a source-code hypothesis.

In the NPO Radio 2 capture, the main-thread hot paths were full-frame presentation/rotation, pixel conversion/blitting, and solid rectangle filling. The decoder thread predominantly executed libmad MP3 decoding. This supports prioritizing rendering work before speculative audio or allocator rewrites.

This is **not** a controlled idle-versus-scrolling comparison, a measured optimization gain, or a proof that a static screen should consume a particular percentage of CPU. A quiet-menu/non-scrolling profile remains a separate useful control.

## Captures

Both builds retain the existing three-steps-per-second scrolling and blocked full-frame rotation candidate. They add diagnostic sampling, not new rendering optimizations.

| Capture | Scene | Main samples | Decoder samples | Drops |
| --- | --- | ---: | ---: | ---: |
| v1, 60 s | User-confirmed Italian station, text scrolling | 328 | 0: signal blocked | 0 |
| v2, 60 s | NPO Radio 2; scrolling confirmed about 12 s into capture | 321 | 238 | 0 |
| v2, post-confirmation subset, about 48 s | Text scrolling confirmed | 256 | 190 | 0 |

The subset excludes the first 113 binary records. At the confirmation marker the sample file was 1808 bytes (16 bytes/record). The cutoff is a near-time file-size snapshot, not a per-sample timestamp; it does not establish that scrolling remained continuous for every subsequent frame.

### Main thread: confirmed-scrolling subset

All percentages below are **fractions of this thread's samples**, not percentages of total CPU.

| Location | Samples | Fraction | Interpretation |
| --- | ---: | ---: | --- |
| `jive_surface_flip` | 66/256 | 25.8% | Direct samples in Jive's full-frame rotation/presentation function; not inclusive time in called SDL functions |
| All SDL library code | 66/256 | 25.8% | Includes pixel conversion/blitting; some internal functions are stripped |
| `boxColor` | 20/256 | 7.8% | SDL_gfx filled-rectangle code; separate from the SDL library row |
| `luaV_execute` | 14/256 | 5.5% | Lua VM itself; additional Lua table/call/GC work appears in separate functions |

The two most concentrated SDL address ranges were:

- ELF-relative `0x246b4` and nearby instructions: 32-bit pixel loads, masks `0x00f80000`, `0x0000fc00`, `0x000000f8`, shifts, and 16-bit stores. This directly demonstrates RGB888-to-RGB565-style conversion. The available SDL source contains the corresponding conversion operation, but the stripped installed image does not expose the internal function name, so the machine-readable report leaves it unresolved rather than substituting SDK symbols.
- Around `0x1fb70`: 32-bit pixel reads, alpha-bit tests and branches, with 16-bit destination accesses in the surrounding routine. This is consistent with an alpha-blit pixel loop; exact caller/asset attribution is not established by flat samples.

Disassembly of `boxColor`'s sampled addresses shows the 16-bit solid-fill store loop, not font rendering or text rasterization. Source inspection shows a route from `_blit_tile` for `TILE_FLAG_BG` into `jive_surface_boxColor` and then `boxColor`, but there are other callers: the flat profile does **not** prove which widget or background accounts for each sample.

The first, different-station capture independently showed similar hot paths: 19.8% `jive_surface_flip`, 14.0% `boxColor`, and 18.6% in SDL overall. It is supporting evidence, not a matched A/B result.

### Decoder thread: confirmed-scrolling subset

- 151/190 samples (79.5%) were inside the exact installed `libmad.so.0.2.1` image.
- 22/190 (11.6%) were in Jive's `decode_mad_callback` itself, **excluding** time in called libmad routines.
- Remaining samples included copying and synchronization.

Thus most sampled decoder work is real MP3 processing, not a demonstrated busy idle loop. This capture does not separately profile the `jive_alsa` process and does not justify changing its mixer/output behavior.

## Method and validation

- Temporary diagnostic-only Jive build from the already tested scrolling candidate; exact ELF files retained per session.
- Each selected thread creates a `CLOCK_THREAD_CPUTIME_ID` POSIX timer with Linux `SIGEV_THREAD_ID`, delivering `SIGPROF` specifically to that thread every requested 40 ms of thread CPU time. A process-directed timer was deliberately avoided because it can deliver to the wrong thread.
- Before device integration, a four-second standalone two-thread test produced 16 records from each thread. Symbol lookup mapped sampled test addresses to the actual busy-loop source.
- SIGPROF handler records four 32-bit words: magic, TID, interrupted ARM PC and CPSR. No allocation, stdio, Lua API, locks or stack unwinding in the handler; it saves/restores errno and writes one bounded record to a nonblocking FIFO.
- FIFO is opened read/write so a disconnected external reader cannot kill Jive with SIGPIPE. Writes can be dropped rather than block; drop counts are reported. No records were dropped in either capture.
- Reader stores samples on the second UBIFS volume. Installed images were copied read-only to the workstation; ELF load segments and captured process maps translate PCs to image-relative addresses. Only actual function symbol ranges are used, not guessed nearest exported names.
- Profiling starts only after a control file is created. It automatically disarms after 60 wall-clock seconds, with a 2000-record cap per thread. The UI session additionally has a 300-second lifetime and independent 330-second rollback guard.
- Original ABI exports, loader, required library versions and ARM attributes were checked; ARM-emulated and real-device eager-binding/headless startup checks passed.

In v1, SDL's worker signal mask blocked SIGPROF. `/proc/15120/task/15154/status` showed SIGPROF both blocked and pending. V2 explicitly unblocks only the private profiling signal in the instrumented threads; `/proc` confirmed it cleared in the decoder, and samples from that thread then arrived. No other worker signal masks were changed.

### Limitations

- Flat user-space PC sampling, not a call graph, kernel profile, hardware-cycle profile or Lua source-line profile.
- This old PREEMPT_RT kernel has previously demonstrated CPU-accounting anomalies. CPU timers use that kernel's accounting too; their scheduling/delivery is not an independent precision CPU meter. Deferred signals, coalescing and tick aliasing can bias samples.
- Only a few hundred samples per thread. Treat percentages as approximate hotspot evidence, not precise timing or an unbiased confidence interval.
- Instrumentation and sample writing introduce unmeasured overhead. Some read-only library copying/SSH monitoring occurred during the second capture. Neither capture is a clean wall-clock performance benchmark.
- A stripped shared library may resolve only to its image and an offset. No locally rebuilt library's debug symbols were applied to a non-identical installed library.
- Allocator/GC samples exist, but this does not measure allocation counts, retained objects or a leak. No memory-leak conclusion follows.

## Safety, restoration and evidence

Neither `/usr/bin/jive` nor `/usr/bin/jive_alsa` was replaced. Their recorded MD5 values remained:

```
ee632b3141dbecb2f08467fa73923072  /usr/bin/jive
95fa00abb73ee78ae14a14f0bc06885b  /usr/bin/jive_alsa
```

Watchdog PID 711 remained running and uptime advanced across the tests. The loading screens during controlled transitions were Jive UI restarts, not full Radio reboots. Both profiling sessions completed their rollback to the original startup path. Restoration of the non-profiling, until-reboot 3-fps test is recorded separately in the latest device-session manifest.

Final restoration verified: Jive PID 20569, audio PID 20592, watchdog PID 711, uptime 17805.84 seconds; no SIGPROF handler installed in the restored Jive. The active supervisor/session is `/tmp/jive-render-test-20261024T220015Z-e99ab1`. It reuses the original verified RAM executable through a symlink rather than allocating another RAM copy: `/tmp/jive-render-test-20261024T182508Z-c8e8b3/scroll3-long/bin/jive`. **Do not remove that older stage while this new session runs**, even though its original supervisor has completed. The candidate SHA256 remains `9b0367e0e0cdd36d15220771d07db4df9842e988a91b6e2bd07c911e24343bf2`. Reboot still returns to installed stock Jive; no startup configuration was changed.

Profiling binaries and their small raw sample files remain in their uniquely named second-volume staging directories for reproducibility; they are not active and have no boot-time hook. Workstation copies are preserved. No user data was deleted.

Workstation workspace: `C:/Projects/squeeze_userbuild`.

- v1: `device-tests/20261024T202004Z-923b8e/`.
- v2: `device-tests/20261024T202449Z-e6431d/`.
- Each includes `samples.bin`, `capture-maps.txt`, exact `profile-jive`, library archives, machine-readable `native-profile.json`, readable `native-profile.md`, and streamed supervisor/candidate logs.
- v2 also includes `native-profile-scrolling.json` / `.md` for the marked subset.
- Diagnostic implementation/build: `jive-prof.c`, `jive-prof.h`, `cpu-prof.patch`, `build-cpu-prof.sh`.
- Decoder: `analyze-cpu-prof.py`.
- OProfile failure and kernel-hook hypothesis: `oprofile/pilot1-report.md`.

v1 executable SHA256: `c2e4031aace408d02a1960fa519a1d9667b4c1b4ae4c0dc7d05e24436ae0e2f2`.

v2 executable SHA256: `7d3573803d8bcebd3ad71b1f8980aedd7c0ba305542bb1290a2de21e9b8f591a`.

## Recommended optimization order

1. Measure presentation count, dirty area and invalidation causes in a quiet scene versus scrolling. Then reduce unnecessary redraws/full-frame presentation while retaining correct framebuffer-page repair. Do not assume the current blocked rotation is already optimal.
2. Identify repeatedly converted source surfaces and cache them in a suitable destination format where alpha/color semantics permit. Do not globally force all assets to RGB565: transparent assets still need correct blending.
3. Consider replacing opaque filled rectangles with a clip-correct accelerated/optimized SDL fill path, with pixel-equivalence tests and separate treatment of translucent fills. Profile first to identify the dominant callers.
4. Profile Lua source activity/allocation only if it remains significant after rendering work is reduced. The current evidence does not make audio decoder replacement or generic allocator rewrites the first priority.

No additional production optimization was implemented as part of this profiling run.
