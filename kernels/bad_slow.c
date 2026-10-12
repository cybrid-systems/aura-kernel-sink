/* Deliberately SLOW kernel for `hot-sum-sq`: correct result, but sleeps 3 ms
 * per call, slower than the Aura body at the bench size. The loop must
 * reject it on speed and roll back. */
#define _POSIX_C_SOURCE 199309L
#include <time.h>
#include "sink_abi.h"
AURA_SINK_MODULE

AURA_SINK_EXPORT long sink_sum_sq(long n) {
  struct timespec ts = {0, 3 * 1000 * 1000};
  nanosleep(&ts, 0);
  long s = 0;
  for (long i = 0; i < n; i++) s += i * i;
  return s;
}
