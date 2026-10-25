/*
 * Installable applet catalog and local ZIP package management.
 *
 * Validates catalog edits and uploads, selects storage and publishes package
 * downloads through http_server.c. The resulting catalog is also read by
 * sbbase to supply Jive's LMS-compatible Applet Installer responses.
 * catalog_sync.c uses its transactional merge to import newer GitHub release
 * entries without replacing device settings, custom applets or local ZIPs.
 */

#define _POSIX_C_SOURCE 200809L
#include "repository.h"
#include "mongoose.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/file.h>
#include <sys/statvfs.h>
#include <signal.h>
#endif

#define CONFIG_LIMIT (256u * 1024u)
#define LOCAL_PREFIX "http://127.0.0.1:80/applets/packages/"
#define INTERNAL_RESERVE (1024u * 1024u)

/**
 * @brief Handle err.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @param s Input text for s.
 */
static void err(char *out, size_t cap, const char *s)
{
	if (out && cap)
		snprintf(out, cap, "%s", s);
}

/**
 * @brief Handle config dir.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *config_dir(void)
{
	const char *p = getenv("SBWEBSERVER_CONFIG_DIR");
	return p && *p ? p : "/mnt/storage/standalonebase";
}

/**
 * @brief Handle repo config path.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
const char *repo_config_path(void)
{
	static char p[512];
	const char *e = getenv("SB_CATALOG_CONFIG");
	if (e && *e)
		return e;
	snprintf(p, sizeof p, "%s/config.json", config_dir());
	return p;
}

static int mounted_sbdata(void);

/**
 * @brief Check whether dir.
 * @param p Input text for p.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int is_dir(const char *p)
{
	struct stat s;
	return !stat(p, &s) && S_ISDIR(s.st_mode);
}

/**
 * @brief Handle repo root.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
const char *repo_root(void)
{
	const char *e = getenv("SB_REPOSITORY_ROOT");
	if (e && *e)
		return e;
	e = getenv("SB_REPOSITORY_INTERNAL_ROOT");
	if (e && *e)
		return e;
	if (is_dir("/mnt/sbdata/applet-repository"))
		return "/mnt/sbdata/applet-repository";
	if (is_dir("/mnt/storage/standalonebase/applet-repository"))
		return "/mnt/storage/standalonebase/applet-repository";
	return mounted_sbdata()
		       ? "/mnt/sbdata/applet-repository"
		       : "/mnt/storage/standalonebase/applet-repository";
}

/**
 * @brief Handle mkdir one.
 * @param p Input text for p.
 * @param m Input value for m.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int mkdir_one(const char *p, mode_t m)
{
	return mkdir(p, m) == 0 || errno == EEXIST ? 0 : -1;
}

/**
 * @brief Check whether name is safe.
 * @param s Input text for s.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int safe_name(const char *s)
{
	size_t n = 0;
	if (!s || !isalpha((unsigned char)*s))
		return 0;
	for (; *s; s++, n++)
		if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-') ||
		    n >= 63)
			return 0;
	return n > 0;
}

/**
 * @brief Check whether version is safe.
 * @param s Input text for s.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int safe_version(const char *s)
{
	size_t n = 0;
	if (!s || !*s)
		return 0;
	for (; *s; s++, n++)
		if (!(isalnum((unsigned char)*s) || strchr("._+-", *s)) ||
		    n >= 31)
			return 0;
	return 1;
}

/**
 * @brief Check whether sha is valid.
 * @param s Input text for s.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int sha_valid(const char *s)
{
	int i;
	if (!s || strlen(s) != 40)
		return 0;
	for (i = 0; i < 40; i++)
		if (!isxdigit((unsigned char)s[i]))
			return 0;
	return 1;
}

/**
 * @brief Check whether repo is local url.
 * @param u Input text for u.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_is_local_url(const char *u)
{
	return u && strncmp(u, LOCAL_PREFIX, strlen(LOCAL_PREFIX)) == 0;
}

/**
 * @brief Handle mounted sbdata.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int mounted_sbdata(void)
{
	const char *override = getenv("SB_REPOSITORY_ROOT");
	FILE *f;
	char dev[128], mnt[256], type[32], line[512];
	if (override && *override)
		return 1;
	f = fopen("/proc/mounts", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%127s %255s %31s", dev, mnt, type) == 3 &&
		    !strcmp(mnt, "/mnt/sbdata") &&
		    (!strcmp(type, "sbubifs") || !strcmp(type, "ubifs")) &&
		    (!strcmp(dev, "ubi0:sbdata") || !strcmp(dev, "/dev/ubi0_5"))) {
			fclose(f);
			return 1;
		}
	fclose(f);
	return 0;
}

/**
 * @brief Handle repo upload limit.
 * @return Number of bytes or items produced by the operation.
 */
size_t repo_upload_limit(void)
{
#ifndef _WIN32
	struct statvfs v;
	uint64_t free_bytes;
	const char *p = repo_root();
	if (statvfs(p, &v)) {
		p = mounted_sbdata() ? "/mnt/sbdata" : config_dir();
		if (statvfs(p, &v))
			return 0;
	}
	free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
	if (free_bytes <= INTERNAL_RESERVE)
		return 0;
	free_bytes -= INTERNAL_RESERVE;
	return free_bytes > REPO_MAX_UPLOAD ? REPO_MAX_UPLOAD
					    : (size_t)free_bytes;
#else
	return REPO_MAX_UPLOAD;
#endif
}

/**
 * @brief Handle repo storage ready.
 * @param reason Mutable text buffer for reason.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_storage_ready(char *reason, size_t n)
{
	char p[600];
	if (!mounted_sbdata() && !getenv("SB_REPOSITORY_ROOT") &&
	    !getenv("SB_REPOSITORY_INTERNAL_ROOT") &&
	    mkdir_one("/mnt/storage/standalonebase", 0700)) {
		err(reason, n, "Internal storage is unavailable");
		return 0;
	}
	if (mkdir_one(repo_root(), 0700)) {
		err(reason, n, "Cannot create repository directory");
		return 0;
	}
	snprintf(p, sizeof p, "%s/packages", repo_root());
	if (mkdir_one(p, 0700)) {
		err(reason, n, "Cannot create package directory");
		return 0;
	}
	snprintf(p, sizeof p, "%s/tmp", repo_root());
	if (mkdir_one(p, 0700)) {
		err(reason, n, "Cannot create temporary directory");
		return 0;
	}
#ifndef _WIN32
	if (!repo_upload_limit()) {
		err(reason, n, "Insufficient repository storage");
		return 0;
	}
#endif
	return 1;
}

/**
 * @brief Read file.
 * @param p Input text for p.
 * @param len Length of the supplied data.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static char *read_file(const char *p, size_t *len)
{
	FILE *f = fopen(p, "rb");
	long z;
	char *b;
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) || (z = ftell(f)) < 2 ||
	    (size_t)z > CONFIG_LIMIT || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return NULL;
	}
	b = malloc((size_t)z + 1);
	if (!b) {
		fclose(f);
		return NULL;
	}
	if (fread(b, 1, (size_t)z, f) != (size_t)z) {
		free(b);
		fclose(f);
		return NULL;
	}
	fclose(f);
	b[z] = 0;
	*len = (size_t)z;
	return b;
}

/**
 * @brief Handle array bounds.
 * @param json Mutable text buffer for json.
 * @param end Mutable text buffer for end.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *array_bounds(char *json, char **end)
{
	char *p = strstr(json, "\"applets\"");
	int depth = 0, str = 0, esc = 0;
	if (!p || (p = strchr(p, '[')) == NULL)
		return NULL;
	for (char *q = p; *q; q++) {
		char c = *q;
		if (str) {
			if (esc)
				esc = 0;
			else if (c == '\\')
				esc = 1;
			else if (c == '\"')
				str = 0;
		} else if (c == '\"')
			str = 1;
		else if (c == '[')
			depth++;
		else if (c == ']' && !--depth) {
			*end = q;
			return p;
		}
	}
	return NULL;
}

/**
 * @brief Copy json.
 * @param obj Input value for obj.
 * @param key Input text for key.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 */
static void copy_json(struct mg_str obj, const char *key, char *out, size_t cap)
{
	char path[80], *v;
	snprintf(path, sizeof path, "$.%s", key);
	v = mg_json_get_str(obj, path);
	if (v) {
		snprintf(out, cap, "%s", v);
		free(v);
	} else
		out[0] = 0;
}

/**
 * @brief Parse items.
 * @param json Mutable text buffer for json.
 * @param items Object referenced by items.
 * @param count Object referenced by count.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int parse_items(char *json, RepoApplet *items, int *count)
{
	char *end;
	const char *a = array_bounds(json, &end), *p;
	int c = 0;
	if (!a)
		return -1;
	p = a + 1;
	while (p < end) {
		const char *s, *q;
		int d = 0, str = 0, esc = 0;
		while (p < end && *p != '{')
			p++;
		if (p >= end)
			break;
		s = p;
		q = p;
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
		if (d || c >= REPO_MAX_APPLETS)
			return -1;
		struct mg_str o = {(char *)s, (size_t)(q - s)};
		RepoApplet *i = &items[c];
		memset(i, 0, sizeof *i);
		copy_json(o, "name", i->name, sizeof i->name);
		copy_json(o, "title", i->title, sizeof i->title);
		copy_json(o, "version", i->version, sizeof i->version);
		copy_json(o, "target", i->target, sizeof i->target);
		copy_json(o,
			  "min_target_version",
			  i->min_target_version,
			  sizeof i->min_target_version);
		copy_json(o, "url", i->url, sizeof i->url);
		copy_json(o, "sha", i->sha, sizeof i->sha);
		copy_json(o, "desc", i->desc, sizeof i->desc);
		copy_json(o, "changes", i->changes, sizeof i->changes);
		copy_json(o, "creator", i->creator, sizeof i->creator);
		copy_json(o, "email", i->email, sizeof i->email);
		if (!i->name[0])
			return -1;
		c++;
		p = q;
	}
	*count = c;
	return 0;
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
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			fputc('\\', f);
			fputc(c, f);
		} else if (c == '\n')
			fputs("\\n", f);
		else if (c == '\r')
			fputs("\\r", f);
		else if (c == '\t')
			fputs("\\t", f);
		else if (c >= 32)
			fputc(c, f);
	}
	fputc('"', f);
}

/**
 * @brief Write item.
 * @param f Open stream to process.
 * @param i Object referenced by i.
 */
static void write_item(FILE *f, const RepoApplet *i)
{
	const char *k[] = {"name",
			   "title",
			   "version",
			   "target",
			   "min_target_version",
			   "url",
			   "sha",
			   "desc",
			   "changes",
			   "creator",
			   "email"};
	const char *v[] = {i->name,
			   i->title,
			   i->version,
			   i->target,
			   i->min_target_version,
			   i->url,
			   i->sha,
			   i->desc,
			   i->changes,
			   i->creator,
			   i->email};
	fputs("    {\n", f);
	for (int x = 0; x < 11; x++) {
		fprintf(f, "      \"%s\": ", k[x]);
		js(f, v[x]);
		fprintf(f, "%s\n", x == 10 ? "" : ",");
	}
	fputs("    }", f);
}

/**
 * @brief Handle lock config.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int lock_config(void)
{
	char p[600];
	int fd;
	snprintf(p, sizeof p, "%s/.catalog.lock", config_dir());
	fd = open(p, O_CREAT | O_RDWR, 0600);
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
 * @brief Handle unlock config.
 * @param fd Open file descriptor.
 */
static void unlock_config(int fd)
{
#ifndef _WIN32
	flock(fd, LOCK_UN);
#endif
	close(fd);
}

/* Remote release metadata is deliberately separate from runtime settings. */
typedef struct {
	struct mg_str object;
	char name[65], target[33], version[33], url[513];
	int pinned;
} CatalogEntry;

static const char *release_fields[] = {
	"name", "title", "version", "target", "min_target_version", "url",
	"sha", "desc", "changes", "creator", "email"
};

/** Return a release field index, or -1 for a local-only extension field. */
static int release_field(const char *name)
{
	for (unsigned i = 0; i < sizeof release_fields / sizeof *release_fields; i++)
		if (!strcmp(name, release_fields[i]))
			return (int)i;
	return -1;
}

/** Disallow embedded NULs in metadata decoded into bounded C strings. */
static int catalog_string_nul(struct mg_str value)
{
	for (size_t i = 0; i < value.len; i++) {
		if (value.buf[i] != '\\')
			continue;
		i++;
		if (i + 4 < value.len && value.buf[i] == 'u' &&
		    !memcmp(value.buf + i + 1, "0000", 4))
			return 1;
	}
	return 0;
}

/** Validate a complete object and select its unique top-level applets array. */
static int catalog_array(struct mg_str json, struct mg_str *array)
{
	int length = 0, offset = mg_json_get(json, "$", &length), found = 0;
	struct mg_str key, value;
	size_t position = 0;
	int string = 0, escape = 0;
	char previous = 0;

	/* Mongoose tolerates trailing commas; persisted catalogs must not. */
	for (size_t i = 0; i < json.len; i++) {
		unsigned char c = (unsigned char)json.buf[i];
		if (string) {
			if (c < 32)
				return -1;
			if (escape)
				escape = 0;
			else if (c == '\\')
				escape = 1;
			else if (c == '"')
				string = 0;
		} else {
			if ((c == '}' || c == ']') && previous == ',')
				return -1;
			if (c == '"')
				string = 1;
		}
		if (!isspace(c))
			previous = (char)c;
	}

	if (offset < 0 || length < 2 || json.buf[offset] != '{' ||
	    memchr(json.buf, 0, json.len))
		return -1;
	for (size_t i = (size_t)(offset + length); i < json.len; i++)
		if (!isspace((unsigned char)json.buf[i]))
			return -1;
	json = mg_str_n(json.buf + offset, (size_t)length);
	while ((position = mg_json_next(json, position, &key, &value))) {
		char *name = mg_json_get_str(key, "$");
		if (!name)
			return -1;
		if (!strcmp(name, "applets")) {
			if (found++ || !value.len || value.buf[0] != '[') {
				free(name);
				return -1;
			}
			*array = value;
		}
		free(name);
	}
	return found == 1 ? 0 : -1;
}

/** Parse bounded entries; remote fields must be known, untruncated strings. */
static int catalog_entries(struct mg_str array, CatalogEntry *entries,
			   int *count, int remote)
{
	struct mg_str object, key, value;
	size_t position = 0;
	int total = 0;

	while ((position = mg_json_next(array, position, NULL, &object))) {
		RepoApplet item = {0};
		char *fields[] = {item.name, item.title, item.version, item.target,
			item.min_target_version, item.url, item.sha, item.desc,
			item.changes, item.creator, item.email};
		size_t sizes[] = {sizeof item.name, sizeof item.title,
			sizeof item.version, sizeof item.target,
			sizeof item.min_target_version, sizeof item.url,
			sizeof item.sha, sizeof item.desc, sizeof item.changes,
			sizeof item.creator, sizeof item.email};
		unsigned seen = 0;
		size_t field_position = 0;

		if (total == REPO_MAX_APPLETS || !object.len || object.buf[0] != '{')
			return -1;
		while ((field_position = mg_json_next(object, field_position, &key, &value))) {
			char *name = mg_json_get_str(key, "$");
			int field = name ? release_field(name) : -1;
			free(name);
			if (field < 0) {
				if (remote)
					return -1;
				continue;
			}
			char *text = mg_json_get_str(value, "$");
			if (!text || catalog_string_nul(value) || strlen(text) >= sizes[field] ||
			    (seen & (1u << field))) {
				free(text);
				return -1;
			}
			seen |= 1u << field;
			strcpy(fields[field], text);
			free(text);
		}
		if (!safe_name(item.name) || !item.target[0] || !safe_version(item.version) ||
		    (remote && (repo_validate_metadata(&item, 1, NULL, 0) ||
				 repo_is_local_url(item.url))))
			return -1;
		if (remote) {
			for (const unsigned char *p = (unsigned char *)item.url; *p; p++)
				if (*p <= 32 || *p == 127)
					return -1;
			const char *authority = strstr(item.url, "://") + 3;
			if (!*authority || strchr("/?#", *authority))
				return -1;
			for (const char *p = authority; *p && !strchr("/?#", *p); p++)
				if (*p == '@')
					return -1;
		}
		for (int i = 0; i < total; i++)
			if (!strcmp(entries[i].name, item.name) &&
			    !strcmp(entries[i].target, item.target))
				return -1;
		CatalogEntry *entry = &entries[total++];
		entry->object = object;
		strcpy(entry->name, item.name);
		strcpy(entry->target, item.target);
		strcpy(entry->version, item.version);
		strcpy(entry->url, item.url);
		bool pin = false;
		mg_json_get_bool(object, "$.catalog_pin", &pin);
		entry->pinned = pin || repo_is_local_url(item.url);
	}
	*count = total;
	return 0;
}

/** Compare dotted numeric releases without integer overflow; -2 means custom. */
static int release_compare(const char *a, const char *b)
{
	if (!isdigit((unsigned char)*a) || !isdigit((unsigned char)*b))
		return -2;
	for (const char *s = a; *s; s++)
		if ((!isdigit((unsigned char)*s) && *s != '.') ||
		    (*s == '.' && !isdigit((unsigned char)s[1])))
			return -2;
	for (const char *s = b; *s; s++)
		if ((!isdigit((unsigned char)*s) && *s != '.') ||
		    (*s == '.' && !isdigit((unsigned char)s[1])))
			return -2;
	while (*a || *b) {
		const char *ae = a, *be = b;
		while (isdigit((unsigned char)*ae)) ae++;
		while (isdigit((unsigned char)*be)) be++;
		while (a < ae && *a == '0') a++;
		while (b < be && *b == '0') b++;
		if (ae - a != be - b)
			return ae - a > be - b ? 1 : -1;
		int order = memcmp(a, b, (size_t)(ae - a));
		if (order)
			return order > 0 ? 1 : -1;
		a = *ae ? ae + 1 : ae;
		b = *be ? be + 1 : be;
	}
	return 0;
}

/** Append raw JSON into the native service's bounded catalog envelope. */
static int catalog_append(char *out, size_t *used, struct mg_str text)
{
	if (text.len > 65535u - *used)
		return -1;
	memcpy(out + *used, text.buf, text.len);
	*used += text.len;
	out[*used] = 0;
	return 0;
}

/** Replace release fields while retaining unknown local per-applet settings. */
static int catalog_replace(char *out, size_t *used,
			   struct mg_str local, struct mg_str remote)
{
	struct mg_str key, value;
	size_t position = 0;
	if (catalog_append(out, used, mg_str_n(remote.buf, remote.len - 1)))
		return -1;
	while ((position = mg_json_next(local, position, &key, &value))) {
		char *name = mg_json_get_str(key, "$");
		int known = name ? release_field(name) : -1;
		free(name);
		if (known >= 0)
			continue;
		if (catalog_append(out, used, mg_str(",")) ||
		    catalog_append(out, used, key) ||
		    catalog_append(out, used, mg_str(":")) ||
		    catalog_append(out, used, value))
			return -1;
	}
	return catalog_append(out, used, mg_str("}"));
}

/** Atomically commit a changed catalog, preserving mode and closing on errors. */
static int catalog_commit(const char *data, size_t length)
{
	char path[600];
	struct stat st;
	snprintf(path, sizeof path, "%s.refresh.XXXXXX", repo_config_path());
	int fd = mkstemp(path), failed = 0;
	if (fd < 0)
		return -1;
	if (!stat(repo_config_path(), &st) && fchmod(fd, st.st_mode & 0777))
		failed = 1;
	FILE *stream = fdopen(fd, "wb");
	if (!stream) {
		close(fd);
		unlink(path);
		return -1;
	}
	if (fwrite(data, 1, length, stream) != length || fflush(stream) || fsync(fd))
		failed = 1;
	if (fclose(stream))
		failed = 1;
	if (failed || rename(path, repo_config_path())) {
		unlink(path);
		return -1;
	}
	fd = open(config_dir(), O_RDONLY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	return 0;
}

/** Merge upstream releases only; retain settings, pins and newer/custom builds.
 * @param json Remote UTF-8 catalog bytes, not necessarily NUL terminated.
 * @param length Remote byte count (at most 16 KiB).
 * @param changed Receives number of added or upgraded entries.
 * @param error Receives a generic error, if supplied.
 * @param error_size Capacity of error.
 * @return Zero on success (including no change); -1 leaves the catalog intact.
 */
int repo_merge_catalog(const char *json, size_t length, int *changed,
		       char *error, size_t error_size)
{
	struct mg_str remote_array, local_array;
	CatalogEntry *entries = calloc(REPO_MAX_APPLETS * 2, sizeof *entries);
	CatalogEntry *upstream = entries, *local = entries ? entries + REPO_MAX_APPLETS : NULL;
	char *original = NULL, *out = NULL;
	size_t original_size, used = 0, array_start;
	int remote_count, local_count, changes = 0, lock = -1, rc = -1;
	unsigned consumed = 0;

	*changed = 0;
	if (!entries || !json || length > 16384 ||
	    catalog_array(mg_str_n(json, length), &remote_array) ||
	    catalog_entries(remote_array, upstream, &remote_count, 1) || !remote_count)
		goto done;
	lock = lock_config();
	if (lock < 0)
		goto done;
	original = read_file(repo_config_path(), &original_size);
	if (!original || original_size > 65535 ||
	    catalog_array(mg_str_n(original, original_size), &local_array) ||
	    catalog_entries(local_array, local, &local_count, 0) ||
	    !(out = malloc(65536)))
		goto done;
	if (catalog_append(out, &used, mg_str_n(original, (size_t)(local_array.buf - original))) ||
	    catalog_append(out, &used, mg_str("[")))
		goto done;
	array_start = used;
	for (int i = 0; i < local_count; i++) {
		int match = -1;
		for (int j = 0; j < remote_count; j++)
			if (!strcmp(local[i].name, upstream[j].name) &&
			    !strcmp(local[i].target, upstream[j].target))
				match = j;
		if (i && catalog_append(out, &used, mg_str(",")))
			goto done;
		if (match >= 0)
			consumed |= 1u << match;
		if (match >= 0 && !local[i].pinned &&
		    release_compare(upstream[match].version, local[i].version) == 1) {
			if (catalog_replace(out, &used, local[i].object, upstream[match].object))
				goto done;
			changes++;
		} else if (catalog_append(out, &used, local[i].object))
			goto done;
	}
	for (int j = 0; j < remote_count; j++) {
		if (consumed & (1u << j))
			continue;
		if (local_count == REPO_MAX_APPLETS ||
		    (local_count && catalog_append(out, &used, mg_str(","))) ||
		    catalog_append(out, &used, upstream[j].object))
			goto done;
		local_count++;
		changes++;
	}
	/* sbbase's installer response is bounded at 16 KiB. */
	if (used - array_start > 15000 ||
	    catalog_append(out, &used, mg_str("]")) ||
	    catalog_append(out, &used, mg_str_n(local_array.buf + local_array.len,
		original_size - (size_t)(local_array.buf + local_array.len - original))) ||
	    (changes && catalog_commit(out, used)))
		goto done;
	*changed = changes;
	rc = 0;
done:
	if (rc)
		err(error, error_size, "Catalog validation, capacity or storage failure");
	if (lock >= 0)
		unlock_config(lock);
	free(entries);
	free(original);
	free(out);
	if (!rc && changes)
		repo_notify_catalog();
	return rc;
}

/**
 * @brief Save all.
 * @param original Mutable text buffer for original.
 * @param items Object referenced by items.
 * @param count Numeric count.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int save_all(char *original, RepoApplet *items, int count)
{
	char *ae;
	const char *as = array_bounds(original, &ae);
	char tmp[600];
	FILE *f;
	struct stat st;
	if (!as)
		return -1;
	snprintf(tmp,
		 sizeof tmp,
		 "%s.tmp.%ld",
		 repo_config_path(),
		 (long)getpid());
	f = fopen(tmp, "wb");
	if (!f)
		return -1;
	fwrite(original, 1, (size_t)(as - original), f);
	fputs("[\n", f);
	for (int i = 0; i < count; i++) {
		write_item(f, &items[i]);
		fputs(i + 1 < count ? ",\n" : "\n", f);
	}
	fputc(']', f);
	fputs(ae + 1, f);
	if (fflush(f)
#ifndef _WIN32
	    || fsync(fileno(f))
#endif
	    || fclose(f)) {
		unlink(tmp);
		return -1;
	}
	if (!stat(repo_config_path(), &st))
		chmod(tmp, st.st_mode & 0777);
	if (rename(tmp, repo_config_path())) {
		unlink(tmp);
		return -1;
	}
#ifndef _WIN32
	{
		int d = open(config_dir(), O_RDONLY);
		if (d >= 0) {
			fsync(d);
			close(d);
		}
	}
#endif
	return 0;
}

/**
 * @brief Handle mark local catalog.
 */
static void mark_local_catalog(void)
{
	char p[600];
	FILE *f;
	snprintf(p, sizeof p, "%s/.catalog-local", config_dir());
	f = fopen(p, "w");
	if (f) {
		fputs("Managed by StandaloneBase web repository\n", f);
		fclose(f);
	}
}

/**
 * @brief Handle repo validate metadata.
 * @param i Object referenced by i.
 * @param external Numeric value for external.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_validate_metadata(const RepoApplet *i, int external, char *e, size_t n)
{
	if (!safe_name(i->name)) {
		err(e, n, "Invalid applet name");
		return -1;
	}
	if (!safe_version(i->version)) {
		err(e, n, "Invalid version");
		return -1;
	}
	if (!i->title[0] || !i->target[0] || !i->min_target_version[0]) {
		err(e,
		    n,
		    "Title, target and minimum target version are required");
		return -1;
	}
	if (external && strncmp(i->url, "http://", 7) &&
	    strncmp(i->url, "https://", 8)) {
		err(e, n, "Invalid external URL");
		return -1;
	}
	if (external && !sha_valid(i->sha)) {
		err(e, n, "SHA-1 must contain 40 hexadecimal characters");
		return -1;
	}
	return 0;
}

/**
 * @brief Handle repo get.
 * @param name Name of the item to process.
 * @param out Destination buffer for the result.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_get(const char *name, RepoApplet *out)
{
	size_t z;
	char *b = read_file(repo_config_path(), &z);
	RepoApplet a[REPO_MAX_APPLETS];
	int c;
	if (!b || parse_items(b, a, &c)) {
		free(b);
		return -1;
	}
	for (int i = 0; i < c; i++)
		if (!strcmp(a[i].name, name)) {
			*out = a[i];
			free(b);
			return 0;
		}
	free(b);
	return 1;
}

/**
 * @brief Handle out text.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @param u Object referenced by u.
 * @param s Input text for s.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int out_text(char *out, size_t cap, size_t *u, const char *s)
{
	while (*s) {
		unsigned char c = (unsigned char)*s++;
		if (*u + 7 >= cap)
			return -1;
		if (c == '\"' || c == '\\') {
			out[(*u)++] = '\\';
			out[(*u)++] = (char)c;
		} else if (c == '\n') {
			out[(*u)++] = '\\';
			out[(*u)++] = 'n';
		} else if (c >= 32)
			out[(*u)++] = (char)c;
	}
	out[*u] = 0;
	return 0;
}

/**
 * @brief Handle out key.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @param u Object referenced by u.
 * @param k Input text for k.
 * @param v Input text for v.
 * @param comma Numeric value for comma.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int out_key(char *out,
		   size_t cap,
		   size_t *u,
		   const char *k,
		   const char *v,
		   int comma)
{
	int z = snprintf(
		out + *u, cap - *u, "%s\"%s\":\"", comma ? "," : "", k);
	if (z < 0 || (size_t)z >= cap - *u)
		return -1;
	*u += (size_t)z;
	if (out_text(out, cap, u, v))
		return -1;
	if (*u + 2 >= cap)
		return -1;
	out[(*u)++] = '\"';
	out[*u] = 0;
	return 0;
}

/**
 * @brief Handle repo list json.
 * @param out Destination buffer for the result.
 * @param cap Size or count for cap.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_list_json(char *out, size_t cap)
{
	size_t z, u = 0, limit = repo_upload_limit();
	char *b = read_file(repo_config_path(), &z);
	RepoApplet a[REPO_MAX_APPLETS];
	int c;
	if (!b || parse_items(b, a, &c)) {
		free(b);
		return -1;
	}
	u += (size_t)snprintf(out + u, cap - u, "{\"applets\":[");
	for (int i = 0; i < c; i++) {
		RepoApplet *x = &a[i];
		if (u + 2 >= cap)
			goto overflow;
		if (i)
			out[u++] = ',';
		out[u++] = '{';
		out[u] = 0;
		if (out_key(out, cap, &u, "name", x->name, 0) ||
		    out_key(out, cap, &u, "title", x->title, 1) ||
		    out_key(out, cap, &u, "version", x->version, 1) ||
		    out_key(out, cap, &u, "target", x->target, 1) ||
		    out_key(out,
			    cap,
			    &u,
			    "min_target_version",
			    x->min_target_version,
			    1) ||
		    out_key(out, cap, &u, "url", x->url, 1) ||
		    out_key(out, cap, &u, "sha", x->sha, 1) ||
		    out_key(out, cap, &u, "desc", x->desc, 1) ||
		    out_key(out, cap, &u, "changes", x->changes, 1) ||
		    out_key(out, cap, &u, "creator", x->creator, 1) ||
		    out_key(out, cap, &u, "email", x->email, 1) ||
		    out_key(out,
			    cap,
			    &u,
			    "source",
			    repo_is_local_url(x->url) ? "local" : "external",
			    1))
			goto overflow;
		if (u + 2 >= cap)
			goto overflow;
		out[u++] = '}';
		out[u] = 0;
	}
	if (snprintf(out + u,
		     cap - u,
		     "],\"upload_limit\":%lu,\"storage_active\":%s}",
		     (unsigned long)limit,
		     limit ? "true" : "false") < 0)
		goto overflow;
	free(b);
	return 0;
overflow:
	free(b);
	if (cap)
		out[cap - 1] = 0;
	return -1;
}

/**
 * @brief Handle repo save applet.
 * @param i Object referenced by i.
 * @param existing Numeric value for existing.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_save_applet(const RepoApplet *i, int existing, char *e, size_t n)
{
	int lock = lock_config(), c, found = -1, rc = -1;
	size_t z;
	char *b;
	RepoApplet next = *i, a[REPO_MAX_APPLETS];
	if (lock < 0) {
		err(e, n, "Catalog is busy or read-only");
		return -1;
	}
	b = read_file(repo_config_path(), &z);
	if (!b || parse_items(b, a, &c)) {
		err(e, n, "Invalid catalog configuration");
		goto done;
	}
	for (int x = 0; x < c; x++)
		if (!strcmp(a[x].name, i->name))
			found = x;
	if (existing && found < 0) {
		err(e, n, "Applet does not exist");
		goto done;
	}
	/* URL and digest of a local package are derived from immutable ZIP
	 * bytes. */
	if (existing && found >= 0 && repo_is_local_url(a[found].url)) {
		snprintf(next.url, sizeof next.url, "%s", a[found].url);
		snprintf(next.sha, sizeof next.sha, "%s", a[found].sha);
	}
	if (found < 0) {
		if (c >= REPO_MAX_APPLETS) {
			err(e, n, "Catalog is full");
			goto done;
		}
		found = c++;
	}
	a[found] = next;
	if (save_all(b, a, c)) {
		err(e, n, "Atomic catalog update failed");
		goto done;
	}
	mark_local_catalog();
	rc = 0;
done:
	free(b);
	unlock_config(lock);
	if (!rc)
		repo_notify_catalog();
	return rc;
}

/**
 * @brief Handle local id.
 * @param u Input text for u.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *local_id(const char *u)
{
	return repo_is_local_url(u) ? u + strlen(LOCAL_PREFIX) : NULL;
}

/**
 * @brief Handle repo package path.
 * @param id Identifier of the requested item.
 * @param p Mutable text buffer for p.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_package_path(const char *id, char *p, size_t n)
{
	if (!id || strstr(id, "..") || strchr(id, '/') || strchr(id, '\\') ||
	    strlen(id) > 120 || strlen(id) < 5 ||
	    strcmp(id + strlen(id) - 4, ".zip"))
		return -1;
	for (const char *q = id; *q; q++)
		if (!(isalnum((unsigned char)*q) || strchr("._+-", *q)))
			return -1;
	snprintf(p, n, "%s/packages/%s", repo_root(), id);
	return 0;
}

/**
 * @brief Handle repo remove applet.
 * @param name Name of the item to process.
 * @param cleanup Numeric value for cleanup.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_remove_applet(const char *name, int cleanup, char *e, size_t n)
{
	int lock = lock_config(), c, at = -1, rc = -1;
	size_t z;
	char *b, id[160] = {0}, path[700];
	RepoApplet a[REPO_MAX_APPLETS];
	if (lock < 0) {
		err(e, n, "Catalog is busy or read-only");
		return -1;
	}
	b = read_file(repo_config_path(), &z);
	if (!b || parse_items(b, a, &c)) {
		err(e, n, "Invalid catalog configuration");
		goto done;
	}
	for (int i = 0; i < c; i++)
		if (!strcmp(a[i].name, name))
			at = i;
	if (at < 0) {
		err(e, n, "Applet does not exist");
		goto done;
	}
	if (cleanup && local_id(a[at].url))
		snprintf(id, sizeof id, "%s", local_id(a[at].url));
	memmove(&a[at], &a[at + 1], (size_t)(c - at - 1) * sizeof *a);
	c--;
	if (save_all(b, a, c)) {
		err(e, n, "Atomic catalog update failed");
		goto done;
	}
	rc = 0;
	if (id[0] && repo_package_path(id, path, sizeof path) == 0)
		unlink(path);
done:
	free(b);
	unlock_config(lock);
	if (!rc)
		repo_notify_catalog();
	return rc;
}

/* Small SHA-1 implementation: protocol compatibility, not package trust. */
typedef struct {
	uint32_t h[5];
	uint64_t bits;
	unsigned char b[64];
	size_t n;
} SHA1;

/**
 * @brief Handle rol.
 * @param x Input value for x.
 * @param n Capacity or item count associated with the operation.
 * @return Value produced by the operation.
 */
static uint32_t rol(uint32_t x, unsigned n)
{
	return (x << n) | (x >> (32 - n));
}

/**
 * @brief Handle shablock.
 * @param s Object referenced by s.
 * @param b Input text for b.
 */
static void shablock(SHA1 *s, const unsigned char *b)
{
	uint32_t w[80], a, c, d, e, f, k, t, bb;
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 |
		       (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = s->h[0];
	bb = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	for (int i = 0; i < 80; i++) {
		if (i < 20) {
			f = (bb & c) | ((~bb) & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = bb ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (bb & c) | (bb & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = bb ^ c ^ d;
			k = 0xca62c1d6;
		}
		t = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol(bb, 30);
		bb = a;
		a = t;
	}
	s->h[0] += a;
	s->h[1] += bb;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
}

/**
 * @brief Handle shainit.
 * @param s Object referenced by s.
 */
static void shainit(SHA1 *s)
{
	memset(s, 0, sizeof *s);
	s->h[0] = 0x67452301;
	s->h[1] = 0xefcdab89;
	s->h[2] = 0x98badcfe;
	s->h[3] = 0x10325476;
	s->h[4] = 0xc3d2e1f0;
}

/**
 * @brief Handle shaup.
 * @param s Object referenced by s.
 * @param p Object referenced by p.
 * @param n Capacity or item count associated with the operation.
 */
static void shaup(SHA1 *s, const void *p, size_t n)
{
	const unsigned char *b = p;
	s->bits += (uint64_t)n * 8;
	while (n) {
		size_t x = 64 - s->n;
		if (x > n)
			x = n;
		memcpy(s->b + s->n, b, x);
		s->n += x;
		b += x;
		n -= x;
		if (s->n == 64) {
			shablock(s, s->b);
			s->n = 0;
		}
	}
}

/**
 * @brief Handle shafinal.
 * @param s Object referenced by s.
 * @param d Numeric value for d.
 */
static void shafinal(SHA1 *s, unsigned char d[20])
{
	uint64_t bits = s->bits;
	unsigned char one = 0x80, zero = 0;
	shaup(s, &one, 1);
	while (s->n != 56)
		shaup(s, &zero, 1);
	unsigned char z[8];
	for (int i = 0; i < 8; i++)
		z[7 - i] = (unsigned char)(bits >> (i * 8));
	shaup(s, z, 8);
	for (int i = 0; i < 5; i++) {
		d[i * 4] = s->h[i] >> 24;
		d[i * 4 + 1] = s->h[i] >> 16;
		d[i * 4 + 2] = s->h[i] >> 8;
		d[i * 4 + 3] = s->h[i];
	}
}

/**
 * @brief Handle repo sha1 file.
 * @param p Input text for p.
 * @param hex Input value for hex.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_sha1_file(const char *p, char hex[41])
{
	FILE *f = fopen(p, "rb");
	SHA1 s;
	unsigned char b[32768], d[20];
	size_t n;
	if (!f)
		return -1;
	shainit(&s);
	while ((n = fread(b, 1, sizeof b, f)))
		shaup(&s, b, n);
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	shafinal(&s, d);
	for (int i = 0; i < 20; i++)
		sprintf(hex + i * 2, "%02x", d[i]);
	return 0;
}

/**
 * @brief Handle le16.
 * @param p Input text for p.
 * @return Value produced by the operation.
 */
static uint16_t le16(const unsigned char *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

/**
 * @brief Handle le32.
 * @param p Input text for p.
 * @return Value produced by the operation.
 */
static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/**
 * @brief Handle repo validate zip.
 * @param p Input text for p.
 * @param det Mutable text buffer for det.
 * @param dc Size or count for dc.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_validate_zip(const char *p, char *det, size_t dc, char *e, size_t n)
{
	FILE *f = fopen(p, "rb");
	long size, start;
	unsigned char tail[65557], h[46];
	size_t got;
	long eocd = -1;
	uint16_t entries;
	uint32_t cdsize, cdoff;
	int meta = 0, applet = 0;
	if (det && dc)
		det[0] = 0;
	if (!f) {
		err(e, n, "Cannot open uploaded package");
		return -1;
	}
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22) {
		err(e, n, "Truncated ZIP archive");
		goto bad;
	}
	start = size > (long)sizeof tail ? size - (long)sizeof tail : 0;
	fseek(f, start, SEEK_SET);
	got = fread(tail, 1, (size_t)(size - start), f);
	for (long i = (long)got - 22; i >= 0; i--)
		if (le32(tail + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	if (eocd < 0) {
		err(e, n, "ZIP central directory is missing");
		goto bad;
	}
	entries = le16(tail + eocd + 10);
	cdsize = le32(tail + eocd + 12);
	cdoff = le32(tail + eocd + 16);
	if (!entries || entries > REPO_MAX_ENTRIES ||
	    (uint64_t)cdoff + cdsize > (uint64_t)size) {
		err(e, n, "Invalid ZIP central directory");
		goto bad;
	}
	fseek(f, cdoff, SEEK_SET);
	uint64_t total = 0;
	for (unsigned x = 0; x < entries; x++) {
		if (fread(h, 1, 46, f) != 46 || le32(h) != 0x02014b50) {
			err(e, n, "Invalid ZIP entry");
			goto bad;
		}
		uint16_t flags = le16(h + 8), method = le16(h + 10),
			 nl = le16(h + 28), xl = le16(h + 30),
			 cl = le16(h + 32);
		uint32_t cs = le32(h + 20), us = le32(h + 24),
			 attr = le32(h + 38), loff = le32(h + 42);
		char name[513];
		if (!nl || nl >= sizeof name || fread(name, 1, nl, f) != nl) {
			err(e, n, "Invalid ZIP filename");
			goto bad;
		}
		name[nl] = 0;
		if (fseek(f, (long)xl + cl, SEEK_CUR)) {
			err(e, n, "Truncated ZIP entry");
			goto bad;
		}
		long next = ftell(f);
		unsigned char lh[30];
		if ((uint64_t)loff + 30 >= (uint64_t)size ||
		    fseek(f, loff, SEEK_SET) || fread(lh, 1, 30, f) != 30 ||
		    le32(lh) != 0x04034b50 ||
		    (uint64_t)loff + 30 + le16(lh + 26) + le16(lh + 28) + cs >
			    (uint64_t)size ||
		    fseek(f, next, SEEK_SET)) {
			err(e, n, "ZIP entry points outside the archive");
			goto bad;
		}
		if ((flags & 1) || (method != 0 && method != 8)) {
			err(e, n, "Encrypted or unsupported ZIP entry");
			goto bad;
		}
		if (name[0] == '/' || name[0] == '\\' || strstr(name, "../") ||
		    strstr(name, "..\\") || strchr(name, ':')) {
			err(e, n, "Unsafe ZIP path");
			goto bad;
		}
		if ((attr >> 16 & 0170000) &&
		    ((attr >> 16 & 0170000) != 0100000) &&
		    name[nl - 1] != '/') {
			err(e, n, "Unsupported ZIP special file");
			goto bad;
		}
		total += us;
		if (total > REPO_MAX_UNCOMPRESSED || (cs && us / cs > 200)) {
			err(e, n, "ZIP expansion limit exceeded");
			goto bad;
		}
		const char *base = strrchr(name, '/');
		base = base ? base + 1 : name;
		size_t bl = strlen(base);
		if (bl > 8 && !strcmp(base + bl - 8, "Meta.lua")) {
			meta = 1;
			if (det && dc) {
				size_t z = bl - 8;
				if (z >= dc)
					z = dc - 1;
				memcpy(det, base, z);
				det[z] = 0;
			}
		}
		if (bl > 10 && !strcmp(base + bl - 10, "Applet.lua"))
			applet = 1;
	}
	fclose(f);
	if (!meta || !applet) {
		err(e,
		    n,
		    "ZIP does not contain a Jive applet Meta.lua and "
		    "Applet.lua");
		return -1;
	}
	return 0;
bad:
	fclose(f);
	return -1;
}

/**
 * @brief Handle repo commit upload.
 * @param tmp Input text for tmp.
 * @param i Object referenced by i.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int repo_commit_upload(const char *tmp, RepoApplet *i, char *e, size_t n)
{
	char detected[65], sha[41], dir[700], dest[800], id[160];
	if (repo_validate_zip(tmp, detected, sizeof detected, e, n)) {
		return -1;
	} /* A valid Meta.lua filename is safer than accepting a display title
or other user text as a filesystem/catalog identifier. Preserve a valid explicit
name, but recover empty or syntactically invalid form values from the package.
*/
	if (!safe_name(i->name) && safe_name(detected))
		snprintf(i->name, sizeof i->name, "%s", detected);
	if (repo_validate_metadata(i, 0, e, n) || repo_sha1_file(tmp, sha)) {
		if (!e[0])
			err(e, n, "Package validation failed");
		return -1;
	}
	if (detected[0] && strcmp(detected, i->name)) {
		err(e, n, "Applet name does not match package metadata");
		return -1;
	}
	snprintf(id, sizeof id, "%s-%s.zip", i->name, i->version);
	snprintf(dir, sizeof dir, "%s/packages", repo_root());
	if (mkdir_one(dir, 0700)) {
		err(e, n, "Cannot create package directory");
		return -1;
	}
	if (repo_package_path(id, dest, sizeof dest)) {
		err(e, n, "Invalid package filename");
		return -1;
	}
	if (access(dest, F_OK) == 0) {
		err(e, n, "This applet version already exists");
		return -1;
	}
	if (rename(tmp, dest)) {
		err(e, n, "Cannot commit uploaded package");
		return -1;
	}
	snprintf(i->sha, sizeof i->sha, "%s", sha);
	snprintf(i->url, sizeof i->url, LOCAL_PREFIX "%s", id);
	if (repo_save_applet(i, 0, e, n)) {
		unlink(dest);
		return -1;
	}
	chmod(dest, 0600);
	return 0;
}

/**
 * @brief Handle repo notify catalog.
 */
void repo_notify_catalog(void)
{
	const char *e = getenv("SBBASE_PID_FILE");
	char p[512];
	FILE *f;
	long pid;
	if (!e || !*e)
		e = "/tmp/standalonebase/sbbase.pid";
	snprintf(p, sizeof p, "%s", e);
	f = fopen(p, "r");
	if (!f)
		return;
	if (fscanf(f, "%ld", &pid) == 1 && pid > 1)
#ifndef _WIN32
		kill((pid_t)pid, SIGHUP);
#else
		(void)pid;
#endif
	fclose(f);
}
