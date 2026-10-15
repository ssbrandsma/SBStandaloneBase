/*
 * User-defined radio station configuration and logo management.
 *
 * Validates and persists the station catalog exposed by http_server.c for
 * the browser's Standalone Radio tab. Supplies configuration to Standalone
 * Radio while leaving stream playback to that separate applet.
 */

#define _POSIX_C_SOURCE 200809L
#include "radio_stations.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/file.h>
#include <sys/statvfs.h>
#endif

#define MAX_STATIONS 250
#define DOC_LIMIT (256u * 1024u)
#define LOGO_LIMIT (1024u * 1024u)

typedef struct {
	char id[65], name[257], description[1025], stream[8193], logo_url[4097],
		logo_file[129], codec[8];
	int enabled, order;
} Station;

typedef struct {
	unsigned revision;
	int count;
	Station *stations;
} Config;

static time_t detect_at;
static int detect_value = -1;

/**
 * @brief Check whether extended is mounted.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int extended_mounted(void)
{
	FILE *f;
	char line[512], dev[128], mnt[256], type[32];
	if (getenv("SB_RADIO_CONFIG_ROOT"))
		return 1;
	f = fopen("/proc/mounts", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%127s %255s %31s", dev, mnt, type) == 3 &&
		    !strcmp(mnt, "/mnt/sbdata") && !strcmp(type, "sbubifs")) {
			fclose(f);
			return 1;
		}
	fclose(f);
	return 0;
}

/**
 * @brief Get the active configuration storage root.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *root(void)
{
	const char *e = getenv("SB_RADIO_CONFIG_ROOT");
	if (e && *e)
		return e;
	if (extended_mounted())
		return "/mnt/sbdata/config/radio";
	e = getenv("SB_RADIO_INTERNAL_ROOT");
	return e && *e ? e : "/mnt/storage/standalonebase/radio";
}

/**
 * @brief Build a path below the active configuration root.
 * @param out Destination buffer for the result.
 * @param n Capacity or item count associated with the operation.
 * @param leaf Input text for leaf.
 */
static void path(char *out, size_t n, const char *leaf)
{
	snprintf(out, n, "%s/%s", root(), leaf);
}

/**
 * @brief Send an HTTP response with the supplied body.
 * @param c Mongoose connection handling the request.
 * @param code Status or response code.
 * @param type Input text for type.
 * @param b Input text for b.
 */
static void
reply(struct mg_connection *c, int code, const char *type, const char *b)
{
	char h[260];
	snprintf(h,
		 sizeof h,
		 "Content-Type: %s\r\nCache-Control: "
		 "no-store\r\nX-Content-Type-Options: "
		 "nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: "
		 "close\r\n",
		 type);
	mg_http_reply(c, code, h, "%s", b ? b : "");
}

/**
 * @brief Send a JSON error response.
 * @param c Mongoose connection handling the request.
 * @param code Status or response code.
 * @param m Input text for m.
 */
static void fail(struct mg_connection *c, int code, const char *m)
{
	char b[900];
	size_t u = snprintf(b, sizeof b, "{\"error\":\"");
	for (; *m && u + 8 < sizeof b; m++) {
		unsigned char x = *m;
		if (x == '\"' || x == '\\')
			b[u++] = '\\';
		if (x >= 32)
			b[u++] = (char)x;
	}
	snprintf(b + u, sizeof b - u, "\"}");
	reply(c, code, "application/json", b);
}

/**
 * @brief Compare a bounded string with a null-terminated string.
 * @param a Input value for a.
 * @param b Input text for b.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int exact(struct mg_str a, const char *b)
{
	return a.len == strlen(b) && !memcmp(a.buf, b, a.len);
}

/**
 * @brief Check whether radio is api.
 * @param u Input value for u.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int radio_is_api(struct mg_str u)
{
	return u.len >= 10 && !memcmp(u.buf, "/api/radio", 10);
}

/**
 * @brief Check whether component is safe.
 * @param s Input text for s.
 * @param max Size or count for max.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int safe_component(const char *s, size_t max)
{
	size_t n;
	if (!s || (n = strlen(s)) < 1 || n > max || strstr(s, ".."))
		return 0;
	for (size_t i = 0; i < n; i++)
		if (!(isalnum((unsigned char)s[i]) || s[i] == '-' ||
		      s[i] == '_' || s[i] == '.'))
			return 0;
	return strchr(s, '/') == NULL && strchr(s, '\\') == NULL;
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
		if (*p < 0x80) {
			if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r')
				return 0;
			p++;
			continue;
		}
		unsigned n = (*p & 0xe0) == 0xc0   ? 2
			     : (*p & 0xf0) == 0xe0 ? 3
			     : (*p & 0xf8) == 0xf0 ? 4
						   : 0;
		if (!n || (n == 2 && *p < 0xc2) || (n == 4 && *p > 0xf4))
			return 0;
		unsigned cp = *p & ((1u << (8 - n - 1)) - 1);
		for (unsigned i = 1; i < n; i++) {
			if ((p[i] & 0xc0) != 0x80)
				return 0;
			cp = (cp << 6) | (p[i] & 63);
		}
		if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
		    (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
			return 0;
		p += n;
	}
	return 1;
}

/**
 * @brief Validate url.
 * @param s Input text for s.
 * @param optional Whether optional is enabled.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int valid_url(const char *s, int optional)
{
	if (!s || !*s)
		return optional;
	return (!strncmp(s, "http://", 7) && s[7]) ||
	       (!strncmp(s, "https://", 8) && s[8]);
}

/**
 * @brief Check whether storage is available.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int storage_available(void)
{
	struct stat st;
	const char *p = root(), *parent, *base;
	if (!stat(p, &st))
		return S_ISDIR(st.st_mode) && access(p, W_OK) == 0;
	if (errno != ENOENT)
		return 0;
	if (getenv("SB_RADIO_CONFIG_ROOT") || getenv("SB_RADIO_INTERNAL_ROOT"))
		return 1;
	if (extended_mounted()) {
		parent = "/mnt/sbdata/config";
		base = "/mnt/sbdata";
	} else {
		parent = "/mnt/storage/standalonebase";
		base = "/mnt/storage";
	}
	if (!stat(parent, &st))
		return S_ISDIR(st.st_mode) && access(parent, W_OK) == 0;
	return errno == ENOENT && access(base, W_OK) == 0;
}

/**
 * @brief Handle mkdir one.
 * @param p Input text for p.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int mkdir_one(const char *p)
{
	return mkdir(p, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

/**
 * @brief Handle prepare storage.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int prepare_storage(char *e, size_t n)
{
	char p[600];
	if (getenv("SB_RADIO_CONFIG_ROOT") ||
	    getenv("SB_RADIO_INTERNAL_ROOT")) {
		if (mkdir_one(root())) {
			snprintf(e,
				 n,
				 "Cannot create radio configuration directory");
			return -1;
		}
	} else if (extended_mounted()) {
		if (mkdir_one("/mnt/sbdata/config") || mkdir_one(root())) {
			snprintf(e,
				 n,
				 "Cannot create radio configuration directory");
			return -1;
		}
	} else if (mkdir_one("/mnt/storage/standalonebase") ||
		   mkdir_one(root())) {
		snprintf(e, n, "Internal storage is unavailable");
		return -1;
	}
	path(p, sizeof p, "logos");
	if (mkdir_one(p)) {
		snprintf(e, n, "Cannot create radio logo directory");
		return -1;
	}
	return 0;
}

/**
 * @brief Handle dir has.
 * @param base Input text for base.
 * @param name Name of the item to process.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int dir_has(const char *base, const char *name)
{
	char p[600];
	struct stat st;
	snprintf(p, sizeof p, "%s/%s", base, name);
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @brief Get radio applet status.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int radio_applet_status(void)
{
	time_t now = time(NULL);
	const char *override = getenv("SB_APPLET_ROOTS");
	if (detect_at && now - detect_at < 5)
		return detect_value;
	detect_at = now;
	detect_value = 0;
	if (override && *override) {
		char *b = strdup(override), *save = NULL, *p;
		if (!b) {
			detect_value = -1;
			return -1;
		}
		for (p = strtok_r(b, ";:", &save); p;
		     p = strtok_r(NULL, ";:", &save))
			if (dir_has(p, "StandaloneRadio") ||
			    dir_has(p, "SBStandalone")) {
				detect_value = 1;
				break;
			}
		free(b);
		return detect_value;
	}
	const char *roots[] = {"/usr/share/jive/applets",
			       "/mnt/storage/usr/share/jive/applets",
			       "/mnt/sbdata/jive/applets",
			       "/mnt/sbdata/applets",
			       NULL};
	int seen = 0;
	for (int i = 0; roots[i]; i++) {
		struct stat st;
		if (!stat(roots[i], &st) && S_ISDIR(st.st_mode)) {
			seen = 1;
			if (dir_has(roots[i], "StandaloneRadio") ||
			    dir_has(roots[i], "SBStandalone")) {
				detect_value = 1;
				break;
			}
		}
	}
	if (!seen)
		detect_value = -1;
	return detect_value;
}

/**
 * @brief Handle radio applet cache invalidate.
 */
void radio_applet_cache_invalidate(void)
{
	detect_at = 0;
}

/**
 * @brief Release cfg.
 * @param c Object referenced by c.
 */
static void free_cfg(Config *c)
{
	if (c) {
		free(c->stations);
		memset(c, 0, sizeof *c);
	}
}

/**
 * @brief Handle field.
 * @param o Input value for o.
 * @param k Input text for k.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @param required Numeric value for required.
 * @param e Mutable text buffer for e.
 * @param en Size or count for en.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int field(struct mg_str o,
		 const char *k,
		 char *out,
		 size_t cap,
		 int required,
		 char *e,
		 size_t en)
{
	char p[80], *v;
	snprintf(p, sizeof p, "$.%s", k);
	v = mg_json_get_str(o, p);
	if (!v) {
		if (required) {
			snprintf(e, en, "%s must be a string", k);
			return -1;
		}
		out[0] = 0;
		return 0;
	}
	if (strlen(v) >= cap || !valid_utf8(v)) {
		snprintf(e, en, "%s is too long or is not valid UTF-8", k);
		free(v);
		return -1;
	}
	strcpy(out, v);
	free(v);
	return 0;
}

/**
 * @brief Handle obj bounds.
 * @param json Input value for json.
 * @param key Input text for key.
 * @param begin Input text for begin.
 * @param end Input text for end.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int obj_bounds(struct mg_str json,
		      const char *key,
		      const char **begin,
		      const char **end)
{
	char needle[64];
	snprintf(needle, sizeof needle, "\"%s\"", key);
	const char *p = strstr(json.buf, needle);
	if (!p || p >= json.buf + json.len || (p = strchr(p, '[')) == NULL ||
	    p >= json.buf + json.len)
		return -1;
	int d = 0, str = 0, esc = 0;
	for (const char *q = p; q < json.buf + json.len; q++) {
		char x = *q;
		if (str) {
			if (esc)
				esc = 0;
			else if (x == '\\')
				esc = 1;
			else if (x == '\"')
				str = 0;
		} else if (x == '\"')
			str = 1;
		else if (x == '[')
			d++;
		else if (x == ']' && !--d) {
			*begin = p + 1;
			*end = q;
			return 0;
		}
	}
	return -1;
}

/**
 * @brief Parse config.
 * @param j Input value for j.
 * @param c Object referenced by c.
 * @param e Mutable text buffer for e.
 * @param en Size or count for en.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int parse_config(struct mg_str j, Config *c, char *e, size_t en)
{
	double schema, rev;
	const char *p, *end;
	memset(c, 0, sizeof *c);
	if (!j.buf || j.len > DOC_LIMIT ||
	    !mg_json_get_num(j, "$.schemaVersion", &schema) || schema != 1 ||
	    schema != (int)schema) {
		snprintf(e,
			 en,
			 "Unsupported or missing schemaVersion (expected 1)");
		return -1;
	}
	if (!mg_json_get_num(j, "$.revision", &rev) || rev < 0 ||
	    rev > 4294967295.0 || rev != (unsigned)rev) {
		snprintf(e, en, "revision must be a non-negative integer");
		return -1;
	}
	if (obj_bounds(j, "stations", &p, &end)) {
		snprintf(e, en, "stations must be an array");
		return -1;
	}
	c->stations = calloc(MAX_STATIONS, sizeof *c->stations);
	if (!c->stations) {
		snprintf(e, en, "Out of memory");
		return -1;
	}
	c->revision = (unsigned)rev;
	while (p < end) {
		while (p < end && (*p == ' ' || *p == '\r' || *p == '\n' ||
				   *p == '\t' || *p == ','))
			p++;
		if (p == end)
			break;
		if (*p != '{') {
			snprintf(e, en, "stations contains a non-object value");
			goto bad;
		}
		const char *s = p, *q = p;
		int d = 0, str = 0, esc = 0;
		do {
			char x = *q++;
			if (str) {
				if (esc)
					esc = 0;
				else if (x == '\\')
					esc = 1;
				else if (x == '\"')
					str = 0;
			} else if (x == '\"')
				str = 1;
			else if (x == '{')
				d++;
			else if (x == '}')
				d--;
		} while (q <= end && d);
		if (d || c->count >= MAX_STATIONS) {
			snprintf(e,
				 en,
				 "Invalid station object or more than %d "
				 "stations",
				 MAX_STATIONS);
			goto bad;
		}
		struct mg_str o = {(char *)s, (size_t)(q - s)};
		Station *x = &c->stations[c->count];
		bool enabled;
		double order;
		if (field(o, "id", x->id, sizeof x->id, 1, e, en) ||
		    field(o, "name", x->name, sizeof x->name, 1, e, en) ||
		    field(o,
			  "description",
			  x->description,
			  sizeof x->description,
			  0,
			  e,
			  en) ||
		    field(o,
			  "streamUrl",
			  x->stream,
			  sizeof x->stream,
			  1,
			  e,
			  en) ||
		    field(o,
			  "logoUrl",
			  x->logo_url,
			  sizeof x->logo_url,
			  0,
			  e,
			  en) ||
		    field(o,
			  "logoFile",
			  x->logo_file,
			  sizeof x->logo_file,
			  0,
			  e,
			  en) ||
		    field(o, "codec", x->codec, sizeof x->codec, 0, e, en))
			goto bad;
		if (!x->codec[0])
			strcpy(x->codec, "auto");
		if (!mg_json_get_bool(o, "$.enabled", &enabled) ||
		    !mg_json_get_num(o, "$.order", &order) ||
		    order != (int)order) {
			snprintf(e,
				 en,
				 "enabled must be boolean and order must be an "
				 "integer");
			goto bad;
		}
		x->enabled = enabled;
		x->order = (int)order;
		if (!safe_component(x->id, 64) || strchr(x->id, '.') ||
		    !x->name[0] || !valid_url(x->stream, 0) ||
		    !valid_url(x->logo_url, 1) ||
		    (x->logo_file[0] && !safe_component(x->logo_file, 128)) ||
		    (strcmp(x->codec, "auto") && strcmp(x->codec, "mp3") &&
		     strcmp(x->codec, "aac") && strcmp(x->codec, "ogg") &&
		     strcmp(x->codec, "flac"))) {
			snprintf(e,
				 en,
				 "Station %d has an invalid ID, URL, logo "
				 "filename, codec, or empty name",
				 c->count + 1);
			goto bad;
		}
		for (int i = 0; i < c->count; i++)
			if (!strcmp(c->stations[i].id, x->id) ||
			    c->stations[i].order == x->order) {
				snprintf(e,
					 en,
					 "Station IDs and order values must be "
					 "unique");
				goto bad;
			}
		c->count++;
		p = q;
	}
	return 0;
bad:
	free_cfg(c);
	return -1;
}

/**
 * @brief Read file.
 * @param z Object referenced by z.
 * @param missing Object referenced by missing.
 * @param e Mutable text buffer for e.
 * @param en Size or count for en.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static char *read_file(size_t *z, int *missing, char *e, size_t en)
{
	char p[600];
	FILE *f;
	long n;
	char *b;
	path(p, sizeof p, "stations.json");
	*missing = 0;
	f = fopen(p, "rb");
	if (!f) {
		if (errno == ENOENT)
			*missing = 1;
		else
			snprintf(e, en, "Cannot read stations.json");
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 2 ||
	    (size_t)n > DOC_LIMIT || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		snprintf(e, en, "stations.json is invalid or too large");
		return NULL;
	}
	b = malloc((size_t)n + 1);
	if (!b) {
		fclose(f);
		snprintf(e, en, "Out of memory");
		return NULL;
	}
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		snprintf(e, en, "Cannot read stations.json");
		return NULL;
	}
	fclose(f);
	b[n] = 0;
	*z = (size_t)n;
	return b;
}

/**
 * @brief Handle load.
 * @param c Object referenced by c.
 * @param e Mutable text buffer for e.
 * @param en Size or count for en.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int load(Config *c, char *e, size_t en)
{
	size_t z = 0;
	int missing = 0;
	char *b = read_file(&z, &missing, e, en);
	if (!b) {
		if (missing) {
			memset(c, 0, sizeof *c);
			c->stations = calloc(MAX_STATIONS, sizeof *c->stations);
			return c->stations ? 0 : -1;
		}
		return -1;
	}
	int rc = parse_config((struct mg_str){b, z}, c, e, en);
	free(b);
	return rc;
}

/**
 * @brief Handle js.
 * @param f Open stream to process.
 * @param s Input text for s.
 */
static void js(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++) {
		unsigned char x = *s;
		if (x == '"' || x == '\\') {
			fputc('\\', f);
			fputc(x, f);
		} else if (x == '\n')
			fputs("\\n", f);
		else if (x == '\r')
			fputs("\\r", f);
		else if (x == '\t')
			fputs("\\t", f);
		else if (x >= 32)
			fputc(x, f);
	}
	fputc('"', f);
}

/**
 * @brief Write cfg.
 * @param f Open stream to process.
 * @param c Object referenced by c.
 */
static void write_cfg(FILE *f, const Config *c)
{
	fprintf(f,
		"{\n  \"schemaVersion\": 1,\n  \"revision\": %u,\n  "
		"\"stations\": [",
		c->revision);
	for (int i = 0; i < c->count; i++) {
		Station *x = &c->stations[i];
		fputs(i ? ",\n    {" : "\n    {", f);
		fputs("\n      \"id\": ", f);
		js(f, x->id);
		fputs(",\n      \"name\": ", f);
		js(f, x->name);
		fputs(",\n      \"description\": ", f);
		js(f, x->description);
		fputs(",\n      \"streamUrl\": ", f);
		js(f, x->stream);
		fputs(",\n      \"logoUrl\": ", f);
		js(f, x->logo_url);
		fputs(",\n      \"logoFile\": ", f);
		js(f, x->logo_file);
		fputs(",\n      \"codec\": ", f);
		js(f, x->codec);
		fprintf(f,
			",\n      \"enabled\": %s,\n      \"order\": %d\n    }",
			x->enabled ? "true" : "false",
			x->order);
	}
	fputs(c->count ? "\n  ]\n}\n" : "\n  ]\n}\n", f);
}

/**
 * @brief Handle serialize.
 * @param c Object referenced by c.
 * @param z Object referenced by z.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static char *serialize(const Config *c, size_t *z)
{
	FILE *f = tmpfile();
	char *b;
	long n;
	if (!f)
		return NULL;
	write_cfg(f, c);
	if (fflush(f) || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 ||
	    (size_t)n > DOC_LIMIT || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return NULL;
	}
	b = malloc((size_t)n + 1);
	if (!b) {
		fclose(f);
		return NULL;
	}
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	fclose(f);
	b[n] = 0;
	*z = (size_t)n;
	return b;
}

/**
 * @brief Acquire the station-configuration lock.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int lock_cfg(void)
{
	char p[600];
	path(p, sizeof p, ".stations.lock");
	int fd = open(p, O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return -1;
#ifndef _WIN32
	if (flock(fd, LOCK_EX)) {
		close(fd);
		return -1;
	}
#endif
	return fd;
}

/**
 * @brief Release the station-configuration lock.
 * @param fd Open file descriptor.
 */
static void unlock_cfg(int fd)
{
#ifndef _WIN32
	flock(fd, LOCK_UN);
#endif
	close(fd);
}

/**
 * @brief Handle persist.
 * @param c Object referenced by c.
 * @param e Mutable text buffer for e.
 * @param en Size or count for en.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int persist(Config *c, char *e, size_t en)
{
	char final[600], tmp[650];
	FILE *f;
	size_t z;
	char *b;
	if (prepare_storage(e, en))
		return -1;
	b = serialize(c, &z);
	if (!b) {
		snprintf(e, en, "Configuration exceeds 256 KiB");
		return -1;
	}
	path(final, sizeof final, "stations.json");
	snprintf(tmp, sizeof tmp, "%s.tmp.%ld", final, (long)getpid());
	f = fopen(tmp, "wb");
	if (!f) {
		free(b);
		snprintf(e, en, "Cannot create temporary configuration");
		return -1;
	}
	chmod(tmp, 0600);
	int bad = fwrite(b, 1, z, f) != z || fflush(f);
#ifndef _WIN32
	bad = bad || fsync(fileno(f));
#endif
	bad = bad || fclose(f);
	free(b);
	if (bad || rename(tmp, final)) {
		unlink(tmp);
		snprintf(e, en, "Atomic configuration write failed");
		return -1;
	}
#ifndef _WIN32
	int d = open(root(), O_RDONLY);
	if (d >= 0) {
		fsync(d);
		close(d);
	}
#endif
	return 0;
}

/**
 * @brief Handle body station.
 * @param b Input value for b.
 * @param x Object referenced by x.
 * @param partial Numeric value for partial.
 * @param e Mutable text buffer for e.
 * @param en Size or count for en.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int
body_station(struct mg_str b, Station *x, int partial, char *e, size_t en)
{
	memset(x, 0, sizeof *x);
	if (field(b, "id", x->id, sizeof x->id, partial ? 0 : 0, e, en) ||
	    field(b, "name", x->name, sizeof x->name, 1, e, en) ||
	    field(b,
		  "description",
		  x->description,
		  sizeof x->description,
		  0,
		  e,
		  en) ||
	    field(b, "streamUrl", x->stream, sizeof x->stream, 1, e, en) ||
	    field(b, "logoUrl", x->logo_url, sizeof x->logo_url, 0, e, en) ||
	    field(b, "logoFile", x->logo_file, sizeof x->logo_file, 0, e, en) ||
	    field(b, "codec", x->codec, sizeof x->codec, 0, e, en))
		return -1;
	bool enabled;
	double order;
	if (!x->codec[0])
		strcpy(x->codec, "auto");
	if (!mg_json_get_bool(b, "$.enabled", &enabled))
		enabled = true;
	x->enabled = enabled;
	if (mg_json_get_num(b, "$.order", &order) && order == (int)order)
		x->order = (int)order;
	if (!x->name[0] || !valid_url(x->stream, 0) ||
	    !valid_url(x->logo_url, 1) ||
	    (x->logo_file[0] && !safe_component(x->logo_file, 128)) ||
	    (strcmp(x->codec, "auto") && strcmp(x->codec, "mp3") &&
	     strcmp(x->codec, "aac") && strcmp(x->codec, "ogg") &&
	     strcmp(x->codec, "flac"))) {
		snprintf(e,
			 en,
			 "Invalid station name, URL, logo filename, or codec");
		return -1;
	}
	return 0;
}

/**
 * @brief Handle expected.
 * @param b Input value for b.
 * @param ok Object referenced by ok.
 * @return Value produced by the operation.
 */
static unsigned expected(struct mg_str b, int *ok)
{
	double d;
	*ok = mg_json_get_num(b, "$.expectedRevision", &d) && d >= 0 &&
	      d == (unsigned)d;
	return (unsigned)d;
}

/**
 * @brief Handle slug.
 * @param name Name of the item to process.
 * @param out Destination buffer for the result.
 * @param n Capacity or item count associated with the operation.
 * @param c Object referenced by c.
 */
static void slug(const char *name, char *out, size_t n, const Config *c)
{
	size_t u = 0;
	int dash = 0;
	for (const unsigned char *p = (const unsigned char *)name;
	     *p && u + 1 < n;
	     p++) {
		if (isalnum(*p)) {
			out[u++] = (char)tolower(*p);
			dash = 0;
		} else if (!dash && u) {
			out[u++] = '-';
			dash = 1;
		}
	}
	while (u && out[u - 1] == '-')
		u--;
	if (!u)
		strcpy(out, "station");
	else
		out[u] = 0;
	char base[65];
	snprintf(base, sizeof base, "%s", out);
	for (unsigned x = 2;; x++) {
		int found = 0;
		for (int i = 0; i < c->count; i++)
			if (!strcmp(c->stations[i].id, out))
				found = 1;
		if (!found)
			return;
		snprintf(out, n, "%.50s-%u", base, x);
	}
}

/**
 * @brief Send cfg.
 * @param c Mongoose connection handling the request.
 * @param cfg Object referenced by cfg.
 */
static void send_cfg(struct mg_connection *c, const Config *cfg)
{
	size_t z;
	char *b = serialize(cfg, &z), h[180];
	int available = storage_available();
	if (!b) {
		fail(c, 500, "Cannot serialize configuration");
		return;
	}
	if (z >= 2 && b[z - 2] == '}') {
		char *next = realloc(b, z + 40);
		if (next) {
			b = next;
			snprintf(b + z - 2,
				 42,
				 ",\n  \"storageAvailable\": %s\n}\n",
				 available ? "true" : "false");
		}
	}
	snprintf(h,
		 sizeof h,
		 "Content-Type: application/json\r\nCache-Control: "
		 "no-store\r\nX-Storage-Available: %s\r\nConnection: close\r\n",
		 available ? "true" : "false");
	mg_http_reply(c, 200, h, "%s", b);
	free(b);
}

/**
 * @brief Find station.
 * @param c Object referenced by c.
 * @param id Identifier of the requested item.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int find_station(Config *c, const char *id)
{
	for (int i = 0; i < c->count; i++)
		if (!strcmp(c->stations[i].id, id))
			return i;
	return -1;
}

/**
 * @brief Save checked.
 * @param c Mongoose connection handling the request.
 * @param cfg Object referenced by cfg.
 * @param want Numeric value for want.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int save_checked(struct mg_connection *c, Config *cfg, unsigned want)
{
	char e[256];
	if (cfg->revision != want) {
		fail(c, 409, "Configuration changed; reload before saving");
		return -1;
	}
	cfg->revision++;
	if (persist(cfg, e, sizeof e)) {
		cfg->revision--;
		fail(c, 507, e);
		return -1;
	}
	send_cfg(c, cfg);
	return 0;
}

/**
 * @brief Handle csvq.
 * @param f Open stream to process.
 * @param s Input text for s.
 */
static void csvq(FILE *f, const char *s)
{
	int q = strpbrk(s, "\",\r\n") != NULL;
	if (q)
		fputc('"', f);
	for (; *s; s++) {
		if (*s == '"')
			fputc('"', f);
		fputc(*s, f);
	}
	if (q)
		fputc('"', f);
}

/**
 * @brief Handle export csv.
 * @param c Mongoose connection handling the request.
 * @param cfg Object referenced by cfg.
 */
static void export_csv(struct mg_connection *c, Config *cfg)
{
	FILE *f = tmpfile();
	char *b;
	long n;
	if (!f) {
		fail(c, 500, "Cannot create export");
		return;
	}
	fputs("id,name,description,streamUrl,logoUrl,logoFile,codec,enabled,"
	      "order\n",
	      f);
	for (int i = 0; i < cfg->count; i++) {
		Station *x = &cfg->stations[i];
		const char *v[] = {x->id,
				   x->name,
				   x->description,
				   x->stream,
				   x->logo_url,
				   x->logo_file,
				   x->codec};
		for (int k = 0; k < 7; k++) {
			if (k)
				fputc(',', f);
			csvq(f, v[k]);
		}
		fprintf(f, ",%s,%d\n", x->enabled ? "true" : "false", x->order);
	}
	fflush(f);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(n + 1);
	if (!b || fread(b, 1, n, f) != (size_t)n) {
		free(b);
		fclose(f);
		fail(c, 500, "Cannot create export");
		return;
	}
	fclose(f);
	b[n] = 0;
	mg_http_reply(c,
		      200,
		      "Content-Type: text/csv; "
		      "charset=utf-8\r\nContent-Disposition: attachment; "
		      "filename=stations.csv\r\nCache-Control: no-store\r\n",
		      "%s",
		      b);
	free(b);
}

/**
 * @brief Handle image type.
 * @param b Input text for b.
 * @param n Capacity or item count associated with the operation.
 * @param ext Input text for ext.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int image_type(const unsigned char *b, size_t n, const char **ext)
{
	if (n >= 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8) &&
	    !memcmp(b + 12, "IHDR", 4)) {
		unsigned w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) |
			     b[19],
			 h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) |
			     b[23];
		if (w && h && w <= 4096 && h <= 4096) {
			*ext = "png";
			return 1;
		}
	}
	if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) {
		*ext = "webp";
		return 1;
	}
	if (n >= 4 && b[0] == 0xff && b[1] == 0xd8 && b[n - 2] == 0xff &&
	    b[n - 1] == 0xd9) {
		*ext = "jpg";
		return 1;
	}
	return 0;
}

/**
 * @brief Handle upload logo.
 * @param c Mongoose connection handling the request.
 * @param m HTTP request message.
 */
static void upload_logo(struct mg_connection *c, struct mg_http_message *m)
{
	struct mg_http_part part;
	size_t off = 0;
	const char *ext = NULL;
	char e[200], dir[600], final[700], tmp[730], json[220];
	do {
		memset(&part, 0, sizeof part);
		off = mg_http_next_multipart(m->body, off, &part);
		if (part.name.len == 4 && !memcmp(part.name.buf, "logo", 4))
			break;
	} while (off);
	if (!part.body.buf || part.body.len < 4) {
		fail(c, 400, "Multipart field 'logo' is required");
		return;
	}
	if (part.body.len > LOGO_LIMIT) {
		fail(c, 413, "Logo exceeds 1 MiB limit");
		return;
	}
	if (!image_type((unsigned char *)part.body.buf, part.body.len, &ext)) {
		fail(c,
		     400,
		     "Logo content must be a bounded PNG, JPEG, or WebP image");
		return;
	}
	if (prepare_storage(e, sizeof e)) {
		fail(c, 507, e);
		return;
	}
	path(dir, sizeof dir, "logos");
	unsigned char rnd[8];
	FILE *r = fopen("/dev/urandom", "rb");
	if (!r || fread(rnd, 1, sizeof rnd, r) != sizeof rnd) {
		if (r)
			fclose(r);
		fail(c, 500, "Random source unavailable");
		return;
	}
	fclose(r);
	char id[17];
	for (int i = 0; i < 8; i++)
		sprintf(id + i * 2, "%02x", rnd[i]);
	snprintf(final, sizeof final, "%s/logo-%s.%s", dir, id, ext);
	snprintf(tmp, sizeof tmp, "%s.tmp.%ld", final, (long)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f || fwrite(part.body.buf, 1, part.body.len, f) != part.body.len ||
	    fflush(f)
#ifndef _WIN32
	    || fsync(fileno(f))
#endif
	    || fclose(f) || rename(tmp, final)) {
		if (f)
			fclose(f);
		unlink(tmp);
		fail(c, 507, "Cannot publish logo");
		return;
	}
	chmod(final, 0600);
	snprintf(json,
		 sizeof json,
		 "{\"logoFile\":\"logo-%s.%s\",\"url\":\"/api/radio/logos/"
		 "logo-%s.%s\"}",
		 id,
		 ext,
		 id,
		 ext);
	reply(c, 201, "application/json", json);
}

/**
 * @brief Serve logo.
 * @param c Mongoose connection handling the request.
 * @param m HTTP request message.
 * @param name Name of the item to process.
 */
static void
serve_logo(struct mg_connection *c, struct mg_http_message *m, const char *name)
{
	char p[700];
	if (!safe_component(name, 128)) {
		fail(c, 400, "Invalid logo filename");
		return;
	}
	snprintf(p, sizeof p, "%s/logos/%s", root(), name);
	if (access(p, R_OK)) {
		fail(c, 404, "Logo not found");
		return;
	}
	struct mg_http_serve_opts o = {0};
	o.extra_headers = "Cache-Control: private, "
			  "max-age=3600\r\nX-Content-Type-Options: nosniff\r\n";
	mg_http_serve_file(c, m, p, &o);
}

/**
 * @brief Handle import config.
 * @param c Mongoose connection handling the request.
 * @param m HTTP request message.
 */
static void import_config(struct mg_connection *c, struct mg_http_message *m)
{
	char e[300] = {0}, why[200];
	int ok = 0, len = 0, off;
	unsigned want = expected(m->body, &ok);
	char *mode = NULL;
	Config current = {0}, incoming = {0};
	if (!ok) {
		fail(c, 400, "expectedRevision is required");
		return;
	}
	off = mg_json_get(m->body, "$.config", &len);
	mode = mg_json_get_str(m->body, "$.mode");
	if (off < 0 || len < 2 ||
	    parse_config((struct mg_str){m->body.buf + off, (size_t)len},
			 &incoming,
			 e,
			 sizeof e)) {
		free(mode);
		fail(c,
		     400,
		     e[0] ? e
			  : "config must be a valid schemaVersion 1 document");
		return;
	}
	if (prepare_storage(why, sizeof why)) {
		free_cfg(&incoming);
		free(mode);
		fail(c, 507, why);
		return;
	}
	int lock = lock_cfg();
	if (lock < 0) {
		free_cfg(&incoming);
		free(mode);
		fail(c, 503, "Station configuration is busy");
		return;
	}
	if (load(&current, e, sizeof e)) {
		fail(c, 500, e);
		goto done;
	}
	if (current.revision != want) {
		fail(c, 409, "Configuration changed; reload before importing");
		goto done;
	}
	if (mode && !strcmp(mode, "merge")) {
		for (int i = 0; i < incoming.count; i++) {
			int at =
				find_station(&current, incoming.stations[i].id);
			if (at >= 0)
				current.stations[at] = incoming.stations[i];
			else if (current.count < MAX_STATIONS)
				current.stations[current.count++] =
					incoming.stations[i];
			else {
				fail(c,
				     409,
				     "Merged configuration exceeds station "
				     "limit");
				goto done;
			}
		}
	} else {
		memcpy(current.stations,
		       incoming.stations,
		       (size_t)incoming.count * sizeof *current.stations);
		current.count = incoming.count;
	}
	save_checked(c, &current, want);
done:
	free_cfg(&current);
	free_cfg(&incoming);
	free(mode);
	unlock_cfg(lock);
}

/**
 * @brief Handle radio handle.
 * @param c Mongoose connection handling the request.
 * @param m HTTP request message.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int radio_handle(struct mg_connection *c, struct mg_http_message *m)
{
	char e[300], id[160] = {0};
	Config cfg;
	int status = radio_applet_status();
	if (status != 1) {
		fail(c,
		     status < 0 ? 503 : 404,
		     status < 0 ? "Standalone Radio applet detection is "
				  "unavailable"
				: "Standalone Radio applet is not installed");
		return 1;
	}
	if (m->body.len > DOC_LIMIT && !(exact(m->uri, "/api/radio/logos"))) {
		fail(c, 413, "Request exceeds 256 KiB limit");
		return 1;
	}
	if (!memcmp(m->uri.buf, "/api/radio/logos/", 18)) {
		size_t n = m->uri.len - 18;
		if (n >= sizeof id)
			n = sizeof id - 1;
		memcpy(id, m->uri.buf + 18, n);
		id[n] = 0;
		if (exact(m->method, "GET")) {
			serve_logo(c, m, id);
			return 1;
		}
		if (exact(m->method, "DELETE")) {
			char p[700];
			if (!safe_component(id, 128)) {
				fail(c, 400, "Invalid logo filename");
				return 1;
			}
			snprintf(p, sizeof p, "%s/logos/%s", root(), id);
			if (unlink(p)) {
				fail(c,
				     errno == ENOENT ? 404 : 507,
				     errno == ENOENT ? "Logo not found"
						     : "Cannot delete logo");
				return 1;
			}
			reply(c, 200, "application/json", "{\"ok\":true}");
			return 1;
		}
	}
	if (exact(m->uri, "/api/radio/logos") && exact(m->method, "POST")) {
		upload_logo(c, m);
		return 1;
	}
	if (exact(m->uri, "/api/radio/import") && exact(m->method, "POST")) {
		import_config(c, m);
		return 1;
	}
	if (exact(m->uri, "/api/radio/stream-test")) {
		fail(c,
		     501,
		     "Stream testing is disabled: no safe asynchronous HTTP "
		     "client is available");
		return 1;
	}
	if (load(&cfg, e, sizeof e)) {
		fail(c, 500, e[0] ? e : "Cannot load station configuration");
		return 1;
	}
	if (exact(m->uri, "/api/radio/stations") && exact(m->method, "GET")) {
		send_cfg(c, &cfg);
		free_cfg(&cfg);
		return 1;
	}
	if (exact(m->uri, "/api/radio/export") && exact(m->method, "GET")) {
		char f[12];
		mg_http_get_var(&m->query, "format", f, sizeof f);
		if (!strcmp(f, "csv"))
			export_csv(c, &cfg);
		else
			send_cfg(c, &cfg);
		free_cfg(&cfg);
		return 1;
	}
	int ok = 0;
	unsigned want = expected(m->body, &ok);
	if (!ok) {
		free_cfg(&cfg);
		fail(c, 400, "expectedRevision is required");
		return 1;
	}
	char why[200];
	if (prepare_storage(why, sizeof why)) {
		free_cfg(&cfg);
		fail(c, 507, why);
		return 1;
	}
	int lock = lock_cfg();
	if (lock < 0) {
		free_cfg(&cfg);
		fail(c, 503, "Station configuration is busy");
		return 1;
	}
	free_cfg(&cfg);
	if (load(&cfg, e, sizeof e)) {
		unlock_cfg(lock);
		fail(c, 500, e);
		return 1;
	}
	if (exact(m->uri, "/api/radio/stations") && exact(m->method, "POST")) {
		if (cfg.count >= MAX_STATIONS) {
			fail(c, 409, "Station limit reached");
			goto done;
		}
		Station x;
		if (body_station(m->body, &x, 0, e, sizeof e)) {
			fail(c, 400, e);
			goto done;
		}
		slug(x.name, x.id, sizeof x.id, &cfg);
		if (!x.order)
			x.order =
				cfg.count
					? cfg.stations[cfg.count - 1].order + 10
					: 10;
		for (int i = 0; i < cfg.count; i++)
			if (cfg.stations[i].order == x.order) {
				x.order =
					cfg.stations[cfg.count - 1].order + 10;
				break;
			}
		cfg.stations[cfg.count++] = x;
		save_checked(c, &cfg, want);
		goto done;
	}
	if (!memcmp(m->uri.buf, "/api/radio/stations/", 20) &&
	    m->uri.len > 20) {
		size_t n = m->uri.len - 20;
		if (n >= sizeof id)
			n = sizeof id - 1;
		memcpy(id, m->uri.buf + 20, n);
		id[n] = 0;
		int at = find_station(&cfg, id);
		if (at < 0) {
			fail(c, 404, "Station not found");
			goto done;
		}
		if (exact(m->method, "DELETE")) {
			memmove(&cfg.stations[at],
				&cfg.stations[at + 1],
				(cfg.count - at - 1) * sizeof *cfg.stations);
			cfg.count--;
			save_checked(c, &cfg, want);
			goto done;
		}
		if (exact(m->method, "PUT")) {
			Station x;
			if (body_station(m->body, &x, 1, e, sizeof e)) {
				fail(c, 400, e);
				goto done;
			}
			strcpy(x.id, id);
			if (!x.order)
				x.order = cfg.stations[at].order;
			for (int i = 0; i < cfg.count; i++)
				if (i != at &&
				    cfg.stations[i].order == x.order) {
					fail(c, 400, "order must be unique");
					goto done;
				}
			cfg.stations[at] = x;
			save_checked(c, &cfg, want);
			goto done;
		}
	}
	if (exact(m->uri, "/api/radio/stations/reorder") &&
	    exact(m->method, "POST")) {
		int toklen = 0, off = mg_json_get(m->body, "$.ids", &toklen);
		if (off < 0) {
			fail(c, 400, "ids array is required");
			goto done;
		}
		int seen[MAX_STATIONS] = {0};
		for (int i = 0; i < cfg.count; i++) {
			char p[40], *v;
			snprintf(p, sizeof p, "$.ids[%d]", i);
			v = mg_json_get_str(m->body, p);
			if (!v) {
				fail(c,
				     400,
				     "ids must contain every station exactly "
				     "once");
				goto done;
			}
			int at = find_station(&cfg, v);
			free(v);
			if (at < 0 || seen[at]) {
				fail(c,
				     400,
				     "ids contains an unknown or duplicate "
				     "station");
				goto done;
			}
			seen[at] = 1;
			cfg.stations[at].order = (i + 1) * 10;
		}
		char p[40];
		snprintf(p, sizeof p, "$.ids[%d]", cfg.count);
		char *extra = mg_json_get_str(m->body, p);
		if (extra) {
			free(extra);
			fail(c, 400, "ids contains too many stations");
			goto done;
		}
		save_checked(c, &cfg, want);
		goto done;
	}
	fail(c, 405, "Unsupported radio management operation");
done:
	unlock_cfg(lock);
	free_cfg(&cfg);
	return 1;
}
