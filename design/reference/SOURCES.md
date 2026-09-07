# 参考资料索引（Lunar 24）

## ⚠️ 原始 PDF 的真实位置
- `<本地>/Solar_42N/solar42N_instruct_08_04_v15.pdf` —— 用户手册，**28 页**，75.9 MB
- `<本地>/Solar_42N/Solar42_panel_42n_04 copy.pdf` —— 面板高清图，**1 页**，3.6 MB

## 本目录已生成的衍生件
- `solar42N_manual_text.txt` —— 手册全文（`pdftotext -layout` 提取，102,532 字符）
- `solar42N_panel_2400px.png` —— 面板整图 2400×1551
- `effector-catalog-23.png` / `effector-catalog-24.png` —— 手册 p23–24 效果器卡带目录原页，120 dpi；用于避免三栏 PDF 被文字提取串列

## 工具
本机原本**没有 poppler / ghostscript**，PDF 读不了。已 `brew install poppler`（开源，符合项目许可要求）。
- 取文字：`pdftotext -layout <pdf> <out.txt>`
- 渲染页：`pdftoppm -png -r <dpi> -f <n> -l <n> <pdf> <prefix>`
- 单页缩略（无需 poppler 的原生兜底）：`qlmanage -t -s 2400 -o <dir> <pdf>`

## 官方在线来源

- ELTA Music Solar 42N 产品/支持页：<https://www.eltamusic.com/solar-42f>
- ELTA Music 官方用户手册：<https://www.eltamusic.com/_files/ugd/12d408_d4c9881033524945ae276dc3559589ff.pdf>

2026-08-23 核查结果：官方手册与 block scheme 明确 VCO A/B 有独立 dry outputs，但没有说明插入 dry jack 是否带 switching/normalization；因此该行为在 P0 保持 `unverified`，不能类推 Solar 50 的 switching 设计。
