# System Log Viewer

## Architecture

The System Logs page is part of the existing authenticated Standalone Base
web interface. It adds no daemon, logging thread, database, or persistent
state.

```text
/var/log/messages ──read-only seek/read──┐
                                         ├─ sbwebserver ─ JSON ─ browser RAM
kernel ringbuffer ──klogctl(READ_ALL)────┘
```

The backend reconstructs all state from the requested source, the current
boot identifier, and the cursor supplied by the browser. Each browser keeps
its own collected history in JavaScript memory. Navigating away pauses
polling but keeps that in-memory history until the browser page is closed or
reloaded.

No implementation path writes to the Radio filesystem. Test-only source
overrides are environment variables and are not set in production.

## Web interface

The **System Logs** tab contains independent viewers for:

- Kernel Log (`dmesg`)
- System Log (`/var/log/messages`)

Each source polls independently every two seconds and prevents overlapping
requests. The page appends only new DOM nodes. Temporary request failures mark
the source offline without clearing history, and polling continues.

Controls provide pause/resume, auto-scroll, browser-history clearing, an
initial-line selector, and per-source `.txt` downloads. Clearing or downloading
uses the browser copy only and never changes a device log. Warning and error
keywords receive lightweight color highlighting.

Each source retains at most 20,000 browser lines. When the limit is exceeded,
the oldest entries are removed and the discarded-line count is displayed.
No history is put in cookies, local storage, or another persistent browser
store.

## HTTP API

Both endpoints require a valid paired-browser session:

```text
GET /api/logs/messages
GET /api/logs/dmesg
```

Supported query parameters are:

- `boot_id`: boot identifier returned by the preceding response.
- `cursor`: opaque source-specific cursor returned by the preceding response.
- `lines`: initial line count, default 200 and clamped to 1–1000.

Responses contain:

```json
{
  "boot_id": "current boot identifier",
  "source": "messages",
  "reset": false,
  "reset_reason": null,
  "available": true,
  "lines": [],
  "next_cursor": "opaque cursor",
  "more": false
}
```

At most 500 lines are returned per response. `more: true` directs the browser
to request the next page immediately using `next_cursor`. Response memory,
source reads, line counts, cursor sizes, and JSON output are bounded.

Missing or unreadable sources return HTTP 200 with `available: false`, an
empty line array, and a null cursor. This lets monitoring recover without
special error loops when syslog starts later.

## `/var/log/messages` cursor

The messages cursor contains the filesystem device, inode, and byte offset:

```text
m:<device>:<inode>:<offset>
```

Incremental requests seek directly to the byte offset and read at most 64 KiB;
the complete file is not reread. The next cursor advances only past complete
newline-terminated records. An incomplete trailing line is therefore retained
for a later request and is emitted once completed.

The initial request reads at most the final 128 KiB and returns the requested
number of trailing complete records. A reboot reset starts at offset zero so
the earliest still-available records from the new boot are returned.

Cursor resets are explicit:

- `file_replaced`: device or inode changed.
- `file_truncated`: stored offset is beyond the current size.
- `invalid_cursor`: cursor syntax is invalid.
- `reboot`: request boot ID differs from the current boot.

File access is read-only. The server never creates, truncates, rotates, locks,
or otherwise modifies `/var/log/messages`.

## Kernel cursor

Linux 2.6.26 does not expose the modern `/dev/kmsg` sequence-number API used by
newer kernels. Standalone Base reads a bounded snapshot with
`klogctl(SYSLOG_ACTION_READ_ALL)`, numeric action 3. This operation copies the
ringbuffer without consuming or clearing it and does not compete with a
`/proc/kmsg` reader.

The kernel cursor contains the delivered line count and FNV-1a fingerprints
for the last eight delivered lines. On the next request, the backend first
checks the expected position and then searches the snapshot for the complete
overlap window. Searching for the full ordered window and selecting the last
match handles repeated identical messages more safely than a timestamp or
single-line hash.

When the overlap is absent, the response sets `reset: true` and
`reset_reason: "lost_overlap"`, then returns a fresh bounded tail. This is an
explicit resynchronization: a ringbuffer wrap can lose messages when the
kernel produces them faster than the browser polls. Hash collisions are
theoretically possible but unlikely with the bounded 64-bit overlap window.

The kernel snapshot is capped at 256 KiB. No external `dmesg` process is
spawned.

## Boot and restart detection

The preferred boot identifier is
`/proc/sys/kernel/random/boot_id`. Firmware without that interface falls back
to the `btime` value in `/proc/stat`. `btime` has one-second resolution, so an
extremely fast reboot within the same second is the fallback's known edge
case.

When the browser sends a different boot ID, the backend ignores the old
cursor and returns `reset_reason: "reboot"`. The frontend keeps existing
history, inserts:

```text
--- Squeezebox reboot detected ---
```

and appends records from the new boot. A Standalone Base process restart does
not change the boot ID, and because the backend stores no cursor state, normal
incremental reading continues after the browser pairs again.

Network failures alone never imply a reboot. A reboot is confirmed only by a
successful response containing a different boot ID.

## NAND flash protection

Normal operation introduces no NAND writes:

- Both sources are opened read-only.
- Kernel reads use the non-destructive ringbuffer action.
- There are no backend log files or cursor files.
- There are no temporary files, periodic snapshots, or rotation changes.
- There is no background worker when clients are absent.
- Browser history and downloads are generated entirely in browser RAM.

Existing syslog activity is outside this feature; the viewer neither enables
nor reconfigures it.

## Performance limits

The implementation allocates memory only while serving a request and releases
it before returning to the Mongoose event loop. File reads are bounded to
64 KiB incrementally and 128 KiB initially. Kernel snapshots are bounded to
256 KiB. JSON responses are bounded to 512 KiB and 500 lines. Idle cost is
zero when no browser has the page open.

Multiple clients are independent because cursors are opaque request data and
there is no shared mutable log state.

## Testing

Host integration coverage is in `tests/logs/test_logs_http.py`. It validates:

- paired-session enforcement;
- default 200-line initial retrieval;
- empty and populated incremental retrieval;
- identical records and incomplete trailing records;
- pagination without skipped entries;
- truncation, replacement, and invalid cursors;
- kernel append, duplicate lines, and lost overlap;
- full reboot detection;
- Standalone Base restart without reboot;
- browser retention limit, polling interval, and in-memory download code.

Build and run all configured tests before release. A physical six-hour test
should leave the page open during playback, disconnect and reconnect the
network, and observe CPU/audio behavior and browser retention. NAND-write
validation should compare mount/write statistics before and after monitoring.
Do not deliberately crash or reboot a Radio without explicit authorization.

## Known limitations

- Ringbuffer entries lost before the next poll cannot be recovered.
- The fallback `btime` boot ID has one-second resolution.
- A `/var/log/messages` line longer than the bounded read window is withheld
  until a newline is visible within a later bounded read; ordinary syslog
  records are far smaller.
- Browser history intentionally disappears when the whole web page is
  reloaded or closed.
- Browser management sessions are intentionally held in server RAM. A full
  Radio or Standalone Base restart therefore requires physical PIN pairing
  again before protected polling can resume. History already held by an open
  browser tab remains in JavaScript memory throughout that pairing flow.
- Long-run and physical NAND-write validation require device testing and are
  not claimed by the host integration suite.
