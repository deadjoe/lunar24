# Solar 42N 架构还原（Lunar 24 声音引擎基础）
> @Claude 2026-08-23。**全部来自手册原文与面板图，无推测。**
> 出处：`reference/solar42N_manual_text.txt`（28 页全文）、`reference/solar42N_panel_2400px.png`、手册 p6 BLOCK SCHEME。

## 0. 产品定义（手册原文）
> "analogue **microtonal ambient drone machine** and **semi-modular stand-alone synthesizer**"
灵感来源：1920s–1950s 早期电子乐器（特别是 Theremin 的 "Harmonium"）＋《Blade Runner 2049》配乐。
**关键词：模拟、微分音、drone、半模块化。**

## 1. 音频通路（手册 p6 BLOCK SCHEME，逐字还原）
```
VOICE 1  ┐
VOICE 2  │
VOICE 3  │
VOICE 4  ├─→ MIX / PAN ─→ DUAL LP/BP VCF ─→ DISTORTION ─→ DUAL EFFECTOR ─→ WET OUT L/R
VOICE 5  │      (L/R)         (L/R)            (L/R)          (L/R)
VOICE 6  │
EXT IN   │
PIEZZO   ┘
PREAMP

VCO A ──┬─→ MIX/PAN（进湿链）
        └─→ DRY OUT（左，**旁路整条链**）
VCO B ──┬─→ MIX/PAN（进湿链）
        └─→ DRY OUT（右，**旁路整条链**）
```
📌 **输出是三路**：`DRY OUT(VCO A) | WET OUT L/R | DRY OUT(VCO B)`。
📌 VCO 有**独立干输出**，这是架构级特征，不是可选项。

## 2. 控制信号源（同页）
| 源 | 产出 |
|---|---|
| LFOs | CV |
| JOYSTICK（物理摇杆）| CV |
| SEQUENCER | CV · GATE · CLOCK |
| TOUCHPLATE KEYBOARD | CV · GATE · CLOCK |
| EGs（包络）| CV |
| VOICEs | CV · GATE |
| DRONE KEYS | GATE → VOICEs |
| TOUCHPLATE 的 PIEZZO → PREAMP → ENVELOPE FOLLOWER | CV · GATE |

## 3. 模块清单（面板图逐个点数）
- **6 个 drone 声部**：1/2/4/5 = "CLASSIC"；**3/6 = "NEW"（Papa Srapa 噪声合成器血统**——警笛/鸟鸣/太空怪物/枪响/海浪风声）
  - CLASSIC 每声部：5 路 MOD/TUNE + 5 段 MUTE + VOLT + GATE/HOLD + ATT + RLS + CV
  - NEW 每声部：LFO(rate/mod/divider/PITCH) + hi/low + NOISE + S&H + GATE/HOLD + ATT/RLS + env out + clock
- **VCO A / VCO B**：cv amt、**MORPHING WAVEFORM**、oct+3、low、sub、tune、pwm、pw、1v/oct、sync/osc
- **ENVELOPE A / B**：hold + A D S R + gate + env + vca cv
- **VOICE MIXER**：10 路（DRONE1/2/3、EXT.AUDIO、VCO A、VCO B、PREAMP、DRONE4/5/6），每路 **PAN + VOL**
- **DUAL VCF —— 是 POLIVOKS 滤波器，不是通用 LP/BP**（手册 p21 与规格行 L145）：
  > "**Double 12 dB filter.** The new circuit on the **POLIVOKS FILTER** is more stable and does not
  > require tuning, unlike the previous circuit on the classic **UD12 chips**.
  > Still, **you will not lose low frequencies when increasing resonance.**"
  - **12 dB＝2 极**；是新电路（比 UD12 老电路稳定、免调校）
  - ⚠️ **"提高共振不丢低频"是一条可测的行为规格**——常见谐振滤波器在共振拉高时低频会被反馈路径减掉，
    **这颗明确不会**。用教科书 ladder/SVF 直接套会错。
  - 控件：L 路 FREQ/RES/CV L/DIST，R 路 GAIN/CV R/FREQ/RES，BP↔LP 模式，**link**
  - ⚠️ **CV L 是 normalized 到 CV R 的**：手册原文 "CV L is normally connected to CV R.
    CV L controls both channels if there is no CV-signal in the CV R." → **拔线恢复/插线覆盖的典型样例**
  - **DIST＝干湿平衡，GAIN＝失真量**；失真在滤波器**之后**（L145: "dual distortion after filter"）
- **DUAL EFFECTOR**：X/Y/Z + BLEND + MASTER VOLUME + cv x/y/z + 卡带槽 + 每通道 1-2-3 程序选择
- **LFO A / LFO B**：wave、rate、×1/×6/×10
- **5 STEP SEQ**：pulser、clock、stages(3/4/5)、step1-5 各带 gate
- **JOYSTICK**：X/Y
- **PREAMP**：ext.source、gain、mic  ｜ **ENVELOPE FOLLOWER**：attack、release、env、gate
- **TOUCHPLATE 键盘**（手册 p13–20，**28 页里占 8 页，是最大单一子系统**）：
  微分音、压感、ARPEGGIATOR、DIRECTION、CV OUTPUT、PORTAMENTO、PRESSURE、ROOT NOTE、校准
- **DRONE VOICES 1–6 按键**

## 4. DUAL EFFECTOR 卡带目录（**13 张 × 3 程序 = 39 个效果**，已逐条核对）
| 卡带 | 类型 | 卡带 | 类型 |
|---|---|---|---|
| CATHEDRAL | 各类混响 | INFINITY | 大型氛围效果 |
| MAGIC | 变调延迟 | STRING RINGER | 音频率调制（ring mod）|
| TIME | 经典调制延迟 | SYNTEX-1 | 贝斯合成器 |
| VIBROTREM | 调制效果 | DIGITAL | 采样率破坏 |
| FILTER | 滤波与哇音 | GENERATOR | 噪声迷你合成器 |
| VIBE | 旋转相位调制 | ORCHE | 反向延迟 |
| PITCH SHIFTER | 八度与变调 | | |
每个程序有各自的 X / Y / Z 三个参数（全部已记录在手册 p23–24，需要时查 `reference/` 全文）。
**左右两个通道可以各装一张不同卡带的不同程序。**

## 5. 接口事实（**对桌面移植是决定性的**）
手册全文：**MIDI 出现 0 次，USB 出现 0 次。** 而 CV 86 次、gate 58 次、clock 47 次、1v/oct 4 次。
面板上只有：3.5mm CV/gate 插孔阵列、立体声 WET OUT、EXT AUDIO IN、耳机、12V 电源。
👉 **Solar 42N 的"半模块化"表现力，实体是跳线。桌面软件没有跳线，这是最大的移植落差。**

## 6. OUTS VOLTAGE SPECIFICATION（手册 L150–160 原文，**electrical domain 的地基**）
| 输出 | 范围 | 输出 | 范围 |
|---|---|---|---|
| **DRY V4 V5** | max **1 V** | JOYSTICK | −10…+10V（左 −10…−5…0／中 −5…0…+5／右 0…+5…+10）|
| **WET** | max **2 V** | S&H | −5…+5V |
| VCO | **10V（−5…+5）** | VOICE 3, 5 MODULATOR | **0…+12V** |
| EG | **0…8V** | 5 STEP SEQ CV | 0…+5V |
| ENV VOICES | −10…+10V | 5 STEP SEQ GATE | 0…+10V |
| **LFOs** | **0…+10V** | ENV FOLLOWER CV / GATE | 0…+10V ／ 0…+8V |
| PULSERL | −10…+10V | | |

> **注（2026-08-31 GH#11 收口）**：`PULSERL` 行后置 `L` 为 manual **笔误**——同名器件即 **PULSER**，其周期输出对应 registry `sequencer.clock_out`，**双极 −10…+10V 已确认**（与上表 token 一致）。此前"命名/电气对应尚未核清"的 raw-evidence conflict 由此闭合。

**电源 DC 12V 1A–2A｜重量 5.4 kg｜尺寸 49.5 × 32 × 2.9 cm（含旋钮高 5.6 cm）**

### 三条影响声音的结论
1. **范围异质，不是统一 ±1** → **内部单位用伏特**。归一化成 ±1 会丢掉各 jack 之间的相对强度，
   而那正是"LFO→FM 到底调多深"的答案。
   ⚠️ **但这张表只是 OUTS（输出）规格**，**不能反推任何 input 的 saturation/clamp 电压**。
   输入端的限幅/软饱和要各自找证据，未证实一律标 `provisional`。
2. ⚠️ **LFO 是单极 `0…+10V`，不是双极** → 拿去 FM 只往一个方向推音高。**这是原机特征，不要"修正"。**
3. **WET max 2V、DRY max 1V → 规格幅度比 2:1**。
   ⚠️ **先不要写成 −6.02 dB**：两个 "max" 用的是峰值/有效值/峰峰值哪种口径手册没说，
   口径一致才能换算成 dB。**在确认前只记比值。**

⚠️ **待核实**：表里写 "ENV **VOICES 1,2,3,6,7,8**"，与 p6 方块图同样的怪编号（应为 1–6）。**不要默默归一化。**

