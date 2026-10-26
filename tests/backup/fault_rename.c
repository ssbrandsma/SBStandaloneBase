/* Host-only fault injection: fail a late restore commit, not its rollback. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int rename(const char *source, const char *destination)
{
	static int failed;
	int (*original)(const char *, const char *) =
		dlsym(RTLD_NEXT, "rename");
	if (getenv("SB_BACKUP_TEST_RENAME_FAILURE") && !failed &&
	    strstr(source, ".restore-new") &&
	    strstr(destination, "StandaloneRadio.lua")) {
		failed = 1;
		errno = EIO;
		return -1;
	}
	return original(source, destination);
}
