# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# 39 个程序 → 必需基元集合。依据 = 手册 p23/p24 每个程序的 X/Y/Z 参数语义。
# P1 延迟线+反馈 P2 混响 P3 变调 P4 LFO调制 P5 滤波 P6 环形调制/S&H P7 降采样 P8 迷你合成器
P = {
 # CATHEDRAL
 "CATHEDRAL-1 Shimmer":            ({"P3","P2"}, "X/Y=Octave up/down→P3; Z=Decay→P2"),
 "CATHEDRAL-2 Oct up delay":       ({"P1","P3","P2"}, "X=Fb,Y=Delay→P1; 卡带名 Oct up→P3; Z=Reverb→P2"),
 "CATHEDRAL-3 Space reverb":       ({"P1","P2"}, "X=Fb,Y=Delay→P1; Z=Reverb→P2"),
 # MAGIC
 "MAGIC-1 Pitch delay":            ({"P1","P3"}, "X=Fb,Y=Delay→P1; Z=Pitch→P3"),
 "MAGIC-2 Reverse Pitch delay":    ({"P1","P3"}, "同上；reverse 视作 P1 变体"),
 "MAGIC-3 Bell pitchdelay":        ({"P1","P3"}, "同上"),
 # TIME
 "TIME-1 Delay reverb":            ({"P1","P2"}, "X=Fb,Y=Delay→P1; Z=Reverb→P2"),
 "TIME-2 Delay chorus":            ({"P1","P4"}, "Z=Mod depth→P4"),
 "TIME-3 Delay Vibrato":           ({"P1","P4"}, "Y=Delay/vibrato rate,Z=Mod depth→P4"),
 # VIBROTREM
 "VIBROTREM-1 Tremolo":            ({"P4","P2"}, "X=Depth,Y=Rate→P4; Z=Reverb→P2"),
 "VIBROTREM-2 Vibrato":            ({"P4","P2"}, "同上"),
 "VIBROTREM-3 Chorus":             ({"P4","P2"}, "同上"),
 # FILTER
 "FILTER-1 Auto Wah":              ({"P5","P2"}, "X=Filter amt,Y=Envelope→P5(+已有包络跟随); Z=Reverb→P2"),
 "FILTER-2 HP/LP filter":          ({"P5"}, "X/Y=HP/LP cutoff,Z=Res→P5"),
 "FILTER-3 Notch filter":          ({"P5"}, "X/Y=Cut1/2,Z=Res→P5"),
 # VIBE
 "VIBE-1 Phaser":                  ({"P4","P2"}, "X=Depth,Y=Rate→P4; Z=Reverb→P2"),
 "VIBE-2 Flanger":                 ({"P4","P2"}, "同上"),
 "VIBE-3 Resonance flanger":       ({"P4"}, "X=Res,Y=Rate,Z=Mod depth→P4"),
 # PITCH SHIFTER
 "PITCHSHIFT-1 SynthTaver":        ({"P3"}, "X/Y=Octave down/up,Z=Direct→P3"),
 "PITCHSHIFT-2 Octaver":           ({"P3"}, "同上"),
 "PITCHSHIFT-3 Pitch Harmonizer":  ({"P3"}, "X/Y=Pitch1/2,Z=Voice mix→P3"),
 # INFINITY
 "INFINITY-1 Resonance Reverb":    ({"P2","P4"}, "X=Pre delay,Z=Decay→P2; Y=Pre delay MOD→P4"),
 "INFINITY-2 O.D.D":               ({"P1","P3"}, "X=Fb,Y=Delay→P1; Z=Pitch→P3"),
 "INFINITY-3 Resonance Delay":     ({"P1","P3"}, "同上"),
 # STRING RINGER
 "RINGER-1 Synthetic Ring":        ({"P6"}, "X=Freq,Y=Res,Z=Sub→P6"),
 "RINGER-2 Ring Mod":              ({"P6","P2"}, "X=Freq,Y=Rate→P6; Z=Reverb→P2"),
 "RINGER-3 S&H Ring Mod":          ({"P6"}, "X=Pitch speed,Y=S&H rate,Z=Freq→P6"),
 # SYNTEX-1
 "SYNTEX-1 Vibe Synth":            ({"P8"}, "自发声卡带"),
 "SYNTEX-2 Pulse Synth":           ({"P8"}, "自发声卡带"),
 "SYNTEX-3 Acid Synth":            ({"P8"}, "自发声卡带"),
 # DIGITAL
 "DIGITAL-1 Filter DAC":           ({"P7","P5"}, "X=Sample rate→P7; Y=Cutoff→P5"),
 "DIGITAL-2 LFO DAC":              ({"P7","P4"}, "X=SR→P7; Y/Z=LFO speed/amount→P4"),
 "DIGITAL-3 Envelope crusher":     ({"P7"}, "X=SR→P7; Y=Env amount(已有包络跟随)"),
 # GENERATOR
 "GENERATOR-1 FM tone":            ({"P8"}, "自发声卡带"),
 "GENERATOR-2 Ramp":               ({"P8"}, "自发声卡带"),
 "GENERATOR-3 Voice":              ({"P8","P5"}, "Z=LP/HP→P5"),
 # ORCHE
 "ORCHE-1 One-shot Long":          ({"P1"}, "X=Delay time,Y=Fb→P1; Z=Trigger threshold 用已有包络"),
 "ORCHE-2 One-shot Short":         ({"P1"}, "同上"),
 "ORCHE-3 Free-run Loop":          ({"P1","P4"}, "Z=Delay mod LFO/RND→P4"),
}
assert len(P)==39, len(P)
tiers=[("A 空间核心", {"P1","P2","P3"}), ("B 完整处理", {"P1","P2","P3","P4","P5","P6","P7"}),
       ("C 全量", {"P1","P2","P3","P4","P5","P6","P7","P8"})]
prev=set()
for name,done in tiers:
    ok=[k for k,(need,_) in P.items() if need <= done]
    new=[k for k in ok if k not in prev]
    print(f"\n【{name}】完成 DSP 家族 {sorted(done)}")
    print(f"  可交付程序 {len(ok)}/39 = {100*len(ok)/39:.0f}%   （本档新增 {len(new)}）")
    prev=set(ok)
print("\n=== 各 DSP 家族被多少程序『需要』(≠可交付) ===")
from collections import Counter
c=Counter(p for need,_ in P.values() for p in need)
for k,v in sorted(c.items()): print(f"  {k}: {v}")
