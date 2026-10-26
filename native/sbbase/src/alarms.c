/*
 * Bounded LMS alarm configuration, menu protocol and scheduler for sbbase.
 * main.c calls this from its existing event loop and publishes stock alarm
 * playerstatus fields. Jive's AlarmSnooze owns RTC wake-up/fallback/UI;
 * AlarmPlayback.lua consumes the event fields for local audio and volume.
 * Only configuration and alarm transitions are persisted, never idle ticks.
 */
#define _POSIX_C_SOURCE 200809L
#include "alarms.h"
#include "nowplaying.h"
#include "sleep.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
	unsigned id, enabled, repeat, days, shuffle;
	long when;
	int volume, last_date;
	char source[32];
} alarm_entry;

struct sb_alarms {
	alarm_entry entries[SB_ALARM_MAX];
	unsigned enabled, volume, fade, snooze, timeout, counter, serial;
	unsigned active_id, phase; /* 0=idle, 1=active, 2=snoozed */
	char event[16], path[512];
	time_t next, end, previous_wall;
	uint64_t deadline, previous_mono;
	int next_slot, blocked, dirty;
};

typedef struct {
	char *out;
	size_t capacity, used;
	int failed;
} writer;

/** Append formatted JSON; overflow is an error, never a truncated response. */
static void emit(writer *w, const char *format, ...)
{
	if (w->failed || w->used >= w->capacity)
		return;
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(w->out + w->used, w->capacity - w->used, format, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= w->capacity - w->used)
		w->failed = 1;
	else
		w->used += (size_t)n;
}

/** Parse a strict decimal protocol/configuration value within inclusive bounds.
 */
static int number(const char *s, long low, long high, long *value)
{
	char *end;
	if (!s || !*s || *s == '+' || *s == ' ' || *s == '\t')
		return 0;
	errno = 0;
	long n = strtol(s, &end, 10);
	if (errno || *end || n < low || n > high)
		return 0;
	*value = n;
	return 1;
}

/** Find one tagged LMS argument; command arrays are decoded by nowplaying.c. */
static const char *tag(char args[][512], int count, const char *key)
{
	size_t length = strlen(key);
	for (int i = 1; i < count; i++)
		if (!strncmp(args[i], key, length) && args[i][length] == ':')
			return args[i] + length + 1;
	return NULL;
}

/** Only explicit local source identifiers can enter persistence or JSON output.
 */
static int valid_source(const char *source)
{
	return !strcmp(source, "CURRENT_PLAYLIST") ||
	       !strcmp(source, "sb-tone://default") ||
	       (strlen(source) == 12 && !strncmp(source, "sbpreset://", 11) &&
		source[11] >= '1' && source[11] <= '6');
}

/** Locate a configured alarm by its stable per-device numeric identity. */
static alarm_entry *find_alarm(sb_alarms *a, unsigned id)
{
	for (unsigned i = 0; i < SB_ALARM_MAX; i++)
		if (a->entries[i].id == id && id)
			return &a->entries[i];
	return NULL;
}

/** Atomic same-directory configuration/state save; no extended storage
 * required. */
static int persist(sb_alarms *a)
{
	char temporary[550];
	if (a->blocked)
		return 0;
	if (!a->path[0])
		return 1;
	snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", a->path);
	int fd = mkstemp(temporary);
	if (fd < 0)
		return 0;
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(temporary);
		return 0;
	}
	int ok = fprintf(f,
			 "SBALARMS 1\noptions %u %u %u %u %u %u %u %u %u %lld "
			 "%s\n",
			 a->enabled,
			 a->volume,
			 a->fade,
			 a->snooze,
			 a->timeout,
			 a->counter,
			 a->serial,
			 a->active_id,
			 a->phase,
			 (long long)a->end,
			 a->event) > 0;
	for (unsigned i = 0; i < SB_ALARM_MAX; i++) {
		alarm_entry *e = &a->entries[i];
		if (e->id && fprintf(f,
				     "alarm %u %u %u %u %u %ld %d %d %s\n",
				     e->id,
				     e->enabled,
				     e->repeat,
				     e->days,
				     e->shuffle,
				     e->when,
				     e->volume,
				     e->last_date,
				     e->source) < 0)
			ok = 0;
	}
	if (fflush(f) || fsync(fd))
		ok = 0;
	if (fclose(f))
		ok = 0;
	if (ok && rename(temporary, a->path) == 0)
		return 1;
	unlink(temporary);
	return 0;
}

/** Compute a wall-clock occurrence in libc's configured local timezone/DST. */
static time_t occurrence(const alarm_entry *e, time_t after)
{
	if (!e->id || !e->enabled)
		return 0;
	if (e->when >= 86400)
		return e->when > after ? (time_t)e->when : 0;
	struct tm base;
	if (!localtime_r(&after, &base))
		return 0;
	for (int day = 0; day < 8; day++) {
		struct tm t = base;
		t.tm_mday += day;
		t.tm_hour = (int)e->when / 3600;
		t.tm_min = ((int)e->when % 3600) / 60;
		t.tm_sec = 0;
		t.tm_isdst = -1;
		time_t candidate = mktime(&t);
		int date = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 +
			   t.tm_mday;
		if (candidate > after && (e->days & (1u << t.tm_wday)) &&
		    date != e->last_date)
			return candidate;
	}
	return 0;
}

/** Recompute the earliest occurrence only after edits, transitions or clock
 * jumps. */
static void plan(sb_alarms *a, time_t now)
{
	a->next = 0;
	a->next_slot = -1;
	a->dirty = 0;
	if (!a->enabled || now < 1577836800)
		return; /* Wait for a plausible boot clock. */
	for (unsigned i = 0; i < SB_ALARM_MAX; i++) {
		time_t due = occurrence(&a->entries[i], now - 1);
		if (due && (!a->next || due < a->next)) {
			a->next = due;
			a->next_slot = (int)i;
		}
	}
}

/** Load a bounded, versioned file. Corrupt files remain untouched and
 * read-only. */
sb_alarms *sb_alarms_open(const char *path)
{
	sb_alarms *a = calloc(1, sizeof *a);
	if (!a)
		return NULL;
	a->enabled = 1;
	a->volume = 50;
	a->snooze = 540;
	a->timeout = 3600;
	a->counter = 1;
	a->dirty = 1;
	a->next_slot = -1;
	strcpy(a->event, "idle");
	if (!path || !*path)
		return a;
	if (strlen(path) >= sizeof a->path) {
		free(a);
		return NULL;
	}
	strcpy(a->path, path);
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		a->blocked = errno != ENOENT;
		return a;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 8192) {
		close(fd);
		a->blocked = 1;
		return a;
	}
	FILE *f = fdopen(fd, "r");
	char line[512], extra;
	long long end;
	int bad = !f;
	if (!bad &&
	    (!fgets(line, sizeof line, f) || strcmp(line, "SBALARMS 1\n")))
		bad = 1;
	if (!bad && (!fgets(line, sizeof line, f) ||
		     sscanf(line,
			    "options %u %u %u %u %u %u %u %u %u %lld %15s %c",
			    &a->enabled,
			    &a->volume,
			    &a->fade,
			    &a->snooze,
			    &a->timeout,
			    &a->counter,
			    &a->serial,
			    &a->active_id,
			    &a->phase,
			    &end,
			    a->event,
			    &extra) != 11))
		bad = 1;
	if (!bad) {
		a->end = (time_t)end;
		if (a->enabled > 1 || a->volume > 100 || a->fade > 1 ||
		    a->snooze < 1 || a->snooze > 86400 || a->timeout < 1 ||
		    a->timeout > 86400 || a->phase > 2 || end < 0 ||
		    end > INT_MAX || !a->counter)
			bad = 1;
	}
	unsigned used = 0;
	while (!bad && fgets(line, sizeof line, f)) {
		alarm_entry e;
		memset(&e, 0, sizeof e);
		if (used == SB_ALARM_MAX ||
		    sscanf(line,
			   "alarm %u %u %u %u %u %ld %d %d %31s %c",
			   &e.id,
			   &e.enabled,
			   &e.repeat,
			   &e.days,
			   &e.shuffle,
			   &e.when,
			   &e.volume,
			   &e.last_date,
			   e.source,
			   &extra) != 9 ||
		    !e.id || e.enabled > 1 || e.repeat > 1 || e.days > 127 ||
		    e.shuffle > 2 || e.when < 0 || e.when > INT_MAX ||
		    e.volume < -1 || e.volume > 100 ||
		    !valid_source(e.source) || find_alarm(a, e.id)) {
			bad = 1;
			break;
		}
		a->entries[used++] = e;
	}
	if (f) {
		if (ferror(f))
			bad = 1;
		fclose(f);
	} else
		close(fd);
	if (a->phase && !find_alarm(a, a->active_id))
		bad = 1;
	if (strcmp(a->event, "idle") && strcmp(a->event, "start") &&
	    strcmp(a->event, "snooze") && strcmp(a->event, "stop") &&
	    strcmp(a->event, "dismiss"))
		bad = 1;
	for (unsigned i = 0; i < used; i++)
		if (a->entries[i].id >= a->counter)
			bad = 1;
	if (bad) {
		memset(a->entries, 0, sizeof a->entries);
		a->enabled = 0;
		a->phase = 0;
		a->blocked = 1;
		a->volume = 50;
		a->snooze = 540;
		a->timeout = 3600;
		a->fade = 0;
		a->active_id = 0;
		a->serial = 0;
		a->counter = 1;
		a->end = 0;
		strcpy(a->event, "idle");
		fprintf(stderr,
			"alarms: invalid/unreadable configuration; preserving "
			"original file\n");
	}
	return a;
}

/** Free in-memory scheduler state; the configuration file is retained. */
void sb_alarms_close(sb_alarms *a)
{
	free(a);
}

/** Return whether an alarm is sounding or snoozing, for sleep-timer
 * arbitration. */
int sb_alarms_active(const sb_alarms *a)
{
	return a && a->phase;
}

/** Validate a staged backup and remove all live alarm authorization/state. */
int sb_alarms_prepare_restore(const char *path)
{
	sb_alarms *a = sb_alarms_open(path);
	if (!a)
		return 0;
	int ok = !a->blocked;
	a->enabled = 0;
	a->phase = 0;
	a->active_id = 0;
	a->serial = 0;
	a->end = 0;
	strcpy(a->event, "idle");
	for (unsigned i = 0; i < SB_ALARM_MAX; i++)
		a->entries[i].last_date = 0;
	if (ok)
		ok = persist(a);
	sb_alarms_close(a);
	return ok;
}

/** Record one audio transition for the Jive bridge. Serial avoids duplicate
 * starts. */
static void
transition(sb_alarms *a, const char *event, unsigned phase, time_t end)
{
	a->phase = phase;
	a->end = end;
	a->deadline = 0;
	/* Seed the monotonic deadline at the transition, not at the next tick:
	 * a clock correction between those two must not expire/extend an alarm.
	 * Commands arrive between loop ticks; account for their elapsed wall
	 * seconds only within the normal five-second event-loop window. */
	if (phase && a->previous_wall) {
		unsigned duration = phase == 2 ? a->snooze : a->timeout;
		long since_tick = (long)(end - duration - a->previous_wall);
		if (since_tick >= 0 && since_tick <= 5)
			a->deadline =
				a->previous_mono +
				((uint64_t)duration + (uint64_t)since_tick) *
					1000;
	}
	a->serial++;
	if (!a->serial)
		a->serial++;
	snprintf(a->event, sizeof a->event, "%s", event);
}

/** Advance without replaying old missed alarms or double-firing a repeated DST
 * hour. */
int sb_alarms_tick(sb_alarms *a, time_t wall, uint64_t mono, int connected)
{
	int changed = 0;
	if (!a)
		return 0;
	unsigned old_serial = a->serial;
	long delta = a->previous_wall ? (long)(wall - a->previous_wall) : 0;
	uint64_t elapsed =
		mono >= a->previous_mono ? (mono - a->previous_mono) / 1000 : 0;
	if (delta < 0 || delta > (long)elapsed + 5 ||
	    (!a->next && wall >= 1577836800 && a->previous_wall < 1577836800)) {
		a->dirty = 1;
		if (a->next && a->next <= wall && wall - a->next <= 90)
			a->dirty = 0;
		changed = 1;
		/* Preserve active/snooze durations across NTP/manual wall-clock
		 * jumps, including a subsequent restart. Never extend the
		 * monotonic deadline. */
		if (a->phase && a->deadline) {
			a->end = wall + (time_t)(a->deadline > mono
							 ? (a->deadline - mono +
							    999) / 1000
							 : 0);
			if (!persist(a))
				fprintf(stderr,
					"alarms: cannot persist clock "
					"correction\n");
		}
	}
	a->previous_wall = wall;
	a->previous_mono = mono;
	if (a->dirty) {
		plan(a, wall);
		changed = 1;
	}
	if (a->phase && !a->deadline) {
		long left = (long)(a->end - wall);
		a->deadline = mono + (uint64_t)(left > 0 ? left : 0) * 1000;
	}
	if (a->phase && mono >= a->deadline) {
		if (a->phase == 2 && connected)
			transition(a, "start", 1, wall + a->timeout);
		else
			transition(a, "stop", 0, 0);
		changed = 1;
	}
	if (a->next && wall >= a->next && a->next_slot >= 0) {
		alarm_entry *e = &a->entries[a->next_slot];
		struct tm civil;
		localtime_r(&a->next, &civil);
		e->last_date = (civil.tm_year + 1900) * 10000 +
			       (civil.tm_mon + 1) * 100 + civil.tm_mday;
		if (!e->repeat || e->when >= 86400)
			e->enabled = 0;
		if (connected && !a->phase && wall - a->next <= 90) {
			a->active_id = e->id;
			transition(a, "start", 1, wall + a->timeout);
			fprintf(stderr, "alarms: alarm %u active\n", e->id);
		}
		plan(a, wall);
		changed = 1;
		if (!persist(a))
			fprintf(stderr, "alarms: cannot persist occurrence\n");
	} else if (a->serial != old_serial) {
		if (!persist(a))
			fprintf(stderr, "alarms: cannot persist transition\n");
	}
	return changed;
}

/** Build the ordinary server-owned Settings entry without patching stock Jive.
 */
void sb_alarms_menu(char *out,
		    size_t capacity,
		    const char *player,
		    int notification)
{
	const char *item = "{\"id\":\"settingsAlarm\",\"node\":\"settings\","
			   "\"text\":\"Alarm "
			   "Clock\",\"weight\":29,\"actions\":{\"go\":{\"cmd\":"
			   "[\"alarmsettings\"],\"player\":0}}}," SB_SLEEP_MENU;
	if (notification)
		snprintf(out,
			 capacity,
			 "[\"menustatus\",[%s],\"add\",\"%s\"]",
			 item,
			 player);
	else
		snprintf(out,
			 capacity,
			 "{\"count\":2,\"offset\":0,\"item_loop\":[%s]}",
			 item);
}

/** Append stock v2 alarm metadata plus a private, bounded local playback event.
 */
int sb_alarms_status(sb_alarms *a, char *json, size_t capacity, time_t now)
{
	if (!a)
		return 0;
	if (a->dirty)
		plan(a, now);
	size_t length = strlen(json);
	if (!length || json[length - 1] != '}')
		return 0;
	alarm_entry *next =
		a->next_slot >= 0 ? &a->entries[a->next_slot] : NULL;
	alarm_entry *active = find_alarm(a, a->active_id);
	char days[8];
	for (unsigned i = 0; i < 7; i++)
		days[i] = next && (next->days & (1u << i)) ? '1' : '0';
	days[7] = 0;
	const char *state = a->phase == 1   ? "active"
			    : a->phase == 2 ? "snooze"
			    : a->next	    ? "set"
					    : "none";
	char suffix[768];
	writer w = {suffix, sizeof suffix, 0, 0};
	emit(&w,
	     "%s\"alarm_state\":\"%s\",\"alarm_version\":2,\"alarm_next\":%lld,"
	     "\"alarm_next2\":%lld,"
	     "\"alarm_repeat\":%u,\"alarm_days\":\"%s\",\"alarm_snooze_"
	     "seconds\":%u,\"alarm_timeout_seconds\":%u,"
	     "\"sb_alarm_serial\":%u,\"sb_alarm_event\":\"%s\",\"sb_alarm_"
	     "source\":\"%s\","
	     "\"sb_alarm_volume\":%u,\"sb_alarm_fade\":%u}",
	     length > 2 ? "," : "",
	     state,
	     (long long)a->next,
	     (long long)a->next,
	     next ? next->repeat : 0,
	     days,
	     a->snooze,
	     a->timeout,
	     a->serial,
	     a->event,
	     active ? active->source : "sb-tone://default",
	     active && active->volume >= 0 ? (unsigned)active->volume
					   : a->volume,
	     a->fade);
	if (w.failed || length - 1 + w.used >= capacity)
		return 0;
	memcpy(json + length - 1, suffix, w.used + 1);
	return 1;
}

/** Emit a time-entry item using the stock SlimBrowser time input widget. */
static void time_item(writer *w, const char *text, unsigned id, long seconds)
{
	emit(w,
	     "{\"text\":\"%s\",\"input\":{\"initialText\":%ld,\"title\":\"%s\","
	     "\"_inputStyle\":\"time\",\"len\":1},"
	     "\"actions\":{\"do\":{\"player\":0,\"cmd\":[\"alarm\",\"%s\"],"
	     "\"params\":{\"time\":\"__TAGGEDINPUT__\"%s,\"id\":%u}}},"
	     "\"nextWindow\":\"%s\"}",
	     text,
	     seconds,
	     text,
	     id ? "update" : "add",
	     id ? "" : ",\"enabled\":1",
	     id,
	     id ? "parent" : "refresh");
}

/** Generate one toggle using LMS action dictionaries and native checkbox style.
 */
static void toggle(writer *w,
		   const char *text,
		   const char *command,
		   const char *key,
		   unsigned id,
		   unsigned value)
{
	emit(w,
	     "{\"text\":\"%s\",\"checkbox\":%u,\"onClick\":\"refreshOrigin\","
	     "\"actions\":{"
	     "\"on\":{\"player\":0,\"cmd\":[\"%s\"%s],\"params\":{\"id\":%u,\"%"
	     "s\":1}},"
	     "\"off\":{\"player\":0,\"cmd\":[\"%s\"%s],\"params\":{\"id\":%u,"
	     "\"%s\":0}}}}",
	     text,
	     value,
	     command,
	     !strcmp(command, "alarm") ? ",\"update\"" : "",
	     id,
	     key,
	     command,
	     !strcmp(command, "alarm") ? ",\"update\"" : "",
	     id,
	     key);
}

/** Build settings/edit/day/sound menus with the same LMS command vocabulary. */
static void menu(sb_alarms *a, const char *command, alarm_entry *e, writer *w)
{
	unsigned count = 0;
	emit(w, "{\"offset\":0,\"item_loop\":[");
	if (!strcmp(command, "alarmsettings")) {
		emit(w,
		     "{\"text\":\"All "
		     "Alarms\",\"choiceStrings\":[\"Off\",\"On\"],"
		     "\"selectedIndex\":%u,\"actions\":{\"do\":{\"choices\":["
		     "{\"player\":0,\"cmd\":[\"alarm\",\"disableall\"]},{"
		     "\"player\":0,\"cmd\":[\"alarm\",\"enableall\"]}]}}}",
		     a->enabled + 1);
		count++;
		for (unsigned i = 0; i < SB_ALARM_MAX; i++)
			if (a->entries[i].id) {
				alarm_entry *item = &a->entries[i];
				long seconds =
					item->when < 86400 ? item->when : 0;
				emit(w,
				     ",{\"text\":\"Alarm %u: "
				     "%02ld:%02ld%s\",\"actions\":{\"go\":{"
				     "\"player\":0,\"cmd\":["
				     "\"jiveupdatealarm\"],\"params\":{\"id\":%"
				     "u}}}}",
				     ++count - 1,
				     seconds / 3600,
				     seconds % 3600 / 60,
				     item->enabled ? "" : " (Off)",
				     item->id);
			}
		emit(w, ",");
		time_item(w, "Add Alarm", 0, 25200);
		count++;
		emit(w,
		     ",{\"text\":\"Alarm "
		     "Volume\",\"actions\":{\"go\":{\"player\":0,\"cmd\":["
		     "\"jivealarmvolume\"]}}},");
		count++;
		toggle(w, "Fade In", "jivealarm", "fadein", 0, a->fade);
		count++;
	} else if (!strcmp(command, "jivealarmvolume")) {
		emit(w,
		     "{\"slider\":1,\"min\":1,\"max\":100,\"sliderIcons\":"
		     "\"volume\",\"initial\":%u,\"actions\":{\"do\":{"
		     "\"player\":0,\"cmd\":[\"alarm\",\"defaultvolume\"],"
		     "\"params\":{\"valtag\":\"volume\"}}}}",
		     a->volume);
		count = 1;
	} else if (!strcmp(command, "jiveupdatealarmdays") && e) {
		const char *days[] = {"Sunday",
				      "Monday",
				      "Tuesday",
				      "Wednesday",
				      "Thursday",
				      "Friday",
				      "Saturday"};
		for (unsigned day = 0; day < 7; day++) {
			if (day)
				emit(w, ",");
			emit(w,
			     "{\"text\":\"%s\",\"checkbox\":%u,\"onClick\":"
			     "\"refreshGrandparent\",\"actions\":{"
			     "\"on\":{\"player\":0,\"cmd\":[\"alarm\","
			     "\"update\"],\"params\":{\"id\":%u,\"dowAdd\":%u}}"
			     ","
			     "\"off\":{\"player\":0,\"cmd\":[\"alarm\","
			     "\"update\"],\"params\":{\"id\":%u,\"dowDel\":%u}}"
			     "}}",
			     days[day],
			     !!(e->days & (1u << day)),
			     e->id,
			     day,
			     e->id,
			     day);
		}
		count = 7;
	} else if (!strcmp(command, "playlists") && e) {
		for (unsigned i = 0; i < 8; i++) {
			char source[32], label[40];
			if (!i) {
				strcpy(source, "CURRENT_PLAYLIST");
				strcpy(label, "Current Playlist");
			} else if (i == 1) {
				strcpy(source, "sb-tone://default");
				strcpy(label, "Alarm Sound");
			} else {
				snprintf(source,
					 sizeof source,
					 "sbpreset://%u",
					 i - 1);
				snprintf(label,
					 sizeof label,
					 "Preset %u",
					 i - 1);
			}
			if (i)
				emit(w, ",");
			emit(w,
			     "{\"text\":\"%s\",\"radio\":%u,\"nextWindow\":"
			     "\"refreshOrigin\",\"actions\":{\"do\":{"
			     "\"player\":0,\"cmd\":[\"alarm\",\"update\"],"
			     "\"params\":{\"id\":%u,\"playlisturl\":\"%s\"}}}}",
			     label,
			     !strcmp(e->source, source),
			     e->id,
			     source);
		}
		count = 8;
	} else if (e) {
		toggle(w, "Enabled", "alarm", "enabled", e->id, e->enabled);
		emit(w, ",");
		time_item(w, "Set Time", e->id, e->when < 86400 ? e->when : 0);
		emit(w,
		     ",{\"text\":\"Set "
		     "Days\",\"actions\":{\"go\":{\"player\":0,\"cmd\":["
		     "\"jiveupdatealarmdays\"],\"params\":{\"id\":%u}}}},"
		     "{\"text\":\"Select "
		     "Playlist\",\"actions\":{\"go\":{\"player\":0,\"cmd\":["
		     "\"alarm\",\"playlists\"],\"params\":{\"id\":%u,\"menu\":"
		     "1}}}},",
		     e->id,
		     e->id);
		emit(w,
		     "{\"text\":\"Shuffle\",\"count\":3,\"offset\":0,\"item_"
		     "loop\":[");
		const char *shuffle[] = {"Off", "Songs", "Albums"};
		for (unsigned i = 0; i < 3; i++) {
			emit(w,
			     "%s{\"text\":\"%s\",\"radio\":%u,\"onClick\":"
			     "\"refreshOrigin\",\"nextWindow\":\"refresh\","
			     "\"actions\":{\"do\":{\"player\":0,\"cmd\":["
			     "\"alarm\",\"update\"],\"params\":{\"id\":%u,"
			     "\"shufflemode\":%u}}}}",
			     i ? "," : "",
			     shuffle[i],
			     e->shuffle == i,
			     e->id,
			     i);
		}
		emit(w, "]}");
		for (unsigned i = 0; i < 2; i++) {
			emit(w,
			     ",{\"text\":\"%s\",\"radio\":%u,\"onClick\":"
			     "\"refreshOrigin\",\"actions\":{\"do\":{"
			     "\"player\":0,\"cmd\":[\"alarm\",\"update\"],"
			     "\"params\":{\"id\":%u,\"repeat\":%u}}}}",
			     i ? "One-time Alarm" : "Repeat Alarm",
			     e->repeat == !i,
			     e->id,
			     !i);
		}
		emit(w,
		     ",{\"text\":\"Delete "
		     "Alarm\",\"count\":2,\"offset\":0,\"item_loop\":["
		     "{\"text\":\"Cancel\",\"nextWindow\":\"parent\","
		     "\"actions\":{\"go\":{\"cmd\":[\"jiveblankcommand\"],"
		     "\"player\":0}}},"
		     "{\"text\":\"Delete "
		     "Alarm\",\"nextWindow\":\"grandparent\",\"actions\":{"
		     "\"go\":{\"cmd\":[\"alarm\",\"delete\"],\"params\":{"
		     "\"id\":%u},\"player\":0}}}]}",
		     e->id);
		count = 8;
	}
	emit(w, "],\"count\":%u}", count);
}

/** Handle bounded LMS alarm queries/edits. Mutations roll back on write
 * failure. */
int sb_alarms_request(sb_alarms *a,
		      const char *json,
		      size_t length,
		      const char *player,
		      char *out,
		      size_t capacity,
		      time_t now)
{
	char args[24][512], requested[32];
	int count = np_lms_arguments(
		json, length, requested, sizeof requested, args, 24);
	if (count < 1)
		return 0;
	const char *command = args[0];
	if (strcmp(command, "alarm") && strcmp(command, "alarms") &&
	    strcmp(command, "alarmsettings") &&
	    strcmp(command, "jiveupdatealarm") &&
	    strcmp(command, "jiveupdatealarmdays") &&
	    strcmp(command, "jivealarmvolume") && strcmp(command, "jivealarm"))
		return 0;
	if (!a || !player[0] || (requested[0] && strcmp(requested, player)))
		goto invalid;
	long id = 0, value;
	const char *parameter = tag(args, count, "id");
	if (parameter && !number(parameter, 0, INT_MAX, &id))
		goto invalid;
	alarm_entry *e = id ? find_alarm(a, (unsigned)id) : NULL;
	writer w = {out, capacity, 0, 0};
	const char *sub = count > 1 ? args[1] : "";
	if (!strcmp(command, "alarms")) {
		unsigned items = 0;
		const char *filter = tag(args, count, "filter");
		if (filter && strcmp(filter, "all") &&
		    strcmp(filter, "defined") && strcmp(filter, "enabled"))
			goto invalid;
		emit(&w,
		     "{\"offset\":0,\"fade\":%u,\"alarms_loop\":[",
		     a->fade);
		for (unsigned i = 0; i < SB_ALARM_MAX; i++) {
			alarm_entry *item = &a->entries[i];
			if (!item->id ||
			    ((!filter || !strcmp(filter, "enabled")) &&
			     !item->enabled))
				continue;
			char days[20];
			size_t used = 0;
			for (unsigned d = 0; d < 7; d++)
				if (item->days & (1u << d))
					used += (size_t)snprintf(
						days + used,
						sizeof days - used,
						"%s%u",
						used ? "," : "",
						d);
			days[used] = 0;
			emit(&w,
			     "%s{\"id\":%u,\"time\":%ld,\"enabled\":%u,"
			     "\"repeat\":%u,\"dow\":\"%s\",\"volume\":%u,"
			     "\"usesdefaultvolume\":%u,\"shufflemode\":%u,"
			     "\"url\":\"%s\"}",
			     items++ ? "," : "",
			     item->id,
			     item->when,
			     item->enabled,
			     item->repeat,
			     days,
			     item->volume < 0 ? a->volume
					      : (unsigned)item->volume,
			     item->volume < 0,
			     item->shuffle,
			     item->source);
		}
		emit(&w, "],\"count\":%u}", items);
	} else if (!strcmp(command, "alarmsettings") ||
		   !strcmp(command, "jivealarmvolume") ||
		   !strcmp(command, "jiveupdatealarm") ||
		   !strcmp(command, "jiveupdatealarmdays") ||
		   (!strcmp(command, "alarm") && !strcmp(sub, "playlists"))) {
		if (strcmp(command, "alarmsettings") &&
		    strcmp(command, "jivealarmvolume") && !e)
			goto invalid;
		menu(a, !strcmp(command, "alarm") ? sub : command, e, &w);
	} else {
		sb_alarms before = *a;
		int valid = 1;
		if (!strcmp(command, "jivealarm")) {
			if ((parameter = tag(args, count, "fadein"))) {
				if (!number(parameter, 0, 1, &value))
					valid = 0;
				else
					a->fade = (unsigned)value;
			} else if ((parameter = tag(args, count, "snooze")) &&
				   !strcmp(parameter, "1")) {
				if (a->phase == 1)
					transition(a,
						   "snooze",
						   2,
						   now + a->snooze);
			} else if ((parameter = tag(args, count, "stop")) &&
				   !strcmp(parameter, "1")) {
				if (a->phase)
					transition(
						a,
						tag(args,
						    count,
						    "continueAudio") &&
								!strcmp(tag(args,
									    count,
									    "co"
									    "nt"
									    "in"
									    "ue"
									    "Au"
									    "di"
									    "o"),
									"1")
							? "dismiss"
							: "stop",
						0,
						0);
			} else
				valid = 0;
		} else if (!strcmp(sub, "enableall") ||
			   !strcmp(sub, "disableall")) {
			a->enabled = !strcmp(sub, "enableall");
			if (!a->enabled && a->phase)
				transition(a, "stop", 0, 0);
		} else if (!strcmp(sub, "defaultvolume")) {
			if (!number(tag(args, count, "volume"), 1, 100, &value))
				valid = 0;
			else
				a->volume = (unsigned)value;
		} else if (!strcmp(sub, "delete")) {
			if (!e)
				valid = 0;
			else {
				if (a->active_id == e->id && a->phase)
					transition(a, "stop", 0, 0);
				memset(e, 0, sizeof *e);
			}
		} else if (!strcmp(sub, "add") || !strcmp(sub, "update")) {
			if (!strcmp(sub, "add")) {
				e = NULL;
				if (!number(tag(args, count, "time"),
					    0,
					    INT_MAX,
					    &value))
					valid = 0;
				else
					for (unsigned i = 0; i < SB_ALARM_MAX;
					     i++)
						if (!a->entries[i].id) {
							e = &a->entries[i];
							memset(e, 0, sizeof *e);
							if (a->counter >=
							    INT_MAX) {
								valid = 0;
								break;
							}
							e->id = a->counter++;
							e->enabled = 1;
							e->repeat = 1;
							e->days = 127;
							e->volume = -1;
							strcpy(e->source,
							       "CURRENT_"
							       "PLAYLIST");
							break;
						}
			}
			if (!e)
				valid = 0;
			if (valid) {
#define SET_NUMBER(key, low, high, assignment)                                 \
	if ((parameter = tag(args, count, key))) {                             \
		if (!number(parameter, low, high, &value))                     \
			valid = 0;                                             \
		else {                                                         \
			assignment;                                            \
		}                                                              \
	}
				SET_NUMBER("time",
					   0,
					   INT_MAX,
					   e->when = value < 86400
							     ? value / 60 * 60
							     : value;
					   e->last_date = 0)
				SET_NUMBER("enabled",
					   0,
					   1,
					   e->enabled = (unsigned)value)
				SET_NUMBER("repeat",
					   0,
					   1,
					   e->repeat = (unsigned)value)
				SET_NUMBER("volume",
					   0,
					   100,
					   e->volume = (int)value)
				SET_NUMBER("usedefaultvolume",
					   0,
					   1,
					   if (value) e->volume = -1)
				SET_NUMBER("shufflemode",
					   0,
					   2,
					   e->shuffle = (unsigned)value)
				SET_NUMBER(
					"dowAdd", 0, 6, e->days |= 1u << value)
				SET_NUMBER("dowDel",
					   0,
					   6,
					   e->days &= ~(1u << value))
#undef SET_NUMBER
				if ((parameter = tag(args, count, "dow"))) {
					e->days = 0;
					while (*parameter) {
						if (*parameter < '0' ||
						    *parameter > '6') {
							valid = 0;
							break;
						}
						e->days |= 1u << (*parameter++ -
								  '0');
						if (!*parameter)
							break;
						if (*parameter++ != ',' ||
						    !*parameter) {
							valid = 0;
							break;
						}
					}
				}
				if ((parameter =
					     tag(args, count, "playlisturl")) ||
				    (parameter = tag(args, count, "url"))) {
					if (!strcmp(parameter, "0"))
						parameter = "CURRENT_PLAYLIST";
					if (!valid_source(parameter))
						valid = 0;
					else
						strcpy(e->source, parameter);
				}
				if (!e->enabled && a->active_id == e->id &&
				    a->phase)
					transition(a, "stop", 0, 0);
			}
		} else
			valid = 0;
		if (!valid) {
			*a = before;
			goto invalid;
		}
		if (!persist(a)) {
			*a = before;
			snprintf(out,
				 capacity,
				 "{\"error\":\"Alarm configuration could not "
				 "be saved\"}");
			return 1;
		}
		a->dirty = 1;
		plan(a, now);
		emit(&w, "{\"id\":%u}", e ? e->id : 0);
	}
	if (w.failed)
		goto invalid;
	return 1;
invalid:
	snprintf(out,
		 capacity,
		 "{\"error\":\"Invalid alarm parameters or alarm limit "
		 "reached\",\"count\":0,\"offset\":0,\"item_loop\":[]}");
	return 1;
}
