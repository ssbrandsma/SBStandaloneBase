#ifndef SB_SETTINGS_H
#define SB_SETTINGS_H

typedef struct {
	int port, enabled, autostart, readonly;
	char name[64];
    int artwork_external_fallback_enabled, artwork_external_fallback_timeout;
    unsigned artwork_external_fallback_max_bytes;
    char artwork_external_fallback_base_url[512];
} Settings;

void settings_load(Settings *s);
int settings_save(const Settings *s);
#endif
