# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Lunar 24 声音实验室 — the editable source of every page.

Run `python3 learn/source/content.py` to rebuild .build/slides.json (read by deck.mjs and
preview.py) and the per-page evidence table in README.md.

Page fields
  ch      chapter key (CHAPTERS)            title   page title
  lead    one short line under the title    layout  cover | split | wide | mission | map | flow | abbr | glossary | table
  figs    panel figures: fig(name, marks)   blocks  two (heading, [lines]) pairs
  try_    the "动手试试" line                terms   keys of TERMS shown in the creature card
  more    reader-facing extra explanation (reading page only)
  audio   id in assets/audio/manifest.json  src     maintainer evidence (README only, never shown)
Figures are crops of the real panel in panel units (2400 x 1552), see FIGS. Marks outline
controls by their registry ids (see .build/widgets.json) and may carry a number or a label.
"""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]

CHAPTERS = {
    'start': ('开始玩', 'red'),
    'classic': ('经典 Drone', 'teal'),
    'papa': ('Drone 3 / 6', 'teal'),
    'vco': ('VCO A / B', 'green'),
    'ext': ('外部声音', 'teal'),
    'more': ('更多发声方式', 'orange'),
    'keys': ('键盘演奏', 'green'),
    'mix': ('混音与塑形', 'orange'),
    'fx': ('效果器', 'orange'),
    'mod': ('让参数动起来', 'red'),
    'midi': ('MIDI 与保存', 'teal'),
    'inside': ('幕后：它怎样工作', 'ink'),
    'make': ('做一段作品', 'red'),
    'keep': ('把作品留下', 'red'),
    'help': ('排查与复习', 'red'),
    'appendix': ('附录', 'ink'),
}

# Crops of the rendered panel: (panel state, x, y, w, h) in panel units. Module crops sit
# 3.5 units outside the module frame, so the frame shows whole and no neighbour leaks in.
FIGS = {
    'panel': ('panel', 8, 118, 2384, 1386),
    'drone1': ('panel', 18.5, 254.5, 395, 308),
    'drone3': ('panel', 18.5, 562.5, 395, 321),
    'vcoa': ('panel', 413.5, 562.5, 396, 321),
    'vcob': ('panel', 1595.5, 562.5, 394, 321),
    'enva': ('panel', 801.5, 711.5, 350, 172),
    'envb': ('panel', 1245.5, 711.5, 357, 172),
    'mixer': ('panel', 805.5, 531.5, 791, 181),
    'processing': ('panel', 805.5, 178.5, 791, 357),
    'generator': ('generator', 805.5, 178.5, 791, 357),
    'lfoa': ('panel', 17.5, 880.5, 280, 152),
    'joystick': ('panel', 294.5, 880.5, 306, 152),
    'seq5': ('panel', 599.5, 880.5, 950, 152),
    'external': ('panel', 1546.5, 880.5, 559, 152),
    'keyboard': ('panel', 392, 1058, 1614, 438),
    'jacks_l': ('panel', 868, 1104, 172, 85),
    'jacks_r': ('panel', 1360, 1104, 172, 85),
    'dronekeys': ('panel', 2105, 1055, 290, 425),
    'stick': ('panel', 30, 1140, 300, 315),
    'rec': ('panel', 1497, 222, 98, 180),
    'rec_on': ('recording', 1497, 222, 98, 180),
    'm_play': ('play', 427, 1177, 466, 249),
    'm_quant': ('play', 893, 1180, 622, 244),
    'm_tempo': ('play', 1513, 1180, 477, 244),
    'm_expr': ('expression', 406, 1108, 1588, 313),
    'm_arp': ('arp', 406, 1108, 1588, 313),
    'm_steps': ('steps', 406, 1108, 1588, 313),
    'm_service': ('service', 406, 1108, 1588, 313),
    'm_presets': ('play', 1030, 1419, 545, 67),
    'm_devices': ('devices', 406, 1108, 1588, 378),
    'm_learn': ('midi', 406, 1108, 1588, 378),
    'm_profiles': ('profiles', 1000, 1172, 994, 314),
}


def fig(name, *marks, cap=None, zoom=1):
    return dict(name=name, marks=list(marks), cap=cap, zoom=zoom)


def mk(ids, label=None, n=None, split=False):
    """Outline one control, the box around several, or (split) each of several under one number."""
    return dict(ids=[ids] if isinstance(ids, str) else list(ids), label=label, n=n, split=split)


def row(prefix, part):
    return [f'{prefix}.{part}_{i}' for i in range(1, 6)]


# The creature's dictionary. term / full English name / Chinese name / plain explanation.
TERMS = {
    'ambient': ('Ambient · Drone', '', '氛围音乐 · 持续音音乐', 'Ambient 重视空间和气氛；Drone 用一直延续的长音做音乐，让细小的变化成为主角。'),
    'module': ('模块', 'Module', '', '面板上负责一件事的区域：有的发声，有的控制音量，有的改变音色。模块之间用跳线连接。'),
    'patch': ('跳线', 'Patch Cable', '', '把一个插孔的输出拖到另一个插孔的输入。模块合成器的“编程”就是接线，所以一套接法也叫一个 patch。'),
    'hz': ('Hz', 'Hertz', '赫兹', '每秒重复的次数。440 Hz 就是每秒振动 440 次，是标准 A 音；数字越大，音越高。'),
    'cv': ('CV', 'Control Voltage', '控制电压', '用电压大小去控制参数，例如音高、音量、亮度。Lunar 里的“电压”是软件里的数字。'),
    'gate': ('Gate', '', '门信号', '只有“开”和“关”两种状态的信号。按下琴键时门打开，松开时关闭。'),
    'wetdry': ('Wet · Dry', '', '湿声 · 干声', '湿声经过了效果处理，干声是直接的原声。Lunar 的 DRY 输出专指 VCO A / B。'),
    'vco': ('VCO', 'Voltage-Controlled Oscillator', '压控振荡器', '产生音调的模块，音高可以被电压控制。弹琴时，键盘就是在给 VCO 送音高电压。'),
    'vcf': ('VCF', 'Voltage-Controlled Filter', '压控滤波器', '削掉或突出一部分频率，改变声音的明暗。截止频率可以被电压推动。'),
    'vca': ('VCA', 'Voltage-Controlled Amplifier', '压控放大器', '决定声音通过多少，也就是音量的“阀门”。包络通常就是用来开关 VCA 的。'),
    'lfo': ('LFO', 'Low-Frequency Oscillator', '低频振荡器', '慢到听不出音高的振荡器（常低于 20 Hz），专门用来让别的参数周期性地变化。'),
    'env': ('ENV', 'Envelope', '包络', '一段随时间变化的轮廓：声音怎样升起、保持、落下。它是控制信号，不是声音本身。'),
    'ar': ('AR 包络', 'Attack · Release', '起音 · 释放', '最简单的包络，只有两段：A 是声音升起要多久，R 是关掉后要多久才消失。'),
    'adsr': ('ADSR', 'Attack · Decay · Sustain · Release', '起音 · 衰减 · 持续 · 释放', '四段式包络。A、D、R 是时间，S 是按住时停留的电平——注意 S 不是时间。'),
    'sh': ('S&H', 'Sample and Hold', '采样保持', '每来一个时钟，就记下输入此刻的值，并一直保持到下一个时钟。随机信号会变成一级级台阶。'),
    'fm': ('FM', 'Frequency Modulation', '调频', '用一个信号去推动另一个振荡器的频率。慢了是颤音或跳音，快到音频速度就会长出全新的音色。'),
    'am': ('AM', 'Amplitude Modulation', '调幅', '用一个信号去改变音量的大小。慢的是一强一弱，快的会让音色变粗糙。'),
    'pwm': ('PWM', 'Pulse-Width Modulation', '脉宽调制', 'pw 是方波“高”的部分占一个周期多宽；PWM 让这个宽度不停变化，音色会像在流动。'),
    'midi': ('MIDI', 'Musical Instrument Digital Interface', '乐器数字接口', '乐器之间传“哪个音、多大力、哪个旋钮转了”的消息格式。它不传声音本身。'),
    'daw': ('DAW', 'Digital Audio Workstation', '数字音频工作站', '录音、编曲、混音用的软件，比如 Logic、Ableton Live、FL Studio。Lunar 也能作为插件在里面运行。'),
    'bpm': ('BPM', 'Beats Per Minute', '每分钟拍数', '音乐速度的单位。120 BPM 就是每秒两拍。'),
    'cc': ('CC', 'Control Change', '控制变化消息', 'MIDI 里描述“某个旋钮或推子到了哪里”的消息，编号 0–127。'),
    'absrel': ('ABS · REL', 'Absolute · Relative', '绝对 · 相对', 'ABS 旋钮直接报告“我在哪个位置”；REL 编码器只报告“往哪边转了几格”，可以无限转圈。'),
    'voct': ('1 V/oct', 'One Volt per Octave', '每伏一个八度', '控制电压每加 1 V，音高升高一个八度（频率翻倍）。这是模块合成器通用的音高约定。'),
    'octave': ('八度 · 半音', 'Octave · Semitone', '', '高一个八度，频率正好翻倍；一个八度平均分成 12 个半音，钢琴上相邻两个键就差一个半音。'),
    'cent': ('音分', 'Cent', '', '比半音更细的音高单位：100 音分 = 1 个半音。微分音就是用到半音之间的音。'),
    'beating': ('拍频', 'Beating', '', '两个很接近的频率叠在一起，会周期性地互相加强、抵消，听起来像声音在“呼吸”。'),
    'harmonics': ('谐波', 'Harmonics', '', '一个音里除了最低的基频，还有基频 2 倍、3 倍……的成分。谐波越多，声音越亮、越尖。'),
    'aliasing': ('混叠', 'Aliasing', '', '数字声音有一个最高可表示频率。超过它的成分会被“折”回低处，变成不该有的刺耳杂音。'),
    'polyblep': ('polyBLEP', 'polynomial Band-Limited stEP', '多项式带限阶跃', '锯齿波每次突然跳回时最容易产生混叠。polyBLEP 在跳变处补一小段平滑修正，经典 Drone 就用它。'),
    'blepblamp': ('BLEP · BLAMP', 'Band-Limited Step · Ramp', '带限阶跃 · 带限斜坡', 'BLEP 修补波形的突然跳变（锯齿、方波），BLAMP 修补斜率的突然转折（三角波尖角），都为了减少混叠。'),
    'negistor': ('张弛振荡器', 'Relaxation Oscillator', '', '电容慢慢充电、到点后瞬间放电，周而复始，得到一条弯弯的锯齿波。经典 Drone 模拟的负阻管振荡器就是这一类。'),
    'drift': ('慢漂移', 'Random Walk Drift', '随机游走', '每个发生器的音高都在极慢地随机游走，又被拉回中心（Ornstein–Uhlenbeck 过程），像真实电路一样永远不完全稳定。'),
    'ratio': ('纯律叠音', 'Just-Intonation Stack', '', '五个发生器默认按 1 : 1.5 : 2 : 3 : 4 的整数比叠在一起，也就是根音、五度、八度……听起来很融合。'),
    'photo': ('光敏电阻', 'CdS Photoresistor', '', '受光越多电阻越小。它的反应分两段：先快速变化一大截，剩下的一小段要几秒钟才慢慢稳定。Lunar 模拟了这种脾气。'),
    'depth': ('调制深度', 'Modulation Depth', '', '控制信号能把参数推多远。同一个 LFO，深度小是轻轻晃动，深度大就是大幅摆动。'),
    'schmitt': ('施密特触发器', 'Schmitt Trigger', '', '有上、下两个门限的开关：升过上门限才翻转，降过下门限才翻回。让它反复充放电，就成了一个振荡器。'),
    'noise': ('白噪声', 'White Noise', '', '每一刻都是随机值的声音，所有频段能量差不多，像电视雪花、风声、海浪声。'),
    'prng': ('伪随机数', 'Pseudo-Random Number', '', '电脑没有真正的“随机”，而是用公式算出看起来毫无规律的数列。Lunar 的噪声和随机都来自它。'),
    'divider': ('分频器', 'Clock Divider', '', '每收到 N 个时钟脉冲，才放行一个。N 越大，节奏越慢。'),
    'vcocore': ('三角波核心 VCO', 'Triangle-Core VCO', '', '内部先产生三角波，再变形出其它波形。Lunar 参照的是 AS3340 一类振荡器芯片——经典模拟合成器常用的设计。'),
    'morph': ('波形渐变', 'Waveform Morphing', '', '不是只能在几种波形之间跳，而是在相邻两种之间连续混合，旋钮停在中间也有自己的音色。'),
    'sub': ('次振荡器', 'Sub-Oscillator', '', '比主音低一个八度的辅助声音，用来加厚低频。Lunar 的 SUB 与主振荡器相位锁定，永远同步。'),
    'loop': ('包络自循环', 'Looping Envelope', '', '包络走完 A 和 R 后自动重来，不用按键也能不停起伏——包络于是变成了一个形状特别的 LFO。'),
    'linexp': ('线性 · 指数 FM', 'Linear · Exponential FM', '', '线性 FM 按“加多少赫兹”推频率，音色稳定；指数 FM 按“升降几个音”推，深了音高会整体跑偏。'),
    'sync': ('硬同步', 'Hard Sync', '', '另一路振荡器每走完一圈，就强迫这一路从头开始。被同步的那一路改音高时，会出现撕裂般扫动的音色。'),
    'phase': ('相位', 'Phase', '', '波形“走到这一圈的哪里”了，从 0 到 360 度。同步就是把相位强行归零。'),
    'normal': ('常态连接', 'Normalled Connection', '', '没插线时，面板背后已经替你接好的默认线路。插入新线，就会替换掉对应的默认来源。'),
    'preamp': ('前级放大 · 饱和', 'Preamp · Saturation', '', '前级把很小的信号放大；推得太大时波形被柔和地压扁（饱和），声音变厚、谐波变多。'),
    'follower': ('包络跟随器', 'Envelope Follower', '', '把一段声音的响度变成控制曲线：先取绝对值（整流），再抹平抖动（平滑）。'),
    'selfosc': ('自激振荡', 'Self-Oscillation', '', '滤波器的共振开得够大时，内部反馈自己就能维持振荡，不需要输入也会发出一个纯音。'),
    'resynth': ('再合成', 'Resynthesis', '', '先“听”输入的音高和响度，再用新的声源重新弹出来。输入越清楚，跟得越准。'),
    'velocity': ('力度 · 触后', 'Velocity · Aftertouch', '', '力度是按下那一瞬间有多重；触后是按住之后继续往下压的力量。'),
    'arp': ('琶音器 · 音序器', 'Arpeggiator · Sequencer', '', '琶音器把你按住的几个音轮流弹出来；音序器按事先写好的步骤，一步一步自动演奏。'),
    'quant': ('量化器', 'Quantizer', '', '把任意音高“吸”到最近的允许音上。选定音阶后，乱弹也不会跑调。根音决定音阶从哪里开始。'),
    'glide': ('滑音 · 颤音', 'Portamento · Vibrato', '', '滑音让音高从一个音平滑滑到下一个；颤音让音高在中心附近快速轻微摆动。'),
    'interval': ('音程 · 移调', 'Interval · Transpose', '', '音程是两个音的距离（如 +7 半音是纯五度）。整句移调保持音程不变，只换起点。'),
    'rest': ('休止', 'Rest', '', '音乐里不发声的一拍。节奏感常常来自“哪里不响”。'),
    'ppqn': ('MIDI Clock', '24 PPQN', '每四分音符 24 个脉冲', '设备之间同步速度的 MIDI 消息：每拍发 24 个“滴答”。Lunar 收到后换算成自己的步进。'),
    'pan': ('等功率声像', 'Equal-Power Panning', '', 'PAN 把声音分到左右。等功率曲线让声音从左移到右时，总响度大致不变，中间不会“塌下去”。'),
    'svf': ('状态变量滤波器', 'State-Variable Filter', '', '用两个“积分器”首尾相连搭成的滤波器，可同时取出低通、带通。Lunar 用的是 ZDF（零延迟反馈）写法，旋钮扫得再快也稳定。'),
    'polivoks': ('Polivoks', '', '', '1982 年的苏联合成器，滤波器以凶猛、粗糙出名。它的特点是共振开大时低音不变薄，Lunar 保留了这一点。'),
    'lpbp': ('LP · BP', 'Low-Pass · Band-Pass', '低通 · 带通', '低通让低频通过、削掉高频，声音变暗；带通只留中间一段，像隔着电话听。'),
    'res': ('共振', 'Resonance', '', '把截止频率附近的声音抬高，听起来像“哇”的一声尖峰。开到最大会自激。'),
    'softclip': ('软削波', 'Soft Clipping · tanh', '', '用 tanh（双曲正切）曲线把太高的波峰柔和地压弯，而不是一刀切平，所以失真听起来温暖。'),
    'adaa': ('ADAA', 'Antiderivative Anti-Aliasing', '反导数抗混叠', '失真会制造大量高频，容易混叠。ADAA 不逐点计算曲线，而是算一小段的平均值，用很少的运算压住混叠。'),
    'feedback': ('反馈 · 尾音', 'Feedback · Tail', '', '把处理后的声音再送回输入，一圈圈循环、一点点衰减，就形成延迟回声和混响的尾巴。'),
    'fdn': ('FDN', 'Feedback Delay Network', '反馈延迟网络', '让声音在多条长度不同的延迟线之间互相反馈，模拟房间里无数次反射。Lunar 用了 16 条。'),
    'shimmer': ('Shimmer', '', '微光混响', '把升高（或降低）八度的声音送进混响反馈，尾音里会不断长出闪闪发光的高音。'),
    'granular': ('颗粒移调', 'Granular Pitch Shifting', '', '把声音切成几十毫秒的小颗粒，用不同速度重放再交叉淡化，就能变调而不改变速度。'),
    'ringmod': ('环形调制', 'Ring Modulation', '', '把两路信号直接相乘，得到两者频率的和与差，常常像金属、钟声或机器人声。'),
    'crush': ('降采样 · 降位', 'Sample-Rate · Bit Crushing', '', '故意降低每秒采样次数和每个样本的精度，声音变得粗糙、有颗粒感，像老游戏机。'),
    'unipolar': ('单极 · 双极', 'Unipolar · Bipolar', '', '单极信号只在 0 和正值之间变化；双极信号围绕 0 上下摆动。Lunar 的 LFO 输出是单极 0–10 V。'),
    'clock': ('Clock · Gate', '时钟 · 门', '', 'Clock 是“走下一步”的脉冲；Gate 决定这一步要不要发声。它们都不是音高。'),
    'stepmod': ('步进调制', 'Step Modulation', '', '每一步给参数一个不同的目标值。用在滤波器上，节奏就变成了明暗交替。'),
    'offset': ('偏置', 'Offset', '', '给整路控制信号加一个固定量，让同样的手势围绕不同的中心变化。'),
    'learn': ('MIDI Learn', '', 'MIDI 学习', '点一下屏幕上的旋钮，再动一下实体控制器，两者就自动绑在一起，不用手动查编号。'),
    'preset': ('预设 · 配置', 'Preset · Profile', '', 'Preset 是一组设置的快照；Lunar 的 MIDI Profile 只保存控制器绑定方案，不保存声音。'),
    'thread': ('音频线程', 'Audio Thread', '', '专门按时计算下一小段声音的程序线路。它一旦等待，就会断音、爆音，所以里面不加锁、不读写硬盘。'),
    'queue': ('无锁队列', 'Lock-Free Queue', '', '界面和音频线程之间的传送带：一边放、一边取，双方都不用互相等待。'),
    'smooth': ('参数平滑', 'Parameter Smoothing', '', '旋钮跳到新值时，声音参数用几毫秒追过去，而不是瞬间跳变，避免“咔哒”声。'),
    'signalpath': ('信号路径', 'Signal Path', '', '声音从产生到你耳朵所经过的路线。排查问题时顺着它一段段检查最有效。'),
    'samplerate': ('采样率 · 位深', 'Sample Rate · Bit Depth', '', '采样率是每秒记录多少个点（48 kHz = 4.8 万个）；位深是每个点记得多精细，24-bit 比 16-bit 更细腻。'),
    'fade': ('淡出 · 淡入', 'Fade Out · Fade In', '', 'Lunar 换效果程序时，先用约 15 毫秒把旧程序淡出，清空后换上新程序，再用约 15 毫秒淡入，避免咔哒声。新旧声音不会重叠，所以旧尾音会被切断。'),
}

P = []


def page(ch, title, layout='split', figs=(), blocks=(), try_='', terms=(), more='', audio=None,
         lead='', src='', **extra):
    P.append(dict(ch=ch, title=title, layout=layout, figs=list(figs),
                  blocks=[dict(h=h, lines=list(l)) for h, l in blocks], try_=try_,
                  terms=list(terms), more=more, audio=audio, lead=lead, src=src, **extra))


# ── 开始玩 ──────────────────────────────────────────────────────────────────────────────
page('start', '养一团会呼吸的声音', 'cover',
     blocks=[('LUNAR 24 声音实验室', ['一份边玩边懂的合成器教程']),
             ('今天的目标', ['做出一段 60 秒的声音小世界'])],
     more='这是一份可以随时暂停、动手的教程。先做出一个声音，再一点点认识新的模块。',
     src='README.md：开源 ambient / drone 乐器，独立 App 与 VST3 / AU 插件；参考 Solar 42N，声音凭耳调校。')

page('start', '旋钮，也是你的乐器', 'split', lead='Lunar 24 是一台装在电脑里的氛围合成器。',
     figs=[fig('panel')],
     blocks=[('它能做什么', ['用持续的长音、噪声和可以弹奏的振荡器，做氛围、配乐和实验声音。',
                            '没有“正确答案”，转动旋钮本身就是演奏。']),
             ('这份教程怎么用', ['先听一个声部，再动一个控件。',
                               '每页底部都有“动手试试”，做完再翻页。'])],
     try_='每次转旋钮前先猜一猜：会变的是音高、音色、音量，还是节奏？',
     terms=['ambient'],
     more='一段长音也可以有音乐的结构：它什么时候出现、怎样变化、何时让出空间，都由你决定。',
     src='ARCH §1；machine_runtime.h')

page('start', '第一个声音：DRONE 1', 'split', figw=0.36,
     figs=[fig('dronekeys', mk('drone_key.1', label='DRONE 1'), cap='开机时六个键都是暗的')],
     blocks=[('先接好耳机或音箱', ['Mac：菜单 Lunar 24 → Preferences…',
                                  'Windows：File → Preferences…（Ctrl+,）',
                                  '选好 Output Device 和输出通道，点 Apply。主音量先放低。']),
             ('让它慢慢出现', ['开机提示音结束后，六个 Drone 默认都是关的。',
                              '点右下角 DRONE VOICES 的 1，等约 3 秒听它升起；再点一次，听它慢慢消失。'])],
     try_='旋钮可以拖动；按住 Shift 拖动是微调；双击回到默认值。',
     terms=['ar'],
     more='想从同一个起点开始练习，可以在键盘菜单里用 RESET PANEL（要连续确认两次）。它会清掉当前所有旋钮和跳线，先把喜欢的作品录下来。作为 DAW 插件使用时，音频设备在 DAW 里设置，不用 Preferences。',
     src='ARCH §1.2、2.7；main.rc_mac_dlg；main.rc；main.mm；STATUS')

page('start', '面板地图：六个区域', 'map', figs=[fig('panel')],
     blocks=[('声音从哪里来', ['① 两侧上排：经典 Drone 1、2、4、5', '② 两侧中排：Drone 3 / 6 与 VCO A / B']),
             ('声音在哪里被加工', ['③ 中间：混音器、滤波器、失真和效果器']),
             ('谁来让它动起来', ['④ 下排：LFO、摇杆输出、5 步音序器、外部输入', '⑤ 底部：触摸键盘和摇杆', '⑥ 右下：Drone 开关、MUTE、MIDI'])],
     try_='不用一次记住所有名字。先在面板上找到 DRONE 1、VCO A、混音器和 MASTER VOLUME。',
     terms=['module'],
     more='地图只帮你找区域，具体的控件会在后面的页面放大讲。',
     src='ARCH §1；panel_ui_layout.h；panel_art.generated.h 模块边框')

page('start', '面板上的缩写，一页看懂', 'abbr',
     lead='合成器面板空间小，名字常常是英文缩写。看不懂时回到这一页。',
     try_='在面板上找到上面任意三个缩写，猜猜它们负责什么，再翻到对应的页面验证。',
     more='每个缩写在第一次出现的页面里，小精灵“嗡嗡”还会再解释一遍。',
     src='spec/machine/lunar24.json 模块与参数名')

page('start', '发声和控制，是两种工作', 'split',
     figs=[fig('vcoa', mk('vco_a.v_oct_in', label='音高 CV 输入'), mk(['vco_a.cv_in', 'vco_a.pwm_in', 'vco_a.sync_in'], label='其它控制输入'))],
     blocks=[('Audio：声音本身', ['振荡器不停地重复一个波形。',
                                '每秒重复的次数叫频率，单位 Hz；重复得越快，听起来越高。']),
             ('CV 与 Gate：指挥怎样发声', ['CV（控制电压）是连续变化的控制数值，比如“音高要多高”。',
                                         'Gate（门）只有开和关：琴键按下就开，松开就关。'])],
     try_='在图里找到 1 号音高 CV 输入和 2 号其它控制输入。这里的“电压”只是软件里的数字，不需要真的电线。',
     terms=['cv', 'hz'],
     more='Audio 和 CV 的区别在于“你拿这个信号去做什么”。可以听见的音频也能接到控制输入上，产生非常快的参数变化——后面的 FM 就是这样。',
     src='ARCH §1.4、4；signal.h；vco.h')

page('start', '声音到耳朵，中间经过什么', 'flow', diagram='signal',
     blocks=[('主声路 WET', ['所有声源先进入 10 路混音器，再经过左右滤波器、失真、左右效果器，最后由 MASTER 控制总音量。']),
             ('另一条路 DRY', ['只有 VCO A / B 的声音，经过音量包络后直接输出，绕过混音、滤波、失真和效果器。'])],
     try_='把效果器的 BLEND 转到最小，只是拿掉效果声；滤波和失真仍然在主声路里。',
     terms=['wetdry', 'signalpath'],
     more='普通双声道耳机或声卡听到的是 WET L / R。独立 App 在声卡输出通道足够多时，也可以把 DRY 送到另外两个通道；作为插件时只输出 WET 立体声。',
     src='ARCH §1.1、1.2、4.6；machine_runtime.h；stream_plan.h')

# ── 经典 Drone ──────────────────────────────────────────────────────────────────────────
page('classic', '一个声部，五个发生器', 'split', audio='01-classic',
     figs=[fig('drone1', mk(row('drone_1', 'mute') + row('drone_1', 'mod'), label='五个发生器（五列）'))],
     blocks=[('Drone 1、2、4、5 是一家', ['每个经典声部里有五个发生器：两个低音、一个中音、两个高音。',
                                       '它们自己跑，不跟随键盘音符。']),
             ('里面在发生什么', ['每个发生器像电容充电、放电一样，画出一条弯弯的锯齿波。',
                               '五个音带着小偏差和慢漂移，叠成会微微起伏的长音。'])],
     try_='只开 DRONE 1，把效果器 BLEND 转到最小，先听它本来的纹理。',
     terms=['negistor', 'polyblep'],
     more='五个发生器默认按 1 : 1.5 : 2 : 3 : 4 的比例叠起来，每个还带约 ±1% 的固定偏差和几音分的慢漂移，所以它永远不会完全静止。锯齿每次跳回时，软件用 polyBLEP 修正，减少数字杂音。',
     src='ARCH §2.1、2.2；drone_bank.h（负阻管模型、OU 漂移、polyBLEP）')

page('classic', 'TUNE 与 MUTE：现场改写叠音', 'split',
     figs=[fig('drone1', mk(row('drone_1', 'mute'), label='MUTE'), mk(row('drone_1', 'tune'), label='TUNE'))],
     blocks=[('每一列就是一个发生器', ['TUNE 改这一列的音高，范围上下各 12 个半音。',
                                     'MUTE 只让这一列静音；逐列开关，就是把叠音拆开再组合。']),
             ('为什么微调也很好听', ['两个接近的音叠在一起，会一会儿加强、一会儿抵消。',
                                   '这种起伏叫拍频，是 Drone “呼吸感”的来源。'])],
     try_='打开第 2–5 列的 MUTE，只听第 1 列；再一列一列取消 MUTE，听叠音怎样变厚。',
     terms=['beating', 'octave'],
     more='TUNE 的变化带约 30 毫秒的平滑，所以转动时音高是滑过去的。恢复：取消刚才打开的 MUTE，双击改过的 TUNE。注意 MUTE 灯亮表示这一列“不出声”。',
     src='ARCH §2.3；drone_bank.h')

page('classic', 'VOLT：从降调到互相调频', 'split', audio='01-classic',
     figs=[fig('drone1', mk('drone_1.volt', label='VOLT'))],
     blocks=[('前半圈：整组往下沉', ['VOLT 把五个发生器一起降调，整个行程约 5 个八度。',
                                   '它不是音量旋钮！']),
             ('后半圈：五个音连成环', ['过了中点，每个发生器开始推动下一个的频率，五个首尾相接。',
                                     '越往后，声音越粗糙、越复杂。'])],
     try_='先把主音量放低，再从最小慢慢转过中点：听“更低”和“更粗糙”是从哪里开始同时出现的。',
     terms=['fm'],
     more='很低的音可能听不太清，但振荡器还在运行。恢复时双击 VOLT。试听里第 4 秒改 TUNE，第 8 秒改 VOLT。',
     src='ARCH §2.4；drone_bank.h（mutualFM）；machine_runtime.h')

page('classic', 'MOD 与光敏眼：用手势改变音高', 'split',
     figs=[fig('drone1', mk(row('drone_1', 'mod'), label='MOD'), mk('drone_1.eye', label='光敏眼'))],
     blocks=[('先选谁参与变化', ['每一列的 MOD 决定这个发生器是否听光敏眼和 CV 的指挥。',
                               '关掉 MOD 也不会消除它自己的慢漂移。']),
             ('鼠标扮演你的手', ['按住白色圆顶就是“遮住”，往上拖更暗；左右拖像手指扫过。',
                               '遮得越暗音越低，而且每一列反应不同。'])],
     try_='只开第 1 列的 MOD，再按住圆顶；然后多开几列 MOD，比较一下。',
     terms=['photo', 'depth'],
     more='光敏眼完全由鼠标模拟，不会打开摄像头或读取环境光。软件模拟了真实光敏电阻的脾气：变化先快后慢，在暗处待久了，回到明亮时也会更慢；连手的轻微颤抖也算了进去。经典声部的 CV 旋钮开满时，10 V 大约移动一个八度。恢复时关掉刚打开的 MOD。',
     src='ARCH §2.5、2.6；photo_sensor.h（CdS、vactrol 式快慢响应）；panel_editor.h')

page('classic', '开关与包络：声音怎样进来、离开', 'split',
     figs=[fig('drone1', mk(['drone_1.gate_hold', 'drone_1.gate_in'], label='HOLD / GATE'),
               mk(['drone_1.att', 'drone_1.rls'], label='ATT / RLS'), mk('drone_1.env_out', label='env 输出'))],
     blocks=[('三种打开声部的方法', ['DRONE VOICES 按一下开、再按一下关。',
                                   'HOLD 让声部一直开着；GATE 插上线后，由外部信号代替按键。']),
             ('ATT 与 RLS 画出音量轮廓', ['ATT（起音）决定升起要多久，RLS（释放）决定消失要多久。',
                                       '发生器一直在跑，包络只管音量；env 插孔能把轮廓送给别的模块。'])],
     try_='关掉 HOLD，把 ATT 和 RLS 都开大，再开关声部，听它慢慢进来、慢慢离开。',
     terms=['ar', 'env'],
     more='经典声部用的是直线升降的 AR 包络，范围约 1 毫秒到 10 秒，旋钮正中约 1.25 秒。释放途中重新打开，会从当前音量继续升起，不会先跳到零。GATE 拔线后恢复按键控制。',
     src='ARCH §2.7；drone_bank.h；ar_envelope.h')

page('classic', '小实验：一组叠音的四种表情', 'mission', audio='01-classic',
     blocks=[('准备', ['只开 DRONE 1，效果器 BLEND 最小。', 'HOLD 关，确认 GATE 上没有插线。']),
             ('30 秒的动作', ['① 逐列 MUTE ② 微转一个 TUNE', '③ 慢慢转 VOLT ④ 用光敏眼收尾',
                            '一次只改一样，给耳朵留两三秒。'])],
     try_='听完说一说：哪些动作改变了叠音，哪些改变了起伏或粗糙感？',
     more='结束后：双击改过的旋钮回默认，关掉新开的 MOD，再关 DRONE 1。想留下的版本先录下来（见“REC”一页）。',
     src='ARCH §2；MANUAL_TESTS')

# ── Drone 3 / 6 ─────────────────────────────────────────────────────────────────────────
page('papa', '同样叫 Drone，内部却不同', 'split', audio='02-fm-am',
     figs=[fig('drone3', mk('drone_3.pitch', label='音调'), mk('drone_3.rate', label='LFO'), mk('drone_3.noise', label='噪声'))],
     blocks=[('一条音调、一个调制、一路噪声', ['一个音频振荡器发出三角波音调；一个低频振荡器（LFO）产生方波控制信号。',
                                         '白噪声也加进来，最后一起经过 AR 包络。']),
             ('“来回翻转”的感觉从哪来', ['振荡器在上、下两个门限之间来回充放电，叫施密特触发器振荡器。',
                                       '方波 LFO 让调制在两种状态间切换。'])],
     try_='只开 DRONE 3，把 NOISE 转到最小、fm 和 am 都关掉，先只听音调。',
     terms=['schmitt', 'lfo'],
     more='三角波的尖角处会产生数字杂音，软件用 BLAMP 修正这种“斜率的突然转折”。内部 LFO 取三角波的正负号得到方波。Drone 3 和 Drone 6 各有一套独立的振荡器。',
     src='ARCH §3.1；schmitt_osc.h（BLAMP）；machine_runtime.h')

page('papa', 'PITCH 与 rate：谁管高，谁管快', 'split',
     figs=[fig('drone3', mk(['drone_3.pitch', 'drone_3.hi_low'], label='PITCH · hi/low'), mk(['drone_3.rate', 'drone_3.rate_switch'], label='rate · 1:10'), mk('drone_3.mod', label='mod'))],
     blocks=[('PITCH 管音调高低', ['hi / low 先选音区：low 约 C0–C4，hi 约 E3–E7。',
                                 'PITCH 在音区里连续调；转到最小，音调就关掉了。']),
             ('rate 管变化快慢', ['rate 调内部 LFO 的速度，1:10 开关再把它乘十。',
                                'mod 决定 FM / AM 变化有多深。rate 转到零会停在最后的状态。'])],
     try_='先打开 fm，mod 开一点，再转 rate：分清“跳得更快”和“音变高”的区别。',
     terms=['lfo', 'hz'],
     more='rate 为零时 LFO 停在最后的状态，所以可能留下一个固定的偏移。外接 S&H 时钟仍能继续更新。恢复：关掉 fm，双击改过的旋钮。',
     src='ARCH §3.2；machine_runtime.h')

page('papa', 'FM 改音高，AM 改音量', 'split', audio='02-fm-am',
     figs=[fig('drone3', mk('drone_3.fm', label='fm'), mk('drone_3.am', label='am'), mk('drone_3.mod', label='mod'))],
     blocks=[('FM：调频', ['方波轮流把音高往上、往下推。',
                         '慢的时候像两个音来回跳；快的时候长出新音色。mod 越大，两个音离得越远。']),
             ('AM：调幅', ['方波轮流改变音量。',
                         'mod 小是轻微强弱，mod 大更像一下下被切断。'])],
     try_='NOISE 最小，先只开 fm，再只开 am，最后两个一起开，比较三种感觉。',
     terms=['fm', 'am'],
     more='FM 最深可以向上、向下各推约 3 个八度。AM 的两种音量是 1−mod 和 1+mod，所以只有 mod 开满时才会出现完全无声的一侧。噪声在这两种调制之后才混进来。',
     src='ARCH §3.2；machine_runtime.h；fm_am.h')

page('papa', 'NOISE：风、沙沙声和空气', 'split', audio='03-noise',
     figs=[fig('drone3', mk('drone_3.noise', label='NOISE'), mk('drone_3.pitch', label='PITCH'))],
     blocks=[('白噪声没有固定的周期', ['软件不停地产生随机数，听起来就是“沙——”。',
                                     'NOISE 只决定它混进声部多少；PITCH 最小、NOISE 打开，就只剩噪声。']),
             ('它走自己的小路', ['噪声不受本声部 fm / am 的影响，但会经过 ATT / RLS 和后面的滤波器。',
                               '慢慢转滤波器，就像风从明亮变得低沉。'])],
     try_='只听 DRONE 3：PITCH 最小，NOISE 开一点，然后慢慢转左右两个滤波器的 FREQ。',
     terms=['noise', 'prng'],
     more='即使 NOISE 转到最小，内部的噪声仍然在给 S&H 使用。试听在第 6 秒降低滤波器的截止频率，注意噪声从亮变暗。做完后双击 PITCH，让音调回来，下一页还要用。',
     src='ARCH §3.2；drone_noise.h（seeded PRNG）；machine_runtime.h')

page('papa', 'S&H：把随机变成台阶', 'split',
     figs=[fig('drone3', mk('drone_3.divider', label='divider'), mk('drone_3.sh_out', label='S&H OUT'), mk(['drone_3.noise_in', 'drone_3.clock_in'], label='IN · clock'))],
     blocks=[('采样保持只做两件事', ['时钟一来，就记下输入此刻的值；没有新时钟时，一直保持这个值。',
                                   '默认记录的是白噪声，所以 S&H OUT 输出的是随机台阶。']),
             ('divider 管更新节奏', ['没接外部 clock 时，它把内部 LFO 分频后当作时钟。',
                                   'divider 越大，台阶换得越慢；接上外部 clock 后它就不管了。'])],
     try_='还没把 S&H OUT 接到任何地方时，只转 divider 不会改变声音——下一页我们就把它接起来。',
     terms=['sh', 'divider'],
     more='IN 插孔插线后，会替换默认的噪声输入。内部 divider 范围是 1 到 16。',
     src='ARCH §3.4；sample_hold.h；machine_runtime.h')

page('papa', '第一根跳线：随机音高', 'split', audio='04-sample-hold',
     figs=[fig('drone3', mk('drone_3.sh_out', n='1'), mk('drone_3.cv_in', n='2'), cap='从 ① S&H OUT 拖一根线到 ② cv')],
     blocks=[('接法和起点', ['只开 DRONE 3：双击 PITCH 让音调回来（上一页把它关了），fm、am 关，NOISE 最小。',
                            '从 ① 拖线到 ②；rate 放中间，divider 先放最小。']),
             ('接着演奏它', ['慢慢加大 divider，听音高换得越来越慢。',
                           '转 PITCH，整段随机旋律跟着移到别的音区。'])],
     try_='恢复：把线从 cv 插孔拖到空白处松开。随机音高停了，音调还在。',
     terms=['patch', 'voct'],
     more='① 是底部最右边的 S&H OUT，② 是中间那排的 cv 输入。随机电压可能跨很大的音区，先保持低音量。这些随机音没有经过键盘的音阶量化，所以不一定“在调上”。试听第 6 秒增大 divider。',
     src='ARCH §3.4；machine_runtime.h；MANUAL_TESTS')

# ── VCO A / B ───────────────────────────────────────────────────────────────────────────
page('vco', '跟着你的键盘，弹出音高', 'split',
     figs=[fig('vcoa', mk('vco_a.v_oct_in', label='1v/oct'), mk('vco_a.tune', label='tune'))],
     blocks=[('VCO 的工作', ['VCO 是“压控振荡器”：音高可以被控制电压指挥。',
                            '1v/oct 输入每多 1 V，频率翻一倍，也就是高一个八度。']),
             ('线已经替你接好了', ['键盘的音高（V/OCT）默认同时送到 A 和 B。',
                                 '键盘的 Gate 打开 A、B 各自的音量包络，按一个键两个 VCO 一起响。'])],
     try_='关掉所有 Drone，把混音器 VCO B 的 VOL 转到最小，只听 A，在键盘上弹几下。',
     terms=['vco', 'normal'],
     more='“每伏一个八度”是指数关系：控制量每增加相同的一份，频率就乘上相同的倍数。按键主要是打开 VCA（音量阀门），不会每次都让波形从头开始。',
     src='ARCH §4.1、4.6；vco.h；spec lunar24.json')

page('vco', '波形决定声音的轮廓', 'split', audio='05-waveforms',
     figs=[fig('vcoa', mk('vco_a.morph', label='MORPHING WAVEFORM'))],
     blocks=[('不同波形，不同性格', ['正弦最纯，三角柔和；锯齿谐波丰富，很亮；方波空空的，像单簧管。',
                                 '相邻的两个图标之间，软件会把两种波形连续混合。']),
             ('转一圈会经过什么', ['从锯齿到反锯齿之间会经过方波区域。',
                                 '最后一段是正弦到三角的连续变形。'])],
     try_='关掉 sub，BLEND 最小，慢慢转大旋钮：一边看图标，一边听音色怎样变。',
     terms=['harmonics', 'vcocore'],
     more='大旋钮不是六档开关，图标之间也有能用的音色。锯齿和方波的跳变处用 BLEP、三角的尖角用 BLAMP 修正，减少混叠。试听每 3 秒依次换正弦、三角、锯齿、方波。',
     src='ARCH §4.1；vco_wave_map.h；vco.h（三角核心、BLEP / BLAMP）')

page('vco', 'pw 与 pwm：让方波变瘦', 'split', figw=0.6,
     figs=[fig('lfoa', mk('lfo_a.cv_out', n='1')), fig('vcoa', mk('vco_a.pwm_in', n='2'))],
     patch=True,
     blocks=[('pw 是方波的胖瘦', ['先把大旋钮放在方波图标。',
                               'pw 改变一个周期里“高”的部分占多少；50% 左右最对称。']),
             ('pwm 让胖瘦动起来', ['把 ① LFO A 的输出接到 ② VCO A 的 pwm 输入。',
                                 'pwm 旋钮决定这根线影响多少：慢的像音色在流动。'])],
     try_='只听 A：方波，pw 居中；接好线后，把 pwm 从零一点点加大。',
     terms=['pwm'],
     more='LFO A 输出 0 到 10 V，所以它会把 pw 往一个方向推，而不是围着中心摆。恢复：拔线，pw 回中，pwm 回零。如果大旋钮离方波太远，变化可能不明显。',
     src='ARCH §4.4；machine_runtime.h；vco.h')

page('vco', 'tune、八度和 sub 各管什么', 'split',
     figs=[fig('vcoa', mk('vco_a.tune', label='tune'), mk('vco_a.oct_sel', label='oct'), mk('vco_a.sub_sel', label='sub'))],
     blocks=[('主振荡器的音区', ['tune 连续微调，范围共一个八度。',
                               'oct 开关在 +3 / 中间 / low 之间换大音区。它们都在移动同一个音。']),
             ('sub 真的多了一个声音', ['sub 加入一个低八度的方波，混入量固定。',
                                    '它能补厚低频，也可能盖住其它声部。'])],
     try_='按住同一个键：先切换 sub，再改 oct。分清“多加一层”和“整体移动”。',
     terms=['sub', 'octave'],
     more='sub 和主振荡器一起经过 VCA；面板上原始的 osc 输出不包含 sub。恢复：tune 双击回默认，八度回 low。',
     src='ARCH §4.2；machine_runtime.h；vco.h（sub 相位锁定）')

page('vco', 'ADSR：画出一个音的起落', 'split', figw=0.56,
     figs=[fig('enva', mk(['envelope_a.a', 'envelope_a.d', 'envelope_a.s', 'envelope_a.r'], label='A · D · S · R'))],
     blocks=[('按下之后：A、D、S', ['A（起音）：升到最高点要多久。',
                                  'D（衰减）：从最高点落到 S 要多久。',
                                  'S（持续）：按住时停在多高——它是高度，不是时间。']),
             ('松开之后：R', ['R（释放）：松手后要多久落到零。',
                            '长 R 会让一串短音连成一片。'])],
     try_='只听 A：先短 A、低 S、短 R 弹几下；再把 A 和 R 开大，比较“短音”和“铺底”。',
     terms=['adsr', 'vca'],
     more='这里的 A、D、R 控制的是“指数趋近”的速度，所以不是到了某个时间点就刚好结束。包络除了控制 VCA，也能从 env 插孔输出给别的模块用。',
     src='ARCH §4.6；envelope_generator.h')

page('vco', 'HOLD、自循环和 vca cv', 'split', figw=0.56,
     figs=[fig('enva', mk('envelope_a.hold', label='hold'), mk('envelope_a.self_gen', label='自循环'), mk('vco_a.vca_ctl', label='vca cv'))],
     blocks=[('HOLD 和自循环不一样', ['HOLD 让 VCA 一直开着，音量保持不变。',
                                    '自循环让包络在 A 和 R 之间来回，不按键也能一起一落。']),
             ('插上 vca cv 就换了指挥', ['外部 CV 代替包络控制音量：0 V 关，约 8 V 全开。',
                                     '这时 HOLD 也盖不过外部控制。'])],
     try_='试自循环时从短 A、短 R 开始；不想让它一直响，就把自循环和 HOLD 都关掉。',
     terms=['loop'],
     more='HOLD 不会冻结 env 输出。外接的 vca cv 优先于内置包络和 HOLD，拔线后才恢复内部控制。',
     src='ARCH §4.6；machine_runtime.h；envelope_generator.h')

page('vco', 'A 不出声，也能改变 B', 'split', audio='06-vco-fm',
     figs=[fig('vcob', mk('vco_b.cv_amt', label='cv amt'), mk('vco_b.lin_exp', label='lin / exp'))],
     blocks=[('背后有一根默认的调频线', ['VCO A 的原始波形默认接进 B 的 cv 输入。',
                                     'B 的 cv amt 默认为零，所以一开始听不到；慢慢打开，B 就被 A 推动。']),
             ('lin 和 exp：两种推法', ['lin 按频率比例推，exp 按音程推。',
                                    '推的速度到了音频范围，就会长出新的频率成分。'])],
     try_='混音器只留 B，按住一个键；慢慢转 B 的 cv amt，再换 A 的波形听听。',
     terms=['linexp', 'normal'],
     more='A 的原始波形取在音量包络之前，所以就算把 A 的混音音量关掉，它仍然能调制 B。这里的线性 FM 不会穿过零频率，往负方向推得太远会停在零。',
     src='ARCH §4.3、4.6；machine_runtime.h；vco.h')

page('vco', 'SYNC：强迫波形重新开始', 'split', figw=0.6,
     figs=[fig('vcob', mk('vco_b.vco_out', n='1')), fig('vcoa', mk('vco_a.sync_in', n='2'))],
     patch=True,
     blocks=[('这一次 B 指挥 A', ['把 ① VCO B 的 osc 输出接到 ② VCO A 的 sync。',
                               'B 每走完一圈，A 就被迫从头开始，不管自己走没走完。']),
             ('tune 变成演奏动作', ['只听 A，B 的音高不动，慢慢转 A 的 tune。',
                                  '会听到“撕裂般”扫动的音色。太刺耳就先把滤波器调暗。'])],
     try_='接线前先把 B 的 cv amt 归零；结束时从 sync 插孔拔掉线。',
     terms=['sync', 'phase'],
     more='先关掉 B 的 cv amt，避免默认的 A→B 调频和这次同步混在一起。只有 A 有 sync 输入；B 那个位置的插孔是 osc 输出。',
     src='ARCH §4.5、4.8；vco.h（hard sync 仅 A）；machine_runtime.h')

page('vco', '小实验：一个长音的三个版本', 'mission', audio='06-vco-fm',
     blocks=[('版本一：柔软', ['只听 A，选正弦，sub 关。', '慢 A、慢 R，让一个音像雾一样出现。']),
             ('版本二、三：流动和金属感', ['二：方波 + 慢 PWM，听颜色慢慢变。', '三：拔掉实验线，只听 B，加大 A 对 B 的调频。',
                                       '每个版本录几秒，拿来比较，而不是全叠在一起。'])],
     try_='如果你能说清“声源是谁、调制源是谁、被改变的参数是哪个”，你已经开始懂合成了。',
     more='每次切换版本前，关掉 HOLD 和自循环，拔掉实验跳线，避免把上一个版本的变化带进来。',
     src='ARCH §4；vco.h')

# ── 外部声音 ────────────────────────────────────────────────────────────────────────────
page('ext', '把自己的声音送进 Lunar', 'split', figw=0.6,
     figs=[fig('vcob', mk('vco_b.vco_out', n='1')), fig('external', mk('preamp.ext_source_in', n='2'))],
     patch=True,
     blocks=[('独立 App 的两个入口', ['声卡输入 1 → 混音器的 EXT.AUDIO 通道；输入 2 → PREAMP（前级）。',
                                    '单声道输入会同时送给两者。用耳机，免得麦克风收到音箱的声音。']),
             ('PREAMP 把小信号放大', ['GAIN 最小时无声，加大后会进入柔和的饱和。',
                                    'EXT SOURCE 插线会替换声卡输入。'])],
     try_='没有麦克风也能练：把 ① VCO B 的 osc 接到 ② EXT SOURCE，混音器里 B 的 VOL 关掉，只听 PREAMP。',
     terms=['preamp'],
     more='PREAMP 的输出同时送到混音器和包络跟随器。GAIN 在中点时已经约放大 50 倍，要从很小开始加。作为插件时没有外部音频输入，但这根内部跳线照样能用。',
     src='ARCH §1.3、7；preamp.h；stream_plan.h')

page('ext', '包络跟随器：让声音指挥声音', 'split', figw=0.62,
     figs=[fig('external', mk('env_follower.env_out', n='1')), fig('processing', mk('vcf.cv_l_in', n='2'), mk(['vcf.l_mod', 'vcf.r_mod'], label='MOD L / R'))],
     patch=True, stack=True,
     blocks=[('env：跟着响度走', ['先把声音变成正值，再抹平抖动，得到一条响度曲线。',
                               'attack 管上升快慢，release 管回落快慢。']),
             ('gate：把响度变成开关', ['另一个更快的检测器判断“够不够响”。',
                                   '越过门限就打开，变小就关闭，不受 attack / release 影响。'])],
     try_='PREAMP 有声音后，把 ① env 接到 ② 滤波器 CV L，MOD L / R 开一点：越响越亮。',
     terms=['follower'],
     more='env 输出约 0–10 V，gate 输出约 0–8 V。输入太小时 gate 不触发是正常的，先调整输入或 GAIN。结束时拔线，把 GAIN 归零。',
     src='ARCH §7.3；envelope_follower.h（整流 + 非对称一阶平滑）')

page('more', '没有按键，也可能有声音', 'split', audio='08-generator', figw=0.58,
     figs=[fig('generator', mk('effector.slot', label='GENERATOR 卡带'), mk(['effector.x', 'effector.y', 'effector.z'], label='X · Y · Z'))],
     blocks=[('GENERATOR 卡带自己会发声', ['FM tone：X、Y 是两路音高，Z 是调频深度。',
                                       'Ramp：慢波推动锯齿音高。Voice：锯齿和噪声经过滤波。']),
             ('另外两种“自己响”', ['滤波器共振开到很大时会自激，自己发出音调。',
                                 'SYNTEX-1 跟着输入的音高和响度再合成，需要有输入才行。'])],
     try_='没按键也有声音时，除了 HOLD，还要检查 GENERATOR、滤波器共振和效果器的尾音。',
     terms=['selfosc', 'resynth'],
     more='GENERATOR 在主滤波器之后才发声，所以前面的 FILTER 管不到它。BLEND 转到最小可以拿掉效果器的声音。SYNTEX-1 更适合清楚的单音，复杂和弦不一定跟得准。',
     src='ARCH §16、17；effector.h（两个小合成声部）；polivoks_vcf.h')

# ── 键盘演奏 ────────────────────────────────────────────────────────────────────────────
page('keys', '触摸板、电脑键盘与 MIDI', 'wide',
     figs=[fig('keyboard', mk('keyboard.encoder', label='编码器 · 菜单'), mk('keyboard.display', label='OCT 显示'))],
     blocks=[('三条路，弹的是同一个键盘', ['鼠标点触摸板，或用电脑键盘：A W S E D F T G Y H U J… 按半音排列。',
                                       'Z / X 换八度，屏幕显示 OCT。也可以接 MIDI 键盘。']),
             ('压力也是表情', ['鼠标点得越靠下，模拟的压力越大；MIDI 用力度和触后。',
                             '压力输出要接到某个目标上，才会改变那个参数。'])],
     try_='点中间红色的编码器打开 KEYBOARD MENU，按 Esc 关闭。',
     terms=['velocity'],
     more='完整的电脑键排列是 A W S E D F T G Y H U J K O L P ;。如果正在输入文字，先退出输入框，再回面板弹奏。',
     src='ARCH §8、9；computer_keys.h；panel_editor.h')

page('keys', 'PLAY 与 MODE：两层选择', 'split', figw=0.52,
     figs=[fig('m_play')],
     blocks=[('PLAY：键盘怎样分工', ['SINGLE：整个键盘一套设置。',
                                  'TWIN：两半独立演奏，共用设置。SPLIT：两半各有设置。']),
             ('MODE：音符怎样发生', ['KEYBOARD：按下就响。',
                                   'ARPEGGIATOR：把按住的音轮流弹。SEQUENCER：播放写好的音序。'])],
     try_='先停在 SINGLE + KEYBOARD。分成两半以后，还需要接线（后面会讲）。',
     terms=['arp'],
     more='先把 SINGLE + KEYBOARD 玩熟。TWIN 和 SPLIT 都要留意两半的输出怎样分工，“两半键盘”那一页会给出 B 声部的接法。',
     src='ARCH §10、11；keyboard_mode.h；keyboard_menu_view.h')

page('keys', '音阶量化：弹不跑调', 'split', figw=0.52,
     figs=[fig('m_quant')],
     blocks=[('SCALE、ROOT、NOTES', ['SCALE 选音阶，ROOT 选主音，NOTES 单独开关每个音。',
                                   '不在音阶里的音，会被吸到最近的允许音上。']),
             ('同一手势，两种音阶', ['选 Ionian（大调），ROOT 设 C，弹一组。',
                                   '换成 Aeolian（小调）再弹，听情绪的变化。'])],
     try_='量化管键盘、MIDI、琶音和键盘音序；它不会把 Drone 也调到音阶里。回到 SEMITONES 就不再量化。',
     terms=['quant'],
     more='ROOT 移动的是“允许的音阶集合”，不保证每个音都被平移相同距离。标着 NOT MODELLED 的音阶目前不做限制；也可以直接用 NOTES 选自己的音符组合。',
     src='ARCH §13.3、14.1；keyboard_behaviour.h')

page('keys', '滑音、颤音与微分音', 'wide',
     figs=[fig('m_expr')],
     blocks=[('PORTAMENTO 与 VIBRATO', ['GLIDE（滑音）让一个音平滑地走到下一个音。',
                                    'VIBRATO（颤音）让音高在中心附近摆动：先加一点 DEPTH，再调 RATE。']),
             ('每块触摸板都能单独调音', ['Mac：⌘ + 在板上滚动，每格 10 音分；加 ⌥ 改成半音，⌘ + 点击归零。',
                                     '这只改触摸板，不改 MIDI 音符。'])],
     try_='听微分音之前先关掉音阶量化，否则细小的差别会被吸掉。100 音分 = 1 个半音。',
     terms=['glide', 'cent'],
     more='LEGATO 关时每次都滑音；开时只在按键重叠时滑音。VIBRATO 的 DELAY 是“多久后才慢慢加到全深度”。',
     src='ARCH §9.3、12.2、13.1；keyboard_behaviour.h；panel_editor.h')

page('keys', '两半键盘，分别控制 A 和 B', 'wide',
     figs=[fig('jacks_r', mk('keyboard.pressure_out', n='1'), mk('keyboard.gate_right_out', n='2'), zoom=1.8),
           fig('vcob', mk('vco_b.v_oct_in', n='1')), fig('envb', mk('envelope_b.gate_in', n='2'))],
     blocks=[('SPLIT 只是第一步', ['左半保留默认的音高和 GATE LEFT。',
                                 '右半的音高从 PRESSURE 插孔出来，门信号从 GATE RIGHT 出来。']),
             ('右半需要两根线', ['① PRESSURE → VCO B 的 1v/oct；② GATE RIGHT → 包络 B 的 gate。',
                               '菜单右上角 EDITING 切换正在编辑左半还是右半。'])],
     try_='没接这两根线时，B 仍可能跟着左半键盘走——这是常态连接在工作。',
     terms=['normal'],
     more='右侧三个插孔里，中间斜上方那个 PRESSURE 在 SPLIT 时变成右半的音高。恢复：先拔两根线，再回 SINGLE。MIDI 则按每个设备的 SPLIT 音分左右。',
     src='ARCH §10.2；keyboard_side_bank.h；spec lunar24.json')

page('keys', '琶音器：把和弦摊开成节奏', 'wide',
     figs=[fig('m_arp')],
     blocks=[('先让一组音转起来', ['PLAY 页的 MODE 选 ARPEGGIATOR。',
                                 '用电脑键按住 A、D、G（就是 C、E、G），先用 FORWARD，再试 PING-PONG。']),
             ('HOLD 和节奏格', ['HOLD 让松开的音也留在循环里，新按的音会加进去。',
                              'ARP RHYTHM 里关掉的格子会“吞掉”一拍，变成休止。'])],
     try_='想换一组干净的和弦：关掉 HOLD，松开所有键，再重新按。',
     terms=['arp', 'rest'],
     more='关掉 HOLD 并松开所有键，就能清掉之前积累的音。RHYTHM 关掉的一格会吞掉一个时钟，这和音序器里把某一步 GATE 关掉不一样。',
     src='ARCH §11；arp_sequencer.h')

page('keys', '16 步音序：先写下，再演奏', 'wide',
     figs=[fig('m_steps')],
     blocks=[('做一个四步小句子', ['SEQ 页：LENGTH 设 4，RUN 设 KEYBOARD。',
                                 'SEQ STEPS 页：前四步设 0、+7、+12、+7，四步的 GATE 都打开。最后在 PLAY 页选 SEQUENCER。']),
             ('按住的键决定起点', ['按住一块板，每一步的音程都加到这块板的音高上。',
                                 '换一块板，整句一起移调。关掉某一步的 GATE，就多了一个休止。'])],
     try_='先把 0、+7、+12、+7 听熟，再关掉其中一步的 GATE 比较。',
     terms=['interval'],
     more='先保存想保留的键盘预设。CV OUTPUT 选 continuous 时，休止的那一步音高仍会更新；选 gated 时保持上一步的音高。练习结束后把 MODE 改回 KEYBOARD。',
     src='ARCH §12；arp_sequencer.h；keyboard_menu_view.h')

page('keys', '谁在喊“下一步”？', 'split', figw=0.55,
     figs=[fig('m_tempo'), fig('jacks_l', mk('keyboard.clock_in', label='CLOCK 输入'), zoom=1.6)], stack=True,
     blocks=[('内部 TEMPO', ['琶音器和音序器每拍走四步。',
                           '120 BPM 时一步是 0.125 秒。改 TEMPO 会回到内部时钟。']),
             ('外部 CLOCK 和 MIDI Clock', ['收到外部脉冲或 MIDI Clock 后会跟着走；RESET 或 MIDI START 回到第一步。',
                                        '面板上的 5 步音序器有自己的 PULSER，不跟这里。'])],
     try_='ARP / SEQ 页的 CLOCK 倍率目前固定为 1:1，别用它来排查速度问题。',
     terms=['bpm', 'ppqn'],
     more='120 BPM 是每秒两拍，每拍四步，所以一步 0.125 秒。键盘的 CLOCK 插孔只能输入，没有时钟输出。',
     src='ARCH §6、11、14.2；midi_timing.h；machine_runtime.h')

# ── 混音与塑形 ──────────────────────────────────────────────────────────────────────────
page('mix', '10 路混音：谁在前，谁在旁边', 'wide',
     figs=[fig('mixer', mk(['mixer.ch1_pan', 'mixer.ch10_pan'], label='PAN'), mk(['mixer.ch1_vol', 'mixer.ch10_vol'], label='VOL'))],
     blocks=[('VOL 管层次，PAN 管左右', ['每一路先调自己的音量，再决定放在左边还是右边。',
                                     '先让主角清楚，再少量加入配角。']),
             ('十路分别是谁', ['Drone 1、2、3、EXT.AUDIO、VCO A、VCO B、PREAMP、Drone 4、5、6。',
                             '把 VOL 关小，不一定会停掉这个声源送出的调制。'])],
     try_='留一条低音、一条中高音，把它们稍微分到左右两边，再加第三条。',
     terms=['pan'],
     more='混音降低的只是你听到的这一路音量。声部叠得越多，越可能把后面的滤波和失真推进饱和，加了新声部后要回头听整体响度。',
     src='ARCH §16.1；voice_mixer.h（等功率 cos/sin）')

page('mix', '双滤波器：留下什么，突出什么', 'split', figw=0.6,
     figs=[fig('processing', mk(['vcf.l_freq', 'vcf.r_freq'], label='FREQ', split=True), mk(['vcf.l_res', 'vcf.r_res'], label='RES', split=True), mk(['vcf.l_mod', 'vcf.r_mod'], label='MOD L / R'), mk('vcf.link', label='link'))],
     blocks=[('FREQ、RES、LP / BP', ['LP（低通）留下低音、削掉高音；BP（带通）只留中间一段。',
                                   'RES（共振）抬高截止频率附近的声音，开到最后会自激。']),
             ('左右独立，也能一起动', ['MOD L / R 决定 CV 推动截止频率的深度。',
                                    'CV R 没插线时跟着 CV L；LINK 打开后两边都听 CV L。'])],
     try_='先固定 RES，慢慢扫 FREQ；再固定 FREQ，一点点加 RES，比较两种变化。',
     terms=['vcf', 'polivoks'],
     more='滤波器采用 Polivoks 风格的状态变量结构：共振开大时，低音不会变薄。低通斜率约 12 dB/oct。内部积分器会饱和，所以推得很猛时声音更粗。LINK 连接的是控制信号，不会把左右旋钮数值复制成一样。',
     src='ARCH §16.2；polivoks_vcf.h（TPT/ZDF SVF，Simper）；machine_runtime.h')

page('mix', '失真：把波形推弯', 'split', figw=0.56,
     figs=[fig('processing', mk('vcf.gain', label='GAIN'), mk('vcf.dist', label='DIST'))],
     blocks=[('GAIN 决定推多狠', ['软件用 tanh 曲线把波形柔和地压弯，增加谐波。',
                                '轻推更厚，重推像 fuzz；内置补偿让音量不会暴涨。']),
             ('DIST 决定混入多少', ['DIST 调失真前、后两种声音的比例。',
                                  '它不是效果器的 BLEND；失真在滤波器之后、效果器之前。'])],
     try_='DIST 先开一点；转 GAIN 找到喜欢的颗粒感，再用 DIST 决定它占多少。',
     terms=['softclip', 'adaa'],
     more='GAIN 的推动量按 1 + 40 × GAIN² 增长，所以旋钮前半段更细腻。失真用一阶 ADAA 减少混叠，没有用过采样。不同素材的响度仍会变化，用耳朵检查。恢复：双击 DIST 和 GAIN。',
     src='ARCH §16.3；distortion.h（tanh、ADAA、1+40·GAIN²）')

# ── 效果器 ──────────────────────────────────────────────────────────────────────────────
page('fx', '先选卡带，再让一侧载入', 'split', figw=0.58,
     figs=[fig('processing', mk('effector.slot', label='卡带槽'), mk(['effector.select_l', 'effector.select_r'], label='1-2-3'), mk('effector.blend', label='BLEND'))],
     blocks=[('槽里放的，不等于正在运行的', ['点卡带槽换一盒候选卡带。',
                                         '拨左边或右边的 1-2-3 开关，才把它载入那一侧。']),
             ('左右两台效果器', ['左右可以运行不同的程序；混音器的 PAN 决定声音送去哪边。',
                               'X、Y、Z 和 BLEND 两边共用。换程序会切断旧的尾音。'])],
     try_='左边先载入 CATHEDRAL 1；再换一盒卡带载到右边，比较左右的空间感。',
     terms=['feedback', 'fade'],
     more='卡带槽标签第一行是候选卡带，下面的 L / R 是两侧正在运行的程序。右键或 Shift + 点击换到上一盒。X / Y / Z 的含义随程序变化，见附录“卡带索引”。',
     src='ARCH §17；effector.h（约 15 ms 淡出 → 清空切换 → 约 15 ms 淡入，不重叠）；panel_editor.h')

page('fx', 'Shimmer：尾音里长出八度', 'split', audio='07-shimmer', figw=0.58,
     figs=[fig('processing', mk(['effector.x', 'effector.y', 'effector.z'], label='X · Y · Z'), mk('effector.blend', label='BLEND'))],
     blocks=[('混响为什么会拖尾', ['多条延迟线把声音错开、混合，再送回去循环。',
                                 '每绕一圈损失一点能量，尾音就慢慢消失。']),
             ('CATHEDRAL 1 的 X、Y、Z', ['X 把高八度送进混响，Y 是低八度，Z 改变衰减时间。',
                                     'BLEND 决定效果声混进来多少。'])],
     try_='弹一个短音就松开：先听原声，再一点点加大 BLEND 和 X。',
     terms=['fdn', 'shimmer'],
     more='这里的反馈延迟网络有 16 条延迟线。升、降八度的声音回到网络里继续循环，低频会被高通滤波控制，避免低八度越堆越浑。Z 大约对应 1.5 到 40 秒的衰减。',
     src='ARCH §17；effector.h（16 线 FDN、颗粒移调）')

page('fx', '卡带地图：按想要的声音找', 'split', figw=0.42,
     figs=[fig('processing', mk('effector.slot', label='13 盒 × 3 个程序'))],
     blocks=[('空间、时间和音高', ['CATHEDRAL：混响、八度尾音。MAGIC / TIME / INFINITY：延迟与反馈。',
                                'PITCH SHIFTER：移调。ORCHE：反向延迟。']),
             ('运动、颗粒和再合成', ['VIBROTREM / VIBE：颤动和摇摆。FILTER / DIGITAL：滤波与数字颗粒。',
                                  'STRING RINGER：环形调制。SYNTEX-1 / GENERATOR：跟踪合成与自己发声。'])],
     try_='一次只换一种程序，用同一个短音试听；每次都重新确认 X / Y / Z 的意思。',
     terms=['ringmod', 'crush'],
     more='ORCHE 在这里是反向延迟，不是交响乐采样器。比较程序时用同一个短音，并对照附录“卡带索引”里 X / Y / Z 的实际作用。',
     src='ARCH §18；spec lunar24.json；effector.h')

# ── 让参数动起来 ────────────────────────────────────────────────────────────────────────
page('mod', 'LFO：请一个慢动作助手', 'split', figw=0.66,
     figs=[fig('lfoa', mk('lfo_a.cv_out', n='1')), fig('processing', mk('vcf.cv_l_in', n='2'), mk(['vcf.l_mod', 'vcf.r_mod'], label='MOD L / R'))],
     patch=True,
     blocks=[('wave、rate 和倍率', ['wave 在三角（平滑来回）和方波（两端跳）之间混合。',
                                  'rate 决定快慢，x1 / x6 / x10 开关再加速。']),
             ('接线后还要给它深度', ['把 ① LFO A 接到 ② 滤波器 CV L，rate 慢一点，MOD L / R 开一点。',
                                   'FREQ 定好起点，LFO 让它“呼吸”。'])],
     try_='先小深度，再改速度；深度和速度同时开大，就很难听出各自的作用。',
     terms=['lfo', 'unipolar'],
     more='LFO A / B 和 Drone 3 / 6 里的 LFO 互不相干。倍率开高后能进入音频速度（最高约 200 Hz）。输出是 0 到 +10 V 的单极信号，只往正方向推。',
     src='ARCH §5.1、16.2；lfo.h；machine_runtime.h')

page('mod', '5 步音序器：每一步一个姿态', 'wide',
     figs=[fig('seq5', mk('sequencer.pulser', label='PULSER'), mk('sequencer.stages', label='stages'),
               mk(['sequencer.step_cv_1', 'sequencer.step_gate_5'], label='5 个 step'), mk(['sequencer.cv_out', 'sequencer.gate_out'], label='cv · gate'))],
     blocks=[('每一步存的是电压，不是音名', ['五个 step 旋钮各给出 0 到 +5 V；STAGES 选 3、4 或 5 步。',
                                         '每步的小开关决定这一步发不发门信号。']),
             ('速度和输出', ['PULSER 管内部速度，cv / gate 插孔输出结果。',
                           'clock 输入可以接外部节拍；clock 输出永远是 PULSER 的节奏。'])],
     try_='某一步的 gate 关掉时，cv 照样更新——它和键盘的 16 步旋律音序器不是一回事。',
     terms=['clock'],
     more='菜单里设成 EXT 却没接时钟线时，它会停住不走。直接把 cv 接到音高输入时，整个范围能跨五个八度。',
     src='ARCH §6；five_step_sequencer.h')

page('mod', '一根线，做出五种明暗', 'wide',
     figs=[fig('seq5', mk('sequencer.cv_out', n='1')), fig('processing', mk('vcf.cv_l_in', n='2'), mk(['vcf.l_mod', 'vcf.r_mod'], label='MOD L / R'))],
     patch=True,
     blocks=[('先做一个能听的起点', ['只开 DRONE 1，BLEND 最小。',
                                  '把 ① 5 步 cv 接到 ② 滤波器 CV L；MOD L / R 开一点，PULSER 放慢。']),
             ('把五个 step 当成手势', ['五步设成：低、低、中、高、中，听滤波一步一步换亮度。',
                                    '试 STAGES 3，再回到 5，节奏轮廓会变。'])],
     try_='恢复：拔掉 CV L 上的线。每个插孔只能接一根线，新线会替换旧线。',
     terms=['stepmod'],
     more='可以从 0、0.5、1、2、1 V 开始，再凭耳朵调整。不要试图从同一个插孔分出好几根线去不同地方。',
     src='ARCH §6、16.2；patch_graph.h；five_step_sequencer.h')

page('mod', '摇杆：一个手势，两个方向', 'split', figw=0.56,
     figs=[fig('stick', mk('joystick.stick', label='拖这里')), fig('joystick', mk(['joystick.x_out', 'joystick.y_out'], label='X / Y OUT'), mk(['joystick.offset_x'], label='offset X'), mk(['joystick.offset_y'], label='offset Y'))],
     blocks=[('X、Y 是两路控制', ['左右、上下移动分别产生一路 CV。',
                                'offset 旋钮移动各自的中心；松手后停在原处，双击摇杆回中。']),
             ('两维演奏的起点', ['X OUT 接滤波器 CV L（MOD 开一点）；Y OUT 接效果器的 cv z。',
                               '左右改音色，上下改当前程序的 Z。'])],
     try_='先确认当前卡带的 Z 是什么；同一个手势换个程序，就变成另一种演奏。',
     terms=['offset'],
     more='左下角的大圆盘才是摇杆，上面一排的小旋钮是 offset。位置和偏置相加，输出限制在 ±10 V。建议配 CATHEDRAL 1（Z 是衰减）从中间开始，避免突然的大动作。结束时拔线，offset 回中，双击摇杆。',
     src='ARCH §5.2、17；joystick_cv.h；effector.h')

# ── MIDI 与保存 ─────────────────────────────────────────────────────────────────────────
page('midi', '接上 MIDI，先让一个音响起来', 'wide',
     figs=[fig('m_devices')],
     blocks=[('独立 App', ['点右下角的 MIDI 按钮，进入 DEVICES，打开你的设备（最多同时 4 个）。',
                         'CHANNEL、TRANSPOSE、VELOCITY、SPLIT 每个设备单独设置。']),
             ('DAW 插件', ['在 DAW 的轨道上选择 MIDI 输入，并打开监听或录音待命。',
                         '这时音频输出也归 DAW 管；DEVICES 里的设置不是插件的入口。'])],
     try_='先确认普通琴键能发声，再去做 Learn——不要一开始就同时排查两件事。',
     terms=['midi', 'daw'],
     more='图里的设备名是示例，你的列表以自己的控制器为准。不同 DAW 的监听、录音待命按钮名字可能不一样。',
     src='ARCH §1.5；midi_devices.h；midi_settings_view.h')

page('midi', 'Learn：把旋钮变成你的手', 'wide',
     figs=[fig('m_learn')],
     blocks=[('建立一个绑定', ['点 + LEARN A CONTROL，再点主面板上的 FREQ L，然后转一下实体旋钮。',
                             '表里就会出现“控制器 → 面板目标”的一行。']),
             ('确认模式和手感', ['普通旋钮一般是 ABS，无限旋转的编码器用 REL。',
                               '让 DRONE 1 响起来，往两个方向转一转，确认没有突然跳动。'])],
     try_='打击垫优先绑定 CC，免得它的音符号和琴键弹奏冲突。',
     terms=['cc', 'absrel'],
     more='Learn 一开始按 ABS 建立，再观察输入自动识别相对编码器。如果旋钮转一下数值就跳到最大或最小，多半是模式不对。',
     src='ARCH §1.5；midi_learn.h；midi_map.h；DECISIONS')

page('midi', '三种保存，存的东西不同', 'wide',
     figs=[fig('m_profiles', cap='MIDI 面板 · PROFILES 页'), fig('m_presets', cap='键盘菜单底部 · 预设 A–D', zoom=1.35)],
     blocks=[('键盘预设 A–D', ['存键盘的玩法、音阶、步进和单板调音。',
                             '不存整机音色，也不含 TEMPO。选好槽位点 SAVE，下次点 LOAD。']),
             ('MIDI Profile 和整机状态', ['Profile 只存控制器绑定，不是声音预设；修改会自动保存。',
                                       '独立 App 会记住整机状态，插件则把状态存进 DAW 工程。'])],
     try_='切换 MIDI Profile 不会换音色：换的是“哪只手管哪个旋钮”，声音继续。',
     terms=['preset'],
     more='想留一份备份就用 COPY。DAW 工程里不包含共享的 Profile 库。独立 App 每次启动时 Drone 键都是关的；插件会保存它们的状态。RESET PANEL 保留 A–D 预设、MIDI 和外观，只清当前面板和跳线。',
     src='ARCH §14.3；keyboard_presets.h；midi_map_store.h；plugin_state_chunk.h')

# ── 幕后 ────────────────────────────────────────────────────────────────────────────────
page('inside', '一个旋钮，怎样变成声音', 'flow', diagram='engine',
     blocks=[('界面把动作排进队列', ['拖旋钮、MIDI 都变成一条条控制消息，放进一条“无锁”传送带。',
                                   '音频线程取出后，按当前接线一个采样一个采样地计算。']),
             ('持续发声靠“从不等待”', ['音频线程不读写硬盘、不加锁、不临时申请内存。',
                                    '需要平滑的参数用几毫秒追到新值；录音先进缓冲，再由另一个线程写成 WAV。'])],
     try_='你看到的面板只是操作层；真正的乐器，是一直在运行的声源、控制与处理网络。',
     terms=['thread', 'queue'],
     more='接线里出现“反馈环”时，软件会在环上插入一个采样的延迟，保证计算有先后顺序。参数平滑能减少咔哒声，但不是每个开关都会慢慢切换。',
     src='ARCH §1.5；standalone_audio_engine.h；live_command_queue.h；graph_compiler.h（SCC 断环）；wav_recorder.h')

page('inside', '知道边界，才知道怎样继续玩', 'wide',
     figs=[fig('m_service')],
     blocks=[('快速变化也有数字边界', ['很快的 FM、PWM、SYNC 可能产生混叠：高频折回来，变成不和谐的杂音。',
                                   '先减小深度、降低音区，再比较听感。']),
             ('有些菜单项只是“摆设”', ['8 个音阶标着 NOT MODELLED；ARP / SEQ 的时钟倍率固定 1:1。',
                                    'SERVICE 页除了编码器方向，其它都不影响声音。'])],
     try_='遇到某个选项“没反应”，先确认它是不是还没实现，再怀疑自己操作错了。',
     terms=['aliasing'],
     more='作为插件时没有外部音频输入，只输出 WET 立体声，录音用 DAW 完成。先认清这些边界，就不会把没反应的选项当成自己的失误。',
     src='ARCH §4.8、13.3、15、19；state_disposition.h；STATUS')

# ── 作品 ────────────────────────────────────────────────────────────────────────────────
page('make', '你的 60 秒声音小世界', 'mission',
     blocks=[('0–30 秒：出现与靠近', ['0–15 秒：只开一条经典 Drone，慢慢升起。',
                                     '15–30 秒：加入 VCO 的三四个稀疏音符，放在不同音区和左右位置。']),
             ('30–60 秒：变化与离开', ['30–45 秒：小幅转滤波器，或加入慢 LFO。',
                                     '45–55 秒：关声部、松键，让释放开始；最后 5 秒留给尾音。'])],
     try_='只用两种声源、一种调制、一种效果。留出空白，比塞满更好听。',
     more='长混响可能超过 60 秒，不用硬在整点停止录音：可以把 Z 调短，或者一直录到尾音自然结束。',
     src='ARCH §1、2、4、17；STATUS')

page('keep', 'REC：录下整段演奏', 'split', figw=0.4,
     figs=[fig('rec', cap='开始前'), fig('rec_on', cap='录音中：亮红并计时')],
     blocks=[('独立 App 的录音', ['先用 REC 下方的小旋钮选来源：WET、DRY 或 ALL，再点 REC。',
                                'WET 录主立体声，DRY 录 VCO A / B，ALL 同时生成两个文件。']),
             ('结束和文件', ['等尾音结束再点 REC 停止。',
                           '24-bit WAV 存在“音乐”文件夹的 Lunar 24 里。插件里用 DAW 录音。'])],
     try_='录完马上回放一次：开头有没有被切掉？结尾有没有给尾音留时间？',
     terms=['samplerate'],
     more='DRY 文件左声道是 A，右声道是 B。WET 受 MASTER 影响，DRY 绕过主处理链。',
     src='ARCH §1.2；wav_recorder.h；main.mm / window_metrics_win.cpp（Music/Lunar 24）')

page('help', '没声音？沿着声路往后找', 'flow', diagram='check',
     blocks=[('先问：声音有没有产生？', ['Drone 键开了吗？MUTE 有没有亮？',
                                     'VCO 有按键、Gate 或 HOLD 吗？插在 vca cv / gate 上的线有没有替换掉默认？']),
             ('再问：声音能不能出去？', ['这一路的 VOL、MASTER 是不是太低？滤波器是不是关得太暗？',
                                     'App 的输出设备，或 DAW 轨道的监听，选对了吗？'])],
     try_='声音停不下来时：检查 HOLD、自循环、GENERATOR、滤波器自激和长尾音。',
     terms=['signalpath'],
     more='如果突然很响，先按右下角的整机 MUTE，再去找反馈或过高的共振。需要 RESET PANEL 时先把作品录下来——它会清掉当前的旋钮和跳线。只听 DRY 时，MASTER 不在它的路径上。',
     src='ARCH §1、2、4、17；MANUAL_TESTS')

page('help', '三个可以继续玩的挑战', 'mission',
     blocks=[('挑战一：风经过房间', ['只用 Drone 3 的噪声、慢慢变化的滤波器和混响。', '不用旋律，做出“远—近—远”的变化。']),
             ('挑战二、三：会走路的声音', ['二：用 S&H 做不规则的音高，再控制它的更新速度。', '三：用 A 对 B 的调频，做一个能用键盘弹的怪声。',
                                     '给作品起个名字，说说你最喜欢的一个动作。'])],
     try_='好作品的标准：你的动作和听到的变化，有没有清楚的对应？',
     more='选一个挑战，录两个版本，每次只改一个地方。再说说哪个动作最能表达你想要的感觉。',
     src='ARCH §3、4、16、17')

page('appendix', '查词卡：把术语翻译成声音', 'glossary',
     lead='忘了某个词？在这里找到它，再回到那一页亲手听一次。',
     more='完整的术语表在阅读版最后，按出现顺序排列。继续创作时，先保存一个自己喜欢的版本，再试下一种变化。',
     src='—')

page('appendix', '附录：卡带索引（一）', 'table', part=0,
     lead='每盒卡带有 3 个程序。X / Y / Z 的作用随程序改变。',
     src='spec/machine/lunar24.json programs；effector.h')
page('appendix', '附录：卡带索引（二）', 'table', part=1,
     lead='ORCHE 在 Lunar 里是反向延迟；GENERATOR 不需要输入就能发声。',
     src='spec/machine/lunar24.json programs；effector.h')

# Abbreviations on the "一页看懂" page: (abbr, full English, what it does on Lunar).
ABBR = [
    ('VCO', 'Voltage-Controlled Oscillator', '压控振荡器：发出音调'),
    ('VCF', 'Voltage-Controlled Filter', '压控滤波器：改变明暗'),
    ('VCA', 'Voltage-Controlled Amplifier', '压控放大器：控制音量'),
    ('LFO', 'Low-Frequency Oscillator', '低频振荡器：让参数慢慢动'),
    ('ENV', 'Envelope', '包络：一个音的起落轮廓'),
    ('A D S R', 'Attack · Decay · Sustain · Release', '起音 · 衰减 · 持续 · 释放'),
    ('ATT · RLS', 'Attack · Release', '起音 · 释放（经典 Drone）'),
    ('S&H', 'Sample and Hold', '采样保持：随机台阶'),
    ('CV', 'Control Voltage', '控制电压：连续的控制数值'),
    ('FM · AM', 'Frequency · Amplitude Modulation', '调频 · 调幅'),
    ('PW · PWM', 'Pulse Width (Modulation)', '脉宽 · 脉宽调制'),
    ('FREQ · RES', 'Frequency · Resonance', '截止频率 · 共振'),
    ('LP · BP', 'Low-Pass · Band-Pass', '低通 · 带通'),
    ('DIST', 'Distortion', '失真'),
    ('MOD', 'Modulation', '调制：接受多少控制'),
    ('VOL · PAN', 'Volume · Panorama', '音量 · 左右声像'),
    ('OCT · SUB', 'Octave · Sub-oscillator', '八度 · 低八度次振荡器'),
    ('EXT', 'External', '外部输入'),
    ('MIDI', 'Musical Instrument Digital Interface', '乐器数字接口'),
    ('BPM', 'Beats Per Minute', '每分钟拍数'),
]

# The one-page glossary: the terms a reader meets most, in order of first use.
GLOSSARY_PAGE = ['cv', 'gate', 'hz', 'vco', 'vca', 'lfo', 'env', 'fm', 'sh', 'harmonics', 'aliasing',
                 'beating', 'normal', 'patch', 'feedback', 'midi']

# X / Y / Z role names in the spec, in plain Chinese.
ROLE_ZH = {
    'Octave up': '升八度', 'Octave down': '降八度', 'Decay': '衰减', 'Feedback': '反馈', 'Delay': '延迟时间',
    'Reverb': '混响', 'Pitch': '音高', 'Mod depth': '调制深度', 'Delay/Vibrato rate': '延迟 / 颤音速度',
    'Depth': '深度', 'Rate': '速度', 'Filter amount': '滤波量', 'Envelope': '包络', 'HP cutoff': '高通截止',
    'LP cutoff': '低通截止', 'Resonance': '共振', 'Cut 1': '陷波 1', 'Cut 2': '陷波 2', 'Direct': '原声',
    'Pitch 1': '音高 1', 'Pitch 2': '音高 2', 'Voice mix': '声部混合', 'Pre delay': '预延迟',
    'Pre delay mod': '预延迟调制', 'Frequency': '频率', 'Sub': '低八度', 'Pitch Speed': '音高速度',
    'S&H Rate': 'S&H 速度', 'Freq Ring Mod': '环调频率', 'Vibrato rate': '颤音速度', 'Tremolo rate': '震音速度',
    'Tone': '音色', 'Color': '色彩', 'Sample rate': '采样率', 'Cutoff': '截止频率', 'Input gain': '输入增益',
    'LFO speed': 'LFO 速度', 'LFO amount': 'LFO 深度', 'Envelope amount': '包络深度', 'FM 2-1': '调频 2→1',
    'LFO rate': 'LFO 速度', 'Pitch mod +/-': '音高调制 ±', 'LP/HP': '低通 / 高通', 'Delay time': '延迟时间',
    'Feedback amount': '反馈量', 'Trigger threshold': '触发门限', 'Delay mod LFO/RND': '延迟调制 LFO / 随机',
}


def cartridges():
    spec = json.loads((ROOT.parent / 'spec/machine/lunar24.json').read_text())
    rows = []
    for p in spec['programs']:
        roles = {v['role']: v['name'] for v in p['parameters']}
        rows.append(dict(cart=p['cartridge'], slot=p['slot'], name=p['name'],
                         **{k: [roles.get(k, ''), ROLE_ZH.get(roles.get(k, ''), '')] for k in 'xyz'}))
    return rows


def build():
    seen, missing = set(), []
    for i, p in enumerate(P, 1):
        p['id'] = f'{i:02d}'
        p['chapter'], p['color'] = CHAPTERS[p['ch']]
        for t in p['terms']:
            if t not in TERMS:
                missing.append(t)
        p['first_terms'] = [t for t in p['terms'] if t not in seen]
        seen.update(p['terms'])
    assert not missing, missing
    assert all(len(p['terms']) <= 2 for p in P), 'the creature card holds two terms'
    terms = {k: dict(term=a, full=b, zh=c, text=d) for k, (a, b, c, d) in TERMS.items()}
    order = []
    for p in P:
        order += [t for t in p['terms'] if t not in order]
    order += [k for k in TERMS if k not in order]
    data = dict(pages=P, terms=terms, term_order=order, figs=FIGS, abbr=ABBR,
                glossary_page=GLOSSARY_PAGE, cartridges=cartridges())
    out = ROOT / '.build/slides.json'
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(data, ensure_ascii=False, indent=1))
    # Evidence for re-checking pages against the code lives in README, never in the tutorial.
    readme = ROOT / 'README.md'
    head = readme.read_text().split('<!-- PAGE EVIDENCE')[0].rstrip()
    rows = '\n'.join(f"| {p['id']} | {p['title']} | {p['src']} |" for p in P)
    readme.write_text(head + '\n\n<!-- PAGE EVIDENCE: generated by source/content.py -->\n'
                      '## 逐页核对依据\n\nARCH = 仓库 `design/ARCHITECTURE.md`；头文件在 `core/include/lunar24/core/` 或 `host/include/host/`。'
                      '软件功能变化后，按这里重新核对对应页。\n\n| 页 | 标题 | 依据 |\n|---|---|---|\n' + rows + '\n')
    print(len(P), 'pages,', len(TERMS), 'terms ->', out.relative_to(ROOT))


if __name__ == '__main__':
    build()
