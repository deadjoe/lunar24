// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Cut the tutorial's panel figures out of the 3x masters (render-panels.mjs) and cut the
// red creature out of the cover art. Crop boxes live in content.py (FIGS).
// Run from the repo root after content.py: node learn/source/prepare-assets.mjs
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import sharp from 'sharp';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const data = JSON.parse(await fs.readFile(path.join(root, '.build/slides.json'), 'utf8'));
const out = path.join(root, 'assets/figs');
await fs.mkdir(out, {recursive: true});

const S = 3, MAX = 2400;  // master scale; longest side kept in a figure
for (const [name, [variant, x, y, w, h]] of Object.entries(data.figs)) {
  const src = path.join(root, '.build/masters', variant + '.png');
  let img = sharp(src).extract({left: Math.round(x * S), top: Math.round(y * S), width: Math.round(w * S), height: Math.round(h * S)});
  const long = Math.max(w, h) * S;
  if (long > MAX) img = img.resize(w >= h ? {width: MAX} : {height: MAX});
  await img.jpeg({quality: 90, chromaSubsampling: '4:4:4', mozjpeg: true}).toFile(path.join(out, name + '.jpg'));
}
console.log(Object.keys(data.figs).length, 'figures');

// The red creature: keep red pixels (and its dark eyes) and make the paper transparent.
const cover = path.join(root, 'assets/art/cover-panel-colors.png');
const {data: px, info} = await sharp(cover).raw().toBuffer({resolveWithObject: true});
async function critter(name, x0, y0, w, h) {
  const buf = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = ((y0 + y) * info.width + x0 + x) * info.channels, r = px[i], g = px[i + 1], b = px[i + 2];
    const red = r - Math.max(g, b), dark = 255 - Math.max(r, g, b);
    const a = Math.max(0, Math.min(1, Math.max((red - 40) / 60, (dark - 150) / 60)));
    buf.set([r, g, b, Math.round(a * 255)], (y * w + x) * 4);
  }
  await sharp(buf, {raw: {width: w, height: h, channels: 4}}).png().toFile(path.join(root, 'assets/art', name + '.png'));
}
await critter('critter-a', 895, 110, 163, 150);
await critter('critter-b', 1505, 575, 140, 165);
console.log('critters');
