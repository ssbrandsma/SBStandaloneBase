/*
 * Configuration-only backup and restore for sbwebserver. http_server.c supplies
 * browser authentication; this module enforces origin/write/size checks and a
 * fixed four-file allowlist. A short-lived copy of the existing executable
 * applies validated restores while SqueezePlay is stopped. Credentials,
 * firmware, storage layout, binaries and caches are never included or restored.
 */
#define _POSIX_C_SOURCE 200809L
#include "backup.h"
#include "auth.h"
#include "settings.h"
#include "radio_stations.h"
#include "../../sbbase/include/alarms.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_LIMIT (128u * 1024u)
#define FILES 4
#define TARGETS (FILES + 1)
static const char *names[FILES] = {
	"settings", "alarms", "stations", "radioPreferences"};
static int preserve_lock;

/** Return the existing Base configuration root, including internal-flash
 * fallback. */
static const char *config_root(void)
{
	const char *value = getenv("SBWEBSERVER_CONFIG_DIR");
	return value && *value ? value : "/mnt/storage/standalonebase";
}

/** A bounded lock serializes restores and blocks other configuration writes. */
int backup_busy(void)
{
	char path[800];
	snprintf(path, sizeof path, "%s/.backup-restore.lock", config_root());
	return access(path, F_OK) == 0;
}

/** Record completion atomically so the browser reports failures, not just
 * acceptance. */
static void result_file(int ok)
{
	int recorded = 0;
	char path[800], temporary[840];
	snprintf(path, sizeof path, "%s/backup-result.json", config_root());
	snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
	int fd = mkstemp(temporary);
	if (fd >= 0) {
		const char *text = ok ? "{\"ok\":true}" : "{\"ok\":false}";
		if (write(fd, text, strlen(text)) == (ssize_t)strlen(text) &&
		    !fsync(fd)) {
			close(fd);
			recorded = rename(temporary, path) == 0;
		} else
			close(fd);
		unlink(temporary);
	}
	snprintf(path, sizeof path, "%s/.backup-restore.lock", config_root());
	if (recorded && !preserve_lock)
		unlink(path);
}

/** Map a logical archive entry to a fixed target; never accept client paths. */
static int target_path(unsigned index, char *path, size_t capacity)
{
	const char *prefs = getenv("SB_BACKUP_JIVE_SETTINGS");
	int n;
	if (index == 0)
		n = snprintf(path, capacity, "%s/settings.conf", config_root());
	else if (index == 1)
		n = snprintf(path, capacity, "%s/alarms.conf", config_root());
	else if (index == 2)
		n = snprintf(path,
			     capacity,
			     "%s/stations.json",
			     radio_config_root());
	else
		n = snprintf(path,
			     capacity,
			     "%s/%s",
			     prefs && *prefs
				     ? prefs
				     : "/etc/squeezeplay/userpath/settings",
			     index == FILES ? "AlarmSnooze.lua"
					    : "StandaloneRadio.lua");
	return n > 0 && (size_t)n < capacity;
}

/** Read a bounded regular file without following a final-component symlink. */
static char *read_file(const char *path, size_t *length)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    st.st_size > FILE_LIMIT) {
		close(fd);
		errno = EFBIG;
		return NULL;
	}
	char *data = calloc((size_t)st.st_size + 1, 1);
	if (!data) {
		close(fd);
		return NULL;
	}
	size_t used = 0;
	while (used < (size_t)st.st_size) {
		ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
		if (n <= 0) {
			free(data);
			close(fd);
			errno = EIO;
			return NULL;
		}
		used += (size_t)n;
	}
	close(fd);
	*length = used;
	return data;
}

/** Write a mode-0600 staging file exclusively, syncing its contents before use.
 */
static int write_file(const char *path, const char *data, size_t length)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0)
		return 0;
	size_t used = 0;
	int ok = 1;
	while (used < length) {
		ssize_t n = write(fd, data + used, length - used);
		if (n <= 0) {
			ok = 0;
			break;
		}
		used += (size_t)n;
	}
	if (fsync(fd))
		ok = 0;
	if (close(fd))
		ok = 0;
	if (!ok)
		unlink(path);
	return ok;
}

/** Invalidate open station-editor revisions when restored content replaces live
 * data. */
static int station_revision(char **data, size_t *length, const char *target)
{
	struct mg_str incoming = mg_str_n(*data, *length);
	double value = 0;
	mg_json_get_num(incoming, "$.revision", &value);
	unsigned long long revision = (unsigned long long)value;
	size_t old_length = 0;
	char *old = read_file(target, &old_length);
	if (old) {
		value = 0;
		mg_json_get_num(
			mg_str_n(old, old_length), "$.revision", &value);
		if (value > 0 && value <= 4294967295.0 &&
		    (unsigned long long)value > revision)
			revision = (unsigned long long)value;
		free(old);
	}
	if (revision >= 4294967295ULL)
		return 0;
	int token_length = 0,
	    offset = mg_json_get(incoming, "$.revision", &token_length);
	if (offset < 0 || token_length < 1)
		return 0;
	char *updated = mg_mprintf(
		"%.*s%llu%.*s",
		offset,
		*data,
		revision + 1,
		(int)(*length - (size_t)offset - (size_t)token_length),
		*data + offset + token_length);
	if (!updated)
		return 0;
	free(*data);
	*data = updated;
	*length = strlen(updated);
	return 1;
}

typedef struct {
	const char *p, *end;
	unsigned tokens;
} LuaData;

/** Consume whitespace only; comments and executable statements are not data. */
static void space(LuaData *l)
{
	while (l->p < l->end && isspace((unsigned char)*l->p))
		l->p++;
}

/** Validate a serializer-produced quoted Lua string without evaluating it. */
static int lua_string(LuaData *l)
{
	char quote = *l->p++;
	while (l->p < l->end) {
		unsigned char c = (unsigned char)*l->p++;
		if (c == quote)
			return 1;
		if (!c || c < 32)
			return 0;
		if (c == '\\') {
			if (l->p == l->end || !*l->p)
				return 0;
			if (isdigit((unsigned char)*l->p)) {
				unsigned value = 0, count = 0;
				while (l->p < l->end &&
				       isdigit((unsigned char)*l->p) &&
				       count++ < 3)
					value = value * 10 +
						(unsigned)(*l->p++ - '0');
				if (!value || value > 255)
					return 0;
			} else {
				if (!strchr("abfnrtv\\\"'\n", *l->p))
					return 0;
				l->p++;
			}
		}
	}
	return 0;
}

static int lua_value(LuaData *l, unsigned depth);

/** Validate data-only table fields, including sparse numeric preset keys. */
static int lua_table(LuaData *l, unsigned depth)
{
	l->p++;
	space(l);
	while (l->p < l->end && *l->p != '}') {
		if (*l->p == '[') {
			l->p++;
			space(l);
			if (l->p == l->end || (!isdigit((unsigned char)*l->p) &&
					       *l->p != '"' && *l->p != '\''))
				return 0;
			if (!lua_value(l, depth + 1))
				return 0;
			space(l);
			if (l->p == l->end || *l->p++ != ']')
				return 0;
			space(l);
			if (l->p == l->end || *l->p++ != '=')
				return 0;
		} else if (isalpha((unsigned char)*l->p) || *l->p == '_') {
			const char *saved = l->p;
			while (l->p < l->end &&
			       (isalnum((unsigned char)*l->p) || *l->p == '_'))
				l->p++;
			size_t wordlen = (size_t)(l->p - saved);
			space(l);
			if (l->p < l->end && *l->p == '=') {
				const char *reserved[] = {
					"and",	    "break", "do",    "else",
					"elseif",   "end",   "false", "for",
					"function", "if",    "in",    "local",
					"nil",	    "not",   "or",    "repeat",
					"return",   "then",  "true",  "until",
					"while"};
				for (unsigned n = 0;
				     n < sizeof reserved / sizeof *reserved;
				     n++)
					if (strlen(reserved[n]) == wordlen &&
					    !memcmp(saved,
						    reserved[n],
						    wordlen))
						return 0;
				l->p++;
			} else
				l->p = saved;
		}
		space(l);
		if (!lua_value(l, depth + 1))
			return 0;
		space(l);
		if (l->p < l->end && (*l->p == ',' || *l->p == ';')) {
			l->p++;
			space(l);
		} else if (l->p == l->end || *l->p != '}')
			return 0;
	}
	return l->p < l->end && *l->p++ == '}';
}

/** Accept tables, quoted strings, finite decimal numbers and boolean/nil values
 * only. */
static int lua_value(LuaData *l, unsigned depth)
{
	space(l);
	if (depth > 16 || ++l->tokens > 16000 || l->p == l->end)
		return 0;
	if (*l->p == '{')
		return lua_table(l, depth);
	if (*l->p == '"' || *l->p == '\'')
		return lua_string(l);
	for (unsigned i = 0; i < 3; i++) {
		const char *word = i == 0 ? "true" : i == 1 ? "false" : "nil";
		size_t n = strlen(word);
		if ((size_t)(l->end - l->p) >= n && !memcmp(l->p, word, n)) {
			l->p += n;
			return 1;
		}
	}
	if (*l->p == '-' || isdigit((unsigned char)*l->p)) {
		char *end;
		double n = strtod(l->p, &end);
		if (end == l->p || end > l->end || !isfinite(n))
			return 0;
		/* Lua 5.1 decimal literals only, not C hex/NaN or an
		 * expression. */
		for (const char *p = l->p; p < end; p++)
			if (!strchr("0123456789.eE+-", *p))
				return 0;
		l->p = end;
		return 1;
	}
	return 0;
}

/** Refuse executable Lua: only `settings = {serialized data}` may be restored.
 */
static int radio_preferences_valid(const char *data, size_t length)
{
	if (memchr(data, 0, length))
		return 0;
	LuaData l = {data, data + length, 0};
	space(&l);
	if ((size_t)(l.end - l.p) < 8 || memcmp(l.p, "settings", 8))
		return 0;
	l.p += 8;
	space(&l);
	if (l.p == l.end || *l.p++ != '=')
		return 0;
	space(&l);
	if (l.p == l.end || *l.p != '{' || !lua_value(&l, 0))
		return 0;
	space(&l);
	return l.p == l.end;
}

/** Apply the upload bound at headers, before Mongoose buffers the body. */
int backup_headers(struct mg_connection *c, struct mg_http_message *m)
{
	if (!mg_match(m->uri, mg_str("/api/backup/*"), NULL) ||
	    !mg_match(m->method, mg_str("POST"), NULL))
		return 0;
	int status = 0;
	struct mg_str *length = mg_http_get_header(m, "Content-Length");
	if (!auth_session_ok(m, NULL))
		status = 401;
	else if (!length || mg_http_get_header(m, "Transfer-Encoding"))
		status = 411;
	else {
		char text[32];
		if (length->len >= sizeof text)
			status = 413;
		else {
			memcpy(text, length->buf, length->len);
			text[length->len] = 0;
			if (!*text || strspn(text, "0123456789") != length->len)
				status = 400;
			else if (strtoull(text, NULL, 10) > SB_BACKUP_LIMIT)
				status = 413;
		}
	}
	if (!status)
		return 0;
	mg_http_reply(c,
		      status,
		      "Content-Type: application/json\r\nCache-Control: "
		      "no-store\r\nConnection: close\r\n",
		      "{\"error\":\"Backup upload unauthorized, oversized or "
		      "missing length\"}");
	c->is_draining = 1;
	return 1;
}

/** SHA-256 detects accidental archive damage; it is not an authenticity
 * signature. */
static void digest(const char *data, size_t length, char out[65])
{
	unsigned char bytes[32];
	mg_sha256(bytes, (uint8_t *)data, length);
	for (unsigned i = 0; i < 32; i++)
		snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

/** Return a small uncached JSON result; no paths or configuration data in
 * errors. */
static void respond(struct mg_connection *c, int status, const char *body)
{
	mg_http_reply(c,
		      status,
		      "Content-Type: application/json\r\nCache-Control: "
		      "no-store\r\nX-Content-Type-Options: "
		      "nosniff\r\nReferrer-Policy: no-referrer\r\n",
		      "%s",
		      body);
}

/** Generate a versioned fixed-entry backup without walking directories. */
static void download(struct mg_connection *c)
{
	struct mg_iobuf out = {0};
	const char *head = "{\"format\":\"StandaloneBaseBackup\",\"version\":1,"
			   "\"files\":{";
	if (mg_iobuf_add(&out, out.len, head, strlen(head)) != strlen(head))
		goto failed;
	int entries = 0;
	for (unsigned i = 0; i < FILES; i++) {
		char path[700], hash[65];
		size_t length = 0;
		if (!target_path(i, path, sizeof path))
			goto failed;
		errno = 0;
		char *data = read_file(path, &length);
		if (!data) {
			if (errno == ENOENT)
				continue;
			goto failed;
		}
		char *encoded = malloc((length + 2) / 3 * 4 + 1);
		if (!encoded) {
			free(data);
			goto failed;
		}
		mg_base64_encode((unsigned char *)data,
				 length,
				 encoded,
				 (length + 2) / 3 * 4 + 1);
		digest(data, length, hash);
		char *entry = mg_mprintf(
			"%s\"%s\":{\"sha256\":\"%s\",\"data\":\"%s\"}",
			entries++ ? "," : "",
			names[i],
			hash,
			encoded);
		free(data);
		free(encoded);
		if (!entry)
			goto failed;
		size_t n = strlen(entry);
		int ok = mg_iobuf_add(&out, out.len, entry, n) == n;
		free(entry);
		if (!ok || out.len > SB_BACKUP_LIMIT - 3)
			goto failed;
	}
	if (!entries) {
		mg_iobuf_free(&out);
		respond(c,
			404,
			"{\"error\":\"No saved configuration exists yet\"}");
		return;
	}
	if (mg_iobuf_add(&out, out.len, "}}", 2) != 2)
		goto failed;
	mg_http_reply(c,
		      200,
		      "Content-Type: application/json\r\nContent-Disposition: "
		      "attachment; "
		      "filename=standalone-base-backup.json\r\nCache-Control: "
		      "no-store\r\nX-Content-Type-Options: nosniff\r\n",
		      "%.*s",
		      (int)out.len,
		      out.buf);
	mg_iobuf_free(&out);
	return;
failed:
	mg_iobuf_free(&out);
	respond(c,
		500,
		"{\"error\":\"Configuration could not be backed up "
		"completely\"}");
}

/** Remove only known files from a validated private staging directory. */
static void cleanup(const char *directory)
{
	char path[800];
	for (unsigned i = 0; i < FILES; i++) {
		snprintf(path, sizeof path, "%s/%s", directory, names[i]);
		unlink(path);
	}
	snprintf(path, sizeof path, "%s/settings.conf", directory);
	unlink(path);
	rmdir(directory);
}

/** Reject duplicate, escaped or unknown structural keys to avoid parser
 * ambiguity. */
static int
exact_fields(struct mg_str object, const char **allowed, unsigned count)
{
	struct mg_str key, value;
	size_t offset = 0;
	unsigned seen = 0;
	if (object.len < 2 || object.buf[0] != '{')
		return 0;
	while ((offset = mg_json_next(object, offset, &key, &value)) > 0) {
		if (key.len < 2 || key.buf[0] != '"' ||
		    key.buf[key.len - 1] != '"')
			return 0;
		key.buf++;
		key.len -= 2;
		int index = -1;
		for (unsigned i = 0; i < count; i++)
			if (key.len == strlen(allowed[i]) &&
			    !memcmp(key.buf, allowed[i], key.len))
				index = (int)i;
		if (index < 0 || (seen & (1u << index)))
			return 0;
		seen |= 1u << index;
	}
	return seen == ((1u << count) - 1);
}

/** Validate the entire bundle before any live file is touched. */
static int stage_bundle(struct mg_str body, char directory[700])
{
	while (body.len && isspace((unsigned char)*body.buf)) {
		body.buf++;
		body.len--;
	}
	while (body.len && isspace((unsigned char)body.buf[body.len - 1]))
		body.len--;
	int root_length = 0;
	if (mg_json_get(body, "$", &root_length) != 0 || root_length < 2 ||
	    (size_t)root_length != body.len)
		return 0;
	const char *root_keys[] = {"format", "version", "files"};
	if (!exact_fields(body, root_keys, 3))
		return 0;
	char *format = mg_json_get_str(body, "$.format");
	double version = 0;
	int valid = format && !strcmp(format, "StandaloneBaseBackup") &&
		    mg_json_get_num(body, "$.version", &version) &&
		    version == 1;
	free(format);
	if (!valid)
		return 0;
	struct mg_str files = mg_json_get_tok(body, "$.files");
	if (files.len < 2 || files.buf[0] != '{')
		return 0;
	struct mg_str key, value;
	size_t offset = 0;
	unsigned seen = 0;
	while ((offset = mg_json_next(files, offset, &key, &value)) > 0) {
		const char *entry_keys[] = {"sha256", "data"};
		if (!exact_fields(value, entry_keys, 2))
			return 0;
		if (key.len < 2 || key.buf[0] != '"' ||
		    key.buf[key.len - 1] != '"')
			return 0;
		key.buf++;
		key.len -= 2;
		int match = -1;
		for (unsigned i = 0; i < FILES; i++)
			if (key.len == strlen(names[i]) &&
			    !memcmp(key.buf, names[i], key.len))
				match = (int)i;
		if (match < 0 || (seen & (1u << match)))
			return 0;
		seen |= 1u << match;
	}
	if (!seen)
		return 0;
	int n = snprintf(directory, 700, "%s/restore.XXXXXX", config_root());
	if (n < 0 || n >= 700 || !mkdtemp(directory))
		return 0;
	for (unsigned i = 0; i < FILES; i++)
		if (seen & (1u << i)) {
			char selector[100], path[800], actual[65];
			int length = 0;
			snprintf(selector,
				 sizeof selector,
				 "$.files.%s.data",
				 names[i]);
			char *data = mg_json_get_b64(body, selector, &length);
			snprintf(selector,
				 sizeof selector,
				 "$.files.%s.sha256",
				 names[i]);
			char *hash = mg_json_get_str(body, selector);
			valid = data && length > 0 &&
				(unsigned)length <= FILE_LIMIT && hash &&
				strlen(hash) == 64;
			if (valid) {
				digest(data, (size_t)length, actual);
				valid = !strcmp(hash, actual);
			}
			if (valid && i == 2)
				valid = radio_validate_backup(
					mg_str_n(data, (size_t)length));
			if (valid && i == 3)
				valid = radio_preferences_valid(data,
								(size_t)length);
			snprintf(path,
				 sizeof path,
				 "%s/%s",
				 directory,
				 names[i]);
			if (valid)
				valid = write_file(path, data, (size_t)length);
			free(data);
			free(hash);
			if (valid && i == 1)
				valid = sb_alarms_prepare_restore(path);
			if (!valid) {
				cleanup(directory);
				directory[0] = 0;
				return 0;
			}
		}
	return 1;
}

/** Run the firmware's own SqueezePlay service, without a shell or
 * substitutions. */
static int service(const char *action)
{
	pid_t child = fork();
	if (child < 0)
		return 0;
	if (!child) {
		execl("/etc/init.d/squeezeplay",
		      "squeezeplay",
		      action,
		      (char *)NULL);
		_exit(127);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR)
			return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Apply staged files while Jive is stopped; roll back ordinary write failures.
 * directory is created by stage_bundle; restart_jive is used only on the Radio.
 */
int backup_apply(const char *directory, int restart_jive)
{
	char paths[TARGETS][700], temps[TARGETS][740], old[TARGETS][740];
	unsigned staged = 0, committed = 0, original = 0;
	int ok = 0;
	size_t rootlen = strlen(config_root());
	if (strncmp(directory, config_root(), rootlen) ||
	    strncmp(directory + rootlen, "/restore.", 9) ||
	    strchr(directory + rootlen + 1, '/'))
		return 1;
	if (restart_jive) {
		sleep(2);
		if (!service("stop")) {
			service("start");
			result_file(0);
			return 1;
		}
	}
	for (unsigned i = 0; i < TARGETS; i++) {
		char source[800];
		size_t length = 0;
		errno = 0;
		char *data;
		if (i == FILES) {
			if (!(staged & 2u))
				continue;
			/* Clear the RTC cache before Jive starts: an old cached
			 * occurrence must not fire before the first disabled
			 * status. */
			data = strdup("settings = "
				      "{alarmNext=false,alarmRepeat=false,"
				      "alarmDays=false}\n");
			if (data)
				length = strlen(data);
		} else {
			snprintf(source,
				 sizeof source,
				 "%s/%s",
				 directory,
				 names[i]);
			data = read_file(source, &length);
		}
		if (!data) {
			if (errno == ENOENT)
				continue;
			goto done;
		}
		if (!target_path(i, paths[i], sizeof paths[i])) {
			free(data);
			goto done;
		}
		if (i == 2 && !station_revision(&data, &length, paths[i])) {
			free(data);
			goto done;
		}
		/* A missing station directory can be created, but never a
		 * volume/mount. */
		if (i == 2) {
			char parent[700];
			snprintf(parent,
				 sizeof parent,
				 "%s",
				 radio_config_root());
			if (mkdir(parent, 0700) && errno != EEXIST) {
				free(data);
				goto done;
			}
		}
		snprintf(temps[i], sizeof temps[i], "%s.restore-new", paths[i]);
		snprintf(old[i], sizeof old[i], "%s.restore-old", paths[i]);
		struct stat st;
		if (!lstat(paths[i], &st)) {
			if (!S_ISREG(st.st_mode)) {
				free(data);
				goto done;
			}
			original |= 1u << i;
		} else if (errno != ENOENT) {
			free(data);
			goto done;
		}
		if (!lstat(old[i], &st)) {
			free(data);
			goto done;
		}
		if (!write_file(temps[i], data, length)) {
			free(data);
			goto done;
		}
		free(data);
		staged |= 1u << i;
	}
	/* Preserve listener/read-only policy; restore only user-facing
	 * preferences. */
	if (staged & 1) {
		Settings current, incoming;
		settings_load(&current);
		char source[800];
		snprintf(source, sizeof source, "%s/settings.conf", directory);
		if (rename(temps[0], source))
			goto done;
		setenv("SBWEBSERVER_CONFIG_DIR", directory, 1);
		settings_load(&incoming);
		snprintf(
			current.name, sizeof current.name, "%s", incoming.name);
		current.artwork_external_fallback_enabled =
			incoming.artwork_external_fallback_enabled;
		current.artwork_external_fallback_timeout =
			incoming.artwork_external_fallback_timeout;
		current.artwork_external_fallback_max_bytes =
			incoming.artwork_external_fallback_max_bytes;
		snprintf(current.artwork_external_fallback_base_url,
			 sizeof current.artwork_external_fallback_base_url,
			 "%s",
			 incoming.artwork_external_fallback_base_url);
		int saved = settings_save(&current) == 0;
		char parent[700];
		snprintf(parent, sizeof parent, "%s", paths[0]);
		*strrchr(parent, '/') = 0;
		setenv("SBWEBSERVER_CONFIG_DIR", parent, 1);
		if (!saved || rename(source, temps[0]))
			goto done;
	}
	for (unsigned i = 0; i < TARGETS; i++)
		if (staged & (1u << i)) {
			if ((original & (1u << i)) && rename(paths[i], old[i]))
				goto done;
			if (rename(temps[i], paths[i])) {
				if ((original & (1u << i)) &&
				    rename(old[i], paths[i]))
					preserve_lock = 1;
				goto done;
			}
			committed |= 1u << i;
		}
	ok = 1;
done:
	for (unsigned i = 0; i < TARGETS; i++)
		if (staged & (1u << i)) {
			if (!ok && (committed & (1u << i))) {
				if (unlink(paths[i]) && errno != ENOENT)
					preserve_lock = 1;
				if ((original & (1u << i)) &&
				    rename(old[i], paths[i]))
					preserve_lock = 1;
			}
			unlink(temps[i]);
			if (ok)
				unlink(old[i]);
		}
	if (ok)
		cleanup(directory);
	fprintf(stderr,
		"Standalone Base restore %s\n",
		ok ? "completed" : "failed; rollback attempted");
	if (preserve_lock)
		fputs("Restore rollback incomplete; configuration writes "
		      "remain locked for recovery\n",
		      stderr);
	if (restart_jive) {
		/* Only the applet's own sbbase is stopped; never an arbitrary
		 * PID. */
		FILE *f = fopen("/tmp/standalonebase/sbbase.pid", "r");
		long pid = 0;
		if (f) {
			if (fscanf(f, "%ld", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		if (pid > 1) {
			char proc[80], cmd[300] = {0};
			snprintf(proc, sizeof proc, "/proc/%ld/cmdline", pid);
			f = fopen(proc, "r");
			if (f) {
				size_t n = fread(cmd, 1, sizeof cmd - 1, f);
				fclose(f);
				if (n && !strcmp(cmd,
						 "/usr/share/jive/applets/"
						 "StandaloneBase/sbbase"))
					kill((pid_t)pid, SIGTERM);
			}
		}
		if (!service("start")) {
			result_file(0);
			return 1;
		}
	}
	result_file(ok);
	return ok ? 0 : 1;
}

/** Start a short-lived apply helper, returning before the HTTP response drains.
 */
static int launch(const char *directory)
{
	/* Resolve limits before fork: the webserver can have worker threads.
	 * The child must use only async-signal-safe operations until exec. */
	long maximum = sysconf(_SC_OPEN_MAX);
	if (maximum < 0 || maximum > 4096)
		maximum = 4096;
	pid_t first = fork();
	if (first < 0)
		return 0;
	if (!first) {
		pid_t second = fork();
		if (second < 0)
			_exit(1);
		if (second > 0)
			_exit(0);
		int fd = open("/tmp/standalonebase/backup-restore.log",
			      O_WRONLY | O_CREAT | O_APPEND,
			      0600);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) {
			dup2(fd, 0);
			if (fd > 2)
				close(fd);
		}
		for (int inherited = 3; inherited < maximum; inherited++)
			close(inherited);
		execl("/proc/self/exe",
		      "sbwebserver",
		      "--apply-backup",
		      directory,
		      "--restart-jive",
		      (char *)NULL);
		static const char failure[] =
			"Cannot execute restore helper; pending lock retained "
			"for recovery\n";
		(void)write(2, failure, sizeof failure - 1);
		_exit(127);
	}
	int status;
	while (waitpid(first, &status, 0) < 0)
		if (errno != EINTR)
			return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Dispatch paired-browser backup download, validation preview and confirmed
 * restore. */
int backup_handle(struct mg_connection *c,
		  struct mg_http_message *m,
		  int readonly)
{
	if (mg_match(m->uri, mg_str("/api/backup/status"), NULL) &&
	    mg_match(m->method, mg_str("GET"), NULL)) {
		char path[800];
		size_t length = 0;
		snprintf(path,
			 sizeof path,
			 "%s/backup-result.json",
			 config_root());
		char *result = read_file(path, &length);
		char body[200];
		snprintf(body,
			 sizeof body,
			 "{\"busy\":%s,\"result\":%s}",
			 backup_busy() ? "true" : "false",
			 result && length < 64 ? result : "null");
		free(result);
		respond(c, 200, body);
		return 1;
	}
	if (mg_match(m->uri, mg_str("/api/backup"), NULL) &&
	    mg_match(m->method, mg_str("GET"), NULL)) {
		if (backup_busy())
			respond(c,
				423,
				"{\"error\":\"Restore is in progress\"}");
		else
			download(c);
		return 1;
	}
	int validate = mg_match(m->uri, mg_str("/api/backup/validate"), NULL);
	int restore = mg_match(m->uri, mg_str("/api/backup/restore"), NULL);
	if (!validate && !restore)
		return 0;
	if (!mg_match(m->method, mg_str("POST"), NULL)) {
		respond(c, 405, "{\"error\":\"POST required\"}");
		return 1;
	}
	if (!auth_origin_ok(m) || readonly) {
		respond(c,
			403,
			"{\"error\":\"Restore is not authorized in this "
			"mode/origin\"}");
		return 1;
	}
	struct mg_str *ct = mg_http_get_header(m, "Content-Type");
	if (!ct || ct->len < 16 || memcmp(ct->buf, "application/json", 16) ||
	    (ct->len > 16 && ct->buf[16] != ';')) {
		respond(c, 415, "{\"error\":\"JSON backup required\"}");
		return 1;
	}
	if (m->body.len > SB_BACKUP_LIMIT) {
		respond(c, 413, "{\"error\":\"Backup exceeds 768 KiB\"}");
		return 1;
	}
	char directory[700] = {0};
	if (!stage_bundle(m->body, directory)) {
		respond(c,
			400,
			"{\"error\":\"Invalid, damaged or unsupported "
			"backup\"}");
		return 1;
	}
	if (validate) {
		cleanup(directory);
		respond(c,
			200,
			"{\"valid\":true,\"alarmsWillBeDisabled\":true,"
			"\"restartRequired\":true}");
		return 1;
	}
	char lock[800];
	snprintf(lock, sizeof lock, "%s/.backup-restore.lock", config_root());
	if (!write_file(lock, directory, strlen(directory))) {
		cleanup(directory);
		respond(c,
			409,
			"{\"error\":\"Another restore is pending; inspect "
			"recovery log if it was interrupted\"}");
		return 1;
	}
	char result[800];
	snprintf(result, sizeof result, "%s/backup-result.json", config_root());
	unlink(result);
	if (access("/etc/init.d/squeezeplay", X_OK) || !launch(directory)) {
		unlink(lock);
		cleanup(directory);
		respond(c,
			503,
			"{\"error\":\"Radio restart service unavailable\"}");
		return 1;
	}
	respond(c, 202, "{\"accepted\":true,\"restarting\":true}");
	return 1;
}
