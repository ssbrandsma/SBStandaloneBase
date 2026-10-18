# Squeezebox Radio CPU accounting investigation

Date: 2026-10-18. Target tested: `192.168.1.222` only. This is an
investigation, not a production CPU-algorithm change.

## Executive summary

**The observed 100% CPU reading does not mean that the CPU was continuously
executing useful work or interrupt handlers. The kernel's independent
high-resolution idle-path timer advanced substantially while its tick-based
idle counter did not advance at all.** This is evidence of a kernel CPU
accounting discrepancy, not a JavaScript sorting or process-parser error.

In a main-menu baseline, the CPU accumulated 3,143 base CPU ticks, all reported
busy, over approximately 31.45 seconds. Matched processes used 1,001 ticks;
matched threads used 1,002. Independently, `/proc/timer_list` recorded
**16.173 seconds in the idle path and 2,974 idle entries**. Its count of actual
tick-stopping sleeps did not increase. The idle tick delta remained zero.

The kernel exposes two different accounting mechanisms: tick sampling for
system-wide CPU categories, and scheduler-runtime accounting for the total
CPU time presented in process stat records. The RT patch also appends
overlapping real-time subtotals to `/proc/stat`. Neither the production
collector nor BusyBox top adds those subtotals twice.

The leading explanation is biased periodic tick sampling: ticks repeatedly
observe an RT task, while short idle intervals between ticks are missed.
Source and hardware observations support this mechanism, but **the exact
interrupt/task timing that causes this sampling bias has not been traced**.
Do not describe it as a demonstrated faulty instruction or a proven NO_HZ,
audio-driver, or RT-patch defect.

The counter resumed counting idle during a later capture. The user confirmed
that they had not changed playback, screen, or settings. No service was
restarted. This is not a permanently frozen counter. The unexplained residual
must not simply be relabeled idle, and an exact corrected total CPU percentage
is not established by these measurements.

## Scope and reproduction

Only read-only SSH diagnostics were executed. No services were stopped,
restarted, reconfigured, or reprioritized; no firmware, volumes, or production
binaries were changed. The script was streamed into `/bin/sh -s`; captures
were saved on the workstation, not on NAND or a device temporary filesystem.
Normal execution still creates transient diagnostic processes and network
traffic and therefore perturbs scheduling. This matters when interpreting the
spontaneous recovery.

The user identified the condition as **main menu, playback stopped or paused**.
Stopped versus paused was not independently established. No audible-playback
claim is made. Playback and GUI state were not remotely changed for a test.

Single device-side command, after making the script available on the device:

```sh
sh cpu-accounting-diagnostic.sh 5 10 30
```

To avoid staging any device file, stream it over a suitably configured SSH
client from a workstation:

```sh
ssh root@192.168.1.222 '/bin/sh -s -- 5 10 30' < tools/cpu-accounting-diagnostic.sh > radio-cpu.txt
python3 tools/cpu_accounting_analyze.py radio-cpu.txt
```

Old Dropbear may require explicitly enabled legacy SSH algorithms on the
client. Do not place passwords in the command or in version-controlled files.
For a lower-overhead control, use `--light 5 10 30`; this omits task enumeration
and must not be used to infer a process-accounting residual.

The script preserves raw CPU records including all eleven fields, per-CPU
records, task records, interrupt counts, timer state, uptime, configuration,
and load. Unsupported procfs/sysfs interfaces are explicitly indicated. It
does not enable tracing, mount debug filesystems, or change kernel settings.

## Verified platform and source provenance

On-device findings:

- `Linux 2.6.26.8-rt16 #1 PREEMPT RT Fri Feb 14 09:02:51 PST 2014`.
- ARMv5/ARM926, single `cpu0`; aggregate and cpu0 counter rows agreed at the
  captured boundaries.
- BusyBox `1.18.2`, built 2014-02-14.
- `/proc/config.gz`: `CONFIG_NO_HZ=y`, `CONFIG_HIGH_RES_TIMERS=y`,
  `CONFIG_PREEMPT_RT=y`, `CONFIG_HZ=100`, `CONFIG_MXC_EPIT_CLKSRC=y`.
- CPU frequency support and PM are disabled. No cpuidle residency/frequency
  sysfs statistics were available.
- `/proc/softirqs`, `/proc/schedstat`, `/proc/sched_debug`, and per-task
  `schedstat` were unavailable; `/proc/timer_list` was readable.
- Clock source/event infrastructure uses the Logitech EPIT implementation;
  `epit_clkevt`, high-resolution active, NO_HZ mode 2 were observed.

Local source inspected:

1. `C:/Projects/squeezeos`, commit
   `e51036464ce6750df06303a0f2ddccc7e4674002`.
2. `poky/meta-squeezeos/packages/linux/linux-imx25_svn.bb`: starts with Linux
   2.6.26 and applies the imx25 quilt series.
3. `src/imx25/patches/series`: begins with 2.6.26.8 and rt16, then Freescale,
   UBIFS and Logitech patches. The canary debugging patch is commented out.
4. Existing reconstructed tree
   `/tmp/sb-kernel-exact/linux-2.6.26` in WSL: Makefile identifies `.8-rt16`;
   used to inspect the composed functions and line numbers below.
5. Independently downloaded upstream
   [2.6.26.8-rt16 patch](https://cdn.kernel.org/pub/linux/kernel/projects/rt/2.6.26/patch-2.6.26.8-rt16.bz2).
   Its CPU-stat additions agree with the local quilt patch.
6. BusyBox upstream tag 1_18_2:
   [top.c](https://github.com/mirror/busybox/blob/1_18_2/procps/top.c) and
   [procps.c](https://github.com/mirror/busybox/blob/1_18_2/libbb/procps.c),
   plus the local SqueezeOS BusyBox recipe and patch list.

This is the matching available patch family, **not a cryptographic proof that
every instruction in the running 2014 kernel matches this checkout**. No
running kernel image was extracted or patched. Source line references below
refer to the reconstructed local tree, not modern Linux documentation.

## Raw evidence and collection limitations

Raw captures are retained under ignored `.local/cpu-investigation/` so they
can be reanalyzed without exposing device-specific diagnostics in Git.

| Capture | Purpose | SHA-256 |
|---|---|---|
| `20261018T084538Z.txt` | Full enumeration plus high-resolution idle measurements | `b4773819d7bd1c4f7cfc7738820876da30e1989896a6e123a971f18865a0b71d` |
| `20261018T084732Z.txt` | Light control; idle counting resumes within final interval | `ef0c68756fbe342a80cef0e2e569899bea28958c04486c52eec9287baa781990` |
| `20261018T084930Z.txt` | Full enumeration after recovery | `afc15a7ce83914ff4fea90ec9509a959fdd75d833fb1b1121d2f59de3181ef21` |

An earlier capture `20261018T084400Z.txt` also reproduced the discrepancy but
predates timer-list capture. The initial script trial
`20261018T084257Z.txt` disabled pathname expansion accidentally and therefore
did **not** enumerate tasks. It is not used as process-accounting evidence;
the glob problem was corrected before the captures above.

Snapshots are sequential, not atomic. Uptime/stat are read at both ends of
each enumeration. Full enumeration spans approximately 1.2–1.4 seconds;
the light snapshot spans approximately 0.25–0.28 seconds. The sleeping period
is therefore **not** the complete measurement interval. Tables use midpoint
uptime differences for approximate task-window duration; the analyzer also
reports the start-to-start CPU window and its tick rate separately. Never
divide these captures by just 5, 10, or 30 seconds.

The collector shell and SSH server are visible CPU consumers: for example
about 43 shell ticks per full-enumeration interval in the initial run. Short
experiments have proportionately greater overhead. New/exited tasks are
reported separately; matched-task sums are not exhaustive lifecycle totals.

### Initial main-menu measurements

| Requested sleep | Approx. elapsed s | CPU ticks / busy ticks | Process / thread ticks | Idle tick delta | High-resolution idle s | Idle entries |
|---|---:|---:|---:|---:|---:|---:|
| 5 | 6.490 | 653 / 653 | 265 / 265 | 0 | 2.485 | 479 |
| 10 | 11.435 | 1143 / 1143 | 410 / 411 | 0 | 5.197 | 929 |
| 30 | 31.445 | 3143 / 3143 | 1001 / 1002 | 0 | 16.173 | 2974 |

In the 30-second-sleep interval:

- Jive: 419 ticks, approximately 13.3% of wall-clock CPU capacity.
- jive_alsa: 407 ticks, approximately 12.9%.
- Process sum: approximately 31.8%; unassigned difference: 2,142 ticks.
- Base CPU deltas: user 2,215, system 928; all other base deltas zero.
- RT subtotals: user_rt 2,215, system_rt 928. Essentially every sampled tick
  was charged to RT categories, although the precise task sums were much lower.
- High-resolution idle advanced 16.173 s, approximately 51% of this window.
- `idle_sleeps` delta zero: entering the idle path is **not** the same as
  actually stopping the scheduling tick.

The remaining approximately 17 percentage points between measured idle-path
time and matched process time are **unresolved**. These are different
non-atomic accounting domains; possible contributors include IRQ execution,
scheduler accounting limitations, and endpoint/lifecycle effects. Interrupt
counts alone do not measure interrupt duration. It is not justified to assign
the entire 2,142-tick residual to idle or interrupts.

### Recovery without an intentional state change

The light run first reproduced zero idle, then its final interval gained
1,949 idle ticks out of 3,041 total. The subsequent full run found:

| Approx. elapsed s | Total / busy ticks | Process / thread ticks | Idle ticks | High-resolution idle s |
|---:|---:|---:|---:|---:|
| 6.345 | 639 / 272 | 237 / 237 | 367 | 2.766 |
| 11.325 | 1137 / 398 | 345 / 344 | 739 | 5.673 |
| 31.340 | 3132 / 900 | 829 / 830 | 2232 | 17.426 |

In the final interval the process sum is approximately 26.5%, the conventional
busy reading 28.7%, and RT subtotals total only 16 ticks. jive_alsa falls to
approximately 7%, while Jive stays near 14%. The process identities/start
times remain the same. Tick-idle and high-resolution-idle fractions still do
not agree exactly, so resumption does not make every instantaneous percentage
ground truth.

BusyBox `top -b -d 5 -n 3` was captured during the transition/recovery. Its
headers reported 26%, 65%, then 72% idle; Jive rows approximately 13–16% and
jive_alsa about 7%. The first top header is a cumulative/startup baseline,
not a clean five-second interval. These top windows overlap the light run
but are not exactly aligned; do not subtract their percentages from its
interval totals as if they were synchronized.

IRQ activity remained measurable in both states. Across the light capture's
first-to-last boundaries, EPIT IRQ 28 increased 4,875 times, SDMA IRQ 34
4,701 times, I2C IRQs 3/4 increased 758/917, and FEC IRQ 57 increased 729.
No claim about IRQ CPU cost follows from these event counts.

## Audit of userspace accounting

### Standalone Base

`native/sbwebserver/src/monitor.c`:

- `append_cpu()` (line 135): reads only the first eight base counters. It
  tolerates appended fields and does not double-count the RT subtotals.
- `read_process_stat()` (line 298): finds the final `)` of comm before token
  parsing; supports embedded spaces and parentheses. Uses fields 14/15 for
  user/system and field 22 for identity. Does not add child CPU fields 16/17.
- `append_processes()` enumerates numeric top-level proc directories, not
  each thread and then the process again. Group stat includes thread time.
  Kernel threads are not excluded because their cmdline is empty.
- Enumeration is bounded at 256 processes; skipped/racing processes are
  reported. This is not a complete accounting ledger at high process counts.

`native/sbwebserver/web/js/performance.js`:

- `metrics()` uses matching successful snapshots, boot identity, uptime and
  clock-rate checks. Total CPU uses base busy/base total tick deltas.
- `calculateProcessData()` keys baselines by `(pid,start_ticks)` and divides
  own CPU deltas by elapsed uptime times `_SC_CLK_TCK`, not by the biased
  aggregate counter total. It does not add child time.
- Sorting/filtering operate on derived rows, not raw baselines. Refresh and
  polling interval changes do not change the counter meanings.
- New/reused PIDs have no matching baseline; exited processes disappear.
  Per-process sums therefore omit CPU consumed by unobserved task lifecycles.

The earlier user-supplied diagnostic's exact source is not present in the
tracked tools/scripts inspected. Its arbitrary labels `guest/guest_nice`
were wrong for this RT ABI. Its raw first-eight-counter arithmetic can still
be checked, and the independent new diagnostic reproduces the discrepancy.
Thus this is not evidence that all earlier diagnostic assumptions were sound.

### What the kernel actually publishes

`fs/proc/proc_misc.c`, `show_stat()` lines 521–605, after rt16:

| Position | Meaning on this kernel | Include in CPU total? |
|---:|---|---|
| 1 | user, including user_rt | yes |
| 2 | nice | yes |
| 3 | system, including system_rt | yes |
| 4–8 | idle, iowait, irq, softirq, steal | yes |
| 9 | user_rt, already in position 1 | no |
| 10 | system_rt, already in position 3 | no |
| 11 | guest, also included in user accounting | no |

`kernel/timer.c:1029`, `account_process_tick()`, charges one jiffy to the
task seen at the tick; `update_process_times():1048` uses `current`.
`kernel/sched.c:4176`, `account_user_time()`, and `:4228`,
`account_system_time()`, route those samples to user/RT/IRQ/idle categories.

Conversely `kernel/sched.c:4306`, `task_utime()` and `task_stime()`, derive
the exported task total from `se.sum_exec_runtime`, converting nanoseconds
to user clock ticks. The user/system split still uses the tick sample ratio
and monotonic previous-value guards. Therefore even a misleading split does
not imply a wrong userspace field offset. `/proc/<pid>/stat` is a group sum
via `fs/proc/array.c:350–442`; per-task paths use the individual task variant.

`fs/proc/proc_misc.c:132`, `uptime_read_proc()`, derives its second value from
`init_task.utime + init_task.stime`: another presentation of the idle task's
tick accounting, **not an independent corroborating idle measurement**.

### Idle path and independent evidence

The matching ARM idle loop is `arch/arm/kernel/process.c:151–183`; it enters
NO_HZ idle bookkeeping and eventually `arch_idle()` in
`arch/arm/mach-mx25/system.c:128`, which selects WAIT_UNCLOCKED and calls
`cpu_do_idle()` unless JTAG mode prevents it.

`kernel/time/tick-sched.c:147–174` accumulates `idle_sleeptime` using
`ktime_get()` at idle-path entry/exit. `kernel/softirq.c:413–425`, `irq_enter()`,
stops idle timing on interrupt entry. `/proc/timer_list` publishes these
fields via `kernel/time/timer_list.c:151–175`. This is independent of
`cpustat.idle` and demonstrates idle-path residence, not merely an unexplained
arithmetic residual. It does not directly measure electrical sleep residency.

`tick_nohz_stop_sched_tick():191` starts idle timing before deciding whether
to stop the tick. At lines 252–260 an imminent timer/RCU requirement can leave
the tick running. `tick_nohz_restart_sched_tick():366–405` catches up idle
ticks only when the tick was actually stopped. This explains how short idle
windows can exist without an idle catch-up adjustment.

High-resolution `tick_sched_timer():548–615` samples at periodic tick
boundaries; it is IRQ-safe, not a new userspace/softirq polling thread. It
calls `update_process_times(user_mode(regs))`. Source therefore supports
sampling bias when repeated RT work and tick timing align. Exact event
ordering during the bad state remains unobserved.

The platform provides a sub-jiffy scheduler clock:
`arch/arm/plat-mxc/time.c:340–356` calls `mxc_epit_sched_clock()`, defined in
`drivers/clocksource/mxc_epit_clksrc.c:300`. The Logitech EPIT patch includes
wrap/backward handling. The generic scheduler-clock filtering layer also
exists in `kernel/sched_clock.c`; it is a reason not to treat process-runtime
sums as an unquestionable replacement total. No clock discontinuity was
demonstrated in these captures.

## Hypotheses and confidence

| Explanation | Assessment |
|---|---|
| Genuine uninterrupted 100% CPU consumption | Contradicted by high-resolution idle-path deltas in the bad state. |
| Extra RT fields double-counted by Base/top | Rejected by source inspection and the eleven-field regression fixture. |
| Missed Jive threads or adding child CPU | Not the main cause: independent process/thread sums differ by about one tick; child fields are excluded. |
| Wrong stat field offsets or comm parsing | Rejected for observed records; regression tests cover spaces, nested/trailing parentheses and child counters. |
| Snapshot overhead/skew alone | Insufficient to explain persistent 40–68% gaps and independent seconds of idle; still affects short-window accuracy. |
| Permanently defective idle counter | Rejected: it resumed without an intentional device-state change. |
| Periodic tick sampling biased toward RT execution | Leading mechanism, high confidence in accounting-domain mismatch; timing trigger not conclusively traced. |
| Interrupts explain all missing ticks | Unsupported: event counts are not durations, and idle-path residence is positive. |
| NO_HZ inherently broken | Not established. Actual tick-stopping sleep count stayed unchanged in the measured windows. |
| PREEMPT_RT itself or jive_alsa is defective | Not established. RT classification and audio timing are correlations, not a demonstrated offending patch. |
| EPIT rollover/clock defect | Possible area for further validation, not demonstrated by monotonic captures. |

Confidence is **high** that the bad-state headline is misleading and that
simple userspace summation errors do not cause it. Confidence in a specific
kernel fix is **insufficient**. The transition may have been affected by
diagnostic scheduling/network traffic or ordinary background activity;
"no user action" does not prove a completely unchanged workload.

## Impact and recommended next steps

BusyBox top reads the same first eight counters in `read_cpu_jiffy()` and
therefore has the same misleading headline during the bad state. It is not
an independent CPU-load oracle. Its process display uses task deltas plus
busy/total normalization and a denominator guard; in this case it need not
force the process rows to sum to the misleading 100% headline.

Standalone Base's total graph faithfully reflects the kernel's tick report,
but that report is not reliable total utilization on this workload. The
per-process wall-time calculation should not be replaced or clamped merely
to force agreement. No production CPU calculation has been changed here.

Potential follow-up, requiring separate justification and approval:

1. Expose a clearly named **observed process CPU sum**, wall-time normalized,
   alongside the kernel-reported total and a diagnostic warning. It is a
   lower-bound-style observed-work measure, not a reliable complete CPU total.
   Missing task lifecycles, clock/accounting errors and boundary rounding mean
   even that description needs careful UI wording.
2. Evaluate a separate, explicitly experimental high-resolution idle-path
   metric with its own monotonic timestamp. Do not silently replace the total
   graph with `100 - idle_sleeptime/wall_time` without validating semantics,
   resets, wrap, interval alignment and firmware variants.
3. A correction to what `/proc/stat` itself publishes would require kernel
   accounting changes. Userspace cannot repair historical missed samples.
   Do not make a kernel patch before capturing the exact bad-state timing.
4. A bounded development-kernel trace of tick time/current PID, idle entry/
   exit and task switches could establish periodic phase bias versus another
   architecture bug. That exceeds this read-only production investigation.
   Do not enable tracing or rebuild/reboot this Radio automatically.

No accessible scheduler-debug runtime totals, idle residency sysfs, or usable
hardware PMU measurement was found. `CONFIG_OPROFILE=y` alone is insufficient:
the inspected ARM OProfile backend selects XScale/ARMv6/MPCore, not a hardware
ARM926 model; enabling timer-based profiling would neither be read-only nor
an independent tick measurement. No OProfile filesystem was mounted/enabled.

## Manual operating-condition matrix still required

Keep any performance-browser tab closed or its polling state identical across
tests. Manually set each condition, wait 20 seconds for transients, record the
exact screen/station/playback state and then collect full and light captures.
Do not infer stopped versus paused from silence alone.

| Condition | Status |
|---|---|
| Main menu, playback stopped | Current test covers main menu but stopped/paused distinction remains unresolved. |
| Main menu, playback paused | Same limitation; repeat with explicit pause state. |
| Now Playing, paused | Not measured in this investigation. |
| Now Playing, audible audio | Not measured; manually verify sound. |
| Stream active but silent | Not reproduced; only test if naturally reproducible. |

For every capture retain total/idle/RT deltas, matched process/thread totals,
Jive/jive_alsa rows, kernel thread rows, IRQ counts and timer-list idle deltas.
The existing script collects all of these without repeated high-frequency
polling. Repeat the same condition to catch intermittent bad/recovered states.
The two original supplied A/B measurements remain useful context, not newly
verified controlled-condition experiments.

## Validation

- Seven offline parser/accounting tests pass: unusual comm strings, exact
  field offsets, ignored child counters, RT overlapping fields, PID reuse,
  new/vanished tasks, counter reset/boot change, independent timer evidence,
  and raw section decoding.
- Existing process-monitor HTTP integration test extended with an eleven-field
  RT16 fixture: exactly eight JSON CPU fields and the expected base total.
- Fresh host-built webserver passed that HTTP test; the existing ARMv5 release
  webserver also passed it under QEMU. No replacement build was deployed.
- BusyBox `/bin/sh` ran the full and light diagnostics on the physical Radio
  successfully. Host `sh -n` passed. Missing task files during enumeration are
  ordinary races, not grounds for silently trusting an exhaustive process sum.
- The frontend regression command was attempted, but Node.js is not installed
  in this execution environment. No frontend production code changed.
- `git diff --check` passed, with pre-existing line-ending warnings only.

Workstation commands:

```sh
python3 tests/monitor/test_cpu_diagnostic.py
python3 tests/monitor/test_monitor_http.py /path/to/host/sbwebserver native/sbwebserver/web
node tests/monitor/test_performance_logic.js native/sbwebserver/web/js/performance.js
sh -n tools/cpu-accounting-diagnostic.sh
```
