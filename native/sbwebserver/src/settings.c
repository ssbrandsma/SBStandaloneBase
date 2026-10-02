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
static const char*dir(void){const char*e=getenv("SBWEBSERVER_CONFIG_DIR");if(e&&*e)return e;if(access("/mnt/storage",W_OK)==0)return "/mnt/storage/sbwebserver";return "/tmp/sbwebserver";}
void settings_load(Settings*s){FILE*f;char k[32],v[128],p[256];memset(s,0,sizeof(*s));s->port=80;s->enabled=1;s->autostart=1;snprintf(s->name,sizeof s->name,"Squeezebox Radio");snprintf(p,sizeof p,"%s/settings.conf",dir());f=fopen(p,"r");if(!f)return;while(fscanf(f,"%31[^=]=%127[^\n]\n",k,v)==2){if(!strcmp(k,"port"))s->port=atoi(v);else if(!strcmp(k,"enabled"))s->enabled=atoi(v)!=0;else if(!strcmp(k,"autostart"))s->autostart=atoi(v)!=0;else if(!strcmp(k,"readonly"))s->readonly=atoi(v)!=0;else if(!strcmp(k,"name"))snprintf(s->name,sizeof s->name,"%.63s",v);}fclose(f);if(s->port<1||s->port>65535)s->port=80;}
int settings_save(const Settings*s){char p[256],t[280];
#ifdef _WIN32
_mkdir(dir());
#else
mkdir(dir(),0755);
#endif
snprintf(p,sizeof p,"%s/settings.conf",dir());snprintf(t,sizeof t,"%s/settings.conf.tmp",dir());FILE*f=fopen(t,"w");if(!f)return-1;fprintf(f,"port=%d\nenabled=%d\nautostart=%d\nreadonly=%d\nname=%s\n",s->port,s->enabled,s->autostart,s->readonly,s->name);fflush(f);
#ifndef _WIN32
fsync(fileno(f));
#endif
if(fclose(f))return-1;
return rename(t,p);}
