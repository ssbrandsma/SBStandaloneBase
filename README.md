# StandaloneBase

The web interface includes an authenticated **Virtual Radio** at `/remote` for
live LCD capture and remote operation through the real Jive action system. See
[`docs/VIRTUAL_RADIO_IMPLEMENTATION.md`](docs/VIRTUAL_RADIO_IMPLEMENTATION.md).
Photorealistic and Classic layouts, asset provenance, normalized geometry, and
hotspot calibration are documented in
[`docs/VIRTUAL_RADIO_PHOTOREALISTIC.md`](docs/VIRTUAL_RADIO_PHOTOREALISTIC.md).

StandaloneBase is a three-process native runtime plus SqueezePlay applet for a Logitech Squeezebox Radio. It provides a minimal local LMS-compatible bootstrap (`sbbase`), the existing configuration web server (`sbwebserver`), and the existing loopback HTTPS compatibility proxy (`sbproxy`).

Browser management uses physical six-digit PIN pairing and short-lived HttpOnly sessions; see [Web Access pairing](docs/WEB_ACCESS_PAIRING.md).

SVG artwork can be converted through the local image endpoint and standalone
ARMv5 renderer; see [SVG rendering](docs/SVG_RENDERING.md).

The authenticated web interface also provides read-only live system logs and a
stateless System Performance/process monitor. See
[`docs/SYSTEM_LOG_VIEWER.md`](docs/SYSTEM_LOG_VIEWER.md) and
[`docs/PROCESS_MONITOR_IMPLEMENTATION.md`](docs/PROCESS_MONITOR_IMPLEMENTATION.md).

This repository is an engineering baseline. Host protocol tests are automated. Local-LMS activation is intentionally not performed until the hardware checklist is approved and run on a recoverable Radio.

## Build and test

On Linux with CMake, a C compiler, pthreads, and libcurl development files:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`scripts/build-arm.sh` automatically reuses the hardware-validated toolchain and static TLS prefix in `C:/Projects/SBHttpsProxy` when run under WSL. Override `SBHTTPSPROXY_ROOT`, `ARM_TOOLCHAIN_DIR`, or `ARM_TLS_PREFIX` for another location. `packaging/build-package.sh` stages the applet and creates the installer ZIP.

See `docs/deployment.md`, `docs/recovery.md`, and `docs/testing.md` before device use.

Read-only flash capacity reporting and the disabled expansion research milestone are documented in `docs/STORAGE.md`, `docs/STORAGE_TESTING.md`, and `docs/STORAGE_RECOVERY.md`.

The host-tested Extended Storage integration and its explicit physical-validation boundary are documented in `docs/EXTENDED_STORAGE.md`.

On Windows:
```
wsl.exe sh -lc 'cd /mnt/c/Projects/SBStandaloneBase/StandaloneBase && ./scripts/build-arm.sh'
$env:SB_RELEASE_BASE_URL = 'https://downloads.example.invalid/standalonebase'
.\packaging\build-package.ps1
```

To produce another version, update:
- $Version in [build-package.ps1 (line 8)]
- zipfile in [build-package.sh (line 13)]
- The displayed version in [StandaloneBaseApplet.lua]
- Native version constants in [sbbase.h (line 10)]and [sbproxy.c (line 19)]
- standalone_base_version in the tracked root `config.json`; update its applet versions, public download URLs and ZIP checksums when publishing applets


## deploy remote

Deployment endpoints, account names, filesystem paths and container names are
private local configuration. Copy `packaging/deployment.env.example` to
`.local/deployment.env`, populate it locally, and never commit that file.

## Artwork routing

See [local artwork routing](docs/ARTWORK_ROUTING.md) for the HTTP/HTTPS loopback
transport, SVG integration, firmware adapters, tests and hardware validation.

## HLS audio

See [the HLS implementation report](docs/HLS_AUDIO.md) for the native loopback
AAC/MP3 relay, supported formats, resource limits, tests and hardware status.
See [timed ID3 metadata](docs/HLS_TIMED_METADATA.md) for session-aware track
updates, bounded artwork and synchronization limits.
The [metadata-delivery optimization](docs/HLS_METADATA_DELIVERY.md) replaces
500 ms HTTP polling with bounded long polling and one-shot playback timers.
