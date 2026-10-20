// Render every panel state the tutorial shows to a 3x master PNG.
// The SVG comes from the tutorial copy of the app's panel_preview (shared drawing code);
// Chrome draws its labels with Noto Sans, the font the app embeds.
// Run from the repo root: node learn/source/render-panels.mjs
import fs from 'node:fs/promises';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright-core';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const tool = path.join(root, '.build/tutorial_preview');
const out = path.join(root, '.build/masters');
await fs.mkdir(out, {recursive: true});

export const VARIANTS = {
  panel: [],
  play: ['--menu=play'],
  expression: ['--menu=expression'],
  arp: ['--menu=arp'],
  seq: ['--menu=seq'],
  steps: ['--menu-example=steps'],
  split: ['--menu-split'],
  service: ['--menu=service'],
  midi: ['--midi-example'],
  devices: ['--midi-devices'],
  profiles: ['--midi-profiles'],
  recording: ['--recording'],
  generator: ['--generator'],
};

const only = process.argv.slice(2);
const browser = await chromium.launch({channel: 'chrome'});
const page = await browser.newPage({viewport: {width: 2400, height: 1552}, deviceScaleFactor: 3});
const font = `@font-face{font-family:'Panel Sans';src:url('../../assets/fonts/NotoSans[wdth,wght].ttf');font-weight:100 900}`;
for (const [name, args] of Object.entries(VARIANTS)) {
  if (only.length && !only.includes(name)) continue;
  const svg = execFileSync(tool, args, {maxBuffer: 32 * 1024 * 1024, encoding: 'utf8'});
  const html = path.join(out, `_${name}.html`);
  await fs.writeFile(html, `<!doctype html><meta charset=utf-8><style>${font}html,body{margin:0}svg text{font-family:'Panel Sans'!important}</style>${svg}`);
  await page.goto('file://' + html);
  await page.evaluate(() => document.fonts.ready);
  await page.screenshot({path: path.join(out, name + '.png')});
  await fs.rm(html);
  console.log('master', name);
}
await browser.close();
