# Local Mongoose changes

## Runtime clock fallback

The Unix `mg_millis()` implementation checks clock return values at runtime.
It tries `CLOCK_MONOTONIC_RAW`, then `CLOCK_MONOTONIC`, then a wall-clock
fallback for systems with no working monotonic clock. Other platform paths
remain unchanged. Reapply this compatibility patch when updating Mongoose.

The ARM SDK defines `CLOCK_MONOTONIC_RAW`, but the Radio's Linux 2.6.26 kernel
rejects it with `EINVAL`. The original implementation ignored the error and
returned zero indefinitely. Consequently the ten-second initial catalog
refresh deadline never elapsed. The same defect affects other Mongoose-based
deadlines and timers. `CLOCK_MONOTONIC` was confirmed to work on the physical
7.7.2 Radio with a temporary read-only clock probe.

The fallback has no shared mutable state or added dependency. A rejected RAW
clock adds one failed clock syscall per sample; there is no idle background
thread. When MONOTONIC works, wall-clock changes do not reset these deadlines.

`tests/protocol/test_mongoose_clock.c` links the actual vendored implementation
with simulated kernel clocks. It reproduces RAW rejection, advancing fallback
time, catalog-style deadlines, real Mongoose timer firing, wall-clock changes,
working RAW behavior, and the final wall-clock fallback. It fails against the
unpatched implementation.

An installed old local catalog can still contain an older Base entry. The
stock Jive installer labels any unequal version an update; this patch does
not modify that firmware behavior. A successful GitHub refresh replaces the
stale catalog entry using the existing merge and sbbase reload path.
