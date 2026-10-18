/* Host/QEMU driver for the repository's transactional upstream catalog merge. */
#define _POSIX_C_SOURCE 200809L
#include "repository.h"
#include <stdio.h>
#include <stdlib.h>

/** Read one bounded fixture and report changed entries; never contact a server. */
int main(int argc, char **argv)
{
	char input[16385], error[128];
	int changed = 0;
	if (argc != 2)
		return 2;
	FILE *stream = fopen(argv[1], "rb");
	if (!stream)
		return 2;
	size_t length = fread(input, 1, sizeof input, stream);
	int failed = ferror(stream);
	fclose(stream);
	if (failed || repo_merge_catalog(input, length, &changed, error, sizeof error)) {
		fputs("merge rejected\n", stderr);
		return 1;
	}
	printf("%d\n", changed);
	return 0;
}
