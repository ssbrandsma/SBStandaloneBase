/* Host/QEMU driver for read-only firmware validation; never installs images. */
#include "firmware_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Run numeric comparison or validate a fixture, returning nonzero on failure.
 */
int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "compare")) {
		int result;
		if (!firmware_version_compare(argv[2], argv[3], &result))
			return 2;
		printf("%d\n", result);
		return 0;
	}
	if (argc != 4)
		return 2;
	FirmwareImage image;
	char error[256];
	if (!firmware_image_validate(argv[1],
				     argv[2],
				     (unsigned)atoi(argv[3]),
				     &image,
				     error,
				     sizeof error)) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	printf("%s %u %u\n", image.version, image.kernel_size, image.root_size);
	return 0;
}
