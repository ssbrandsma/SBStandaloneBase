# StandaloneBase

StandaloneBase is a three-process native runtime plus SqueezePlay applet for a Logitech Squeezebox Radio. It provides a minimal local LMS-compatible bootstrap (`sbbase`), the existing configuration web server (`sbwebserver`), and the existing loopback HTTPS compatibility proxy (`sbproxy`).

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
