<!-- SPDX-License-Identifier: Apache-2.0 -->
# Lunar 24 阶段性独立审计报告

| 项 | 值 |
|---|---|
| **报告日期** | 2026-09-06 |
| **审计基线（exact head）** | `4a4c7b99c1b59ad0619a3ae9c2af05b458182ed1`（`4a4c7b9`） |
| **分支** | `feat/p0-full-registry`（领先 `origin/main` = `baf1e116` 共 264 提交） |
| **工作树** | clean（0 modified） |
| **本地门禁** | `ctest` **67/67 通过**，`Lunar24Host` 构建成功（macOS） |
| **托管 CI** | run `33966071105` on `4a4c7b9`：**success**（Ubuntu g++ / Ubuntu clang++ / macOS / Windows） |
| **审计者** | 外部独立审计（Claude Opus 5），非本项目实施线 |

---

## 0. 本报告的证据口径

本报告遵循与本项目相同的证据纪律。请按下列标记读取每一条结论：

| 标记 | 含义 |
|---|---|
| **[实测]** | 审计者编写探针程序、编译并运行，数字来自实际运行输出 |
| **[代码]** | 直接引自仓库源码，附 `file:line` |
| **[手册]** | 引自 `design/reference/solar42N_manual_text.txt`，附行号 |
| **[推断]** | 基于上述证据的工程判断，**不是**被测事实 |

### 0.1 已验证的范围

- 完整通读 `design/` 的 6 份核心文档（`00-status` / `01-architecture` / `02-sound-capability` / `05-effector-families` / `06-master-plan` / `07-core-contract`）与 `design/reference/solar42N_manual_text.txt`
  - ⚠️ `03-ui-framework.md` 与 `04-panel-control-map.md` **未逐行通读**（仅经 `00-status` 的引用间接接触）——涉及 UI/面板映射的结论请以这两份文档为准
- GitHub issue：阅读全部 15 个（#2–#16）的标题与状态；**完整阅读 4 个 OPEN issue（#6 / #12 / #15 / #16）的正文与全部 comment**；抽样阅读已关闭 issue（#3 / #4 / #11 / #14）的收口结论；列出 `review/codex-*` 分支的审计记录清单
- 实际构建并运行全部 67 个 CTest
- 编写并运行多个独立探针程序，覆盖：VCF 跨采样率频响、四路输出时域行为、VCO 逐波形混叠、产品路径整机渲染。**其中 3 个完整程序 + 1 个普查脚本收录于 §6 附录，已验证可原样编译并复现报告中的全部数字**
- 对 `machine_runtime.h` 做全量类引用普查（判定"零件是否被产品消费"）

### 0.2 **未**验证的范围（不得据本报告推断这些结论）

- **未做听感评估。** 本报告所有音质结论均为频谱/时域测量，非主观听音。
- **未与实机比对。** 与 `design/02 §四` 第 ④ 维度一致，音色相似度维持"未验证"。
- **未评估 `spike/` 目录**下的 ASan/TSan 探针工程。
- **未验证真实音频设备行为**（CoreAudio 聚合设备、ASIO 四输出）——与 issue #4 关闭说明中的边界声明一致。
- **未审计 P5 面板几何的测量精度**（`tools/measure_panel_regions.py` 的锚点提取质量）。

### 0.3 一处方法学更正（请实施方注意，不要复用错误测法）

审计初稿曾报告"产品路径 DRY A 混叠 −54.0 dB"。**该数字作废，测法无效。**

错误原因：采用"谐波间频段"探针（在 `f0*(k+0.5)` 处取幅度）。当 `f0` 与 `sr` 成整数关系时（如 220 Hz @ 48 kHz），**混叠像恰好折回谐波位置**，谐波间频段内没有混叠分量，测到的是非 bin 对齐基波的**频谱泄漏**。

本报告 §2 N-2 采用的正确方法：Hann 窗抑制泄漏 + 解析定位每条折叠谱线（`fold(k·f0)`，`k·f0 > Nyquist`）+ 排除落在真实带内谐波 ±5 Hz 内的像 + 按可闻频段筛选。

---

## 1. 执行摘要

### 1.1 总体判断

**设计方向正确，工程纪律优异，但当前实施尚未构成一件可演奏的乐器。**

设计层面没有发现方向性错误。证据分级制度（confirmed / provisional / unverified）、Core 与框架隔离、能力边界纪律、"零件在盘上 ≠ 机器用了它"的产品路径判据要求——这四条是本项目最有价值的资产，**必须继续保持，不得为赶进度放弃**。

实施层面存在显著的**投入结构失衡**：治理/状态/注册表管道占 `core/` 代码 54%，真正发声的 DSP 占 20%。两块决定"是否成为乐器"的工作——面板 219 个控件、效果器 39 个程序——**均未开始**。

### 1.2 新发现（本次审计产出，**不在任何现有 issue 中**）

| ID | 严重度 | 标题 | 一句话 |
|---|---|---|---|
| **N-1** | 🔴 阻断 | DRY B 在开机默认态直流钉死 | 四路逻辑输出之一开机 24 ms 后死锁在 −1.0 V，永不恢复 |
| **N-2** | 🔴 架构 | 全链路无带限处理 | 默认三角波侥幸温和；切换到锯齿/脉冲，可闻带混叠断崖 40–60 dB |
| **N-3** | 🔴 契约违反 | VCF 截止频率 `sr/8` 硬帽 | 同一 patch 在 44.1k 与 96k 下音色差 3.7 dB；FREQ 旋钮上端 19% 死区 |
| **N-4** | 🟠 拓扑 | Chamberlin SVF 选型限制后续 | 与 `07 §7`"不是通用 ladder/SVF"冲突；非线性无法放在正确位置 |
| **N-5** | 🟡 契约缺口 | `ParameterSmoother` 无产品消费者 | 违反 `07 §3` 第 2 条；P5 接上旋钮即产生 zipper noise |
| **N-6** | ⚪ 易用性 | `processBlock` 输入数组契约未文档化 | 公开 header-only API 的踩坑点，审计者已实际踩中 |
| **N-7** | ⚪ 文档漂移 | 产品代码注释与已关闭的 #11 矛盾 | `machine_runtime.h:2099-2101` 仍称六个控制源 "not integrated" |

**N-1 是本次审计最重要的发现。** 它使 P3 出口条件第 3 条"四输出正确"在开机默认态下**不成立**。

### 1.3 已跟踪问题的独立复核

审计独立复现了全部 4 个 OPEN issue 的现状，**结论与 issue 记录一致，无夸大也无遗漏**：

| Issue | 标题 | 独立复核结论 |
|---|---|---|
| **#12** | 解码状态未应用到产品运行时 | ✅ 属实。169 个 `applied_to_dsp` 已落地（task #78 验收），**剩余：35 键盘参数 + preset、patch/route 恢复、APP 启动/保存 codec 集成** |
| **#15** | 16 个 P3 控件无可执行消费者 | ✅ 属实。审计独立测得 `transfer_unavailable` = 16，与 issue 列举完全一致 |
| **#16** | P6/P8 效果器状态无消费者 | ✅ 属实。125 个参数（8 effector + 117 program X/Y/Z）+ 左右选择 |
| **#6** | VCF 无 identity/calibration 与电平非线性 | ⚠️ **部分已修**（输入级 `tanh` 已落地，`polivoks_vcf.h:176-179`），但 **N-3/N-4 是该 issue 未覆盖的独立缺陷** |

---

## 2. 新发现详述

> 每条按本项目既有规矩组织：**证据 → 根因 → 影响 → 修复方向 → 会红的验收判据**。
> 所有验收判据均满足项目规矩 1（先写复现测试让它红，再修）与规矩 2（验收跑产品路径）。

---

### N-1 🔴 DRY B 在开机默认态直流钉死（阻断级）

#### 现象 [实测]

从 canonical 开机默认态（`MachineRuntimeDefinition(seed, 48000)`，未做任何参数改动）渲染 5.5 秒：

```
t=0.000s..+5ms   min=-0.976208  max=+0.981667   正常振荡
t=0.005s..+5ms   min=-0.999720  max=-0.976644   正在坍缩
t=0.010s..+5ms   min=-0.999997  max=-0.999725
t=0.020s..+5ms   min=-1.000000  max=-1.000000   ← 完全钉死
t=0.050s ... t=5.000s            min=max=-1.000000  持续到渲染结束

首个完全平坦的 50 ms 窗口起点：sample 1530（t=0.0319 s）
末 1 秒统计：min=-1.000000  max=-1.000000  零交叉 0 次

对照 DRY A：全程 5.5 s 内不存在任何 50 ms 平段，末秒零交叉 220 次（健康）
```

**四路逻辑输出中的 DRY B，在开机后约 20 ms 内进入直流死锁，且不可恢复**（上表 `t=0.020s` 切片已 `min=max=-1.000000`；`sample 1530 / t=0.0319 s` 是首个能容纳完整 50 ms 平坦窗口的起点，属滞后指标）。

#### 根因链（三段，逐段验证）

**① 注册表把它连成了自反馈边 [代码]**

`spec/machine/lunar24.json`，`normalizedRoutes` id 4：

```json
{
  "id": 4,
  "stable_id": "route.vco_b_vco_out_to_cv_in",
  "sourceJack": "vco_b.vco_out",
  "sinkJack":   "vco_b.cv_in",
  "description": "VCO B-region vco_b.vco_out carries the VCO A normalised signal into vco_b.cv_in; CV AMT sets the depth.",
  "evidence": { "line": 410 },
  "status": "provisional"
}
```

`core/include/lunar24/core/machine_definition.h:214-221`——它是**六条 normalized route 中唯一 `kActive` 的一条**：

```cpp
{RouteId::route_keyboard_v_oct_to_vco,   RouteDisposition::kDeferred},
{RouteId::route_keyboard_gate_to_eg,     RouteDisposition::kDeferred},
{RouteId::route_vcf_cv_l_to_cv_r,        RouteDisposition::kIntraVcfFallback},
{RouteId::route_keyboard_v_oct_to_vco_b, RouteDisposition::kDeferred},
{RouteId::route_vco_b_vco_out_to_cv_in,  RouteDisposition::kActive},   // ← 唯一激活
{RouteId::route_keyboard_gate_to_eg_b,   RouteDisposition::kDeferred},
```

`core/include/lunar24/core/machine_runtime.h:2220` 的注释自认其为自边：

> `// Generic CV (cv_in, -5..+5): the VCO-B self-edge (vco_b.vco_out -> cv_in) sinks ...`

**② 该连法与手册不符 [手册 L411–414]**

```
VCO A and VCO B mirror each other except for two things. VCO A
has a SYNC input and VCO B has a VCO output. In VCO B, the output
from VCO A is normalised to the CV input jack, which can be adjusted
using the CV AMT knob.
```

手册描述的归一化连接是 **VCO A 的输出 → VCO B 的 CV 输入**。

**注册表条目自身内部矛盾**：`description` 字段写的是 "carries the **VCO A** normalised signal"，而 `sourceJack` 字段写的是 `vco_b.vco_out`（VCO B 自己）。**描述与字段不一致，且字段侧被运行时按字面执行。**

> ⚠️ 这里存在一个尚未澄清的次级问题：面板上 VCO B 区域那个 "VCO OUT" 插孔到底输出的是 VCO A 还是 VCO B 的信号。手册 "VCO B has a VCO output" 读作 VCO B 的输出。**但无论该插孔归属如何，把 VCO B 自己的输出接回 VCO B 自己的音高 CV，都不是手册描述的连接**，因此本缺陷成立不依赖于该问题的答案。**建议在修复时同步澄清并记录该插孔语义。**

**③ 机制是自锁吸收态，不是暂态 [代码 + 实测]**

`core/include/lunar24/core/vco.h:193-204`，线性 CV 模式：

```cpp
const double cvEff = cv_ * cvAmt_;
if (cvMode_ == VcoControlMode::kExponential) {
  p *= std::pow(2.0, cvEff);                     // PROVISIONAL exp scaling.
} else {
  p *= (1.0 + cvEff);                            // PROVISIONAL linear scaling.   ← 问题在此
}
return p > 0.0 ? p : 0.0;
```

默认态 [实测]：`vcoBCvAmt = 1.000`，`vcoBControlMode = linear`。

VCO B 输出范围 ±1 V。当输出走到 **−1 V** 时，`(1.0 + cv·cvAmt)` = **恰好 0** → 频率 0 → 相位停止推进 → 输出永远停在 −1 V。**进入即不可逆。**

CV AMT 扫描验证 [实测]：

| `cvAmt` | 末 1 秒 min / max | 零交叉 | 结果 |
|---:|---|---:|---|
| 1.00（默认） | −1.000000 / −1.000000 | 0 | **钉死** |
| 0.50 | −0.999992 / +0.999987 | 200 | 存活 |
| 0.20 | −0.999983 / +0.999938 | 217 | 存活 |
| 0.05 | −0.999983 / +0.999703 | 219 | 存活 |
| 0.00 | −1.000000 / +1.000000 | 220 | 存活 |

#### 影响

1. **P3 出口条件第 3 条"四输出正确"在开机默认态下不成立。** issue #11 已于 `cac04f80` 判 P3 MET；该判定基于当时的判据集，**本发现不否定那次判定的程序正确性，但补充了一条当时无人检查的事实**。
2. **`07 §5` 的 finite guard / runaway recovery 正确地不会触发。** 输出是有限值 −1.0，不是 NaN/Inf。这套机制防的是数值污染，**不防行为死锁**——两者需要不同的检测手段。
3. **现有测试体系结构性地无法发现它** [代码]。`tests/core/test_machine_definition.cpp:694`：

   ```cpp
   if (oA.wetL != 0.0 || oA.dryA != 0.0 || oA.dryB != 0.0) nonSilent = true;
   ```

   `nonSilent` 被任何非零值满足——**一个恒定 −1.0 的死输出可以通过这条"非静音"断言**。且该循环只跑 32 帧，远早于 sample 1530 的锁死点。

   全仓 `grep dryB tests/` 结果：所有断言只覆盖 **finite / 逐位确定性 / 分块不变性 / 同种子可重现**，**没有任何一条断言 DRY B 非恒定**。

#### 修复方向（两条路径，需先做证据裁决）

| 方案 | 内容 | 代价与风险 |
|---|---|---|
| **A（推荐先评估）** | 改 `sourceJack` 为 VCO A 的输出，使 route 与手册 L411–414 一致 | 触碰**冻结的 P0 registry**，按 `00-status.md §1` 属"必须等工程总监或 owner"的三件事之一；需 id-stability 与 regen zero-diff 全绿 |
| **B** | 保留自边，改线性传递函数使其不可归零（如以 0 V 为中心的双向缩放，而非 `1 + cv`） | 不动 registry；但 `1 + cv` 本身标注为 PROVISIONAL，改动需在 FINDINGS 记录新的 provisional 依据 |

> **⚠️ 不要在没有 route 语义裁决的情况下直接改 `cvAmt` 默认值。** 把默认值从 1.0 改成 0.5 能让现象消失，但那是掩盖：`cvAmt` 是**用户可调旋钮**，用户调回 1.0 死锁立刻重现。

#### 会红的验收判据（必须先红后修）

```
判据 N-1-a（产品路径 · 四输出存活）
  从 canonical 开机默认态渲染 ≥ 2 秒，对 wetL/wetR/dryA/dryB 四路各断言：
  末 1 秒内不存在任何长度 ≥ 50 ms 的恒定窗口（|x[j] - x[i]| ≤ 1e-12 视为恒定）。
  self-proof：在当前 head 上运行，dryB 一条必须红；修复后四条全绿。

判据 N-1-b（分辨力 · 只红对应的那一条）
  单独把 dryA 强制为常数 → 恰好红 1 条（dryA），不得一动一片红。

判据 N-1-c（旋钮全域安全）
  对 vco_b.cv_amt 在 [0, 1] 上取 ≥ 11 个点（含端点 1.0），每点重跑 N-1-a。
  self-proof：把修复回退成 `p *= (1.0 + cvEff)` ⇒ cvAmt=1.0 那一点必须红。
```

---

### N-2 🔴 全链路无带限处理，波形切换后可闻带混叠断崖

#### 事实 [代码]

全仓 `grep -rin "polyblep\|blep\|BLIT\|oversampl\|band-limit" core/` — **命中 1 处，且是一条自认未做的注释**（`drone_noise.h:23`，原文见本节末）。`polyblep` / `BLIT` **零命中**。所有振荡器为朴素波形：

| 位置 | 生成方式 |
|---|---|
| `drone_bank.h` `sapply_()` | `2*(phase/2π) - 1`，朴素锯齿 |
| `vco.h:223-243` `waveformSampleAt()` | 相位直接映射（saw / triangle / sine / pulse / 两组 morph） |
| `vco.h:213` sub 振荡器 | `2.0 * subPhase() - 1.0`，朴素方波 |
| `schmitt_osc.h` | 离散阈值翻转，无带限 |

#### 测量 [实测]

**方法**：孤立 `Vco`（不经信号链），sr = 48000，0.2 s 预热后取 1 s 窗；Hann 窗抑制泄漏；解析定位每条折叠谱线 `fold(k·f0)`（`k·f0 > Nyquist`）；排除落在真实带内谐波 ±5 Hz 内的像；**只统计折回可闻中频段 100 Hz – 5 kHz 的混叠能量**（相对基波）。

| 波形 | f0 = 220 Hz | f0 = 932 Hz | f0 = 1865 Hz |
|---|---:|---:|---:|
| **三角（当前默认）** | −79.2 dB | −60.9 dB | −53.0 dB |
| **锯齿** | **−27.7 dB** | **−22.4 dB** | **−19.3 dB** |
| **脉冲（duty 0.5）** | −30.7 dB | −25.5 dB | −22.9 dB |

全频带（不限 100 Hz–5 kHz）聚合值更高：锯齿 @1865 Hz 达 **−11.3 dB**。

#### 关键判断

**当前开机默认态恰好落在整套波形里最温和的角落。** `vco.h:76` 默认 `kTriangle`，产品路径从不调 `setWaveform`；默认音高 220 Hz。该组合下混叠 −79 dB，实际不可闻。

**但 `kSaw` / `kPulse` 都在 `VcoWaveform` 枚举中（`vco.h:74-81`），都是面板 MORPHING WAVEFORM 选择器的合法位置。** 用户一转波形选择器，可闻带混叠立即从 −79 dB 跳到 −28 dB；音高推到 2 kHz 再降到 −19 dB。**落差 40–60 dB，毫无疑问可闻。**

这与项目自己在 `FINDINGS.md` 中记录的数量级一致（Schmitt 折叠 −16.24 dB、窄脉冲折叠 −14.32 dB、preamp 饱和 −10.01 dB、后置失真 −12.52 dB、硬同步 splatter +69.09 dB、高偏移 FM +37.87 dB）——**那些才是常态，三角默认态是例外。**

#### 为什么这条对**这台琴**格外要命 [推断]

模拟电路的"脏"是**谐波**失真：改变音高时，所有分量按比例跟随，听感上仍是"同一个音"。混叠是**非谐**分量：改变音高时，折叠像**向相反方向移动**。这是唯一一种明确宣告"这是廉价数字实现"的失真类型。

而 Solar 42N 是一台 **drone 机**——声音是持续的，听者有无限时间去锁定那些逆向移动的非谐分音。**对持续音的伤害远大于对打击类音色的伤害。**

#### 一个必须纠正的分类错误

项目当前把此事记为**待证据决定**：

- `design/00-status.md:1027`：「混叠只测量未修（数字与参数出处记在 FINDINGS，留 P6/后续按证据决定）」
- `core/include/lunar24/core/drone_noise.h:23-25`：「the physically-correct treatment (band-limit/oversample, or normalize by bandwidth) is a **P3-exit decision to make on evidence**」

**这是分类错误，而且是会自我延续的那种。**

混叠是**数字采样的产物，原硬件上根本不存在这个现象**——模拟振荡器没有 Nyquist 频率。因此：

> **不存在任何一份手册、任何一张面板图、任何一次实机 line-out 测量，能够回答"该不该带限"。唯一可能到来的"证据"，就是项目自己已经量到的那些 dB 值，而它们全部指向同一个答案。**

把一个**架构决定**挂在"等证据"上，而本项目的证据纪律对证据决定采取无限期等待——**结果是它永远不会到期**。

**建议：把该条从 `FINDINGS.md` 的待证据项移入 `design/07-core-contract.md` 的架构决定项，并明确它不是证据问题。**

#### 紧迫性论证

这是唯一一个"越晚做越贵"的架构决定：

1. 加入过采样会横向触碰**每一个** DSP 模块。
2. 会触碰 `graph_compiler.h` 的含环 SCC 逐 sample 执行路径（过采样倍率与逐 sample 破环的交互需要重新定义）。
3. 会触碰 `module_execution_contract.h:84` 的 `intrinsicLatencySamples` 声明与最小 causal delay 契约（抽取滤波器引入实际延迟）。
4. **P6 的 39 个效果程序会全部按当时存在的约定写一遍。** 在 P6 之后再改，返工量是现在的数倍。

#### 修复方向

不必一次做完。建议分两步：

1. **先定契约**（低成本、高价值）：在 `07` 中确定过采样倍率策略、抽取/插值滤波器规格、哪些模块必须过采样（含非线性/硬同步/PWM 的）、`ModuleExecutionContract` 如何声明过采样引入的延迟。**这一步不写 DSP 代码，但解除 P6 的架构不确定性。**
2. **再按模块落地**：优先级建议 VCO（含 sync/PWM）→ classic drone bank（20 振荡器，量最大）→ distortion / preamp（波形整形）→ Schmitt。

#### 会红的验收判据

```
判据 N-2-a（产品路径 · 逐波形混叠上限）
  对 VcoWaveform 每一个枚举值 × f0 ∈ {220, 932, 1865} Hz，
  测量折回 100 Hz–5 kHz 的混叠能量（方法见本节，Hann 窗 + 解析定位）。
  断言：聚合值 ≤ 目标阈值（阈值由契约步骤 1 确定，建议不劣于 −60 dB）。
  self-proof：在当前 head 上运行，kSaw / kPulse 各点必须红。

判据 N-2-b（不得用测法作弊）
  测试必须包含至少一个与 sr 非公度的 f0（如 233.08 Hz）。
  self-proof：若把 f0 全部改成 sr 的整数分频，混叠像折回谐波位置、判据失去分辨力
  ——该退化必须由一条独立的元测试挡住。
```

---

### N-3 🔴 VCF 截止频率 `sr/8` 硬帽 → 跨采样率音色不一致 + 旋钮死区

#### 证据 [代码]

`core/include/lunar24/core/polivoks_vcf.h:152-158`：

```cpp
double baseFreqHz_(double freqNorm, double sr) const {
  double fc = kFreqMinHz * std::pow(kFreqMaxHz / kFreqMinHz, freqNorm);
  const double cap = sr * kCutoffCapRatio;      // kCutoffCapRatio = 1.0/8.0 (:134)
  if (fc > cap) fc = cap;
  if (fc < kFreqMinHz) fc = kFreqMinHz;
  return fc;
}
```

同文件已自认该帽为 PROVISIONAL 且非手册规格：

> `// PROVISIONAL stability cap: the Chamberlin SVF is capped at sr/8 (kept well away from Nyquist so the resonance floor stays stable). Not a manual spec.`

#### 测量 [实测]

**旋钮死区**（`fc = 20 · 1000^norm`，被 `sr/8` 截断）：

| 采样率 | 截止上限 | FREQ 旋钮失效起点 | 死区占旋钮行程 |
|---:|---:|---:|---:|
| 44100 Hz | 5512 Hz | norm > 0.813 | **18.7%** |
| 48000 Hz | 6000 Hz | norm > 0.826 | 17.4% |
| 88200 Hz | 11025 Hz | norm > 0.914 | 8.6% |
| 96000 Hz | 12000 Hz | norm > 0.926 | 7.4% |

**同一 patch（FREQ 全开 norm = 1.0，RES = 0，LP 模式）的幅频响应**：

| 探测频率 | 44100 Hz | 48000 Hz | 88200 Hz | 96000 Hz | 44.1k↔96k 差 |
|---:|---:|---:|---:|---:|---:|
| 100 Hz | 0.9995 | 0.9996 | 0.9999 | 0.9999 | 0.00 dB |
| 1000 Hz | 0.9443 | 0.9524 | 0.9851 | 0.9874 | 0.39 dB |
| 4000 Hz | 0.6029 | 0.6319 | 0.8231 | 0.8441 | **2.92 dB** |
| 8000 Hz | 0.4109 | 0.4271 | 0.6029 | 0.6319 | **3.74 dB** |
| 12000 Hz | 0.3861 | 0.3815 | 0.4761 | 0.5000 | 2.25 dB |

#### 影响

1. **直接违反 P3 出口条件** [design/06-master-plan.md:112]：「Core 无固定 48 kHz 常数，**44.1/48/88.2/96 kHz 与不同 buffer 下行为稳定**」。当前实现下，同一份 DeviceState 在 44.1 kHz 与 96 kHz 主机上产生**可闻不同**的音色。
2. **同时违反 `07 §5`** 的 sample-rate 不变量精神。
3. **FREQ 旋钮上端 1/5 行程在 44.1 kHz 下完全无响应**——这是用户可直接察觉的功能缺陷，不是内部细节。

#### 为什么现有测试抓不到 [代码]

`tests/core/test_mix_filter_distortion.cpp:418-435` 的跨采样率断言只有两条：

```cpp
CHECK(exact);                  // dist=0 是干直通（这条与 VCF 无关）
CHECK(g1 / g0 > 0.85);         // 变的是 RESONANCE，测的是低频段不丢失
```

**它变的是共振、测的是 100 Hz 低频段。截止频率的跨采样率一致性完全不在其覆盖范围内。** 这不是测试写错了，是覆盖缺口。

#### 修复方向

**根因是拓扑选型**：Chamberlin SVF 是单采样延迟反馈结构，必须远离 Nyquist 才稳定，因此才需要 `sr/8` 这个帽子；且其截止频率有固有 warping（上表 1 kHz 行 0.9443 vs 0.9874 的差异即来自此，与帽子无关）。

**建议改用 TPT / ZDF SVF（Zavalishin 拓扑）**，一次解决三件事：

1. 截止频率精确预畸变（`g = tan(π·fc/sr)`）→ **跨采样率频响一致**，帽子可提高到接近 Nyquist；
2. 结构稳定至 Nyquist → 去掉 `sr/8`，恢复完整旋钮行程；
3. **在反馈路径上留出放非线性的正确位置** → 直接支撑 N-4。

与 N-4 合并实施可共用一次改造与一次验收。

#### 会红的验收判据

```
判据 N-3-a（跨采样率频响一致）
  对 norm ∈ {0.2, 0.5, 0.8, 1.0} × RES ∈ {0.0, 0.5} × 模式 {lp, bp}，
  在 44100 / 48000 / 88200 / 96000 Hz 下测量归一化幅频响应，
  探测点取 100 / 1000 / 4000 / 8000 Hz（低于各 sr 的 Nyquist 时）。
  断言：任意两个采样率间同点增益差 ≤ 0.5 dB。
  self-proof：在当前 head 上运行，norm=1.0 的 4 kHz / 8 kHz 点必须红。

判据 N-3-b（旋钮行程无死区）
  norm 从 0 到 1 单调扫描，断言有效截止频率严格单调递增，
  且 norm ∈ [0.85, 1.0] 区间内的截止频率变化 > 0。
  self-proof：恢复 kCutoffCapRatio = 1.0/8.0 ⇒ 44.1 kHz 下必须红。

判据 N-3-c（不得回归）
  「提高共振不丢低频」的既有判据必须继续绿（这是手册确证行为，
  改拓扑时最容易牺牲的一条）。
```

---

### N-4 🟠 Chamberlin SVF 选型与 `07 §7` 的 "不是通用 SVF" 冲突

#### 契约原文 [代码 design/07-core-contract.md:157]

> VCF 是 Solar 新版 **12 dB Polivoks LP/BP**，**不是通用 ladder/SVF**：必须保持"提高 resonance 不丢低频"的可测行为，并允许输入电平驱动非线性

#### 现状 [代码]

`polivoks_vcf.h:181-194` `tick_()` 是教科书 Chamberlin 状态变量滤波器：

```cpp
const double f = 2.0 * std::sin(3.14159265358979323846 * fc / sr_);   // :185
const double damp = kDampMax + (kDampMin - kDampMax) * c.res;         // :188
c.low += f * c.band;
const double high = x - c.low - damp * c.band;
c.band += f * high;
```

即**一个通用 SVF**，加上一个**输入级** `tanh`（`inputStage_()`，issue #6 的修复成果）。

#### 两点具体差距

**① 非线性位置不对 [推断]**

Polivoks 滤波器的标志性音色来自其 OTA 结构在**共振反馈路径**上的硬限幅。当前实现把非线性放在**输入级**——这能产生电平依赖性（满足 issue #6 的验收），但**不产生 Polivoks 那种"共振越推越咬人"的特征**，因为共振回路本身仍是完全线性的。

**② 最大 Q 为 10，不能自激 [实测]**

`kDampMax = 2.0`（res=0，Q=0.5）、`kDampMin = 0.1`（res=1，Q=10）——`polivoks_vcf.h:130-131`。

审计已核实 [手册]：全文检索 `resonan` / `self-oscil` 的结果中，**手册未提及滤波器自激**（L1140 只写 "RESONANCE - Filter resonance (boost the frequency near cutoff point)"）。

> **因此项目当前的保守选择在证据纪律上是站得住的，本报告不要求改变它。**
> 但建议将其**显式登记为"已知硬件行为缺口"**而非默认无问题：实体 Polivoks 滤波器可自激是广为人知的行为，任何接触过原型的用户会立即察觉。登记的意义是——将来若获得实机或高清资料，这是一条待验证清单项，而不是一个已被遗忘的默认值。

#### 修复方向

与 N-3 合并：改为 TPT/ZDF SVF 后，非线性可以放进反馈路径（`tanh` 或软限幅作用于反馈项），这是结构上正确的位置。

**边界（必须遵守 `07 §7`）**：结构保真 ≠ 发明常数。非线性的**结构**可以按电路拓扑落位，但限幅点、曲率等**电路常数一律保持 PROVISIONAL**，不得声称测量所得。

#### 会红的验收判据

```
判据 N-4-a（非线性在反馈路径，不只在输入级）
  固定输入电平，只改 RESONANCE：断言输出的谐波失真随共振升高而升高。
  self-proof：把非线性移回纯输入级（反馈路径线性）⇒ 必须红
  （纯输入级非线性下，THD 不随共振量变化）。

判据 N-4-b（issue #6 既有能力不回归）
  L/R 独立 identity/calibration、输入电平扫描的电平依赖性、
  版本化 profile golden 全部继续绿。
```

---

### N-5 🟡 `ParameterSmoother` 无产品消费者，P5 接上旋钮即产生 zipper noise

#### 契约原文 [代码 design/07-core-contract.md:71]

> 三种执行语义必须分开：… 2. knob、joystick、MIDI CC：以秒为单位的 **continuous smoothing，避免 zipper noise**；

#### 现状 [实测 · 引用普查]

审计对 `machine_runtime.h` 做全量类引用计数：

```
ParameterSmoother      在 machine_runtime.h 中出现 0 次
全仓非测试消费者        仅 keyboard_behaviour.h / joystick_cv.h / parameter_smoothing.h 自身
```

而 `applyControlEvent_()` 对参数事件是**逐帧直接赋值**，无平滑段。

#### 为什么现在听不出来

**因为还没有旋钮。** `host/plugin.cpp:75-80` 的 `mLayoutFunc` 只挂了 `AttachPanelBackground(COLOR_GRAY)`，控件数为 0。

**P5-③（按区域铺控件）一落地，这个缺陷立刻在每一次鼠标拖动上可闻。**

#### 流程层面的观察 [推断]

`design/00-status.md`「🚧 P3 的门」一节明确把 `ParameterSmoother` 排除在 P3 出口之外，理由是「P3 出口原文没要求它们」——**该决定在流程上是正确的**（避免用未写在出口条件里的东西挡实施者）。

**但副作用是：一条写在 `07` 里的硬契约，现在既没有 owner 也没有排期。** 建议在 P5-③ 的任务定义中显式纳入，而不是让它继续悬空。

#### 会红的验收判据

```
判据 N-5-a（产品路径 · 参数阶跃不产生不连续）
  在产品运行时中，于某一 sample 对一个连续型参数（如 vcf.l_freq）
  施加一次大幅阶跃（0.2 → 0.9）；
  断言输出信号的一阶差分最大值不超过同参数缓慢扫过同一区间时的最大值 × K
  （K 由平滑时间常数决定，建议 K ≤ 2）。
  self-proof：把平滑器旁路成直接赋值 ⇒ 必须红。

判据 N-5-b（平滑不污染 sample-accurate 事件）
  gate / clock / sync 边沿事件不得被平滑。
  self-proof：给 critical lane 事件也套上平滑 ⇒ 既有的
  「同批事件在 64/128/256 buffer 下落在同一 sample 位置」判据必须红。
```

---

### N-6 ⚪ `SynthRuntime::processBlock` 的输入数组契约未文档化

#### 证据 [代码]

`core/include/lunar24/core/machine_runtime.h:1592-1605`：

```cpp
void processBlock(const RuntimeInputs* inputs, std::size_t n, RuntimeOutput* out,
                  bool driveGraph = true) {
  ...
  out[i] = processFrame(inputs[i], driveGraph);   // 逐帧索引 inputs
}
```

`inputs` 被逐帧索引，要求调用方提供**长度为 `n` 的数组**。但函数签名与其上方注释均未说明这一点（注释只讲事件时基与分块不变性）。

#### 实际后果

审计者编写探针时传入了单个 `RuntimeInputs` 对象，产生栈缓冲区越界读。ASan 报告：

```
ERROR: AddressSanitizer: stack-buffer-overflow
  READ of size 8 at ... machine_runtime.h:1603
```

越界读到的垃圾数据使输出变为 NaN，**且现象随分块大小变化**（blk=1/64/128/256 恰好读到零值不报错，blk=512 才暴露）——这是最难诊断的一类症状。

> 这是审计者的使用错误，不是产品缺陷。但对一个 header-only 的公开 API 而言，**一个只在特定分块大小下才暴露、且症状表现为 NaN 的踩坑点，值得用一行注释或类型约束消除。**

#### 修复方向

三选一：① 在签名上方加明确契约注释；② 改为显式 span/长度对；③ 增加 debug 断言。建议 ①+③。

---

### N-7 ⚪ 产品代码注释与已关闭的 issue #11 矛盾（文档漂移）

#### 证据 [代码]

`core/include/lunar24/core/machine_runtime.h:2099-2101`：

```cpp
// Strict fail-closed preflight (rebuild_, @Codex 7C2): a compiled-region module
// EXPLICITLY bound to ExecutionKind::kUnsupported. The canonical fixed-chain table binds
// the six control sources + effector/voices to kUnsupported (not integrated in this
// slice), so patching any of them into the graph is a real semantics violation ...
```

但 `machine_definition.h:161-184` 的 canonical 绑定表当前实际为：

```cpp
{ModuleId::envelope_a,    ExecutionKind::kEnvelope},    // :177
{ModuleId::envelope_b,    ExecutionKind::kEnvelope},    // :178
{ModuleId::lfo_a,         ExecutionKind::kLfo},         // :179
{ModuleId::lfo_b,         ExecutionKind::kLfo},         // :180
{ModuleId::joystick,      ExecutionKind::kJoystick},    // :181
{ModuleId::sequencer,     ExecutionKind::kSequencer},   // :182
{ModuleId::keyboard,      ExecutionKind::kUnsupported}, // :176
{ModuleId::effector,      ExecutionKind::kUnsupported}, // :183
{ModuleId::voices,        ExecutionKind::kUnsupported}, // :184
```

**六个控制源早已不是 `kUnsupported`**——这正是 issue #11 于 `cac04f80` 关闭时落地的成果。当前仍为 `kUnsupported` 的只有 `keyboard` / `effector` / `voices` 三个。

#### 影响

无运行时影响（注释不参与执行）。但对**以代码注释为主要上下文来源的 AI 实施 agent** 而言，这条过期注释会直接导致错误判断——它会让 agent 认为 LFO/EG/Joystick/Sequencer 尚未集成，从而重复实现或错误排期。

> 本项目的注释密度极高且承载大量裁决记录，这是优点；**但也意味着注释漂移的代价高于一般项目**。建议在关闭 issue 时把"搜索并更新受影响的注释"列为收口清单的一项。

#### 修复

更新该注释为当前事实。**不改代码行为，不动 registry。**


---

## 3. 对已跟踪 issue 的独立复核

> 审计独立复现，未采信 issue 记录的自述。以下为独立测得的现状。

### 3.1 #12（解码状态未应用到产品运行时）— 属实，已推进过半

**独立测得的 disposition 分布**（解析 `state_disposition.h:54` 的 `kDeviceStateDisposition[]`：**345 行显式条目，无重复 ParameterId**，四类 disposition 均为显式行、无默认回退）：

| Disposition | 实测数 | #12 amendment 目标 | 一致 |
|---|---:|---:|:---:|
| `applied_to_dsp` | 169 | 169 | ✅ |
| `applied_to_keyboard` | 35 | 35 | ✅ |
| `preserved_deferred_p6_p8` | 125 | 125 | ✅ |
| `transfer_unavailable` | 16 | 16 | ✅ |
| `invalid/unlanded` | 0 | 0 | ✅ |
| **合计** | **345** | **345** | ✅ |

**已达成**：169 个 `applied_to_dsp` 参数经 `buildMachineRuntimeCandidate` 路径到达真实 DSP 成员（`test_state_apply_oracle_169` 覆盖，含 typed reject 与反串扰）。

**剩余**（与 issue 最新 comment 一致，审计独立确认）：

1. **35 个键盘参数 + 4 组 preset** 的运行时应用 — 阻塞于 3.2
2. **patch / normalized-route 恢复**
3. **APP 层 codec 启动/保存集成** — 审计独立确认：`host/plugin.cpp` 中 `grep SerializeState|UnserializeState` **零命中**，`StateSaveDebounce` 与 `StateSnapshotPool` 在 `core/` + `host/` 中**产品消费者为 0**

### 3.2 键盘子系统孤岛（#12 第 4 项范围内）

审计做了独立的引用普查。以下类在 `core/include` + `host/` + `generated/` 中**只被自己人引用**，产品路径引用数为 0：

```
InputStateMachine   KeyboardBehaviour   PortamentoGlide   Vibrato
PressureOutlet      PressureNormalizer  KeyboardMenuNav   ArpSeq
```

合计 **2,472 行**代码（占 `core/` 的 16.9%），全部单测通过，**产品完全不使用**。

**产品影响的直白表述**：`route.keyboard_v_oct_to_vco` / `_to_vco_b` / `gate_to_eg` / `gate_to_eg_b` 四条 normalized route 均为 `kDeferred`（`machine_definition.h:215-220`）。**当前的 Lunar 24 是一台自己运行的 drone 机加一组旋钮，没有演奏入口——弹不了音。**

手册 28 页中有 8 页讲触摸键盘；`design/02` 将其列入 L4 演奏控制层（与 drone 键并列）。

> `design/00-status.md` 记为「P4 模块达成，集成未验」。**审计认为该措辞低估了状态**：不是"集成未验证"，是**尚未集成**。建议在状态文档中改用后一种表述，以免后续排期时被误读为"只差验证"。

### 3.3 #15（16 个控件无可执行消费者）— 属实，列举完全一致

审计独立测得 `transfer_unavailable` = 16，与 issue 列举逐项吻合：

- `vco_a.pwm`(8)、`vco_b.pwm`(30)
- DRONE 3：`mod`(278) `divider`(279) `att`(282) `rls`(283) `hi_low`(284) `rate_switch`(287) `hold`(288)
- DRONE 6：`mod`(290) `divider`(291) `att`(294) `rls`(295) `hi_low`(296) `rate_switch`(299) `hold`(300)

**审计补充一条观察**：`vco_a.pwm` / `vco_b.pwm` 无消费者，意味着 **PWM 调制深度当前不可用**。而 PWM 是 `01-architecture.md §3` 逐面板点数确认存在的 VCO 控件。此项一旦实现，**将成为 N-2（混叠）的新暴露面**——动态脉宽调制的朴素实现混叠尤其严重。建议 #15 的实施排在 N-2 契约确定之后。

### 3.4 #16（P6/P8 效果器状态无消费者）— 属实

125 个参数（8 个 `effector.*` + 117 个 program X/Y/Z）+ 左右 program 选择，字节可往返但无法影响音频。`MachineRuntimeDefinition` 的 canonical 绑定表将 effector / voices 标为 `ExecutionKind::kUnsupported`（`machine_definition.h:183-184`）。

**审计确认 P6 = 0/39**，8 个 DSP 家族均未开始。这是剩余工作量中最大的单块。

### 3.5 #6（VCF identity/calibration 与非线性）— 部分已修，但有未覆盖缺陷

**已修部分**（审计确认）：`polivoks_vcf.h:176-179` 的 `inputStage_()` 提供了 L/R 独立、单调、有界的输入电平非线性；`configureVcfIdentity` 消费持久化 seed 与 L/R calibration trim。

**issue #6 未覆盖、本报告新增**：

- **N-3**（`sr/8` 帽导致跨采样率不一致）——与 identity/calibration 无关，是独立的 sample-rate 契约违反
- **N-4**（非线性位置在输入级而非反馈路径；Q 上限 10 不自激）

**建议**：N-3/N-4 单独开 issue，不并入 #6，以免 #6 的关闭条件被无限扩张（这与 #12 拆出 #15/#16 的做法一致）。

### 3.6 已关闭 issue 的抽样复核

| Issue | 关闭时结论 | 审计复核 |
|---|---|---|
| **#4** | 产品运行时/host 所有权 | ✅ 确认。`host/plugin.cpp:138-151` 的 `ProcessBlock` 是纯委托，经 `StandaloneAudioEngine` → `DeviceAdapter::renderBlock`，无第二套映射 |
| **#11** | P3 六个控制源 | ✅ 确认。LFO / EnvelopeGenerator / JoystickCv / FiveStepSequencer 在 `machine_runtime.h` 引用数分别为 5 / 5 / 3 / 7，非零引用 |
| **#3** | StateSnapshotPool 竞态 | ⚠️ 修复本身有效，但 **`StateSnapshotPool` 产品消费者仍为 0**（`00-status.md` 已自记此点）。该竞态在当前运行时属"尚未发生"，不是"已解决"——重新引入该类时必须重跑原竞态判据 |

---

## 4. 现状量化

### 4.1 代码分布（`core/` 共 14,640 行，header-only）

| 类别 | 行数 | 占比 |
|---|---:|---:|
| 状态 / 注册表 / 图 / 序列化管道 | 7,856 | **53.7%** |
| 真正发声的 DSP（16 个模块） | 2,773 | **18.9%** |
| 键盘子系统（产品未消费） | 2,472 | 16.9% |
| 其他（identity / random / 布局 / 变换等） | ~1,539 | 10.5% |

单文件对比：`machine_runtime.h` = **2,784 行**，与全部 DSP 之和（2,773 行）基本相当。

### 4.2 注册表与参数

| 项 | 数值 |
|---|---|
| 模块 / 插孔 / normalized route / program 身份 | 21 / 64 / 6 / 39 |
| 已落地参数 | **345**（228 模块参数 + 117 program X/Y/Z） |
| 冻结目标 | 357（345 landed + **12 声明为结构性 gap**） |
| 12 个 gap 构成 | 8 个非标量（不得拍扁）+ 4 个无值域 selector（不得臆造） |
| 完整性门禁 | `check_registry_complete.py` 自洽通过 |
| **可经公开 ControlEvent 事件路径到达的参数** | **43**（是 169 `applied_to_dsp` 的**子集**，非独立分区） |

> ⚠️ **给实施 agent 的注意**：4 类 disposition（169/35/125/16）是**互斥分区**，合计 345。上表最后一行的 43 是**另一个维度**（走哪条路径到达），**不可与前四类相加**。

### 4.3 阶段状态（审计独立判断）

| 阶段 | 项目自记 | 审计复核 |
|---|---|---|
| P0 registry | ✅ 收口 | ✅ 确认 |
| P1 跨平台技术切片 | ✅ MET | ✅ 确认（四平台 CI + 真实 host 产物） |
| P2 控制时基与路由图 | ✅ MET | ✅ 确认（模块级证明达成） |
| P3 固定声音核心 | ✅ MET（#11 于 `cac04f80` 关闭） | ⚠️ **出口条件第 3 条"四输出正确"因 N-1 在开机默认态下不成立** |
| P4 演奏系统 | ⚠️「模块达成，集成未验」 | ⚠️ **建议改述为"尚未集成"**（见 3.2） |
| P5 整张面板 | ▶ 进行中（③ 暂停） | ⚠️ **实际控件数 = 0**；待铺 219 个面板控件 |
| P6 dual effector | 未开始 | ✅ 确认 0/39 |

### 4.4 流程风险

| 项 | 现状 | 风险 |
|---|---|---|
| 分支积压 | `feat/p0-full-registry` 领先 `origin/main` **264 提交** | 单分支两周未合并；PR#2 从未开启 |
| `origin/main` | 自 2026-08-23 未变动 | 全部工作集中于一条未合并分支 |
| `design/00-status.md` | 1,047 行 | 长于 01/02/03/04/05/07 六份设计文档之和（623 行） |
| MIDI 输入 | `host/config.h:53` `PLUG_DOES_MIDI_IN 0` | P4 出口三路等价中的 MIDI 路在 host 侧未接 |

---

## 5. 建议的实施顺序

> 排序依据：**① 越晚做越贵的架构决定优先；② 阻断级缺陷优先；③ 能让项目变成"可演奏乐器"的工作优先于进一步的审计与治理。**

### 第一梯队 — 立即（阻断 / 架构窗口正在关闭）

| 序 | 任务 | 依赖 | 关联 |
|---|---|---|---|
| **1** | **修复 N-1（DRY B 直流钉死）** — 先落判据 N-1-a 让它红，再做 route 语义裁决（方案 A/B），最后修 | route 语义裁决需 owner 批准（触碰冻结 P0） | 新增 issue |
| **2** | **定下 N-2 的反混叠契约** — 只写 `07` 契约，不写 DSP 代码 | 无 | 新增 issue |
| **3** | **N-3 + N-4 合并改造 VCF 为 TPT/ZDF** | 建议在 2 之后（过采样策略影响滤波器设计） | 新增 issue（不并入 #6） |

> **为什么 2 必须在 P5/P6 之前**：P6 的 39 个效果程序会全部按当时存在的约定编写。契约缺失下动工，返工量数倍增长。**这一步成本极低（写文档），价值极高（解除 P6 架构不确定性）。**

### 第二梯队 — 让它成为一件乐器

| 序 | 任务 | 关联 |
|---|---|---|
| **4** | **接通键盘子系统**：先激活 `route.keyboard_v_oct_to_vco` / `_to_vco_b` / `gate_to_eg` / `gate_to_eg_b` 四条 route，让 `InputStateMachine` 进产品路径 | #12 第 4 项 |
| **5** | **开启 PR#2**，把 264 个提交合入 `main` | 流程风险 |
| **6** | **P5-③ 铺控件**，同批接上 `ParameterSmoother`（N-5） | P5 + 新增 issue |
| **7** | 完成 #12 剩余：35 键盘参数 + preset 应用、patch/route 恢复、APP 启动/保存 codec | #12 |

> **建议把 4 提到 5、6 之前。** 项目当前最缺的不是更多审计，而是一次"我弹了一个音，它响了"。这对探索性项目的续航价值高于任何治理产出。

### 第三梯队 — 按既有计划推进

| 序 | 任务 | 关联 |
|---|---|---|
| **8** | #15 的 16 个控件（**排在任务 2 之后**，PWM 是新的混叠暴露面） | #15 |
| **9** | 按 N-2 契约逐模块落地带限：VCO → classic drone bank → distortion/preamp → Schmitt | 新增 issue |
| **10** | P6 效果器，按项目自有依赖矩阵顺序 P1(延迟) → P3(变调) → P2(混响) → P4/P5/P6/P7 | #16 |

---

## 6. 附录：复现方法

所有测量可独立复现。编译命令统一为：

```sh
clang++ -std=c++17 -O2 -I core/include -I generated <probe>.cpp -o probe && ./probe
```

### 6.1 N-1 复现（DRY B 直流钉死）

```cpp
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/machine_runtime.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
using namespace lunar24::core;

int main() {
  const double sr = 48000.0;
  MachineRuntimeDefinition def(0xC0FFEEULL, sr);
  SynthRuntime& rt = def.runtime();
  const std::size_t n = static_cast<std::size_t>(sr * 5.5);

  // NOTE: `inputs` must be an ARRAY OF n frames (see finding N-6).
  std::vector<RuntimeInputs> inv(1024);
  std::vector<RuntimeOutput> o(1024);
  std::vector<double> da, db;
  std::size_t done = 0;
  while (done < n) {
    const std::size_t b = std::min<std::size_t>(1024, n - done);
    rt.processBlock(inv.data(), b, o.data());
    for (std::size_t i = 0; i < b; ++i) { da.push_back(o[i].dryA); db.push_back(o[i].dryB); }
    done += b;
  }

  // First fully-flat 50 ms window (the window must be COMPLETE, else no verdict).
  auto firstFlat = [&](const std::vector<double>& v) -> long {
    const std::size_t w = static_cast<std::size_t>(0.05 * sr);
    for (std::size_t i = 0; i + w < v.size(); ++i) {
      bool same = true;
      for (std::size_t j = i; j < i + w; ++j)
        if (std::fabs(v[j] - v[i]) > 1e-12) { same = false; break; }
      if (same) return static_cast<long>(i);
    }
    return -1;
  };
  std::printf("DRY A first flat 50ms window: %ld  (expect -1 = healthy)\n", firstFlat(da));
  std::printf("DRY B first flat 50ms window: %ld  (observed 1530 on 4a4c7b9)\n", firstFlat(db));

  // CV AMT sweep: the latch is a function of the self-edge depth.
  for (double amt : {1.0, 0.5, 0.2, 0.05, 0.0}) {
    MachineRuntimeDefinition d2(0xC0FFEEULL, sr);
    SynthRuntime& r2 = d2.runtime();
    r2.setVcoBCvAmt(amt);
    std::vector<double> v;
    std::size_t k = 0;
    const std::size_t m = static_cast<std::size_t>(sr * 2.0);
    while (k < m) {
      const std::size_t b = std::min<std::size_t>(1024, m - k);
      r2.processBlock(inv.data(), b, o.data());
      for (std::size_t i = 0; i < b; ++i) v.push_back(o[i].dryB);
      k += b;
    }
    std::vector<double> tail(v.end() - static_cast<std::size_t>(sr), v.end());
    const double mn = *std::min_element(tail.begin(), tail.end());
    const double mx = *std::max_element(tail.begin(), tail.end());
    std::printf("  cvAmtB=%.2f  min=%+.6f max=%+.6f  %s\n",
                amt, mn, mx, (mx - mn) < 1e-9 ? "<-- DC PINNED" : "alive");
  }
  return 0;
}
```

### 6.2 N-2 复现（逐波形混叠，正确测法）

```cpp
#include <lunar24/core/vco.h>
#include <cstdio>
#include <cmath>
#include <vector>
using namespace lunar24::core;

// Hann-windowed DFT bin: suppresses leakage so real lines are measured, not skirts.
static double mag(const std::vector<double>& x, double f, double sr) {
  const std::size_t N = x.size();
  double re = 0.0, im = 0.0;
  for (std::size_t i = 0; i < N; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1));
    const double t = 2.0 * M_PI * f * i / sr;
    re += x[i] * w * std::cos(t);
    im -= x[i] * w * std::sin(t);
  }
  return 2.0 * std::sqrt(re * re + im * im) / (0.5 * N);
}
static double foldHz(double f, double sr) {
  double m = std::fmod(f, sr);
  if (m < 0) m += sr;
  return m > sr / 2 ? sr - m : m;
}

static void run(const char* nm, VcoWaveform w, double f0, double sr) {
  Vco v(sr);
  v.setWaveform(w); v.setBaseHz(f0); v.setOctaveSelect(1);
  v.setTune(0.0); v.setMorph(0.5); v.setShape(0.5);
  for (std::size_t i = 0; i < static_cast<std::size_t>(sr * 0.2); ++i) { double a, s; v.tick(&a, &s); }
  const std::size_t n = static_cast<std::size_t>(sr);
  std::vector<double> b(n);
  for (std::size_t i = 0; i < n; ++i) { double a, s; v.tick(&a, &s); b[i] = a; }

  const double fund = mag(b, f0, sr);
  double worst = -999.0, wf = 0.0, sum = 0.0;
  int cnt = 0;
  const int kmax = static_cast<int>(sr * 8 / f0);
  for (int k = 2; k <= kmax; ++k) {
    const double h = k * f0;
    if (h <= sr / 2) continue;                       // only harmonics ABOVE Nyquist fold
    const double a = foldHz(h, sr);
    if (a < 100.0 || a > 5000.0) continue;           // AUDIBLE MID-BAND only
    bool onHarm = false;                             // skip aliases hiding on real harmonics
    for (int j = 1; j * f0 < sr / 2; ++j)
      if (std::fabs(a - j * f0) < 5.0) { onHarm = true; break; }
    if (onHarm) continue;
    const double m = mag(b, a, sr);
    const double d = 20.0 * std::log10(m / fund + 1e-18);
    if (d > worst) { worst = d; wf = a; }
    sum += m * m;
    if (++cnt > 400) break;
  }
  if (cnt == 0) { std::printf("  %-22s f0=%8.2f  (aliases all land ON harmonics)\n", nm, f0); return; }
  std::printf("  %-22s f0=%8.2f  worst %8.1f Hz = %6.1f dB   RMS(%d lines) = %6.1f dB\n",
              nm, f0, wf, worst, cnt, 10.0 * std::log10(sum / fund / fund + 1e-30));
}

int main() {
  const double sr = 48000.0;
  // 233.08 Hz is deliberately NON-commensurate with sr: with a commensurate f0
  // (e.g. 220 Hz) alias images fold exactly onto harmonics and become unmeasurable.
  for (double f0 : {220.0, 233.08, 932.33, 1864.66}) {
    std::printf(" f0 = %.2f Hz\n", f0);
    run("TRIANGLE (default)", VcoWaveform::kTriangle, f0, sr);
    run("SAW", VcoWaveform::kSaw, f0, sr);
    run("PULSE (duty .5)", VcoWaveform::kPulse, f0, sr);
    std::printf("\n");
  }
  return 0;
}
```

### 6.3 N-3 复现（VCF 跨采样率频响）

```cpp
#include <lunar24/core/polivoks_vcf.h>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <initializer_list>
using namespace lunar24::core;

static double gain_at(double sr, double norm, double probe) {
  PolivoksFilter f;
  f.setSampleRate(sr);
  f.setFreq(0, norm);
  f.setRes(0, 0.0);
  f.setMode(0, /*bp=*/false);                 // LP
  const std::size_t n = static_cast<std::size_t>(sr * 0.5);
  const double amp = 0.2;
  double re = 0.0, im = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = amp * std::sin(2.0 * M_PI * probe * i / sr);
    double a, b;
    f.process(x, 0.0, a, b);
    if (i > n / 2) {                          // settle, then integrate
      const double t = 2.0 * M_PI * probe * i / sr;
      re += a * std::cos(t);
      im += a * std::sin(t);
    }
  }
  const double cnt = static_cast<double>(n - n / 2 - 1);
  return 2.0 * std::sqrt(re * re + im * im) / cnt / amp;
}

int main() {
  const double rates[4] = {44100.0, 48000.0, 88200.0, 96000.0};
  for (double norm : {0.5, 0.8, 1.0}) {
    std::printf("\nFREQ norm=%.2f\n  probeHz ", norm);
    for (double r : rates) std::printf("%9.0f", r);
    std::printf("\n");
    for (double p : {100.0, 1000.0, 4000.0, 8000.0, 12000.0}) {
      std::printf("  %7.0f", p);
      for (double r : rates) {
        if (p < r / 2 - 100) std::printf("%9.4f", gain_at(r, norm, p));
        else std::printf("        -");
      }
      std::printf("\n");
    }
  }
  std::printf("\n== cutoff cap (kCutoffCapRatio = 1/8) ==\n");
  for (double r : rates)
    std::printf("  sr=%6.0f  cap=%8.1f Hz  knob dead above norm=%.3f\n",
                r, r / 8.0, std::log(r / 8.0 / 20.0) / std::log(1000.0));
  return 0;
}
```

### 6.4 产品路径引用普查（判定"零件是否被产品消费"）

```sh
for h in core/include/lunar24/core/*.h; do
  for c in $(grep -oE '^class [A-Za-z0-9_]+' "$h" | awk '{print $2}'); do
    n=$(grep -c "\b$c\b" core/include/lunar24/core/machine_runtime.h)
    printf "%-30s %s\n" "$c" "$n"
  done
done | sort -k2 -n
```

引用数为 0 的类需进一步确认它在 `host/` 与 `generated/` 中是否也无消费者：

```sh
grep -rl "\bClassName\b" core/include host/ generated/
```

---

## 7. 给实施方的三条提醒

1. **本报告的每一条修复建议都配了"会红的判据"，请遵守项目既有规矩 1：先写复现测试让它红，再修。** 没有先红过的修复无法区分"修好了"与"根本没触发过"。

2. **N-1 的 route 语义裁决触碰冻结的 P0 registry**，按 `design/00-status.md §1`，属于必须由工程总监或 owner 决定的三件事之一，**不得由实施线自行决定**。建议先落判据、再报裁决、最后修。

3. **不要为修复 N-2/N-3/N-4 而放弃证据纪律。** 带限、TPT 拓扑、反馈路径非线性都是**结构**改动，不需要发明任何硬件常数。所有未测得的电路常数（限幅点、曲率、过采样倍率的听感取舍）继续按 PROVISIONAL 标注并进 FINDINGS 台账。**结构保真 ≠ 发明常数**——这是 `design/00-status.md:550` 的原话（对应 `07-core-contract.md:156`「参数只能随证据校准，不能假装已有精确电路模型」），改造过程中最容易越界的一条。

---

*报告结束。审计基线 `4a4c7b9`，所有测量可按 §6 复现。*
