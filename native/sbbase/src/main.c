#define _POSIX_C_SOURCE 200809L
#include "sbbase.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CLIENTS 12
typedef struct { int fd, kind, helo; time_t keepalive; size_t used; unsigned char buf[SB_MAX_HTTP_HEADER + SB_MAX_HTTP_BODY + 1]; } client;
static int player_connected;
static char player_id[18];
static client *comet_stream;
static char comet_client_id[32];
static char catalog_data[1600]="{\"count\":0,\"item_loop\":[]}";
static sb_config *active_config;
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static int nonblock(int fd) { int f=fcntl(fd,F_GETFL,0); return f<0?-1:fcntl(fd,F_SETFL,f|O_NONBLOCK); }
static int listener(int type, unsigned port) {
    int fd=socket(AF_INET,type,0), yes=1; struct sockaddr_in a;
    if(fd<0)return -1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons((unsigned short)port);
    if(bind(fd,(struct sockaddr*)&a,sizeof a)<0 || (type==SOCK_STREAM && listen(fd,8)<0)){close(fd);return -1;} nonblock(fd);return fd;
}
static void local_ip(char out[16]) {
    int fd; struct sockaddr_in dst, local; socklen_t n=sizeof local;
    strcpy(out,"127.0.0.1");
    fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return;
    memset(&dst,0,sizeof dst);dst.sin_family=AF_INET;dst.sin_port=htons(9);dst.sin_addr.s_addr=htonl(0x08080808u);
    if(connect(fd,(struct sockaddr*)&dst,sizeof dst)==0&&getsockname(fd,(struct sockaddr*)&local,&n)==0)
        if(local.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))inet_ntop(AF_INET,&local.sin_addr,out,16);
    close(fd);
}
static void close_client(client *c){if(comet_stream==c)comet_stream=NULL;if(c->fd>=0)close(c->fd);c->fd=-1;c->used=0;}
static void http_reply(int fd,const char *status,const char *body){char h[256];size_t n=strlen(body);int l=snprintf(h,sizeof h,"HTTP/1.1 %s\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nContent-Length: %lu\r\nConnection: keep-alive\r\n\r\n",status,(unsigned long)n);send(fd,h,(size_t)l,0);send(fd,body,n,0);}
static void stream_start(client*c,const char*body){char h[256],chunk[4608];size_t n=strlen(body);int l=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n");send(c->fd,h,(size_t)l,0);l=snprintf(chunk,sizeof chunk,"%lx\r\n%s\r\n",(unsigned long)n,body);send(c->fd,chunk,(size_t)l,0);c->kind=3;c->used=0;c->keepalive=time(NULL);}
static void stream_heartbeat(client*c){send(c->fd,"2\r\n[]\r\n",7,0);c->keepalive=time(NULL);}
static void stream_event(client*c,const char*body){char chunk[4608];size_t n=strlen(body);int l=snprintf(chunk,sizeof chunk,"%lx\r\n%s\r\n",(unsigned long)n,body);send(c->fd,chunk,(size_t)l,0);c->keepalive=time(NULL);}
static void publish_player(void){char event[2048];sb_config*cfg=active_config;if(!comet_stream||!cfg||!player_id[0])return;snprintf(event,sizeof event,"[{\"channel\":\"/%s/slim/serverstatus\",\"data\":{\"httpport\":\"%u\",\"ip\":\"%s\",\"version\":\"%s\",\"uuid\":\"%s\",\"player count\":1,\"players_loop\":[{\"playerindex\":\"0\",\"playerid\":\"%s\",\"name\":\"Squeezebox Radio\",\"model\":\"baby\",\"modelname\":\"Squeezebox Radio\",\"isplayer\":1,\"connected\":1,\"power\":1,\"firmware\":\"7.7.3\",\"ip\":\"127.0.0.1\",\"seq_no\":0,\"displaytype\":\"none\",\"isplaying\":0,\"canpoweroff\":1}]}}]",comet_client_id,cfg->http_port,cfg->advertise_ip[0]?cfg->advertise_ip:"127.0.0.1",cfg->lms_version,cfg->uuid,player_id);fprintf(stderr,"COMET PUSH %s\n",event);stream_event(comet_stream,event);}
static void token(char out[9]){unsigned long x=(unsigned long)time(NULL)^(unsigned long)getpid();snprintf(out,9,"%08lx",x&0xfffffffful);}
static void json_unescape_slashes(char *s){char*r=s,*w=s;while(*r){if(r[0]=='\\'&&r[1]=='/')r++;*w++=*r++;}*w=0;}
static int json_string(const char *body,const char *key,char *out,size_t cap){const char*p=strstr(body,key);size_t n=0;if(!p||cap<2)return 0;p+=strlen(key);while(*p==' '||*p=='\t'||*p==':')p++;if(*p!='\"')return 0;for(p++;*p&&*p!='\"';p++){if(n+1<cap)out[n++]=*p;}if(*p!='\"')return 0;out[n]=0;return 1;}
static int json_raw(const char *body,const char *key,char *out,size_t cap){const char*p=strstr(body,key),*start;size_t n;if(!p||cap<2)return 0;p+=strlen(key);while(*p==' '||*p=='\t'||*p==':')p++;start=p;if(*p=='\"'){for(p++;*p&&*p!='\"';p++)if(*p=='\\'&&p[1])p++;if(*p=='\"')p++;}else while(*p&&*p!=','&&*p!='}'&&*p!=']'&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n')p++;n=(size_t)(p-start);if(!n||n>=cap)return 0;memcpy(out,start,n);out[n]=0;return 1;}
static int before(const char *start,const char *needle,const char *end){const char*p=strstr(start,needle);return p&&p<end;}
static void add_json(char*out,size_t cap,size_t*used,const char*json){size_t n=strlen(json);if(*used+n+2>=cap)return;if(*used>1)out[(*used)++]=',';memcpy(out+*used,json,n);*used+=n;out[*used]=0;}
static int load_catalog(const char*path){FILE*f;char*b,*p,*start;long size;size_t used=0;int count=0,depth,in_string,escape;if(!path||!(f=fopen(path,"rb")))return -1;if(fseek(f,0,SEEK_END)||((size=ftell(f))<2)||size>65536||fseek(f,0,SEEK_SET)){fclose(f);return -1;}b=malloc((size_t)size+1);if(!b){fclose(f);return -1;}if(fread(b,1,(size_t)size,f)!=(size_t)size){free(b);fclose(f);return -1;}fclose(f);b[size]=0;p=strstr(b,"\"applets\"");if(!p||(p=strchr(p,'['))==NULL){free(b);return -1;}strcpy(catalog_data,"{\"count\":0,\"item_loop\":[");used=strlen(catalog_data);for(p++;*p&&*p!=']';){while(*p&&*p!='{'&&*p!=']')p++;if(*p!='{')break;start=p;depth=0;in_string=escape=0;do{char ch=*p++;if(in_string){if(escape)escape=0;else if(ch=='\\')escape=1;else if(ch=='\"')in_string=0;}else if(ch=='\"')in_string=1;else if(ch=='{')depth++;else if(ch=='}')depth--;}while(*p&&depth);if(depth||used+(size_t)(p-start)+4>=sizeof catalog_data){free(b);return -1;}if(count)catalog_data[used++]=',';memcpy(catalog_data+used,start,(size_t)(p-start));used+=(size_t)(p-start);catalog_data[used]=0;count++;}if(used+3>=sizeof catalog_data){free(b);return -1;}catalog_data[used++]=']';catalog_data[used++]='}';catalog_data[used]=0;{char prefix[32];size_t old=strlen("{\"count\":0");snprintf(prefix,sizeof prefix,"{\"count\":%d",count);if(strlen(prefix)!=old){free(b);return -1;}memcpy(catalog_data,prefix,old);}free(b);return count;}
static void request_responses(const char*body,const sb_config*cfg,char*out,size_t cap){
    const char*p=body,*next;size_t used=1;char id[32],reply[160],msg[2300],data[1700];out[0]='[';out[1]=0;
    while((p=strstr(p,"\"id\""))!=NULL){next=strstr(p+4,"\"id\"");if(!next)next=p+strlen(p);id[0]=0;reply[0]=0;json_raw(p,"\"id\"",id,sizeof id);json_string(p,"\"response\"",reply,sizeof reply);if(!id[0]||!reply[0]){p+=4;continue;}
        if(before(p,"serverstatus",next))snprintf(data,sizeof data,"{\"httpport\":\"%u\",\"ip\":\"%s\",\"version\":\"%s\",\"uuid\":\"%s\",\"player count\":%d,\"players_loop\":%s}",cfg->http_port,cfg->advertise_ip,cfg->lms_version,cfg->uuid,player_id[0]?1:0,player_id[0]?"PLAYER":"[]");
        else if(before(p,"firmwareupgrade",next))strcpy(data,"{\"firmwareupgrade\":0,\"player_needs_upgrade\":0,\"player_is_upgrading\":0}");
        else if(before(p,"\"date\"",next))snprintf(data,sizeof data,"{\"date_epoch\":%lu,\"date\":\"0000-00-00T00:00:00+00:00\"}",(unsigned long)time(NULL));
        else if(before(p,"jiveapplets",next))snprintf(data,sizeof data,"%s",catalog_data);
        else if(before(p,"\"status\"",next))snprintf(data,sizeof data,"{\"player_name\":\"Standalone\",\"player_connected\":1,\"power\":1,\"mode\":\"stop\",\"time\":0,\"duration\":0,\"playlist_tracks\":0,\"playlist_cur_index\":0,\"mixer volume\":50,\"digital_volume_control\":1,\"player_needs_upgrade\":0,\"player_is_upgrading\":0,\"seq_no\":0}");
        else strcpy(data,"{\"count\":0,\"offset\":0,\"item_loop\":[]}");
        if(strstr(data,"PLAYER")){char player[900],*mark=strstr(data,"PLAYER");snprintf(player,sizeof player,"[{\"playerindex\":\"0\",\"playerid\":\"%s\",\"name\":\"Standalone\",\"model\":\"baby\",\"modelname\":\"Squeezebox Radio\",\"isplayer\":1,\"connected\":%d,\"power\":1,\"firmware\":\"7.7.3\",\"ip\":\"127.0.0.1\",\"seq_no\":0,\"displaytype\":\"none\",\"isplaying\":0,\"canpoweroff\":1}]",player_id,player_connected);memmove(mark+strlen(player),mark+6,strlen(mark+6)+1);memcpy(mark,player,strlen(player));}
        snprintf(msg,sizeof msg,"{\"id\":%s,\"channel\":\"/slim/request\",\"successful\":true}",id);add_json(out,cap,&used,msg);snprintf(msg,sizeof msg,"{\"id\":%s,\"channel\":\"%s\",\"data\":%s}",id,reply,data);add_json(out,cap,&used,msg);p=next;
    }if(used+2<cap){out[used++]=']';out[used]=0;}
}
static void handle_http(client *c,const sb_config *cfg){
    char *s=(char*)c->buf,*end=strstr(s,"\r\n\r\n"),*body,*cl,*channel,*id; long need=0; char response[4096],cid[9],reply[160]="/response",mac[32],rid[32]="1";
    if(!end)return;
    cl=strstr(s,"Content-Length:");if(!cl)cl=strstr(s,"content-length:");if(cl)need=strtol(cl+15,NULL,10);
    body=end+4;if(need<0||need>SB_MAX_HTTP_BODY){http_reply(c->fd,"413 Payload Too Large","[{\"successful\":false}]");close_client(c);return;}
    if((size_t)(body-s)+(size_t)need>c->used)return;
    if(strncmp(s,"POST /cometd ",13)&&strncmp(s,"GET /health ",12)){http_reply(c->fd,"404 Not Found","[{\"successful\":false,\"error\":\"not found\"}]");close_client(c);return;}
    if(!strncmp(s,"GET /health ",12)){http_reply(c->fd,"200 OK","{\"status\":\"ok\",\"service\":\"sbbase\",\"version\":\"" SBBASE_VERSION "\",\"lms_version\":\"" SB_LMS_COMPAT_VERSION "\"}");close_client(c);return;}
    json_unescape_slashes(body);channel=strstr(body,"\"channel\""); id=strstr(body,"\"id\""); (void)id;
    if(!channel){http_reply(c->fd,"400 Bad Request","[{\"successful\":false,\"error\":\"invalid Bayeux payload\"}]");close_client(c);return;}
    cid[0]=0;json_string(body,"\"response\"",reply,sizeof reply);json_string(body,"\"clientId\"",cid,sizeof cid);json_raw(body,"\"id\"",rid,sizeof rid);
    if(strstr(channel,"/meta/handshake")){token(cid);if(json_string(body,"\"mac\"",mac,sizeof mac)&&strlen(mac)==17)strcpy(player_id,mac);snprintf(response,sizeof response,"[{\"id\":\"1\",\"channel\":\"/meta/handshake\",\"version\":\"1.0\",\"supportedConnectionTypes\":[\"long-polling\",\"streaming\"],\"clientId\":\"%s\",\"successful\":true,\"advice\":{\"reconnect\":\"retry\",\"interval\":0,\"timeout\":60000}}]",cid);}
    else if(strstr(channel,"/meta/connect")){strcpy(comet_client_id,cid);comet_stream=c;snprintf(response,sizeof response,"[{\"channel\":\"/meta/connect\",\"clientId\":\"%s\",\"successful\":true,\"advice\":{\"interval\":5000}},{\"channel\":\"/meta/subscribe\",\"clientId\":\"%s\",\"successful\":true}]",cid,cid);fprintf(stderr,"COMET RX %.*s\nCOMET STREAM %s\n",(int)need,body,response);stream_start(c,response);return;}
    else if(strstr(channel,"/meta/reconnect"))snprintf(response,sizeof response,"[{\"id\":\"1\",\"channel\":\"/meta/reconnect\",\"successful\":true,\"advice\":{\"interval\":5000}}]");
    else if(strstr(channel,"/meta/disconnect"))snprintf(response,sizeof response,"[{\"id\":\"1\",\"channel\":\"/meta/disconnect\",\"successful\":true}]");
    else request_responses(body,cfg,response,sizeof response);
    fprintf(stderr,"COMET RX %.*s\nCOMET TX %s\n",(int)need,body,response);
    http_reply(c->fd,"200 OK",response);c->used=0;
}
static void keepalive(client *c){unsigned char payload[24]={ 't','0','m','?','?','?','?',0,0,0,'0',0,0,0,0,0,0,0,0,0,0,0,0,0 },frame[40];size_t z=sb_server_frame("strm",payload,sizeof payload,frame,sizeof frame);if(z)send(c->fd,(char*)frame,z,0);c->keepalive=time(NULL);}
static void handle_slim(client *c){size_t off=0,used;char op[5];const unsigned char*p;uint32_t n;fprintf(stderr,"SLIM RX fd=%d bytes=%lu\n",c->fd,(unsigned long)c->used);while(off<c->used){int r=sb_parse_client_frame(c->buf+off,c->used-off,op,&p,&n,&used);if(r<0){fprintf(stderr,"SLIM INVALID fd=%d\n",c->fd);close_client(c);return;}if(!r){fprintf(stderr,"SLIM PARTIAL fd=%d remaining=%lu\n",c->fd,(unsigned long)(c->used-off));break;}fprintf(stderr,"SLIM FRAME fd=%d op=%s len=%lu\n",c->fd,op,(unsigned long)n);if(!strcmp(op,"HELO")){if(n>=8){snprintf(player_id,sizeof player_id,"%02x:%02x:%02x:%02x:%02x:%02x",p[2],p[3],p[4],p[5],p[6],p[7]);player_connected=1;}c->helo=1;keepalive(c);publish_player();}off+=used;}if(off){memmove(c->buf,c->buf+off,c->used-off);c->used-=off;}}
static void defaults(sb_config*c){memset(c,0,sizeof *c);strcpy(c->name,"StandaloneBase");strcpy(c->uuid,"9d989f40-499a-4b85-b92f-8dc415af2a04");strcpy(c->lms_version,SB_LMS_COMPAT_VERSION);local_ip(c->advertise_ip);c->http_port=9000;c->slim_port=c->discovery_port=3483;c->time_sync=1;}
int main(int argc,char**argv){sb_config cfg;int udp,tcp,http,i;client cs[CLIENTS];struct pollfd pf[3+CLIENTS];defaults(&cfg);active_config=&cfg;
    if(argc==2&&!strcmp(argv[1],"--version")){printf("sbbase %s (LMS compatibility %s)\n",SBBASE_VERSION,cfg.lms_version);return 0;}
    if(argc==2&&!strcmp(argv[1],"--self-test")){unsigned char q[]={'e','N','A','M','E',0},o[96];return sb_discovery_response(q,sizeof q,o,sizeof o,&cfg,"127.0.0.1")?0:2;}
    if(argc==3&&!strcmp(argv[1],"--check-config")){if(load_catalog(argv[2])<0)return 2;puts(catalog_data);return 0;}
    if(argc==3&&!strcmp(argv[1],"--config")){snprintf(cfg.catalog_path,sizeof cfg.catalog_path,"%s",argv[2]);if(load_catalog(argv[2])<0){fprintf(stderr,"invalid or unreadable config: %s\n",argv[2]);return 2;}}
    signal(SIGINT,stop);signal(SIGTERM,stop);
    udp=listener(SOCK_DGRAM,cfg.discovery_port);tcp=listener(SOCK_STREAM,cfg.slim_port);http=listener(SOCK_STREAM,cfg.http_port);if(udp<0||tcp<0||http<0){perror("listener");return 1;}for(i=0;i<CLIENTS;i++)cs[i].fd=-1;fprintf(stderr,"sbbase %s (LMS %s): %s UDP/TCP %u, HTTP %u\n",SBBASE_VERSION,cfg.lms_version,cfg.advertise_ip,cfg.slim_port,cfg.http_port);
    while(running){int count=3;pf[0]=(struct pollfd){udp,POLLIN,0};pf[1]=(struct pollfd){tcp,POLLIN,0};pf[2]=(struct pollfd){http,POLLIN,0};for(i=0;i<CLIENTS;i++)if(cs[i].fd>=0)pf[count++]=(struct pollfd){cs[i].fd,POLLIN,0};if(poll(pf,(nfds_t)count,1000)<0){if(errno==EINTR)continue;break;}
        if(pf[0].revents&POLLIN){unsigned char in[SB_MAX_DISCOVERY+1],out[1024];struct sockaddr_in peer;socklen_t pl=sizeof peer;ssize_t n=recvfrom(udp,in,sizeof in,0,(struct sockaddr*)&peer,&pl);if(n>0){size_t z=sb_discovery_response(in,(size_t)n,out,sizeof out,&cfg,cfg.advertise_ip);if(z)sendto(udp,out,z,0,(struct sockaddr*)&peer,pl);}}
        for(int li=1;li<=2;li++)if(pf[li].revents&POLLIN){int fd=accept(pf[li].fd,NULL,NULL);if(fd>=0){nonblock(fd);for(i=0;i<CLIENTS&&cs[i].fd>=0;i++);if(i==CLIENTS){fprintf(stderr,"ACCEPT REJECT kind=%d fd=%d full\n",li,fd);close(fd);}else{fprintf(stderr,"ACCEPT kind=%d fd=%d slot=%d\n",li,fd,i);cs[i].fd=fd;cs[i].kind=-li;cs[i].used=0;cs[i].helo=0;cs[i].keepalive=0;}}}
        count=3;for(i=0;i<CLIENTS;i++)if(cs[i].fd>=0){short rev;if(cs[i].kind<0){cs[i].kind=-cs[i].kind;continue;}rev=pf[count++].revents;if(rev&(POLLERR|POLLHUP|POLLNVAL)){close_client(&cs[i]);continue;}if(rev&POLLIN){ssize_t n=recv(cs[i].fd,cs[i].buf+cs[i].used,sizeof(cs[i].buf)-1-cs[i].used,0);if(n<=0){close_client(&cs[i]);continue;}cs[i].used+=(size_t)n;cs[i].buf[cs[i].used]=0;if(cs[i].kind==1)handle_slim(&cs[i]);else if(cs[i].kind==2)handle_http(&cs[i],&cfg);}if(cs[i].fd>=0&&cs[i].kind==1&&cs[i].helo&&time(NULL)-cs[i].keepalive>=4)keepalive(&cs[i]);if(cs[i].fd>=0&&cs[i].kind==3&&time(NULL)-cs[i].keepalive>=20)stream_heartbeat(&cs[i]);}
    }for(i=0;i<CLIENTS;i++)close_client(&cs[i]);close(udp);close(tcp);close(http);return 0;}
