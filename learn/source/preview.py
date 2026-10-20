# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Build the reading page output/<rev>/index.html and the learn/index.html redirect.

Each page shows its slide image, the audio clip if it has one, and the "再深入一点" text;
the full creature dictionary closes the page. Run after deck.mjs: python3 learn/source/preview.py v4
"""
from pathlib import Path
import html
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
rev = sys.argv[1] if len(sys.argv) > 1 else 'v4'
data = json.loads((ROOT / '.build/slides.json').read_text())
pages, terms = data['pages'], data['terms']
audios = {a['id']: a for a in json.loads((ROOT / 'assets/audio/manifest.json').read_text())}
e = html.escape
COLORS = {'red': '#CB2026', 'teal': '#005E7A', 'green': '#005E3B', 'orange': '#A14E16', 'ink': '#0C0A0A'}


def term_html(k):
    g = terms[k]
    zh = f' <span class="zh">{e(g["zh"])}</span>' if g['zh'] and g['zh'] != g['term'] else ''
    full = f' <span class="full">{e(g["full"])}</span>' if g['full'] else ''
    return f'<dt><b>{e(g["term"])}</b>{zh}{full}</dt><dd>{e(g["text"])}</dd>'


nav, sections = [], []
for p in pages:
    if not nav or nav[-1][0] != p['chapter']:
        nav.append((p['chapter'], p['id'], COLORS[p['color']]))
    extra = []
    if p['audio']:
        a = audios[p['audio']]
        extra.append(f'<div class="audio"><span class="tag">试听 {e(p["audio"][:2])}</span>{e(a["title"])}'
                     f'<audio controls preload="none" src="../../assets/audio/{e(a["file"])}"></audio></div>')
    if p['more']:
        extra.append(f'<div class="more"><span class="tag">再深入一点</span><p>{e(p["more"])}</p></div>')
    sections.append(f'<section id="p{p["id"]}"><h2><span>{p["id"]}</span>{e(p["title"])}</h2>'
                    f'<img loading="lazy" src="slides/{p["id"]}.png" alt="{e(p["title"])}">{"".join(extra)}</section>')

glossary = ''.join(term_html(k) for k in data['term_order'])
navhtml = ''.join(f'<a href="#p{i}" style="--c:{c}">{e(ch)}</a>' for ch, i, c in nav)

CSS = '''
@font-face{font-family:'L24 Latin';src:url('../../assets/fonts/NotoSans[wdth,wght].ttf');font-weight:100 900;unicode-range:U+0000-00FF,U+2000-206F}
@font-face{font-family:'L24 SC';src:url('../../assets/fonts/NotoSansSC[wght].ttf');font-weight:100 900}
@font-face{font-family:'L24 Display';src:url('../../assets/fonts/Saira[wdth,wght].ttf');font-weight:100 900;font-stretch:50% 125%}
:root{--cream:#E9E0D2;--paper:#F3EEE5;--card:#FBF8F3;--ink:#0C0A0A;--text:#221E1B;--muted:#6E655B;--line:#D6CBBB;--red:#CB2026;--teal:#005E7A}
*{box-sizing:border-box}
body{margin:0;background:var(--cream);color:var(--text);font:16px/1.75 'L24 Latin','L24 SC',sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:1180px;margin:0 auto;padding:0 16px}
header.wrap{padding-top:48px;padding-bottom:18px}
.logo{font:640 54px/1 'L24 Display';font-stretch:125%;color:var(--ink);letter-spacing:.01em}.logo span{color:var(--red)}
.kicker{margin-top:10px;font-weight:700;letter-spacing:.4em;color:var(--muted);font-size:14px}
header p{max-width:760px;margin:22px 0 0}
.links{display:flex;flex-wrap:wrap;gap:10px;margin-top:20px}
.links a{display:inline-block;padding:7px 14px;border-radius:6px;background:var(--ink);color:#F2ECE2;text-decoration:none;font-size:14px;font-weight:600}
.links a.ghost{background:none;color:var(--ink);border:1.5px solid var(--ink)}
nav{position:sticky;top:0;z-index:5;background:color-mix(in srgb,var(--cream) 92%,transparent);backdrop-filter:blur(6px);border-bottom:1px solid var(--line)}
nav .wrap{display:flex;gap:6px 16px;overflow-x:auto;padding-top:10px;padding-bottom:10px;scrollbar-width:none}
nav a{flex:none;font-size:13.5px;font-weight:600;color:var(--ink);text-decoration:none;white-space:nowrap}
nav a::before{content:'';display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--c);margin-right:6px;vertical-align:1px}
section{margin:40px 0 56px;scroll-margin-top:56px}
h2{display:flex;align-items:baseline;gap:12px;font-size:19px;margin:0 0 12px;color:var(--ink)}
h2 span{font:600 15px 'L24 Display';font-stretch:112%;color:var(--muted)}
section img{display:block;width:100%;height:auto;border-radius:10px;box-shadow:0 1px 2px rgba(60,40,20,.12),0 14px 30px -18px rgba(60,40,20,.6)}
.audio,.more{margin-top:12px;padding:14px 18px;border-radius:10px;background:var(--card);border:1px solid var(--line);font-size:15px}
.tag{display:inline-block;margin-right:10px;font-size:12px;font-weight:700;letter-spacing:.12em;color:var(--red)}
.audio .tag{color:var(--teal)}
.more p{margin:4px 0 0}
audio{display:block;width:min(560px,100%);margin-top:10px}
.dict{scroll-margin-top:60px;margin:72px 0 40px;padding-top:32px;border-top:2px solid var(--ink)}
.dict h2{font-size:24px}
.dict .intro{display:flex;align-items:center;gap:16px;margin-bottom:20px}.dict .intro img{width:72px}
dl{columns:2 380px;column-gap:44px;margin:0}
dt{break-after:avoid;margin-top:14px;color:var(--ink)}dt b{font-size:16px}
dt .zh{font-weight:600}dt .full{font-size:12.5px;color:var(--muted);margin-left:4px}
dd{margin:2px 0 0;font-size:14.5px;line-height:1.65;break-inside:avoid;padding-bottom:12px;border-bottom:1px solid var(--line)}
footer{padding:28px 0 60px;font-size:13px;color:var(--muted)}
.top{position:fixed;right:16px;bottom:16px;background:var(--ink);color:#F2ECE2;padding:8px 12px;border-radius:6px;font-size:13px;text-decoration:none}
@media (max-width:640px){.logo{font-size:40px}section{margin:28px 0 40px}}
'''

page = f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lunar 24 声音实验室</title><style>{CSS}</style>
<header class="wrap" id="top"><div class="logo">LUNAR <span>24</span></div><div class="kicker">声音实验室</div>
<p>一份边玩边懂的合成器教程，共 {len(pages)} 页。打开 Lunar 24，跟着每页底部的“动手试试”做；遇到新词，右下角的小精灵“嗡嗡”会解释。建议分三次学：上手与声源（1–31），演奏与音色（32–46），调制、MIDI 与作品（47–{len(pages)}）。</p>
<div class="links"><a href="Lunar24-声音实验室.pdf">下载 PDF（可打印）</a><a class="ghost" href="#dict">嗡嗡词典 · 全部 {len(terms)} 个术语</a></div></header>
<nav><div class="wrap">{navhtml}<a href="#dict" style="--c:#CB2026">词典</a></div></nav>
<main class="wrap">{"".join(sections)}
<div class="dict" id="dict"><div class="intro"><img src="../../assets/art/critter-a.png" alt=""><div><h2>嗡嗡词典</h2>教程里出现过的全部术语，按第一次出现的顺序排列。</div></div><dl>{glossary}</dl></div></main>
<footer class="wrap">面板图片由 Lunar 24 自己的界面绘制程序生成，和你在 App 里看到的一致；MIDI 设备和绑定是示例。试听片段由 Lunar 24 的声音引擎离线生成（48 kHz / 16-bit），App 里 REC 录下的是 24-bit。</footer>
<a class="top" href="#top">回顶部</a>
<script>document.querySelectorAll("audio").forEach(a=>a.addEventListener("play",()=>document.querySelectorAll("audio").forEach(b=>{{if(b!==a)b.pause()}})));</script></html>'''

(ROOT / 'output' / rev / 'index.html').write_text(page)
(ROOT / 'index.html').write_text(f'<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=output/{rev}/index.html">'
                                 f'<a href="output/{rev}/index.html">打开 Lunar 24 声音实验室</a>')
print(ROOT / 'output' / rev / 'index.html')
