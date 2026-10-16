/* kernels/host_demo.c — hand-written kernels for tests/test_csink.aura
 * (policy:sink-host!): one correct (Adler-32 of s[start,end), refuses
 * non-ASCII / out of range with the 1<<62 sentinel), one wrong. */
#include <stdint.h>
#include "sink_abi.h"
AURA_SINK_MODULE
#define REFUSE ((long)((uint64_t)1 << 62))
AURA_SINK_EXPORT long host_adler(const char *s, long n, long start, long end) {
  for (long i = 0; i < n; i++) if (s[i] == 0 || (unsigned char)s[i] >= 128) return REFUSE;
  if (start < 0 || end > n) return REFUSE;
  uint64_t a = 1, b = 0;
  for (long i = start; i < end; i++) { a = (a + (unsigned char)s[i]) % 65521; b = (b + a) % 65521; }
  return (long)(b * 65536 + a);
}
AURA_SINK_EXPORT long host_wrong(const char *s, long n, long start, long end) {
  (void)s; (void)n; return end - start;
}
