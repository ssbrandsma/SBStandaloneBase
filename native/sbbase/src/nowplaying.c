/*
 * Shared Now Playing state, ownership leases and playback command queues.
 *
 * Handles the internal playback API used by audio integrations and consumed
 * through sbbase's HTTP/CometD service. Keeps playback state independent of
 * browser management sessions and does not perform audio decoding itself.
 */

#define _POSIX_C_SOURCE 200809L
#include "nowplaying.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JTOKENS 160

typedef enum { J_UNDEF, J_OBJECT, J_ARRAY, J_STRING, J_PRIMITIVE } jtype;

typedef struct {
	jtype type;
	int start, end, size, parent;
} jtok;

typedef struct {
	unsigned pos, next;
	int super;
} jparser;

struct np_manager {
	np_status s;
	unsigned lease_seconds, command_limit, q_head, q_count;
	uint64_t generation_counter, command_counter;
	np_command queue[NP_COMMANDS_MAX];
};

static int
public_metadata_json(const np_status *s, char *out, size_t cap, int length);

/**
 * @brief Handle np monotonic ms.
 * @return Value produced by the operation.
 */
uint64_t np_monotonic_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/**
 * @brief Get reset status.
 * @param m Object referenced by m.
 */
static void reset_status(np_manager *m)
{
	uint64_t generation = m->s.generation;
	memset(&m->s, 0, sizeof m->s);
	m->s.generation = generation;
	m->s.state = NP_STOPPED;
	m->q_head = m->q_count = 0;
}

/**
 * @brief Handle np create.
 * @param lease_seconds Numeric value for lease seconds.
 * @param command_limit Numeric value for command limit.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
np_manager *np_create(unsigned lease_seconds, unsigned command_limit)
{
	np_manager *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	m->lease_seconds = lease_seconds ? lease_seconds : 120;
	m->command_limit = command_limit && command_limit <= NP_COMMANDS_MAX
				   ? command_limit
				   : NP_COMMANDS_MAX;
	return m;
}

/**
 * @brief Handle np destroy.
 * @param m Object referenced by m.
 */
void np_destroy(np_manager *m)
{
	free(m);
}

/**
 * @brief Handle np current.
 * @param m Object referenced by m.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
const np_status *np_current(np_manager *m)
{
	return m ? &m->s : NULL;
}

/**
 * @brief Handle np position.
 * @param m Object referenced by m.
 * @return Value produced by the operation.
 */
uint64_t np_position(np_manager *m)
{
	uint64_t p;
	if (!m)
		return 0;
	p = m->s.position_ms;
	if (m->s.state == NP_PLAYING && m->s.position_clock_ms)
		p += np_monotonic_ms() - m->s.position_clock_ms;
	if (m->s.duration_known && p > m->s.duration_ms)
		p = m->s.duration_ms;
	return p;
}

/**
 * @brief Handle np expire.
 * @param m Object referenced by m.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_expire(np_manager *m)
{
	if (!m || !m->s.active || !m->lease_seconds)
		return 0;
	if (np_monotonic_ms() - m->s.last_update_ms <=
	    (uint64_t)m->lease_seconds * 1000u)
		return 0;
	reset_status(m);
	return 1;
}

/**
 * @brief Handle alloc token.
 * @param out Destination buffer for the result.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int alloc_token(char out[NP_TOKEN_MAX])
{
	unsigned char b[24];
	size_t i;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
		if (fd >= 0)
			close(fd);
		return 0;
	}
	close(fd);
	for (i = 0; i < sizeof b; i++)
		snprintf(out + i * 2, 3, "%02x", b[i]);
	return 1;
}

/**
 * @brief Parse json.
 * @param js Input text for js.
 * @param len Length of the supplied data.
 * @param t Object referenced by t.
 * @param cap Numeric value for cap.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int parse_json(const char *js, size_t len, jtok *t, unsigned cap)
{
	jparser p = {0, 0, -1};
	unsigned i;
	for (; p.pos < len; p.pos++) {
		char c = js[p.pos];
		int n;
		if (isspace((unsigned char)c) || c == ':' || c == ',')
			continue;
		if (c == '{' || c == '[') {
			if (p.next >= cap)
				return -1;
			n = (int)p.next++;
			t[n] = (jtok){c == '{' ? J_OBJECT : J_ARRAY,
				      (int)p.pos,
				      -1,
				      0,
				      p.super};
			if (p.super >= 0)
				t[p.super].size++;
			p.super = n;
			continue;
		}
		if (c == '}' || c == ']') {
			jtype type = c == '}' ? J_OBJECT : J_ARRAY;
			for (n = (int)p.next - 1; n >= 0; n--)
				if (t[n].start >= 0 && t[n].end < 0) {
					if (t[n].type != type)
						return -1;
					t[n].end = (int)p.pos + 1;
					p.super = t[n].parent;
					break;
				}
			if (n < 0)
				return -1;
			continue;
		}
		if (c == '"') {
			unsigned start = ++p.pos;
			for (; p.pos < len; p.pos++) {
				c = js[p.pos];
				if (c == '"')
					break;
				if (c == '\\') {
					p.pos++;
					if (p.pos >= len)
						return -1;
				}
				if ((unsigned char)c < 32)
					return -1;
			}
			if (p.pos >= len || p.next >= cap)
				return -1;
			n = (int)p.next++;
			t[n] = (jtok){
				J_STRING, (int)start, (int)p.pos, 0, p.super};
			if (p.super >= 0)
				t[p.super].size++;
			continue;
		}
		{
			unsigned start = p.pos;
			while (p.pos < len &&
			       !isspace((unsigned char)js[p.pos]) &&
			       strchr(",}]", js[p.pos]) == NULL)
				p.pos++;
			if (start == p.pos || p.next >= cap)
				return -1;
			n = (int)p.next++;
			t[n] = (jtok){J_PRIMITIVE,
				      (int)start,
				      (int)p.pos,
				      0,
				      p.super};
			if (p.super >= 0)
				t[p.super].size++;
			p.pos--;
		}
	}
	for (i = 0; i < p.next; i++)
		if (t[i].end < 0)
			return -1;
	return p.next && t[0].type == J_OBJECT ? (int)p.next : -1;
}

/**
 * @brief Handle eq.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param s Input text for s.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int eq(const char *j, const jtok *t, const char *s)
{
	size_t n = strlen(s);
	return t->type == J_STRING && (size_t)(t->end - t->start) == n &&
	       !memcmp(j + t->start, s, n);
}

/**
 * @brief Handle subtree.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @param i Numeric value for i.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int subtree(jtok *t, int n, int i)
{
	int end = t[i].end;
	i++;
	while (i < n && t[i].start < end)
		i++;
	return i;
}

/**
 * @brief Handle member.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @param obj Numeric value for obj.
 * @param key Input text for key.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int member(const char *j, jtok *t, int n, int obj, const char *key)
{
	int i;
	if (obj < 0 || t[obj].type != J_OBJECT)
		return -1;
	for (i = obj + 1; i < n && t[i].start < t[obj].end;) {
		int v = i + 1;
		if (eq(j, &t[i], key) && v < n)
			return v;
		if (v >= n)
			return -1;
		i = subtree(t, n, v);
	}
	return -1;
}

/**
 * @brief Handle hex4.
 * @param s Input text for s.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int hex4(const char *s)
{
	int v = 0, i;
	for (i = 0; i < 4; i++) {
		int c = (unsigned char)s[i];
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'a' && c <= 'f')
			c = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			c = c - 'A' + 10;
		else
			return -1;
		v = v * 16 + c;
	}
	return v;
}

/**
 * @brief Handle put utf8.
 * @param o Mutable text buffer for o.
 * @param cap Size or count for cap.
 * @param n Capacity or item count associated with the operation.
 * @param v Numeric value for v.
 * @return Number of bytes or items produced by the operation.
 */
static size_t put_utf8(char *o, size_t cap, size_t n, unsigned v)
{
	if (v < 0x80) {
		if (n + 1 < cap)
			o[n++] = (char)v;
	} else if (v < 0x800) {
		if (n + 2 < cap) {
			o[n++] = (char)(0xc0 | (v >> 6));
			o[n++] = (char)(0x80 | (v & 63));
		}
	} else if (v < 0xd800 || v > 0xdfff) {
		if (n + 3 < cap) {
			o[n++] = (char)(0xe0 | (v >> 12));
			o[n++] = (char)(0x80 | ((v >> 6) & 63));
			o[n++] = (char)(0x80 | (v & 63));
		}
	}
	return n;
}

/**
 * @brief Handle string value.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param o Mutable text buffer for o.
 * @param cap Size or count for cap.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int string_value(const char *j, const jtok *t, char *o, size_t cap)
{
	int i;
	size_t n = 0;
	if (!t || t->type != J_STRING || !cap)
		return 0;
	for (i = t->start; i < t->end; i++) {
		unsigned char c = (unsigned char)j[i];
		if (c == '\\') {
			if (++i >= t->end)
				return 0;
			c = (unsigned char)j[i];
			if (c == 'u') {
				int v;
				if (i + 4 >= t->end ||
				    (v = hex4(j + i + 1)) < 0)
					return 0;
				if (!v || (v >= 0xd800 && v <= 0xdfff) ||
				    n + (v < 0x80    ? 1u
					 : v < 0x800 ? 2u
						     : 3u) >=
					    cap)
					return 0;
				n = put_utf8(o, cap, n, (unsigned)v);
				i += 4;
				continue;
			}
			if (c == 'n')
				c = '\n';
			else if (c == 'r')
				c = '\r';
			else if (c == 't')
				c = '\t';
			else if (c == 'b')
				c = '\b';
			else if (c == 'f')
				c = '\f';
			else if (c != '"' && c != '\\' && c != '/')
				return 0;
		}
		if (!c || n + 1 >= cap)
			return 0;
		o[n++] = (char)c;
	}
	o[n] = 0;
	return 1;
}

/**
 * @brief Handle boolean.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param d Object referenced by d.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int boolean(const char *j, const jtok *t, int *d)
{
	int n = t->end - t->start;
	if (t->type != J_PRIMITIVE)
		return 0;
	if (n == 4 && !memcmp(j + t->start, "true", 4)) {
		*d = 1;
		return 1;
	}
	if (n == 5 && !memcmp(j + t->start, "false", 5)) {
		*d = 0;
		return 1;
	}
	return 0;
}

/**
 * @brief Handle number.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param d Object referenced by d.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int number(const char *j, const jtok *t, uint64_t *d)
{
	char b[32], *e;
	unsigned long long v;
	int n = t->end - t->start;
	if (t->type != J_PRIMITIVE || n <= 0 || n >= (int)sizeof b)
		return 0;
	memcpy(b, j + t->start, n);
	b[n] = 0;
	errno = 0;
	v = strtoull(b, &e, 10);
	if (errno || *e)
		return 0;
	*d = (uint64_t)v;
	return 1;
}

/**
 * @brief Validate utf8.
 * @param s Input text for s.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int valid_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		unsigned n;
		if (*p < 0x80) {
			p++;
			continue;
		}
		if ((*p & 0xe0) == 0xc0)
			n = 1;
		else if ((*p & 0xf0) == 0xe0)
			n = 2;
		else if ((*p & 0xf8) == 0xf0)
			n = 3;
		else
			return 0;
		p++;
		while (n--)
			if ((*p++ & 0xc0) != 0x80)
				return 0;
	}
	return 1;
}

/**
 * @brief Handle esc.
 * @param o Mutable text buffer for o.
 * @param cap Size or count for cap.
 * @param s Input text for s.
 * @return Number of bytes or items produced by the operation.
 */
static size_t esc(char *o, size_t cap, const char *s)
{
	size_t n = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		const char *x = NULL;
		if (c == '"')
			x = "\\\"";
		else if (c == '\\')
			x = "\\\\";
		else if (c == '\n')
			x = "\\n";
		else if (c == '\r')
			x = "\\r";
		else if (c == '\t')
			x = "\\t";
		if (x) {
			while (*x && n + 1 < cap)
				o[n++] = *x++;
		} else if (c >= 32 && n + 1 < cap)
			o[n++] = (char)c;
	}
	if (cap)
		o[n < cap ? n : cap - 1] = 0;
	return n;
}

/**
 * @brief Handle state name.
 * @param s Input value for s.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *state_name(np_state s)
{
	return s == NP_PLAYING	   ? "playing"
	       : s == NP_PAUSED	   ? "paused"
	       : s == NP_BUFFERING ? "buffering"
	       : s == NP_ERROR	   ? "error"
				   : "stopped";
}

/**
 * @brief Handle lms mode.
 * @param s Input value for s.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *lms_mode(np_state s)
{
	return s == NP_PLAYING ? "play" : s == NP_PAUSED ? "pause" : "stop";
}

/**
 * @brief Handle np public json.
 * @param m Object referenced by m.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_public_json(np_manager *m, char *out, size_t cap)
{
	char so[192], sn[384], id[384], ti[512], ar[512], al[512], st[512],
		aw[768];
	const np_status *s = &m->s;
	int length;
	esc(so, sizeof so, s->source);
	esc(sn, sizeof sn, s->source_name);
	esc(id, sizeof id, s->track.id);
	esc(ti, sizeof ti, s->track.title);
	esc(ar, sizeof ar, s->track.artist);
	esc(al, sizeof al, s->track.album);
	esc(st, sizeof st, s->track.station);
	esc(aw, sizeof aw, s->track.artwork);
	length = snprintf(
		out,
		cap,
		"{\"api_version\":1,\"active\":%s,\"source\":\"%s\",\"name\":"
		"\"%s\",\"generation\":%llu,\"state\":\"%s\",\"live\":%s,"
		"\"position_ms\":%llu,\"duration_ms\":%llu,\"duration_known\":%"
		"s,\"track\":{\"id\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\","
		"\"album\":\"%s\",\"station\":\"%s\",\"artwork\":\"%s\"}}",
		s->active ? "true" : "false",
		so,
		sn,
		(unsigned long long)s->generation,
		state_name(s->state),
		s->live ? "true" : "false",
		(unsigned long long)np_position(m),
		(unsigned long long)s->duration_ms,
		s->duration_known ? "true" : "false",
		id,
		ti,
		ar,
		al,
		st,
		aw);
	return public_metadata_json(s, out, cap, length);
}

/**
 * @brief Handle np playerstatus json.
 * @param m Object referenced by m.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_playerstatus_json(np_manager *m, char *out, size_t cap)
{
	const np_status *s = &m->s;
	char ti[512], ar[512], al[512], st[512], aw[768], text[1600];
	const char *display_album;
	uint64_t pos = np_position(m);
	int length;
	if (!s->active || s->state == NP_STOPPED)
		return snprintf(out,
				cap,
				"{\"player_name\":\"Standalone\",\"player_"
				"connected\":1,\"power\":1,\"mode\":\"stop\","
				"\"time\":0,\"duration\":0,\"playlist_tracks\":"
				"0,\"count\":0,\"offset\":0,\"playlist_cur_"
				"index\":0,\"mixer "
				"volume\":50,\"digital_volume_control\":1,"
				"\"player_needs_upgrade\":0,\"player_is_"
				"upgrading\":0,\"seq_no\":0}");
	esc(ti, sizeof ti, s->track.title);
	esc(ar, sizeof ar, s->track.artist);
	esc(al, sizeof al, s->track.album);
	esc(st,
	    sizeof st,
	    s->station.name[0] ? s->station.name : s->track.station);
	esc(aw,
	    sizeof aw,
	    s->track.artwork[0] ? s->track.artwork : s->station.logo);
	display_album = s->live && !al[0] ? st : al;
	snprintf(text,
		 sizeof text,
		 "%s\\n%s%s%s",
		 ti,
		 ar,
		 ar[0] && display_album[0] ? " - " : "",
		 display_album);
	length = snprintf(
		out,
		cap,
		"{\"player_name\":\"Standalone\",\"player_connected\":1,"
		"\"power\":1,\"mode\":\"%s\",\"rate\":%d,\"time\":%.3f,"
		"\"duration\":%.3f,\"playlist_tracks\":1,\"count\":1,"
		"\"offset\":0,\"playlist_cur_"
		"index\":0,\"playlist_timestamp\":%llu,\"playlist "
		"repeat\":0,\"playlist "
		"shuffle\":0,\"remote\":%d,\"current_title\":\"%s\",\"mixer "
		"volume\":50,\"digital_volume_control\":1,\"use_volume_"
		"control\":1,\"player_needs_upgrade\":0,\"player_is_"
		"upgrading\":0,\"seq_no\":0,\"item_loop\":[{\"text\":\"%s\","
		"\"id\":-1,\"track\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
		"\"duration\":%.3f,\"icon\":\"%s\",\"params\":{\"track_id\":-1,"
		"\"playlist_index\":0}}],\"base\":{\"actions\":{\"more\":{"
		"\"cmd\":[\"contextmenu\"],\"itemsParams\":\"params\","
		"\"params\":{"
		"\"menu\":\"track\",\"context\":\"playlist\"},\"window\":{"
		"\"isContextMenu\":1}}}}}",
		lms_mode(s->state),
		s->state == NP_PLAYING ? 1 : 0,
		(double)pos / 1000.0,
		s->duration_known ? (double)s->duration_ms / 1000.0 : 0.0,
		(unsigned long long)s->playlist_revision,
		strcmp(s->source, "demo") ? 1 : 0,
		s->live && st[0] ? st : ti,
		text,
		ti,
		ar,
		display_album,
		s->duration_known ? (double)s->duration_ms / 1000.0 : 0.0,
		aw);
	if (length < 0 || (size_t)length >= cap) {
		snprintf(out,
			 cap,
			 "{\"count\":0,\"offset\":0,\"item_loop\":[],\"error\":"
			 "\"status_too_large\"}");
		return -1;
	}
	return length;
}

/**
 * @brief Handle getstr.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @param obj Numeric value for obj.
 * @param k Input text for k.
 * @param o Mutable text buffer for o.
 * @param cap Size or count for cap.
 * @param required Numeric value for required.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int getstr(const char *j,
		  jtok *t,
		  int n,
		  int obj,
		  const char *k,
		  char *o,
		  size_t cap,
		  int required)
{
	int x = member(j, t, n, obj, k);
	if (x < 0)
		return !required;
	if (!string_value(j, &t[x], o, cap) || !valid_utf8(o))
		return 0;
	return 1;
}

/**
 * @brief Handle getbool.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @param obj Numeric value for obj.
 * @param k Input text for k.
 * @param d Object referenced by d.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int
getbool(const char *j, jtok *t, int n, int obj, const char *k, int *d)
{
	int x = member(j, t, n, obj, k);
	return x < 0 ? 1 : boolean(j, &t[x], d);
}

/**
 * @brief Handle getnum.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @param obj Numeric value for obj.
 * @param k Input text for k.
 * @param d Object referenced by d.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int
getnum(const char *j, jtok *t, int n, int obj, const char *k, uint64_t *d)
{
	int x = member(j, t, n, obj, k);
	return x < 0 ? 1 : number(j, &t[x], d);
}

/**
 * @brief Check whether member.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @param obj Numeric value for obj.
 * @param k Input text for k.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int has_member(const char *j, jtok *t, int n, int obj, const char *k)
{
	return member(j, t, n, obj, k) >= 0;
}

/** Read a bounded optional unsigned metadata value; zero means unknown. */
static int metadata_uint(const char *j,
			 jtok *t,
			 int n,
			 int obj,
			 const char *key,
			 unsigned *out,
			 unsigned maximum)
{
	uint64_t value = *out;
	if (!getnum(j, t, n, obj, key, &value) || value > maximum)
		return 0;
	*out = (unsigned)value;
	return 1;
}

/** Parse an optional audio object atomically; explicit null clears it. */
static int audio_read(const char *j,
		      jtok *t,
		      int n,
		      int parent,
		      const char *key,
		      np_audio *audio)
{
	int obj = member(j, t, n, parent, key), x, value;
	char gain[48], *end;
	double db;
	if (obj < 0)
		return 1;
	/* A supplied audio object is a quality snapshot, not a partial merge.
	 */
	memset(audio, 0, sizeof *audio);
	if (t[obj].type == J_PRIMITIVE && t[obj].end - t[obj].start == 4 &&
	    !memcmp(j + t[obj].start, "null", 4)) {
		memset(audio, 0, sizeof *audio);
		return 1;
	}
	if (t[obj].type != J_OBJECT ||
	    !getstr(j,
		    t,
		    n,
		    obj,
		    "codec",
		    audio->codec,
		    sizeof audio->codec,
		    0) ||
	    !getstr(j,
		    t,
		    n,
		    obj,
		    "bitrate_mode",
		    audio->bitrate_mode,
		    sizeof audio->bitrate_mode,
		    0) ||
	    !metadata_uint(j,
			   t,
			   n,
			   obj,
			   "bitrate_kbps",
			   &audio->bitrate_kbps,
			   100000) ||
	    !metadata_uint(j,
			   t,
			   n,
			   obj,
			   "sample_rate_hz",
			   &audio->sample_rate_hz,
			   768000) ||
	    !metadata_uint(j, t, n, obj, "bit_depth", &audio->bit_depth, 64) ||
	    !metadata_uint(j, t, n, obj, "channels", &audio->channels, 32))
		return 0;
	if (audio->bitrate_mode[0] && strcmp(audio->bitrate_mode, "CBR") &&
	    strcmp(audio->bitrate_mode, "VBR"))
		return 0;
	x = member(j, t, n, obj, "lossless");
	if (x >= 0) {
		if (!boolean(j, &t[x], &value))
			return 0;
		audio->lossless = value ? 2 : 1;
	}
	x = member(j, t, n, obj, "replaygain_db");
	if (x >= 0) {
		int length = t[x].end - t[x].start;
		if (t[x].type != J_PRIMITIVE || length <= 0 ||
		    (size_t)length >= sizeof gain)
			return 0;
		memcpy(gain, j + t[x].start, (size_t)length);
		gain[length] = 0;
		db = strtod(gain, &end);
		if (*end || !isfinite(db) || db < -100 || db > 100)
			return 0;
		audio->replaygain_known = 1;
		audio->replaygain_millidb = (int)(db * 1000);
	}
	return 1;
}

/** Read station attributes separately from track attributes; each object is a
 * snapshot. */
static int station_read(const char *j, jtok *t, int n, np_station *station)
{
	int obj = member(j, t, n, 0, "station");
	if (obj < 0)
		return 1;
	memset(station, 0, sizeof *station);
	if (t[obj].type == J_PRIMITIVE && t[obj].end - t[obj].start == 4 &&
	    !memcmp(j + t[obj].start, "null", 4))
		return 1;
	if (t[obj].type != J_OBJECT)
		return 0;
#define STATION_STRING(key, field)                                             \
	if (!getstr(j,                                                         \
		    t,                                                         \
		    n,                                                         \
		    obj,                                                       \
		    key,                                                       \
		    station->field,                                            \
		    sizeof station->field,                                     \
		    0))                                                        \
	return 0
	STATION_STRING("id", id);
	STATION_STRING("name", name);
	STATION_STRING("description", description);
	STATION_STRING("homepage", homepage);
	STATION_STRING("stream_url", stream_url);
	STATION_STRING("logo", logo);
	STATION_STRING("country", country);
	STATION_STRING("language", language);
	STATION_STRING("genre", genre);
#undef STATION_STRING
	return audio_read(j, t, n, obj, "audio", &station->audio);
}

/**
 * @brief Handle track equal.
 * @param a Object referenced by a.
 * @param b Object referenced by b.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int track_equal(const np_track *a, const np_track *b)
{
	return !memcmp(a, b, sizeof *a);
}

/**
 * @brief Handle next revision.
 * @param value Value to process.
 * @return Value produced by the operation.
 */
static uint64_t next_revision(uint64_t value)
{
	value++;
	return value ? value : 1;
}

/**
 * @brief Handle session ok.
 * @param m Object referenced by m.
 * @param j Input text for j.
 * @param t Object referenced by t.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int session_ok(np_manager *m, const char *j, jtok *t, int n)
{
	char token[NP_TOKEN_MAX];
	return getstr(j, t, n, 0, "session_id", token, sizeof token, 1) &&
	       m->s.active && !strcmp(token, m->s.session_id);
}

/**
 * @brief Handle error json.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @param code Status or response code.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int error_json(char *out, size_t cap, const char *code)
{
	snprintf(out, cap, "{\"success\":false,\"error\":\"%s\"}", code);
	return 0;
}

/**
 * @brief Handle np queue command.
 * @param m Object referenced by m.
 * @param command Input text for command.
 * @param value Value to process.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_queue_command(np_manager *m, const char *command, int64_t value)
{
	unsigned tail;
	if (!m || !m->s.active || m->q_count >= m->command_limit)
		return 0;
	tail = (m->q_head + m->q_count) % NP_COMMANDS_MAX;
	m->queue[tail].id = ++m->command_counter;
	m->queue[tail].generation = m->s.generation;
	snprintf(m->queue[tail].command,
		 sizeof m->queue[tail].command,
		 "%s",
		 command);
	m->queue[tail].value = value;
	m->q_count++;
	return 1;
}

/**
 * @brief Handle commands json.
 * @param m Object referenced by m.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int commands_json(np_manager *m, char *out, size_t cap)
{
	size_t u = 0;
	unsigned i;
	u += (size_t)snprintf(
		out + u, cap - u, "{\"success\":true,\"commands\":[");
	for (i = 0; i < m->q_count && u < cap; i++) {
		np_command *q = &m->queue[(m->q_head + i) % NP_COMMANDS_MAX];
		u += (size_t)snprintf(
			out + u,
			cap - u,
			"%s{\"id\":%llu,\"command\":\"%s\",\"value\":%lld}",
			i ? "," : "",
			(unsigned long long)q->id,
			q->command,
			(long long)q->value);
	}
	snprintf(out + u, u < cap ? cap - u : 0, "]}");
	return 200;
}

/**
 * @brief Handle np api.
 * @param m Object referenced by m.
 * @param method Input text for method.
 * @param path Filesystem path to process.
 * @param body Input text for body.
 * @param len Length of the supplied data.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @param changed Object referenced by changed.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_api(np_manager *m,
	   const char *method,
	   const char *path,
	   const char *body,
	   size_t len,
	   char *out,
	   size_t cap,
	   int *changed)
{
	jtok t[JTOKENS];
	int n = -1;
	uint64_t now = np_monotonic_ms();
	if (changed)
		*changed = 0;
	if (!strcmp(method, "GET") && !strcmp(path, "/api/nowplaying")) {
		np_public_json(m, out, cap);
		return 200;
	}
	if (len > 16384) {
		error_json(out, cap, "payload_too_large");
		return 413;
	}
	if (strcmp(method, "POST") &&
	    strncmp(path, "/api/nowplaying/commands", 24)) {
		error_json(out, cap, "method_not_allowed");
		return 405;
	}
	if (len)
		n = parse_json(body, len, t, JTOKENS);
	if (len && n < 0) {
		error_json(out, cap, "invalid_json");
		return 400;
	}
	if (!strcmp(path, "/api/nowplaying/claim")) {
		int co;
		char source[NP_SOURCE_MAX], name[NP_NAME_MAX];
		if (strcmp(method, "POST") || n < 0 ||
		    !getstr(body,
			    t,
			    n,
			    0,
			    "source",
			    source,
			    sizeof source,
			    1) ||
		    !getstr(body, t, n, 0, "name", name, sizeof name, 1)) {
			error_json(out, cap, "invalid_claim");
			return 400;
		}
		reset_status(m);
		m->s.active = 1;
		m->s.generation = ++m->generation_counter;
		m->s.activated_ms = m->s.last_update_ms = now;
		snprintf(m->s.source, sizeof m->s.source, "%s", source);
		snprintf(m->s.source_name, sizeof m->s.source_name, "%s", name);
		if (!alloc_token(m->s.session_id)) {
			reset_status(m);
			error_json(out, cap, "entropy_unavailable");
			return 503;
		}
		co = member(body, t, n, 0, "capabilities");
		if (co >= 0 && t[co].type != J_OBJECT) {
			reset_status(m);
			error_json(out, cap, "invalid_capabilities");
			return 400;
		}
		if (co >= 0) {
			if (!getbool(body,
				     t,
				     n,
				     co,
				     "play",
				     &m->s.capabilities.play) ||
			    !getbool(body,
				     t,
				     n,
				     co,
				     "pause",
				     &m->s.capabilities.pause) ||
			    !getbool(body,
				     t,
				     n,
				     co,
				     "stop",
				     &m->s.capabilities.stop) ||
			    !getbool(body,
				     t,
				     n,
				     co,
				     "next",
				     &m->s.capabilities.next) ||
			    !getbool(body,
				     t,
				     n,
				     co,
				     "previous",
				     &m->s.capabilities.previous) ||
			    !getbool(body,
				     t,
				     n,
				     co,
				     "seek",
				     &m->s.capabilities.seek) ||
			    !getbool(body,
				     t,
				     n,
				     co,
				     "volume",
				     &m->s.capabilities.volume)) {
				reset_status(m);
				error_json(out, cap, "invalid_capabilities");
				return 400;
			}
		}
		snprintf(out,
			 cap,
			 "{\"success\":true,\"session_id\":\"%s\","
			 "\"generation\":%llu,\"active\":true}",
			 m->s.session_id,
			 (unsigned long long)m->s.generation);
		if (changed)
			*changed = NP_CHANGE_OWNER;
		return 201;
	}
	if (!strcmp(path, "/api/nowplaying/update")) {
		char state[20] = {0}, incoming_id[NP_ID_MAX] = {0};
		np_status candidate = m->s;
		int tr, flags = 0, live = candidate.live;
		uint64_t pos = candidate.position_ms,
			 dur = candidate.duration_ms;
		if (n < 0 || !session_ok(m, body, t, n)) {
			error_json(out, cap, "invalid_session");
			return 409;
		}
		if (!getstr(body, t, n, 0, "state", state, sizeof state, 0) ||
		    !getbool(body, t, n, 0, "live", &live) ||
		    !getnum(body, t, n, 0, "position_ms", &pos)) {
			error_json(out, cap, "invalid_update");
			return 400;
		}
		if (state[0]) {
			if (!strcmp(state, "playing"))
				candidate.state = NP_PLAYING;
			else if (!strcmp(state, "paused"))
				candidate.state = NP_PAUSED;
			else if (!strcmp(state, "stopped"))
				candidate.state = NP_STOPPED;
			else if (!strcmp(state, "buffering"))
				candidate.state = NP_BUFFERING;
			else if (!strcmp(state, "error"))
				candidate.state = NP_ERROR;
			else {
				error_json(out, cap, "invalid_state");
				return 400;
			}
		}
		if (!station_read(body, t, n, &candidate.station)) {
			error_json(out, cap, "invalid_station");
			return 400;
		}
		if ((m->s.station.id[0] && candidate.station.id[0] &&
		     strcmp(m->s.station.id, candidate.station.id)) ||
		    (!m->s.station.id[0] && m->s.station.name[0] &&
		     candidate.station.name[0] &&
		     strcmp(m->s.station.name, candidate.station.name))) {
			memset(&candidate.track, 0, sizeof candidate.track);
			dur = pos = 0;
			flags |= NP_CHANGE_IDENTITY;
		}
		tr = member(body, t, n, 0, "track");
		if (tr >= 0) {
			if (t[tr].type != J_OBJECT) {
				error_json(out, cap, "invalid_track");
				return 400;
			}
			if (has_member(body, t, n, tr, "id")) {
				if (!getstr(body,
					    t,
					    n,
					    tr,
					    "id",
					    incoming_id,
					    sizeof incoming_id,
					    1)) {
					error_json(out, cap, "invalid_track");
					return 400;
				}
				if (strcmp(incoming_id, candidate.track.id)) {
					memset(&candidate.track,
					       0,
					       sizeof candidate.track);
					memset(&candidate.station,
					       0,
					       sizeof candidate.station);
					dur = 0;
				}
				snprintf(candidate.track.id,
					 sizeof candidate.track.id,
					 "%s",
					 incoming_id);
			}
			if (!getstr(body,
				    t,
				    n,
				    tr,
				    "title",
				    candidate.track.title,
				    sizeof candidate.track.title,
				    0) ||
			    !getstr(body,
				    t,
				    n,
				    tr,
				    "artist",
				    candidate.track.artist,
				    sizeof candidate.track.artist,
				    0) ||
			    !getstr(body,
				    t,
				    n,
				    tr,
				    "album",
				    candidate.track.album,
				    sizeof candidate.track.album,
				    0) ||
			    !getstr(body,
				    t,
				    n,
				    tr,
				    "album_artist",
				    candidate.track.album_artist,
				    sizeof candidate.track.album_artist,
				    0) ||
			    !getstr(body,
				    t,
				    n,
				    tr,
				    "station",
				    candidate.track.station,
				    sizeof candidate.track.station,
				    0) ||
			    !getstr(body,
				    t,
				    n,
				    tr,
				    "content_type",
				    candidate.track.content_type,
				    sizeof candidate.track.content_type,
				    0) ||
			    !getstr(body,
				    t,
				    n,
				    tr,
				    "artwork_url",
				    candidate.track.artwork,
				    sizeof candidate.track.artwork,
				    0) ||
			    !getnum(body, t, n, tr, "duration_ms", &dur)) {
				error_json(out, cap, "invalid_track");
				return 400;
			}
			if (!audio_read(body,
					t,
					n,
					tr,
					"audio",
					&candidate.track.audio) ||
			    !audio_read(body,
					t,
					n,
					tr,
					"transport",
					&candidate.track.transport) ||
			    !metadata_uint(body,
					   t,
					   n,
					   tr,
					   "year",
					   &candidate.track.year,
					   9999) ||
			    !metadata_uint(body,
					   t,
					   n,
					   tr,
					   "track_number",
					   &candidate.track.track_number,
					   100000) ||
			    !metadata_uint(body,
					   t,
					   n,
					   tr,
					   "disc_number",
					   &candidate.track.disc_number,
					   10000)) {
				error_json(out, cap, "invalid_audio");
				return 400;
			}
#define TRACK_STRING(key, field)                                               \
	if (!getstr(body,                                                      \
		    t,                                                         \
		    n,                                                         \
		    tr,                                                        \
		    key,                                                       \
		    candidate.track.field,                                     \
		    sizeof candidate.track.field,                              \
		    0)) {                                                      \
		error_json(out, cap, "invalid_track");                         \
		return 400;                                                    \
	}
			TRACK_STRING("genre", genre);
			TRACK_STRING("comment", comment);
			TRACK_STRING("subtitle", subtitle);
			TRACK_STRING("url", url);
			TRACK_STRING("info_link", info_link);
#undef TRACK_STRING
		}
		if (!station_read(body, t, n, &candidate.station)) {
			error_json(out, cap, "invalid_station");
			return 400;
		}
		if (candidate.state == NP_STOPPED) {
			memset(&candidate.track, 0, sizeof candidate.track);
			memset(&candidate.station, 0, sizeof candidate.station);
			dur = pos = 0;
		}
		candidate.live = live;
		candidate.position_ms = pos;
		candidate.duration_ms = dur;
		candidate.duration_known = !live && dur > 0;
		candidate.last_update_ms = now;
		if (candidate.state != m->s.state ||
		    candidate.live != m->s.live ||
		    candidate.position_ms != m->s.position_ms ||
		    candidate.duration_ms != m->s.duration_ms ||
		    candidate.duration_known != m->s.duration_known)
			flags |= NP_CHANGE_PLAYBACK;
		if (!track_equal(&candidate.track, &m->s.track) ||
		    memcmp(&candidate.station,
			   &m->s.station,
			   sizeof candidate.station))
			flags |= NP_CHANGE_METADATA;
		if (strcmp(candidate.track.id, m->s.track.id))
			flags |= NP_CHANGE_IDENTITY;
		if (strcmp(candidate.track.artwork, m->s.track.artwork))
			flags |= NP_CHANGE_ARTWORK;
		if (flags & NP_CHANGE_METADATA)
			candidate.metadata_revision =
				next_revision(m->s.metadata_revision);
		if (flags & NP_CHANGE_PLAYBACK)
			candidate.playback_revision =
				next_revision(m->s.playback_revision);
		if (flags & NP_CHANGE_IDENTITY)
			candidate.playlist_revision =
				next_revision(m->s.playlist_revision);
		if (flags & NP_CHANGE_ARTWORK)
			candidate.artwork_revision =
				next_revision(m->s.artwork_revision);
		candidate.position_clock_ms = (flags & NP_CHANGE_PLAYBACK)
						      ? now
						      : m->s.position_clock_ms;
		m->s = candidate;
		snprintf(out,
			 cap,
			 "{\"success\":true,\"generation\":%llu}",
			 (unsigned long long)m->s.generation);
		if (changed)
			*changed = flags;
		return 200;
	}
	if (!strcmp(path, "/api/nowplaying/heartbeat")) {
		if (n < 0 || !session_ok(m, body, t, n)) {
			error_json(out, cap, "invalid_session");
			return 409;
		}
		m->s.last_update_ms = now;
		snprintf(out,
			 cap,
			 "{\"success\":true,\"generation\":%llu}",
			 (unsigned long long)m->s.generation);
		return 200;
	}
	if (!strcmp(path, "/api/nowplaying/release")) {
		if (n < 0 || !session_ok(m, body, t, n)) {
			error_json(out, cap, "invalid_session");
			return 409;
		}
		reset_status(m);
		snprintf(out, cap, "{\"success\":true}");
		if (changed)
			*changed = NP_CHANGE_OWNER;
		return 200;
	}
	if (!strncmp(path, "/api/nowplaying/commands?", 25)) {
		const char *q = strstr(path, "session_id=");
		if (strcmp(method, "GET") || !q || !m->s.active ||
		    strcmp(q + 11, m->s.session_id)) {
			error_json(out, cap, "invalid_session");
			return 409;
		}
		return commands_json(m, out, cap);
	}
	if (!strcmp(path, "/api/nowplaying/commands/ack")) {
		uint64_t id = 0;
		if (n < 0 || !session_ok(m, body, t, n) ||
		    !getnum(body, t, n, 0, "id", &id)) {
			error_json(out, cap, "invalid_ack");
			return 400;
		}
		while (m->q_count && m->queue[m->q_head].id <= id) {
			m->q_head = (m->q_head + 1) % NP_COMMANDS_MAX;
			m->q_count--;
		}
		snprintf(out, cap, "{\"success\":true}");
		return 200;
	}
	error_json(out, cap, "not_found");
	return 404;
}

/**
 * @brief Handle np demo.
 * @param m Object referenced by m.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_demo(np_manager *m)
{
	reset_status(m);
	m->s.active = 1;
	m->s.generation = ++m->generation_counter;
	m->s.activated_ms = m->s.last_update_ms = np_monotonic_ms();
	m->s.state = NP_PLAYING;
	m->s.capabilities.next = 1;
	snprintf(m->s.source, sizeof m->s.source, "demo");
	snprintf(m->s.source_name, sizeof m->s.source_name, "Now Playing Demo");
	snprintf(m->s.session_id, sizeof m->s.session_id, "internal-demo");
	snprintf(m->s.track.id, sizeof m->s.track.id, "900001");
	snprintf(m->s.track.title, sizeof m->s.track.title, "Sultans of Swing");
	snprintf(m->s.track.artist, sizeof m->s.track.artist, "Dire Straits");
	snprintf(m->s.track.album, sizeof m->s.track.album, "Dire Straits");
	m->s.duration_ms = 348000;
	m->s.duration_known = 1;
	m->s.position_clock_ms = np_monotonic_ms();
	m->s.playlist_revision = 1;
	return 1;
}

/**
 * @brief Handle np demo next.
 * @param m Object referenced by m.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int np_demo_next(np_manager *m)
{
	if (!m || strcmp(m->s.source, "demo"))
		return 0;
	if (!strcmp(m->s.track.id, "900001")) {
		snprintf(m->s.track.id, sizeof m->s.track.id, "900002");
		snprintf(m->s.track.title,
			 sizeof m->s.track.title,
			 "Money for Nothing");
		snprintf(m->s.track.album,
			 sizeof m->s.track.album,
			 "Brothers in Arms");
		m->s.duration_ms = 506000;
	} else
		np_demo(m);
	m->s.position_ms = 0;
	m->s.position_clock_ms = np_monotonic_ms();
	m->s.playlist_revision++;
	return 1;
}

/* LMS serializers share one bounded writer. A short buffer produces an error
 * object, never a truncated JSON document or an out-of-bounds append. */
typedef struct {
	char *out;
	size_t cap, used;
	int failed;
} np_writer;

typedef struct {
	const char *key, *label, *value;
	char number[48];
	int numeric;
} np_field;

/** Append formatted JSON to writer w; retain failure once capacity is exceeded.
 */
static void write_json(np_writer *w, const char *format, ...)
{
	va_list args;
	int length;
	if (w->failed)
		return;
	va_start(args, format);
	length = vsnprintf(w->out + w->used, w->cap - w->used, format, args);
	va_end(args);
	if (length < 0 || (size_t)length >= w->cap - w->used)
		w->failed = 1;
	else
		w->used += (size_t)length;
}

/** Append an already safe string span, leaving room for the terminating NUL. */
static void write_bytes(np_writer *w, const unsigned char *bytes, size_t length)
{
	if (w->failed)
		return;
	if (length >= w->cap - w->used) {
		w->failed = 1;
		return;
	}
	memcpy(w->out + w->used, bytes, length);
	w->used += length;
	w->out[w->used] = 0;
}

/** Escape string contents in spans, avoiding a printf call for every byte. */
static void write_string_content(np_writer *w, const char *value)
{
	const unsigned char *p = (const unsigned char *)value;
	while (*p && !w->failed) {
		const unsigned char *start = p;
		while (*p >= 32 && *p != '"' && *p != '\\')
			p++;
		write_bytes(w, start, (size_t)(p - start));
		if (!*p || w->failed)
			break;
		if (*p == '"' || *p == '\\')
			write_json(w, "\\%c", *p);
		else
			write_json(w, "\\u%04x", *p);
		p++;
	}
}

/** Write one JSON UTF-8 string, including its opening and closing quotes. */
static void write_string(np_writer *w, const char *value)
{
	write_json(w, "\"");
	write_string_content(w, value);
	write_json(w, "\"");
}

/** Add a known string field, skipping empty/unknown metadata. */
static void field_string(np_field *f,
			 int *count,
			 const char *key,
			 const char *label,
			 const char *value)
{
	if (!value || !value[0])
		return;
	f[*count].key = key;
	f[*count].label = label;
	f[*count].value = value;
	f[*count].numeric = 0;
	(*count)++;
}

/** Add a normalized numeric field; decimal precision is selected by its caller.
 */
static void field_number(np_field *f,
			 int *count,
			 const char *key,
			 const char *label,
			 double value,
			 int decimals)
{
	np_field *field = &f[*count];
	field->key = key;
	field->label = label;
	snprintf(field->number, sizeof field->number, "%.*f", decimals, value);
	field->value = field->number;
	field->numeric = 1;
	(*count)++;
}

/** Select source metadata using Queries.pm's standard track tag map. */
static int collect_fields(const np_status *s, const char *tags, np_field *f)
{
	const np_track *t = &s->track;
	const np_audio *a = &t->audio;
	const char *station = s->station.name[0] ? s->station.name : t->station;
	int count = 0;
	const char *p;
	field_number(f, &count, "id", NULL, -1, 0);
	field_string(f, &count, "title", "Title", t->title);
	for (p = tags; *p; p++) {
		if (memchr(tags, *p, (size_t)(p - tags)))
			continue;
		switch (*p) {
#define STRING_TAG(tag, key, label, value)                                     \
	case tag:                                                              \
		field_string(f, &count, key, label, value);                    \
		break
#define NUMBER_TAG(tag, key, label, value)                                     \
	case tag:                                                              \
		if (value)                                                     \
			field_number(f, &count, key, label, value, 0);         \
		break
			STRING_TAG('a', "artist", "Artist", t->artist);
		case 'A':
			if (!strchr(tags, 'a'))
				field_string(f,
					     &count,
					     "artist",
					     "Artist",
					     t->artist);
			break;
			STRING_TAG('l', "album", "Album", t->album);
			STRING_TAG('g',
				   "genre",
				   "Genre",
				   t->genre[0] ? t->genre : s->station.genre);
			STRING_TAG('k', "comment", "Comment", t->comment);
			STRING_TAG('z', "subtitle", "Subtitle", t->subtitle);
			STRING_TAG('o',
				   "type",
				   "Type",
				   a->codec[0] ? a->codec : t->content_type);
			STRING_TAG('u',
				   "url",
				   "URL",
				   t->url[0] ? t->url : s->station.stream_url);
			STRING_TAG('K',
				   "artwork_url",
				   NULL,
				   t->artwork[0] ? t->artwork
						 : s->station.logo);
			STRING_TAG('N', "remote_title", "Station", station);
			STRING_TAG('L',
				   "info_link",
				   "Information",
				   t->info_link[0] ? t->info_link
						   : s->station.homepage);
			NUMBER_TAG('y', "year", "Year", t->year);
			NUMBER_TAG('t', "tracknum", "Track", t->track_number);
			NUMBER_TAG('i', "disc", "Disc", t->disc_number);
			NUMBER_TAG('T',
				   "samplerate",
				   "Sample rate (Hz)",
				   a->sample_rate_hz);
			NUMBER_TAG('I',
				   "samplesize",
				   "Sample size (bits)",
				   a->bit_depth);
			NUMBER_TAG('H', "channels", "Channels", a->channels);
		case 'r':
			if (a->bitrate_kbps) {
				np_field *field = &f[count];
				snprintf(field->number,
					 sizeof field->number,
					 "%ukbps%s%s",
					 a->bitrate_kbps,
					 a->bitrate_mode[0] ? " " : "",
					 a->bitrate_mode);
				field_string(f,
					     &count,
					     "bitrate",
					     "Bitrate",
					     field->number);
			}
			break;
		case 'Q':
			if (a->lossless)
				field_number(f,
					     &count,
					     "lossless",
					     "Lossless",
					     a->lossless == 2,
					     0);
			break;
		case 'Y':
			if (a->replaygain_known)
				field_number(f,
					     &count,
					     "replay_gain",
					     "ReplayGain (dB)",
					     a->replaygain_millidb / 1000.0,
					     3);
			break;
		case 'x':
			field_number(f, &count, "remote", NULL, 1, 0);
			break;
		case 'd':
			if (s->duration_known)
				field_number(f,
					     &count,
					     "duration",
					     "Duration (seconds)",
					     s->duration_ms / 1000.0,
					     3);
			break;
#undef STRING_TAG
#undef NUMBER_TAG
		default:
			break;
		}
	}
	return count;
}

/** Emit one track/songinfo metadata field, preserving JSON numeric value types.
 */
static void write_field(np_writer *w, const np_field *field)
{
	write_string(w, field->key);
	write_json(w, ":");
	if (field->numeric)
		write_json(w, "%s", field->value);
	else
		write_string(w, field->value);
}

/** Append optional normalized audio to the internal status response. */
static void write_audio(np_writer *w, const char *key, const np_audio *a)
{
	int separator = 0;
	if (!a->codec[0] && !a->bitrate_kbps && !a->sample_rate_hz &&
	    !a->bit_depth && !a->channels && !a->lossless &&
	    !a->replaygain_known)
		return;
	write_json(w, ",\"%s\":{", key);
#define AUDIO_STRING(name, value)                                              \
	if (value[0]) {                                                        \
		write_json(w, "%s\"%s\":", separator++ ? "," : "", name);      \
		write_string(w, value);                                        \
	}
#define AUDIO_NUMBER(name, value)                                              \
	if (value)                                                             \
	write_json(w, "%s\"%s\":%u", separator++ ? "," : "", name, value)
	AUDIO_STRING("codec", a->codec);
	AUDIO_STRING("bitrate_mode", a->bitrate_mode);
	AUDIO_NUMBER("bitrate_kbps", a->bitrate_kbps);
	AUDIO_NUMBER("sample_rate_hz", a->sample_rate_hz);
	AUDIO_NUMBER("bit_depth", a->bit_depth);
	AUDIO_NUMBER("channels", a->channels);
#undef AUDIO_STRING
#undef AUDIO_NUMBER
	if (a->lossless)
		write_json(w,
			   "%s\"lossless\":%s",
			   separator++ ? "," : "",
			   a->lossless == 2 ? "true" : "false");
	if (a->replaygain_known)
		write_json(w,
			   "%s\"replaygain_db\":%.3f",
			   separator ? "," : "",
			   a->replaygain_millidb / 1000.0);
	write_json(w, "}");
}

/** Extend the existing internal status without exposing owner credentials.
 * s is the current snapshot; out/cap contain the legacy response of length.
 * Returns its completed length, or -1 with a JSON error on capacity failure.
 */
static int
public_metadata_json(const np_status *s, char *out, size_t cap, int length)
{
	np_writer w = {out, cap, 0, 0};
	if (length < 2 || (size_t)length >= cap)
		goto overflow;
	w.used = (size_t)length - 2;
	write_audio(&w, "audio", &s->track.audio);
	write_audio(&w, "transport", &s->track.transport);
#define PUBLIC_TRACK(key, field)                                               \
	if (s->track.field[0]) {                                               \
		write_json(&w, ",\"%s\":", key);                               \
		write_string(&w, s->track.field);                              \
	}
	PUBLIC_TRACK("genre", genre);
	PUBLIC_TRACK("comment", comment);
	PUBLIC_TRACK("subtitle", subtitle);
	PUBLIC_TRACK("url", url);
	PUBLIC_TRACK("info_link", info_link);
#undef PUBLIC_TRACK
	if (s->track.year)
		write_json(&w, ",\"year\":%u", s->track.year);
	if (s->track.track_number)
		write_json(&w, ",\"track_number\":%u", s->track.track_number);
	if (s->track.disc_number)
		write_json(&w, ",\"disc_number\":%u", s->track.disc_number);
	write_json(&w, "},\"station\":{\"name\":");
	write_string(&w, s->station.name);
#define PUBLIC_STATION(key, field)                                             \
	if (s->station.field[0]) {                                             \
		write_json(&w, ",\"%s\":", key);                               \
		write_string(&w, s->station.field);                            \
	}
	PUBLIC_STATION("id", id);
	PUBLIC_STATION("description", description);
	PUBLIC_STATION("homepage", homepage);
	PUBLIC_STATION("country", country);
	PUBLIC_STATION("language", language);
	PUBLIC_STATION("genre", genre);
	PUBLIC_STATION("logo", logo);
	PUBLIC_STATION("stream_url", stream_url);
#undef PUBLIC_STATION
	write_audio(&w, "audio", &s->station.audio);
	write_json(&w, "}}");
	if (!w.failed)
		return (int)w.used;
overflow:
	snprintf(out, cap, "{\"error\":\"metadata_response_too_large\"}");
	return -1;
}

/** Parse current-track LMS queries from a Comet message using bounded JSON
 * tokens. m owns the snapshot; json/len delimit one message; out/cap receive
 * valid JSON. Returns zero for other commands, one for handled queries
 * (including errors).
 */
int np_lms_query_json(
	np_manager *m, const char *json, size_t len, char *out, size_t cap)
{
	jtok tokens[JTOKENS];
	np_field fields[40];
	np_writer w = {out, cap, 0, 0};
	char command[32] = "", tags[96] = "", arg[NP_ART_MAX];
	const np_status *s = &m->s;
	int n, data, request, args, i, ordinal = 0, menu = 0, selected = 1;
	int index = 0, quantity = 100, count, last, songinfo, info;
	int has_tags = 0, selector_seen = 0;
	if (len > 16384 || !cap)
		return 0;
	n = parse_json(json, len, tokens, JTOKENS);
	if (n < 0)
		return 0;
	data = member(json, tokens, n, 0, "data");
	request = member(json, tokens, n, data, "request");
	if (request < 0 || tokens[request].type != J_ARRAY)
		return 0;
	i = request + 1;
	if (i >= n)
		return 0;
	args = subtree(tokens, n, i);
	if (args >= n || tokens[args].type != J_ARRAY || args + 1 >= n ||
	    !string_value(json, &tokens[args + 1], command, sizeof command))
		return 0;
	songinfo = !strcmp(command, "songinfo");
	info = !strcmp(command, "contextmenu") || !strcmp(command, "trackinfo");
	if (strcmp(command, "status") && !songinfo && !info)
		return 0;
	for (i = args + 2; i < n && tokens[i].start < tokens[args].end; i++) {
		if (tokens[i].type == J_STRING) {
			if (!string_value(json, &tokens[i], arg, sizeof arg))
				goto invalid;
		} else if (tokens[i].type == J_PRIMITIVE &&
			   tokens[i].end - tokens[i].start < (int)sizeof arg) {
			memcpy(arg,
			       json + tokens[i].start,
			       (size_t)(tokens[i].end - tokens[i].start));
			arg[tokens[i].end - tokens[i].start] = 0;
		} else
			goto invalid;
		if (!strncmp(arg, "tags:", 5)) {
			if (strlen(arg + 5) >= sizeof tags)
				goto invalid;
			strcpy(tags, arg + 5);
			has_tags = 1;
		} else if (!strncmp(arg, "menu:", 5))
			menu = 1;
		else if (!strncmp(arg, "track_id:", 9)) {
			selector_seen = 1;
			selected = !strcmp(arg + 9, "-1") ||
				   !strcmp(arg + 9, s->track.id);
		} else if (!strncmp(arg, "url:", 4)) {
			selector_seen = 1;
			selected = !strcmp(arg + 4, s->track.url) ||
				   !strcmp(arg + 4, s->station.stream_url);
		} else if (!strncmp(arg, "playlist_index:", 15))
			selected = !strcmp(arg + 15, "0");
		else if (!strchr(arg, ':') && strcmp(arg, "items")) {
			char *end;
			long value =
				!strcmp(arg, "-") ? 0 : strtol(arg, &end, 10);
			if (strcmp(arg, "-") &&
			    (*end || value < 0 || value > 1000000))
				goto invalid;
			if (!ordinal++)
				index = (int)value;
			else
				quantity = (int)value;
		}
	}
	if (menu && !info && !songinfo) {
		if (np_playerstatus_json(m, out, cap) < 0)
			goto invalid;
		return 1;
	}
	if (!has_tags)
		strcpy(tags,
		       info	  ? "aAlgrytiodrTIHQYxuKNLzk"
		       : songinfo ? "abcdefghijJklmnopqrstvwxyzBCDEFHIJKLMNOQRT"
				    "UVWXY"
				  : "gald");
	if (songinfo && !selector_seen)
		goto invalid;
	count = s->active && s->state != NP_STOPPED && selected
			? collect_fields(s, tags, fields)
			: 0;
	if (info) {
		int total = 0, item = 0;
		/* These are standard XMLBrowser text rows, not invented track
		 * tags. */
		if (count) {
			field_string(fields,
				     &count,
				     "description",
				     "Station description",
				     s->station.description);
			field_string(fields,
				     &count,
				     "country",
				     "Country",
				     s->station.country);
			field_string(fields,
				     &count,
				     "language",
				     "Language",
				     s->station.language);
			field_string(fields,
				     &count,
				     "station_codec",
				     "Station format (declared)",
				     s->station.audio.codec);
			if (s->station.audio.bitrate_kbps)
				field_number(fields,
					     &count,
					     "station_bitrate",
					     "Station bitrate (declared, kbps)",
					     s->station.audio.bitrate_kbps,
					     0);
		}
		for (i = 0; i < count; i++)
			if (fields[i].label)
				total++;
		write_json(&w,
			   "{\"count\":%d,\"offset\":%d,\"window\":{\"text\":"
			   "\"More Info\"},\"item_loop\":[",
			   total,
			   index < total ? index : total);
		for (i = 0; i < count; i++) {
			if (!fields[i].label)
				continue;
			if (item >= index && item - index < quantity) {
				if (item > index)
					write_json(&w, ",");
				write_json(&w,
					   "{\"type\":\"text\",\"style\":"
					   "\"itemNoAction\",\"action\":"
					   "\"none\",\"text\":");
				write_json(&w, "\"");
				write_string_content(&w, fields[i].label);
				write_json(&w, ": ");
				write_string_content(&w, fields[i].value);
				write_json(&w, "\"}");
			}
			item++;
		}
		write_json(&w, "]}");
	} else if (songinfo) {
		if (index > count)
			index = count;
		last = quantity > count - index ? count : index + quantity;
		write_json(&w, "{\"songinfo_loop\":[");
		for (i = index; i < last; i++) {
			write_json(&w, "%s{", i > index ? "," : "");
			write_field(&w, &fields[i]);
			write_json(&w, "}");
		}
		write_json(&w, "]}");
	} else {
		write_json(&w,
			   "{\"player_name\":\"Standalone\",\"player_"
			   "connected\":1,\"power\":1,"
			   "\"mode\":\"%s\",\"time\":%.3f,\"duration\":%.3f,"
			   "\"playlist_tracks\":%d,"
			   "\"playlist_cur_index\":0,\"count\":%d,\"offset\":0,"
			   "\"remote\":1,\"current_title\":",
			   lms_mode(s->state),
			   np_position(m) / 1000.0,
			   s->duration_known ? s->duration_ms / 1000.0 : 0,
			   count ? 1 : 0,
			   count ? 1 : 0);
		write_string(&w,
			     s->station.name[0]		      ? s->station.name
			     : s->live && s->track.station[0] ? s->track.station
							      : s->track.title);
		{
			const np_audio *a = &s->track.transport;
			if (strchr(tags, 'o') && a->codec[0]) {
				write_json(&w, ",\"type\":");
				write_string(&w, a->codec);
			}
			if (strchr(tags, 'b') && a->bitrate_kbps)
				write_json(&w,
					   ",\"bitrate\":\"%ukbps\"",
					   a->bitrate_kbps);
			if (strchr(tags, 'T') && a->sample_rate_hz)
				write_json(&w,
					   ",\"samplerate\":%u",
					   a->sample_rate_hz);
			if (strchr(tags, 'I') && a->bit_depth)
				write_json(
					&w, ",\"samplesize\":%u", a->bit_depth);
		}
		write_json(&w, ",\"playlist_loop\":[");
		if (count && index == 0 && quantity) {
			write_json(&w, "{\"playlist index\":0");
			for (i = 0; i < count; i++) {
				write_json(&w, ",");
				write_field(&w, &fields[i]);
			}
			write_json(&w, "}");
		}
		write_json(&w, "]}");
	}
	if (!w.failed)
		return 1;
invalid:
	snprintf(out,
		 cap,
		 "{\"error\":\"invalid_or_oversized_metadata_query\"}");
	return 1;
}
