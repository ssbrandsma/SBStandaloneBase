#ifndef SBWEBSERVER_LOGS_H
#define SBWEBSERVER_LOGS_H

#include "mongoose.h"

int logs_is_api(struct mg_str uri);
int logs_handle(struct mg_connection *connection,
		struct mg_http_message *request);

#endif
