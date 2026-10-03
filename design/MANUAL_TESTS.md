# Lunar 24 人工测试指南

按模块逐项在 Mac 应用里测试。每项包含目标、准备、步骤、期望和最近一次结果。
名称一律用**面板上印的字**（大小写照抄），找不到时先看下面的「面板地图」。
新测试和新结果直接补在对应位置。

---

## 通用操作

| 操作 | 方法 |
|---|---|
| 转旋钮 | 按住上下拖。按住 Shift 拖是微调；滚轮也可以；双击回到出厂值。最左约 7 点钟，最右约 5 点钟 |
| 看数值 | 鼠标悬停在旋钮上，上方显示当前数值 |
| 按钮 | 点一下切换，打开时有琥珀色光圈 |
| 拨杆开关 | 点开关上半部往上拨，点下半部往下拨 |
| 接线 | 从一个插孔按住拖到另一个插孔松手。只能**输出 → 输入**：红字或带红色 ▲ 的插孔是输出，带黑色 ▶ 的是输入 |
| 拔线 | 在线的**输入端**插孔上按住往外拖 |
| 一个输出只能接一根线 | 同一个输出再接一根，旧的那根会被换掉 |
| 弹琴 | 鼠标按住键盘区的金属触摸板；按住后上下移动 = 按压力度（越往下越重） |
| 电脑键盘弹琴 | 先点一下面板。A S D F G H J K L ; = 白键，W E T Y U O P = 黑键；Z / X = 降 / 升八度（蓝色小屏显示 OCT） |
| 键盘菜单 | 点键盘区中间的红色大旋钮打开；右上角 CLOSE 或 Esc 关闭 |
| 恢复出厂 | 键盘菜单右下角 **RESET PANEL**，点一次变成 CLICK TO CONFIRM，4 秒内再点一次 |
| 静音 | DRONE VOICES 右边的 **MUTE** 按钮：所有输出以约 10 ms 淡出静音，再点恢复；静音时亮琥珀色光圈（MIDI 绑定触发时也一样） |
| 音频设备 / 麦克风 | 屏幕顶部菜单栏 Lunar 24 → Preferences…（⌘,） |

### 测试常用准备

- **出厂状态**：RESET PANEL（点两次）。几乎每项测试都从这里开始；不会清除 MIDI 绑定或 CHANNEL / TRANSPOSE / VELOCITY。
- **去掉混响**：DUAL EFFECTOR 区的 **BLEND** 拖到最左。待参数平滑结束后只听干声；这不排除包络释放、滤波器振铃或其它仍在发声的通道。
- **只听某一路（solo）**：VOICE MIXER 里其它 9 路的 **VOL** 拖到最左。或者在 DRONE VOICES 里只留要听的 drone。
- **关掉所有 drone**：键盘区右边 DRONE VOICES 的 6 个键全部关掉。
- **说法约定**：本文说"打开 / 关掉 DRONE N"或"DRONE VOICES 的 N"，都是指键盘区右边 DRONE VOICES 的第 N 个按键（drone 的开关，带包络）；VOICE MIXER 里的 **VOL** 只调音量，会明确写"VOL"。
- 卡音（声音停不下来）时：先点 **MUTE** 静音，再试 RESET PANEL。

---

## 面板地图

面板从上到下分三排，下面是键盘区。下表从左到右列出每排的模块。

| 位置 | 从左到右 |
|---|---|
| 上排 | DRONE 1 · DRONE 2 · DUAL EFFECTOR（上半）+ FILTER L / FILTER R（下半一排）· DRONE 4 · DRONE 5 |
| 中排 | DRONE 3 · VCO A · VOICE MIXER（下方是 envelope A / envelope B）· VCO B · DRONE 6 |
| 下排（白字标签） | LFO A · JOYSTICK · 5 STEP SEQ. VOLTAGE · PREAMP · ENVELOPE FOLLOWER · LFO B |
| 键盘区 | 左下角摇杆 · 12 块触摸板 · 中间红色大旋钮（菜单）和蓝色小屏 · 右边 DRONE VOICES（1 2 3 / 4 5 6）和上下排列的 **MUTE**、**MIDI** 按钮 |

### 各模块控件（与面板印字对应）

**经典 drone（DRONE 1 / 2 / 4 / 5，布局相同）**

- 5 列振荡器，列顶印 1–5。每列从上到下依次是：
  - **MUTE** 按钮；
  - **TUNE** 旋钮；
  - **MOD** 按钮。
- 右侧：
  - 红色 LED 灯条；
  - 青色 **VOLT** 旋钮；
  - 大白圆是光敏传感器，目前只是装饰。
- 下半部从左到右：
  - **HOLD** 按钮、**ATT** 旋钮、**RLS** 旋钮、**CV ▼** 旋钮；
  - 再往下是插孔：GATE 输入（最左）、红字 **env** 输出、CV MOD 输入（CV ▼ 正下方）。

**DRONE 3 / DRONE 6（布局相同）**

- 上方：
  - 小字 **LFO 1 : 10**，下面是 1:10 按钮；
  - **fm** 按钮、**am** 按钮。
- 青色旋钮：**rate**、**mod**、**divider**、**PITCH**。
- PITCH 下方是 **hi/low** 按钮。
- 中部插孔：
  - rate 下方是 LFO 输出；
  - mod 下方是 **cv** 输入（▲ cv）。
- 下半部：**HOLD** 按钮、**ATT**、**RLS**、青色 **NOISE**。
- 最下排插孔从左到右：
  - GATE 输入；
  - 红字 **env out**；
  - **S&H** 框里的 IN、**clock**、OUT（最右，带红三角）。

**VCO A / VCO B（布局相同）**

- 上排：绿色 **cv amt**、拨杆 **oct+3 / low**、拨杆 **-1 / sub**、绿色 **tune**。
- 左边拨杆：**lin / exp**（上 = lin，下 = exp）。
- 下排旋钮：绿色 **pwm**、中间大旋钮 **MORPHING WAVEFORM**、绿色 **pw**。
- 最下排插孔：
  - VCO A：**1v/oct**、**cv**、**pwm**、**sync**；
  - VCO B：**1v/oct**、**cv**、**pwm**、红字 **osc**（VCO B 的输出）。

**MORPHING WAVEFORM 的图标位置**（旋钮指向图标就是那种波形，图标之间平滑过渡）

| 时钟方向 | 约 9:30 | 10:30 | 11:30 | 12:30 | 1:30 | 2:30 → 最右 |
|---|---|---|---|---|---|---|
| 波形 | 正弦 ∿ | 三角 | 锯齿 | 方波 ⊓ | 反锯齿（✕ 图标） | 正弦渐变到三角 |

**envelope A / envelope B（VOICE MIXER 下方）**

- 上面两个黑色按钮：
  - 左边（"hold" 字右边）：**hold**；
  - 右边（⋀⋀ 图标右边、包络形状图标左边）：自激 self-gen。
  - 旁边的金色圆点是指示灯。
- 4 个绿色旋钮：**A D S R**。
- 下排插孔：
  - envelope A 从左到右：**gate**、红字 **env**、**vca cv**、红字 **VCO A**（VCO A 的 dry 输出）；
  - envelope B 从左到右：红字 **VCO B**（dry 输出）、**gate**、红字 **env**、**vca cv**。
- envelope A 控制 VCO A 的音量，envelope B 控制 VCO B。

**VOICE MIXER**

- 10 路，上排黑色 **PAN**，下排灰色 **VOL**。
- 顺序：DRONE 1 · DRONE 2 · DRONE 3 · EXT.AUDIO · VCO A · VCO B · PREAMP · DRONE 4 · DRONE 5 · DRONE 6。

**DUAL EFFECTOR + FILTER**

- 上排插孔：**cv x**、**cv y**、**cv z**；下方是橙色 **X**、**Y**、**Z** 旋钮。
- 中间卡带窗口：
  - 显示卡带名和 L/R 程序名；
  - 点击换下一盒，右键或 Shift+点击退回上一盒；
  - 下方两个 **1-2-3** 拨杆（左边管左声道，右边管右声道），中间小按钮也是换卡带。
- 1-2-3 拨杆下方两个小黑旋钮：**MOD L**、**MOD R**（滤波器 CV 的深度）。
- 右边：**BLEND**、大旋钮 **MASTER VOLUME**、耳机音量。
- 滤波器一排（FILTER L → FILTER R）：
  - **FREQ**、**BP/LP** 小按钮（FREQ 与 RES 之间偏上）、**RES**；
  - **CV L** 插孔、**DIST**、**link** 按钮、**GAIN**、**CV R** 插孔；
  - **FREQ**、**BP/LP**、**RES**。

**下排**

- **LFO A**：红色 **wave**、它右边的输出插孔、拨杆 **x6 / x1 / x10**、红色 **rate**。LFO B 相同。
- **JOYSTICK**：
  - 红色 **X**（OFFSET X）旋钮；
  - 两个输出插孔，上面是 X、下面是 Y；
  - 红色 **Y**（OFFSET Y）旋钮；
  - 摇杆本体在键盘区左下角，拖动后停在原位，双击回中心。
- **5 STEP SEQ. VOLTAGE**：
  - 红色 **pulser**（速度）；
  - **clock** 两个插孔：上面是 CLOCK OUT，下面是 EXT CLOCK 输入；
  - 拨杆 **stages**（4 / 5 / 3）；
  - **step 1–5** 红色旋钮，各自右边一个 **gate** 拨杆；
  - 最右两个插孔：上面红字 **cv**，下面红字 **gate**。
- **PREAMP**：**ext. source** 插孔、红色 **gain**。
- **ENVELOPE FOLLOWER**：红色 **attack**、红色 **release**、红字 **env** 插孔、红字 **gate** 插孔。

**键盘区**（插孔没有文字，用图标标示）

- 红色大旋钮左边一组 3 个插孔，从左到右：
  - ◷（时钟图标）CLOCK 输入；
  - ⚡（闪电图标）RESET 输入；
  - ⊓（脉冲图标）GATE L 输出。
- 右边一组 3 个插孔，从左到右：
  - ⊓ GATE R 输出；
  - ↓（往下按的箭头）**PRESSURE** 输出；
  - ↳（拐弯箭头）V/OCT 输出。
- 红色大旋钮两侧的白色圆按钮：▽ 降八度，△ 升八度。下方蓝色小屏显示 OCT。

### 键盘菜单（6 个页签）

| 页签 | 卡片：设置 |
|---|---|
| **PLAY** | KEYBOARD：PLAY、MODE · QUANTISER：SCALE（< > 切换）、ROOT（小琴键） · CLOCK：TEMPO（旋钮，下方 < > 每次 ±1 BPM）、5-STEP SEQ CLOCK |
| **EXPRESSION** | PORTAMENTO：GLIDE、LEGATO · VIBRATO：RATE、DEPTH、DELAY、PRESSURE · PRESSURE OUTPUT：模式一排、RISE、FALL |
| **ARP** | ARPEGGIATOR：HOLD、DIRECTION、VARIATION、INTERVAL · ARP RHYTHM：LENGTH 和 8 个节奏按钮 |
| **SEQ** | SEQUENCER：RUN、CV OUTPUT、DIRECTION、LENGTH · SEQ RHYTHM：LENGTH 和 8 个节奏按钮 |
| **SEQ STEPS** | 16 步：每步一个推子（音高）和一个 GATE 按钮 |
| **SERVICE** | 顶部警告条 · OUTPUT CALIBRATION：V/OCT OUT、PRESS OUT、DAC VREF · TOUCH SENSOR：TOUCH、RELEASE、P MIN、P MAX · MPR121：CHARGE、DISCHARGE、DEBOUNCE · ENCODER：DIRECTION |

- 选项直接点选（亮的是当前项）；开关（LEGATO、HOLD）的红灯亮 = ON。< > 到头时变灰，不循环。
- 底部一排：**PRESET** A B C D（直接点选）、**LOAD**、**SAVE**、**INIT**，右边 **RESET PANEL**。
- PLAY = SPLIT 时标题行出现 **EDITING LEFT C-F / RIGHT F#-B**；按左右分开的卡片标签显示 LEFT / RIGHT，共用的显示 GLOBAL。
- 重新打开菜单回到上次的页签；RESET PANEL 后回到 PLAY。

---

## 测试用例

结果一栏：✅ 通过；🔧 发现问题，已修复后通过（括号里是修复的 PR）；⏳ 还没测。

### 0. 应用与面板

**T0.1 启动和外观** ✅ (#56)
- 目标：应用能打开，面板完整、可缩放。
- 步骤：
  1. 打开应用。
  2. 拖动窗口边角改变大小。
  3. 点绿色按钮进入全屏，再退出。
- 期望：
  - 标签文字都显示；控件有立体感；面板随窗口等比缩放。
  - 缩到最小（约 1200×776）后拖不动。
  - 全屏时面板居中，四周是窄边框。

**T0.2 基本操作** ✅ (#56)
- 步骤：
  1. 悬停、拖动、Shift 拖动、双击一个旋钮。
  2. 从一个红字输出插孔拖线到一个黑字输入插孔，再从输入端把线拔掉。
  3. 点卡带窗口。
  4. 打开再关闭键盘菜单。
- 期望：
  - 旋钮显示数值，能转动，能微调，双击回默认。
  - 线两端对准插孔，能拔掉。
  - 卡带名切换。
  - 菜单能开关。

**T0.3 音频设备** ✅ (#57, #66)
- 步骤：
  1. 戴上或摘下 AirPods 各一次。
  2. 打开菜单栏 Lunar 24 → Preferences…。
- 期望：
  - 声音自动跟随当前输出设备，没有杂音。
  - 音频启动不了时，蓝色小屏显示 NO AUDIO。
  - Preferences 窗口能打开。

**T0.4 RESET PANEL** ✅ (#58)
- 步骤：
  1. 随便转几个旋钮，接一根线。
  2. 键盘菜单 → RESET PANEL → 4 秒内再点一次。
- 期望：旋钮回到原位、线消失，声音短暂停顿后恢复成默认 drone。

**T0.4a 重置和设备重开后的操作** ✅ (#81；Mac 实测重置、弹奏及 BlackHole 切回 Speakers)
- 准备：记录想保留的面板设置；这项测试会 RESET PANEL。
- 步骤：
  1. 键盘菜单 MODE 改成 ARPEGGIATOR，PRESSURE 改成 LOOP，接一根 PRESSURE → VCO A cv 的线；然后 RESET PANEL（两次确认）。
  2. 检查 MODE 回到 KEYBOARD、PRESSURE 回到默认值、接线清空。关闭 DRONE VOICES 1–6，BLEND 最左，按电脑 A 键再松开。
  3. 预期按下能出声、松开后按包络释放，未按键时没有旧音符自行触发；MODE 和接线不自行跳回。
  4. 将 PRESSURE OUTPUT 改成 AD、RISE 调到约 3/4；在 Settings 将 Output 从 MacBook Speakers 切到 BlackHole 2CH，Apply，再切回 Speakers，Apply / OK。预期设置和接线保持切换前的状态，A 键按下、松开仍正常。
- 队列中“恰好尚未处理”的旧操作由自动回归测试确定性覆盖；人工步骤检查真实重置和设备重开流程，不要求反复抢时间操作。不需要接 MIDI 控制器。

**T0.5 MUTE** ✅
- 准备：RESET PANEL（默认 drone 在响）。
- 步骤：
  1. 点 DRONE VOICES 右边的 **MUTE**。
  2. 等几秒，转几个旋钮。
  3. 再点一次 **MUTE**。
- 期望：
  - 第 1 步：声音很快淡出到完全无声，没有咔哒声；按钮亮琥珀色光圈。
  - 第 2 步：仍然无声。
  - 第 3 步：声音恢复成当前面板的声音（机器在静音期间一直在运行）。
  - 重新打开应用时 MUTE 是关的。

**T0.6 指示灯** ✅
- 准备：RESET PANEL。
- 步骤与期望：
  - 每个 drone 的 **HOLD** 下面的琥珀灯：drone 开着时亮；在 DRONE VOICES 里关掉它，灯按 **RLS** 的时间慢慢变暗（RLS 拉大更明显）。
  - envelope A / B 的琥珀灯（envelope 字样右边的金色圆点）：按住一个键时亮，松开后随 **R** 变暗；打开自激时有节奏地闪。
  - LFO A / B 的蓝灯（输出插孔下方）：跟着 LFO 一亮一暗；调 **rate** 时闪的快慢跟着变。
  - 5 STEP SEQ. VOLTAGE 每步上方的红灯：按 T10.1 接好线，灯依次走动，显示当前是第几步。
  - PREAMP / ENVELOPE FOLLOWER（按 T11 打开麦克风，gain 转到中间）：
    - 说话时 ENVELOPE FOLLOWER 的红灯跟着音量亮；有声音时绿灯（gate）亮；
    - **gain** 开得很大、声音很响时，PREAMP 的蓝灯（削波）闪亮。
  - DRONE 3 / 6 S&H 图案里的红灯：按 T3.4 接好 S&H，每跳一个音闪一下。
  - JOYSTICK 的两颗灯不亮（有意如此，摇杆位置屏幕上看得见）。

### 1. VOICE MIXER

**T1.1 VOL** ✅
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 只留 1。
- 步骤：把 DRONE 1 的灰色 **VOL** 拖到最左，再拖回去。
- 期望：拖到最左时没声，拖回来恢复，过程中没有爆音。
- 备注：所有 drone 一起响时，单独调一路听不出来，要用 solo 的办法。

**T1.2 PAN** ✅
- 准备：同 T1.1。
- 步骤：把 DRONE 1 的黑色 **PAN** 拖到最左，再拖到最右，最后放回中间。
- 期望：声音分别在左耳、右耳、两耳之间。

### 2. 经典 drone（以 DRONE 1 为例，2 / 4 / 5 相同）

准备（本节通用）：RESET PANEL；BLEND 最左；DRONE VOICES 只留 1。

**T2.1 MUTE** ✅
- 步骤：
  1. 依次点亮第 1–5 列的 **MUTE**。
  2. 再逐个取消。
- 期望：每点亮一个，声音变薄一层；5 个全亮时完全没声；取消后一层层恢复。

**T2.2 TUNE 和拍频** ✅
- 步骤：
  1. MUTE 2–5 点亮，只留第 1 列。
  2. 上下拖第 1 列的 **TUNE**。
  3. 取消 MUTE 2，把两列的 TUNE 调到很接近。
- 期望：
  - 音高连续升降。
  - 两个音很接近时出现"哇哇"起伏（拍频），调得越近起伏越慢。

**T2.3 VOLT** ✅
- 步骤：把 **VOLT** 从最小慢慢拖到最大，再拖回。
- 期望：
  - 前半段 5 个振荡器一起降调。
  - 过半后声音变粗、带颗粒感。
- 备注（调音待办）：过半后听起来音量变小。

**T2.4 MOD + CV MOD 插孔 + CV ▼** ✅
- 步骤：
  1. 接线：LFO A 输出（wave 右边的插孔）→ DRONE 1 的 CV MOD 插孔（CV ▼ 正下方）。
  2. 点亮第 1 列的 **MOD**。
  3. 拖 **CV ▼**。
  4. 拖 LFO A 的 **rate**。
- 期望：
  - 第 1 个振荡器的音高有规律地上下摆动，其它振荡器不受影响。
  - CV ▼ 改变摆动幅度；rate 改变摆动快慢。
  - 不插线时 MOD 按钮没有作用，这是正常的。

**T2.5 ATT / RLS / HOLD** 🔧 (#59)
- 步骤：
  - a. **RLS** 拖到最大，关掉 DRONE VOICES 的 1：声音用几秒慢慢淡出。RLS 拖到最小再关：声音很快停。
  - b. **ATT** 拖到最大，关掉 1 再打开：声音用几秒慢慢升起。
  - c. 点亮 **HOLD**，关掉 1：声音一直响。取消 HOLD 后按 RLS 淡出。
- 期望：ATT / RLS 最长约 10 秒；HOLD 等于"这一路一直开着"。

### 3. DRONE 3 / DRONE 6（以 DRONE 3 为例）

准备（本节通用）：RESET PANEL；BLEND 最左；DRONE VOICES 只留 3；VOICE MIXER 里 DRONE 3 的 VOL 拖到一半以上；**NOISE** 拖到最小。

**T3.1 PITCH 和 hi/low** ✅
- 步骤：
  1. 从最小拖到最大 **PITCH**。
  2. 点亮 **hi/low**，再拖 PITCH。
- 期望：音高从很低到很高连续变化；hi/low 亮时整体音区变低。

**T3.2 fm / am / rate / mod** ✅
- 步骤：
  1. 点亮 **fm**。
  2. 拖 **rate**。
  3. 拖 **mod**。
  4. 关掉 fm，点亮 **am**。
  5. 两个都开。
- 期望：
  - fm：音高在高低两个音之间来回跳。
  - rate 改变跳的快慢；mod 改变两个音的音高距离，大时高音很尖。
  - am：音量一开一关，"嘟嘟嘟"。
- 备注（调音待办）：mod 最大时跨度 6 个八度，太夸张。

**T3.3 NOISE** ✅
- 步骤：
  1. 从最小拖到最大 **NOISE**。
  2. PITCH 拖到最小，mod 和 divider 拖到最大，NOISE 最大。
- 期望：沙沙噪声逐渐加大；第 2 步基本只剩风声或海浪声。

**T3.4 S&H 随机音高** 🔧 (#60)
- 步骤：
  1. fm 和 am 都关，PITCH 放中间。
  2. 接线：S&H 框最右的 OUT → **cv** 插孔（mod 下方）。
  3. 把 **rate** 调大，**divider** 调小。
- 期望：
  - NOISE 在最小时，音高也会有规律地随机跳动。
  - rate、divider 改变跳动快慢。
- 备注（调音待办）：默认速度慢，跳动幅度大。

**T3.5 LFO 1 : 10** ✅
- 步骤：点亮 fm，再点亮 **LFO 1 : 10** 下面的按钮，然后关掉。
- 期望：跳动一下快约 10 倍，变成嗡嗡的粗糙音色；关掉后恢复。

### 4. 键盘

**T4.1 触摸板和电脑键盘** ✅
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关。
- 步骤：
  1. 按住一块触摸板再松开。
  2. 从左到右按几块。
  3. 用电脑键盘 A S D F… 弹，按 Z / X。
- 期望：
  - 按住出声，松开慢慢消失。
  - 从左到右越来越高。
  - 电脑键盘按钢琴布局发声；小屏 OCT 跟着 Z / X 变化。

**T4.2 PRESSURE 输出** 🔧 (#61)
- 准备：同 T4.1。
- 步骤：
  1. 接线：键盘区右边一组中间的 ↓ 插孔（**PRESSURE**）→ FILTER 一排的 **CV L**。
  2. **MOD L** 拖到一半以上。
  3. 左边的 **FREQ** 拖到约 1/4。
  4. 按住触摸板上下移动鼠标。
- 期望：往下移声音变亮变响，往上移变闷；左耳更明显。
- 备注：FREQ 太高时滤波器已经全开，听不出变化。

### 5. VCO A / VCO B

准备（本节通用）：RESET PANEL；BLEND 最左；DRONE VOICES 全关；VOICE MIXER 只留 VCO A（测 VCO B 时只留 VCO B）；按住一个键让声音持续。

**T5.1 MORPHING WAVEFORM** 🔧 (#62)
- 步骤：把大旋钮依次对准每个图标（见面板地图里的图标位置表）。
- 期望：指向哪个图标就是那种波形，中间连续过渡。

**T5.2 tune / oct / sub** ✅
- 步骤：
  1. 拖 **tune**。
  2. 拨 **oct+3 / low**。
  3. 拨 **-1 / sub** 到 sub。
- 期望：
  - tune 让音高连续变化，约一个八度。
  - oct+3 升高很多，low 降低。
  - sub 叠加一个低八度的底音。

**T5.3 pw / pwm** ✅
- 步骤：
  1. 波形对准方波 ⊓（约 12:30），拖 **pw**。
  2. 接线：LFO A 输出 → **pwm** 插孔，把 **pwm** 旋钮调大。
- 期望：pw 让音色从空心变细薄；pwm 让音色有规律地"呼吸"。

**T5.4 cv amt 和 lin / exp** ✅
- 步骤：
  1. 接线：LFO A 输出 → **cv** 插孔。
  2. **cv amt** 先放最左，再慢慢右转。
  3. cv amt 放中间，来回拨 **lin / exp**。
- 期望：
  - cv amt 最左时音高不动，越往右颤音越深。
  - lin 和 exp 的摆动方式不同：exp 均匀，lin 不对称。

**T5.5 SYNC（只有 VCO A）** 🔧 (#63)
- 步骤：
  1. VCO B 的 **cv amt** 转到最左。
  2. 接线：VCO B 的红字 **osc** → VCO A 的 **sync**。
  3. VCO A 拨到 **oct+3**，慢慢转 VCO A 的 **tune**。
  4. 拔掉线对比。
- 期望：
  - 接线时音高基本不变，音色像"哇"一样扫动、变尖。
  - 拔线后 tune 又变回改变音高。

**T5.6 VCO A → VCO B 内部连线** ✅
- 准备：只听 VCO B；VCO B 的 cv amt 在最右（默认）。
- 步骤：
  1. 转 VCO A 的 **tune**。
  2. VCO B 的 cv amt 转到最左，再转 VCO A 的 tune。
  3. VCO A 的 tune 停在偏右，把 VCO B 的 cv amt 从左转到右。
- 期望：
  - 第 1 步 B 的音色跟着变。
  - 第 2 步 B 完全不变。
  - 第 3 步 B 从干净逐渐变粗糙。
- 备注：调制偏温和，最大时约 18% 是杂成分。

**T5.7 插线断开内部连线** ✅
- 步骤：
  1. 接线：LFO A 输出 → VCO B 的 **cv**，再转 VCO A 的 tune。
  2. 拔掉线后再转。
- 期望：插线时 B 跟着 LFO 摆动、不受 VCO A 影响；拔线后恢复受 VCO A 影响。

**T5.8 VCO B 自己的控件** ✅
- 步骤：对 VCO B 重复 T5.1–T5.4（VCO B 没有 sync）。
- 期望：和 VCO A 一致。

### 6. envelope A / B

准备（本节通用）：RESET PANEL；只听 VCO A（测 B 时只听 VCO B）。

**T6.1 A D S R** ✅
- 步骤与期望：
  - **A** 转到约 3 点钟，按住键：声音慢慢升起。
  - **S** 最左，按住：声音响后按 **D** 的时间衰减到无。
  - **S** 最右，按住：声音一直保持。
  - **R** 转到约 3 点钟，按一下松开：松开后慢慢消失。

**T6.2 hold 和自激** ✅
- 步骤：
  1. 点亮 **hold**，不按键。
  2. 关掉 hold，点亮自激按钮，A 和 R 先放最左，再把 R 往右转。
- 期望：
  - hold 亮时 VCO A 一直响。
  - 自激时声音自己有节奏地反复触发，R 越大越慢。

**T6.3 env 输出、gate 输入、envelope B** ✅
- 步骤：
  - a. 只听 VCO B，VCO B 的 cv amt 放中间。接线：envelope A 的红字 **env** → VCO B 的 **cv**。按一下键。
  - b. 只听 VCO A。接线：LFO A 输出 → envelope A 的 **gate**，不按键。
  - c. 对 envelope B 重复 T6.1 和 T6.2。
- 期望：
  - a：VCO B 的音高随包络滑上去再落回。
  - b：VCO A 按 LFO 的节奏自己响，拔线后恢复成按键才响。
  - c：envelope B 和 A 一致。

### 7. FILTER 和失真

准备（本节通用）：RESET PANEL；戴耳机；按住键或开一个 drone 让声音持续。

**T7.1 FREQ / RES / BP/LP** ✅
- 步骤：
  1. 左边 **FREQ** 从中间转到最左再转回。
  2. 右边 **FREQ** 同样操作。
  3. 左边 FREQ 放中间偏左，**RES** 往右转，再转 FREQ。
  4. 点 **BP/LP** 按钮。
- 期望：
  - 左 FREQ 只让左耳变暗或变亮；右 FREQ 只影响右耳。
  - RES 出现口哨般的共鸣，转 FREQ 有"哇"的扫频。
  - BP 时声音变薄、低音减少，按的时候"咔"一声是正常的。

**T7.2 CV L / CV R / MOD L / MOD R / link** ✅
- 准备：两边 FREQ 都放在约 10 点钟。
- 步骤：
  - a. 接线：LFO A 输出 → **CV L**；**MOD L** 和 **MOD R** 放中间偏右。然后把 MOD R 转到最左。
  - b. MOD R 放回。接线：LFO B 输出 → **CV R**；LFO B 的 rate 调快。
  - c. 点亮 **link**，再关掉。
- 期望：
  - a：两边一起随 LFO A 一明一暗；MOD R 最左后右边停止变化。
  - b：左边跟 LFO A（慢），右边跟 LFO B（快）。
  - c：link 亮时右边改回跟 LFO A。

**T7.3 DIST / GAIN** 🔧 (#64)
- 准备：先把 MASTER VOLUME 调小一点。
- 步骤：
  - a. **GAIN** 放中间，**DIST** 从最左转到最右。
  - b. DIST 最右，GAIN 从最左转到最右。
  - c. DIST 回到最左，随便转 GAIN。
- 期望：
  - a：声音从干净逐渐变成失真。
  - b：从轻微发毛变成很重的压碎声，音量变化不大。
  - c：始终是干净的原声。

### 8. DUAL EFFECTOR

准备（本节通用）：RESET PANEL；按住键或开一个 drone；戴耳机。

**T8.1 BLEND / 1-2-3 / MASTER VOLUME** ✅
- 步骤：
  1. 从最左转到右 **BLEND**。
  2. 左右两个 **1-2-3** 拨杆分别拨到 1、2、3。
  3. 转 **MASTER VOLUME**。
- 期望：
  - BLEND 最左时只有原声，往右效果越来越多。
  - 拨杆换程序时左右耳分别变化，有很短的淡出淡入。
  - MASTER VOLUME 改变整体音量。

**T8.2 换卡带和 X / Y / Z** ✅
- 准备：BLEND 偏右。
- 步骤：
  1. 点卡带窗口，13 盒逐个换一遍，右键退回一盒。
  2. 挑几盒（如 TIME、VIBE、INFINITY）转 **X**、**Y**、**Z**。
- 期望：
  - 每盒声音明显不同，没有爆音、刺耳噪声或卡住。
  - X / Y / Z 都会改变效果。
- 卡带顺序：CATHEDRAL、MAGIC、TIME、VIBROTREM、FILTER、VIBE、PITCH SHIFTER、INFINITY、STRING RINGER、SYNTEX-1、DIGITAL、GENERATOR、ORCHE。

**T8.3 cv x / cv y / cv z** ✅
- 步骤：
  1. 换到 TIME，BLEND 偏右。
  2. 接线：LFO A 输出 → **cv x**。
  3. 再分别改插 **cv y**、**cv z**。
- 期望：效果的某个参数随 LFO 自动变化，三个插孔各控制不同的参数。

### 9. LFO 和 JOYSTICK

准备（本节通用）：RESET PANEL；只听 VCO A；按住键；VCO A 的 **cv amt** 放中间。

**T9.1 LFO A / LFO B** 🔧 (#79)
- 步骤：
  1. 接线：LFO A 输出 → VCO A 的 **cv**。
  2. 拖 **rate**。
  3. 拨 **x6 / x1 / x10**。
  4. 拖 **wave**。
  5. 线改接 LFO B 的输出，重复 2–4。
- 期望：
  - rate 改变摆动快慢；x6、x10 快很多倍，最快时变成颤抖的音色。
  - wave 最左（Λ 图标）是三角（平滑滑动）、最右（⊓ 图标）是方波（音高跳），和面板图标一致（#79 修正了原来左右相反的问题）。
  - LFO B 和 LFO A 一致。
- 备注：LFO 只输出正电压，所以是"单向"摆动。

**T9.2 JOYSTICK** ✅
- 步骤：
  - a. 接线：JOYSTICK 上面的 X 输出插孔 → VCO A 的 **cv**。用鼠标左右拖摇杆（键盘区左下角），然后松手。
  - b. 摇杆放中间，转 **X**（OFFSET X）。
  - c. 线改为 Y 输出（下面的插孔）→ **CV L**；MOD L 放中间偏右，左边 FREQ 放中间。上下拖摇杆，再转 **Y**（OFFSET Y）。
- 期望：
  - a：音高跟着降升，松手后停在原位。
  - b：音高整体偏移。
  - c：声音一明一暗，左右拖不影响。

### 10. 5 STEP SEQ. VOLTAGE

**T10.1 5 步音序器** 🔧 (#65)
- 准备：
  1. RESET PANEL；只听 VCO A。
  2. 接线：音序器最右上的红字 **cv** → VCO A 的 **1v/oct**。
  3. 接线：最右下的红字 **gate** → envelope A 的 **gate**。
- 步骤：
  - a. 不按键，把 **step 1–5** 转到不同位置。
  - b. 转 **pulser**。
  - c. 拨 **stages**。
  - d. 关掉某一步右边的 **gate** 拨杆。
  - e. 接线：LFO A 输出 → **clock** 下面的插孔（EXT CLOCK），再调 LFO A 的 rate。
- 期望：
  - a：自动循环弹 5 个音的旋律。旋钮很灵敏，整圈约 5 个八度。
  - b：速度变化。
  - c：变成 3 或 4 个音一循环。
  - d：那一步变成空拍。
  - e：改由 LFO 推进节奏，不用改菜单。

**T10.2 5-STEP SEQ CLOCK（菜单时钟开关）** ✅（2026-10-03 Mac 实测：不插 EXT CLOCK 线时切 ext 停走，切回 int 按 pulser 速度恢复）
- 说明：键盘菜单 PLAY 页 CLOCK 卡片的 **5-STEP SEQ CLOCK**（INT / EXT）是 5 步音序器时钟源开关唯一的 UI 入口。插线进 EXT CLOCK 插孔时线缆永远接管（T10.1 e 已测），这个开关只在**没插线**时起作用。
- 准备：按 T10.1 接好 cv / gate（不接 EXT CLOCK 的线），音序器在走。
- 步骤：菜单里把 **5-STEP SEQ CLOCK** 切到 **EXT**，再切回 **INT**。
- 期望：切到 ext 后音序器停走（没有外部时钟）；切回 int 后按 pulser 速度恢复。

### 11. PREAMP 和 ENVELOPE FOLLOWER

准备（本节通用）：Preferences 里把输入设备选成 MacBook 自带麦克风（不要用 AirPods 麦克风），（它是单声道，Input 1 (L) / Input 2 (R) 显示为空是正常的，麦克风照样是开的）；戴耳机，防止扬声器的声音被麦克风收回去；RESET PANEL。RESET 后 PREAMP 的红色 **gain** 和 VOICE MIXER 的 **EXT.AUDIO** 音量都在最左（静音），T11.2、T11.3 先把 gain 转到中间。

**T11.1 PREAMP 发声** 🔧 (#72)
- 步骤：
  1. RESET PANEL。在 VOICE MIXER 里把 **PREAMP**（第 7 路）以外的 9 路 VOL 都拖到最左；PREAMP 自己的 VOL 不动（默认在中间）。
  2. 对着麦克风说话或拍手（gain 还在最左）。
  3. 把 PREAMP 区的红色 **gain** 慢慢往右转，边转边拍手。
- 期望：第 2 步没声；转 gain 后听到经过滤波器和效果器处理的麦克风声音，gain 越大越响；声音很大时 PREAMP 的蓝灯（削波）闪。

**T11.2 env 输出** 🔧 (#72)
- 步骤：
  1. RESET PANEL（6 个 drone 在响）。PREAMP 的 **gain** 转到中间；VOICE MIXER 的 **PREAMP** VOL 拖到最左（只用麦克风去控制，不听麦克风本身）。
  2. 接线：ENVELOPE FOLLOWER 的红字 **env** → FILTER 一排的 **CV L**。
  3. **MOD L** 转到中间偏右；左边滤波器的 **FREQ** 转到偏左（drone 声音变闷）。
  4. 对着麦克风说话或连续拍手。
  5. 分别把 **attack**、**release** 转到最左和最右，再说话对比。
- 期望：说话时 drone 声音变亮（滤波器打开），停下后变回闷；ENVELOPE FOLLOWER 的红灯跟着亮。attack 越大变亮越慢，release 越大变回闷越慢。

**T11.3 gate 输出** 🔧 (#72)
- 步骤：
  1. RESET PANEL。PREAMP 的 **gain** 转到中间；VOICE MIXER 只留 **VCO A**（第 5 路），其它 9 路 VOL 拖到最左。
  2. 接线：ENVELOPE FOLLOWER 的红字 **gate** → envelope A 的 **gate** 插孔。
  3. 先不出声，看 ENVELOPE FOLLOWER 的绿灯（gate）：应该是灭的。如果一直亮（房间太吵），把 **gain** 往左转到刚好熄灭。
  4. 不按键，拍一下手；再连续拍几下。
- 期望：每拍一下 VCO A 响一下，绿灯同时亮。不拍手时安静。
- 提示：gain 就是 gate 的灵敏度。用耳机，否则扬声器里的 VCO A 被麦克风收进去，会让 gate 一直开着。

**T11.4 麦克风默认不出声** 🔧 (#72)
- 步骤：
  1. 输入设备仍选麦克风；RESET PANEL。
  2. 在 DRONE VOICES 里把 6 个键全关掉，等混响尾巴消失（十几秒）。
  3. 拍一下手。
- 期望：完全安静，拍手也听不到回声。电脑输入的两个声道分别进 PREAMP 和 EXT.AUDIO；之前两者默认都开着，会把麦克风收到的环境声送出来。
- 补充：
  - 把 VOICE MIXER 的 **EXT.AUDIO** 音量转到中间再拍手，能听到带混响的拍手声（这一路直接接电脑输入）。
  - EXT.AUDIO 转回最左，把 PREAMP 的 **gain** 转到中间再拍手，也能听到（这一路经过放大，ENVELOPE FOLLOWER 的红灯跟着亮）。MacBook 自带麦克风是单声道，它同时送进这两路。

### 12. 键盘菜单（演奏设置）

准备（本节通用）：RESET PANEL；只听 VCO A；多个音同时按住时用电脑键盘。

**T12.1 菜单显示** 🔧 (#66)
- 期望：每个设置都有完整的名字；旋钮下显示数值，选项显示完整名称（如 ARPEGGIATOR）。

**T12.2 GLIDE / VIB** ✅
- 步骤：
  1. EXPRESSION 页 **GLIDE** 调到中间，弹两个相隔较远的音。
  2. VIBRATO 的 **DEPTH** 调大，**RATE** 放中间，按住一个音；再把 **DELAY** 调大。
- 期望：
  - GLIDE：音高从前一个音滑到后一个，GLIDE 越大越慢。
  - VIBRATO：音高自动颤动；DELAY 大时先平稳一会儿再颤。

**T12.3 ARPEGGIATOR** 🔧 (#67, #68)
- 步骤：
  1. **MODE** 切到 ARPEGGIATOR。
  2. 同时按住 2–3 个键，然后全部松开。
  3. 调 PLAY 页 **TEMPO**、ARP 页 **DIRECTION**。
  4. 打开 ARP 页 **HOLD** 后松键，再关掉。
  5. MODE 切回 KEYBOARD。
- 期望：
  - 按住时按节奏循环弹这几个音，**松开马上停**。
  - TEMPO 改变速度；DIRECTION 改变顺序。
  - HOLD 时松键继续播放。
  - 切回 KEYBOARD 时声音停下，不卡住。

**T12.4 SCALE / ROOT** ✅（PR-B，2026-10-03 Mac 实测 1–9 步全部通过；第 8 步按补上接线的新写法复测通过）
- 界面位置：点键盘区中间的红色大旋钮打开 KEYBOARD MENU，顶部第一个页签 **PLAY**。PLAY 页有三张卡片：
  - 左边 **KEYBOARD** 卡：上面一行 **PLAY**（SINGLE / TWIN / SPLIT），下面一行 **MODE**（KEYBOARD / ARPEGGIATOR / SEQUENCER）。
  - 中间 **QUANTISER** 卡：上面是 **SCALE**，一条 `<  音阶名  >`，右上角小字 `n / 19`；点 `>` 下一个、`<` 上一个。下面是 **ROOT**，一个只有一个八度的小钢琴（C 到 B），点哪个琴键就选哪个，选中的变蓝，右上角显示字母。
  - 右边 CLOCK 卡（本项不用）。
  - SCALE 的顺序：1 SEMITONES、2 IONIAN（大调）、3 DORIAN、4 PHRYGIAN、5 LYDIAN、6 MIXOLYDIAN、7 AEOLIAN（小调）、8 LOCRIAN、9 BLUES-MAJOR、10 BLUES-MINOR、11 PENTATONIC-MAJOR、12 PENTATONIC-MINOR、13 FOLK、14 JAPANESE、15 GAMELAN、16 GYPSY、17 ARABIAN、18 FLAMENCO、19 WHOLE-TONE。
- 准备：
  1. 菜单右下角 **RESET PANEL** 点两下（4 秒内），菜单会关掉。
  2. 键盘区右边 **DRONE VOICES** 的 6 个键全部点灭（LED 灭），只留键盘的声音。
  3. 再点红色大旋钮打开菜单，确认在 PLAY 页，PLAY = **SINGLE**、MODE = **KEYBOARD**（都是蓝色）。
  4. 菜单开着就能用电脑键盘弹，菜单盖住了金属触摸板也没关系。按键没声音时，先在菜单空白处点一下再弹。
  5. 电脑键盘一个八度：A = C，W = C#，S = D，E = D#，D = E，F = F，T = F#，G = G，Y = G#，H = A，U = A#，J = B，K = 高八度 C。下面「依次弹」都是指按顺序弹 A W S E D F T G Y H U J K 这 13 个键，一次一个。
- 步骤与期望：
  1. SCALE 显示 **SEMITONES**（1 / 19）。依次弹：13 个键每个音都不一样，一个半音一个半音往上走。
  2. SCALE 点 `>` 一次，显示 **IONIAN**（2 / 19）；ROOT 点最左边的白键 **C**。依次弹：只有 do re mi fa sol la si do 这 8 个音。每个黑键（W、E、T、Y、U）和它左边的白键同音，听起来是"同一个音按两次"。
  3. ROOT 点白键 **A**（右数第二个白键）。依次弹：变成 A 大调；W（C#）、Y（G#）现在是自己的音了；S（D）和 E（D#）同音。A（C）被吸到它下面的 B，和 J（B）是同一个音名，但低一个八度。
  4. SCALE 点 `>` 5 次到 **AEOLIAN**（7 / 19），ROOT 保持 A。依次弹：结果和第 2 步**完全一样**（A 小调和 C 大调用的是同一组音），说明 SCALE 和 ROOT 一起决定音。
  5. SCALE 继续点 `>` 到 **BLUES-MAJOR**（9 / 19）：名字后面显示 **(NOT MODELLED)**；依次弹，和第 1 步一样 13 个音都不同（手册没给这些音阶的音，所以原样通过）。FOLK、JAPANESE、GAMELAN 等也一样。
  6. SCALE 点 `<` 回到 **IONIAN**（2 / 19），ROOT 点 **C**。MODE 点 **ARPEGGIATOR**，同时按住 A、W、S 三个键：琶音反复出的只有 C 和 D 两个音高（C# 被吸到 C）。松开，MODE 点回 **KEYBOARD**。
  7. 按 3 次 Z（屏幕 OCT 显示 -3），依次弹：仍是 do re mi……，没有一串键卡在同一个音上；按 6 次 X 到 OCT +3 再弹一遍，同样。按 3 次 Z 回到 0。有 MIDI 键盘的话，弹最低和最高的几个键，也都在音阶上。
  8. 左右分开：SPLIT 时右半边不再驱动 VCO A，要先接线让它出声（同 T12.7）：
     - 关掉菜单。VOICE MIXER 只留 **VCO A** 和 **VCO B**（其它 VOL 拖到最左）。
     - 接线：键盘区右边一组的 ⊓ **GATE R**（第 1 个）→ envelope B 的 **gate**；右边一组的 ↓ **PRESSURE**（第 2 个）→ VCO B 的 **1v/oct**（SPLIT 时 PRESSURE 插孔输出右半边的音高）。
     - 打开菜单，PLAY 点 **SPLIT**。标题栏右边（CLOSE 左边）出现 **EDITING：LEFT C-F | RIGHT F#-B**。
     - 点 **LEFT C-F**，SCALE 点 `<` 到 **SEMITONES**。
     - 点 **RIGHT F#-B**，SCALE 一直点 `>` 到最后一个 **WHOLE-TONE**（19 / 19），ROOT 点 **C**。QUANTISER 卡右上角显示蓝色小标签 RIGHT。
     - 弹 A W S E D F（左半边 C 到 F）：6 个音都不同（左边不受影响）。
     - 弹 T G Y H U J（右半边 F# 到 B）：两两同音，T=G、Y=H、U=J，只有 3 个不同的音（全音音阶 F# G# A#）。
     - 弹完把 PLAY 点回 **SINGLE**，拔掉这两根线。
  9. SCALE 设成 IONIAN、ROOT 设成 D，⌘Q 退出，再打开程序：打开菜单 PLAY 页仍是 IONIAN / D，依次弹仍是 D 大调：W（C#）和 T（F#）是自己的音；A（C）被吸到它下面的 B，比 J（B）低一个八度。

**T12.5 16 步音序器（SEQ STEPS 页）** 🔧 (#68)
- 步骤：
  1. 点 **SEQ STEPS** 页签：把几步的推子拖到不同高度，关掉一两步的 GATE 按钮。
  2. PLAY 页 **MODE** 切到 SEQUENCER（SEQ 页 **RUN** = FREE）。
  3. 按住一个键，换另一个键。
  4. 调 SEQ 页 **LENGTH**、**DIRECTION**。
  5. RUN 切到另一个选项，按住键再松开。
  6. MODE 切回 KEYBOARD。
- 期望：
  - 第 1 步：16 列，每列有步号、推子（显示半音数）、GATE 按钮；超过 LENGTH 的步变暗但仍可编辑。
  - 第 2 步：不按键也自动循环播放，关掉 gate 的步是空拍。
  - 第 3 步：整段旋律以按住的键为起点移调。
  - 第 4 步：长度和方向变化（BACKWARD / PING-PONG / RANDOM）。
  - 第 5 步：只有按住键时才播放，松开就停。
  - 第 6 步：声音停下，不卡住。

**T12.6 PLAY = TWIN（两个 6 键的键盘）** 🔧 (#75)
- 准备：RESET PANEL；DRONE VOICES 6 个键全关；VOICE MIXER 只留 **VCO A** 和 **VCO B**。
- 电脑键盘上，左半边 6 个键是 **A W S E D F**（C 到 F），右半边是 **T G Y H U J**（F# 到 B）。
- 步骤：
  1. 打开键盘菜单，**PLAY** 选 TWIN，关掉菜单。
  2. 接线：键盘区右边一组的 ⊓ **GATE R**（第 1 个）→ envelope B 的 **gate**；右边一组的 ↓ **PRESSURE**（第 2 个）→ VCO B 的 **1v/oct**。
  3. 分辨左右：把 **VCO A** 的 VOL 拖到最左，弹左右两半；再把 VCO A 拖回、**VCO B** 拖到最左，再弹一次。
  4. 两边同时：两个 VOL 都拖回中间，VCO B 的 **oct+3** 拨杆拨到上面（VCO B 高 3 个八度）。按住 A，再按住 H，然后先松开 A；再反过来先按 H、后按 A、先松开 H。
  5. PLAY 改回 SINGLE，重复第 3 步。
- 期望：
  - 第 3 步：只开 VCO B 时，只有右半边响；只开 VCO A 时，只有左半边响。
  - 第 4 步：低音（左）和高音（右）同时响；松开一边，另一边继续响。
  - 第 5 步：只开 VCO B 时两半都不响，只开 VCO A 时两半都响（12 个键是一个键盘）；同时按住两个键时只响后按的那个音。
- 测完把 oct+3 拨回 low。

**T12.7 PLAY = SPLIT（左右各一套设置）** 🔧 (#75)
- 准备：同 T12.6（RESET PANEL；drone 全关；VOICE MIXER 只留 VCO A、VCO B；GATE R → envelope B 的 gate，PRESSURE → VCO B 的 1v/oct）；VCO B 的 **oct+3** 拨到上面，右边是高音。
- 步骤：
  1. 打开键盘菜单，**PLAY** 选 SPLIT。标题行（CLOSE 左边）出现 **EDITING LEFT C-F / RIGHT F#-B**。
  2. 它决定菜单里的设置改的是哪一半键盘：LEFT C-F 亮时改左半边，RIGHT F#-B 亮时改右半边。目标是左半边普通弹奏、右半边琶音：
     - 左半边不用改（**MODE** 默认就是 KEYBOARD）；
     - 点 **RIGHT F#-B**；
     - 把 **MODE** 改成 ARPEGGIATOR（只改了右半边）；
     - 点 **CLOSE** 关掉菜单。
  3. 按住左半边的 **A**；再同时按住右半边的 **H** 和 **J**。
  4. 重新打开菜单，在 LEFT C-F / RIGHT F#-B 之间切换，看 MODE 显示。
  5. **PLAY** 改成 TWIN。
- 期望：
  - 第 3 步：左边是一个持续的低音（普通键盘）；右边两个高音按节奏轮流响（琶音）。
  - 第 4 步：LEFT 时 MODE 显示 KEYBOARD，RIGHT 时显示 ARPEGGIATOR。
  - 第 5 步：EDITING 切换消失；左右两边都按左边的设置弹（都是普通键盘，右边不再琶音）。
- 测完把 oct+3 拨回 low。

**T12.7a SPLIT 左右设置隔离（队列拒绝修复后）** ✅ (#82；Mac 实测通过)
- 键盘菜单 PLAY 选 SPLIT，EDITING 选 LEFT，MODE 设 KEYBOARD；选 RIGHT，MODE 改 ARPEGGIATOR，再改 SEQUENCER。
- 来回切换 EDITING：预期 LEFT 一直为 KEYBOARD，RIGHT 为 SEQUENCER。将 RIGHT 的 MODE 改回 KEYBOARD；此时修改共享的 PLAY 为 SINGLE，预期仍能正常切换。
- 队列满的拒绝路径由自动测试覆盖，不需要人工高速拖旋钮。此项只检查实际 UI 的左右路由和共享设置没有回归。

**T12.8 PRESETS（键盘预设 A–D）** 🔧 (#76)
- 说明：预设保存键盘菜单的全部设置（左右两边、两套 16 步音序），不含速度 CLOCK。按钮在菜单底部：**PRESET** A B C D（直接点选）、**LOAD**（载入）、**SAVE**（保存）、**INIT**（清回出厂设置）。
- 步骤：
  1. 打开键盘菜单，点 PRESET 的 **B**。把 **MODE** 改成 ARPEGGIATOR，点 **SAVE**。
  2. 把 **MODE** 改回 KEYBOARD，关掉菜单，同时按住 **A** 和 **S**。
  3. 打开菜单（仍是 B），点 **LOAD**，关掉菜单，再同时按住 **A** 和 **S**。
  4. 退出程序再打开，打开菜单，点 **B**，点 **LOAD**。
  5. 点 **INIT**，再点一次；然后点 **LOAD**。
- 期望：
  - 第 1 步：SAVE 按钮变亮，短暂显示 SAVED。
  - 第 2 步：两个音一起持续响（普通键盘）。
  - 第 3 步：LOAD 短暂显示 LOADED，菜单里 MODE 变回 ARPEGGIATOR；两个音轮流响。载入时声音不中断。
  - 第 4 步：MODE 仍是 ARPEGGIATOR（预设随程序一起保存）。
  - 第 5 步：第一次点 INIT 显示 SURE?，第二次显示 CLEARED；再 LOAD 后 MODE 回到 KEYBOARD。
- 测完点 **RESET PANEL**（会清掉所有预设）。

**T12.9 PRESSURE OUTPUT / RISE / FALL（压力输出）** 🔧 (#77)
- 准备：同 T4.2（PRESSURE → FILTER 的 **CV L**，**MOD L** 一半以上，左边 **FREQ** 约 1/4）。用鼠标按触摸板：按得越靠下压力越大，按住上下拖动压力跟着变。
- 步骤（EXPRESSION 页 PRESSURE OUTPUT 保持 **PRESSURE**）：
  1. **RISE** 拉满，**FALL** 最小。在触摸板最上面按住，快速拖到最下面**停住 3 秒**，再快速拖回最上面。
  2. **RISE** 最小，**FALL** 拉满。在最上面按住，快速拖到最下面，再快速拖回最上面**停住 3 秒**。
  3. **RISE** 放到一半，**FALL** 最小，像平时一样按住上下来回扫。
  4. 换成听音高（比听亮度容易分辨）：拔掉 PRESSURE → CV L 的线，改接 PRESSURE → VCO A 的 **cv**；VCO A 的 **cv amt** 放约 1/4；VOICE MIXER 里 **VCO B** 拉到最左。**RISE**、**FALL** 都放到约 3/4。PRESSURE OUTPUT 依次换成 ASR、AD、LOOP、RANDOM，每种都在触摸板偏下的位置按住 3 秒左右再松开；RANDOM 多按几次。
- 期望：
  - 第 1 步：拖到底后声音慢慢变亮，约 2–3 秒才到最亮；拖回去时马上变闷。
  - 第 2 步：拖到底时马上变亮；拖回去后慢慢变闷，约 2–3 秒。
  - 第 3 步：扫动的效果和 RISE 最小时差不多，只是变亮的一瞬间稍微圆滑一点（RISE 旋钮前半段是很短的时间，后半段才是明显的慢变化）。
  - ASR：按下后音高滑上去并停住，按住时上下拖动音高不变；松开后滑下来。
  - AD：按下后音高滑上去，不等松手就自己滑回原来的音高（约 1 秒内）。
  - LOOP：按住期间音高上下来回滑，像警笛。
  - RANDOM：每按一次音高都不一样，按住期间不变。

**T12.9a PRESSURE 设置随整机恢复** ✅ (#80；Mac 实测 AD / LOOP 重启恢复及 BlackHole 2CH 设备选择保存)
- 目标：切换压力输出模式后，退出重开和切换音频设备都能保留面板设置、接线。
- 第一次打开修复版时先不要 RESET：如果旧版曾在改 PRESSURE 后无法恢复设置，先检查旧设置、接线是否回来。此修复可读取仅压力模式镜像不一致的旧文件；其他损坏文件不会自动覆盖。
- 准备：记录当前面板后再 RESET PANEL；**BLEND** 拉到最左。接线：键盘区右侧一组中间的 ↓ **PRESSURE** 输出 → VCO A 最下排左数第 2 个 **cv** 输入；VCO A 的 **cv amt** 放约 1/4。
- 步骤：
  1. 红色大旋钮打开键盘菜单，EXPRESSION 页 PRESSURE OUTPUT 选 **AD**，**RISE**、**FALL** 都放约 3/4。截图记下菜单和接线。不用点 PRESET 的 SAVE（它只保存键盘预设）。
  2. 正常退出应用（⌘Q），重新打开同一个测试包，查看键盘菜单和面板。
  3. PRESSURE OUTPUT 改成 **LOOP**，再次正常退出并重开。
  4. 在 Preferences 中把输出切到另一个可用设备，确认出声后切回原设备，再看设置；只有一个可用输出设备时可跳过这步并说明。
- 期望：第 2 步仍为 AD，第 3、4 步仍为 LOOP；RISE / FALL、BLEND、cv amt 及这根线都保留，没有整个面板退回出厂值。压力包络的听感仍按 T12.9 描述。
- 反馈：使用的测试包/提交、哪一步异常；异常前后截图，以及是否做过 RESET。Mac 上先测一台即可；自动测试另覆盖五种压力模式和 SINGLE / TWIN / SPLIT。

**T12.10 VIBRATO PRESSURE（压力控制颤音）** 🔧 (#77)
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关；不接线。EXPRESSION 页 VIBRATO 的 **DEPTH** 约 1/4，**RATE** 放中间。
- 步骤：
  1. VIBRATO 的 **PRESSURE** 最小。在触摸板最上面按住，慢慢拖到最下面。
  2. **PRESSURE** 拉满，重复第 1 步。
- 期望：
  - 第 1 步：颤音深浅不变。
  - 第 2 步：在最上面（轻按）几乎不颤；越往下颤得越深，最下面约是第 1 步的两倍。
  - 补充：VIBRATO **PRESSURE** 放一半时，轻按也有一半深度的颤音，压力的影响减半。

**T12.11 LEGATO** ✅
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关。键盘菜单 EXPRESSION 页 **GLIDE** 放中间。
- 步骤：
  1. **LEGATO** 保持 OFF。用电脑键盘一个一个地弹 **A**、**K**（每次松开再按下一个）。
  2. **LEGATO** 改成 ON，重复第 1 步。
  3. 仍是 ON：按住 **A** 不放，再按 **K**，再松开 K。
- 期望：
  - 第 1 步：每个新音都从上一个音滑过去。
  - 第 2 步：分开弹时不滑，直接跳到新音。
  - 第 3 步：按住 A 再按 K 时滑上去；松开 K 时滑回 A。

**T12.12 VARIATION / INTERVAL（ARP 页）** ✅
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关。键盘菜单 **MODE** 改成 ARPEGGIATOR。
- 步骤：同时按住 **A** 和 **D**（C 和 E），一直按着，依次改：
  1. **VARIATION** OFF。
  2. **VARIATION** x1，**INTERVAL** 用 > 调到头（显示 12 semitones，> 变灰）。
  3. **VARIATION** x2。
  4. **VARIATION** x1，**INTERVAL** 调到 7 semitones。
- 期望：
  - 第 1 步：C、E 来回。
  - 第 2 步：C、E，然后高八度的 C、E，再从头。
  - 第 3 步：再多一轮，高两个八度。
  - 第 4 步：C、E，然后高 7 个半音的 G、B。

**T12.13 CV OUTPUT（SEQ 页，休止步的音高）** 🔧 (#78)
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关。envelope A 的 **R** 拉到 3/4 左右（让每个音的尾巴长一点）。键盘菜单 **MODE** 改成 SEQUENCER，SEQ 页 **LENGTH** 调到 2 steps。SEQ STEPS 页：第 1 步音高 0、GATE 亮；第 2 步音高拉到 +12、GATE 点灭。
- 步骤：按住 **A** 听几轮；**CV OUTPUT** 分别选 CONTINUOUS 和 GATED。
- 期望：
  - CONTINUOUS：听起来是低、高、低、高来回（第 2 步不重新起音，但正在消失的尾音跳高一个八度）。
  - GATED：一直是同一个音高（第 2 步尾音保持原来的音高，只是慢慢消失）。

**T12.14 RHYTHM（节奏型）** 🔧 (#78)
- 说明：节奏型是一排最多 8 个"拍"，夹在时钟和琶音器 / 音序器之间，每一拍可以设成"响"或"不响"。ARP 页的 **ARP RHYTHM** 卡片管琶音器，SEQ 页的 **SEQ RHYTHM** 卡片管音序器。每个按钮：
  - **红灯亮** = 这一拍响；
  - **灯灭、按钮凹下** = 这一拍不响（点一下切换）；
  - **变淡** = 超出长度，不起作用。
  - 长度用卡片右上角的 **LENGTH**（< >）调。默认长度 1，所以一开始只有第 1 个亮。
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关。键盘菜单：**MODE** 改成 ARPEGGIATOR，ARP 页 **HOLD** 改成 ON。同时按一下 **A**、**D**、**G** 再松开（HOLD 让琶音一直走，不用一直按着）。
- 步骤：
  1. 听一会儿。
  2. ARP RHYTHM 的 **LENGTH** 调到 4 steps。
  3. 点一下第 2 个按钮。
  4. 再点一下第 4 个按钮。
  5. **MODE** 改成 SEQUENCER（不用按键，音序器自己走）。SEQ 页 SEQUENCER 卡片的 **LENGTH** 调到 3 steps；SEQ STEPS 页把第 2 步音高拉到 +7、第 3 步拉到 +12（三步音高不同才听得出每一步）。先听一会儿；再把 SEQ RHYTHM 的 **LENGTH** 调到 3 steps，点一下第 3 个按钮。
- 期望：
  - 第 1 步：C、E、G 均匀轮流。
  - 第 2 步：LENGTH 显示 4 steps；第 1–4 个亮，5–8 变淡。声音不变。
  - 第 3 步：第 2 个灭。节奏变成"响、停、响、响"循环，音的顺序仍是 C、E、G 依次（停的那拍不跳音）。
  - 第 4 步：第 4 个也灭。变成"响、停、响、停"，琶音只剩一半速度。
  - 第 5 步：改节奏前是低、中、高三个音均匀循环；点灭 SEQ RHYTHM 第 3 个后，每响两个音停一拍，旋律照样按顺序往下走：低、中、（停）、高、低、（停）、中、高、（停）……停顿的位置在三个音之间轮换。
- 补充：PLAY = SPLIT 时，节奏按钮同样跟着 **EDITING LEFT / RIGHT** 切换左右两边。

**T12.15 CLOCK 输入（外部时钟）** ✅
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关。键盘菜单 **MODE** 改成 ARPEGGIATOR，ARP 页 **HOLD** 改成 ON；同时按一下 **A**、**D**、**G** 再松开。
- 步骤：
  1. 接线：5 STEP SEQ. VOLTAGE 的 **clock** 上面那个插孔（CLOCK OUT）→ 键盘区 ◷ CLOCK 输入。来回转 **pulser**。
  2. 拔掉这根线。
  3. 键盘菜单 PLAY 页随便动一下 **TEMPO**（或点一下 < >）。
- 期望：
  - 第 1 步：琶音跟着 pulser 的速度走，pulser 转快琶音就快。
  - 第 2 步：琶音停住（外部时钟没了）。
  - 第 3 步：琶音恢复，按 TEMPO 的速度走（改 TEMPO 就切回内部时钟，说明书 p.19）。

**T12.16 RESET 输入** 🔧 (#79)
- 准备：同 T12.15，但和弦改成同时按一下 **A**、**D**、**G**、**J**（C、E、G、B 四个音）。
- 步骤：
  1. 接线：LFO B 输出 → 键盘区 ⚡ RESET 输入。LFO B 的 **wave** 拖到最右（⊓ 方波），拨杆 **x1**，**rate** 从最慢慢慢往上加。
  2. 拔掉线。
- 期望：
  - 第 1 步：每当 LFO 跳一下，琶音就回到第一个音 C 重新开始（rate 慢时听起来是偶尔"打个嗝"重来）；rate 加快后只听到 C、E（或 C、E、G）反复，到不了 B。琶音一直在走，不会停。
  - 第 2 步：恢复 C、E、G、B 完整循环。

**T12.17 GATE L / V/OCT 输出（带 drone 的 GATE 输入）** 🔧 (#79)
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 只留 **1**；VOICE MIXER 里 **VCO A**、**VCO B** 的 VOL 拖到最左（只听 drone 1）。
- 步骤：
  1. 接线：键盘区 ⊓ GATE L → DRONE 1 下排最左的 GATE 输入。不按键听一会儿；再按住任意键、松开。
  2. 拔掉线。
  3. DRONE VOICES 的 1 关掉、只开 **3**（VCO A / B 仍在最左）；DRONE 3 的 **mod** 和 **NOISE** 拖到最小。接线：键盘区 ↳ V/OCT → DRONE 3 的 **cv** 输入（mod 下方）。按住一个键不放，换几个高低不同的键（Z / X 换八度）。
- 期望：
  - 第 1 步：不按键时 drone 1 不响；按住键时响，松开后按 RLS 淡出。
  - 第 2 步：drone 1 恢复一直响（没接线时由 DRONE VOICES 的键决定）。
  - 第 3 步：drone 3 的音高跟着键走，按高的键它就变高（V/OCT 每伏一个八度）。松开键时音高保持在最后一个音。
- 补充：
  - DRONE 2–6 的 GATE 输入用同样方法各试一下（3 和 6 的 GATE 在最下排最左）。
  - 第 3 步对 DRONE 6 同样适用：只开 DRONE VOICES 的 **6**，线改接到 DRONE 6 的 **cv** 输入。

**T12.18 新版键盘菜单（Mac 复测）** ⏳
- 准备：RESET PANEL；红色大旋钮打开键盘菜单。
- 步骤与期望：
  1. 6 个页签逐个点开：每个设置都能改，名字完整不截断；SERVICE 页顶部有警告条。
  2. < > 到头变灰，再点不变（SCALE 1/19–19/19、INTERVAL、LENGTH）；ROOT 点哪个琴键就选哪个；TEMPO 下方 < > 每次 ±1 BPM。
  3. 旋钮：上下拖动、Shift 细调、滚轮、双击回默认值都和以前一样；快速连点选项、< >、节奏按钮、GATE 按钮时每一下都生效（不会被当成双击吞掉）。
  4. SEQ STEPS：拖推子改音高，双击推子回 0；超过 LENGTH 的步变暗、仍可编辑。
  5. PLAY = SPLIT：EDITING 切到 RIGHT 后只改右半边（参照 T12.7）；共用设置显示 GLOBAL。
  6. PRESET 直接点 A–D；LOAD / SAVE / INIT 按 T12.8 检查（INIT 两次确认，LOAD 不确认）。
  7. RESET PANEL 点两下（4 秒内）：面板复位，菜单关掉；再打开是 PLAY 页。
  8. 换到 ARP 页后按 Esc 关闭，再打开仍是 ARP 页；鼠标停在某个按钮上时按 Esc，再打开不残留高亮。菜单打开时点 MIDI 按钮会关掉菜单（反之亦然）；菜单区域内的点击不会传到下面的面板。
  9. 重启程序后菜单里的设置都在；拉伸窗口后菜单比例正常、点击位置准确；MIDI LEARN 学一个面板旋钮仍正常（菜单设置不能学）。

**T12.19 菜单盖住接线与插孔；ENCODER DIRECTION** ⏳ (PR-A)
- 准备：RESET PANEL。接两根穿过键盘区上方的线：LFO A 的输出 → 键盘区 **CLOCK** 输入；键盘区 **V/OCT** 输出 → DRONE 3 的 **cv** 输入。
- 步骤与期望：
  1. 红色大旋钮打开键盘菜单：两根线都被菜单**盖住**（线在菜单下面，菜单完整不被线遮挡）；关掉菜单后线照常显示在面板上。
  2. 菜单打开时，从菜单外的一个输出（例如 LFO B 输出）拖线，在菜单上方（键盘区 CLOCK / RESET / GATE / V/OCT 插孔所在位置）松手：**不接上任何线**。关掉菜单再拖到同一插孔，正常接上。
  3. 点 MIDI 按钮打开 MIDI 设置，重复第 2 步：同样不接线。
  4. 菜单打开时，鼠标悬停在菜单外的旋钮上，数值气泡照常显示。
  5. 滚轮换八度：「红色大旋钮」就是键盘区中间点一下打开键盘菜单的那个红色旋钮。「滚轮」= 鼠标滚轮；MacBook 触控板上是**两指同时上下滑动**（不要按下）。
     - 菜单关着，把鼠标指针停在红色大旋钮上（不点击），两指往一个方向滑一下：蓝色小屏的 OCT 变 1 格（例如 0 → 1），手指离开后**停在新的数字上**，不会继续跳或跳回 -3。滑得快、滑得长也只变 1 格；停半秒再滑一次再变 1 格。左右滑不改变 OCT。记下这个方向是升还是降（macOS「自然滚动」设置会让方向因人而异，以第一次看到的为准）。
     - 点红色大旋钮打开菜单 → SERVICE 页 → ENCODER 的 DIRECTION 选 **REVERSED** → CLOSE。
     - 指针停回红色大旋钮，用**同一个方向**再滑：这次 OCT 往反方向变。
     - DIRECTION 选回 NORMAL：方向恢复成第一次的样子。改成 REVERSED 后退出重开程序，SERVICE 页里仍是 REVERSED。

### 13. 其余插孔

准备（本节通用）：RESET PANEL；BLEND 最左；DRONE VOICES 全关；点亮 envelope A 的 **hold**（VCO A 不按键也一直响，当作"监听器"）；VOICE MIXER 里 **VCO B** 拖到最左。

**T13.1 drone 的 env 输出** 🔧 (#79)
- 步骤：
  1. VOICE MIXER 里 **DRONE 1** 的 VOL 拖到最左（只用它的包络，不听它的声音）。DRONE 1 的 **ATT**、**RLS** 都拖到一半以上。
  2. 接线：DRONE 1 的红字 **env** → VCO A 的 **cv**；VCO A 的 **cv amt** 放约 1/4。
  3. 点亮键盘区右边 **DRONE VOICES** 的按键 **1**（不是 VOICE MIXER 里的音量），等几秒，再点一下关掉。
  4. 对 DRONE 2 / 4 / 5（红字 **env**）和 DRONE 3 / 6（红字 **env out**）各做一遍。
- 期望：打开时 VCO A 的音高按 ATT 慢慢滑上去，关掉时按 RLS 慢慢滑回来。

**T13.2 DRONE 3 / 6 的 LFO 输出** ✅
- 步骤：接线：DRONE 3 **rate** 下方的 LFO 输出 → VCO A 的 **1v/oct**。拖 DRONE 3 的 **rate**；再拨 **LFO 1 : 10**。DRONE 6 同样做一遍。
- 期望：VCO A 在两个音之间来回跳（方波）；rate 改变跳的快慢，1 : 10 时慢很多。DRONE 3 / 6 本身不用开（LFO 一直在走）。

**T13.3 S&H 的 IN 和 clock 插孔** ✅
- 插孔位置：DRONE 3 右下角的 **S&H** 框里有 3 个插孔，从左到右：**IN**（▲）、**clock**（▲ clock）、**OUT**（右上角带红三角）。LFO A 在面板最下排最左：红色 **wave** 旋钮、中间的输出插孔（下面有蓝灯）、拨杆 **x6 / x1 / x10**、红色 **rate**。LFO B 在最下排最右，布局相同。
- 准备：
  1. RESET PANEL；BLEND 最左；DRONE VOICES 全关。
  2. 点亮 envelope A 的 **hold**（envelope A 左上角的黑按钮），VCO A 一直响，用它来听 S&H。
  3. 接线：S&H 的 **OUT** → VCO A 最下排的 **cv**（左数第 2 个）。VCO A 的 **cv amt**（左上角绿色旋钮）放约 1/4。
- 步骤：
  1. 先听：VCO A 的音高在随机跳（S&H 在采样 drone 3 内部的噪声）。
  2. 接线：LFO A 的输出 → S&H 的 **IN**。LFO A 的 **wave** 和 **rate** 都往左拖到**拖不动为止**（Λ 纯三角、最慢 0.1 Hz）。
  3. 接线：LFO B 的输出 → S&H 的 **clock**。LFO B 的 **wave** 最右（⊓ 方波），**rate** 拖到约 1/3。再把 LFO B 的 rate 往左、往右各拖一下。
- 期望：
  - 第 2 步：不再是噪声那样快速乱跳，而是每隔几秒才变一次音高，在几个高低不同的音之间换（没接 clock 时用 DRONE 3 内部时钟，默认约 6–8 秒采样一次，一个 LFO 来回只采到一两个点）。
  - 第 3 步：音高一小级一小级地往上爬，到顶后停一会儿（S&H 输出最高 5 V，LFO 上半段被削平），再一小级一小级地往下走，约 10 秒一个来回。LFO B 越快，台阶越密越细；越慢，台阶越大越稀。
- 第 3 步之后还是"一高一低"交替：说明 LFO A 相对采样太快（每次采样时 LFO A 正好走了半圈）——确认 LFO A 的 rate 拖到底，把 LFO B 的 rate 往右加。
- DRONE 6 的 S&H 同样做一遍（3 根线改插到 DRONE 6 的 S&H 框）。

**T13.4 VCO 的 vca cv 输入** ✅
- 插孔位置：envelope A 在 VOICE MIXER 正下方偏左，最下排 4 个插孔从左到右：**gate**、红字 **env**、**vca cv**、红字 **VCO A**。要用的是第 3 个 **vca cv**。envelope B 在 VOICE MIXER 下方偏右，最下排从左到右：红字 **VCO B**、**gate**、红字 **env**、**vca cv**（最右）。
- 准备：RESET PANEL；BLEND 最左；DRONE VOICES 全关；envelope A 的 **hold** 不要点亮。不按键时 VCO A 没声。
- 步骤：
  1. 接线：LFO A 的输出 → envelope A 的 **vca cv**。LFO A 的 **wave** 拖到最右（⊓ 方波），拨杆 **x1**，**rate** 拖到约 1/10（每秒 2 次左右）。不按任何键。
  2. 来回拖 LFO A 的 **rate**。
  3. 拔掉线，再按一下键。
  4. 对 VCO B 做一遍：线改插到 envelope B 的 **vca cv**（最右）。
- 期望：
  - 第 1 步：不按键 VCO A 也会响，"嘀、嘀、嘀"一开一关（插线后 VCO A 的音量由这根线控制，代替 envelope A）。
  - 第 2 步：开关的快慢跟着 rate 变。
  - 第 3 步：恢复为按键才响。
  - 第 4 步：VCO B 一样（RESET 后 VCO A、B 都在 VOICE MIXER 里开着）。

---

**T13.5 envelope 区 VCO A / VCO B 输出到 PREAMP** ✅ (#84；2026-10-03 Mac 实测 B 输出有声，tune / MORPHING 正常；波形设置对齐后 A/B 对照正常)
- 准备：记录当前面板后 RESET PANEL，DRONE VOICES 全关，BLEND 最左；VOICE MIXER 的 VCO A、VCO B、EXT.AUDIO VOL 最小，只把 PREAMP VOL 放约一半；PREAMP gain 放约一半，MASTER 从小音量开始。不用按键，envelope A/B hold 关闭。
- 先接 envelope A 最下排最右红字 VCO A → PREAMP 的 ext. source：应持续有声。
- 只换源头到 envelope B 最下排最左红字 VCO B，目的地不变：应同样持续有声，调 VCO B 的 tune / MORPHING WAVEFORM 应能听到变化。
- 再只换源头到 VCO B 模块最下排最右红字 osc：应有声，幅度可以更大（两个输出的现有标度不同），不要求等响。
- 最后回接 envelope B 的 VCO B，再拔掉线：PREAMP 恢复音频输入设备信号；不要把麦克风本底当作残留振荡声。PREAMP 灯是否亮取决于增益和峰值，不把亮灯作为唯一通过条件。

**T14.1 MIDI 延音踏板按通道释放** ⏳
- 安排：留到外部 MIDI 集成测试；当前已自动覆盖 16 个通道的实际引擎 gate 释放。没有踏板时不要求购买设备或执行本项。
- 准备：可发送 CC64 的 MIDI 控制器或软件发送器；PLAY = SINGLE、MODE = KEYBOARD，BLEND 最左、DRONE VOICES 全关、envelope A hold 关闭、R 调短。
- 通道 2（设备界面编号）：按住一个音，踩下踏板，松开琴键。预期持续发声；再松开踏板，预期按 R 释放，没有卡音。通道 1、16 各重复一次。
- 同一通道：踩踏板、按下并松开音符，再按住同一个音；松踏板时应仍发声，最后松琴键才释放。
- 多通道（软件发送器可测）：通道 2 踏板按下不应延长通道 1 音符；通道 2 和 10 同音同时被各自踏板保持时，松通道 2 踏板不能释放通道 10 的音符。

**T14.2 MIDI 输入关闭/切换时音符停止** ✅（2026-10-03 Mac + MPK MINI IV：按住音时切 off 即停，切回后弹奏恢复）
- 安排：需要 MIDI 键盘（如 MPK mini IV）。
- 准备：Preferences 里 MIDI Input 选到该键盘；PLAY = SINGLE、MODE = KEYBOARD，BLEND 最左、DRONE VOICES 全关，只听 VCO A。
- 步骤：
  1. 在 MIDI 键盘上按住一个音不放（声音持续）。
  2. 保持按住，打开 Preferences，把 MIDI Input 切成 off。
- 期望：切掉的瞬间声音停止（松不松手都不应再响）。再把 MIDI Input 切回键盘，弹奏恢复正常。
- 备注：直接拔 USB 线的热拔场景目前还没有检测机制（RtMidi 不支持设备移除通知），拔线造成的卡音要切一次输入设备才会清；这是已知缺口。

**T14.3 关闭 MIDI 输入只停 MIDI 的音，电脑键盘的琶音继续** ✅（PR-D，2026-10-03 Mac + MPK MINI IV：B 通过；A 第一次点 OK 后"轰"一声琶音停止，修掉 OK 重开音频后复测：选 off 和点 OK 时琶音都继续，没有轰响）
- 准备：MPK 切到测试用预设；Preferences 里 MIDI Input = MPK Mini IV MIDI Port。RESET PANEL（菜单右下角点两下）；DRONE VOICES 6 个键全部点灭。
- A. 电脑键盘的琶音不受影响（这是修复的重点）：
  1. 点红色大旋钮打开键盘菜单。PLAY 页左边 MODE 点 **ARPEGGIATOR**；再点顶部 **ARP** 页签，点亮左上角的 **HOLD**（LED 亮）。点 CLOSE 关掉菜单。
  2. 电脑键盘同时按下 A、F、H 三个键，再全部松开：琶音一直循环 C、F、A。
  3. 琶音还在响的时候，⌘, 打开 Preferences，把 MIDI Input 切成 **off**，再点 **OK**。
  - 期望：选 off 时和点 OK 后，**琶音都继续循环 C、F、A，不停，也没有"轰"的一声**。（修复前选 off 就会全停；第一版修复后点 OK 会重开音频而停掉。）
  4. 收尾：Preferences 把 MIDI Input 切回 MPK Mini IV MIDI Port；打开菜单，ARP 页关掉 HOLD（琶音停下），PLAY 页 MODE 点回 **KEYBOARD**。
- B. MIDI 自己的音照样会停（确认修复没把这点弄坏，同 T14.2）：
  1. 在 MPK 上按住一个键不放，声音一直响。
  2. 保持按住，⌘, 打开 Preferences，把 MIDI Input 切成 **off**，点 OK。
  - 期望：**声音立即停**，手还按着也不响。
  3. 把 MIDI Input 切回 MPK Mini IV MIDI Port，MPK 弹奏恢复正常。

**T14.5 打开 Preferences 点 Cancel 不再"轰"一声** ✅（PR-E 附带修复，2026-10-03 Mac 通过）
- 准备：同 T14.3 A：键盘菜单 MODE = **ARPEGGIATOR**，ARP 页 **HOLD** 打开；电脑键盘同时按下 A、F、H 再松开，琶音循环。
- 步骤与期望：
  1. ⌘, 打开 Preferences，什么都不改，点 **Cancel**：琶音继续，**没有"轰"一声**。
  2. 再打开 Preferences，MIDI Input 切成 off，点 **Cancel**：琶音继续，没有"轰"一声；MIDI 输入恢复成 MPK Mini IV MIDI Port，在 MPK 上弹能出声（Cancel 真正撤销了 MIDI 的改动）。
  3. 再打开 Preferences，把 Buffer Size 换一个值，点 **OK**：这次会重开音频（可能有一声，琶音停下），这是改了音频设置时的正常行为。测完改回原来的 Buffer Size。
  4. 收尾：关 HOLD，MODE 改回 KEYBOARD。

**T14.4 MIDI 切换 MUTE 时按钮跟着亮灭** ✅（PR-D，2026-10-03 Mac + MPK MINI IV 通过）
- 准备：Learn 一个打击垫到 **MUTE**（MIDI CONTROL → + LEARN A CONTROL → 点 MUTE 按钮 → 敲垫子）。
- 步骤：敲这个垫子几次。
- 期望：每敲一下声音静音 / 恢复，同时面板上 MUTE 按钮的琥珀色光圈**立即**跟着亮 / 灭（以前要把鼠标移过去才更新）。测完删掉这个绑定。

### 15. MIDI 设置与绑定

2026-10-03 汇总：macOS（MBP M1 Max）+ MPK MINI IV。T15.5 / T15.6 中具备条件的项目先在重设计前的界面上通过；随后用 `2a5991a` 的 CI macOS 构建复测新界面（`7e9d803` + `2a5991a`）：布局、字体、分页和点击范围正常，左侧裁字（#4）和删除图标 x（#5）确认已修复；`2a5991a` 的细节（禁用按钮描边、Learn 状态文字红色、Esc 关闭后悬停高亮清除）真机确认。未测项见各条标注。

**T15.1 MIDI 设置界面** ✅（2026-10-03 `2a5991a` 新界面实测：CLOSE / Esc、< / > 改值、分页、EXTRA LEARN TARGETS 通过）
- 步骤：
  1. 点 DRONE VOICES 右边、MUTE 正下方的 **MIDI** 按钮。
  2. 看界面：暖色面板盖住键盘区；顶部 INPUT，左边 CHANNEL / TRANSPOSE / VELOCITY 和 LEARN，右边每页四条绑定（初次显示引导）。
  3. 点 **CLOSE** 关掉；再打开，按 Esc 关掉。
- 期望：开合正常；INPUT 显示 Preferences 里选的输入设备名（没选提示 Choose an input in Preferences）。
- 旧界面的左侧裁字、删除图标方框（反馈 #4 / #5）已在重设计中修复，2026-10-03 真机确认。缩放窗口后检查标题、设置值、行尾按钮、底部提示不截断或重叠；长设备名应带省略号，字号不被缩小。
- 只点设置两侧的 < / > 会改变数值，点 CHANNEL 等标签不会误改；TRANSPOSE 的最低值应显示 -36 st。
- 设置值首尾循环是既定行为：TRANSPOSE 在 -36 st 再点 < 跳到 +36 st；CHANNEL（ANY ↔ 16）、VELOCITY（LINEAR ↔ HARD）同样循环。
- 建立五条以上绑定后检查分页；Learn 完成后应自动显示刚绑定的行，删除最后一页唯一一行后回到前一页。
- EXTRA LEARN TARGETS：LEARN 等待选择目标时点 PRESET A，再敲 pad，应出现一行 TRIGGER；不在等待目标时这些按钮为灰色。

**T15.2 Learn 绑定一个旋钮** ✅（旧界面 CC24 → FREQ，绝对拾取无跳变；删除绑定通过）
- 准备：Preferences 里 MIDI Input 选到你的键盘；只听 VCO A。
- 步骤：
  1. 打开 MIDI 设置，点 **+ LEARN A CONTROL**（变红色 CANCEL LEARN，底部显示操作提示）。
  2. 点面板上 FILTER 一排左边的 **FREQ** 旋钮。
  3. 在键盘上转动一个旋钮（如 MPK 的 K1）。
- 期望：
  - 列表出现一行（CC 号 · 通道 → vcf 的 FREQ），LEARN 自动解除。
  - 转动该旋钮，滤波截止跟着动，面板上的旋钮也跟着动。
  - 绝对旋钮的接管：硬件旋钮位置离软件值很远时，先扫过当前值才开始跟（不跳值）。

**T15.3 相对模式、解绑、动作绑定** 部分通过（删除、pad → DRONE 1 每敲一次切换一次；REL 未测）
- 步骤：
  1. 点 T15.2 那行的 MODE，从 ABS 换成 REL 1（MPK 旋钮在硬件上设成 Relative 模式时用）。
  2. 点行尾的 x 删掉这条绑定。
  3. LEARN → 点 DRONE VOICES 的键 **1** → 按键盘的一个打击垫。
- 期望：
  - REL 1 下拧动按格增减、不跳值。
  - 删除后该旋钮不再控制滤波。
  - 打击垫按一下开 drone 1，再按一下关（切换，不是按住才响）。

**T15.4 通道过滤、八度、力度曲线与断电保存** ✅（八度、通道过滤、退出重开保存通过；2026-10-03 按下述 PRESSURE → VCO A cv 接线确认力度曲线：同样的轻 / 中 / 重击，HARD 的压力和音高低于 SOFT，反馈 #8 关闭）
- 步骤：
  1. MIDI 设置里 TRANSPOSE 调到 +12 st（st = 半音），弹几个音。
  2. 按下面的压力接线准备，对比 SOFT / LINEAR / HARD，使用相同的中等 Note On 力度。
  3. CHANNEL 设成一个键盘不在用的通道，弹琴；设回 ANY。
  4. 绑定一两条后退出应用重开。
- 期望：+12 时音高高一个八度；SOFT 将同样轻击映射为更高的压力值（是否更响取决于压力的接线和用途）；通道不匹配时完全无声、设回 ANY 恢复；重开后绑定和设置都还在。

- 力度曲线的听法（反馈 #8）：Note On 力度进入键盘压力通路，并不直接控制混音音量。PLAY = SINGLE、MODE = KEYBOARD、PRESSURE OUTPUT = PRESSURE、RISE / FALL = 0；只听 VCO A，BLEND 最左。将键盘右侧插孔组中间的 **PRESSURE** 输出接 VCO A 的 **cv**，cv amt 约 1/4。对同一个音、同一个中等力度值，SOFT 应比 LINEAR 压力高、音高更高，HARD 更低；力度极值不适合比较。用 MIDI Monitor 核对输入力度，或软件固定发 velocity 64；手弹的差异不能单独判定曲线失效。此项不等于 pad Aftertouch 测试。

**T15.5 修改设置 / Learn 时松键不挂音** 部分通过（通道、Learn 松键、切 off、off 不变直接 OK 通过；踏板未测，virtual input 误停鼠标琶音见 #14）
- 准备：PLAY = SINGLE、MODE = KEYBOARD；envelope A 的 HOLD 关闭、R 调小，DRONE VOICES 全关；先确认普通按键和松键能正常起音、停止。
- 步骤与期望：
  1. CHANNEL = ANY，按住一个琴键；改成键盘不使用的通道，再松键。原来的音应释放；新按下的键不发声。恢复 ANY 后弹奏正常。
  2. LEARN → 点 MUTE → 按住一个琴键，等绑定出现在列表后再松开。第一次按下可能仍作为演奏音，但松开后必须停止。随后按此键只切换 MUTE，松开不再切换。测完删除绑定，恢复 MUTE 关闭。
  3. 持续弹奏时，在 Preferences 把 MIDI Input 切成 off。不得残留持续音；切回键盘后正常弹奏。如果有延音踏板，再用踩住踏板的音重复通道切换，抬踏板后应释放。
  4. MIDI Input 已是 off 时，用键盘 MODE = ARPEGGIATOR、HOLD（ARP 页）保持一个正在运行的琶音，再打开 Preferences，保持 off、不改任何设置，直接点 **OK**，不要点 Apply。仅重复选择 off 不应停音或重启琶音；测完关闭 HOLD、恢复 MODE = KEYBOARD。另测 macOS virtual input → off：MIDI 音符应释放，但鼠标保持的琶音不应被清掉；后者曾失败（MIDI 待办 #14），PR-D 已修，复测见 T14.3。

**T15.6 映射数值与动作** 部分通过（分档开关、pad 力度到 BLEND、保存通过；pad → MUTE 学成 NOTE 40 / CH 10，每按一次切换一次；REL、CC 127/0 动作（MPK pad 发的是音符）、CC64 防误学未测）
- LEARN 一个硬件旋钮到 VCO A 的 **oct+3** 开关，保持 ABS，来回扫过全程。面板开关和实际音高应一致地按档变化，不得只动图形而声音不变。
- 若使用相对编码器：绑定到 **BLEND**，设成与硬件编码匹配的 REL 模式；先用鼠标把 BLEND 调到较高位置，再转编码器一格。应从当前位置小幅变化，不能跳回绑定时的旧位置。
- LEARN 一个打击垫到 **BLEND**：轻击 / 重击应直接给出不同的面板值，不必先扫过旧值。此项检查力度映射，不以音量大小判定。
- 若控制器有发送 CC 127 / 0 的按钮或可设为 CC 的打击垫：绑定 **MUTE**，按下只切换一次、松开不切换；再按才切换回来。
- 若有延音踏板：LEARN 选好目标后只踩 / 抬踏板，不应生成 CC64 绑定；再动一个普通旋钮应完成 Learn。
- 测完退出重开，确认绑定、CHANNEL / TRANSPOSE / VELOCITY 设置仍在；清除本次临时测试绑定。

**T15.7 打击垫 / 按钮控制面板开关；旋钮拾取不再"空转"** ✅（PR-C，2026-10-03 Mac + MPK MINI IV：第 1–4 步通过；第 3 步在用户预设下 SHIFT + 3 号垫切 CC# 后学成 CC 36 / CH 1、MODE = TOGGLE，按一下翻一次；第 5 步未单独测，BLEND 力度映射见 T15.6）
- 背景：以前打击垫或按钮绑到面板上的拨杆开关，按下只在按住时有效、第一下没反应，重击轻击还会给出不同结果；旋钮拾取时第一条消息总被忽略。
- 准备：RESET PANEL；DRONE VOICES 6 个键全部点灭；按住电脑键盘 H（A 音）能听到 VCO A。MIDI 输入在 Preferences 里选 MPK MINI IV。
- Learn 的方法（下面每一步都用）：点 **MIDI** 按钮（MUTE 下面）打开 MIDI CONTROL → 点 **+ LEARN A CONTROL** → 点面板上要控制的开关或旋钮 → 敲一下打击垫 / 转一下旋钮。表格里出现新的一行就是学好了。
- 步骤与期望：
  1. 打击垫 → VCO A 的 **-1 / sub** 拨杆。表格这一行 MODE 列显示 **STEP**。关掉 MIDI CONTROL，按住 H 不放，敲这个垫：每敲一下拨杆翻一次（拨到 sub 时叠加一个低八度的底音，拨回 -1 时底音消失），第一下就生效；轻敲重敲效果一样；松开垫子拨杆不动。
  2. 另一个打击垫 → VCO A 的 **oct+3 / low** 拨杆（三档）。MODE 显示 **STEP**。每敲一下拨杆移到下一档，三下回到原位，音高跟着变。
  3. 按钮发 CC 的情况（可选）。Learn 后看表格 **CONTROLLER** 列：`NOTE 数字` 是音符，`CC 数字` 是 CC。MPK 的 DAW / Plugin 预设下打击垫只能发音符，要先换到用户预设：
     - 按住 SHIFT 再按 **PLUGIN/DAW**（屏幕出现 User Presets），转大旋钮选一个用户预设，按下旋钮载入。弹一下琴键，确认 Lunar 24 还能收到（Preferences 里输入仍是 MPK Mini IV MIDI Port）。
     - 按住 SHIFT 再按 **3 号垫（CC#）**；继续按住 SHIFT，3 号垫一直亮绿灯才说明 CC# 模式开了。
     - Learn 一个垫子到 **-1 / sub**：CONTROLLER 显示 `CC …`，MODE 显示 **TOGGLE**；每按一下翻一次，松开不翻，第一下就生效。若要按两下才翻一次，是 MPK 全局设置 Toggle = On，改成 Off（SHIFT + LOOP 进 Global Menu）。
     - 测完删掉绑定，SHIFT + 4 号垫切回音符，再按 PLUGIN/DAW 回到原来的预设。
  4. MPK 的一个旋钮 → VCF 的 **FREQ L**（MODE = ABS）。先用鼠标把面板上的 FREQ L 拖到大约中间，再把 MPK 旋钮也转到大约中间、慢慢转动：面板旋钮马上跟着动，不用先转过头再转回来。然后用鼠标把 FREQ L 拖到最右，再转 MPK 旋钮：要转到和面板差不多的位置才接管（不会一转就跳）。
  5. 绑到 BLEND 的打击垫（若有，T15.6）：仍然是轻击 / 重击给出不同的值（BLEND 是旋钮，不是开关）。
  6. 测完在 MIDI CONTROL 里点每行右边的 **x** 删掉这几个临时绑定。

**T15.8 MIDI 弹奏点亮面板触摸板** ✅（PR-E，2026-10-03 Mac + MPK MINI IV 通过）
- 准备：MPK 用测试用预设；RESET PANEL。键盘菜单关着（能看到键盘区的 12 块金属触摸板）。
- 步骤与期望：
  1. 在 MPK 上按住一个 C 键：面板上 **C**（最左边）那块触摸板亮起，松开就灭。换不同八度的 C，亮的都是同一块。
  2. 同时按住 C、E、G：三块板一起亮；松开其中一个，只灭那一块。
  3. 敲一个没有绑定的打击垫（它发音符，比如 NOTE 40 = E）：对应音名的板（E）亮，松开灭。
  4. 敲一个绑定了 MUTE 或开关的打击垫：声音 / 开关照常变化，但**不点亮**任何触摸板。
  5. MIDI 设置里 TRANSPOSE 调到 +2：按 C 键，亮的是 **D**（亮的是实际发出的音）。测完调回 +0。
  6. 按住 MPK 的一个键时，把 Preferences 的 MIDI Input 切成 off：声音停，板也灭。切回 MPK Mini IV MIDI Port。
  7. 鼠标 / 电脑键盘弹奏照常点亮触摸板，和 MIDI 同时弹也互不影响。

**T15.9 MIDI 时钟：START 从头开始，不再停掉所有音** ⏳ (MIDI 时钟 PR)
- 需要一个能**发出 MIDI 时钟**的软件（这里用 Ableton Live）。MPK 手册只写了它能接收时钟，没写能发送。
- 准备（一次性，macOS 自带的虚拟 MIDI 线 IAC）：
  1. 打开「音频 MIDI 设置」（应用程序 → 实用工具），菜单 窗口 → 显示 MIDI 工作室，双击 **IAC 驱动程序**，勾选 **设备在线**，点 应用。
  2. Ableton Live → Settings（⌘,）→ **Link, Tempo & MIDI**：MIDI Ports 的 **Output: IAC 驱动程序 (总线 1)** 那一行，只打开 **Sync**（Track、Remote 保持关闭，免得 Ableton 的音符也传过来）。
  3. Ableton 顶部速度改成 **80**（和 Lunar 默认的 120 分得开，好听出是谁在控制速度）。
  4. Lunar 24 → Preferences → MIDI Input 选 **IAC 驱动程序 总线 1**（名字可能是英文 IAC Driver Bus 1），点 OK。这时 MPK 不在输入里，用电脑键盘弹。
  5. 键盘菜单 MODE = **ARPEGGIATOR**，ARP 页 **HOLD** 打开；电脑键盘同时按下 A、F、H 再松开。
- 步骤与期望：
  1. 时钟源不播放时：琶音按 Lunar 自己的 TEMPO 循环。
  2. Ableton 里按两下停止键（回到开头），再按播放：琶音改跟随 Ableton 的 80 BPM（变慢），**从和弦的第一个音开始**，琶音不会停、电脑键盘按的音也不会丢（以前一按播放所有音都停）。
  3. Ableton 按停止：琶音停在原地不再前进（最后一个音在半个节拍后收掉）；按两下停止再播放，又从第一个音开始。
  4. 改一下 Lunar 的 TEMPO：回到 Lunar 自己的时钟。
  5. 收尾：关 HOLD，MODE 改回 KEYBOARD；Preferences 的 MIDI Input 选回 MPK Mini IV MIDI Port。

## 还没测的（⏳）

- MIDI：REL（先配置 MPK 相对输出）、CC 127/0 动作、CC64 防误学；T14.1 踏板，以及运行中物理拔线（与 T14.2 的选 off 不同）。pad Aftertouch → PRESSURE / VIBRATO PRESSURE 未测（MPK 琴键只有力度，pad 可设 Chan / Poly Aftertouch）。
- VCO A / VCO B 的 dry 输出（需要 4 个以上输出的声卡，可用 BlackHole 16ch：声道 3 = DRY A、声道 4 = DRY B。注意 DRY 在 VCA 之后，要按住键或点亮 envelope 的 hold 才有声）。
- PREAMP 的 ext. source 插孔（不需要额外硬件：把 LFO A 输出插进去，麦克风输入应被取代，ENVELOPE FOLLOWER 的灯改跟 LFO 走；拔线恢复）。
- 键盘菜单 SERVICE 页的 10 个校准类设置（V/OCT OUT、PRESS OUT、DAC VREF、TOUCH、RELEASE、P MIN、P MAX、CHARGE、DISCHARGE、DEBOUNCE）：已核实为存储但无声音/行为效果（都是校准真机硬件的概念，软件里没有对应误差），只需确认改动能保存、重启后还在。ENCODER DIRECTION 现在会反转红色大旋钮的滚轮方向，见 T12.19。

已核实无需列入：DRONE 6 的控件与 DRONE 3 结构性同码（同一个 PapaVoice 实现，12 参数 7 插孔全部正确映射，无独立风险）；VOICE MIXER 的 EXT.AUDIO 通道已被 T11.4 的补充步骤覆盖；耳机音量旋钮按设计无功能（只存状态，不接任何音频通路）。

## TODO

### MIDI 待办与当前行为（2026-10-03 真机反馈；编号沿用反馈）

| 项目 | 核实结果与下一步 |
|---|---|
| #10 · 中：MIDI MUTE 按钮不刷新 | PR-D 已修：MUTE 状态一变面板就重画。复测见 T14.4。 |
| #14 · 中：virtual input → off 误停鼠标琶音 | PR-D 已修：关闭 / 切换 MIDI 输入时只释放 MIDI 自己按住或踏板保持的音，并让弯音轮归零；鼠标 / 电脑键盘的音和琶音不受影响（事件队列满时才退回全部停音的保护）。复测见 T14.3。 |
| #1 / #2：设备发现和默认选择 | 列表仅启动时枚举，运行中插入设备尚不刷新。启动时输入为未设置或 off，会尝试选第一个真实设备，连保存的 off 也会被覆盖；这是当前限制，不是已验证的 off 保存承诺。 |
| #3 / #6 / #15：交互反馈缺口 | MIDI 弹奏点亮屏幕键盘：PR-E 已做（按音名点亮，见 T15.8）；ABS 等待拾取无提示；CHANNEL 会过滤 Learn 输入且无提示。Learn 前先确认 CHANNEL = ANY 或设备通道。RESET PANEL 保留 MIDI 配置，见 DECISIONS。 |
| #7：无限旋钮端点空转 | 原因未确认。用 MIDI Monitor 记录到端点后继续转、再反转的原始 CC 值，区分控制器仍发送端点值与软件拾取/相对解码问题；暂不归因于 MPK 硬件。 |
| #9：R = 0、BLEND 最小时仍有尾音 | 尚未定位，不直接归因于混响。按下面的隔离准备复测并记录持续时间。 |

- **MPK MINI IV 的预设与端口（2026-10-03，据 MPK mini IV User Guide v1.2，手册由 owner 按需上传，不进仓库）**：
  - PLUGIN/DAW 键在两种"厂商预设"之间切换：Plugin 模式（配 AKAI 自带软件）和 DAW 模式（配一般 DAW）。owner 实测：白灯时 Lunar 24 能收到，按成红灯后收不到。
  - 第三种是 **User Presets**（用户预设）：按住 SHIFT 再按 PLUGIN/DAW，旋转屏幕下的大旋钮选择，按下旋钮载入。只有用户预设下才能：把打击垫切成 CC#（SHIFT + 3 号垫；SHIFT + 4 号垫回音符）、改打击垫 / 旋钮发的号码（SHIFT + OCT− 进 Program Edit）、改全局设置（SHIFT + LOOP 进 Global Menu，按 PLUGIN/DAW 退出）。
  - 琴键、打击垫、旋钮、弯音 / 调制轮、延音踏板的演奏数据都从 **MPK Mini IV MIDI Port** 发出（所有预设都是）；另外几个端口（DAW / Plugin / Software Control / Din）发的是给软件脚本的按钮和屏幕控制，Lunar 24 不需要。Preferences 里 MIDI 输入选 MIDI Port。
  - 全局设置里和我们有关的：**Toggle**（打击垫"瞬时 / 切换"，测 TOGGLE 绑定时应为 Off）、**KnobM**（8 个旋钮一起设 Abs / Rel，测 REL 用；Program Edit 里也能单个设）、**Aft**（打击垫按压后发 Chan / Poly aftertouch，或 Off）、**MidiCh / PadCh**（琴键和打击垫的通道，打击垫默认通道 10，所以 Learn 出来是 CH 10）。
  - **测试用预设**（2026-10-03 owner 已建）：一个用户预设，全局设置 Toggle = Off、KnobM = Abs、Aft = Chan、FullVel = Off，MidiCh / PadCh 保持默认（琴键 1、打击垫 10）。以后 MIDI 测试都先切到这个预设；Lunar 24 的 MIDI 设置 CHANNEL 保持 ANY。
  - 实测：用户预设下打击垫切成 CC# 后，学到的是 `CC 36 / CH 1`（CC 走通道 1，不是打击垫音符用的通道 10）。
  - 手册没写 Rel 模式用哪种相对编码，测 REL 时在 MODE 里轮流试 REL 1 / 2 / 3，哪种方向和速度正确就用哪种。
  - 手册只写了 MPK 的琶音器能**接收**外部时钟（Clock = Ext），没写它会往外发 MIDI 时钟；用 MPK 驱动 Lunar 24 的琶音 / 音序器时钟可能不行，需要实测或用 DAW 发时钟。
- **输入设置与 Apply（#12 / #13）**：MIDI 输入下拉框切换立即生效。PR-D 起，音频设置（设备、采样率、缓冲、声道）没变时，OK / Apply 不再重开音频：以前只改 MIDI 输入点 OK 也会重开音频、重建引擎，琶音和按住的音被清掉，还会"轰"一声（2026-10-03 T14.3 实测发现）。音频设置变了时，OK / Apply 照旧重开音频，这时琶音停下是正常的。PR-E 起 Cancel 也一样：音频设置没变就不重开（以前只是打开再 Cancel 也可能"轰"一声，见 T14.5）。
- **尾音隔离准备（#9）**：先记录要保留的设置。PLAY = SINGLE、MODE = KEYBOARD、HOLD（ARP 页）= OFF；只开 VOICE MIXER 的 VCO A，其它九路 VOL 最小，DRONE VOICES 全关。envelope A hold 关、R = 0、S 适中，移除 gate / vca cv 外部接线；两侧 FILTER 的 RES 最小、FREQ 较高，DIST 最小；BLEND 最左后等一秒，MASTER 用低音量。按下再松开同一音，检查是否仍有明显长尾；滤波器等下游处理可有短暂衰减，不要求逐采样立即归零。若仍有长尾，保留接线和设置截图、录音再定位。默认同时开启的 VCO B 或直送 PREAMP 的振荡源不能用于此项隔离。
- **MUTE 尾音（#11）**：MUTE 只淡出输出，不停止演奏或效果处理；解除时能听到静音期间音符的剩余尾音，属于既定行为，见 DECISIONS。

### 调音待办（测试中记下的听感问题，未改；括号内为 2026-10-02 核实的当前值）

- DRONE VOLT 过半后听起来变小。（满行程降 5 个八度、过半启动互调 FM，无响度补偿——音高进入次声频段，结构性变小）
- DRONE 3 的 mod 最大跨度太大。（当前峰峰正好 6 个八度：kNewDroneFmOctaves = ±3）
- S&H 默认太慢、跳动幅度太大。（默认约 6 秒采一次；输出 ±5 V，接 cv 后音高最多 ±5 个八度）
- 所有 drone 一起响时混音太满。（混音器是裸加和、无归一化，靠效果器末尾 tanh 软限幅兜底）
- 光敏传感器可以改成用鼠标操作。（目前纯装饰，无交互）
- VCO A → VCO B 的默认调制偏温和。（1:1 直通路由：±1 V 源进 ±5 V 输入，只用约 20% 量程）

### 键盘待办

- MIDI 键盘的音符都算左半边；TWIN / SPLIT 下如何分左右（按通道或按音区）还没定。（2026-10-02 核实：MIDI 输入从不设 side，默认 Left；延音踏板延迟释放的 note_off 同样算左半边，修的时候要一起改。）

### 音频设备备注

- MacBook 麦克风是单声道，Preferences 里 Input 1 (L) / Input 2 (R) 显示为空（iPlug2 设置窗口的显示问题），麦克风其实是开的。
- AirPods 的麦克风只有通话音质（24000 Hz），48000 Hz 打不开；戴 AirPods 时请用「MacBook 麦克风 + AirPods 输出」。选了打不开的输入时，程序只开输出，并弹窗「Audio input is off」（每个设备每次运行只提示一次）。
- 音频日志：`~/Library/Application Support/Lunar24/audio.log`（Finder 里按 ⇧⌘G 粘贴这个路径）。记录每次打开设备的设备名、声道、采样率、成功或失败，以及每 5 秒收到的输入峰值（input peak 为 0 表示输入没声音）。复现问题后把这个文件发过来。
