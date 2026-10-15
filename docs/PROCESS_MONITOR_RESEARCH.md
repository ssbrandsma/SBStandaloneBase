# Lightweight Process Monitoring Research

## Scope and conclusion

This report evaluates read-only monitoring for the Logitech Squeezebox Radio
(ARM926EJ-S/ARMv5, Linux 2.6.26.8-rt16) without changing production code or
contacting a device. Historical source review and host-side builds were
performed on 2026-10-13.

**Recommendation:** add a small, in-process C collector to the existing
`sbwebserver` which reads a deliberately limited set of procfs files and returns
raw counters in one authenticated JSON snapshot. Keep all deltas, percentages,
sorting, filtering, and history in browser RAM. Do not execute BusyBox tools,
link BusyBox code, add libproc, or run a monitoring daemon.

This is not reinvention of a kernel interface: BusyBox, procps, libstatgrab,
and SIGAR all ultimately parse the same Linux procfs text. The required subset
is small enough that a dependency adds more compatibility, licensing, and
maintenance burden than it removes.

```text
Linux 2.6 procfs (read only)
             |
   sbwebserver snapshot collector
   (one request, no resident sampler)
             |
 authenticated GET /api/monitor/snapshot
             |
     raw integer counters in JSON
             |
 browser deltas + bounded RAM history
             |
 uPlot charts + sortable process table
```

No daemon, worker thread, database, temporary file, or NAND write is required.

## Evidence and method

The following were inspected:

- the SqueezeOS BusyBox 1.18.2 recipe and exact defconfig at SqueezeOS commit
  `585fb6501704f0ca93e2bde662ccf4f8c4c9af10`;
- pristine BusyBox 1.18.2 source (downloaded from the
  [BusyBox release archive](https://busybox.net/downloads/busybox-1.18.2.tar.bz2));
- pristine procps 3.2.8 source, SHA-256
  `11ed68d8a4433b91cd833deb714a3aa849c02aea738c42e6b4557982419c1535`;
- Linux v2.6.26 procfs implementation and documentation;
- current Standalone Base build settings and existing procfs readers.

The SqueezeOS repository is the strongest available configuration evidence,
but a future device check must still compare `busybox --help`, `top --help`,
and the actual procfs files because a shipped image may differ from the public
recipe.

## A. Existing BusyBox capabilities

### Exact firmware configuration

The public SqueezeOS 1.18.2 recipe selects a shared
[`defconfig`](https://github.com/LMS-Community/squeezeos/blob/585fb6501704f0ca93e2bde662ccf4f8c4c9af10/poky/meta-squeezeos/packages/busybox/files/defconfig)
with these relevant settings:

```text
CONFIG_DESKTOP=n
CONFIG_PS=y
CONFIG_FEATURE_PS_WIDE=y
CONFIG_FEATURE_PS_TIME=n
CONFIG_FEATURE_PS_ADDITIONAL_COLUMNS=n
CONFIG_TOP=y
CONFIG_FEATURE_TOP_CPU_USAGE_PERCENTAGE=y
CONFIG_FEATURE_TOP_CPU_GLOBAL_PERCENTS=y
CONFIG_FEATURE_TOP_SMP_CPU=n
CONFIG_FEATURE_TOP_DECIMALS=n
CONFIG_FEATURE_TOP_SMP_PROCESS=n
CONFIG_FEATURE_TOPMEM=n
CONFIG_FEATURE_SHOW_THREADS=n
CONFIG_IOSTAT=n
CONFIG_MPSTAT=n
CONFIG_FREE=y
CONFIG_UPTIME=y
```

BusyBox `top` therefore supports `-b` batch mode, `-n N`, `-d SEC`, global CPU
percentages, and per-process CPU percentages. Its rows include PID, PPID, user,
state, VSZ, memory percentage, CPU percentage, and command. BusyBox 1.18.2
performs an extra approximately 100 ms internal sample on its first CPU-enabled
iteration so that it can calculate a delta. See the
[`top.c` implementation](https://github.com/mirror/busybox/blob/1_18_2/procps/top.c).

The configured non-desktop `ps` is much less useful. It accepts only the width
option and emits PID, user, VSZ, state, and command. PPID and selectable columns
are compiled out. `ps` cannot provide a complete Task Manager snapshot.

### `procps_scan()` reuse

BusyBox 1.18.2 has a capable internal `procps_scan()` routine in `libbb`. Its
flags cover PID, PPID, command, state, VSZ, RSS, user/system ticks, start time,
and other fields. This is reusable *source*, but not an installed library ABI.
The multicall `/bin/busybox` binary does not provide a supported C API that
`sbwebserver` can link against. Reuse would mean importing a configuration-
dependent portion of BusyBox plus its `libbb` helpers, maintaining local build
glue, and tracking upstream fixes.

### Why not parse `top -b`

Executing `top -b -n 1` for every browser poll would add a fork/exec, an extra
sampling delay, text formatting, text parsing, locale/terminal assumptions,
and a child-process failure mode. Column availability depends on the BusyBox
configuration. Commands can contain spaces and truncated fields, so the output
is not a reliable structured protocol. Multiple browsers would multiply this
work. It is acceptable as a manual diagnostic, not as the backend API.

### BusyBox licensing

BusyBox is GPLv2. Copying or linking `procps_scan()` and required `libbb` code
into the MIT-licensed `sbwebserver` would create GPL distribution obligations
for the resulting combined binary, including corresponding-source obligations.
Calling the already installed `busybox top` as a separate program avoids source
integration but has the technical drawbacks above. This report is engineering
analysis, not legal advice.

## B. procps / libproc

### Historically appropriate version

procps 3.2.8 was released in 2009 and is contemporary with Linux 2.6.26. Its
`proc/readproc.h` exposes `openproc()`, `readproc()`, `closeproc()`, and flags
such as `PROC_FILLSTAT`, `PROC_FILLMEM`, `PROC_FILLSTATUS`, and `PROC_FILLARG`.
It reads `/proc/<pid>/stat`, `statm`, `status`, and `cmdline`, so it provides a
structured process enumeration API. The source is archived with a file listing
at [Fossies](https://fossies.org/linux/misc/old/procps-3.2.8.tar.gz/).

Important limitation: `readproc()` does **not** fill its `pcpu` member. A caller
still has to retain successive process and system tick counters and calculate
CPU percentages. System, network, and disk data also require other libproc
functions or direct parsing, so `readproc()` solves only part of this feature.

Modern procps-ng is not recommended. Its API and dependencies have evolved,
and using it would solve no target portability problem that the 2009 code or a
small direct reader does not already solve.

### Build experiment

Measured on the development host:

- procps 3.2.8 built as a static `proc/libproc.a` without source changes;
- the x86-64 archive was 122 KiB (archive size, not linked contribution);
- the build emitted warnings involving old scanf constructs, fixed-size paths,
  alignment, and a compiler-detected `realloc` use-after-free expression;
- the repository's ARMv5/musl GCC 8.4 cross-build failed because procps 3.2.8
  assumes `PATH_MAX` is defined. It also emitted an ARM alignment warning.

The ARM failure is fixable, but proves that this is not a drop-in dependency.
A production integration would need a maintained patch set and dead-code
linking measurements. No target runtime measurement was made.

### Licensing

The `proc/` library in 3.2.8 carries the GNU Library General Public License
version 2 or later. Static linking is especially undesirable for the current
single-file static ARM deployment because LGPL relinking requirements must be
handled. A shared library would add installation and ABI management. Again,
obtain legal review before distribution.

## C. Other concrete native candidates

### libstatgrab

[libstatgrab](https://libstatgrab.org/) exposes CPU, memory, network, disk I/O,
and process statistics through a C API and supports Linux. The library is LGPL;
its tools are GPL. It is the best feature-complete library candidate if
cross-platform monitoring becomes a project goal.

It is not the best fit here: Standalone Base supports one known Linux target,
the library exposes much more than is needed, it still parses procfs, an old-
kernel/ARMv5 build would need validation, and integration adds an LGPL
dependency for roughly six small readers. No target-compatible size or runtime
measurement was found, so any numeric footprint claim would be speculation.

### Hyperic SIGAR

[Hyperic SIGAR](https://github.com/hyperic/sigar) has an Apache-2.0-licensed C
core and APIs for system and per-process CPU/memory/network/filesystem data.
Its historical support matrix included Linux 2.2, 2.4, and 2.6, making kernel
compatibility plausible. Its Linux backend directly parses procfs.

SIGAR is cross-platform infrastructure with bindings and a broad API, and the
original project is historical rather than an attractive small embedded
dependency. ARMv5/musl is not one of its published binary targets. It is more
code and maintenance exposure than the required collector, despite its
permissive license.

### Rejected categories

`libgtop`, collectd, PCP, Netdata, and similar systems are not close candidates:
they introduce GLib or other dependencies, a daemon/plugin system, persistent
or streaming infrastructure, or a footprint intended for general-purpose
systems. They were not benchmarked because they violate the stated architecture
before performance becomes the deciding factor.

## D. Direct procfs collection

### Required readers

| File | Data required | Work and cautions |
|---|---|---|
| `/proc/stat` | aggregate CPU tick counters, `btime` | Parse the first `cpu` row as 64-bit integers. Fields may be appended; tolerate missing optional fields. |
| `/proc/meminfo` | total/free/buffers/cache/swap/slab | Parse named keys, not line order. `MemAvailable` does not exist on this kernel. Return raw fields and document the UI formula. |
| `/proc/net/dev` | per-interface RX/TX bytes, packets, errors, drops | Skip two headers; split at the last interface-name colon; use 64-bit counters and handle wrap/reset in the browser. |
| `/proc/diskstats` | reads/writes/sectors/time where exported | Present since Linux 2.5.69. Parse at least major, minor, name and the documented counters; tolerate shorter rows and expose `available:false` when not meaningful. |
| `/proc/<pid>/stat` | PID, comm, state, PPID, user/system ticks, start time, VSZ, RSS | Find the final `)` around `comm` before splitting because the name can contain spaces or parentheses. Match a process by `(pid,starttime)`, not PID alone. |
| `/proc/<pid>/status` | optional readable name and memory fields | Not required if RSS and VSZ come from `stat`; reading it for every PID doubles opens. Use only if a verified UI field needs it. |
| `/proc/<pid>/cmdline` | display command line | NUL-separated; cap bytes (for example 512), replace NUL with spaces, JSON-escape, and fall back to `comm` for kernel threads or denied/empty reads. |

Linux's [procfs documentation](https://github.com/torvalds/linux/blob/v2.6.26/Documentation/filesystems/proc.txt)
describes the process and system files. Linux's historical
[`iostats.txt`](https://github.com/torvalds/linux/blob/v2.6.26/Documentation/iostats.txt)
documents `/proc/diskstats`; disk statistics moved out of `/proc/stat` in Linux
2.6. Fields introduced after 2.6.26 must never be required.

### Kernel compatibility

All core CPU, memory, network, and process inputs predate Linux 2.6.26. The
target is single-core, simplifying the primary CPU graph, but the parser should
accept `cpuN` rows without assuming exactly one CPU.

Important old-kernel behavior:

- no `MemAvailable`; expose raw memory keys and calculate a clearly labelled
  approximation such as `MemTotal - MemFree - Buffers - Cached`;
- `/proc/<pid>/stat` RSS is page-count based and approximate; convert with the
  collector's runtime page size and return both raw pages and bytes if useful;
- counters can wrap on an old 32-bit kernel or reset at reboot/interface
  recreation; negative or implausible browser deltas must be discarded;
- processes can exit between directory enumeration and file open; `ENOENT` is
  normal, not a snapshot failure;
- process names can contain difficult characters; parsing and JSON escaping
  require hostile-input tests;
- flash/UBI/MTD activity may not appear as a conventional block device in
  `/proc/diskstats`. The UI must call disk I/O unsupported when no relevant row
  exists rather than display a fabricated zero.

The presence and exact format of `/proc/diskstats`, relevant device names, page
size, `_SC_CLK_TCK`, and counter widths remain physical-device verification
items.

### Complexity

A focused implementation is estimated at 500-800 lines of readable C including
bounded JSON emission, parsers, error paths, and tests. The actual parsing core
should be much smaller; tests dominate because procfs is race-prone and text is
untrusted. This estimate is not a measurement.

## Comparison

| Approach | Added size | Runtime/process cost at 2 s | Dependencies | Old-kernel/ARMv5 risk | License | Maintenance | Verdict |
|---|---:|---|---|---|---|---|---|
| Execute BusyBox `top -b` | No packaged code | One child, formatted scan, ~100 ms first CPU sample per request; multiplied by clients | Installed BusyBox | Output/config dependent | Separate GPL program | Fragile parser | Reject for API |
| Import BusyBox `procps_scan()` | Unknown until linked | One in-process scan | BusyBox `libbb` source/config | Source is era-compatible; integration is coupled | GPLv2 combined-work concern | Carry fork/config | Reject |
| procps 3.2.8 `libproc` | 122 KiB measured x86-64 archive; linked ARM size unmeasured | One in-process scan; still needs delta logic and other readers | libproc or static objects | ARM/musl build failed on `PATH_MAX`; patching required | LGPL-2+ library | Historical patch set | Reject |
| libstatgrab | Unmeasured | In-process; likely one procfs scan per requested family | libstatgrab | Requires cross-build and exact-kernel validation | LGPL library | Maintained but broad | Reserve alternative |
| Hyperic SIGAR | Unmeasured | In-process | SIGAR C core | Linux 2.6 plausible; ARMv5/musl unverified | Apache-2.0 | Historical/broad | Reject |
| Direct focused procfs | Estimated 10-25 KiB linked code; measure during implementation | One process; about 3 system opens plus 1-2 opens per PID; no idle cost | libc only | Interfaces contemporary with target; exact files still verify | Project MIT code | Small explicit surface | **Recommend** |

Size and runtime values explicitly marked estimated are not measurements.
Current `build-arm/sbwebserver` is 164,992 bytes, measured before this feature.
No candidate was run on the Radio and no target CPU/RSS claim is made.

At an estimated 30-60 processes, a snapshot would perform roughly 65-125
procfs opens if both `stat` and `cmdline` are read. Procfs is RAM-backed, so
these are kernel-memory reads, not NAND reads. CPU cost should be small at a
two-second interval, but only a target soak test can quantify it.

## Recommended backend design

Add the collector as ordinary `sbwebserver` source, invoked only by an HTTP
request. Do not run a timer in the service. Bound all work:

- maximum process records (for example 256);
- maximum command-line bytes per process (for example 512);
- maximum input line and JSON response sizes;
- one directory pass per request;
- 64-bit parsing with overflow checks;
- skip vanished/inaccessible processes and report a skipped count;
- no `/proc/<pid>/smaps`, `/proc/<pid>/io`, environment, file descriptors, or
  expensive username lookups in the initial version;
- no server-side sample history or per-browser state.

Reuse the existing management-session authentication, same-origin checks,
security headers, and Mongoose response conventions. Monitoring exposes command
lines and system topology and must not be public. Return `Cache-Control:
no-store`. Read-only collection should be a GET and must not add process-kill or
renice operations to its scope.

Multiple open browser tabs will independently poll. The frontend should pause
polling when `document.hidden`, stop when the tab is inactive, prevent
overlapping requests, and use a single two-second timer for the complete
snapshot. If simultaneous-client load becomes measurable, a later optimization
may cache the most recent serialized snapshot in RAM for less than one sampling
interval; it is unnecessary initially.

## Proposed HTTP API

```text
GET /api/monitor/snapshot
```

Example shape (values are illustrative):

```json
{
  "schema": 1,
  "boot_id": "btime:1700000000",
  "uptime_seconds": 12345.67,
  "clock_ticks_per_second": 100,
  "page_size": 4096,
  "cpu": {
    "user": 100, "nice": 0, "system": 40, "idle": 900,
    "iowait": 2, "irq": 1, "softirq": 3, "steal": 0
  },
  "memory_kib": {
    "total": 60924, "free": 8000, "buffers": 1000,
    "cached": 12000, "swap_total": 0, "swap_free": 0
  },
  "network": [
    {"name":"eth0","rx_bytes":1000,"rx_packets":10,"rx_errors":0,
     "rx_dropped":0,"tx_bytes":2000,"tx_packets":12,"tx_errors":0,
     "tx_dropped":0}
  ],
  "disk": {
    "available": true,
    "devices": [
      {"major":179,"minor":0,"name":"mmcblk0","reads":10,
       "sectors_read":80,"writes":5,"sectors_written":32,
       "io_ms":120}
    ]
  },
  "processes": [
    {"pid":123,"ppid":1,"start_ticks":9000,"state":"S",
     "comm":"sbwebserver","cmd":"/usr/share/.../sbwebserver",
     "user_ticks":20,"system_ticks":5,"vsize_bytes":700000,
     "rss_pages":80}
  ],
  "skipped_processes": 1
}
```

Raw counters should be JSON integers, never preformatted strings. JavaScript
can exactly represent integers only through `2^53-1`; these kernel counters are
well below that on the target, but the API contract should state the limit.

### Browser calculations

For consecutive snapshots from the same `boot_id`:

- system CPU busy percentage = `100 * delta(non-idle ticks) / delta(all ticks)`;
- per-process CPU = `100 * delta(user_ticks + system_ticks) /
  delta(all system CPU ticks)` on this single-core target;
- identify a process by `(pid,start_ticks)` to prevent PID-reuse spikes;
- network rate = `delta(bytes) / delta(uptime_seconds)`;
- disk rate = `delta(sectors) * 512 / delta(uptime_seconds)` where the target
  confirms the standard diskstats sector unit;
- discard history continuity on boot ID change, counter decrease, missing
  process/interface/device, or unreasonable elapsed time.

Calculating percentages in the browser keeps the endpoint stateless, makes
multiple clients harmless, and lets graphs retain bounded history without
server RAM. It also avoids sampling continuously when no monitor is open.

## Frontend visualization

[uPlot](https://github.com/leeoniya/uPlot) is the recommended optional graph
library. It is MIT licensed, Canvas-based, dependency-free, and approximately
50 KiB minified according to its project documentation. Its published desktop
benchmarks are not Radio measurements; rendering runs in the management
browser, not on the Radio. Vendor a pinned minified file and its license in the
package rather than using a CDN, because the interface must work offline.

At a two-second interval, ten minutes of history is only 300 points per series.
CPU, memory, network RX/TX, and optional disk RX/TX therefore remain tiny. A
hand-written canvas graph could save about 50 KiB but would add interaction,
axis, resize, accessibility, and maintenance work. uPlot is justified once the
UI needs several polished charts; otherwise begin with simple numeric cards and
add it when graphs are implemented.

The process table should use ordinary DOM rows, client-side sorting/filtering,
stable `(pid,start_ticks)` keys, and a capped command column. With fewer than a
few hundred processes, virtualization is unnecessary.

## Performance and NAND impact

Expected steady behavior with one visible browser:

- one existing `sbwebserver` process and no additional thread;
- one snapshot every two seconds;
- procfs reads only, all backed by kernel memory;
- zero configuration writes, logs, temporary files, or persistent history;
- browser history stored in bounded JavaScript arrays;
- response allocation bounded and released after the request.

No procfs read described here writes NAND. Network transmission and CPU work do
not imply flash activity. Existing access logging must not be expanded to write
one line per polling request; the current server's logging behavior should be
checked during implementation.

Estimated initial JSON size is 10-30 KiB for 30-60 processes, heavily dependent
on command lengths. At 2 seconds that is approximately 5-15 KiB/s per visible
client. These are estimates, not measurements.

## Implementation effort estimate

| Work | Estimate |
|---|---:|
| Procfs parsers, bounded snapshot model, JSON endpoint | 2-3 engineering days |
| Fixture/unit tests including races, malformed fields, wrap/reboot | 2-3 days |
| Browser cards, uPlot history, sorting/filtering/table | 2-3 days |
| ARM build, package-size measurement, security/regression review | 1 day |
| Physical validation and soak testing | 1-2 days |
| Total | 8-12 engineering days |

The estimate assumes read-only monitoring only and no process-control actions.

## Required device verification

The implementation must not be considered validated until a read-only device
test answers these questions:

1. Does the shipped BusyBox identify as 1.18.2 and match the public defconfig?
2. What are `_SC_CLK_TCK`, page size, typical process count, and maximum command
   line length?
3. Which exact fields occur in `/proc/stat`, `/proc/meminfo`, and
   `/proc/net/dev`?
4. Does `/proc/diskstats` exist, and which rows correspond to meaningful Radio
   storage rather than RAM/loop/pseudo devices? Are UBI/MTD writes observable?
5. Are all `/proc/<pid>/{stat,status,cmdline}` files readable by the service's
   runtime user, including kernel threads?
6. What is snapshot wall time and CPU time at 30, 60, and the maximum observed
   process count?
7. What are `sbwebserver` RSS and binary-size deltas versus the baseline?
8. Does two-second polling affect audio, Jive responsiveness, Wi-Fi, or existing
   Virtual Radio/log polling when run simultaneously?
9. How do 32-bit counters behave during a six-hour soak and around interface
   reconnects?
10. Does a service or Radio restart reset browser history cleanly without a
    false spike?

Perform these checks read-only first. No deployment, restart, or physical
device access was performed for this research.
