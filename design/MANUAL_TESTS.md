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
| 恢复出厂 | 键盘菜单顶部 **RESET PANEL**，点一次变成 CLICK TO CONFIRM，4 秒内再点一次 |
| 静音 | DRONE VOICES 右边的 **MUTE** 按钮：所有输出立即静音（亮琥珀色光圈），再点恢复 |
| 音频设备 / 麦克风 | 屏幕顶部菜单栏 Lunar24Host → Preferences…（⌘,） |

### 测试常用准备

- **出厂状态**：RESET PANEL（点两次）。几乎每项测试都从这里开始。
- **去掉混响**：DUAL EFFECTOR 区的 **BLEND** 拖到最左。声音变"干"，静音后不会有长尾音。
- **只听某一路（solo）**：VOICE MIXER 里其它 9 路的 **VOL** 拖到最左。或者在 DRONE VOICES 里只留要听的 drone。
- **关掉所有 drone**：键盘区右边 DRONE VOICES 的 6 个键全部关掉。
- 卡音（声音停不下来）时：先点 **MUTE** 静音，再试 RESET PANEL。

---

## 面板地图

面板从上到下分三排，下面是键盘区。下表从左到右列出每排的模块。

| 位置 | 从左到右 |
|---|---|
| 上排 | DRONE 1 · DRONE 2 · DUAL EFFECTOR（上半）+ FILTER L / FILTER R（下半一排）· DRONE 4 · DRONE 5 |
| 中排 | DRONE 3 · VCO A · VOICE MIXER（下方是 envelope A / envelope B）· VCO B · DRONE 6 |
| 下排（白字标签） | LFO A · JOYSTICK · 5 STEP SEQ. VOLTAGE · PREAMP · ENVELOPE FOLLOWER · LFO B |
| 键盘区 | 左下角摇杆 · 12 块触摸板 · 中间红色大旋钮（菜单）和蓝色小屏 · 右边 DRONE VOICES（1 2 3 / 4 5 6）和 **MUTE** |

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

### 键盘菜单 SETTINGS 页（按位置，从左到右）

| 行 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | PLAY | MODE | SCALE | ROOT | BPM | GLIDE | LEGATO | VIB RATE | VIB DEPTH | VIB DELAY | VIB PRESS | PRESSURE |
| 2 | P RISE | P FALL | ARP HOLD | ARP DIR | ARP VAR | ARP INT | ARP LEN | SEQ RUN | SEQ LEN | SEQ DIR | SEQ CV | RHYTHM |
| 3 | ENCODER | CAL V/OCT | CAL PRESS | DAC REF | TOUCH | RELEASE | P MIN | P MAX | CHARGE | DISCHARGE | DEBOUNCE | 5-STEP CLK |

- 框形开关点一下切到下一项，右键切到上一项。
- MODE 的选项依次是 KEYBOARD / ARPEGGIATOR（显示为 ARPEGGIATO）/ SEQUENCER。

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
  2. 打开菜单栏 Lunar24Host → Preferences…。
- 期望：
  - 声音自动跟随当前输出设备，没有杂音。
  - 音频启动不了时，蓝色小屏显示 NO AUDIO。
  - Preferences 窗口能打开。

**T0.4 RESET PANEL** ✅ (#58)
- 步骤：
  1. 随便转几个旋钮，接一根线。
  2. 键盘菜单 → RESET PANEL → 4 秒内再点一次。
- 期望：旋钮回到原位、线消失，声音短暂停顿后恢复成默认 drone。

**T0.5 MUTE** ⏳
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

**T0.6 指示灯** ⏳
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

**T9.1 LFO A / LFO B** ✅
- 步骤：
  1. 接线：LFO A 输出 → VCO A 的 **cv**。
  2. 拖 **rate**。
  3. 拨 **x6 / x1 / x10**。
  4. 拖 **wave**。
  5. 线改接 LFO B 的输出，重复 2–4。
- 期望：
  - rate 改变摆动快慢；x6、x10 快很多倍，最快时变成颤抖的音色。
  - wave 最左是方波（音高跳）、最右是三角（平滑滑动）。
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
- 期望：每个旋钮下有完整的名字和数值，每个框形开关下有名字。

**T12.2 GLIDE / VIB** ✅
- 步骤：
  1. **GLIDE** 调到中间，弹两个相隔较远的音。
  2. **VIB DEPTH** 调大，**VIB RATE** 放中间，按住一个音；再把 **VIB DELAY** 调大。
- 期望：
  - GLIDE：音高从前一个音滑到后一个，GLIDE 越大越慢。
  - VIB：音高自动颤动；VIB DELAY 大时先平稳一会儿再颤。

**T12.3 ARPEGGIATOR** 🔧 (#67, #68)
- 步骤：
  1. **MODE** 切到 ARPEGGIATOR。
  2. 同时按住 2–3 个键，然后全部松开。
  3. 调 **BPM**、**ARP DIR**。
  4. 打开 **ARP HOLD** 后松键，再关掉。
  5. MODE 切回 KEYBOARD。
- 期望：
  - 按住时按节奏循环弹这几个音，**松开马上停**。
  - BPM 改变速度；ARP DIR 改变顺序。
  - ARP HOLD 时松键继续播放。
  - 切回 KEYBOARD 时声音停下，不卡住。

**T12.4 SCALE / ROOT** ✅
- 步骤：切换 **SCALE**，弹几个键；再改 **ROOT**。
- 期望：音被"吸"到所选音阶上；ROOT 改变音阶的起始音。

**T12.5 16 步音序器（SEQUENCER 页）** 🔧 (#68)
- 步骤：
  1. 菜单左上角点 **SEQUENCER**：把几步的滑块拖到不同高度，关掉一两步的 gate 按钮。
  2. 回到 SETTINGS，**MODE** 切到 SEQUENCER（**SEQ RUN** = FREE）。
  3. 按住一个键，换另一个键。
  4. 调 **SEQ LEN**、**SEQ DIR**。
  5. SEQ RUN 切到另一个选项，按住键再松开。
  6. MODE 切回 KEYBOARD。
- 期望：
  - 第 1 页：16 列，每列有步号、滑块、音名、gate 按钮。
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
  1. 打开键盘菜单，**PLAY** 选 SPLIT。标题行（RESET PANEL 右边）出现 **EDIT: LEFT** 按钮。
  2. 这个按钮决定菜单里的设置改的是哪一半键盘：显示 EDIT: LEFT 时改左半边，显示 EDIT: RIGHT 时改右半边。目标是左半边普通弹奏、右半边琶音：
     - 左半边不用改（**MODE** 默认就是 KEYBOARD）；
     - 点一下 **EDIT: LEFT**，它变成 **EDIT: RIGHT**；
     - 把 **MODE** 改成 ARPEGGIATOR（只改了右半边）；
     - 点 **CLOSE** 关掉菜单。
  3. 按住左半边的 **A**；再同时按住右半边的 **H** 和 **J**。
  4. 重新打开菜单，在 EDIT: LEFT / EDIT: RIGHT 之间切换，看 MODE 显示。
  5. **PLAY** 改成 TWIN。
- 期望：
  - 第 3 步：左边是一个持续的低音（普通键盘）；右边两个高音按节奏轮流响（琶音）。
  - 第 4 步：EDIT: LEFT 时 MODE 显示 KEYBOARD，EDIT: RIGHT 时显示 ARPEGGIATOR。
  - 第 5 步：EDIT 按钮消失；左右两边都按左边的设置弹（都是普通键盘，右边不再琶音）。
- 测完把 oct+3 拨回 low。

---

## 还没测的（⏳）

- 键盘：
  - **PRESETS**（预设保存和载入）；
  - **PRESSURE**、**P RISE**、**P FALL**、**VIB PRESS**；
  - **LEGATO**、**ARP VAR / ARP INT / ARP LEN**、**SEQ CV**、**RHYTHM**；
  - 键盘区的 CLOCK / RESET 输入、GATE L / GATE R / V/OCT 输出。
- MIDI 键盘输入。
- DRONE 2 / 4 / 5 / 6 逐个过一遍（和 DRONE 1 / 3 同一套代码）。
- 所有 drone 的 GATE 输入和 env 输出插孔；DRONE 3 / 6 的 LFO 输出和 S&H 的 IN / clock 插孔。
- VCO 的 vca cv 插孔、VCO A / VCO B 的 dry 输出。
- VOICE MIXER 的 EXT.AUDIO 通道、PREAMP 的 ext. source 插孔、耳机音量旋钮。
- 5 STEP SEQ. VOLTAGE 的 CLOCK OUT。
- 键盘菜单第 3 行的校准类设置（软件里意义不大）。

## TODO

### 调音待办（测试中记下的听感问题，未改）

- DRONE VOLT 过半后听起来变小。
- DRONE 3 的 mod 最大跨度太大。
- S&H 默认太慢、跳动幅度太大。
- 所有 drone 一起响时混音太满。
- 光敏传感器可以改成用鼠标操作。
- VCO A → VCO B 的默认调制偏温和。

### 键盘待办

- MIDI 键盘的音符都算左半边；TWIN / SPLIT 下如何分左右（按通道或按音区）还没定。

### 音频设备待办（先跳过，以后加日志再查）

- 戴 AirPods Pro（输出走 AirPods）时，MacBook 麦克风收不到声音（PREAMP 蓝灯、ENVELOPE FOLLOWER 绿灯都不亮）。手动改回 MacBook 麦克风、摘掉 AirPods、RESET PANEL 都不恢复。不用 AirPods 时正常。很可能和下一条同源：启动时输入被换成了 AirPods 的麦克风，AirPods 进入通话模式，带输入打不开，只开了输出。下一条修好后请复测。
- 🔧 重启应用后输入设备变成 AirPods Pro / BlackHole 2ch：已找到原因并修复。设备名在保存时带了一个前导空格（" MacBook Pro Microphone"），设置文件读回来时会去掉空格，于是对不上任何设备，程序就退回 macOS 的默认输入设备。现在设备名统一去掉首尾空格。复测：选好 MacBook 麦克风，退出再打开，Preferences 里应该还是 MacBook Pro Microphone；戴着 AirPods 打开应用也一样。
- MacBook 麦克风是单声道，Preferences 里 Input 1 (L) / Input 2 (R) 显示为空（iPlug2 设置窗口的显示问题），麦克风其实是开的。
- 🔧 AirPods 相关（日志查明，已修）：
  - AirPods Pro 在系统里是两个同名设备（一个只有麦克风，一个只有耳机），程序按名字找输出时拿到了麦克风那个，打不开。现在按方向查找。
  - 打不开时程序临时关掉输入以保证有声音；紧接着点 OK 会把"输入关"存成你的选择，以后每次启动麦克风都不开。现在临时关闭不会被存下来。
  - 应用运行中插拔设备时设备列表不刷新（Preferences 里看不到新连上的 AirPods）。现在设备增减会自动刷新并重新打开。
  - AirPods 的麦克风只有通话音质（24000 Hz），打不开（系统拒绝 48000 Hz）；戴 AirPods 时请用「MacBook 麦克风 + AirPods 输出」（已验证可用）。选了打不开的输入时，程序只开输出，并弹窗「Audio input is off」说明原因（每个设备每次运行只提示一次）。
  - 复测：应用开着戴上 / 摘下 AirPods，Preferences 列表应立刻出现 / 消失 AirPods；选 MacBook 麦克风 + AirPods 输出，拍手有反应；把输入误选成 AirPods 后再改回 MacBook 麦克风，拍手有反应，重启后仍然有。
- 音频日志：`~/Library/Application Support/Lunar24Host/audio.log`（Finder 里按 ⇧⌘G 粘贴这个路径）。记录每次打开设备的设备名、声道、采样率、成功或失败，以及每 5 秒收到的输入峰值（input peak 为 0 表示输入没声音）。复现问题后把这个文件发过来。
