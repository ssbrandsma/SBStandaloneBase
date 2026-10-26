/* Authenticated, allowlisted configuration backup/restore; no system images. */
#ifndef SB_BACKUP_H
#define SB_BACKUP_H
#include "mongoose.h"
#define SB_BACKUP_LIMIT (768u * 1024u)
int backup_handle(struct mg_connection *c,
		  struct mg_http_message *m,
		  int readonly);
int backup_headers(struct mg_connection *c, struct mg_http_message *m);
int backup_busy(void);
int backup_apply(const char *directory, int restart_jive);
#endif
