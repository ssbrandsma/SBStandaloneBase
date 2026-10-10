#define _POSIX_C_SOURCE 200809L
#include "nowplaying.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JTOKENS 160
typedef enum { J_UNDEF, J_OBJECT, J_ARRAY, J_STRING, J_PRIMITIVE } jtype;
typedef struct { jtype type; int start, end, size, parent; } jtok;
typedef struct { unsigned pos, next; int super; } jparser;

struct np_manager {
  np_status s;
  unsigned lease_seconds, command_limit, q_head, q_count;
  uint64_t generation_counter, command_counter;
  np_command queue[NP_COMMANDS_MAX];
};

uint64_t np_monotonic_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void reset_status(np_manager *m) {
  uint64_t generation = m->s.generation;
  memset(&m->s, 0, sizeof m->s);
  m->s.generation = generation;
  m->s.state = NP_STOPPED;
  m->q_head = m->q_count = 0;
}

np_manager *np_create(unsigned lease_seconds, unsigned command_limit) {
  np_manager *m = calloc(1, sizeof *m);
  if (!m) return NULL;
  m->lease_seconds = lease_seconds ? lease_seconds : 120;
  m->command_limit = command_limit && command_limit <= NP_COMMANDS_MAX ? command_limit : NP_COMMANDS_MAX;
  return m;
}
void np_destroy(np_manager *m) { free(m); }
const np_status *np_current(np_manager *m) { return m ? &m->s : NULL; }

uint64_t np_position(np_manager *m) {
  uint64_t p;
  if (!m) return 0;
  p = m->s.position_ms;
  if (m->s.state == NP_PLAYING && m->s.position_clock_ms)
    p += np_monotonic_ms() - m->s.position_clock_ms;
  if (m->s.duration_known && p > m->s.duration_ms) p = m->s.duration_ms;
  return p;
}

int np_expire(np_manager *m) {
  if (!m || !m->s.active || !m->lease_seconds) return 0;
  if (np_monotonic_ms() - m->s.last_update_ms <= (uint64_t)m->lease_seconds * 1000u) return 0;
  reset_status(m);
  return 1;
}

static int alloc_token(char out[NP_TOKEN_MAX]) {
  unsigned char b[24]; size_t i; int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) { if (fd >= 0) close(fd); return 0; }
  close(fd);
  for (i = 0; i < sizeof b; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
  return 1;
}

static int parse_json(const char *js, size_t len, jtok *t, unsigned cap) {
  jparser p = {0,0,-1}; unsigned i;
  for (; p.pos < len; p.pos++) {
    char c = js[p.pos]; int n;
    if (isspace((unsigned char)c) || c == ':' || c == ',') continue;
    if (c == '{' || c == '[') {
      if (p.next >= cap) return -1;
      n=(int)p.next++; t[n]=(jtok){c=='{'?J_OBJECT:J_ARRAY,(int)p.pos,-1,0,p.super};
      if (p.super >= 0) t[p.super].size++;
      p.super=n; continue;
    }
    if (c == '}' || c == ']') {
      jtype type=c=='}'?J_OBJECT:J_ARRAY;
      for (n=(int)p.next-1;n>=0;n--) if(t[n].start>=0&&t[n].end<0){if(t[n].type!=type)return -1;t[n].end=(int)p.pos+1;p.super=t[n].parent;break;}
      if(n<0)return -1;
      continue;
    }
    if (c == '"') {
      unsigned start=++p.pos;
      for (;p.pos<len;p.pos++) { c=js[p.pos]; if(c=='"')break; if(c=='\\'){p.pos++;if(p.pos>=len)return -1;} if((unsigned char)c<32)return -1; }
      if(p.pos>=len||p.next>=cap)return -1;
      n=(int)p.next++;t[n]=(jtok){J_STRING,(int)start,(int)p.pos,0,p.super};
      if(p.super>=0)t[p.super].size++;
      continue;
    }
    { unsigned start=p.pos;
      while(p.pos<len&&!isspace((unsigned char)js[p.pos])&&strchr(",}]",js[p.pos])==NULL)p.pos++;
      if(start==p.pos||p.next>=cap)return -1;
      n=(int)p.next++;t[n]=(jtok){J_PRIMITIVE,(int)start,(int)p.pos,0,p.super};
      if(p.super>=0)t[p.super].size++;
      p.pos--;
    }
  }
  for(i=0;i<p.next;i++)if(t[i].end<0)return -1;
  return p.next && t[0].type==J_OBJECT ? (int)p.next : -1;
}

static int eq(const char *j, const jtok *t, const char *s) { size_t n=strlen(s);return t->type==J_STRING&&(size_t)(t->end-t->start)==n&&!memcmp(j+t->start,s,n); }
static int subtree(jtok *t,int n,int i){int end=t[i].end;i++;while(i<n&&t[i].start<end)i++;return i;}
static int member(const char *j,jtok *t,int n,int obj,const char *key){int i;if(obj<0||t[obj].type!=J_OBJECT)return -1;for(i=obj+1;i<n&&t[i].start<t[obj].end;){int v=i+1;if(eq(j,&t[i],key)&&v<n)return v;if(v>=n)return -1;i=subtree(t,n,v);}return -1;}

static int hex4(const char *s) { int v=0,i;for(i=0;i<4;i++){int c=(unsigned char)s[i];if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else if(c>='A'&&c<='F')c=c-'A'+10;else return -1;v=v*16+c;}return v; }
static size_t put_utf8(char *o,size_t cap,size_t n,unsigned v){if(v<0x80){if(n+1<cap)o[n++]=(char)v;}else if(v<0x800){if(n+2<cap){o[n++]=(char)(0xc0|(v>>6));o[n++]=(char)(0x80|(v&63));}}else if(v<0xd800||v>0xdfff){if(n+3<cap){o[n++]=(char)(0xe0|(v>>12));o[n++]=(char)(0x80|((v>>6)&63));o[n++]=(char)(0x80|(v&63));}}return n;}
static int string_value(const char *j,const jtok *t,char *o,size_t cap){int i;size_t n=0;if(!t||t->type!=J_STRING||!cap)return 0;for(i=t->start;i<t->end;i++){unsigned char c=(unsigned char)j[i];if(c=='\\'){if(++i>=t->end)return 0;c=(unsigned char)j[i];if(c=='u'){int v;if(i+4>=t->end||(v=hex4(j+i+1))<0)return 0;n=put_utf8(o,cap,n,(unsigned)v);i+=4;continue;}if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c!='"'&&c!='\\'&&c!='/')return 0;}if(n+1<cap)o[n++]=(char)c;}o[n]=0;return 1;}
static int boolean(const char*j,const jtok*t,int*d){int n=t->end-t->start;if(t->type!=J_PRIMITIVE)return 0;if(n==4&&!memcmp(j+t->start,"true",4)){*d=1;return 1;}if(n==5&&!memcmp(j+t->start,"false",5)){*d=0;return 1;}return 0;}
static int number(const char*j,const jtok*t,uint64_t*d){char b[32],*e;unsigned long long v;int n=t->end-t->start;if(t->type!=J_PRIMITIVE||n<=0||n>=(int)sizeof b)return 0;memcpy(b,j+t->start,n);b[n]=0;errno=0;v=strtoull(b,&e,10);if(errno||*e)return 0;*d=(uint64_t)v;return 1;}
static int valid_utf8(const char*s){const unsigned char*p=(const unsigned char*)s;while(*p){unsigned n;if(*p<0x80){p++;continue;}if((*p&0xe0)==0xc0)n=1;else if((*p&0xf0)==0xe0)n=2;else if((*p&0xf8)==0xf0)n=3;else return 0;p++;while(n--)if((*p++&0xc0)!=0x80)return 0;}return 1;}

static size_t esc(char *o,size_t cap,const char*s){size_t n=0;for(;*s;s++){unsigned char c=(unsigned char)*s;const char*x=NULL;if(c=='"')x="\\\"";else if(c=='\\')x="\\\\";else if(c=='\n')x="\\n";else if(c=='\r')x="\\r";else if(c=='\t')x="\\t";if(x){while(*x&&n+1<cap)o[n++]=*x++;}else if(c>=32&&n+1<cap)o[n++]=(char)c;}if(cap)o[n<cap?n:cap-1]=0;return n;}
static const char *state_name(np_state s){return s==NP_PLAYING?"playing":s==NP_PAUSED?"paused":s==NP_BUFFERING?"buffering":s==NP_ERROR?"error":"stopped";}
static const char *lms_mode(np_state s){return s==NP_PLAYING?"play":s==NP_PAUSED?"pause":"stop";}

int np_public_json(np_manager*m,char*out,size_t cap){char so[192],sn[384],id[384],ti[512],ar[512],al[512],st[512],aw[768];const np_status*s=&m->s;esc(so,sizeof so,s->source);esc(sn,sizeof sn,s->source_name);esc(id,sizeof id,s->track.id);esc(ti,sizeof ti,s->track.title);esc(ar,sizeof ar,s->track.artist);esc(al,sizeof al,s->track.album);esc(st,sizeof st,s->track.station);esc(aw,sizeof aw,s->track.artwork);return snprintf(out,cap,"{\"api_version\":1,\"active\":%s,\"source\":\"%s\",\"name\":\"%s\",\"generation\":%llu,\"state\":\"%s\",\"live\":%s,\"position_ms\":%llu,\"duration_ms\":%llu,\"duration_known\":%s,\"track\":{\"id\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"station\":\"%s\",\"artwork\":\"%s\"}}",s->active?"true":"false",so,sn,(unsigned long long)s->generation,state_name(s->state),s->live?"true":"false",(unsigned long long)np_position(m),(unsigned long long)s->duration_ms,s->duration_known?"true":"false",id,ti,ar,al,st,aw);}

int np_playerstatus_json(np_manager*m,char*out,size_t cap){const np_status*s=&m->s;char id[384],ti[512],ar[512],al[512],st[512],aw[768],text[1600];const char*display_album;uint64_t pos=np_position(m);if(!s->active)return snprintf(out,cap,"{\"player_name\":\"Standalone\",\"player_connected\":1,\"power\":1,\"mode\":\"stop\",\"time\":0,\"duration\":0,\"playlist_tracks\":0,\"playlist_cur_index\":0,\"mixer volume\":50,\"digital_volume_control\":1,\"player_needs_upgrade\":0,\"player_is_upgrading\":0,\"seq_no\":0}");esc(id,sizeof id,s->track.id);esc(ti,sizeof ti,s->track.title);esc(ar,sizeof ar,s->track.artist);esc(al,sizeof al,s->track.album);esc(st,sizeof st,s->track.station);esc(aw,sizeof aw,s->track.artwork);display_album=s->live&&!al[0]?st:al;snprintf(text,sizeof text,"%s\\n%s%s%s",ti,ar,ar[0]&&display_album[0]?" - ":"",display_album);return snprintf(out,cap,"{\"player_name\":\"Standalone\",\"player_connected\":1,\"power\":1,\"mode\":\"%s\",\"rate\":%d,\"time\":%.3f,\"duration\":%.3f,\"playlist_tracks\":1,\"playlist_cur_index\":0,\"playlist_timestamp\":%llu,\"playlist repeat\":0,\"playlist shuffle\":0,\"remote\":%d,\"current_title\":\"%s\",\"mixer volume\":50,\"digital_volume_control\":1,\"use_volume_control\":1,\"player_needs_upgrade\":0,\"player_is_upgrading\":0,\"seq_no\":0,\"item_loop\":[{\"text\":\"%s\",\"track\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"duration\":%.3f,\"icon\":\"%s\",\"params\":{\"track_id\":\"%s\"}}]}",lms_mode(s->state),s->state==NP_PLAYING?1:0,(double)pos/1000.0,s->duration_known?(double)s->duration_ms/1000.0:0.0,(unsigned long long)s->playlist_revision,s->live?1:0,s->live&&st[0]?st:ti,text,ti,ar,display_album,s->duration_known?(double)s->duration_ms/1000.0:0.0,aw,id);}

static int getstr(const char*j,jtok*t,int n,int obj,const char*k,char*o,size_t cap,int required){int x=member(j,t,n,obj,k);if(x<0)return !required;if(!string_value(j,&t[x],o,cap)||!valid_utf8(o))return 0;return 1;}
static int getbool(const char*j,jtok*t,int n,int obj,const char*k,int*d){int x=member(j,t,n,obj,k);return x<0?1:boolean(j,&t[x],d);}
static int getnum(const char*j,jtok*t,int n,int obj,const char*k,uint64_t*d){int x=member(j,t,n,obj,k);return x<0?1:number(j,&t[x],d);}
static int has_member(const char*j,jtok*t,int n,int obj,const char*k){return member(j,t,n,obj,k)>=0;}
static int track_equal(const np_track*a,const np_track*b){return !memcmp(a,b,sizeof *a);}
static uint64_t next_revision(uint64_t value){value++;return value?value:1;}
static int session_ok(np_manager*m,const char*j,jtok*t,int n){char token[NP_TOKEN_MAX];return getstr(j,t,n,0,"session_id",token,sizeof token,1)&&m->s.active&&!strcmp(token,m->s.session_id);}
static int error_json(char*out,size_t cap,const char*code){snprintf(out,cap,"{\"success\":false,\"error\":\"%s\"}",code);return 0;}

int np_queue_command(np_manager*m,const char*command,int64_t value){unsigned tail;if(!m||!m->s.active||m->q_count>=m->command_limit)return 0;tail=(m->q_head+m->q_count)%NP_COMMANDS_MAX;m->queue[tail].id=++m->command_counter;m->queue[tail].generation=m->s.generation;snprintf(m->queue[tail].command,sizeof m->queue[tail].command,"%s",command);m->queue[tail].value=value;m->q_count++;return 1;}

static int commands_json(np_manager*m,char*out,size_t cap){size_t u=0;unsigned i;u+=(size_t)snprintf(out+u,cap-u,"{\"success\":true,\"commands\":[");for(i=0;i<m->q_count&&u<cap;i++){np_command*q=&m->queue[(m->q_head+i)%NP_COMMANDS_MAX];u+=(size_t)snprintf(out+u,cap-u,"%s{\"id\":%llu,\"command\":\"%s\",\"value\":%lld}",i?",":"",(unsigned long long)q->id,q->command,(long long)q->value);}snprintf(out+u,u<cap?cap-u:0,"]}");return 200;}

int np_api(np_manager*m,const char*method,const char*path,const char*body,size_t len,char*out,size_t cap,int*changed){jtok t[JTOKENS];int n=-1;uint64_t now=np_monotonic_ms();if(changed)*changed=0;if(!strcmp(method,"GET")&&!strcmp(path,"/api/nowplaying")){np_public_json(m,out,cap);return 200;}if(len>16384){error_json(out,cap,"payload_too_large");return 413;}if(strcmp(method,"POST")&&strncmp(path,"/api/nowplaying/commands",24)){error_json(out,cap,"method_not_allowed");return 405;}if(len)n=parse_json(body,len,t,JTOKENS);if(len&&n<0){error_json(out,cap,"invalid_json");return 400;}
  if(!strcmp(path,"/api/nowplaying/claim")){int co;char source[NP_SOURCE_MAX],name[NP_NAME_MAX];if(strcmp(method,"POST")||n<0||!getstr(body,t,n,0,"source",source,sizeof source,1)||!getstr(body,t,n,0,"name",name,sizeof name,1)){error_json(out,cap,"invalid_claim");return 400;}reset_status(m);m->s.active=1;m->s.generation=++m->generation_counter;m->s.activated_ms=m->s.last_update_ms=now;snprintf(m->s.source,sizeof m->s.source,"%s",source);snprintf(m->s.source_name,sizeof m->s.source_name,"%s",name);if(!alloc_token(m->s.session_id)){reset_status(m);error_json(out,cap,"entropy_unavailable");return 503;}co=member(body,t,n,0,"capabilities");if(co>=0&&t[co].type!=J_OBJECT){reset_status(m);error_json(out,cap,"invalid_capabilities");return 400;}if(co>=0){if(!getbool(body,t,n,co,"play",&m->s.capabilities.play)||!getbool(body,t,n,co,"pause",&m->s.capabilities.pause)||!getbool(body,t,n,co,"stop",&m->s.capabilities.stop)||!getbool(body,t,n,co,"next",&m->s.capabilities.next)||!getbool(body,t,n,co,"previous",&m->s.capabilities.previous)||!getbool(body,t,n,co,"seek",&m->s.capabilities.seek)||!getbool(body,t,n,co,"volume",&m->s.capabilities.volume)){reset_status(m);error_json(out,cap,"invalid_capabilities");return 400;}}snprintf(out,cap,"{\"success\":true,\"session_id\":\"%s\",\"generation\":%llu,\"active\":true}",m->s.session_id,(unsigned long long)m->s.generation);if(changed)*changed=NP_CHANGE_OWNER;return 201;}
  if(!strcmp(path,"/api/nowplaying/update")){char state[20]={0},incoming_id[NP_ID_MAX]={0};np_status candidate=m->s;int tr,flags=0,live=candidate.live;uint64_t pos=candidate.position_ms,dur=candidate.duration_ms;if(n<0||!session_ok(m,body,t,n)){error_json(out,cap,"invalid_session");return 409;}if(!getstr(body,t,n,0,"state",state,sizeof state,0)||!getbool(body,t,n,0,"live",&live)||!getnum(body,t,n,0,"position_ms",&pos)){error_json(out,cap,"invalid_update");return 400;}if(state[0]){if(!strcmp(state,"playing"))candidate.state=NP_PLAYING;else if(!strcmp(state,"paused"))candidate.state=NP_PAUSED;else if(!strcmp(state,"stopped"))candidate.state=NP_STOPPED;else if(!strcmp(state,"buffering"))candidate.state=NP_BUFFERING;else if(!strcmp(state,"error"))candidate.state=NP_ERROR;else{error_json(out,cap,"invalid_state");return 400;}}tr=member(body,t,n,0,"track");if(tr>=0){if(t[tr].type!=J_OBJECT){error_json(out,cap,"invalid_track");return 400;}if(has_member(body,t,n,tr,"id")){if(!getstr(body,t,n,tr,"id",incoming_id,sizeof incoming_id,1)){error_json(out,cap,"invalid_track");return 400;}if(strcmp(incoming_id,candidate.track.id)){memset(&candidate.track,0,sizeof candidate.track);dur=0;}snprintf(candidate.track.id,sizeof candidate.track.id,"%s",incoming_id);}if(!getstr(body,t,n,tr,"title",candidate.track.title,sizeof candidate.track.title,0)||!getstr(body,t,n,tr,"artist",candidate.track.artist,sizeof candidate.track.artist,0)||!getstr(body,t,n,tr,"album",candidate.track.album,sizeof candidate.track.album,0)||!getstr(body,t,n,tr,"album_artist",candidate.track.album_artist,sizeof candidate.track.album_artist,0)||!getstr(body,t,n,tr,"station",candidate.track.station,sizeof candidate.track.station,0)||!getstr(body,t,n,tr,"content_type",candidate.track.content_type,sizeof candidate.track.content_type,0)||!getstr(body,t,n,tr,"artwork_url",candidate.track.artwork,sizeof candidate.track.artwork,0)||!getnum(body,t,n,tr,"duration_ms",&dur)){error_json(out,cap,"invalid_track");return 400;}}
    candidate.live=live;candidate.position_ms=pos;candidate.duration_ms=dur;candidate.duration_known=!live&&dur>0;candidate.last_update_ms=now;if(candidate.state!=m->s.state||candidate.live!=m->s.live||candidate.position_ms!=m->s.position_ms||candidate.duration_ms!=m->s.duration_ms||candidate.duration_known!=m->s.duration_known)flags|=NP_CHANGE_PLAYBACK;if(!track_equal(&candidate.track,&m->s.track))flags|=NP_CHANGE_METADATA;if(strcmp(candidate.track.id,m->s.track.id))flags|=NP_CHANGE_IDENTITY;if(strcmp(candidate.track.artwork,m->s.track.artwork))flags|=NP_CHANGE_ARTWORK;if(flags&NP_CHANGE_METADATA)candidate.metadata_revision=next_revision(m->s.metadata_revision);if(flags&NP_CHANGE_PLAYBACK)candidate.playback_revision=next_revision(m->s.playback_revision);if(flags&NP_CHANGE_IDENTITY)candidate.playlist_revision=next_revision(m->s.playlist_revision);if(flags&NP_CHANGE_ARTWORK)candidate.artwork_revision=next_revision(m->s.artwork_revision);candidate.position_clock_ms=(flags&NP_CHANGE_PLAYBACK)?now:m->s.position_clock_ms;m->s=candidate;snprintf(out,cap,"{\"success\":true,\"generation\":%llu}",(unsigned long long)m->s.generation);if(changed)*changed=flags;return 200;}
  if(!strcmp(path,"/api/nowplaying/heartbeat")){if(n<0||!session_ok(m,body,t,n)){error_json(out,cap,"invalid_session");return 409;}m->s.last_update_ms=now;snprintf(out,cap,"{\"success\":true,\"generation\":%llu}",(unsigned long long)m->s.generation);return 200;}
  if(!strcmp(path,"/api/nowplaying/release")){if(n<0||!session_ok(m,body,t,n)){error_json(out,cap,"invalid_session");return 409;}reset_status(m);snprintf(out,cap,"{\"success\":true}");if(changed)*changed=NP_CHANGE_OWNER;return 200;}
  if(!strncmp(path,"/api/nowplaying/commands?",25)){const char*q=strstr(path,"session_id=");if(strcmp(method,"GET")||!q||!m->s.active||strcmp(q+11,m->s.session_id)){error_json(out,cap,"invalid_session");return 409;}return commands_json(m,out,cap);}
  if(!strcmp(path,"/api/nowplaying/commands/ack")){uint64_t id=0;if(n<0||!session_ok(m,body,t,n)||!getnum(body,t,n,0,"id",&id)){error_json(out,cap,"invalid_ack");return 400;}while(m->q_count&&m->queue[m->q_head].id<=id){m->q_head=(m->q_head+1)%NP_COMMANDS_MAX;m->q_count--;}snprintf(out,cap,"{\"success\":true}");return 200;}
  error_json(out,cap,"not_found");return 404;
}

int np_demo(np_manager*m){reset_status(m);m->s.active=1;m->s.generation=++m->generation_counter;m->s.activated_ms=m->s.last_update_ms=np_monotonic_ms();m->s.state=NP_PLAYING;m->s.capabilities.next=1;snprintf(m->s.source,sizeof m->s.source,"demo");snprintf(m->s.source_name,sizeof m->s.source_name,"Now Playing Demo");snprintf(m->s.session_id,sizeof m->s.session_id,"internal-demo");snprintf(m->s.track.id,sizeof m->s.track.id,"900001");snprintf(m->s.track.title,sizeof m->s.track.title,"Sultans of Swing");snprintf(m->s.track.artist,sizeof m->s.track.artist,"Dire Straits");snprintf(m->s.track.album,sizeof m->s.track.album,"Dire Straits");m->s.duration_ms=348000;m->s.duration_known=1;m->s.position_clock_ms=np_monotonic_ms();m->s.playlist_revision=1;return 1;}
int np_demo_next(np_manager*m){if(!m||strcmp(m->s.source,"demo"))return 0;if(!strcmp(m->s.track.id,"900001")){snprintf(m->s.track.id,sizeof m->s.track.id,"900002");snprintf(m->s.track.title,sizeof m->s.track.title,"Money for Nothing");snprintf(m->s.track.album,sizeof m->s.track.album,"Brothers in Arms");m->s.duration_ms=506000;}else np_demo(m);m->s.position_ms=0;m->s.position_clock_ms=np_monotonic_ms();m->s.playlist_revision++;return 1;}
