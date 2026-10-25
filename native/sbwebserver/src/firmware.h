/* Firmware preparation/permission HTTP integration; never installs firmware. */
#ifndef SB_FIRMWARE_H
#define SB_FIRMWARE_H
#include "mongoose.h"
void firmware_init(int port);
void firmware_tick(void);
/* Return nonzero when an upload event is owned by the firmware module. */
int firmware_upload_event(struct mg_connection *c,
			  int event,
			  void *data,
			  int readonly);
int firmware_remote_locked(void);
int firmware_handle(struct mg_connection *c,
		    struct mg_http_message *m,
		    int internal,
		    int readonly);
#endif
