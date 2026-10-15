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
identify.aura    热点识别：observed-calls（实时按 define 调用计数，lib/counter.aura）× 实测 ms/call + query:calls 调用点
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
scripts/run_tests.sh                       # 4 个测试文件，112 项检查
```

`run_mvp.sh` 在 `94ca96362` 上的一次输出（节选；计时每次略有浮动）：

```
-- 0b. instrument candidates (mutate:rebind -> counting wrappers), run live workload
  live calls: hot-sum-sq=400 cold-add=4800 label-of=400
-- 1. identify (policy = workspace sink-policy)
  hot-sum-sq  sink=#t  score=374.333  calls=400  sites=1  ms/call=0.933333  numeric=#t
  cold-add  sink=#f  score=51.4  calls=4800  sites=1  ms/call=0.0105  numeric=#t
  label-of  sink=#f  score=5.2  calls=400  sites=1  ms/call=0.0105  numeric=#f
  selected: (hot-sum-sq)
-- 1b. hot-swap sink-policy (budget 50 ms -> 1e9 ms), re-identify, restore
  selected: ()
  [real] hot-sum-sq: reject reasons=(wrong-result) speedup=0 t-before=58ms t-after=0ms stage=score rolled-back=#t rollback-ok=#t
  [real] hot-sum-sq: reject reasons=(too-slow) speedup=0.170213 t-before=56ms t-after=329ms stage=score rolled-back=#t rollback-ok=#t
  [real] hot-sum-sq: reject reasons=(load-failed) speedup=0 t-before=67ms t-after=0ms stage=load rolled-back=#f rollback-ok=#t
  [real] cold-add: reject reasons=(too-slow) speedup=0.511111 t-before=23ms t-after=45ms stage=score rolled-back=#t rollback-ok=#t
  [real] hot-sum-sq: accept reasons=() speedup=29.5 t-before=59ms t-after=2ms stage=score rolled-back=#f rollback-ok=#t
-- 4. final state
  (hot-sum-sq 100) = 328350
  hot-strategy version = 6, active = hot-sum-sq
checks: 8 passed, 0 failed
```

### 实时调用计数（`lib/counter.aura`，过渡实现）

- `fixture:instrument!` 用 `mutate:rebind` 把每个候选换成计数包装：
  `(lambda (a0) (vector-set! sink-calls 0 (+ 1 (vector-ref sink-calls 0))) (sink-impl-hot-sum-sq a0))`，原函数体留在 `sink-impl-<name>`。
- `counter:observe`：清零 → 跑真实负载（`app-workload`）→ 读计数，交给 `identify:run` 的 `calls-of`。不再有写死的 calls 列。
- 换个负载（`app-workload` 的 `hot?=#f`，从不调 `hot-sum-sq`）同一份代码就不选它：决策由实时计数驱动。
- 下沉后的包装也计数；`hot-strategy:heal!` 回滚恢复的是计数版函数体，计数不断。
- 限制：计数器在 workspace 里，`eval-current` 会重新求值 `(define sink-calls ...)` 并清零，所以计数是从上次 `eval-current` 开始的窗口；包装引用的是 workspace 名字（引用宿主文件的 define 会让 `mutate:rebind` 类型检查报 unbound）。这不是 VM 级计数器，Aura 提供按 define 的原生计数后应替换。
- workspace 要保持 ≤8 个过程 define：第 9 个会让无关函数调用慢约 35 倍（aura#4479），这会让基线变慢、把本该拒绝的 kernel 判成更快。所以计数是内联在包装里，没有单独的 `sink-tick!`。

### 自动发现候选（`lib/discover.aura`）

不再有手写候选清单。`discover:scan` 读 `(query:code)`（规范化后的工作区源码），解析出每个顶层 `(define NAME (lambda ...))`，按以下规则筛选：不在 `sink-*` 命名空间；叶子函数（函数体不引用其他工作区 define，C kernel 无法回调 Aura，所以 `app-workload` 这类驱动函数被排除）；固定 1–4 个参数；用小整数实际调用一次返回整数（Int-callable，`label-of` 因返回字符串被排除）。发现的集合经 `discover:instrument!` 自动换成计数包装（槽位按工作区顺序，最多 8 个），跑完活负载后 `discover:hot` 只保留调用数 ≥ 阈值的（没有活窗口时退回到 `query:calls` 静态调用点 ≥1）。`discover:rows` 生成 identify 行（自动标定重复次数），最终决策仍由可热换的 `sink-policy` 做。新加一个 define 不用改任何清单就能被发现。

### C kernel 生成器（`lib/kgen.aura`）

`scripts/gen_kernel.sh` 对发现出的候选逐个生成 C：读 `(query:code)` 里的 define 树，支持整数字面量、参数、`+ - *`、`if`、比较/`and`/`or`/`not` 条件、对自身或 `letrec` 循环（named let 规范化后的形式）的尾调用（编译成 `goto`）。生成 `int64_t` 代码（带溢出检查，见下），编进 `build/kernels/libgen_<name>.so`，导出 `sink_gen_<name>`，带 `aot_emit_version`，直接走现有 `aot:reload` + `c-func` + `mutate:rebind` + 评分闭环。字符串/浮点/布尔值、调用其他函数、非尾递归、除法（Aura 除零语义）、`begin`/`set!`、把整数当条件（真值语义不同）一律跳过并给出原因，不会生成可疑 kernel。示例：`hot-sum-sq`、`cold-add` 生成成功，`label-of` 因字符串字面量跳过。MVP 先试生成版，生成版被拒才回退手写版；实测生成版加速约 16×（加溢出检查前约 29×），与手写版逐点一致。

**整数溢出检查**：Int kernel 不再用 `-fwrapv`。每个 `+ - *`（和一元 `-`）都走 `kernels/sink_abi.h` 里的 `sink_ck_add/sub/mul/neg`：`__builtin_*_overflow` 判 int64 溢出，再判是否超出 Aura fixnum 范围 [-2^62, 2^62-1]，超了就置位局部 `ovf`。每次 `return` 和每次循环回跳都会检查 `ovf`，一旦溢出就立刻 `goto ovf_exit`：返回 0，并置位线程局部状态，经导出函数 `sink_gen_<name>_overflowed`（`() -> Int`，读后清零）查询。`sink-plan:bind-body` 会把生成的 Int kernel 槽位绑定成闭包：先调 kernel，再查状态，非零就 `(error "sink: int overflow/div0 in <name> (outside Aura Int range, or division by zero)")`，所以 0 哨兵永远到不了 Aura 代码，属于 fail closed。之所以不对齐「Aura Soft 的行为」，是因为 Soft 在这些输入上没有定义行为：直接 SIGABRT（#4462 的残留）。另外，pinned tip 上 ≤ -5·10^15 的负整数无法经 `c-func` 回传（#4482），所以结果 < -4999999999999999 也按溢出 fail closed（`AURA_SINK_RET_MIN`，修好后可用 `-DAURA_SINK_RET_MIN=AURA_SINK_FIX_MIN` 去掉）；中间值仍按完整 fixnum 范围检查。不用越界哨兵是因为 `c-func` 返回越界 long 会直接 abort（#4483）。手写 kernel 和 Float kernel 不受影响。代价：`hot-sum-sq` 加速从约 29× 降到约 16×（检查 + 多一次状态调用），仍远高于门槛。`tests/test_kgen_overflow.aura` 覆盖：边界内对拍 Aura、各运算溢出 fail closed、循环在首次溢出就停、状态逐次清零，以及下沉后的 `hot-sum-sq` 在超范围输入上抛 Aura 错误、之后仍可用。

**带保护的除法**：Int 的 `quotient` 和 Int `/` 都生成 `sink_ck_quo(a, b, &ovf)`。Aura 在 Int 上两者都向零截断，与 C99 `/` 一致，例如 `(quotient -7 2)` = -3；`(/ a b c)` 按左结合折叠。执行 `/` 之前先检查除数：`b == 0` 就置位状态 bit 2（div0）并返回 0；`b == -1` 改走带检查的取负，所以 `INT64_MIN / -1`（C 里是 UB）永远不会执行。唯一会越出 fixnum 范围的商是 `-2^62 / -1` = 2^62，置位 bit 1（溢出）。这两种情况都和 `+ - *` 走同一条出口，`<symbol>_overflowed` 的值：1 = 溢出，2 = 除零，3 = 两者都有。包装层会抛 `(error "sink: int overflow/div0 in <name> ...")`。Aura Soft 遇到除零会抛一个可以 catch 的错误（`quotient: division by zero`），pinned 构建上不会 SIGABRT，所以除零时两边的结果都是 Aura 错误。MIN/-1 没法拿 Soft 当参照，因为 Aura 里用不了 -2^62（#4482）。一元 `/` 和 Float `/` 仍然带原因跳过。整数字面量现在按字符识别、原样输出，不再经过 `string->number`；后者超过 2^53 会舍入，到 2^62-1 会 SIGABRT（aura#4485）。`tests/test_kgen_quotient.aura` 共 16 项。

**带保护的 remainder / modulo**：Int 的 `(remainder a b)`、`(modulo a b)` 生成 `sink_ck_rem` / `sink_ck_mod`。`remainder` 向零截断，符号跟被除数，与 C99 `%` 一致，例如 `(remainder -7 2)` = -1。`modulo` 对齐 pinned Aura Soft 的实际行为：`r = a % b; if (r < 0) r += |b|`，结果总是非负，所以 `(modulo 7 -2)` = 1，而不是 R7RS 地板取模的 -1。这是 Aura 的 bug（#4486）；kernel 先照抄，保证下沉不改结果，Aura 修好后用 `-DAURA_SINK_MODULO_FLOOR=1` 切到地板语义。做 `%` 之前先查除数：`b == 0` 置位状态 bit 2（div0）返回 0，和 quotient 同一条出口、同一条包装错误。`b == -1` 不执行 `%` 直接返回 0，所以 `INT64_MIN % -1`（C 里是 UB）不会执行；fixnum 范围内 `-2^62 % -1` 本来就是 0，Soft 也返回 0，所以这里对齐 Soft，不 fail closed。fixnum 范围外的操作数（只有非 Aura 调用方才会传入）置位 bit 1。`tests/test_kgen_remmod.aura` 共 17 项。

**Float kernel**：`kgen:compile-float`（或 `kgen:compile-kind name tree "Float"`）用同一子集生成 `double` 代码：浮点字面量原样保留为 C double，整数字面量变成 `(N.0)`，参数/循环变量/返回值都是 `double`，签名 `(Float ...) -> Float`。类型由 discover 决定：静态上用小整数调用返回整数就是 Int，否则用小浮点调用返回浮点就是 Float；没有浮点字面量的数值函数（如 `hot-fsum`）两者都满足，先当 Int，再靠**活类型反馈**：计数包装同时把首参写进 `sink-argf`，活负载跑完后 `discover:retype!` 把首参是浮点的候选改成 Float。`fixture:float-workspace-code` 是第二个工作区（`hot-fsum`/`cold-add`/`label-of`，与 Int 工作区分开以免超过 8 个过程，#4479）；MVP 第 5 步在里面走完 发现 → 类型反馈 → identify → 生成 double kernel → 闭环，实测约 29–41×，与 Aura 逐位相等。字符串/布尔/除法/调用其他函数在 Float 下同样带原因跳过；Int 路径不变。限制：工作区过程体里的浮点字面量仍读成 0（#4475），所以工作区里的 Float 候选不能写浮点常量（带字面量的 `fpoly` 只在宿主文件里对拍）；Aura 里 `n<=0` 时返回整数 0 而 kernel 返回 0.0，所以 `hot-fsum` 用 `(- h h)` 起步。

### f64vector kernel（`lib/vkgen.aura`）

针对 Aura #4503 的 `f64vector`（连续存放、不装箱的 double），`vkgen` 把遍历向量的循环 define 生成为紧凑的 C 循环。参数种类由 `vkgen:kinds` 决定：在 `f64vector-ref`/`f64vector-length` 里当向量用的参数是 V，其余参数按实际调用值定为 I（精确整数）或 F（Float）。表达式按类型生成，Int/Float 混合运算和 Aura 一样按左折叠规则提升；Int 运算全部带溢出检查；每次 `f64vector-ref` 都做越界检查，越界时置状态位 4，wrapper 抛 Aura 错误（fail closed）。支持的形式有 `+ - * /`、`quotient/remainder/modulo`、`sqrt abs min max exact->inexact`、`if`、比较、`and/or/not`，以及 named let / 自尾递归。如果循环变量在回边上会从 Int 变成 Float，就跳过不生成：循环一次都不执行时 Aura 返回的是 Int，不存在一个对所有情况都正确的 C 类型。编译不加 `-ffast-math`，累加顺序和 Aura 相同，Float 结果逐位相等。

调用约定绕开了 aura#4531（`c-func` 的混合签名会走错调用约定）：向量按 String 参数传入，紧跟它的长度（Int）；Float 标量打包进每次调用新分配的 aux f64vector，所以签名里从不出现 Float 参数。Float 返回值另外导出 `<sym>_bits` 版本，按 long 返回 double 的位模式；绑定时用 `aura_sink_f64ret_probe` 探测一次，决定用哪个版本。wrapper 带类型守卫，参数类型不符（比如 Float 的 n、Int 的 k）时退回原来的 Aura body（deopt），结果和下沉前一致。

测试：`tests/test_vkgen.aura`、`tests/test_vkgen_more.aura`。这两个文件分开跑，因为 aura#4532：每次 `c-func` 都会占用一个 FFI id，id 多了会和闭包 id 冲突。闭环全部 accept，含错误 kernel 的 reject + heal。基准 `scripts/bench_vec.aura`（ms/次，Soft 指 workspace define 本身）：

| | Soft | 下沉后 | 裸 c-func | 原生 `f64vector-dot` |
|---|---|---|---|---|
| dot n=1000 | 1.03 | 0.075（13.7×） | 0.015 | 0.015 |
| sum n=1000 | 0.675 | 0.06（11.3×） | 0.02 | – |
| dot n=100000 | 119.3 | 0.4（298×） | 0.2 | 0.1 |
| sum n=100000 | 61.6 | 0.3（205×） | 0.1 | – |

下沉后和裸 c-func 之间的差距，是 wrapper 的守卫开销加上 String 参数的整段拷贝（aura#4531 里提了零拷贝借用槽位的需求）。

### 后台编译 + 门控换入 + 编辑重沉（`lib/bg.aura`、`lib/watch.aura`、`loop:sink-gated`）

- **后台编译**：`bg:launch!` 把 vkgen 生成的 C 源按内容哈希命名（`build/kernels/libvgen_<name>_<hash>.so`），以分离的 OS 进程（`kernels/build_one.sh … &`，写完 `.done` 再原子 `mv`）编译，立即返回 `pending`。同一份 C 源已经编过就直接返回 `cached`，不支持的函数体返回 `skipped` 并带原因。`bg:wait-serving` 在等待期间继续调用 Soft define 服务请求（测试里 cc 运行期间服务了 13 次 Soft 调用）。`bg:launch-fiber!` 把同一流程放进 fiber，`fiber:join` 取结果。注意：thread 后端上 fiber 和主求值器是串行的（aura#2738），fiber 里 `shell "sleep 1"` 不会和主线程重叠，所以真正的并行来自分离的 cc 进程，不来自 fiber。
- **门控换入（MutationBoundary 之前中止）**：`loop:sink-gated` 先在宿主侧对 bind body 求值，用 `shadow-args` 逐个和当前 define（`(eval name)`）比对结果（含 deopt 参数、出错要一致）。不一致就在 `gate` 阶段以 `gate-mismatch` 拒绝，**不产生任何 mutation**（`ast:generation` 不变，不需要回滚）。通过后在一个 `mutate:atomic-batch`（一次 MutationBoundary）里 bind 槽位 + 换 wrapper，再 `eval-current` + 评分，不达标就 heal。`mutate:atomic-batch` 已默认打开（#4476 已修）。
- **脏追踪驱动的重编**：`watch:register!` 记下已下沉 define 的 wrapper 和样本参数。`watch:check` 先看 `ast:generation`：没变就是 clean（快路径），变了再用 `discover:scan` 比树，得到 `clean/edited/removed` 和新树。用户编辑一个已下沉的 define（`mutate:rebind`）后，新体立即以 Soft 生效，`watch:check` 报告 `edited`，后台按新树重编，再门控换入。改成不支持的体就保持 Soft，job 状态为 `skipped`（原因如 `unsupported call exp`）；改回之前的体命中缓存（`cached`，同一个 .so）。

测试 `tests/test_bg.aura`（28 项）：后台构建、门控接受、错一位 kernel 在门口被拒（generation 不变）、编辑检测 + 重沉、不支持的编辑保持 Soft、缓存命中、fiber 构建 + vdot 门控下沉。

### 值编码 ABI（单一来源）

`kernels/sink_abi.h` 不再自己写编码常量，而是直接包含 Aura 的 `lib/aura_value_abi.h`（aura#4502，ABI 4）：`kernels/build.sh` 加 `-I$AURA_LIB`，该头文件变化时重编所有 kernel；fixnum 范围取自 `AURA_FIXNUM_MIN/MAX`，低于 ABI 4 直接 `#error`。每个 kernel 导出 `aura_sink_value_abi()`，`plant:real` 在 `c-load` 后、任何 mutation 之前检查它，不匹配就在 load 阶段拒绝（`kernels/stale_abi.c` 对应测试）。

### 什么是真的，什么是替身

| 步骤 | 状态 |
|---|---|
| 识别：实测 ms/call、`query:calls` 调用点、workspace 策略决策、策略热替换 | **真跑** |
| 调用次数画像 | **真跑（过渡实现）**：`mutate:rebind` 把候选换成计数包装，计数器在 workspace 的 `sink-calls` 向量里；Aura 还没有原生按 define 的调用计数，见 IMPROVEMENTS |
| 生成 C kernel | **真跑（最小子集）**：`lib/kgen.aura` 从工作区 define 的 AST 生成 `build/gen/gen_<name>.c`；不支持的函数体带原因跳过；手写 `kernels/*.c` 保留为回退 |
| 编译 `.so` | **真跑**（`kernels/build.sh`，`cc -O2 -shared -fPIC`） |
| `aot:reload` 加载 | **真跑**，Soft 面返回 `#t`、走 staging + epoch bump；但 kernel 不是 Aura AOT 模块，调用路径走 `c-func` |
| `c-load` + `c-func` 绑定 | **真跑** |
| `mutate:rebind` 换封装 + `eval-current` | **真跑** |
| 评分 / 接受 / `hot-strategy:heal!` 回滚 + golden 复查 | **真跑** |
| `mutate:atomic-batch` 一次边界内 bind+swap | **真跑，默认打开**（#4476 已修）；`loop:sink-gated` 在边界前做影子门控 |
| FakePlant | 同一闭环、Aura 替身体（闭式公式/错一位/慢 4 倍），任何构建都能跑 |

### 已知上游问题（Aura，均已在本仓库 `probes/` 里给出最小复现）

- [#4475](https://github.com/cybrid-systems/aura/issues/4475)：workspace 过程体里的浮点字面量变成 `0`（策略阈值 `0.05` 失效）→ 本仓库 workspace 常量只用整数。
- [#4476](https://github.com/cybrid-systems/aura/issues/4476)：两步 `mutate:atomic-batch` + `ast:restore` 在一个顶层 form 里会让整个文件静默重跑；`(define x (mutate:atomic-batch …))` 后 `x` 未绑定 → 默认用两次 `mutate:rebind`，结果用 `set!` 存。
- [#4477](https://github.com/cybrid-systems/aura/issues/4477)：与原语同名的用户 define（`check`）在 `eval-current` 后被原语顶掉 → 测试助手改名 `expect`。
- [#4481](https://github.com/cybrid-systems/aura/issues/4481)：把 plant（含闭包）内联作为参数传给执行 register!/rebind/eval-current 的顶层 form 时报 `cannot call: ''` 且整文件静默重跑 → plant 先绑到顶层 define。
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
| `lib/identify.aura` | hotspot signals (live call counts × measured ms/call, `query:calls` sites); decision delegated to the workspace `sink-policy` |
| `lib/counter.aura` | live per-define call counters: counting wrappers via `mutate:rebind`, `counter:observe` = reset → run workload → read |
| `lib/sink_plan.aura` | plan: kernel slot, `.so`, symbol, `c-func` signature, wrapper body, documented primitive sequence |
| `lib/plant.aura` | RealPlant (`aot:reload` + `c-load` + `c-func` probe) and FakePlant (Aura stand-in bodies) |
| `lib/score.aura` | correctness (golden; errors count as wrong), determinism, speedup ≥ min |
| `lib/loop.aura` | one attempt: register! → load → rebind slot + wrapper → eval-current → score → accept / heal! + golden re-check |
| `lib/fixture.aura` | toy workspace: `hot-sum-sq` (hot loop), `cold-add` (trivial), `label-of` (strings), policy, counters, `app-workload` (the live traffic) |
| `lib/harness.aura`, `lib/expect.aura` | shared attempt runner, PASS/FAIL helper |
| `lib/kgen.aura` | C kernel generator: define AST -> `int64_t` (Int) or `double` (Float) C, or skip with a reason |
| `kernels/` | hand-written fallback `sum_sq.c` (good), `example_add.c` (trivial), `bad_wrong.c`, `bad_slow.c`; `build.sh` builds these and `build/gen/*.c` |
| `scripts/` | `env.sh`, `run_mvp.sh`, `run_tests.sh`, `mvp.aura`, `gen_kernel.sh` + `kgen.aura` (generate + build) |
| `tests/` | `test_discover`, `test_identify`, `test_kgen`, `test_score`, `test_loop_real`, `test_loop_fake`, `test_kgen_float`, `test_kgen_overflow`, `test_kgen_quotient`, `test_kgen_remmod` (272 checks) |
| `probes/` | minimal repros of the Aura bugs found while building this |

### Run

```bash
export AURA_BIN=~/aura/build-rel/aura      # an Aura build; AURA_LIB defaults to <checkout>/lib
scripts/run_mvp.sh                         # end to end with real .so files (RealPlant)
SINK_PLANT=fake scripts/run_mvp.sh         # same loop, Aura stand-in data plane
scripts/gen_kernel.sh                      # generate build/gen/*.c + build .so (run_* call it)
scripts/run_tests.sh                       # 10 test files, 272 checks
```

The runner fails a test file on a FAIL line, a crash, a non-zero exit, an error
line, zero PASS lines, or a `RUN-ONCE` marker that is not printed exactly once
(the Soft file runner can silently re-run a whole file, which would repeat every
mutation).

### Automatic candidate discovery (`lib/discover.aura`)

No hand-written candidate list: `discover:scan` reads `(query:code)`, keeps top-level `(define NAME (lambda ...))` forms that are outside `sink-*`, leaves (body names no other workspace define), fixed arity 1..4, and Int-callable (a probe call with small integers returns an integer). `discover:instrument!` swaps them into counting wrappers (slots in workspace order, max 8), `discover:hot` keeps those with live calls >= min-calls (static `query:calls` sites when no live window), and `discover:rows` feeds identify; the decision is still the hot-swappable `sink-policy`. `scripts/mvp.aura` uses the discovered set.

### C kernel generator (`lib/kgen.aura`)

`scripts/gen_kernel.sh` generates C for each discovered candidate from its `(query:code)` define tree. Subset: integer literals, parameters, `+ - *`, `if`, comparison / `and` / `or` / `not` conditions, and tail calls to the define itself or to a `letrec` loop (what named let desugars to), compiled to `goto`. Output is overflow-checked `int64_t` C (below) built into `build/kernels/libgen_<name>.so`, exporting `sink_gen_<name>` plus `aot_emit_version`, so it goes through the existing `aot:reload` + `c-func` + `mutate:rebind` + score loop unchanged. Int `quotient` / `remainder` / `modulo` (2 args) and Int `/` (2+ args) are generated as checked division (below). Strings, floats, boolean values, calls to other procedures, non-tail recursion, Float division / Float `remainder`/`modulo`, `begin`/`set!`, and Int-as-condition (truthiness differs) are skipped with a reason — never a guessed kernel. On the fixture `hot-sum-sq` and `cold-add` generate, `label-of` is skipped (string literal). The MVP tries the generated kernel first and falls back to the hand-written one only if it is rejected; measured ~16x with overflow checks (~29x before), identical to the hand-written kernel on all cases.

**Checked integer arithmetic.** Int kernels no longer use `-fwrapv`. Every `+ - *` (and unary `-`) goes through `sink_ck_add/sub/mul/neg` in `kernels/sink_abi.h`. `__builtin_*_overflow` catches int64 overflow, then a range check catches anything outside the Aura fixnum range [-2^62, 2^62-1]; either one sets a local `ovf` flag. Every `return` and every loop back-jump tests the flag. On overflow the kernel jumps to `ovf_exit`, returns 0 and sets a thread-local status, exported as `sink_gen_<name>_overflowed` (`() -> Int`, read-and-clear). `sink-plan:bind-body` binds the slot of a generated Int kernel to a closure that calls the kernel, reads the status, and raises `(error "sink: int overflow/div0 in <name> (outside Aura Int range, or division by zero)")` when it is set, so the 0 sentinel never reaches Aura code (fail closed). The kernels cannot copy Aura Soft's behaviour here because Soft has none defined for these inputs: it SIGABRTs (residual of #4462). On the pinned tip a negative Int at or below -5e15 also cannot be passed back across `c-func` (aura#4482), so results < -4999999999999999 fail closed the same way (`AURA_SINK_RET_MIN`; build with `-DAURA_SINK_RET_MIN=AURA_SINK_FIX_MIN` once that is fixed). Intermediates are still checked against the full fixnum range. Overflow is not signalled with an out-of-range sentinel because a `c-func` returning such a long aborts the process (aura#4483). Hand-written and Float kernels are unchanged. Cost: the `hot-sum-sq` speedup drops from ~29x to ~16x (the checks plus the status call), still far above the threshold. `tests/test_kgen_overflow.aura` covers in-range edge cases against Aura, fail-closed overflow for each op, loops stopping at the first overflow, per-call status clearing, and the sunk `hot-sum-sq` raising an Aura error on an out-of-range input and staying usable afterwards.

**Checked quotient.** Int `quotient` and Int `/` (Aura truncates both toward zero on Int, same as C99 `/`: `(quotient -7 2)` = -3) compile to `sink_ck_quo(a, b, &ovf)`, and `(/ a b c)` folds left. The divisor is tested *before* any `/` runs. `b == 0` sets status bit 2 (div0) and returns 0. `b == -1` goes through checked negation, so `INT64_MIN / -1` (UB in C) never runs. The only fixnum quotient that leaves the fixnum range, `-2^62 / -1` = 2^62, sets bit 1 (overflow). Both go through the same exit as `+ - *`: `ovf_exit` stores the bits in `<symbol>_overflowed` (now 1 = overflow, 2 = div0, 3 = both), and the slot wrapper raises `(error "sink: int overflow/div0 in <name> ...")`. Aura Soft raises a catchable `quotient: division by zero` / `/: division by zero` error, so with div0 both sides end in an Aura error. Soft does not SIGABRT on div0 on the pinned build. On MIN/-1 Soft cannot be the reference, because -2^62 is not usable from Aura (#4482). Unary `/` and Float `/` are still skipped with a reason. Integer literals are now recognised by their characters and emitted verbatim. Before, they went through `string->number`, which rounds above 2^53 and SIGABRTs at 2^62-1 (aura#4485). `tests/test_kgen_quotient.aura` (16 checks) covers sign and edge quotients and Int `/` against Aura, a quotient loop, Soft's catchable div0, kernel div0 → status 2 (including inside a loop, which stops on the first iteration), MIN/-1 → status 1, per-call clearing, and the checked wrapper text and its error.

**Checked remainder / modulo.** Int `(remainder a b)` and `(modulo a b)` compile to `sink_ck_rem` / `sink_ck_mod` (kernels/sink_abi.h). `remainder` truncates and takes the sign of the dividend, like C99 `%`: `(remainder -7 2)` = -1. `modulo` matches what the pinned Aura Soft actually returns: `r = a % b; if (r < 0) r += |b|`, a non-negative result, so `(modulo 7 -2)` = 1 rather than the R7RS floor modulo -1. That is an Aura bug (#4486); the kernel copies it so a sunk define never changes answers, and `-DAURA_SINK_MODULO_FLOOR=1` switches to floor semantics once Aura is fixed. The divisor is tested before any `%` runs: `b == 0` sets status bit 2 (div0) and returns 0, the same exit and wrapper error (`sink: int overflow/div0 ...`) as quotient. `b == -1` returns 0 without executing `%`, so `INT64_MIN % -1` (UB in C) never runs. Within the fixnum range `-2^62 % -1` is 0 and Soft returns 0 too, so there is nothing to fail closed on: the kernel matches Soft instead. An operand outside the fixnum range (only possible from a non-Aura caller) sets bit 1. `|r| < |b|`, so results cannot overflow. `tests/test_kgen_remmod.aura` (17 checks) covers all 18 sign combinations for both ops against Aura, edges at 2^62-1, -2^62 and -2^52, MIN % -1, a gcd tail loop, Soft's catchable div0, kernel div0 → status 2, per-call clearing, and the checked wrapper.

**Float kernels.** `kgen:compile-float` emits the same subset as `double` C: float literals stay C doubles, integer literals become `(N.0)`, signature `(Float ...) -> Float`. discover picks the kind: Int when small integer arguments return an integer, Float when only small float arguments return a float. A numeric body without float literals (`hot-fsum`) is both, so it starts as Int and is promoted by **live type feedback**: the counting wrapper also stores its first argument in `sink-argf`, and `discover:retype!` turns a candidate whose live calls pass a float into Float. `fixture:float-workspace-code` is a second workspace (kept apart from the Int one, aura#4479); MVP step 5 runs discover -> type feedback -> identify -> generated double kernel -> closed loop there (~29-41x, bit-equal to Aura). Workspace Float candidates cannot use float literals yet (aura#4475: they read back as 0), so the literal case (`fpoly`) is checked against a host-file define.

### f64vector kernels (`lib/vkgen.aura`)

For Aura's #4503 `f64vector` (contiguous unboxed doubles), `vkgen` turns loop defines over vectors into tight C loops. `vkgen:kinds` assigns parameter kinds: a parameter used as a vector in `f64vector-ref`/`f64vector-length` is V, and the rest are I (exact Int) or F (Float) according to the live sample arguments. Expressions are typed, and mixed Int/Float arithmetic promotes along Aura's left fold. Int ops are overflow-checked. Every `f64vector-ref` is bounds-checked; out of range sets status bit 4 and the wrapper raises an Aura error (fail closed). Supported: `+ - * /`, `quotient/remainder/modulo`, `sqrt abs min max exact->inexact`, `if`, comparisons, `and/or/not`, named let and self tail calls. A loop variable that would switch between Int and Float on the back edge is a skip, because Aura returns the entry type when the loop does not run, so no single C type is right. No `-ffast-math`: summation order is Aura's, so Float results are bit-equal.

The calling convention works around aura#4531 (mixed `c-func` signatures use the wrong convention). A vector is a String arg followed by its length as an Int arg. Float scalars go in an aux f64vector allocated per call, so no Float arg is ever declared. A Float result is also exported as `<sym>_bits`, which returns the double's bit pattern as a long; `aura_sink_f64ret_probe` is called once to pick the version. The wrapper has type guards: arguments that don't fit (a Float n, an Int k) fall back to the original Aura body (deopt), so answers stay the same.

Tests: `tests/test_vkgen.aura` and `tests/test_vkgen_more.aura`. They are split into two files because of aura#4532: every `c-func` takes an FFI id, and enough of them collide with closure ids. Every closed-loop attempt is accepted, and a wrong kernel is rejected and healed. Benchmark `scripts/bench_vec.aura` (ms/call; Soft = the workspace define itself):

| | Soft | sunk | raw c-func | native `f64vector-dot` |
|---|---|---|---|---|
| dot n=1000 | 1.03 | 0.075 (13.7x) | 0.015 | 0.015 |
| sum n=1000 | 0.675 | 0.06 (11.3x) | 0.02 | – |
| dot n=100000 | 119.3 | 0.4 (298x) | 0.2 | 0.1 |
| sum n=100000 | 61.6 | 0.3 (205x) | 0.1 | – |

The gap between sunk and raw c-func is the wrapper's guards plus the full copy of each String arg (aura#4531 also asks for a zero-copy borrowed slot).

### Background compile + gated swap + re-sink on edit (`lib/bg.aura`, `lib/watch.aura`, `loop:sink-gated`)

- **Background compile**: `bg:launch!` names the vkgen C source by content hash (`build/kernels/libvgen_<name>_<hash>.so`), compiles it in a detached OS process (`kernels/build_one.sh … &`, `.done` written then `mv`ed atomically), and returns `pending` at once. A C source that was already built returns `cached`; an unsupported body returns `skipped` with the reason. `bg:wait-serving` keeps serving calls to the Soft define while it waits (13 Soft calls served during cc in the test). `bg:launch-fiber!` runs the same pipeline in a fiber, joined with `fiber:join`. Note: on the thread backend fibers are serialized with the main evaluator (aura#2738): a fiber's `shell "sleep 1"` does not overlap main work. The real parallelism comes from the detached cc process, not from the fiber.
- **Gated swap, aborted before the MutationBoundary**: `loop:sink-gated` evaluates the bind body host-side and compares it on `shadow-args` against the current define (`(eval name)`), including deopt arguments and matching errors. On any mismatch it rejects at stage `gate` with `gate-mismatch`, and **no mutation happens** (`ast:generation` unchanged, nothing to roll back). If the gate passes, slot bind and wrapper swap run in one `mutate:atomic-batch` (one MutationBoundary), followed by `eval-current` and scoring, with a heal if the score fails. `mutate:atomic-batch` is now on by default (#4476 fixed).
- **Dirty-tracking re-sink**: `watch:register!` records a sunk define's wrapper and sample args. `watch:check` first compares `ast:generation`: if unchanged, the define is clean (fast path). Otherwise it compares trees from `discover:scan` and returns `clean/edited/removed` plus the new tree. When the user edits a sunk define (`mutate:rebind`), the new body is live at once on Soft and `watch:check` reports `edited`. The background job then rebuilds from the new tree and swaps it in through the gate. An edit to an unsupported body stays Soft with job status `skipped` (e.g. `unsupported call exp`). An edit back to an earlier body is a cache hit (`cached`, same .so).

Test `tests/test_bg.aura` (28 checks) covers: background build, gated accept, an off-by-one kernel rejected at the gate (generation unchanged), edit detection + re-sink, an unsupported edit staying Soft, a cache hit, and a fiber build + gated vdot sink.

### Value ABI (single source)

`kernels/sink_abi.h` restates no encoding constant: it includes Aura's `lib/aura_value_abi.h` (aura#4502, ABI 4). `kernels/build.sh` adds `-I$AURA_LIB` and rebuilds every kernel when that header changes; the fixnum range comes from `AURA_FIXNUM_MIN/MAX`; anything below ABI 4 is an `#error`. Every kernel exports `aura_sink_value_abi()`, and `plant:real` checks it right after `c-load`, before any mutation, refusing a mismatch at the load stage (tested with `kernels/stale_abi.c`).

### What runs for real vs stubbed

| step | status |
|---|---|
| identify: measured ms/call, `query:calls` call sites, workspace-policy decision, policy hot-swap | **real** |
| per-function call counts | **real, interim**: candidates hot-swapped (`mutate:rebind`) into counting wrappers; counts live in the workspace vector `sink-calls`, read after a workload run (`lib/counter.aura`). Not a VM counter: Aura has none per define yet |
| C kernel generation | **real, minimal subset**: `lib/kgen.aura` emits `build/gen/gen_<name>.c` from the workspace define's AST; unsupported bodies are skipped with a reason; hand-written `kernels/*.c` stay as fallback |
| build `.so` | **real** (`kernels/build.sh`) |
| `aot:reload` | **real**; returns `#t` on Soft with staging + epoch bump. The kernel is not an Aura AOT module, so calls go through `c-func` |
| `c-load` + `c-func` binding | **real** |
| `mutate:rebind` wrapper + `eval-current` | **real** |
| score / accept / `hot-strategy:heal!` rollback + golden re-check | **real** |
| bind + swap in one `mutate:atomic-batch` | **real, on by default** (#4476 fixed); `loop:sink-gated` runs a shadow gate before the boundary |
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
