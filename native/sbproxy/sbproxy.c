/*
 * Loopback HTTP/HTTPS compatibility proxy and streaming artwork transport.
 *
 * Uses libcurl and bounded worker threads to fetch upstream resources for
 * legacy Jive clients. Runs as the separate sbproxy service supervised by the
 * Standalone Base Lua applet, alongside sbbase and sbwebserver.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../common/public_address.h"
#include "hls.h"

/** Enforce opt-in public destinations on actual sockets, including redirects.
 */
static curl_socket_t public_image_socket(void *data,
					 curlsocktype purpose,
					 struct curl_sockaddr *address)
{
	(void)data;
	(void)purpose;
#ifdef SB_PROXY_TESTING
	/* Dedicated host-test executable only; never compiled into shipped
	 * binaries. */
	const char *ports = getenv("SB_PROXY_TEST_PORTS");
	unsigned first = 0, second = 0;
	if (ports && address->family == AF_INET &&
	    sscanf(ports, "%u,%u", &first, &second) == 2) {
		struct sockaddr_in *test = (struct sockaddr_in *)&address->addr;
		unsigned port = ntohs(test->sin_port);
		if (ntohl(test->sin_addr.s_addr) == INADDR_LOOPBACK &&
		    (port == first || port == second))
			return socket(address->family,
				      address->socktype,
				      address->protocol);
	}
#endif
	if (address->family != AF_INET ||
	    !sb_public_ipv4((const unsigned char *)&(
				    (struct sockaddr_in *)&address->addr)
				    ->sin_addr))
		return CURL_SOCKET_BAD;
	unsigned port = ntohs(((struct sockaddr_in *)&address->addr)->sin_port);
	if (port != 80 && port != 443)
		return CURL_SOCKET_BAD;
	return socket(address->family, address->socktype, address->protocol);
}

#define VERSION "0.5.6"

#define MAX_REQUEST 16384
#define MAX_HEADERS 32768
#define MAX_WORKERS 8
#define WORKER_STACK (256U * 1024U)

typedef struct {
	int fd;
} worker_arg;

typedef struct {
	int fd;
	int started;
	int status;

	char headers[MAX_HEADERS + 1];

	size_t length;
	size_t bytes;
	/* Only SVG is dispatched to the existing renderer. Raster bytes stream.
	 */
	const char *artwork_source;
	unsigned width, height, web_port;
	int svg, svg_redirect;
} transfer_ctx;

static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_workers = 0;
static const char *ca_bundle = NULL;

/* -------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle log line.
 * @param format Input text for format.
 */
static void log_line(const char *format, ...)
{
	va_list args;

	va_start(args, format);

	fputs("[sbproxy] ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);

	va_end(args);
}

/* -------------------------------------------------------------------------
 * Socket helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Send all.
 * @param fd Open file descriptor.
 * @param data Data supplied to the operation.
 * @param length Size or count for length.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int send_all(int fd, const void *data, size_t length)
{
	const unsigned char *p = data;

	while (length > 0) {
		ssize_t n = send(fd, p, length, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR) {
			continue;
		}

		if (n <= 0) {
			return -1;
		}

		p += n;
		length -= (size_t)n;
	}

	return 0;
}

/**
 * @brief Send error.
 * @param fd Open file descriptor.
 * @param code Status or response code.
 * @param reason Input text for reason.
 */
static void send_error(int fd, int code, const char *reason)
{
	char buffer[256];
	int length;

	length = snprintf(buffer,
			  sizeof(buffer),
			  "HTTP/1.1 %d %s\r\n"
			  "Content-Type: text/plain\r\n"
			  "Content-Length: 0\r\n"
			  "Connection: close\r\n"
			  "\r\n",
			  code,
			  reason);

	if (length > 0) {
		(void)send_all(fd, buffer, (size_t)length);
	}
}

/* -------------------------------------------------------------------------
 * HTTP header helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Check whether static hop header.
 * @param name Name of the item to process.
 * @return Nonzero when the condition is true; otherwise zero.
 */
static int is_static_hop_header(const char *name)
{
	return !strcasecmp(name, "connection") ||
	       !strcasecmp(name, "keep-alive") ||
	       !strcasecmp(name, "proxy-authenticate") ||
	       !strcasecmp(name, "proxy-authorization") ||
	       !strcasecmp(name, "te") || !strcasecmp(name, "trailer") ||
	       !strcasecmp(name, "transfer-encoding") ||
	       !strcasecmp(name, "upgrade");
}

/**
 * @brief Get header name.
 * @param line Input text for line.
 * @param name Name of the item to process.
 * @param capacity Size or count for capacity.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int get_header_name(const char *line, char *name, size_t capacity)
{
	const char *colon = strchr(line, ':');
	size_t length;

	if (!colon || colon == line) {
		return 0;
	}

	length = (size_t)(colon - line);

	if (length >= capacity) {
		return 0;
	}

	memcpy(name, line, length);
	name[length] = '\0';

	return 1;
}

/**
 * @brief Handle token contains.
 * @param tokens Input text for tokens.
 * @param name Name of the item to process.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int token_contains(const char *tokens, const char *name)
{
	size_t name_length = strlen(name);

	while (*tokens) {
		const char *end;

		while (*tokens == ' ' || *tokens == '\t' || *tokens == ',') {
			tokens++;
		}

		end = tokens;

		while (*end && *end != ',' && *end != '\r' && *end != '\n') {
			end++;
		}

		while (end > tokens && (end[-1] == ' ' || end[-1] == '\t')) {
			end--;
		}

		if ((size_t)(end - tokens) == name_length &&
		    !strncasecmp(tokens, name, name_length)) {
			return 1;
		}

		tokens = *end ? end + 1 : end;
	}

	return 0;
}

/**
 * @brief Handle connection tokens.
 * @param headers Input text for headers.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *connection_tokens(const char *headers)
{
	const char *line = headers;

	while (line && *line) {
		const char *next = strstr(line, "\r\n");

		if (!strncasecmp(line, "Connection:", 11)) {
			return line + 11;
		}

		if (!next) {
			break;
		}

		line = next + 2;
	}

	return "";
}

/**
 * @brief Handle reason phrase.
 * @param status Numeric value for status.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *reason_phrase(int status)
{
	switch (status) {
	case 200:
		return "OK";

	case 206:
		return "Partial Content";

	case 304:
		return "Not Modified";

	case 400:
		return "Bad Request";

	case 404:
		return "Not Found";

	case 416:
		return "Range Not Satisfiable";

	default:
		return "Upstream Response";
	}
}

/* -------------------------------------------------------------------------
 * Upstream response handling
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle emit headers.
 * @param ctx Operation context.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int emit_headers(transfer_ctx *ctx)
{
	char status_line[96];
	const char *tokens = connection_tokens(ctx->headers);
	const char *line = ctx->headers;
	int length;

	length = snprintf(status_line,
			  sizeof(status_line),
			  "HTTP/1.1 %d %s\r\n",
			  ctx->status,
			  reason_phrase(ctx->status));

	if (length <= 0 ||
	    send_all(ctx->fd, status_line, (size_t)length) != 0) {
		return -1;
	}

	while (line && *line) {
		const char *next = strstr(line, "\r\n");

		size_t line_length =
			next ? (size_t)(next - line) : strlen(line);

		char copy[4096];
		char name[128];

		if (line_length >= sizeof(copy)) {
			return -1;
		}

		memcpy(copy, line, line_length);
		copy[line_length] = '\0';

		if (get_header_name(copy, name, sizeof(name)) &&
		    !is_static_hop_header(name) &&
		    !token_contains(tokens, name)) {

			if (send_all(ctx->fd, copy, line_length) != 0 ||
			    send_all(ctx->fd, "\r\n", 2) != 0) {
				return -1;
			}
		}

		if (!next) {
			break;
		}

		line = next + 2;
	}

	if (send_all(ctx->fd, "Connection: close\r\n\r\n", 21) != 0) {
		return -1;
	}

	ctx->started = 1;

	return 0;
}

/**
 * @brief Handle the header callback.
 * @param data Data supplied to the operation.
 * @param size Size of the supplied buffer or object.
 * @param count Size or count for count.
 * @param opaque Object referenced by opaque.
 * @return Number of bytes or items produced by the operation.
 */
static size_t header_cb(char *data, size_t size, size_t count, void *opaque)
{
	transfer_ctx *ctx = opaque;
	size_t length = size * count;

	/*
	 * A new HTTP status line also occurs for redirects and
	 * informational responses. Reset the collected headers.
	 */
	if (length >= 5 && !strncasecmp(data, "HTTP/", 5)) {

		const char *space = memchr(data, ' ', length);

		ctx->status = space ? atoi(space + 1) : 0;
		ctx->length = 0;
		ctx->headers[0] = '\0';
		ctx->svg = 0;

		return length;
	}

	/*
	 * Empty line: end of this response's headers.
	 *
	 * Do not forward informational or redirect headers because
	 * libcurl handles redirects internally.
	 */
	if ((length == 2 && data[0] == '\r' && data[1] == '\n') ||
	    (length == 1 && data[0] == '\n')) {

		if ((ctx->status >= 100 && ctx->status < 200) ||
		    (ctx->status >= 300 && ctx->status < 400)) {
			return length;
		}

		if (ctx->artwork_source && ctx->status == 200 && ctx->svg) {
			/* Escape the source, not its query components
			 * independently. */
			static const char hex[] = "0123456789ABCDEF";
			char escaped[4096 * 3 + 1];
			size_t n = 0;
			const unsigned char *p =
				(const unsigned char *)ctx->artwork_source;
			for (; *p; p++) {
				if ((*p >= 'a' && *p <= 'z') ||
				    (*p >= 'A' && *p <= 'Z') ||
				    (*p >= '0' && *p <= '9') ||
				    strchr("-._~", *p))
					escaped[n++] = (char)*p;
				else {
					escaped[n++] = '%';
					escaped[n++] = hex[*p >> 4];
					escaped[n++] = hex[*p & 15];
				}
			}
			escaped[n] = 0;
			char header[4096 * 3 + 320];
			int count = snprintf(
				header,
				sizeof(header),
				"HTTP/1.1 302 Found\r\nLocation: "
				"http://127.0.0.1:%u/api/image/"
				"render?url=%s&width=%u&height=%u\r\n"
				"Content-Length: 0\r\nCache-Control: "
				"no-store\r\nConnection: close\r\n\r\n",
				ctx->web_port,
				escaped,
				ctx->width,
				ctx->height);
			if (count < 0 || count >= (int)sizeof(header))
				return 0;
			ctx->started = ctx->svg_redirect = 1;
			return send_all(ctx->fd, header, (size_t)count)
				       ? 0
				       : length;
		}
		return emit_headers(ctx) ? 0 : length;
	}
	if (length >= 13 && !strncasecmp(data, "Content-Type:", 13)) {
		const char *value = data + 13, *end = data + length;
		while (value < end && (*value == ' ' || *value == '\t'))
			value++;
		if (end - value >= 13 &&
		    !strncasecmp(value, "image/svg+xml", 13) &&
		    (end - value == 13 || strchr("; \t\r\n", value[13])))
			ctx->svg = 1;
	}

	if (ctx->length + length > MAX_HEADERS) {
		return 0;
	}

	memcpy(ctx->headers + ctx->length, data, length);

	ctx->length += length;
	ctx->headers[ctx->length] = '\0';

	return length;
}

/**
 * @brief Handle the body callback.
 * @param data Data supplied to the operation.
 * @param size Size of the supplied buffer or object.
 * @param count Size or count for count.
 * @param opaque Object referenced by opaque.
 * @return Number of bytes or items produced by the operation.
 */
static size_t body_cb(char *data, size_t size, size_t count, void *opaque)
{
	transfer_ctx *ctx = opaque;
	size_t length = size * count;
	if (ctx->svg_redirect)
		return 0;

	/*
	 * Redirect bodies are ignored because libcurl follows
	 * the redirect and the client should only receive the
	 * final response.
	 */
	if (!ctx->started) {
		if (ctx->status >= 300 && ctx->status < 400) {
			return length;
		}

		return 0;
	}

	if (send_all(ctx->fd, data, length) != 0) {
		return 0;
	}

	ctx->bytes += length;

	return length;
}

/* -------------------------------------------------------------------------
 * Client request handling
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle receive request.
 * @param fd Open file descriptor.
 * @param request Mutable text buffer for request.
 * @param capacity Size or count for capacity.
 * @param length_out Object referenced by length out.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int
receive_request(int fd, char *request, size_t capacity, size_t *length_out)
{
	size_t length = 0;

	request[0] = '\0';

	while (length < capacity - 1) {
		ssize_t n =
			recv(fd, request + length, capacity - 1 - length, 0);

		if (n < 0 && errno == EINTR) {
			continue;
		}

		if (n <= 0) {
			return -1;
		}

		length += (size_t)n;
		request[length] = '\0';

		if (strstr(request, "\r\n\r\n")) {
			*length_out = length;
			return 0;
		}
	}

	return 1;
}

/**
 * @brief Build request headers.
 * @param request Mutable text buffer for request.
 * @param public_only Restrict public fetches to non-credential image headers.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static struct curl_slist *build_request_headers(char *request, int public_only)
{
	struct curl_slist *result = NULL;
	char *save = NULL;
	char *line;

	const char *tokens = connection_tokens(request);

	/*
	 * Skip the HTTP request line.
	 */
	(void)strtok_r(request, "\r\n", &save);

	while ((line = strtok_r(NULL, "\r\n", &save))) {
		char name[128];

		if (get_header_name(line, name, sizeof(name)) &&
		    (!public_only || !strcasecmp(name, "Range") ||
		     !strcasecmp(name, "If-Range") ||
		     !strcasecmp(name, "Accept") ||
		     !strcasecmp(name, "Accept-Encoding") ||
		     !strcasecmp(name, "User-Agent") ||
		     !strcasecmp(name, "If-None-Match") ||
		     !strcasecmp(name, "If-Modified-Since")) &&
		    strcasecmp(name, "host") &&
		    strcasecmp(name, "x-sb-public-fetch") &&
		    !is_static_hop_header(name) &&
		    !token_contains(tokens, name)) {

			struct curl_slist *next =
				curl_slist_append(result, line);

			if (!next) {
				curl_slist_free_all(result);
				return NULL;
			}

			result = next;
		}
	}

	return result;
}

/* -------------------------------------------------------------------------
 * Health endpoint
 * ------------------------------------------------------------------------- */

/**
 * @brief Send health response.
 * @param fd Open file descriptor.
 * @param head Numeric value for head.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int send_health_response(int fd, int head)
{
	const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);

	char body[256];
	char headers[256];

	int body_length;
	int header_length;

	body_length = snprintf(
		body,
		sizeof(body),
		"sbproxy %s\n"
		"libcurl: %s\n"
		"TLS: %s\n"
		"status: OK\n",
		VERSION,
		info && info->version ? info->version : "unknown",
		info && info->ssl_version ? info->ssl_version : "unknown");

	header_length = snprintf(headers,
				 sizeof(headers),
				 "HTTP/1.1 200 OK\r\n"
				 "Content-Type: text/plain\r\n"
				 "X-SB-Public-Fetch: 1\r\n"
				 "X-SB-HLS: aac-mp3-ts\r\n"
				 "Content-Length: %d\r\n"
				 "Connection: close\r\n"
				 "\r\n",
				 head ? 0 : body_length);

	if (send_all(fd, headers, (size_t)header_length) != 0) {
		return -1;
	}

	if (!head && send_all(fd, body, (size_t)body_length) != 0) {
		return -1;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Worker bookkeeping
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle release worker.
 */
static void release_worker(void)
{
	pthread_mutex_lock(&worker_lock);

	active_workers--;

	pthread_mutex_unlock(&worker_lock);
}

/* -------------------------------------------------------------------------
 * libcurl configuration
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle configure curl.
 * @param curl Object referenced by curl.
 * @param url URL to process.
 * @param headers Object referenced by headers.
 * @param transfer Object referenced by transfer.
 * @param is_head Flag controlling whether the indicated behavior is enabled.
 * @param error_buffer Mutable text buffer for error buffer.
 */
static void configure_curl(CURL *curl,
			   const char *url,
			   struct curl_slist *headers,
			   transfer_ctx *transfer,
			   int is_head,
			   char *error_buffer)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

	curl_easy_setopt(curl, CURLOPT_NOBODY, is_head ? 1L : 0L);

	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);

	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);

	curl_easy_setopt(curl, CURLOPT_WRITEDATA, transfer);

	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

	curl_easy_setopt(curl, CURLOPT_HEADERDATA, transfer);

	/*
	 * Make libcurl/wolfSSL provide a detailed error string.
	 * This is particularly useful for diagnosing TLS failures
	 * which otherwise only appear as CURLE_SSL_CONNECT_ERROR.
	 */
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

	/*
	 * Certificate and hostname verification are intentionally disabled for
	 * compatibility with certificate chains rejected by this wolfSSL build.
	 * TLS still encrypts the stream, but does not authenticate the server.
	 */
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	if (ca_bundle) {
		curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
	}
}

/* -------------------------------------------------------------------------
 * Transfer logging
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle elapsed ms.
 * @param start Object referenced by start.
 * @param end Object referenced by end.
 * @return Value produced by the operation.
 */
static long elapsed_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000L +
	       (end->tv_nsec - start->tv_nsec) / 1000000L;
}

/**
 * @brief Handle log request.
 * @param method Input text for method.
 * @param target Input text for target.
 */
static void log_request(const char *method, const char *target)
{
	const char *path = target + 7;
	const char *query = strchr(path, '?');

	size_t visible_length = query ? (size_t)(query - path) : strlen(path);

	int shown;

	if (visible_length > 512) {
		visible_length = 512;
	}

	shown = (int)visible_length;

	log_line("%s %.*s%s",
		 method,
		 shown,
		 path,
		 query ? " [query redacted]" : "");
}

/**
 * @brief Handle log transfer result.
 * @param result Input value for result.
 * @param error_buffer Input text for error buffer.
 * @param transfer Object referenced by transfer.
 * @param start Object referenced by start.
 * @param end Object referenced by end.
 */
static void log_transfer_result(CURLcode result,
				const char *error_buffer,
				const transfer_ctx *transfer,
				const struct timespec *start,
				const struct timespec *end)
{
	if (result != CURLE_OK) {
		log_line("curl error=%d (%s): %s",
			 (int)result,
			 curl_easy_strerror(result),
			 error_buffer[0] ? error_buffer
					 : "no additional information");
	}

	log_line("result=%d status=%d bytes=%zu duration_ms=%ld",
		 (int)result,
		 transfer->status,
		 transfer->bytes,
		 elapsed_ms(start, end));
}

/* -------------------------------------------------------------------------
 * Worker
 * ------------------------------------------------------------------------- */

/**
 * @brief Handle worker main.
 * @param opaque Object referenced by opaque.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static void *worker_main(void *opaque)
{
	worker_arg *arg = opaque;
	int fd = arg->fd;

	char request[MAX_REQUEST + 1];
	char request_copy[MAX_REQUEST + 1];

	char method[8];
	char target[MAX_REQUEST + 1];
	char protocol[16];

	char url[MAX_REQUEST + 16];

	/*
	 * CURLOPT_ERRORBUFFER requires at least CURL_ERROR_SIZE bytes.
	 */
	char curl_error[CURL_ERROR_SIZE] = {0};

	size_t request_length = 0;

	int received;
	int is_head;
	int artwork = 0, plain_http = 0, hls = 0;
	unsigned width = 0, height = 0, web_port = 0;
	const char *source_target = target;

	CURL *curl = NULL;
	struct curl_slist *headers = NULL;

	transfer_ctx transfer;
	CURLcode result;

	struct timespec start;
	struct timespec end;

	free(arg);

	/*
	 * Receive and validate the client's HTTP request.
	 */
	received =
		receive_request(fd, request, sizeof(request), &request_length);

	if (received > 0) {
		send_error(fd, 431, "Request Header Fields Too Large");
		goto done;
	}

	if (received < 0 || !strstr(request, "\r\n\r\n") ||
	    sscanf(request, "%7s %16384s %15s", method, target, protocol) !=
		    3) {

		send_error(fd, 400, "Bad Request");

		goto done;
	}

	/*
	 * Only GET and HEAD are supported.
	 */
	if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {

		send_error(fd, 405, "Method Not Allowed");

		goto done;
	}

	is_head = !strcasecmp(method, "HEAD");
	if (hls_metadata_http(fd, target, is_head)) goto done;

	/*
	 * Local health endpoint.
	 */
	if (!strcmp(target, "/health")) {
		(void)send_health_response(fd, is_head);

		goto done;
	}

	/*
	 * Proxy URLs must have this form:
	 *
	 *   /https/example.com/path
	 *
	 * which becomes:
	 *
	 *   https://example.com/path
	 */
	if (!strncmp(target, "/hls/", 5)) {
		hls = 1;
		source_target = target + 4;
	}
	if (!strncmp(target, "/artwork/", 9)) {
		int used = 0;
		if (sscanf(target,
			   "/artwork/%u/%u/%u/%n",
			   &width,
			   &height,
			   &web_port,
			   &used) != 3 ||
		    !used || width < 16 || width > 512 || height < 16 ||
		    height > 512 || !web_port || web_port > 65535) {
			send_error(fd, 400, "Malformed Artwork URL");
			goto done;
		}
		artwork = 1;
		source_target = target + used - 1;
	}
	plain_http = !strncmp(source_target, "/http/", 6);
	if (!plain_http && strncmp(source_target, "/https/", 7)) {
		send_error(fd, 404, "Not Found");

		goto done;
	}

	const char *authority = source_target + (plain_http ? 6 : 7);
	const char *slash = strchr(authority, '/');
	if (!*authority || !slash || slash == authority ||
	    strpbrk(authority, "\r\n\\#") ||
	    memchr(authority, '@', slash ? (size_t)(slash - authority) : 0) ||
	    ((artwork || hls) && strlen(authority) > 4087)) {

		send_error(fd, 400, "Malformed Proxy URL");

		goto done;
	}

	if (snprintf(url,
		     sizeof(url),
		     "%s://%s",
		     plain_http ? "http" : "https",
		     authority) >= (int)sizeof(url)) {

		send_error(fd, 414, "URI Too Long");

		goto done;
	}

	/*
	 * build_request_headers() modifies its input, so work on
	 * a copy of the original request.
	 */
	memcpy(request_copy, request, request_length + 1);

	headers = build_request_headers(request_copy,
					artwork || plain_http || hls);

	/*
	 * Create the upstream libcurl connection.
	 */
	curl = curl_easy_init();

	if (!curl) {
		send_error(fd, 500, "Internal Server Error");

		goto done;
	}

	memset(&transfer, 0, sizeof(transfer));

	transfer.fd = fd;
	if (artwork) {
		transfer.artwork_source = url;
		transfer.width = width;
		transfer.height = height;
		transfer.web_port = web_port;
	}

	configure_curl(curl, url, headers, &transfer, is_head, curl_error);
	/* Opt-in only: ordinary compatibility-proxy requests retain their
	 * policy. */
	if (artwork || plain_http || hls ||
	    strstr(request, "\r\nX-SB-Public-Fetch: 1\r\n")) {
		curl_easy_setopt(
			curl, CURLOPT_OPENSOCKETFUNCTION, public_image_socket);
		curl_easy_setopt(curl, CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(
			curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
		curl_easy_setopt(curl,
				 CURLOPT_PROTOCOLS,
				 (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
		curl_easy_setopt(curl,
				 CURLOPT_REDIR_PROTOCOLS,
				 (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
		if (!artwork && !plain_http && !hls)
			curl_easy_setopt(curl,
					 CURLOPT_MAXFILESIZE_LARGE,
					 (curl_off_t)(1024u * 1024u));
	}

	/*
	 * Avoid logging URL query parameters because they may
	 * contain tokens or other sensitive data.
	 */
	if (hls) {
		/* Signed CDN paths can contain credentials as well as queries.
		 */
		log_line("%s HLS loopback stream", method);
		int expected_codec = 0;
		const char *line = strstr(request, "\r\n");
		while (line && line[2]) {
			line += 2;
			const char *end = strstr(line, "\r\n");
			if (!end || end == line)
				break;
			if (end - line >= 15 &&
			    !strncasecmp(line, "X-SB-HLS-Codec:", 15)) {
				const char *value = line + 15;
				while (value < end &&
				       (*value == ' ' || *value == '\t'))
					value++;
				const char *last = end;
				while (last > value &&
				       (last[-1] == ' ' || last[-1] == '\t'))
					last--;
				if (expected_codec || last - value != 3 ||
				    (strncasecmp(value, "aac", 3) &&
				     strncasecmp(value, "mp3", 3))) {
					send_error(
						fd, 400, "Invalid HLS Codec");
					goto done;
				}
				expected_codec = !strncasecmp(value, "aac", 3)
							 ? HLS_AAC
							 : HLS_MP3;
			}
			line = end;
		}
		hls_relay(curl, fd, url, is_head, is_head ? 0 : expected_codec);
		goto done;
	}
	log_request(method, target);

	clock_gettime(CLOCK_MONOTONIC, &start);

	result = curl_easy_perform(curl);

	clock_gettime(CLOCK_MONOTONIC, &end);

	/*
	 * If no upstream response was sent to the client, translate
	 * the libcurl failure to a local HTTP 502.
	 *
	 * CURLE_WRITE_ERROR is excluded because it can simply mean
	 * that the local client disconnected while receiving data.
	 */
	if (result != CURLE_OK && !transfer.started &&
	    result != CURLE_WRITE_ERROR) {

		send_error(fd, 502, "Bad Gateway");
	}

	log_transfer_result(result, curl_error, &transfer, &start, &end);

done:
	if (curl) {
		curl_easy_cleanup(curl);
	}

	curl_slist_free_all(headers);

	close(fd);
	release_worker();

	return NULL;
}

/* -------------------------------------------------------------------------
 * Command-line handling
 * ------------------------------------------------------------------------- */

/**
 * @brief Parse port.
 * @param address Input text for address.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int parse_port(const char *address)
{
	char *end;
	long port;

	if (strncmp(address, "127.0.0.1:", 10)) {
		return -1;
	}

	errno = 0;

	port = strtol(address + 10, &end, 10);

	if (errno || *end || port < 1 || port > 65535) {
		return -1;
	}

	return (int)port;
}

/**
 * @brief Parse arguments.
 * @param argc Number of command-line arguments.
 * @param argv Command-line argument vector.
 * @param listen_address Input text for listen address.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int parse_arguments(int argc, char **argv, const char **listen_address)
{
	int index;

	for (index = 1; index < argc; index++) {

		if (!strcmp(argv[index], "--help")) {
			puts("usage: sbproxy "
			     "[--listen 127.0.0.1:8765] "
			     "[--ca-bundle PATH]");

			return 1;
		}

		if (!strcmp(argv[index], "--version")) {
			puts("sbproxy " VERSION);
			return 1;
		}

		if (!strcmp(argv[index], "--listen") && index + 1 < argc) {

			*listen_address = argv[++index];

			continue;
		}

		if (!strcmp(argv[index], "--ca-bundle") && index + 1 < argc) {

			ca_bundle = argv[++index];

			continue;
		}

		fprintf(stderr,
			"unknown or incomplete option: %s\n",
			argv[index]);

		return -1;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

/**
 * @brief Run the program entry point.
 * @param argc Number of command-line arguments.
 * @param argv Command-line argument vector.
 * @return Process exit status.
 */
int main(int argc, char **argv)
{
	const char *listen_address = "127.0.0.1:8765";

	int listener;
	int one = 1;
	int port;
	int argument_result;

	struct sockaddr_in address;
	pthread_attr_t attributes;

	/*
	 * Command-line options.
	 */
	argument_result = parse_arguments(argc, argv, &listen_address);

	if (argument_result > 0) {
		return 0;
	}

	if (argument_result < 0) {
		return 2;
	}

	/*
	 * sbproxy deliberately only listens on localhost.
	 */
	port = parse_port(listen_address);

	if (port < 0) {
		fputs("sbproxy only accepts "
		      "--listen 127.0.0.1:<port>\n",
		      stderr);

		return 2;
	}

	/*
	 * We handle failed client writes ourselves.
	 */
	signal(SIGPIPE, SIG_IGN);

	/*
	 * Initialise libcurl.
	 */
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		return 1;
	}

	/*
	 * Create local HTTP listener.
	 */
	listener = socket(AF_INET, SOCK_STREAM, 0);

	if (listener < 0) {
		perror("socket");
		return 1;
	}

	(void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&address, 0, sizeof(address));

	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);

	inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
	    listen(listener, MAX_WORKERS)) {

		perror("bind/listen");
		close(listener);
		return 1;
	}

	/*
	 * Configure detached worker threads.
	 */
	pthread_attr_init(&attributes);

	pthread_attr_setstacksize(&attributes, WORKER_STACK);

	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

	log_line("version %s listening %s workers=%d",
		 VERSION,
		 listen_address,
		 MAX_WORKERS);

	/*
	 * Main accept loop.
	 */
	for (;;) {
		worker_arg *arg;
		pthread_t thread;

		int fd = accept(listener, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("accept");
			continue;
		}

		/*
		 * Limit concurrent transfers.
		 */
		pthread_mutex_lock(&worker_lock);

		if (active_workers >= MAX_WORKERS) {
			pthread_mutex_unlock(&worker_lock);

			send_error(fd, 503, "Service Unavailable");

			close(fd);
			continue;
		}

		active_workers++;

		pthread_mutex_unlock(&worker_lock);

		/*
		 * Pass the socket to a detached worker.
		 */
		arg = malloc(sizeof(*arg));

		if (!arg) {
			send_error(fd, 500, "Internal Server Error");

			close(fd);
			release_worker();
			continue;
		}

		arg->fd = fd;

		if (pthread_create(&thread, &attributes, worker_main, arg)) {

			free(arg);

			send_error(fd, 500, "Internal Server Error");

			close(fd);
			release_worker();
		}
	}
}
