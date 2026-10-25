# Diagnostic Jive rendering patch

Status, 2026-10-24: **baseline and diagnostic Jive executables now built** in
`C:\Projects\squeeze_userbuild`. Both pass emulated headless startup with the
reconstructed legacy userspace. Subsequently tested from RAM on the Radio with
user approval, then stock Jive restored with its checksum unchanged. See that
workspace's `device-results.md` for the rendering measurements and CPU-clock
anomaly caveat, and `logs/verify.log` for build checks. No kernel changes are needed. This is
measurement code, not an optimization.

## Source and patch

`render-timing.patch` targets the public SqueezePlay source at
`031ea8c9d75d2230355df5c3317088ea85874611`, the closest inspected pre-firmware
revision, not a proven exact match to the installed native binary.

The isolated working tree is `/home/sjoerd/jive-render-diag-20261024` in WSL
Ubuntu-24.04. The original source checkout and UBIFS work are unmodified.
Apply the patch only to a separate clean checkout of that revision:

```sh
git apply --check /path/to/render-timing.patch
git apply /path/to/render-timing.patch
```

The Linux-only diagnostic adds six disjoint phase buckets: layout, animation,
background, drawing, rotation (including surface lock/unlock), and SDL flip.
Each reports call count, cumulative wall milliseconds, UI-thread CPU
milliseconds, and maximum wall duration. Layout/animation are also counted for
frames that do not redraw. Rotation and flip are only called when actually
needed. Standalone screenshot draws can contribute layout/drawing time, so avoid
remote screenshots during comparisons. Transitions are included in drawing.

Set `JIVE_RENDER_DIAG=1` in the diagnostic process environment to enable it.
Otherwise there are no diagnostic clock reads or log writes. It emits seven
summary lines approximately every five seconds while frames are processed and
stops measuring at the first report after 120 seconds. There are no timers,
Lua debug hooks, heap allocations or file creation in the timing helper itself.
The C library's stderr implementation can allocate internally. Log I/O is
outside the measured phases and can block: use a draining capture into bounded
RAM, not flash or a slow serial terminal. The patch adds `-lrt` for old Linux
clock symbols. It is not intended as a portable upstream production patch.

The clock-failure path disables measurement. UI errors can interrupt an active
phase, leaving that individual call uncounted; this is not a sampling profiler.
Phase CPU totals exclude unrelated main-thread work, audio/decoder threads,
diagnostic overhead and logging, so they need not sum to the process CPU.
Wall time includes preemption and waiting; compare CPU time as well. Frame count
is completed update calls, not display flips; use rotation/flip calls for flips.

## Checks performed

```sh
sh tools/jive-render-diag/check-local.sh /home/sjoerd/jive-render-diag-20261024
```

- Host tests pass: disabled path, timing accumulation, five-second reporting and
  reset, 120-second shutoff, clock-error disablement.
- Same tests compiled for ARMv5 soft-float and pass under qemu-arm.
- `jive_render_diag.c`, `jive_framework.c`, and `jive_surface.c` compile into
  ARM EABI5 objects, including the rotation path.
- `git diff --check` passes in the diagnostic tree.

The tiny `compile-headers` configuration is explicitly only for object checks.
It is **not** SDL's or Jive's vendor build configuration and must never be used
to claim that a complete binary matches the firmware. Tests use mocked clocks;
QEMU results do not measure real Radio overhead or test its old clock support.

## Remaining build gate

Historical note: the missing-SDK state below was subsequently resolved by the
dedicated `squeeze_userbuild` reconstruction. A complete binary now exists;
actual device-library compatibility and hardware UI/audio checks remain pending.

The installed ARM userspace compiler targets a modern userspace. The prepared
kernel compiler is a no-libc compiler. Neither constitutes a matching Jive SDK.
There is no configured legacy Jive build/sysroot with the required target SDL,
Lua/tolua++, codec and ALSA development libraries in the inspected environments.
Most historical dependency sources are bundled, but rebuilding/linking against
the Radio's legacy libc and matching libraries is still necessary. A modern
host/QEMU test executable is **not safe to copy to the Radio** as a replacement.

Before making a runnable diagnostic executable:

1. Recover or reconstruct the legacy target SDK/sysroot and dependency build
   configurations; retain the Radio's ARMv5/soft-float ABI and libc compatibility.
2. Build an uninstrumented baseline and instrumented variant from the same
   source/configuration; verify needed libraries, ELF attributes, loader and
   symbol versions against the Radio. Keep generated bindings consistent.
3. Preserve both baby-specific recipe patches, including effects resampler and
   XRUN handling. Do not inadvertently change sound behavior while measuring it.
4. Quantify measurement overhead, including old-kernel thread CPU clock cost.

## Device test, pending separate deployment approval

Do not overwrite `/usr/bin/jive`, edit startup scripts or run a second Jive next
to the live process. Plan an off-flash diagnostic executable, verified original
binary/launch environment and rollback first. Running it requires temporarily
stopping/restarting the UI and may interrupt audio; that was not authorized or
performed in preparing this patch.

With navigation sounds disabled and playback paused, collect approximately
30 seconds stationary, 30 scrolling, 30 stationary, using consistent menu and
browser polling. Correlate summaries with process/thread CPU and avoid screenshots.
Then test Now Playing label scrolling separately. Repeat with feedback sounds
only if measuring the audio path. Compare a diagnostic-disabled run to bound
observer overhead. No flash/kernel/UART operation is required for this design.

For each five-second interval, `cpu_ms / (interval_s * 1000) * 100` gives the
bucket's single-core CPU percentage. High rotation CPU implicates full-frame
copying; high drawing/layout CPU implicates different work. High flip wall time
with low CPU suggests waiting, not an expensive copying loop.
