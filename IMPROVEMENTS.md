# Improvements and findings

Pinned Aura tip: `d89d39c56` (RelWithDebInfo, Soft face).

## Aura bugs filed from this repo

Each has a minimal repro in `probes/`.

| issue | what | probe | hit in this repo |
|---|---|---|---|
| [#4475](https://github.com/cybrid-systems/aura/issues/4475) | float literals in `set-code` workspace procedure bodies evaluate to `0` | `probes/float_literal_in_workspace_body.aura` | `sink-policy` floor `0.05` became `0`; a trivial function was selected for sinking |
| [#4476](https://github.com/cybrid-systems/aura/issues/4476) | two-op `mutate:atomic-batch` + `ast:restore` in one top-level form silently re-runs the whole file; `(define x (mutate:atomic-batch …))` leaves `x` unbound | `probes/atomic_batch_rerun.aura`, `probes/atomic_batch_define_unbound.aura` | rejected attempts re-ran every earlier mutation; attempt results were unbound |
| [#4477](https://github.com/cybrid-systems/aura/issues/4477) | a user define shadowing a primitive (`check`) reverts to the primitive after `eval-current` | `probes/primitive_shadow_after_eval_current.aura` | the PASS/FAIL helper turned into a silent no-op (`0 passed, 0 failed`) |
| [#4478](https://github.com/cybrid-systems/aura/issues/4478) | runtime `cannot call` re-runs earlier forms (residual of #4355); `(5 5)` / `(z 5)` return a value silently | `probes/cannot_call_rerun.aura`, `probes/apply_non_procedure.aura` | a kernel slot holding `0` (failed `c-func`) re-runs the file or yields `0` |

- #4481 closures (a plant built by `plant:real`) passed inline as an argument to a top-level form that runs `hot-strategy:register!` + `mutate:rebind` + `eval-current` become `cannot call: ''` and the file silently re-runs; bound to a top-level define first it works. Probe: `probes/inline_plant_cannot_call.aura`. Hit by the first draft of `tests/test_kgen.aura`.
- **Fixed (aura#4482, value ABI 2; RET_MIN guard removed):** #4482 negative Int at or below about -5·10^15 is Float (literals, arithmetic results) and comes back corrupt from a `c-func ... -> Int` (prints `0`/`<unknown>`, `integer?` is #f, `=` raises `expected Int, got String ''`); the positive side is fine up to 2^62-1. Hit by `tests/test_kgen_overflow.aura`; the `AURA_SINK_RET_MIN` fail-closed guard is gone (defaults to the fixnum minimum).
- #4483 a `c-func ... -> Int` returning a long outside the fixnum range aborts the process (SIGABRT, `try` cannot catch it). This is why overflow is signalled through a separate `<symbol>_overflowed` status, not an out-of-range sentinel.
- #4485 `string->number` on integer strings goes through double: it rounds above 2^53 (`"9007199254740993"` → ...992) and SIGABRTs, uncatchable, at `"4611686018427387903"`. It crashed kernel generation on a define containing 2^62-1. Workaround: `kg-int-literal?` recognises integer literals by their characters (≤ 2^62-1) and emits them verbatim.
- #4486 FIXED upstream (re-pin 72b452f75): `modulo` is floor modulo; kernels default to `AURA_SINK_MODULO_FLOOR=1`. Was: `modulo` with a negative divisor returns a non-negative (Euclidean) result: `(modulo 7 -2)` = 1, `(modulo -7 -2)` = 1, `(modulo 9 -4)` = 1, where floor modulo (the primitive's own docstring, and R7RS) gives -1, -1, -3. Cause: `r = n % d; if (r < 0) r += |d|` in `evaluator_primitives_math.cpp`. Hit by `tests/test_kgen_remmod.aura`; generated kernels reproduce Soft's result (`AURA_SINK_MODULO_FLOOR=0`) so a sunk define never changes answers; flip the macro when #4486 is fixed.
- #4480 FIXED upstream (re-pin 72b452f75): lib/discover.aura builds rows with `map` again. Was: `map` corrupts earlier results when the mapped procedure calls another procedure (regression in b13af9334..94ca96362, still on d89d39c56). Workaround: lib/discover.aura builds rows with plain recursion.

## Aura API gaps (not filed as bugs)

- No define/node accessor bound in file mode: `query:defines` returns node ids but `query:node` / `query:children` / `ast:defs` are unbound, so discovery parses the `(query:code)` string itself. No `procedure-arity` / `string->symbol`; `(eval "name")` is used to resolve a define.
- `(eval "nosuch")` error says `did you forget to define ` (name lost).
- A probe closure over an `(eval name)` value called in a calibration loop inside a library failed with `invalid closure: eval_flat` on d89d39c56 (not minimized, not filed); discover calls the procedure directly instead.


1. **No per-define runtime call counter.** No `query:*` / `engine:metrics` key
   on `94ca96362` reports invocations per define; `query:calls` is static call
   sites. Interim: `lib/counter.aura` hot-swaps candidates into counting
   wrappers with `mutate:rebind` and keeps counts in a workspace vector
   (`sink-calls`). Costs: one extra call + a vector bump per invocation, only
   candidates are counted, counts reset on every `eval-current`, and the
   wrappers use workspace procedure slots (see #4479). A native
   `query:define-call-stats` (calls, total ns per define, sampled) would replace
   all of that and cover functions nobody thought to instrument.
2. **`c-load` returns `0` on failure, and `0` is also the first handle.** Aura
   treats `0` as false, so `(if lib ...)` rejects the first good handle, and a
   failed `c-load` followed by `c-func 0 ...` can bind a symbol from a different
   library. `plant.aura` gates on `aot:reload` first (it fails on dlopen
   errors), probes `c-func` with `procedure?`, and boxes the handle as
   `(list lib)`. A `-1` / error value on failure would remove the ambiguity.
3. **`c-func` returns `0` on a missing symbol or bad signature** instead of an
   error value. A slot bound to `0` then either returns `0` silently at top
   level or triggers a file re-run (see the cannot-call issue).
4. **`aot:reload` accepts a plain C `.so`.** It has no Aura func-table
   registrations, but the reload still commits a table swap and bumps the epoch.
   The loop uses it as a gate (version/region/defuse probe + proof stamp). A
   native-kernel registration ABI (constructor registers `name -> fnptr` with a
   signature) would let `aot:reload` bind the kernel directly instead of a
   second `dlopen` through `c-load`.
5. **`ast:rollback` does not exist.** Rollback is `ast:restore <snapshot>` +
   `eval-current`, wrapped by `hot-strategy:heal!`.
6. **`hot-strategy` tracks one active name.** Sinking several functions at
   once needs per-name last-good records, or a snapshot per attempt (what the
   loop does now through `register!`).
7. **`list-sort` takes only a list of ints** and returns `()` when given a
   comparator first. `identify.aura` uses its own insertion sort.
8. **Default face.** On this tip the std prelude does not load on the default
   Restricted face, and workspace calls after `set-code` + `eval-current` hit
   "tree-walker fallback forbidden". The MVP therefore runs on the Soft face
   only. `std/ffi` additionally needs `effect:ffi` under a sandbox.

- No `real?` predicate (unbound); `float?` / `integer?` exist. discover uses `float?` for the Float kind.
- No per-call argument type feedback: the sink records the first argument of the last counted call itself (`sink-argf`, lib/counter.aura) to decide Int vs Float for polymorphic numeric defines. A native per-define arg-kind sample next to a call counter would replace it.

## Workarounds in this repo (remove when fixed upstream)

- tests/mvp: plants are bound to a top-level define before the attempt (#4481).
- kgen: no `map`, results threaded as `(ok? value)` lists (#4480).
- discover: ~~no `map` with calling procedures (#4480)~~ uses `map` since the re-pin; calibration calls `(eval name)` directly, not via a probe closure.

| workaround | where | upstream |
|---|---|---|
| workspace constants are integers (`(>= (* 1000 ms-per-call) 50)`) | `lib/fixture.aura` | #4475 |
| Float candidate `hot-fsum` has no float literal (`(- h h)` as 0.0, workload derives n as `(* h 200)`); literal Float kernel `fpoly` golden-checked against a host-file define | `lib/fixture.aura`, `tests/test_kgen_float.aura` | #4475 |
| two `mutate:rebind` calls instead of one `mutate:atomic-batch` | `lib/loop.aura` (`loop:set-atomic-batch!`) | #4476 |
| results stored with `set!`, not `(define x (attempt ...))` | `scripts/mvp.aura`, `tests/test_loop_*.aura` | #4476 |
| test helper named `expect`, not `check` | `lib/expect.aura` | #4477 |
| `RUN-ONCE` marker checked by `run_tests.sh` | `scripts/run_tests.sh` | #4476, #4478 |
| counter bump inlined in each wrapper (no `sink-tick!` helper), no `driver` define, one `app-workload` with a `hot?` flag: workspace kept at 8 procedure defines | `lib/counter.aura`, `lib/fixture.aura` | #4479 |
| counting wrapper calls a named `sink-impl-<name>`, not an inline `((lambda ...) a0)` (closure per call skews the baseline) | `lib/fixture.aura` | — |
| `c-func` probed with `procedure?` before rebinding; handle boxed | `lib/plant.aura` | API gaps 2–3 |

## Next steps

1. ~~Runtime call profile instead of the fixture column~~ done (interim wrappers, `lib/counter.aura`); switch to a native per-define counter when Aura has one (gap 1). Also: auto-discover candidates (every numeric define) instead of the fixed three, and keep a rolling window across `eval-current`.
2. ~~Kernel generator~~ done for the Int subset (`lib/kgen.aura`). Next: `let`
   bindings, ~~`quotient` with an explicit zero guard~~ (done: `sink_ck_quo`,
   divisor checked before `/`, div0 = status bit 2, MIN/-1 = overflow; Int `/` too),
   ~~`remainder`/`modulo`~~ (done: `sink_ck_rem`/`sink_ck_mod`, same div0 bit;
   modulo matches Soft's non-negative result until #4486, then build with
   `-DAURA_SINK_MODULO_FLOOR=1`), ~~checked arithmetic for the
   fixnum range instead of `-fwrapv`~~ (done: `sink_ck_*` + `<symbol>_overflowed`
   status, fail closed with an Aura error; drop the -5e15 return guard once
   #4482 is fixed), ~~Float bodies~~, and multi-loop/nested-loop coverage tests.
3. Float ABI kernels (`(Float Float) -> Float`), vector/opaque buffers through
   `ffi:pin-buffer`.
4. Turn `mutate:atomic-batch` back on once the re-run bug is fixed (one
   MutationBoundary for bind + swap).
5. Hot-swap the scoring weights and `sink-min-speedup` from an outer agent loop,
   and record accepted sinks as "stable data plane" in a manifest.
6. Restricted face: grant `effect:ffi`, check the AOT reload capability gate.
