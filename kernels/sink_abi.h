/* aura-kernel-sink kernel ABI.
 *
 * Every kernel .so exports `aot_emit_version` so that `(aot:reload path 0)`
 * can probe it (aura_reload_aot_module reads this symbol; version 0 means
 * "unversioned baseline", accepted when the host passes version 0).
 *
 * Kernels use only the c-func scalar ABI: Int = C long (int64), Float = double.
 * Results must stay inside the Aura fixnum range [-2^62, 2^62-1].
 *
 * Generated Int kernels (lib/kgen.aura) do every + - * through the checked
 * helpers below: the int64 op must not overflow (__builtin_*_overflow) AND the
 * result must stay in the fixnum range, else the kernel-local flag is set. On
 * overflow the kernel returns 0 and sets a thread-local status, exported as
 * `<symbol>_overflowed` (() -> Int, returns the status and clears it). The
 * Aura side (sink-plan:bind-body) checks it after every call and raises an
 * error, so an overflowed sentinel never reaches Aura code.
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

#define AURA_SINK_FIX_MAX ((int64_t)(((uint64_t)1 << 62) - 1))
#define AURA_SINK_FIX_MIN (-AURA_SINK_FIX_MAX - 1)

static inline int64_t sink_fix_(int64_t r, int wrapped, int *ovf) {
  if (wrapped || r > AURA_SINK_FIX_MAX || r < AURA_SINK_FIX_MIN) *ovf |= 1;
  return r;
}
static inline int64_t sink_ck_add(int64_t a, int64_t b, int *ovf) {
  int64_t r; int w = __builtin_add_overflow(a, b, &r); return sink_fix_(r, w, ovf);
}
static inline int64_t sink_ck_sub(int64_t a, int64_t b, int *ovf) {
  int64_t r; int w = __builtin_sub_overflow(a, b, &r); return sink_fix_(r, w, ovf);
}
static inline int64_t sink_ck_mul(int64_t a, int64_t b, int *ovf) {
  int64_t r; int w = __builtin_mul_overflow(a, b, &r); return sink_fix_(r, w, ovf);
}
static inline int64_t sink_ck_neg(int64_t a, int *ovf) {
  int64_t r; int w = __builtin_sub_overflow((int64_t)0, a, &r); return sink_fix_(r, w, ovf);
}

/* Truncating quotient (Aura `quotient` and Int `/`: (quotient -7 2) = -3,
 * same as C99 `/`). The divisor is checked BEFORE dividing: b == 0 sets the
 * div0 bit (2) and returns 0 without executing `/`; INT64_MIN / -1 (UB in C)
 * is never executed (b == -1 goes through checked negation). A fixnum
 * quotient can only leave the fixnum range as -2^62 / -1 = 2^62, which the
 * range check flags as overflow (1). */
static inline int64_t sink_ck_quo(int64_t a, int64_t b, int *ovf) {
  if (b == 0) { *ovf |= 2; return 0; }
  if (b == -1) return sink_ck_neg(a, ovf);
  return sink_fix_(a / b, 0, ovf);
}

/* Return guard. The pinned Aura tip cannot take a negative Int at or below
 * -5*10^15 back across c-func (observed boundary: -4999999999999999 works,
 * -5000000000000000 comes back corrupt; Aura literals and arithmetic there
 * turn Float, see IMPROVEMENTS.md). Kernel RESULTS < AURA_SINK_RET_MIN
 * therefore fail closed like an overflow; intermediates use the full fixnum
 * range. Build with -DAURA_SINK_RET_MIN=AURA_SINK_FIX_MIN once Aura is fixed. */
#ifndef AURA_SINK_RET_MIN
#define AURA_SINK_RET_MIN INT64_C(-4999999999999999)
#endif
static inline int sink_ck_ret(int64_t r, int ovf) { return ovf || r < AURA_SINK_RET_MIN; }

/* Per-kernel status (thread-local; read-and-clear accessor): 0 ok, bit 1 = overflow, bit 2 = div0. */
#define AURA_SINK_OVF_DECL(sym) static _Thread_local long sym##_ovf_;
#define AURA_SINK_OVF_STATUS(sym) \
  AURA_SINK_EXPORT long sym##_overflowed(void) { long v = sym##_ovf_; sym##_ovf_ = 0; return v; }
#endif
