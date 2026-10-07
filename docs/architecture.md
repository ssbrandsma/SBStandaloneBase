# Architecture

The Lua applet is the supervisor and user-visible status surface. It starts `sbbase`, then `sbwebserver`, then `sbproxy`; failures are isolated and restarted at most five times with increasing delay. PID ownership is limited to files created by this applet. Unknown port owners are never killed.

`sbbase` is an event-driven, single-process C server using `poll()`. UDP/TCP 3483 and TCP 9000 are independent sockets bound exclusively to `127.0.0.1`; the embedded LMS endpoint neither broadcasts nor accepts LAN clients. It owns LMS-compatible protocol state, catalog state, and eventually SNTP/RTC synchronization. `sbwebserver` owns the port-80 configuration UI and reads status/config files. `sbproxy` owns loopback TCP 8765 only.

Persistent state belongs under `/mnt/storage/standalonebase`; transient PIDs/logs belong under `/tmp/standalonebase`. Services do not require each other for ordinary operation.
# Storage component

`StorageManager.lua` is the applet-facing API. It consumes the read-only, line-oriented output of the static `sb-storage-helper`. The helper owns kernel-interface parsing and compatibility policy; Lua owns presentation. Expansion is intentionally absent from both layers.

The separately validated `sbubifs` architecture is integrated through
`storage-setup.sh` and the fail-open early-boot `storage-boot.sh`; see
`EXTENDED_STORAGE.md`. Lua remains a non-blocking frontend and does not perform
UBI or mount operations directly.
