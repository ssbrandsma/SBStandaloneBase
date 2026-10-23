#ifndef SB_TLS_DIAGNOSTICS_H
#define SB_TLS_DIAGNOSTICS_H

/* Optional per-request TLS metadata. Never forward curl's raw verbose stream.
 */
#include <stdlib.h>
#include "http_trace_curl.h"

typedef struct {
	sb_http_trace *trace;
	int enabled;
	char attempt_ip[16];
	unsigned attempt_port;
	char negotiated[96];
} sb_tls_diagnostic;

/** Preserve HTTP hop tracing and capture only allowlisted TLS connection text.
 */
static int sb_tls_debug(
	CURL *curl, curl_infotype kind, char *data, size_t size, void *opaque)
{
	sb_tls_diagnostic *diag = opaque;
	sb_trace_curl_debug(curl, kind, data, size, diag->trace);
	if (diag->enabled && kind == CURLINFO_TEXT) {
		char line[256];
		char ip[16];
		unsigned port;
		size_t length = size < sizeof line - 1 ? size : sizeof line - 1;
		memcpy(line, data, length);
		line[length] = 0;
		/* sbproxy deliberately uses IPv4. This is an attempted address,
		 * not evidence that TCP or the TLS handshake completed. */
		if (sscanf(line, " Trying %15[0-9.]:%u", ip, &port) == 2 &&
		    port > 0 && port <= 65535) {
			memcpy(diag->attempt_ip, ip, strlen(ip) + 1);
			diag->attempt_port = port;
		}
		if (!strncmp(line, "SSL connection using ", 21))
			sb_trace_clean(diag->negotiated,
				       sizeof diag->negotiated,
				       line + 21,
				       0);
	}
	return 0;
}

/** Install the optional callback; absent/zero environment keeps existing
 * tracing. */
static void
sb_tls_configure(CURL *curl, sb_tls_diagnostic *diag, sb_http_trace *trace)
{
	const char *enabled = getenv("SBPROXY_TLS_DIAGNOSTICS");
	memset(diag, 0, sizeof *diag);
	diag->trace = trace;
	diag->enabled = enabled && !strcmp(enabled, "1");
	sb_trace_curl(curl, trace);
	if (diag->enabled) {
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, sb_tls_debug);
		curl_easy_setopt(curl, CURLOPT_DEBUGDATA, diag);
	}
}

/** Emit available TLS metadata once per transfer, only when explicitly enabled.
 */
static void sb_tls_finish(CURL *curl,
			  const sb_tls_diagnostic *diag,
			  CURLcode result,
			  const char *error)
{
	char *url = NULL, *host = NULL, *ip = NULL;
	char detail[513], backend[96], hostname[256], peer[80] = "";
	long port = 0;
	CURLU *parsed;
	const curl_version_info_data *version;
	if (!diag->enabled)
		return;
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
	if (!url || strncmp(url, "https://", 8))
		return;
	parsed = curl_url();
	if (parsed && !curl_url_set(parsed, CURLUPART_URL, url, 0))
		curl_url_get(parsed, CURLUPART_HOST, &host, 0);
	sb_trace_clean(hostname, sizeof hostname, host ? host : "unknown", 0);
	curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
	curl_easy_getinfo(curl, CURLINFO_PRIMARY_PORT, &port);
	if (ip && *ip && port > 0)
		snprintf(peer,
			 sizeof peer,
			 " remote_ip=%s remote_port=%ld",
			 ip,
			 port);
	else if (diag->attempt_ip[0] && diag->attempt_port)
		snprintf(peer,
			 sizeof peer,
			 " attempt_ip=%s attempt_port=%u",
			 diag->attempt_ip,
			 diag->attempt_port);
	version = curl_version_info(CURLVERSION_NOW);
	sb_trace_clean(backend,
		       sizeof backend,
		       version ? version->ssl_version : NULL,
		       0);
	sb_trace_clean(detail,
		       sizeof detail,
		       error && *error ? error : curl_easy_strerror(result),
		       1);
	sb_trace_log("event=tls_diagnostics id=%s host=%s curl_code=%d "
		     "tls_backend=\"%s\" tls_detail=\"%s\" "
		     "verification=disabled%s%s%s",
		     diag->trace->id,
		     hostname,
		     (int)result,
		     backend,
		     detail,
		     peer,
		     diag->negotiated[0] ? " negotiated=" : "",
		     diag->negotiated);
	curl_free(host);
	curl_url_cleanup(parsed);
}
#endif
