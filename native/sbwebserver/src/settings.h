#ifndef SB_SETTINGS_H
#define SB_SETTINGS_H
typedef struct { int port, enabled, autostart, readonly; char name[64]; } Settings;
void settings_load(Settings *s); int settings_save(const Settings *s);
#endif
