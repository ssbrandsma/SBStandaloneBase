#ifndef NOWPLAYING_H
#define NOWPLAYING_H

#include <stddef.h>
#include <stdint.h>

#define NP_SOURCE_MAX 48
#define NP_NAME_MAX 96
#define NP_TEXT_MAX 256
#define NP_ID_MAX 192
#define NP_ART_MAX 384
#define NP_TOKEN_MAX 65
#define NP_COMMANDS_MAX 32

typedef enum {
	NP_STOPPED,
	NP_PLAYING,
	NP_PAUSED,
	NP_BUFFERING,
	NP_ERROR
} np_state;

enum {
	NP_CHANGE_NONE = 0,
	NP_CHANGE_PLAYBACK = 1 << 0,
	NP_CHANGE_METADATA = 1 << 1,
	NP_CHANGE_IDENTITY = 1 << 2,
	NP_CHANGE_OWNER = 1 << 3,
	NP_CHANGE_ARTWORK = 1 << 4
};

typedef struct {
	int play, pause, stop, next, previous, seek, volume;
} np_capabilities;

/* Zero numeric values mean unknown. lossless: 0 unknown, 1 lossy, 2 lossless.
 */
typedef struct {
	char codec[32], bitrate_mode[8];
	unsigned bitrate_kbps, sample_rate_hz, bit_depth, channels, lossless;
	int replaygain_known, replaygain_millidb;
} np_audio;

typedef struct {
	char id[NP_ID_MAX], name[NP_TEXT_MAX], description[NP_TEXT_MAX];
	char homepage[NP_ART_MAX], stream_url[NP_ART_MAX], logo[NP_ART_MAX];
	char country[96], language[96], genre[NP_TEXT_MAX];
	np_audio audio;
} np_station;

typedef struct {
	char id[NP_ID_MAX], title[NP_TEXT_MAX], artist[NP_TEXT_MAX];
	char album[NP_TEXT_MAX], album_artist[NP_TEXT_MAX],
		station[NP_TEXT_MAX];
	char content_type[64], artwork[NP_ART_MAX];
	char genre[NP_TEXT_MAX], comment[NP_TEXT_MAX], subtitle[NP_TEXT_MAX];
	char url[NP_ART_MAX], info_link[NP_ART_MAX];
	np_audio audio, transport;
	unsigned year, track_number, disc_number;
} np_track;

typedef struct {
	int active, live, duration_known;
	char source[NP_SOURCE_MAX], source_name[NP_NAME_MAX],
		session_id[NP_TOKEN_MAX];
	uint64_t generation, activated_ms, last_update_ms, position_ms,
		duration_ms;
	uint64_t position_clock_ms, metadata_revision, playback_revision;
	uint64_t playlist_revision, artwork_revision;
	np_state state;
	np_capabilities capabilities;
	np_track track;
	np_station station;
} np_status;

typedef struct {
	uint64_t id, generation;
	char command[16];
	int64_t value;
} np_command;

typedef struct np_manager np_manager;

np_manager *np_create(unsigned lease_seconds, unsigned command_limit);
void np_destroy(np_manager *m);
uint64_t np_monotonic_ms(void);
const np_status *np_current(np_manager *m);
uint64_t np_position(np_manager *m);
int np_expire(np_manager *m);
int np_demo(np_manager *m);
int np_demo_next(np_manager *m);
int np_playerstatus_json(np_manager *m, char *out, size_t cap);
int np_public_json(np_manager *m, char *out, size_t cap);
/* Handle an LMS command array inside a Comet message. Returns 1 if handled. */
int np_lms_query_json(
	np_manager *m, const char *json, size_t len, char *out, size_t cap);
/* Decode one bounded Comet command array for native protocol modules. */
int np_lms_arguments(const char *json,
		     size_t len,
		     char *player,
		     size_t capacity,
		     char arguments[][512],
		     int maximum);

/* Returns an HTTP status code and writes a JSON response. changed is set when
 * SqueezePlay-visible state changed. */
int np_api(np_manager *m,
	   const char *method,
	   const char *path,
	   const char *body,
	   size_t body_len,
	   char *out,
	   size_t cap,
	   int *changed);

/* Queue a native LMS command without changing confirmed playback state. */
int np_queue_command(np_manager *m, const char *command, int64_t value);

#endif
