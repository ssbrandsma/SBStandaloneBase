#define _POSIX_C_SOURCE 200809L
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    char directory[]="/tmp/sb-artwork-settings.XXXXXX",path[256];
    assert(mkdtemp(directory));assert(!setenv("SBWEBSERVER_CONFIG_DIR",directory,1));
    Settings s,copy;settings_load(&s);
    assert(!s.artwork_external_fallback_enabled && s.artwork_external_fallback_timeout==3);
    assert(s.artwork_external_fallback_max_bytes==524288);
    assert(!strcmp(s.artwork_external_fallback_base_url,"https://api.lms-community.org/img/compatible/"));
    s.artwork_external_fallback_enabled=1;s.artwork_external_fallback_timeout=5;
    s.artwork_external_fallback_max_bytes=12345;
    assert(!settings_save(&s));settings_load(&copy);
    assert(copy.artwork_external_fallback_enabled==1 && copy.artwork_external_fallback_timeout==5);
    assert(copy.artwork_external_fallback_max_bytes==12345);
    assert(!strcmp(s.artwork_external_fallback_base_url,copy.artwork_external_fallback_base_url));
    snprintf(path,sizeof path,"%s/settings.conf",directory);
    FILE *f=fopen(path,"w");assert(f);
    fprintf(f,"artwork_external_fallback_timeout=999\nartwork_external_fallback_max_bytes=999999999\n");fclose(f);
    settings_load(&s);assert(s.artwork_external_fallback_timeout==3 && s.artwork_external_fallback_max_bytes==524288);
    unlink(path);rmdir(directory);puts("artwork fallback settings defaults, persistence and limit clamping passed");
}
