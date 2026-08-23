# 面板到桌面交互映射

> @Codex 2026-08-23。原则：**原面板决定可见结构；桌面层只解决没有实体旋钮、插孔、触摸片和音频接口的问题。**

## 1. 固定布局

| 原面板区域 | Lunar 24 对应 | 桌面适配 |
|---|---|---|
| 左上 DRONE 1/2 | 原位、原控制数量与标签关系 | 五组 MUTE/TUNE/MOD、VOLT、gate/hold、ATT/RLS、CV 均可直接操作 |
| 左中 DRONE 3 | 原位 | LFO/hi-low/noise/S&H/gate/envelope 与真实插孔一一对应 |
| 中上 DUAL EFFECTOR | 原位 | 卡带槽点击选择；左右 program 1/2/3；X/Y/Z、blend、master、耳机保持原义 |
| 中上 DUAL VCF | 原位 | L/R LP/BP、freq/res/CV、dist/gain/link 保持原义和归一化连接 |
| 中部 VOICE MIXER | 十路原顺序 | 每路 PAN+VOL；不增加 solo、aux、bus 或 meter |
| 中部 VCO A/B + ENV A/B | 原位镜像 | 六波形/形变、PWM、FM、sync、1V/oct、ADSR/VCA 与插孔保持原义 |
| 右中 DRONE 6；右上 4/5 | 与左侧镜像 | 不因屏幕布局而合并成复用组件页面 |
| 下排 LFO A / joystick / 5-step seq / preamp / envelope follower / LFO B | 原顺序 | joystick 用二维拖动；序列器仍限 3/4/5 stages 与 5 个 step |
| 底部 12 触摸片 + 显示屏/编码器 | 原位置、原跨度 | 指针/电脑键盘/MIDI 都进入同一 keyboard state machine |
| 右下 DRONE VOICES 1–6 | 原 2×3 排列 | 瞬时/保持行为由对应 voice 设置决定 |

## 2. 虚拟跳线

- 屏幕上只出现原面板插孔，并保留硬件的通用模拟跳线自由：任何红色输出都可接到任何黑色输入。软件可以提示 `CV / gate / clock / audio` 的典型用途，但**不能替用户禁止跨类型实验连接**。每个 Jack 使用与显示名称/坐标无关的稳定 ID。
- 保留手册写明的 normalized connection；插入虚拟线时按硬件语义断开归一化路径，拔线时恢复。
- 默认按一个物理插孔一根线：每个输入和输出各只接受一条可见跳线；不提供隐形 fan-out、混合器或逻辑处理节点。只有实机/手册证实原配连接方式支持 stackable/multiple 后，才放开同一输出多接。
- 线色仅表达信号类型，不加入随机彩虹色；路径可暂时隐藏、选中高亮或一键清空。
- `PatchGraph` 是 canonical 连接状态，屏幕 cable 只是它的可视化。patch 状态可作为唯一的“上次退出状态”随应用恢复；这只是让虚拟旋钮/跳线像实体件一样停在原位置，不增加声音能力。不得提供多套整机 preset 保存、浏览或切换；原 keyboard 自带的 4 个 presets 仍按手册实现。

## 3. I/O 映射

| 硬件端点 | 软件端点 |
|---|---|
| EXT AUDIO | Audio Settings 中选择的输入通道 → 原 EXT AUDIO mixer channel |
| PIEZO/PREAMP | 另一可选输入通道 → 原 preamp/envelope follower；无输入时静音 |
| WET OUT L/R | 主立体声输出 |
| DRY VCO A / B | 两个独立单声道逻辑输出，可映射到多通道设备输出 3/4 等 |
| Headphones | 跟随主立体声监听，不另造声音路径 |
| 12V/POWER | 保留原位置的静态电源/运行状态指示，不作为可调音频参数；由桌面应用启停替代 |

## 4. 键盘与压力

- 指针点击触摸片：固定中等压力；拖过触摸片产生与硬件相同的触键变化。
- 电脑键盘：固定压力 note/gate，仅作无 MIDI 设备时的基本演奏入口。
- MIDI velocity 初始化 pressure；channel aftertouch 连续更新 pressure。没有 aftertouch 时保持 velocity 值。
- 所有输入都必须经过原 keyboard 的 single/twin/split、quantizer、root、portamento、vibrato、arp/sequencer、clock 逻辑，不能绕过它直接驱动额外声部。
- 校准页改为输入归一化校准（velocity/aftertouch min/max → 原 pressure CV 范围），不增加新的表达维度。

## 5. 非面板设置的最小集合

系统窗口只提供以下设备级设置，且不占用主面板：

- audio input/output device、sample rate、buffer size、逻辑 I/O channel mapping；
- MIDI input device、channel、现有控制的 CC learn；
- UI scale、语言、恢复默认面板状态；
- About/开源许可。

除此之外的功能先视为越界，必须重新对照硬件能力后才能进入设计。
