# Reference material

The official Solar 42N documents are ELTA Music copyright and are **not** stored in this repo.
The owner uploads them per session when a manual check is needed:

- `Solar42_panel_42n_04 copy.pdf` — the 1-page official panel drawing (also the source for
  `tools/gen_panel_art.py`).
- `solar42n_to_agent.zip` — the user manual v15 (`solar42N_instruct_08_04_v15.pdf`, 28 pages)
  converted to `manual.md` (English original, page references), `verification.md` (known
  doubts in the manual — read first), `cartridges.json` (13 cartridges x 3 programs, X/Y/Z)
  and `assets/` (figures).

The effector program table is already in `spec/machine/lunar24.json`.

## Official online sources
- ELTA Music Solar 42N page: <https://www.eltamusic.com/solar-42f>
- Official user manual: <https://www.eltamusic.com/_files/ugd/12d408_d4c9881033524945ae276dc3559589ff.pdf>

The manual and block scheme show separate VCO A/B dry outputs but do not say whether
plugging a dry jack switches/normals anything; the app does not assume it does.
