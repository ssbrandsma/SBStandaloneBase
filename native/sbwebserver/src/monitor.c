#define _POSIX_C_SOURCE 200809L
#include "monitor.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JSON_CAPACITY (512u * 1024u)
#define MAX_PROCESSES 256
#define CMDLINE_LIMIT 512

typedef struct {
	char *data;
	size_t length;
	size_t capacity;
	int failed;
} Json;

/** Compare a bounded Mongoose string with a C string. */
static int equal(struct mg_str value, const char *expected)
{
	return value.len == strlen(expected) &&
	       !memcmp(value.buf, expected, value.len);
}

/** Append formatted content to the bounded response buffer. */
static int json_add(Json *json, const char *format, ...)
{
	va_list arguments;
	int written;

	if (json->failed || json->length >= json->capacity)
		return -1;
	va_start(arguments, format);
	written = vsnprintf(json->data + json->length,
			    json->capacity - json->length, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= json->capacity - json->length) {
		json->failed = 1;
		return -1;
	}
	json->length += (size_t)written;
	return 0;
}

/** Append a safely escaped JSON string value. */
static int json_string(Json *json, const char *value)
{
	const unsigned char *p = (const unsigned char *)value;

	json_add(json, "\"");
	while (*p && !json->failed) {
		unsigned char c = *p++;

		if (c == '\"' || c == '\\')
			json_add(json, "\\%c", c);
		else if (c >= 0x20 && c < 0x80)
			json_add(json, "%c", c);
		else
			json_add(json, "\\u%04x", c);
	}
	return json_add(json, "\"");
}

/** Return the production or fixture procfs root. */
static const char *proc_root(void)
{
	const char *root = getenv("SB_MONITOR_PROC_ROOT");

	return root && *root ? root : "/proc";
}

/** Construct a bounded path below the selected procfs root. */
static int proc_path(char *out, size_t size, const char *suffix)
{
	int n = snprintf(out, size, "%s/%s", proc_root(), suffix);

	return n > 0 && (size_t)n < size ? 0 : -1;
}

/** Read one trimmed line from a procfs file. */
static int read_first_line(const char *suffix, char *out, size_t size)
{
	char path[PATH_MAX];
	FILE *stream;

	if (proc_path(path, sizeof path, suffix) || !(stream = fopen(path, "r")))
		return -1;
	if (!fgets(out, (int)size, stream)) {
		fclose(stream);
		return -1;
	}
	fclose(stream);
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

/** Append a stable boot identifier without exposing other state. */
static void append_boot(Json *json)
{
	char value[128] = "unknown";
	char line[256];
	FILE *stream;
	char path[PATH_MAX];

	if (read_first_line("sys/kernel/random/boot_id", value, sizeof value)) {
		if (!proc_path(path, sizeof path, "stat") &&
		    (stream = fopen(path, "r"))) {
			while (fgets(line, sizeof line, stream))
				if (sscanf(line, "btime %127s", value) == 1)
					break;
			fclose(stream);
		}
	}
	json_add(json, "\"boot_id\":");
	json_string(json, value);
}

/** Parse and append aggregate cumulative CPU counters. */
static int append_cpu(Json *json)
{
	char line[512];
	unsigned long long v[8] = { 0 };
	int count;

	if (read_first_line("stat", line, sizeof line))
		return -1;
	count = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	if (count < 4)
		return -1;
	json_add(json,
		 "\"cpu\":{\"user\":%llu,\"nice\":%llu,\"system\":%llu,"
		 "\"idle\":%llu,\"iowait\":%llu,\"irq\":%llu,"
		 "\"softirq\":%llu,\"steal\":%llu}",
		 v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
	return 0;
}

/** Parse and append system uptime. */
static int append_uptime(Json *json)
{
	char line[128];
	double uptime;

	if (read_first_line("uptime", line, sizeof line) ||
	    sscanf(line, "%lf", &uptime) != 1)
		return -1;
	json_add(json, "\"uptime_seconds\":%.2f", uptime);
	return 0;
}

/** Parse named memory counters and append values in KiB. */
static int append_memory(Json *json)
{
	char path[PATH_MAX], line[256], key[64];
	unsigned long long value, values[7] = { 0 };
	const char *keys[] = { "MemTotal", "MemFree", "Buffers", "Cached",
			       "SwapTotal", "SwapFree", "Slab" };
	FILE *stream;
	int found = 0;

	if (proc_path(path, sizeof path, "meminfo") || !(stream = fopen(path, "r")))
		return -1;
	while (fgets(line, sizeof line, stream)) {
		if (sscanf(line, "%63[^:]: %llu", key, &value) != 2)
			continue;
		for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
			if (!strcmp(key, keys[i])) {
				values[i] = value;
				found |= 1 << i;
			}
	}
	fclose(stream);
	if ((found & 0x0f) != 0x0f)
		return -1;
	json_add(json,
		 "\"memory_kib\":{\"total\":%llu,\"free\":%llu,"
		 "\"buffers\":%llu,\"cached\":%llu,\"swap_total\":%llu,"
		 "\"swap_free\":%llu%s",
		 values[0], values[1], values[2], values[3], values[4], values[5],
		 found & (1 << 6) ? ",\"slab\":" : "");
	if (found & (1 << 6))
		json_add(json, "%llu", values[6]);
	json_add(json, "}");
	return 0;
}

/** Parse and append cumulative counters for all network interfaces. */
static void append_network(Json *json)
{
	char path[PATH_MAX], line[512], name[64];
	unsigned long long v[16];
	FILE *stream;
	int first = 1;

	json_add(json, "\"network\":[");
	if (!proc_path(path, sizeof path, "net/dev") &&
	    (stream = fopen(path, "r"))) {
		while (fgets(line, sizeof line, stream)) {
			char *colon = strrchr(line, ':');
			char *start = line;
			if (!colon)
				continue;
			while (isspace((unsigned char)*start))
				start++;
			if ((size_t)(colon - start) >= sizeof name)
				continue;
			memcpy(name, start, (size_t)(colon - start));
			name[colon - start] = 0;
			if (sscanf(colon + 1,
				   "%llu %llu %llu %llu %llu %llu %llu %llu "
				   "%llu %llu %llu %llu %llu %llu %llu %llu",
				   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
				   &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14],
				   &v[15]) != 16)
				continue;
			json_add(json, first ? "{" : ",{");
			json_add(json, "\"name\":");
			json_string(json, name);
			json_add(json,
				 ",\"rx_bytes\":%llu,\"rx_packets\":%llu,"
				 "\"rx_errors\":%llu,\"rx_dropped\":%llu,"
				 "\"tx_bytes\":%llu,\"tx_packets\":%llu,"
				 "\"tx_errors\":%llu,\"tx_dropped\":%llu}",
				 v[0], v[1], v[2], v[3], v[8], v[9], v[10], v[11]);
			first = 0;
		}
		fclose(stream);
	}
	json_add(json, "]");
}

/** Decide whether conventional block statistics are meaningful. */
static int meaningful_disk(const char *name)
{
	return strncmp(name, "ram", 3) && strncmp(name, "loop", 4) &&
	       strncmp(name, "mtdblock", 8) && strncmp(name, "ubi", 3);
}

/** Parse supported disk counters or append an unavailable result. */
static void append_disk(Json *json)
{
	char path[PATH_MAX], line[512], name[64];
	unsigned major, minor;
	unsigned long long r, rm, sr, rt, w, wm, sw, wt, active, io_ms, weighted;
	FILE *stream;
	int first = 1;

	json_add(json, "\"disk\":{\"available\":");
	if (!proc_path(path, sizeof path, "diskstats") &&
	    (stream = fopen(path, "r"))) {
		while (fgets(line, sizeof line, stream)) {
			if (sscanf(line,
				   "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu "
				   "%llu %llu %llu",
				   &major, &minor, name, &r, &rm, &sr, &rt, &w, &wm, &sw,
				   &wt, &active, &io_ms, &weighted) != 14 ||
			    !meaningful_disk(name))
				continue;
			if (first)
				json_add(json, "true,\"devices\":[");
			else
				json_add(json, ",");
			json_add(json,
				 "{\"major\":%u,\"minor\":%u,\"name\":", major, minor);
			json_string(json, name);
			json_add(json,
				 ",\"reads\":%llu,\"sectors_read\":%llu,"
				 "\"writes\":%llu,\"sectors_written\":%llu,"
				 "\"io_ms\":%llu}", r, sr, w, sw, io_ms);
			first = 0;
		}
		fclose(stream);
	}
	if (first)
		json_add(json, "false,\"reason\":\"No meaningful block-device statistics\",\"devices\":[]}");
	else
		json_add(json, "]}");
}

/** Parse the required fields from one process stat record. */
static int read_process_stat(const char *pid, char *comm, size_t comm_size,
			     char *state, unsigned *ppid,
			     unsigned long long *utime,
			     unsigned long long *stime,
			     unsigned long long *start,
			     unsigned long long *vsize, long long *rss)
{
	char suffix[300], line[2048], *left, *right, *rest, *save, *token;
	unsigned field = 3;

	snprintf(suffix, sizeof suffix, "%s/stat", pid);
	if (read_first_line(suffix, line, sizeof line) ||
	    !(left = strchr(line, '(')) || !(right = strrchr(left, ')')))
		return -1;
	if ((size_t)(right - left - 1) >= comm_size)
		return -1;
	memcpy(comm, left + 1, (size_t)(right - left - 1));
	comm[right - left - 1] = 0;
	rest = right + 2;
	for (token = strtok_r(rest, " ", &save); token;
	     token = strtok_r(NULL, " ", &save), field++) {
		char *end;
		unsigned long long value;
		if (field == 3) {
			*state = token[0];
			continue;
		}
		errno = 0;
		value = strtoull(token, &end, 10);
		if (errno || *end)
			return -1;
		if (field == 4) *ppid = (unsigned)value;
		else if (field == 14) *utime = value;
		else if (field == 15) *stime = value;
		else if (field == 22) *start = value;
		else if (field == 23) *vsize = value;
		else if (field == 24) { *rss = (long long)value; return 0; }
	}
	return -1;
}

/** Read and normalize a bounded NUL-separated process command line. */
static void read_cmdline(const char *pid, char *out, size_t size)
{
	char suffix[300], path[PATH_MAX];
	FILE *stream;
	size_t length;

	out[0] = 0;
	snprintf(suffix, sizeof suffix, "%s/cmdline", pid);
	if (proc_path(path, sizeof path, suffix) || !(stream = fopen(path, "rb")))
		return;
	length = fread(out, 1, size - 1, stream);
	fclose(stream);
	while (length && out[length - 1] == 0)
		length--;
	for (size_t i = 0; i < length; i++)
		if (!out[i])
			out[i] = ' ';
	out[length] = 0;
}

/** Enumerate and append the bounded process snapshot. */
static unsigned append_processes(Json *json)
{
	DIR *directory = opendir(proc_root());
	struct dirent *entry;
	unsigned count = 0, skipped = 0;
	int first = 1;

	json_add(json, "\"processes\":[");
	if (!directory) {
		json_add(json, "]");
		return 1;
	}
	while ((entry = readdir(directory))) {
		char comm[256], cmd[CMDLINE_LIMIT + 1], state = '?';
		unsigned ppid = 0;
		unsigned long long utime = 0, stime = 0, start = 0, vsize = 0;
		long long rss = 0;
		const char *p = entry->d_name;

		if (!*p || strspn(p, "0123456789") != strlen(p))
			continue;
		if (count >= MAX_PROCESSES) { skipped++; continue; }
		if (read_process_stat(p, comm, sizeof comm, &state, &ppid, &utime,
				      &stime, &start, &vsize, &rss)) {
			skipped++;
			continue;
		}
		read_cmdline(p, cmd, sizeof cmd);
		json_add(json, first ? "{" : ",{");
		json_add(json, "\"pid\":%s,\"ppid\":%u,\"start_ticks\":%llu,\"state\":\"%c\",\"comm\":",
			 p, ppid, start, state);
		json_string(json, comm);
		json_add(json, ",\"cmd\":");
		json_string(json, *cmd ? cmd : comm);
		json_add(json,
			 ",\"user_ticks\":%llu,\"system_ticks\":%llu,"
			 "\"vsize_bytes\":%llu,\"rss_pages\":%lld}",
			 utime, stime, vsize, rss);
		first = 0;
		count++;
	}
	closedir(directory);
	json_add(json, "]");
	return skipped;
}

/** Handle the authenticated, read-only process monitor endpoint. */
int monitor_handle(struct mg_connection *connection,
		   struct mg_http_message *request)
{
	Json json;
	unsigned skipped;
	long ticks, pages;

	if (!equal(request->uri, "/api/monitor/snapshot"))
		return 0;
	if (!equal(request->method, "GET")) {
		mg_http_reply(connection, 405, "Content-Type: application/json\r\nCache-Control: no-store\r\n",
			      "{\"error\":\"Method not allowed\"}");
		return 1;
	}
	json.data = malloc(JSON_CAPACITY);
	if (!json.data) {
		mg_http_reply(connection, 500, "Content-Type: application/json\r\nCache-Control: no-store\r\n",
			      "{\"error\":\"Snapshot allocation failed\"}");
		return 1;
	}
	json.length = 0; json.capacity = JSON_CAPACITY; json.failed = 0;
	ticks = sysconf(_SC_CLK_TCK); pages = sysconf(_SC_PAGESIZE);
	json_add(&json, "{\"schema\":1,"); append_boot(&json); json_add(&json, ",");
	if (append_uptime(&json) || json_add(&json, ",") || append_cpu(&json) ||
	    json_add(&json, ",") || append_memory(&json)) {
		free(json.data);
		mg_http_reply(connection, 503, "Content-Type: application/json\r\nCache-Control: no-store\r\n",
			      "{\"error\":\"Required procfs statistics unavailable\"}");
		return 1;
	}
	json_add(&json, ",\"clock_ticks_per_second\":%ld,\"page_size\":%ld,", ticks, pages);
	append_network(&json); json_add(&json, ","); append_disk(&json); json_add(&json, ",");
	skipped = append_processes(&json);
	json_add(&json, ",\"skipped_processes\":%u}", skipped);
	if (json.failed)
		mg_http_reply(connection, 507, "Content-Type: application/json\r\nCache-Control: no-store\r\n",
			      "{\"error\":\"Snapshot response limit exceeded\"}");
	else
		mg_http_reply(connection, 200, "Content-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n", "%.*s", (int)json.length, json.data);
	free(json.data);
	return 1;
}
