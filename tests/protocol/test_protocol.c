#include "sbbase.h"
#include <assert.h>
#include <string.h>
int main(void){
 sb_config c; unsigned char out[256],req[]={ 'e','N','A','M','E',0,'J','S','O','N',0 }; size_t n;
 memset(&c,0,sizeof c);strcpy(c.name,"StandaloneBase");strcpy(c.lms_version,SB_LMS_COMPAT_VERSION);c.http_port=9000;
 n=sb_discovery_response(req,sizeof req,out,sizeof out,&c,"127.0.0.1");
 assert(n==1+5+14+5+4);assert(out[0]=='E');assert(!memcmp(out+1,"NAME",4));
 {unsigned char vreq[]={ 'e','V','E','R','S',0 };n=sb_discovery_response(vreq,sizeof vreq,out,sizeof out,&c,"");assert(n==1+5+9);assert(!memcmp(out+6,"7.999.999",9));}
 {unsigned char f[]={ 'H','E','L','O',0,0,0,2,1,2 };char op[5];const unsigned char*p;uint32_t l;size_t u;
 assert(sb_parse_client_frame(f,sizeof f,op,&p,&l,&u)==1);assert(!strcmp(op,"HELO")&&l==2&&u==10);}
 {const char *json="{\"version\":\"1\",\"applets\":[]}";assert(sb_catalog_valid(json,strlen(json)));}
 return 0;
}
