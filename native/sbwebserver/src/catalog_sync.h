#ifndef SB_CATALOG_SYNC_H
#define SB_CATALOG_SYNC_H
#include "mongoose.h"

void catalog_sync_tick(struct mg_mgr *mgr);
int catalog_sync_request(void);
void catalog_sync_status(char *out, size_t capacity);
#endif
