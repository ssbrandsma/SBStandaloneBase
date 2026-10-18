/* Streaming ADTS/MP3 framing and MPEG-TS audio extraction for the HLS relay.
 * No decoding, re-encoding or segment allocations. ID3 is bounded and removed
 * from audio; an optional caller-owned parser extracts timed metadata.
 * complete elementary frames pass to the existing SqueezePlay decoder. */
#include "hls.h"
#include <string.h>

/** Initialize caller-owned framing state and its complete-frame callback.
 * @param a State; output callback; opaque callback context.
 */
void hls_audio_init(hls_audio *a, hls_output output, void *opaque)
{
	memset(a, 0, sizeof(*a));
	a->output = output;
	a->opaque = opaque;
}

/** Determine the next elementary frame or ID3 length.
 * @param a Framing state; frame length output; codec/config outputs.
 * @return 0 need more bytes, 1 frame, 2 ID3, -1 invalid/unsupported input.
 */
static int
frame_header(hls_audio *a, size_t *length, int *codec, unsigned *config)
{
	const unsigned char *b = a->buffer;
	if (a->size < 4)
		return 0;
	if (!memcmp(b, "ID3", 3)) {
		if (a->size < 10)
			return 0;
		if (b[3] < 2 || b[3] > 4 || (b[6] | b[7] | b[8] | b[9]) & 128)
			return -1;
		*length = ((size_t)b[6] << 21) | ((size_t)b[7] << 14) |
			  ((size_t)b[8] << 7) | b[9];
		*length += 10 + (b[3] == 4 && (b[5] & 16) ? 10 : 0);
		if (*length > 1024u * 1024u)
			return -1;
		return 2;
	}
	if (b[0] != 255 || (b[1] & 224) != 224)
		return -1;
	if ((b[1] & 246) == 240) {
		if (a->size < 7)
			return 0;
		unsigned rate = (b[2] >> 2) & 15,
			 channels = ((b[2] & 1) << 2) | (b[3] >> 6);
		*length = ((b[3] & 3u) << 11) | (b[4] << 3) | (b[5] >> 5);
		if (rate > 12 || !channels || (b[6] & 3) ||
		    *length < (b[1] & 1 ? 8u : 10u))
			return -1;
		*codec = HLS_AAC;
		*config = ((b[1] & 8u) << 12) | ((b[2] >> 6) << 8) |
			  (rate << 4) | channels;
		return 1;
	}
	unsigned version = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3;
	unsigned bitrate = b[2] >> 4, rate = (b[2] >> 2) & 3;
	static const unsigned rates[] = {44100, 48000, 32000};
	static const unsigned v1[] = {0,
				      32,
				      40,
				      48,
				      56,
				      64,
				      80,
				      96,
				      112,
				      128,
				      160,
				      192,
				      224,
				      256,
				      320};
	static const unsigned v2[] = {
		0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
	if (version == 1 || layer != 1 || !bitrate || bitrate == 15 ||
	    rate == 3)
		return -1;
	unsigned frequency = rates[rate] / (version == 3   ? 1
					    : version == 2 ? 2
							   : 4);
	*length = (version == 3 ? 144000 : 72000) *
			  (version == 3 ? v1[bitrate] : v2[bitrate]) /
			  frequency +
		  ((b[2] >> 1) & 1);
	*codec = HLS_MP3;
	*config = (version << 8) | (rate << 4) | ((b[3] >> 6) == 3);
	return 1;
}

/** Consume incremental elementary bytes, skipping ID3 without storing its body.
 * @param a State; data/size input bytes. Returns zero or -1 with a->error.
 */
int hls_audio_feed(hls_audio *a, const unsigned char *data, size_t size)
{
	while (size || a->size) {
		if (a->skip) {
			size_t n = size < a->skip ? size : a->skip;
			if (a->metadata)
				hls_id3_feed(a->metadata, data, n);
			a->skip -= n;
			size -= n;
			data += n;
			if (!size)
				return 0;
		}
		if (size) {
			size_t n = sizeof(a->buffer) - a->size;
			if (n > size)
				n = size;
			memcpy(a->buffer + a->size, data, n);
			a->size += n;
			data += n;
			size -= n;
		}
		size_t length = 0;
		int codec = 0;
		unsigned config = 0;
		int result = frame_header(a, &length, &codec, &config);
		if (result < 0 || (length > sizeof(a->buffer) && result != 2)) {
			a->error = "unsupported/malformed elementary audio "
				   "(expected ADTS AAC or MP3)";
			return -1;
		}
		if (!result || (result == 1 && a->size < length)) {
			if (size)
				continue;
			return 0;
		}
		if (result == 2) {
			a->id3++;
			if (a->metadata)
				hls_id3_feed(a->metadata,
					     a->buffer,
					     length < a->size ? length
							      : a->size);
			if (length > a->size) {
				a->skip = length - a->size;
				a->size = 0;
			} else {
				memmove(a->buffer,
					a->buffer + length,
					a->size - length);
				a->size -= length;
			}
			continue;
		}
		if (a->codec && (a->codec != codec || a->config != config)) {
			a->error = "audio codec configuration changed";
			return -1;
		}
		a->codec = codec;
		a->config = config;
		if (a->output(a->opaque, a->buffer, length, codec)) {
			a->error = "downstream stopped";
			return -1;
		}
		a->frames++;
		static const unsigned aac_rates[] = {96000,
						     88200,
						     64000,
						     48000,
						     44100,
						     32000,
						     24000,
						     22050,
						     16000,
						     12000,
						     11025,
						     8000,
						     7350};
		static const unsigned mp3_rates[] = {44100, 48000, 32000};
		unsigned rate, samples;
		if (codec == HLS_AAC) {
			rate = aac_rates[(a->buffer[2] >> 2) & 15];
			samples = 1024;
		} else {
			unsigned version = (a->buffer[1] >> 3) & 3;
			rate = mp3_rates[(a->buffer[2] >> 2) & 3] /
			       (version == 3   ? 1
				: version == 2 ? 2
					       : 4);
			samples = version == 3 ? 1152 : 576;
		}
		a->elapsed_us += (uint64_t)samples * 1000000 / rate;
		memmove(a->buffer, a->buffer + length, a->size - length);
		a->size -= length;
	}
	return 0;
}

/** Initialize transport-stream demux state; preserves the separate audio state.
 * @param ts State; audio destination framing state.
 */
void hls_ts_init(hls_ts *ts, hls_audio *audio)
{
	memset(ts, 0, sizeof(*ts));
	ts->audio = audio;
	ts->pmt_pid = ts->audio_pid = ts->audio_cc = ts->pat.cc = ts->pmt.cc =
		-1;
	ts->metadata_pid = ts->metadata_cc = -1;
}

/** Validate a complete PSI section and select its program/audio PID.
 * @param ts Demux; data/size section including CRC; pat selects PAT vs PMT.
 * @return Zero or -1 for malformed/unsupported program tables.
 */
static int
table_section(hls_ts *ts, const unsigned char *data, size_t size, int pat)
{
	uint32_t crc = UINT32_MAX;
	for (size_t i = 0; i < size; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^
			      ((crc & 0x80000000u) ? 0x04c11db7u : 0);
	}
	if (crc || size < (pat ? 12u : 16u) || data[0] != (pat ? 0 : 2) ||
	    !(data[1] & 128))
		return -1;
	if (!(data[5] & 1))
		return 0;
	if (data[6] || data[7])
		return -1; /* multiple program-table sections are not supported
			    */
	if (pat) {
		if ((size - 12) % 4)
			return -1;
		for (size_t i = 8; i + 4 <= size - 4; i += 4) {
			unsigned program =
				((unsigned)data[i] << 8) | data[i + 1];
			if (program) {
				int pid =
					((data[i + 2] & 31) << 8) | data[i + 3];
				if (ts->pmt_pid >= 0 &&
				    (ts->pmt_pid != pid ||
				     ts->program != program))
					return -1;
				ts->pmt_pid = pid;
				ts->program = program;
				return 0;
			}
		}
		return -1;
	}
	if ((((unsigned)data[3] << 8) | data[4]) != ts->program)
		return -1;
	size_t i = 12 + (((data[10] & 15) << 8) | data[11]);
	if (i > size - 4)
		return -1;
	int selected = -1, metadata_pid = -1;
	while (i < size - 4) {
		if (i + 5 > size - 4)
			return -1;
		size_t next = i + 5 + (((data[i + 3] & 15) << 8) | data[i + 4]);
		if (next > size - 4)
			return -1;
		if (selected < 0 &&
		    (data[i] == 15 || data[i] == 3 || data[i] == 4))
			selected = ((data[i + 1] & 31) << 8) | data[i + 2];
		/* Timed ID3 uses stream_type 0x15; private-data type 0x06
		 * additionally requires the registered ID3 descriptor. */
		int id3 = data[i] == 0x15;
		for (size_t d = i + 5; d + 2 <= next;) {
			size_t length = data[d + 1];
			if (length > next - d - 2)
				break;
			if (data[i] == 6 && data[d] == 5 && length >= 4 &&
			    !memcmp(data + d + 2, "ID3 ", 4))
				id3 = 1;
			d += 2 + length;
		}
		if (id3 && metadata_pid < 0)
			metadata_pid = ((data[i + 1] & 31) << 8) | data[i + 2];
		i = next;
	}
	if (selected < 0 || (ts->audio_pid >= 0 && ts->audio_pid != selected))
		return -1;
	ts->audio_pid = selected;
	if (ts->metadata_pid != metadata_pid) {
		ts->metadata_pid = metadata_pid;
		ts->metadata_cc = -1;
		ts->metadata_active = 0;
		ts->has_metadata_pts = 0;
		if (ts->metadata)
			hls_id3_reset(ts->metadata);
	}
	return 0;
}

/** Accumulate PSI sections across TS packets, including pointer-field tails.
 * @param ts Demux; s section state; data/size payload; start PUSI; pat PAT
 * flag.
 * @return Zero or -1 on invalid section boundaries/CRC.
 */
static int section_feed(hls_ts *ts,
			hls_section *s,
			const unsigned char *data,
			size_t size,
			int start,
			int pat)
{
	if (start) {
		if (!size)
			return -1;
		unsigned pointer = *data++;
		size--;
		if (pointer > size)
			return -1;
		if (s->size && section_feed(ts, s, data, pointer, 0, pat))
			return -1;
		if (s->size)
			return -1;
		data += pointer;
		size -= pointer;
	} else if (!s->size)
		return 0;
	while (size) {
		if (!s->size && *data == 255)
			return 0;
		size_t need = s->need ? s->need : 3;
		size_t n = need - s->size;
		if (n > size)
			n = size;
		memcpy(s->data + s->size, data, n);
		s->size += n;
		data += n;
		size -= n;
		if (!s->need && s->size == 3) {
			s->need = 3 + (((s->data[1] & 15) << 8) | s->data[2]);
			if (s->need < 12 || s->need > sizeof(s->data))
				return -1;
		}
		if (s->need && s->size == s->need) {
			if (table_section(ts, s->data, s->need, pat))
				return -1;
			s->size = s->need = 0;
		}
	}
	return 0;
}

/** Strip one audio PES header incrementally and feed only its payload.
 * @param ts Demux; data/size TS payload; start PUSI flag. Returns 0 or -1.
 */
/** Validate MPEG's 33-bit timestamp marker bits before decoding PTS. */
static int pts(const unsigned char *h, size_t n, uint64_t *value)
{
	if (n < 14 || !(h[7] & 128) || h[8] < 5 || !(h[9] & 1) ||
	    !(h[11] & 1) || !(h[13] & 1))
		return 0;
	*value = ((uint64_t)(h[9] & 14) << 29) | ((uint64_t)h[10] << 22) |
		 ((uint64_t)(h[11] & 254) << 14) | ((uint64_t)h[12] << 7) |
		 (h[13] >> 1);
	return 1;
}

static int
pes_feed(hls_ts *ts, const unsigned char *data, size_t size, int start)
{
	if (start) {
		if (ts->pes_size || (ts->pes_active && ts->remaining > 0))
			return -1;
		ts->pes_active = 1;
		ts->pes_need = 9;
		ts->remaining = -1;
	}
	if (!ts->pes_active)
		return 0;
	while (ts->pes_need && size) {
		size_t n = ts->pes_need - ts->pes_size;
		if (n > size)
			n = size;
		memcpy(ts->pes + ts->pes_size, data, n);
		ts->pes_size += n;
		data += n;
		size -= n;
		if (ts->pes_size < ts->pes_need)
			return 0;
		if (ts->pes_need == 9) {
			if (memcmp(ts->pes, "\0\0\1", 3) || ts->pes[3] < 192 ||
			    ts->pes[3] > 223 || (ts->pes[6] & 192) != 128)
				return -1;
			unsigned length = (ts->pes[4] << 8) | ts->pes[5];
			if (length && length < 3u + ts->pes[8])
				return -1;
			ts->remaining =
				length ? (int)length - 3 - ts->pes[8] : -1;
			if (ts->pes[8]) {
				ts->pes_need = 9 + ts->pes[8];
				continue;
			}
		}
		if (pts(ts->pes, ts->pes_size, &ts->audio_pts)) {
			ts->has_audio_pts = 1;
			ts->audio_pts_position_ms =
				ts->audio->elapsed_us / 1000;
		}
		ts->pes_size = ts->pes_need = 0;
	}
	if (ts->pes_need)
		return 0;
	if (ts->remaining >= 0 && size > (size_t)ts->remaining)
		size = (size_t)ts->remaining;
	if (hls_audio_feed(ts->audio, data, size)) {
		ts->error = ts->audio->error;
		return -1;
	}
	if (ts->remaining >= 0)
		ts->remaining -= (int)size;
	return 0;
}

/** Process a complete TS packet with continuity, adaptation and PID checks.
 * @param ts Demux; b exactly 188 bytes. Returns zero or -1.
 */
/** Isolate timed-metadata PES parsing from audio errors and continuity loss. */
static void
metadata_pes(hls_ts *ts, const unsigned char *p, size_t n, int start)
{
	if (!ts->metadata)
		return;
	if (start) {
		ts->metadata_active = 1;
		ts->metadata_pes_size = 0;
		ts->metadata_pes_need = 9;
		ts->metadata_remaining = -1;
		hls_id3_reset(ts->metadata);
	}
	if (!ts->metadata_active)
		return;
	while (ts->metadata_pes_need && n) {
		size_t take = ts->metadata_pes_need - ts->metadata_pes_size;
		if (take > n)
			take = n;
		memcpy(ts->metadata_pes + ts->metadata_pes_size, p, take);
		p += take;
		n -= take;
		ts->metadata_pes_size += take;
		if (ts->metadata_pes_size < ts->metadata_pes_need)
			return;
		if (ts->metadata_pes_need == 9) {
			const unsigned char *h = ts->metadata_pes;
			unsigned length = (h[4] << 8) | h[5];
			if (memcmp(h, "\0\0\1", 3) || h[3] != 0xbd ||
			    (h[6] & 192) != 128 ||
			    (length && length < 3u + h[8])) {
				ts->metadata_active = 0;
				return;
			}
			ts->metadata_remaining =
				length ? (int)length - 3 - h[8] : -1;
			if (h[8]) {
				ts->metadata_pes_need = 9 + h[8];
				continue;
			}
		}
		ts->has_metadata_pts = pts(ts->metadata_pes,
					   ts->metadata_pes_size,
					   &ts->metadata_pts);
		ts->metadata_pes_size = ts->metadata_pes_need = 0;
	}
	if (ts->metadata_remaining >= 0 && n > (size_t)ts->metadata_remaining)
		n = (size_t)ts->metadata_remaining;
	hls_id3_feed(ts->metadata, p, n);
	if (ts->metadata_remaining >= 0)
		ts->metadata_remaining -= (int)n;
}

static int packet(hls_ts *ts, const unsigned char *b)
{
	if (b[0] != 71)
		return -1;
	unsigned pid = ((b[1] & 31) << 8) | b[2], control = (b[3] >> 4) & 3;
	int metadata = (int)pid == ts->metadata_pid;
	if ((b[1] & 128) || (b[3] & 192) || !control) {
		if (metadata) {
			ts->metadata_active = 0;
			if (ts->metadata)
				hls_id3_reset(ts->metadata);
			return 0;
		}
		return -1;
	}
	size_t offset = 4;
	if (control & 2) {
		offset += 1 + b[4];
		if (offset > 188) {
			if (metadata) {
				ts->metadata_active = 0;
				if (ts->metadata)
					hls_id3_reset(ts->metadata);
				return 0;
			}
			return -1;
		}
	}
	if (!(control & 1) || offset == 188)
		return 0;
	if ((int)pid == ts->metadata_pid) {
		int cc = b[3] & 15;
		if (ts->metadata_cc == cc)
			return 0;
		if (ts->metadata_cc >= 0 &&
		    cc != ((ts->metadata_cc + 1) & 15)) {
			ts->metadata_active = 0;
			if (ts->metadata)
				hls_id3_reset(ts->metadata);
		}
		ts->metadata_cc = cc;
		metadata_pes(ts, b + offset, 188 - offset, b[1] & 64);
		return 0;
	}
	int *previous = pid == 0		    ? &ts->pat.cc
			: (int)pid == ts->pmt_pid   ? &ts->pmt.cc
			: (int)pid == ts->audio_pid ? &ts->audio_cc
						    : NULL;
	if (!previous)
		return 0;
	int cc = b[3] & 15;
	if (*previous == cc)
		return 0; /* retransmission */
	if (*previous >= 0 && cc != ((*previous + 1) & 15))
		return -1;
	*previous = cc;
	int start = b[1] & 64;
	if (!pid)
		return section_feed(
			ts, &ts->pat, b + offset, 188 - offset, start, 1);
	if ((int)pid == ts->pmt_pid)
		return section_feed(
			ts, &ts->pmt, b + offset, 188 - offset, start, 0);
	return pes_feed(ts, b + offset, 188 - offset, start);
}

/** Consume arbitrary network chunks of 188-byte MPEG-TS packets.
 * @param ts State; data/size bytes. Returns zero or -1 with a fixed error.
 */
int hls_ts_feed(hls_ts *ts, const unsigned char *data, size_t size)
{
	while (size) {
		size_t n = sizeof(ts->packet) - ts->size;
		if (n > size)
			n = size;
		memcpy(ts->packet + ts->size, data, n);
		ts->size += n;
		data += n;
		size -= n;
		if (ts->size == sizeof(ts->packet)) {
			ts->size = 0;
			if (packet(ts, ts->packet)) {
				if (!ts->error)
					ts->error =
						"invalid/unsupported MPEG-TS "
						"program, continuity or PES";
				return -1;
			}
		}
	}
	return 0;
}
