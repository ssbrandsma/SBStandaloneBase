/* Loopback-only HLS metadata snapshots/artwork. Reuses sbproxy workers; two
 * bounded session slots, eight ordered events each, RAM only, no owner claims.
 */
#define _POSIX_C_SOURCE 200809L
#include "hls_metadata.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

typedef struct {
	char id[33];
	uint64_t revision;
	unsigned count, next;
	time_t finished;
	hls_metadata events[HLS_EVENTS];
} metadata_session;

static metadata_session *sessions[2];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	char id[33];
	int read_fd, write_fd;
} metadata_waiter;

static metadata_waiter *waiters[2];

/** Signal bounded registered waits; lock held, writes never block audio. */
static void notify(const char *id)
{
	for (unsigned i = 0; i < 2; i++)
		if (waiters[i] && !strncmp(waiters[i]->id, id, 32)) {
			unsigned char byte = 1;
			ssize_t result = write(waiters[i]->write_fd, &byte, 1);
			(void)result;
		}
}

/** Bound natural-EOF retention without a cleanup thread or wall-clock timers.
 */
static time_t monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec;
}

/** Expire finished slots on demand; caller holds the registry mutex. */
static void expire_finished(int reclaim)
{
	time_t now = monotonic_seconds();
	for (unsigned i = 0; i < 2; i++)
		if (sessions[i] && sessions[i]->finished &&
		    (reclaim || now - sessions[i]->finished >= 120)) {
			notify(sessions[i]->id);
			free(sessions[i]);
			sessions[i] = NULL;
		}
}

/** Allocate a session only with kernel-generated unpredictable identity. */
void *hls_metadata_open(char id[33])
{
	unsigned char random[16];
	size_t used = 0;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return NULL;
	while (used < sizeof random) {
		ssize_t n = read(fd, random + used, sizeof random - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			return NULL;
		}
		used += (size_t)n;
	}
	close(fd);
	metadata_session *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	for (unsigned i = 0; i < 16; i++)
		snprintf(s->id + i * 2, 3, "%02x", random[i]);
	pthread_mutex_lock(&lock);
	expire_finished(1);
	for (unsigned i = 0; i < 2; i++)
		if (!sessions[i]) {
			sessions[i] = s;
			memcpy(id, s->id, 33);
			pthread_mutex_unlock(&lock);
			return s;
		}
	pthread_mutex_unlock(&lock);
	free(s);
	return NULL;
}

/** Append one ordered event; duplicates are suppressed independent of timing.
 */
void hls_metadata_publish(void *opaque,
			  const hls_metadata *event,
			  uint64_t position_ms)
{
	metadata_session *s = opaque;
	if (!s ||
	    (!event->fields && !event->art_size && !event->artwork_url[0]))
		return;
	pthread_mutex_lock(&lock);
	if (s->count) {
		hls_metadata *previous =
			&s->events[(s->next + HLS_EVENTS - 1) % HLS_EVENTS];
		if (previous->fields == event->fields &&
		    !strcmp(previous->title, event->title) &&
		    !strcmp(previous->artist, event->artist) &&
		    !strcmp(previous->album, event->album) &&
		    !strcmp(previous->artwork_url, event->artwork_url) &&
		    previous->art_size == event->art_size &&
		    previous->art_type == event->art_type &&
		    !memcmp(previous->art, event->art, event->art_size)) {
			pthread_mutex_unlock(&lock);
			return;
		}
	}
	hls_metadata *dest = &s->events[s->next];
	*dest = *event;
	dest->revision = ++s->revision;
	dest->position_ms = position_ms;
	s->next = (s->next + 1) % HLS_EVENTS;
	if (s->count < HLS_EVENTS)
		s->count++;
	notify(s->id);
	pthread_mutex_unlock(&lock);
}

/** Retire identity/artwork atomically before freeing the bounded session. */
void hls_metadata_close(void *opaque)
{
	if (!opaque)
		return;
	pthread_mutex_lock(&lock);
	notify(((metadata_session *)opaque)->id);
	for (unsigned i = 0; i < 2; i++)
		if (sessions[i] == opaque)
			sessions[i] = NULL;
	pthread_mutex_unlock(&lock);
	free(opaque);
}

/** Retain natural-EOF events briefly for audio already buffered in Jive.
 * New streams may reclaim the slot; cancellation never enters this state. */
void hls_metadata_finish(void *opaque)
{
	if (!opaque)
		return;
	pthread_mutex_lock(&lock);
	((metadata_session *)opaque)->finished = monotonic_seconds();
	notify(((metadata_session *)opaque)->id);
	pthread_mutex_unlock(&lock);
}

/** JSON-escape already validated UTF-8 text into a caller-owned buffer. */
static size_t quoted(char *out, const char *text)
{
	size_t n = 0;
	out[n++] = '"';
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p == '"' || *p == '\\')
			out[n++] = '\\';
		if (*p >= 32)
			out[n++] = (char)*p;
	}
	out[n++] = '"';
	out[n] = 0;
	return n;
}

/** Send only a small bounded response outside the registry mutex. */
static void response(int fd,
		     int status,
		     const char *mime,
		     const void *body,
		     size_t size,
		     int head)
{
	char header[256];
	int n = snprintf(
		header,
		sizeof header,
		"HTTP/1.1 %d Metadata\r\nContent-Type: %s\r\nContent-Length: "
		"%zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: "
		"nosniff\r\nX-SB-Metadata-Wait: 1\r\nConnection: close\r\n\r\n",
		status,
		mime,
		size);
	if (send(fd, header, (size_t)n, MSG_NOSIGNAL) < 0 || head)
		return;
	const unsigned char *p = body;
	while (size) {
		ssize_t sent = send(fd, p, size, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			return;
		p += sent;
		size -= (size_t)sent;
	}
}

/** Serve the next event/artwork only for an active opaque session identity.
 * /hls-meta/<id>?after=N and /hls-meta/<id>/art/<revision> are read-only.
 */
int hls_metadata_http(int fd, const char *target, int head)
{
	if (strncmp(target, "/hls-meta/", 10))
		return 0;
	const char *id = target + 10;
	unsigned i = 0;
	while (i < 32 && ((id[i] >= '0' && id[i] <= '9') ||
			  (id[i] >= 'a' && id[i] <= 'f')))
		i++;
	if (i != 32 || (id[32] && id[32] != '?' && id[32] != '/')) {
		response(fd, 400, "application/json", "{}", 2, head);
		return 1;
	}
	int art = !strncmp(id + 32, "/art/", 5);
	uint64_t requested = 0;
	unsigned wait = 0;
	if (id[32]) {
		const char *number = art			       ? id + 37
				     : !strncmp(id + 32, "?after=", 7) ? id + 39
								       : NULL;
		size_t digits = number ? strspn(number, "0123456789") : 0;
		const char *tail = number ? number + digits : NULL;
		int valid = digits && digits <= 16;
		if (valid && *tail && !art && !strncmp(tail, "&wait=", 6)) {
			const char *value = tail + 6;
			size_t length = strspn(value, "0123456789");
			valid = length && length <= 2 && !value[length];
			if (valid) {
				wait = (unsigned)strtoul(value, NULL, 10);
				valid = wait <= 30;
			}
		} else if (valid && *tail)
			valid = 0;
		if (!valid) {
			response(fd, 400, "application/json", "{}", 2, head);
			return 1;
		}
		requested = strtoull(number, NULL, 10);
	}
	/* Allocate a response copy only for an actual event/artwork, not for
	 * idle waits. Network backpressure stays outside the registry lock. */
	hls_metadata *copy = NULL;
	int found = 0, active = 0, finished = 0, allocation_failed = 0;
	metadata_waiter waiter = {{0}, -1, -1};
	int registered = -1;
	int was_waiting = 0;
lookup:
	found = active = finished = 0;
	pthread_mutex_lock(&lock);
	expire_finished(0);
	for (unsigned j = 0; j < 2; j++) {
		metadata_session *s = sessions[j];
		if (!s || strncmp(s->id, id, 32))
			continue;
		active = 1;
		finished = s->finished != 0;
		for (unsigned k = 0; k < s->count; k++) {
			hls_metadata *e = &s->events[(s->next + HLS_EVENTS -
						      s->count + k) %
						     HLS_EVENTS];
			if (art ? e->revision == requested && e->art_size
				: e->revision > requested) {
				copy = malloc(sizeof *copy);
				if (copy) {
					*copy = *e;
					found = 1;
				} else
					allocation_failed = 1;
				break;
			}
		}
		break;
	}
	if (wait && !head && !art && active && !finished && !found &&
	    !allocation_failed && !was_waiting) {
		int slot = -1, duplicate = 0;
		for (unsigned j = 0; j < 2; j++) {
			if (!waiters[j])
				slot = (int)j;
			else if (!strncmp(waiters[j]->id, id, 32))
				duplicate = 1;
		}
		if (slot < 0 || duplicate) {
			pthread_mutex_unlock(&lock);
			free(copy);
			response(fd, 429, "application/json", "{}", 2, head);
			return 1;
		}
		int pipes[2];
		if (pipe(pipes) < 0) {
			pthread_mutex_unlock(&lock);
			free(copy);
			response(fd, 503, "application/json", "{}", 2, head);
			return 1;
		}
		waiter.read_fd = pipes[0];
		waiter.write_fd = pipes[1];
		int flags = fcntl(pipes[1], F_GETFL);
		if (flags < 0 ||
		    fcntl(pipes[1], F_SETFL, flags | O_NONBLOCK) < 0) {
			close(pipes[0]);
			close(pipes[1]);
			pthread_mutex_unlock(&lock);
			free(copy);
			response(fd, 503, "application/json", "{}", 2, head);
			return 1;
		}
		memcpy(waiter.id, id, 32);
		waiters[slot] = &waiter;
		registered = slot;
	}
	pthread_mutex_unlock(&lock);
	if (allocation_failed) {
		response(fd, 503, "application/json", "{}", 2, head);
		return 1;
	}
	if (registered >= 0) {
		/* No metadata mutex, response-copy work or periodic wakeups
		 * while asleep. Socket EOF wakes cancellation independently of
		 * events. */
		struct pollfd ready[2] = {{fd, POLLIN, 0},
					  {waiter.read_fd, POLLIN, 0}};
		struct timespec start, now;
		clock_gettime(CLOCK_MONOTONIC, &start);
		int result, remaining = (int)wait * 1000;
		do {
			result = poll(ready, 2, remaining);
			clock_gettime(CLOCK_MONOTONIC, &now);
			remaining =
				(int)wait * 1000 -
				(int)((now.tv_sec - start.tv_sec) * 1000 +
				      (now.tv_nsec - start.tv_nsec) / 1000000);
		} while (result < 0 && errno == EINTR && remaining > 0);
		if (result < 0 && errno == EINTR && remaining <= 0) {
			result = 0;
			ready[0].revents = ready[1].revents = 0;
		}
		pthread_mutex_lock(&lock);
		waiters[registered] = NULL;
		pthread_mutex_unlock(&lock);
		close(waiter.read_fd);
		close(waiter.write_fd);
		registered = -1;
		was_waiting = 1;
		if (result < 0 || ready[0].revents) {
			free(copy);
			return 1;
		}
		goto lookup;
	}
	if (!active || (art && !found))
		response(fd, 404, "application/json", "{}", 2, head);
	else if (!found)
		response(fd,
			 200,
			 "application/json",
			 finished ? "{\"pending\":true,\"finished\":true}"
				  : "{\"pending\":true}",
			 finished ? 32 : 16,
			 head);
	else if (art)
		response(fd,
			 200,
			 copy->art_type == 1 ? "image/png" : "image/jpeg",
			 copy->art,
			 copy->art_size,
			 head);
	else {
		char body[8192];
		size_t n = (size_t)snprintf(
			body,
			sizeof body,
			"{\"session\":\"%.32s\",\"revision\":%llu,\"position_"
			"ms\":%llu,\"fields\":%u,\"timestamp_90khz\":",
			id,
			(unsigned long long)copy->revision,
			(unsigned long long)copy->position_ms,
			copy->fields);
		n += (size_t)snprintf(body + n,
				      sizeof body - n,
				      copy->has_timestamp ? "%llu" : "null",
				      (unsigned long long)copy->timestamp);
		n += (size_t)snprintf(
			body + n,
			sizeof body - n,
			",\"dropped\":%llu",
			(unsigned long long)(copy->revision > requested + 1
						     ? copy->revision -
							       requested - 1
						     : 0));
		n += (size_t)snprintf(body + n, sizeof body - n, ",\"title\":");
		n += quoted(body + n, copy->title);
		n += (size_t)snprintf(
			body + n, sizeof body - n, ",\"artist\":");
		n += quoted(body + n, copy->artist);
		n += (size_t)snprintf(body + n, sizeof body - n, ",\"album\":");
		n += quoted(body + n, copy->album);
		if (copy->art_size)
			n += (size_t)snprintf(
				body + n,
				sizeof body - n,
				",\"artwork_url\":\"http://127.0.0.1:8765/"
				"hls-meta/%.32s/art/%llu\"",
				id,
				(unsigned long long)copy->revision);
		else if (copy->artwork_url[0]) {
			/* Reuse public-only artwork transport, not its LAN
			 * bypass. */
			const char *scheme =
				!strncmp(copy->artwork_url, "https://", 8)
					? "https"
					: "http";
			const char *source =
				strstr(copy->artwork_url, "://") + 3;
			char routed[384];
			snprintf(routed,
				 sizeof routed,
				 "http://127.0.0.1:8765/artwork/320/320/80/%s/"
				 "%s",
				 scheme,
				 source);
			n += (size_t)snprintf(
				body + n, sizeof body - n, ",\"artwork_url\":");
			n += quoted(body + n, routed);
		}
		n += (size_t)snprintf(body + n, sizeof body - n, "}");
		response(fd, 200, "application/json", body, n, head);
	}
	free(copy);
	return 1;
}
