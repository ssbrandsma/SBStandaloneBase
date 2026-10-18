/* Loopback HLS audio response, called inside an existing sbproxy worker.
 * Reuses its public-only curl/TLS handle. One bounded manifest, incremental
 * framing and socket backpressure replace any segment cache or transcoder. */
#define _POSIX_C_SOURCE 200809L
#include "hls.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	CURL *curl;
	int fd, head, started, head_done, bad, cancelled, status, container;
	int expected_container, resume_ok, fetching_manifest, failure_status,
		expected_codec;
	uint64_t bytes, offset, resume, segments, retries, skipped;
	uint64_t total, last_audio_ms, started_ms;
	unsigned idle_ms, segment_seconds;
	char *manifest;
	size_t manifest_size;
	hls_audio audio;
	hls_ts ts;
	void *metadata_session;
	char metadata_id[33];
	hls_id3 *metadata, *ts_metadata;
	const char *error;
} relay;

static pthread_mutex_t hls_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned hls_active;

/** Read monotonic milliseconds for startup and starvation deadlines.
 * @return Milliseconds independent of wall-clock/NTP changes.
 */
static uint64_t milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (unsigned)now.tv_nsec / 1000000;
}

/** Test whether the downstream socket is alive without blocking.
 * @param r Relay context. Returns 1 while client remains connected.
 */
static int alive(relay *r)
{
	unsigned char b;
	ssize_t n = recv(r->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
	if (!n || n > 0 ||
	    (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
	     errno != EINTR))
		r->cancelled = 1;
	return !r->cancelled;
}

/** Send all bounded output with existing socket backpressure/timeouts.
 * @param r Relay; data/size bytes. Returns zero or -1 on client failure.
 */
static int send_bytes(relay *r, const void *data, size_t size)
{
	const unsigned char *p = data;
	uint64_t deadline = milliseconds() + 15000;
	while (size) {
		ssize_t n = send(r->fd, p, size, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!alive(r) || milliseconds() >= deadline) {
				fprintf(stderr,
					"[hls] downstream "
					"stalled/disconnected\n");
				r->cancelled = 1;
				return -1;
			}
			struct pollfd ready = {r->fd, POLLOUT | POLLIN, 0};
			if (poll(&ready, 1, 100) < 0 && errno != EINTR) {
				r->cancelled = 1;
				return -1;
			}
			continue;
		}
		if (n <= 0) {
			r->cancelled = 1;
			return -1;
		}
		p += n;
		size -= (size_t)n;
		deadline = milliseconds() + 15000;
	}
	return 0;
}

/** Emit a validated complete frame, creating the local response lazily.
 * @param opaque Relay; frame/size encoded audio; codec AAC or MP3.
 * @return Zero, or -1 to stop on HEAD completion/client cancellation.
 */
static int
output(void *opaque, const unsigned char *frame, size_t size, int codec)
{
	relay *r = opaque;
	if (r->expected_codec && r->expected_codec != codec) {
		r->error = "HLS codec changed since probe";
		return -1;
	}
	if (!r->started) {
		char header[384];
		int n = snprintf(header,
				 sizeof header,
				 "HTTP/1.1 200 OK\r\nContent-Type: "
				 "%s\r\nCache-Control: no-store\r\n"
				 "X-Content-Type-Options: "
				 "nosniff\r\n%s%s%sConnection: close\r\n\r\n",
				 codec == HLS_AAC ? "audio/aac" : "audio/mpeg",
				 r->metadata_session ? "X-SB-HLS-Session: "
						     : "",
				 r->metadata_id,
				 r->metadata_session ? "\r\n" : "");
		if (send_bytes(r, header, (size_t)n))
			return -1;
		r->started = 1;
		fprintf(stderr,
			"[hls] selected codec=%s\n",
			codec == HLS_AAC ? "AAC/ADTS" : "MP3");
	}
	if (r->head) {
		r->head_done = 1;
		return -1;
	}
	if (send_bytes(r, frame, size))
		return -1;
	r->bytes += size;
	r->last_audio_ms = milliseconds();
	return 0;
}

/** CURL progress hook; cancels slow upstream activity when Jive disconnects.
 * @param opaque Relay; remaining parameters are libcurl transfer counters.
 * @return Nonzero to abort a cancelled stream.
 */
static int progress(void *opaque,
		    curl_off_t total,
		    curl_off_t received,
		    curl_off_t upload_total,
		    curl_off_t uploaded)
{
	(void)total;
	(void)received;
	(void)upload_total;
	(void)uploaded;
	relay *r = opaque;
	uint64_t now = milliseconds();
	if ((!r->started && now - r->started_ms > 20000) ||
	    (r->started && now - r->last_audio_ms > r->idle_ms)) {
		r->bad = 1;
		r->failure_status = 504;
		r->error = "HLS startup/audio starvation deadline exceeded";
		return 1;
	}
	return !alive(r);
}

/** Collect response status and validate range-resume headers before any body.
 * @param data/size/count Header bytes; opaque relay. Returns consumed bytes.
 */
static size_t header(char *data, size_t size, size_t count, void *opaque)
{
	relay *r = opaque;
	size_t n = size * count;
	if (n >= 5 && !strncasecmp(data, "HTTP/", 5)) {
		char *space = memchr(data, ' ', n);
		r->status = space ? atoi(space + 1) : 0;
		r->resume_ok = !r->resume;
	}
	if (r->resume && n > 14 && !strncasecmp(data, "Content-Range:", 14)) {
		char text[128];
		unsigned long long begin, end, total;
		if (n < sizeof text) {
			memcpy(text, data, n);
			text[n] = 0;
			if (sscanf(text + 14,
				   " bytes %llu-%llu/%llu",
				   &begin,
				   &end,
				   &total) == 3 &&
			    begin == r->resume && end >= begin && total > end &&
			    (!r->total || r->total == total)) {
				r->resume_ok = 1;
				r->total = total;
			}
		}
	}
	if (!r->fetching_manifest && !r->resume && r->status == 200 && n > 15 &&
	    !strncasecmp(data, "Content-Length:", 15)) {
		char value[64];
		if (n < sizeof value) {
			memcpy(value, data, n);
			value[n] = 0;
			r->total = strtoull(value + 15, NULL, 10);
		}
	}
	return n;
}

/** Accumulate only bounded manifests; redirect/error bodies are discarded.
 * @param data/size/count Input chunk; opaque relay. Returns consumed bytes.
 */
static size_t manifest_body(char *data, size_t size, size_t count, void *opaque)
{
	relay *r = opaque;
	size_t n = size * count;
	if (r->status != 200)
		return n;
	if (n > HLS_MANIFEST - r->manifest_size || memchr(data, 0, n)) {
		r->bad = 1;
		r->error = "oversized or binary HLS playlist";
		return 0;
	}
	memcpy(r->manifest + r->manifest_size, data, n);
	r->manifest_size += n;
	r->manifest[r->manifest_size] = 0;
	return n;
}

/** Stream segment bytes through container detection and incremental framing.
 * @param data/size/count Input chunk; opaque relay. Returns consumed bytes.
 */
static size_t segment_body(char *data, size_t size, size_t count, void *opaque)
{
	relay *r = opaque;
	size_t n = size * count;
	if (r->status != 200 && r->status != 206)
		return n;
	if (n > 16u * 1024u * 1024u - r->offset) {
		r->bad = 1;
		r->error = "oversized HLS segment";
		return 0;
	}
	if ((r->resume && (r->status != 206 || !r->resume_ok)) ||
	    (!r->resume && r->status != 200)) {
		r->bad = 1;
		r->error = "origin did not honor exact segment resume";
		return 0;
	}
	if (!r->container && n) {
		r->container = (unsigned char)data[0] == 71 ? 2 : 1;
		if (r->expected_container &&
		    r->expected_container != r->container) {
			r->bad = 1;
			r->error = "HLS container changed";
			return 0;
		}
		r->expected_container = r->container;
	}
	int result =
		r->container == 2
			? hls_ts_feed(&r->ts, (unsigned char *)data, n)
			: hls_audio_feed(&r->audio, (unsigned char *)data, n);
	if (result) {
		r->bad = !r->head_done && !r->cancelled;
		if (!r->error)
			r->error = r->container == 2 ? r->ts.error
						     : r->audio.error;
		return 0;
	}
	r->offset += n;
	return n;
}

/** Configure one bounded transfer using sbproxy's existing TLS/socket policy.
 * @param r Relay; url validated source; manifest selects body handling.
 * @return libcurl result; status and parser failures remain in r.
 */
static CURLcode fetch(relay *r, const char *url, int manifest)
{
	r->status = 0;
	r->bad = 0;
	r->fetching_manifest = manifest;
	if (manifest) {
		r->manifest_size = 0;
		r->manifest[0] = 0;
		r->resume = 0;
	}
	curl_easy_setopt(r->curl, CURLOPT_URL, url);
	curl_easy_setopt(r->curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(r->curl, CURLOPT_NOBODY, 0L);
	curl_easy_setopt(r->curl, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(r->curl, CURLOPT_USERAGENT, "StandaloneBaseHLS/1.0");
	curl_easy_setopt(r->curl, CURLOPT_ACCEPT_ENCODING, "identity");
	curl_easy_setopt(r->curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
	curl_easy_setopt(r->curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
	curl_easy_setopt(r->curl, CURLOPT_HEADERFUNCTION, header);
	curl_easy_setopt(r->curl, CURLOPT_HEADERDATA, r);
	curl_easy_setopt(r->curl,
			 CURLOPT_WRITEFUNCTION,
			 manifest ? manifest_body : segment_body);
	curl_easy_setopt(r->curl, CURLOPT_WRITEDATA, r);
	curl_easy_setopt(r->curl, CURLOPT_XFERINFOFUNCTION, progress);
	curl_easy_setopt(r->curl, CURLOPT_XFERINFODATA, r);
	curl_easy_setopt(r->curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(r->curl,
			 CURLOPT_TIMEOUT,
			 manifest ? 8L : (long)r->segment_seconds);
	curl_easy_setopt(r->curl, CURLOPT_CONNECTTIMEOUT, 3L);
	curl_easy_setopt(r->curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(r->curl, CURLOPT_LOW_SPEED_TIME, 8L);
	curl_easy_setopt(
		r->curl,
		CURLOPT_MAXFILESIZE_LARGE,
		(curl_off_t)(manifest ? HLS_MANIFEST : 16u * 1024u * 1024u));
	char range[64];
	if (!manifest && r->resume)
		snprintf(range,
			 sizeof range,
			 "%llu-",
			 (unsigned long long)r->resume);
	curl_easy_setopt(
		r->curl, CURLOPT_RANGE, !manifest && r->resume ? range : NULL);
	return curl_easy_perform(r->curl);
}

/** Wait in cancellation-aware 100ms intervals without background polling.
 * @param r Relay; milliseconds bounded delay. Returns zero or -1 cancelled.
 */
static int pause_stream(relay *r, unsigned milliseconds)
{
	while (milliseconds && alive(r)) {
		if (progress(r, 0, 0, 0, 0))
			return -1;
		unsigned n = milliseconds > 100 ? 100 : milliseconds;
		struct pollfd p = {r->fd, POLLIN, 0};
		if (poll(&p, 1, (int)n) < 0 && errno != EINTR)
			return -1;
		milliseconds -= n;
	}
	return alive(r) ? 0 : -1;
}

/** Fetch one manifest with a bounded transient retry and effective URL base.
 * @param r Relay; url initial URL; effective final redirect URL output.
 * @return Zero or -1; does not overwrite an already set parser error.
 */
static int playlist_fetch(relay *r, const char *url, char effective[HLS_URL])
{
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		CURLcode code = fetch(r, url, 1);
		if (code == CURLE_OPERATION_TIMEDOUT)
			r->failure_status = 504;
		if (code == CURLE_OK && r->status == 200 && r->manifest_size) {
			char *final = NULL;
			curl_easy_getinfo(
				r->curl, CURLINFO_EFFECTIVE_URL, &final);
			if (hls_url(NULL, final, effective)) {
				r->error = "unsafe final playlist URL";
				return -1;
			}
			r->failure_status = 415;
			return 0;
		}
		if (r->bad || r->cancelled ||
		    (r->status >= 400 && r->status < 500 && r->status != 429))
			break;
		r->retries++;
		fprintf(stderr,
			"[hls] playlist refresh error; bounded retry\n");
		if (pause_stream(r, 500))
			break;
	}
	if (!r->error) {
		r->error = "HLS playlist fetch failed";
		if (r->failure_status != 504)
			r->failure_status = 502;
	}
	return -1;
}

/** Fetch a segment, resuming exact offsets on interrupted transfers.
 * @param r Relay; url segment source. Returns 0 success, 1 expired, -1 fatal.
 */
static int segment_fetch(relay *r, const char *url)
{
	r->offset = r->resume = r->total = 0;
	r->container = 0;
	/* A new HLS segment may restart continuity counters; PES/audio state is
	 * retained so frame boundaries spanning segments are preserved. */
	r->ts.audio_cc = r->ts.pat.cc = r->ts.pmt.cc = -1;
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		r->resume = r->offset;
		CURLcode code = fetch(r, url, 0);
		if (code == CURLE_OPERATION_TIMEDOUT)
			r->failure_status = 504;
		if (r->head_done)
			return 0;
		if (r->bad || r->cancelled)
			return -1;
		if ((r->status == 404 || r->status == 410) && !r->offset)
			return 1;
		if (code == CURLE_OK &&
		    (r->status == 200 || r->status == 206) && r->offset &&
		    (!r->total || r->offset == r->total)) {
			if (r->container == 2 && r->ts.size) {
				r->error = "truncated TS packet";
				return -1;
			}
			r->segments++;
			r->failure_status = 415;
			return 0;
		}
		if (r->status >= 400 && r->status < 500 && r->status != 429)
			break;
		r->retries++;
		fprintf(stderr,
			"[hls] segment download error; retry offset=%llu\n",
			(unsigned long long)r->offset);
		if (pause_stream(r, 300))
			break;
	}
	r->error = "HLS segment download failed";
	if (r->failure_status != 504)
		r->failure_status = 502;
	return -1;
}

/** Execute bounded manifest refresh and sequential segment selection.
 * @param r Relay; initial URL. Returns on ENDLIST, cancellation or error.
 */
/** Report query structure, never arbitrary query values or source URLs.
 * @param url Validated fetch URL; stage fixed diagnostic label.
 */
static void query_diagnostic(const char *url, const char *stage)
{
	const char *enabled = getenv("SB_HLS_DIAGNOSTICS");
	if (!enabled || strcmp(enabled, "1"))
		return;
	const char *q = strchr(url, '?');
	fprintf(stderr,
		"[hls-diag] stage=%s query_bytes=%u xml_amp=%d "
		"encoded_amp=%d\n",
		stage,
		q ? (unsigned)strlen(q + 1) : 0,
		q && strstr(q, "&amp;") != NULL,
		q && (strstr(q, "%26") || strstr(q, "%2526")));
	if (!q)
		return;
	q++;
	for (unsigned i = 0; *q && i < 16; i++) {
		const char *end = strchr(q, '&');
		if (!end)
			end = q + strlen(q);
		const char *eq = memchr(q, '=', (size_t)(end - q));
		size_t key = eq ? (size_t)(eq - q) : (size_t)(end - q);
		int bitrate = key == 7 && !strncmp(q, "bitrate", 7);
		int station = key == 7 && !strncmp(q, "station", 7);
		char number[11] = "hidden";
		if (bitrate && eq && end - eq > 1 && end - eq <= 11 &&
		    strspn(eq + 1, "0123456789") == (size_t)(end - eq - 1)) {
			memcpy(number, eq + 1, (size_t)(end - eq - 1));
			number[end - eq - 1] = 0;
		}
		fprintf(stderr,
			"[hls-diag] parameter=%u key=%s value_bytes=%u "
			"bitrate=%s\n",
			i,
			bitrate	  ? "bitrate"
			: station ? "station"
				  : "other",
			eq ? (unsigned)(end - eq - 1) : 0,
			number);
		q = *end ? end + 1 : end;
	}
}

static void run(relay *r, const char *initial)
{
	char url[HLS_URL], effective[HLS_URL], next[HLS_URL];
	char visited[4][HLS_URL];
	unsigned nesting = 0, stale = 0;
	uint64_t next_sequence = 0, previous_base = 0;
	int initialized = 0;
	if (hls_url(NULL, initial, url)) {
		r->error = "invalid HLS URL";
		return;
	}
	while (alive(r)) {
		if (!initialized)
			query_diagnostic(url, "native-request");
		if (playlist_fetch(r, url, effective))
			return;
		if (!initialized) {
			query_diagnostic(effective, "native-effective");
			const char *diagnostics = getenv("SB_HLS_DIAGNOSTICS");
			if (diagnostics && !strcmp(diagnostics, "1"))
				fprintf(stderr,
					"[hls-diag] manifest_bytes=%u "
					"redirects_changed_url=%d\n",
					(unsigned)r->manifest_size,
					strcmp(url, effective) != 0);
		}
		hls_playlist list;
		if (hls_parse(r->manifest, &list, &r->error))
			return;
		r->error = NULL;
		if (list.master) {
			if (initialized) {
				r->error =
					"media playlist changed into a master";
				return;
			}
			if (nesting == 4 ||
			    hls_url(effective, list.selected, next)) {
				r->error = "HLS nesting limit or unsafe "
					   "variant URL";
				return;
			}
			for (unsigned i = 0; i < nesting; i++)
				if (!strcmp(visited[i], next)) {
					r->error = "HLS playlist cycle";
					return;
				}
			strcpy(visited[nesting++], effective);
			strcpy(url, next);
			fprintf(stderr,
				"[hls] selected bounded audio variant "
				"depth=%u\n",
				nesting);
			continue;
		}
		if (initialized && list.sequence < previous_base) {
			r->error = "HLS media sequence reset";
			return;
		}
		previous_base = list.sequence;
		r->idle_ms = list.target_ms * 3;
		if (r->idle_ms < 30000)
			r->idle_ms = 30000;
		if (!initialized) {
			next_sequence =
				list.sequence + (!list.endlist && list.count > 3
							 ? list.count - 3
							 : 0);
			initialized = 1;
		}
		if (next_sequence < list.sequence) {
			r->skipped += list.sequence - next_sequence;
			next_sequence = list.sequence;
			r->audio.size = r->audio.skip = 0;
			hls_ts_init(&r->ts, &r->audio);
			r->ts.metadata = r->ts_metadata;
			if (r->metadata)
				hls_id3_reset(r->metadata);
			if (r->ts_metadata)
				hls_id3_reset(r->ts_metadata);
			fprintf(stderr,
				"[hls] live window expired; resynchronizing\n");
		}
		unsigned processed = 0;
		for (unsigned i = 0; i < list.count; i++) {
			uint64_t sequence = list.sequence + i;
			if (sequence < next_sequence)
				continue;
			if (hls_url(effective, list.segments[i].uri, next)) {
				r->error = "unsafe HLS segment URL";
				return;
			}
			r->segment_seconds =
				(list.segments[i].duration_ms + 999) / 1000 +
				15;
			if (r->segment_seconds < 20)
				r->segment_seconds = 20;
			int result = segment_fetch(r, next);
			if (r->head_done || result < 0)
				return;
			if (result == 1) {
				if (list.endlist || r->offset) {
					r->error = "missing finite HLS segment";
					r->failure_status = 502;
					return;
				}
				r->skipped++;
				next_sequence = sequence + 1;
				r->audio.size = r->audio.skip = 0;
				hls_ts_init(&r->ts, &r->audio);
				r->ts.metadata = r->ts_metadata;
				if (r->metadata)
					hls_id3_reset(r->metadata);
				if (r->ts_metadata)
					hls_id3_reset(r->ts_metadata);
				fprintf(stderr,
					"[hls] live segment expired; "
					"refreshing\n");
				break;
			}
			next_sequence = sequence + 1;
			processed++;
		}
		if (list.endlist) {
			if (r->audio.size || r->audio.skip || r->ts.pes_size ||
			    r->ts.remaining > 0)
				r->error = "truncated final audio frame/PES";
			else if (!r->started)
				r->error = "no supported audio in HLS stream";
			return;
		}
		if (!processed && ++stale > 10) {
			r->error = "HLS live playlist stalled";
			return;
		}
		/* An unchanged snapshot is normal at half-target refresh
		 * cadence. Report only the bounded stalled-playlist/idle
		 * failure above, not speculative decoder underruns that this
		 * relay cannot observe. */
		if (processed)
			stale = 0;
		/* Keep redirects' final media URL as the refresh base. */
		strcpy(url, effective);
		unsigned delay = list.target_ms / 2;
		if (delay < 500)
			delay = 500;
		if (pause_stream(r, delay))
			return;
	}
}

/** Serve one HLS stream using an existing public-policy curl handle.
 * @param curl Existing transport handle; fd local client; url source; head
 * probe; expected_codec optional first-frame codec lock (zero auto-detects).
 */
/** Publish without claiming Now Playing; Lua applies at consumed audio time. */
static void metadata_event(void *opaque, const hls_metadata *event)
{
	relay *r = opaque;
	hls_metadata_publish(
		r->metadata_session, event, r->audio.elapsed_us / 1000);
}

/** Align separate ID3 PID events to audio PES with wrap-safe PTS. */
static void ts_metadata_event(void *opaque, const hls_metadata *event)
{
	relay *r = opaque;
	uint64_t position = r->audio.elapsed_us / 1000;
	if (r->ts.has_metadata_pts) {
		r->ts_metadata->event.timestamp = r->ts.metadata_pts;
		r->ts_metadata->event.has_timestamp = 1;
		if (r->ts.has_audio_pts) {
			uint64_t difference =
				(r->ts.metadata_pts - r->ts.audio_pts) &
				((UINT64_C(1) << 33) - 1);
			int64_t delta = difference >= (UINT64_C(1) << 32)
						? (int64_t)difference -
							  (INT64_C(1) << 33)
						: (int64_t)difference;
			/* Do not queue indefinitely on malformed/discontinuous
			 * clocks. */
			if (delta >= -10800000 && delta <= 10800000) {
				int64_t aligned =
					(int64_t)r->ts.audio_pts_position_ms +
					delta / 90;
				position = aligned > 0 ? (uint64_t)aligned : 0;
			}
		}
	}
	hls_metadata_publish(r->metadata_session, event, position);
}

void hls_relay(
	CURL *curl, int fd, const char *url, int head, int expected_codec)
{
	pthread_mutex_lock(&hls_lock);
	if (hls_active >= 2) {
		pthread_mutex_unlock(&hls_lock);
		const char busy[] = "HTTP/1.1 503 Busy\r\nContent-Length: "
				    "0\r\nConnection: close\r\n\r\n";
		(void)send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL);
		return;
	}
	hls_active++;
	pthread_mutex_unlock(&hls_lock);
	relay *r = calloc(1, sizeof(*r));
	if (r) {
		r->curl = curl;
		r->fd = fd;
		r->head = head;
		r->expected_codec = expected_codec;
		r->failure_status = 415;
		r->idle_ms = 30000;
		r->started_ms = r->last_audio_ms = milliseconds();
		r->manifest = malloc(HLS_MANIFEST + 1);
		if (!r->manifest) {
			r->error = "HLS allocation failed";
			r->failure_status = 503;
		} else {
			hls_audio_init(&r->audio, output, r);
			hls_ts_init(&r->ts, &r->audio);
			if (!head) {
				r->metadata_session =
					hls_metadata_open(r->metadata_id);
				if (r->metadata_session) {
					r->metadata =
						calloc(1, sizeof(hls_id3));
					r->ts_metadata =
						calloc(1, sizeof(hls_id3));
					if (r->metadata && r->ts_metadata) {
						hls_id3_init(r->metadata,
							     metadata_event,
							     r);
						hls_id3_init(r->ts_metadata,
							     ts_metadata_event,
							     r);
						r->audio.metadata = r->metadata;
						r->ts.metadata = r->ts_metadata;
					} else {
						free(r->metadata);
						free(r->ts_metadata);
						r->metadata = r->ts_metadata =
							NULL;
						hls_metadata_close(
							r->metadata_session);
						r->metadata_session = NULL;
						r->metadata_id[0] = 0;
					}
				}
			}
			unsigned buffer = 8192;
			const char *setting = getenv("SB_HLS_BUFFER_BYTES");
			if (setting) {
				unsigned n =
					(unsigned)strtoul(setting, NULL, 10);
				if (n >= 1024 && n <= 16384)
					buffer = n;
			}
			curl_easy_setopt(
				curl, CURLOPT_BUFFERSIZE, (long)buffer);
			fprintf(stderr,
				"[hls] initialization buffer=%u head=%d "
				"working_heap=%zu playlist_view=%zu\n",
				buffer,
				head,
				sizeof(*r) + HLS_MANIFEST + 1,
				sizeof(hls_playlist));
			run(r, url);
		}
		if (!r->started && !r->cancelled) {
			const char *reason =
				r->error ? r->error : "no HLS audio";
			char header[256];
			int n = snprintf(
				header,
				sizeof header,
				"HTTP/1.1 %d HLS Unavailable\r\nContent-Type: "
				"text/plain\r\nX-SB-HLS-Error: "
				"%s\r\nContent-Length: %zu\r\nConnection: "
				"close\r\n\r\n",
				r->failure_status,
				reason,
				strlen(reason));
			if (n > 0 && n < (int)sizeof header &&
			    !send_bytes(r, header, (size_t)n) && !head)
				(void)send_bytes(r, reason, strlen(reason));
		}
		fprintf(stderr,
			"[hls] termination segments=%llu frames=%llu "
			"bytes=%llu retries=%llu skipped=%llu id3=%llu "
			"metadata_bad=%llu reason=%s\n",
			(unsigned long long)r->segments,
			(unsigned long long)r->audio.frames,
			(unsigned long long)r->bytes,
			(unsigned long long)r->retries,
			(unsigned long long)r->skipped,
			(unsigned long long)r->audio.id3,
			(unsigned long long)((r->metadata
						      ? r->metadata->malformed
						      : 0) +
					     (r->ts_metadata
						      ? r->ts_metadata
								->malformed
						      : 0)),
			r->head_done   ? "HEAD complete"
			: r->cancelled ? "client disconnected"
			: r->error     ? r->error
				       : "ENDLIST");
		/* The owning worker cleans up curl after this function returns.
		 */
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, NULL);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
		free(r->manifest);
		if (!r->cancelled && !r->error && !r->head)
			hls_metadata_finish(r->metadata_session);
		else
			hls_metadata_close(r->metadata_session);
		free(r->metadata);
		free(r->ts_metadata);
		free(r);
	} else {
		const char failed[] = "HTTP/1.1 503 Busy\r\nContent-Length: "
				      "0\r\nConnection: close\r\n\r\n";
		(void)send(fd, failed, sizeof failed - 1, MSG_NOSIGNAL);
	}
	pthread_mutex_lock(&hls_lock);
	hls_active--;
	pthread_mutex_unlock(&hls_lock);
}
