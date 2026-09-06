# task #85 (GH #19) — 真实产品混叠基线、测量门禁与首个改进方案

状态：**基线交付**（baseline + 计划，**未实现/未合并/未发布/MET 未声明**）。
任务来源：@Codex msg `6e5bb801` + `2a7a13ca`（完整范围见下方「边界」）。
基线：`c61b837`。隔离分支/工作树：`measure/19-product-alias-baseline`。

---

## 结论先行

1. **关键发现 — 真实 VCO-A 默认三角波是「naive 三角波」，无任何抗混叠。** 4 个采样率 × 220/440/880 共 12 个单元中，Method A 带内混叠与 Method B「解析 naive 三角折叠」逐一吻合（≤0.2 dB），说明产品 VCO 没有 polyBLEP / 无过采样。全带别名 `aliasA_full` 从 **−49.26 dB（880@44.1k）到 −77.59 dB（220@96k）**，每个单元均测出整数周期窗（gap≈0）、可复现。
   > Method B 的 `aliasB_inband` 是「naive 三角理想解析系数」与真实样本的对账；A≈B 即证明「真实产品输出 = naive 三角」，而非被理想替身或静音替身欺骗。

2. **测量工具本身通过全部负控制（G1–G4），混叠是证据而非预置门槛。** `--check` 退出码 GATE PASS；G1 复数共轭折叠（constructive-add + destructive-partial-cancel 双双按复数和判据通过）、G2 错采样率/参考频率标签被抓（good=−78.0 / wrong_sr=−90.3 / wrong_f0=−77.8 dB）、G3 28 个已产出单元 finite+非静音+域量程、G4 动态范围 naive −40.3 dB vs 带限 −245.6 dB → **205.3 dB**（隔离源码突变活在分析器，不在生产头加故障宏）。**没有断言任何 −60 dB 门槛**；未设阈值的真实缺陷只被记录。

3. **composite 路径如实标注，不冒称单模块：** `drone1_gn1`（saw+cubic，非单周期）`aliasA_full≈−0.1 dB，带内≈−4.5..−5.5 dB`；`wet_chain`（kVcfPath，非单模块）`aliasA_full≈−3.6..−4.3 dB`。二者均判为「复合链测量」，其谐波梳状解释力不足（残差≈总信号），**不接受**单模块混叠归因。

4. **blocker：** `preamp`（tanh 输入级）无 AC 刺激（共享 harness 的 host 预放馈入是恒定电压 → 单侧断点取值 → 输出为 DC）。它是 Class-B 每样本非线性，混叠应通过过采样**对比**而非谐波折叠测量，本片**不偷修**，记录为显式 blocked，其测量推迟到过采样路径（不在首片）。**真实单侧断点输出不可当作可忽略伪迹。**

5. **机器证据：** quad 输出在 4 采样率下全部成员 finite + 分块一致（fail=0）；单线程 callback **487.9 ns/sample（≈2.05 Msamples/s，per-frame processBlock(1) 路径）**；prep（encode→decode→applyDeviceState）中位 **0.048 ms**；本机 arm64 / Darwin 25.6.0 / Apple LLVM 21.0.0。CPU=本机证据，**不定跨机器预算**。

6. **首个改进方案（仅计划，未实现）：** 对 VCO 三角波做 **polyBLEP**（Class-A，零延迟）。首片 = 最窄 = **VCO 三角波振荡器本身**（已确证 dryA 触达；若 VCO A/B 共用同一三角生成器则一处修复同时覆盖 dryA/dryB）。候选 2（不推荐）= 2–4× 过采样重置（Class-B 式，带延迟）。详见「首片方案」。

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

静态 `DeviceStateV1` 只改**用户可达参数**（`vco_a_oct_sel`/`vco_a_tune`、`drone_1_mute_1`/`gate_hold`/`tune_1`、`mixer_ch*_vol`/`preamp_gain`），不改生产默认/路由/registry。每个已产出单元写 `.raw`（小端 f64）到 `report/gh19-probe/`，由工具消费。

**矩阵**：4 采样率（44.1/48/88.2/96k）× 周期路径 3 频率（220/440/880）× 低/高电平/合法刺激。记录**实际测出**的 f0（`f0_refined`）与峰值，非仅目标值。

**方法 A（测出带限参考）**：从信号自身的带内谐波系数（在 `k·f0 ≤ sr/2` 处做匹配滤波投影）重建参考，残差=混叠能量；形状/相位/幅度无关。`aliasA_full`=全带，`aliasA_inband`=报告带 [100,5000] Hz（零填充 Hann FFT）。

**整数周期窗（关键修正）**：小数周期窗下带内 `{e^{j2πk f0 n/sr}}` 不正交 → 投影/重建泄漏谐波 → 假底（≈−37..−48 dB）。修复：`per=sr/gcd(round(f0),sr)`，取能塞进缓冲区的最大整数周期倍数；`gap=|f0−round(f0)|/f0` 归零 ⇒ 谐波落整数 bin ⇒ 正交 ⇒ 无泄漏且 N 收敛。

**Method B（交叉核对，三角）**：理想三角精确解析 `a_tri(k)=4/(π²k²)`（奇 k），按**共轭镜像规则**折叠 + 对去重后的独立谱线求和，带内 [100,5000] dBc。

---

## 2. 结果矩阵（完整 28-cell TSV → `report/gh19-task85-matrix.tsv`）

关键 12 个 VCO-A 三角单元（全带 / 带内 / Method B 交叉）：

| sr | f0 | `aliasA_full` | `aliasA_inband` | `aliasB_inband` | 整数周期 |
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

> A−B 差异 ≤0.2 dB（A 带内 vs B 独立解析），即「产品=naive 三角」的**对账证据**。f0 全部整数周期（除 96000_220 gap=0.0001，在 INT_TOL 内），N 收敛、无泄漏窗、可复现。

composite / blocker 行（完整矩阵见 TSV）：`drone1_gn1` 12 行、`wet_chain` 4 行、`preamp` 8 行（blocked:no-ac-stimulus）。

---

## 3. 负控制门禁（工具可信度）

`python3 tools/gh19_alias_analyze.py --dir report/gh19-probe --check` → **GATE PASS**：

- **G1 共轭**（重合谱线对，复数和）：constructive-add（k=196&2596，`conj-sum≈proj≈0.00175`）、destructive-partial-cancel（k=196&2204，`conj-sum≈proj≈0.00148` vs `buggy-no-conj=0.00177`）→ 正确复数共轭折叠；无共轭的错和**不会**被误判为通过（幅值比较会在镜像同相时假阳性，故用复数比较）。
- **G2 标签对账**：good=−78.0 / wrong_sr=−90.3 / wrong_f0=−77.8 → 错采样率、错参考频率**可被抓**（功率归一化随标签一致性校验）。
- **G3 真实非替身**：28 个已产出单元 finite+非静音+域量程（probe `capture` 的域钳位即「未缩放理想替身」检测器）。
- **G4 动态范围**：naive=−40.28 / bandlimited=−245.62 → **205.34 dB** 分离；突变活在分析器（合成 naive vs 带限），生产头**零改动**。

---

## 4. 机器证据（本机，非跨机器预算）

`report/gh19-probe/gh19_cpu.tsv`：
- `cpu_four_output`：4 采样率下 wetL/wetR/dryA/dryB 全 finite + 等长（render() 只在每次 processBlock=Rendered 时返回 true ⇒ 分块一致）——**fail=0**。
- `cpu_prep`：encode→decode→applyDeviceState 中位 **0.048 ms**。
- `cpu_callback`：单线程 per-frame processBlock(1) **487.89 ns/sample（≈2.05 Msamples/s）**。
- 机器 arm64 / Darwin 25.6.0 / Apple LLVM 21.0.0（clang-2100.1.1.101）。

---

## 5. 分类

- **Class A**（带限振荡器不连续 → polyBLEP/BLEP，零延迟）：VCO saw 回绕、pulse 边沿、sub-saw、hard-sync；classic-drone saw；Schmitt 方波。**VCO 三角波属于此类**（其在两个极值处斜率不连续）。
- **Class B**（每样本非线性 → 过采样，含延迟）：preamp/distortion/VCF inputStage tanh —— 无 AC 刺激（preamp 被记录为 blocker），其过采样对比**另立契约**，不在首片。

---

## 6. 首个改进方案（仅计划，未实现）

**最窄首片目标：对 VCO 三角波振荡器做 polyBLEP。**

- **实现候选（最多 2）**
  1. **候选 A（推荐）：polyBLEP**。在相位累加器式三角波的两个极值斜率不连续处施加带限误差校正，零延迟，O(1) 每样本。与已验证工具对三角形参考一致（`a_tri(k)=4/(π²k²)` 为带限三角形目标）。零延迟、无 look-ahead、CPU 最低。
  2. **候选 B（考虑，不推荐）：2–4× 分数过采样 + 重置**。Class-B 式处理，牺牲延迟、CPU 最高；单个振荡器上不值得，除非选 VCF/非线性链（另有其契约）。

- **产品可达性**：`dryA`（VCO-A 默认三角）已确证、干净、周期 → 本片唯一可达路径。`dryB`（VCO-B）同款链前 tap；若 VCO A/B 共用同一三角生成器则一处修复同时覆盖；否则同法施加于 VCO-B 调用点（实现时确认）。VCO 的 saw/pulse/morph 无产品 `setWaveform` 入口（`setWaveform` 无产品消费）→ 属**模块研发探针**，不在首片；morph 写入不影响默认三角 → 事实单列，不在本片偷修。

- **波形/调制保持**：polyBLEP 只局部修正接近不连续点的采样；不改基础基频、幅度、三角形状类别。频率分配（VCO 调谐）、CV 混音、FM/AM 交互均在下游；polyBLEP 是逐样本相位校正，对相位累加式 FM 也成立，故调制保持成立。

- **别名改善量的可验证目标（用同工具，实现后对照）**：
  - 实现后重跑 `gh19_alias_probe`（探针不变）+ `gh19_alias_analyze.py --check`，对 VCO-A 三角各 cell 的 `aliasA_full` 与**本次 naive 基线**做前后对照，记录实测改善量。
  - 判据（不断言 −60 dB，不预设未测量数值）：polyBLEP 后 `aliasA_full` 必须**在每个 cell 上相对 naive 基线单调改善（可测的负向增量）**，且结果**更接近解析带限三角参考**（该参考 aliasA_full≈−245 dB，是理想带限的上界，**不是** polyBLEP 保证值）；若任一 cell 无改善 → 目标失败。
  - 改善量随实现落地后写入 reftable，以数据裁决，不以预算/经验阈值先行。

- **CPU / 延迟代价**：polyBLEP 见「候选 A」，每样本常数级（本机既证缓存/CPU 相关）；零新增延迟（无 look-ahead）。真实数字由实现后在同一 harness 测（callback 487.9 ns/sample 为对照组）。延迟/破环约束与 GH#11 只读测量契约一致（不以滤波器群延迟替代破环最小延迟）。

- **仍缺证据**：
  1. polyBLEP 三角**实测**达到的 `aliasA_full` 需实现+测量后才知道（现在没有该实现）。
  2. VCO-B 是否与该三角生成器共用（决定是否一处修复覆盖 dryA/dryB）——实现时核对调用点。
  3. sah/pulse/saw、sub-saw、hard-sync、Schmitt 方波、classic-drone saw 的**产品路径**各需模块级探针（本片 VCO 三角为唯一已达产品路径）。
  4. Class-B（preamp/distortion/VCF 的 tanh）需**过采样对比**契约，另立任务。
  5. 跨采样率的 VCF 频响/非线性链未分离 → 只报总波形误差，不做单模块归因。

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
# 4) 只出矩阵（与提交入包的 report/gh19-task85-matrix.tsv 一致）
python3 tools/gh19_alias_analyze.py --dir report/gh19-probe > report/gh19-task85-matrix.tsv
```

> `report/gh19-probe/`（5.3 MB .raw）是确定性可再生成产物，已 gitignore；提交入包的矩阵工件为 `report/gh19-task85-matrix.tsv`。探针/工具均**不改生产 DSP/默认值/路由/registry/持久化**，也不改 12 项已声明键盘缺口门禁。

---

## 8. 边界（与 2a7a13ca 一致）

- 仅允许：tests/probes、测量工具、必要 CMake 注册、report 文档。禁止：生产 DSP/默认值/路由/registry/持久化；不改 12 项已声明缺口门禁。
- 真实产品调用不能被 ideal 替身或静音输出替换而继续成功（G3 已保住）；复数折叠漏共轭必红（G1）；错采样率/参考频率标签与功率归一化可被对账抓住（G2）；隔离源码突变、不在生产头加故障宏（G4）。
- 0.5 dB 仅原探索标签，不升格为产品指标；真实单侧断点输出当可忽略伪迹被明确禁止。
- 最终建议只选一个最窄首片目标（VCO 三角 polyBLEP），≤2 候选，写产品可达性/波形调制保持/别名改善可验证目标/CPU 延迟代价/仍缺证据；−60 dB 不是既定全域门槛。
- **不自行** merge / 关闭 GH#19 / 发布 / MET。@Codex 负责独立复核、算法裁决、merge。
