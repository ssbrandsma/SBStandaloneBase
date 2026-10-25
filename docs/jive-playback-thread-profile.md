# Playback CPU split by thread

24 October 2026. Read-only /proc sampling of the running RAM candidate; no Jive
restart, playback changes, profiler attachment or Radio filesystem writes.

## Method

The user confirmed NPO Radio 2 playing with Now Playing visible. Collector
`C:/Projects/squeeze_userbuild/profile-jive-threads.py` reads process and task
statistics in nine snapshots spaced by five-second sleeps. Actual timestamps
come from `/proc/uptime` bracketing each snapshot, not the sleep counter. The
target's `AT_CLKTCK` auxiliary-vector value confirms 100 accounting ticks/s.
Only the conventional first eight /proc/stat CPU fields are used; appended RT
subtotals are not added again.

PID/start-time identity, task membership, negative/greater-than-one-core CPU,
snapshot skew and task/process consistency are checked. Known kernel CPU-clock
limitations remain: /proc task accounting shares the suspect scheduler runtime
clock discussed in the earlier investigation. Plausible intervals are not proof
that this clock is entirely accurate.

An initial capture using external `cat` commands had ~0.7-second snapshot spans;
all its intervals were rejected. It is not included in the results. The revised
collector uses shell built-ins inside the snapshot loop, avoiding multiple fork/
exec calls on the Radio. Its accepted snapshot brackets were 0.04-0.10 seconds.
Earlier executable-link checks using unavailable/nonfunctional `readlink` also
failed before sampling; `ls -l` verified the actual running candidate instead.

## Now Playing sample

Run directory under `device-tests/20261024T182508Z-c8e8b3/thread-profiles/`:
`20261024T193614Z-nowplaying`. Eight intervals, 41.325 s accepted, none rejected.
Jive PID 6047 remained the staged `scroll3-long` executable.
The user subsequently confirmed artist, track title and station name were
scrolling at approximately 3 fps, so this is an animated Now Playing sample,
not a non-scrolling playback baseline.

| Component | Weighted CPU, one core | Attribution |
| --- | ---: | --- |
| Jive process | 40.4% | Aggregate, do not add again to its threads |
| Main thread 6047 | 24.4% | UI/Lua/event/network work, not graphics alone |
| Worker 6071 | 16.0% | Source-correlated decoder thread |
| Worker 6063 | 0.0% | Source-correlated DNS resolver |
| Separate jive_alsa process | 13.5% | Audio output, not all decoding work |

Across individual ~5-second intervals, main-thread CPU ranged 18.3-31.3%, while
the decoder stayed about 15.8-16.3%. Therefore this sample does not establish a
constant 35% Jive workload; its main-thread contribution varies.

Thread attribution uses the actual compiled source: `src/audio/decode/decode.c:1052`
creates `decode_thread_execute`; `src/net/jive_dns.c:300` creates the resolver.
The other `SDL_CreateThread` call is in the unused null-audio backend. Worker
6063 is blocked in `unix_stream_recvmsg` and consumes no ticks; worker 6071 is
the remaining active decoder worker, observed waiting in `futex_wait` at the
metadata snapshot. These are source/wait-state correlations, not sampled user-
space backtraces. All threads have the generic comm name `jive`.

## Quiet-menu comparison

The attempted long menu capture `20261024T193951Z-menu` passed numeric accounting
checks but is **invalid as a quiet-menu comparison**: the operator reported
automatic return to Now Playing during collection. Its 37.9% aggregate must not
be compared as if the menu stayed visible. A short pre-armed capture was then
made, with SSH/metadata setup completed before the operator pressed Home and
~8 seconds of snapshots after explicit readiness confirmation. No screensaver
or auto-return setting was changed.

### Valid short menu sample

Run `20261024T194150Z-menu`: four intervals, 8.635 s accepted, none rejected by
accounting checks. The operator confirmed the main menu still visible after
collection, with the same stream playing. Snapshot brackets were 0.06-0.09 s.

| Component | Scrolling Now Playing | Quiet menu |
| --- | ---: | ---: |
| Jive process | 40.4% | 31.3% |
| Main thread | 24.4% | 15.1% |
| Decoder worker | 16.0% | 16.1% |
| DNS worker | 0.0% | 0.0% |
| Separate audio output | 13.5% | 13.4% |

Values are weighted averages of approximate single-core CPU accounting, not a
matched stock/candidate optimization benchmark. The menu sample is much shorter.
Its first interval was elevated (Jive 43.9%, main 27.1%); the remaining three
intervals were Jive 26.1-27.4%, main 10.3-11.3%. That early higher interval may
include settling or unrelated periodic work; it is retained in the primary
average, not silently discarded. A repeat with a longer confirmed quiet state
would improve precision.

### Interpretation

The dominant stable audio component inside Jive is approximately 16% decoder
CPU, **in addition to** the separate ~13.5% audio-output process. Much of the
remaining variation is on the main thread: the full-capture comparison changes
it by ~9.3 percentage points while audio components barely change. Around 35%
Jive during playback is therefore plausible and cannot be dismissed as 35%
wasted graphics work. It also does not prove all remaining CPU is necessary.

The main thread runs UI/Lua, network/event tasks and stream-related work. The
Now Playing versus menu difference includes the whole screen workload (scrolling,
progress, artwork, polling and transitions), not a clean scrolling-only causal
estimate. There is no measured saving versus stock and no pure-rendering CPU
attribution from these /proc samples.

Jive stayed PID 6047 with the same executable and task identities throughout.
No restart, setting changes or writes to the Radio were performed by the
collector. The until-reboot candidate, watchdog and off-device long-test logging
were left untouched. Raw snapshots and analysis JSON remain in the run's
`thread-profiles` directory; the invalid long-menu sample is explicitly labelled.
