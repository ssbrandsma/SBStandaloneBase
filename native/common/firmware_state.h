/* Private, bounded firmware descriptor shared by sbwebserver and sbbase.
 * This is advertisement state only: no install command or flashing authority.
 */
#ifndef SB_FIRMWARE_STATE_H
#define SB_FIRMWARE_STATE_H
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/** Read an unexpired descriptor using the same clock selection as Mongoose.
 * url/installed receive bounded, NUL-terminated values; returns 0 if untrusted.
 */
static int sb_firmware_state(char url[256], char installed[64])
{
	const char *path = getenv("SB_FIRMWARE_STATE");
	if (!path)
		path = "/tmp/standalonebase/firmware.state";
	url[0] = installed[0] = 0;
	int flags = O_RDONLY;
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int fd = open(path, flags);
	if (fd < 0)
		return 0;
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0022) ||
	    st.st_uid != geteuid() || st.st_size > 511) {
		close(fd);
		return 0;
	}
	FILE *file = fdopen(fd, "r");
	if (!file) {
		close(fd);
		return 0;
	}
	unsigned long long expiry;
	int ok = fscanf(file,
			"%llu\n%255[^\n]\n%63[^\n]",
			&expiry,
			url,
			installed) == 3;
	fclose(file);
	struct timespec ts;
	int clock_ok = -1;
#ifdef CLOCK_MONOTONIC_RAW
	clock_ok = clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#endif
#ifdef CLOCK_MONOTONIC
	if (clock_ok)
		clock_ok = clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	if (clock_ok)
		return 0;
	uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	if (!ok || expiry <= now || expiry - now > 50000 ||
	    strncmp(url, "http://127.0.0.1:", 17) ||
	    !strstr(url, "/firmware/") || !strstr(url, "/custom.baby.bin"))
		return 0;
	for (const char *p = url; *p; p++)
		if ((unsigned char)*p <= 32 || *p == '"' || *p == '\\')
			return 0;
	return 1;
}
#endif
