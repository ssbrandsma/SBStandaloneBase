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
