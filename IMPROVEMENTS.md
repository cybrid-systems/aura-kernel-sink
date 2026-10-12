# Improvements and findings

Pinned Aura tip: `c4fc6cf23` (RelWithDebInfo, Soft face).

## Aura bugs filed from this repo

Each has a minimal repro in `probes/`.

| issue | what | probe | hit in this repo |
|---|---|---|---|
| [#4475](https://github.com/cybrid-systems/aura/issues/4475) | float literals in `set-code` workspace procedure bodies evaluate to `0` | `probes/float_literal_in_workspace_body.aura` | `sink-policy` floor `0.05` became `0`; a trivial function was selected for sinking |
| [#4476](https://github.com/cybrid-systems/aura/issues/4476) | two-op `mutate:atomic-batch` + `ast:restore` in one top-level form silently re-runs the whole file; `(define x (mutate:atomic-batch …))` leaves `x` unbound | `probes/atomic_batch_rerun.aura`, `probes/atomic_batch_define_unbound.aura` | rejected attempts re-ran every earlier mutation; attempt results were unbound |
| [#4477](https://github.com/cybrid-systems/aura/issues/4477) | a user define shadowing a primitive (`check`) reverts to the primitive after `eval-current` | `probes/primitive_shadow_after_eval_current.aura` | the PASS/FAIL helper turned into a silent no-op (`0 passed, 0 failed`) |
| [#4478](https://github.com/cybrid-systems/aura/issues/4478) | runtime `cannot call` re-runs earlier forms (residual of #4355); `(5 5)` / `(z 5)` return a value silently | `probes/cannot_call_rerun.aura`, `probes/apply_non_procedure.aura` | a kernel slot holding `0` (failed `c-func`) re-runs the file or yields `0` |

- #4481 closures (a plant built by `plant:real`) passed inline as an argument to a top-level form that runs `hot-strategy:register!` + `mutate:rebind` + `eval-current` become `cannot call: ''` and the file silently re-runs; bound to a top-level define first it works. Probe: `probes/inline_plant_cannot_call.aura`. Hit by the first draft of `tests/test_kgen.aura`.
- #4480 `map` corrupts earlier results when the mapped procedure calls another procedure (regression in b13af9334..94ca96362, still on d89d39c56). Workaround: lib/discover.aura builds rows with plain recursion.

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

## Workarounds in this repo (remove when fixed upstream)

- tests/mvp: plants are bound to a top-level define before the attempt (#4481).
- kgen: no `map`, results threaded as `(ok? value)` lists (#4480).
- discover: no `map` with calling procedures (#4480); calibration calls `(eval name)` directly, not via a probe closure.

| workaround | where | upstream |
|---|---|---|
| workspace constants are integers (`(>= (* 1000 ms-per-call) 50)`) | `lib/fixture.aura` | #4475 |
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
   bindings, `quotient`/`remainder` with an explicit zero guard that returns a
   sentinel the wrapper turns into the Aura error, checked arithmetic for the
   fixnum range (|x| < 2^62) instead of `-fwrapv`, Float bodies, and
   multi-loop/nested-loop coverage tests.
3. Float ABI kernels (`(Float Float) -> Float`), vector/opaque buffers through
   `ffi:pin-buffer`.
4. Turn `mutate:atomic-batch` back on once the re-run bug is fixed (one
   MutationBoundary for bind + swap).
5. Hot-swap the scoring weights and `sink-min-speedup` from an outer agent loop,
   and record accepted sinks as "stable data plane" in a manifest.
6. Restricted face: grant `effect:ffi`, check the AOT reload capability gate.
