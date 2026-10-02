#include "http_server.h"
#include "mongoose.h"
#include "system_info.h"
#include "applets.h"
#include "settings.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
static int is(struct mg_str a,const char*b){return a.len==strlen(b)&&memcmp(a.buf,b,a.len)==0;}
static void json(struct mg_connection*c,int status,const char*b){mg_http_reply(c,status,"Content-Type: application/json\r\nCache-Control: no-store\r\n","%s",b);}
static void fn(struct mg_connection*c,int ev,void*data){if(ev!=MG_EV_HTTP_MSG)return;struct mg_http_message*m=data;char b[32768];Settings s;const char*root=(const char*)c->fn_data;
if(is(m->uri,"/system-information")||is(m->uri,"/overview-information")){system_json(b,sizeof b);json(c,200,b);return;}
if(is(m->uri,"/installed-applets")){applets_json(b,sizeof b);json(c,200,b);return;}
if(is(m->uri,"/webserver-settings")){settings_load(&s);if(is(m->method,"GET")){snprintf(b,sizeof b,"{\"port\":%d,\"enabled\":%s,\"autostart\":%s,\"readonly\":%s,\"name\":\"%s\"}",s.port,s.enabled?"true":"false",s.autostart?"true":"false",s.readonly?"true":"false",s.name);json(c,200,b);return;}if(is(m->method,"PUT")){struct mg_str body=m->body;double port;bool bv;char*name;if(mg_json_get_num(body,"$.port",&port))s.port=(int)port;if(mg_json_get_bool(body,"$.enabled",&bv))s.enabled=bv;if(mg_json_get_bool(body,"$.autostart",&bv))s.autostart=bv;if(mg_json_get_bool(body,"$.readonly",&bv))s.readonly=bv;name=mg_json_get_str(body,"$.name");if(name)snprintf(s.name,sizeof s.name,"%s",name);if(s.port<1||s.port>65535||settings_save(&s)){json(c,400,"{\"error\":\"Invalid settings or write failure\"}");return;}json(c,200,"{\"ok\":true}");return;}}
if(is(m->uri,"/standalone-radio/stations")){if(is(m->method,"GET")){stations_json(b,sizeof b);json(c,200,b);return;}if(is(m->method,"PUT")){if(stations_put(m->body.buf)){json(c,400,"{\"error\":\"Invalid CSV station data\"}");return;}json(c,200,"{\"ok\":true}");return;}}
if(is(m->uri,"/standalone-radio/stations.csv")){stations_csv(b,sizeof b);mg_http_reply(c,200,"Content-Type: text/csv\r\nContent-Disposition: attachment; filename=stations.csv\r\n","%s",b);return;}
if(is(m->uri,"/standalone-radio/import")&&is(m->method,"POST")){if(stations_import(m->body.buf,m->body.len)){json(c,400,"{\"error\":\"Invalid CSV upload\"}");return;}json(c,200,"{\"ok\":true}");return;}
struct mg_http_serve_opts o={.root_dir=root?root:"web"};mg_http_serve_dir(c,m,&o);}
int http_server_run(int port,const char*root){char url[64];snprintf(url,sizeof url,"http://0.0.0.0:%d",port);struct mg_mgr mgr;mg_mgr_init(&mgr);if(!mg_http_listen(&mgr,url,fn,(void*)root)){fprintf(stderr,"Cannot listen on %s\n",url);return 1;}for(;;)mg_mgr_poll(&mgr,1000);}
