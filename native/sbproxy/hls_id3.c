/* Streaming ID3v2.3/v2.4 parser for HLS. Collects one bounded frame, not a tag
 * or segment. Delivers UTF-8 fields and validated small APIC to the registry.
 */
#include "hls_metadata.h"
#include <string.h>

/** Decode big-endian or synchsafe integer; reject non-synchsafe input. */
static size_t integer(const unsigned char *b, int sync)
{
	if (sync && ((b[0] | b[1] | b[2] | b[3]) & 128))
		return (size_t)-1;
	return sync ? ((size_t)b[0] << 21) | ((size_t)b[1] << 14) |
			       ((size_t)b[2] << 7) | b[3]
		    : ((size_t)b[0] << 24) | ((size_t)b[1] << 16) |
			       ((size_t)b[2] << 8) | b[3];
}

/** Append one Unicode scalar to bounded UTF-8, replacing invalid scalars. */
static void scalar(char *out, size_t *n, unsigned c)
{
	if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		c = 0xfffd;
	if (c < 32)
		c = ' ';
	unsigned bytes = c < 128 ? 1 : c < 2048 ? 2 : c < 65536 ? 3 : 4;
	if (*n + bytes >= HLS_TEXT_MAX)
		return;
	if (bytes == 1)
		out[(*n)++] = (char)c;
	else {
		out[(*n)++] = (char)((bytes == 2   ? 0xc0
				      : bytes == 3 ? 0xe0
						   : 0xf0) |
				     (c >> (6 * (bytes - 1))));
		for (unsigned i = bytes - 1; i; i--)
			out[(*n)++] =
				(char)(0x80 | ((c >> (6 * (i - 1))) & 63));
	}
	out[*n] = 0;
}

/** Convert bounded ID3 text including UTF-16 surrogate pairs to valid UTF-8. */
static void
text(char out[HLS_TEXT_MAX], unsigned enc, const unsigned char *p, size_t size)
{
	size_t n = 0, i = 0;
	int little = 0;
	out[0] = 0;
	if (enc > 3)
		return;
	if (enc == 1) {
		if (size < 2)
			return;
		if (p[0] == 255 && p[1] == 254)
			little = 1;
		else if (p[0] != 254 || p[1] != 255)
			return;
		i = 2;
	}
	while (i < size && n + 4 < HLS_TEXT_MAX) {
		unsigned c = p[i++];
		if (enc == 1 || enc == 2) {
			if (i >= size)
				break;
			c = little ? c | (p[i++] << 8) : (c << 8) | p[i++];
			if (!c)
				break;
			if (c >= 0xd800 && c <= 0xdbff && i + 1 < size) {
				unsigned low = little ? p[i] | (p[i + 1] << 8)
						      : (p[i] << 8) | p[i + 1];
				if (low >= 0xdc00 && low <= 0xdfff) {
					i += 2;
					c = 0x10000 + ((c - 0xd800) << 10) +
					    (low - 0xdc00);
				}
			}
		} else if (!c)
			break;
		else if (enc == 3 && c >= 128) {
			unsigned count = c >= 0xc2 && c <= 0xdf	  ? 1
					 : c >= 0xe0 && c <= 0xef ? 2
					 : c >= 0xf0 && c <= 0xf4 ? 3
								  : 0;
			unsigned value =
				count ? c & ((1u << (6 - count)) - 1) : 0xfffd;
			if (!count || i + count > size)
				c = 0xfffd;
			else {
				unsigned j = 0;
				for (; j < count && (p[i + j] & 192) == 128;
				     j++)
					value = (value << 6) | (p[i + j] & 63);
				if (j != count ||
				    value < (count == 1	  ? 128u
					     : count == 2 ? 2048u
							  : 65536u))
					c = 0xfffd;
				else {
					c = value;
					i += count;
				}
			}
		}
		scalar(out, &n, c);
	}
}

/** Find the terminator of an ID3 string in its declared encoding. */
static size_t terminator(const unsigned char *p, size_t n, unsigned enc)
{
	size_t step = (enc == 1 || enc == 2) ? 2 : 1;
	for (size_t i = 0; i + step <= n; i += step)
		if (!p[i] && (step == 1 || !p[i + 1]))
			return i + step;
	return 0;
}

/** Validate APIC signature and decoded dimensions without image decoding. */
static int image(const unsigned char *p, size_t n, unsigned type)
{
	unsigned w = 0, h = 0;
	if (type == 1) {
		if (n < 24 || memcmp(p, "\x89PNG\r\n\x1a\n", 8) ||
		    memcmp(p + 12, "IHDR", 4))
			return 0;
		w = (unsigned)integer(p + 16, 0);
		h = (unsigned)integer(p + 20, 0);
		int image_data = 0, end = 0;
		for (size_t i = 8; i + 12 <= n;) {
			size_t length = integer(p + i, 0);
			if (length > n - i - 12)
				return 0;
			if (i == 8 &&
			    (length != 13 || memcmp(p + i + 4, "IHDR", 4)))
				return 0;
			if (!memcmp(p + i + 4, "IDAT", 4))
				image_data = 1;
			if (!memcmp(p + i + 4, "IEND", 4)) {
				if (length || i + 12 != n)
					return 0;
				end = 1;
				break;
			}
			i += 12 + length;
		}
		if (!image_data || !end)
			return 0;
	} else {
		if (n < 4 || p[0] != 255 || p[1] != 216 || p[n - 2] != 255 ||
		    p[n - 1] != 217)
			return 0;
		for (size_t i = 2; i + 4 <= n;) {
			if (p[i++] != 255)
				return 0;
			while (i < n && p[i] == 255)
				i++;
			if (i >= n)
				break;
			unsigned marker = p[i++];
			if (marker == 0xd9 || marker == 0xda)
				break;
			if (marker == 0x01 ||
			    (marker >= 0xd0 && marker <= 0xd7))
				continue;
			if (i + 2 > n)
				break;
			size_t length = (p[i] << 8) | p[i + 1];
			if (length < 2 || length > n - i)
				break;
			if ((marker >= 0xc0 && marker <= 0xc3) ||
			    (marker >= 0xc5 && marker <= 0xc7) ||
			    (marker >= 0xc9 && marker <= 0xcb) ||
			    (marker >= 0xcd && marker <= 0xcf)) {
				if (length < 8)
					return 0;
				h = (p[i + 3] << 8) | p[i + 4];
				w = (p[i + 5] << 8) | p[i + 6];
				break;
			}
			i += length;
		}
	}
	return w && h && w <= 1024 && h <= 1024 && (uint64_t)w * h <= 262144;
}

/** Decode a completed bounded frame. Unknown/flagged frames never affect audio.
 */
static void frame(hls_id3 *s)
{
	unsigned char *p = s->frame;
	size_t n = s->stored;
	if (s->stored != s->frame_size)
		return;
	if (s->version == 3 ? s->frame_flags & 0xe0 : s->frame_flags & 0x4d)
		return;
	if (s->version == 4 && ((s->flags & 128) || (s->frame_flags & 2))) {
		size_t j = 0;
		for (size_t i = 0; i < n; i++) {
			p[j++] = p[i];
			if (p[i] == 255 && i + 1 < n && !p[i + 1])
				i++;
		}
		n = j;
	}
	char *out = NULL;
	unsigned bit = 0;
	if (!strcmp(s->id, "TIT2")) {
		out = s->event.title;
		bit = 1;
	}
	if (!strcmp(s->id, "TPE1")) {
		out = s->event.artist;
		bit = 2;
	}
	if (!strcmp(s->id, "TALB")) {
		out = s->event.album;
		bit = 4;
	}
	if (out && n) {
		text(out, p[0], p + 1, n - 1);
		if (*out)
			s->event.fields |= bit;
		return;
	}
	if (!strcmp(s->id, "PRIV")) {
		static const char owner[] =
			"com.apple.streaming.transportStreamTimestamp";
		if (n == sizeof(owner) + 8 && !memcmp(p, owner, sizeof owner)) {
			uint64_t t = 0;
			for (size_t i = sizeof owner; i < n; i++)
				t = (t << 8) | p[i];
			if (t < (UINT64_C(1) << 33)) {
				s->event.timestamp = t;
				s->event.has_timestamp = 1;
			}
		}
		return;
	}
	/* WXXX/TXXX are recognized but deliberately have no generic artwork
	 * semantics. Never treat arbitrary provider text as a download URL. */
	if (strcmp(s->id, "APIC") || n < 8 || p[0] > 3)
		return;
	unsigned enc = p[0];
	size_t mime = 1;
	while (mime < n && p[mime])
		mime++;
	if (mime + 2 >= n)
		return;
	unsigned type = !strcmp((char *)p + 1, "image/png")    ? 1
			: !strcmp((char *)p + 1, "image/jpeg") ? 2
							       : 0;
	int linked = !strcmp((char *)p + 1, "-->");
	if ((!type && !linked) ||
	    (p[mime + 1] != 3 &&
	     (s->event.art_size || s->event.artwork_url[0])))
		return;
	size_t description = terminator(p + mime + 2, n - mime - 2, enc);
	if (!description)
		return;
	size_t start = mime + 2 + description, bytes = n - start;
	if (linked) {
		while (bytes && !p[start + bytes - 1])
			bytes--;
		if (!bytes || bytes >= sizeof s->event.artwork_url)
			return;
		char url[241];
		memcpy(url, p + start, bytes);
		url[bytes] = 0;
		const char *host = !strncmp(url, "https://", 8)	 ? url + 8
				   : !strncmp(url, "http://", 7) ? url + 7
								 : NULL;
		if (!host || !*host || strpbrk(url, "\r\n\t \\#@"))
			return;
		const char *slash = strchr(host, '/');
		if (!slash || slash == host)
			return;
		for (const char *c = host; c < slash; c++)
			if (!((*c >= 'a' && *c <= 'z') ||
			      (*c >= 'A' && *c <= 'Z') ||
			      (*c >= '0' && *c <= '9') || *c == '.' ||
			      *c == '-' || *c == ':'))
				return;
		for (size_t i = 0; i < bytes; i++)
			if ((unsigned char)url[i] < 33 ||
			    (unsigned char)url[i] >= 127)
				return;
		memcpy(s->event.artwork_url, url, bytes + 1);
		s->event.art_size = 0;
		return;
	}
	if (bytes > HLS_ART_MAX || !image(p + start, bytes, type))
		return;
	memcpy(s->event.art, p + start, bytes);
	s->event.art_size = (unsigned)bytes;
	s->event.art_type = type;
	s->event.artwork_url[0] = 0;
}

/** Reset partial parsing but retain callback ownership; no allocations. */
void hls_id3_reset(hls_id3 *s)
{
	uint64_t malformed = s->malformed;
	hls_metadata_output output = s->output;
	void *opaque = s->opaque;
	memset(s, 0, sizeof *s);
	s->output = output;
	s->opaque = opaque;
	s->malformed = malformed;
}

/** Initialize a caller-owned parser and completion callback. */
void hls_id3_init(hls_id3 *s, hls_metadata_output output, void *opaque)
{
	memset(s, 0, sizeof *s);
	s->output = output;
	s->opaque = opaque;
}

/** Incrementally consume a bounded ID3 tag; malformed metadata is discarded. */
void hls_id3_feed(hls_id3 *s, const unsigned char *p, size_t n)
{
	while (n--) {
		unsigned char c = *p++;
		if (s->stage == 0) {
			s->header[s->header_size++] = c;
			if (s->header_size <= 3 &&
			    memcmp(s->header, "ID3", s->header_size)) {
				s->header_size = c == 'I' ? 1 : 0;
				if (s->header_size)
					s->header[0] = 'I';
				continue;
			}
			if (s->header_size < 10)
				continue;
			if (memcmp(s->header, "ID3", 3)) {
				hls_id3_reset(s);
				continue;
			}
			s->version = s->header[3];
			s->flags = s->header[5];
			s->remaining = integer(s->header + 6, 1);
			s->invalid = (s->version != 3 && s->version != 4) ||
				     s->remaining == (size_t)-1 ||
				     s->remaining > 1024u * 1024u;
			if (s->remaining == (size_t)-1 ||
			    s->remaining > 1024u * 1024u) {
				hls_id3_reset(s);
				continue;
			}
			s->stage = (s->flags & 64) ? 1 : 2;
			s->header_size = 0;
			if (!s->remaining)
				hls_id3_reset(s);
			continue;
		}
		if (s->stage == 4) {
			if (!--s->remaining)
				hls_id3_reset(s);
			continue;
		}
		s->remaining--;
		int keep = 1;
		if (s->version == 3 && (s->flags & 128)) {
			if (s->unsync_zero && !c)
				keep = 0;
			s->unsync_zero = c == 255;
		}
		if (!s->invalid && keep) {
			if (s->stage == 1) {
				if (s->header_size < 4) {
					s->header[s->header_size++] = c;
					if (s->header_size == 4) {
						size_t length = integer(
							s->header,
							s->version == 4);
						if (length == (size_t)-1 ||
						    length < 6u ||
						    length >
							    s->remaining +
								    (s->version == 4
									     ? 4
									     : 0))
							s->invalid = 1;
						else
							s->extended =
								length -
								(s->version == 4
									 ? 4
									 : 0);
					}
				} else if (s->extended && !--s->extended) {
					s->stage = 2;
					s->header_size = 0;
				}
			} else if (s->stage == 2) {
				if (!s->header_size && !c) {
					/* Padding ends frames, not the whole
					 * upstream segment. */
					s->stage = 5;
				} else {
					s->header[s->header_size++] = c;
					if (s->header_size == 10) {
						memcpy(s->id, s->header, 4);
						s->id[4] = 0;
						if (!s->id[0])
							s->invalid = 1;
						for (unsigned i = 0; i < 4; i++)
							if (!((s->id[i] >=
								       'A' &&
							       s->id[i] <=
								       'Z') ||
							      (s->id[i] >=
								       '0' &&
							       s->id[i] <=
								       '9')))
								s->invalid = 1;
						s->frame_size = integer(
							s->header + 4,
							s->version == 4);
						s->frame_flags = s->header[9];
						s->capture =
							!strcmp(s->id,
								"APIC") ||
							!strcmp(s->id,
								"PRIV") ||
							!strcmp(s->id,
								"TIT2") ||
							!strcmp(s->id,
								"TPE1") ||
							!strcmp(s->id, "TALB");
						if (s->frame_size >
						    s->remaining)
							s->invalid = 1;
						s->frame_read = s->stored = 0;
						s->stage = 3;
						if (!s->frame_size) {
							s->stage = 2;
							s->header_size = 0;
						}
					}
				}
			} else if (s->stage == 3) {
				if (s->capture && s->stored < sizeof s->frame &&
				    s->frame_size <= sizeof s->frame)
					s->frame[s->stored++] = c;
				if (++s->frame_read == s->frame_size) {
					frame(s);
					s->stage = 2;
					s->header_size = 0;
				}
			}
		}
		if (!s->remaining) {
			if (s->invalid || s->stage == 3 ||
			    (s->stage == 2 && s->header_size))
				s->malformed++;
			if (!s->invalid && (s->stage == 2 || s->stage == 5) &&
			    s->header_size == 0 && s->output &&
			    (s->event.fields || s->event.art_size ||
			     s->event.has_timestamp || s->event.artwork_url[0]))
				s->output(s->opaque, &s->event);
			/* Padding is valid; retain complete frames before zero
			 * padding. */
			else if (s->invalid && s->id[0] == 0 &&
				 s->event.fields && s->output)
				s->output(s->opaque, &s->event);
			if (s->version == 4 && (s->flags & 16)) {
				s->stage = 4;
				s->remaining = 10;
			} else
				hls_id3_reset(s);
		}
	}
}
