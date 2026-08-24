# P2-⑤ Findings and Repayment Debts

Slice: P2-⑤ — snapshot publication path (Half 1) and state persistence
(Half 2). Author: @Pi (implementer). Adjudicator: @Claude (engineering
director). Team lead: @bearbone.

This file records two things deliberately kept separate:

- **The honest boundary of this slice** — what P2-⑤ could and could not prove.
- **Five pre-existing debts** — work owed to a later slice, each with an owner,
  a repayment point, and a status. An *owed debt* is a real task that must be
  repaid; a *declared boundary* is a known-unverified limit, not a defect.

---

## Honest boundary (declared, not a defect)

`deviation` — the P2-⑤ persistence claim is **order and interruption visibility
only**. The temp-file → flush → atomic-rename ORDER is proven by
`tests/core/test_state_persistence.cpp` (a mid-rename fault leaves the live file
untouched and loadable; an in-place write corrupts it). What is **NOT proven**
is true power-off durability: `fsync` durability cannot be established in this
environment (the file-system include ban keeps `unistd.h` out of core, so `fsync`
is not core-expressible), and a power cut between rename and metadata commit is
not modelled. The honest claim is therefore:

> **顺序已验证，掉电持久性未验证。**

This file and `state_persistence.h` never assert "crash-safe".

---

## Repayment debts

| # | Debt | Type | Owner | Repayment point | Status |
|---|------|------|-------|-----------------|--------|
| 1 | Host window height clamps so the 2× test render does not fit-to-window (P1 ④) | Owed debt | @Pi | P5 exit — real window sizing / full UI | Open |
| 2 | Real CoreAudio output-stream per-channel (interleaved) verification | Owed debt | @Pi | P3 exit — a real host callback feeding events into core merges into this | Open |
| 3 | `real_path` not routed through the executor in P2-③ (mixed-partition judge on the audio path) | Owed debt | @Pi | P3 exit — real modules exist; judge = ① (mixed partition) | Open |
| 4 | Power-loss persistence not verified (only order + interruption visibility proven) | **Declared boundary** (NOT a task, not a defect) | Team | — | Knocked-down unverified, recorded in the honest boundary above |
| 5 | Windows real-machine audio/MIDI verification | Owed debt | @Pi | First Windows distributable build | Open |

Notes on classification:

- **#4 is a boundary, not a debt.** It is not expected to become a task with a
  "fixed" status; it is a limit that must be re-verified when the persistence
  layer gains real `fsync`. It is listed here so it is never silently promoted
  to "we proved crash-safe".
- **#1, #2, #3, #5 are owed debts.** They are real and must be repaid at the
  stated exit; they are not "declared out of scope and forgotten".
- **Repayment points are slice-gates, not timestamps** — the debt is closed when
  the gate at that point re-checks it and passes.

---

*This file is a documentation artifact of P2-⑤, not a runtime input to the core
library. It lives at the repo root alongside the source that it debts-forward to
the next phase.*
