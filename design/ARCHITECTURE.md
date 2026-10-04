# Lunar 24 核心架构说明（对照 Solar 42N 官方手册）

本文逐模块说明 Lunar 24 的实现机制，以及它和 ELTA Solar 42N 硬件的相同与不同之处。
对照依据是官方手册 v15（`solar42N_instruct_08_04_v15.pdf`），页码写作 "p.N"。
手册是 ELTA 的版权，不进仓库；本文只做中文概括，不照抄原文。

标记说明：

- **【一致】**：Lunar 24 的做法与手册相同。
- **【软件化调整】**：纯软件做不到硬件那样，或者在软件里换了一种做法。这些是有意的设计，不是缺陷。
- **【手册未写明】**：手册没有给出这项信息，Lunar 24 自己选了一个合理做法，大多凭耳朵调定。
- **【手册矛盾】**：手册前后说法不一致。这里写明 Lunar 24 采用了哪一处。
- **【待办】**：核查时发现、还需要处理的问题。

---

## 1. 总体信号流（p.6 BLOCK SCHEME）

### 1.1 音频通路

```
DRONE 1-6 + EXT IN + PIEZO PREAMP + VCO A/B
  → MIX / PAN（10 路，L/R）
  → DUAL LP/BP VCF（L/R）→ DISTORTION（L/R）→ DUAL EFFECTOR（L/R）→ WET OUT L/R
VCO A → DRY OUT A，VCO B → DRY OUT B（绕过整条处理链）
```

- **【一致】主链顺序**：混音器 → 双滤波器 → 失真 → 双效果器 → WET L/R，全程左右两路立体声。
  每帧的计算在 `core/include/lunar24/core/machine_runtime.h` 的 `processFrame`：
  - 滤波器和失真合在同一步执行（`ExecutionKind::kVcfPath`），失真紧接在滤波器之后，与手册一致。
  - 效果器最后处理（`effector_.process`），输出就是 WET。
- **【一致】混音器有 10 路输入**，顺序按 p.21：DRONE 1、2、3、EXT.AUDIO、VCO A、VCO B、PREAMP、DRONE 4、5、6（`voice_mixer.h`）。
  - **【手册未写明】声像（PAN）规律**：手册没给。Lunar 24 用等功率声像，即旋钮从左到右转时，左右声道的总能量保持不变。
- **【一致】VCO A/B 同时走两条路**：既进混音器（第 5、6 路），也直通 DRY 输出。
  - **【手册未写明】DRY 从 VCA 之前还是之后取**：框图没有区分。（VCA 是"压控放大器"，由包络控制音量的开关。）
  - Lunar 24 从 VCA 之后取，并且已经混入了 SUB（低八度）。所以松开琴键后 DRY 也会随包络静下来。
- **【软件化调整】输出限幅**：WET 在 MASTER 之后加了一个软拐点限幅器（`fx::kneeLimit`）。
  - 1.3 V 以内不处理，往上平滑压向 1.95 V 的上限。
  - 硬件的模拟电路自然会饱和；软件如果没有这一步，就会在 0 dBFS 处硬削波，产生刺耳的失真。
- **【软件化调整】电压与数字电平的换算**：引擎内部一律用"虚拟伏特"计算，输出时 2 V 对应 0 dBFS（`device_adapter.h` 的 `kDeviceScaleProvisional = 0.5`）。
  - 这样手册 p.4 的上限可以直接比较：WET 最大 2 V，DRY 最大 1 V。
  - 实测（`lunar24_render`，VCO 默认设置弹一个音）：DRY 峰值约 0.57 V，在手册上限以内；WET 的限幅上限是 1.95 V，略低于 2 V。

### 1.2 输出口（DRY / WET / 耳机）

- **【软件化调整】输出口映射到电脑声卡通道**（`host/include/host/stream_plan.h`）：
  - 声卡有 4 个及以上输出：第 1–4 通道依次为 WET L、WET R、DRY A、DRY B。
  - 只有 2–3 个输出：只送 WET L/R，DRY 不输出。
- **【软件化调整】耳机口改作录音**：软件没有耳机口，面板上耳机插孔的位置改成 REC 按钮，PHONE 旋钮的位置改成 WET / DRY / ALL 选择。
  - 录音文件是 24-bit WAV，保存在 `Music/Lunar 24` 目录（见 `DECISIONS.md`）。
- **【软件化调整】额外的 MUTE 按钮**：硬件没有这个按钮。它是整机输出静音，带 10 ms 淡出，机器本身照常运行（见 `DECISIONS.md`）。

### 1.3 外部输入与压电拾音（EXT IN、PIEZO PREAMP）

- **硬件的做法**：
  - 面板上有一个 EXT AUDIO 输入口，直接接到混音器第 4 路。
  - 前置放大器（PREAMP）的信号默认来自键盘下方焊着的压电拾音片。压电片相当于接触式麦克风，敲、刮机壳都能拾到声音。
  - EXT SOURCE 插孔插上外部信号后，会替代压电片（p.12）。
- **【软件化调整】两路外部输入都来自电脑的音频输入**，没有物理插孔：
  - 第 1 通道 → EXT.AUDIO（混音器第 4 路）。
  - 第 2 通道 → PREAMP。
  - 只有一个输入通道时（比如笔记本内置麦克风），这一路同时送给两者，让内置麦克风像硬件的压电片一样能进前置放大器。
  - 面板上的 EXT SOURCE 插孔仍然可以接内部信号，插上线后会替代声卡输入（`machine_runtime.h` 的 `RuntimeInputs` 和 `kPreamp` 分支）。
- **【软件化调整】没有压电片**：在屏幕上点击触摸板不会产生拾音信号。
- **【软件化调整】两路输入默认静音**：PREAMP GAIN 和混音器 EXT.AUDIO VOL 都默认在最小。否则电脑麦克风会把房间噪声混进声音里（见 `DECISIONS.md`）。

### 1.4 控制信号源

框图下半部分列出的控制源，在 Lunar 24 中都有对应的面板插孔。任何输出口都可以用跳线接到任何输入口，每个插孔只能插一根线。

| 控制源 | 手册标注的输出 | Lunar 24 的插孔 | 结论 |
|---|---|---|---|
| LFOs | CV | LFO A/B 的 CV OUT（0..+10 V）；另外 DRONE 3/6 各有一个 LFO OUT | 【一致】 |
| JOYSTICK | CV | X OUT、Y OUT（-10..+10 V） | 【一致】；【软件化调整】用鼠标拖动屏幕上的摇杆 |
| SEQUENCER（5 步） | CV · GATE · CLOCK | CV OUT、GATE OUT、CLOCK OUT，外加 EXT CLOCK 输入 | 【一致】 |
| TOUCHPLATE KEYBOARD | CV · GATE · CLOCK | 输出：V/OCT、GATE LEFT、GATE RIGHT、PRESSURE；输入：CLOCK、RESET | 见下方"键盘的 CLOCK" |
| EGs（包络 A/B） | CV | 每个包络有 ENV 和 VCA CV 两个输出 | 【一致】 |
| VOICES（由 DRONE KEYS 触发） | CV · GATE | 每个 drone 的 ENV OUT；DRONE 3/6 另有 S&H OUT | 【一致】，详见下方 |
| ENVELOPE FOLLOWER | CV · GATE | ENV OUT（0..+10 V）、GATE OUT（0..+8 V） | 【一致】；输入就是 PREAMP 的输出 |

各控制源的细节：

- **键盘的 CLOCK**【手册矛盾】：
  - p.6 框图把 CLOCK 画成键盘的输出。
  - p.13 键盘插孔表里，CLOCK 只是外部时钟输入，没有时钟输出口。
  - Lunar 24 按 p.13 实现：没有键盘时钟输出；CLOCK 是输入，插入时钟脉冲后自动改用外部时钟。
- **键盘的演奏方式**【软件化调整】：
  - 触摸板可以用鼠标点（点得越靠下，"压力"越大）、用电脑键盘弹，或者接 MIDI 键盘。
  - 硬件的电容感应换成了这几种方式：MIDI 的力度和触后（按下后继续加压）都会驱动 PRESSURE 输出。
- **默认连线（normalled，插线后自动断开）**【一致】：
  - 键盘 V/OCT → VCO A/B 的 V/OCT 输入。
  - 键盘 GATE LEFT → 包络 A/B 的 GATE 输入。
  - VCO A → VCO B 的 CV 输入（由 CV AMT 控制深度）。
  - VCF 的 CV L → CV R。
  - 以上定义在 `spec/machine/lunar24.json` 的 `normalizedRoutes`。
- **DRONE KEYS → 声部**：
  - 【一致】按键向对应 drone 的 GATE 输入送一个门信号。门开时，ATT/RLS 包络打开声部内部的 VCA，声部发声。
  - 【一致】GATE IN 插线后，由外部信号代替按键触发（`machine_runtime.h` 的 `setDroneVoiceKey` 和 GATE IN 的处理）。
  - 【一致】每个声部的 ENV OUT 可以拿去触发下一个声部，或调制滤波器、效果器，对应框图的 "CV · GATE"（p.7/p.8）。
- **DRONE KEYS 的按键方式**：
  - 【手册未写明】按键是"按住才响"还是"按一下保持"，手册没说清楚，只提到按键会产生 5 V 门信号，另外每个声部还有 GATE/HOLD 开关。
  - 【软件化调整】Lunar 24 的按键是锁存的：按一下开，再按一下关，LED 亮表示开。这样用鼠标也能同时开多个声部。HOLD 开关保留，打开时声部常开。
  - 启动时和 RESET PANEL 后，6 个按键全部关闭（owner 的选择，见 `DECISIONS.md`）。
- **声部编号**【手册矛盾】：
  - 框图里写的是 "VOICES 1,2,3,6,7,8"，p.4 的电压表也是这组编号。
  - p.7/p.8 写的是 6 个 drone 声部，编号 1–6。
  - Lunar 24 按 p.7/p.8 的 1–6 实现。
- **LFO 的数量**【手册未写明】：p.4 说有 5 个 LFO，但没有逐个列出是哪 5 个。Lunar 24 中能单独看到的 LFO 有 LFO A、LFO B，以及 DRONE 3/6 各自的 LFO。

### 1.5 硬件没有、软件额外加的入口

- **MIDI 输入**：MIDI 键盘是演奏触摸键盘的另一种方式；MIDI 时钟可以驱动琶音器和 16 步音序器；MIDI Learn 可以把控制器的旋钮或打击垫绑定到面板控件上。
- **参数改动的路径**：面板操作和 MIDI 都通过引擎的实时队列（`StandaloneAudioEngine::post*`）把改动送进音频线程，不直接修改运行中的引擎，保证音频线程不卡顿。

### 1.6 本节核查发现

- **【待办】`machine_runtime.h` 文件开头的说明注释过时了**：
  - 注释说效果器的 4 条连线和 drone 门信号的 6 条连线"尚未接入"，WET 直接取自失真输出。
  - 实际代码中，效果器已经接在失真之后，WET 是效果器的输出；DRONE KEYS 和 GATE IN 也已经在控制各声部。
  - 只是注释错，声音没有问题。建议单独开一个小 PR 更正。
