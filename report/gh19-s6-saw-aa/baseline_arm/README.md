# S6 baseline arm — the NAIVE head's own output (task #120, GH #19)

This directory is the FIXED HISTORICAL BASELINE the improvement columns are measured against. It
is not a copy of the candidate: it is what the product rendered **before** the (P6) corrections
existed, i.e. the same probe source compiled against the tree at `ab8bf99a`.

    gh19_s6_plan.tsv        the probe's DECLARED stimulus plan, written before its first render
    gh19_s6_scenarios.tsv   the probe's manifest: id -> raw file, warm, win, f0_meas_hz
    gh19_s6_scn*.raw        raw float64 windows, from the same render

## Why only 8 of the 168 cells carry their raw windows

`gh19_s6_scn{125,126,127,128,133,134,135,136}.raw` are exactly the eight `m50000` cells — the
SINE node, `morph = 0.5`, four sample rates × two VCOs. They are here because exactly one criterion
reads raw windows out of this arm:

* `sine_node_bit_diff_count` compares the candidate's window against the baseline's **frame by
  frame**, and the failure report names the differing frame numbers, so the baseline window has to
  be present as samples. A whole-window digest would not do: it can only say "equal" or "not", and
  the criterion is a COUNT of differing frames.

The other 160 windows are NOT committed (they are ~33 MB, and 192 KB each) because **nothing reads
them**:

* the 120 improvement cells and the 8 anti-phase-midpoint cells are read out of the analyzer's
  MATRIX (`base_report.txt`), columns `A` / `res_db` / `res_1k5k_db` / `res_effbd_db` / `fund` /
  `dc` / `max_abs_out` — no window is opened;
* `GUARD-FREQ` reads `f0_meas_hz` from `gh19_s6_scenarios.tsv`, which IS committed in full.

So the absence of a window here is not a hole that the gate falls through silently: reading a raw
that is not present is a REFUSAL BY NAME (`REFUSE-EQUALITY-SINE ... could not read the base window`),
not an empty comparison. That path was exercised when this arm was staged — pointing `--base-arm` at
a directory holding only the manifest refuses 8 times with exit 4 (no judgement issued).

## What pins this arm

`tools/run_gh19_s6_saw_pipeline.py` carries three SHA256 constants and REFUSES before measuring
anything if any of them moves, each under its own name so a run says *which* artifact changed:

* `REFUSE BASELINE-PIN` — `gh19_s6_scenarios.tsv`. Fixes the stimulus, the window geometry, and the
  `f0_meas_hz` every `GUARD-FREQ` bound is computed from.
* `REFUSE BASELINE-REPORT-PIN` — `base_report.txt`. Every improvement column is read out of this
  file's residual columns, never re-derived from the arm.
* `REFUSE BASELINE-RAW-PIN` — the eight `m50000` windows below. `sine_node_bit_diff_count` compares
  these **frame by frame**; neither pin above covers a byte of them.

All three digests are also recorded in `../evidence/baseline_pins.txt`, and
`tools/run_gh19_s6_saw_baseline_report_pin.py` demonstrates that each refusal fires by name before
any measurement, issues no verdict, and protects something the gate actually reads.
