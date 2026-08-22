# 桌面框架与界面方案

> @Codex 2026-08-23。目标：在不扩大 Solar 42N 能力边界的前提下，建立可在 macOS 开发、macOS/Windows 发布的开源桌面实现。

## 1. 选型结论

**采用 C++ + iPlug2 + IGraphics，CMake 构建，只生成 standalone app。**

- iPlug2 以固定 commit 的 submodule 引入，Lunar 24 自己编写很薄的 out-of-source CMake 壳；不直接修改框架主仓目录。
- iPlug2 官方直接支持 macOS/Windows 独立应用、音频与 MIDI I/O；IGraphics 面向音频产品，支持矢量控件和缩放。
- 主框架为 zlib-like 许可；所需内置依赖为 WDL（zlib-like）、NanoVG/NanoSVG（zlib）、MetalNanoVG/RtAudio/RtMidi（MIT）。
- 第一实现使用 IGraphics 的 NanoVG 路径；macOS 使用 MetalNanoVG，Windows 使用 NanoVG。面板是有限的 2D 矢量控件，不需要引入更重的 Skia。
- **只启用 APP/standalone target**；不生成 AU/VST/CLAP/AAX/WAM，不下载或接入任何专有插件 SDK。
- 不使用 WebView、HTML/CSS UI 或系统网页运行时；两平台共享同一套 C++ 面板与交互代码。

### 候选对照

| 方案 | 结论 | 原因 |
|---|---|---|
| **iPlug2 + IGraphics** | **采用** | 音频/MIDI/桌面/矢量 UI 一体；许可宽松；最少胶水层 |
| JUCE 9 | 不采用 | 技术成熟，但当前模块是 AGPLv3 / 商业许可双轨：开源轨会把整项工程锁进 AGPL，商业轨又含 EULA；相较 iPlug2 的宽松许可，这是没有必要承担的许可约束 |
| miniaudio + RtMidi + SDL3 自组 | 备选，不采用 | 都是宽松开源，但要自行补齐设备管理、参数绑定、状态、HiDPI 控件和跨平台窗口胶水；收益不足以抵消集成与维护成本 |

这不是功能性妥协：iPlug2 的 standalone 路径已经覆盖本项目需要的平台入口；`lunar-core` 是不 include iPlug2/IGraphics/平台 API 的纯 C++ library，声音算法、canonical state/parameter/patch model 均不依赖框架。

### iPlug2OOS 的许可边界

iPlug2 官方 README 推荐 `iPlug2OOS` 作为 out-of-source 起点，但截至本次核查，该模板仓库顶层**没有明确 LICENSE 文件**。因此本项目只参考它的目录思路，不复制其模板文件，也不把它列为依赖；待上游补充明确许可后才能重新评估。iPlug2 主仓本身的 zlib-like 许可不受此问题影响。

## 2. 运行结构

```text
macOS CoreAudio / Windows WASAPI
          │
   iPlug2 standalone shell
          │
 ┌────────┴────────┐
 │                 │
audio callback   UI/MIDI/input adapter
 │                 │
Lunar DSP core ← bounded parameter messages
 │
4 logical outputs: WET L/R + DRY VCO A + DRY VCO B
```

- 实时音频线程不分配内存、不加互斥锁、不做文件或 UI 工作。
- UI/MIDI 只改变原面板已有参数、gate、clock 与 CV 关系；通过有界消息通道交给音频线程。
- 逻辑输出严格保留 `WET L/R + DRY A + DRY B`。普通双声道设备只连接 WET L/R；四路同时外送需要至少四输出的音频接口，不新增内部混音捷径。
- `EXT AUDIO` 与 `PREAMP` 分别映射到用户选择的音频输入通道；它们仍走各自原始信号路径。

### 已知 standalone I/O 限制

iPlug2 当前 `IPlugAPP_host.cpp` 会以插件声明的**最大输入/输出通道数**打开 RtAudio stream，源码旁仍有 `TODO: flexible channel count`。如果直接声明四输出，只有双声道的设备可能无法打开。因此技术切片必须加入一个很薄的应用侧 host adapter：

- DSP 永远保留四个逻辑输出；
- 2-out 设备打开 2 通道，仅接 WET L/R；
- ≥4-out 设备可打开 4 通道，再把 DRY A/B 映射到所选物理通道；
- 不把 DRY 折回 WET，也不改变硬件信号图。

这是设备兼容层，不是声音功能扩张；修改/替换的 host 胶水必须独立封装，并按 iPlug2 zlib-like 许可明确标记改动。

## 3. 面板还原规则

### 几何

- 以官方面板参考图 **2400 × 1551（约 1.547:1）** 为唯一基准坐标系。
- 所有模块、标签、旋钮、插孔、键盘和留白按同一归一化坐标绘制；窗口只做等比缩放和留边，**不重排、不折叠、不分页**。
- 小屏只允许整体缩放与画布平移；核心控制不藏进标签页或“高级设置”。
- 控件和文字均用矢量绘制；参考 PNG/PDF 只作测量依据，不作为成品背景贴图。

### 视觉

- 保留 Solar 面板的扁平印刷感、黑色模块底、细线分区和有限强调色；不做 3D 拟物机箱、星空、发光月球或动态装饰。
- 奶油底改为克制的月尘灰；品牌改为 **LUNAR 24**。不复制 ELTA/SOLAR 商标、logo 或原面板插画。
- 原面板颜色不是装饰，而是功能编码。月亮配色必须保持五类清晰可辨：
  - drone 调音 / LFO
  - VCO / envelope
  - effector / filter / master
  - joystick / sequencer / preamp
  - mixer volume
- 最终色值在 UI 样机中用灰阶和常见色觉缺陷模式复核；在此之前只确定角色，不擅自定一套炫技色盘。

### 交互

- 旋钮：垂直拖动；修饰键细调；双击回默认值。开关保持原面板的锁定/瞬时语义。
- 插孔：从输出拖到输入形成可见虚拟跳线；输入被新线占用时替换旧线；只暴露原面板真实存在的插孔和归一化连接。
- 允许临时隐藏跳线以看清面板，但不改变连接；不提供隐藏调制矩阵。
- 双效果器保留“卡带槽”语义：点击槽位选择 13 张卡带之一，左右仍各选程序 1/2/3，X/Y/Z 含义随卡带变化；不改造成通用效果器浏览器。
- 触摸键盘、旋钮、按钮、显示屏/编码器与六个 drone 键保持原位置和作用；不臆测显示屏具体技术类型。

## 4. 桌面输入适配边界

| 桌面输入 | 只允许映射到 | 明确不做 |
|---|---|---|
| 鼠标/触控板 | 面板旋钮、开关、插孔、触摸键；触摸键压力使用固定中值 | 手势宏、XY 多点调制 |
| 电脑键盘 | 12 触摸键与 6 个 drone gate；仍受 single/twin/split 模式约束 | 增加声部或和弦记忆 |
| MIDI Note | 触摸键的 pitch/gate | 超出硬件行为的任意复音 |
| Velocity / Aftertouch | 触摸压力或已有 CV 控制 | MPE、每音符新增调制目标 |
| MIDI Clock | 原 external clock | 新增同步系统或时间线 |
| MIDI CC learn | 原面板已有旋钮、开关、CV 输入 | 新参数、隐藏调制、宏控制 |

MIDI 是**控制适配层**，不是新的合成能力。它替代手指、触摸压力和外部 CV/gate 控制器，但不能增加声源、调制目的地、复音数或效果器。

## 5. 明确排除

- 插件格式、DAW hosting、时间线、多轨录音、采样库、云服务。
- 通用 modulation matrix、无限路由、随机化面板、宏旋钮。
- 频谱仪/示波器等原硬件没有的创作功能。
- 超出 13×3 卡带目录的效果、额外滤波器或额外振荡器。

这些排除项防止桌面便利性悄悄变成产品能力扩张。

## 6. 开发前验证门

在写 DSP 主体前先做一个可丢弃的技术切片：

1. macOS/Windows 各构建 standalone 空壳；验证 2-out 与 ≥4-out 设备的可变物理通道打开、四逻辑输出、音频输入、MIDI 输入和设备切换。
2. 用同一份归一化坐标绘制一个 drone 模块、一个 VCO 模块、十路 mixer、虚拟跳线和 12 键键盘。
3. 以 2400×1551 Design Coordinate Space 和统一 transform，在 50%/67%/75%/100%/125%/150%/200% 及 fit-to-window 下检查文字、旋钮命中区、线宽与帧率；缩放不能改变逻辑坐标或状态。
4. 音频线程压力测试确认 UI 拖动和大量 MIDI CC 不产生爆音。

只有四项均通过，才锁定 iPlug2 版本与渲染后端并进入完整实现；失败时才回退到“miniaudio + RtMidi + SDL3”自组方案。

## 7. 官方来源

- iPlug2 官方说明与平台/许可：<https://github.com/iPlug2/iPlug2>
- iPlug2OOS（仅作结构参考；当前顶层无明确 LICENSE）：<https://github.com/iPlug2/iPlug2OOS>
- iPlug2 许可及内置依赖清单：<https://github.com/iPlug2/iPlug2/blob/master/LICENSE.txt>
- iPlug2 standalone 当前通道打开逻辑：<https://github.com/iPlug2/iPlug2/blob/master/IPlug/APP/IPlugAPP_host.cpp>
- JUCE 当前许可：<https://github.com/juce-framework/JUCE/blob/master/LICENSE.md>
- miniaudio 平台、后端与许可：<https://github.com/mackron/miniaudio>
- SDL3 平台与 zlib 许可：<https://wiki.libsdl.org/SDL3/FrontPage>
- RtMidi 平台入口：<https://github.com/thestk/rtmidi>
