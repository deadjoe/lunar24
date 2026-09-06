# task #85 (GH #19) — 真实产品混叠基线、测量门禁与首个改进方案

状态：**基线交付**（baseline + 计划，**未实现/未合并/未发布/MET 未声明**）。
任务来源：@Codex msg `6e5bb801` + `2a7a13ca`（完整范围见「边界」）。
基线：`c61b837`。隔离分支/工作树：`measure/19-product-alias-baseline`。

---

## 结论先行

1. **关键发现 — 真实 VCO-A 默认三角波没有任何抗混叠。** 4 个采样率 × 220/440/880 共 12 个单元，`harmris_inband_db`（谐波拟合残差，下界）从 **−59.70 dB（880@44.1k）到 −91.36 dB（220@96k）**；`analytic_ref_inband_db`（独立解析 naive-三角参考）从 **−59.6 dB（880@44.1k）到 −91.6 dB（220@96k）**。二者在带内逐一吻合到 **0.020–0.370 dB**（**不是**统一 ≤0.2 dB——详见 ② 的措辞约束）。每个单元均测出整数周期窗（gap≈0）、N 收敛、可复现。

2. **Method A 不是独立的带限参考，只是「谐波拟合残差」下界；权威混叠值来自 Method B 解析参考（仅干净三角）。**
   - **Method A（`harmris_*`）= 谐波拟合残差**：从信号**自身**的带内谐波系数做匹配滤波重建参考，残差 = 模型未解释的能量。它会把**折叠到带内谐波 bin 上的混叠吸收进重建**，因此**吸收**了合法谐波上的混叠 → 只能当**下界**，**不能**当总混叠。
   - **Method B（`analytic_ref_inband_db`）= 独立解析参考**（干净三角）：用独立解析形状 `a_tri(k)=4/(π²k²)`（奇 k）+ 实测相位频率 `f0_refined` + 设备缩放 `a1_mag` 重建理想 naive 三角，再按**共轭镜像规则**折叠并去重。对干净三角为**权威总混叠**。保留重合折叠（constructive-add / destructive-partial-cancel 均已 G1 验证）。
   - **不把两列都叫「alias」**：`harmris_*` = 谐波拟合残差（下界），`analytic_ref_inband_db` = 解析参考。A≈B 只是**对账证据**（残差与解析模型一致、产品=naive 三角），**不是**「A 独立测出总混叠」。

3. **composite / 非线性路径如实标注，不做单模块混叠归因。** 无单模块混叠参考的复合链，只报**带内总误差/谐波拟合残差** + 所属类别，**不在列名或正文冒称「单模块混叠」**：

   | 路径 | 单元 | `harmris_full_db` | `harmris_inband_db` | 类别 |
   |---|---|---|---|---|
   | drone1_classic（saw+cubic 链） | 12 | −9.99 … −19.77 | −12.20 … −23.97 | composite(saw+cubic_chain) |
   | drone3_schmitt | 8 | −13.45 … −263.26 | −20.09 … −266.14 | composite(Schmitt_voice) |
   | drone6_schmitt | 4 | −13.45 … −68.00 | −20.09 … −75.17 | composite(Schmitt_voice) |
   | wet_chain（kVcfPath） | 4 | −3.57 … −4.30 | −5.39 … −6.35 | composite(kVcfPath) |
   | preamp_ac（tanh 输入级） | 8 | l0: −226 … −244 · l1: −56.65 … −92.36 | l0: −241 … −247 · l1: −57.89 … −104.67 | nonlinear(preamp_tanh) |

   > drone1_classic 谐波残差≈总带内功率（−3.6…−6.4 dB 带内误差说明其谐波梳状解释力不足），**不**解读为单模块混叠。

4. **preamp 不是产品阻断（已用 AC 刺激测出）。** 生产 host 输入可接受 AC；探针通过共享 harness 的**每样本输入馈送**（`renderFeed`，走 `engine.processBlock` 输入数组）在物理 ch1 喂零中心正弦，按 `mixer_ch7_vol`/`preamp_gain` 电平分级：

   - **l0（低电平，近乎线性区）：** `harmris_full_db −226 … −244 dB`、`harmris_inband_db −241 … −247 dB` ≈ **近干净**（tanh 在低幅近似线性，几乎无折叠）。
   - **l1（高电平，进入饱和）：** `harmris_full_db −56.65 … −92.36 dB`、`harmris_inband_db −57.89 … −104.67 dB`，属**失真**分量，类别 `nonlinear(preamp_tanh)`，**不归因为单模块混叠**。
   - 与旧报告「preamp blocker:no-ac-stimulus」不同——该 blocker 已由 AC 馈送**解除**；是否需过采样对比（Class-B）见「分类」。

5. **机器证据（分块一致 + 多次回调统计）：** 四个输出通道在 4 采样率下全部 finite + 分块一致（`finite_block_failcount=0`）；**分块划分不变性**（per-frame processBlock(1) vs 单次 processBlock(N)，同态同输入）`block_partition_maxdiff=0.00e+00`（**成功 render ≠ 分块不一致**这一缺陷在此路径不存在）；单线程 callback **中位 475.82 ns/sample**（±1.61 ns stdev，7 次），**块路径 474.23 ns/sample @ 1,048,576 samples**，prep（encode→decode→applyDeviceState）中位 **0.048 ms**；本机 arm64 / Darwin 25.6.0 / Apple LLVM 21.0.0。CPU 为本机证据，**不定跨机器预算**。

6. **A/B 共享 VCO 调用点：本片已确证。** 将合法面板旋钮 `vco_b_cv_amt` 置 0（见下）后，VCO-B 干信号与 VCO-A **完全一致**（12 个单元数值相同）→ 二者调用**同一 VCO 三角生成器**，一处三角修正同时覆盖 dryA/dryB。

7. **VCO-B 默认不是干净单载波（本片新发现，须记录）。** 默认图把 `vco_b.cv_in` 由 `vco_a.dry_out` 实时馈送（task#83 A→B 路由，`machine_runtime.h:2226-2237`），默认 `vco_b_cv_amt>0` 时 dryB 是被 VCO-A 三角**音频率 FM 调制**的声部，**不是**干净三角。因此对 VCO-B 的测量一律在**置 `vco_b_cv_amt=0`**（合法面板旋钮）下作为**稳态载波**测定；该默认 FM 行为作为产品事实记录，本片不修改。

8. **首个改进方案 = VCO 三角 BLAMP（带限斜率校正），非 step-polyBLEP；仅计划未实现。** 三角波在相位回绕 p=0 与 p=0.5 处**值连续但斜率不连续** → 需要**带限斜率修正**（BLAMP / 带限脉冲积分路线），**不能**把普通 step-polyBLEP 改名挪用（polyBLEP 是给**阶跃/边沿**——saw 回绕、pulse 脉冲、hard-sync——用的）。候选 ≤2，逐个给出**可测改善目标**，且**不预先承诺**「零延迟」「全单元改善」（见「首片方案」）。

---

## 1. 测的是什么（不是 ideal()）

探针 `tests/probes/gh19_alias_probe.cpp` 走**唯一约定产品入口**（与 host 相同）：
`make_default_device_state → encode → decode → engine.applyDeviceState → StandaloneAudioEngine::processBlock`（封装于 `tests/host/test_engine_harness.h`），捕获真实输出通道：

| 观察点 | 说明 | 域 |
|---|---|---|
| `dryA` | VCO-A，链前 tap（machine_runtime.h:117） | device (0.5 满度) |
| `dryB` | VCO-B，链前 tap（machine_runtime.h:118） | device |
| `drone1` | 经典 drone-1 混音前通道总线 `droneChannel(0)`（machine_runtime.h:1633） | volt |
| `wet_l` | WET 混音输出通道（device_layout WET_L=0） | device |

静态 `DeviceStateV1` 只改**用户可达参数**（`vco_a_oct_sel`/`vco_a_tune`、`drone_1_mute_1`/`gate_hold`/`tune_1`、`mixer_ch*_vol`/`preamp_gain`、`vco_b_oct_sel`/`vco_b_tune`/`vco_b_cv_amt`），不改生产默认/路由/registry。每个已产出单元写 `.raw`（小端 f64）到 `report/gh19-probe/`，由工具消费。

**矩阵 = 60 个产品可达单元（`required=1`）+ 2 个显式声明块（`required=0`，见「覆盖」）：**

| 家族 | 路径 | 数量 | 频率 | 刺激/电平 |
|---|---|---|---|---|
| VCO-A 三角 | `vco_a_tri` | 12 | 220/440/880 × 4 sr | 默认三角 |
| VCO-B 三角 | `vco_b_tri` | 12 | 同上 | 同 VCO-A，**cv_amt=0** 稳态载波 |
| 经典 drone-1 | `drone1_classic` | 12 | 亚音频调谐（f0_refined ~13–31 Hz） | 默认音型 |
| Schmitt drone-3 | `drone3_schmitt` | 8 | ~1160–2004 Hz | 2 电平 |
| Schmitt drone-6 | `drone6_schmitt` | 4 | ~2666–2756 Hz | 1 电平 |
| preamp AC | `preamp_ac` | 8 | 440 Hz 正弦 | 物理 ch1，l0/l1 电平（预放增益） |
| wet 链 | `wet_chain` | 4 | ~220 Hz | 经 WET 完整链 |

**方法 A（谐波拟合残差，下界）**：从信号自身的带内谐波系数（在 `k·f0 ≤ sr/2` 处做匹配滤波投影）重建参考，残差=模型未解释能量。`harmris_full_db`=全带，`harmris_inband_db`=报告带 [100,5000] Hz（零填充 Hann FFT）。**只当残差下界，不当总混叠**（见结论 ②）。

**Method B（交叉核对，仅干净三角）**：理想三角精确解析 `a_tri(k)=4/(π²k²)`（奇 k），按**共轭镜像规则**折叠 + 对去重后的独立谱线求和，带内 [100,5000] dBc = `analytic_ref_inband_db`，**权威总混叠**。

**整数周期窗（关键修正）**：小数周期窗下带内 `{e^{j2πk f0 n/sr}}` 不正交 → 投影/重建泄漏谐波 → 假底（≈−37..−48 dB）。修复：`per=sr/gcd(round(f0),sr)`，取能塞进缓冲区的最大整数周期倍数；`gap=|f0−round(f0)|/f0` 归零 ⇒ 谐波落整数 bin ⇒ 正交 ⇒ 无泄漏且 N 收敛。

**复合链（drone1/drone3/drone6/wet_chain）与非线性（preamp_tanh）**：无单模块混叠参考 → 只报谐波拟合残差 + 带内总功率，类别标注，**不冒称单模块混叠**。

---

## 2. 结果矩阵（完整 60-cell TSV → `report/gh19-analyze.tsv`）

关键 12 个 VCO-A 三角单元（`harmris_full` / `harmris_inband` / `analytic_ref`）：

| sr | f0 | `harmris_full` | `harmris_inband` | `analytic_ref` | 整数周期 |
|---|---|---|---|---|---|
| 44100 | 220 | −67.54 | −77.98 | −78.0 | gap 0.0000 |
| 44100 | 440 | −58.81 | −68.75 | −68.7 | 0.0000 |
| 44100 | 880 | −49.26 | −59.70 | −59.6 | 0.0000 |
| 48000 | 220 | −68.44 | −79.35 | −79.6 | 0.0000 |
| 48000 | 440 | −59.65 | −70.66 | −70.9 | 0.0000 |
| 48000 | 880 | −50.15 | −60.85 | −61.1 | 0.0000 |
| 88200 | 220 | −76.45 | −90.03 | −90.3 | 0.0000 |
| 88200 | 440 | −67.54 | −81.32 | −81.3 | 0.0000 |
| 88200 | 880 | −58.81 | −71.88 | −71.8 | 0.0000 |
| 96000 | 220 | −77.59 | −91.36 | −91.6 | gap 0.0001 |
| 96000 | 440 | −68.44 | −82.11 | −82.4 | 0.0000 |
| 96000 | 880 | −59.65 | −74.63 | −75.0 | 0.0000 |

> 带内 `harmris` 与 `analytic_ref` 逐单元差 **0.020–0.370 dB**（如 96000_880 为 0.370 dB），**不是**统一 ≤0.2 dB。`harmris` 是谐波拟合**残差下界**，`analytic_ref` 是独立解析参考；二者接近即「产品=naive 三角」的对账证据，但 `harmris` **不**是独立总混叠测值。f0 全部整数周期（除 96000_220 gap=0.0001，在 INT_TOL 内），N 收敛、无泄漏窗、可复现。

VCO-B 12 个单元与 VCO-A **完全相同**（cv_amt=0 稳态载波），证明 A/B 共用同一三角生成器。

composite / 非线性行见「结论 ③」表；完整 60-cell 见 `report/gh19-analyze.tsv`。

**覆盖（FAIL-CLOSED）：** `tools/gh19_manifest.tsv` 用 `required` 1/0 声明。`required=1` 的 60 个单元**每一个**都必须：表中存在对应行、`produced=true`、有原始 `.raw`（且非空）、信号 finite+非静音+域量程；**任一不满足 = 总体 FAIL（exit 1），不是 info 行**。`required=0` 的 2 个单元为**显式声明块，需给出具体源码证据**：

- `vco_sub`（sub-saw）：产品**无 `setWaveform` 消费入口**（无产品可设 sub-saw 波形）→ 属**模块研发探针**；`setWaveform` 无产品消费，故无产品可达 sub-saw 单元。非「漏掉」家族，是**全家族无产品路径**。
- `vco_saw_pulse_morph`：saw/pulse/morph 同样无产品波形入口（`setWaveform` 无产品消费）→ 研发探针，`morph` 写入不影响默认三角 → 事实单列，本片不偷修。

---

## 3. 负控制门禁（工具可信度）

`python3 tools/gh19_alias_analyze.py --dir report/gh19-probe --check` → **GATE PASS（exit 0）**。**不**断言任何 −60 dB 门槛；真实缺陷只被记录。

- **G1 共轭**（重合谱线对，复数和）：constructive-add（k=196&2596，`conj-sum≈proj≈0.00175`）、destructive-partial-cancel（k=196&2204，`conj-sum≈proj≈0.00148` vs `buggy-no-conj=0.00177`）→ 正确复数共轭折叠；无共轭的错和**不会**被误判为通过。
- **G2 标签契约（用 `label_contract` 接受/拒绝入口）**：正常接受（f0_target 220.00 / sr 44100 / measured 219.818）；改 sr·0.5 → **拒绝**（声明 sr 下重导出基频 187.272 Hz ≠ 实测）；改 f0×2 → **拒绝**（440 vs 实测 219.818）；**24 个三角单元格全部通过**（标签 → 输出对应）。旧「错误 target 被频率搜索纠正」是估计器行为，非标签校验，已删除。
- **G2 功率归一化**：正确分母(carrier)=−78.0 dB / 错误分母(信号总功率)=−40.09 dB → **必被抓**（不复用 naive vs dedup——它在无折叠重合的单元上会趋同，属假阳性）。
- **G3 真实非替身**：**60 个**已产出单元 finite + 非静音 + 域量程；覆盖门禁对 required 单元强制 finite/非静音/scale。
- **G4 改写（不再判「真产品必须有足够混叠」，那会把后续成功的抗混叠修复判为假）**：正确缩放干净带限 ideal 在**正确状态频率** = **合法**输出（分类为有效信号 + `label_contract` 接受）；**固定正确缩放 ideal 无视状态频率**（输出 2·f0，状态声明 f0）→ **拒绝**（f0-label，rel dev 50.00%）= 状态 → 输出关联成立；naive=−67.44 / bandlimited=−83.31 → **分离 15.87 dB ≥ SEP_MIN_DB** 验证工具分辨率。突变活在分析器（合成），生产头**零改动**、不在生产头加故障宏。
- **SELF-NEG（fail-closed 检测原语）**：silence 检测、non-finite 检测、skip-render(missing-buffer) 检测、**missing-file-gate**（复用覆盖门禁的 `miss_reason` 原语，对**3 种合成缺失**——not-produced/silent、produced 但无 raw、raw 不存在——全部返回原因；对**完整记录**不误报）。门禁在缺行/未产出/无 raw/0 产出/非有限/静音时**整体 FAIL**，不做 info 行。

---

## 4. 机器证据（本机，非跨机器预算）

`report/gh19-probe/gh19_cpu.tsv`：
- `finite_block_failcount`：4 采样率下 wetL/wetR/dryA/dryB 全 finite + 等长 → **0**（`cpu_four_note=clean`）。
- `block_partition_maxdiff`：per-frame processBlock(1) vs 单次 processBlock(N)（同态同输入）**0.00e+00**（`block_partition_note=clean`）——见结论 ⑤。
- `cpu_prep`：encode→decode→applyDeviceState 中位 **0.04769 ms**。
- `cpu_callback`：单线程 per-frame processBlock(1) **中位 475.82 ns/sample**，**±1.61 ns/sample stdev**（7 次）。
- `cpu_block_ns`：单次 processBlock 块路径 **474.23 ns/sample @ 1,048,576 samples**（`cpu_block_size`）。
- `cpu_callback_msg`：**2.1016 Msamples/s**。
- 机器 arm64 / Darwin 25.6.0 / Apple LLVM 21.0.0（clang-2100.1.1.101）。

> 块路径（474.23）与逐帧路径（475.82）差 <0.34%，且分块不变性 maxdiff=0 → 测量对**缓冲区分块不敏感**·缓冲准备（prep）不占计时（已 `reserve` 双幂容量）；真实 callback 多次采样取中位 + 报告方差 + 块大小。

---

## 5. 分类

- **Class A — 带限振荡器不连续（polyBLEP / BLAMP）：**
  - **阶跃/边沿不连续 → polyBLEP**：VCO saw 回绕、pulse 脉冲、sub-saw、hard-sync；classic-drone saw；Schmitt 方波。
  - **斜率不连续 → BLAMP（带限脉冲积分）**：**VCO 三角波**在极值 p=0、p=0.5 处值连续但**斜率不连续**，需要**带限斜率修正**，**不能**用 step-polyBLEP 改名。
- **Class B — 每样本非线性（过采样，含延迟）：** preamp/distortion/VCF inputStage tanh。已用 AC 刺激测出（preamp_ac，见结论 ④）；其**过采样对比**另立契约，不在首片。

---

## 6. 首个改进方案（仅计划，未实现）

**最窄首片目标：对 VCO 三角波谐振器做带限斜率修正（BLAMP / 带限脉冲积分），而不是 step-polyBLEP。**

- **A/B 共享调用点（本片已确证）：** VCO-A 与 VCO-B 在 `vco_b_cv_amt=0` 下 12 个单元数值**完全一致** → 二者调用**同一** VCO 三角生成器，一处修正同时覆盖 dryA/dryB。（旧报告「需实现时核对」已由本片数据**解决**。）

- **实现候选（≤2，各给可测目标，不预先承诺零延迟/全单元改善）**
  1. **候选 A（推荐）：三角 BLAMP / 带限积分修正。** 在相位累加器式三角波的两个**斜率不连续点**（p=0、p=0.5）施加带限误差校正（带限脉冲积分路线），O(1) 每样本。以 **Method BL `blref_full_db`（真实总混叠，权威参考，见 §8）** 为对照——`analytic_ref_inband_db`/`harmris_*` 仅当交叉核对/诊断。
  2. **候选 B（考虑，不推荐）：2–4× 分数过采样 + 重置。** Class-B 式处理，牺牲延迟、CPU 最高；单个振荡器上不值得，除非选 VCF/非线性链（另有其契约）。

- **产品可达性：** `dryA`（VCO-A 默认三角）已确证、干净、周期；`dryB`（VCO-B，cv_amt=0 稳态）共享同一生成器。VCO saw/pulse/morph/sub 无产品 `setWaveform` 入口 → 属模块研发探针，不在首片。

- **波形/调制保持（须实测）：** BLAMP 只局域修正接近不连续点的采样，不改基频、幅度、三角形状类别；对相位累加式 FM/AM 交互的调制保持**必须实测**，不能仅按「逐样本相位校正故成立」口头推断——列入首片验收据。

- **别名改善量的可验证目标（用同工具，实现后对照）：**
  - 实现后重跑 `gh19_alias_probe`（探针不变）+ `gh19_alias_analyze.py --check`，对 VCO-A 三角各 cell 的 **`blref_full_db`（权威，真实总混叠）**与本次 naive 基线做前后对照，记录实测改善量；`harmris_full_db`/`harmris_inband_db` 同步记录（诊断/核对）。
  - 判据（**不断言 −60 dB，不预设未测量数值**）：BLAMP 后 `blref_full_db` 必须**逐 cell 相对 naive 基线单调改善（可测的负向增量）**，且更接近 Method BL 全带参考（该参考上界为理想值，**不是** BLAMP 保证值）；**任一 cell 无改善 → 目标失败**。
  - 改善量随实现落地后写入 reftable，以数据裁决，不以预算/经验阈值先行。

- **CPU / 延迟代价：** 候选 A 每样本常数级；零新增 look-ahead。真实数字由实现后在同一 harness 测（callback 475.82 ns/sample、block 474.23 ns/sample 为对照组）。延迟/破环约束与 GH#11 只读测量契约一致（不以滤波器群延迟替代破环最小延迟）。**不预先承诺**「零延迟」「全部件改善」。

- **仍缺证据：**
  1. BLAMP 三角**实测**达到的 `harmris_*` 值，需实现+测量后才知道（现在没有该实现）。
  2. saw/pulse/morph/sub、hard-sync、Schmitt 方波、classic-drone saw 的**产品路径**各需模块级探针（本片 VCO 三角为唯一已达产品路径）。
  3. Class-B（preamp/distortion/VCF 的 tanh）需**过采样对比**契约，另立任务。
  4. 跨采样率的 VCF 频响/非线性链未分离 → 复合链只报总波形误差，不做单模块归因。

---

## 7. 复现

在隔离工作树根目录（`measure/19-product-alias-baseline`）执行：

```bash
# 1) 构建探针（Release）
cmake --build build --target gh19_alias_probe -j
# 2) 重新生成真实产品采样（确定性，产出 report/gh19-probe/ 的 .raw + TSV + cpu.tsv）
./build/gh19_alias_probe --out report/gh19-probe
# 3) 运行已验证测量 + 负控制门禁（--check 退出码 0 = GATE PASS）
python3 tools/gh19_alias_analyze.py --dir report/gh19-probe --check
# 4) 只出矩阵（与提交入包的 report/gh19-analyze.tsv 一致）
python3 tools/gh19_alias_analyze.py --dir report/gh19-probe > report/gh19-analyze.tsv
# (分析器依赖 numpy；无 numpy 的环境用 `uv run --no-project --with numpy python3 tools/...`)
```

> `report/gh19-probe/`（.raw 确定性可再生成产物）已 gitignore；提交入包的矩阵工件为 `report/gh19-analyze.tsv`、`report/gh19-check.txt`。探针/工具均**不改生产 DSP/默认值/路由/registry/持久化**，也不改 12 项已声明键盘缺口门禁。

---

## 8. REVISION (a1a71ae5) — 权威带限参考 + 真实入口负控

本报告基线在 `4e19f01`（PR #23）之后，针对 @Codex `a1a71ae5` 三个承重缺口做了定向复核修订，新增/改写如下；随后按 @Codex `1307b784` 收束为**三个检查入口**（设备输出缩放契约 / 标签契约 / 理想替身改写），每个入口交付正常/错误输入返回结果 + 代码定位（见 8.3）。**不改生产 DSP/默认值/路由/registry/持久化**，仍是只读测量。**参考尺度不再称「独立尺度」**。

### 8.1 Method BL：权威逐样本带限参考（item ①）

Method B 重述为**纯理论交叉核对**（见 8.2），新增 **Method BL** 作为权威参考。参考由真实产品采样**自身**构建（不再用理想尺度拟合设备缩放）：

- 参考 = 理想三角，落在产品**吸附的** `f0_snap`（`intperiod_window` 取整周），**设备缩放**用实测 `a1_mag`、**初相**用实测 `arg_a1`。
- **只带限到全 Nyquist（sr/2）**，非 `NYQ_FRAC·sr/2`（后者把 k=99·220 这类带内谐波误截掉——早先版本因此出现 10.43 dB 假缝）。
- 系数：`r[i] = 2·a1_mag·Σ_{奇 k, MIN_HZ≤k·f0_snap≤sr/2} a_tri(k)/a_tri(1) · cos(k·(2π·f0_snap·i/sr)+k·arg_a1)`，`a_tri(k)=4/(π²k²)`。
- `blref_inband_db` = 带内 [100,5000] 功率比 `10·log10(pe/px)`，`pe`=x−ref 带内功率、`px`=x 带内功率（均零填充 Hann FFT）。
- `blref_full_db` = 同式但**全带**（全 Nyquist），即真实总混叠。
- `blshape_max_db` = 带内奇 k 上 `max 20·log10(| |proj(x,k·f0_snap)| − (scale·a_tri(k)/a_tri(1))| / (scale·a_tri(k)/a_tri(1)))`——验证产品**确为干净三角**（参考归属有效）。

**尺度不变性**：混叠比按构造对尺度不变（信号减半 → 同 dBc），这对一个**混叠比**是正确的，不是幅度计。参考用**实测** `a1_mag`（属**拟合/实测**尺度，不是独立量；见 8.3 对断点/拟合尺度语义的修订）。独立于信号的设备缩放**契约**是另一条 raw 侧检查（8.3 入口 1 `dry_triangle_scale_contract`，由产品源导出 `0.5×1.0=0.5`），**不再**把参考尺度当「独立尺度」。

**与独立理论对账（24 个三角单元格，`blref_inband` vs `theory_dedup`）：** 44.1/48 kHz 一致在 ~0.06 dB 内；88.2/96 kHz 一致在 ~0.28 dB 内（深混叠 −90…−91 dB 靠近参考精度噪声底，故偏差略大）。

| 单元 | blref_inband（dB） | theory_dedup（dB） | |偏差|（dB） |
|---|---|---|---|---|
| 220 @ 44100 | −77.98 | −78.00 | 0.02 |
| 440 @ 44100 | −68.74 | −68.70 | 0.04 |
| 880 @ 44100 | −59.70 | −59.64 | 0.06 |
| 220 @ 88200 | −90.03 | −90.31 | 0.28 |
| 440 @ 88200 | −81.32 | −81.35 | 0.03 |
| 880 @ 88200 | −71.88 | −71.84 | 0.04 |
| 220 @ 96000 | −91.36 | −91.63 | 0.27 |

（全部 24 个三角单元格见 `report/gh19-analyze.tsv`；`blref_inband` 对干净三角与 `harmris_inband` 逐格相等——理想参考 ≡ 自信号拟合参考在带内等价。）`blshape_max_db` 范围 −43.33…−79.53 dB，确认产品是干净三角。**全带真实混叠 `blref_full_db` = −50.30 dB（880 @ 44100）… −77.83 dB（220 @ 96000）**，即首片抗混叠的优化目标指标。

### 8.2 Method A / Method B 归位（item ①）

- **Method A（`harmris_*`）** 明确降为**诊断/下界**：对产品**自身**带内谐波做拟合重建的残差，会把折叠到带内谐波 bin 上的混叠**吸收进去**，故**不能**叫「总混叠下界」，只能当诊断。**不是**独立参考。
- **Method B（`theory_dedup_db`）** 明确为**纯理论预测**：**不读任何采样、不读相位**，仅理想三角解析折叠线；**实测 `a1_mag` 在比值中相消**。作为交叉核对，不当作权威参考。
- 权威参考 = Method BL（8.1）。

### 8.3 负控改为真实入口注入（item ②，G2/G4；@Codex 1307b784 三入口修订）

原 G2（仅断言不同模型参数给不同结果）与 G4（两个合成信号）改为在**真实检查入口**注入并必须失败。@Codex 1307b784 收束到**三个检查入口**，每个都交付正常/错误输入的返回结果 + 代码定位；工具的 `--check` 即这些入口的接受/拒绝判据：

- **入口 1 设备输出缩放契约（raw 侧，`dry_triangle_scale_contract`）**：期望峰值**从产品源导出**（`device_adapter.h:90` `kDeviceScaleProvisional=0.5` × `vco.h:228` 三角峰值 `1.0` = **0.5**），**不是**从 raw 重估、也**不是**拟合的 `a1_mag`。真实单元 `ok=True`（peak 0.50000，dev 0.0%）；把 **raw 乘 0.5** 喂**同一函数** `ok=False`（peak 0.25000，dev 50.0%）→ **拒绝**。原 `scale_override=2·a1_mag`（参考侧改坏）已删除——它只改坏参考，不证明 raw 缩放错误可被抓。参考尺度不再称「独立尺度」。
- **入口 2 标签契约（接受/拒绝入口 `label_contract`）**：对**同一个**接受/拒绝入口，正常时比较**声明** `sr/f0` 与**独立实际状态预期**（探针实测 `f0_meas_hz`）。真实记录 `ok=True`（f0_target 220.00, sr 44100, measured 219.818）；**复制记录改 sr·0.5**（raw + 实际状态预期不变）→ `ok=False [sr-label]`（在声明 sr 下重导出基频 187.272 Hz ≠ 实测 219.818 Hz）**拒绝**；**复制记录改 f0×2** → `ok=False [f0-label]`（声明 440 vs 实测 219.818，rel dev 100.17%）**拒绝**。原「错误 target 被频率搜索纠正」是**估计器行为不是标签校验**，已删除。**矩阵-wide**：24 个三角单元格（含 FM 调制 vco_b_tri）全部通过 `label_contract`（标签 → 输出对应成立）。
- **入口 3 理想替身改写（状态 → 输出关联）**：**不再**以「输出必须有足够混叠」判真产品（那会把后续成功的抗混叠修复判为假）。正确缩放干净带限理想在**正确状态频率** = 合法输出：分类为有效信号（非静音/非有限）且 `label_contract` **接受**（clean-legal）。而**固定正确缩放 ideal 无视状态频率**（输出在 `2·f0`，状态声明 `f0`）→ `label_contract` **拒绝**（f0-label，rel dev 50%）——即「返回固定正确缩放 ideal」错误被识别为红。静音替换被分类为静音。
- **G2 保留**：功率归一化（正确分母带内 `pe/px` vs 错误分母信号总功率，断言不同）。
- 保留既有：固定缺失行/缺失文件门禁（SELF-NEG 检测 3 种合成缺失均检出，无空成功）。

代码定位：入口 1/2/3 均在 `tools/gh19_alias_analyze.py`——`dry_triangle_scale_contract`（约 750 行区）、`label_contract`（约 770 行区）、检查块 G2/G4（约 925 行区）；`--check` 的接受/拒绝汇总在 `check_fails`。全部是**测量工具**改动：生产 DSP/默认/路由/registry/持久化与 12 缺口门禁**零改动**。

### 8.4 纯 DSP CPU 计时（item ③）

- **`processPure`**：新增于共享 harness `tests/host/test_engine_harness.h`（**超出「tests/probes」的小扩展，需 @Codex 认可**），直接 `engine_.processBlock(in,out,kInCh,kOutCh,frames)`，**零向量分配、零输入生成、零输出插入**；块大小为 `block=512`，跨 7 条 `blkLanes` 中位数统计。
- **CPU 纯内核 = 490.17 ns/sample**（stdev 7.47，512 块，7 lanes）——测 DSP 本体，非 harness 渲染环。
- 原 harness 渲染环成本改为 `cpu_render_loop` = 481.91 ns/sample（重命名，不再叫「callback」）。

---

## 9. 边界（与 2a7a13ca 一致）

- 仅允许：tests/probes、测量工具、必要 CMake 注册、report 文档。禁止：生产 DSP/默认值/路由/registry/持久化；不改 12 项已声明缺口门禁。
- 真实产品调用不能被 ideal 替身或静音输出替换而继续成功（G3 + SELF-NEG 已保住——干净带限 ideal 在**正确**状态频率是**合法**输出，但**无视状态频率**的固定 ideal 必红）；复数折叠漏共轭必红（G1）；错采样率/参考频率标签经 `label_contract` 与**功率归一化**可被抓（G2）；隔离源码突变、不在生产头加故障宏（G4）。
- 0.5 dB 仅原探索标签，不升格为产品指标；真实单侧断点输出当可忽略伪迹被明确禁止。
- 最终建议只选一个最窄首片目标（VCO 三角 BLAMP），≤2 候选，写产品可达性/波形调制保持（须实测）/别名改善可验证目标/CPU 延迟代价/仍缺证据；−60 dB 不是既定全域门槛。
- **不协商** merge / 关闭 GH#19 / 发布 / MET。@Codex 负责独立复核、算法裁决、merge。
