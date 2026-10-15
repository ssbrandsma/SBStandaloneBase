#define _POSIX_C_SOURCE 200809L
#include "logs.h"

#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/klog.h>
#endif

#define DEFAULT_INITIAL_LINES 200
#define MAX_INITIAL_LINES 1000
#define MAX_RESPONSE_LINES 500
#define FILE_READ_LIMIT (64u * 1024u)
#define INITIAL_READ_LIMIT (128u * 1024u)
#define DMESG_READ_LIMIT (256u * 1024u)
#define JSON_LIMIT (512u * 1024u)
#define OVERLAP_LINES 8

typedef struct {
	char *data;
	size_t length;
	size_t capacity;
	int failed;
} Json;

typedef struct {
	char *text;
	uint64_t hash;
} KernelLine;

/**
 * @brief Check whether a URI addresses the system-log API.
 * @param uri Request URI to inspect.
 * @return Nonzero for a supported log endpoint; otherwise zero.
 */
int logs_is_api(struct mg_str uri)
{
	static const char prefix[] = "/api/logs/";

	return uri.len >= sizeof(prefix) - 1 &&
	       !memcmp(uri.buf, prefix, sizeof(prefix) - 1);
}

/**
 * @brief Compare a bounded Mongoose string with a C string.
 * @param value Bounded string to compare.
 * @param expected Null-terminated expected value.
 * @return Nonzero when both strings are equal; otherwise zero.
 */
static int equal(struct mg_str value, const char *expected)
{
	return value.len == strlen(expected) &&
	       !memcmp(value.buf, expected, value.len);
}

/**
 * @brief Append formatted text to a bounded JSON buffer.
 * @param json JSON builder receiving the text.
 * @param format printf-compatible format string.
 * @return Zero on success; otherwise -1.
 */
static int json_add(Json *json, const char *format, ...)
{
	va_list arguments;
	int written;

	if (json->failed || json->length >= json->capacity)
		return -1;
	va_start(arguments, format);
	written = vsnprintf(json->data + json->length,
			    json->capacity - json->length,
			    format,
			    arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= json->capacity - json->length) {
		json->failed = 1;
		return -1;
	}
	json->length += (size_t)written;
	return 0;
}

/**
 * @brief Append a JSON-escaped string to a bounded JSON buffer.
 * @param json JSON builder receiving the string.
 * @param value String to escape and append.
 * @return Zero on success; otherwise -1.
 */
static int json_string(Json *json, const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;

	if (json_add(json, "\""))
		return -1;
	while (*cursor && !json->failed) {
		unsigned char byte = *cursor++;

		if (byte == '"' || byte == '\\')
			json_add(json, "\\%c", byte);
		else if (byte == '\n')
			json_add(json, "\\n");
		else if (byte == '\r')
			json_add(json, "\\r");
		else if (byte == '\t')
			json_add(json, "\\t");
		else if (byte < 0x20)
			json_add(json, "\\u%04x", byte);
		else
			json_add(json, "%c", byte);
	}
	return json_add(json, "\"");
}

/**
 * @brief Read the first line from a small virtual file.
 * @param path Filesystem path to read.
 * @param output Destination receiving the trimmed line.
 * @param capacity Capacity of the destination buffer.
 * @return Zero on success; otherwise -1.
 */
static int first_line(const char *path, char *output, size_t capacity)
{
	FILE *stream;
	size_t length;

	stream = fopen(path, "r");
	if (!stream)
		return -1;
	if (!fgets(output, (int)capacity, stream)) {
		fclose(stream);
		return -1;
	}
	fclose(stream);
	length = strlen(output);
	while (length &&
	       (output[length - 1] == '\n' || output[length - 1] == '\r'))
		output[--length] = 0;
	return length ? 0 : -1;
}

/**
 * @brief Determine a stable identifier for the current system boot.
 * @param output Destination receiving the boot identifier.
 * @param capacity Capacity of the destination buffer.
 */
static void boot_id(char *output, size_t capacity)
{
	const char *override = getenv("SB_LOG_BOOT_ID");
	const char *override_path = getenv("SB_LOG_BOOT_ID_PATH");
	FILE *stream;
	char line[256];

	if (override && *override) {
		snprintf(output, capacity, "%s", override);
		return;
	}
	if (override_path && *override_path &&
	    !first_line(override_path, output, capacity))
		return;
	if (!first_line("/proc/sys/kernel/random/boot_id", output, capacity))
		return;
	stream = fopen("/proc/stat", "r");
	if (stream) {
		while (fgets(line, sizeof line, stream)) {
			unsigned long value;

			if (sscanf(line, "btime %lu", &value) == 1) {
				snprintf(output, capacity, "btime-%lu", value);
				fclose(stream);
				return;
			}
		}
		fclose(stream);
	}
	snprintf(output, capacity, "boot-unknown");
}

/**
 * @brief Parse and clamp the requested initial line count.
 * @param request HTTP request containing the query string.
 * @return Requested count within the supported bounds.
 */
static int initial_lines(struct mg_http_message *request)
{
	char value[24];
	long result;
	char *end;

	if (mg_http_get_var(&request->query, "lines", value, sizeof value) <= 0)
		return DEFAULT_INITIAL_LINES;
	errno = 0;
	result = strtol(value, &end, 10);
	if (errno || *end || result < 1)
		return DEFAULT_INITIAL_LINES;
	if (result > MAX_INITIAL_LINES)
		result = MAX_INITIAL_LINES;
	return (int)result;
}

/**
 * @brief Read an optional query parameter.
 * @param request HTTP request containing the query string.
 * @param name Query parameter name.
 * @param output Destination receiving the decoded value.
 * @param capacity Capacity of the destination buffer.
 * @return Nonzero when the parameter was present; otherwise zero.
 */
static int query(struct mg_http_message *request,
		 const char *name,
		 char *output,
		 size_t capacity)
{
	int length = mg_http_get_var(&request->query, name, output, capacity);

	if (length <= 0) {
		output[0] = 0;
		return 0;
	}
	return 1;
}

/**
 * @brief Send a completed log API response.
 * @param connection Mongoose connection handling the request.
 * @param json JSON response builder.
 */
static void send_json(struct mg_connection *connection, Json *json)
{
	if (json->failed) {
		mg_http_reply(
			connection,
			500,
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n",
			"{\"error\":\"Log response exceeded its limit\"}");
		return;
	}
	mg_http_reply(connection,
		      200,
		      "Content-Type: application/json\r\n"
		      "Cache-Control: no-store\r\n"
		      "X-Content-Type-Options: nosniff\r\n"
		      "Referrer-Policy: no-referrer\r\n",
		      "%s",
		      json->data);
}

/**
 * @brief Begin a standard log API JSON document.
 * @param json JSON builder receiving the document.
 * @param boot Current boot identifier.
 * @param source Log source name.
 * @param reset Whether the client cursor was reset.
 * @param reason Reset reason, or NULL when no reset occurred.
 * @param available Whether the requested source is readable.
 */
static void begin_response(Json *json,
			   const char *boot,
			   const char *source,
			   int reset,
			   const char *reason,
			   int available)
{
	json_add(json, "{\"boot_id\":");
	json_string(json, boot);
	json_add(json, ",\"source\":");
	json_string(json, source);
	json_add(json,
		 ",\"reset\":%s,\"reset_reason\":",
		 reset ? "true" : "false");
	if (reason)
		json_string(json, reason);
	else
		json_add(json, "null");
	json_add(json, ",\"available\":%s", available ? "true" : "false");
}

/**
 * @brief Append complete newline-terminated file records to a response.
 * @param json JSON builder receiving line strings.
 * @param data File bytes beginning at the requested cursor.
 * @param length Number of bytes available in data.
 * @param base_offset File offset corresponding to data[0].
 * @param line_limit Maximum number of records to append.
 * @param next_offset Destination receiving the next safe byte offset.
 * @return Number of lines appended.
 */
static int append_file_lines(Json *json,
			     char *data,
			     size_t length,
			     uint64_t base_offset,
			     int line_limit,
			     uint64_t *next_offset)
{
	size_t start = 0;
	int count = 0;

	json_add(json, ",\"lines\":[");
	for (size_t index = 0; index < length && count < line_limit; index++) {
		if (data[index] != '\n')
			continue;
		data[index] = 0;
		if (index > start && data[index - 1] == '\r')
			data[index - 1] = 0;
		if (count)
			json_add(json, ",");
		json_string(json, data + start);
		count++;
		start = index + 1;
		*next_offset = base_offset + start;
	}
	json_add(json, "]");
	return count;
}

/**
 * @brief Parse a system-log file cursor.
 * @param cursor Encoded cursor supplied by the browser.
 * @param device Destination receiving the filesystem device identity.
 * @param inode Destination receiving the inode identity.
 * @param offset Destination receiving the byte offset.
 * @return Zero on success; otherwise -1.
 */
static int parse_file_cursor(const char *cursor,
			     uint64_t *device,
			     uint64_t *inode,
			     uint64_t *offset)
{
	unsigned long long parsed_device, parsed_inode, parsed_offset;
	char extra;
	int fields;

	fields = sscanf(cursor,
			"m:%llx:%llx:%llu%c",
			&parsed_device,
			&parsed_inode,
			&parsed_offset,
			&extra);
	if (fields != 3)
		return -1;
	*device = (uint64_t)parsed_device;
	*inode = (uint64_t)parsed_inode;
	*offset = (uint64_t)parsed_offset;
	return 0;
}

/**
 * @brief Serve incremental records from /var/log/messages.
 * @param connection Mongoose connection handling the request.
 * @param request HTTP request containing cursor state.
 * @param boot Current boot identifier.
 */
static void serve_messages(struct mg_connection *connection,
			   struct mg_http_message *request,
			   const char *boot)
{
	const char *path = getenv("SB_LOG_MESSAGES_PATH");
	char cursor[256], previous_boot[128];
	char *data = NULL, *response = NULL;
	struct stat status;
	FILE *stream = NULL;
	Json json;
	uint64_t device = 0, inode = 0, offset = 0, next = 0;
	uint64_t current_device, current_inode, size;
	const char *reason = NULL;
	size_t read_limit, received = 0, start_in_buffer = 0;
	int reset = 0, has_cursor, more, requested = initial_lines(request);

	if (!path || !*path)
		path = "/var/log/messages";
	query(request, "boot_id", previous_boot, sizeof previous_boot);
	has_cursor = query(request, "cursor", cursor, sizeof cursor);
	response = malloc(JSON_LIMIT);
	if (!response)
		return;
	json = (Json){response, 0, JSON_LIMIT, 0};
	if (stat(path, &status) || !S_ISREG(status.st_mode)) {
		begin_response(&json, boot, "messages", 0, NULL, 0);
		json_add(&json,
			 ",\"next_cursor\":null,\"more\":false,\"lines\":[]}");
		send_json(connection, &json);
		free(response);
		return;
	}
	current_device = (uint64_t)status.st_dev;
	current_inode = (uint64_t)status.st_ino;
	size = (uint64_t)status.st_size;
	if (previous_boot[0] && strcmp(previous_boot, boot)) {
		reset = 1;
		reason = "reboot";
		has_cursor = 0;
		offset = 0;
	} else if (has_cursor &&
		   parse_file_cursor(cursor, &device, &inode, &offset)) {
		reset = 1;
		reason = "invalid_cursor";
		has_cursor = 0;
		offset = 0;
	} else if (has_cursor &&
		   (device != current_device || inode != current_inode)) {
		reset = 1;
		reason = "file_replaced";
		has_cursor = 0;
		offset = 0;
	} else if (has_cursor && offset > size) {
		reset = 1;
		reason = "file_truncated";
		has_cursor = 0;
		offset = 0;
	}
	stream = fopen(path, "rb");
	if (!stream)
		goto unavailable;
	if (!has_cursor && !reset) {
		uint64_t start = size > INITIAL_READ_LIMIT
					 ? size - INITIAL_READ_LIMIT
					 : 0;
		int seen = 0;

		read_limit = (size_t)(size - start);
		data = malloc(read_limit + 1);
		if (!data || fseeko(stream, (off_t)start, SEEK_SET))
			goto unavailable;
		received = fread(data, 1, read_limit, stream);
		data[received] = 0;
		if (start && received) {
			char *newline = memchr(data, '\n', received);
			if (newline)
				start_in_buffer = (size_t)(newline - data) + 1;
		}
		for (size_t index = received; index > start_in_buffer; index--)
			if (data[index - 1] == '\n' && ++seen > requested) {
				start_in_buffer = index;
				break;
			}
		offset = start + start_in_buffer;
		memmove(data,
			data + start_in_buffer,
			received - start_in_buffer);
		received -= start_in_buffer;
	} else {
		read_limit = (size_t)((size - offset) > FILE_READ_LIMIT
					      ? FILE_READ_LIMIT
					      : size - offset);
		data = malloc(read_limit + 1);
		if (!data || fseeko(stream, (off_t)offset, SEEK_SET))
			goto unavailable;
		received = fread(data, 1, read_limit, stream);
		data[received] = 0;
	}
	fclose(stream);
	stream = NULL;
	next = offset;
	begin_response(&json, boot, "messages", reset, reason, 1);
	append_file_lines(
		&json, data, received, offset, MAX_RESPONSE_LINES, &next);
	more = received == FILE_READ_LIMIT;
	if (!more && next < offset + received) {
		size_t remaining = (size_t)(offset + received - next);
		char *pending = data + (size_t)(next - offset);

		more = memchr(pending, '\n', remaining) != NULL;
	}
	json_add(&json,
		 ",\"next_cursor\":\"m:%llx:%llx:%llu\",\"more\":%s}",
		 (unsigned long long)current_device,
		 (unsigned long long)current_inode,
		 (unsigned long long)next,
		 more ? "true" : "false");
	send_json(connection, &json);
	free(data);
	free(response);
	return;

unavailable:
	if (stream)
		fclose(stream);
	free(data);
	json.length = 0;
	json.failed = 0;
	begin_response(&json, boot, "messages", reset, reason, 0);
	json_add(&json, ",\"next_cursor\":null,\"more\":false,\"lines\":[]}");
	send_json(connection, &json);
	free(response);
}

/**
 * @brief Calculate a compact fingerprint for one kernel log line.
 * @param text Log line to fingerprint.
 * @return Stable 64-bit FNV-1a fingerprint.
 */
static uint64_t line_hash(const char *text)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	while (*text) {
		hash ^= (unsigned char)*text++;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

/**
 * @brief Split a kernel snapshot into individually fingerprinted lines.
 * @param data Mutable kernel log snapshot.
 * @param lines Destination array receiving line references.
 * @param capacity Maximum number of line references.
 * @return Number of parsed lines.
 */
static int split_kernel_lines(char *data, KernelLine *lines, int capacity)
{
	char *start = data;
	int count = 0;

	while (*start && count < capacity) {
		char *end = strchr(start, '\n');

		if (end)
			*end = 0;
		if (*start) {
			lines[count].text = start;
			lines[count].hash = line_hash(start);
			count++;
		}
		if (!end)
			break;
		start = end + 1;
	}
	return count;
}

/**
 * @brief Read a bounded, non-destructive kernel ringbuffer snapshot.
 * @param output Destination receiving the allocated snapshot.
 * @return Number of bytes read, or -1 when unavailable.
 */
static int read_kernel_log(char **output)
{
	const char *override = getenv("SB_LOG_DMESG_PATH");
	char *buffer;
	int length;

	buffer = malloc(DMESG_READ_LIMIT + 1);
	if (!buffer)
		return -1;
	if (override && *override) {
		FILE *stream = fopen(override, "rb");

		if (!stream) {
			free(buffer);
			return -1;
		}
		length = (int)fread(buffer, 1, DMESG_READ_LIMIT, stream);
		fclose(stream);
	} else {
#ifdef _WIN32
		length = -1;
#else
		length = klogctl(3, buffer, DMESG_READ_LIMIT);
#endif
	}
	if (length < 0) {
		free(buffer);
		return -1;
	}
	buffer[length] = 0;
	*output = buffer;
	return length;
}

/**
 * @brief Encode the delivered kernel-log position as an overlap cursor.
 * @param output Destination receiving the cursor.
 * @param capacity Capacity of the destination buffer.
 * @param lines Parsed kernel log lines.
 * @param delivered Number of lines delivered through this cursor.
 */
static void
kernel_cursor(char *output, size_t capacity, KernelLine *lines, int delivered)
{
	int first = delivered > OVERLAP_LINES ? delivered - OVERLAP_LINES : 0;
	size_t used = (size_t)snprintf(
		output, capacity, "d:1:%d:%d", delivered, delivered - first);

	for (int index = first; index < delivered && used < capacity; index++)
		used += (size_t)snprintf(output + used,
					 capacity - used,
					 ":%016llx",
					 (unsigned long long)lines[index].hash);
}

/**
 * @brief Decode a kernel overlap cursor.
 * @param cursor Encoded cursor supplied by the browser.
 * @param delivered Destination receiving the prior delivered line count.
 * @param hashes Destination receiving trailing line fingerprints.
 * @param hash_count Destination receiving the fingerprint count.
 * @return Zero on success; otherwise -1.
 */
static int parse_kernel_cursor(const char *cursor,
			       int *delivered,
			       uint64_t *hashes,
			       int *hash_count)
{
	const char *position;
	char *end;
	long version, count, overlap;

	if (strncmp(cursor, "d:", 2))
		return -1;
	position = cursor + 2;
	version = strtol(position, &end, 10);
	if (version != 1 || *end != ':')
		return -1;
	position = end + 1;
	count = strtol(position, &end, 10);
	if (count < 0 || *end != ':')
		return -1;
	position = end + 1;
	overlap = strtol(position, &end, 10);
	if (overlap < 0 || overlap > OVERLAP_LINES || overlap > count ||
	    (*end && *end != ':'))
		return -1;
	position = end;
	for (int index = 0; index < overlap; index++) {
		unsigned long long value;

		if (*position++ != ':' ||
		    sscanf(position, "%16llx", &value) != 1)
			return -1;
		hashes[index] = (uint64_t)value;
		position += 16;
	}
	if (*position)
		return -1;
	*delivered = (int)count;
	*hash_count = (int)overlap;
	return 0;
}

/**
 * @brief Find where a previous kernel cursor overlaps a new snapshot.
 * @param lines Current kernel log lines.
 * @param count Number of current lines.
 * @param delivered Prior delivered line count.
 * @param hashes Prior trailing fingerprints.
 * @param hash_count Number of prior fingerprints.
 * @return Index after the overlap, or -1 when no overlap exists.
 */
static int find_kernel_overlap(KernelLine *lines,
			       int count,
			       int delivered,
			       uint64_t *hashes,
			       int hash_count)
{
	int expected = delivered;
	int found = 0;

	if (!hash_count)
		return delivered == 0 ? 0 : -1;
	if (expected <= count && expected >= hash_count) {
		int matches = 1;

		for (int index = 0; index < hash_count; index++)
			if (lines[expected - hash_count + index].hash !=
			    hashes[index])
				matches = 0;
		if (matches)
			return expected;
	}
	for (int end = hash_count; end <= count; end++) {
		int matches = 1;

		for (int index = 0; index < hash_count; index++)
			if (lines[end - hash_count + index].hash !=
			    hashes[index])
				matches = 0;
		if (matches)
			expected = end, found = 1;
	}
	return found ? expected : -1;
}

/**
 * @brief Serve incremental records from the kernel ringbuffer.
 * @param connection Mongoose connection handling the request.
 * @param request HTTP request containing cursor state.
 * @param boot Current boot identifier.
 */
static void serve_dmesg(struct mg_connection *connection,
			struct mg_http_message *request,
			const char *boot)
{
	char cursor[512], previous_boot[128], next_cursor[512];
	char *snapshot = NULL, *response = NULL;
	KernelLine *lines = NULL;
	uint64_t hashes[OVERLAP_LINES];
	Json json;
	const char *reason = NULL;
	int count, start, end, delivered = 0, hash_count = 0;
	int reset = 0, has_cursor, requested = initial_lines(request);

	query(request, "boot_id", previous_boot, sizeof previous_boot);
	has_cursor = query(request, "cursor", cursor, sizeof cursor);
	response = malloc(JSON_LIMIT);
	lines = calloc(DMESG_READ_LIMIT / 2, sizeof *lines);
	if (!response || !lines)
		goto done;
	json = (Json){response, 0, JSON_LIMIT, 0};
	if (read_kernel_log(&snapshot) < 0) {
		begin_response(&json, boot, "dmesg", 0, NULL, 0);
		json_add(&json,
			 ",\"next_cursor\":null,\"more\":false,\"lines\":[]}");
		send_json(connection, &json);
		goto done;
	}
	count = split_kernel_lines(snapshot, lines, DMESG_READ_LIMIT / 2);
	if (previous_boot[0] && strcmp(previous_boot, boot)) {
		reset = 1;
		reason = "reboot";
		start = 0;
	} else if (has_cursor &&
		   parse_kernel_cursor(
			   cursor, &delivered, hashes, &hash_count)) {
		reset = 1;
		reason = "invalid_cursor";
		start = count > requested ? count - requested : 0;
	} else if (has_cursor &&
		   (start = find_kernel_overlap(
			    lines, count, delivered, hashes, hash_count)) < 0) {
		reset = 1;
		reason = "lost_overlap";
		start = count > requested ? count - requested : 0;
	} else if (!has_cursor) {
		start = count > requested ? count - requested : 0;
	}
	end = start + MAX_RESPONSE_LINES;
	if (end > count)
		end = count;
	begin_response(&json, boot, "dmesg", reset, reason, 1);
	json_add(&json, ",\"lines\":[");
	for (int index = start; index < end; index++) {
		if (index != start)
			json_add(&json, ",");
		json_string(&json, lines[index].text);
	}
	json_add(&json, "]");
	kernel_cursor(next_cursor, sizeof next_cursor, lines, end);
	json_add(&json, ",\"next_cursor\":");
	json_string(&json, next_cursor);
	json_add(&json, ",\"more\":%s}", end < count ? "true" : "false");
	send_json(connection, &json);

done:
	free(snapshot);
	free(lines);
	free(response);
}

/**
 * @brief Handle an authenticated system-log API request.
 * @param connection Mongoose connection handling the request.
 * @param request HTTP request to process.
 * @return Nonzero when the request belongs to the log API.
 */
int logs_handle(struct mg_connection *connection,
		struct mg_http_message *request)
{
	char current_boot[128];

	if (!logs_is_api(request->uri))
		return 0;
	if (!equal(request->method, "GET")) {
		mg_http_reply(connection,
			      405,
			      "Content-Type: application/json\r\n"
			      "Cache-Control: no-store\r\n",
			      "{\"error\":\"Method not allowed\"}");
		return 1;
	}
	boot_id(current_boot, sizeof current_boot);
	if (equal(request->uri, "/api/logs/messages"))
		serve_messages(connection, request, current_boot);
	else if (equal(request->uri, "/api/logs/dmesg"))
		serve_dmesg(connection, request, current_boot);
	else
		mg_http_reply(connection,
			      404,
			      "Content-Type: application/json\r\n"
			      "Cache-Control: no-store\r\n",
			      "{\"error\":\"Unknown log source\"}");
	return 1;
}
