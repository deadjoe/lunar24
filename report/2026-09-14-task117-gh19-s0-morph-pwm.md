<!-- SPDX-License-Identifier: Apache-2.0 -->
# task #117 — GH#19 S0：连续波形映射与双侧 PWM 真实产品接入

- 分支 `feat/19-morph-pwm-product`，隔离 worktree `wt-117-gh19-morph-pwm`，基线 **`cc92b74`**（= `origin/main` tip，#111 S5 的 merge）。
- 授权：@Codex 任务卡 `9e4d9bb8`（线程 `#Lunar24:9e4d9bb8`），实施契约 `a323acad`；追加硬约束 `04da0901`（两条三角测量口径）。
- 复用 #116 已核对候选 `d8ed4c7` 的结论；**未整包搬入**已废弃的 B 实验与错误推导。
- 工作树状态：**HEAD 仍 = `cc92b74`，全部改动未 commit、未 push**。改动集 = **19 个已跟踪文件 + 3 个新文件 + 本报告**（§1.9）。
- **状态口径：GH#19 仍 OPEN；未 push / 未开 PR / 未 merge / 未关 GH#19 或 GH#15 / 未发布 / 未宣称 MET。** S3 / S6 **未宣称完成**（§9）。

## 0. 证据口径

按 `report/README.md`：`[实测]` = 本轮真跑出的数；`[代码]` = 读源码得到的事实；`[手册]` = 依据文档/registry；`[推断]` = 未直接验证的解释。**未标注者一律 `[推断]`。**

所有 `[实测]` 数字都经**唯一约定入口**产出：`encode_device_state -> decode_device_state -> StandaloneAudioEngine::applyDeviceState -> StandaloneAudioEngine::processBlock`（`tests/host/test_engine_harness.h`），断言打在**真实输出通道**（WET_L/WET_R/DRY_A/DRY_B）与只读回读面上。**没有任何一条判据走 `buildMachineRuntimeCandidate`/`processFrame` 直连路径。**

## 1. 改动是什么

### 1.1 波形映射：单一连续旋钮（裁决 ①）

**新文件** `core/include/lunar24/core/vco_wave_map.h`：节点环 `saw / 反相 saw / 正弦 / 三角 / 脉冲`，坐标 `{0, .25, .5, .75, 1}`（`kRingEqual`），相邻节点以**线性等权（partition of unity）**混合 `sampleAt(ring, morph, phase, duty)`；`triangleWeight(ring, morph)` 给出三角节点在当次混合中的权重；`morphSawInvSaw` / `morphSineTriangle` 保留**既有闭式**（旧 morph 形状的逐字表达式）。`[代码]`

**`vco.h`**：
- 新增 `VcoWaveform::kMorphRing` = **生产渲染律**，并把它设为**构造默认**（`wave_ = kMorphRing`，原 `kTriangle`）。 ⇒ **映射是渲染律而不是 opt-in：构造一个 Vco 就生效，产品路径不需要任何额外调用。** `[代码]`
- 其余六个枚举值降级为**模块开发用原始波形接口**（注释与枚举成员名都写死这一身份）：只有显式 `setWaveform()` 能到达，**产品路径无任何调用者**；保留它们只为模块自身单测能逐个隔离波形。**没有任何运行期开关可以切回 #117 之前的固定三角行为**（裁决 ②）。`[代码]`
- `emittedAt_()` 的三角 BLAMP 由 `triangleBlampWeight_()` 加权：`kTriangle`（模块原始三角）=1.0（与 #117 之前**逐位相同**）；`kMorphRing` = 三角节点权重（纯三角节点 0.75 处恰为 1.0，stretch 2/3 之外为 0.0）；其余波形 0.0（不变）。`[代码]`
- 脉冲节点的 duty 改读 `effectiveDuty()`（= `basePW + depth*cv/10` 后夹紧）；**depth 默认 0 时与 `duty_` 逐位相同**。`[代码]`
- 相位约定、CV 律、sub/sync 时序**未动**。

### 1.2 PWM 消费者（裁决 ③）

`vco.h` 新增：`setPwDepth/setPwCv/pwDepth()/pwCv()/effectiveDuty()`，以及
```
effectiveDuty = clamp(basePW + depth * cvVolts / 10, 0.001, 0.999)
```
- ⚠️ **软件暂定传递，不是硬件事实**：`/10` 使插孔标称 ±5 V 在 depth 1 时贡献 ±0.5 占空比；**正 CV 抬高占空比**。注释里写明这一点（`vco.h`），本报告 §4 复述同一口径。
- **两个输入是彼此独立、时序不同的两个量**：`setPwDepth` 是**被平滑的旋钮深度**（走 GH#21 秒级平滑家族，由 runtime 平滑，DSP 类不平滑）；`setPwCv` 是**本采样从 patch graph 读到的 CV**，**故意不平滑**（平滑会低通 patch 并破坏同帧消费契约）。
- 两者都**不写 `duty_`**（basePW 的规范值不被覆写）。**depth = 0 ⇒ 输出与 #117 之前逐位相同**。
- 非法值按既有数值保护契约处理：非有限 depth/CV 记 0，非有限和回退 `duty_` ⇒ **无 NaN 透传**。`[代码]`

`machine_runtime.h` 新增 `setVcoPwmBindings`、每侧 `pwmInA_/pwmInB_` + `*Bound_` 标志（**标志而非 `JackId{0}` 哨兵才是准入态**，因为 `JackId{0}` 是真实插孔）、`setVcoAPwm/B`、回读 `vcoAPwm/vcoBPwm/vcoAPwmCv/vcoBPwmCv/vcoAEffectiveDuty/vcoBEffectiveDuty`；并在 `kVcoA`/`kVcoB` slot 内、`vcA_.tick()` **之前**加入逐采样消费：
```
double pwm = 0.0;
if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
vcA_.setPwCv(pwm);
```
**复用既有 sink 解析/回读机制（与 v_oct / cv_in / sync 消费者同一套），不绕过 graph 调度器**；因此用户改线/拔线的语义与那些插孔完全一致。`[代码]`

### 1.3 自动产品接线（裁决 ②）

`machine_definition.h` 内 `runtime_.setVcoPwmBindings(JackId::vco_a_pwm_in, JackId::vco_b_pwm_in);`。
**这是自动生效的产品入口**：不依赖测试恢复之后再去调 `setWaveMap`/`setVcoPwmBindings`。**不新增插孔、不新增 route**：20 / 22 两个插孔本来就是已注册产品插孔，这里只是把它们绑到既有执行/求解机制上；A、B 是两个**彼此独立**的插孔。`[代码]`

### 1.4 disposition 改判（裁决 ④）

`state_disposition.h`：`vco_a_pwm`(8)、`vco_b_pwm`(30) `transfer_unavailable → applied_to_dsp`；编译期 `static_assert` 随之钉死为 **183 → 185**、**2 → 0**，总数 **345 不变**。**冻结描述符值与 `fieldEvidence` 未动**（本轮未改 registry 里的任何描述符字段）。`[代码]`

### 1.5 数据与清单（裁决 ⑤）

- `spec/machine/p0_inventory_manifest.json`：**只做定向修正** `target.controls[101]` / `[112]` 的身份与分类 —— before `{"stable_id": "vco_a.wave", "kind": "selector-toggle", "status": "confirmed"}` / after `{"stable_id": "vco_a.morph", "kind": "continuous", "status": "confirmed"}`（`vco_b` 同）。该位置在修正前是**任何 target 数组里都不存在的 id**（`panelControls[101]` 与 `parameters[101]` 早已是连续量 `vco_a.morph`），修正后三处一致。**未向 `target.parameters` 增加条目**（`parameters` 仍是 357 项，present 345 项）。`[实测]`+`[代码]`
- **依赖盘点（先核依赖再改）**：改后重跑 `python3 tools/check_registry_complete.py` ⇒ **RC=0**，`parameters: target=357 present=345`，**12 项 gap 逐条不变且全部是 `keyboard.*`**（8 non-scalar + 4 no-value-domain，见 §6）。**原 12-gap 的含义与构成未被动过**。`[实测]`
- ⚠️ **未动** `tools/check_registry_complete.py:202` 的 `LEGACY_ROGUE_PARAM_CEILING`（含 `"vco_a.wave"`）：那是**registry 侧独立冻结的祖父上限集合**，与 manifest 无关，且其 shrink-only 语义要求原样保留。`[代码]`
- `tools/gh19_manifest.tsv`：`vco_a_tri_*` / `vco_b_tri_*` 的 **`consumer` 与 `source_evidence` 两列描述**从「default triangle wave」改为「the continuous morph map 的三角节点（`morph=0.75`）」并点名 GH#19 S0；**`id` / `path` / `sr` / `stim` / `part` / `domain` / `required` 各列逐字节未动**（`stim` 是数值刺激列：220/440/880 全数保留）。（§2.4 说明为什么必须改描述而不是删 gate。）
- `design/00-status.md`：只加入本片的**明确软件裁决与默认变更**（①连续单旋钮、②默认三角→正弦、③PWM 软件传递、④disposition 计数），并显式声明**不改任何阶段的 MET / NOT MET**、**不改上面基于 `origin/main` 的事实快照**、GH#19 仍 OPEN。

### 1.6 验收面（第 ⑤ 步）

**新文件** `tests/host/test_gh19_s0_morph_pwm_acceptance.cpp`（1152 行，**82 项检查**，14 个段落）。⚠️**读源码时会数到 92 条 `CHECK(` —— 差额 10 条是「前置条件守卫」**：形如 `if (!h.load(...)) { const bool xRenderCommitted = false; CHECK(xRenderCommitted); return; }`，**只在** load/render 失败时触发（绿跑时不计入），这样失败信息里出现的是**具名判据**而不是一句 `false`。**92 − 10 = 82** 与 summary 的 `82 checks OK` 自洽。，注册进顶层 `CMakeLists.txt`（`lunar_add_test` + `lunar_host` + `tests/core` include），并复用 `tests/host/test_state_apply_oracle_allocator.cpp` 作为该 target 的第二个 TU（零分配探针需要被替换的 `operator new/delete` 与调用点**分处不同 TU**，否则触发 GCC `-Wmismatched-new-delete`）。`[实测]`

### 1.7 定向负控 runner（第 ⑤/⑥ 步）

**新文件** `tools/run_gh19_s0_mutants.py`：**13 条隔离生产突变**（8 条判据臂 + 5 条验收面臂），每条都是对**生产头文件**的一处聚焦改动，在**隔离 shadow include 树**里编译（`-I <shadow>` 优先），**committed 树从不被写**。`[实测]`

### 1.8 既有测试的同步加固

`test_vco.cpp`(+301)、`test_machine_definition.cpp`(+268)、`test_state_apply_oracle_169.cpp`(+141)、`test_state_disposition.cpp`、`test_state_validation.cpp`、`test_machine_control_sources.cpp`、`test_machine_audio_families.cpp`、`test_vco_normal_source.cpp`、`tests/probes/gh19_alias_probe.cpp`、`test_state_apply_oracle.cpp`（仅注释）。**无文件删除**（`git diff --diff-filter=D` 为空）。

**关于 9 处被删的 `CHECK` 行（逐条核过，没有一条是「删 gate 换绿」）**：它们全是**随 disposition 改判必须同步的计数/分类断言**，且**每一处都有对应更新后的替身**：
- `count_disposition(applied_to_dsp) 183 → 185`、`appliedCount 183 → 185`、`n == 183 → 185`；
- `rdCount 148 → 150`（= 新增两条 PWM 深度的回读），且 **`rdCount + ctCount == appliedCount`（150 + 35 = 185）这条内部一致性断言原样保留**；
- `dspAppliedCount() < 183 → < 185`；
- `vco_a/b_pwm == transfer_unavailable` → `== applied_to_dsp`（**两条一起**，裁决 ④ 要求成对移动）；
- 「不可用集」从 2 元 pin 改为 **空集，但空集是被断言的**：`kUnavailableCount == 0u` 单独断言一次，且原「不是 hole/slack id」的遍历改成对这两个 id 的**正向**断言（`is_landed_parameter` 为真、`disposition != invalid_unlanded`）——所以「重新把一个 id 登记为不可用」必须回来改这里，而不是静默通过；
- T4 零分配探针的消息串从写死 `36 smoothers` 改为按 `famSize` 动态打印（38），**断言本身（`after == before`）未动**。

### 1.9 改动集汇总

19 个已跟踪文件（`git diff --shortstat`：**19 files changed, 1137 insertions(+), 140 deletions(-)**）+ 3 个新文件（`core/include/lunar24/core/vco_wave_map.h` 225 行、`tests/host/test_gh19_s0_morph_pwm_acceptance.cpp` 1152 行、`tools/run_gh19_s0_mutants.py` 474 行）+ 本报告（**全改动集 `git diff --cached --shortstat`：23 files changed, 3339 insertions(+), 140 deletions(-)**）。**`build-117-rel/` 为未跟踪的本地构建目录，不在改动集内、不会 commit。**`[实测]`

## 2. 波形映射（裁决 ①②）

### 2.1 五节点签名（四个采样率）`[实测]`

聚合口径是**相位鲁棒的整周期聚合**（首个→末个实测上升沿之间），所以不依赖捕获起点落在周期何处：

| 采样率 | saw (meanAbs, medSlope) | 反相 saw | 正弦 (meanAbs, rms) | 三角 (meanAbs, pp) | 脉冲 (meanAbs, rms) |
|---|---|---|---|---|---|
| 44.1 k | 0.25000, +0.00499 | 0.25000, −0.00499 | 0.31830, 0.35355 | 0.25001, 0.99593 | 0.50000, 0.50000 |
| 48 k | 0.25001, +0.00458 | 0.24999, −0.00458 | 0.31832, 0.35356 | 0.25000, 0.99628 | 0.50000, 0.50000 |
| 88.2 k | 0.25000, +0.00249 | 0.25000, −0.00249 | 0.31831, 0.35355 | 0.25000, 0.99798 | 0.50000, 0.50000 |
| 96 k | 0.25001, +0.00229 | 0.25000, −0.00229 | 0.31831, 0.35356 | 0.25001, 0.99814 | 0.50000, 0.50000 |

三个可判读的结构事实：**①** 反相 saw 与 saw 的 `medSlope` 严格反号（互为镜像）；**②** 正弦是**唯一**的高负载节点（`meanAbs ≈ 2/π·0.5`、`rms = 1/√2·0.5`），故「默认落在正弦」是可听差异而不是记号差异（§2.3）；**③** 三角是**唯一** pp < 1 的节点（BLAMP 圆峰），且 pp 随采样率单调趋 1。`[实测]`

### 2.2 连续性 `[实测]`

以 **0.01 步长**细扫 0..1，取相邻格的最大变化：四采样率下 `worstStep = 0.020000 / 0.020001 / 0.020000 / 0.020001`（判据阈值 0.05 ⇒ **2.5× 余量**）。离散化的「映射跳变」会把该值推到 ~0.5 量级（§7 `acc-mapping-jump` 实测确实把它打红）。`[实测]`

**本旋钮的连续行为已由 owner 确认**（裁决 ①）—— 它**就是**连续控制，**不存在「是否另有离散选波」这个待证项**。**仍属软件暂定的是精确曲线本身**：节点顺序与坐标向量 `kRingEqual` 是软件选型，**没有硬件/面板依据**，本片也不附带任何硬件性主张（#115 已撤回全部硬件性结论）。`[代码]`

### 2.3 默认音色变更（有意，且被单独断言）`[实测]`

morph 默认 `0.5` 落在**纯正弦节点** ⇒ 默认音色由三角变正弦。三个「未变」：**wire 字节、参数 ID、默认值本身**。但**声音确实变了**，而且这条断言是**与字节断言分开写的**：

- 字节侧：`morph 0.50 → 0.25` 在保存镜像上只动 **1 个字节 @ [38..38]**，且**全部差异字节都落在同一个 8 字节 f64 参数槽内**、槽下标的前移量**恰等于 ParameterId 之差**（`b_morph` 在 `[110..110]`，id 差 9 ⇒ 槽差 9）；PWM 深度在 `[94..95]`（id 差 7 ⇒ 槽差 7）。**这些结构断言是从实测读出来的，不是我先验假设的字节布局**（首版曾按「原始 f64 镜像」建模而 4 条检查假红，已改）。`[实测]`
- 数值侧：同一改动经**生产者自己的解码器** `decode_device_state` 解回 `0.25`。`[实测]`
- 声音侧：同一改动的音频最坏差 = **0.500000**（> 1e-6），即**保存字节的变化同时也是一次真实音色变化**，两者分别断言、互不代证。`[实测]`

**saw ↔ 反相 saw 中点静音保持实测样态，不隐藏补增益。**`[实测]`

### 2.4 保留旧 gate 的方式（不是删 gate 换绿，也没有动历史参考）

@Codex `04da0901` 的两条硬约束，逐条对账：

1. **「A/B 各设自己的 morph=.75」**——别名探针三处**各自**显式设定：`vco_a_tri` 臂设 `vco_a_morph = 0.75`，`vco_b_tri` 臂设 **`vco_b_morph = 0.75`**（**不是只设 A**；且该臂同时保留 `vco_b_cv_amt = 0` 以隔离默认 A→B 调制），sync 臂的载波也设 `vco_a_morph = 0.75`。`[代码]`
2. **「『参考若位移就改参考』不在授权内」**——**本轮没有改动任何数值参考文件**：`git status` 显示被改的参考类文件只有 `tools/gh19_manifest.tsv`，且**只动描述列**（§1.5）；`tools/gh19_hardsync_naive_baseline.tsv` 与各 acceptance 参考文件**逐字节未动**。`[实测]`

**定位思路与实测结果**：纯三角处（ring 坐标 0.75）三角节点权重恰为 1.0、其余 law 不变 ⇒ **预期历史数值可复现**。所以本片先把**刺激**钉成历史刺激（显式 `morph = 0.75`），而不是去改参考；历史三角/BLAMP 与 S5 门禁在**同一实际波形/同一频率/同一参考**下重跑，**全绿且无参考改动**（§5.4：全量 ctest **86/86 通过**，四条历史 gh19 门禁各自 ~1150 s 全绿；四份历史基线 TSV 与 `cc92b74` **逐字节相同**）。若当初真的复现不出，正确的下一步是**定位差异来自刺激、分析器还是生产行为**并并列新增基线交裁决，而**不是**覆盖原基线——本片没有走到那一步，也没有覆盖任何基线。`[实测]`

**结论**：旧三角 / BLAMP 与 S5 门禁**保全未删**（`git diff --diff-filter=D` 为空、9 处 CHECK 更替逐条核过，§1.8），新增的 pulse / mixed 路径只作**未改善基线**，**不套用三角指标**。`[实测]`

### 2.5 混合 / 脉冲 / saw 路径：未改善的基线

`kRingEqual` 的混合段、脉冲节点与 saw 节点**没有任何新的抗混叠**：本片只搬入映射与消费接线，**不宣称 S3 / S6 完成**。§2.1 的表就是它们的**基线**（脉冲 `meanAbs = rms = 0.5`，无带限；混合段无带限）。`[实测]`

## 3. PWM（裁决 ③④）

### 3.1 直流 CV 律（四采样率）`[实测]`

`joystick.x_out → vco_a.pwm_in`，depth 1，扫电压：最坏 `|audio − law| = 0.0022`；该最坏格是 `n = 0.00` 端点，其整个高电平时间只有 ~218 帧周期的 0.001（**亚采样**）⇒ 容差按**实测的一采样周期长度**给出：`1.5/(sr/220) + 1e-3`。`[实测]`

### 3.2 反相与饱和 `[实测]`

`−5 V → 0.001665`（夹紧下界附近）、`base = 0.500000`、`+5 V → 0.998380`（夹紧上界附近），即**负 CV 降占空比、正 CV 抬高占空比、两端都被夹紧窗口（0.001 / 0.999）截住**。用 0..10 V 的单极性 LFO 驱动时在 depth 1 下**饱和到 0.999**（graph 级夹紧）。`[实测]`

### 3.3 时变 CV：逐周期跟踪（四采样率）`[实测]`

`lfo_a.cv_out → vco_a.pwm_in`（`lfo_a.rate = 5.0` ⇒ **实测 5.0000 Hz**，9600 帧周期），depth 0.1：

| 采样率 | 周期数 | 最坏逐周期 gap | 实测 duty 区间 |
|---|---|---|---|
| 44.1 k | 109 | 0.00341 | 0.5000..0.6000 |
| 48 k | 108 | 0.00412 | 0.5023..0.6009 |
| 88.2 k | 109 | 0.00248 | 0.5000..0.6000 |
| 96 k | 108 | 0.00206 | 0.5011..0.6009 |

判据容差 0.02（实测最坏 0.005，**4× 余量**）。duty 全程**未饱和**，走满 0.1 的整个摆幅。`[实测]`

### 3.4 depth 0 严格不变 `[实测]`

带**活线**（源真的在动：实测源跨度 9.9979 V）而 depth = 0 时：`duty = 0.499937`，离散度 0.004566 = **恰好一个 ~218 帧周期的单个采样**（即纯量化残差，不是调制）。⇒ **depth 0 严格惰性**，且此结论**不是**靠「没有连线所以读 0」得到的。`[实测]`

### 3.5 depth 0 / mid / 1 三档 `[实测]`

同一 +2.5 V 刺激下：`d0 = 0.499937`、`d0.5 = 0.624974`、`d1 = 0.750001`（期望 0.625000 / 0.750000）⇒ 三档单调、中点恰为一半调制、满档恰为整个调制。`[实测]`

### 3.6 同帧消费（独立边沿/索引裁决，不是残差）`[实测]`

`vcoA.pwm` 的**本采样读回值**与**同一帧发布的** `controlVoltageAt(lfo_a_cv_out)` 相等 **9599/9599** 帧，与**前一帧**的值相等 **0/9599**。（早期探针另测 4799/4799 与 0/4799。）

⇒ 这条判据用的是**同帧/异帧计数**这种对「整体平移一格」敏感的独立索引，而不是对绝对时序免疫的残差指标；**一个「晚一采样消费」的突变会同时翻转两个计数**（§5.4 第 11 条 `acc-cv-one-sample-late` 实测把它打红，且只打红 2 条检查）。`[实测]`

### 3.7 A / B 独立（含「默认 A→B 是真路由」的诚实声明）`[实测]`

- 只驱动 A 时：`cvA = 5.0000 V`、`cvB = 0.0000 V`、`depthB = 0.2500`（**B 自己的深度没有被 A 的线改动**）。`[实测]`
- A/B 音频节点互相独立：`A(morph=0.75) → (meanAbs 0.25000, pp 0.99628)`，而 B 保持 `0.50000`（脉冲节点）；回读 `A = 0.750 / B = 1.000`。`[实测]`
- **默认 A→B 路由是合法活路由**：B 的音频高电平占比 = `0.249999`，而 B 的回读 duty = `0.500000` —— **两者不相等是正确的**（B 被 A 以音频速率调频）。**隔离 B（`vco_b.cv_amt = 0`）后**才做干净的 duty 声明：`audio = 0.499937` vs `readback = 0.500000`。⇒ 全文把默认路由的调制**断言为合法**，**从不**称其为「无串扰」。`[实测]`

### 3.8 改线 / 拔线 / 复位 / 失败原子性 `[实测]`

- 接线后渲染变化：`|base − patched| = 8704.000000`；**拔线后逐位复原**：`|base − restored| = 0.000e+00`（严格 0）。`[实测]`
- 未接线时 sink **读恰好 0**，且 basePW 的 duty **不变**。`[实测]`
- 反复复位走 **setter**（不是只让字节活在某处）：`repeatedRestoreReachesThePwmDepthSetter`。`[实测]`
- 被拒的 apply 是**原子**的：两个引擎先渲染同一窗口，其一投一个越界状态被拒，之后**两条 trace 逐位相同**；`sampleRate/blockSize/inputCapability/outputCapability`、`plan().valid()/outputCapability/outputCount/inputCh[0..1]` 全部保持。`[实测]`

### 3.9 非规则块 / 四输出有限 / 回调零分配 `[实测]`

- 16 段非规则分块 `{7,13,32,64,100,441,512,37,128,91,512,512,512,512,512,111}`（和 = 4096，均 ≤ 512）与均匀分块**逐位相同**。`[实测]`
- 四采样率 × morph 5 档 × depth 3 档 = **每率 15 格**，四输出全有限。`[实测]`
- `processPure` 在调用方预分配的 planar 缓冲上跑 **24 个测量块**：零 `new`、零 `free`；并带**阳性对照**（探针能测到一次分配/一次释放），所以绿灯不是死探针。`[实测]`

## 4. 裁决 ⑤ 的依赖核查与 before/after

| 项 | before | after | 依据 |
|---|---|---|---|
| `target.controls[101]` | `vco_a.wave` / `selector-toggle` / confirmed | `vco_a.morph` / `continuous` / confirmed | `[实测]` §1.5 |
| `target.controls[112]` | `vco_b.wave` / `selector-toggle` / confirmed | `vco_b.morph` / `continuous` / confirmed | `[实测]` §1.5 |
| `target.parameters` 条目数 | 357 | 357 | `[实测]` checker |
| present 参数数 | 345 | 345 | `[实测]` checker |
| 12 项 gap 构成 | 8 keyboard non-scalar + 4 keyboard no-value-domain | **完全相同** | `[实测]` checker |
| 冻结描述符值 / `fieldEvidence` | — | **未动** | `[代码]` |
| 阶段 MET | — | **未动** | `[代码]` |

**未发现新增的冻结机制冲突**（对 `04da0901`「依赖盘点收到，未见新增冻结冲突」的复核维持不变）。`[实测]`

## 5. 定向负控 runner

### 5.1 判红口径（修掉 #116 runner 的 rc 缺口）

**参考臂**：必须 `rc == 0` **且** 打印**完整** summary **且** 0 失败 —— 三者缺一即 INVALID。
**突变臂**：必须 `rc == 1`（正常失败退出）**且** summary 完整**且** 在具名失败行里命中**该突变自己的判据**。
**编译失败 / 信号退出（rc ≥ 128 或负）/ rc 0 / summary 截断 / 栽在别的检查上 ⇒ 一律 INVALID，不算红。** `[实测]`

### 5.2 八条判据臂（对单元级见证）

| 突变 | 目标 | 具名失败判据 | 证明的声明 |
|---|---|---|---|
| `fixed-old-triangle` | `test_vco` | `v.waveform() == core::VcoWaveform::kMorphRing` | 渲染律是连续映射，不是改前的原始三角 |
| `wrong-side` | `test_vco` | `worstSaw < 1e-15` | 每个节点保持其文档形状公式 |
| `mapping-jump` | `test_vco` | `maxStep < 0.05` | morph 在 0..1 上连续 |
| `missing-pwm-consumer` | `test_machine_definition` | `consumes the source's published value` | 打上线的 PWM 插孔经真实 graph 到达自己的 VCO |
| `wrong-side-cable` | `test_machine_definition` | `reads B's OWN source, not A's` | 两个 PWM 插孔互相独立 |
| `depth0-still-modulating` | `test_vco` | `v.effectiveDuty() == canonical` | depth 0 不改变任何输出样本 |
| `cv-one-sample-late` | `test_machine_definition` | `same_frame_edge` | 图 CV 在**发布它的同一帧**被消费 |
| `restore-skips-the-setter` | `test_state_apply_oracle_169` | `restore_keeps_pwm_depth` | 复位**重新到达 setter**（不是字节侥幸存活） |

### 5.3 五条验收面臂（对端到端验收面）

| 突变 | 具名失败判据 | 结果 |
|---|---|---|
| `acc-mapping-jump` | `morphSweepIsContinuous44k1` | RED（4/82 失败） |
| `acc-missing-pwm-consumer` | `pwmCvIsConsumedInTheSameFrameItIsPublished` | RED（21/82） |
| `acc-cv-one-sample-late` | `pwmCvIsConsumedInTheSameFrameItIsPublished` | RED（**仅 2/82**） |
| `acc-depth0-still-modulating` | `depthZeroIsStrictlyInert` | RED（11/82） |
| `acc-restore-skips-the-setter` | `repeatedRestoreReachesThePwmDepthSetter` | RED（21/82） |

⇒ **验收面本身是可证伪的**，而不是「只跑绿过」的摆设；`acc-cv-one-sample-late` 只打红 2 条检查，说明同帧判据是**尖锐**的而非大面积误伤。`[实测]`

### 5.4 全量运行结果（原样跑完，日志在 `/tmp/d117_mut_full.log` / `/tmp/d117_ctest_all.log`）

**（a）13 条突变全量矩阵** —— 参考臂 **4/4 GREEN**，13 条突变 **13/13 各中自己的具名判据**，**0 INVALID**（无编译失败 / 无信号退出 / 无跨判据误伤）：

```
[REFERENCE test_gh19_s0_morph_pwm_acceptance] GREEN  ('[test_gh19_s0_morph_pwm_acceptance] 82 checks OK',)
[REFERENCE test_machine_definition]           GREEN  ('[PASS] 226 checks, 0 failed',)
[REFERENCE test_state_apply_oracle_169]       GREEN  ('[test_state_apply_oracle_169] 2397 checks OK',)
[REFERENCE test_vco]                          GREEN  ('[vco] 133 checks OK',)
```

| # | 突变 | 目标 | 该突变的具名失败行 | 红了几条 |
|---|---|---|---|---|
| 1 | `fixed-old-triangle` | `test_vco` | `v.waveform() == core::VcoWaveform::kMorphRing` | 10/133 |
| 2 | `wrong-side` | `test_vco` | `worstSaw < 1e-15` | 1/133 |
| 3 | `mapping-jump` | `test_vco` | `maxStep < 0.05` | 1/133 |
| 4 | `missing-pwm-consumer` | `test_machine_definition` | `vco_a.pwm_in consumes the source's published value (graph-delivered CV)` | 11/226 |
| 5 | `wrong-side-cable` | `test_machine_definition` | `vco_b.pwm_in reads B's OWN source, not A's` | 6/226 |
| 6 | `depth0-still-modulating` | `test_vco` | `v.effectiveDuty() == canonical` | 11/133 |
| 7 | `cv-one-sample-late` | `test_machine_definition` | `same_frame_edge_A: …consumed in the SAME frame it is published` | 2/226 |
| 8 | `restore-skips-the-setter` | `test_state_apply_oracle_169` | `restore_keeps_pwm_depth` | 5/2397 |
| 9 | `acc-mapping-jump` | 验收面 | `morphSweepIsContinuous44k1` | 4/82 |
| 10 | `acc-missing-pwm-consumer` | 验收面 | `pwmCvIsConsumedInTheSameFrameItIsPublished` | 21/82 |
| 11 | `acc-cv-one-sample-late` | 验收面 | `pwmCvIsConsumedInTheSameFrameItIsPublished` | **2/82** |
| 12 | `acc-depth0-still-modulating` | 验收面 | `depthZeroIsStrictlyInert` | 11/82 |
| 13 | `acc-restore-skips-the-setter` | 验收面 | `repeatedRestoreReachesThePwmDepthSetter` | 21/82 |

runner 末行原样：

```
MUTANTS PASS: the reference completed cleanly (rc 0, complete summary, 0 failures)
and all 13 production mutations were rejected, each by its OWN named check.
```

**（b）全量 ctest（`build-117-rel`，Release）** —— **86/86 通过**，其中四条历史 gh19 门禁**各自全绿**（这是「旧 gate 未删、历史参考未动」的直接证据）：

```
86/86 Test #75: gh20_vcf_acceptance .................   Passed  447.08 sec
100% tests passed out of 86
Total Test time (real) = 1626.49 sec

Test #66: test_gh19_s0_morph_pwm_acceptance .........   Passed    8.20 sec   <- 本片验收面
Test #69: gh19_blamp_acceptance .....................   Passed 1151.56 sec   <- 历史三角/BLAMP
Test #70: gh19_schmitt_blamp_acceptance .............   Passed 1147.52 sec   <- 历史 S1
Test #71: gh19_classic_saw_acceptance ...............   Passed 1149.23 sec   <- 历史 S2
Test #72: gh19_hardsync_acceptance ..................   Passed 1150.95 sec   <- 历史 S5
```

四份历史基线 TSV 与 `cc92b74` 的 sha256 前 16 位**逐字节相同**（`gh19_naive_baseline` `7a5e5f063df5803d`、`gh19_schmitt_naive_baseline` `9d13069ea2dd8608`、`gh19_classic_saw_naive_baseline` `abf0976d07ccb490`、`gh19_hardsync_naive_baseline` `3f879ade9e1a3c06`）⇒ 绿不是靠改参考换来的。`[实测]`

## 6. 验收面逐条对账（任务卡 acceptance）

| 卡上要求 | 落在哪 | 结果 |
|---|---|---|
| 四采样率 | 全部 5 组四率检查 + DC/changing-CV 四率 | ✅ |
| 全扫 / 边界连续性 | 0.01 细扫，最坏相邻步 0.02（阈值 0.05） | ✅ |
| A/B 非对称 | `morphReadbackIsPerSide` 等 6 条 | ✅ |
| PW 与 PWM depth 0/mid/1 | `depthZeroIsStrictlyInert` / `pwmDepthMidMatchesHalfTheModulation` / `pwmDepthOneMatchesTheFullModulation` / `pwmDepthArmsAreOrdered` | ✅ |
| 合法 DC / 时变 CV 的 duty | DC 四率 + 逐周期跟踪四率 | ✅ |
| 输入反相 / 饱和 | `negativeCvLowersDuty` / `positiveCvSaturatesAtDutyMax` / `negativeCvSaturatesAtDutyMin` | ✅ |
| 无连线 / 拔线 | 未接线读 0；拔线**逐位**复原 | ✅ |
| 同帧消费 | 9599/9599 对 0/9599 | ✅ |
| 非规则块 | 16 段分块与均匀分块逐位相同 | ✅ |
| 反复复位 | `repeatedRestoreReachesThePwmDepthSetter` / `restoreBackToDefaultTracksTheStateWithoutLatching` | ✅ |
| 失败保持 state / format / plan / 后续 trace | 被拒 apply 后两条 trace 逐位相同 | ✅ |
| 四输出有限 | 四率 × 15 格 | ✅ |
| 回调零分配 / 零释放 | 24 块 + 阳性对照 | ✅ |
| 隔离 B 后测无串扰（且默认路由的调制**合法**） | §3.7 | ✅ |
| 保存字节与声音语义**分开**断言 | §2.3 | ✅ |

**已知不做的（本片外）**：`S3/S6` 抗混叠实现与真实可达性基线；过采样裁决（S4）。**未宣称 MET。**

## 7. 复现命令（均已原样跑过）

```bash
# 1) 全量构建 + 全量 ctest（本 worktree 内）
#    Release；86 项，实测 1626 s（四条历史 gh19 门禁各 ~1150 s，占绝大部分）
cmake -S . -B build-117-rel
cmake --build build-117-rel -j8
ctest --test-dir build-117-rel -j4      # -> "100% tests passed out of 86"

# 2) 只跑验收面（经真实构建系统）
ctest --test-dir build-117-rel -R test_gh19_s0_morph_pwm_acceptance --output-on-failure

# 3) 独立编译（不经 CMake；本项目标准 C++17，零告警）
clang++ -std=c++17 -O1 -Wall -Wextra -I tests/host -I tests/core -I host/include \
  -I core/include -I generated \
  tests/host/test_gh19_s0_morph_pwm_acceptance.cpp \
  tests/host/test_state_apply_oracle_allocator.cpp -o /tmp/acc117

# 4) 隔离生产突变（13 条；参考臂必须 rc0 + 完整 summary + 0 失败）
python3 tools/run_gh19_s0_mutants.py
python3 tools/run_gh19_s0_mutants.py --only acc-cv-one-sample-late --verbose

# 5) 清单/登记完整性（不带 --require-full）
python3 tools/check_registry_complete.py
```

## 8. 与 GitHub issue 的对应

- **GH#19（仍 OPEN）**：本片是 S0 的**残余分段之一**（波形映射 + 双侧 PWM 生产接入）。**不关 issue。**
- **GH#15**：D1–D5 已闭环，余量 `vco_a_pwm`(8) / `vco_b_pwm`(30) 两条**挂 GH#19**；本片把这两条从 `transfer_unavailable` 移到 `applied_to_dsp` ⇒ 该余量在**代码分类层面**消除，但 **GH#15 issue 未关**（其收口归 @Codex/bearbone）。
- **GH#21**：两个 PWM id 加入**既有**秒级平滑家族（Surface-1+Surface-2 union 36 → 38）；未新开机制。
- **新发现**：无新增独立缺陷。旧 `vco_a.wave`/`vco_b.wave` 的冻结规格三方不一致（#115 记录）在本片按裁决 ⑤ **定向修正**（§4）。

## 9. 明确未验证 / 不宣称

1. **不宣称 S3 / S6 完成**：混合段、saw、脉冲节点**没有任何新的抗混叠**，§2.1/§2.5 是它们的**未改善基线**。
2. **PWM 传递律是软件暂定**（`/10` 与「正 CV 抬高占空比」**不是硬件事实**），无手册/面板证据。
3. **本旋钮的连续行为已由 owner 确认**：它**就是**连续控制，**不是**「选择方式未证」。**软件暂定的是精确曲线本身**（`kRingEqual` 的节点顺序与坐标 0/.25/.5/.75/1），它不仅没有硬件依据，是否与硬件实际曲线一致也**未证**。
4. **面板用户可达性**：验收面走 canonical state 设定刺激（探针可精确设定；面板用户只能近似拨到）。
5. **未做**：`--require-full` 的 12 项覆盖缺口仍**单列不变**（按 PR 逐次批准例外，非永久豁免）。
6. **配置覆盖**（按 @Codex `dd01a880` 口径，**逐项配置结果在 `#Lunar24:9e4d9bb8` 单独报，不并入本报告正文**）：**Release 全量 ctest 86/86 已跑完**（§5.4b）；**Debug 全量 ctest 86/86 已跑完**（四条历史门禁各自全绿，验收面 38.66 s，0 sanitizer 报告）；**真实 host 目标 `Lunar24Host.app` 在 Release 与 Debug 两配置下都已构建成功**（`build-117-rel/out/`、`/tmp/b117-dbg/out/`）；**ASan+UBSan 全量 ctest 86/86 已跑完**（7098 s；四条历史门禁 1370.5 / 1369.6 / 1371.2 / 1370.6 s，验收面 114.09 s；**0 条 sanitizer 报告**，且已核 `-fsanitize=address,undefined` 真的生效 —— 编译行带 flag、产物 `nm -u` 有 53 个 asan/ubsan 符号、链 `libclang_rt.asan_osx_dynamic.dylib`）。**未跑**：Windows/MSVC（本机只有 Apple clang ⇒ **本地绿 ≠ MSVC 绿**，这是 #110 已记录的教训）。
7. **本片以 draft PR 交付**（@Codex `8e1ffa5e` 已授权提交 / push / 开 draft PR）；**未 merge / 未发布 / 未关 GH#19 或 GH#15 / 未判 MET** —— 最终合并由 @Codex 核终态后处理。
