# aura-kernel-sink

**中文** | [English](#english)

用 Aura 自己的热更新闭环把热点函数**自动下沉**到 C kernel：识别 → 生成计划 →
加载 `.so` → `mutate:rebind` 换成薄封装 → `eval-current` + 基准 + 多维评分 →
接受，或 `hot-strategy:heal!` 回滚。全程在**一个进程**里完成，不重启。

- **控制面**（纯 Aura，可热更新）：`sink-policy`（是否下沉）、`sink-min-speedup`（接受门槛）、
  封装函数体。都是 workspace define，用 `mutate:rebind` / `hot-strategy:swap!` 换。
- **数据面**（C kernel）：`kernels/*.c` → `build/kernels/lib*.so`，经
  `aot:reload`（AOT 热更新门：staging dlopen、`aot_emit_version` 探测、表 epoch bump）
  加 `c-load` / `c-func`（std/ffi）接进来。

MVP，钉在 Aura `c4fc6cf23`（RelWithDebInfo，Soft 面：`AURA_SANDBOX=off AURA_PIPELINE_STRICT=0`）。

### 闭环

```
identify.aura    热点识别：observed-calls（fixture 画像）× 实测 ms/call + query:calls 调用点
                 决策交给 workspace 里的 sink-policy（可热更新）
      ↓
sink_plan.aura   计划：slot sink-kernel-<name>、.so、符号、c-func 签名、封装体
      ↓
plant.aura       RealPlant: (aot:reload so 0) → (c-load so) → (c-func lib sym sig) 探测
                 FakePlant: 不碰 .so，用 Aura 替身体，控制面照样跑完
      ↓
loop.aura        hot-strategy:register!（last-good + ast:snapshot）
                 mutate:rebind slot ← (c-func lib "sym" "sig")
                 mutate:rebind name ← (lambda (a0) (sink-kernel-<name> a0))
                 eval-current
      ↓
score.aura       正确性（golden，异常算错）+ 确定性 + 加速比 ≥ (sink-min-speedup)
      ↓
接受 → hot-strategy:register! 封装体为新 last-good（之后的坏尝试回滚到「已下沉版」）
拒绝 → hot-strategy:heal!（ast:restore + eval-current）并复查 golden
```

### 运行

```bash
export AURA_BIN=~/aura/build-rel/aura      # Aura 构建（AURA_LIB 默认 <checkout>/lib）
scripts/run_mvp.sh                         # 端到端：真 .so（RealPlant）
SINK_PLANT=fake scripts/run_mvp.sh         # 同一闭环，Aura 替身数据面
scripts/run_tests.sh                       # 4 个测试文件，95 项检查
```

`run_mvp.sh` 在 `c4fc6cf23` 上的一次输出（节选；计时每次略有浮动）：

```
-- 1. identify (policy = workspace sink-policy)
  hot-sum-sq  sink=#t  score=515.333  calls=400  sites=2  ms/call=1.28333  numeric=#t
  cold-add  sink=#f  score=61  calls=5000  sites=1  ms/call=0.012  numeric=#t
  label-of  sink=#f  score=3.6  calls=300  sites=0  ms/call=0.012  numeric=#f
  selected: (hot-sum-sq)
-- 1b. hot-swap sink-policy (budget 50 ms -> 1e9 ms), re-identify, restore
  selected: ()
-- attempt hot-sum-sq / wrong      (libbad_wrong.so)
  [real] hot-sum-sq: reject reasons=(wrong-result) speedup=0 t-before=75ms t-after=0ms stage=score rolled-back=#t rollback-ok=#t
-- attempt hot-sum-sq / slow       (libbad_slow.so)
  [real] hot-sum-sq: reject reasons=(too-slow) speedup=0.388889 t-before=77ms t-after=198ms stage=score rolled-back=#t rollback-ok=#t
-- attempt hot-sum-sq / missing    (no .so: nothing mutated)
  [real] hot-sum-sq: reject reasons=(load-failed) speedup=0 t-before=79ms t-after=0ms stage=load rolled-back=#f rollback-ok=#t
-- attempt cold-add / good         (libexample_add.so: FFI call costs as much as the body)
  [real] cold-add: reject reasons=(too-slow) speedup=0.609756 t-before=25ms t-after=41ms stage=score rolled-back=#t rollback-ok=#t
-- attempt hot-sum-sq / good       (libsum_sq.so)
  [real] hot-sum-sq: accept reasons=() speedup=38.5 t-before=77ms t-after=2ms stage=score rolled-back=#f rollback-ok=#t
-- 4. final state
  (hot-sum-sq 100) = 328350
  hot-strategy version = 6, active = hot-sum-sq
checks: 7 passed, 0 failed
```

### 什么是真的，什么是替身

| 步骤 | 状态 |
|---|---|
| 识别：实测 ms/call、`query:calls` 调用点、workspace 策略决策、策略热替换 | **真跑** |
| 调用次数画像 | **fixture**（Aura 还没有按 define 的运行时调用计数，见 IMPROVEMENTS） |
| 生成 C kernel | **手写**（`kernels/*.c`；生成器是后续项） |
| 编译 `.so` | **真跑**（`kernels/build.sh`，`cc -O2 -shared -fPIC`） |
| `aot:reload` 加载 | **真跑**，Soft 面返回 `#t`、走 staging + epoch bump；但 kernel 不是 Aura AOT 模块，调用路径走 `c-func` |
| `c-load` + `c-func` 绑定 | **真跑** |
| `mutate:rebind` 换封装 + `eval-current` | **真跑** |
| 评分 / 接受 / `hot-strategy:heal!` 回滚 + golden 复查 | **真跑** |
| `mutate:atomic-batch` 一次边界内 bind+swap | **已写好，默认关闭**：上游 bug（见下），用两次 `mutate:rebind` 代替 |
| FakePlant | 同一闭环、Aura 替身体（闭式公式/错一位/慢 4 倍），任何构建都能跑 |

### 已知上游问题（Aura，均已在本仓库 `probes/` 里给出最小复现）

- [#4475](https://github.com/cybrid-systems/aura/issues/4475)：workspace 过程体里的浮点字面量变成 `0`（策略阈值 `0.05` 失效）→ 本仓库 workspace 常量只用整数。
- [#4476](https://github.com/cybrid-systems/aura/issues/4476)：两步 `mutate:atomic-batch` + `ast:restore` 在一个顶层 form 里会让整个文件静默重跑；`(define x (mutate:atomic-batch …))` 后 `x` 未绑定 → 默认用两次 `mutate:rebind`，结果用 `set!` 存。
- [#4477](https://github.com/cybrid-systems/aura/issues/4477)：与原语同名的用户 define（`check`）在 `eval-current` 后被原语顶掉 → 测试助手改名 `expect`。
- [#4478](https://github.com/cybrid-systems/aura/issues/4478)：顶层 `cannot call` 会重跑前面的 form（#4355 残留）；`(5 5)` 静默返回 → `c-func` 绑定前先 `procedure?` 探测。

更多 API 缺口和后续计划见 [IMPROVEMENTS.md](IMPROVEMENTS.md)。

---

## English

**aura-kernel-sink** sinks a hot Aura function into a C kernel through Aura's own
hot-update loop: identify → plan → load the `.so` → `mutate:rebind` the define
to a thin wrapper → `eval-current` + bench + multi-dimensional score → accept,
or roll back with `hot-strategy:heal!`. Everything runs in **one process** with
no restart.

- **Control plane** (pure Aura, hot-updatable): `sink-policy` (whether to sink),
  `sink-min-speedup` (acceptance bar), and the wrapper bodies. They are workspace
  defines, swapped with `mutate:rebind` / `hot-strategy:swap!`.
- **Data plane** (C kernel): `kernels/*.c` → `build/kernels/lib*.so`, brought in
  through `aot:reload` (AOT hot-update gate: staging dlopen,
  `aot_emit_version` probe, table epoch bump) plus `c-load` / `c-func` (std/ffi).

MVP, pinned to Aura `c4fc6cf23` (RelWithDebInfo, Soft face:
`AURA_SANDBOX=off AURA_PIPELINE_STRICT=0`).

### Layout

| path | role |
|---|---|
| `lib/identify.aura` | hotspot signals (observed calls × measured ms/call, `query:calls` sites); decision delegated to the workspace `sink-policy` |
| `lib/sink_plan.aura` | plan: kernel slot, `.so`, symbol, `c-func` signature, wrapper body, documented primitive sequence |
| `lib/plant.aura` | RealPlant (`aot:reload` + `c-load` + `c-func` probe) and FakePlant (Aura stand-in bodies) |
| `lib/score.aura` | correctness (golden; errors count as wrong), determinism, speedup ≥ min |
| `lib/loop.aura` | one attempt: register! → load → rebind slot + wrapper → eval-current → score → accept / heal! + golden re-check |
| `lib/fixture.aura` | toy workload: `hot-sum-sq` (hot loop), `cold-add` (trivial), `label-of` (strings) + policy |
| `lib/harness.aura`, `lib/expect.aura` | shared attempt runner, PASS/FAIL helper |
| `kernels/` | `sum_sq.c` (good), `example_add.c` (trivial), `bad_wrong.c`, `bad_slow.c`, `build.sh` |
| `scripts/` | `env.sh`, `run_mvp.sh`, `run_tests.sh`, `mvp.aura` |
| `tests/` | `test_identify`, `test_score`, `test_loop_real`, `test_loop_fake` (95 checks) |
| `probes/` | minimal repros of the Aura bugs found while building this |

### Run

```bash
export AURA_BIN=~/aura/build-rel/aura      # an Aura build; AURA_LIB defaults to <checkout>/lib
scripts/run_mvp.sh                         # end to end with real .so files (RealPlant)
SINK_PLANT=fake scripts/run_mvp.sh         # same loop, Aura stand-in data plane
scripts/run_tests.sh                       # 4 test files, 95 checks
```

The runner fails a test file on a FAIL line, a crash, a non-zero exit, an error
line, zero PASS lines, or a `RUN-ONCE` marker that is not printed exactly once
(the Soft file runner can silently re-run a whole file, which would repeat every
mutation).

### What runs for real vs stubbed

| step | status |
|---|---|
| identify: measured ms/call, `query:calls` call sites, workspace-policy decision, policy hot-swap | **real** |
| per-function call counts | **fixture** (Aura has no per-define runtime call counter yet) |
| C kernel generation | **hand-written** (`kernels/*.c`; generator is future work) |
| build `.so` | **real** (`kernels/build.sh`) |
| `aot:reload` | **real**; returns `#t` on Soft with staging + epoch bump. The kernel is not an Aura AOT module, so calls go through `c-func` |
| `c-load` + `c-func` binding | **real** |
| `mutate:rebind` wrapper + `eval-current` | **real** |
| score / accept / `hot-strategy:heal!` rollback + golden re-check | **real** |
| bind + swap in one `mutate:atomic-batch` | **implemented, off by default** (upstream bug); two `mutate:rebind` calls instead |
| FakePlant | same loop with Aura stand-in bodies (closed form / off by one / 4× slower) |

The default (Restricted) face is not supported yet: on `c4fc6cf23` the std
prelude does not load there ("tree-walker fallback forbidden"), and
`set-code` + `eval-current` workspace calls hit the same gate.

Aura issues filed from this repo: [#4475](https://github.com/cybrid-systems/aura/issues/4475)
(float literals in workspace bodies → 0), [#4476](https://github.com/cybrid-systems/aura/issues/4476)
(atomic-batch: silent whole-file re-run, define unbound), [#4477](https://github.com/cybrid-systems/aura/issues/4477)
(primitive shadowing lost after eval-current), [#4478](https://github.com/cybrid-systems/aura/issues/4478)
(cannot-call re-run, silent non-procedure apply).

See [IMPROVEMENTS.md](IMPROVEMENTS.md) for details,
the API gaps, and next steps. Machine-readable status: [STATE.txt](STATE.txt).

## License

Apache-2.0, see [LICENSE](LICENSE).
