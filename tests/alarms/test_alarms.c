/* Host/ARM alarm protocol and scheduler tests. No device, audio or flash
 * access. */
#define _POSIX_C_SOURCE 200809L
#include "alarms.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *player = "00:04:20:00:00:01";
static char reply[18000];

/** Execute one LMS command array against a synthetic clock. */
static const char *request(sb_alarms *a, const char *args, time_t now)
{
	char input[4096];
	snprintf(input,
		 sizeof input,
		 "{\"data\":{\"request\":[\"%s\",[%s]]}}",
		 player,
		 args);
	assert(sb_alarms_request(
		a, input, strlen(input), player, reply, sizeof reply, now));
	return reply;
}

/** Return playerstatus fields without touching a live player. */
static const char *status(sb_alarms *a, time_t now)
{
	strcpy(reply, "{}");
	assert(sb_alarms_status(a, reply, sizeof reply, now));
	return reply;
}

/** Stable local calendar test fixture, including explicit TZ/DST rules. */
static time_t civil(int year, int month, int day, int hour, int minute)
{
	struct tm t = {.tm_year = year - 1900,
		       .tm_mon = month - 1,
		       .tm_mday = day,
		       .tm_hour = hour,
		       .tm_min = minute,
		       .tm_isdst = -1};
	return mktime(&t);
}

int main(void)
{
	setenv("TZ", "UTC0", 1);
	tzset();
	time_t start = civil(2026, 10, 26, 6, 59), due = start + 60;
	sb_alarms *a = sb_alarms_open("");
	assert(a);
	assert(strstr(status(a, start), "\"alarm_state\":\"none\""));
	assert(strstr(request(a, "\"alarm\",\"add\",\"time:25200\"", start),
		      "\"id\":1"));
	assert(strstr(status(a, start), "\"alarm_state\":\"set\""));
	assert(strstr(request(a, "\"alarmsettings\"", start),
		      "__TAGGEDINPUT__"));
	assert(strstr(request(a, "\"jiveupdatealarmdays\",\"id:1\"", start),
		      "\"count\":7"));
	assert(strstr(
		request(a, "\"alarm\",\"update\",\"id:1\",\"time:bad\"", start),
		"error"));
	assert(!strstr(
		request(a,
			"\"alarm\",\"update\",\"id:1\",\"dow:1,2,3,4,5\"",
			start),
		"error"));
	sb_alarms_tick(a, start, 1000, 1);
	assert(sb_alarms_tick(a, due, 61000, 1));
	assert(strstr(status(a, due), "\"alarm_state\":\"active\""));
	assert(strstr(status(a, due), "\"sb_alarm_serial\":1"));
	assert(!sb_alarms_tick(a, due + 1, 62000, 1));
	assert(!strstr(request(a, "\"jivealarm\",\"snooze:1\"", due + 1),
		       "error"));
	assert(strstr(status(a, due + 1), "\"alarm_state\":\"snooze\""));
	sb_alarms_tick(a, due + 2, 63000, 1);
	sb_alarms_tick(a, due + 541, 602000, 1);
	assert(strstr(status(a, due + 541), "\"alarm_state\":\"active\""));
	request(a, "\"jivealarm\",\"stop:1\",\"continueAudio:1\"", due + 542);
	assert(strstr(status(a, due + 542), "\"sb_alarm_event\":\"dismiss\""));
	/* Backward wall-clock correction must not ring twice on the same date.
	 */
	sb_alarms_tick(a, start, 603000, 1);
	sb_alarms_tick(a, due, 663000, 1);
	assert(strstr(status(a, due), "\"alarm_state\":\"set\""));
	request(a, "\"alarm\",\"update\",\"id:1\",\"repeat:0\"", due);
	time_t next = civil(2026, 10, 27, 7, 0);
	sb_alarms_tick(a, next - 1, 664000, 1);
	sb_alarms_tick(a, next, 665000, 1);
	request(a, "\"jivealarm\",\"stop:1\"", next);
	assert(strstr(status(a, next), "\"alarm_state\":\"none\""));
	assert(strstr(request(a, "\"alarms\",\"filter:all\"", next),
		      "\"enabled\":0"));
	/* Fixed capacity: a supplied existing id cannot cause an add to
	 * overwrite. */
	for (unsigned i = 1; i < SB_ALARM_MAX; i++)
		assert(!strstr(
			request(a, "\"alarm\",\"add\",\"time:30000\"", next),
			"error"));
	assert(strstr(
		request(a, "\"alarm\",\"add\",\"id:1\",\"time:30000\"", next),
		"error"));
	assert(strstr(request(a, "\"alarms\",\"filter:all\"", next),
		      "\"count\":8"));
	char tiny[3] = "{}";
	assert(!sb_alarms_status(a, tiny, sizeof tiny, next));
	assert(!strcmp(tiny, "{}"));
	sb_alarms_close(a);
	/* Atomic persistence/restart, permissions, failed writes and corrupt
	 * files. */
	char path[] = "/tmp/sb-alarms-test-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	close(fd);
	unlink(path);
	a = sb_alarms_open(path);
	request(a,
		"\"alarm\",\"add\",\"time:25200\",\"playlisturl:sb-tone://"
		"default\"",
		start);
	sb_alarms_close(a);
	a = sb_alarms_open(path);
	assert(strstr(request(a, "\"alarms\",\"filter:all\"", start),
		      "sb-tone://default"));
	sb_alarms_tick(a, start, 1000, 1);
	sb_alarms_tick(a, due, 61000, 1);
	sb_alarms_close(a);
	a = sb_alarms_open(path);
	sb_alarms_tick(a, due + 1, 9000, 1);
	assert(strstr(status(a, due + 1), "\"alarm_state\":\"active\""));
	request(a, "\"jivealarm\",\"snooze:1\"", due + 1);
	sb_alarms_close(a);
	a = sb_alarms_open(path);
	sb_alarms_tick(a, due + 2, 10000, 1);
	assert(strstr(status(a, due + 2), "\"alarm_state\":\"snooze\""));
	sb_alarms_tick(a, due + 541, 549000, 1);
	assert(strstr(status(a, due + 541), "\"alarm_state\":\"active\""));
	sb_alarms_close(a);
	FILE *f = fopen(path, "w");
	assert(f);
	fputs("broken\n", f);
	fclose(f);
	a = sb_alarms_open(path);
	assert(strstr(request(a, "\"alarm\",\"add\",\"time:25200\"", start),
		      "could not be saved"));
	sb_alarms_close(a);
	unlink(path);
	a = sb_alarms_open("/nonexistent/sb-alarms-test/file");
	assert(strstr(request(a, "\"alarm\",\"add\",\"time:25200\"", start),
		      "could not be saved"));
	assert(strstr(request(a, "\"alarms\",\"filter:all\"", start),
		      "\"count\":0"));
	sb_alarms_close(a);
	/* An NTP step immediately after firing cannot expire/extend the
	 * timeout. */
	a = sb_alarms_open("");
	start = civil(2026, 10, 26, 6, 59);
	due = start + 60;
	request(a, "\"alarm\",\"add\",\"time:25200\"", start);
	sb_alarms_tick(a, start, 1000, 1);
	sb_alarms_tick(a, due, 61000, 1);
	sb_alarms_tick(a, due + 86400, 62000, 1);
	assert(strstr(status(a, due + 86400), "\"alarm_state\":\"active\""));
	sb_alarms_tick(a, due + 89999, 3660000, 1);
	assert(strstr(status(a, due + 89999), "\"alarm_state\":\"active\""));
	sb_alarms_tick(a, due + 90000, 3661000, 1);
	assert(!strstr(status(a, due + 90000), "\"alarm_state\":\"active\""));
	sb_alarms_close(a);
	/* libc calendar scheduling uses a named timezone, not fixed 86400s
	 * days. */
	setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
	tzset();
	a = sb_alarms_open("");
	start = civil(2026, 3, 28, 8, 0);
	request(a, "\"alarm\",\"add\",\"time:25200\"", start);
	char expected[80];
	snprintf(expected,
		 sizeof expected,
		 "\"alarm_next2\":%lld",
		 (long long)civil(2026, 3, 29, 7, 0));
	assert(strstr(status(a, start), expected));
	sb_alarms_close(a);
	puts("alarm protocol, scheduling, snooze, clock, capacity and "
	     "persistence tests passed");
	return 0;
}
