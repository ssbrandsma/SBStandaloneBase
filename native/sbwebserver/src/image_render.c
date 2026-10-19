/*
 * Generic cached SVG artwork endpoint for Jive applets and paired browsers.
 * A bounded short-lived worker uses Mongoose for HTTP, the running sbproxy for
 * HTTPS, and the separate svgpngrenderer executable for local-file conversion.
 * The management server event loop only accepts jobs and delivers their
 * results.
 */
#define _POSIX_C_SOURCE 200809L
#include "image_render.h"
#include "auth.h"
#include "remote.h"
#include "../../common/public_address.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SVG_LIMIT (1024u * 1024u)
#define IMAGE_LIMIT (8u * 1024u * 1024u)
#define PNG_LIMIT (2u * 1024u * 1024u)
#define URL_LIMIT 4096
#define CACHE_LIMIT (20u * 1024u * 1024u)
#define CACHE_TTL (7 * 86400)
#define WAITER_LIMIT 2

typedef struct {
	char normalized[URL_LIMIT], host[254], path[URL_LIMIT];
	unsigned port;
	int https;
} ImageUrl;

typedef struct {
	unsigned status, size, cached;
} Result;

static struct {
	pid_t pid;
	int pipe;
	unsigned long clients[WAITER_LIMIT];
	unsigned clients_count;
	int head[WAITER_LIMIT];
	char key[65], directory[128];
	unsigned char *data;
	size_t received;
	uint64_t deadline;
	size_t input_limit;
} job;

/** Compare an HTTP token without reading beyond its bounded Mongoose string. */
static int equal(struct mg_str s, const char *text)
{
	return s.len == strlen(text) && !memcmp(s.buf, text, s.len);
}

/** Parse a bounded decimal header without accepting signs, suffixes or
 * overflow. */
static size_t header_size(struct mg_str value)
{
	size_t result = 0;
	if (!value.len)
		return (size_t)-1;
	for (size_t i = 0; i < value.len; i++) {
		if (value.buf[i] < '0' || value.buf[i] > '9' ||
		    result > IMAGE_LIMIT)
			return (size_t)-1;
		result = result * 10 + (size_t)(value.buf[i] - '0');
	}
	return result;
}

/** Return a generic error without URLs, credentials, input data or local paths.
 */
static void image_error(struct mg_connection *c, unsigned code)
{
	const char *message =
		code == 400   ? "Invalid image request"
		: code == 401 ? "Browser pairing required"
		: code == 403 ? "Image access or destination rejected"
		: code == 405 ? "Method not allowed"
		: code == 413 ? "Image exceeds input limit"
		: code == 415 ? "Malformed or unsupported image"
		: code == 503 ? "Image resource limit or renderer unavailable"
		: code == 504 ? "Image operation timed out"
			      : "Image download failed";
	mg_http_reply(
		c,
		(int)code,
		"Content-Type: application/json\r\nCache-Control: no-store\r\n"
		"X-Content-Type-Options: nosniff\r\nReferrer-Policy: "
		"no-referrer\r\nConnection: close\r\n",
		"{\"error\":\"%s\"}",
		message);
	c->is_draining = 1;
}

/** Normalize a strict credential-free HTTP(S) URL with ordinary service ports.
 */
static int parse_url(const char *text, ImageUrl *u)
{
	const char *p, *end, *tail, *colon;
	size_t n;
	memset(u, 0, sizeof *u);
	if (!text || strlen(text) >= URL_LIMIT)
		return 0;
	for (p = text; *p; p++)
		if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 ||
		    *p == '\\' || *p == '#')
			return 0;
	if (!strncasecmp(text, "https://", 8)) {
		u->https = 1;
		p = text + 8;
	} else if (!strncasecmp(text, "http://", 7))
		p = text + 7;
	else
		return 0;
	end = p + strcspn(p, "/?");
	colon = memchr(p, ':', (size_t)(end - p));
	n = (size_t)((colon ? colon : end) - p);
	if (!n || n >= sizeof u->host)
		return 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];
		if (!isalnum(c) && c != '-' && c != '.')
			return 0;
		u->host[i] = (char)tolower(c);
	}
	if (u->host[0] == '.' || u->host[n - 1] == '.')
		return 0;
	u->port = u->https ? 443 : 80;
	if (colon &&
	    ((size_t)(end - colon - 1) != (u->https ? 3u : 2u) ||
	     memcmp(colon + 1, u->https ? "443" : "80", u->https ? 3 : 2)))
		return 0;
	tail = *end ? end : "/";
	if (snprintf(u->path,
		     sizeof u->path,
		     "%s%s",
		     *tail == '?' ? "/" : "",
		     tail) >= (int)sizeof u->path)
		return 0;
	if (snprintf(u->normalized,
		     sizeof u->normalized,
		     "%s://%s%s",
		     u->https ? "https" : "http",
		     u->host,
		     u->path) >= (int)sizeof u->normalized)
		return 0;
	return 1;
}

/** Resolve and pin a public IPv4 address; the worker can block during DNS. */
static int resolve_public(const ImageUrl *u, char ip[INET_ADDRSTRLEN])
{
	struct addrinfo hints = {0}, *addresses = NULL;
	int ok = 0;
#ifdef SB_IMAGE_TESTING
	/* Test-only origin mapping is absent from release binaries. */
	if (!strcmp(u->host, "image.test") && getenv("SB_IMAGE_TEST_IP")) {
		snprintf(ip, INET_ADDRSTRLEN, "%s", getenv("SB_IMAGE_TEST_IP"));
		return 1;
	}
#endif
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(u->host, NULL, &hints, &addresses))
		return 0;
	for (struct addrinfo *a = addresses; a; a = a->ai_next) {
		struct in_addr address =
			((struct sockaddr_in *)a->ai_addr)->sin_addr;
		if (!sb_public_ipv4((unsigned char *)&address)) {
			ok = 0;
			break;
		}
		if (!ok && inet_ntop(AF_INET, &address, ip, INET_ADDRSTRLEN))
			ok = 1;
	}
	freeaddrinfo(addresses);
	return ok;
}

typedef struct {
	char *body;
	size_t size;
	unsigned status;
	int done, capability;
	char location[URL_LIMIT];
	FILE *stream;
	size_t remaining, chunk_remaining;
	int detached, chunked, chunk_end;
	size_t limit;
} Fetch;

/* Consume an HTTP entity into the private job file, keeping only a network
 * window in memory. Supports length, chunked and connection-close framing. */
static void stream_body(struct mg_connection *c, Fetch *f)
{
	while (c->recv.len && !f->done) {
		if (f->chunked && !f->chunk_remaining) {
			if (f->chunk_end) {
				if (c->recv.len < 2) return;
				if (memcmp(c->recv.buf, "\r\n", 2)) goto invalid;
				mg_iobuf_del(&c->recv, 0, 2);
				f->chunk_end = 0;
			}
			size_t i = 0, value = 0, digits = 0;
			while (i + 1 < c->recv.len && !(c->recv.buf[i] == '\r' && c->recv.buf[i+1] == '\n')) i++;
			if (i + 1 >= c->recv.len) { if (c->recv.len > 128) goto invalid; return; }
			if (i > 128) goto invalid;
			for (size_t j = 0; j < i && c->recv.buf[j] != ';'; j++) {
				unsigned char ch = c->recv.buf[j];
				unsigned d = ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : 16;
				if (d > 15 || value > f->limit/16) goto invalid;
				value = value * 16 + d; digits++;
			}
			if (!digits) goto invalid;
			mg_iobuf_del(&c->recv, 0, i+2);
			if (!value) { f->done=1; c->is_closing=1; return; }
			if (value > f->limit-f->size) goto too_large;
			f->chunk_remaining=value;
		}
		size_t count=c->recv.len;
		if (f->chunked && count > f->chunk_remaining) count=f->chunk_remaining;
		if (!f->chunked && count > f->remaining) count=f->remaining;
		if (count > f->limit-f->size) goto too_large;
		if (count && fwrite(c->recv.buf,1,count,f->stream) != count) { f->status=503; goto stop; }
		f->size += count;
		mg_iobuf_del(&c->recv,0,count);
		if (f->chunked) { f->chunk_remaining-=count; if (!f->chunk_remaining) f->chunk_end=1; }
		else if (f->remaining != (size_t)-1) {
			f->remaining-=count;
			if (!f->remaining) { f->done=1; c->is_closing=1; return; }
		}
	}
	return;
too_large:
	f->status=413; goto stop;
invalid:
	f->status=502;
stop:
	f->done=1; c->is_closing=1;
}

/** Receive bounded HTTP data with the existing Mongoose client implementation.
 */
static void fetch_event(struct mg_connection *c, int event, void *data)
{
	Fetch *f = c->fn_data;
	if (f->detached) {
		if (event == MG_EV_READ && !f->done) stream_body(c,f);
		if ((event == MG_EV_ERROR || event == MG_EV_CLOSE) && !f->done) {
			if (event == MG_EV_ERROR || f->chunked || f->remaining != (size_t)-1) f->status=502;
			f->done=1;
		}
		return;
	}
	if (event == MG_EV_READ && c->recv.len > SVG_LIMIT + 16384u) {
		f->status = 413;
		f->done = 1;
		c->is_closing = 1;
	} else if (event == MG_EV_HTTP_HDRS) {
		struct mg_http_message *m = data;
		struct mg_str *length = mg_http_get_header(m, "Content-Length");
		if (length && header_size(*length) > (f->stream ? f->limit : SVG_LIMIT)) {
			f->status = 413;
			f->done = 1;
			c->is_closing = 1;
		}
		if (f->stream && !f->done && mg_http_status(m)==200) {
			struct mg_str *encoding=mg_http_get_header(m,"Transfer-Encoding");
			if (encoding && !equal(*encoding,"chunked")) { f->status=502; f->done=1; c->is_closing=1; return; }
			f->status=200;
			f->remaining=length ? header_size(*length) : (size_t)-1;
			f->chunked=encoding != NULL;
			f->detached=1;
			mg_iobuf_del(&c->recv,0,(size_t)(m->body.buf-m->message.buf));
			stream_body(c,f);
			if (!f->chunked && !f->remaining) { f->done=1; c->is_closing=1; }
		}
	} else if (event == MG_EV_HTTP_MSG && !f->done) {
		struct mg_http_message *m = data;
		struct mg_str *location = mg_http_get_header(m, "Location");
		struct mg_str *capability =
			mg_http_get_header(m, "X-SB-Public-Fetch");
		f->status = (unsigned)mg_http_status(m);
		f->capability = capability && equal(*capability, "1");
		if (location && location->len < sizeof f->location)
			snprintf(f->location,
				 sizeof f->location,
				 "%.*s",
				 (int)location->len,
				 location->buf);
		if (m->body.len > SVG_LIMIT)
			f->status = 413;
		else if (f->status == 200) {
			f->body = malloc(m->body.len + 1);
			if (!f->body)
				f->status = 503;
			else {
				memcpy(f->body, m->body.buf, m->body.len);
				f->body[m->body.len] = 0;
				f->size = m->body.len;
			}
		}
		f->done = 1;
		c->is_closing = 1;
	} else if (event == MG_EV_ERROR || event == MG_EV_CLOSE) {
		if (!f->done) {
			f->status = 502;
			f->done = 1;
		}
	}
}

/** Fetch once over plain HTTP; TLS is exclusively the existing proxy's job. */
static Fetch fetch_once(const char *address,
			const char *host,
			const char *path,
			int public_only,
			uint64_t deadline, const char *file)
{
	struct mg_mgr mgr;
	Fetch f = {0};
	if (file) {
		f.limit=job.input_limit;
		int fd=open(file,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
		if (fd < 0) { f.status=503; return f; }
		f.stream=fdopen(fd,"wb");
		if (!f.stream) { close(fd); f.status=503; return f; }
	}
	mg_mgr_init(&mgr);
	struct mg_connection *c =
		mg_http_connect(&mgr, address, fetch_event, &f);
	if (!c) {
		f.status = 503;
		f.done = 1;
	} else
		mg_printf(
			c,
			"GET %s HTTP/1.1\r\nHost: %s\r\nAccept: "
			"image/png,image/jpeg,image/svg+xml\r\n"
			"User-Agent: StandaloneBaseSVG/1.0 "
			"(+https://github.com/ssbrandsma/SBStandaloneBase)\r\n"
			"Accept-Encoding: identity\r\n%sConnection: "
			"close\r\n\r\n",
			path,
			host,
			public_only ? "X-SB-Public-Fetch: 1\r\n" : "");
	while (!f.done && mg_millis() < deadline)
		mg_mgr_poll(&mgr, 20);
	if (!f.done) {
		f.status = 504;
		f.done = 1;
	}
	mg_mgr_free(&mgr);
	if (f.stream && fclose(f.stream)) f.status=503;
	f.stream=NULL;
	return f;
}

/** Download with per-hop validation and a single overall eight-second deadline.
 */
static Fetch download(ImageUrl u, const char *file)
{
	uint64_t deadline = mg_millis() + 8000;
	Fetch f = {0};
	for (unsigned hop = 0; hop <= 3; hop++) {
		char ip[INET_ADDRSTRLEN], address[128], path[URL_LIMIT + 16];
		if (!resolve_public(&u, ip)) {
			f.status = 403;
			return f;
		}
		if (u.https) {
			const char *proxy = getenv("SB_IMAGE_PROXY_URL");
			if (!proxy)
				proxy = "http://127.0.0.1:8765";
			/* Require the opt-in SSRF guard before sending a source
			 * URL. */
			f = fetch_once(
				proxy, "127.0.0.1", "/health", 0, deadline, NULL);
			free(f.body);
			f.body = NULL;
			if (f.status != 200 || !f.capability) {
				f.status = 503;
				return f;
			}
			if (snprintf(path,
				     sizeof path,
				     "/https/%s%s",
				     u.host,
				     u.path) >= (int)sizeof path) {
				f.status = 400;
				return f;
			}
			return fetch_once(
				proxy, "127.0.0.1", path, 1, deadline, file);
		}
		unsigned port = 80;
#ifdef SB_IMAGE_TESTING
		if (!strcmp(u.host, "image.test") &&
		    getenv("SB_IMAGE_TEST_PORT"))
			port = (unsigned)atoi(getenv("SB_IMAGE_TEST_PORT"));
#endif
		snprintf(address, sizeof address, "http://%s:%u", ip, port);
		f = fetch_once(address, u.host, u.path, 0, deadline, file);
		if (f.status < 300 || f.status >= 400)
			return f;
		if (hop == 3 || !f.location[0]) {
			f.status = 502;
			return f;
		}
		char next[URL_LIMIT];
		int length;
		if (f.location[0] == '/' && f.location[1] != '/')
			length = snprintf(next,
					  sizeof next,
					  "%s://%s%s",
					  u.https ? "https" : "http",
					  u.host,
					  f.location);
		else
			length = snprintf(next, sizeof next, "%s", f.location);
		if (length < 0 || length >= (int)sizeof next) {
			f.status = 403;
			return f;
		}
		if (!parse_url(next, &u)) {
			f.status = 403;
			return f;
		}
	}
	f.status = 502;
	return f;
}

/** Select extended storage only, with an explicit host-test override. */
static int cache_directory(char *path, size_t capacity)
{
	const char *override = getenv("SB_IMAGE_CACHE_ROOT");
	if (override && *override) {
		if (strlen(override) >= capacity)
			return 0;
		strcpy(path, override);
	} else {
		FILE *f = fopen("/proc/mounts", "r");
		char line[512], dev[128], mount[128], type[32];
		int found = 0;
		if (!f)
			return 0;
		while (fgets(line, sizeof line, f))
			if (sscanf(line,
				   "%127s %127s %31s",
				   dev,
				   mount,
				   type) == 3 &&
			    !strcmp(mount, "/mnt/sbdata") &&
			    (!strcmp(type, "ubifs") ||
			     !strcmp(type, "sbubifs")))
				found = 1;
		fclose(f);
		if (!found)
			return 0;
		if (mkdir("/mnt/sbdata/cache", 0700) && errno != EEXIST)
			return 0;
		struct stat parent;
		if (lstat("/mnt/sbdata/cache", &parent) ||
		    !S_ISDIR(parent.st_mode))
			return 0;
		snprintf(path, capacity, "/mnt/sbdata/cache/svg-render");
	}
	if (mkdir(path, 0700) && errno != EEXIST)
		return 0;
	struct stat st;
	return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

/** Read a bounded regular PNG file; never follow a cached symlink. */
static unsigned char *read_png(const char *path, size_t *size)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	unsigned char *data = NULL;
	*size = 0;
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 33 ||
	    st.st_size > PNG_LIMIT)
		goto done;
	data = malloc((size_t)st.st_size);
	if (!data)
		goto done;
	while (*size < (size_t)st.st_size) {
		ssize_t n = read(fd, data + *size, (size_t)st.st_size - *size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		*size += (size_t)n;
	}
	if (*size != (size_t)st.st_size ||
	    memcmp(data, "\211PNG\r\n\032\n", 8)) {
		free(data);
		data = NULL;
	}
done:
	close(fd);
	return data;
}

typedef struct {
	char name[69];
	time_t time;
	off_t size;
} CacheEntry;

/** Sort cached PNGs oldest-first without touching their access timestamps. */
static int oldest_first(const void *a, const void *b)
{
	const CacheEntry *x = a, *y = b;
	return x->time < y->time ? -1 : x->time > y->time;
}

/** Evict oldest entries before atomic publication; cache failures are nonfatal.
 */
static void cache_publish(const char *directory,
			  const char *target,
			  const unsigned char *data,
			  size_t size)
{
	CacheEntry entries[256];
	unsigned count = 0;
	size_t total = 0;
	char path[1024], pending[1024];
	struct dirent *entry;
	DIR *dir = opendir(directory);
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		struct stat st;
		if (strlen(entry->d_name) != 68 ||
		    strcmp(entry->d_name + 64, ".png") ||
		    strspn(entry->d_name, "0123456789abcdef") != 64)
			continue;
		if (count == 256) {
			closedir(dir);
			return;
		}
		snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
		if (lstat(path, &st) || !S_ISREG(st.st_mode))
			continue;
		strcpy(entries[count].name, entry->d_name);
		entries[count].time = st.st_mtime;
		entries[count].size = st.st_size;
		total += (size_t)st.st_size;
		count++;
	}
	closedir(dir);
	qsort(entries, count, sizeof entries[0], oldest_first);
	for (unsigned i = 0; i < count; i++) {
		if (total + size <= CACHE_LIMIT && count - i < 256 &&
		    time(NULL) - entries[i].time <= CACHE_TTL)
			break;
		snprintf(path,
			 sizeof path,
			 "%s/%.68s",
			 directory,
			 entries[i].name);
		if (!unlink(path))
			total -= (size_t)entries[i].size;
	}
	if (total + size > CACHE_LIMIT)
		return;
	snprintf(pending, sizeof pending, "%s/.pending", directory);
	struct stat pending_stat;
	if (!lstat(pending, &pending_stat) && S_ISREG(pending_stat.st_mode) &&
	    time(NULL) - pending_stat.st_mtime > 60)
		unlink(pending);
	int fd = open(pending, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0)
		return;
	size_t written = 0;
	while (written < size) {
		ssize_t n = write(fd, data + written, size - written);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		written += (size_t)n;
	}
	int closed = close(fd);
	if (written != size || closed || rename(pending, target))
		unlink(pending);
}

/** Write a complete pipe/file payload, retrying interrupted system calls. */
static int write_all(int fd, const void *data, size_t size)
{
	const unsigned char *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return 0;
		p += n;
		size -= (size_t)n;
	}
	return 1;
}

/** Execute local-file conversion without a shell; watchdog kills the whole
 * group. */
static unsigned
convert_file(const char *input, const char *output, int width, int height)
{
	char executable[1024], w[16], h[16];
	const char *override = getenv("SB_IMAGE_RENDERER");
	if (override)
		snprintf(executable, sizeof executable, "%s", override);
	else {
		ssize_t n = readlink(
			"/proc/self/exe", executable, sizeof executable - 1);
		if (n <= 0)
			return 503;
		executable[n] = 0;
		char *slash = strrchr(executable, '/');
		if (!slash ||
		    (size_t)(slash - executable) + sizeof "/svgpngrenderer" >=
			    sizeof executable)
			return 503;
		strcpy(slash + 1, "svgpngrenderer");
	}
	snprintf(w, sizeof w, "%d", width);
	snprintf(h, sizeof h, "%d", height);
	pid_t pid = fork();
	if (pid < 0)
		return 503;
	if (!pid) {
		execl(executable,
		      executable,
		      "--input",
		      input,
		      "--output",
		      output,
		      "--width",
		      w,
		      "--height",
		      h,
		      (char *)NULL);
		_exit(127);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return 503;
	if (!WIFEXITED(status))
		return 503;
	return WEXITSTATUS(status) == 0	   ? 200
	       : WEXITSTATUS(status) == 65 ? 415
					   : 503;
}

/** Run all blocking download, filesystem, cache and conversion work off-loop.
 */
static void worker(int pipe_fd,
		   ImageUrl *source,
		   const char *body,
		   size_t body_size,
		   int width,
		   int height)
{
	Result result = {200, 0, 0};
	char directory[768] = "", target[1024], input[180], output[180];
	unsigned char *png = NULL;
	size_t png_size = 0;
	struct stat st;
	Fetch fetched = {0};
	int cache = cache_directory(directory, sizeof directory);
	snprintf(target, sizeof target, "%s/%s.png", directory, job.key);
	snprintf(input, sizeof input, "%s/input.svg", job.directory);
	snprintf(output, sizeof output, "%s/output.png", job.directory);
	if (cache && !lstat(target, &st) && S_ISREG(st.st_mode) &&
	    time(NULL) >= st.st_mtime && time(NULL) - st.st_mtime < CACHE_TTL) {
		png = read_png(target, &png_size);
		if (png) {
			result.cached = 1;
			goto send;
		}
	}
	fprintf(stderr,
		"[image] cache miss; %s host=%s requested=%dx%d input_limit=%u\n",
		body ? "local" : "remote",
		body ? "upload" : source->host,
		width,
		height, IMAGE_LIMIT);
	if (!body) {
		fprintf(stderr, "[svg-image] download start\n");
		fetched = download(*source, input);
		if (fetched.status != 200 || !fetched.size) {
			result.status = fetched.status == 403 ||
							fetched.status == 413 ||
							fetched.status == 503 ||
							fetched.status == 504
						? fetched.status
						: 502;
			goto send;
		}
		body_size = fetched.size;
		fprintf(stderr,
			"[svg-image] download complete %zu bytes\n",
			body_size);
	}
	if (body) {
		int fd = open(input, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (fd < 0) {
			result.status = 503;
			goto send;
		}
		int ok = write_all(fd, body, body_size);
		if (close(fd) || !ok) {
			result.status = 503;
			goto send;
		}
	}
	result.status = convert_file(input, output, width, height);
	if (result.status != 200)
		goto send;
	png = read_png(output, &png_size);
	if (!png) {
		result.status = 503;
		goto send;
	}
	if (cache)
		cache_publish(directory, target, png, png_size);
send:
	result.size = png ? (unsigned)png_size : 0;
	if (write_all(pipe_fd, &result, sizeof result) && png)
		write_all(pipe_fd, png, png_size);
	if (result.status != 200 || getenv("SB_IMAGE_DIAGNOSTICS"))
		fprintf(stderr,
			"[svg-image] result=%u cache=%u bytes=%u\n",
			result.status,
			result.cached,
			result.size);
	free(fetched.body);
	free(png);
}

/** Restrict unpaired loopback access to non-browser, literal-local-host
 * clients. */
static int local_artwork(struct mg_connection *c, struct mg_http_message *m)
{
	struct mg_str *host = mg_http_get_header(m, "Host");
	if (c->rem.is_ip6 || memcmp(c->rem.ip, "\177\0\0\1", 4) || !host ||
	    mg_http_get_header(m, "Origin") ||
	    mg_http_get_header(m, "Sec-Fetch-Site"))
		return 0;
	if (equal(*host, "127.0.0.1") || equal(*host, "localhost"))
		return 1;
	if (host->len <= 10 || host->len > 15 ||
	    memcmp(host->buf, "127.0.0.1:", 10))
		return 0;
	for (size_t i = 10; i < host->len; i++)
		if (!isdigit((unsigned char)host->buf[i]))
			return 0;
	return 1;
}

/** Enforce upload body size before Mongoose buffers a complete request. */
int image_render_headers(struct mg_connection *c, struct mg_http_message *m)
{
	if (!equal(m->uri, "/api/image/render"))
		return 0;
	struct mg_str *length = mg_http_get_header(m, "Content-Length");
	if ((equal(m->method, "POST") && !length) ||
	    (length && header_size(*length) > SVG_LIMIT)) {
		image_error(c, length ? 413 : 400);
		return 1;
	}
	if (mg_http_get_header(m, "Transfer-Encoding")) {
		image_error(c, 400);
		return 1;
	}
	return 0;
}

/** Validate parameters and start or coalesce one bounded image job. */
int image_render_handle(struct mg_connection *c, struct mg_http_message *m)
{
	ImageUrl source;
	char url[URL_LIMIT], w[16], h[16], key[65];
	unsigned char digest[32];
	int post = equal(m->method, "POST"), head = equal(m->method, "HEAD");
	if (!equal(m->uri, "/api/image/render"))
		return 0;
	if (!equal(m->method, "GET") && !head && !post) {
		image_error(c, 405);
		return 1;
	}
	int session = auth_session_ok(m, NULL);
	if (!session &&
	    !(post ? remote_internal_authorized(c, m) : local_artwork(c, m))) {
		image_error(c, 401);
		return 1;
	}
	if (!auth_origin_ok(m)) {
		image_error(c, 403);
		return 1;
	}
	if (m->body.len > SVG_LIMIT) {
		image_error(c, 413);
		return 1;
	}
	int wr = mg_http_get_var(&m->query, "width", w, sizeof w);
	int hr = mg_http_get_var(&m->query, "height", h, sizeof h);
	if (wr == -1 || wr == -4)
		strcpy(w, "320");
	if (hr == -1 || hr == -4)
		strcpy(h, "320");
	if (wr == -3 || hr == -3 || (wr >= 0 && (size_t)wr != strlen(w)) ||
	    (hr >= 0 && (size_t)hr != strlen(h)) || !*w || !*h ||
	    strspn(w, "0123456789") != strlen(w) ||
	    strspn(h, "0123456789") != strlen(h)) {
		image_error(c, 400);
		return 1;
	}
	long width = strtol(w, NULL, 10), height = strtol(h, NULL, 10);
	if (width < 16 || width > 512 || height < 16 || height > 512) {
		image_error(c, 400);
		return 1;
	}
	if (post) {
		struct mg_str *type = mg_http_get_header(m, "Content-Type");
		if (!type || !equal(*type, "image/svg+xml") || !m->body.len) {
			image_error(c, 400);
			return 1;
		}
		memset(&source, 0, sizeof source);
	} else {
		int length = mg_http_get_var(&m->query, "url", url, sizeof url);
		if (length <= 0 || (size_t)length != strlen(url) ||
		    !parse_url(url, &source)) {
			image_error(c, 400);
			return 1;
		}
	}
	mg_sha256_ctx hash;
	mg_sha256_init(&hash);
	const char *version = "image-v2:area-fit-no-upscale:";
	mg_sha256_update(
		&hash, (const unsigned char *)version, strlen(version));
	mg_sha256_update(&hash,
			 (const unsigned char *)(post ? "content:" : "url:"),
			 post ? 8 : 4);
	if (post)
		mg_sha256_update(
			&hash, (const unsigned char *)m->body.buf, m->body.len);
	else
		mg_sha256_update(&hash,
				 (const unsigned char *)source.normalized,
				 strlen(source.normalized));
	char dimensions[32];
	int n = snprintf(
		dimensions, sizeof dimensions, ":%ld:%ld", width, height);
	mg_sha256_update(&hash, (unsigned char *)dimensions, (size_t)n);
	mg_sha256_final(digest, &hash);
	for (unsigned i = 0; i < 32; i++)
		snprintf(key + i * 2, 3, "%02x", digest[i]);
	if (job.pid) {
		if (strcmp(key, job.key) || job.clients_count == WAITER_LIMIT) {
			image_error(c, 503);
			return 1;
		}
	} else {
		int pipes[2];
		job.data = malloc(PNG_LIMIT + sizeof(Result));
		if (!job.data) {
			image_error(c, 503);
			return 1;
		}
		/* Large compressed originals must not occupy the Radio's ramfs.
		 * Use the existing verified extended-storage cache for private jobs. */
		char scratch[128];
		job.input_limit=512u*1024u;
		strcpy(job.directory, "/tmp/sb-svg.XXXXXX");
		if (cache_directory(scratch,sizeof scratch) &&
		    strlen(scratch)+sizeof "/job.XXXXXX" < sizeof job.directory) {
			snprintf(job.directory,sizeof job.directory,"%s/job.XXXXXX",scratch);
			job.input_limit=IMAGE_LIMIT;
		}
		if (!mkdtemp(job.directory)) {
			free(job.data);
			job.data = NULL;
			image_error(c, 503);
			return 1;
		}
		strcpy(job.key, key);
		if (pipe(pipes)) {
			rmdir(job.directory);
			free(job.data);
			job.data = NULL;
			image_error(c, 503);
			return 1;
		}
		job.pid = fork();
		if (!job.pid) {
			setpgid(0, 0);
			close(pipes[0]);
			/* Close inherited listeners/clients; only the private
			 * result pipe survives exec. */
			for (struct mg_connection *p = c->mgr->conns; p;
			     p = p->next)
				if (p->fd && (int)(intptr_t)p->fd != pipes[1])
					close((int)(intptr_t)p->fd);
			fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
			struct rlimit limit = {40u * 1024u * 1024u,
					       40u * 1024u * 1024u},
				      core = {0, 0};
			if (setrlimit(RLIMIT_AS, &limit) ||
			    setrlimit(RLIMIT_CORE, &core))
				_exit(75);
			mg_log_set(0);
			worker(pipes[1],
			       &source,
			       post ? m->body.buf : NULL,
			       m->body.len,
			       (int)width,
			       (int)height);
			close(pipes[1]);
			_exit(0);
		}
		close(pipes[1]);
		if (job.pid < 0) {
			close(pipes[0]);
			job.pid = 0;
			free(job.data);
			job.data = NULL;
			rmdir(job.directory);
			image_error(c, 503);
			return 1;
		}
		setpgid(job.pid, job.pid);
		job.pipe = pipes[0];
		fcntl(job.pipe, F_SETFL, O_NONBLOCK);
		job.deadline = mg_millis() + 20000;
	}
	job.clients[job.clients_count] = c->id;
	job.head[job.clients_count++] = head;
	c->is_resp = 1;
	return 1;
}

/** Drain bounded worker results, reap children and deliver PNGs on the main
 * loop. */
void image_render_tick(struct mg_mgr *mgr)
{
	if (!job.pid)
		return;
	int ended = 0, timed_out = mg_millis() >= job.deadline;
	for (unsigned i = 0;
	     i < 16 && job.received < PNG_LIMIT + sizeof(Result);
	     i++) {
		ssize_t n = read(
			job.pipe,
			job.data + job.received,
			PNG_LIMIT + sizeof(Result) - job.received > 16384
				? 16384
				: PNG_LIMIT + sizeof(Result) - job.received);
		if (n > 0)
			job.received += (size_t)n;
		else {
			if (!n || (errno != EAGAIN && errno != EINTR))
				ended = 1;
			break;
		}
	}
	if (timed_out) {
		fprintf(stderr, "[svg-image] worker deadline exceeded\n");
		kill(-job.pid, SIGKILL);
		ended = 1;
	}
	if (!ended)
		return;
	int status;
	if (waitpid(job.pid, &status, WNOHANG) == 0)
		return;
	Result result = {timed_out ? 504u : 503u, 0, 0};
	if (!timed_out && job.received >= sizeof result) {
		memcpy(&result, job.data, sizeof result);
		if (result.size > PNG_LIMIT ||
		    job.received != sizeof result + result.size)
			result.status = 503;
	}
	if (result.status != 200 && !timed_out)
		fprintf(stderr,
			"[svg-image] job failed status=%u\n",
			result.status);
	for (unsigned i = 0; i < job.clients_count; i++) {
		for (struct mg_connection *c = mgr->conns; c; c = c->next) {
			if (c->id != job.clients[i] || c->is_closing)
				continue;
			if (result.status != 200)
				image_error(c, result.status);
			else {
				mg_printf(c,
					  "HTTP/1.1 200 OK\r\nContent-Type: "
					  "image/png\r\nContent-Length: %u\r\n"
					  "Cache-Control: private, "
					  "max-age=86400\r\n"
					  "X-SB-Image-Cache: "
					  "%s\r\nX-Content-Type-Options: "
					  "nosniff\r\n"
					  "Referrer-Policy: "
					  "no-referrer\r\nConnection: "
					  "close\r\n\r\n",
					  result.size,
					  result.cached ? "hit" : "miss");
				if (!job.head[i])
					mg_send(c,
						job.data + sizeof result,
						result.size);
				c->is_resp = 0;
				c->is_draining = 1;
			}
		}
	}
	DIR *dir = opendir(job.directory);
	struct dirent *entry;
	if (dir) {
		while ((entry = readdir(dir))) {
			char path[512];
			if (!strcmp(entry->d_name, ".") ||
			    !strcmp(entry->d_name, ".."))
				continue;
			snprintf(path,
				 sizeof path,
				 "%s/%s",
				 job.directory,
				 entry->d_name);
			unlink(path);
		}
		closedir(dir);
	}
	rmdir(job.directory);
	close(job.pipe);
	free(job.data);
	memset(&job, 0, sizeof job);
}
