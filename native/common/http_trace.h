#ifndef SB_HTTP_TRACE_H
#define SB_HTTP_TRACE_H
/* Fixed-size request metadata. No body copies, secrets or per-chunk logging. */
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
typedef struct {
    char id[80];
    unsigned long long started;
} sb_http_trace;
static inline unsigned long long sb_trace_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (unsigned long long)ts.tv_sec*1000+(unsigned)ts.tv_nsec/1000000;
}
static inline void sb_trace_clean(char *out,size_t cap,const char *text,int url) {
    size_t n=0;
    if (!text || !*text) text="-";
    const char *start=strstr(text,"://");
    const char *authority=start ? start+3 : text;
    const char *end=authority+strcspn(authority,"/?#");
    const char *at=memchr(authority,'@',(size_t)(end-authority));
    for (const char *p=text;*p && n+1<cap;p++) {
        if (url && p==authority && at) {
            const char *redacted="[redacted]@";
            while (*redacted && n+1<cap) out[n++]=*redacted++;
            p=at; continue;
        }
        if (url && (*p=='?' || *p=='#')) {
            const char *redacted="?[redacted]";
            while (*redacted && n+1<cap) out[n++]=*redacted++;
            break;
        }
        unsigned char c=(unsigned char)*p;
        out[n++]=(c<32 || c==127 || c=='"' || c=='\\') ? '_' : (char)c;
    }
    out[n]=0;
}
static inline void sb_trace_log(const char *format,...) {
    char line[1536];va_list args;
    va_start(args,format);vsnprintf(line,sizeof line,format,args);va_end(args);
    syslog(LOG_INFO,"HTTPTRACE %s",line);
    fprintf(stderr,"HTTPTRACE %s\n",line);
}
static inline void sb_trace_begin(sb_http_trace *t,const char *source,const char *method,const char *url,const char *parent) {
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    static unsigned long seq;
    pthread_mutex_lock(&lock);unsigned long next=++seq;pthread_mutex_unlock(&lock);
    t->started=sb_trace_ms();
    snprintf(t->id,sizeof t->id,"%ld-%s-%lu",(long)getpid(),source,next);
    char safe[513],link[81];sb_trace_clean(safe,sizeof safe,url,1);sb_trace_clean(link,sizeof link,parent,0);
    sb_trace_log("event=request id=%s parent=%s source=%s method=%s url=\"%s\"",t->id,link,source,method,safe);
}
static inline void sb_trace_header(sb_http_trace *t,int status,const char *data,size_t length) {
    if (status) sb_trace_log("event=response id=%s status=%d duration_ms=%llu",t->id,status,sb_trace_ms()-t->started);
    const char *name=NULL;int is_url=0;
    if (length>13 && !strncasecmp(data,"Content-Type:",13)) name="type";
    else if (length>15 && !strncasecmp(data,"Content-Length:",15)) name="length";
    else if (length>9 && !strncasecmp(data,"Location:",9)) { name="location";is_url=1; }
    if (name) {
        const char *value=(const char *)memchr(data,':',length)+1;
        while ((size_t)(value-data)<length && (*value==' ' || *value=='\t')) value++;
        size_t n=length-(size_t)(value-data);char raw[513],safe[513];
        if (n>=sizeof raw) n=sizeof raw-1;
        while (n && (value[n-1]=='\r' || value[n-1]=='\n')) n--;
        memcpy(raw,value,n);raw[n]=0;sb_trace_clean(safe,sizeof safe,raw,is_url);
        sb_trace_log("event=header id=%s %s=\"%s\"",t->id,name,safe);
    }
}
static inline void sb_trace_end(sb_http_trace *t,int status,unsigned long long bytes,const char *err) {
    char safe[257];sb_trace_clean(safe,sizeof safe,err,1);
    sb_trace_log("event=complete id=%s status=%d bytes=%llu duration_ms=%llu error=\"%s\"",t->id,status,bytes,sb_trace_ms()-t->started,safe);
}
static inline void sb_trace_parent(const char *request,char *out,size_t size) {
    out[0]=0;
    for (const char *p=request; p && *p; p=strchr(p,'\n')) {
        if (*p=='\n') p++;
        if (!strncasecmp(p,"X-SB-Trace-Id:",14)) {
            p+=14;while (*p==' ' || *p=='\t') p++;
            size_t n=0;
            while (*p && n+1<size && (isalnum((unsigned char)*p) || *p=='-' || *p=='_')) out[n++]=*p++;
            out[n]=0;return;
        }
    }
}
#endif
