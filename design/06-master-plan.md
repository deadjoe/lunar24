# Lunar 24 总体开发设计

> @Codex 整合，@Claude 复核。**这是依赖顺序，不是工期表。**

## 1. 产品边界

Lunar 24 是 macOS/Windows 的开源独立桌面乐器：保持 Solar 42N 的声音拓扑、面板结构和操作关系，以克制的月亮主题映射其视觉美学。

**允许的桌面替代**：音频设备代替实体输入/输出；鼠标、电脑键盘、MIDI 代替触摸片、gate、clock 和已有 CV 控制；应用启动时恢复上次退出的唯一整机状态，等价于实体旋钮和跳线停留在原位置。

**禁止的能力扩张**：新增声源/声部/效果、通用调制矩阵、无限路由、宏、插件/DAW、录音时间线、分析器、云功能，以及多套整机 preset 的保存、浏览或切换。原机只有 keyboard 菜单参数的 4 个 presets，必须与整机状态严格分开。MIDI 不能绕过原 keyboard state machine 或增加复音/调制目标。

## 2. 技术定案

- **C++ + iPlug2 + IGraphics + CMake；只构建 standalone app。**
- iPlug2 以固定 commit 的主仓子模块引入，Lunar 自己维护薄的 out-of-source CMake wrapper；`iPlug2OOS` 当前无明确顶层许可证，只作结构参考，不复制、不依赖。
- 所需依赖均为 zlib-like/zlib/MIT；固定 commit 并保留许可证与修改标记。
- 不接入 AU/VST/CLAP/AAX/WAM、专有 SDK、WebView 或网页 UI。
- **Synth Core 是不依赖框架/GUI/平台 API 的纯 C++ library**；iPlug2 只作 standalone/device/MIDI adapter，IGraphics 只作 UI。Lunar canonical state/parameter/patch model 不建立在 `IParam` 或 UI 对象上。
- UI 使用 2400×1551 单画布等比矢量还原，不重排、不分页；五类功能色区分必须保留。
- DSP 永远保留四个逻辑输出：`WET L/R + DRY VCO A + DRY VCO B`。应用侧 host adapter 让双声道设备只连接 WET，四输出设备再外送 DRY。
- 音频线程无分配、无锁、无文件/序列化、无慢日志是全生命周期不变量；完整契约见 `07-core-contract.md`。

## 3. 声音架构

```text
6 drone + EXT AUDIO + PREAMP + VCO A/B
                 │
          10-channel PAN/VOL mixer
                 │
        dual LP/BP VCF → distortion
                 │
              dual effector
                 │
              WET L/R

VCO A ───────────────────────── DRY A
VCO B ───────────────────────── DRY B
```

- L1 声源：四组 5-osc negistor classic drone、两组 new drone、VCO A/B、noise、外部输入/preamp。
- L2 控制与路由：LFO×2、EG×2、joystick、5-step sequencer、S&H、envelope follower、真实面板插孔与 normalized routes。
- L3 处理：双 VCF、双 distortion、13×3 dual-effector programs；SYNTEX-1/GENERATOR 算法会自发声，但产品归属仍是占卡带槽的 L3。
- L4 演奏：原 12 触摸片的 single/twin/split、微分音/quantizer、pressure、portamento、vibrato、arp/16-step sequencer、clock、六个 drone gate。

## 4. 能力报告口径

不制造一个伪精确的“整机百分比”。每次里程碑固定报告四项：

| 维度 | 报告方式 |
|---|---|
| 核心声音拓扑 | 完整/缺失的声源、固定处理和四输出路径 |
| 路由与演奏行为 | 原插孔/normalized route/keyboard 行为的完成矩阵 |
| 效果器 | **可完整工作的程序数 X/39**；只有程序全部依赖 DSP 家族都完成才计数 |
| 音色相似度 | 未校准 / 参考材料校准 / 受控实机录音验证；不能用功能完成度冒充 |

negistor 的单元差异、慢漂移和非线性是核心音色，不得用干净 saw+detune 代替。没有受控实机 line-out 对照前，只能声明“结构与行为还原”，不能声称电路级音色复刻。

## 5. 严格实施顺序

### P0 — 锁定可审计基线

- 固定两份原始 PDF、面板图、手册页码来源、iPlug2 commit 和全部依赖许可证。
- 建立唯一 `Module / Parameter / Jack / NormalizedRoute / Program` registry：稳定 ID、真实单位、范围、默认值、持久化和信号语义。
- 定义框架无关的 `AudioBlockView / ControlEvent / DeviceStateV1 / PatchGraph / ModuleExecutionContract` 与 Core/adapter API；同一 registry 同时绑定 DSP、UI、MIDI、状态和测试。
- 为每个 jack 冻结 virtual-volts electrical contract、极性、source/output 范围、输入 rail 证据状态、gate threshold/hysteresis、每伏 transfer/depth 与 connection cardinality；手册输出表不能反推输入 clamp。
- 冻结 ControlEvent 的 parameter coalescing、critical edge 不任意丢弃、overflow reconcile 与 same-sample deterministic ordering。
- `DeviceStateV1` 从此阶段冻结 schema/version，区分唯一整机状态与 keyboard 4 presets；加入持久 `UnitIdentitySeed` 和 calibration state。
- 官方资料只证明 VCO A/B 存在 dry outputs，未证明插入 jack 是否切换 wet route；把该语义列为 `unverified`，获得一手证据前不实现切断行为。

**退出条件**：每个可操作件、稳定 ID、状态字段和固定/跳线路由都能追到证据；Core public headers 不含 iPlug2/IGraphics/平台类型。

### P1 — 可丢弃的跨平台技术切片

- 用固定版本 iPlug2 主仓与 Lunar 自有 CMake wrapper 只构建 macOS/Windows standalone。
- 实测 2/4-channel CoreAudio、2/4-channel WASAPI、输入/MIDI、设备热切换和 channel mapping；Core 始终只看四逻辑输出。
- 绘制代表性 drone、VCO、10路 mixer、虚拟跳线和 12 键；以 2400×1551 Design Coordinate Space 统一变换，验证 50/67/75/100/125/150/200% 与 fit-to-window。
- 压力测试 UI 拖动/MIDI burst，确保音频线程无锁、无分配、无爆音。
- 实测 immutable graph/state 的 audio-thread handle 与 non-audio reclamation；不能让 `shared_ptr` 最后一次 release/destructor 偷跑到 callback。

**退出条件**：macOS 上①–⑤全部通过；Windows 侧**本阶段只验 CI 能编译**（`windows-latest`），
**真机音频/MIDI 行为暂不验证**（bearbone 2026-08-24 决定：Windows 调试先不考虑）。
⚠️ Windows 真机验证**挂起未完成**，补回时点：**首个面向 Windows 的可分发构建之前**，不得再往后推。
若①在 macOS 即失败，按原计划在此处回退 miniaudio+RtMidi+SDL3，不让框架风险进入 DSP 主体。

### P2 — 固定控制时基与路由图

- 分开三种语义：sample-accurate discrete event、按真实时间平滑的连续参数、audio-rate internal modulation signal；外部 input event 进入 core 后才转成 CV/gate/clock/parameter 行为。
- `PatchGraph` 是真实 signal network，不是 modulation matrix：实现稳定 Jack ID、正式 normalized-route graph 与“插线覆盖/拔线恢复”；UI cable 只是 graph 的可视化。
- 任何真实输出可接任何真实输入；类型/范围只提示。gate/clock 在 sink 端用 threshold/hysteresis/edge 解释，不与 CV 隔绝。
- graph compiler 在拓扑变化时做 SCC 分解：无环区域按 block，含环 SCC 逐 sample；以确定性 feedback-edge set 的 `z⁻¹` 破环，禁止 one-buffer delay。模块内部 zero-delay feedback 由模块 solver 负责。
- 模块在实际算法 `prepare` 后声明 intrinsic latency，并按具体 input→output 路径声明最小 causal delay/零延迟直通可能性；只有真正落在 cycle 上且最小延迟为正的路径可替代额外 `z⁻¹`。模块级 latency 数字不能授予破环资格；不做 PDC，窗口/FFT 模块未提供 cycle-safe 接口前不得进入 cyclic SCC。
- 图/状态更新由 control thread 构造不可变 snapshot，经预分配队列或无锁交换生效；状态保存采用 snapshot、debounce、临时文件、flush、atomic replace。

**退出条件**：自动测试证明 event/smoothing/audio-rate 三域、跨类型连接、normalized route、循环破环与状态恢复正确；64/128/256 buffer 下 feedback/event sample timing 不变。

### P3 — 完成固定声音核心

1. classic drone：20 个独立、free-running oscillator；固定 component tolerance 与动态 drift/noise 分离，negistor 非线性从首个可听版本预留；
2. new drone 3/6：Schmitt oscillators、FM/AM、noise、S&H；
3. VCO A/B：AS3340 行为、六波形/morph、PWM/FM/sync/sub；
4. EXT AUDIO、preamp、envelope follower；
5. 十路 mixer/pan；双 12 dB Polivoks LP/BP VCF（提高 resonance 不丢低频、CV L normalled 到 CV R）；post-filter distortion（DIST=blend、GAIN=amount）；WET/DRY 输出。左右 calibration/nonlinear state 独立。
6. 可跳线控制源（L2「控制与路由」中需要模块级即时声源、且 **非 P4 keyboard 演奏系统**的部分；即 GH #11 的完整范围，共 6 个控制源，须满足本阶段退出条件的硬门）：
   - **Envelope A / Envelope B**：两只独立 ADSR 包络（`envelope_a`/`envelope_b`，带 HOLD 与 SELF-GEN surface、gate 输入 `gate_in`、ENV `env_out` 与 VCA-CV `vca_cv_out` 两路输出）。ENV 0..8V（manual OUTS VOLTAGE SPECIFICATION，confirmed）；VCA-CV 极性/transfer provisional；ATT/DEC/RLS 曲线与真实秒数、HOLD/SELF-GEN transfer unverified（见下证据口径）。
   - **LFO A / LFO B**：两只独立低频振荡器（`lfo_a`/`lfo_b`，square↔triangle WAVE、RATE、×1/×6/×10 `speed_mult`），公开 CV 单极 0..+10V（manual OUTS SPEC，confirmed，须保留单极性）。精确波形 blend 曲线、频率 transfer、通电相位属 evidence disposition，不作已校真机断言。
   - **Joystick**：独立 X/Y 与 OFFSET X/Y，两路可跳线输出（`joystick.x_out`/`joystick.y_out`，confirmed −10..+10V）；连续 param→audio-rate CV 源。机械 taper/center/offset transfer unverified。
   - **物理 5-step sequencer**：独立五段状态机（`sequencer`，ModuleId 11），STAGES 仅 3/4/5；五步 step-CV（0..+5V，`cv_out`）与五步 gate enable（`gate_out` 0..+10V）；内部 PULSER **只控制内部 clock rate**，其 period 输出呈现在 `clock_out`，与 external clock in（`ext_clock_in`）。⚠️ manual 输出表原始 token `PULSERL: −10..+10V` 与 registry `sequencer.clock_out`（descriptor 0..5、Polarity::unknown、FieldEvidence 全 unverified）的**命名/电气对应尚未核清**，属 raw evidence conflict，**不得当作已确认实现常数**；`clock_out`/`ext_clock_in` 的 range/polarity/threshold 均 unverified。脉冲宽度、通电 playhead unverified/provisional；不得凭空发明 reset（连通语义见设计/07）。

   > 明确排除：`EnvelopeFollower`（`env_follower`）是 preamp 相关的 **L2 控制与路由**检测器/控制源（design/06 §3 L2；实现归属为 P3 第 4 项 EXT AUDIO / preamp 关联器件），**不是** Envelope A/B（即不作为 GH #11 六个控制源之一），不归入本节；物理 5-step 是 P3 独立 sequencer，**不得**并入 P4 keyboard 的 16-step（那是两套独立 sequencer）。

**退出条件**：除 dual effector 外的整条信号链可演奏、可跳线、四逻辑输出正确；Core 无固定 48 kHz 常数，44.1/48/88.2/96 kHz 与不同 buffer 下行为稳定；固定测试 seed 可重现。**上述 6 个控制源（Envelope A/B、LFO A/B、Joystick、物理 5-step）必须补齐生产实现与消费**，逐项满足硬门：框架无关、固定/无堆、逐 sample 的生产实现；canonical factory/实例化；被 SynthRuntime/PatchGraph 按 registry JackId 真实消费；公开输出使用 virtual volts；有 forward behavior test 与 negative（旧错红）。仅 class/registry/结构扫描当“有实现”**不计入**。

### P4 — 完成演奏系统与输入适配

- 实现原 keyboard 的全部声音行为与四个 keyboard presets。**P4 是 keyboard 自己的 arp/16-step 演奏系统**，包含 keyboard 专用的 16-step sequencer、pressure / portamento / vibrato / quantiser 与 six gate 等发音行为；它通过既有 CV/gate/clock 与 P3 控制源相接，**不拥有、不扩展、不取代物理 5-step**（物理 5-step 是 P3 可跳线控制源下的独立 sequencer，ModuleId 11；两套 sequencer 相互独立，禁止写成 “5→16 extension”）。
- 指针/电脑键盘/MIDI 全部进入同一 state machine；velocity/aftertouch 只映射原 pressure，MIDI clock 只映射 external clock，CC learn 只指向原控件/CV。
- 完成底部显示屏＋encoder 菜单与输入归一化校准；只保留原 keyboard 的 4 个 presets。

**退出条件**：相同控制序列从面板、电脑键盘和 MIDI 进入后，产生相同的内部 CV/gate/clock 结果。

### P5 — 完成并绑定整张面板

- 按 2400×1551 Design Coordinate Space 实现全部模块、标签、旋钮、按钮、插孔、键盘、卡带槽与六个 drone 键；geometry、hit testing、touch plate、patch point/cable 共用一个可逆 transform。
- 保留原几何/分区；只把材质与色值映射为月尘灰＋克制的五类强调色。
- 替换品牌与原创图形，不复制 ELTA/SOLAR logo 或原插画。

**退出条件**：控制清单 100% 有可见控件且无新增控件；所有缩放下逻辑坐标/状态不变，命中区、色觉区分与跳线遮挡均通过。

### P6 — 按选定档完成 dual effector

- 首次产品范围已选 **B：P1–P7，33/39**；P8 六个自发声卡带程序延期。
- 每个 program 是 DSP-family graph configuration、parameter mapping/range/default 与必要的少量特有行为；只有依赖全集、X/Y/Z/Blend/左右槽行为通过才计入 X/39。
- 左右 slot 是真正独立 instances；delay/modulator/noise/envelope/filter state 不共享，除非硬件证据要求。
- program 在 worker/control thread 完成 buffer/FFT 等全部 prepare，audio thread 只做安全 swap，旧实例在 non-audio thread 回收。切换 tail 语义无一手证据，暂用“短 fade-out、停止/重置旧状态、swap、短 fade-in”，不保留并行旧 tail、不做 PDC。
- 档位与准确覆盖数以 `05a-program-primitive-matrix.py` 的 39 行依赖矩阵复算结果为准。

**退出条件**：选定档中的每个程序可独立测试；未完成程序不能出现在可选卡带状态里冒充成品。

### P7 — 音色校准与反越界验收

- 用公开、可追溯音频先调校行为趋势；有条件时用受控实机 line-out 校准 negistor 与 VCF→distortion→gain staging 的整条 level-dependent path。
- 校准 manual source/output 电压、输入 rail/threshold/headroom 与 device normalization；明确 WET/DRY 的 `2:1 max-spec` 不等于已经证明统一口径的精确 dB 值。
- 检查参数 sweep、频响、漂移统计、FM/resonance、失真输入电平响应、动态、左右微差与 feedback，不只挑好听 preset。
- 验证 finite/runaway contract：物理 saturation 与 numerical guard 分离、denormal 安全、NaN/Inf 定位及确定性 module quarantine/reset/recovery fade；禁止全局 `[-1,1]` clamp。
- 反向审计：从 UI、MIDI、状态文件和 patch graph 尝试制造原硬件没有的路由/复音/效果；必须失败。

**退出条件**：能力报告四维全部有证据，所有相似度声明与证据等级一致。

### P8 — 桌面发布闭环

- macOS 与 Windows 重现构建、依赖 SBOM、许可证归档、`DeviceState` schema migration、缺字段默认值、损坏/临时文件恢复、崩溃与音频设备失联恢复；状态系统此时只做迁移/恢复，不是首次实现。
- 只发布独立应用；签名/公证属于分发步骤，不改变开源源码与依赖边界。

**退出条件**：干净机器安装/启动/设备选择/MIDI/音频/状态恢复/卸载均通过。

## 6. 已定产品范围

固定声音核心、路由、演奏与面板均按全量设计。bearbone 已选择 **B**；39 行依赖矩阵已经脚本复算：

| 档位 | DSP 家族 | 完整可用程序 | 产品含义 |
|---|---|---|---|
| **A 空间核心** | P1 delay/feedback + P2 reverb + P3 pitch | **14/39（36%）** | 完成主要空间、延迟与变调卡带 |
| **B 完整处理（已选）** | P1–P7 | **33/39（85%）** | 所有“处理输入信号”的卡带完整；只不做 6 个自发声卡带程序 |
| **C 全目录** | P1–P8 | **39/39（100%）** | 再完成 SYNTEX-1/GENERATOR 六个自发声程序 |

**B 是首个完整产品目标。** 它保留全部传统处理能力，缺口只落在拓扑上最特殊、内部实际包含六个不同合成程序的 P8；A/B/C 共享同一架构，后续增加 P8 不返工。

这里的百分比只表示**效果程序覆盖率**，不是整机声音相似度或总完成度。

## 7. 详细依据

- `01-architecture.md`：原机架构与信号流
- `02-sound-capability.md`：声音能力口径
- `03-ui-framework.md`：框架、许可、UI 与 I/O 约束
- `04-panel-control-map.md`：逐区域桌面映射
- `05-effector-families.md`：效果程序与 DSP 家族
- `05a-program-primitive-matrix.py`：39 个程序的可执行依赖矩阵与档位复算
- `07-core-contract.md`：Core/adapter、signal/event、PatchGraph feedback、state、实时与模拟个体契约
- `reference/`：原始资料衍生件与来源索引
