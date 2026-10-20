// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Lay out every page of .build/slides.json as a 1280 x 720 HTML slide, then let Chrome render
// output/<rev>/slides/NN.png (1920 x 1080) and output/<rev>/Lunar24-声音实验室.pdf.
// Run from the repo root: LEARN_REV=v4 node learn/source/deck.mjs   (LEARN_ONLY=07,12 renders a few)
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright-core';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const rev = process.env.LEARN_REV || 'v4';
const only = process.env.LEARN_ONLY?.split(',');
const outDir = path.join(root, 'output', rev);
const data = JSON.parse(await fs.readFile(path.join(root, '.build/slides.json'), 'utf8'));
const {pages, terms, figs: FIGS} = data;
const total = pages.length;

// ── panel geometry ────────────────────────────────────────────────────────────────────────
const widgets = Object.fromEntries(JSON.parse(await fs.readFile(path.join(root, '.build/widgets.json'), 'utf8')).map(w => [w.name, w]));
const ALIAS = {
  'drone_1.eye': 'kind15.0', 'drone_2.eye': 'kind15.1', 'drone_4.eye': 'kind15.2', 'drone_5.eye': 'kind15.3',
  'joystick.stick': 'kind5.153', 'keyboard.encoder': 'kind8.0', 'keyboard.display': 'kind10.0',
  'effector.slot': 'kind6.0', 'app.mute': 'kind11.0', 'app.midi': 'kind12.0',
};
for (let i = 1; i <= 6; i++) ALIAS[`drone_key.${i}`] = `kind7.${i - 1}`;
function box(ids) {
  let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const id of ids) {
    const w = widgets[ALIAS[id] || id];
    if (!w) throw new Error('unknown control ' + id);
    x0 = Math.min(x0, w.cx - w.w / 2); y0 = Math.min(y0, w.cy - w.h / 2);
    x1 = Math.max(x1, w.cx + w.w / 2); y1 = Math.max(y1, w.cy + w.h / 2);
  }
  const pad = 7;
  return [x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad];
}

// ── text ──────────────────────────────────────────────────────────────────────────────────
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
// Space between Chinese and Latin letters / digits, never next to full-width punctuation.
const CJK = '㐀-鿿';
function t(s) {
  return esc(String(s)
    .replace(new RegExp(`([${CJK}])([A-Za-z0-9])`, 'g'), '$1 $2')
    .replace(new RegExp(`([A-Za-z0-9%])([${CJK}])`, 'g'), '$1 $2'));
}
const COLORS = {red: '#CB2026', teal: '#005E7A', green: '#005E3B', orange: '#A14E16', ink: '#0C0A0A'};

// ── figures ───────────────────────────────────────────────────────────────────────────────
// Figures are sized in the browser (fitFigures below) to the space their box gets.
const CIRCLED = ['①', '②', '③', '④', '⑤', '⑥'];
// Two kinds of marks: red numbered endpoints of a patch cable (n), and dark numbered "look here"
// marks whose names go into a legend under the figure (label).
function marksHtml(f, counter) {
  const [, cx, cy, cw, ch] = FIGS[f.name];
  const legend = [];
  const html = f.marks.map(m => {
    let badge, cls = 'mark';
    if (m.n) badge = `<span class="num">${esc(m.n)}</span>`, cls += ' patch';
    else { const k = ++counter.n; badge = `<span class="num">${k}</span>`; legend.push(`<span class="lg"><i>${k}</i>${t(m.label)}</span>`); }
    return (m.split ? m.ids.map(id => [id]) : [m.ids]).map(ids => {
      const [x, y, bw, bh] = box(ids);
      const L = (x - cx) / cw * 100, T = (y - cy) / ch * 100, W = bw / cw * 100, H = bh / ch * 100;
      return `<div class="${cls}" style="left:${L}%;top:${T}%;width:${W}%;height:${H}%">${badge}</div>`;
    }).join('');
  }).join('');
  return {html, legend};
}
function figure(f, counter, overlay = '') {
  const [, , , cw, ch] = FIGS[f.name];
  const {html, legend} = marksHtml(f, counter);
  const cap = f.cap || legend.length ? `<figcaption>${f.cap ? `<span class="cap">${t(f.cap)}</span>` : ''}${legend.join('')}</figcaption>` : '';
  return `<figure data-w="${cw * (f.zoom || 1)}" data-h="${ch * (f.zoom || 1)}"><div class="img"><img src="../../assets/figs/${f.name}.jpg">${html}${overlay}</div>${cap}</figure>`;
}
function figureArea(p, overlay = '') {
  const arrow = p.patch ? `<div class="cable"><span></span></div>` : '';
  const counter = {n: 0};
  const items = p.figs.map(f => figure(f, counter, overlay));
  const cls = ['figs', p.stack ? 'stack' : '', p.patch ? 'patch' : '', items.some(h => h.includes('<figcaption>')) ? 'caps' : ''].join(' ');
  return `<div class="${cls}">${items.join(arrow)}</div>`;
}

// ── shared pieces ─────────────────────────────────────────────────────────────────────────
const block = b => `<section class="block"><h2>${t(b.h)}</h2><ul>${b.lines.map(l => `<li>${t(l)}</li>`).join('')}</ul></section>`;
const HEADPHONES = '<svg viewBox="0 0 24 24" width="15" height="15"><path d="M4 14v-2a8 8 0 0 1 16 0v2" fill="none" stroke="currentColor" stroke-width="2.2"/><rect x="3" y="13" width="5" height="8" rx="2" fill="currentColor"/><rect x="16" y="13" width="5" height="8" rx="2" fill="currentColor"/></svg>';
function header(p) {
  const audio = p.audio ? `<div class="audio">${HEADPHONES}<span>试听 ${esc(p.audio.slice(0, 2))}</span></div>` : '';
  return `<header><div class="tab"><i></i>${t(p.chapter)}</div>${audio}<h1>${t(p.title)}</h1>${p.lead ? `<p class="lead">${t(p.lead)}</p>` : ''}</header>`;
}
const footer = p => `<footer><span>LUNAR 24 · 声音实验室</span><span class="pno">${p.id}<small> / ${total}</small></span></footer>`;
function termEntry(k) {
  const g = terms[k];
  const zh = g.zh && g.zh !== g.term ? ` ${t(g.zh)}` : '';
  return `<div class="entry"><div class="name"><b>${t(g.term)}</b>${zh}${g.full ? ` <em>${esc(g.full)}</em>` : ''}</div><p>${t(g.text)}</p></div>`;
}
let critterTurn = 0;
function band(p, intro = false) {
  const tryBox = p.try_ ? `<div class="try"><span class="label">动手试试</span><p>${t(p.try_)}</p></div>` : '';
  let card = '';
  if (intro) {
    card = `<div class="critter"><img src="../../assets/art/critter-a.png"><div><div class="who">嗡嗡词典</div>
      <div class="entry"><div class="name"><b>你好，我是嗡嗡</b></div><p>我住在每页的右下角。新词第一次出现时，我会在这里用一两句话解释它。</p></div></div></div>`;
  } else if (p.terms.length) {
    const img = critterTurn++ % 2 ? 'critter-b' : 'critter-a';
    card = `<div class="critter"><img src="../../assets/art/${img}.png"><div><div class="who">嗡嗡词典</div>${p.terms.map(termEntry).join('')}</div></div>`;
  }
  if (!tryBox && !card) return '';
  const cols = tryBox && card ? (p.terms.length > 1 ? 'two' : 'pair') : 'single';
  return `<div class="band ${cols}">${tryBox}${card}</div>`;
}
const frame = (p, main, cls = '') => `${header(p)}<main class="${cls}">${main}</main>${band(p, p.layout === 'abbr')}${footer(p)}`;

// ── layouts ───────────────────────────────────────────────────────────────────────────────
const L = {};
L.cover = p => `<img class="bg" src="../../assets/art/cover-panel-colors.png">
  <div class="cover">
    <div class="logo">LUNAR <span>24</span></div>
    <div class="kicker">声音实验室</div>
    <h1>${t(p.title)}</h1>
    <p class="sub">${t(p.blocks[0].lines[0])}</p>
    <p class="goal"><span>${t(p.blocks[1].h)}</span>${t(p.blocks[1].lines[0])}</p>
    <p class="foot">从声音出发，理解每一个动作</p>
  </div>`;

L.split = p => frame(p, `<div class="left" style="flex-basis:${(p.figw || 0.5) * 100}%">${figureArea(p)}</div><div class="right fit">${p.blocks.map(block).join('')}</div>`, 'm-split');
L.wide = p => frame(p, `<div class="wide-fig">${figureArea(p)}</div><div class="cols fit">${p.blocks.map(block).join('')}</div>`, 'm-wide');
L.mission = p => `<img class="bg" src="../../assets/art/cover-panel-colors.png">${frame(p, `<div class="mission fit">${p.blocks.map(block).join('')}</div>`, 'm-mission')}`;

// Six numbered regions on the full panel (panel units; badge corner tl / tr / bl).
const REGIONS = [
  [1, '#005E7A', [[22, 258, 782, 301, 'tl'], [1598, 259, 782, 301, 'tr']]],
  [2, '#005E3B', [[22, 566, 783, 314, 'tl'], [1599, 566, 781, 314, 'tr']]],
  [3, '#A14E16', [[809, 182, 784, 698, 'tr']]],
  [4, '#CB2026', [[21, 884, 2357, 145, 'tl']]],
  [5, '#0C0A0A', [[400, 1103, 1598, 387, 'tl'], [55, 1172, 250, 250, '']]],
  [6, '#005E7A', [[2105, 1060, 285, 420, 'bl']]],
];
L.map = p => {
  const [, cx, cy, cw, ch] = FIGS.panel;
  const regions = REGIONS.map(([n, c, boxes]) => boxes.map(([x, y, w, h, corner]) =>
    `<div class="region" style="left:${(x - cx) / cw * 100}%;top:${(y - cy) / ch * 100}%;width:${w / cw * 100}%;height:${h / ch * 100}%;--c:${c}">${corner ? `<span class="${corner}">${CIRCLED[n - 1]}</span>` : ''}</div>`).join('')).join('');
  return frame(p, `<div class="left" style="flex-basis:64%">${figureArea({figs: [{name: 'panel', marks: []}]}, regions)}</div><div class="right fit map-legend">${p.blocks.map(block).join('')}</div>`, 'm-split');
};

L.abbr = p => {
  const rows = data.abbr.map(([a, full, zh]) => `<div class="abbr-row"><b>${esc(a)}</b><div><span>${t(zh)}</span><em>${esc(full)}</em></div></div>`);
  const half = Math.ceil(rows.length / 2);
  return frame(p, `<div class="abbr fit"><div>${rows.slice(0, half).join('')}</div><div>${rows.slice(half).join('')}</div></div>`, 'm-abbr');
};

L.glossary = p => {
  const rows = data.glossary_page.map(k => {
    const g = terms[k];
    return `<div class="gl-row"><div class="gl-name"><b>${t(g.term)}</b>${g.zh && g.zh !== g.term ? `<span>${t(g.zh)}</span>` : ''}</div><p>${t(g.text)}</p></div>`;
  });
  const half = Math.ceil(rows.length / 2);
  return frame(p, `<div class="glossary fit"><div>${rows.slice(0, half).join('')}</div><div>${rows.slice(half).join('')}</div></div>`, 'm-glossary');
};

L.table = p => {
  const rows = data.cartridges.slice(p.part * 20, p.part * 20 + 20);
  const cell = ([en, zh]) => `<td>${zh ? `<span>${t(zh)}</span>` : ''}<em>${esc(en)}</em></td>`;
  let last = '';
  const body = rows.map(r => {
    const first = r.cart !== last; last = r.cart;
    return `<tr class="${first ? 'first' : ''}"><th>${first ? esc(r.cart) : ''}</th><td class="slot">${r.slot}</td><td class="prog">${esc(r.name)}</td>${cell(r.x)}${cell(r.y)}${cell(r.z)}</tr>`;
  }).join('');
  return frame(p, `<table class="carts fit"><thead><tr><th>卡带</th><td>档</td><td>程序</td><td>X</td><td>Y</td><td>Z</td></tr></thead><tbody>${body}</tbody></table>`, 'm-table');
};

// Flow diagrams: the signal path, the troubleshooting walk, and the engine inside.
function node(tab, zh, cls = '') {
  return `<div class="node ${cls}"><div class="ntab">${esc(tab)}</div><div class="nzh">${t(zh)}</div></div>`;
}
const arrow = '<div class="arr"></div>';
const lane = (nodes, cls = '') => `<div class="lane ${cls}">${nodes.join(arrow)}</div>`;
const DIAGRAMS = {
  signal: () => lane([node('DRONE · VCO · EXT · PREAMP', '各种声源'), node('MIXER', '10 路混音'), node('FILTER L / R', '滤波'), node('DIST', '失真'),
    node('EFFECTOR L / R', '效果器'), node('MASTER', '总音量'), node('WET L / R', '主输出', 'out')]) +
    lane([node('VCO A / B', '两个振荡器'), node('ENVELOPE A / B', '音量包络'), node('DRY A / B', '干声输出', 'out')], 'dry'),
  check: () => lane([node('DRONE · VCO', '① 有没有发声'), node('MIXER', '② VOL 开了吗'), node('FILTER', '③ 是不是太暗'), node('EFFECTOR', '④ BLEND · 尾音'),
    node('MASTER · MUTE', '⑤ 总音量'), node('OUTPUT', '⑥ 设备 · 监听', 'out')]),
  engine: () => lane([node('YOU', '鼠标 · 电脑键 · MIDI'), node('UI', '界面线程'), node('QUEUE', '无锁队列'), node('AUDIO THREAD', '按接线逐采样计算', 'hot'),
    node('OUTPUT', '声卡 · 耳机', 'out')]) +
    lane([node('AUDIO THREAD', '同一份声音'), node('BUFFER', '录音缓冲'), node('WRITER', '另一个线程'), node('WAV', '音乐文件夹', 'out')], 'dry'),
};
L.flow = p => frame(p, `<div class="flow">${DIAGRAMS[p.diagram]()}</div><div class="cols fit">${p.blocks.map(block).join('')}</div>`, 'm-flow');

// Sizes every figure group to fill its box: side by side at a common height, or stacked at a
// common width. Runs in the page, after fonts load.
function fitFigures() {
  for (const g of document.querySelectorAll('.figs')) {
    const figs = [...g.querySelectorAll(':scope>figure')], ws = figs.map(f => +f.dataset.w), hs = figs.map(f => +f.dataset.h);
    const stack = g.classList.contains('stack');
    const gaps = [...g.querySelectorAll(':scope>.cable')].reduce((a, c) => a + (stack ? c.offsetHeight : c.offsetWidth), 0) +
      parseFloat(getComputedStyle(g).columnGap || 0) * (g.children.length - 1);
    const cap = g.classList.contains('caps') ? 27 : 0;
    const W = g.clientWidth, H = g.clientHeight, sum = a => a.reduce((x, y) => x + y, 0);
    // One scale for every figure in the group, so the crops keep their true relative size.
    const k = stack ? Math.min(W / Math.max(...ws), (H - gaps - cap * figs.length) / sum(hs))
                    : Math.min((H - cap) / Math.max(...hs), (W - gaps) / sum(ws));
    figs.forEach((f, i) => { const im = f.firstChild; im.style.width = ws[i] * k + 'px'; im.style.height = hs[i] * k + 'px'; });
  }
}

// ── styles ────────────────────────────────────────────────────────────────────────────────
const CSS = `
@font-face{font-family:'L24 Latin';src:url('../../assets/fonts/NotoSans[wdth,wght].ttf');font-weight:100 900;
  unicode-range:U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+2000-2017,U+2020-206F,U+20AC,U+2122,U+2212}
@font-face{font-family:'L24 SC';src:url('../../assets/fonts/NotoSansSC[wght].ttf');font-weight:100 900}
@font-face{font-family:'L24 Display';src:url('../../assets/fonts/Saira[wdth,wght].ttf');font-weight:100 900;font-stretch:50% 125%}
:root{--cream:#E9E0D2;--paper:#F3EEE5;--card:#FBF8F3;--ink:#0C0A0A;--text:#221E1B;--muted:#6E655B;--line:#D6CBBB;--red:#CB2026;--teal:#005E7A;--green:#005E3B;--orange:#A14E16}
*{box-sizing:border-box;margin:0;padding:0}
html,body{background:#555}
body{font-family:'L24 Latin','L24 SC',sans-serif;color:var(--ink);-webkit-font-smoothing:antialiased;font-kerning:normal}
.slide{position:relative;width:1280px;height:720px;overflow:hidden;background:var(--cream);break-after:page;line-break:strict;text-wrap:pretty;
  display:flex;flex-direction:column;padding:34px 56px 20px}
.slide>.bg{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
.slide>header,.slide>main,.slide>.band,.slide>footer{position:relative;z-index:1}
header{position:relative;flex:none}
.tab{display:inline-flex;height:24px;padding:0 11px 0 9px;align-items:center;gap:7px;background:var(--ink);color:#F2ECE2;
  font:600 12.5px/1 'L24 Display','L24 SC';font-stretch:112%;letter-spacing:.06em;border-radius:4px}
.tab i{width:7px;height:7px;border-radius:50%;background:var(--accent)}
.audio{position:absolute;right:0;top:0;height:24px;display:flex;align-items:center;gap:6px;padding:0 11px;border:1.5px solid var(--teal);border-radius:12px;color:var(--teal);font-size:12.5px;font-weight:700}
h1{margin-top:15px;font-size:31px;line-height:1.25;font-weight:700;letter-spacing:.005em}
.lead{margin-top:7px;font-size:16px;color:var(--muted);line-height:1.45}
main{flex:1;min-height:0;margin:24px 0 22px;display:flex;gap:44px}
footer{flex:none;margin-top:14px;display:flex;justify-content:space-between;align-items:baseline;font-size:11.5px;color:var(--muted);letter-spacing:.04em}
footer .pno{font:600 13px 'L24 Display';font-stretch:112%;color:var(--ink)}footer small{font-size:11px;color:var(--muted)}
.left{flex:none;min-width:0;display:flex}
.right{flex:1;display:flex;flex-direction:column;justify-content:center;gap:24px;min-width:0;overflow:hidden}
.block h2{font-size:18.5px;line-height:1.35;font-weight:700;color:var(--accent);margin-bottom:9px}
.block ul{list-style:none}
.block li{position:relative;padding-left:17px;font-size:16.5px;line-height:1.62;margin-bottom:5px;color:var(--text);text-wrap:balance}
.block li:last-child{margin-bottom:0}
.block li::before{content:'';position:absolute;left:1px;top:.68em;width:6px;height:6px;border-radius:50%;background:var(--accent)}
main.m-wide{flex-direction:column;gap:20px}
main.m-flow{flex-direction:column;justify-content:center;gap:40px}
.wide-fig{flex:1;min-height:0;display:flex}
.cols{flex:none;display:grid;grid-template-columns:1fr 1fr;gap:44px;align-content:start}
.figs{flex:1;min-width:0;min-height:0;display:flex;align-items:center;justify-content:center;gap:22px}
.figs.stack{flex-direction:column}
.figs.patch{gap:10px}
figure{display:flex;flex-direction:column;align-items:center;flex:none}
figure .img{position:relative;flex:none;border-radius:7px;box-shadow:0 1px 2px rgba(60,40,20,.10),0 10px 24px -14px rgba(60,40,20,.55)}
figure .img>img{display:block;width:100%;height:100%;border-radius:7px}
figcaption{margin-top:8px;font-size:12.5px;line-height:19px;color:var(--muted);text-align:center}
.mark{position:absolute;border:2.5px solid var(--ink);border-radius:9px;box-shadow:0 0 0 2px rgba(255,255,255,.92)}
.mark .num{position:absolute;left:-12px;top:-12px;min-width:23px;height:23px;padding:0 6px;border-radius:12px;background:var(--ink);color:#fff;font:700 13px/23px 'L24 Latin','L24 SC';text-align:center;box-shadow:0 0 0 2px #fff}
.mark.patch{border-color:var(--red)}.mark.patch .num{background:var(--red)}
figcaption{display:flex;flex-wrap:wrap;justify-content:center;gap:4px 14px;max-width:100%}
.lg{display:inline-flex;align-items:center;gap:5px;font-size:13px;color:var(--text);font-weight:600;white-space:nowrap}
.lg i{font-style:normal;display:inline-block;min-width:18px;height:18px;border-radius:9px;background:var(--ink);color:#fff;font:700 11px/18px 'L24 Latin';text-align:center}
.cap{color:var(--muted)}
.cable{flex:none;width:44px;display:flex;align-items:center;justify-content:center}
.cable span{display:block;width:34px;height:3px;background:repeating-linear-gradient(90deg,var(--red) 0 7px,transparent 7px 11px);position:relative}
.cable span::after{content:'';position:absolute;right:-3px;top:-5px;border:6.5px solid transparent;border-left:9px solid var(--red);border-right:0}
.stack>.cable{width:auto;height:34px}.stack>.cable span{width:3px;height:26px;background:repeating-linear-gradient(180deg,var(--red) 0 7px,transparent 7px 11px)}
.stack>.cable span::after{right:auto;left:-5px;top:auto;bottom:-3px;border:6.5px solid transparent;border-top:9px solid var(--red);border-bottom:0}
.band{flex:none;display:grid;grid-template-columns:1fr 1fr;gap:24px}
.band.two{grid-template-columns:5fr 7fr}.band.single{grid-template-columns:1fr}
.try{background:var(--paper);border-radius:10px;padding:13px 18px;border-left:5px solid var(--accent);display:flex;flex-direction:column;justify-content:center}
.try .label{font-size:12px;font-weight:700;letter-spacing:.12em;color:var(--accent);margin-bottom:5px}
.try p{font-size:15.5px;line-height:1.55;font-weight:600;color:var(--text)}
.critter{position:relative;background:var(--card);border-radius:10px;padding:11px 18px 12px 84px;border:1.5px solid var(--line);display:flex;align-items:center}
.critter>img{position:absolute;left:12px;top:50%;width:60px;transform:translateY(-50%) rotate(-4deg)}
.critter>div{width:100%}
.critter .who{font-size:11px;font-weight:700;letter-spacing:.16em;color:var(--red);margin-bottom:3px}
.entry+.entry{margin-top:7px;padding-top:7px;border-top:1px dashed var(--line)}
.entry .name{font-size:14.5px;line-height:1.35;margin-bottom:3px}
.entry .name b{font-weight:700}
.entry .name em{font-style:normal;font-size:12px;color:var(--muted);margin-left:3px}
.entry p{font-size:13.5px;line-height:1.55;color:#3A3430;text-wrap:pretty}
.cover{position:absolute;left:96px;top:118px;width:560px}
.cover .logo{font:640 92px/1 'L24 Display';font-stretch:125%;letter-spacing:.01em;color:var(--ink)}
.cover .logo span{color:var(--red)}
.cover .kicker{margin-top:14px;font-size:16px;font-weight:700;letter-spacing:.42em;color:var(--muted)}
.cover h1{margin-top:62px;font-size:46px;letter-spacing:.02em}
.cover .sub{margin-top:18px;font-size:21px;color:var(--text)}
.cover .goal{margin-top:44px;font-size:20px;color:var(--teal);font-weight:600}
.cover .goal span{display:inline-block;margin-right:12px;padding:3px 10px;border-radius:4px;background:var(--teal);color:#fff;font-size:13px;letter-spacing:.1em;vertical-align:3px}
.cover .foot{margin-top:70px;font-size:15px;color:var(--muted);letter-spacing:.12em}
.mission{display:flex;flex-direction:column;gap:30px}
main.m-mission{max-width:600px;flex-direction:column;justify-content:center}
.slide:has(main.m-mission) .band{max-width:600px;position:relative}
.slide:has(main.m-mission) footer{position:relative}
main.m-mission .block li{font-size:17px}
.region{position:absolute;border:2.5px solid var(--c);border-radius:8px;background:color-mix(in srgb,var(--c) 6%,transparent)}
.region span{position:absolute;width:24px;height:24px;border-radius:50%;background:var(--c);color:#fff;font:700 15px/24px 'L24 SC';text-align:center;box-shadow:0 0 0 2px #fff}
.region .tl{left:-12px;top:-12px}.region .tr{right:-12px;top:-12px}.region .bl{left:-12px;bottom:-12px}
.map-legend{gap:16px}.map-legend .block li{font-size:15.5px;line-height:1.55;margin-bottom:3px}.map-legend .block h2{font-size:17px;margin-bottom:5px}
.abbr,.glossary{flex:1;display:grid;grid-template-columns:1fr 1fr;gap:0 48px;align-content:center}
.abbr-row{display:flex;align-items:center;gap:14px;height:38px;border-bottom:1px solid var(--line)}
.abbr-row b{flex:none;width:116px;font:700 15.5px 'L24 Display','L24 SC';font-stretch:108%;color:var(--accent);letter-spacing:.02em}
.abbr-row div{display:flex;align-items:baseline;gap:10px;min-width:0}
.abbr-row span{font-size:14.5px;white-space:nowrap}.abbr-row em{font-style:normal;font-size:11.5px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.gl-row{display:grid;grid-template-columns:108px 1fr;gap:12px;padding:7px 0;border-bottom:1px solid var(--line);align-items:center}
.gl-name b{display:block;font-size:15px;color:var(--accent);line-height:1.3}.gl-name span{font-size:11.5px;color:var(--muted)}
.gl-row p{font-size:13px;line-height:1.5;color:var(--text)}
main.m-table{display:block;margin-top:14px;margin-bottom:12px}
.carts{width:100%;border-collapse:collapse;font-size:13px}
.carts thead td,.carts thead th{font-size:11.5px;color:var(--muted);font-weight:700;letter-spacing:.08em;text-align:left;padding:0 8px 6px;border-bottom:2px solid var(--ink)}
.carts td,.carts tbody th{padding:1px 8px;height:21.5px;text-align:left;vertical-align:middle;border-bottom:1px solid var(--line)}
.carts tbody th{font:700 13px 'L24 Display';font-stretch:108%;color:var(--accent);width:150px}
.carts tr.first td,.carts tr.first th{border-top:1.5px solid #B9AD9B}
.carts .slot{width:34px;color:var(--muted);font-weight:700}.carts .prog{width:250px;font-weight:600}
.carts td span{margin-right:6px}.carts td em{font-style:normal;color:var(--muted);font-size:11.5px}
.flow{flex:none;display:flex;flex-direction:column;gap:26px;padding:4px 0}
.lane{display:flex;align-items:center}
.node{flex:none;background:var(--card);border:1.5px solid var(--ink);border-radius:8px;overflow:hidden;min-width:96px}
.ntab{background:var(--ink);color:#F2ECE2;font:600 12.5px/1 'L24 Display','L24 SC';font-stretch:108%;letter-spacing:.04em;padding:8px 12px;text-align:center;white-space:nowrap}
.nzh{font-size:15px;padding:9px 12px 10px;text-align:center;white-space:nowrap}
.node.out{border-color:var(--accent)}.node.out .ntab{background:var(--accent)}
.node.hot{border-color:var(--red)}.node.hot .ntab{background:var(--red)}
.arr{flex:1;min-width:16px;height:2px;background:var(--ink);position:relative;margin:0 8px 0 4px}
.arr::after{content:'';position:absolute;right:-2px;top:-5px;border:6px solid transparent;border-left:8px solid var(--ink);border-right:0}
.lane.dry{width:62%}
`;

// ── assemble, render, check ───────────────────────────────────────────────────────────────
const slidesHtml = pages.map(p => `<div class="slide" id="s${p.id}" style="--accent:${COLORS[p.color]}">${L[p.layout](p)}</div>`).join('\n');
const doc = `<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>Lunar 24 声音实验室</title><style>${CSS}</style><body>${slidesHtml}<script>${fitFigures.toString()}</script></body></html>`;
const build = path.join(root, '.build', rev);
await fs.mkdir(build, {recursive: true});
await fs.mkdir(path.join(outDir, 'slides'), {recursive: true});
// The document sits two levels below learn/ so its relative asset links resolve.
const htmlPath = path.join(build, 'deck.html');
await fs.writeFile(htmlPath, doc);

const browser = await chromium.launch({channel: 'chrome'});
const page = await browser.newPage({viewport: {width: 1280, height: 720}, deviceScaleFactor: 1.5});
await page.goto('file://' + htmlPath);
await page.evaluate(() => document.fonts.ready);
await page.waitForFunction(() => [...document.images].every(i => i.complete));
await page.evaluate(() => fitFigures());

// Overflow check: text boxes that spill, and anything poking outside its slide.
const problems = await page.evaluate(() => {
  const out = [];
  for (const s of document.querySelectorAll('.slide')) {
    const r = s.getBoundingClientRect();
    for (const f of s.querySelectorAll('.fit,.try,.critter')) {
      if (f.scrollHeight > f.clientHeight + 2) out.push(`${s.id}: ${f.className.split(' ')[0]} overflows by ${f.scrollHeight - f.clientHeight}px`);
    }
    const main = s.querySelector('main');
    if (main) {
      const mb = main.getBoundingClientRect();
      for (const e of main.querySelectorAll('li,figure,.node,tr,.abbr-row,.gl-row')) {
        const b = e.getBoundingClientRect();
        if (b.bottom > mb.bottom + 2 || b.top < mb.top - 2) { out.push(`${s.id}: ${e.tagName.toLowerCase()} spills out of the body`); break; }
      }
      if (main.getBoundingClientRect().height < 150 && !main.classList.contains('m-table')) out.push(`${s.id}: body squeezed to ${Math.round(mb.height)}px`);
    }
    for (const e of s.querySelectorAll('.tag,.num,.node,h1,li,.entry')) {
      const b = e.getBoundingClientRect();
      if (b.right > r.right - 20 || b.left < r.left + 20 || b.bottom > r.bottom - 8 || b.top < r.top + 8) out.push(`${s.id}: ${e.className || e.tagName} outside the safe area`);
    }
    const hd = s.querySelector('header');
    if (hd) for (const e of s.querySelectorAll('main .tag,main .num')) { const b = e.getBoundingClientRect(), h = hd.getBoundingClientRect(); if (b.top < h.bottom + 4) out.push(`${s.id}: tag touches the title`); }
    const h1 = s.querySelector('h1');
    if (h1 && !s.querySelector('.cover') && h1.getBoundingClientRect().height > 45) out.push(`${s.id}: title wraps`);
  }
  return out;
});
for (const pr of problems) console.log('CHECK', pr);

for (const p of pages) {
  if (only && !only.includes(p.id)) continue;
  await page.locator('#s' + p.id).screenshot({path: path.join(outDir, 'slides', p.id + '.png')});
}
if (!only) {
  await page.pdf({path: path.join(outDir, 'Lunar24-声音实验室.pdf'), width: '1280px', height: '720px', printBackground: true, margin: {top: 0, right: 0, bottom: 0, left: 0}});
}
await browser.close();
console.log(`${only ? only.length : pages.length} slides -> output/${rev}${only ? '' : ' (+ PDF)'}; ${problems.length} layout checks`);
