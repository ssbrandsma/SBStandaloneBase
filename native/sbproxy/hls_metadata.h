/* Bounded timed-ID3 extraction and per-relay delivery. The relay owns parsers;
 * the loopback HTTP worker reads snapshots without touching Now Playing. */
#ifndef SB_HLS_METADATA_H
#define SB_HLS_METADATA_H
#include <stddef.h>
#include <stdint.h>
#define HLS_ART_MAX 32768
#define HLS_TEXT_MAX 256
#define HLS_EVENTS 8

typedef struct {
	char title[HLS_TEXT_MAX], artist[HLS_TEXT_MAX], album[HLS_TEXT_MAX];
	char artwork_url[241];
	unsigned fields, art_size, art_type;
	unsigned char art[HLS_ART_MAX];
	uint64_t timestamp, position_ms, revision;
	int has_timestamp;
} hls_metadata;

typedef void (*hls_metadata_output)(void *, const hls_metadata *);

typedef struct {
	unsigned char header[10], frame[HLS_ART_MAX + 512];
	size_t header_size, remaining, frame_size, frame_read, stored, extended;
	unsigned version, flags, frame_flags, stage, invalid, unsync_zero;
	unsigned capture;
	uint64_t malformed;
	char id[5];
	hls_metadata event;
	hls_metadata_output output;
	void *opaque;
} hls_id3;

/* Parser accepts exactly one complete tag, incrementally, then calls output.
 * Invalid/oversized frames are skipped; no allocation or audio failure. */
void hls_id3_init(hls_id3 *, hls_metadata_output, void *);
void hls_id3_feed(hls_id3 *, const unsigned char *, size_t);
void hls_id3_reset(hls_id3 *);
/* Registry uses two bounded heap slots, random 128-bit opaque identities and
 * immediate cancellation retirement, bounded natural-EOF retention. No writes. */
void *hls_metadata_open(char id[33]);
void hls_metadata_publish(void *, const hls_metadata *, uint64_t position_ms);
void hls_metadata_close(void *);
void hls_metadata_finish(void *);
/* Returns 1 for handled /hls-meta/... requests, 0 for unrelated routes. */
int hls_metadata_http(int fd, const char *target, int head);
#endif
