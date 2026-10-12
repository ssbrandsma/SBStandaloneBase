#ifndef SB_AUTH_H
#define SB_AUTH_H
#include "mongoose.h"
#include <time.h>

void auth_init(void);
void auth_free(void);
int auth_is_endpoint(struct mg_str uri);
int auth_handle(struct mg_connection *c, struct mg_http_message *m, int internal_ok);
int auth_session_ok(struct mg_http_message *m, time_t *expires);
int auth_origin_ok(struct mg_http_message *m);
int auth_pairing_protected(void);
void auth_revoke_all(void);

#endif
