/* HLS manifest selection and URL resolution for sbproxy's audio relay.
 * Stores bounded pointers into the caller's manifest, never media payloads.
 * libcurl's existing URL parser resolves RFC URI references. */
#define _POSIX_C_SOURCE 200809L
#include "hls.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/** Resolve a URI and reject credentials, fragments and non-HTTP schemes.
 * @param base Absolute base URL, or NULL for initial validation.
 * @param reference URI reference to resolve.
 * @param out Fixed-capacity validated URL result; returns 0 or -1.
 */
int hls_url(const char *base, const char *reference, char out[HLS_URL])
{
	CURLU *u = curl_url();
	char *url = NULL, *scheme = NULL, *host = NULL, *user = NULL,
	     *port = NULL;
	int result = -1;
	if (!u || !reference || !*reference || strlen(reference) >= HLS_URL ||
	    strpbrk(reference, "\r\n\t \\#"))
		goto done;
	if (base && curl_url_set(u, CURLUPART_URL, base, 0))
		goto done;
	if (curl_url_set(u, CURLUPART_URL, reference, 0) ||
	    curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) ||
	    curl_url_get(u, CURLUPART_HOST, &host, 0) ||
	    (strcasecmp(scheme, "http") && strcasecmp(scheme, "https")) ||
	    !*host || strchr(host, ':'))
		goto done;
	if (!curl_url_get(u, CURLUPART_USER, &user, 0))
		goto done;
	if (!curl_url_get(u, CURLUPART_PORT, &port, 0) && strcmp(port, "80") &&
	    strcmp(port, "443")) {
#ifndef SB_PROXY_TESTING
		goto done;
#else
		/* Only the socket callback in the separate test binary permits
		 * its explicit fixture ports. Production never accepts this
		 * branch. */
#endif
	}
	if (curl_url_get(u, CURLUPART_URL, &url, 0) || strlen(url) >= HLS_URL)
		goto done;
	strcpy(out, url);
	result = 0;
done:
	curl_free(url);
	curl_free(scheme);
	curl_free(host);
	curl_free(user);
	curl_free(port);
	curl_url_cleanup(u);
	return result;
}

/** Read one bounded, quoted/unquoted HLS attribute.
 * @param text Attribute list; key exact name; out result; cap output capacity.
 * @return 1 found, 0 missing, -1 malformed/oversized/duplicate attribute.
 */
static int attribute(const char *text, const char *key, char *out, size_t cap)
{
	int found = 0;
	while (*text) {
		while (*text == ',' || *text == ' ')
			text++;
		const char *name = text, *end = strchr(text, '=');
		if (!end || memchr(text, ',', (size_t)(end - text)))
			return -1;
		text = end + 1;
		const char *value = text;
		size_t length;
		if (*text == '"') {
			value = ++text;
			end = strchr(text, '"');
			if (!end)
				return -1;
			length = (size_t)(end - value);
			text = end + 1;
			if (*text && *text != ',')
				return -1;
		} else {
			end = strchr(text, ',');
			if (!end)
				end = text + strlen(text);
			length = (size_t)(end - value);
			text = end;
		}
		if ((size_t)(strchr(name, '=') - name) == strlen(key) &&
		    !strncmp(name, key, strlen(key))) {
			if (found || length >= cap)
				return -1;
			memcpy(out, value, length);
			out[length] = 0;
			found = 1;
		}
	}
	return found;
}

/** Parse an unsigned decimal with no trailing characters or overflow.
 * @param text Decimal input; value output. Returns zero or -1.
 */
static int number(const char *text, uint64_t *value)
{
	char *end;
	if (!isdigit((unsigned char)*text))
		return -1;
	errno = 0;
	unsigned long long n = strtoull(text, &end, 10);
	if (errno || *end)
		return -1;
	*value = n;
	return 0;
}

/** Check advertised codecs and whether a variant is audio-only.
 * @param attributes STREAM-INF list; returns -1 incompatible, 0 AAC/unknown,
 * 1 MP3, or the corresponding rank plus 2 when video requires audio rendition.
 */
static int codec_rank(const char *attributes)
{
	char codecs[256], resolution[64], video[64];
	int present = attribute(attributes, "CODECS", codecs, sizeof codecs);
	if (present < 0)
		return -1;
	if (!present)
		return attribute(attributes,
				 "RESOLUTION",
				 resolution,
				 sizeof resolution) ||
				       attribute(attributes,
						 "VIDEO",
						 video,
						 sizeof video)
			       ? 2
			       : 0;
	int audio = -1, other = 0;
	char *save = NULL;
	for (char *s = strtok_r(codecs, ",", &save); s;
	     s = strtok_r(NULL, ",", &save)) {
		while (*s == ' ')
			s++;
		if (!strcmp(s, "mp4a.40.2") || !strcmp(s, "mp4a.40.5") ||
		    !strcmp(s, "mp4a.40.29"))
			audio = 0;
		else if (!strcasecmp(s, "mp3") || !strcasecmp(s, "mp4a.6b") ||
			 !strcasecmp(s, "mp4a.69") ||
			 !strcmp(s, "mp4a.40.34")) {
			if (audio < 0)
				audio = 1;
		} else
			other = 1;
	}
	return audio < 0 ? -1 : other ? audio + 2 : audio;
}

/** Log bounded selection diagnostics only when explicitly enabled.
 * @param attributes Variant attributes (never its URI); index variant number.
 * @param rank Codec classification; bandwidth_result attribute parse result.
 * @param rate Parsed bandwidth, zero when invalid; reason fixed decision text.
 * No source URLs, group names or arbitrary upstream attribute text are logged.
 */
static void selection_diagnostic(const char *attributes,
				 unsigned index,
				 int rank,
				 int bandwidth_result,
				 uint64_t rate,
				 const char *reason)
{
	const char *enabled = getenv("SB_HLS_DIAGNOSTICS");
	if (!enabled || strcmp(enabled, "1"))
		return;
	char codecs[256];
	int present = attribute(attributes, "CODECS", codecs, sizeof codecs);
	fprintf(stderr,
		"[hls-diag] variant=%u codecs=%s codec_rank=%d "
		"bandwidth_attribute=%d bandwidth=%llu decision=%s\n",
		index,
		present < 0 ? "malformed"
		: !present  ? "absent"
		: !*codecs  ? "empty"
		: rank < 0  ? "unsupported"
		: rank >= 2 ? "audio-with-video"
		: rank == 0 ? "AAC"
			    : "MP3",
		rank,
		bandwidth_result,
		(unsigned long long)rate,
		reason);
}

/** Parse one bounded HLS manifest in place.
 * @param text Mutable manifest; out borrowed URI view; error fixed reason.
 * @return Zero on success, -1 on malformed or unsupported content.
 */
int hls_parse(char *text, hls_playlist *out, const char **error)
{
	const char *variants[HLS_CHOICES], *uris[HLS_CHOICES],
		*renditions[HLS_CHOICES];
	unsigned variants_n = 0, renditions_n = 0, duration = 0;
	int first = 1, sequence_seen = 0, target_seen = 0;
	const char *pending = NULL;
	char *save = NULL;
	memset(out, 0, sizeof(*out));
	*error = "malformed HLS playlist";
	if (strlen(text) > HLS_MANIFEST)
		return -1;
	if (!strncmp(text, "\xef\xbb\xbf", 3))
		text += 3;
	for (char *line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		if (strlen(line) >= HLS_URL)
			return -1;
		if (first) {
			if (strcmp(line, "#EXTM3U"))
				return -1;
			first = 0;
			continue;
		}
		if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
			if (pending || variants_n == HLS_CHOICES)
				return -1;
			pending = line + 18;
			out->master = 1;
		} else if (!strncmp(line, "#EXT-X-MEDIA:", 13)) {
			if (renditions_n == HLS_CHOICES)
				return -1;
			renditions[renditions_n++] = line + 13;
			out->master = 1;
		} else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
			uint64_t value;
			if (target_seen++ || number(line + 22, &value) ||
			    !value || value > 120)
				return -1;
			out->target_ms = (unsigned)value * 1000;
		} else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
			if (sequence_seen++ ||
			    number(line + 22, &out->sequence))
				return -1;
		} else if (!strncmp(line, "#EXTINF:", 8)) {
			char *end;
			const char *p = line + 8;
			if (!isdigit((unsigned char)*p))
				return -1;
			while (isdigit((unsigned char)*p))
				p++;
			if (*p == '.') {
				p++;
				if (!isdigit((unsigned char)*p))
					return -1;
				while (isdigit((unsigned char)*p))
					p++;
			}
			if (*p != ',')
				return -1;
			double value = strtod(line + 8, &end);
			if (duration || end == line + 8 || *end != ',' ||
			    !(value > 0 && value <= 120))
				return -1;
			duration = (unsigned)(value * 1000 + .5);
			if (!duration)
				return -1;
		} else if (!strcmp(line, "#EXT-X-ENDLIST"))
			out->endlist = 1;
		else if (!strncmp(line, "#EXT-X-KEY:", 11) ||
			 !strncmp(line, "#EXT-X-SESSION-KEY:", 19)) {
			char method[32];
			const char *a = strchr(line, ':') + 1;
			if (attribute(a, "METHOD", method, sizeof method) !=
				    1 ||
			    strcmp(method, "NONE")) {
				*error = "encrypted HLS is unsupported";
				return -1;
			}
		} else if (!strncmp(line, "#EXT-X-MAP:", 11) ||
			   !strncmp(line, "#EXT-X-BYTERANGE:", 17) ||
			   !strcmp(line, "#EXT-X-DISCONTINUITY") ||
			   !strcmp(line, "#EXT-X-GAP") ||
			   !strcmp(line, "#EXT-X-I-FRAMES-ONLY")) {
			*error = "fMP4, byte ranges, gaps and explicit "
				 "discontinuities are unsupported";
			return -1;
		} else if (*line != '#') {
			if (pending) {
				variants[variants_n] = pending;
				uris[variants_n++] = line;
				pending = NULL;
			} else {
				if (!duration || out->count == HLS_SEGMENTS)
					return -1;
				out->segments[out->count++] =
					(hls_segment){line, duration};
				duration = 0;
			}
		}
	}
	if (first || pending || duration ||
	    out->sequence > UINT64_MAX - out->count)
		return -1;
	if (!out->master) {
		if (!out->count || !out->target_ms)
			return -1;
		for (unsigned i = 0; i < out->count; i++)
			if ((out->segments[i].duration_ms + 500) / 1000 >
			    out->target_ms / 1000)
				return -1;
		return 0;
	}
	if (out->count)
		return -1;
	uint64_t best_bandwidth = UINT64_MAX;
	int best_rank = 9;
	for (unsigned i = 0; i < variants_n; i++) {
		char bandwidth[32], group[128], candidate[HLS_URL];
		uint64_t rate = 0;
		int rank = codec_rank(variants[i]);
		int bandwidth_result = attribute(
			variants[i], "BANDWIDTH", bandwidth, sizeof bandwidth);
		int invalid_bandwidth = bandwidth_result != 1 ||
					number(bandwidth, &rate) || !rate;
		selection_diagnostic(variants[i],
				     i,
				     rank,
				     bandwidth_result,
				     invalid_bandwidth ? 0 : rate,
				     rank < 0		 ? "reject-codecs"
				     : invalid_bandwidth ? "reject-bandwidth"
							 : "evaluate");
		if (rank < 0 || invalid_bandwidth)
			continue;
		if (attribute(variants[i], "AUDIO", group, sizeof group) == 1) {
			int chosen = -1, preferred = -1;
			for (unsigned j = 0; j < renditions_n; j++) {
				char type[32], name[128], uri[HLS_URL], def[16];
				if (attribute(renditions[j],
					      "TYPE",
					      type,
					      sizeof type) != 1 ||
				    strcmp(type, "AUDIO") ||
				    attribute(renditions[j],
					      "GROUP-ID",
					      name,
					      sizeof name) != 1 ||
				    strcmp(name, group) ||
				    attribute(renditions[j],
					      "URI",
					      uri,
					      sizeof uri) != 1)
					continue;
				int yes = attribute(renditions[j],
						    "DEFAULT",
						    def,
						    sizeof def) == 1 &&
					  !strcmp(def, "YES");
				if (yes > preferred) {
					chosen = (int)j;
					preferred = yes;
					strcpy(candidate, uri);
				}
			}
			if (chosen < 0) {
				if (rank >= 2) {
					selection_diagnostic(
						variants[i],
						i,
						rank,
						bandwidth_result,
						rate,
						"reject-no-audio-rendition");
					continue;
				}
				strcpy(candidate, uris[i]);
			}
			if (rank >= 2)
				rank -= 2;
		} else {
			if (rank >= 2) {
				selection_diagnostic(
					variants[i],
					i,
					rank,
					bandwidth_result,
					rate,
					"reject-video-without-audio-group");
				continue;
			}
			strcpy(candidate, uris[i]);
		}
		if (rank < best_rank ||
		    (rank == best_rank && rate < best_bandwidth)) {
			strcpy(out->selected, candidate);
			best_rank = rank;
			best_bandwidth = rate;
			selection_diagnostic(variants[i],
					     i,
					     rank,
					     bandwidth_result,
					     rate,
					     "selected-best-so-far");
		}
	}
	if (!variants_n) {
		int preferred = -1;
		for (unsigned i = 0; i < renditions_n; i++) {
			char type[32], uri[HLS_URL], def[16];
			if (attribute(
				    renditions[i], "TYPE", type, sizeof type) !=
				    1 ||
			    strcmp(type, "AUDIO") ||
			    attribute(renditions[i], "URI", uri, sizeof uri) !=
				    1)
				continue;
			int yes = attribute(renditions[i],
					    "DEFAULT",
					    def,
					    sizeof def) == 1 &&
				  !strcmp(def, "YES");
			if (yes > preferred) {
				strcpy(out->selected, uri);
				preferred = yes;
			}
		}
	}
	if (!out->selected[0]) {
		*error = "no compatible AAC/MP3 audio variant";
		return -1;
	}
	return 0;
}
