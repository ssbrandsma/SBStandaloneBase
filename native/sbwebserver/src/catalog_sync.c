/*
 * GitHub release-catalog refresh through the existing local HTTPS proxy.
 *
 * Uses the webserver's asynchronous Mongoose client, with no TLS dependency,
 * background daemon or idle polling thread. repository.c validates and merges
 * release metadata under the shared catalog lock, then signals sbbase to reload
 * the local Applet Installer response. HTTP management routes expose status and
 * an authenticated manual refresh; local settings never come from GitHub.
 */
#include "catalog_sync.h"
#include "repository.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CATALOG_PATH "/https/raw.githubusercontent.com/ssbrandsma/SBStandaloneBase/master/config.json"
#define CATALOG_LIMIT 16384u
#define REFRESH_MS (6u * 60u * 60u * 1000u)

static struct {
	struct mg_connection *connection;
	uint64_t next_due, deadline;
	time_t last_success;
	int initialized, pending, health, done, ready_to_fetch, failures, changed;
	const char *state, *message;
} sync_state;

/** Reject malformed or oversized lengths without integer-overflow conversions. */
static int oversized_length(struct mg_str value)
{
	unsigned length = 0;
	if (!value.len)
		return 1;
	for (size_t i = 0; i < value.len; i++) {
		if (value.buf[i] < '0' || value.buf[i] > '9')
			return 1;
		length = length * 10 + (unsigned)(value.buf[i] - '0');
		if (length > CATALOG_LIMIT)
			return 1;
	}
	return 0;
}

/** Report a bounded failure and schedule a monotonic, capped retry. */
static void sync_failed(const char *message)
{
	static const unsigned retry_ms[] = {60000, 300000, 900000, 3600000};
	unsigned index = (unsigned)sync_state.failures;
	if (index > 3)
		index = 3;
	if (sync_state.failures < 4)
		sync_state.failures++;
	sync_state.next_due = mg_millis() + retry_ms[index];
	sync_state.state = "error";
	sync_state.message = message;
	sync_state.ready_to_fetch = 0;
	sync_state.done = 1;
	if (sync_state.connection)
		sync_state.connection->is_closing = 1;
	fprintf(stderr, "Catalog refresh: %s; keeping local configuration\n", message);
}

/** Consume one bounded health/catalog response without blocking on networking. */
static void sync_event(struct mg_connection *connection, int event, void *data)
{
	if (event == MG_EV_CONNECT) {
		mg_printf(connection,
			  "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			  "Accept: application/json\r\nAccept-Encoding: identity\r\n"
			  "User-Agent: StandaloneBaseCatalog/1.0\r\n"
			  "Cache-Control: no-cache\r\nX-SB-Public-Fetch: 1\r\n"
			  "Connection: close\r\n\r\n",
			  sync_state.health ? "/health" : CATALOG_PATH);
	} else if (event == MG_EV_READ && !sync_state.done &&
		   connection->recv.len > CATALOG_LIMIT + 8192u) {
		sync_failed("Response exceeds size limit");
	} else if (event == MG_EV_HTTP_HDRS && !sync_state.done) {
		struct mg_http_message *message = data;
		struct mg_str *length = mg_http_get_header(message, "Content-Length");
		if (length && oversized_length(*length))
			sync_failed("Response exceeds size limit");
	} else if (event == MG_EV_HTTP_MSG && !sync_state.done) {
		struct mg_http_message *message = data;
		if (mg_http_status(message) != 200 || message->body.len > CATALOG_LIMIT) {
			sync_failed("Upstream catalog is unavailable");
			return;
		}
		if (sync_state.health) {
			struct mg_str *guard = mg_http_get_header(message, "X-SB-Public-Fetch");
			if (!guard || guard->len != 1 || guard->buf[0] != '1') {
				sync_failed("HTTPS proxy lacks public-fetch protection");
				return;
			}
			sync_state.ready_to_fetch = 1;
		} else {
			char error[128];
			int changed = 0;
			Settings settings;
			settings_load(&settings);
			if (settings.readonly) {
				sync_failed("Catalog became read-only");
				return;
			}
			if (repo_merge_catalog(message->body.buf, message->body.len,
					       &changed, error, sizeof error)) {
				sync_failed("Remote catalog failed validation or merge");
				return;
			}
			sync_state.changed = changed;
			sync_state.last_success = time(NULL);
			sync_state.state = "ok";
			sync_state.message = changed ? "New releases merged" : "Catalog is up to date";
			sync_state.failures = 0;
			sync_state.next_due = mg_millis() + REFRESH_MS;
			fprintf(stderr, "Catalog refresh: %d added or upgraded entries\n", changed);
		}
		sync_state.done = 1;
		connection->is_closing = 1;
	} else if (event == MG_EV_ERROR && !sync_state.done) {
		sync_failed("HTTPS proxy connection failed");
	} else if (event == MG_EV_CLOSE) {
		if (!sync_state.done)
			sync_failed("Incomplete proxy response");
		sync_state.connection = NULL;
	}
}

/** Start one request to loopback; test builds alone can substitute its port. */
static void sync_connect(struct mg_mgr *mgr, int health)
{
	const char *proxy = "http://127.0.0.1:8765";
#ifdef SB_CATALOG_TESTING
	if (getenv("SB_CATALOG_TEST_PROXY"))
		proxy = getenv("SB_CATALOG_TEST_PROXY");
#endif
	sync_state.health = health;
	sync_state.done = 0;
	sync_state.state = "fetching";
	sync_state.message = "Checking GitHub releases";
	sync_state.deadline = mg_millis() + 10000;
#ifdef SB_CATALOG_TESTING
	if (getenv("SB_CATALOG_TEST_TIMEOUT_MS"))
		sync_state.deadline = mg_millis() + strtoul(getenv("SB_CATALOG_TEST_TIMEOUT_MS"), NULL, 10);
#endif
	sync_state.connection = mg_http_connect(mgr, proxy, sync_event, NULL);
	if (!sync_state.connection)
		sync_failed("Cannot start proxy request");
}

/** Queue an explicit refresh; the HTTP caller must enforce session/CSRF checks. */
int catalog_sync_request(void)
{
	if (sync_state.connection || sync_state.pending || sync_state.ready_to_fetch)
		return 0;
	sync_state.pending = 1;
	sync_state.state = "pending";
	sync_state.message = "Catalog refresh queued";
	return 1;
}

/** Advance startup/periodic refresh and deadlines; no network work occurs idle. */
void catalog_sync_tick(struct mg_mgr *mgr)
{
	uint64_t now = mg_millis();
	if (!sync_state.initialized) {
		sync_state.initialized = 1;
		sync_state.next_due = now + 10000;
#ifdef SB_CATALOG_TESTING
		if (getenv("SB_CATALOG_TEST_START_DELAY_MS"))
			sync_state.next_due = now + strtoul(getenv("SB_CATALOG_TEST_START_DELAY_MS"), NULL, 10);
#endif
		if (!sync_state.state) {
			sync_state.state = "pending";
			sync_state.message = "Startup release check scheduled";
		}
	}
	if (sync_state.connection) {
		if (!sync_state.done && now >= sync_state.deadline)
			sync_failed("Catalog request timed out");
		return;
	}
	if (!sync_state.pending && !sync_state.ready_to_fetch && now < sync_state.next_due)
		return;
	Settings settings;
	settings_load(&settings);
	if (settings.readonly || getenv("SB_CATALOG_SYNC_DISABLED")) {
		sync_state.state = "disabled";
		sync_state.message = "Catalog refresh disabled or read-only";
		sync_state.pending = sync_state.ready_to_fetch = 0;
		sync_state.next_due = now + REFRESH_MS;
		return;
	}
	int health = !sync_state.ready_to_fetch;
	sync_state.pending = sync_state.ready_to_fetch = 0;
	sync_connect(mgr, health);
}

/** Serialize credential-free refresh state for the authenticated management UI. */
void catalog_sync_status(char *out, size_t capacity)
{
	snprintf(out, capacity,
		 "{\"state\":\"%s\",\"message\":\"%s\",\"busy\":%s,"
		 "\"changed\":%d,\"last_success\":%lld}",
		 sync_state.state ? sync_state.state : "pending",
		 sync_state.message ? sync_state.message : "Startup release check scheduled",
		 sync_state.connection || sync_state.pending || sync_state.ready_to_fetch ? "true" : "false",
		 sync_state.changed, (long long)sync_state.last_success);
}
