/* Regression coverage for old-kernel clock rejection in Mongoose timers.
 * Linker wrappers exercise the actual vendored mg_millis implementation.
 */
#include "mongoose.h"
#include <assert.h>
#include <errno.h>

static int mode, raw_calls, mono_calls, wall_calls, fired;
static uint64_t mono_ms = 123450, raw_ms = 987650, wall_ms = 5000000;

/* Simulate a new kernel, an old kernel, or no monotonic clock support.
 * id is the requested kernel clock; ts receives the selected test time.
 */
int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
    uint64_t ms;
    if (id == CLOCK_MONOTONIC_RAW) {
        raw_calls++;
        if (mode != 0) { errno = EINVAL; return -1; }
        ms = raw_ms;
    } else if (id == CLOCK_MONOTONIC) {
        mono_calls++;
        if (mode == 2) { errno = EINVAL; return -1; }
        ms = mono_ms;
    } else {
        errno = EINVAL;
        return -1;
    }
    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)(ms % 1000) * 1000000;
    return 0;
}

/* Supply a wall clock only when both monotonic clocks are unavailable. */
int __wrap_gettimeofday(struct timeval *tv, void *zone)
{
    (void)zone;
    wall_calls++;
    tv->tv_sec = (time_t)(wall_ms / 1000);
    tv->tv_usec = (long)(wall_ms % 1000) * 1000;
    return 0;
}

/* Count real Mongoose timer callbacks; arg is unused. */
static void timer_fired(void *arg)
{
    (void)arg;
    fired++;
}

/* Verify advancing fallback time, deadlines, NTP isolation and normal clocks. */
int main(void)
{
    struct mg_timer timer, *head = NULL;
    uint64_t deadline;

    mode = 1;  /* Linux 2.6.26: RAW returns EINVAL, MONOTONIC works. */
    assert(mg_millis() == mono_ms);
    assert(raw_calls == 1 && mono_calls == 1 && wall_calls == 0);
    deadline = mg_millis() + 10000;
    mono_ms += 9999;
    assert(mg_millis() < deadline);
    mono_ms++;
    assert(mg_millis() == deadline);
    wall_ms = 1;  /* A clock correction must not reset a monotonic deadline. */
    assert(mg_millis() == deadline && wall_calls == 0);

    mg_timer_init(&head, &timer, 100, 0, timer_fired, NULL);
    mg_timer_poll(&head, mg_millis());
    mono_ms += 99;
    mg_timer_poll(&head, mg_millis());
    assert(fired == 0);
    mono_ms++;
    mg_timer_poll(&head, mg_millis());
    assert(fired == 1);
    mg_timer_free(&head, &timer);

    mode = 0;
    mono_calls = 0;
    assert(mg_millis() == raw_ms && mono_calls == 0);
    mode = 2;
    wall_ms = 1234567;
    assert(mg_millis() == wall_ms && wall_calls == 1);
    puts("mongoose-clock-tests-ok");
    return 0;
}
