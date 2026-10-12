/* Deliberately WRONG kernel for `hot-sum-sq` (off by one: starts at i=1 and
 * adds 1). The loop must reject it on correctness and roll back. */
#include "sink_abi.h"
AURA_SINK_MODULE

AURA_SINK_EXPORT long sink_sum_sq(long n) {
  long s = 1;
  for (long i = 1; i < n; i++) s += i * i;
  return s;
}
