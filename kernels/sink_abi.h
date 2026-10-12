/* aura-kernel-sink kernel ABI.
 *
 * Every kernel .so exports `aot_emit_version` so that `(aot:reload path 0)`
 * can probe it (aura_reload_aot_module reads this symbol; version 0 means
 * "unversioned baseline", accepted when the host passes version 0).
 *
 * Kernels use only the c-func scalar ABI: Int = C long (int64), Float = double.
 * Results must stay inside the Aura fixnum range (|x| < 2^62).
 */
#ifndef AURA_SINK_ABI_H
#define AURA_SINK_ABI_H
#include <stdint.h>

#ifndef AURA_SINK_EMIT_VERSION
#define AURA_SINK_EMIT_VERSION 0
#endif

#define AURA_SINK_MODULE \
  __attribute__((visibility("default"))) const uint64_t aot_emit_version = AURA_SINK_EMIT_VERSION;

#define AURA_SINK_EXPORT __attribute__((visibility("default")))
#endif
