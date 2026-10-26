/*
 * LMS-compatible sleep menu/countdown for sbbase. main.c advances the monotonic
 * timer and queues stop for the current Now Playing owner on expiry. Jive's
 * AlarmPlayback bridge consumes the serial field to stop local fallback audio
 * too. No thread, polling client or persistent flash state is required.
 */
#include "sleep.h"
#include "nowplaying.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t deadline;
static unsigned serial;

/** Cancel the countdown without emitting an expiry event. */
int sb_sleep_cancel(void)
{
	int changed = deadline != 0;
	deadline = 0;
	return changed;
}

/** Advance monotonic milliseconds; disconnected players cannot receive stale
 * stops. */
int sb_sleep_tick(uint64_t now, int connected)
{
	if (!connected) {
		deadline = 0;
		return 0;
	}
	if (!deadline || now < deadline)
		return 0;
	deadline = 0;
	if (!++serial)
		++serial;
	return 1;
}

/** Append countdown seconds and the deduplicated expiry serial to playerstatus.
 */
int sb_sleep_status(char *json, size_t capacity, uint64_t now)
{
	size_t length = strlen(json);
	char suffix[160];
	if (!length || json[length - 1] != '}')
		return 0;
	unsigned remaining =
		deadline > now ? (unsigned)((deadline - now + 999) / 1000) : 0;
	int n = snprintf(
		suffix,
		sizeof suffix,
		"%s\"sleep\":%u,\"will_sleep_in\":%u,\"sb_sleep_serial\":%u}",
		length > 2 ? "," : "",
		remaining,
		remaining,
		serial);
	if (n < 0 || length - 1 + (size_t)n >= capacity)
		return 0;
	memcpy(json + length - 1, suffix, (size_t)n + 1);
	return 1;
}

/** Handle stock sleep/sleepsettings commands; now is monotonic milliseconds. */
int sb_sleep_request(const char *json,
		     size_t length,
		     const char *player,
		     char *out,
		     size_t capacity,
		     uint64_t now)
{
	char args[24][512], requested[32];
	int count = np_lms_arguments(
		json, length, requested, sizeof requested, args, 24);
	if (count < 1 ||
	    (strcmp(args[0], "sleep") && strcmp(args[0], "sleepsettings")))
		return 0;
	if (!*player || strcmp(requested, player))
		goto invalid;
	if (!strcmp(args[0], "sleep")) {
		if (count != 2)
			goto invalid;
		if (!strcmp(args[1], "?")) {
			snprintf(
				out,
				capacity,
				"{\"_sleep\":%llu}",
				(unsigned long long)(deadline > now
							     ? (deadline - now +
								999) / 1000
							     : 0));
			return 1;
		}
		if (!args[1][0] ||
		    strspn(args[1], "0123456789") != strlen(args[1]) ||
		    strlen(args[1]) > 5)
			goto invalid;
		unsigned seconds = (unsigned)strtoul(args[1], NULL, 10);
		if (seconds > 86400)
			goto invalid;
		deadline = seconds ? now + (uint64_t)seconds * 1000 : 0;
		snprintf(out, capacity, "{\"_sleep\":%u}", seconds);
	} else {
		unsigned minutes[] = {0, 15, 30, 45, 60, 90};
		unsigned remaining =
			deadline > now
				? (unsigned)((deadline - now + 59999) / 60000)
				: 0;
		int initial =
			snprintf(out,
				 capacity,
				 "{\"offset\":0,\"count\":%u,\"item_loop\":[",
				 remaining ? 7 : 6);
		if (initial < 0 || (size_t)initial >= capacity)
			goto invalid;
		size_t used = (size_t)initial;
		if (remaining) {
			int n = snprintf(
				out + used,
				capacity - used,
				"{\"text\":\"Sleeping in %u "
				"minutes\",\"style\":\"itemNoAction\"},",
				remaining);
			if (n < 0 || used + (size_t)n >= capacity)
				goto invalid;
			used += (size_t)n;
		}
		for (unsigned i = 0; i < 6; i++) {
			char text[80];
			if (!i)
				snprintf(text,
					 sizeof text,
					 "Cancel sleep timer");
			else
				snprintf(text,
					 sizeof text,
					 "Sleep in %u minutes",
					 minutes[i]);
			int n = snprintf(
				out + used,
				used < capacity ? capacity - used : 0,
				"%s{\"text\":\"%s\",\"nextWindow\":\"refresh\","
				"\"actions\":{\"go\":{\"player\":0,\"cmd\":["
				"\"sleep\",\"%u\"]}}}",
				i ? "," : "",
				text,
				minutes[i] * 60);
			if (n < 0 || used + (size_t)n >= capacity)
				goto invalid;
			used += (size_t)n;
		}
		if (used + 3 >= capacity)
			goto invalid;
		memcpy(out + used, "]}", 3);
	}
	return 1;
invalid:
	snprintf(out,
		 capacity,
		 "{\"error\":\"Invalid sleep timer parameters\"}");
	return 1;
}
