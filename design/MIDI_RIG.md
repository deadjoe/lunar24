# MIDI 控制器方案：AKAI MPK mini IV + M-VAVE SMC-Mixer

业主的两台控制器怎么设号码、谁管什么。起步方案（2026-10-16），弹过之后按手感改，改了就更新这里。
MPK 的厂商预设、用户预设怎么进、测试用预设见 [MANUAL_TESTS](MANUAL_TESTS.md) 的「MIDI 待办」。

## 1. 先懂两个概念

- 一条 CC 消息 = 3 个字节：**类型 + 通道**、**CC 号**、**数值**。通道只有 **1–16**；CC 号和数值都只有 **0–127**（数据字节只有 7 位）。没有别的号码可用。
- **独立 App** 分得清"哪个设备、哪个通道"：两台设备用同一个 CC 号也互不干扰。
- **Ableton 里的插件** 分不清：Live 把所有设备合成一路，通常还统一改成通道 1。插件只看"CC 号 / 音符号"，**两台设备的号码重了就会打架**（只有先学的那条绑定生效，两台都能动它）。所以两台设备的号码必须**整体不重叠**，这样 App 和插件用同一套方案，行为一致。

## 2. CC 号码总表（0–127，2026-10-17 盘点）

规则：两台设备的号码不重叠（插件里不分设备、不分通道）；不用有实际作用的号；不用 Lunar 24 出厂 CC 表的号（`host/plugin.h` 的 `kMidiCc`：**没被绑定时** CC 1 / 74 → FREQ L，71 → RES L，91 → BLEND，7 → MASTER）。音符和 CC 是两种消息，号码相同也不冲突。

| 号码 | MIDI 规范里的意思 | 现在归谁 |
|---|---|---|
| 0、32 | 音色库选择（Bank Select）高 / 低位 | **避开** |
| 1 | 调制轮 | MPK 摇杆上推；没绑时按出厂表动 FREQ L |
| 3、9 | 未定义 | 空 |
| 6、38 | 数据输入（配合 RPN / NRPN 改合成器内部参数） | **避开** |
| 7 | 音量 | **避开**（出厂表 → MASTER） |
| 2、4、5、8、10–13 | 呼吸、脚控、滑音时间、平衡、声像、表情、效果控制 | 空 |
| 14、15 | 未定义 | 空 |
| 16–19 | 通用控制 1–4 | SMC Mute 1–4 |
| 20–23 | 未定义 | SMC Mute 5–8 |
| 24–31 | 未定义 | **MPK 旋钮 1–8** |
| 33–37、39–43 | 1–11 号的"低位"（凑 14 位精度用；Lunar 24 不拼） | 空。36–43 留给 MPK 打击垫：切成 CC 模式就是这几个号，所以打击垫保持音符模式 |
| 44–51 | 12–19 号的低位 | SMC Record 1–8 |
| 52–59 | 20–27 号的低位 | SMC Select 1–8 |
| 60–63 | 28–31 号的低位 | **SMC Solo 1–4** |
| 64 | 延音踏板（Learn 会忽略它） | **避开** |
| 65–68 | 滑音开关、持音、弱音、连奏踏板 | **SMC Solo 5–8**。Lunar 24 不处理这几个号；只有把 SMC 也送进另一件实现了这些踏板的乐器（多为钢琴类音源）时，按住按钮那一下才会踩下它的踏板 |
| 69 | 保持踏板 2 | 空 |
| 70–79 | 音色控制（71 共鸣、74 亮度在出厂表里） | 空 |
| 80–83 | 通用控制 5–8 | SMC Play、Stop、Record、Rewind |
| 84 | 滑音控制 | SMC Fast Forward（Lunar 24 不处理，只送进 Lunar 时没影响） |
| 85–87 | 未定义 | SMC Bank◀、Bank▶、Up |
| 88 | 高精度力度前缀 | SMC Down（同 84） |
| 89、90 | 未定义 | SMC Left、Right |
| 91–95 | 效果深度（91 混响在出厂表里） | 空 |
| 96–101 | 数据增减、RPN / NRPN | **避开** |
| 102–109 | 未定义 | SMC 编码器 1–8 |
| 110–117 | 未定义 | SMC 推子 1–8 |
| 118、119 | 未定义 | 空 |
| 120–127 | 通道模式（全部静音、全部音符关、复位等） | **避开** |

真正"未定义"的号只有 3、9、14、15、20–31、85–87、89、90、102–119，加上通用控制 16–19、80–83 共 47 个；MPK 占了 8 个，剩下的装不下 SMC 的 59 个控件，所以 Record / Select / Solo 用了 Lunar 24 不处理的"低位"号和踏板号（跳过 64）。

MPK（业主的用户预设，2026-10-17 核对）：8 个旋钮 CC 24–31（通道 1，REL 2）；打击垫发音符 36–43（通道 10）；摇杆上推 CC 1。

## 3. SMC-Mixer 的号码（CC / User 预设，全部通道 1）

用业主自己的管理工具改。按钮保持 **mode 2**（按下 127、松开 0）：Lunar 24 只在按下时切换一次，松开不管；改成硬件自锁会变成按两下才切一次。备用 SysEx（`F0 7F 7F 06 xx F7`）在 CC 模式下不发，不用管。

| 控件 | 原来 | 改成 | 备注 |
|---|---|---|---|
| 编码器 1–8 | 70–77 | **102–109** | type 从 **CC**（导出文件里的 0，绝对值：到 0 / 127 就停，还要等接管）改成 **CW**（导出文件里的 3，只发"往哪边转了几格"的相对值）。CW 模式下 minimum = 127、maximum = 1：逆时针一格发 127、顺时针一格发 1，Lunar 24 识别为 REL 2，这两个值别改。Learn 后**慢慢**来回拧，MODE 应显示 REL 1/2/3；仍是 ABS 就把 BINDINGS 里看到的数值记下来 |
| 推子 1–8 | 78–85 | **110–117** | 绝对值，带接管（pickup） |
| Mute 1–8 | 20–27 | **16–23** | |
| Solo 1–8 | 28–35 | **60–63、65–68** | 按列 1–8 依次，**跳过 64**（延音踏板，Learn 学不上）。起初定的 24–31 就是 MPK 旋钮的号（2026-10-17 改）；32–39 也不行：32 是音色库选择、38 是数据输入，36–39 还和打击垫的 CC 模式撞 |
| Record 1–8 | 36–43 | **44–51** | 原来的 36–43 和 MPK 打击垫的 CC 模式撞 |
| Select 1–8 | 44–51 | **52–59** | |
| 全局 11 个 | 52–62 | **80–90** | 顺序：Play、Stop、Record、Rewind、Fast Forward、Bank◀、Bank▶、Up、Down、Left、Right |

LED 设置是 255。Lunar 24 现在不往控制器回发 MIDI，按钮灯不会跟着面板状态亮（以后的事，见第 8 节）。

## 4. 分工

原则：**演奏中要动的**上硬件，**一个音色调一次的**用鼠标。手大部分时间在 MPK 上，所以 MPK 管"正在弹的声音的音色"；SMC 像调音台，一列管一路声音。

### MPK mini IV

| 控件 | 绑定 |
|---|---|
| 8 个旋钮（CC 24–31，REL 2） | 24 **BLEND**、25 **X**、26 **Y**、27 **Z**、28 **FREQ L**、29 **FREQ R**、30 **RES L**、31 **RES R** |
| 打击垫（音符 36–43，通道 10） | 36 / 37 VCO A / B 的 **oct+3 / low**、38 / 39 效果器 **SELECT L** / **SELECT R**（每敲一下进一档，循环）；40–43 光敏头 drone 1、2、4、5 |
| 琴键 | 弹 VCO |

### SMC-Mixer

| 第 N 列 | 第 1–6 列 = drone 1–6 | 第 7、8 列 = VCO A、VCO B |
|---|---|---|
| 推子 | VOICE MIXER 里这路的 **VOL** | VOICE MIXER 里 VCO A / B 的 **VOL** |
| 编码器 | drone 1、2、4、5 的 **VOLT**；drone 3、6 的 **PITCH** | VCO A / B 的 **MORPHING WAVEFORM**（想要失真时其中一个换成 **DIST**） |
| Mute | **DRONE VOICES** 第 N 个键（开关） | 空 |
| Solo | **暂不绑定**；想绑时用：这路 drone 的 **HOLD** | **暂不绑定**；想绑时用：envelope A / B 的 **hold**（VCA 常开，VCO 变成持续音） |
| Record、Select | 空 | 空 |

Solo 行暂不绑定（业主，2026-10-17）：DRONE VOICES 的键点开就一直响，drone 的 HOLD 用处不大。号码照上表设好，以后要绑直接 Learn。

VOICE MIXER 的面板顺序是 DRONE 1、2、3、EXT.AUDIO、VCO A、VCO B、PREAMP、DRONE 4、5、6，和 SMC 的列序不同，Learn 时按名字点。

| 全局按钮 | 绑定 |
|---|---|
| Stop | **MUTE** |
| Rewind / Fast Forward | **CART PREV** / 卡带槽（下一个卡带） |
| Up / Down / Left / Right | **PRESET A** / **B** / **C** / **D**（载入后推子要重新接管） |
| Bank◀ / Bank▶ | **PROFILE SWITCH KEYS** 的 PREV / NEXT |
| Play、Record | 空 |

### 鼠标（设置型）

- **MASTER**（紧急静音用 Stop = MUTE）；
- **PAN**：先摆好。注意它不只是声场位置：拧左只进 FILTER L 和左效果器，拧右只进右边，中间各一半；
- envelope A / B 的 A、D、S、R；drone 的 ATT / RLS、内部各振荡器的 tune / mute / mod，drone 3 / 6 的 rate、divider、noise、FM / AM 等；
- VCO 的 tune、pw、pwm、-1 / sub、lin / exp、cv amt；滤波器的 MOD、GAIN、BP / LP；
- LFO A / B（只有接了线才起作用）；键盘、琶音、音序器设置。

## 5. 建方案的步骤

1. 管理工具里按第 3 节改 SMC，导出一份留底；在 BINDINGS 里核对 MPK 没用到这些号。
2. 新建方案（例如 `Live Start`），按第 4 节逐个 Learn。每学一个编码器来回拧一下，确认 MODE 是 REL。
3. 方案切换键在 BINDINGS 的 **PROFILE SWITCH KEYS** 卡片里学，不在方案表里；**CART PREV**、**PRESET A–D** 在 EXTRA LEARN TARGETS 里；下一个卡带、**MUTE**、DRONE VOICES 的键直接点面板上的卡带槽和按键。
4. 先在 App 里弹一轮，再进 Ableton。

## 6. Ableton Live 里

- Settings → Link, Tempo & MIDI：SMC 的 Control Surface 选 **None**，输入端口 **Track** 开、**Remote** 关（否则 Live 的脚本或 MIDI 映射会先拿走消息）。MPK 用白灯的预设（见 MANUAL_TESTS 第 17 节）。
- Lunar 24 所在的 MIDI 轨：MIDI From 选 **All Ins**（或两台各建一条空轨，MIDI To 指向 Lunar 轨），Monitor **In**。
- 方案库和 App 共用：App 里建好的方案在插件里直接能用。
- 打击垫是音符 36–43（C2–G2）。插件里不看通道，琴键弹到这几个音会触发打击垫的绑定、自己不发声；用 MANUAL_TESTS T15.15 的办法把打击垫单独走第 10 通道送进 Lunar 轨（MIDI To 选 `10-Lunar 24`）。

## 7. 已知的坑

- **蓝牙和 USB 是两个设备名**：绑定记着学它时的端口名（现在是 `SMC-Mixer Bluetooth`）。换用 USB-C 连接时端口名不同，独立 App 里这些绑定和方案切换键就不响应了（插件不看设备名，不受影响）。固定用学的那种连接方式，或者换了以后重新 Learn。
- **CC 1**：MPK 摇杆上推发 CC 1，方案里没绑它时会按出厂表动 FREQ L。不想要就把 CC 1 Learn 到一个想要的目标。

## 8. 以后可能加的

- **第二层**：建第二套方案，8 个编码器换成 8 路 **PAN**，或 LFO **RATE** 加几个 envelope 参数；用 Bank◀ / Bank▶ 来回切。代价：第二套里 MPK 和推子的绑定要重学一遍。等弹一段时间、真觉得不够再做。
- **按钮灯回发**：Lunar 24 打开 SMC 的 MIDI 输出口，绑定的开关一变就回发 127 / 0 点灯。先做独立 App；插件要开 MIDI 输出、还要在 Live 里把输出接回 SMC，更麻烦。业主说暂不考虑（2026-10-16）。
