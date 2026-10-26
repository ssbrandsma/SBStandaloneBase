# Legacy broadcaster TLS compatibility

## Radio Enciclopedia / Teveo failure

On 26 October 2026, the ARMv5 probe using the shipped curl 8.18.0 /
wolfSSL 5.8.2 SDK reproduced error -313 (`received alert fatal error`) for
`https://icecast.teveo.cu/9Rnrbjzq`. No HTTP response was received from the
origin; sbproxy translated the TLS failure to HTTP 502. This is not a decoder,
playlist or Jive redirect error.

An OpenSSL client negotiated TLS 1.2 with `AES256-GCM-SHA384` (static RSA
key exchange). An ECDHE-only attempt failed with the server's handshake-failure
alert. The original ARM library could not even select `AES256-GCM-SHA384`:
curl returned error 59 because that cipher was absent. Static RSA is disabled
by default by wolfSSL. This is distinct from the earlier RSA-4096 certificate
capacity issue, whose fix is retained.

The origin's HEAD response was HTTP 400, while GET returned HTTP 200,
`audio/mpeg`, and Radio Enciclopedia stream data. HEAD is advisory in the
existing Radio player; its GET fallback must remain intact.

## Correction and security boundary

Compile the isolated wolfSSL prefix with `WOLFSSL_STATIC_RSA`. The explicit
cipher list in `native/sbproxy/tls_policy.h` retains the previous SDK's
pre-TLS-1.3 ephemeral suites, followed by only these compatibility additions:

- `AES128-GCM-SHA256` (TLS_RSA_WITH_AES_128_GCM_SHA256).
- `AES256-GCM-SHA384` (TLS_RSA_WITH_AES_256_GCM_SHA384).

Explicit names avoid incompatible OpenSSL/wolfSSL cipher-group parsing.
TLS 1.3 cipher configuration is untouched. Existing PFS/CBC suites are retained;
no static-RSA CBC, RC4, DES, NULL or anonymous suite is added to the policy.
Both ordinary proxy transfers and HLS reuse the configured curl handle.

**Static RSA has no forward secrecy.** Compromise of the broadcaster's RSA
private key could permit decryption of recorded sessions. This is an explicit
compatibility concession, not a stronger TLS mode. Ephemeral suites remain
listed first, although a server can impose its own cipher preference.
Certificate/hostname verification policy, protocol versions, public-address
guards, redirect restrictions, timeouts and HTTP handling are unchanged.
There is no automatic downgrade to plain HTTP.

Upstream explanation:
[wolfSSL static cipher compatibility](https://github.com/wolfSSL/wolfssl/blob/v5.8.2-stable/README.md).

## Reproduction and validation

```sh
sh scripts/build-proxy-tls.sh
sh scripts/test-proxy-tls.sh
sh scripts/build-arm.sh
qemu-arm -cpu arm926 build-proxy-tls/tests/probe \
  https://icecast.teveo.cu/9Rnrbjzq
```

The diagnostic consumes at most a small bounded stream sample, not a saved
recording. The loopback TLS tests cover RSA-2048/4096 certificates with TLS
1.2/1.3, both static-RSA AES-GCM additions, a rejected static-RSA CBC-only
server, and an accepted ECDHE server. These tests do not depend on a live
broadcaster. Recheck the pinned cipher list when updating the SDK.

The ARM build requires the new private-prefix capability marker so an old
RSA-4096-only library is rebuilt instead of silently reused. No TLS library is
linked into sbwebserver and no extra daemon is introduced.

## Results

The rebuilt ARM926 probe negotiated TLS 1.2 / AES128-GCM-SHA256 against
Teveo, received HTTP 200 and a bounded MP3 sample. An end-to-end request through
the rebuilt ARM926 sbproxy `/https/icecast.teveo.cu/9Rnrbjzq` route also returned
HTTP 200, `audio/mpeg`, and 32,768 bytes. NPO Radio 2 still negotiated TLS 1.3
and returned HTTP 200. The controlled TLS cases above and sanitizer-enabled
native HLS parser/HTTP tests passed.
All 56 configured host regression tests also passed (154.80 seconds).

The ARM binary text increased by 1,536 bytes; static data/BSS did not grow.
This does not measure handshake heap use or CPU cost on the physical Radio.
No physical Radio was deployed, restarted or audibly tested for this fix.
