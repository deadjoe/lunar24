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
