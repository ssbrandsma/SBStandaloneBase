#include "http_server.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc,char**argv){Settings s;settings_load(&s);const char*root="/usr/share/jive/applets/SBWebserver/web";for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--port")&&i+1<argc)s.port=atoi(argv[++i]);else if(!strcmp(argv[i],"--web-root")&&i+1<argc)root=argv[++i];else if(!strcmp(argv[i],"--config-dir")&&i+1<argc){
#ifdef _WIN32
_putenv_s("SBWEBSERVER_CONFIG_DIR",argv[++i]);
#else
setenv("SBWEBSERVER_CONFIG_DIR",argv[++i],1);
#endif
}printf("SBWebserver listening on 0.0.0.0:%d\n",s.port);return http_server_run(s.port,root);}
