# HLS metadata delivery optimization

## Decision and inspected baseline

Replace the fixed 500 ms HTTP loop with opt-in **25-second HTTP long polling**.
Playback synchronization is a separate one-shot Jive timer used only while one
received event is waiting for its decoder position. The native eight-event queue
remains the ordered backlog; Lua keeps one pending event rather than duplicating
that queue and its artwork references.

Inspection covered `hls_metadata.c`, proxy routing and worker admission in
`sbproxy.c`, `HlsMetadata.lua`, `StreamPlayer.lua`, Jive's `RequestHttp.lua` and
`SocketHttp.lua`, timers, decoder elapsed reporting, and the existing Now Playing
owner. The proxy has eight **in-flight** workers: it creates detached request
threads, not eight permanently running threads. Each has a configured 256 KiB
stack. Jive permits 10 seconds to connect and 70 seconds for response reads.

The original client created a request object, socket object, response chunks and
timeout for every 500 ms request. With no metadata it repeatedly allocated a
33824-byte native response buffer despite having no event to return. Actual Lua
object sizes/device CPU percentages were not measured; request counts below are
arithmetic estimates, not a device benchmark.

## Native transport and worker safety

`GET /hls-meta/<id>?after=N&wait=25` returns an existing newer event immediately.
Otherwise the existing request worker registers a stack-owned waiter, releases
the metadata mutex and blocks in `poll()` on two descriptors: its client socket
and a notification pipe. Metadata publication, cancellation, natural completion
and retirement signal the pipe under the registry mutex. The write end is
nonblocking, so a slow metadata reader never blocks audio publication. Checking
the queue and registering under one mutex avoids lost wakeups.

There is no periodic native wakeup, permanent metadata thread or new process.
Timeouts use monotonic time and preserve the remaining deadline across EINTR.
Socket EOF/error wakes abandoned requests independently of metadata updates.
Waiter removal and pipe close are synchronized with publication, preventing writes
through stale waiter pointers. Session records are never retained by a waiter.

Limits:

- `wait` is an integer from 0 through 30 seconds; omitted/zero remains immediate.
- At most **two blocked metadata requests total, one per session**. A duplicate
  or excess wait receives 429; non-waiting readers still work concurrently.
- Two audio workers plus two waiting workers occupy four of eight slots, leaving
  four for other admitted work. This is a bound, not a new priority scheduler.
  Other unrelated requests can still fill those four, just as they could before.
- A natural-EOF snapshot returns `finished` without waiting. Cancellation or
  expiry returns 404. HEAD does not wait and codec HEAD probes still create no
  metadata session. Existing revision gaps/eviction/artwork semantics are unchanged.
- Pending waits allocate **no metadata response copy**. The 33824-byte copy is
  allocated only when an actual event/artwork must leave the registry lock.
- `X-SB-Metadata-Wait: 1` advertises support without exposing session secrets.

This deliberately trades at most two longer-lived existing worker stacks for
roughly fifty times fewer idle HTTP/thread creation cycles. Adaptive polling was
not selected for current Base because it adds up to its interval of delivery
latency; the bounded wait slots make long polling feasible without consuming the
entire worker budget.

## Lua transport versus synchronization

The first immediate request negotiates the response header. Subsequent requests
include `wait=25`. `new({waitSeconds=N, ...})` can select 1–30 seconds. The request
failsafe is wait plus 15 seconds (40 seconds by default), below Jive's response
timeout and above the server deadline. Bodies remain bounded to 8 KiB, chunks are
compacted, and there is only one socket/request per client.

An empty long-poll timeout schedules the next request through a one-shot event
callback, not a permanent timer. Transport errors use 2/4/8/16/30-second bounded
backoff. Legacy Base responses without the header use immediate requests and a
five-second idle fallback; the initial request omits `wait`, preserving compatibility
with older strict query parsers. Legacy fallback delivery may consequently be up
to five seconds late; the optimized timing expectation applies to upgraded peers.

On receipt of an event, session/generation/revision checks run before it is retained.
No more HTTP requests are needed merely to wait for that event's playback time.
The local timer is scheduled from remaining decoder time, bounded to 50–5000 ms.
Steady playback schedules the final check at the target, rather than rounding it
to the next 500 ms HTTP cycle. Missing/non-finite clocks retry locally after one
second, without publishing early. Stalls retain the event. A backward movement
over 250 ms retires the old metadata timeline; a new stream identity/generation
must restart it, rather than guessing a new origin. A reconnect already does this.

Only sampled resets can be detected: an unobserved reset during a long idle period
is not magically inferable. Large forward jumps/resume can make an event late by
up to the five-second timer cap. Under a steady clock, prompt event delivery and
a responsive Jive event loop, approximately 500 ms or better synchronization remains
the design target, not a physical measurement. Timers always recheck position
before applying; unavailable clocks are never replaced by wall/download time.

Stop/switch cancels synchronization, retry and timeout timers and closes the socket.
Serial guards ignore callbacks from previous generations. There is no idle decoder
timer, no Now Playing ownership change and no audio/ID3 parser modification.

## Requests, sockets and memory comparison

Healthy steady-state arithmetic per stream, with a 25-second wait:

| Item | Original | Optimized |
|---|---|---|
| No changes, HTTP requests/hour | About 7200 | About 144, plus one initial negotiation |
| 15 changed revisions/hour | About 7200 | About 150 if evenly spaced; conservative upper estimate 159, plus negotiation |
| Decoder-position checks without pending metadata | 500 ms client callback still ran | None |
| Maximum metadata connections for two normal clients | Up to two transient | Up to two held waits |
| Maximum blocked metadata workers | None deliberately held | Two |
| Total audio + wait workers | Two audio plus short requests | At most four |
| Added permanent processes/threads | 0 | 0 |

The 150/hour example is nine 25-second timeouts plus one event response in each
four-minute interval. The conservative estimate is 144 timeout responses plus
15 event responses. Buffer-ahead holding, event phase and errors change actual
counts. Repeated short-interval errors are not included in healthy estimates.
Idle request-object/socket creation falls by about 98%; that is **not** a claim
of 98% overall CPU reduction.

ARM ABI/resource measurements:

- Registry static storage increases by **8 bytes** for two waiter pointers.
- Each waiter object is **44 bytes**, within the existing request stack, plus
  small poll/deadline locals. No change to metadata event/queue/parser allocations.
- At most **four pipe descriptors**, in addition to the two held client sockets.
  Kernel pipe memory is bounded/OS-managed and was not measured on the Radio.
- At most **512 KiB of existing configured worker-stack reservation** is now
  longer-lived for metadata waits. This is not measured RSS. The global eight
  worker/2 MiB configured stack-reservation ceiling is unchanged.
- Idle waits no longer hold a 33824-byte response buffer. Actual event/artwork
  responses retain their old bounded copy and network timeout behavior.
- No new NAND writes, persistent history, decoder or TLS dependency.

## Measured footprint and CPU scope

Compared with the deployed 0.4.2 ZIP's proxy, using the same ARMv5 static toolchain:

| Measurement | Before | After | Difference |
|---|---:|---:|---:|
| Stripped ELF file bytes | 866444 | 866444 | 0 |
| `size` text | 862652 | 863732 | +1080 |
| `size` data | 1428 | 1428 | 0 |
| `size` BSS | 21868 | 21876 | +8 |

The added code fits existing ELF file padding; unchanged file length does not
mean unchanged code. A host ASan/UBSan run of the isolated one-second idle-wait
test recorded **0.000365 seconds of request-thread CPU**. Another ARM/QEMU run
recorded 0.003114 seconds of emulation-thread CPU. These are workstation samples,
not Radio CPU percentages or comparable audio workload measurements. The test
also asserts CPU stays below 0.1 seconds during its one-second wait to catch a
polling/spin regression. Hardware CPU, RSS and battery/power effects remain unmeasured.

Metadata publication wakes `poll()` without waiting for the long-poll deadline;
remaining latency is scheduler, loopback HTTP and Jive event-loop handling.
Client cancellation is an immediate socket/pipe wakeup in the implementation.
Tests verify abandoned waits free their cap after short round trips and audio
cancellation wakes the consumer within two seconds; this is a test bound, not
a measured hardware latency guarantee.

## Verification and remaining work

Host ASan/UBSan HLS integration and native wait tests cover:

- Immediate existing-event responses; future-event wake-up; idle timeout;
  wait-parameter limits; immediate legacy queries; finished/expired/invalid sessions.
- Client abandonment/repeated disconnects, audio-session cancellation, natural
  termination and waiter cleanup; one-per-session and two-total caps.
- Two audio streams/two waits with health/ordinary metadata still available;
  filling the remaining four workers returns 503 and releasing them restores service.
- Queue eviction/revision gaps, concurrent IDs, HEAD isolation, encoded AAC/MP3
  preservation, TS metadata/PTS, ID3/APIC and existing fetch/cancellation regressions.

Lua tests cover idle long-polling, no idle decoder timer, multiple ordered events,
future/missing/stalled/reset clocks, generation/session changes, stale callbacks,
timer cancellation, bounded retry, 404 retirement, legacy fallback and existing
Now Playing/artwork behavior. Existing HLS probes, URL transport, LogoCache,
RadioFeeds, navigation and user-station tests pass. Applet syntax checks pass.

`scripts/build-arm.sh` builds all existing static ARM targets. ARM/QEMU runs of
HLS HTTP, native framing, ID3 and metadata-wait/expiry tests pass. Existing artwork
proxy streaming/URL-guard regressions pass. The inherited NanoSVG signedness warning
is unchanged. CMake adds `hls_metadata_wait`; full CTest was not used in the WSL
environment without CMake, so relevant tests were invoked directly.

Changed files: Base `native/sbproxy/hls_metadata.c`, `tests/images/test_hls_http.py`,
new `test_hls_wait.c`, tests CMake and documentation; Radio `HlsMetadata.lua` and
`test/hls-metadata-test.lua`. No transport optimization was applied to audio or ID3
parsing, station directories, ICY handling or Now Playing ownership.

**Not deployed or physically validated.** The existing 0.4.2 dist ZIP and Radio
installation are unchanged by this task. Hardware follow-up should measure a
quiet HLS hour and a metadata-bearing fixture, repeat station switching and
disconnects, and check CPU/RSS/audio underruns with both wait slots active. The
previous timed-ID3 timing, image and bounded-history limitations still apply.
