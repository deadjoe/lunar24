# Lunar 24 Core Architecture Contract

> @Codex / @Claude 共同把关。本文把总计划中的产品边界落实为实现期不可破坏的技术契约；它不是新增产品功能，也不是工期表。

## 1. 依赖方向

```text
IGraphics UI ──→ Lunar control adapter ──→ lunar-core
iPlug2 APP  ──→ audio/MIDI/device adapter ─┘

lunar-core ──→ lightweight snapshots/events ──→ adapters/UI
```

- `lunar-core` 是纯 C++ library，不 include iPlug2、IGraphics、CoreAudio、WASAPI、窗口或文件系统类型。
- iPlug2 只负责 standalone 生命周期、设备 I/O、MIDI、窗口与参数桥接；IGraphics 只负责显示和交互。
- Lunar 的 canonical `Parameter` / `DeviceState` / `PatchGraph` 不能建立在 `IParam` 或 UI 控件对象上；只有 `Lunar Parameter → iPlug2 adapter`。
- UI 不得直接修改 DSP 对象成员。路径固定为 `UI/input → ControlEvent/command layer → core`；反向只传轻量 snapshot，不能锁住音频线程。

因此 P1 若证明 iPlug2 standalone 不合适，替换外围设备/UI 适配器不要求修改 Synth Core。

## 2. P0 必须冻结的 canonical model

唯一的 machine definition 定义并绑定以下概念：

- `ModuleId / ParameterId / JackId / ProgramId`：稳定、与显示名称、语言和 UI 坐标无关；进入状态格式后不可重用或随意修改。
- `ParameterDescriptor`：真实单位、范围、默认值、平滑策略和持久化语义。
- ⚠️ `ParameterDescriptor` 是**标量**：单一数值 ＋ 单位/范围/默认/平滑/持久化。
  **非标量的设备状态不进参数库** —— `vector`（逐音板调音、按钮值）、`record`（序列步、键盘 preset A–D）、
  `mask`（音阶开关）走 DeviceState 的结构化字段（见 §6），不表示为 ParameterDescriptor。
  registry 完整性门禁据此把它们判为**结构性 gap（Root A: "must gap"）**。
  **这不是待办事项：把它们拍扁成标量才是错的。** 冻结目标 357 参数中**有 8 项属于此类**。
- ⚠️ 另有一类 gap：目标声明为 `selector-toggle` 但**未给出 positions（值域）**，
  手册只描述行为不枚举取值时即属此类。**在拿到值域证据前必须保持 gap，不得臆造 positions，
  也不得改判为 continuous 来绕过。**
- 两类合计：冻结目标 357 中 **12 项必须保持 gap**（8 非标量 ＋ 4 无值域 selector），
  因此 **landed 参数上限 = 345**，最终 `kParameterIdSpace = 412`。
  **这 12 项不是欠账**；把 gap 推向 0 意味着拍扁结构或臆造值域，两者都是错的。
- `JackDescriptor`：方向、推荐信号用途/范围、normalized route；用途是提示和保护数值范围，不是阻止跨类型实验连接。
- `NormalizedRoute`：正式图边；插线覆盖，拔线恢复，不允许散落在模块代码里的 `if jack empty` 特判。
- `PatchGraph`：连接事实；屏幕 cable 只是它的 visualization，不是连接状态本身。
- `ControlEvent`：带 block 内 sample offset 的外部离散事件或参数命令。
- `AudioBlockView`：框架无关、预分配的输入/四逻辑输出 buffer view。
- `ModuleExecutionContract`：模块/程序在 `prepare(sampleRate, maxBlockSize)` 后声明实际 `intrinsicLatencySamples`、每条具体 input→output 路径的最小 causal delay/直通可能性、资源上限与是否允许进入 cyclic SCC；这些是调度事实，不是用户可见的 PDC 参数。
- `DeviceStateV1`：唯一整机当前状态；schema/version 从 P0 开始，不等到发布阶段才补。
- `AppSettings`：音频/MIDI 设备、channel mapping、UI scale 等外围偏好；不进入 Synth Core，也不混入声音状态。

同一份 machine definition 必须供 DSP、UI、MIDI adapter、serialization 和 tests 使用，不能维护五份参数表。

## 3. Event 与内部 signal 不是同一种东西

### 外部事件

鼠标、电脑键盘、MIDI note/CC/clock 和 UI command 是 timestamped `ControlEvent`。进入 core 后再转换为既有的 parameter、CV、gate 或 clock 行为。

### 内部连续信号

oscillator、audio、noise、LFO、S&H、envelope、CV 以及 patch cable 上的值属于逐 sample 的内部 signal。

三种执行语义必须分开：

1. clock edge、gate edge、sync、sequencer trigger：sample-accurate discrete event；
2. knob、joystick、MIDI CC：以秒为单位的 continuous smoothing，避免 zipper noise；
3. exposed jack modulation（包括 LFO/CV/audio-rate FM）：保持 audio-rate 能力。

慢速 UI/状态 housekeeping 可以低频运行，但不得因此把所有 LFO/CV 固定成低 control rate。

### Signal / Electrical Domain

- 在 Jack 与模块边界，`SignalSample` 的统一物理含义是 **virtual volt**：数值 `1.0` 表示 1 V。模块内部可以用别的归一化表示，但进入/离开公开端口必须显式转换。
- pitch CV 固定为 1 V/octave-equivalent；不能让每个 VCO/FM 入口各自猜比例。
- 每个 `JackDescriptor` 必须定义 `polarity`、nominal/tolerated voltage range、DC coupling、每伏调制深度或 transfer curve、gate/clock rising/falling threshold 与 hysteresis、输入 saturation/rail 行为及证据状态。
- 手册已给出的输出范围直接进入 registry；未给出的 threshold/rail 值必须标 `provisional/unverified`，在 P2 前冻结，不能藏成代码常数。
- 手册 `OUTS VOLTAGE SPECIFICATION` 当前给出的 source/output 事实是：`DRY V4/V5 max 1 V`、`WET max 2 V`、`VCO -5…+5 V`、`EG 0…8 V`、`ENV VOICES -10…+10 V`、`LFO 0…+10 V`、`PULSER -10…+10 V`、`ENV FOLLOWER CV 0…10 V / GATE 0…8 V`、`JOYSTICK -10…+10 V`、`S&H -5…+5 V`、`VOICE 3/5 MODULATOR 0…12 V`、`5 STEP SEQ CV 0…5 V / GATE 0…10 V`。其中 LFO 的单极性必须保留；`ENV VOICES 1,2,3,6,7,8` 的异常编号原样记录为 `unverified`。
- 上表是**输出**规格，不能反推出所有输入的 clamp/saturation 电压。输入 rail、soft saturation、gate threshold 与 hysteresis 必须逐模块找证据或标 `provisional`，不能拿输出范围替代。
- 模块求和 headroom 与 gain staging 由该模块契约定义；不在每条 cable 或每个模块出口统一 hard-clamp `[-1,1]`。硬件本来存在的 rail/saturation 在对应位置建模。
- Core 的 WET/DRY 仍以 virtual volts 输出；`DeviceAdapter` 才把它映射到 audio-device normalized float。手册只证明 WET:DRY 的 max-spec 幅度比是 `2:1`；在确认 peak/RMS/load 口径一致前，不把它固化为精确 dB 声明。

### ControlEvent queue

- 正常容量内保留参数事件的 sample offset 与顺序；只有 queue pressure/overflow 路径才可按稳定 `ParameterId` coalesce 为尚未消费事件中的最新 target。note/gate/clock/sync/reset edge 属于 critical lane，不能与旋钮事件共用任意丢弃策略。
- critical queue 容量必须覆盖声明的最大 MIDI/clock burst。若外部生产者仍超界，不阻塞音频线程：记录固定大小诊断，下一安全边界用 authoritative performance state 做确定性 reconcile；无法判明时执行 all-gates-off/clock resync，而不是随机丢一条 edge 留下 stuck note。
- 同一 sample 的默认 phase order 固定为：`Reset/failsafe → Parameter/Pitch/Pressure target → Note/Gate Off → Sync/Clock edge → Note/Gate On`；同 phase 再以稳定 source ID 和 producer sequence 排序。若手册证据要求某模块不同，由该模块契约明确覆盖并测试。

## 4. PatchGraph 是信号网络，不是 modulation matrix

- 所有真实面板输出可以连接所有真实面板输入。类型/颜色只作为提示；core 不按 `audio/CV/gate/clock` 标签拒绝连接。
- 普通用户 cable 的 cardinality 固定为：每个 input 最多 1 根、每个 output 最多 1 根；连接已占用 jack 时原子替换旧 cable。硬件证据明确支持 stackable/multiple 前，不提供 fan-out 或隐形 mult。
- `NormalizedRoute` 不占用户 cable cardinality，并可按硬件证据一源多目的；对应 input 插线只覆盖该 input 的 normalized edge，拔线恢复。
- gate/clock 对外仍有清晰语义，但在输入端由 signal 的 threshold、hysteresis 与 edge detector 解释；它们不是与 CV 完全隔绝的另一套网络。
- 这允许 `noise→gate`、`LFO→clock`、`audio oscillator→clock`、`S&H→gate` 等硬件式实验连接。
- 仍然只存在原面板的 jack、normalized route 与模块；统一 signal layer 不能演化成隐藏调制矩阵或新增逻辑节点。

### Feedback 的确定性规则

- 普通无环连接不凭空增加 sample 或 buffer 延迟。
- PatchGraph 只在插拔线导致拓扑变化时重新做 strongly connected component（SCC）分解，不在每个 audio block 重算。无环区域继续按 block 执行；**含环 SCC 必须逐 sample 执行**，否则 `z⁻¹` 会在 block 调度中悄悄退化成整块延迟。
- graph compiler 在每个含环 SCC 中计算确定性的 feedback-edge set（以稳定 ID 排序）。只有某条 cycle 实际经过的**具体 input→output 路径**明确具有正的最小 causal delay，编译器才使用该真实延迟破环，不再额外叠加 `z⁻¹`；模块另一支路有延迟不算。其余被选中的跨模块反馈边引入恰好 `z⁻¹` one-sample delay。任何 cycle 都必须被至少一条已声明 causal path edge 或 `z⁻¹` 切断，sample `n` 必须能读取同一 block 内已产生的 sample `n-1`。
- 不能用 one-audio-buffer delay 破环；64/128/256 buffer 不得改变 feedback 延迟或调制时序。
- `intrinsicLatencySamples > 0` 只是模块级调度信息，不能直接授予破环资格。若某 input→output 仍可能走零延迟 Direct/mix 支路，编译器按该路径在所有可达参数状态中的**最小延迟 0** 处理；除非关闭直通会触发拓扑重编译并由契约证明其 gain 精确为零。窗口/FFT/倒放类模块必须提供确定的 causal 两阶段/逐 sample 接口，或在当前版本声明不允许进入 cyclic SCC，不能静默把整条环变成 block delay。
- 不做 plugin/DAW 式 delay compensation（PDC），也不为了“对齐”给平行路径补延迟；真实算法延迟属于硬件式声音行为。模块/程序数量只有算法选定并 `prepare` 后才能从实际声明统计，不能先按 DSP family 猜数。
- 模块内部确需 zero-delay feedback 的非线性结构，由模块自己的 solver 负责，不交给通用 PatchGraph。
- feedback-edge set 是由最终 graph 确定的编译结果，恢复同一 DeviceState 必须得到同一结果，不依赖用户插线先后顺序。

## 5. 实时与 sample-rate 不变量

- 音频线程在所有阶段都必须无 heap allocation、无 mutex、无文件/JSON、无慢日志；这不是 P1 一次性测试。
- 图或状态结构更新在 control thread 构建不可变 snapshot，通过预分配 command/FIFO 或无锁交换在安全边界生效。
- 音频线程只持有非 owning handle/epoch，不能让 `std::shared_ptr` 的最后一次 release 或任何 graph/state destructor 落在音频线程。旧 snapshot 必须通过 fixed pool、epoch/RCU、deferred reclamation 或 triple buffer 在 non-audio thread 回收；具体机制在 P1 spike 决定。
- Debug/CI 至少监测 audio-thread allocation、command/FIFO overflow、render deadline overrun；锁禁令同时用代码审查/静态规则守住，不虚构“能捕获所有非法锁”的万能检测。
- Core 由 host sample rate 配置，不含固定 48 kHz 常数；时间常数用秒、频率用 Hz。测试 44.1/48/88.2/96 kHz。
- 同一事件流与 patch 在不同 host buffer size 下应产生等价的 sample 时序和基本相同的声音；buffer size 只影响调度/latency。
- Core 永远产出 `WET L / WET R / DRY A / DRY B`；物理 2/4-channel 选择只属于 `DeviceAdapter`。

### 数值安全与 runaway recovery

- **音色 saturation** 与 **数值 finite guard** 是两套机制：前者只在有硬件 rail、非线性 stage 或后续测量证据的位置出现并参与声音；后者只防止非法数值污染整条图，不塑造音色。
- 不做全局或逐模块出口 `[-1,1]` clamp。有限但过大的反馈由正确位置的 rail/saturation、gain staging 与设备输出转换处理；没有输入证据的地方不能借“安全”之名发明可听见的软削波。
- denormal 使用 FTZ/DAZ（平台支持时）或等价的显式近零处理；不得让 denormal 造成跨平台性能崩塌。
- Debug/CI 用固定大小 flags/counters 定位首个 NaN/Inf、module ID 与 sample offset，不在 audio thread 打日志或分配内存。
- Production 遇到非 finite sample 时，立即以安全零值阻断传播，并在下一安全边界隔离/重置受影响 module，配合短 recovery fade 恢复；所有行为确定、无分配、无锁、可回归测试。紧急 numerical guard 必须与音色 saturation 分离，不能成为正常信号路径的一部分。

## 6. DeviceState、KeyboardPreset 与保存

`DeviceStateV1` 至少包含：

- 所有物理旋钮/开关参数；
- patch cables 与 normalized-route override；
- keyboard 当前设置及其原生 4 个 `KeyboardPreset`；
- dual-effector cartridge/program；
- sequencer 的物理设置（不保存当前瞬态 gate、playhead、envelope phase、delay buffer）；
- `UnitIdentitySeed`、calibration state、schema version。

边界：

- `DeviceState` 只用于唯一“上次退出状态”；没有多套整机 preset 浏览器。
- `KeyboardPreset` 只属于原 keyboard subsystem，不能序列化成整机 preset。
- control thread 从 canonical state 取得一致 snapshot，debounce 后写临时文件、flush、atomic replace；音频线程不参与磁盘保存。
- 启动先迁移/校验/恢复 state，再启用正常 audio，并用短 fade-in 避免默认状态瞬间出声。
- P8 处理 schema migration、缺字段默认值、损坏/临时文件恢复；状态系统本身从 P0/P2 就存在。

## 7. “同一台机器”的模拟个体

- `component tolerance` 是一台实例固定的体质；`drift/noise/wander` 是随时间变化的过程，两者不能混成每次启动重新 random。
- 首次运行生成并持久保存 `UnitIdentitySeed`，确定 20 个 classic drone oscillator、左右 VCF/distortion 与 gain calibration 的固定微差。不得依赖未规定算法的 `std::random`；保存 `identityModelVersion` 和派生 calibration profile，使软件升级不会悄悄把同一台机器“重抽一次”。
- 测试可注入固定 seed 与固定随机流；正式运行保留同一 identity，但动态 drift 不必序列化为整机 preset。
- classic drone 从首个可听版本就必须支持独立 oscillator、free-running phase、固定 tolerance、慢漂移与非线性；不得共用 global phase/reset，也不得先用干净 saw+detune 把结构写死。
- drift 至少预留 very-slow drift、medium wander、小噪声、oscillator-specific variation 与可相关环境项；参数只能随证据校准，不能假装已有精确电路模型。
- VCF 是 Solar 新版 **12 dB Polivoks LP/BP**，不是通用 ladder/SVF：必须保持“提高 resonance 不丢低频”的可测行为，并允许输入电平驱动非线性；但不能把经典 UD12 电路的漂移/失准照搬回来，因为手册明确新版更稳定且无需调校。
- VCF→post-filter distortion→gain staging 作为整条 level-dependent path 校准；左右拓扑可共用代码，但 calibration/nonlinear state 独立。`CV L → CV R` 是已证实的 NormalizedRoute。

## 8. UI coordinate contract

- `2400×1551` 是唯一 Design Coordinate Space，不是默认物理窗口像素。
- geometry、hit testing、touch plate、patch point 与 cable 全部经同一个可逆 transform 映射；DPI/scale 不能改逻辑坐标或 DeviceState。
- 不 reflow、不分页、不为不同 DPI 保存多套位置。
- 技术切片验证 50/67/75/100/125/150/200% 与 fit-to-window；较小尺寸允许整体缩放/平移，但不能隐藏或重排面板。

## 9. Dual effector contract（已选 B）

- 首次完整产品范围为 P1–P7：33/39 input-processing programs；P8 六个自发声 programs 延后。
- 一个 program 是 DSP-family graph configuration、parameter mapping/range/default 与必要的少量特有行为，不复制整套 DSP。
- 左右 slot 是真正独立 instances；delay/modulator/noise/envelope/filter state 不共享，除非后续硬件证据要求共享。
- program change 必须在 control/worker thread 创建并按当前 sample rate/max block size 完成 buffer、FFT plan 等全部 `prepare`；audio thread 只在确定的安全边界切换预备实例，旧实例交 non-audio reclamation。callback 内禁止构造 DSP、分配 delay/reverb buffer 或建立 FFT plan。
- 手册没有说明卡带/program 切换瞬间的 tail 行为，记录为 `effect_program_switch_tail_semantics=unverified`。获得实机证据前的 provisional 行为是：短 fade-out → 原实例停止且状态重置 → swap → 短 fade-in；不并行保留旧 tail、不做双实例 crossfade、不做 PDC，从而避免增加硬件没有的第二条效果路径。
- `33/39` 永远只表示 effect-program coverage，不表示 85% 音色准确度或整机完成度。

## 10. 证据边界与未决项

- 无受控实机 line-out：只声明 architecture/behavior reproduction。
- 公开音频可用于 perceptual tuning，不能称为严格 calibration。
- 受控同参数 line-out 才能验证 sweep、频响、漂移统计、FM/resonance、失真电平响应、动态、左右微差与 feedback。
- 不预设 transistor-by-transistor/SPICE；先做 behavioral model、测得的非线性、tolerance、drift 与 gain staging。
- 官方 Solar 42N 手册与 block scheme 证明 VCO A/B 有独立 dry outputs，但未说明插入 dry jack 是否带 switching/normalization。P0 将 `dry_jack_insertion_semantics` 标为 `unverified`；在获得 ELTA/实机证据前，不把“插入会切断 wet route”写进产品行为。

## 11. 对反馈的取舍结论

- **采纳**：Core/框架隔离、canonical registry/state、stable IDs、三类时序、virtual-volts electrical contract、jack cardinality、module causal/latency contract、snapshot 非音频线程回收、queue 优先级与确定顺序、program realtime swap、finite recovery、四逻辑输出、buffer/sample-rate 不变量、状态原子保存、UnitIdentitySeed、统一坐标变换、独立 FX instances、证据等级。
- **限定采纳**：统一 patch signal world 不等于把 external event、parameter 与 audio signal 塞进一个万能类型；输出电压规格不能反推输入 rail；真实 causal delay 可替代对应 cycle 的额外 `z⁻¹`，但算法 latency 数字本身不能；不做 PDC；“arbitrary sample rate”指 core 不硬编码且覆盖 host 支持的标准 rates，不承诺任意非法数值。
- **暂不下结论**：DRY jack 插入是否改变 wet route、effect program 切换时真实 tail 行为，等待一手硬件证据。
- **不采纳为当前方向**：把项目变成 SPICE/逐晶体管仿真、万能软件合成器、整机 preset library，或为调试承诺无法可靠实现的“捕获所有锁”机制。
