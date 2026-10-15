#ifndef SBWEBSERVER_MONITOR_H
#define SBWEBSERVER_MONITOR_H

#include "mongoose.h"

/** Handle GET /api/monitor/snapshot when the URI matches. */
int monitor_handle(struct mg_connection *connection,
		   struct mg_http_message *request);

#endif
