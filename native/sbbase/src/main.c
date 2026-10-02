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
typedef struct { int fd, kind; size_t used; unsigned char buf[SB_MAX_HTTP_HEADER + SB_MAX_HTTP_BODY + 1]; } client;
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
static void close_client(client *c){if(c->fd>=0)close(c->fd);c->fd=-1;c->used=0;}
static void http_reply(int fd,const char *status,const char *body){char h[256];size_t n=strlen(body);int l=snprintf(h,sizeof h,"HTTP/1.1 %s\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",status,(unsigned long)n);send(fd,h,(size_t)l,0);send(fd,body,n,0);}
static void token(char out[9]){unsigned long x=(unsigned long)time(NULL)^(unsigned long)getpid();snprintf(out,9,"%08lx",x&0xfffffffful);}
static void handle_http(client *c,const sb_config *cfg){
    char *s=(char*)c->buf,*end=strstr(s,"\r\n\r\n"),*body,*cl,*channel,*id; long need=0; char response[4096],cid[9];
    if(!end)return;
    cl=strstr(s,"Content-Length:");if(!cl)cl=strstr(s,"content-length:");if(cl)need=strtol(cl+15,NULL,10);
    body=end+4;if(need<0||need>SB_MAX_HTTP_BODY){http_reply(c->fd,"413 Payload Too Large","[{\"successful\":false}]");close_client(c);return;}
    if((size_t)(body-s)+(size_t)need>c->used)return;
    if(strncmp(s,"POST /cometd ",13)&&strncmp(s,"GET /health ",12)){http_reply(c->fd,"404 Not Found","[{\"successful\":false,\"error\":\"not found\"}]");close_client(c);return;}
    if(!strncmp(s,"GET /health ",12)){http_reply(c->fd,"200 OK","{\"status\":\"ok\",\"service\":\"sbbase\"}");close_client(c);return;}
    channel=strstr(body,"\"channel\""); id=strstr(body,"\"id\""); (void)id;
    if(!channel){http_reply(c->fd,"400 Bad Request","[{\"successful\":false,\"error\":\"invalid Bayeux payload\"}]");close_client(c);return;}
    if(strstr(channel,"/meta/handshake")){token(cid);snprintf(response,sizeof response,"[{\"id\":\"1\",\"channel\":\"/meta/handshake\",\"version\":\"1.0\",\"supportedConnectionTypes\":[\"long-polling\",\"streaming\"],\"clientId\":\"%s\",\"successful\":true,\"advice\":{\"reconnect\":\"retry\",\"interval\":0,\"timeout\":60000}}]",cid);}
    else if(strstr(channel,"/meta/connect")||strstr(channel,"/meta/reconnect"))snprintf(response,sizeof response,"[{\"id\":\"1\",\"channel\":\"/meta/connect\",\"successful\":true,\"advice\":{\"interval\":0}}]");
    else if(strstr(channel,"/meta/disconnect"))snprintf(response,sizeof response,"[{\"id\":\"1\",\"channel\":\"/meta/disconnect\",\"successful\":true}]");
    else if(strstr(body,"jiveapplets"))snprintf(response,sizeof response,"[{\"channel\":\"/slim/request\",\"successful\":true},{\"channel\":\"/response\",\"data\":{\"count\":0,\"item_loop\":[]}}]");
    else if(strstr(body,"serverstatus"))snprintf(response,sizeof response,"[{\"channel\":\"/slim/request\",\"successful\":true},{\"channel\":\"/response\",\"data\":{\"httpport\":\"%u\",\"ip\":\"%s\",\"version\":\"%s\",\"uuid\":\"%s\",\"player count\":0,\"players_loop\":[]}}]",cfg->http_port,cfg->advertise_ip[0]?cfg->advertise_ip:"127.0.0.1",cfg->version,cfg->uuid);
    else if(strstr(body,"date"))snprintf(response,sizeof response,"[{\"channel\":\"/slim/request\",\"successful\":true},{\"channel\":\"/response\",\"data\":{\"date_epoch\":%lu,\"date\":\"0000-00-00T00:00:00+00:00\"}}]",(unsigned long)time(NULL));
    else snprintf(response,sizeof response,"[{\"channel\":\"/slim/request\",\"successful\":true},{\"channel\":\"/response\",\"data\":{\"count\":0,\"offset\":0,\"item_loop\":[]}}]");
    http_reply(c->fd,"200 OK",response);close_client(c);
}
static void handle_slim(client *c){size_t off=0,used;char op[5];const unsigned char*p;uint32_t n;while(off<c->used){int r=sb_parse_client_frame(c->buf+off,c->used-off,op,&p,&n,&used);if(r<0){close_client(c);return;}if(!r)break;if(!strcmp(op,"HELO")){unsigned char payload[32]={ 't','0','m','?','?','?','?',0,0,0,'0',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };unsigned char frame[40];size_t z=sb_server_frame("strm",payload,24,frame,sizeof frame);send(c->fd,(char*)frame,z,0);}off+=used;}if(off){memmove(c->buf,c->buf+off,c->used-off);c->used-=off;}}
static void defaults(sb_config*c){memset(c,0,sizeof *c);strcpy(c->name,"StandaloneBase");strcpy(c->uuid,"9d989f40-499a-4b85-b92f-8dc415af2a04");strcpy(c->version,"0.1.0");strcpy(c->advertise_ip,"127.0.0.1");c->http_port=9000;c->slim_port=c->discovery_port=3483;c->time_sync=1;}
int main(int argc,char**argv){sb_config cfg;int udp,tcp,http,i;client cs[CLIENTS];struct pollfd pf[3+CLIENTS];(void)argc;(void)argv;defaults(&cfg);signal(SIGINT,stop);signal(SIGTERM,stop);
    udp=listener(SOCK_DGRAM,cfg.discovery_port);tcp=listener(SOCK_STREAM,cfg.slim_port);http=listener(SOCK_STREAM,cfg.http_port);if(udp<0||tcp<0||http<0){perror("listener");return 1;}for(i=0;i<CLIENTS;i++)cs[i].fd=-1;fprintf(stderr,"sbbase %s: UDP/TCP %u, HTTP %u\n",cfg.version,cfg.slim_port,cfg.http_port);
    while(running){int count=3;pf[0]=(struct pollfd){udp,POLLIN,0};pf[1]=(struct pollfd){tcp,POLLIN,0};pf[2]=(struct pollfd){http,POLLIN,0};for(i=0;i<CLIENTS;i++)if(cs[i].fd>=0)pf[count++]=(struct pollfd){cs[i].fd,POLLIN,0};if(poll(pf,(nfds_t)count,1000)<0){if(errno==EINTR)continue;break;}
        if(pf[0].revents&POLLIN){unsigned char in[SB_MAX_DISCOVERY+1],out[1024];struct sockaddr_in peer;socklen_t pl=sizeof peer;ssize_t n=recvfrom(udp,in,sizeof in,0,(struct sockaddr*)&peer,&pl);if(n>0){size_t z=sb_discovery_response(in,(size_t)n,out,sizeof out,&cfg,cfg.advertise_ip);if(z)sendto(udp,out,z,0,(struct sockaddr*)&peer,pl);}}
        for(int li=1;li<=2;li++)if(pf[li].revents&POLLIN){int fd=accept(pf[li].fd,NULL,NULL);if(fd>=0){nonblock(fd);for(i=0;i<CLIENTS&&cs[i].fd>=0;i++);if(i==CLIENTS)close(fd);else{cs[i].fd=fd;cs[i].kind=li;cs[i].used=0;}}}
        count=3;for(i=0;i<CLIENTS;i++)if(cs[i].fd>=0){short rev=pf[count++].revents;if(rev&(POLLERR|POLLHUP|POLLNVAL)){close_client(&cs[i]);continue;}if(rev&POLLIN){ssize_t n=recv(cs[i].fd,cs[i].buf+cs[i].used,sizeof(cs[i].buf)-1-cs[i].used,0);if(n<=0){close_client(&cs[i]);continue;}cs[i].used+=(size_t)n;cs[i].buf[cs[i].used]=0;if(cs[i].kind==1)handle_slim(&cs[i]);else handle_http(&cs[i],&cfg);}}
    }for(i=0;i<CLIENTS;i++)close_client(&cs[i]);close(udp);close(tcp);close(http);return 0;}
