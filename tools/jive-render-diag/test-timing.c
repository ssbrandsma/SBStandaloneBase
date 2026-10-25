/* Compile with -I pointing to the instrumented squeezeplay/src/ui directory. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static long wall_sec, cpu_ns;
static int clock_reads, fail_clock;
static int fake_clock_gettime(clockid_t id, struct timespec *t) {
 clock_reads++;
 if (fail_clock) return -1;
 t->tv_sec = id == CLOCK_MONOTONIC ? wall_sec : 0;
 t->tv_nsec = id == CLOCK_MONOTONIC ? 0 : cpu_ns;
 return 0;
}
#define clock_gettime fake_clock_gettime
#include "jive_render_diag.c"

int main(int argc, char **argv) {
 struct jive_diag_stamp s;
 int reads;
 assert(argc == 2);
 if (strcmp(argv[1], "disabled") == 0) {
  unsetenv("JIVE_RENDER_DIAG");
  s = jive_diag_begin();
  jive_diag_end(JIVE_DIAG_DRAW, s);
  jive_diag_frame();
  assert(!s.valid && !clock_reads);
 } else if (strcmp(argv[1], "clock-failure") == 0) {
  setenv("JIVE_RENDER_DIAG", "1", 1);
  fail_clock = 1;
  s = jive_diag_begin();
  assert(!s.valid && enabled == 0);
  reads = clock_reads;
  jive_diag_frame();
  assert(clock_reads == reads);
 } else {
  setenv("JIVE_RENDER_DIAG", "1", 1);
  s = jive_diag_begin();
  wall_sec = 1;
  cpu_ns = 2000000;
  jive_diag_end(JIVE_DIAG_DRAW, s);
  assert(totals[JIVE_DIAG_DRAW].calls == 1);
  assert(totals[JIVE_DIAG_DRAW].wall == 1.0);
  assert(totals[JIVE_DIAG_DRAW].cpu > .00199);
  jive_diag_frame();
  assert(frames == 1 && totals[JIVE_DIAG_DRAW].calls == 1);
  wall_sec = 5;
  jive_diag_frame();
  assert(frames == 0 && totals[JIVE_DIAG_DRAW].calls == 0);
  assert(last_report == 5.0);
  wall_sec = 120;
  jive_diag_frame();
  assert(enabled == 0);
  reads = clock_reads;
  s = jive_diag_begin();
  jive_diag_end(JIVE_DIAG_DRAW, s);
  jive_diag_frame();
  assert(!s.valid && reads == clock_reads);
 }
 puts("PASS");
 return 0;
}
