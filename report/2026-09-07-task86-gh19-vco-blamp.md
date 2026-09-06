# task #86 / GH#19 首片 — VCO 三角波带限斜率修正 (BLAMP) — @Codex `b9a77738` 投用 win8 并接入生产

**Branch** `fix/19-triangle-blamp`，置于 `measure/19-product-alias-baseline` 之上（依赖 PR#23）。**@Pi = 实施者**（受命 @Codex）；**不自行 merge / 关 GH#19 / 发布 / 判 MET** — 算法裁决与合并由 @Codex 独立复核。

> 本轮按 @Codex `b9a77738` 裁决：**投用 win8**（L=8）并**接入生产 `kTriangle` 路径**，替换既有 poly 参考。算法正式命名：**「解析来源、Hann 窗截断、线性插值 BLAMP 近似」**（`tools/gen_gh19_blamp.py` 生成）——**不是**「exact」、**不是**「no-droop」、**不是**「无穷支撑」。

---

## 0. 一句话结论

> **已把 win8（解析来源、Hann 窗截断、线性插值、有限支撑 L=8 的 BLAMP 近似）接入生产 VCO 的 `kTriangle`（A/B 共享）调用点**，在真实产品探针上 **24/24 GREEN**（880 Hz 8 格 `blref_full_db` 改善 **+11.43 … +12.00 dB** ≥6 dB；220/440 Hz 16 格无劣化 >0.5 dB），`blockpart_max=0`。生成器已改为**断言式 fail-exit + 生成物一致性**检查。previousA 与 block-error 负控在**最终 win8 head** 重验均为**可载重、可逆**；CPU 增量为本测量可分辨的 ~+2.3%（**非**「本质上零成本」）。polyBLAMP 仅报告实测失败（0/24，`blref_full_db` 每格劣化），不推断频率响应成因。

---

## 1. 交付清单（b9a77738）

| # | 项 | 位置 | 状态 |
|---|---|---|---|
| 1 | win8 接入生产 `kTriangle`（A/B 共享，替换 poly 参考） | `core/include/lunar24/core/vco.h` | ✓ |
| 2 | 生成器：断言式 fail-exit（corner/对称/边界/插值误差）+ 生成物一致性 | `tools/gen_gh19_blamp.py` | ✓ |
| 3 | 24 格矩阵（真实产品探针，接**集成后** vco.h） | 见 §4 | ✓ |
| 4 | previousA 负控（**最终 win8 head** 重验） | `tools/run_gh19_previousa_control.sh` | ✓ |
| 5 | block-error 负控（**最终 win8 head** 重验） | `tools/run_gh19_blockerror_control.sh` | ✓ |
| 6 | BLAMP 载重负控（mutation，win8 基线） | `tests/mutation/run_vco_blamp_mutation.sh` | ✓ |
| 7 | CPU 离散度 + 可分辨增量 | `tools/run_gh19_cpu.sh` + §8 | ✓ |

---

## 2. 算法命名与内核构造

**「解析来源、Hann 窗截断、线性插值 BLAMP 近似」。**

- **解析来源**：R(u) = u·(1/2 + Si(π·u)/π) + cos(π·u)/π² − u·H(u)（论文 Eq.(6) − 平凡斜坡，无量纲残差，偶对称，R(0)=1/π²）。斜率跳变幅度 2µ = 8·|step|，故 g(u)=8·R(u)·w(u)。
- **Hann 窗截断**：w(u)=cos²(π·u/(2L))，|u|≤L，否则 0；**连续窗函数在支撑边界 C1**（值与一阶导同时 → 0）。本片 L=8。
- **线性插值 LUT**：N=256 点均匀采样于 [0,L]，`g(u)` 经线性插值求值 → **最终离散核是分段线性（C0）**，并非处处 C1（见 §2.1）。

**scale**：`mag = dt`（`blampG` 已带那个 8；若再 `8·dt` 会双重计 8、跑大 8 倍）。峰 `corr −= mag·g(uPeak)`，谷 `corr += mag·g(uVal)`。

**多重回绕**：支持半径内**全部**周期角点（整数相位的峰 + 半整数相位的谷）累加，不互相截断；`blampG` 在 L 外为 0，故只扫能蹭到的整周期 `|n| ≤ ceil(8·dt)+1`。

### 2.1 误差口径：连续窗是 C1，但线性插值 LUT 不是处处 C1

`-u` 注释/文档里**必须**区分：**连续（被窗截断后的）核**在 `|u|=L` 处是 C1（值 + 一阶导 → 0）；但**实际用到的线性插值 LUT**是分段线性（C0）。它在支撑边界的**最后一段**仍有一个小残差斜率，然后到支撑外变 0。**保留这个小误差并量化它（不换插值）**：

| 量 | 值 |
|---|---|
| 网格步长 `Δu` = L/(N−1) | 8/255 = **0.03137** |
| 最后一点 g(L) | **0**（精确） |
| 倒数第二点 g(L−Δu) | **2.160e-7** |
| **最后一段斜率** = (g(L)−g(L−Δu))/Δu | **−6.885e-6** |
| 边界外 | 恰为 0 |

- 连续核：边界值、边界一阶导都 → 0（C1，无支撑边界跳变）。
- 线性插值核：仅 C0；最后一段斜率 ≈ **−6.885e-6**，然后到支撑外变 0 —— 是**斜率**的微小不连续，**不是**值跳变。这是**插值 LUT 固有的近似代价**，本片保留并量化，不改为其它插值（改插值不在授权范围）。
- 全程最大线性插值误差（0..L 上 4001 点比对）≈ **9.77e-4**。

@Codex 明示：**连续窗函数是 C1，但线性插值 LUT 不是处处 C1，最后一段斜率约 −6.885e-6，到支撑外变 0 —— 保留这个小误差并量化即可。**

---

## 3. 生成器（断言式 fail-exit + 生成物一致性）

`tools/gen_gh19_blamp.py --L 8 --N 256 --cpp`：

- **断言**（`assert_ok` → `SystemExit`，非打印）：
  - corner = 8/π² = **0.810569469**（≤1e-9）
  - 偶对称 max|g(x)−g(−x)| = **3.029e-15**（≤1e-12）
  - 边界值 g(L) = **9.448e-36**（≤1e-12）
  - 边界一阶导增量 g(L)−g(L−2·step) = **1.338e-06**（≤1e-4）
  - 线性插值误差 **9.7742e-04**（≤默认 tol 2e-3）
- **生成物一致性**：把 emitted LUT 数组体重新解析（限定在 `kBlampLut[] = {` … `};` 之间，避开 license/注释里的 `2.0`），逐点与解析 g 比对，max|emitted−analytic| = **4.996e-10**（≤5e-10）。
- **负控**：`--interp-tol 1e-6`（超容）→ exit 1。
- **常量一致性**：生成器 `kBlampLut` 与集成 vco.h 的 `kLut` **逐值相同**（256 点，max|gen−vco| = **0.0**）—— 只是名字不同，值完全一致（`kLut` 是集成文件内的名字）。

@Codex 明示：把 corner/对称/边界/插值误差由「只打印」改为 **fail-exit 断言**，并**加生成物一致性检查**——本轮两者均已落地并**实测通过**。

---

## 4. 24 格矩阵（接**集成后** vco.h；真实产品探针）

`tools/run_gh19_candidates.sh core/include/lunar24/core/vco.h`（接生产文件本体，非候选副本）：

| 变体 | 880 Hz 档 | 220/440 Hz 档 | 总 | `blref_full_db` 改善 |
|---|---|---|---|---|
| naive（基线） | 参考 | 参考 | — | — |
| **win8（L=8，集成后）** | **8/8 ≥6 dB** | **16/16 不劣化** | **24/24 GREEN** | **+11.43 … +12.00** |
| polyBLAMP | 0/8 | 0/16 | **0/24 RED** | 每格劣化（见 §5） |

代表（880/44100）：naive −50.30 → win8 −61.73（**+11.43**）；(48000,880) −51.09 → −63.09（**+12.00**）。

---

## 5. polyBLAMP：只报告实测失败（不做频率响应推断）

授权的 multipoint polyBLAMP（Esqueda §3 表 1，corner 7/30）在**同一探针/分析器/门禁**下 **0/24 RED**：每个格子 `blref_full_db` 均**劣于**朴素三角（`imp<0`），实测范围 −6.74 … −7.20 dB（880 档 0/8，其余 0/16）。本片**只陈述这个实测结果**（该二次核在 BL 权威带限参考下系统性为负改善），**不**据此推断「>10 kHz −12 dB 主导」等频率响应归因——那需要同采样率的频率响应实测，本片未做、也不臆造。

---

## 6. 高频越界显式回退（b9a77738：明确阈值 + 采样率映射）

- **阈值**：`if (dt·8 ≥ 0.5) return 0.0;` ⟺ **dt ≥ 1/16**（L=8）⟺ **|step| ≥ 1/16**。
- **含义**：`step = f0/fs`，故 `dt ≥ 1/16 ⟺ f0/fs ≥ 1/16 ⟺ f0 ≥ fs/16`。对应采样率频率：

| fs | f0 门槛（≥ 则该档回退为朴素） |
|---|---|
| 44100 | **2756.25 Hz** |
| 48000 | **3000 Hz** |
| 88200 | **5512.5 Hz** |
| 96000 | **6000 Hz** |

- **切换行为**：一旦 `dt·8 ≥ 0.5`，校正**返回精确 0**（有界朴素三角），**不**声明该区间有任何改善；这是**显式、有界、可清单的回退**（`b9a77738` 允许），不是静默截断。
- **FM 下的性质**：`step` 随 FM 逐采样变化，故该阈值在 FM 下是**逐采样**判定的。**更根本地**：单步 BLAMP 是一个**瞬时步长的近似**——它按当前 `(phase, step)` 就地修正，对单一主导角点成立；当 `|step|` 大到支持半径跨半周期（角点相互紧邻不满足单角点几何）或 FM 深调制时，该近似退化为**仅在瞬时的步长意义下有效**，**不**宣称全带 / 多重回绕的改善。§4 的 24 格验证只在 `f0/fs < 1/16` 的信号上成立；越过阈值即回退，**不**把回退区当作更优。

---

## 7. 负控：previousA 与 block-error（**最终 win8 head** 重验）

### 7.1 previousA（路由完整性；B 读 live 同帧）

| 阶段 | test_vco_normal_source (6) 判别 | 结果 |
|---|---|---|
| 基线（无突变） | madSame=2.378e-05, madPrev=4.558e-3 | **GREEN**（107/107，正确建树） |
| previousA 突变 | madSame→4.565e-3, madPrev→2.264e-5 | **RED @(6)**（madSame 变大了；其它 5 测试保持 GREEN） |
| 恢复基线 | madSame=2.378e-05, madPrev=4.558e-3 | **GREEN** |

### 7.2 block-error（块分区守卫非空过；注入点 = `DeviceAdapter::renderBlock`）

| 阶段 | blockpart_max | probe exit | note |
|---|---|---|---|
| 正确路径（win8） | **0.00e+00** | 0 | clean |
| block-error 突变 | **2.05e-03** | **8** (bit8) | mismatch |
| 恢复 | 0.00e+00 | 0 | — |

### 7.3 BLAMP 载重负控（mutation，win8 基线）

`tests/mutation/run_vco_blamp_mutation.sh`（win8 基线；每个真实源码突变必须编译且 probe exit 0，gate 才计 RED）：

| 控制 | 结果 |
|---|---|
| naive（禁 kTriangle 修正） | RED @ 880Hz（imp=0） |
| polyBLAMP | RED @ 每格（imp<0） |
| win8 | **GREEN**（880 8/8, 220/440 16/16） |
| bypass_ / scale_small_ / late_anchor_ | 各 deflected 880Hz→RED |
| signflip_ / scale_big_ | 各 deflected imp<0（RED-WORSE） |
| volume_fake（x100 过载） | 探针自身 OVERSIDE GUARD 拦截（exit≠0） |
| block 不变量 | `blockpart_max=0` |

---

## 8. CPU — 重复样本统计 + 只说明「本测量能否分辨增量」（**最终 win8 head**）

`tools/run_gh19_cpu.sh`（同机 Apple arm64, clang 21, 纯 `processBlock` 每样本成本）：

| 变体 | 纯 processBlock (ns/样本) | ±stdev |
|---|---|---|
| naive（无修正） | 495.675 | ±1.960 |
| **win8（集成后）** | 507.020 | ±0.583 |

增量 win8−naive = **+11.35 ns/样本 ≈ +2.3%**；相对两变体 stdev（约 2.05 汇合），增量 ≈ **5.5×** → **本测量能分辨增量**。BLAMP 是一个**真实、非零、~+2.3%** 的成本增量，**不是**「本质上零成本」。（整 harness 循环列本次 win8 离散偏大 var≈16.7，故只引用纯 `processBlock` 列作为可靠口径。）数值随机器负载浮动，但方向与可分辨性稳定。

---

## 9. 边界 / 范围（b9a77738）

- **只触产品可达 `kTriangle`**（A/B 共享）；保留相位推进 / 频率 CV 律 / 默认 / 幅度契约 / A→B 路由 / 持久化 / 同帧消费（无新增输出缓冲延迟）。
- 不改其它波形 / sub / drone / VCF；无新 selector；不 shore-fix morph；不经降音量 / 移频 / 延迟 / 改参考归一。
- `step=0`（freq 锁零，触碰冻结 P0，本片不修）→ 返回 0（无 pop）。
- **12 键盘缺口** by-design 红不改（与既定基线一致）。
- **非多项式 = 已授权**：本片（win8）为**非多项式、解析 + 显式窗**，经 `b9a77738` **授权投用**；poly 仅作记录性反例。

---

## 10. 判定请求 / 提交（@Codex）

- **投用 win8（L=8）** 已完成并接入生产 `kTriangle`；独立定向检查 + 负控在最终 win8 head 重验全部 GREEN/载重。
- **完整管线**：本片已跑定向检查（24 格 + previousA + block-error + CPU）+ 受影响与全量 Release/Debug/ASan+UBSan、host/generator 门禁；随后 **commit → push 独立分支 → 更新 PR#24 → 等待 exact head 四平台 CI**，fix 范围内失败。
- **提交 SHA / PR / CI 结果**见文末（投用后追加）。
- **merge / 关 GH#19 / 发布 / MET** 由 @Codex 独立完成；@Codex 复核后直接接 GH#20 与 GH#12。

---

## 复现命令

```bash
python3 tools/gen_gh19_blamp.py --L 8 --N 256 --cpp            # 断言 + 生成物一致性 (exit 0)
bash tools/run_gh19_candidates.sh core/include/lunar24/core/vco.h    # 24 格矩阵 + 门禁
bash tools/run_gh19_previousa_control.sh                        # previousA 负控
bash tools/run_gh19_blockerror_control.sh                       # block-error 负控
bash tests/mutation/run_vco_blamp_mutation.sh                    # BLAMP 载重负控
bash tools/run_gh19_cpu.sh build/.vco_blamp_pristine.h core/include/lunar24/core/vco.h  # CPU
```
