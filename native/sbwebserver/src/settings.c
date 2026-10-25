/*
 * Management webserver settings persistence.
 *
 * Loads defaults and stores the Settings values used by main.c and
 * http_server.c, including listener and read-only management configuration.
 * Browser pairing and session state are handled separately by auth.c.
 */

#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
/**
 * @brief Handle dir.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
static const char *dir(void)
{
	const char *e = getenv("SBWEBSERVER_CONFIG_DIR");
	if (e && *e)
		return e;
	if (access("/mnt/storage", W_OK) == 0)
		return "/mnt/storage/sbwebserver";
	return "/tmp/sbwebserver";
}

/**
 * @brief Handle settings load.
 * @param s Object referenced by s.
 */
void settings_load(Settings *s)
{
	FILE *f;
	char k[80], v[1024], p[256];
	memset(s, 0, sizeof(*s));
    s->artwork_external_fallback_enabled=1;
    s->artwork_external_fallback_timeout=3;
    s->artwork_external_fallback_max_bytes=512u*1024u;
    snprintf(s->artwork_external_fallback_base_url,sizeof s->artwork_external_fallback_base_url,
        "https://api.lms-community.org/img/compatible/");
	s->port = 80;
	s->enabled = 1;
	s->autostart = 1;
	snprintf(s->name, sizeof s->name, "Squeezebox Radio");
	snprintf(p, sizeof p, "%s/settings.conf", dir());
	f = fopen(p, "r");
	if (!f)
		return;
	while (fscanf(f, "%79[^=]=%1023[^\n]\n", k, v) == 2) {
        if (!strcmp(k,"artwork_external_fallback_enabled")) s->artwork_external_fallback_enabled=atoi(v)==1;
        else if (!strcmp(k,"artwork_external_fallback_timeout")) s->artwork_external_fallback_timeout=atoi(v);
        else if (!strcmp(k,"artwork_external_fallback_max_bytes")) s->artwork_external_fallback_max_bytes=(unsigned)strtoul(v,NULL,10);
        else if (!strcmp(k,"artwork_external_fallback_base_url")) snprintf(s->artwork_external_fallback_base_url,sizeof s->artwork_external_fallback_base_url,"%.511s",v);
		else if (!strcmp(k, "port"))
			s->port = atoi(v);
		else if (!strcmp(k, "enabled"))
			s->enabled = atoi(v) != 0;
		else if (!strcmp(k, "autostart"))
			s->autostart = atoi(v) != 0;
		else if (!strcmp(k, "readonly"))
			s->readonly = atoi(v) != 0;
		else if (!strcmp(k, "name"))
			snprintf(s->name, sizeof s->name, "%.63s", v);
	}
	fclose(f);
    if (s->artwork_external_fallback_timeout<1 || s->artwork_external_fallback_timeout>5) s->artwork_external_fallback_timeout=3;
    if (!s->artwork_external_fallback_max_bytes || s->artwork_external_fallback_max_bytes>512u*1024u) s->artwork_external_fallback_max_bytes=512u*1024u;
	if (s->port < 1 || s->port > 65535)
		s->port = 80;
}

/**
 * @brief Handle settings save.
 * @param s Object referenced by s.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int settings_save(const Settings *s)
{
	char p[256], t[280];
#ifdef _WIN32
	_mkdir(dir());
#else
	mkdir(dir(), 0755);
#endif
	snprintf(p, sizeof p, "%s/settings.conf", dir());
	snprintf(t, sizeof t, "%s/settings.conf.tmp", dir());
	FILE *f = fopen(t, "w");
	if (!f)
		return -1;
	fprintf(f,
		"port=%d\nenabled=%d\nautostart=%d\nreadonly=%d\nname=%s\n",
		s->port,
		s->enabled,
		s->autostart,
		s->readonly,
		s->name);
    fprintf(f,"artwork_external_fallback_enabled=%d\nartwork_external_fallback_base_url=%s\nartwork_external_fallback_timeout=%d\nartwork_external_fallback_max_bytes=%u\n",
        s->artwork_external_fallback_enabled,s->artwork_external_fallback_base_url,
        s->artwork_external_fallback_timeout,s->artwork_external_fallback_max_bytes);
	fflush(f);
#ifndef _WIN32
	fsync(fileno(f));
#endif
	if (fclose(f))
		return -1;
	return rename(t, p);
}
