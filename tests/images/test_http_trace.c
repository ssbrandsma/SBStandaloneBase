#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static char records[16384];
static void capture_syslog(int priority,const char *format,...) {
    (void)priority;size_t n=strlen(records);va_list args;
    va_start(args,format);vsnprintf(records+n,sizeof records-n,format,args);va_end(args);
}
#define syslog capture_syslog
#include "../../native/common/http_trace.h"
int main(void) {
    sb_http_trace trace,second;
    sb_trace_begin(&trace,"test","GET","https://user:SECRET@example.com/logo?token=TOKEN","jive-1");
    sb_trace_header(&trace,302,"",0);
    const char location[]="Location: https://example.com/new?secret=TOKEN\r\n";
    sb_trace_header(&trace,0,location,sizeof location-1);
    sb_trace_end(&trace,302,123,"cancelled\nunsafe");
    sb_trace_begin(&second,"test","GET","https://example.com/logo",NULL);
    assert(strcmp(trace.id,second.id));
    assert(!strstr(records,"SECRET") && !strstr(records,"TOKEN"));
    assert(strstr(records,"event=response") && strstr(records,"status=302"));
    assert(strstr(records,"bytes=123") && strstr(records,"cancelled_unsafe"));
    char parent[80];sb_trace_parent("GET / HTTP/1.1\r\nx-sb-trace-id: jive-123-7\r\n\r\n",parent,sizeof parent);
    assert(!strcmp(parent,"jive-123-7"));
    puts("Native HTTP trace metadata, IDs, redaction and parent header tests passed");
    return 0;
}
