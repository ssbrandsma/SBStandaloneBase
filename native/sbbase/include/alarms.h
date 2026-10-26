/* LMS-compatible alarm menus/state for sbbase; stock Jive provides RTC/UI. */
#ifndef SB_ALARMS_H
#define SB_ALARMS_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#define SB_ALARM_MAX 8
typedef struct sb_alarms sb_alarms;
/**
 * @brief Allocate and load the bounded alarm manager.
 * @param path Configuration filename; NULL/empty selects an ephemeral instance.
 * @return Owned manager, or NULL on allocation/path failure. Corrupt existing
 *         files produce a read-only manager and are never overwritten.
 */
sb_alarms *sb_alarms_open(const char *path);
/** @brief Free the manager without deleting configuration. @param alarms Owned
 * manager. */
void sb_alarms_close(sb_alarms *alarms);
int sb_alarms_active(const sb_alarms *alarms);
int sb_alarms_prepare_restore(const char *path);
/**
 * @brief Dispatch LMS alarm queries and configuration/control commands.
 * @param alarms Manager owned by sbbase's single event loop.
 * @param json One Comet message containing data.request.
 * @param length Number of input bytes.
 * @param player Actual local player's MAC, used to reject other players.
 * @param out Destination for the JSON query result or error.
 * @param capacity Destination size including terminator.
 * @param now Current wall-clock epoch seconds.
 * @return One for handled commands/errors; zero for unrelated requests.
 */
int sb_alarms_request(sb_alarms *alarms,
		      const char *json,
		      size_t length,
		      const char *player,
		      char *out,
		      size_t capacity,
		      time_t now);
/**
 * @brief Advance the scheduler without allocating a worker or polling audio.
 * @param alarms Manager owned by the calling event loop.
 * @param wall Current epoch seconds; calendar conversion uses the device
 * timezone.
 * @param monotonic Monotonic milliseconds, independent of wall-clock
 * adjustment.
 * @param connected Whether the actual local Radio has a live SlimProto session.
 * @return Nonzero when playerstatus should be republished.
 */
int sb_alarms_tick(sb_alarms *alarms,
		   time_t wall,
		   uint64_t monotonic,
		   int connected);
/**
 * @brief Append LMS alarm metadata/private audio events to a JSON object.
 * @param alarms Alarm manager.
 * @param json Existing, terminated playerstatus object; unchanged on failure.
 * @param capacity Total destination capacity including terminator.
 * @param now Current wall-clock seconds for lazy schedule initialization.
 * @return One on success; zero for invalid input or insufficient space.
 */
int sb_alarms_status(sb_alarms *alarms,
		     char *json,
		     size_t capacity,
		     time_t now);
/**
 * @brief Encode the server-owned Settings/Alarm Clock entry.
 * @param out JSON destination.
 * @param capacity Destination capacity.
 * @param player MAC for a menustatus notification, or "all" before discovery.
 * @param notification Nonzero selects notification-array rather than query
 * form.
 */
void sb_alarms_menu(char *out,
		    size_t capacity,
		    const char *player,
		    int notification);
#endif
