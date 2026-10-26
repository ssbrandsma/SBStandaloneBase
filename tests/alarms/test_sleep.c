/* Synthetic monotonic sleep-timer tests; no live player or physical device. */
#include "sleep.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char out[4096];

static int request(const char *args, uint64_t now)
{
	char json[1500];
	snprintf(json,
		 sizeof json,
		 "{\"data\":{\"request\":[\"radio\",[%s]]}}",
		 args);
	return sb_sleep_request(
		json, strlen(json), "radio", out, sizeof out, now);
}

int main(void)
{
	assert(request("\"sleepsettings\"", 1000));
	assert(strstr(out, "90 minutes"));
	assert(request("\"sleep\",\"900\"", 1000));
	assert(!sb_sleep_tick(900999, 1));
	assert(sb_sleep_tick(901000, 1));
	assert(!sb_sleep_tick(901001, 1));
	strcpy(out, "{}");
	assert(sb_sleep_status(out, sizeof out, 901001));
	assert(strstr(out, "\"sb_sleep_serial\":1"));
	request("\"sleep\",\"90\"", 1000);
	request("\"sleep\",\"0\"", 2000);
	assert(!sb_sleep_tick(999999, 1));
	request("\"sleep\",\"90\"", 1000);
	sb_sleep_tick(2000, 0);
	assert(!sb_sleep_tick(999999, 1));
	request("\"sleep\",\"-1\"", 1000);
	assert(strstr(out, "error"));
	request("\"sleep\",\"86401\"", 1000);
	assert(strstr(out, "error"));
	request("\"sleep\",\"15;reboot\"", 1000);
	assert(strstr(out, "error"));
	request("\"sleep\",\"1\"", 1000);
	sb_sleep_cancel();
	assert(!sb_sleep_tick(2001, 1));
	puts("sleep menu, expiry, cancellation, bounds and handoff passed");
	return 0;
}
