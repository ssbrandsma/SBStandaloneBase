/*
 * Read-only SqueezeOS firmware inspection for the Base firmware preparer.
 * Streams the existing native ZIP image without extracting payload files.
 * Uses Mongoose hashes and the existing zlib dependency; never flashes,
 * mounts, executes archive contents or changes storage configuration.
 */
#include "firmware_image.h"
#include "mongoose.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

/** Decode an unsigned little-endian 16-bit ZIP field at p. */
static unsigned le16(const unsigned char *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/** Decode an unsigned little-endian 32-bit ZIP field at p. */
static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/** Parse strict release/revision text into four bounded numeric fields. */
static int version_tuple(const char *text, unsigned long out[4])
{
	const char *p = text;
	for (unsigned i = 0; i < 4; i++) {
		unsigned digits = 0;
		out[i] = 0;
		while (isdigit((unsigned char)*p)) {
			if (++digits > 9 || out[i] > 100000000UL)
				return 0;
			out[i] = out[i] * 10 + (unsigned)(*p++ - '0');
		}
		if (!digits)
			return 0;
		if (i < 2) {
			if (*p++ != '.')
				return 0;
		} else if (i == 2) {
			if (*p++ != ' ' || *p++ != 'r')
				return 0;
		}
	}
	return *p == '\0';
}

/** Compare complete numeric release/revision values; return zero if malformed.
 */
int firmware_version_compare(const char *candidate,
			     const char *installed,
			     int *result)
{
	unsigned long a[4], b[4];
	if (!version_tuple(candidate, a) || !version_tuple(installed, b))
		return 0;
	*result = 0;
	for (unsigned i = 0; i < 4; i++) {
		if (a[i] != b[i]) {
			*result = a[i] > b[i] ? 1 : -1;
			break;
		}
	}
	return 1;
}

/** Write lowercase hex for a digest; out needs twice length plus one bytes. */
static void digest_hex(char *out, const unsigned char *bytes, size_t length)
{
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < length; i++) {
		out[2 * i] = hex[bytes[i] >> 4];
		out[2 * i + 1] = hex[bytes[i] & 15];
	}
	out[2 * length] = 0;
}

/** Validate a lowercase, fixed-length hex string without accepting suffixes. */
static int valid_hex(const char *value, size_t length)
{
	if (strlen(value) != length)
		return 0;
	for (size_t i = 0; i < length; i++)
		if (!isdigit((unsigned char)value[i]) &&
		    !(value[i] >= 'a' && value[i] <= 'f'))
			return 0;
	return 1;
}

/** Accept exactly SHA-1/SHA-256 hex, normalizing case without guessing bytes.
 */
int firmware_checksum_normalize(const char *input, char output[65])
{
	size_t length = strlen(input);
	if (length != 40 && length != 64)
		return 0;
	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)input[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F')))
			return 0;
		output[i] = (char)tolower(c);
	}
	output[length] = 0;
	return 1;
}

/** Skip bounded ZIP extras, rejecting ZIP64 and malformed TLV lengths. */
static int skip_extra(FILE *file, unsigned length)
{
	unsigned char extra[256];
	if (length > sizeof extra || fread(extra, 1, length, file) != length)
		return 0;
	for (unsigned at = 0; at < length;) {
		if (length - at < 4 || le16(extra + at) == 1)
			return 0;
		unsigned size = le16(extra + at + 2);
		at += 4;
		if (size > length - at)
			return 0;
		at += size;
	}
	return 1;
}

/** Check the entire board list and require this numeric baby revision. */
static int board_allowed(char *text, unsigned board)
{
	char *save = NULL;
	int matched = 0, count = 0;
	for (char *line = strtok_r(text, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		char *end;
		unsigned long revision;
		if (++count > 64 || strncmp(line, "baby:", 5) ||
		    !isdigit((unsigned char)line[5]))
			return 0;
		errno = 0;
		revision = strtoul(line + 5, &end, 10);
		if (errno || *end || revision > 1000000)
			return 0;
		if (revision == board)
			matched = 1;
	}
	return matched;
}

/** Verify a firmware file and its metadata. Returns 1 only on full success.
 * path is a staged regular file; expected_checksum is supplied SHA-1/SHA-256;
 * board_revision comes from the authenticated local hardware identity.
 * image receives validated sizes/version; error receives a bounded reason.
 */
int firmware_image_validate(const char *path,
			    const char *expected_checksum,
			    unsigned board_revision,
			    FirmwareImage *image,
			    char *error,
			    size_t capacity)
{
	static const char *names[] = {"jive.version",
				      "board.version",
				      "upgrade.md5",
				      "zImage",
				      "root.cramfs"};
	uint32_t offsets[5], compressed[5], sizes[5], crcs[5];
	unsigned methods[5], flags[5];
	unsigned char input[4096], output[4096], header[46], hash[32];
	char metadata[3][8193] = {{0}}, payload_md5[2][33], expected[65];
	struct stat st;
	mg_sha256_ctx sha;
	mg_sha1_ctx sha1;
	FILE *file = NULL;
	const char *reason = "Invalid native firmware image";
	int ok = 0;
	memset(image, 0, sizeof *image);
#define REJECT(message)                                                        \
	do {                                                                   \
		reason = (message);                                            \
		goto done;                                                     \
	} while (0)
	if (!firmware_checksum_normalize(expected_checksum, expected))
		REJECT("Expected checksum must be 40 (SHA-1) or 64 (SHA-256) "
		       "hex characters");
	file = fopen(path, "rb");
	if (!file || fstat(fileno(file), &st) || !S_ISREG(st.st_mode) ||
	    st.st_size < 22 || st.st_size > FW_IMAGE_LIMIT)
		REJECT("Firmware file is missing, oversized or not regular");
	mg_sha256_init(&sha);
	mg_sha1_init(&sha1);
	for (;;) {
		size_t count = fread(input, 1, sizeof input, file);
		mg_sha256_update(&sha, input, count);
		mg_sha1_update(&sha1, input, count);
		if (count < sizeof input)
			break;
	}
	if (ferror(file))
		REJECT("Cannot read staged firmware");
	mg_sha256_final(hash, &sha);
	digest_hex(image->sha256, hash, 32);
	mg_sha1_final(hash, &sha1);
	digest_hex(image->sha1, hash, 20);
	image->size = (uint32_t)st.st_size;
	if (strcmp(strlen(expected) == 40 ? image->sha1 : image->sha256,
		   expected))
		REJECT("Firmware checksum does not match supplied digest");
	rewind(file);
	for (unsigned entry = 0; entry < 5; entry++) {
		z_stream stream = {0};
		mg_md5_ctx md5;
		uint32_t remaining, produced = 0;
		uLong crc = crc32(0, Z_NULL, 0);
		int initialized = 0, ended = 0;
		char name[32];
		offsets[entry] = (uint32_t)ftell(file);
		if (fread(header, 1, 30, file) != 30 ||
		    le32(header) != 0x04034b50 ||
		    (le16(header + 6) & ~0x0806u) ||
		    le16(header + 26) != strlen(names[entry]))
			REJECT("Unsupported ZIP header, flags, extra data or "
			       "entry order");
		flags[entry] = le16(header + 6);
		unsigned extra_length = le16(header + 28);
		methods[entry] = le16(header + 8);
		crcs[entry] = le32(header + 14);
		compressed[entry] = remaining = le32(header + 18);
		sizes[entry] = le32(header + 22);
		uint32_t limit = entry < 3    ? 8192
				 : entry == 3 ? 8u * 1024u * 1024u
					      : 32u * 1024u * 1024u;
		if (!remaining || remaining > FW_IMAGE_LIMIT || !sizes[entry] ||
		    sizes[entry] > limit ||
		    (methods[entry] != 0 && methods[entry] != 8) ||
		    fread(name, 1, strlen(names[entry]), file) !=
			    strlen(names[entry]) ||
		    memcmp(name, names[entry], strlen(names[entry])))
			REJECT("Unsupported, oversized or incorrectly named "
			       "firmware entry");
		if (!skip_extra(file, extra_length))
			REJECT("Unsupported ZIP extra fields");
		if (methods[entry] == 0 && remaining != sizes[entry])
			REJECT("Stored ZIP length mismatch");
		mg_md5_init(&md5);
		if (methods[entry] == 8) {
			if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
				REJECT("Cannot initialize bounded firmware "
				       "decompressor");
			initialized = 1;
		}
		while (remaining) {
			size_t count = remaining < sizeof input ? remaining
								: sizeof input;
			if (fread(input, 1, count, file) != count) {
				if (initialized)
					inflateEnd(&stream);
				REJECT("Truncated firmware payload");
			}
			remaining -= (uint32_t)count;
			stream.next_in = input;
			stream.avail_in = (uInt)count;
			do {
				size_t length;
				const unsigned char *data;
				if (initialized) {
					stream.next_out = output;
					stream.avail_out = sizeof output;
					int rc = inflate(&stream, Z_NO_FLUSH);
					/* A full output buffer may consume the
					 * final input byte of this chunk
					 * without ending the stream. No
					 * progress then means refill, not
					 * corruption. */
					if (!ended && rc == Z_BUF_ERROR &&
					    !stream.avail_in &&
					    stream.avail_out == sizeof output)
						break;
					if (ended || (rc != Z_OK &&
						      rc != Z_STREAM_END)) {
						inflateEnd(&stream);
						REJECT("Malformed deflate "
						       "stream");
					}
					ended = rc == Z_STREAM_END;
					length = sizeof output -
						 stream.avail_out;
					data = output;
				} else {
					length = count;
					data = input;
					stream.avail_in = 0;
				}
				if (length > sizes[entry] - produced) {
					if (initialized)
						inflateEnd(&stream);
					REJECT("Expanded firmware exceeds "
					       "declared limit");
				}
				if (entry < 3) {
					if (memchr(data, 0, length)) {
						if (initialized)
							inflateEnd(&stream);
						REJECT("NUL in firmware "
						       "metadata");
					}
					memcpy(metadata[entry] + produced,
					       data,
					       length);
				}
				produced += (uint32_t)length;
				crc = crc32(crc, data, (uInt)length);
				mg_md5_update(&md5, data, length);
			} while (stream.avail_in ||
				 (initialized && !stream.avail_out && !ended));
		}
		if (initialized)
			inflateEnd(&stream);
		if (produced != sizes[entry] || (initialized && !ended) ||
		    crc != crcs[entry])
			REJECT("Firmware ZIP checksum or length mismatch");
		mg_md5_final(&md5, hash);
		if (entry >= 3)
			digest_hex(payload_md5[entry - 3], hash, 16);
	}
	long central = ftell(file);
	for (unsigned entry = 0; entry < 5; entry++) {
		char name[32];
		if (fread(header, 1, 46, file) != 46 ||
		    le32(header) != 0x02014b50 ||
		    le16(header + 8) != flags[entry] ||
		    le16(header + 10) != methods[entry] ||
		    le32(header + 16) != crcs[entry] ||
		    le32(header + 20) != compressed[entry] ||
		    le32(header + 24) != sizes[entry] ||
		    le16(header + 28) != strlen(names[entry]) ||
		    le16(header + 32) || le16(header + 34) ||
		    le32(header + 42) != offsets[entry] ||
		    fread(name, 1, strlen(names[entry]), file) !=
			    strlen(names[entry]) ||
		    memcmp(name, names[entry], strlen(names[entry])))
			REJECT("ZIP central directory does not match firmware "
			       "entries");
		if (!skip_extra(file, le16(header + 30)))
			REJECT("Unsupported central ZIP extra fields");
	}
	long central_size = ftell(file) - central;
	if (fread(header, 1, 22, file) != 22 || le32(header) != 0x06054b50 ||
	    le16(header + 4) || le16(header + 6) || le16(header + 8) != 5 ||
	    le16(header + 10) != 5 ||
	    le32(header + 12) != (uint32_t)central_size ||
	    le32(header + 16) != (uint32_t)central || le16(header + 20) ||
	    fgetc(file) != EOF)
		REJECT("Invalid or trailing ZIP directory data");
	char *line_end = strpbrk(metadata[0], "\r\n");
	if (line_end)
		*line_end = 0;
	int compared;
	if (strlen(metadata[0]) >= sizeof image->version ||
	    !firmware_version_compare(metadata[0], metadata[0], &compared))
		REJECT("Malformed jive.version");
	if (!board_allowed(metadata[1], board_revision))
		REJECT("Firmware is not compatible with this baby board "
		       "revision");
	char *save = NULL;
	unsigned checksum_count = 0;
	for (char *line = strtok_r(metadata[2], "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		if (checksum_count >= 2 || strlen(line) < 34 ||
		    line[32] != ' ' || line[33] != ' ' ||
		    strcmp(line + 34, names[3 + checksum_count]))
			REJECT("Malformed or ambiguous upgrade.md5");
		line[32] = 0;
		if (!valid_hex(line, 32) ||
		    strcmp(line, payload_md5[checksum_count]))
			REJECT("Internal firmware payload MD5 mismatch");
		checksum_count++;
	}
	if (checksum_count != 2)
		REJECT("Missing firmware payload checksums");
	memcpy(image->version, metadata[0], strlen(metadata[0]) + 1);
	image->kernel_size = sizes[3];
	image->root_size = sizes[4];
	ok = 1;
done:
	if (file)
		fclose(file);
	if (capacity)
		snprintf(error, capacity, "%s", ok ? "" : reason);
	return ok;
#undef REJECT
}
