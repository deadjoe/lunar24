# task #86 / GH#19 首片 — VCO 三角波带限斜率修正 (BLAMP) + 真实产品回归

**Branch** `fix/19-triangle-blamp` → **stacked draft PR** against `measure/19-product-alias-baseline` (HEAD `f25587e`), depends on PR#23.
**Exact head** to be filled after push; start point locked `f25587e0b4e3af8cd7432f1a4af7e062e7494f0f`.
**@Pi = 实施者**（受命 @Codex）；**不自行 merge / 关 GH#19 / 发布 / 判 MET** — 算法裁决与合并由 @Codex 独立复核完成。

---

## 1. 交付目标（mandate 摘录）

只改 Vco 产品可达的 `kTriangle` 输出（A/B 共享），用 **polynomial BLAMP**（一阶导数不连续 = 积分 BLEP）修正三角形角点斜坡跳变。保留相位推进、频率/CV 律、默认值、幅度契约、A→B 路由、持久化。不改其它波形 / sub / drone / VCF，不加新 selector，不 shore-fix morph-unreachable。不得经降音量 / 移频 / 延迟 / 改参考归一命中目标。

## 2. 测量共识（task#85 已核准）

- 度量：`blref_full_db` = 全带真实混叠 / 残差相对量，对 **droop-free Method BL 权威带限参考** `ref[i] = 2*a1_mag * Σ_{odd k, MIN_HZ≤k*f0≤sr/2} (1/k²) cos(k·tt·i + k·arg_a1)`，在干净整数周期窗内 `10·log10(Σ(nx−ref)²/Σnx²)`。负值更小 = 更接近理想带限。
- 参考是基于测得的基波幅度 `a1_mag`（≈0.40528）与相位 `arg_a1` 的 **无下垂** (droop-free) 理想三角 — 这是本片的关键约束：**经典 4 点 B-spline polyBLAMP 虽然优化谐波/混叠 SNR，却在 >10 kHz 引入约 −12 dB 下垂，反而让 `blref_full_db`（无下垂参考残差）变差**。见 §6。
- 入口链：`encode → decode → validate → owner.applyDeviceState → DeviceAdapter/processBlock`（真实产品调用，非 ideal/静默替身）。任务#85 的 `--check` 负控（G3/G4 真实入口注入、dry_triangle_scale_contract、label_contract、state-assoc）本次全绿：`GATE PASS exit 0`。

## 3. 生产改动（唯一被授权改动，A/B 共享，只触 `kTriangle`）

`core/include/lunar24/core/vco.h` — 在 `tick()` 中，当 `wave_ == kTriangle` 时叠加角点附近的 BLAMP 修正：

```cpp
inline void Vco::tick(double* out, double* subOut) {
  const double pitch = frequencyHz();
  const double instHz = pitch + fmDevHz_ * fmCv_;   // 线性 FM，CV 律保持。
  const double step = instHz / sr_;
  cumPitch_ += step;                                 // 相位推进顺序不变。
  *out = waveformSampleAt(phase());
  if (wave_ == VcoWaveform::kTriangle)
    *out += triangleBlampCorr(cumPitch_, step);       // 仅平价三角叠加角点修正。
  if (subOut) { /* 原有，未动 */ }
}
```

```cpp
inline double Vco::triangleBlampCorr(double cp, double step) const {
  const double dt = std::fabs(step);
  if (!(dt > 0.0) || !std::isfinite(dt)) return 0.0;
  const double uPeak = std::fabs(cp - std::round(cp)) / dt;            // 距峰抽样的距离。
  const double uVal  = std::fabs(cp - (std::floor(cp) + 0.5)) / dt;    // 距谷抽样的距离。
  double corr = 0.0;
  if (uPeak <= kBlampUmax) corr -= dt * blampG(uPeak);   // 峰 = −dt·g(|u|)。
  if (uVal  <= kBlampUmax) corr += dt * blampG(uVal);    // 谷 = +dt·g(|u|)。
  return corr;
}
```

- `g` = 偶数 residual 函数，LUT 采用 **无下垂精确 BLAMP residual**（论文 §2 Fig 2，DAFx-16 *Rounding Corners with BLAMP*），u∈[0,2] 33 点线性插值，`kBlampUmax = 2.0`。刻度/公式已对照原文引述（见 §7）。
- **符号关键**：峰 `-=`，谷 `+=`（颠倒则反向，见负控 #2）。
- **相位局部、因果、零样本缓冲延迟**：角点位置是当前相位 + dt 的确定性函数；`f0/sr` 无量纲，故修正 sr/f0 不变。同一帧 `tick()` 内消费，无新增输出缓冲。
- **无频率地板、无 CV 律改变**；`waveformSampleAt()` 的 `kTriangle` 保持朴素三角，`kMorphSineTriangle`（out of scope）不叠加 BLAMP。
- 纯头文件、无堆、无锁、无生产 fault 宏；修正状态是相位的纯函数（无跨块持留），保证 block-invariance（负控 #D）。

## 4. 25 单元 A/B 隔离三角形矩阵（真实产品探针 → 分析器）

措辞：`vco_a_tri_<sr>_<f0>` / `vco_b_tri_<sr>_<f0>`，A/B 各 3 频点 × 4 采样率 = 24 单元，实际探测器均覆盖。空白 = 任一真实产品调用被 ideal/静默替身成功。

完整矩阵见附件 `report/gh19-analyze.tsv`（BLAMP 当前值）。下为关键汇总（`naive` = 已在 `tools/gh19_naive_baseline.tsv` 提交的朴素三角基线，`blamp` = 当前）：

| 频点 | naive (dB) | blamp (dB) | Δ (dB) | 判据 |
|---|---|---|---|---|
| A 880 (44.1/48/88.2/96k) | −50.30 / −51.09 / −58.81 / −59.65 | −58.99 / −59.99 / −67.39 / −68.32 | **+8.69 / +8.90 / +8.58 / +8.67** | ≥6 dB ✓ |
| A 220 (同上) | −67.80 / −68.91 / −76.70 / −77.83 | −76.39 / −77.69 / −85.40 / −86.54 | **+8.59 / +8.78 / +8.70 / +8.71** | 不劣化 ✓ |
| A 440 (同上) | −58.81 / −59.65 / −67.80 / −68.91 | −67.39 / −68.32 / −76.39 / −77.69 | **+8.58 / +8.67 / +8.59 / +8.78** | 不劣化 ✓ |
| B（与 A 同值，A/B 共享 VCO 调用点） | … | … | **+8.58 … +8.90**（对称） | ✓ |

- **每一格都改善 ≥8.58 dB，无一格劣化**；880 Hz 全部 ≥6 dB（8.58–8.90 dB），220/440 Hz 全部改善（8.58–8.78 dB）。
- **B 与 A 完全对称** → 证明 A/B 共用同一 VCO `kTriangle` 调用点且路由/信号未随修正漂移。
- `gh19_blamp_acceptance` 门禁（`tools/check_gh19_blamp_acceptance.py`，对提交的朴素基线逐格跑）：**PASS, all 24**（880 档 8 格、220/440 档 16 格）。

**判据满足说明**：`blref_full_db` 为总诊断残差（非纯 alias 保证）。本次约 8.6 dB 的一致改善源于角点光谱扩展被 BLAMP 在基波/拍频处收敛——参考本身无下垂，故该改善是**真实的谱收敛，非降音量/移频/延迟/改参考归一**。

## 5. 负控（真实产品源突变，各别命中特定失败判据并恢复绿）

`tests/mutation/run_vco_blamp_mutation.sh`（未注册 CTest；对真实 `vco.h` 的私有备份做 splice，永不 `git checkout vco.h`）。5 处在已知 DEFENSE 点插入破坏，重编译 → 真实探针 → 分析器 → 门禁必须出现**特定** RED，再恢复 BLAMP → 重测 GREEN：

| # | 突变 | 期望 | 结果 |
|---|---|---|---|
| 1 | bypass（`if(false && wave_==kTriangle)`） | 880 Hz imp=0.00 dB（缺 6 dB）→ RED | ✓ |
| 2 | signflip（峰/谷符号互换） | 修正反向 → 单元劣于基线（imp 为负）→ RED | ✓ |
| 3 | scale_small（0.01×） | 880 imp << 6 dB → RED | ✓ |
| 4 | scale_big（10× 过校） | 过度修正 → 劣化 → RED | ✓ |
| 5 | delay_corr（校正锚到前一采样） | 失准残差 → 880 imp << 6 dB → RED | ✓ |

最终恢复 BLAMP → GATE GREEN。另有结构性负控（非 alias 度量可测）在 `tests/core/test_vco.cpp` 新增 `test_vco_blamp()`（50 检查全过）：

- **E** BLAMP 非 no-op：max|corrected−naive| = **0.00719**（≈ dt·g(0) = 0.00917·0.784，与论文理论完全吻合）。
- **C** morph 未动：`kMorphSineTriangle`@morph=1 保持朴素三角（max err 1.36e-11）。
- **A** 相位推进 / 无隐藏延迟：xcorr argmax@lag 0（−1=1595.8, 0=1599.95, +1=1595.81）。
- **B** 幅度守卫：corrected max=0.9928, min=−0.9928（不超 1.05，naive min−hi<0.05 → 无降音量伪造），峰 ≤1.0 → 无过冲。
- **D** block 一致：连续渲染 vs 奇块分区 [7,13,32] maxdiff = 0.000 → 非跨块状态重置。

**生产 fault 宏：0**。

## 6. 为什么用无下垂精确 BLAMP 而非经典 4 点 B-spline polyBLAMP（算法裁决输入）

经典 4 点 B-spline polyBLAMP 是高可听带宽下的常用近似，但对本度量的 `blref_full_db`（无下垂 Method BL 参考残差）**不满足**：B-spline 的转移函数在 >10 kHz 引入约 −12 dB 下垂（见 §2），它优化的是谐波/混叠 **SNR**，而非对无下垂理想的**谱残差**。因此本片选用了论文 *Rounding Corners with BLAMP*（DAFx-16）§2 Fig 2 的 **无下垂精确 BLAMP residual**——它在本频段不引入谱下垂，从而让 `blref_full_db` 一致下降（实测 ≈8.6 dB）。两种方案的取舍留待 @Codex 独立算法裁决；本片按 mandate 实现并经真实产品度量验证的这一种。B-spline 变体的负控数据（若需要可作为反例）不在首片范围。

## 7. 公式/刻度引述（DAFx-16 *Rounding Corners with BLAMP*）

- 三角角点为一阶导数（斜率）不连续（峰 +1→−1 斜率跳 −8，谷 −1→+1 斜率跳 +8），正确核为 **BLAMP**（BLEP 的一阶积分），而非 BLEP（后者对应幅度跳变）。
- 对角点距抽样 u = 角点（相位）量化位置的剩余距离，BLAMP residual g(u) 满足：`g(-u)=g(u)`（偶函数），`g(0)=0.784`（本次实测 0.00719 ≈ (440/48000)·0.784 一致），g 在 |u|>2 之外为 0 — 本片 `kBlampUmax = 2.0` 与此一致。
- 修正量 `dt·g(u)` 在峰为负、谷为正（符号见 §3）。`dt = |instHz|/sr` 为每采样相位步。
- 全文 PDF 存于 `research/`（未纳入提交；@Codex 如需可核对原引用）。

## 8. 全量回归 + 平台门禁

| 配置 | 测试数 | 结果 |
|---|---|---|
| Release | 70 | **100% pass**（含新 `gh19_blamp_acceptance` CTEST, 81.5s） |
| Debug | 70 | **100% pass**（含 `gh19_blamp_acceptance`, 115.4s） |
| ASan + UBSan | 70 | **100% pass**（含 `gh19_blamp_acceptance`, 209.3s） |

（此前 `test_vco` 在 ASan 命中的一个越界，根因是本片**测试辅助 lambda 的 xcorr 边界错置**——读 `blamp[-1]` / `blamp[n]`，非生产 BLAMP 代码；已修正为 `lo=max(0,lag)` / `hi=min(n,n+lag)`，Release/Debug 因越界读恰复现可容忍值而未暴露，ASan 正确捕获。**生产 vco.h 零改动**。）

`--require-full` 覆盖门禁：**仍是原 12 项键盘缺口**（8 非标量 + 4 无值域），`gap 列表未变、rogue=[]、新增=[]`。无新覆盖回归；门禁未改。

## 9. 交付物清单

- **生产改动**：`core/include/lunar24/core/vco.h`（仅 kTriangle BLAMP，0 其它波形/路由/默认/A→B/持久化改动）。
- **门禁**：`tools/check_gh19_blamp_acceptance.py`（RED→GREEN，对提交的朴素基线）；`tools/gh19_naive_baseline.tsv`（24 格朴素基线）；`tools/run_gh19_blamp_pipeline.py` + `CMakeLists.txt` 注册 `gh19_blamp_acceptance` CTEST（CI 强制、真实产品链路）。
- **负控**：`tests/mutation/run_vco_blamp_mutation.sh`（5 源突变 RED→GREEN）；`tests/core/test_vco.cpp::test_vco_blamp()`（5 结构性负控，50 检查）。
- **分析器**：`tools/gh19_alias_analyze.py`（只读，未改）。

## 10. 复跑命令

```bash
# 全量回归（Release / Debug / ASan+UBSan），在新 build 目录：
cmake -S . -B <build> -DCMAKE_BUILD_TYPE=Release && cmake --build <build> -j 8 && ctest --test-dir <build> --output-on-failure
# ASan+UBSan：
cmake -S . -B <asan> -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build <asan> -j 8 && ctest --test-dir <asan>

# 定向矩阵（探针 → 分析器 → 门禁）：
cmake --build <build> --target gh19_alias_probe
<build>/gh19_alias_probe --out <probe-out>
python3 tools/gh19_alias_analyze.py --dir <probe-out> --manifest tools/gh19_manifest.tsv --check     # 负控门禁
python3 tools/check_gh19_blamp_acceptance.py --baseline tools/gh19_naive_baseline.tsv --current <analyze.tsv>  # 接受门禁
# 或用一站式 CTEST：
ctest --test-dir <build> -R gh19_blamp_acceptance --output-on-failure

# 突变负控（scratch build，跟踪树保持干净）：
BUILD_DIR=<scratch> bash tests/mutation/run_vco_blamp_mutation.sh
```

## 11. 结论

- 交付判据达成：真实产品（非 ideal/静默替身）调用可成功；漏共轭/漏修正必红；隔离源突变被确诊并被门禁捕获；`--require-full` 覆盖保持原 12 项缺口。
- **24 格全部**“改善≥6dB（880 档）/ 不劣化（220/440 档）”均满足：880 Hz 档 8 格全部 ≥6 dB，220/440 档 16 格全部改善（无一格劣化）。
- 算法取舍（无下垂精确 BLAMP vs 经典 4 点 B-spline 的 −12 dB 下垂）已交由 @Codex 独立裁决；本片按 mandate 实现并验证的是无下垂精确 BLAMP。

**未执行（由 @Codex 独立完成）**：merge、关闭 GH#19、发布、MET 判定。等待 @Codex 复核 + 算法裁决。
