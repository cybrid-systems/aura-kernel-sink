/* Correct kernel for the fixture hot path `hot-sum-sq`:
 *   (define (hot-sum-sq n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (* i i))) s)))
 */
#include "sink_abi.h"
AURA_SINK_MODULE

AURA_SINK_EXPORT long sink_sum_sq(long n) {
  long s = 0;
  for (long i = 0; i < n; i++) s += i * i;
  return s;
}
