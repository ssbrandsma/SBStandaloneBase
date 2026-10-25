#ifndef SB_IMAGE_RENDER_H
#define SB_IMAGE_RENDER_H
#include "mongoose.h"

int image_render_handle(struct mg_connection *c, struct mg_http_message *m);
int image_render_headers(struct mg_connection *c, struct mg_http_message *m);
void image_render_tick(struct mg_mgr *mgr);
/* Generic public transport shared with firmware's short-lived worker. */
int sb_fetch_public_url_valid(const char *url);
int sb_fetch_firmware_file(const char *url,
			   const char *path,
			   unsigned *received,
			   char sha1[41],
			   char sha256[65]);
#endif
