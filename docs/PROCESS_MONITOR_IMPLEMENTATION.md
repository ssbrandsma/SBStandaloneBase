# System Performance and Process Monitor

## Architecture

The monitor is an authenticated page in the existing Standalone Base web UI.
Each browser poll invokes one bounded, read-only procfs scan in `sbwebserver`.
There is no sampler thread, daemon, cache, temporary file, database, or device
history. Raw counters travel as JSON; the browser calculates deltas and retains
ten minutes of aggregate history in RAM.

```text
/proc -> monitor.c -> GET /api/monitor/snapshot -> performance.js -> uPlot/table
```

## Backend readers

`monitor.c` reads aggregate CPU counters from `/proc/stat`, uptime from
`/proc/uptime`, named KiB values from `/proc/meminfo`, interface counters from
`/proc/net/dev`, and supported block-device counters from `/proc/diskstats`.
It enumerates numeric `/proc` entries and reads only `stat` and `cmdline` for
each process. Collection is capped at 256 process records, command lines at 512
bytes, individual input buffers are bounded, and JSON is capped at 512 KiB.

Process `comm` is located between the first `(` and final `)` before numbered
fields are parsed, allowing spaces and parentheses. Processes that exit or
become inaccessible during collection are skipped. `(pid,start_ticks)` is the
browser's stable identity. `smaps`, environment, file descriptors and process
control are deliberately absent.

RAM, loop, MTD-block and UBI pseudo/block names are excluded from disk results.
When no conventional meaningful device remains, `disk.available` is false;
the UI does not display zero activity as if it were measured NAND I/O.

Tests can redirect procfs with `SB_MONITOR_PROC_ROOT`. Production defaults to
`/proc`.

## HTTP API

`GET /api/monitor/snapshot` uses the existing management session middleware and
returns `Cache-Control: no-store` and JSON containing:

- `schema`, `boot_id`, `uptime_seconds`, `clock_ticks_per_second`, `page_size`;
- `cpu` cumulative user/nice/system/idle/iowait/irq/softirq/steal ticks;
- `memory_kib` total/free/buffers/cached/swap and optional slab values;
- `network` cumulative per-interface byte, packet, error and drop counters;
- `disk.available`, reason, and cumulative per-device sector/I/O counters;
- `processes` with identity, state, tick and memory counters;
- `skipped_processes`.

Missing required CPU, uptime, or memory files produce HTTP 503. A response that
exceeds its fixed bound produces HTTP 507. Unsupported methods produce 405.

## Browser calculations

Between continuous snapshots:

```text
system CPU % = 100 * delta(total - idle - iowait) / delta(total)
process CPU % = 100 * delta(utime + stime) / delta(system total ticks)
network rate = delta(bytes) / delta(actual uptime seconds)
disk rate = delta(sectors) * 512 / delta(actual uptime seconds)
```

Estimated application RAM is clearly labelled and calculated as:

```text
MemTotal - MemFree - Buffers - Cached
```

Free and cache/buffer values remain separately visible. This is not Linux's
newer `MemAvailable`, which the target kernel does not expose.

Decreasing counters, PID reuse, missing entities, reboot (`boot_id` change),
and excessive sample gaps invalidate the relevant baseline. A reboot inserts a
null chart point so unrelated samples are not joined. No value is interpolated.

## Polling and history

The toolbar provides exactly 1, 2, 5, 10, 30 and 60 second intervals, default
2 seconds. One timeout drives the complete snapshot. A `busy` guard prevents
overlap. Navigation cleanup stops the timer; hidden tabs stop polling and clear
the delta baseline; visibility resumes it immediately. Temporary failures leave
the last display intact, mark it stale, back off to the selected interval, and
reset continuity.

Chart samples older than ten minutes are discarded by timestamp. Process
history is not retained. The interval preference stays in JavaScript memory and
does not create device or browser-storage writes.

## Frontend

The System Performance page contains CPU, memory, network and disk cards, live
charts, uptime/update state, selectors, and a keyed process table. The table is
filterable and sortable by all displayed fields and defaults to descending CPU.
Rows are reused by `(pid,start_ticks)` and vanished rows are removed.

Network monitoring deliberately selects one non-loopback interface rather than
summing bridges and members, avoiding double-counting. Users may select another
individual interface. Disk selection is similarly per device.

Charts use locally vendored **uPlot 1.6.32**, pinned from its upstream release.
`uPlot.iife.min.js`, `uPlot.min.css`, and the MIT `UPLOT-LICENSE` are packaged;
there is no CDN or runtime dependency. RX/TX and read/write share their
respective charts. Chart objects are updated, not recreated per sample.

## Performance

The stripped static ARMv5 `sbwebserver` file measured 164,992 bytes immediately
before this feature and 173,184 bytes afterward: **+8,192 bytes** native binary
growth. The linked section total reported by `size` is 174,788 bytes, including
BSS which does not occupy file space. uPlot adds 51,081 bytes JavaScript, 1,857 bytes CSS and a 1,078-byte
license before ZIP compression.

Host fixture collection validates the endpoint and JSON but is not a target
performance measurement. Runtime RSS, snapshot latency/size, CPU cost at 1/2
seconds, audio impact, and concurrent System Logs behavior require physical
testing. The single-threaded HTTP callback scans only RAM-backed procfs and does
no writes; typical work is two opens per process plus four system files.

## Tests

`tests/monitor/test_monitor_http.py` starts the real host webserver against a
synthetic procfs. It covers session enforcement, old-format CPU and memory,
network counters, explicit unavailable NAND statistics, process names with
spaces/parentheses, NUL-separated and JSON-sensitive command lines, the cache
header, page integration, supported intervals, hidden-tab handling, and bounded
history constants. The common HTTP regression suites protect existing pages.

## Build and deployment

Build all static ARM binaries:

```powershell
wsl bash -lc 'cd /mnt/c/Projects/SBStandaloneBase/StandaloneBase && ./scripts/build-arm.sh'
```

Create the normal release package:

```powershell
.\packaging\build-package.ps1
```

The package scripts include `performance.js`, both uPlot runtime files and its
license. Deployment is intentionally separate; this implementation was not
deployed to or tested on a physical Radio.

## Per-process CPU accounting

Per-process CPU utilization is calculated from two successful snapshots using
the same `(pid, start_ticks)` identity and the cumulative `utime + stime`
counters from `/proc/<pid>/stat`. The denominator is elapsed monotonic uptime
multiplied by `_SC_CLK_TCK`:

```text
process CPU % = 100 * delta(utime + stime) /
                (delta(uptime) * clock_ticks_per_second)
```

This is deliberately different from dividing by the aggregate `/proc/stat`
delta. The original implementation used that aggregate delta for both total
and per-process percentages. On the Radio's old tickless Linux 2.6.26 kernel,
the aggregate counters can advance by materially fewer ticks than wall time.
For example, a process delta of 500 ticks divided by an aggregate delta of 370
ticks produced 135.1% on a single-core CPU. The process did not actually use
more than one core; its denominator represented too short a window.

Total CPU utilization continues to use the aggregate `/proc/stat` counters:
user, nice, system, idle, iowait, irq, softirq and steal. Idle and iowait are
excluded from the busy numerator. This preserves the kernel's total busy/idle
accounting while process utilization uses the stable wall-clock capacity of
the single-core target.

The current and baseline snapshots are passed explicitly to both calculations.
A missing baseline, boot change, CPU counter reset, clock-tick-rate change,
non-positive elapsed time, or changed `(pid, start_ticks)` identity yields no
process delta. Sorting, filtering and DOM row reuse operate only on already
derived rows and cannot update or replace counter baselines. Changing the poll
interval is safe because the actual uptime delta, rather than the configured
timer delay, defines the measurement window.

The collector parses `/proc/<pid>/stat` by locating the first opening and last
closing parenthesis before counting fields, so command names containing spaces
or parentheses do not shift field offsets. It reads fields 14, 15, 22, 23 and
24 for user ticks, system ticks, start time, virtual size and resident pages.
It enumerates `/proc/<pid>` only, not `/proc/<pid>/task`, so thread CPU time is
not added a second time.

## Physical-device verification

On a recoverable Radio, pair a browser and record endpoint latency, response
size, `sbwebserver` RSS, CPU use and audio/Jive behavior at 2 seconds and then 1
second. Run System Logs concurrently. Verify actual network names, procfs field
formats, process count, clock tick/page sizes, and whether `/proc/diskstats`
contains a genuinely meaningful storage device. Exercise Wi-Fi reconnect,
process churn, service restart and full reboot. Run at least six hours and stop
on audio underruns, UI lag, memory growth or implausible counter spikes.

## Known limitations

- NAND/UBI I/O normally has no meaningful conventional diskstats row and is
  therefore reported unavailable.
- Memory use is an explicitly labelled approximation on Linux 2.6.26.
- Process RSS is the kernel's inexpensive approximate page count.
- Browser sessions are in server RAM; a service restart requires pairing again.
- Chart width is chosen when the page opens; major viewport changes may require
  reopening the page for optimal chart sizing.
- Physical performance and audio-safety validation is still outstanding.

## Read-only flash-statistics investigation

A read-only inspection of the physical Radio at `192.0.2.22` confirmed the
reason conventional Disk I/O is unavailable. `/proc/diskstats` contains RAM,
loop and `mtdblock:*` rows only. The writable `ubifs` and `sbdata` volumes are
UBI character-volume mounts; their `mtdblock` rows remain zero and are not a
valid measure of UBI/UBIFS traffic. The one nonzero `mtdblock:cramfs` row is the
read-only firmware image and is not representative of writable storage.

Useful read-only flash health/capacity interfaces do exist:

- `/proc/mtd` exposes partition names, sizes and erase sizes;
- this firmware's `/sys/class/mtd/mtd*` nodes expose only `dev`, `subsystem` and
  `uevent`, with no erase, wear or throughput counters;
- `/sys/class/ubi/ubi0` exposes total and available eraseblocks, reserved bad
  blocks, current bad-PEB count, eraseblock size and `max_ec` (maximum erase
  counter);
- each `/sys/class/ubi/ubi0_N` volume exposes its name, type, reserved LEBs,
  usable LEB size and corruption flag;
- `/proc/mounts` identifies the active UBIFS/SBUBIFS volumes and mount modes,
  but provides no byte or operation counters.

Observed values during the inspection included 1,014 total eraseblocks, 139
available, 10 reserved for bad blocks, zero bad PEBs, 129,024-byte eraseblocks,
and `max_ec=79`. These are instantaneous device-health/capacity observations,
not I/O rates, and must not be graphed as throughput.

A future **Flash Storage** panel would be useful if positioned as slowly
changing health and allocation information: UBI capacity, free LEBs, bad PEBs,
maximum erase count, per-volume reserved LEBs, corruption status and mount
state. It should poll slowly or on demand. It should not attempt experimental
read/write throughput or per-volume activity because this kernel exposes no
reliable counters for those measurements. The inspection performed no writes,
volume changes, initialization or restart.
