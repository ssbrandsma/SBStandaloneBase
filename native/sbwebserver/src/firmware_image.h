/* Read-only native firmware container validation, shared by preparation/tests.
 */
#ifndef SB_FIRMWARE_IMAGE_H
#define SB_FIRMWARE_IMAGE_H
#include <stddef.h>
#include <stdint.h>

#define FW_IMAGE_LIMIT (24u * 1024u * 1024u)

typedef struct {
	char version[64];
	char sha1[41], sha256[65];
	uint32_t size;
	uint32_t kernel_size, root_size;
} FirmwareImage;

int firmware_version_compare(const char *candidate,
			     const char *installed,
			     int *result);
int firmware_image_validate(const char *path,
			    const char *expected_checksum,
			    unsigned board_revision,
			    FirmwareImage *image,
			    char *error,
			    size_t capacity);
int firmware_checksum_normalize(const char *input, char output[65]);
#endif
