# Testing and hardware checklist

Host tests compile protocol helpers and verify discovery, SlimProto framing, and catalog pre-validation. Run the original four projects' suites independently as behavioral baselines. QEMU confirms loader/ABI viability but not kernel, network broadcast, RTC, audio, or SqueezePlay lifecycle behavior.

With explicit authorization and a recoverable Radio:

1. Install the ZIP through Remote Library while retaining the previous server.
2. Confirm all three PIDs and health endpoints; measure RSS, CPU, startup time and storage.
3. Verify UDP/TCP 3483 and HTTP 9000 from the LAN.
4. Exercise handshake/connect/Jive startup and Applet Installer catalog display.
5. Record the current server, attempt a non-persistent local selection, and confirm actual SlimProto plus CometD connections.
6. Only after confirmation, persist localhost/local identity. Reboot and repeat offline.
7. Test web UI, proxy health/Range/redirect, SNTP/RTC, service crashes, corrupt config, unavailable network, upgrade failure, and uninstall rollback.
8. Install StandaloneRadio and verify ordinary HTTP radio playback remains independent of the proxy.

Do not execute step 5 or later without user authorization.

## Current automated result

The supplied Bootlin GCC 8.4.0 toolchain builds all three services as stripped, static ELF32 ARM EABI5 soft-float binaries. QEMU `arm926` executes `sbbase --self-test` successfully. Current file sizes are 37,960 bytes (`sbbase`), 107,640 bytes (`sbwebserver`), and 833,676 bytes (`sbproxy`). The generated ZIP contains only the applet runtime, three binaries, web assets, configuration/catalog fallback, and CA bundle. These results establish build/ABI compatibility only; they are not a substitute for Radio validation.

On 2026-10-02 the package was installed on a stock 7.7.3 r16676 Radio at the user's direction. Native ARM execution, TCP 3483/9000, UDP discovery, HTTP health, Bayeux handshake, loopback proxy health, applet registration, and process startup passed. Measured idle RSS was approximately 96 KiB for `sbbase` and 220 KiB for `sbproxy`. The existing standalone `sbwebserver` remained on port 80, so the bundled webserver detected the conflict and exited without disturbing the other services. Local LMS selection was not changed.
