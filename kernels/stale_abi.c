/* Correct sum-of-squares kernel that claims value ABI 2 (built as if against
 * a pre-aura#4502 header). plant:real must refuse it at the load stage, before
 * any mutation: tests/test_loop_real.aura "stale-abi". */
#define AURA_SINK_VALUE_ABI 2
#include "sink_abi.h"
AURA_SINK_MODULE

AURA_SINK_EXPORT long sink_sum_sq(long n) {
  long s = 0;
  for (long i = 0; i < n; ++i) s += i * i;
  return s;
}
