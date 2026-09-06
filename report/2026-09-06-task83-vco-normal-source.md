# task #83 — 修复默认 DRY B 锁死：纠正 A→B 归一化信号来源（GH #18）

- **方向（@Codex 4fe298c8）：** 路线方向 A。只改 RouteId 4（`route.vco_b_vco_out_to_cv_in`）的**来源**，从旧的自环 `vco_b.vco_out` 纠正为**已存在的** VCO-A 发布信号 `vco_a.dry_out`。
- **唯一授权变更：** 纠正 RouteId 4 的来源为 `vco_a.dry_out`。**绝不做**：改默认 cvAmt / lin_exp / baseHz、给频率加下限、新增插孔、改 B 的 OSC 公开输出 owner（`vco_b.vco_out` 保持不动）、改 stable_id（保留 `route.vco_b_vco_out_to_cv_in` 名字，保留 RouteId 数值 4）。
- **硬规则：** "先红后修"（先写会红的产品判据 test，确认在 buggy 代码上 RED，再修）。

## 1. 缺陷（审计 N-1）

预修复的 route id4 以 `vco_b.vco_out → vco_b.cv_in` 为来源，构成 **VCO-B 自环**（B 把自己的 VCO 输出灌进自己的泛型 CV 输入）。线性泛型 CV 传递 `p *= (1 + cv*cvAmt)`，默认 cvAmt 下：cv = −1 时该系数为 0 → 音高为 0 → 相位锁存 → 默认 DRY B 停在 −1 V 直流。这就是审计 N-1 的"默认 DRY B 锁死"。

## 2. 修复（纯来源，零运行时改动）

来源改为 `vco_a.dry_out`，构成**无环 A→B 边**：`vco_b.cv_in` 在同一帧读取 A 的**发布值**（`resolveSinkValue_` → `cvAt_(src)`），不再读自身。运行时**无需任何改动**——机器运行时已经能正确处理无环 A→B（`sourceOfSink_` / `resolveSinkValue_` / `cvAt_`，`feedbackCount()==0`）。因此 `machine_runtime.h` **零改动**（git diff 确认为 0 处）。

单事实更改，同步写在四处来源并保持彼此一致：

| 文件 | 改动 |
|---|---|
| `spec/machine/lunar24.json` | RouteId 4 `sourceJack` → `vco_a.dry_out`（来源真相）|
| `generated/lunar24/registry.hpp` | 重新生成（`kNormalizedRoutes[4]` 同款事实）|
| `spec/machine/p0_inventory_manifest.json` | 归一化路由条目 `sourceJack` → `vco_a.dry_out` |
| `core/include/lunar24/core/machine_definition.h` | 文档 + 编译期 `static_assert` 来源 pin 改为 `vco_a_dry_out` |

`generate_registry.py --check` 零 diff（rc=0）证明 registry.hpp 与 lunar24.json 同步。

## 3. 先红后修的产品 oracle（新增 `tests/host/test_vco_normal_source.cpp`，CMake 注册 test #64）

走**约定入口**（encode → decode → `StandaloneAudioEngine.applyDeviceState` → `processBlock`，`EngineHarness`），断言 GH #18 契约：

1. **默认 DRY B 不再锁死**：44.1/48/88.2/96k，≥2 s，final 秒非恒定、无 50 ms 平窗（预修复 DRY B 是直流锁，zcr ~0–4，平窗于 sample 1530 触发）。
2. **cvAmt 0..1（11 点）**：非对称 A-only 变更在 cvAmt>0 时到达 B；cvAmt==0 隔离该 A→B 链路；默认 cvAmt==1.0（历史锁点）DRY B 永不直流。
3. **B-only 变更永不成为 A 的来源**（DRY A 逐位不变）。
4. **用户 cable 覆盖 / 移除恢复** A→B 归一化路由（`normalizedActive` 反转）。
5. **A→B 是无环边**：默认机器编译出 0 条反馈边（无人为单样本 z^-1 延迟），且没有 `(vco_a.dry_out, vco_b.cv_in)` 反馈线。

自环保留约定（@Codex）：原用**默认自环**测反馈 D-sample 分级的测试**保留**，改为指向真实的用户 B→B patch cable（`test_machine_definition.cpp` / `test_machine_cable_restore.cpp`），继续锻炼反馈机制，同时默认路由保持无环。

## 4. 突变验证（新增 `tests/mutation/run_vco_normal_source_mutation.sh`）

可重复 GENERATE→BUILD/RUN→RESTORE 工具：注入**分离的生产来源突变**（写进 build/ 下影子头，绝不写入被跟踪源文件），逐点证明 `REAL=绿 / MUTATED=红（且命中具体 pinned 断言）`。每个突变都是真实生产来源回归（绝不放宽校验器），且只对具体 FAIL 表达式验证，不靠"非零即红"。

| # | 突变 | 来源 | pinned 断言 | 结果 |
|---|---|---|---|---|
| 1 | `source_reverts_to_B` | registry.hpp | `fc == 0` | RED（26 子测）|
| 2 | `ignores_override` | patch_graph.h | `!rt->normalizedActive(vco_a_dry_out, vco_b_cv_in)` | RED（1 子测）|
| 3 | `self_read_sink_value` | machine_runtime.h | `d > 1e-9` | RED（10 子测）|
| 4 | `miss_route4_fact` | machine_runtime.h | `d > 1e-9` | RED（10 子测）|
| 5 | `force_dryb_constant` | machine_runtime.h | `bMoved` | RED（44 子测）|

- #1 需中性化编译期 static_assert（影子内），这样是 **oracle 而非编译器守卫**在运行时抓住来源回退。
- 真实基线（无突变）：**100 checks OK**；每个突变 build/run 都红且命中具体断言。
- 脚本有意**未注册进 CTest**：突变 build 按设计必挂。

## 5. Gate 矩阵（本机）

| 项 | 结果 |
|---|---|
| generate_registry.py --check | rc=0（零 diff）|
| check_registry_complete / check_registry_negative | rc=0 |
| check_core_headers / check_evidence_refs / check_host_engine_wiring / check_host_override_drift / check_host_script_codec / check_host_windows_target / check_iplug_pin_clean / check_spike_clean | rc=0（全过）|
| Debug ctest | 68/68 PASS（含新 test_vco_normal_source）|
| Release ctest | 待（见下方）|
| ASan+UBSan ctest | 待（见下方）|

> 注：Release / ASan 结果在下方"构建结果"节确认后回填。

## 6. 12 项已申报的全覆盖缺口（与本次修复**无关**，单独申报）

`check_registry_complete` 给出 `parameters` 的 12 项 gap，全部是**冻结目标允许项**，非本次来源修复的待办，也非回归：

- **Root-A 非标量（8，结构性，不是 to-do；不得被压平成 scalar）**：`keyboard.plate_tune`、`keyboard.preset_a`、`keyboard.preset_b`、`keyboard.preset_c`、`keyboard.preset_d`、`keyboard.pushbutton_value`、`keyboard.quantise_scale_editor`、`keyboard.seq_steps`
- **selector-toggle 无值域（4，冻结目标省略位置、手册无选项，值域证实前保持 gap）**：`keyboard.arp_clock`、`keyboard.arp_rhythm`、`keyboard.seq_clock`、`keyboard.seq_rhythm`

这些是 GH #12 键盘参数那一支（键盘子系统孤岛、4 键盘 route 全 kDeferred）的既有声明，与本 DRY B 来源修复是两码事。本次没有触碰键盘、没有改任何这些值的域/默认。

## 7. 变更文件一览

```
CMakeLists.txt                                     +9   （注册 test_vco_normal_source）
core/include/lunar24/core/machine_definition.h     改   （文档 + static_assert 来源 pin）
generated/lunar24/registry.hpp                     改   （kNormalizedRoutes[4] 来源）
spec/machine/lunar24.json                          改   （RouteId 4 sourceJack）
spec/machine/p0_inventory_manifest.json            改   （归一化路由来源）
tests/core/test_machine_definition.cpp            改   （③c 反馈 self-loop 改引用真实用户 B→B cable）
tests/host/test_machine_audio_families.cpp        改   （VCO-B 默认改判据；DRY B 已活，drone 用 traceDiff）
tests/host/test_machine_cable_restore.cpp         改   （normalizedActive(源→vco_b_cv_in) 6 处）
tests/host/test_vco_normal_source.cpp             新   （oracle，CMake test #64）
tests/mutation/run_vco_normal_source_mutation.sh  新   （5 突变验证工具）
report/2026-09-06-task83-vco-normal-source.md     新   （本报告）
```

## 8. 证据边界（诚实声明）

- **交付即"来源修复"本身**：默认 DRY B 从锁死变为活的振荡器。无环 A→B，B 读 A 的实时值。
- **未做 / 非本次范围**：不产生"可弹音的完整 B 声音集成"——那是 P4 范围；不改任何默认音高参数；不新增插孔；不改 `vco_b.vco_out` owner；不动 GH #6/#12 的键盘/自音声分支。
- **合并/发布**：GH #18 stay OPEN，直到 @Codex 独立审阅放行；不自行 merge / close / 判 MET。
