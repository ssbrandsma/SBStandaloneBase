/* Volatile LMS sleep timer, driven by sbbase's existing event loop. */
#ifndef SB_SLEEP_H
#define SB_SLEEP_H
#include <stddef.h>
#include <stdint.h>
int sb_sleep_request(const char *json,
		     size_t length,
		     const char *player,
		     char *out,
		     size_t capacity,
		     uint64_t now);
int sb_sleep_tick(uint64_t now, int connected);
int sb_sleep_cancel(void);
int sb_sleep_status(char *json, size_t capacity, uint64_t now);
#define SB_SLEEP_MENU                                                          \
	"{\"id\":\"settingsSleep\",\"node\":\"settings\",\"text\":\"Sleep\","  \
	"\"weight\":65,\"actions\":{\"go\":{\"cmd\":[\"sleepsettings\"],"      \
	"\"player\":0}}}"
#endif
