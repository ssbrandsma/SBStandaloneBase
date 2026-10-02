# Source audit

## squeezebox-bootstrap

Python 3/asyncio application. Entry point: `squeezebox_bootstrap.__main__`, orchestration in `server.py`. It listens on UDP 3483 for discovery, TCP 3483 for SlimProto, and TCP 9000 for HTTP/CometD. Configuration is JSON; runtime player/session state is memory-only. Tests use `unittest` and cover discovery, frame parsing, HTTP, Bayeux, Jive dispatch, catalog filtering, limits, and security helpers.

Reusable protocol behavior:

- Discovery requests start with `e` or `E`, followed by 4-byte tag, 1-byte length, value. Responses start with `E` and echo supported requested tags (`IPAD`, `NAME`, `JSON`, `VERS`, `UUID`).
- Client SlimProto frames are opcode[4], big-endian uint32 payload length, payload. Server frames are big-endian uint16 length including opcode, opcode[4], payload. HELO registers a player; STAT updates activity; a `strm/t` frame is the keepalive.
- `/cometd` accepts Bayeux JSON batches. Required channels are handshake, connect/reconnect, disconnect, subscribe/unsubscribe, and Slim request/subscribe. Sessions use eight lowercase hex digits.
- Jive commands are `serverstatus`, `status`, `date`, `firmwareupgrade`, `menu`, `menustatus`, `displaystatus`, and `jiveapplets`. Unsupported UI probes return an empty menu-shaped result.
- Catalog entries expose title/name, target and firmware bounds, URL, version, SHA-1, description/release notes, creator, and email.

Dependencies: Python 3.11 at build/runtime, no native dependency. This is the behavioral reference, not shipped to the Radio.

## SBWebserver

C99 application embedding Mongoose. Entry point `src/main.c`; modules cover HTTP, settings, system information, and installed applets. It reads persistent configuration beneath `SBWEBSERVER_CONFIG_DIR`, defaults to `/mnt/storage/sbwebserver`, and serves static assets plus JSON APIs. Build explicitly sets `MG_ENABLE_EPOLL=0` and `MG_ENABLE_POLL=1`; this is preserved. The Lua applet starts the binary and maintains a PID file. Existing functions include device/network/firmware information, settings, applet tabs, and StandaloneRadio CSV operations. No automated C tests were supplied; `scripts/check-api.py` is an API smoke checker.

## SBHttpsProxy

Single C program using libcurl and pthreads. It binds only `127.0.0.1`, defaults to port 8765, handles GET/HEAD, redirects, streaming, Range and a `/health` endpoint. It uses a bounded worker pool. TLS peer/host verification is deliberately disabled for compatibility and is retained as a security limitation. Integration tests are in `tests/integration_test.py`; ARM scripts use the supplied Bootlin toolchain.

## SBStandaloneTime

Lua applet using SqueezePlay UDP, DNS resolver, timers and process APIs. It tries Google, pool.ntp.org, and Cloudflare with bounded retry delays, validates mode/version/stratum/transmit time, sets UTC with `date`, then writes the RTC with `hwclock -w -u`; resynchronization is daily. There is no native build or automated test suite. Its validated packet rules are the reference for the planned native `sbbase` SNTP module.

## Reuse decision and baseline risks

SBWebserver and SBHttpsProxy are reused directly and remain separate executables. The Python service is ported behaviorally. Time logic belongs in `sbbase`; the Lua implementation remains the reference until native clock-setting tests are complete. Primary risks are exact SqueezePlay streaming-CometD behavior, local loopback server selection, early boot ordering, ARMv5 libcurl/TLS size, and write permission for the RTC. None should be guessed on production hardware.

## Available cross toolchain

`C:/Projects/SBHttpsProxy` contains the hardware-validated Bootlin `armv5-eabi--musl--stable-2020.02-2` toolchain (GCC 8.4.0), CMake 3.31.8, wolfSSL 5.8.2, curl 8.18.0, static install prefixes, and a verified Radio binary. The canonical flags are `-static -Os -marm -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft`. StandaloneBase's ARM script reuses this toolchain and its already-built libcurl/wolfSSL archives without modifying the reference project.
