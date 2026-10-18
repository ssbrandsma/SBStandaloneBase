/* Native HLS parser/framing regression tests. Uses synthetic encoded framing,
 * not an audio decoder; integration tests separately exercise TS and transport.
 */
#include "../../native/sbproxy/hls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Parse an isolated fixture and assert its expected acceptance.
 * @param source Fixture; accepted expected boolean. Returns parsed view.
 */
static hls_playlist parse(const char *source, int accepted)
{
	static char text[HLS_MANIFEST + 1];
	hls_playlist p;
	const char *error;
	strcpy(text, source);
	assert((hls_parse(text, &p, &error) == 0) == accepted);
	return p;
}

/** Count complete emitted frames.
 * @param context Counter; data/size frame; codec actual format. Returns zero.
 */
static int
emit(void *context, const unsigned char *data, size_t size, int codec)
{
	assert(data[0] == 255 && size >= 7 &&
	       (codec == HLS_AAC || codec == HLS_MP3));
	(*(unsigned *)context)++;
	return 0;
}

/** Validate parser bounds, deterministic selection, framing and config changes.
 * @return Test process status.
 */
int main(void)
{
	hls_playlist p =
		parse("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:"
		      "123\n#EXTINF:6.4,\na.ts\n#EXTINF:6.4,\n../b.ts\n",
		      1);
	assert(p.count == 2 && p.sequence == 123 && p.target_ms == 6000 &&
	       !p.endlist);
	parse("#EXTM3U\nhttps://example.com/live.mp3\n", 0);
	parse("#EXTM3U\n#EXTINF:6.4,\na.ts\n", 0);
	parse("#EXTM3U\n#EXT-X-TARGETDURATION:0\n#EXTINF:1,\na.ts\n", 0);
	parse("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:"
	      "18446744073709551615\n#EXTINF:6,\na.ts\n",
	      0);
	parse("#EXTM3U\n#EXT-X-MAP:URI=init.mp4\n", 0);
	parse("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=key\n", 0);
	parse("#EXTM3U\n#EXT-X-BYTERANGE:100\n", 0);
	parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1."
	      "42e01e\"\nvideo.m3u8\n",
	      0);
	p = parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a."
		  "40.2\"\nhigh.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS="
		  "\"mp4a.40.2\"\nlow.m3u8\n",
		  1);
	assert(p.master && !strcmp(p.selected, "low.m3u8"));
	p = parse("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME="
		  "\"One\",DEFAULT=YES,URI=\"audio/"
		  "main.m3u8\"\n#EXT-X-STREAM-INF:BANDWIDTH=500000,CODECS="
		  "\"avc1.42e01e,mp4a.40.2\",AUDIO=\"a\"\nvideo.m3u8\n",
		  1);
	assert(!strcmp(p.selected, "audio/main.m3u8"));
	char resolved[HLS_URL];
	parse("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:1e-1,\na.ts\n", 0);
	char excessive[8192] = "#EXTM3U\n#EXT-X-TARGETDURATION:1\n";
	for (unsigned i = 0; i < HLS_SEGMENTS + 1; i++)
		strcat(excessive, "#EXTINF:1,\na.aac\n");
	parse(excessive, 0);
	assert(!hls_url("https://example.com/a/b/list.m3u8?q=1",
			"../x.aac?k=a%2Fb",
			resolved));
	assert(!strcmp(resolved, "https://example.com/a/x.aac?k=a%2Fb"));
	assert(hls_url(NULL, "file:///tmp/audio", resolved));
	assert(hls_url(NULL, "https://user:secret@example.com/a", resolved));
	assert(hls_url(NULL, "http://example.com:8765/a", resolved));
	hls_audio a;
	unsigned count = 0;
	unsigned char frame[] = {
		255, 241, 80, 128, 1, 127, 252, 1, 2, 3, 4}; /* ADTS length 11
							      */
	hls_audio_init(&a, emit, &count);
	for (size_t i = 0; i < sizeof(frame); i++)
		assert(!hls_audio_feed(&a, frame + i, 1));
	assert(count == 1 && a.frames == 1 && !a.size);
	unsigned char id3[] = {
		'I', 'D', '3', 4, 0, 0, 0, 0, 0, 3, 'a', 'b', 'c'};
	assert(!hls_audio_feed(&a, id3, sizeof id3));
	assert(!hls_audio_feed(&a, frame, sizeof frame));
	assert(count == 2 && a.id3 == 1);
	hls_audio bad_id3;
	hls_audio_init(&bad_id3, emit, &count);
	unsigned char oversized_id3[] = {'I', 'D', '3', 4, 0, 0, 1, 0, 0, 0};
	assert(hls_audio_feed(&bad_id3, oversized_id3, sizeof oversized_id3) ==
	       -1);
	frame[2] = 76; /* sample rate change */
	assert(hls_audio_feed(&a, frame, sizeof frame) == -1 && count == 2);
	hls_audio_init(&a, emit, &count);
	assert(hls_audio_feed(&a,
			      (const unsigned char *)"\0\0\0\x18"
						     "ftyp",
			      8) == -1);
	/* Deterministic malformed-input sweep for sanitizer builds. */
	unsigned random = 1;
	for (unsigned iteration = 0; iteration < 2000; iteration++) {
		unsigned char bytes[376];
		for (size_t i = 0; i < sizeof bytes; i++) {
			random = random * 1664525u + 1013904223u;
			bytes[i] = (unsigned char)(random >> 24);
		}
		hls_audio_init(&a, emit, &count);
		(void)hls_audio_feed(&a, bytes, sizeof bytes);
		hls_ts stream;
		hls_ts_init(&stream, &a);
		bytes[0] = bytes[188] = 71;
		(void)hls_ts_feed(&stream, bytes, sizeof bytes);
	}
	puts("hls-native-tests-ok");
	return 0;
}
