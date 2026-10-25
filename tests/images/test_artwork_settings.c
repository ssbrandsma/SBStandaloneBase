/* Host regression tests for artwork fallback defaults and explicit opt-out. */
#define _POSIX_C_SOURCE 200809L
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Verify default loading, legacy settings and persisted user choices. */
int main(void)
{
	char directory[] = "/tmp/sb-artwork-settings.XXXXXX", path[256];
	Settings settings;
	assert(mkdtemp(directory));
	assert(setenv("SBWEBSERVER_CONFIG_DIR", directory, 1) == 0);
	snprintf(path, sizeof path, "%s/settings.conf", directory);
	settings_load(&settings);
	assert(settings.artwork_external_fallback_enabled == 1);
	assert(settings.artwork_external_fallback_timeout == 3);
	assert(settings.artwork_external_fallback_max_bytes == 524288);
	assert(!strcmp(settings.artwork_external_fallback_base_url,
		       "https://api.lms-community.org/img/compatible/"));
	FILE *file = fopen(path, "w");
	assert(file);
	fputs("port=8080\n", file);
	assert(fclose(file) == 0);
	settings_load(&settings);
	assert(settings.port == 8080 && settings.artwork_external_fallback_enabled == 1);
	settings.artwork_external_fallback_enabled = 0;
	assert(settings_save(&settings) == 0);
	settings_load(&settings);
	assert(settings.artwork_external_fallback_enabled == 0);
	settings.artwork_external_fallback_enabled = 1;
	settings.artwork_external_fallback_timeout = 5;
	settings.artwork_external_fallback_max_bytes = 12345;
	assert(settings_save(&settings) == 0);
	settings_load(&settings);
	assert(settings.artwork_external_fallback_enabled == 1);
	assert(settings.artwork_external_fallback_timeout == 5);
	assert(settings.artwork_external_fallback_max_bytes == 12345);
	assert(!strcmp(settings.artwork_external_fallback_base_url,
		       "https://api.lms-community.org/img/compatible/"));
	file = fopen(path, "w");
	assert(file);
	fputs("artwork_external_fallback_timeout=999\nartwork_external_fallback_max_bytes=999999999\n", file);
	assert(fclose(file) == 0);
	settings_load(&settings);
	assert(settings.artwork_external_fallback_enabled == 1);
	assert(settings.artwork_external_fallback_timeout == 3);
	assert(settings.artwork_external_fallback_max_bytes == 524288);
	assert(unlink(path) == 0);
	assert(rmdir(directory) == 0);
	puts("Artwork fallback default and explicit opt-out passed");
	return 0;
}
