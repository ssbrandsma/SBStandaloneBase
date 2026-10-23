# sbproxy TLS diagnostics and NPO Radio 2 RSA capacity fix

## Confirmed root cause

Investigated on 23 October 2026 using Radio 192.168.1.225 (firmware
7.7.2 r9663), with audible playback subsequently confirmed by the user.
The failing source was `https://icecast.omroep.nl/radio2-bb-mp3`.

The deployed Standalone Base 0.5.9 proxy statically links libcurl 8.18.0
and wolfSSL 5.8.2. Its wolfSSL build selected `WOLFSSL_SP_MATH_ALL`
with the default `SP_INT_BITS=3072`. The NPO endpoint presents an RSA-4096
leaf certificate. This is a cryptographic implementation capacity mismatch,
not an HTTP, playlist, decoder or station configuration problem.

For TLS 1.3, wolfSSL's `DoTls13CertificateVerify()` rejects the 512-byte
RSA signature because its `ENCRYPT_LEN` buffer is derived from the smaller
RSA capacity: error -328, "malformed buffer input error". Forced TLS 1.2
instead fails with -110, "mp_init error state", when initializing the
insufficient fixed-size integer representation.

Relevant pinned upstream implementation:

- [Default integer capacity](https://github.com/wolfSSL/wolfssl/blob/v5.8.2-stable/wolfssl/wolfcrypt/sp_int.h)
- [RSA_MAX_SIZE](https://github.com/wolfSSL/wolfssl/blob/v5.8.2-stable/wolfssl/wolfcrypt/rsa.h)
- [ENCRYPT_LEN](https://github.com/wolfSSL/wolfssl/blob/v5.8.2-stable/wolfssl/internal.h)
- [TLS 1.3 CertificateVerify](https://github.com/wolfSSL/wolfssl/blob/v5.8.2-stable/src/tls13.c)
- [Fixed-size integer initialization](https://github.com/wolfSSL/wolfssl/blob/v5.8.2-stable/wolfcrypt/src/sp_int.c)

## Evidence and eliminated hypotheses

A standalone native probe linked with the exact original libraries reproduced
the failure independently of Jive and sbproxy request handling. Controlled
loopback servers with generated certificates isolated the cause:

| Library capacity | RSA certificate | TLS 1.2 | TLS 1.3 |
| --- | --- | --- | --- |
| Original 3072 | 2048 | HTTP 200 | HTTP 200 |
| Original 3072 | 4096 | -110 | -328 |
| Rebuilt 4096 | 2048 | HTTP 200 | HTTP 200 |
| Rebuilt 4096 | 4096 | HTTP 200 | HTTP 200 |

Modern curl 8.5.0/OpenSSL 3.0.13 succeeded against NPO using both TLS 1.2
and TLS 1.3, including P-256. Modern certificate verification passed. A
no-SNI TLS 1.2 test also succeeded. Original-library tests with P-256 and
ALPN disabled still failed. Original-library Last.fm HTTPS requests worked.
The direct HTTP NPO comparison worked, but no station URL was downgraded.
IPv4 was used, matching the proxy's existing policy; not every DNS backend
address was independently tested. A locally rejected TLS 1.1 attempt does
not establish the server's TLS 1.1 policy.

Ordinary requests create separate curl easy handles. Cross-request connection
reuse and concurrent request state are not needed to reproduce this failure.

## Minimal correction and build

Rebuild the same wolfSSL version and options with **`SP_INT_BITS=4096`**.
Keep the existing libcurl archive, TLS policy, transport and request handling.
The isolated prefix prevents overwriting the original SDK. Its installed
headers export the same integer capacity as the library.

```sh
sh scripts/build-proxy-tls.sh
sh scripts/build-arm.sh
sh scripts/test-proxy-tls.sh
```

The scripts default to the existing sibling SBHttpsProxy SDK. Overrides are
documented by the environment variables at the top of each script, including
`SBHTTPSPROXY_ROOT`, `ARM_TOOLCHAIN_DIR`, and `ARM_WOLFSSL_PREFIX`.
The ARM build checks the new prefix capacity marker before linking sbproxy.
No TLS library was added to sbwebserver, and no new daemon was introduced.

## Diagnostics

Ordinary HTTPTRACE completion errors now retain the sanitized curl error
buffer rather than only the generic "SSL connect error" description.
For additional per-transfer HTTPS diagnostics, start sbproxy with:

```sh
SBPROXY_TLS_DIAGNOSTICS=1 /path/to/sbproxy --listen 127.0.0.1:8765
```

Use the normal service lifecycle rather than starting a second listener on
the Radio. This switch is default-off. Detailed events include backend,
effective hostname, curl result, sanitized error, negotiated TLS information
when available, and connected or explicitly labelled attempted IPv4 peer.
An attempted peer is not evidence of a completed connection.

Only allowlisted curl text is inspected; request/response bodies, TLS record
data, authentication headers, URL credentials and queries are not dumped.
Unit tests inject secrets to check exclusion. Diagnostics cover ordinary
proxy transfers; the delegated HLS transport retains its existing tracing.
The probe in `tools/sbproxy_tls_probe.c` is a bounded test executable, not a
packaged service. It rejects URLs containing credentials, queries or fragments.

**Existing security limitation:** proxy peer and hostname verification remain
disabled, as before. Successful TLS means encrypted transport, not verified
server identity. This fix neither relaxes nor repairs that existing policy.
The self-signed/wrong-host fixture explicitly tests preservation of it.

## Validation

- Full static ARMv5 build passed; unrelated pre-existing compiler warnings remain.
- Diagnostic secret-exclusion/default-off tests passed under ARM926 QEMU.
- Original-versus-fixed RSA capacity tests reproduced both errors and passed
  with the correction.
- Local proxy tests passed: HEAD/GET, self-signed policy, HTTPS redirects,
  HTTP-to-HTTPS and existing HTTPS-to-HTTP redirect behavior, six sequential
  requests, three concurrent requests, stream cancellation, silent TLS peer
  timeout (15-second connection limit), recovery and health.
- Existing HLS HTTP, artwork streaming and image proxy guard tests passed.
- On the Radio: four sequential NPO HEAD requests, HTTP comparison, Last.fm
  JPEG retrieval and three concurrent audio/artwork requests returned 200.
- A physical-device NPO HTTPS stream delivered 1,925,120 bytes over 60.07
  seconds; cancellation, subsequent HEAD and health checks passed.
- User independently confirmed audible NPO playback.

Run the local proxy fixture with a testing-enabled binary:

```sh
python3 tests/tls/test_proxy_tls.py qemu-arm /path/to/sbproxy_test
python3 tests/tls/test_rsa_capacity.py /path/to/original-probe /path/to/fixed-probe
```

Fixtures use temporary host files only. OpenSSL, Python 3, and QEMU are host
test dependencies, not Radio dependencies. CMake registers the diagnostic
and proxy integration tests when the proxy target is enabled.

Measured fixed stripped executable: 874,644 bytes. During streaming, RSS was
1,196 KiB, high-water RSS 1,300 KiB, and three threads. A later sample was
1,120 KiB RSS and two threads, with the same high-water mark. These are
observations, not a comprehensive memory/CPU or descriptor-leak benchmark.
Existing worker limits and stack bounds are unchanged. No sustained CPU
percentage, exhaustive OOM/audio-underrun test, packet capture, or complete
TLS interoperability matrix is claimed.

## Device deployment and rollback

Only sbproxy was replaced and restarted on 192.168.1.225. Jive, sbwebserver,
station configuration and unrelated APIs were not changed. Detailed diagnostics
remain off. Temporary uploaded probe executables were removed.

- Original executable MD5: `1c34b2b76ac6c2c131bc490f9d470693`
- Corrected executable MD5: `6158f014dc9e4865aa1ed022c766293a`
- Device backup: `/mnt/storage/standalonebase/backups/tls-20261023-134809/sbproxy`
- Host backup: `.local/sbproxy-225-before-tls-20261023-134809`

To roll back, stop the existing proxy through the service lifecycle, restore
the backed-up executable to `/usr/share/jive/applets/StandaloneBase/sbproxy`,
ensure executable permissions, and allow the normal supervisor to restart it.
Do not run simultaneous listeners. The backup is specific to this device test.

The initial development executable reported 0.5.9. The published 0.5.9 archive
was not rebuilt or replaced; release 0.6.0 packages this correction.
The unrelated Now Playing HTTP 400 issue was explicitly left out of scope.
