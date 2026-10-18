/* ID3 parser fixtures: version sizes, chunk boundaries, encodings, timestamps,
 * invalid frames, padding/extended headers and bounded artwork acceptance. */
#include "../../native/sbproxy/hls_metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static hls_metadata result;
static unsigned events;

static void emit(void *unused, const hls_metadata *event)
{
	(void)unused;
	result = *event;
	events++;
}

static void number(unsigned char *p, size_t n, int sync)
{
	for (int i = 3; i >= 0; i--) {
		p[i] = (unsigned char)(n & (sync ? 127 : 255));
		n >>= sync ? 7 : 8;
	}
}

static size_t frame(unsigned char *out,
		    const char *id,
		    const void *data,
		    size_t n,
		    int version)
{
	memcpy(out, id, 4);
	number(out + 4, n, version == 4);
	out[8] = out[9] = 0;
	memcpy(out + 10, data, n);
	return n + 10;
}

static void run(unsigned char *body, size_t n, int version, unsigned flags)
{
	unsigned char header[10] = {'I', 'D', '3', 0, 0, 0, 0, 0, 0, 0};
	header[3] = version;
	header[5] = flags;
	number(header + 6, n, 1);
	hls_id3 *s = calloc(1, sizeof *s);
	assert(s);
	hls_id3_init(s, emit, NULL);
	for (size_t i = 0; i < 10; i++)
		hls_id3_feed(s, header + i, 1);
	for (size_t i = 0; i < n; i++)
		hls_id3_feed(s, body + i, 1);
	free(s);
}

int main(void)
{
	unsigned char body[4096];
	for (int version = 3; version <= 4; version++) {
		size_t n = frame(body, "TIT2", "\3Track", 6, version);
		n += frame(body + n, "TPE1", "\0Beyonc\xe9", 8, version);
		const unsigned char album[] = {
			1, 255, 254, 'A', 0, 0x3d, 0xd8, 0, 0xde};
		n += frame(body + n, "TALB", album, sizeof album, version);
		memset(body + n, 0, 7);
		events = 0;
		run(body, n + 7, version, 0);
		assert(events == 1 && result.fields == 7 &&
		       !strcmp(result.title, "Track"));
		assert(!strcmp(result.artist, "Beyonc\xc3\xa9") &&
		       !strcmp(result.album, "A\xf0\x9f\x98\x80"));
		unsigned char owner[80] =
			"com.apple.streaming.transportStreamTimestamp";
		size_t size = strlen((char *)owner) + 1;
		memset(owner + size, 0, 8);
		owner[size + 5] = 1;
		owner[size + 6] = 0x5f;
		owner[size + 7] = 0x90;
		n = frame(body, "PRIV", owner, size + 8, version);
		events = 0;
		run(body, n, version, 0);
		assert(events == 1 && result.has_timestamp &&
		       result.timestamp == 90000 && result.fields == 0);
		/* Unknown/TXXX/WXXX metadata must not create artwork or track
		 * text. */
		n = frame(body,
			  "WXXX",
			  "\0art\0http://example.com/art.png",
			  29,
			  version);
		n += frame(body + n, "TXXX", "\3other\0hello", 12, version);
		events = 0;
		run(body, n, version, 0);
		assert(!events);
		/* Invalid length is contained within the tag. */
		n = frame(body, "TIT2", "\3Good", 5, version);
		number(body + 4, 999, version == 4);
		events = 0;
		run(body, n, version, 0);
		assert(!events);
		/* Extended headers have different inclusive/exclusive size
		 * rules. */
		memset(body, 0, 10);
		number(body, 6, version == 4);
		size_t ext = version == 4 ? 6 : 10;
		n = ext + frame(body + ext, "TIT2", "\3Extended", 9, version);
		events = 0;
		run(body, n, version, 64);
		assert(events == 1 && !strcmp(result.title, "Extended"));
	}
	/* v2.3 global unsync and v2.4 frame unsync. */
	unsigned char text[] = {0, 'x', 255, 0, 0xe1};
	size_t n = frame(body, "TIT2", text, sizeof text, 4);
	body[9] = 2;
	events = 0;
	run(body, n, 4, 0);
	assert(events == 1 && !strcmp(result.title, "x\xc3\xbf\xc3\xa1"));
	/* v2.3 frame size excludes the tag-level inserted unsync byte. */
	const unsigned char plain[]={0,'x',255,0xe1};
	n=frame(body,"TIT2",plain,sizeof plain,3);
	memmove(body+14,body+13,n-13);body[13]=0;n++;
	events=0;run(body,n,3,128);
	assert(events==1&&!strcmp(result.title,"x\xc3\xbf\xc3\xa1"));
	/* UTF-16BE and invalid UTF-8 are converted, never emitted raw. */
	unsigned char be[] = {2, 0, 'X', 0, 'Y'};
	n = frame(body, "TIT2", be, sizeof be, 4);
	events = 0;
	run(body, n, 4, 0);
	assert(!strcmp(result.title, "XY"));
	n = frame(body, "TIT2", "\3\xff", 2, 4);
	run(body, n, 4, 0);
	assert(!strcmp(result.title, "\xef\xbf\xbd"));
	/* Malformed APIC never replaces a valid title. */
	n = frame(body, "TIT2", "\3Track", 6, 4);
	n += frame(body + n, "APIC", "\0image/png\0\3\0bad", 16, 4);
	events = 0;
	run(body, n, 4, 0);
	assert(events == 1 && !result.art_size);
	/* Small bounded JPEG APIC with front-cover description terminator. */
	unsigned char apic[] = {0,   'i', 'm', 'a', 'g', 'e', '/',
				'j', 'p', 'e', 'g', 0,	 3,   0,
				255, 216, 255, 192, 0,	 8,   8,
				0,   1,	  0,   1,   1,	 255, 217};
	n = frame(body, "APIC", apic, sizeof apic, 4);
	events = 0;
	run(body, n, 4, 0);
	assert(events == 1 && result.art_type == 2 && result.art_size == 14);
	/* Linked APIC is standardized; arbitrary WXXX/TXXX are not artwork. */
	const unsigned char link[] =
		"\0-->\0\3\0https://images.example/cover.png";
	n = frame(body, "APIC", link, sizeof link - 1, 4);
	events = 0;
	run(body, n, 4, 0);
	assert(events == 1 &&
	       !strcmp(result.artwork_url, "https://images.example/cover.png"));
	/* Oversized individual artwork frames are skipped while text survives.
	 */
	unsigned char *large = calloc(1, 70000);
	assert(large);
	n = frame(large, "APIC", large + 10, 65000, 4);
	n += frame(large + n, "TIT2", "\3After large frame", 18, 4);
	events = 0;
	run(large, n, 4, 0);
	assert(events == 1 && !result.art_size &&
	       !strcmp(result.title, "After large frame"));
	free(large);
	/* Deterministic malformed input must remain memory-safe. */
	for (unsigned seed = 1; seed < 1000; seed++) {
		unsigned x = seed;
		for (size_t i = 0; i < sizeof body; i++) {
			x = x * 1664525u + 1013904223u;
			body[i] = (unsigned char)(x >> 24);
		}
		run(body, seed % sizeof body, 3 + (seed & 1), seed & 128);
	}
	printf("hls-id3-tests-ok parser=%zu event=%zu\n",
	       sizeof(hls_id3),
	       sizeof(hls_metadata));
	return 0;
}
