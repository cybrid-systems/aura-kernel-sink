/* aura-kernel-sink: stateful native modules (lib/nmod.aura).
 *
 * A c-func can't hold or mutate Aura state, so a stateful native module keeps
 * its state in C. Aura holds only opaque Int handles. This header gives a
 * module:
 *   - identity: aura_nmod_name() / aura_nmod_version(), checked by nmod:load
 *     (plus the kernel ABI symbols from sink_abi.h);
 *   - a handle registry: NMOD_REGISTRY(reg, MAX) holds void* objects. A
 *     handle is (generation << 16) | (slot + 1), so a stale handle (freed,
 *     reused slot, or a handle of ANOTHER version's .so) is refused (NULL),
 *     never dereferenced;
 *   - a return buffer: nmod_ret(n) gives a thread-local, growable buffer for
 *     String results (c-func copies it with strlen, so results must not
 *     contain NUL; NULL arrives in Aura as 0).
 * Every .so has its own statics, so two versions loaded side by side never
 * share state. A swap moves state through the module's own export/import
 * (or the files it persists), at a safe point the caller chooses.
 */
#ifndef AURA_NMOD_H
#define AURA_NMOD_H
#include "sink_abi.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AURA_NMOD(name, version) \
  AURA_SINK_MODULE \
  AURA_SINK_EXPORT const char *aura_nmod_name(void) { return name; } \
  AURA_SINK_EXPORT long aura_nmod_version(void) { return (long)(version); }

typedef struct { void *p; uint32_t gen; } nmod_slot;
#define NMOD_REGISTRY(reg, max) static nmod_slot reg[max]; enum { reg##_max = (max) };

static inline long nmod_reg_put(nmod_slot *r, int max, void *p) {
  for (int i = 0; i < max; i++)
    if (!r[i].p) { r[i].p = p; r[i].gen = (r[i].gen + 1) & 0x7fffffff; if (!r[i].gen) r[i].gen = 1;
                   return ((long)r[i].gen << 16) | (long)(i + 1); }
  return -1;
}
static inline void *nmod_reg_get(nmod_slot *r, int max, long h) {
  if (h <= 0) return NULL;
  long i = (h & 0xffff) - 1; uint32_t g = (uint32_t)(h >> 16);
  if (i < 0 || i >= max || !r[i].p || r[i].gen != g) return NULL;
  return r[i].p;
}
static inline void *nmod_reg_take(nmod_slot *r, int max, long h) {
  void *p = nmod_reg_get(r, max, h);
  if (p) r[(h & 0xffff) - 1].p = NULL;
  return p;
}
static inline long nmod_reg_count(nmod_slot *r, int max) { long n = 0; for (int i = 0; i < max; i++) if (r[i].p) n++; return n; }
#define NMOD_PUT(reg, p) nmod_reg_put(reg, reg##_max, (p))
#define NMOD_GET(reg, h) nmod_reg_get(reg, reg##_max, (h))
#define NMOD_TAKE(reg, h) nmod_reg_take(reg, reg##_max, (h))

/* thread-local growable String-result buffer */
static _Thread_local char *nmod_rbuf_;
static _Thread_local size_t nmod_rcap_;
static inline char *nmod_ret(size_t n) {
  if (n + 1 > nmod_rcap_) {
    size_t c = nmod_rcap_ ? nmod_rcap_ : 256;
    while (c < n + 1) c *= 2;
    char *b = (char *)realloc(nmod_rbuf_, c);
    if (!b) return NULL;
    nmod_rbuf_ = b; nmod_rcap_ = c;
  }
  return nmod_rbuf_;
}
#endif
