#ifndef SB_REMOTE_H
#define SB_REMOTE_H
#include "mongoose.h"
void remote_init(const char *config_dir);
void remote_free(void);
void remote_tick(void);
int remote_handle(struct mg_connection *c,
		  struct mg_http_message *m,
		  int readonly);
int remote_internal_authorized(struct mg_connection *c,
			       struct mg_http_message *m);
#endif
