/*
 * Bounded, short-lived TLS diagnostic client for sbproxy's exact static SDK.
 * Uses libcurl directly, without Jive or the proxy worker implementation. This
 * is test tooling, never installed as a daemon or included in the applet ZIP.
 */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/sbproxy/tls_policy.h"

typedef struct {
	size_t bytes;
	size_t limit;
} probe_state;

/** Discard stream data and stop once the requested diagnostic byte limit is
 * met. */
static size_t discard_body(char *data, size_t size, size_t count, void *opaque)
{
	probe_state *state = opaque;
	size_t length = size * count;
	(void)data;
	state->bytes += length;
	return state->limit && state->bytes >= state->limit ? 0 : length;
}

/** Emit curl's textual connection diagnostics, never headers, bodies or TLS
 * records. */
static int trace_text(
	CURL *curl, curl_infotype type, char *data, size_t length, void *opaque)
{
	(void)curl;
	(void)opaque;
	if (type == CURLINFO_TEXT)
		fprintf(stderr, "TLS_PROBE %.*s", (int)length, data);
	return 0;
}

/** Run one bounded GET/HEAD; optional flags change this probe only, not
 * sbproxy. */
int main(int argc, char **argv)
{
	CURL *curl;
	CURLcode result;
	char error[CURL_ERROR_SIZE] = {0};
	char *ip = NULL;
	long status = 0, port = 0, verify = 0;
	probe_state state = {0, 65536};
	int i;
	long seconds = 10;
	printf("library=%s\n", curl_version());
	if (argc < 2 || strchr(argv[1], '?') || strchr(argv[1], '@') ||
	    strchr(argv[1], '#')) {
		fprintf(stderr,
			"Usage: tls-probe PUBLIC_URL [--head] "
			"[--tls12|--tls13] "
			"[--groups LIST] [--cipher LIST] [--resolve "
			"HOST:PORT:IP] "
			"[--http1] [--no-alpn] [--seconds N]\n"
			"URLs containing credentials, queries or fragments are "
			"refused.\n");
		return 2;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();
	if (!curl)
		return 2;
	curl_easy_setopt(curl, CURLOPT_URL, argv[1]);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, SBPROXY_TLS12_CIPHERS);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, trace_text);
	struct curl_slist *resolve = NULL;
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--head"))
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
		else if (!strcmp(argv[i], "--tls12"))
			curl_easy_setopt(curl,
					 CURLOPT_SSLVERSION,
					 CURL_SSLVERSION_TLSv1_2 |
						 CURL_SSLVERSION_MAX_TLSv1_2);
		else if (!strcmp(argv[i], "--tls13"))
			curl_easy_setopt(curl,
					 CURLOPT_SSLVERSION,
					 CURL_SSLVERSION_TLSv1_3 |
						 CURL_SSLVERSION_MAX_TLSv1_3);
		else if (!strcmp(argv[i], "--groups") && i + 1 < argc)
			curl_easy_setopt(
				curl, CURLOPT_SSL_EC_CURVES, argv[++i]);
		else if (!strcmp(argv[i], "--cipher") && i + 1 < argc)
			curl_easy_setopt(
				curl, CURLOPT_SSL_CIPHER_LIST, argv[++i]);
		else if (!strcmp(argv[i], "--resolve") && i + 1 < argc) {
			resolve = curl_slist_append(resolve, argv[++i]);
			curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
		} else if (!strcmp(argv[i], "--http1"))
			curl_easy_setopt(curl,
					 CURLOPT_HTTP_VERSION,
					 CURL_HTTP_VERSION_1_1);
		else if (!strcmp(argv[i], "--no-alpn"))
			curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = strtol(argv[++i], NULL, 10);
			if (seconds < 1 || seconds > 60)
				return 2;
			state.limit = 0;
		} else
			return 2;
	}
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, seconds);
	result = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
	curl_easy_getinfo(curl, CURLINFO_PRIMARY_PORT, &port);
	curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
	printf("code=%d status=%ld bytes=%zu peer=%s:%ld verify=%ld "
	       "detail=%s\n",
	       (int)result,
	       status,
	       state.bytes,
	       ip ? ip : "unknown",
	       port,
	       verify,
	       error);
	curl_easy_cleanup(curl);
	curl_slist_free_all(resolve);
	curl_global_cleanup();
	return result == CURLE_OK || (status == 200 && state.bytes > 0) ? 0 : 1;
}
