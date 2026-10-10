#define _POSIX_C_SOURCE 200809L
#include "nowplaying.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int call(np_manager *m, const char *path, const char *json, int *changed,
                char *reply, size_t cap) {
  return np_api(m, "POST", path, json, strlen(json), reply, cap, changed);
}

int main(void) {
  np_manager *m = np_create(1, 8);
  char reply[1024], claim[256], token[NP_TOKEN_MAX];
  const char *start, *end;
  int changed;
  uint64_t revision;
  struct timespec delay = {1, 100000000};

  assert(m);
  assert(call(m, "/api/nowplaying/claim",
              "{\"source\":\"radio\",\"name\":\"Radio\"}", &changed,
              reply, sizeof reply) == 201);
  assert(changed == NP_CHANGE_OWNER);
  start = strstr(reply, "\"session_id\":\"") + 14;
  end = strchr(start, '"');
  assert(start && end && (size_t)(end - start) < sizeof token);
  memcpy(token, start, (size_t)(end - start));
  token[end - start] = 0;

  snprintf(claim, sizeof claim,
           "{\"session_id\":\"%s\",\"state\":\"playing\",\"live\":true,"
           "\"track\":{\"id\":\"a\",\"station\":\"A\",\"title\":\"old\"}}",
           token);
  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 200);
  assert((changed & NP_CHANGE_IDENTITY) != 0);
  revision = np_current(m)->playlist_revision;

  /* An exact repeat refreshes the lease but emits no visible-state change. */
  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 200);
  assert(changed == NP_CHANGE_NONE);
  assert(np_current(m)->playlist_revision == revision);

  snprintf(claim, sizeof claim,
           "{\"session_id\":\"%s\",\"track\":{\"id\":\"a\","
           "\"artwork_url\":\"http://127.0.0.1:8765/https/x/cover%%20a.jpg?q=1&x=2\"}}",
           token);
  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 200);
  assert((changed & NP_CHANGE_ARTWORK) != 0);
  assert((changed & NP_CHANGE_IDENTITY) == 0);
  assert(np_current(m)->playlist_revision == revision);
  assert(strstr(np_current(m)->track.artwork, "cover%20a.jpg?q=1&x=2"));

  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 200);
  assert(changed == NP_CHANGE_NONE);

  snprintf(claim, sizeof claim,
           "{\"session_id\":\"%s\",\"state\":\"buffering\",\"live\":true,"
           "\"track\":{\"id\":\"b\",\"station\":\"B\"}}", token);
  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 200);
  assert((changed & NP_CHANGE_IDENTITY) != 0);
  assert(np_current(m)->playlist_revision > revision);
  assert(np_current(m)->track.title[0] == 0);

  snprintf(claim, sizeof claim,
           "{\"session_id\":\"%s\",\"state\":\"playing\",\"live\":true,"
           "\"track\":{\"id\":\"b\",\"station\":\"B\"}}", token);
  revision = np_current(m)->playlist_revision;
  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 200);
  assert((changed & NP_CHANGE_PLAYBACK) != 0);
  assert((changed & NP_CHANGE_IDENTITY) == 0);
  assert(np_current(m)->playlist_revision == revision);

  nanosleep(&delay, NULL);
  assert(np_expire(m) == 1);
  assert(!np_current(m)->active);
  assert(call(m, "/api/nowplaying/update", claim, &changed, reply,
              sizeof reply) == 409);
  np_destroy(m);
  puts("nowplaying-unit-tests-ok");
  return 0;
}
