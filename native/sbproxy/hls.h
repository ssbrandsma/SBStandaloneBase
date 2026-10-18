/* Internal bounded HLS interfaces shared by the playlist, framing and relay
 * modules. sbproxy owns transport policy, sockets and worker lifetime. */
#ifndef SB_HLS_H
#define SB_HLS_H
#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>
#include "hls_metadata.h"

#define HLS_URL 4096
#define HLS_MANIFEST 65536
#define HLS_SEGMENTS 128
#define HLS_CHOICES 32

typedef struct {
	char *uri;
	unsigned duration_ms;
} hls_segment;

typedef struct {
	uint64_t sequence;
	unsigned target_ms, count;
	int endlist, master;
	char selected[HLS_URL];
	hls_segment segments[HLS_SEGMENTS];
} hls_playlist;

/* Parse a mutable, NUL-terminated bounded manifest. URI pointers borrow text.
 * Returns zero or a fixed, non-sensitive error string through error. */
int hls_parse(char *text, hls_playlist *out, const char **error);
/* Resolve/validate HTTP(S) URI references; DNS/socket restrictions remain in
 * sbproxy. Returns zero on success, -1 for unsafe/oversized URLs. */
int hls_url(const char *base, const char *reference, char out[HLS_URL]);

typedef int (*hls_output)(void *opaque,
			  const unsigned char *frame,
			  size_t size,
			  int codec);

enum { HLS_AAC = 1, HLS_MP3 = 2 };

typedef struct {
	unsigned char buffer[8192];
	size_t size, skip;
	int codec;
	unsigned config;
	uint64_t frames, id3;
	uint64_t elapsed_us;
	hls_id3 *metadata;
	hls_output output;
	void *opaque;
	const char *error;
} hls_audio;

typedef struct {
	unsigned char data[1024];
	size_t size, need;
	int cc;
} hls_section;

typedef struct {
	unsigned char packet[188], pes[264];
	size_t size, pes_size, pes_need;
	int pmt_pid, audio_pid, audio_cc, pes_active, remaining;
	unsigned program;
	hls_section pat, pmt;
	hls_audio *audio;
	int metadata_pid, metadata_cc;
	unsigned char metadata_pes[264];
	size_t metadata_pes_size, metadata_pes_need;
	int metadata_remaining, metadata_active;
	hls_id3 *metadata;
	uint64_t audio_pts, metadata_pts, audio_pts_position_ms;
	int has_audio_pts, has_metadata_pts;
	const char *error;
} hls_ts;

/* Initialize elementary framing or TS demux state; caller owns all buffers. */
void hls_audio_init(hls_audio *audio, hls_output output, void *opaque);
void hls_ts_init(hls_ts *ts, hls_audio *audio);
/* Incrementally consume bytes; output only complete validated audio frames.
 * Returns -1 on malformed/unsupported input or downstream cancellation. */
int hls_audio_feed(hls_audio *audio, const unsigned char *data, size_t size);
int hls_ts_feed(hls_ts *ts, const unsigned char *data, size_t size);
/* Run one local response with a preconfigured, public-only curl handle.
 * Does not take ownership of curl/fd; no new process/thread or disk writes. */
void hls_relay(
	CURL *curl, int fd, const char *url, int head, int expected_codec);
#endif
