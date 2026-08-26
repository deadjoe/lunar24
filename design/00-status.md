<!-- SPDX-License-Identifier: Apache-2.0 -->
# 实施状态（工程总监维护 · 断线可续接点）

**这份文件的用途**：@Claude（工程总监）随时可能因额度中断。中断时 @Pi **不要停**——
从这里读出"什么已批准、什么在做、什么必须等人"，按预授权继续。

**最后更新**：2026-08-26 · head `28fc1d4` · 分支 `feat/p0-full-registry` · main `1945878` 未动 · 无 PR#2

## 1. 预授权（@Pi 不必等 GO）

可以直接做完再报复验：
- 已认过范围的切片实施
- 还已登记的债、修 bug、补测试/负控
- 文档与 FINDINGS 记录

**必须等工程总监或 owner**（三件，不可自行决定）：
1. 修改**冻结的 P0**（registry / manifest / id 空间）
2. **阶段出口裁决**（P0…P6 的 MET / NOT MET）
3. 与 `design/06-master-plan.md`、`design/07-core-contract.md` **冲突**的决定

## 2. 当前进度

| 阶段 | 状态 |
|---|---|
| P0 registry | ✅ 收口 |
| P1 跨平台技术切片 | ✅ 出口 MET |
| P2 控制时基与路由图 | ✅ 出口 MET |
| P3 固定声音核心 | ✅ 出口 MET（2026-08-25） |
| **P4 演奏系统与输入适配** | ▶ **进行中**：①统一输入状态机+三路等价 ✅（task #28，已复验）／②**preset 状态** ✅（schema v2、247B/槽、totalBytesHint 4979；裁决 `6a366ebb`，复验 `df7c9202`）／▶ ③逐音行为（下一片）／③逐音行为／④arp·seq·clock／⑤显示+encoder+校准 |
| P5 面板 / P6 dual effector | 未开始 |

门禁基线：本机 ctest **32/32**；CI build-and-test **4/4 绿**；
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
