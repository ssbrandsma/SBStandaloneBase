#ifndef SB_IMAGE_RENDER_H
#define SB_IMAGE_RENDER_H
#include "mongoose.h"

int image_render_handle(struct mg_connection *c, struct mg_http_message *m);
int image_render_headers(struct mg_connection *c, struct mg_http_message *m);
void image_render_tick(struct mg_mgr *mgr);
#endif
