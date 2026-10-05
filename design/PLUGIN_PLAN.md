# Lunar 24 AU / VST3 插件版实施方案（第三版）

- 日期：2026-10-05
- 依据：main 分支（版本 1.0.0，提交 `075431d`）的逐项代码核查；锁定的 iPlug2 子模块（`d54f6905`）源码；VST3 SDK 官方仓库。
- 第三版 = 第二版 + 业主对五个问题的决定（第二节）。实施中有变化时更新本文件。

---

## 一、结论

**可行，没有做不到的功能。** 发声引擎（`core/`）和界面本来就与独立 App 分开，iPlug2 原生支持 VST3 和 AU。

**真正的工作量在四件独立 App 不需要、插件必须做好的事：**

1. **`OnReset` 的含义不同**：插件里它被调用得很频繁（停止走带、旁路、关闭处理……），现在每次都会重建整台机器。
2. **播放中安全切换整台机器的状态**：DAW 打开工程、切预置时音频可能还在跑；现在的切换方式要求音频已经停下。
3. **插件窗口经常是关着的**：MIDI CC 的旋钮变化现在要靠面板刷新才写进保存状态。
4. **只属于独立 App 的功能要拆开**：屏幕尺寸、金属外壳、重开音频、日志、录音、状态文件。

| 阶段 | 内容 | 工作量（以现在一个 PR 的大小计） |
|---|---|---|
| **第 1 阶段：能用的插件** | macOS 上的 AU（AUv2）+ VST3、Windows 上的 VST3；乐器类型，立体声 WET 输出；音色随 DAW 工程保存和恢复；MIDI 弹奏；界面可缩放 | 约 8–10 个 PR |
| **第 2 阶段：像样的插件** | 宿主自动化；跟随 DAW 速度和播放 / 停止；多路输出（DRY A/B 单独出轨） | 约 4–6 个 PR |
| **第 3 阶段：可选** | 音频输入（PREAMP 处理 DAW 里的声音）；CLAP；AUv3 | 每项 1–3 个 PR |

---

## 二、已定的决定

| # | 问题 | 决定 | 技术说明 |
|---|---|---|---|
| 1 | DAW 与格式 | 主力 Ableton Live（MacBook Pro 上的最新版）；VST3 和 AU 都要支持；不考虑只认老格式的老 DAW | 见下方"AU 格式说明"：AU 做 **AUv2** |
| 2 | 插件里的 MIDI 绑定 | 和独立 App **共用全局文件**；实施中发现必须改再说 | 见下方"以后改成随工程保存的代价" |
| 3 | 插件里的 REC | **隐藏** | 插件版不编录音相关的平台代码 |
| 4 | 分发 | 不走 App Store / Apple 开发者账号；附一份说明，用户照着执行一两条命令 | 技术上可行，见第六节；只支持 Apple 芯片的 Mac，不考虑 Intel Mac |
| 5 | 播放中切换音色 | **方案 A**：切换瞬间短暂静音 | 见第三节第 4 点 |

**AU 格式说明（AUv2 和 AUv3）**
- AUv2 是 Mac 桌面上的主流 AU 格式，Logic、Ableton Live 等都直接支持，不是"过时格式"。插件就是一个放进 `~/Library/Audio/Plug-Ins/Components` 的 `.component` 包。
- AUv3 主要为 iPad / iPhone 设计。在 Mac 上它必须装在一个 App 里，以"App 扩展"形式运行，要求沙盒和带权限声明的签名，安装和分发都比 AUv2 麻烦得多。
- 所以第 1 阶段做 AUv2；AUv3 留在第 3 阶段，只有将来要做 iPad 版才值得。

**以后 MIDI 绑定改成随工程保存的代价：不大，约 1 个 PR。**
- 绑定已经有现成的编码 / 解码（`midi_map_encode` / `midi_map_decode`）。
- 第 1 阶段存进工程的数据从一开始就带"格式标记 + 版本 + 分段"的头。以后只是多加一段"MIDI 绑定"，旧工程照样能读（没有这一段就用全局文件）。
- 真正要想清楚的是规则，不是代码：工程里的绑定和全局文件谁优先；在插件里改绑定时写回哪里。

---

## 三、必须改的地方（按技术点）

### 1. 构建
- 新增 VST3、AUv2 两个构建目标（iPlug2 自带 `iplug_configure_vst3` / `iplug_configure_auv2`），只在 macOS / Windows 上加；Linux 照旧只编引擎和测试。
- VST3 SDK：3.8.0 起是 MIT 许可（已核对官方 LICENSE），与本项目的 Apache-2.0 兼容。不进仓库，CI 按**固定的 3.8.x 版本**下载（iPlug2 下载脚本默认拉 master，不能用默认值）。
- `config.h` 按格式区分：
  - 乐器类型 `PLUG_TYPE 1`；
  - 声道：插件 `"0-2"`；独立 App 分支保持现有 `"0-2 1-2 2-2 0-4 1-4 2-4"` 不变；
  - AU：`AUV2_ENTRY` / `AUV2_ENTRY_STR` / `AUV2_FACTORY` 等宏，类型 `aumu`、子类型 `Lu24`、厂商 `Lua2`（沿用现有编码）；
  - VST3 分类 `Instrument|Synth`；
  - Objective-C 类名唯一前缀，避免同一 DAW 同时载入 AU 和 VST3 时冲突。
- 新文件：`host/resources/Lunar24-AU-Info.plist`、`host/resources/Lunar24-VST3-Info.plist`。
- macOS 只出 Apple 芯片（arm64）版本，和独立 App 一样（CI 的 `macos-latest` 就是 Apple 芯片）。不支持 Intel Mac；DAW 也要以原生模式运行，不能用 Rosetta 模式打开。
- CI：mac / win 作业额外编插件，上传为 Actions 产物（`Lunar24-Plugins-macOS`、`Lunar24-Plugins-Windows`）。独立 App 的产物照旧。

### 2. 把只属于独立 App 的功能拆开
`plugin.cpp` 调用 11 个平台函数，分在三个文件：

| 函数 | 现在实现在哪 | 插件版 |
|---|---|---|
| `lunar_host_avail_logical_w/h`、`lunar_host_screen_scale`、`lunar_host_force_clamp` | `main.mm` / `window_metrics_win.cpp` | 不按屏幕开窗，用固定默认大小（第 6 点） |
| `lunar_host_case_margins`、`lunar_host_place_view` | 同上 | 没有金属外壳，边距为 0，面板铺满窗口 |
| `lunar_host_audio_watchdog`、`lunar_host_request_audio_reopen` | `iPlug_app_host_override.cpp` | 插件没有自己的音频设备：不要看门狗；"重开音频"改用第 4 点的安全切换 |
| `lunar_host_log` | `iPlug_app_host_override.cpp` | 不写 audio.log |
| `lunar_host_recordings_dir`、`lunar_host_reveal_dir` | `main.mm` / `window_metrics_win.cpp` | REC 隐藏，不需要 |

其它独立 App 功能：

| 功能 | 插件版 |
|---|---|
| Preferences（音频设备、采样率、MIDI 输入） | 不要，DAW 管 |
| MIDI 设备热插拔、自动选择输入 | 不要，DAW 负责送 MIDI |
| 启动读状态文件、30 秒自动保存、退出保存 | **关掉**。插件绝不能读独立 App 的状态文件，否则每个新实例都带着 App 上次的音色打开。改为存进 DAW 工程（第 5 点） |
| MIDI 绑定文件 | 和独立 App 共用全局文件。多个实例同时写时文件不会损坏（整文件原子替换），后写的覆盖先写的 |
| 菜单栏 About | 插件没有菜单栏；版本号放进 MIDI 设置面板或键盘菜单 SERVICE 页 |
| 面板上"NO AUDIO（见 Preferences）"提示 | 插件版换成不提 Preferences 的说法 |

**做法**：只属于独立 App 的代码用 `#if APP_API` 隔开，独立 App 编译出来的代码和现在一致；插件专用实现放新文件，不改 `main.mm`、`iPlug_app_host_override.cpp`、`window_metrics_win.cpp`。

### 3. `OnReset` 规则
- 现在：`OnReset` 每次都做完整 `engine_.prepare()`（重建整台机器，实测约 2.5 ms，会分配内存），还会读一次状态文件、处理 RESET PANEL。
- 插件里 iPlug2 调用它的场合：
  - AU：设置采样率、设置最大块大小、宿主发 Reset（Logic 在走带跳转、停止等时候会发）、旁路切换（`IPlugAU.cpp` 1169 / 1222 / 1237 / 2448 行）；
  - VST3：`setupProcessing` 和每次关闭处理（`IPlugVST3_ProcessorBase.cpp` 228 / 236 行）。
- 照搬的后果：每按一次停止，嗡鸣、效果器尾音、琶音器都从头开始；有的宿主发 Reset 时音频还在跑，变成实时安全问题。
- 插件版规则：
  - 采样率、最大块大小、声道配置**任一真的变了**：才完整重建（这些情况下宿主保证音频已停）；
  - 其它 Reset：只通过现有实时队列放掉按住的音、清掉延音踏板，不重建、不分配内存。
- 独立 App 的 `OnReset` 不变。

### 4. 播放中安全切换整机状态（方案 A）
- 现状：`commit_()`（`standalone_audio_engine.h:1024`）直接替换引擎内部的 `definition_` 指针，并假设音频线程已停；界面线程也直接写这个对象里的状态。DAW 打开工程或切预置时走这条路，音频线程会读到已释放的内存。
- 方案 A：
  1. 界面线程在后台把新状态解码、校验，并建好新的整机；
  2. 请求切换；音频线程在下一个块做几毫秒淡出，然后进入"静音、不碰整机"的状态，并回应"已停"；
  3. 界面线程看到回应后换上新整机，旧的由界面线程释放；
  4. 音频线程淡入恢复。
- 两条线程只通过原子变量交接，音频线程不等待、不加锁、不分配内存。界面线程等回应时设上限（例如宿主不在处理音频、音频线程根本不跑的时候），超时说明音频没在跑，可以直接切换。
- 同一个机制解决：DAW 载入工程 / 预置；插件里的 RESET PANEL（现在靠 `lunar_host_request_audio_reopen()` 重开音频，插件做不到）。独立 App 的 RESET PANEL 也改用它，不再重开音频。
- 专门测试：切换过程音频线程不分配内存（复用现有分配探测测试的写法）；在另一个线程持续出声时反复切换，不崩、无非法数值；切换前后状态一致；在 ASan + UBSan 下跑；再加一个 ThreadSanitizer 跑法检查数据竞争。

### 5. 音色随 DAW 工程保存
- 实现 `SerializeState`（编码当前整机状态写进工程）和 `UnserializeState`（解码 → 迁移 → 校验 → 交给第 4 点的切换）。
- 数据带"格式标记 + 版本 + 分段"的头，以后加段（例如每工程的 MIDI 绑定）时旧工程还能读。
- 数据损坏或校验失败：保持当前音色，不崩。
- 解码和建整机只在非音频线程做。

### 6. 插件窗口
- 默认大小约为面板的一半（接近现有最小尺寸 1216×836），可缩放并保持比例。先给几档固定大小，在 Ableton 里测稳后再开放自由拖动。
- 没有金属外壳；REC 和 Preferences 相关的界面项隐藏。
- 窗口关闭再打开时面板重建；面板自身状态（键盘菜单页、MIDI 学习状态）随窗口重置，音色不受影响。

### 7. 插件窗口关着时的状态写回
- 现状：MIDI CC 带动的旋钮变化、MIDI 绑定触发的卡带和预置切换，在面板显示刷新里（`panel_editor.h:1720`，`syncParametersFromAudioThread`）写回保存状态。
- DAW 里插件窗口多数时候关着：这些变化存不进工程，长度 256 的音频→界面队列会被塞满。
- 改法：挪到 `OnIdle`（iPlug2 插件不开窗口也有 `OnIdle` 定时器）。面板刷新只负责重画。独立 App 一起改，行为不变。

### 8. 电脑键盘弹奏
- Ableton 自己有电脑 MIDI 键盘，按键一般被它拿走；插件窗口里用 A W S E D… 弹琴板不可靠。
- 插件里弹奏走 DAW 的 MIDI 轨道；鼠标点琴板、拧旋钮不受影响。MANUAL_TESTS 写明，不当作 bug。

### 9. DAW 的速度和走带（第 2 阶段）
- 引擎已通过 `ControlEventKind::clock` 事件接收外部节拍（现在来自 MIDI 时钟或 CLOCK 插孔）。
- 插件版每个块读 iPlug2 提供的速度、播放位置（PPQ）和播放状态，算出本块内每个节拍的样本位置，生成同样的时钟事件；播放开始对齐小节，停止时停下。
- 规则：插件里只跟 DAW 走带，忽略 MIDI 时钟。

### 10. 宿主自动化（第 2 阶段）
- 先定**对外参数清单**：面板旋钮、开关、选择开关；不含键盘菜单内部项和效果器程序内部参数（注册表共 349 个参数）。清单写进 `spec/machine/lunar24.json`，由生成器产出。
- 宿主参数编号 = 注册表数字 id，永久不变；加测试防止编号被改。
- 名称带模块前缀（例如"VCO A Tune"，现在多个参数都叫"Tune"）；选择开关按档位离散化；旋钮用现有旋钮曲线（`knob_to_value`）映射。
- 双向同步：DAW 改参数走现有实时队列，面板跟着动；面板拧旋钮通知宿主（开始 / 数值 / 结束），DAW 能录自动化。

### 11. 多路输出与音频输入（第 2–3 阶段）
- 引擎已有 WET L/R + DRY A/B 四路输出：做成"主输出 + 一组辅助输出"。DAW 开关辅助输出会改变声道数，按第 3 点"声道配置变了"处理。
- PREAMP 接 DAW 的声音：做成带输入的乐器（AU 的 music effect 类型、VST3 的侧链），放在最后按需做。

---

## 四、第 1 阶段：PR 顺序（每步一个 PR）

| # | PR | 内容 | 完成标准 |
|---|---|---|---|
| 0 | **本方案入库** | `design/PLUGIN_PLAN.md` | 业主确认 |
| 1 | **写回挪到 OnIdle** | CC / 绑定动作的写回从面板刷新挪到 `OnIdle` | 独立 App 行为不变；现有测试全过；业主在 Mac 上过一遍 T15.2 / T15.3 |
| 2 | **引擎：播放中安全切换（方案 A）** | 第三节第 4 点；独立 App 的 RESET PANEL 改用它 | 新测试全过（含 ASan / UBSan / TSan）；独立 App 的 RESET PANEL 手测正常 |
| 3 | **拆分 + 插件构建** | App 专用代码用 `#if APP_API` 隔开（要有插件目标才能编译验证，所以和构建放在一起）；VST3 / AUv2 目标、固定 SDK 版本、plist、`config.h`、插件专用平台实现、CI 出插件包 | Ableton 里能载入 VST3 和 AU，能用 MIDI 弹出声音 |
| 4 | **插件 `OnReset` 规则 + 长块拆分** | 第三节第 3 点；块超过最大块时在插件入口拆开 | 停止 / 播放、旁路不让声音从头开始；新测试覆盖拆块 |
| 5 | **音色存进工程** | `SerializeState` / `UnserializeState`，带分段头 | 存工程 → 退出 Ableton → 重开，音色、连线、键盘设置都回来；播放中切预置只有一下短静音 |
| 6 | **插件窗口** | 默认大小、几档缩放、无外壳、隐藏 REC 等 App 专用项 | Ableton 里窗口大小正确、面板不被裁切 |
| 7 | **CI 验证** | VST3 validator（mac / win）、`auval`（mac） | CI 全绿 |
| 8 | **MANUAL_TESTS"插件"一节 + 业主在 Ableton 里测 + 修问题** | 载入、弹奏、存工程重开、多实例、窗口缩放、播放中切预置、窗口关着时 MIDI CC 是否存进工程 | 业主验收；可能要 1–2 轮 |

完成后：在 Ableton 里像普通软件乐器一样用 Lunar 24——载入、弹奏、存工程。

**进度（2026-10-05）**：0–8 的代码和文档都已提交，PR 依次是 #124（方案）、#125–#130（第 1–6 步）、第 7、8 步各一个 PR，后一个叠在前一个上。等业主按 MANUAL_TESTS 第 17 节在 Ableton 里验收。

**实施中和方案不同、或方案没写到的地方**：
- VST3 SDK 固定为 `v3.8.1_build_84`（MIT）。
- 插件的引擎固定按"0 进 2 出"准备；DAW 没接上两个输出时输出静音。
- 窗口不做"几档固定大小"：DAW 拖动和右下角三角都能连续缩放，但始终保持面板比例、限制在半尺寸到原尺寸之间。
- 插件里 REC 的位置放回硬件原样的耳机图标和 PHONE 旋钮（只存值，不影响声音）。
- DRONE VOICES 的开关也存进工程（分段 `KEYS`），重开工程和保存时一样在响；独立 App 仍然每次启动都关着。
- 共用 MIDI 绑定的前提：App 里 Learn 会记下设备名，插件不知道 MIDI 来自哪个控制器。插件里改为"任何设备的绑定都算数"，否则 App 里学的绑定在插件里全部失效。
- 独立 App 的状态文件和插件的工程数据共用同一个读取函数（`restore_saved_state`），旧格式升级和修补只有一份。

---

## 五、保护独立 App 和代码质量

- 风险评估：**中低**。`plugin.cpp` / `plugin.h` 是共用代码；第 4 点的安全切换改的是两个产品共用的引擎。
- 隔离：
  1. App 专用代码用 `#if APP_API` 隔开；
  2. 插件专用实现放新文件，不改 `main.mm`、`iPlug_app_host_override.cpp`、`window_metrics_win.cpp`；
  3. 插件用 iPlug2 标准脚本单独构建，不改独立 App 的构建目标和它的配置检查；
  4. 引擎改动单独成 PR。
- C++ 质量：
  - 新代码用 `std::unique_ptr` 等自动管理所有权，不手写 `new` / `delete`（iPlug2 控件按框架约定由 IGraphics 接管的除外）；
  - 音频线程路径用现有分配探测测试证明不分配内存；
  - 现有 ASan + UBSan 作业覆盖新测试；涉及线程交接的测试另加 TSan 跑法；
  - 每个 PR 检查改动附近的注释，过时的改掉或删掉，新注释只写"为什么"，简短。
- 每个 PR 照常产出独立 App 下载包；碰到共用代码的 PR，在 MANUAL_TESTS 注明要回归的 App 用例，业主在 Mac 上过完再合并。

---

## 六、测试与分发

- **自动验证**：现有全部测试照跑；新增拆块、状态切换、状态存取测试；CI 跑 VST3 validator 和 `auval`。
- **人工测试**：Ableton Live（VST3 和 AU 各测一遍），按 MANUAL_TESTS"插件"一节。
- **macOS 分发（不用开发者账号）**：技术上可行，普通用户照说明执行即可。
  - CI 给插件做本地签名（`codesign --sign -`，和现在的独立 App 一样）；Apple 芯片要求至少有本地签名，否则系统拒绝载入。
  - 用户复制插件：VST3 放 `~/Library/Audio/Plug-Ins/VST3/`，AU 放 `~/Library/Audio/Plug-Ins/Components/`。
  - 网上下载的文件带"隔离"标记，执行一条命令解除：`xattr -dr com.apple.quarantine <插件路径>`。
  - AU 新装后如果 DAW 没看到，执行 `killall -9 AudioComponentRegistrar` 或重新登录一次。
  - 只支持 Apple 芯片的 Mac；DAW 要以原生模式运行（Ableton 默认就是）。
- **Windows**：`.vst3` 文件夹放进 `C:\Program Files\Common Files\VST3\`；不签名也能被 DAW 载入。
- **商标**：只写"VST3 格式"；不使用 VST 标志。
- **多实例同声**：每个实例从同一个固定种子开始，同样音色的两个实例漂移完全一样。要不要每实例不同种子，由业主听了再定。

---

## 七、风险清单

| 风险 | 程度 | 应对 |
|---|---|---|
| 播放中载入状态导致崩溃或爆音 | **高**（不处理会读到已释放内存） | PR 2 安全切换 + 专门测试 |
| 宿主频繁调用 `OnReset` 导致声音重启或实时问题 | **中高** | PR 4 插件版规则 |
| 插件窗口关着时 CC 变化没存进工程 | 中 | PR 1 挪到 `OnIdle` |
| 宿主给的块大于声明的最大块 | 低–中 | PR 4 拆块 |
| 窗口缩放在各 DAW / Windows 高分屏表现不一 | 中 | 先几档固定大小，逐个测 |
| 电脑键盘在 DAW 里弹不了 | 低（预期行为） | MANUAL_TESTS 说明 |
| 自动化参数编号必须永久稳定 | 中（第 2 阶段） | 注册表数字 id + 防变化测试 |
| `auval` 对参数和状态较严格 | 中 | CI 跑 `auval` |
| 多实例 CPU 叠加（默认音色约占一个核的 18%，云端实测） | 中 | Ableton 测试时记录实际占用，必要时另开优化 |
| 独立 App 被插件改动影响 | 中低 | 第五节隔离措施 + App 回归手测 |
