#ifndef SB_HTTP_TRACE_CURL_H
#define SB_HTTP_TRACE_CURL_H
#include "../common/http_trace.h"
/* CURLOPT_VERBOSE is consumed here; curl's headers, credentials and payload
 * dumps never reach stderr. Log only actual request hops with a redacted URL. */
static int sb_trace_curl_debug(CURL *curl,curl_infotype kind,char *data,size_t size,void *opaque) {
    (void)data;(void)size;
    if (kind==CURLINFO_HEADER_OUT) {
        sb_http_trace *t=opaque;char *url=NULL;char safe[513];
        curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&url);
        sb_trace_clean(safe,sizeof safe,url,1);
        sb_trace_log("event=send id=%s url=\"%s\"",t->id,safe);
    }
    return 0;
}
static void sb_trace_curl(CURL *curl,sb_http_trace *t) {
    curl_easy_setopt(curl,CURLOPT_VERBOSE,1L);
    curl_easy_setopt(curl,CURLOPT_DEBUGFUNCTION,sb_trace_curl_debug);
    curl_easy_setopt(curl,CURLOPT_DEBUGDATA,t);
}
#endif
