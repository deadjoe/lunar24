# S6 baseline arm — the NAIVE head's own output (task #120, GH #19)

This directory is the FIXED HISTORICAL BASELINE the improvement columns are measured against. It
is not a copy of the candidate: it is what the product rendered **before** the (P6) corrections
existed, i.e. the same probe source compiled against the tree at `ab8bf99a`.

    gh19_s6_plan.tsv        the probe's DECLARED stimulus plan, written before its first render
    gh19_s6_scenarios.tsv   the probe's manifest: id -> raw file, warm, win, f0_meas_hz
    gh19_s6_scn*.raw        raw float64 windows, from the same render

## Why only 8 of the 168 cells carry their raw windows

`gh19_s6_scn{125,126,127,128,133,134,135,136}.raw` are exactly the eight `m50000` cells — the
SINE node, `morph = 0.5`, four sample rates × two VCOs.

They are **provenance, not a judged basis**, and that changed in this task. `sine_node_bit_diff_count`
used to compare the candidate's window against these windows frame by frame. It does not any more:
it is a **same-build A/B** against arm B, a second binary built from this same tree with the two
correction terms neutralised (`--neutral-probe`). The reason is in the task report and summarised
below; the consequence for this directory is that **no judged criterion reads these bytes**.

The other 160 windows are NOT committed (they are ~33 MB, and 192 KB each) because nothing reads
them:

* the 120 improvement cells and the 8 anti-phase-midpoint cells are read out of the analyzer's
  MATRIX (`base_report.txt`), columns `A` / `res_db` / `res_1k5k_db` / `res_effbd_db` / `fund` /
  `dc` / `max_abs_out` — no window is opened;
* `GUARD-FREQ` reads `f0_meas_hz` from `gh19_s6_scenarios.tsv`, which IS committed in full.

## Why the equality criterion stopped reading this arm

The default patch evaluates `std::sin` (`vco.h:508`). This arm was rendered on macOS / Apple libm;
CI is glibc. Measured on 200 000 identical arguments the two libms disagree on **3.76 %** of them,
every difference exactly **1 ULP**. So the criterion read **0** locally and **652** in CI, and a
literal pre-S6 tree rendered by the CI compiler reproduced the same 652 — the gap was the toolchain,
not the change. A criterion whose baseline cannot be reproduced bit-for-bit by the compiler under
test is measuring the compiler.

## What pins this arm

`tools/run_gh19_s6_saw_pipeline.py` carries three SHA256 constants and REFUSES before measuring
anything if any of them moves, each under its own name so a run says *which* artifact changed:

* `REFUSE BASELINE-PIN` — `gh19_s6_scenarios.tsv`. Fixes the stimulus, the window geometry, and the
  `f0_meas_hz` every `GUARD-FREQ` bound is computed from. JUDGED.
* `REFUSE BASELINE-REPORT-PIN` — `base_report.txt`. Every improvement column is read out of this
  file's residual columns, never re-derived from the arm. JUDGED.
* `REFUSE BASELINE-RAW-PIN` — the eight `m50000` windows below. PROVENANCE: no criterion reads them
  any more. The pin is kept because they are **committed evidence** — the task report makes claims
  *about* these bytes, and an edited artifact makes those claims unverifiable — and because a
  checkout whose evidence has been edited should be refused rather than silently accepted.

All three digests are also recorded in `../evidence/baseline_pins.txt`, and
`tools/run_gh19_s6_saw_baseline_report_pin.py` demonstrates the three refusals fire by name before
any measurement and issue no verdict. Because the raw pin's old claim is no longer true, that runner
**asserts the demotion instead**: moving a committed window must be **inert** to the verdict, and the
criterion's real input — arm B, rendered fresh by the same run — is tampered in its place (one ulp in
one window), which must fire `FAIL-EQUALITY-SINE` with the improvement columns untouched.
