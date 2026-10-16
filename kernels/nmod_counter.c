/* Demo stateful native module for tests/test_nmod.aura: named counters
 * behind handles. -DCTR_VERSION=2 builds the second version (adds a step
 * multiplier of 1 = same semantics, a different implementation tag);
 * -DCTR_BUG=1 builds a wrong one (add drops every 3rd call) for the gate. */
#include "nmod.h"
#include <stdio.h>
#ifndef CTR_VERSION
#define CTR_VERSION 1
#endif
AURA_NMOD("counter", CTR_VERSION)
typedef struct { long v; long calls; } ctr;
NMOD_REGISTRY(ctrs, 64)
AURA_SINK_EXPORT long ctr_new(long start) { ctr *c = calloc(1, sizeof *c); if (!c) return -1; c->v = start; long h = NMOD_PUT(ctrs, c); if (h < 0) free(c); return h; }
AURA_SINK_EXPORT long ctr_free(long h) { ctr *c = NMOD_TAKE(ctrs, h); if (!c) return -1; free(c); return 0; }
AURA_SINK_EXPORT long ctr_add(long h, long n) {
  ctr *c = NMOD_GET(ctrs, h); if (!c) return -1;
  c->calls++;
#ifdef CTR_BUG
  if (c->calls % 3 == 0) return c->v;
#endif
  c->v += n; return c->v;
}
AURA_SINK_EXPORT long ctr_get(long h) { ctr *c = NMOD_GET(ctrs, h); return c ? c->v : -1; }
AURA_SINK_EXPORT long ctr_live(void) { return nmod_reg_count(ctrs, ctrs_max); }
/* export / import: the state as text, for a swap between versions */
AURA_SINK_EXPORT const char *ctr_export(long h) { ctr *c = NMOD_GET(ctrs, h); if (!c) return NULL; char *b = nmod_ret(64); snprintf(b, 64, "%ld", c->v); return b; }
AURA_SINK_EXPORT long ctr_import(const char *s, long n) { char t[64]; if (n <= 0 || n >= 63) return -1; memcpy(t, s, n); t[n] = 0; return ctr_new(strtol(t, NULL, 10)); }
