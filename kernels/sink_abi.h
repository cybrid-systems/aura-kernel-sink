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

/* Value encoding: single source of truth is Aura's lib/aura_value_abi.h
 * (aura#4502). kernels/build.sh passes -I"$AURA_LIB"; nothing below re-states
 * an encoding constant. Kernels only cross c-func as scalars (C long / double),
 * so the one thing they need from the encoding is the fixnum range. */
#if defined(__has_include)
#if __has_include("aura_value_abi.h")
#include "aura_value_abi.h"
#else
#error "aura_value_abi.h not found: build with -I<aura checkout>/lib (AURA_LIB, see scripts/env.sh)"
#endif
#else
#include "aura_value_abi.h"
#endif
#if AURA_VALUE_ABI < 4
#error "aura-kernel-sink needs Aura value ABI >= 4 (aura#4502, symmetric fixnums + ImmKind float/string)"
#endif
/* What this sink was written and tested against. Exported by every kernel
 * (aura_sink_value_abi) and checked by plant:real before any mutation, so a
 * stale .so built against another header is rejected at the load stage. */
#ifndef AURA_SINK_VALUE_ABI /* overridable only to build the stale-ABI test kernel */
#define AURA_SINK_VALUE_ABI AURA_VALUE_ABI
#endif

#ifndef AURA_SINK_EMIT_VERSION
#define AURA_SINK_EMIT_VERSION 0
#endif

/* aura_sink_f64ret_probe: a double-returning function the sink calls as
 * "(Int) -> Float" at bind time. (probe 3) = 1.75 means c-func reads a Float
 * result from xmm0 (the C ABI); anything else means it still reads rax
 * (aura#4531) and Float-returning vector kernels are bound through their
 * <sym>_bits twin, which returns the double's bit pattern as a long. */
#define AURA_SINK_MODULE \
  __attribute__((visibility("default"))) const uint64_t aot_emit_version = AURA_SINK_EMIT_VERSION; \
  __attribute__((visibility("default"))) long aura_sink_value_abi(void) { return AURA_SINK_VALUE_ABI; } \
  __attribute__((visibility("default"))) double aura_sink_f64ret_probe(long a) { return (double)a * 0.5 + 0.25; }

#define AURA_SINK_EXPORT __attribute__((visibility("default")))

#define AURA_SINK_FIX_MAX ((int64_t)AURA_FIXNUM_MAX)
#define AURA_SINK_FIX_MIN ((int64_t)AURA_FIXNUM_MIN)
_Static_assert(AURA_SINK_FIX_MAX == (int64_t)(((uint64_t)1 << 62) - 1), "fixnum max moved: re-check sink_ck_*");

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

/* Remainder / modulo (aura Soft `remainder` / `modulo` on Int).
 * remainder: truncating, sign of the dividend (C99 `%`): (remainder -7 2) = -1.
 * modulo: R7RS floor modulo, sign of the divisor: (modulo 7 -2) = -1. Aura
 * Soft computes this since aura#4486 was fixed (older tips returned a
 * non-negative r + |b|; build with -DAURA_SINK_MODULO_FLOOR=0 to match one).
 * Divisor checked BEFORE `%`: b == 0 sets the div0 bit (2) and returns 0 with
 * no `%` executed. b == -1: the result is always 0 (no `%` executed, so
 * INT64_MIN % -1, UB in C, never runs; Soft also returns 0 for -2^62 % -1).
 * An operand outside the fixnum range (only reachable from a non-Aura
 * caller) sets the overflow bit (1). |r| < |b|, so results stay in range. */
/* aura#4486 is fixed upstream (36bc90f): Aura modulo is R7RS floor modulo, so
 * floor is the default. -DAURA_SINK_MODULO_FLOOR=0 restores the old
 * non-negative behaviour for pre-fix Aura builds. */
#ifndef AURA_SINK_MODULO_FLOOR
#define AURA_SINK_MODULO_FLOOR 1
#endif
static inline int sink_div_pre_(int64_t a, int64_t b, int *ovf) {
  if (b == 0) { *ovf |= 2; return 0; }
  if (a < AURA_SINK_FIX_MIN || a > AURA_SINK_FIX_MAX || b < AURA_SINK_FIX_MIN || b > AURA_SINK_FIX_MAX) { *ovf |= 1; return 0; }
  return 1;
}
static inline int64_t sink_ck_rem(int64_t a, int64_t b, int *ovf) {
  if (!sink_div_pre_(a, b, ovf)) return 0;
  if (b == -1) return 0;
  return a % b;
}
static inline int64_t sink_ck_mod(int64_t a, int64_t b, int *ovf) {
  if (!sink_div_pre_(a, b, ovf)) return 0;
  if (b == -1) return 0;
  int64_t r = a % b;
#if AURA_SINK_MODULO_FLOOR
  if (r != 0 && ((r < 0) != (b < 0))) r += b;
#else
  if (r < 0) r += (b < 0 ? -b : b);
#endif
  return r;
}

/* Return guard. Since aura#4482 Aura fixnums are the symmetric [-2^62, 2^62-1]
 * (AURA_FIXNUM_MIN/MAX), so every in-range kernel result crosses c-func as an
 * exact Int. The guard is just the overflow flag. */
static inline int sink_ck_ret(int64_t r, int ovf) { (void)r; return ovf; }

/* ---- f64vector kernels (lib/vkgen.aura) --------------------------------
 * An f64vector crosses c-func as a String arg: a pointer to a COPY of the
 * slot's 8*n little-endian doubles (read-only, aura#4531). Lengths and Float
 * scalars come in one packed aux f64vector built by the Aura wrapper:
 *   aux[0..nv-1] = lengths of the vector params, aux[nv..] = Float params.
 * Status bits (beyond 1 overflow / 2 div0): 4 index out of range. Every
 * kernel stops at the first set bit (fail closed). */
#include <string.h>
static inline double sink_aux(const char *aux, int k) {
  double d; memcpy(&d, aux + 8 * (long)k, 8); return d;
}
static inline double sink_vref(const char *p, int64_t n, int64_t i, int *ovf) {
  if (i < 0 || i >= n) { *ovf |= 4; return 0.0; }
  double d; memcpy(&d, p + 8 * i, 8); return d;
}
/* (string-ref s i) of an "S" (byte string) parameter: the byte's Int code,
 * unsigned; out of range sets status bit 4 like sink_vref. */
static inline int64_t sink_sref(const char *p, int64_t n, int64_t i, int *ovf) {
  if (i < 0 || i >= n) { *ovf |= 4; return 0; }
  return (int64_t)(unsigned char)p[i];
}
/* (sqrt x) of a negative Float is nan in Aura Soft, the same as C. */
static inline double sink_vsqrt(double x) { return __builtin_sqrt(x); }
static inline int64_t sink_ck_abs(int64_t a, int *ovf) { return a < 0 ? sink_ck_neg(a, ovf) : a; }
static inline int64_t sink_imax(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t sink_imin(int64_t a, int64_t b) { return a < b ? a : b; }
static inline double sink_fmax(double a, double b) { return a > b ? a : b; }
static inline double sink_fmin(double a, double b) { return a < b ? a : b; }
static inline long sink_f64_bits(double d) { long r; memcpy(&r, &d, 8); return r; }

/* Per-kernel status (thread-local; read-and-clear accessor): 0 ok, bit 1 = overflow, bit 2 = div0. */
#define AURA_SINK_OVF_DECL(sym) static _Thread_local long sym##_ovf_;
#define AURA_SINK_OVF_STATUS(sym) \
  AURA_SINK_EXPORT long sym##_overflowed(void) { long v = sym##_ovf_; sym##_ovf_ = 0; return v; }
#endif
