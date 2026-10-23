/* Unit checks for opt-in, bounded TLS metadata and credential/body exclusion.
 * Uses a real curl handle without opening a connection; standalone test only.
 */
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char records[8192];

/** Capture syslog records for assertions instead of writing to system logs. */
static void capture(int priority, const char *format, ...)
{
	va_list args;
	(void)priority;
	va_start(args, format);
	vsnprintf(records + strlen(records),
		  sizeof records - strlen(records),
		  format,
		  args);
	va_end(args);
}

#define syslog capture
#include "../../native/sbproxy/tls_diagnostics.h"

/** Exercise default-off and enabled diagnostics with adversarial synthetic
 * text. */
int main(void)
{
	sb_http_trace trace = {"test-tls-1", 0};
	sb_tls_diagnostic diag;
	CURL *curl = curl_easy_init();
	assert(curl);
	curl_easy_setopt(curl,
			 CURLOPT_URL,
			 "https://user:SECRET@example.com/a?token=TOKEN");
	unsetenv("SBPROXY_TLS_DIAGNOSTICS");
	sb_tls_configure(curl, &diag, &trace);
	sb_tls_finish(curl, &diag, CURLE_SSL_CONNECT_ERROR, "failure");
	assert(!records[0]);
	setenv("SBPROXY_TLS_DIAGNOSTICS", "1", 1);
	sb_tls_configure(curl, &diag, &trace);
	char attempt[] = "  Trying 192.0.2.1:443...\n";
	char failure[] = "SSL_connect failed with error -328: malformed buffer "
			 "input error\n";
	char secret[] = "Authorization: Bearer SECRET\nBODY TOKEN\n";
	sb_tls_debug(curl, CURLINFO_TEXT, attempt, strlen(attempt), &diag);
	sb_tls_debug(curl, CURLINFO_TEXT, failure, strlen(failure), &diag);
	sb_tls_debug(curl, CURLINFO_HEADER_IN, secret, strlen(secret), &diag);
	sb_tls_debug(curl, CURLINFO_DATA_IN, secret, strlen(secret), &diag);
	sb_tls_debug(curl, CURLINFO_SSL_DATA_IN, secret, strlen(secret), &diag);
	sb_tls_finish(curl, &diag, CURLE_SSL_CONNECT_ERROR, failure);
	assert(strstr(records, "host=example.com"));
	assert(strstr(records, "curl_code=35"));
	assert(strstr(records, "attempt_ip=192.0.2.1 attempt_port=443"));
	assert(strstr(records, "-328: malformed buffer input error"));
	assert(!strstr(records, "SECRET") && !strstr(records, "TOKEN"));
	curl_easy_cleanup(curl);
	puts("TLS diagnostics opt-in, attempted peer, error detail and "
	     "redaction passed");
	return 0;
}
