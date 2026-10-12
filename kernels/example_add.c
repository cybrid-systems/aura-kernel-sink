/* Trivial kernel for `cold-add`: (define (cold-add a b) (+ a b)).
 * Used to show that sinking a cheap function does not pay off: the FFI call
 * costs about as much as the Aura body, so score.aura rejects it.
 */
#include "sink_abi.h"
AURA_SINK_MODULE

AURA_SINK_EXPORT long sink_add(long a, long b) { return a + b; }
