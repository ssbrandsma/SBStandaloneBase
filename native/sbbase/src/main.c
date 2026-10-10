#define _POSIX_C_SOURCE 200809L

#include "sbbase.h"
#include "nowplaying.h"

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


#define CLIENTS 32

#define HTTP_IDLE_SECONDS 300

#define STREAM_HEARTBEAT_SECONDS 2
#define SEND_WAIT_MS 2000

typedef struct {
  int fd, kind, helo;
  int playerstatus_subscribed;
  time_t keepalive;
  size_t used;
  char client_id[32];
  char subscribed_player[18];
  unsigned char buf[SB_MAX_HTTP_HEADER + SB_MAX_HTTP_BODY + 1];
}
client;

static int player_connected;
static int test_nowplaying;
static np_manager *nowplaying;

static char player_id[18];

static client * clients;

static char catalog_data[16384] = "{\"count\":0,\"item_loop\":[]}";

static sb_config * active_config;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_catalog = 0;

static int json_string(const char * body, const char * key, char * out, size_t cap);

static void stop(int sig) {
  (void) sig;
  running = 0;
}

static void reload(int sig) {
  (void) sig;
  reload_catalog = 1;
}

static int nonblock(int fd) {
  int f = fcntl(fd, F_GETFL, 0);
  return f < 0 ? -1 : fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int listener(int type, unsigned port) {

  int fd = socket(AF_INET, type, 0), yes = 1;
  struct sockaddr_in a;

  if (fd < 0) return -1;

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, & yes, sizeof yes);

  memset( & a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((unsigned short) port);

  if (bind(fd, (struct sockaddr * ) & a, sizeof a) < 0 || (type == SOCK_STREAM && listen(fd, 8) < 0)) {
    close(fd);
    return -1;
  }
  nonblock(fd);
  return fd;

}

static void close_client(client * c) {
  if (c -> fd >= 0) close(c -> fd);
  c -> fd = -1;
  c -> used = 0;
  c -> client_id[0] = 0;
  c -> playerstatus_subscribed = 0;
  c -> subscribed_player[0] = 0;
}

static int send_data(client * c,
  const char * data, size_t size) {
  size_t off = 0;

  while (off < size) {
    ssize_t n = send(c -> fd, data + off, size - off, 0);

    if (n > 0) {
      off += (size_t) n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {
        c -> fd,
        POLLOUT,
        0
      };
      int r;
      do {
        r = poll( & pfd, 1, SEND_WAIT_MS);
      } while (r < 0 && errno == EINTR);

      if (r > 0 && (pfd.revents & POLLOUT))
        continue;
    }

    fprintf(stderr, "SEND CLOSE fd=%d errno=%d (%s)\n",
      c -> fd, errno, strerror(errno));
    close_client(c);
    return 0;
  }
  return 1;
}

static void http_reply(client * c,
  const char * status,
    const char * body, int keep) {
  char h[256];
  size_t n = strlen(body);
  int l = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nContent-Length: %lu\r\nConnection: %s\r\n\r\n", status, (unsigned long) n, keep ? "keep-alive" : "close");
  if (!send_data(c, h, (size_t) l) || !send_data(c, body, n)) return;
  if (!keep) close_client(c);
  else c -> keepalive = time(NULL);
}

static void stream_start(client * c,
  const char * body) {
  char h[256], chunk[9216];
  size_t n = strlen(body);
  int l = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n");
  if (!send_data(c, h, (size_t) l)) return;
  l = snprintf(chunk, sizeof chunk, "%lx\r\n%s\r\n", (unsigned long) n, body);
  if (!send_data(c, chunk, (size_t) l)) return;
  c -> kind = 3;
  c -> used = 0;
  c -> keepalive = time(NULL);
}

static void stream_heartbeat(client * c) {
  if (send_data(c, "2\r\n[]\r\n", 7)) c -> keepalive = time(NULL);
}

static void stream_event(client * c,
  const char * body) {
  char chunk[9216];
  size_t n = strlen(body);
  int l = snprintf(chunk, sizeof chunk, "%lx\r\n%s\r\n", (unsigned long) n, body);
  if (send_data(c, chunk, (size_t) l)) c -> keepalive = time(NULL);
}

static void player_status_json(char * out, size_t cap) {
  np_playerstatus_json(nowplaying, out, cap);
}

static int split_playerstatus_path(const char * path, char * client_id,
  size_t client_cap, char * requested_player, size_t player_cap) {
  const char * marker = strstr(path, "/slim/playerstatus/");
  size_t client_len, player_len;
  if (!marker || path[0] != '/') return 0;
  client_len = (size_t)(marker - path - 1);
  player_len = strlen(marker + strlen("/slim/playerstatus/"));
  if (!client_len || client_len >= client_cap || player_len != 17 || player_len >= player_cap) return 0;
  memcpy(client_id, path + 1, client_len);
  client_id[client_len] = 0;
  memcpy(requested_player, marker + strlen("/slim/playerstatus/"), player_len + 1);
  return 1;
}

static client * stream_for_id(const char * id) {
  int i;
  for (i = 0; i < CLIENTS; i++)
    if (clients[i].fd >= 0 && clients[i].kind == 3 && !strcmp(clients[i].client_id, id)) return &clients[i];
  return NULL;
}

static void publish_playerstatus(void) {
  char data[4096], event[8192], channel[128];
  int i, subscribers = 0, delivered = 0;
  if (!player_id[0]) return;
  player_status_json(data, sizeof data);
  for (i = 0; i < CLIENTS; i++) {
    client * c = &clients[i];
    if (c -> fd < 0 || c -> kind != 3 || !c -> playerstatus_subscribed ||
      strcmp(c -> subscribed_player, player_id)) continue;
    subscribers++;
    snprintf(channel, sizeof channel, "/%s/slim/playerstatus/%s", c -> client_id, player_id);
    snprintf(event, sizeof event, "[{\"channel\":\"%s\",\"data\":%s}]", channel, data);
    if (test_nowplaying) fprintf(stderr, "NOWPLAYING PUSH client=%s\n", c -> client_id);
    stream_event(c, event);
    if (c -> fd >= 0) delivered++;
  }
  if (test_nowplaying) fprintf(stderr,
    "NOWPLAYING DELIVERY subscribers=%d delivered=%d revision=%llu\n",
    subscribers, delivered,
    (unsigned long long)np_current(nowplaying) -> playlist_revision);
}

static void register_playerstatus(const char * path) {
  char cid[32], requested[18];
  client * stream;
  if (!split_playerstatus_path(path, cid, sizeof cid, requested, sizeof requested)) return;
  if (!player_id[0] || strcmp(requested, player_id)) return;
  stream = stream_for_id(cid);
  if (!stream) return;
  stream -> playerstatus_subscribed = 1;
  snprintf(stream -> subscribed_player, sizeof stream -> subscribed_player, "%s", requested);
  if (test_nowplaying) fprintf(stderr, "NOWPLAYING SUBSCRIBE player=%s client=%s\n", requested, cid);
}

static void unregister_playerstatus(const char * body) {
  char path[160], cid[32], requested[18];
  client * stream;
  if (!json_string(body, "\"unsubscribe\"", path, sizeof path) ||
    !split_playerstatus_path(path, cid, sizeof cid, requested, sizeof requested)) return;
  stream = stream_for_id(cid);
  if (stream && stream -> playerstatus_subscribed && !strcmp(stream -> subscribed_player, requested)) {
    stream -> playerstatus_subscribed = 0;
    stream -> subscribed_player[0] = 0;
  }
}

static void publish_player(void) {
  char event[2048];
  sb_config * cfg = active_config;
  int i;
  if (!cfg || !player_id[0]) return;
  for (i = 0; i < CLIENTS; i++)
    if (clients[i].fd >= 0 && clients[i].kind == 3 && clients[i].client_id[0]) {
      snprintf(event, sizeof event, "[{\"channel\":\"/%s/slim/serverstatus\",\"data\":{\"httpport\":\"%u\",\"ip\":\"%s\",\"version\":\"%s\",\"uuid\":\"%s\",\"player count\":1,\"players_loop\":[{\"playerindex\":\"0\",\"playerid\":\"%s\",\"name\":\"Standalone\",\"model\":\"baby\",\"modelname\":\"Squeezebox Radio\",\"isplayer\":1,\"connected\":1,\"power\":1,\"firmware\":\"7.7.3\",\"ip\":\"127.0.0.1\",\"seq_no\":0,\"displaytype\":\"none\",\"isplaying\":%d,\"canpoweroff\":1}]}}]", clients[i].client_id, cfg -> http_port, cfg -> advertise_ip[0] ? cfg -> advertise_ip : "127.0.0.1", cfg -> lms_version, cfg -> uuid, player_id, np_current(nowplaying)->state == NP_PLAYING);
      fprintf(stderr, "COMET PUSH client=%s\n", clients[i].client_id);
      stream_event( & clients[i], event);
    }
}

static void token(char out[9]) {
  static unsigned long sequence;
  unsigned long x = (unsigned long) time(NULL) ^ (unsigned long) getpid() ^ (++sequence * 2654435761ul);
  snprintf(out, 9, "%08lx", x & 0xfffffffful);
}

static void json_unescape_slashes(char * s) {
  char * r = s, * w = s;
  while ( * r) {
    if (r[0] == '\\' && r[1] == '/') r++;
    *w++ = *r++;
  }
  *w = 0;
}

static int valid_player_id(const char * id) {
  int i;
  if (strlen(id) != 17) return 0;
  for (i = 0; i < 17; i++) {
    if ((i + 1) % 3 == 0) {
      if (id[i] != ':') return 0;
    } else if (!((id[i] >= '0' && id[i] <= '9') ||
      (id[i] >= 'a' && id[i] <= 'f') || (id[i] >= 'A' && id[i] <= 'F'))) return 0;
  }
  return 1;
}

static int json_string(const char * body,
  const char * key, char * out, size_t cap) {
  const char * p = strstr(body, key);
  size_t n = 0;
  if (!p || cap < 2) return 0;
  p += strlen(key);
  while ( * p == ' ' || * p == '\t' || * p == ':') p++;
  if ( * p != '\"') return 0;
  for (p++;* p && * p != '\"'; p++) {
    if (n + 1 < cap) out[n++] = * p;
  }
  if ( * p != '\"') return 0;
  out[n] = 0;
  return 1;
}

static int json_raw(const char * body,
  const char * key, char * out, size_t cap) {
  const char * p = strstr(body, key),
    * start;
  size_t n;
  if (!p || cap < 2) return 0;
  p += strlen(key);
  while ( * p == ' ' || * p == '\t' || * p == ':') p++;
  start = p;
  if ( * p == '\"') {
    for (p++;* p && * p != '\"'; p++)
      if ( * p == '\\' && p[1]) p++;
    if ( * p == '\"') p++;
  } else
    while ( * p && * p != ',' && * p != '}' && * p != ']' && * p != ' ' && * p != '\t' && * p != '\r' && * p != '\n') p++;
  n = (size_t)(p - start);
  if (!n || n >= cap) return 0;
  memcpy(out, start, n);
  out[n] = 0;
  return 1;
}

static int before(const char * start,
  const char * needle,
    const char * end) {
  const char * p = strstr(start, needle);
  return p && p < end;
}

static void add_json(char * out, size_t cap, size_t * used,
  const char * json) {
  size_t n = strlen(json);
  if ( * used + n + 2 >= cap) return;
  if ( * used > 1) out[( * used) ++] = ',';
  memcpy(out + * used, json, n);* used += n;
  out[ * used] = 0;
}

static int load_catalog(const char * path) {
  FILE * f;
  char * b, * p, * start;
  long size;
  size_t used = 0;
  int count = 0, depth, in_string, escape;
  if (!path || !(f = fopen(path, "rb"))) return -1;
  if (fseek(f, 0, SEEK_END) || ((size = ftell(f)) < 2) || size > 65536 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return -1;
  }
  b = malloc((size_t) size + 1);
  if (!b) {
    fclose(f);
    return -1;
  }
  if (fread(b, 1, (size_t) size, f) != (size_t) size) {
    free(b);
    fclose(f);
    return -1;
  }
  fclose(f);
  b[size] = 0;
  p = strstr(b, "\"applets\"");
  if (!p || (p = strchr(p, '[')) == NULL) {
    free(b);
    return -1;
  }
  strcpy(catalog_data, "{\"count\":0,\"item_loop\":[");
  used = strlen(catalog_data);
  for (p++;* p && * p != ']';) {
    while ( * p && * p != '{' && * p != ']') p++;
    if ( * p != '{') break;
    start = p;
    depth = 0;
    in_string = escape = 0;
    do {
      char ch = * p++;
      if (in_string) {
        if (escape) escape = 0;
        else if (ch == '\\') escape = 1;
        else if (ch == '\"') in_string = 0;
      } else if (ch == '\"') in_string = 1;
      else if (ch == '{') depth++;
      else if (ch == '}') depth--;
    } while ( * p && depth);
    if (depth || used + (size_t)(p - start) + 4 >= sizeof catalog_data) {
      free(b);
      return -1;
    }
    if (count) catalog_data[used++] = ',';
    memcpy(catalog_data + used, start, (size_t)(p - start));
    used += (size_t)(p - start);
    catalog_data[used] = 0;
    count++;
  }
  if (used + 3 >= sizeof catalog_data) {
    free(b);
    return -1;
  }
  catalog_data[used++] = ']';
  catalog_data[used++] = '}';
  catalog_data[used] = 0;
  {
    char prefix[32];
    size_t old = strlen("{\"count\":0");
    snprintf(prefix, sizeof prefix, "{\"count\":%d", count);
    if (strlen(prefix) != old) {
      free(b);
      return -1;
    }
    memcpy(catalog_data, prefix, old);
  }
  free(b);
  return count;
}

static void request_responses(const char * body,
  const sb_config * cfg, char * out, size_t cap) {

  const char * p = body,
    * next;
  size_t used = 1;
  char id[32], reply[160], msg[20000], data[18000];
  out[0] = '[';
  out[1] = 0;

  while ((p = strstr(p, "\"id\"")) != NULL) {
    next = strstr(p + 4, "\"id\"");
    if (!next) next = p + strlen(p);
    id[0] = 0;
    reply[0] = 0;
    json_raw(p, "\"id\"", id, sizeof id);
    if (before(p, "\"response\"", next)) json_string(p, "\"response\"", reply, sizeof reply);
    if (!id[0] || !reply[0]) {
      p += 4;
      continue;
    }

    if (before(p, "serverstatus", next)) snprintf(data, sizeof data, "{\"httpport\":\"%u\",\"ip\":\"%s\",\"version\":\"%s\",\"uuid\":\"%s\",\"player count\":%d,\"players_loop\":%s}", cfg -> http_port, cfg -> advertise_ip, cfg -> lms_version, cfg -> uuid, player_id[0] ? 1 : 0, player_id[0] ? "PLAYER" : "[]");

    else if (before(p, "firmwareupgrade", next)) strcpy(data, "{\"firmwareupgrade\":0,\"player_needs_upgrade\":0,\"player_is_upgrading\":0}");

    else if (before(p, "\"date\"", next)) snprintf(data, sizeof data, "{\"date_epoch\":%lu,\"date\":\"0000-00-00T00:00:00+00:00\"}", (unsigned long) time(NULL));

    else if (before(p, "jiveapplets", next)) snprintf(data, sizeof data, "%s", catalog_data);

    /* SlimMenus waits for the initial menustatus subscription result before
     * it considers the connected server's Home menu loaded. An empty menu is
     * valid, but it must use LMS' notification-array shape. */
    else if (before(p, "menustatus", next))
      snprintf(data, sizeof data, "[\"menustatus\",[],\"add\",\"%s\"]",
        player_id[0] ? player_id : "all");

    else if (before(p, "\"menu\"", next))
      strcpy(data, "{\"count\":0,\"offset\":0,\"item_loop\":[]}");

    else if (before(p, "\"status\"", next)) {
      player_status_json(data, sizeof data);
      register_playerstatus(reply);
      if (test_nowplaying && strstr(reply, "/slim/playerstatus/"))
        fprintf(stderr, "NOWPLAYING STATUS requested player=%s\n", player_id[0] ? player_id : "unknown");
    }

    else if (before(p, "\"pause\"", next)) {
      np_queue_command(nowplaying, before(p, "\"0\"", next) ? "play" : "pause", 0);
      strcpy(data, "{\"count\":0,\"offset\":0,\"item_loop\":[]}");
    }

    else if (before(p, "\"play\"", next)) {
      np_queue_command(nowplaying, "play", 0);
      strcpy(data, "{\"count\":0,\"offset\":0,\"item_loop\":[]}");
    }

    else if (before(p, "\"stop\"", next)) {
      np_queue_command(nowplaying, "stop", 0);
      strcpy(data, "{\"count\":0,\"offset\":0,\"item_loop\":[]}");
    }

    else if (before(p, "\"playlist\"", next) && before(p, "\"index\"", next)) {
      np_queue_command(nowplaying, before(p, "\"-1\"", next) ? "previous" : "next", 0);
      strcpy(data, "{\"count\":0,\"offset\":0,\"item_loop\":[]}");
    }

    else strcpy(data, "{\"count\":0,\"offset\":0,\"item_loop\":[]}");

    if (strstr(data, "PLAYER")) {
      char player[900], * mark = strstr(data, "PLAYER");
      /* This status bootstraps SqueezePlay's server switch. Reporting zero
       * until the later SlimProto HELO makes the client detach before it can
       * establish that connection, resulting in a reconnect loop. */
      snprintf(player, sizeof player, "[{\"playerindex\":\"0\",\"playerid\":\"%s\",\"name\":\"Standalone\",\"model\":\"baby\",\"modelname\":\"Squeezebox Radio\",\"isplayer\":1,\"connected\":1,\"power\":1,\"firmware\":\"7.7.3\",\"ip\":\"127.0.0.1\",\"seq_no\":0,\"displaytype\":\"none\",\"isplaying\":%d,\"canpoweroff\":1}]", player_id, np_current(nowplaying)->state == NP_PLAYING);
      memmove(mark + strlen(player), mark + 6, strlen(mark + 6) + 1);
      memcpy(mark, player, strlen(player));
    }

    snprintf(msg, sizeof msg, "{\"id\":%s,\"channel\":\"/slim/request\",\"successful\":true}", id);
    add_json(out, cap, & used, msg);
    snprintf(msg, sizeof msg, "{\"id\":%s,\"channel\":\"%s\",\"data\":%s}", id, reply, data);
    add_json(out, cap, & used, msg);
    p = next;

  }
  if (used + 2 < cap) {
    out[used++] = ']';
    out[used] = 0;
  }

}

static void handle_http(client * c,
  const sb_config * cfg) {

  char * s = (char * ) c -> buf, * end = strstr(s, "\r\n\r\n"), * body, * cl, * channel, * id, * size_end, * size_parse;
  long need = 0;
  char response[24000], cid[32], reply[160] = "/response", mac[32], rid[32] = "1";
  int i, chunked, keep_response = 0;
  char method[8], path[512];

  if (!end) return;

  cl = strstr(s, "Content-Length:");
  if (!cl) cl = strstr(s, "content-length:");
  if (cl) need = strtol(cl + 15, NULL, 10);

  chunked = !cl && (strstr(s, "Transfer-Encoding: chunked") || strstr(s, "transfer-encoding: chunked"));
  body = end + 4;

  if (chunked) {
    size_end = strstr(body, "\r\n");
    if (!size_end) return;
    errno = 0;
    need = strtol(body, & size_parse, 16);
    if (errno || size_parse != size_end) need = -1;
    if (need >= 0 && (size_t)(size_end + 2 - s) + (size_t) need + 2 > c -> used) return;
    if (need >= 0) {
      memmove(body, size_end + 2, (size_t) need);
      body[need] = 0;
    }
  }

  if (need < 0 || need > SB_MAX_HTTP_BODY) {
    http_reply(c, "413 Payload Too Large", "[{\"successful\":false}]", 0);
    return;
  }

  if (!chunked && (size_t)(body - s) + (size_t) need > c -> used) return;

  method[0] = path[0] = 0;
  if (sscanf(s, "%7s %511s", method, path) != 2) {
    http_reply(c, "400 Bad Request", "{\"success\":false,\"error\":\"invalid_request\"}", 0);
    return;
  }

  if (!strncmp(path, "/api/nowplaying", 15)) {
    int was_active = np_current(nowplaying) -> active;
    int was_playing = np_current(nowplaying) -> state == NP_PLAYING;
    int changed = 0, status = np_api(nowplaying, method, path, body, (size_t)need,
      response, sizeof response, &changed);
    const char * phrase = status == 200 ? "200 OK" : status == 201 ? "201 Created" :
      status == 400 ? "400 Bad Request" : status == 405 ? "405 Method Not Allowed" :
      status == 409 ? "409 Conflict" : status == 413 ? "413 Payload Too Large" :
      status == 429 ? "429 Too Many Requests" : status == 503 ? "503 Service Unavailable" :
      "404 Not Found";
    if (changed) {
      /* Server status controls attachment, not playback mode. A station
       * switch briefly buffers; re-announcing the player then races and
       * resets the track update that follows. */
      int is_active = np_current(nowplaying) -> active;
      (void) was_playing;
      if (was_active != is_active) publish_player();
      publish_playerstatus();
    }
    if (test_nowplaying && !strcmp(path, "/api/nowplaying/update"))
      fprintf(stderr,
        "NOWPLAYING UPDATE status=%d flags=%d generation=%llu revision=%llu id=%s station=%s artwork_changed=%d artwork_len=%lu\n",
        status, changed,
        (unsigned long long)np_current(nowplaying) -> generation,
        (unsigned long long)np_current(nowplaying) -> playlist_revision,
        np_current(nowplaying) -> track.id,
        np_current(nowplaying) -> track.station,
        !!(changed & NP_CHANGE_ARTWORK),
        (unsigned long)strlen(np_current(nowplaying) -> track.artwork));
    http_reply(c, phrase, response, 0);
    return;
  }

  if (strncmp(s, "POST /cometd ", 13) && strncmp(s, "GET /health ", 12) &&
    strncmp(s, "POST /test/next-track ", 22)) {
    http_reply(c, "404 Not Found", "[{\"successful\":false,\"error\":\"not found\"}]", 0);
    return;
  }

  if (!strncmp(s, "GET /health ", 12)) {
    http_reply(c, "200 OK", "{\"status\":\"ok\",\"service\":\"sbbase\",\"version\":\""
      SBBASE_VERSION "\",\"lms_version\":\""
      SB_LMS_COMPAT_VERSION "\"}", 0);
    return;
  }

  if (!strncmp(s, "POST /test/next-track ", 22)) {
    if (!test_nowplaying) {
      http_reply(c, "404 Not Found", "{\"error\":\"not found\"}", 0);
      return;
    }
    np_demo_next(nowplaying);
    fprintf(stderr, "NOWPLAYING TRACK changed\n");
    publish_playerstatus();
    http_reply(c, "200 OK", "{\"status\":\"ok\"}", 0);
    return;
  }

  /* Comet owns a reusable request socket (rhttp). Sinks can enqueue another
   * request while processing this response, so closing the socket here races
   * that re-entrant fetch and loses menu/playerstatus subscriptions. */
  keep_response = 1;
  json_unescape_slashes(body);
  channel = strstr(body, "\"channel\"");
  id = strstr(body, "\"id\"");
  (void) id;

  if (!channel) {
    http_reply(c, "400 Bad Request", "[{\"successful\":false,\"error\":\"invalid Bayeux payload\"}]", 0);
    return;
  }

  cid[0] = 0;
  json_string(body, "\"response\"", reply, sizeof reply);
  json_string(body, "\"clientId\"", cid, sizeof cid);
  json_raw(body, "\"id\"", rid, sizeof rid);

  if (strstr(channel, "/meta/handshake")) {
    token(cid);
    if (json_string(body, "\"mac\"", mac, sizeof mac) && valid_player_id(mac)) strcpy(player_id, mac);
    snprintf(response, sizeof response, "[{\"id\":\"1\",\"channel\":\"/meta/handshake\",\"version\":\"1.0\",\"supportedConnectionTypes\":[\"long-polling\",\"streaming\"],\"clientId\":\"%s\",\"successful\":true,\"advice\":{\"reconnect\":\"retry\",\"interval\":0,\"timeout\":60000}}]", cid);
    /* SqueezePlay queues /meta/connect on this same chttp socket as soon as
     * the handshake sink fires, so this one response must stay reusable. */
    keep_response = 1;
  } else if (strstr(channel, "/meta/connect") || strstr(channel, "/meta/reconnect")) {
    int reconnecting = strstr(channel, "/meta/reconnect") != NULL;
    if (!cid[0]) {
      http_reply(c, "400 Bad Request", "[{\"successful\":false,\"error\":\"missing clientId\"}]", 0);
      return;
    }
    /* A Bayeux client has one event stream. Replace a stale duplicate so it
     * cannot consume a client slot indefinitely. */
    for (i = 0; i < CLIENTS; i++)
      if ( & clients[i] != c && clients[i].fd >= 0 && clients[i].kind == 3 &&
        !strcmp(clients[i].client_id, cid)) {
        fprintf(stderr, "COMET REPLACE client=%s oldfd=%d newfd=%d\n",
          cid, clients[i].fd, c -> fd);
        c -> playerstatus_subscribed = clients[i].playerstatus_subscribed;
        snprintf(c -> subscribed_player, sizeof c -> subscribed_player, "%s",
          clients[i].subscribed_player);
        close_client( & clients[i]);
      }
    snprintf(c -> client_id, sizeof c -> client_id, "%s", cid);
    snprintf(response, sizeof response,
      "[{\"channel\":\"/meta/%s\",\"clientId\":\"%s\",\"successful\":true,"
      "\"advice\":{\"reconnect\":\"retry\",\"interval\":0,\"timeout\":60000}},"
      "{\"channel\":\"/meta/subscribe\",\"clientId\":\"%s\",\"successful\":true}]",
      reconnecting ? "reconnect" : "connect", cid, cid);
    fprintf(stderr, "COMET RX %.*s\nCOMET STREAM client=%s\n", (int) need, body, cid);
    stream_start(c, response);
    /* A replacement stream inherits the logical subscription. Immediately
     * replay the current snapshot, so updates that raced the reconnect cannot
     * leave SqueezePlay displaying stale metadata. */
    if (c -> fd >= 0 && c -> playerstatus_subscribed) publish_playerstatus();
    return;
  } else if (strstr(channel, "/meta/disconnect")) {
    keep_response = 0;
    snprintf(response, sizeof response, "[{\"id\":\"1\",\"channel\":\"/meta/disconnect\",\"successful\":true}]");
    for (i = 0; i < CLIENTS; i++)
      if ( & clients[i] != c && clients[i].fd >= 0 && clients[i].kind == 3 && cid[0] && !strcmp(clients[i].client_id, cid)) close_client( & clients[i]);
  } else {
    unregister_playerstatus(body);
    request_responses(body, cfg, response, sizeof response);
  }

  fprintf(stderr, "COMET RX %.*s\nCOMET TX %s\n", (int) need, body, response);

  /* Content-Length delimits each response while rhttp remains reusable. */
  http_reply(c, "200 OK", response, keep_response);
  if (c -> fd >= 0) c -> used = 0;

}

static void keepalive(client * c) {
  unsigned char payload[24] = {
    't',
    '0',
    'm',
    '?',
    '?',
    '?',
    '?',
    0,
    0,
    0,
    '0',
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0
  }, frame[40];
  size_t z = sb_server_frame("strm", payload, sizeof payload, frame, sizeof frame);
  if (z) send(c -> fd, (char * ) frame, z, 0);
  c -> keepalive = time(NULL);
}

static void handle_slim(client * c) {
  size_t off = 0, used;
  char op[5];
  const unsigned char * p;
  uint32_t n;
  fprintf(stderr, "SLIM RX fd=%d bytes=%lu\n", c -> fd, (unsigned long) c -> used);
  while (off < c -> used) {
    int r = sb_parse_client_frame(c -> buf + off, c -> used - off, op, & p, & n, & used);
    if (r < 0) {
      fprintf(stderr, "SLIM INVALID fd=%d\n", c -> fd);
      close_client(c);
      return;
    }
    if (!r) {
      fprintf(stderr, "SLIM PARTIAL fd=%d remaining=%lu\n", c -> fd, (unsigned long)(c -> used - off));
      break;
    }
    fprintf(stderr, "SLIM FRAME fd=%d op=%s len=%lu\n", c -> fd, op, (unsigned long) n);
    if (!strcmp(op, "HELO")) {
      if (n >= 8) {
        snprintf(player_id, sizeof player_id, "%02x:%02x:%02x:%02x:%02x:%02x", p[2], p[3], p[4], p[5], p[6], p[7]);
        player_connected = 1;
      }
      c -> helo = 1;
      keepalive(c);
      publish_player();
    }
    off += used;
  }
  if (off) {
    memmove(c -> buf, c -> buf + off, c -> used - off);
    c -> used -= off;
  }
}

static void defaults(sb_config * c) {
  memset(c, 0, sizeof * c);
  strcpy(c -> name, "StandaloneBase");
  strcpy(c -> uuid, "9d989f40-499a-4b85-b92f-8dc415af2a04");
  strcpy(c -> lms_version, SB_LMS_COMPAT_VERSION);
  strcpy(c -> advertise_ip, "127.0.0.1");
  c -> http_port = 9000;
  c -> slim_port = c -> discovery_port = 3483;
  c -> time_sync = 1;
}

int main(int argc, char ** argv) {
  sb_config cfg;
  int udp, tcp, http, i;
  client cs[CLIENTS];
  struct pollfd pf[3 + CLIENTS];
  defaults( & cfg);
  active_config = & cfg;
  clients = cs;
  nowplaying = np_create(120, 32);
  if (!nowplaying) {
    fputs("nowplaying: out of memory\n", stderr);
    return 1;
  }

  if (argc == 2 && !strcmp(argv[1], "--version")) {
    printf("sbbase %s (LMS compatibility %s)\n", SBBASE_VERSION, cfg.lms_version);
    return 0;
  }

  if (argc == 2 && !strcmp(argv[1], "--self-test")) {
    unsigned char q[] = {
      'e',
      'N',
      'A',
      'M',
      'E',
      0
    }, o[96];
    return sb_discovery_response(q, sizeof q, o, sizeof o, & cfg, "127.0.0.1") ? 0 : 2;
  }

  if (argc == 3 && !strcmp(argv[1], "--check-config")) {
    if (load_catalog(argv[2]) < 0) return 2;
    puts(catalog_data);
    return 0;
  }

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--config") && i + 1 < argc) {
      const char * path = argv[++i];
      snprintf(cfg.catalog_path, sizeof cfg.catalog_path, "%s", path);
      if (load_catalog(path) < 0) {
        fprintf(stderr, "invalid or unreadable config: %s\n", path);
        return 2;
      }
    } else if (!strcmp(argv[i], "--test-nowplaying")) test_nowplaying = 1;
    else {
      fprintf(stderr, "usage: %s [--config FILE] [--test-nowplaying]\n", argv[0]);
      return 2;
    }
  }

  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  signal(SIGHUP, reload);
  signal(SIGPIPE, SIG_IGN);

  if (test_nowplaying) np_demo(nowplaying);

  udp = listener(SOCK_DGRAM, cfg.discovery_port);
  tcp = listener(SOCK_STREAM, cfg.slim_port);
  http = listener(SOCK_STREAM, cfg.http_port);
  if (udp < 0 || tcp < 0 || http < 0) {
    perror("listener");
    return 1;
  }
  for (i = 0; i < CLIENTS; i++) cs[i].fd = -1;
  fprintf(stderr, "sbbase %s (LMS %s): %s UDP/TCP %u, HTTP %u\n", SBBASE_VERSION, cfg.lms_version, cfg.advertise_ip, cfg.slim_port, cfg.http_port);
  if (test_nowplaying) fprintf(stderr, "NOWPLAYING TEST enabled\n");

  while (running) {
    if (reload_catalog) {
      reload_catalog = 0;
      if (cfg.catalog_path[0] && load_catalog(cfg.catalog_path) >= 0)
        fprintf(stderr, "catalog reloaded: %s\n", cfg.catalog_path);
      else fprintf(stderr, "catalog reload rejected; retaining active catalog\n");
    }
    pf[0] = (struct pollfd) {
      udp,
      POLLIN,
      0
    };
    pf[1] = (struct pollfd) {
      tcp,
      POLLIN,
      0
    };
    pf[2] = (struct pollfd) {
      http,
      POLLIN,
      0
    };
    for (i = 0; i < CLIENTS; i++) pf[3 + i] = (struct pollfd) {
      cs[i].fd, POLLIN, 0
    };
    if (poll(pf, 3 + CLIENTS, 1000) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (np_expire(nowplaying)) publish_playerstatus();

    if (pf[0].revents & POLLIN) {
      unsigned char in [SB_MAX_DISCOVERY + 1], out[1024];
      struct sockaddr_in peer;
      socklen_t pl = sizeof peer;
      ssize_t n = recvfrom(udp, in, sizeof in , 0, (struct sockaddr * ) & peer, & pl);
      if (n > 0) {
        size_t z = sb_discovery_response(in, (size_t) n, out, sizeof out, & cfg, cfg.advertise_ip);
        if (z) sendto(udp, out, z, 0, (struct sockaddr * ) & peer, pl);
      }
    }

    for (int li = 1; li <= 2; li++)
      if (pf[li].revents & POLLIN) {
        int fd = accept(pf[li].fd, NULL, NULL);
        if (fd >= 0) {
          nonblock(fd);
          for (i = 0; i < CLIENTS && cs[i].fd >= 0; i++);
          if (i == CLIENTS) {
            fprintf(stderr, "ACCEPT REJECT kind=%d fd=%d full\n", li, fd);
            close(fd);
          } else {
            fprintf(stderr, "ACCEPT kind=%d fd=%d slot=%d\n", li, fd, i);
            cs[i].fd = fd;
            cs[i].kind = -li;
            cs[i].used = 0;
            cs[i].helo = 0;
            cs[i].keepalive = time(NULL);
            cs[i].client_id[0] = 0;
            cs[i].playerstatus_subscribed = 0;
            cs[i].subscribed_player[0] = 0;
          }
        }
      }

    for (i = 0; i < CLIENTS; i++)
      if (cs[i].fd >= 0) {
        short rev;
        if (cs[i].kind < 0) {
          cs[i].kind = -cs[i].kind;
          continue;
        }
        rev = pf[3 + i].revents;
        if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
          close_client( & cs[i]);
          continue;
        }
        if (rev & POLLIN) {
          ssize_t n = recv(cs[i].fd, cs[i].buf + cs[i].used, sizeof(cs[i].buf) - 1 - cs[i].used, 0);
          if (n <= 0) {
            close_client( & cs[i]);
            continue;
          }
          cs[i].keepalive = time(NULL);
          cs[i].used += (size_t) n;
          cs[i].buf[cs[i].used] = 0;
          if (cs[i].kind == 1) handle_slim( & cs[i]);
          else if (cs[i].kind == 2) handle_http( & cs[i], & cfg);
        }
        if (cs[i].fd >= 0 && cs[i].kind == 1 && cs[i].helo && time(NULL) - cs[i].keepalive >= 4) keepalive( & cs[i]);
        if (cs[i].fd >= 0 && cs[i].kind == 2 && time(NULL) - cs[i].keepalive >= HTTP_IDLE_SECONDS) {
          fprintf(stderr, "HTTP IDLE CLOSE fd=%d\n", cs[i].fd);
          close_client( & cs[i]);
        }
        if (cs[i].fd >= 0 && cs[i].kind == 3 && time(NULL) - cs[i].keepalive >= STREAM_HEARTBEAT_SECONDS) stream_heartbeat( & cs[i]);
      }

  }
  for (i = 0; i < CLIENTS; i++) close_client( & cs[i]);
  close(udp);
  close(tcp);
  close(http);
  np_destroy(nowplaying);
  return 0;
}
