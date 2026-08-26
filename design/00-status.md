<!-- SPDX-License-Identifier: Apache-2.0 -->
# 实施状态（工程总监维护 · 断线可续接点）

**这份文件的用途**：@Claude（工程总监）随时可能因额度中断。中断时 @Pi **不要停**——
从这里读出"什么已批准、什么在做、什么必须等人"，按预授权继续。

**最后更新**：2026-08-26 · **代码 head** `4e8b1d4`（本文件自身的 docs commit 必然在其后，故此处永远记*代码*head，不是 tip）· 分支 `feat/p0-full-registry` · `origin/main` = `baf1e11`（PR #1 合入点）；本分支严格领先、merge-base==origin/main · 无 PR#2

## 1. 预授权（@Pi 不必等 GO）

可以直接做完再报复验：
- 已认过范围的切片实施
- 还已登记的债、修 bug、补测试/负控
- 文档与 FINDINGS 记录

**必须等工程总监或 owner**（三件，不可自行决定）：
1. 修改**冻结的 P0**（registry / manifest / id 空间）
2. **阶段出口裁决**（P0…P6 的 MET / NOT MET）
3. 与 `design/06-master-plan.md`、`design/07-core-contract.md` **冲突**的决定

## 1a. ⚠️ 工作顺序（2026-08-26 owner 指正后立的硬规矩）

**动手或裁决前的查证顺序：① 本项目 `design/` 文档 → ② `design/reference/` 原始资料（面板图优先于手册文字）→ ③ 才轮到自己推。**
顺序反了会出现两种错：辛苦论证出一个文档里早已写好的结论；或者更糟——推出与文档相反的结论。

**已发生的实例（都因跳过 ①②）**：
- `04 §2` 早已写死「每个插孔一根线，除非证实支持 stackable」→ 我却重新去手册找 mult 证据论证一遍。
- `04 §2` / `03 §3` 都写着「只暴露原面板真实存在的插孔」→ `fm_in` 违反了它。
- `01 §3` 我自己 8/23 从面板图点出的 VCO 控制件清单**本就没有 FM**→ 正确答案在自己的文档里躺了三天，我还批准往里加。
- `02 §四` 早已规定进度按**四维度**分报、第④项「音色相似度」写明"未开始需实测、不可用数字代替"→ 我却当新发现汇报。

**判定物理控件/插孔存不存在：面板图是权威**（`03 §3`：参考 PNG/PDF 是"测量依据"），**手册文字只是描述**——
描述会把"通过 cv+lin 实现的线性 FM"写成"有一个 FM 输入"，那不是错，是省略；照它建模就会多出一个孔。

**实机不可得**（owner 2026-08-26 明确：不可能提供 Solar 42N）⇒ `02 §四` 第④维度永远停在"未验证"，
provisional 数值按"待校"如实交付，**不再就此提问**。

## 1b. git 事实（2026-08-26 核实）

`origin/main` = **`baf1e11`**（"Merge pull request #1 from feat/p0-foundation"）。
本地 `main` ref = `1945878` 是**陈旧未更新**的——我和 @Pi 先前都把它当权威，**记错了**。
`git merge-base HEAD origin/main` == `baf1e11` == `origin/main` 本身 ⇒ 本分支**严格领先 127 个提交**、
PR #2 将是干净合并（无冲突）。

⚠️ **规矩**：问"main 动没动"时，**权威是 `origin/main`，不是本地 ref**；本地 ref 不 fetch 就永远停在旧值。

## 1c. 审查线与实施线的分工（2026-08-26 owner 指令 msg `58d9c337`）

**@Codex = 审查总监**（额度恢复后归队）。职责：读 `design/` + `design/reference/` + 仓库代码，
查**实施与设计的偏离**，写进 repo 的 `review/` 目录并开 GitHub issue，**按相关性成批**通知我，不在频道里发长分析。

**角色边界（owner 明令，不要越界）**：
- @Codex **不参与实施、不参与实施讨论**。
- @Pi **不看 Codex 的审查**，只听我的明确要求；@Pi 有问题找我，不找 @Codex。
- 我（工程总监）是**唯一接口**：Codex 的 issue → 我判断 → 我下达给 @Pi。

**我收到 Codex 通知后的动作**：读本地 `review/` 文档或 GitHub issue → 判断 →
① 属实且需修 ⇒ 开任务派 @Pi；② 与设计/实施有冲突 ⇒ **与 @Codex 讨论**，不擅自否掉也不照单全收。

### ⚠️ 两条实施线不得互相打架（owner 特别强调"这个很重要，你要记忆好"）

**线 A = 功能推进**（P5 → P6）；**线 B = 审查问题修复**。执行规矩：

1. **修复不插进正在做的切片**。@Pi 手上有 in_progress 切片时，issue 修复**排到切片边界**再下发，
   除非该 issue **正好使在做的切片作废**（那时立刻叫停，避免在错地基上继续堆）。
2. **各自独立任务号、独立提交**，提交信息注明 issue 编号；不把修复混进功能提交，否则出问题无法二分。
3. **一次只给 @Pi 一条主线**。同时压两批活会让他自行排序，那等于把调度决定推给实施工程师。
4. **修复同样要负控**：审查发现的 bug 修完，必须有一个"回到旧行为就红"的测试，否则等于没修。

**硬件不可实现项**（owner 第 3 点）：硬件上存在、软件不可能等价实现的部分，各设计文档已有记录。
Codex 报此类"偏离"时，先查是否属于**已声明边界**（§4）或 provisional（§3）——
**已声明的边界不是缺陷，不要当 bug 去"修"**；若确属文档未覆盖，才作为真问题处理。

## 2. 当前进度

| 阶段 | 状态 |
|---|---|
| P0 registry | ✅ 收口 |
| P1 跨平台技术切片 | ✅ 出口 MET |
| P2 控制时基与路由图 | ✅ 出口 MET |
| P3 固定声音核心 | ⚠️ **改判：模块达成，集成未验**（2026-08-26，见 §2l）——无产品运行时，`compile_graph` 零消费者 |
| **P4 演奏系统与输入适配** | ⚠️ **改判：模块达成，集成未验**（见 §2l）——三路等价只在测试内的 framework-free stub 上成立，host 无真实适配器、`PLUG_DOES_MIDI_IN 0`。各片实施细节见 §2c–§2i |
| **P5 整张面板** | ▶ **进行中**：①自撰宿主+真窗消费 fit ✅（task #31）／②唯一可逆 transform ⚠️ **部分达成**——①-④ 成立，**⑤ 坐标锚定不成立已改判**（见 §2k，task #36 修）／③按区域铺控件 **暂停**（task #34，等 #36） |
| P6 dual effector | 未开始 |

门禁基线：本机 ctest **38/38**（+ASan 22/22 内存错误零）；CI build-and-test **4/4 绿**；
`full coverage (--require-full)` **按设计红**（PR#2 merge 门，非回归）。

## 2b. 已裁决的冻结-P0 变更（2026-08-26）

**`DeviceStateV1.keyboard_presets` 由 8 字节壳扩容为承载 `keyboard_params_minus_clock` 全 payload。**
理由：冻结 manifest 已把 preset 定义成该 **31 参数** record（evidence L1040-1045），而 `device_state.h` 那个壳
自己写着「the remaining keyboard-owned state lands when the keyboard subsystem is designed」——就是现在。
把 payload 放到子系统自持（方案 B）会把一个概念劈成两套持久化 ⇒ 两个真相来源，且 P2-⑤ 只覆盖一半。

约束：**只追加不重排**／schema version +1／`totalBytesHint` 重算／**id-stability 必须仍绿（红＝动了既有布局，停下找人）**／regen zero-diff。
连带：P0 deferred-to-P4 的四个非标量已建模（`seq_steps`→`KeyboardSeq` 16×6B／`quantise_scale_editor`→u16 掩码／
`plate_tune`→float[12]／`pushbutton_value`→float[8]）。**已落地**：schema v1→v2、`kKeyboardPresetRecordBytes` 8→247、
`totalBytesHint` 3841→4979；id-stability 与 regen zero-diff 全绿（确认只追加未重排）。
preset 身份维持**槽索引派生**（A=0…D=3，顺序有据 L1040-1045、数值编码是我们的，标 PROVISIONAL），
并要求断言 `preset.id == slot` 钉住该冗余——**永远等于下标的字段最易被后人当成有独立含义**。

## 2c. split per-side 裁决（2026-08-26，msg `60df2e43`）

**split 下整份 keyboard 参数 bank 每侧一份**（唯一全局项＝`behaviour`，即 single/twin/split 选择器本身——
决定"是否存在两侧"的开关不可能每侧一份）。证据（**行号已逐条 `sed -n` 核过**）：
- **L710**：split ＝ "two sides, **each having separate parameters**"（无限定词）
- **L724**：例子 ＝ "having an **arpeggiator on the left side** and a simple keyboard on the right side"
- **L806**：split 下每侧可各自 **mode**

取窄子集＝我们自己挑哪些参数共享，**无证据支持**；原文无限定、例子横跨 mode 与 arp ⇒ 整份 bank 每侧一份是贴合证据的最小假设。

**连带**：preset 含 `mode` ⇒ **preset 也要装两侧**，schema v2→v3、`totalBytesHint` 再涨（约束同 P4-② Decision A）。
⚠️ **P4-④ 做 arp/seq 时必须按侧实例化**（L724 证明 arp 每侧不同），**不得做成全局单例**，否则又是重写。

✅ **更正（2026-08-26，msg `e7ad49ec`）**：我先前记"手册没有 twin 共享参数这句话"是**错的**——**原文存在**，
只是被竖排 `AMBIENT` 列头劈碎、且 "parameters" 断成 `parame-`/`ters`，所以**短语 grep 永远命中不了**。
原始碎片：L675 "…While the two sides" ／ L679 "share" ／ L681 "same parame-" ／ L677 "ters, they can be played independently."
⇒ twin=共享一 bank、split=每侧独立，**有原文直接支持**，不是反推。

🚨 **由此立的硬规矩（比这一句重要）**：在这份 `pdftotext -layout` 手册上，
**短语 grep 只能用来"找到"，绝不能用来"证否"**；要证否**必须分区通读原始行**。
理由：句子跨列碎裂、单词还会被连字符断开，`grep` 的"零命中"是假阴性。

✅ **两条承重结论已重核完毕（2026-08-26，msg `77a2218b`）——原判不变，但依据从"搜不到"换成正面证据**：
1. **用户 cable 每 output ≤1**：依据＝手册**目录**（L174-193）逐条枚举全部功能块，**其中没有 MULT/SPLITTER/UTILITY**；
   加上面板插孔行逐个列名、无可堆叠件。⇒「这台琴没有 mult」出自它自己的功能块清单。
2. **无反相门**：依据＝`OUTS VOLTAGE SPECIFICATION`（L151-161）逐条列死各输出电压范围，
   **所有门类输出一律单极正**（EG 0…8V／ENV FOLLOWER GATE 0…+8V／5 STEP SEQ GATE 0…+10V）；
   该表**确实会标双极**（JOYSTICK −10…+10、ENV VOICES −10…+10、PULSER −10…+10、S&H −5…+5），
   所以"门类全是 0…+V"是有意义的正面事实而非遗漏。
**诚实边界**：目录/规格表很强但非绝对 ⇒ 记为**"有正面证据支持"，不是"已穷尽证明"**。

💡 **由此得到的方法（比这两条本身更有用）**：**枚举型来源天然适合证否** ——
目录声明"这就是全部功能块"、规格表声明"这就是全部输出范围"，因此某项**不在其中**是信息，而不是搜索失败。
**要证否，先找有没有枚举型来源，再谈分区通读；短语 grep 永远不能证否。**

## 2d. per-side 存储裁决（2026-08-26，msg `695564a7`）

**preset（已落 head `405e450`）**：记录 247→487＝左半 247B **逐字节保留 v2** ＋ 右半 30 个 `_r` 字段；
schema v2→v3、`totalBytesHint` 4979→5939。绝对基址锚（`buf[247]`、`record==487`）与 round-trip **不重复**：
前者钉"右岸整体没错位"，后者只证相对一致。

**live 态**：非标量走 **L1**（v2 字段为左/基准、右岸追加 `_r`），**不做 L2**（复制 182B ＝ 双重真相来源）。

🔑 **标量：每侧一个值 bank，`ParameterId` 空间不动 —— 不得新增 `_r` ParameterId。**
依据是 **P5 出口硬约束**（`design/06` L121）：「控制清单 100% 有可见控件**且无新增控件**」。
面板上每个参数**只有一个物理旋钮**，split 下它编辑的是"当前选中侧"的值；新增 30 个 `_r` id 会让 registry 出现
**30 个没有物理控件的参数**，与面板证据直接矛盾、P5 台账必然对不上。
⇒ 模型＝**ParameterId 标识"哪个参数"（对应那一个旋钮），侧别标识"哪一份值"**。
⇒ **P4-③ 读参必须带侧别上下文**，不得直接读全局 `parameters[]`；`keyboard_mode` 地基先把该上下文定出来。

✅ 地基已落 `core/include/lunar24/core/keyboard_mode.h`（commit `e43411e`）：`KeyboardMode{Single,Twin,Split}`／
`KeyboardSide{Left,Right}`、`side_bank(mode,side)->0/1`、`sides_share_bank(mode)`、`read_side_scalar(bank,mode,side,id)`（读参唯一咽喉，
`id` 透传不变——即"无 `_r` id"）、`mode_from_behaviour(u8)`（PROVISIONAL：只验 totality+unknown→Single，不把 0/1/2 当证据）。负控证明：
临时让 Split 忽略 side → 测试 3/277 真红。

✅ **live-state 非标量 `_r` 已落**（schema **v3→v4**、`totalBytesHint` **5939→6121**，commit `06fc722`，@Claude L1 裁决 695564a7）：
`DeviceStateV1` 五个 live 非标量/无-域选择器各加 `_r` 右岸镜像（`keyboardSeqCurrentR`/`keyboardScaleEditorR`/`keyboardPlateTuneR`/
`keyboardPushbuttonR`/`keyboardClockSelectorsR`，+182B）；对应 `kDeviceStorageFields` 追加 5 条（versionFrom=4，**只追加不重排**）；
encode/decode 各加分支。测试：`fill_state`/`states_identical` 现覆盖全部 live 字段（左/右**不同值**，防共享-field 病），并新增绝对尾锚
`live_side_bank_is_tail_and_independent`（`_r` 块始于 field-table offset、尾触 `totalBytesHint`==6121、182B 连续性 + 左/右独立性）。负控：
把 `keyboard_seq_current_r` 编码成左 field → 6 处真红（本测试 4 + 既有 round-trip 2），撤后全绿。ctest **33/33 绿**（executable 数不变，内部 check 35→50）。

🔑 **开放 A/B（待 @Claude 裁，未动）—— live 标量 bank 的表示**：live keyboard 标量目前住在机器-wide `parameters[ParameterId]`。split-right 需要 bank[1]。两表示：
- **A 镜像全数组**：加 `double parametersRight[kDeviceParamCapacity]`。bank[0]=parameters（左/共享），bank[1]=parametersRight。id 直索引零映射，但 +3392B、~394 个非-keyboard 槽"可被编辑"（其实从不读）、易误读"右份有整套参数"。
- **B 紧凑 keyboard 标量 bank（我推荐）**：加 `float keyboardScalarRight[N]`（N=keyboard_params 中**带 ParameterId 的标量**，即 mode/arp×/seq×/portamento×/vibrato×/pressure×/quantise_load_scale/root_note/pressure_output，≈22）+ 一张 `ParameterId→index` 映射。bank[0]=`parameters[ParameterId]`（左/共享，**不变，无双重真相**），bank[1]=`keyboardScalarRight`。精确（只有 keyboard 参数按侧）、便宜、符合 L1"左在原 bit / 右在 `_r`"与 preset 的紧凑侧 bank。待定：`pressure_output` 是否也按侧（preset 已按侧 pressure_output@5+`_r`@247，但 live `KeyboardSettings` 目前把 output 当全局壳值）。

## 2e. live 标量 bank 裁决（2026-08-26）

**选 B（紧凑 keyboard 标量 bank），不选 A（镜像整个 `parameters[]`）。**
理由不是省字节：A 会让右岸凭空多出 ~394 个**非 keyboard** 参数槽（VCO/滤波/效果器…），
**这台琴分裂的是键盘不是整机** ⇒ A 存了一个不存在的东西，与 polarity 反相／bp_lp morph 同族。

**`pressure_output` 按侧**（它在冻结的 31 参清单内，`behaviour` 是唯一全局项）。

🔑 **由此立的不变量（比逐个参数拍板管用）**：
**同一参数在 preset 里按侧，在 live 里就必须按侧；反之亦然。**
否则 `load preset → live` 会丢掉右侧值，save/load 变成有损——而无损正是 P2-⑤ 持久化要保证的。
**负控＝某参数 live 全局而 preset 按侧 → preset→live→preset 往返丢右侧值 → 红。**
⇒ 以后新参数的按侧与否由这条自动对齐，不必逐个上报裁决。

✅ **已落地（2026-08-26，含本节裁决）：live 标量 bank（B）+ preset/live 按侧不变量**。
`schema v4→v5`、`totalBytesHint 6121→6297`（22×8=176B）。新头 `core/include/lunar24/core/keyboard_side_bank.h`：
`kKeyboardScalarParameterIds[22]`（mode…root_note，**不含 behavior=100** 与 4 个无-域 selector）、
`keyboard_scalar_index(id)->0..21/-1`（**不新增 `_r` ParameterId**）、`read/write_preset_scalar_pair`（按 id 开关 preset 两侧字段）、
`load/save_live_side_bank`（22 个 id 驱动 preset→live→preset 双岸往返）。f64 与 `parameters[]` 同型，`read_side_scalar` 可一种 Value。
`pressure_output` 按侧（labelled，负控即以此触发）。测试：`test_state_persistence`（fill/states_identical 覆盖 keyboardScalarRight
左/右不同值+绝对尾锚迁到 `keyboard_scalar_right` 为新 record tail）+`test_keyboard_presets` 新增
`live_scalar_bank_preset_live_round_trip`（绝对锚=bank0`parameters[id]`→左、bank1`keyboardScalarRight[idx]`→右，mode 与 pressure_output 双锚+idx-map
同时验逆+整体往返无损）。**负控（改名后真红 2/191）**：`load_live_side_bank` 临时跳过 `i==17`（pressure_output 右岸）
→ 该参 preset 按侧而 live 全局 → 往返丢右侧值（绝对锚 404 + presets_equal 417 红），撤后 191 全绿。ctest 33/33（ASan+UBSan detect_leaks=0）、
regression 4/4、regen zero-diff / id-stability / core_headers / spike_clean / evidence_refs / evidence_layout 全绿。main 未动，无 PR#2。

## 2f. 逐音行为（P4-③ 核心产出 · 未提交，@Claude Go `37db4aa5`）

**新头 `core/include/lunar24/core/keyboard_behaviour.h`** —— 待 @Claude 复验。四个逐音行为：pressure output modes/rise-fall、portamento、
vibrato、note quantiser（scale+root）。**合同：读参一律经 `read_side_scalar`（侧别咽喉），非标量 scale editor 走同一 `side_bank()` 解析的 `_r` 镜像；
全链路 `translate()` 喂 ControlEvent；只出控制信号（CV/gate/clock），绝不出音频。** translate() 无状态、行为有状态 ⇒ 新增一个**有状态组合引擎**
`KeyboardBehaviour`，吃掉 translate() 的 canonical ControlEvent、产出每样本 pitch/pressure/gate 控制信号——它**从不自己用原始输入造 ControlEvent**，
所以"单一解释咽喉"（design/07 §1）结构性成立。

**证据与实现：**
- quantiser =**离散、确定**（design/07 §3 语义 1）：`quantize_pitch(pitch, scale_mask, root)`，all-off→微音程直通，否则按 12-TET 就近吸附。
  `preset_scale_mask(0..18)` 覆盖 19 栏：7 modes/pentatonic×2/whole-tone/semitones 为**无歧义 12-TET 集合**；**8 个风格 scale（blues-major/minor、
  folk、japanese、gamelan、gypsy、arabian、flamenco）手册只命名不列音程 ⇒ UNRESOLVED（`kScaleUnresolved`），拒绝猜填。**
- root_note：norm 0..1→semitone 0..11（C..H）。**PROVISIONAL step 划分**（手册只给 C..H 名义，无数值）。
- portamento/vibrato：复用 `ParameterSmoother`（design/07 §3 不重造秒级 smoother）；legato=单音跳变/≥2 音滑行（手册 L910-923）。
- pressure OUTPUT 五模式（Pressure/Asr/Ad/Loop/Random，手册 L951-965）：Pressure=跟随+边缘 slew；Asr/Ad/Loop=ADSR；Random=确定性 LCG。
- **PROVISIONAL 边界（勿当证据）**：norm→秒/量的线性映射（`kPortamentoMaxSeconds=2.5`/`kVibratoMaxHz=15`/`kVibratoMaxDepthCv=2/12`/
  `kVibratoMaxDelaySec=2.5`/`kPressureMaxSeconds=2.5`）是**文档化线性上限**，待测后签（design/00 §5 "先量后签"）；ASR/AD/LOOP 的 sustain+peak=按下时 pressure 电平
  （手册只列模式不列段电平）；`root_note_semitone` split 名义。

**测试 `tests/core/test_keyboard_behaviour.cpp`**（46 检查，后加 scale-editor 绝对锚）。结构/不变式为主，不编精确曲线：
- quantiser **绝对锚**：Ionian@C → 精确 CV、微音程直通、chromatic 就近、root 平移。
- portamento：legato 判定 + 结构滑行 + **跨采样率不变式**（48k/96k 同 wall-clock 同值）。
- vibrato：周期结构 + delay ramp + pressure 深度缩放 + gate off 停。
- pressure：五模式路由、slew 无过冲、ASR/AD/Loop 形状、Random 确定性（同 seed 同值/新按新值）。
- **mandate-#4 负控（真红 3/46）**：`read_behaviour_params` 临时丢弃侧别（Split 右→一律读左 bank 0）→ `pressureRise` / `cpres≈0` / 发散三检查真红，撤后全绿。
  即"右岸音符读了全局 parameters[]"这一真实错误被咽喉钉死。附加绝对锚：scale editor 回调收到 `side_bank(Split,Right)==1`。

**门禁**：本机 ctest **34/34**（executable 33→34，+test_keyboard_behaviour）、ASan+UBSan detect_leaks=0（46 检查 OK）、CI build-and-test 同源；full-coverage 按设计红（PR#2 merge 门，非回归）。
main 未动，无 PR#2。**下一片 P4-④**：arp/seq 按侧实例化（设计/00 §2c `60df2e43`：L724 证明 arp 每侧不同，不得全局单例）。

## 2g. P4-④ 裁决 + 一个门禁洞（2026-08-26，msg `30f82596`）

**接入点**：arp/seq 夹在 `translate()` **之后**、`KeyboardBehaviour` **之前**
（`translate()` 输入是原始 `PerformanceInput`、arp/seq 产出 canonical `ControlEvent`，
"再进 translate()"类型上不通、概念成环）。⇒ translate() 仍是唯一"原始输入→事件"咽喉，
直弹音与 arp 音走同一行为链。

**时钟**：**全局 tempo（`clock_bpm`，preset 明确排除它）＋ 每侧分频**。
分频控件的**存在**有据（L827 ARP CLOCK／L875 SEQ CLOCK「clock multiplication/division ratio」），
但**档位值域手册一个都没列** ⇒ 按 8 音阶先例：**控件存在、值域 UNRESOLVED**，不扩 registry 凭空定义档位（选 B）。

🔴 **门禁洞（比 A/B 更要紧）**：冻结 manifest 的 `keyboard_params_minus_clock` 引用
`keyboard.arp_clock` / `keyboard.seq_clock` / `keyboard.arp_rhythm`，而这三个 **在 spec/registry 里不存在**
（四个同类名字只有 `keyboard_seq_rhythm` 落了）；`check_registry_complete` 只比 `mustComplete == landed`，
**不校验 storage schema 引用的 stable_id 是否解析得到真参数** ⇒ 照样全绿。
⇒ **要加门禁**：遍历所有 storage schema 的参数引用，断言每个 stable_id 都解析到 registry 参数；
负控＝故意引不存在的 id → 必须红。**先让门禁看见它，再决定是补参数还是改引用（先量再决定范围）。**

⚠️ 我的措辞教训：我说 `arp_clock`「在按侧集内」——**对 manifest 成立、对 registry 不成立**，
而我没把两者分开说。**"在不在"必须指明在哪个产物里。**

**✅ 门禁洞已闭合 + 无域 selector 落 homes（head `4e8b1d4`，@Claude msg `82a77317` 定案=manifest 声明 home、门禁一条通用规则）：**

- **新增 manifest `positionlessSelectorDisposition`**（冻结改动，**只追加**）：4 个无域 selector
  （arp_clock/arp_rhythm/seq_clock/seq_rhythm）各标 `{belongsTo: deviceState, status: deferred-to-P4}`。
  值域 **UN-EVIDENCED**（B 方案，未凭空造档位），仅控件存在性有据（L827/L875）；值结构性落在
  DeviceState 字节数组 `keyboardClockSelectors[4]`。`mustComplete` 保持 345 不动、无 id 空间改动、
  无既有条目改动、regen zero-diff（只追加，门禁成立）。
- **门禁**：`validate_root_a_disposition` 重构为**一条通用** `validate_gap_disposition(disposition, computed, label, schemas)`，
  参数化 `label` —— 门禁**永不硬编码某个容器/字段名**（它是数据，不是门禁逻辑；msg `82a77317`）。
  原 Root-A 项与新的 positionless 项走**同一条规则**：声明的 gap 集合与计算 gap 集合**双向精确匹配**，
  缺/盗/坏全红。
- **新 always-on `validate_schema_param_resolution`**：storage schema 的 `params`（或 record 的 `excludes`）
  成员，必须解析到**真实 registry 参数**或**声明 home**。声明 home 集 = 遍历 manifest 所有以 `Disposition`
  结尾的 key 取 items（**数据**），绝非从门禁对参数形状的分类重推。两者皆非 → **真悬空引用**
  （纸上声明、未实现、未归家），消息即此。
- **负控（mandate #4 真红）**：`mi` 删 home 声明但**留 schema 引用** → 解析判据红（`resolves to neither a registry
  parameter nor a declared home`）；`mj` 往 disposition 塞 stale id → 双向判据红（`not a current positionless-selector gap`）；
  `mk` 把 status 打成 `landed` → 非法状态红。已实测 mi 确因**解析**判据红（另一条"缺 disposition"判据也红，但断言钉的是解析那根针）。
- **量**：恰好 **4 个**真悬空（正是这 4 个无域 selector）；8 个 Root-A 全绿（经 Root-A disposition 归家，无误报）⇒ 不用扩 registry。

**门禁**：本机 ctest **34/34**、ASan 21/21 detect_leaks=0、negative exit 0、regen zero-diff、id-stability、core_headers 88、
spike_clean 42、evidence_refs、evidence_layout、registry_negative 5 夹具全绿；`--require-full` **按设计红**（12 个声明 gap，
PR#2 merge 门，非回归）。main 未动，无 PR#2。**下一片 P4-④ arp/seq 按侧实例化**（§2c `60df2e43`：L724 证明 arp 每侧不同，不得全局单例）。

## 2h. P4-④ arp/seq 按侧引擎（code head `4c129d2`，@Claude mandate `8b19b72a`）

**新头 `core/include/lunar24/core/arp_sequencer.h`** + **测试 `tests/core/test_arp_sequencer.cpp`**（44 检查，CTest #34）。
§2g 接入点落地：arp/seq **夹在 `translate()` 之后、`KeyboardBehaviour` 之前**，是**事件转换器**——吃 canonical `ControlEvent`、出 canonical `ControlEvent`，**绝不出音频**；
**无全局单例**（每侧一个 `ArpSeq`，L724 证据：split 时左可 arp 右可键盘）。

**模式 mux = `keyboard.mode`（id 101）**：`[keyboard/arpeggiator/sequencer]`，**≠** id 100 `keyboard.behaviour` 的
Single/Twin/Split 值选器（→ keyboard_mode.h `mode_from_behaviour`）。`arp_seq_mode()` 防御解码，未知原值→keyboard（不凭空 arp/seq，同 P4-③ 纪律）。

**引擎**：
- **全局 tempo 共享**（`clock_bpm` id 129）；每侧分频档位**读原值但不数值应用**（UN-RESOLVED=FINDINGS；§2g 已把 4 个无域 selector 声明 home 归 DeviceState）。
- 每 incoming `clock` 边推进**一步**，**名义 1:1**。分频比/rhythm 图案 UN-RESOLVED（手册 L827 ARP CLOCK／L875 SEQ CLOCK 只给"multiplication/division ratio"不给档位，同 TUNE/oct_sel 先例）。
- **arp**：press 板（`pitch` 事件；gate_on 不携音高身份）建 chord（LIFO 栈，gate_off 弹出）；一 clock 一音；**note 排序 pitch-ordered PROVISIONAL**（手册"sequence number of pressed plates"UN-RESOLVED）；HOLD 跨 release 保持 chord。
- **seq**：每 clock 推进一步，转调=持板基音（C=0V），`seq_len`（2..16，PROVISIONAL）回绕；`seq_cv_output` 定 gate 模式（gated→跟随 step gate 位／continuous→运行即常高）。
- **值映射 PROVISIONAL linear 上限**（design/00 §5"先量后签"，非证据）：arp interval 1..12／arp_len 1..8／seq_len 2..16／rhythm_len 1..8。

**side 读咽喉（mandate #4 负控真红）**：`read_arp_seq_params` 经 `read_side_scalar` 读每侧标量（mode/arp×/seq×），
非标量 side paths（steps+clock selectors）走 `side_bank()` 镜像——**product-side 咽喉**，钉死"split 右岸读全局/左 bank"真错误。
**负控 `side_drop`**：左 bank mode=0（keyboard）右 bank mode=2（sequencer）+ 更长 step；conforming reader 解右 bank、rogue 读 bank0 →
mode/seq_len/gate 计数四处真红，撤后全绿。

**无全局单例负控（@Claude msg fae330d9 指出原负控同音高=测不到全局单例）**：原 `per_side_instantiation_independent` 两侧 `note_on`
用**同一音高**（都 0/12），`chord_` 若静态共享则两侧 index0 写同值、污染不可见（44 checks 全绿）。**修法=拆两腿**：
Leg A 保留 "左 arp 右键盘直通" 的模式路由；**Leg B 两侧都 arpeggiator、持不同音**（左 C=0V 右 G=+7 半音）→ 共享 `chord_` 时右写 index0 变 G、
左下一 clock 读出 G 发 G+interval（正确应 C+interval）→ **@Claude 突变 `chord_` static inline → 2/47 真红**（`rl.near(rl.pitchAt(0), 0.0+i0)` 312、
`!rl.near(...,rr...)` 316），撤后 47 绿。**教训（本片第三次同类"用不能暴露错误的输入去测=没测"）**：状态隔离测试须让两侧**同时写入**且持**不同**值。

**门禁**：本机 ctest **35/35**（executable 34→35，+test_arp_sequencer）、ASan **22/22** 内存错误零（macOS 无 LeakSanitizer）、
regen zero-diff / id-stability / core_headers / spike_clean / evidence_refs / evidence_layout 全绿；`--require-full` 按设计红（12 个声明 gap，PR#2 merge 门，非回归）。
main 未动，无 PR#2。**下一片 P4-④**：显示+encoder+校准（⑤）。

**core_headers 门禁洞已闭（@Claude msg a24777d2 → 我选 c，head `25b48e0`）**：手维护的 `SKIP_DIRS={".git","build",third_party}` 只跳了 `build`，
本片加进 `.gitignore` 的 `build-asan/` 未被跳→门禁扫描 CMake 生成物、本地红 CI 绿（build-asan gitignore 后 CI 不碰——反向洞：CI 绿本地红）。
修法=c：门禁扫描范围改为 **git 派生**（`git ls-files`），因为"源码"的权威定义=项目 tracked 的文件；untracked 的 build 废料（build/、build-asan/、未来 build-cov/）定义上非源码，
任何 `.gitignore`/build 目录名都不可能再让门禁去扫生成物。`SKIP_DIRS` 整表退休——同一漂移类（build-asan 咬人）不可能再发生；与 manifest home 集用 `validate_gap_disposition`
同一"权威来源而非手写清单"原则。从干净状态复验：ctest 35/35、门禁扫 **90** 个 tracked authored file（88→90，+arp_sequencer.h＋test_arp_sequencer.cpp）全绿；
负控仍真红（tracked 无 SPDX 的 `_neg_gate.h` → 1 文件 fail，撤后绿）——重做未削弱对"应覆盖文件"的检查。

## 2i. P4-⑤ 显示+encoder菜单+输入归一化/输出校准（P4 最后一片，@Claude GO msg `87862933`）

**四新头 + 两测试**：`keyboard_display.h`（Decis ① 纯数据显示模型）、`keyboard_input_normalization.h`+`keyboard_output_calibration.h`（Decis ③ 两层分测）、
`keyboard_menu.h`（Decis ④ 全菜单项），测试 `test_keyboard_calibration.cpp`（48 检查，CTest #35）+ `test_keyboard_menu.cpp`（778 检查，CTest #36）。
ctest **37/37**（executable 35→36，+2）。

**层分成两层（Decis ③，写进注释）**：**输入归一化＝FRONT**（`translate()` 咽喉前，把因源而异的 raw 收敛成**唯一内部 pressure**——出口判据唯一允许差异之处，因为差异在归一化前被吃掉）；
**输出校准＝BACK**（咽喉后，把已收敛的内部值缩放到物理插孔电压）。**一前一后、不在同一层**——日后若有人以为"校准"能解释源间差异，那是错误：源间差异必须在咽喉处消掉。
**两测试独立**：FRONT 测 hysteresis（touch 650/release 690 双阈值，证据backs）+raw→pressure 线性映射；BACK 测 trim band（±kOutputCalTrimRange）单调 + pressure clamp 0..8V。绝不混层。

**Decis ② MPR121/debounce 不建模**：仍存进 parameters[]（能改、能存、随 preset 往返），但**无运行时效果**——与 4 个无域 selector 同等待遇，说明写进注释+FINDINGS。

**Decis ④ 菜单全做（不只有 preset+校准）**：`kMenuItems[]` 覆盖 Main/Arp/Seq/Preset/Calibration 五页。**reachability 测试**断言 22 个 per-side 标量全可达、
12 个无面板旋钮的全局（clock_bpm+校准/debounce/encoder 130-140）全可达、4 个无域 selector（slot 0-3）全可达、Main 页能进 Arp/Seq/Preset 子菜单、
Calibration 页非空（hold-while-boot 门）。只做"preset+校准"会把一整批无旋钮参数（vibrato/portamento/arp×/seq×）留成无可达路径，P5 完整性门一验就炸——测试钉死这一点。

**side 咽喉写路由（Decis ④ 负控真红）**：`write_item_value` 对 PerSideScalar 一律走 `write_side_scalar_value`（**同 MIDI/live 路径**），split 右岸绝写全局 `parameters[id]`。
**正控 `menu_path_and_midi_path_converge_split_right`**：菜单写 arp_hold=3.3（split right）与 MIDI 写同值 → 都落 `keyboardScalarRight[idx]`、`parameters[id]` 为 0 → **两表面收敛**（这就是出口判据要看到的）。
**负控 `wrong_bank_menu_write_diverges_and_is_detected`**：若菜单写全局而非当前侧 bank（@Claude 点名错误）→ `parameters[id]`=5 而 `keyboardScalarRight[idx]`=0 → 与 MIDI 路径**分叉**，测试断言分叉可检出（buggy 路径 != correct 路径）。
**门禁洞点**：`menu_item_is_scalar` 原本把 ARPEGGIATOR/SEQUENCER 两个子菜单行（都带占位 id 0/sub 0）误判为标量 → 无重复控件守卫红（1/855）→ 修法=子菜单行非标量（`if(i.submenu) return false`），撤后全绿。
**教训**：子菜单入口不是可编辑标量，duplicate-control 守卫必须跳过它，否则两行同占位 id 被误报为双控件。

**FINDINGS（不猜填）**：pressure 映射曲线=linear 占位、输出 trim=仿射占位、encoder step 每项=占位（注明 PROVISIONAL）、dac_vref 只作 context 传递（无证据电压域效果就不改电压）、
A/R 秒=registry norm 是 UI mapping。**校准多点 0/2/5/8V 分段不可表示**：冻结 registry 每输出只有一个标量（calibration_v_oct/pressure），不扩参数则 4 点分段装不下 → FINDINGS。
**门禁**：本机 ctest **37/37**、core_headers **90** tracked file 全绿（+keyboard_display/menu/input_normalization/output_calibration.h + 两测试）、regen zero-diff / id-stability / evidence_refs / evidence_layout / spike_clean 全绿；
`--require-full` 按设计红（12 个声明 gap，PR#2 merge 门，非回归）。main 未动，无 PR#2。**P4 全片收官**，等 @Claude P4 出口裁决。

## 2j. task #15 的还款方式（2026-08-26，msg `52a001a3`）

⚠️ **我原 mandate 的前提是错的**：我说"改宿主 height clamp"，但**仓库里没有任何产品 host/建窗源码**
（`git ls-files` 只有 core/、generated/、spike/、tests/）。那 141px 是当初靠**改 pinned iPlug2** 在 spike 里量出来的，
而改上游正是被禁止的实现路径。**没有宿主可改。**

**决定**：不为修一个 clamp 提前搭 host 层（那等于把 P5 的活乱序做）。改为先落
**框架无关几何模块**（`core/.../host_window_fit.h` + 测试）：输入 design 空间＋可用逻辑区域＋retina
⇒ 输出 fit drawScale 与"某控件 design 矩形是否垂直可达"；**141 不得写死**（它是本机 visibleFrame 算出来的，
写死＝把这台机器当规格）；负控＝退回 clamp（drawScale=design 让 WM 裁）→ 底部控件越界 → **必须红**。

🔑 **但这一片不关闭 #15**。模块交付的是**决策逻辑＋可达性谓词**；债的原文是"底部 141 逻辑 px 不可达"，
**真正可达与否要等 P5 宿主真开窗才成立**。
**理由＝我们栽过的那一次**：P2-③ 的 `real_path` 决策层验过、执行器没兑现，off-by-one 一直活到债到期才被抓。
**决策模块正确 ≠ 消费方照做。**

✅ **前驱片已落（`ccdb3b2`）**：`core/.../host_window_fit.h` + 27 检查。我用比他负控更贴近现实的突变验过——
把 `min(sx,sy)` 改成**只取 `sx`**（宽度 fit、高度交给 WM 裁，现实中最易写出的版本）→ **exit 1、10 条 FAIL**。
`141 = 1551−1410` 是算出来的非硬编码；retina 保持独立乘子未折进 drawScale（④ 那个 bug 的根因）。

▶ **P5-① mandate 已发（msg `07af6c26`）＝自撰宿主层 + 真窗口消费 fit**，也是 **#15 的真正偿还**：
   按 ①b 自写 bootstrap（SWELLAppController/setDelegate/ONLOAD+LOADED/建窗），iPlug2 保持未修改固定 commit；
   host 代码**不进 core/**（framework-free 门禁须继续绿），新起顶层目录并说明门禁如何覆盖。
   必测：①宿主必须消费 `compute_host_window_fit`（绕开→红）；
   ②**#15 偿还判据＝真实窗口下面板底部那行控件底边在可见逻辑高之内**（这条过了 #15 才关）；
   ③跨 7 档缩放+fit 下逻辑坐标不变（复用 ④ 判据）；④iPlug2 pin 未被修改要有**机械检查**非自觉。
   范围外：控件绘制/命中区、色彩材质、品牌图形（P5-② 之后）。

⇒ **写进将来的 P5 mandate**：宿主层**必须消费**该模块定窗高；**负控＝宿主自己算尺寸绕开它 → 必须红**；
**#15 在"真实窗口下底部那一行控件可达"被实测证明后才关闭**。

## 2k. P5-①/② 裁决（2026-08-26）

### P5-① 自撰宿主 + 真窗消费 fit（task #31 → done）
**#15 的偿还是实测的，不是 skip 换来的。** 我没用 @Pi 的数，自建独立探针（CGWindowList）量真窗口：

```
FIT（默认）      Lunar24Host  2181 x 1410
CLAMP（强制）    Lunar24Host  2400 x 1410
```
`2400 × (1410/1551) = 2181.82` → 实测 **2181**。**判别柱是宽不是高**——两种情形高都是 1410，只看高分不出来。
fit 时面板高 `1551 × 0.909 = 1410 ≤ 可见 1410` ⇒ 底行可达，那 141px 不再被裁。

**当时立的规矩（仍在效）**：测试可以在无显环境 loud-skip（环境事实），但 **#15 不能靠 skip 关闭**——
一条"在 CI 永远 skip"的 E2E 若被当成偿还依据，等于用一个从没运行过的测试关掉一笔债。**skip 不是 pass。**

### ⚠️ P5-② 改判（2026-08-26，裁决 msg `0843a838`）—— criterion-5 不成立

**@Pi 在 P5-③ 前置测量中顶回来，属实。我复测确认，并收回我自己那部分裁决。**

`panel_layout.h` 的区域矩形与面板图**大面积错位**。我独立量的真值（分离列检测，非目测）：

| 区域 | 实测 | 旧值 |
|---|---|---|
| VOICE MIXER | `x807-1592, **y536-711**` ⚠️见下方订正 | `{416,880,1984,1035}` x/y 全错 |
| ENV A / ENV B | `x807-1146` / `x1248-1591`, **`y711-880`** ⚠️见下方订正 | `{602,880,2101,1030}` 全错 |
| VCO A / VCO B | `x415-805` / `x1598-1985` | 被并进一个跨 mixer 的大矩形 |
| 下排六卡 `y≈880-1030` | LFO A 19-292｜JOYSTICK 296-596｜5STEP 600-1546｜PREAMP 1549-1723｜ENV FOLL 1727-2103｜LFO B 2104-2377 | **六条 x 全错**，且 joystick/5-step 调序，y 下界误作 1080 |

**为什么我的复验没抓住（根因，不是借口）**：criterion-5 的 zone 表与 region 矩形**同源** ⇒ 它只能证明
「标签与测量互相自洽」。我那次突变（改标签）证明了**不一致会红**，**没有**证明**一致 ⇒ 正确**。
**两者一起错时它永远绿。**

⇒ **这正是本文件 §5 早写着的「不变性是必要非充分，错得一致的实现照样全绿，关键路径要有绝对锚」——
我写下了这条，然后踩了进去。** 判据自洽 ≠ 判据正确；**同源的两个量互相印证，等于没有印证。**

**出口改判**：P5-② **①②③④ 仍成立**（变换数学，突变验证有效，不受影响）；**⑤ 不成立**。
**P5-② = 部分达成，待 ⑤ 重验**（task #36）。task #34（P5-③）**暂停**，地基修好再解冻——
错的 region 矩形上铺 283 个控件＝把错误放大 283 倍。


#### ⚠️ 订正：我上面给的中带 y 值也是错的（2026-08-26，裁决 msg `9c660e3d`）

**我在 §2k 里给的中带数字是目测带状裁图得来的，不是量出来的。** 机械核验（逐边检测图上是否真有卡框）结果：

| 我原写 | 实测 | 差 |
|---|---|---|
| VOICE MIXER y0 = 560 | **536** | 24 |
| VOICE MIXER y1 = 700 | **711** | 11 |
| ENV A/B y0 = 700 | **711** | 11 |
| 底部 12 触摸片 y0 = 1104 | **1068** | 36 |
| 右下 DRONE VOICES `{1700,1104,2400,1492}` | **x2126-2304, y1070-1468** | 四边均无边框，且与触摸片区重叠 |

**下排六卡四条边全部精确命中，那批是对的**（分离列检测得来）。

**这件事真正的教训（比数字重要）**：@Pi 把 §2k 的数字抄成了 `kExternalAnchors`，rect 也照同一批数字填 ⇒
**「rect 收敛到 anchor」恒真**。上一轮是"他与他自己自洽"，这一轮变成"他与我自洽"——**判据强度没有提高**。
注释里写 "FIGURE TRUTH / a different measurement source" **是断言，不是事实**。

⇒ **立规矩：锚不得由人转录。** anchor 必须由**提交进仓库的生成器**从 `solar42N_panel_2400px.png` 直接量出来
（比照 `tools/build_evidence_layout.py` + `generated/…json` 先例），配 **regen zero-diff** 门禁 + **改一个生成值必红**的负控。
**出处必须是那张图，人（包括我）不在链路里。**

### P5-② 唯一可逆 transform + 命中测试（task #33，①-④ 成立；⑤ 见上方改判）
head `7e1fa5b`，ctest **42/42**（我自己跑的）。**两条突变都是改真实现，不是改副本**：

| 突变 | 结果 |
|---|---|
| `screen_to_design` 改成自算 `bx / drawScale`（不走共享逆变换、丢 origin 与 retina） | **40 FAIL**，含 `hit == 0` 与命中中心偏移 |
| `右中 DRONE 6` → `左上 DRONE 6` | 红：`CONFLICT [左上 DRONE 6] measured center (2186,720) outside zone` |

第二条证明区域标签守卫**不是同义反复**：测量坐标与标签是两个独立来源，对不上就报。

**@Pi 主动划的两条边界，记下别当 bug 修**：
- 粒度是**区域级不是每控件像素级**（17 个区域测出来 ≠ 面板已还原）。
- 中部横带与 VCF/effector 带 y 向**重叠是物理面板本来如此**，不是测量错误。

### P5-③ mandate（msg `b922490a`）的关键要求
出口条件前半句「控制清单 **100%** 有可见控件**且无新增控件**」⇒ **必须双向测**：
registry→矩形（不漏）＋ 矩形→registry（不多），**两个方向各配一个负控**。
**只测"不漏"会放过多画出来的东西——fm_in 那次就是单向思维的代价**（多记了一个面板上不存在的孔）。

## 2l. ⚠️ P3/P4 出口改判 + Codex 审查首两批裁决（2026-08-26，msg `8a2d4c4e`）

**@Codex（审查总监）首两批共 7 条，我逐条裁决，没有一条按伪阳性打回。**

### 我改判自己的裁决：P3/P4 由 MET → 「模块达成，集成未验」

**事实我自己验的**（不是转述 Codex）：
- `compile_graph` 在 `core/`、`host/` 里**零消费者** —— 只有头文件自身 + `tests/core/` 两个测试。
- `host/plugin.cpp` 的 `ProcessBlock` 输出静音/直通，注释写明「no audio is produced」。
- `host/config.h`：`PLUG_DOES_MIDI_IN 0`。

P3 出口原文（`06` L105）：「除 dual effector 外的整条信号链**可演奏、可跳线**、四逻辑输出正确」。
**没有可执行路径时「可演奏」不可能为真 ⇒ 我当初判 MET 判早了。**

**但我不接受 Codex 的一条必修项**：他要求真实 host 面板/MIDI 适配器。**那会让 P3/P4 依赖 P5**
（`06` 把整张面板绑定放在 P5），**倒置计划顺序**。
⇒ 缺的不是面板 UI，是 **framework-free 的机器运行时**（sources → 图执行器 → 四输出，可编程驱动）。
这不依赖 P5，且正是「可演奏、可跳线」在 core 层的含义。**验收必须跑产品路径，不接受测试内自建的第二套执行器。**

### 逐条裁决

| GH | 结论 | 要点 |
|---|---|---|
| #3 (A02) snapshot 回收竞态 | **CONFIRMED，最高优先级** | `snapshot(slot)` 返回引用但 reader 无法钉住；`recycleOne()` 直接 `snapshots_[slot]=Snapshot{}`。audio 线程 data race。**我 P2-⑤ 复验漏了**——当时突变的是 mock `fs_write`，从没让 reader 跨越 publish+retire+reclaim。 |
| #4 (A03) 无产品运行时 | **事实属实，部分采纳** | 见上方改判 |
| #5 (A04) classic drone 占位 | **部分成立** | 成立：波形是 `std::sin`（手册为**锯齿**）、随机 20-2000Hz **无 low/med/high 角色**、TUNE/MUTE/MOD/VOLT/mutual-FM **无绑定**。<br>**不成立**：`06` 原文「negistor 非线性**从首个可听版本预留**」——**"预留"就是留缝**，`nonlinear_` 恒等缝**字面符合计划**，不算违约。 |
| #2 #6 #7 #8 | 接受，按证据修 | 每条必须先有"回到旧行为就红"的复现测试 |

### 由此定的三条通用规矩（对所有审计修复）
1. **先写复现测试让它红，再修**。没先红的修复不认——无法区分"修好了"与"根本没触发过"。
2. **验收跑产品路径**，不接受测试内自建的第二套执行器/适配器。
3. **修复与功能分开提交**，提交信息带 GH issue 号。

**任务**：#37–#43（顺序即优先级）。主线顺序：**#36（P5-② anchor 生成器）→ 审计修复 → 再回 P5-③（#34）**。
**理由与 panel_layout 那次相同：地基被审出问题时，继续在上面铺面板是重复同一个错误。**

## 3. 未解的证据冲突（provisional，不阻塞实施）

按"查不出来是合法结论"处理：实现按冻结 registry 走，冲突标 UNEVIDENCED 留在 FINDINGS，
**不许为关闭条目编造数字**。等 owner 提供高清面板照 / 服务手册 / 实机后再定。

1. **TUNE 范围** — 手册 L393「over one octave」 vs registry `±1 oct`
2. **oct_sel 档数** — 手册 L391「+3 / low」（2 档） vs registry 3 档（中间 `"0"` 无出处）
3. **六波形中"传统"那四个** — 手册未枚举，AS3340 惯例为推断
4. **FM 深度真实范围** — 手册只给形式（linear FM + attenuator），无常量

## 4. 已声明的边界（不是债，不要当待办去"修"）

- driver → 物理插孔的路由未证（只验到 adapter 填进 buffer 的 stride/interleave）
- 掉电持久性未证（只验 temp→flush→rename 顺序与中断可见性）
- CI 验不了「指纹 == 现册」（手册 gitignored，不进库）
- `test_coreaudio_output` 在 CI 永远 SKIP（无设备），不构成 CI 覆盖
- `landedDescriptorFacts` 缺 cathedral.1/magic.1 六枚叶 → 该六枚无独立转写交叉核对
- 混叠只测量未修（数字与参数出处记在 FINDINGS，留 P6/后续按证据决定）

## 5. 工作纪律（历次栽过的坑，续接时照此办）

- 报告先给**结论 + 证据 + 下一步**；方法论不写长篇。
- **负控必须真会红**，且要代表**真实会犯的错**，不是为触发判据而构造的。
- **不变性是必要非充分** —— 错得一致的实现照样全绿，关键路径要有**绝对锚**。
- **容差由测量能力决定**，不是由"让它过"决定；**先量后签**。
- **没有出处的数字不得驱动设计**；字段"被读了"不是目标，"行为符合证据"才是。
- **EvidenceRef 行号承重**，写前 `sed -n` 开一眼；手册按 `split('\n')` 计行（含 28 个 `\x0c`，
  `splitlines()` 会多出 27 行）。
- **不注册 = 看不见**；只在 CI 跳过的检查也要注册并**响亮跳过**。
- **债要带还款点，到期真去还**（债 2 还款时抓出 executor off-by-one）。
- **完整性判据必须双向**：「都有」与「不多」是两条独立命题，只测前者会放过凭空多出来的东西。
  （2026-08-26 实例：fm_in —— registry 里记了一个面板上根本不存在的插孔，单向检查全绿。）
- **管道会吞掉退出码**：`cmd | tail` 的 `$?` 是 `tail` 的状态。量门禁真假时不要带管道。
- **突变测试的突变本身必须是合法程序**：触发 UB 的突变会让 `-O1` 把程序整个优化没（零输出、exit 0），
  那个"没红"和"判据无效"**长得一模一样**，会把人引向反向的错误结论。
  ⇒ 突变后若见到"零输出/异常通过"，**先怀疑突变自己**，再谈判据。
  （2026-08-26 实例：给 `read_side_scalar` 合成 id 时用了 +1000，越过 `bank[2][128]` 边界 → 程序被优化掉；
  换成 +1 后立刻红 3 条，判据其实一直有效。）
