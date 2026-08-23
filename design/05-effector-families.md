# 双效果器 39 个程序 → 归并成 8 个 DSP 家族
> @Claude 2026-08-23。来源：手册 p23–24（我渲染成图逐条读的，**不是文字提取**——
> 那两页是三栏排版，pdftotext 会把三列串行、名字互相穿插，不可用）。

## 核心结论
**39 个程序不是 39 个互不相干的算法，它们归属 8 个 DSP 家族。**
⚠️ **是「家族」不是「单一基元」**：族内仍有多个实现（P8 本身就是 6 个不同的合成程序；
P1 内的 reverse / windowed 播放是子实现，不因为做了普通 delay 就免费得到）。
做完一个家族会**同时推进**多个程序，但**一个程序只有在它需要的全部家族都齐备时才可交付** → 成本非线性，但也非免费。

## 8 个家族 与 **有多少程序需要它**（⚠️ 不等于「做完就可交付」）

| # | 家族 | **被多少程序需要** | 相关程序 |
|---|---|---|---|
| **P1** | **延迟线＋反馈** | **13** | TIME×3、MAGIC×3、ORCHE×3、INFINITY(O.D.D/Resonance Delay)、CATHEDRAL(Oct up delay/Space reverb) |
| **P3** | **变调器 pitch shifter** | **8** | PITCH SHIFTER×3、MAGIC×3(Pitch)、CATHEDRAL Shimmer、INFINITY(Pitch) |
| **P4** | **LFO 调制延迟／全通** | **6** | VIBROTREM×3(Tremolo/Vibrato/Chorus)、VIBE×3(Phaser/Flanger/Resonance flanger) |
| **P8** | **迷你合成器**（⚠️ 见下）| **6** | SYNTEX-1×3(Vibe/Pulse/Acid Synth)、GENERATOR×3(FM tone/Ramp/Voice) |
| **P2** | **混响网络** | **3＋** | CATHEDRAL(Shimmer/Space)、INFINITY(Resonance Reverb)；**另有约 8 个程序把 Reverb 用作 Z 参数** |
| **P5** | **滤波器** | **3** | FILTER×3(Auto Wah/HP-LP/Notch)　←**可复用主 VCF 代码** |
| **P6** | **环形调制＋S&H** | **3** | STRING RINGER×3 |
| **P7** | **降采样／破坏** | **3** | DIGITAL×3(Filter DAC/LFO DAC/Envelope crusher)　←**最便宜** |

## ⚠️ SYNTEX-1 / GENERATOR 的归类（**已定：留在 L3**）
它们算法上确实是**声源**（bass synth / noise mini synth，产生声音而非处理声音）。
我最初主张归入 L1 声源，**@Codex 反对，他是对的，已改。**

**定论：算法性质是声源，产品归属是效果器卡带 → 记账留在 L3/P8。**
**理由（决定性的一条）**：它们只能插进 DUAL EFFECTOR 槽、占一个卡带位、受左右槽与 X/Y/Z 控制，
物理位置在 filter/distortion **之后**。
👉 **若记作 L1，就等于宣称它们能像独立声源那样在 mixer/filter 之前并行常开**——
**那是硬件做不到的路由和同时发声能力，等于凭分类失误偷偷越过了能力边界。**
📌 通则：**能力记账要服从硬件拓扑，不能服从算法性质。**

## 实现顺序（按「每单位工作量推进的程序数」排，**≠ 产品档位**）
1. **P1 延迟线＋反馈** → 13（最高产出，且是 P3/P2 的基础设施）
2. **P3 变调器** → +8（与 P1 组合出全部 pitched delay）
3. **P2 混响** → +3，**并且让另外约 8 个程序的 Reverb 参数变成真的**
4. **P4 调制延迟／全通** → +6（chorus/flanger/phaser 同源）
5. **P5 滤波器** → +3（复用主 VCF）
6. **P6 环形调制** → +3
7. **P7 降采样** → +3（最便宜，可随时插队）
8. **P8 迷你合成器** → +6（**建议单独决策**，它们是音源不是效果）

📌 **可交付要求 required ⊆ done。P1+P2+P3 = 14/39（36%）。** 逐条依赖与可执行核算见 `05a-program-primitive-matrix.py`、`05a-tier-recount.txt`。
📌 **本节是实现顺序，不是产品档位**；档位见 `02` 第四节（按能力边界分，不按开发顺序截断）。
