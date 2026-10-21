# HTTP request tracing

Standalone Base enables the `StandaloneHTTP` logger at INFO and records Jive
HTTP transactions in syslog (`/var/log/messages` on the radio). Native proxy,
HLS, catalog and image requests use the same `HTTPTRACE` record format through
`syslog(LOG_INFO)`, while retaining their existing service log files.

Use `grep HTTPTRACE /var/log/messages` to inspect requests. Each request has an
`id`; `parent` links redirects and local proxy/render work. Records include:

- `request`: source, method and redacted URL, before the connection attempt.
- `send`: each actual libcurl request, including redirect hops.
- `response`/`header`: status, content type, content length and redirect target.
- `complete`: received bytes, elapsed milliseconds and transport/HTTP error.
- `resource_result`/`image_result`: logo caching and native decode/render result.
- `dns`: Radio resolver results and rejected/unresolved native image origins.

HTTP traces cover the shared Jive SocketHttp/RequestHttp layer, including
directories, playlists, artwork, installer traffic and local service requests.
Standalone Radio additionally traces native audio connections (which bypass
SocketHttp), and starts logo, artwork and RadioFeeds attempts before DNS. The
proxy logs each HTTPS transfer and HLS manifest/segment fetch. Native image
and catalog clients log their own HTTP connections and propagate parent IDs
to the proxy. Trace IDs are diagnostic headers, not authentication tokens;
the proxy removes them from upstream request headers.

Response headers are logged immediately, including for streams that remain
open. Native audio has no exposed byte counter, so its closing record says
`bytes=unknown`; proxy/HLS and ordinary HTTP transfers report measured bytes.
Stream bodies, images, cookies and authorization headers are never logged.
URL userinfo, query values and fragments are redacted; lines and header values
are bounded. Trace state stores counters only, with no payload buffering or
per-audio-chunk logs. Syslog's existing rotation controls retained history;
`/var/log/messages` is temporary and does not survive a radio reboot.

HTTP tracing is included in the Standalone Base 0.5.8 release.
