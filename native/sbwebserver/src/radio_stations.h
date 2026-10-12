#ifndef SB_RADIO_STATIONS_H
#define SB_RADIO_STATIONS_H
#include "mongoose.h"
#include <stddef.h>

/* 1 installed, 0 not installed, -1 detection unavailable. Cached briefly. */
int radio_applet_status(void);
void radio_applet_cache_invalidate(void);
int radio_is_api(struct mg_str uri);
/* Handles an authenticated /api/radio request. Returns non-zero if handled. */
int radio_handle(struct mg_connection *c, struct mg_http_message *m);

#endif
