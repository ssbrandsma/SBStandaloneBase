/*
 * Installed Jive applet discovery and metadata reporting.
 *
 * Inspects applet directories for the management overview and applet APIs.
 * Works with http_server.c and complements repository.c, which manages the
 * installable catalog and uploaded packages rather than installed applets.
 */

#include "applets.h"
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APPLET_ROOT "/usr/share/jive/applets"
#define APPLET_MAX 128
#define APPLET_NAME_MAX 127

typedef struct {
	char *p;
	size_t left;
} Writer;

/**
 * @brief Handle add.
 * @param w Object referenced by w.
 * @param fmt Input text for fmt.
 */
static void add(Writer *w, const char *fmt, ...)
{
	va_list ap;
	int r;
	size_t used;
	if (w->left < 2)
		return;
	va_start(ap, fmt);
	r = vsnprintf(w->p, w->left, fmt, ap);
	va_end(ap);
	if (r < 0)
		return;
	used = (size_t)r;
	if (used >= w->left) {
		w->p += w->left - 1;
		w->left = 1;
	} else {
		w->p += used;
		w->left -= used;
	}
}

/**
 * @brief Handle json string.
 * @param w Object referenced by w.
 * @param s Input text for s.
 */
static void json_string(Writer *w, const char *s)
{
	add(w, "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '\"' || c == '\\')
			add(w, "\\%c", c);
		else if (c >= 32)
			add(w, "%c", c);
	}
	add(w, "\"");
}

/**
 * @brief Handle compare names.
 * @param a Object referenced by a.
 * @param b Object referenced by b.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int compare_names(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/**
 * @brief Handle applet id.
 * @param name Name of the item to process.
 * @param out Destination buffer for the result.
 * @param n Capacity or item count associated with the operation.
 */
static void applet_id(const char *name, char *out, size_t n)
{
	size_t used = 0;
	int separator = 0;
	while (*name && used + 1 < n) {
		unsigned char c = (unsigned char)*name++;
		if (isalnum(c)) {
			if (separator && used && used + 1 < n)
				out[used++] = '-';
			out[used++] = (char)tolower(c);
			separator = 0;
		} else
			separator = 1;
	}
	out[used] = 0;
}

/**
 * @brief Handle applets json.
 * @param out Destination buffer for the result.
 * @param n Capacity or item count associated with the operation.
 */
void applets_json(char *out, size_t n)
{
	char names[APPLET_MAX][APPLET_NAME_MAX + 1];
	size_t count = 0;
	DIR *dir = opendir(APPLET_ROOT);
	struct dirent *entry;
	Writer w = {out, n};
	if (dir) {
		while (count < APPLET_MAX && (entry = readdir(dir)) != NULL) {
			char path[512], meta[1024];
			struct stat st;
			size_t name_len = strlen(entry->d_name);
			if (entry->d_name[0] == '.' ||
			    name_len > APPLET_NAME_MAX)
				continue;
			snprintf(path,
				 sizeof path,
				 "%s/%s",
				 APPLET_ROOT,
				 entry->d_name);
			if (stat(path, &st) || !S_ISDIR(st.st_mode))
				continue;
			snprintf(meta,
				 sizeof meta,
				 "%s/%sMeta.lua",
				 path,
				 entry->d_name);
			if (stat(meta, &st) || !S_ISREG(st.st_mode))
				continue;
			memcpy(names[count], entry->d_name, name_len + 1);
			count++;
		}
		closedir(dir);
	}
	qsort(names, count, sizeof names[0], compare_names);
	add(&w, "[");
	for (size_t i = 0; i < count; i++) {
		char id[APPLET_NAME_MAX * 2 + 1];
		if (strcmp(names[i], "StandaloneRadio") == 0)
			snprintf(id, sizeof id, "standalone-radio");
		else
			applet_id(names[i], id, sizeof id);
		add(&w, "%s{\"id\":", i ? "," : "");
		json_string(&w, id);
		add(&w, ",\"name\":");
		json_string(&w, names[i]);
		add(&w,
		    ",\"installed\":true,\"supported\":%s}",
		    strcmp(names[i], "StandaloneRadio") == 0 ? "true"
							     : "false");
	}
	add(&w, "]");
}
