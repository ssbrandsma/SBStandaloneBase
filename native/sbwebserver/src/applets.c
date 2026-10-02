#include "applets.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>

#define MAX_STATIONS 128
#define FIELD 512
typedef struct { char name[FIELD], url[FIELD], logo[FIELD]; } Station;
static const char *base(void) { const char *e=getenv("SBWEBSERVER_CONFIG_DIR");if(e&&*e)return e;if(access("/mnt/storage", W_OK) == 0)return "/mnt/storage/sbwebserver";return "/tmp/sbwebserver"; }
static void file_path(char *p,size_t n,const char *f){snprintf(p,n,"%s/%s",base(),f);}
static void ensure_dir(void){char p[128];snprintf(p,sizeof p,"%s",base());
#ifdef _WIN32
_mkdir(p);
#else
mkdir(p,0755);
#endif
}
static int csv_field(const char **src,char *dst,size_t cap){size_t n=0;const char*p=*src;int quoted=(*p=='"');if(quoted)p++;while(*p){if(quoted&&*p=='"'){if(p[1]=='"'){if(n+1<cap)dst[n++]='"';p+=2;continue;}p++;while(*p==' '||*p=='\t')p++;if(*p==',')p++;else if(*p=='\r'||*p=='\n'||!*p){}else return -1;dst[n]=0;*src=p;return 0;}if(!quoted&&(*p==','||*p=='\r'||*p=='\n')){if(*p==',')p++;dst[n]=0;*src=p;return 0;}if(n+1>=cap)return -1;dst[n++]=*p++;}dst[n]=0;*src=p;return quoted?-1:0;}
static int parse_csv(const char*data,Station*out,int*count){const char*p=data;int c=0;char h[FIELD];if(csv_field(&p,h,sizeof h)||strcmp(h,"name"))return -1;if(csv_field(&p,h,sizeof h)||strcmp(h,"stream_url"))return -1;if(csv_field(&p,h,sizeof h)||strcmp(h,"logo_url"))return -1;while(*p=='\r'||*p=='\n')p++;while(*p){Station s={{0},{0},{0}};if(c>=MAX_STATIONS)return -1;if(csv_field(&p,s.name,sizeof s.name)||csv_field(&p,s.url,sizeof s.url)||csv_field(&p,s.logo,sizeof s.logo))return -1;while(*p=='\r'||*p=='\n')p++;if(!s.name[0]||!s.url[0])return -1;out[c++]=s;}*count=c;return 0;}
static int write_stations(Station*a,int count){char path[160],tmp[180];ensure_dir();file_path(path,sizeof path,"stations.csv");snprintf(tmp,sizeof tmp,"%s.tmp",path);FILE*f=fopen(tmp,"w");if(!f)return -1;fputs("name,stream_url,logo_url\n",f);for(int i=0;i<count;i++){const char*vals[]={a[i].name,a[i].url,a[i].logo};for(int j=0;j<3;j++){if(j)fputc(',',f);fputc('"',f);for(const char*q=vals[j];*q;q++){if(*q=='"')fputc('"',f);fputc(*q,f);}fputc('"',f);}fputc('\n',f);}fflush(f);
#ifndef _WIN32
fsync(fileno(f));
#endif
fclose(f);return rename(tmp,path);}
void applets_json(char*out,size_t n){int a=access("/usr/share/jive/applets/StandaloneRadio",F_OK)==0||access("/usr/share/jive/applets/SBStandalone",F_OK)==0;snprintf(out,n,"[{\"id\":\"standalone-radio\",\"name\":\"Standalone Radio\",\"installed\":%s,\"supported\":true},{\"id\":\"https-proxy\",\"name\":\"HTTPS Proxy\",\"installed\":%s,\"supported\":false},{\"id\":\"spotify-connect\",\"name\":\"Spotify Connect\",\"installed\":%s,\"supported\":false}]",a?"true":"false",access("/usr/share/jive/applets/HTTPSProxy",F_OK)==0?"true":"false",access("/usr/share/jive/applets/SpotifyConnect",F_OK)==0?"true":"false");}
void stations_json(char*out,size_t n){char p[160],line[2048];file_path(p,sizeof p,"stations.csv");FILE*f=fopen(p,"r");Station items[MAX_STATIONS];int count=0;if(f){fgets(line,sizeof line,f);while(fgets(line,sizeof line,f)&&count<MAX_STATIONS){Station s;const char*q=line;if(csv_field(&q,s.name,sizeof s.name)||csv_field(&q,s.url,sizeof s.url)||csv_field(&q,s.logo,sizeof s.logo))continue;items[count++]=s;}fclose(f);}size_t u=snprintf(out,n,"[");for(int i=0;i<count&&u<n;i++)u+=snprintf(out+u,n-u,"%s{\"name\":\"%s\",\"stream_url\":\"%s\",\"logo_url\":\"%s\"}",i?",":"",items[i].name,items[i].url,items[i].logo);snprintf(out+(u<n?u:n-1),u<n?n-u:1,"]");}
int stations_put(const char*body){Station a[MAX_STATIONS];int count=0;if(!body||parse_csv(body,a,&count))return -1;return write_stations(a,count);}
int stations_csv(char*out,size_t n){char p[160];file_path(p,sizeof p,"stations.csv");FILE*f=fopen(p,"r");if(!f){snprintf(out,n,"name,stream_url,logo_url\n");return 0;}size_t u=0;int ch;while((ch=fgetc(f))!=EOF&&u+1<n)out[u++]=(char)ch;out[u]=0;fclose(f);return 0;}
int stations_import(const char*body,size_t len){if(!body||!len||len>256*1024)return -1;char*copy=malloc(len+1);if(!copy)return -1;memcpy(copy,body,len);copy[len]=0;Station a[MAX_STATIONS];int count=0;int rc=parse_csv(copy,a,&count);free(copy);return rc?-1:write_stations(a,count);}
