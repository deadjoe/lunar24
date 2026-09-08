# GH#21 Surface-2 前置 — vco/vcf 16 个 seconds 参数平滑接入设计（只读取证） — task #93

- Worktree: `wt-21-gh21-smoothing`, branch `feat/21-continuous-smoothing`, HEAD `e2873b0`
  (== PR#30 merged head == main `e61d0ed` content; Surface-1 已 LANDED, GH#21 issue 仍 OPEN)
- Base for evidence: `core/include/lunar24/core/machine_runtime.h` @ `e2873b0`
- Scope: **只读取证 + 契约草案**, 未实现 / 未 merge. 目的: 为 Surface-2 (vco/vcf 16) 先立
  live-vs-restore 分流裁决点与契约, 提交 @Kimi 审核。

---

## 0. 结论一句话

**16 个 vco/vcf `Smoothing::seconds` 参数今天只有"整体状态恢复(snap)"这一条到达路径; 没有 live 路径。**
建议采用 **candidate (ii)**: 把 16 接入**既有共享平滑机制** (controlSmoothers_ / advanceControlSmoothing_ /
applySmoothedControl_ / currentControlParam_), 让 **live 面板旋钮走 ControlEvent 车道(ramp)**、
**整体状态恢复走 applyDspParam(保持 snap)**。 这样 live-vs-restore 分流与 Surface-1 分层
**完全一致** (ControlEvent=ramp, applyDspParam=snap), 且**严禁第二套平滑实现**的硬约束满足——
只是平滑族的成员集从 20 扩到 36 (20 control-source + 16 panel-knob), 机制本体零新增。

---

## 1. @Kimi 六项取证

### (1) 16-param 清单 + dispatch 落点 + setter 单位/域

Registry 权威行: `generated/lunar24/registry.hpp` (第 261–286 行)。全部
`ParamRole::knob`, `Persistence::deviceState`, `ParamFieldEvidence` 六字段全 `unverified`
(= 值语义 PROVISIONAL)。

| ParameterId | 单位 | [min,max] | 默认 | applyDspParam dispatch line | setter |
|---|---|---|---|---|---|
| `vco_a_tune`    | oct  | [-1,1]  | 0   | 991  | `setVcoATune(v)` |
| `vco_a_morph`   | norm | [0,1]   | 0.5 | 994  | `setVcoAMorph(v)` |
| `vco_a_pw`      | norm | [0,1]   | 0.5 | 997  | `setVcoAPw(v)` |
| `vco_a_cv_amt`  | norm | [0,1]   | 1   | 1006 | `setVcoACvAmt(v)` |
| `vco_b_tune`    | oct  | [-1,1]  | 0   | 1012 | `setVcoBTune(v)` |
| `vco_b_morph`   | norm | [0,1]   | 0.5 | 1015 | `setVcoBMorph(v)` |
| `vco_b_pw`      | norm | [0,1]   | 0.5 | 1018 | `setVcoBPw(v)` |
| `vco_b_cv_amt`  | norm | [0,1]   | 1   | 1027 | `setVcoBCvAmt(v)` |
| `vcf_l_freq`    | norm | [0,1]   | 0.3 | 1033 | `setVcfFreq(0,v)` |
| `vcf_l_res`     | norm | [0,1]   | 0   | 1036 | `setVcfRes(0,v)` |
| `vcf_l_mod`     | norm | [0,1]   | 0   | 1039 | `setVcfMod(0,v)` |
| `vcf_r_freq`    | norm | [0,1]   | 0.3 | 1045 | `setVcfFreq(1,v)` |
| `vcf_r_res`     | norm | [0,1]   | 0   | 1048 | `setVcfRes(1,v)` |
| `vcf_r_mod`     | norm | [0,1]   | 0   | 1051 | `setVcfMod(1,v)` |
| `vcf_dist`      | norm | [0,1]   | 0   | 1060 | `setDistortionAmount(v)` |
| `vcf_gain`      | norm | [0,1]   | 0   | 1063 | `setDistortionGain(v)` |

全部 16 个 setter 直接消费 registry 单位域 (oct [-1,1] / norm [0,1]) 的 `v`, 无二次换算。
→ setter 单位为 "域内原值", 与 register 行一致。

### (2) 枚举 applyDspParam 全部 caller, 标 live / restore

- `machine_runtime.h:1427` (在 `applyDspState` 内) — **restore**。
- `tests/core/test_machine_control_sources.cpp:2504/2541/2560/2564/2571` — **测试** (t17-(E) 负控,
  用 `joystick_x` 验证 seconds-snap 分支的 fail-closed keep-old; 模拟的是 applyDspParam→snap 语义)。
- **无 live production caller。** `applyControlEvent_` (live lane, :1820) 直接走
  `setControlParamValue` 或 drone 直 set, **从不调用 applyDspParam**。

`applyDspState` caller:
- `machine_definition.h:482` (`runtime_.applyDspState(state_, ...)`) — **restore** (state 候选构建/装载)。
- `tests/host/test_state_apply_oracle_169.cpp:922/941/959/965` — **测试** (exactly-169 gate oracle)。

**结论:** `applyDspParam` 是纯整体状态 choke, 全部 production caller 属 restore/装载。
→ 这坐实了"16 无 live 路径": 今天的 16 只在恢复时被 snap 到; 任何 live 旋钮转动对 16 都
落 `unsupported_parameter` (见 §4 路线说明)。

### (3) P5 面板旋钮设计依据 (design/03 + 07)

- `design/07-core-contract.md:71` (§3.2): `knob、joystick、MIDI CC：以秒为单位的 continuous smoothing，避免 zipper noise` — **ramp 权威**。
- `design/07-core-contract.md:17`: `UI 不得直接修改 DSP 对象成员。路径固定为 UI/input → ControlEvent/command layer → core` — **UI 不直写 DSP, 必须经 ControlEvent 车道**。
- `design/07-core-contract.md:62`: `鼠标、电脑键盘、MIDI note/CC/clock 和 UI command 是 timestamped ControlEvent` — **旋钮命令 = ControlEvent**。
- `design/03-ui-framework.md:41`: `Lunar DSP core ← bounded parameter messages` — **iPlug2 host→core 参数 seam**。

→ 三条设计权威一致指向: P5 面板旋钮 = timestamped ControlEvent → core 控制车道 → seconds 平滑。
即 candidate (ii) 所主张的 live 路径。

### (4) Design fork 候选 (i/ii/iii) — pros/cons + 推荐

**candidate (i) — applyDspParam 加 ramp/snap 参数**
给 `applyDspParam(id, v, smoothOpt=snap)` 增加一个 mode 参数, 调用方显式选 ramp/snap。
- 缺点: 改**公开签名**, 波及全部 caller (applyDspState, machine_definition, oracle test);
  且把 restore-lane 的语义选择下沉到调用方, 破坏了 Surface-1 已锁死的"applyDspParam=snap"边界;
  调用方一传错就 ramp 一个本应 exact 的恢复, 正是 GH#21 stop-flow init 要避免的反模式。
- **否决** (或最不推荐)。

**candidate (ii) — live 走 ControlEvent 车道 (applyDspParam 保持 snap-only)【推荐】**
把 16 加进既有平滑族 (controlSmoothOrdinals_/controlSmoothers_/advanceControlSmoothing_/
applySmoothedControl_/currentControlParam_), 从而:
- live `ControlEvent(16, v)` → `applyControlEvent_` `default:` → `setControlParamValue` (识别到 16
  → 走 smooth ramp); 满足 design/07 §3.2 + 途径固定。
- 整体状态恢复 `applyDspState` → `applyDspParam` → (route 1) seconds-snap 分支 仍 **snap**;
  满足 GH#21 停止流精确初始化。
- 优点: 复用 Surface-1 完全相同的机制; **live=ramp, restore=snap 分层与 Surface-1 逐字一致**;
  无第二套平滑实现 (硬约束满足); 不改 applyDspParam 签名; exactly-169 gate 不受影响
  (applied_to_dsp 仍是 169; 只是 134 里搬走 16 → 118, control-source 35 → 51)。
- 代价: 需扩 4 处成员集 (见 §6), 且 (关键) 16 的**值域接纳**必须走 `dspParamValid_`
  而非 `controlParamValid_` (见 §6 last bullet —— 因为 controlParamValid_ `default:false` 拒 oct)。

**candidate (iii) — 其它**
- 每 dispatch case 单独加 per-param smooth 开关: 散、重复、违背"共享机制"约束。否决。
- 16 当 audio-rate CV 接入: **错**, 16 是 `ParamRole::knob` 面板旋钮, 不是音频率调制
  (与 parameter_smoothing.h 语义/分层范畴不符)。否决。

### (5) Acceptance design (16-param 轨迹/过阈/分区/零分配 + bypass 负控 RED + 回归锁)

建议契约 (red-first; 实现前先写"会红"判据):
- **T1 轨迹 (trajectory):** 对 16 逐个, 先 `applyDspParam(id, v0)` snap 到底值, 再
  `setControlParamValue(id, v1)`, `gh21_advance(rt, settleN)` 后 getter==v1; v1≠v0 且
  `gh21_settle_frames(kSr)` 窗口内为单极爬坡、非跳变。红线: tau→0 突变下, 轨迹断言必红 (无 ramp)。
- **T2 过阈/边沿 (crossing):** (仅对会驱动边沿的, 但 16 无 gate 语义 —— 从简) 垂直轨迹
  证明单调 + 5% 容差窗内达 target; 反旧帧: `advance(1)` 首帧 getter≠v1 (未 snap)。
- **T3 分区一致 (partition):** 64/128/mixed 分区 `processBlock` 读出逐位一致 (镜像 Surface-1
  t14(a2) partition-consistency; 禁止放宽)。
- **T4 零分配 (zero-alloc):** 全 64/128 分区 + 36 平滑器推进, 无运行时堆分配; 用既有无
  mock/计数断言 (与 Surface-1 同法)。
- **BYPASS 负控 (必须 RED):** 删 `dspParamValid_` 接纳 → 16 任一 `applyDspParam(oct<-1 或 norm>1)`
  从 invalid_value 变为 applied(越界被 setter 钳制) → 负控红; 恢复后绿。**这一条即 item 6 验收,
  证明新分支没有绕过既有 fail-closed 验证。**
- **回归锁:** ①Surface-1 count 钉 177 seconds/168 none (129 回归); ②exactly-169 gate
  (test_state_apply_oracle_169 全绿, applied_to_dsp 仍是 169 —— 改动的只是"哪条路线送达",
  不是"哪些 id 属于 applied_to_dsp"); ③GH#12 probe (8/8) 回归; ④三腿 Release/Debug/ASan+UBSan
  各 全绿; ⑤唯一允许红 = full-coverage (12 键盘缺口, by-design 非本 lane)。

### (6) 与 134-dispatch dspParamValid_ 的衔接 (不绕过既有 fail-closed)

`dspParamValid_` (machine_runtime.h:2171) 是 registry 域通用接纳: `isfinite` + (selector
`step>0`→精确整数 in[range]) / (continuous→in[range])。今天 16 在 route(3) 由它 admit。
**Surface-1 教训: 任何新分支不得跳过既有的 fail-closed 验证。**
因此 16 接入平滑族后, 其 live-ramp 与 snap 两条 admission **必须仍旧走 `dspParamValid_`**。
关键校验: `controlParamValid_` (2013) 无 16 的 case, `default: return false` → 会**拒 oct
[-1,1]**。故 16 **不能用 controlParamValid_ 门禁**, 否则 vco tune 的 oct 值域被误判 invalid。
→ 契约规定: **16 (panel-knob 族) 接纳一律走 `dspParamValid_`** (正是今天 route(3) 校验它的
那个), 满足 item 6 逐字且避免 oct 缺口。controlParamValid_ 仍专管 35 control-source
(step CV [0,5V]/boolean 精确/selector 等乐音语义域)。

---

## 2. Count 对账 (169 / 134 / 35)

- `169` = applied_to_dsp 总数 (exactly-169 gate)。
- `35` = control-source 参数 (route 1: `controlSourceParamRecognized_`, :1984)。其中 20 continuous
  (seconds 平滑) + 15 discrete。
- `134` = applyDspParam 显式 dispatch (`switch` :990–1395, `default:`→unsupported)。**已实测
  `case ParameterId::` 恰好 134 条**, 与 header 注释 :947 & @Kimi 口径一致。
- `169 = 35 + 134` ✓。
- 16 = 134 的子集, 仅经 route(3) 到达。

## 3. 相邻观察 (排除项, 非 16 范围)

- `vco_a_pwm`(registry :268) / `vco_b_pwm`(:273): 也声明 `Smoothing::seconds` + `ParamRole::knob`,
  但**不在 134 dispatch** 且不在 control-source 族。因 applied_to_dsp 恰为 169 且已被
  35+134 占满, pwm 必 **非 applied_to_dsp** → 任何 apply 路径落 route(2) `unsupported_parameter`。
  → 这是个**声明未接线**的既有缺口 (声明 seconds 却无 apply 路径), 与 16 (声明 seconds **且**已
  dispatched) 是两类。**归入后续/待 @Kimi 裁决**, 不并进本 slice。

---

## 4. 推荐落点 (candidate ii 具体接线, 供契约引用)

把 16 接入**既有共享平滑族** (只扩成员集, 机制本体不动):
1. `controlSmoothOrdinals_` 20 → 36 (构造时加入 16 id)。
2. `controlSmoothers_` 容量 20 → 36 (同 Surface-1 定长预分配, 禁 map)。
3. `applySmoothedControl_` (:2100) 加 16 case → 现有 setter (setVcoATune/setVcoAMorph/
   setVcoAPw/setVcoACvAmt/setVcoBTune/.../setDistortionAmount/setDistortionGain)。
4. `currentControlParam_` (:2132) 加 16 reader → 各 VCO/VCF getter (零跳变 priming)。
5. `setControlParamValue` (:1882) recognition gate 加 16 → live ControlEvent 可 ramp。
6. 16 的 live+s nap admission 走 `dspParamValid_` (:2171); **不动** `controlParamValid_` oct 缺口。
7. `applyDspParam` route(1) seconds-snap 分支 (:963): 因 16 现被 `controlSourceParamRecognized_`?
   —— **不**, 16 不进 `controlSourceParamRecognized_` (那是乐音 control-source 语义)。
   需在 applyDspParam 增加"先判 panel-knob-smooth 族, 走 snap分支"识别, 使 16 在 restore 时
   snap (reset+apply), 从 134 switch 移除。
8. 134 switch 移除这 16 case → 118; 35 control-source + 51 (35+16) = applied_to_dsp 仍 169。

> 注 (7)(8) 是唯一**结构新增**: 需要一个"panel-knob smooth 族"识别判定 (比如复用一个
> `paramSmoothingSet_` 或常量表), 但**机制仍是共享的那种**, 无第二套 smoother。命名语义:
> 16 是 `ParamRole::knob` 而非乐音 control-source, 故把它们并入"continuous smoothing 族"而不是
> "control-source 集"。@Kimi 硬约束是"严禁第二套平滑实现", 此接线满足。

---

## 5. 提交 @Kimi 裁决的 3 个点

1. **candidate (ii) 是否采纳** (live=ControlEvent ramp, restore=applyDspParam snap; 与 Surface-1 分层一致)。
2. **16 纳入共享平滑族** (成员 20→36, 机制复用) 是否 OK —— 满足"严禁第二套"。
3. **16 的 acceptance 走 `dspParamValid_` 而非 `controlParamValid_`** (oct 值域 + item 6 逐字不绕过)。

## 6. 排除项 / 边界 (本 slice 不做)

- 不实现/不 merge/不关 GH#21/不发布/MET。只读取证 + 契约草案。
- Surface-2 不含 vco/vcf 之外的任何新增平滑; 不含 pwm 缺口 (见 §3)。
- 不新增 ParameterId, 不改 registry/wire schema (GH#12/#6 不动)。
- 16 全部为软件 PROVISIONAL (未测硬件校准); 不发明常数。

---

## 附: 关键 file:line 索引 (证据)

| 项 | 位置 |
|---|---|
| applyDspParam 4-route 契约 | machine_runtime.h:943–952 |
| 16 dispatch (switch 990–1395, 134 case) | :990–1395 |
| seconds-snap 分支 | :963–979 |
| applyDspParam 唯一 restore caller | :1427 (在 applyDspState) |
| applyDspState exactly-169 gate | :1409–1459 |
| applyControlEvent_ (live lane) | :1820–1866 (`default: setControlParamValue`) |
| setControlParamValue (recognition gate) | :1882 |
| controlSourceParamRecognized_ (35) | :1984–2006 |
| controlParamValid_ (35 乐音域, default:false) | :2013–2069 |
| advanceControlSmoothing_ | :2075–2093 |
| applySmoothedControl_ (20 ids) | :2100–2125 |
| currentControlParam_ (priming) | :2132–2157 |
| dspParamValid_ (registry 通用, route3 接纳) | :2171–2180 |
| registry 16 行 | generated/registry.hpp:261–286 |
| design §3.2 ramp 权威 | design/07:71 |
| UI 不直写 DSP / ControlEvent 路径 | design/07:17, :62 |
| host→core bounded param seam | design/03:41 |
| `kMaxControlSmoothParams=35` + "sized generously" 注释 | machine_runtime.h:277 (:430 cap, :3017 controlSmoothOrdinals_) |
| `controlSmoothers_` (按 id 空间定长, O(1)) | machine_runtime.h:3016 `std::array<ParameterSmoother, registry::kParameterIdSpace>` |
| 构造集注释 "AND are Surface-1 control-source params" | machine_runtime.h:3013–3015 |
| six-field non-unverified (Codex 644ea86e Root 2, selector-only) | tools/check_registry_complete.py:1868–1878 (+ selector 循环 :1832–1855) |
| `ParameterFieldEvidence` 6 字段 + anyUnverified() | core/include/lunar24/core/evidence_policy.h:59–70 |
| `valid_pfe` (每字段 confirmed/unverified/provisional) | tests/core/test_registry.cpp:56–60 |
| unknown-enum biconditional (不含参数/smoothing) | tests/core/test_registry.cpp:76–95 (仅 jack signalType/polarity/coupling + program family/selfOscillating) |

---

# REV-2 终审稿 — after @Kimi 裁决 (msg ca89af98) + 4 补充要求

## 裁决采纳摘要

- **① candidate (ii) 采纳** — 契约强制 (design/07:17 已钉死 UI→ControlEvent 车道; applyDspParam 保持 snap-only)。候选 (i) 改签名破坏 Surface-1 边界, 否; (iii) 散乱开关, 否。
- **② 16 纳入共享平滑族 (20→36), 机制本体零新增** — 带一项落地修正:
  - **`kMaxControlSmoothParams=35` 须扩容** (36>35, 现注释 "sized generously" **失实** — 它恰是 control-source 总数, 非宽松; 36 会触发 :430 defensive cap)。改定长值 (建议 40) + 把注释改成真实依据。**已验证**: :277 常量, :430 cap, :3017 `std::array<ParameterId, kMaxControlSmoothParams> controlSmoothOrdinals_`。
  - **`controlSmoothers_` 无需改** — 它是 `std::array<ParameterSmoother, registry::kParameterIdSpace>` (:3016), 按 id 空间定长 O(1) 索引, 已覆盖全部 id。只改 controlSmoothOrdinals_ 容量。
  - 注释 :3013–3015 "AND are Surface-1 control-source params" 须同步改为 "continuous smoothing 集 (control-source + panel-knob)"。
- **③ 16 验证走 `dspParamValid_` (:2171), 不走 `controlParamValid_`** — 正确 (后者 default:false 拒 oct[-1,1])。**且必须带 Surface-1 教训: dispatch 顺序 = 先验证、后 reset+apply, 任何路径不得跳过 fail-closed**。

## 4 补充要求落地 (均已取证)

### 1. ControlEvent 识别集扩展
`controlSourceParamRecognized_` (:1984) 只认 35 control-source; 16 走 ControlEvent 车道须显式扩展识别, **按 `Smoothing::seconds` + applied_to_dsp 门控, 而非手写 16-id 清单**。契约钉死这个扩展点: 新增一个 "continuous smoothing set" 判定 (作用于已具名 seconds + applied_to_dsp 的参数), 供 setControlParamValue 识别门 / applyDspParam route(1) snap 门 / 平滑族构建三处统一使用。**不并入 controlSourceParamRecognized_ 语义** (那是乐音 control-source, 16 是 panel-knob, 命名与语义须分离, 但机制是同一个)。

### 2. fieldEvidence.smoothing 翻转 (unverified→provisional) — 取证结果
- **2a — 16 在 target.landedDescriptorFacts 282 条里的分布**: **仅 2 个在**: `vcf.r_res`、`vcf.r_mod`。→ 这 2 个须 target 侧同步重冻结 (独立 commit, 动因写清, 同 GH#21 先例)。**其余 14 个不在**该集 (target.landedDescriptorFacts.parameters 只含 Phase B mods-gap 闭合事实片; vco/vcf 常规参数大多不在), 记录原因, registry 侧翻转独立, 无需 target 重冻结。**已验证**: `target.landedDescriptorFacts.parameters` len=282 (`target` sub-object, 非顶层), 16 中以 dot 名查得 in= [`vcf.r_res`,`vcf.r_mod`]。
- **2b — 六字段 non-unverified 规则是否覆盖 knob**: **不覆盖**。规则 (Codex 644ea86e Root 2) 位于 **selector-toggle 循环内** (check_registry_complete.py:1868–1878, 前置 `if not tpos: continue` :1850), 只强制 selector; 16 全为 **knob/continuous** (step==0, `ParamRole::knob`), 不在该规则覆盖内。因此 **翻转非机器强制** — 是 manual discipline (镜像 Surface-1 @Kimi 裁决), 非 gate 会拒绝 unverified 的情形。
  - ⚠️ 落账: 本翻转是 **smoothing 值域的 evidence 状态变更, 非 smoothing 值变更** (16 早已 `Smoothing::seconds`) → **count 177/168 应不变** (@Kimi 补充要求 4 印证; 若变=越界)。

### 3. pwm 排除项落账
`vco_a.pwm`/`vco_b.pwm` = 声明 `Smoothing::seconds` 但 **transfer_unavailable** (不在 134 dispatch 且非 applied_to_dsp → 任何 apply 路径落 unsupported)。记录为**既有缺口归后置** (可能在 GH#15 范围), 本 slice 不并。**已验证**: registry :268/:273 有, 134 dispatch 无 (134 实测恰含 16, 不含 pwm)。

### 4. 验收沿用 Surface-1 形态 + count 钉死
- T1 轨迹/tau窗口、T2 反旧帧、T3 分区一致(64/128)、T4 零分配、**bypass 负控必红**、范围外逐字节一致回归锁。
- **count 断言钉死新值 177/168 不变** (本 lane 无 smoothing 值变更, 只有 evidence-status 变更); 若变=越界红。
- 回归锁: exactly-169 gate, count 129 回归, GH#12 probe 8/8, 三腿 Release/Debug/ASan+UBSan, 4-platform CI。唯一允许红 = full-coverage (12 键盘缺口, by-design)。

## 实施任务 #94 (待 @Kimi 终审通过后另立) — 范围
- (a) `kMaxControlSmoothParams` 扩容 + 注释; :3013–3015 注释同步。
- (b) 平滑族构建集 20→36 (加 16 panel-knob)。
- (c) `applySmoothedControl_` +16→现有 setter; `currentControlParam_` +16→现有 getter。
- (d) `setControlParamValue` 识别门扩展 (seconds + applied_to_dsp 门控)。
- (e) `applyDspParam` route(1) 加 panel-knob-smooth snap 分支 (走 `dspParamValid_`), 从 134 switch 移除 16 (→118)。
- (f) 16 `fieldEvidence.smoothing` unverified→provisional (registry); `vcf.r_res`/`vcf.r_mod` target 重冻结。
- (g) 验收 T1–T4 + bypass 负控 + 回归 + 三腿 + 4-platform CI。
- **排除**: 不新增 ParameterId; 不改 registry/wire schema (GH#12/#6 不动); 不关 GH#21/发布/MET; pwm 缺口后置。
