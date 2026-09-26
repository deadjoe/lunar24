# task #121 — GH #19 S6 PR-B: dynamic-PWM reference convergence, and a gate that separates two classes of evidence

Baseline: `c422197e9362a56fb6fea127da888536977a37a4` (`origin/main` tip at claim time; the PR #46 merge).
Worktree: `wt-121-gh19-s6-prb-dynpwm`, branch `feat/19-s6-prb-dynpwm`, **unpushed**.
Nothing was pushed, no PR was opened, no registry / wire / default mapping / gate threshold was
touched, the shared checkout was not written to, and GH #19 / #15 remain open with **no MET claim**.
`build-prb/` is a regenerable artifact and is left untracked.

**This slice has no production delta.** `git diff --name-only c422197e93... -- core host tests/probes`
is empty (§0). It measures the product and pins the measurement; the only tracked files that move are
the build targets for the shadow arms and the analyser's R1 instrument.

**WHERE THIS STANDS — read this before the evidence.** Running the legs surfaced **seven defects, every
one of them in this slice's own new files** (§6.4, §6.5 and §6.7). None is a product defect; none is
waived. The first three blocked the slice outright. The next three were found *because* fixing the first
three let more of the machinery actually execute — a suite that dies early hides the tail of its own
checks, and a CTest entry that dies on its own command line never runs at all. The seventh was found
differently: by checking a published claim against the tree rather than by running anything (§6.7), and
it is the one that invalidated evidence this report had already presented. All seven are fixed, and all
four legs are **green at that tree** — legs 1 and 2 through the registered entry
`gh19_prb_dynpwm_acceptance` (exit 0, `verdict=PASS refusals=0 failures=0`, all 15 controls
`ok=15 bad=0 controls=15 verdict=ALL-HIT`, `skipped=(none)`), leg 3 through the registered entry
`gh19_prb_dynpwm_pipeline_mutant` (exit 0, `PRB-MUTANT PASS`), leg 4 by re-running the two standing
surfaces unchanged (§6.8).

**An eighth was found by CI rather than by running anything, and it is why this report has a §5.1.** The
first push of this branch was red on exactly one of the four platform jobs — `windows-latest`, failing in
**3m18s**, i.e. build time, not test time — because the `improvement-bypass` stager emitted the shipped
function body beneath its own `return`, and MSVC reports the unreachable remainder at `/W4` (C4702) which
`/WX` turns fatal, while clang's and gcc's `-Wall -Wextra` do not diagnose unreachable code at all. **The
same mutation text had been compiling green for months inside the ubuntu-only `probe-gate` job**; this
branch is what first made the four-platform matrix compile it. The fix deletes the unreachable remainder
and *asserts* the cut rather than assuming it, and the measurement is unchanged — four byte-identical
report digests and a byte-identical rebuilt neutral probe (§5.1). Counting it, this slice surfaced
**eight** defects, all of them in this slice's own new files, none in the product.

1. **The pinned `improvement_min_db = 6.0` sat above the pristine corrected arm's own measured minimum.**
   Four independent instruments reported the same three `44100 Hz / square / ratio 20` cells below it
   (worst 4.70 dB), out of 56 judged dynamic cells; every other cell cleared. The criteria itself labelled
   this value `borrowed_from_s3_not_derived_here` and states the requirement as *separation* between the
   two arms, which holds for anything in (0, 4.70]. **Fixed:** re-pinned to **4.0**, and its provenance
   changed from `borrowed_from_s3_not_derived_here` to `derived_from_prb_survey`, with the distribution it
   was derived from recorded in `evidence/improvement_distribution.txt` (median 7.485 dB over the same 56
   judged cells). The two quantities that were *not* re-pinned are named in §2 so a reader is not left to
   assume a blanket re-pin.
2. **The gate recorded a `PRB-REFUSE-CRITERIA` refusal for a missing criterion and then indexed that key
   anyway**, so it died with a `KeyError` and exited 1 — its RED code, not its REFUSE code — with a
   truncated transcript. The control that exists to catch exactly this could not hit. **Fixed:** `run()`
   now returns at the refusal, immediately after `parse_criteria` and before any block that reads a
   criterion value; sufficiency is argued in §3.1, not assumed.
3. **The control suite's sandbox rolled back the file bytes but not its own in-memory text cache**, so each
   criteria mutation was applied *on top of* the previous one. The controls were therefore not the
   single-change controls their own docstring promises, and the suite died preparing its fifth one, having
   completed 5 of its 15 controls. **Fixed:** the sandbox snapshots the pre-mutation text and `restore()`
   re-reads every touched key from the restored file and asserts it equals that snapshot, naming the first
   diverging line. **All 15 controls now run: `NEGCTL-PRB ok=15 bad=0 controls=15`, `verdict=ALL-HIT`.**
4. **The fence's `emitted` count was reconciled against nothing.** `check_fence` compared the fence's
   `emitted` to the fence's own `declared` — both read off the same line — so a data row deleted from the
   matrix reconciled clean, while the function's docstring claimed it was reconciled against what was
   actually read. **Fixed** (§3.2): the parsed row list is passed in and the two must agree.
5. **A negative control was aimed at a surface the gate never reads.** `report_column_dropped` edited the
   first `id … res_db … fund` header in the report; that report carries three matrix blocks of that shape
   and the gate parses only the **dynamic** one, so the control tested a different program. It surfaced as
   a loud `MISS` only because it expected a *refusal* — the same mis-aim in a no-change control would have
   passed vacuously. **Fixed:** the containing block is located first, then the header inside it.
6. **The registered leg-3 CTest entry could not run at all.** `add_test(gh19_prb_dynpwm_pipeline_mutant …)`
   passed `--degen-probe <binary>`, which `run_gh19_prb_dynpwm_mutants.py` does not accept: the entry died
   on `unrecognized arguments` in **0.14 s**. Leg 3 had therefore never run *as registered* — the leg-3
   evidence in this report's earlier revision came from a hand-invocation of the same runner. **Fixed:**
   the argument is removed, and removing it is the isolation rather than an omission, because the runner
   compiles all three arms itself — a prebuilt binary would have been a second, unstated difference on the
   compile line the comparison claims to be about.

The code-wiring change itself (§7) is **accepted on both directions** of the two-direction rule: identity
is byte-identical at the pinned criteria and non-degenerate, and liveness puts the criteria's sentinel on
both printed surfaces end-to-end, with the pre-wiring build as the negative control. **No PR was opened**;
the director reviews the legs first.

---

## 0. THE PRODUCTION DELTA — WHY IT IS EMPTY

```
$ git diff --name-only c422197e9362a56fb6fea127da888536977a37a4 -- core host tests/probes
$ git diff --stat c422197e9362a56fb6fea127da888536977a37a4
 CMakeLists.txt                 | 114 +++++++++++++++++++++++++++++++++++++++++
 tools/gh19_s3_pulse_analyze.py |  21 ++++++--
 2 files changed, 132 insertions(+), 3 deletions(-)
```

Correction, kept visible rather than quietly fixed: this block carried **106** and **124 insertions** in an
earlier revision — numbers taken from a run of this command made **before** the last edits to
`CMakeLists.txt`, and presented here as the command's output ever since. It is the same defect §6.7 is
about, in this report's own §0: a recorded artifact that was true when recorded, cited later as if it were
current. The block and the sentence below now carry the values this tree actually reports (`114`, `132`),
re-derived by running both commands again. §11 carried `+114/-0` throughout, so that column and this block
now agree instead of contradicting each other.

`CMakeLists.txt` (+114, additions only) declares the two new binaries
(`gh19_prb_degen_probe`, `gh19_s3_pulse_probe_neutral`), the header-staging target
(`gh19_prb_shadow`, a `add_custom_command` running `tools/stage_gh19_prb_shadow.py
--mutation improvement-bypass`), and **three** `slow` CTest entries — `gh19_prb_degen_probe`,
`gh19_prb_dynpwm_acceptance` (the pipeline, run with `--negcontrol`) and
`gh19_prb_dynpwm_pipeline_mutant` (leg 3). The neutralised arm's compile line
is the probe's, unchanged, with `target_include_directories(... BEFORE PRIVATE)` putting the staged
shadow root ahead of `core/include`. `tools/gh19_s3_pulse_analyze.py` (+21/-3) adds `--taps2`, R1's
instrument (§8), and nothing else. No kernel, no header, no probe under `tests/probes/` moves.

## 1. WHAT THIS SLICE IS FOR

The 48 moving-duty cells named by the S3 criteria have no individual reference cell. The pinned
`baseline_arm/` carries no traces at all, so a pipeline that judged them would have to render every
arm itself. Two facts had to be established before a gate could be written about them:

1. **"un-referenced" is a budget, not a capability.** `tools/gh19_s3_pulse_analyze.py:1431-1432` takes
   the first `--dynamic-cells` cells after tri-priority sorting (default 8). The reachability census
   (task #121 §9, `lunar24-task121/reference_reachability_48.tsv`) records every one of the 48 having a
   reachable reference: 22 tri, 26 sq. A full-coverage run is achievable and was demonstrated:
   `declared=56 emitted=56 skipped=0 unreferenced=0` (64886 B, sha `b7fb53a2…`).
2. **The third surface, `skipped`, can wash the gap away silently.** A budget shortfall can present as
   `skipped` rather than as a loud `unreferenced` count, so the criteria pin `skipped=0` explicitly:
   the gate refuses to read a matrix whose coverage was achieved by silently skipping cells.

## 2. THE PINNED EXPECTATION

`report/gh19-prb-dynpwm/acceptance_criteria.tsv`, 431 lines,
sha256 `0e11bb9af6a657b19d6ad48098f0593cdf947dc37db27ef6364c402dafa41cdb`.

Line 3 states the contract:

```
# This file is the EXPECTATION. tools/gh19_prb_dynpwm_acceptance.py READS it and NEVER writes it.
```

The **sixteen** `code_*` rows are the gate's complete code vocabulary: the nine it was first given
(`code_floor_missing`, `code_floor_unresolved`, `code_trace`, `code_band`, `code_cellset`,
`code_missing_arm`, `code_improvement`, `code_degen_identity`, `code_degen_nonvacuous`) plus the seven it
also prints and the criteria had not named (`code_criteria`, `code_plan`, `code_matrix_shape`,
`code_matrix_exit`, `code_scenarios`, `code_trace_shape`, `code_window`). Until 2026-09-25 **all sixteen**
were declared and never read — the gate printed its own literals. §7 is the record of wiring them; §7.3 is
the record of why stopping at nine would have been the wrong completeness claim.

**The one number this slice re-pinned, and the two it deliberately did not.** `improvement_min_db` moved
6.0 → **4.0** and its provenance from `borrowed_from_s3_not_derived_here` to `derived_from_prb_survey`;
`evidence/improvement_distribution.txt` carries the distribution it was derived from (over the 56 judged
cells: min 4.70, median 7.485) together with an addendum recording that regenerating it changes exactly
one line. Two quantities that *look* like re-pin candidates were left as they were, and the reason is not
tidiness — symmetry is not a rule here:

* `scale_delta_provenance` stays `borrowed_from_s3_not_derived_here`: that number is **reported, not
  gated**, so a borrowed one needs no derivation, and the criteria keeps the marker to say so.
* `floor_margin_min_db` stays **6.0** and is `[DERIVED, not fitted]` (criteria `:75-95`): M = 6 bounds the
  floor's amplitude contamination of the reported residual at 0.97 dB — "at most 1 dB", the budget this
  slice pins — against a minimum judged margin of **7.24 dB** and **0/56** refusals at that M. It was
  derived from this slice's own survey, before any run of the gate, not chosen so a cell would pass, and
  the criteria prints the whole refusal-vs-M sensitivity row so that claim can be checked rather than
  believed.

Two negcontrol rows make the gate's own falsifiability part of the expectation. Verbatim apart from
the separator — the file is tab-separated at lines 277-281 and the tabs are shown here as aligned
spaces so the columns read:

```
criterion  negcontrol_degen_identity_tamper              one_ulp_render_tamper
criterion  negcontrol_degen_identity_tamper_expect_token  PRB-FAIL-DEGEN-IDENTITY
criterion  negcontrol_degen_dispatch_arm                  PRB-DEGEN-DISPATCH-MUTANT
criterion  negcontrol_degen_dispatch_class                PREMISE-LIVE
criterion  negcontrol_degen_dispatch_expect_exit_bit      6
```

## 3. THE GATE — REFUSE BEFORE JUDGE

`tools/gh19_prb_dynpwm_acceptance.py` (961 lines). Every refusal check runs before any judgement, and
a cell that fails an input check gets a NAMED REFUSAL and **no verdict**:

* exit 0 — `PASS`; exit 4 — `REFUSE`; exit 1 — `RED`. (Setup errors, as opposed to verdicts, are 2.)
  All three tools in this slice share the convention: `gh19_prb_dynpwm_acceptance.py:52-54`,
  `gh19_prb_dynpwm_gate_negcontrol.py:47-49`, `run_gh19_prb_dynpwm_mutants.py:73-75`.
  **These are not the probe's exit bits.** The degen probe's bit 5 / bit 6 in §4 are bits *within* a
  probe's own exit code and mean STIMULUS and PREMISE; the gate's 0/1/4 are the verdict codes an outer
  runner tests with `!= EXIT_PASS` / `!= EXIT_RED`. The two numbering schemes are independent and
  reading one as the other is a mistake this report made in an earlier draft.
* A refusal is not a red verdict about the product. The gate says so itself:
  `PRB NOTE no judgement is issued: the artifacts failed an input check, so a red or green verdict here
  would not be a statement about the product.`
* A code is recorded once, as a **bare suffix**, and the printed token is derived from the bucket
  (`PRB-REFUSE-<suffix>` / `PRB-FAIL-<suffix>`). The same recorded string feeds the per-code detail
  line and the counts line, so the two surfaces cannot disagree (§7).
* The gate has exactly **three** FAIL codes: `IMPROVEMENT`, `DEGEN-IDENTITY`, `DEGEN-NONVACUOUS`.
* Only the first `MAX_DETAIL` examples of a code print in full; the counts line carries the total.

The identity leg sits in the JUDGEMENT section, **after** the refusal return, so a band refusal hides
the identity result entirely. Verifying the identity therefore requires a report that passes BAND —
which is what R1's instrument exists to produce (§8).

### 3.1 Why one early return is sufficient, rather than merely present

A missing criterion is a *refusal* in this gate's vocabulary, not a crash — and a Python `KeyError`
traceback exits **1**, which is this tool's RED code. That collision is the whole of defect 2: a gate
that had issued no verdict at all was reported to its caller as a failed product. The fix is at the top
of `run()`, and it is three lines because the right place is the one place that knows:

```python
crit, sval, axis, cells, pairs, reports = parse_criteria(g, args.criteria)
...
if g.refusals:
    return finish(g, "REFUSE", EXIT_REFUSE)
```

Sufficiency is an argument about the data, not about the placement: `crit` and `sval` are built **only**
by `parse_criteria`, every required key is checked for presence there, and no other code writes them. So
an absent key is always recorded as a refusal before that line, and every index below it is reached only
with a complete criteria. The comment in the source says the same thing, so the next reader does not have
to re-derive it. Proven by the control that exists for exactly this case (§6.2): `--only
criteria_drop_criterion` now reports `exit=4 expected=4 code=PRB-REFUSE-CRITERIA HIT`; before the fix it
was a `KeyError` traceback, `exit=1`, and a truncated transcript.

### 3.2 The fence count is reconciled against the rows actually parsed

`check_fence` reconciles each count the matrix fence carries, and the half that was missing until
2026-09-25 is the one that ties the fence to the file:

```python
if parsed is not None and parsed != emitted:
    g.refuse("MATRIX-SHAPE", "%s: the fence declares emitted=%d but %d data rows were read "
                             "between the fences; ..." % (name, emitted, parsed))
```

`emitted != declared` compares the fence to **itself** — both numbers come off the same line — so it can
never notice a row deleted from the file; until this check existed, a matrix missing a row was reconciled
clean while the function's docstring claimed the reconciliation was against what was read. The
producer-side invariant the criterion rests on is `emitted + skipped == declared`, and a skipped cell
prints `no source trace` instead of a data row; so on any run this gate does not already refuse,
`parsed` must equal `emitted` exactly, and neither face can wash the other. Proof of aim: the control
`report_cell_row_dropped` refuses with `MATRIX-SHAPE` (§6.2).

## 4. TWO CLASSES OF EVIDENCE, AND WHY THEY ARE NOT INTERCHANGEABLE

The slice carries two different claims about the degenerate cells, and a single bit cannot express
both. `tools/gh19_prb_degen_probe.cpp:83-99` names them as separate exit bits:

* **bit 5 — STIMULUS.** The traced SOURCE span of every cell must be `> 0`: an INPUT to the product.
* **bit 6 — PREMISE.** The traced `effectiveDuty()` span must match the declaration: what the product
  DID with that input. A product-derived quantity.

> WHY 5 AND 6 ARE TWO BITS AND NOT ONE. They read two different quantities — bit 5 the traced source
> (an INPUT to the product), bit 6 the traced effectiveDuty() (what the product DID with that input) —
> and folding them together would leave a reader unable to tell "the stimulus was not what we declared"
> from "the product did not do what we declared" without reading prose.

The two classes are therefore reported separately everywhere in this slice, and the criteria name them
in separate rows (§2). A run firing bit 6 is **PREMISE-LIVE** evidence; it is not identity evidence,
and the gate's `DEGEN-IDENTITY` criterion is never reached by that arm.

## 5. THE FOUR ARMS

| arm | how it is built | what it is for |
| --- | --- | --- |
| A `probe_A` | the tree's probe, unmodified | the candidate (corrected) arm |
| B `probe_B` | the tree's probe with the staged `improvement-bypass` header prepended | the neutralised arm — improvement must vanish |
| C `degen_C` | `gh19_prb_degen_probe.cpp` with the staged `degen-dispatch` header | PREMISE-LIVE (fires bit 6) |
| A2 `degen_A2` | `gh19_prb_degen_probe.cpp`, pristine | the identity's stabilising side |

`tools/stage_gh19_prb_shadow.py` (304 lines) performs the header staging. It refuses unless the anchor
occurs exactly once and the result differs from the input — and, for the bypass mutation, unless the
region it *deletes* is pinned at both ends to statements of the shipped body (§5.1). Its output for the
improvement-bypass mutation, verbatim (every line carries the `PRB-SHADOW ` prefix; the shadow root in
the path is the caller's `--out`, not a fixed name):

```
PRB-SHADOW mutation=improvement-bypass path=/tmp/shadow-recheck-rev3/lunar24/core/pulse_blep_kernel.h anchor_hits=1
PRB-SHADOW tree_sha256=08fde1f63daf46ae6dd1ac0313bd37fa51186bc3e57e9bec11b34a53b7cbea75
PRB-SHADOW mutated_sha256=afbc1a1f7bad115d169a338c9bca6a984a990b91afd5f29ce6a750fc7e4fecda differs=yes
PRB-SHADOW dead_body_removed_bytes=982 unreachable_under_bypass=yes
```

`tree_sha256` is **unchanged** from the value this report recorded before the Windows fix
(`08fde1f6…`, §5.1), and that is the independent confirmation that the fix edited the *generator* rather
than the shipped header: the header's own bytes are the same file, here and in the shadow root. What
moved is the mutation text — `mutated_sha256` went from `8ae91eb6…` to `afbc1a1f…`, and the fourth line,
reporting the deleted region, is new. The output was re-produced after the fix, so the mutation is
reproducible from the shipped stager and the shipped header rather than recorded from a run whose inputs
have since moved.

What the bypass does, verbatim: the stager replaces the function head with one whose parameters are
**unnamed** (`double /*t*/, double /*duty*/, double /*dt*/`), writes a short control banner into the
body, and then `return 0.0;  // CONTROL: the two-edge correction bypassed entirely`. The statements the
shipped header carries below that point — the `dt` guard, the capped kernel width, and the two-edge
residual — are **not re-emitted**; the stager cuts them out and asserts the cut, 982 bytes (§5.1). So
the correction contributes nothing and everything else in the function stays put. The generated file
lives only in a shadow include root; the shipped header is untouched — confirmed by
`git diff --name-only main -- core host tests/probes` being empty and by `tree_sha256` above.

The idiom is not new here: `main` already carries the same construction — `tools/stage_gh19_s6_shadow.py`
generating a shadow root for `gh19_s6_saw_probe_neutral`, declared with the same `BEFORE PRIVATE`
include order and `lunar_enable_warnings`. **But that is an argument about the idiom, not about this
mutation text, and an earlier revision of this report drew the wrong conclusion from it** — it said that
S6's PR-A (#46) merging with all four platforms green proved an unreachable statement of this shape
passes this repo's `/W4 /WX` build on MSVC. It proves no such thing, and the difference is what §5.1
records: S6's substitution deletes statements **inside a live block** — its replacement keeps the body
reachable and drops a `(void)r;` use — so it creates no unreachable code at all, whereas this slice's
bypass did. The text this stager reuses is S3's, and S3's mutated compile happens **only inside the
ubuntu-only `probe-gate` job**, so the four-platform matrix had never compiled it before this slice
turned the same mutation into a CMake target.

### 5.1 The Windows build break on the first push, and what the fix changed

This is the **eighth** defect of this slice (the preamble counts seven found by running it); this one was
found by CI, on the first push, and it is the one whose repair had to be shown not to move any measurement.

The first push of this slice was red on exactly one of the four platform jobs and green on the other
three. `windows-latest (cl)` failed in **3m18s** — build time, not test time — with

```
build/gh19_prb_shadow/lunar24/core/pulse_blep_kernel.h(128,1): error C2220: the following warning is treated as an error [gh19_s3_pulse_probe_neutral.vcxproj]
build/gh19_prb_shadow/lunar24/core/pulse_blep_kernel.h(128,1): warning C4702: unreachable code
build/gh19_prb_shadow/lunar24/core/pulse_blep_kernel.h(135,1): warning C4702: unreachable code
build/gh19_prb_shadow/lunar24/core/pulse_blep_kernel.h(138,1): warning C4702: unreachable code
```

verbatim from the job log apart from the runner's timestamp prefix, the `D:\a\lunar24\lunar24\`
checkout prefix and the backslash separators. **Three** sites, one per statement the bypass left beneath
itself — which is the mechanism stated rather than described. The mechanism is a property of the tools, not of the product:
the bypass replacement emitted `return 0.0;` and then the shipped body beneath it, so every statement in
that body was dead code. **MSVC reports unreachable code at `/W4` (C4702), and `/WX` makes that fatal
(C2220); clang's and gcc's `-Wall -Wextra` do not diagnose unreachable code at all.** Three platforms
green and one red is therefore the expected shape of this mistake rather than evidence against it — and
because the four-platform matrix runs with `--label-exclude slow`, unlike `probe-gate` it compiles this
arm on every push.

Two repairs were rejected on purpose. An MSVC-only `/wd4702` would be this repo's **first** warning
suppression in a non-`third_party` file (there are zero today), and it would also falsify the arms'
"identical flags" property that the comparison rests on. Moving the compile out of the matrix would hide
the red and leave the control arm uncompiled on four platforms. The fix is in the generator: **delete
the unreachable remainder rather than suppress the warning about it.**

Deleting it introduces a second-order trap, which is why the deletion is *asserted* and not merely
performed. With the body gone, nothing references the parameters, so `/W4` emits **C4100** unreferenced
formal parameter — the obvious repair would trade one fatal warning for another. Both are fixed in one
pass, by unnamed parameters. Then five refusals guard the cut, each because the failure it prevents
yields a control arm that **builds and passes while measuring a different experiment than the report
describes**: the cut must start at the replacement's own end and at a line break; exactly one
closing-brace landmark may follow it; the cut must end at that landmark; no closing brace may appear
inside the cut before its end; each named body statement must occur exactly once within it; above the
body's first statement only blanks and comments may be discarded; and the region must be byte-identical
to the same region derived from the **shipped** text through the anchor. Each refusal was exercised
against the shipped text before the fix was committed, and it fires with its own named reason rather than
staging a wrong arm.

**The neutrality of the deletion is evidenced by artifact identity, not by reports.** The neutral probe
was rebuilt from the new shadow header and compared to the pre-fix build byte for byte. Both binaries are
still on this host and hash equal today:

```
fea9601cff31866b96decdb787181cf2b6723324941920884d03484a0827a935  /tmp/prb-rev1/gh19_s3_pulse_probe_neutral   (preserved before the fix)
fea9601cff31866b96decdb787181cf2b6723324941920884d03484a0827a935  build-prb/gh19_s3_pulse_probe_neutral        (built from the fixed stager)
```

Had the removed statements ever reached the emitted code, removing them would have changed the binary. That is stronger *and* cheaper than "the two runs' reports agree", because it does not depend
on the report format, the `--label`, or the analyzer.

**And the reports agree anyway.** Legs 1+2 and leg 3 were re-run at the fixed tree through their
**registered CTest entries** (#76 and #77), and all four report digests are byte-identical to the values
this report recorded before the fix — which are committed in
`evidence/leg1_leg2_registered_entry_green.txt` and `evidence/leg3_registered_entry_green.txt`. The
post-fix transcripts are `evidence/leg1_leg2_registered_entry_green_windowsfix.txt` and
`evidence/leg3_registered_entry_green_windowsfix.txt`; those two pre-fix files are left in place rather
than overwritten, because they are the comparison partner this table cites.

| report | pre-fix (committed) | post-fix (re-run) |
| --- | --- | --- |
| `cand` (legs 1+2) | `3efee15b…` | `3efee15b95d4567c2b1af557fecc71cb488f704831c9bb6757192bc02a4c61aa` |
| `neut` (legs 1+2) | `07a18188…` | `07a181884f273de461ac5c320ad9b4ac3d2c5fe1ca1a6c6cc28e66d11b660da7` |
| `probe_A` (leg 3) | `15bc6c94…` | `15bc6c94aab5d5b226d00ee6a6e3afcfee9bec2a2f80f246743b1a41e6e5272e` |
| `probe_B` (leg 3) | `82ac1c7f…` | `82ac1c7fd18251201effba2f6151cdc14698ea1f464170fe8250ca82709cb6c2` |

Both entries exit 0 — `gh19_prb_dynpwm_acceptance` in 1733.33 s (`verdict=PASS refusals=0 failures=0`,
`NEGCTL-PRB verdict=ALL-HIT ok=15 bad=0 controls=15 skipped=(none)`) and `gh19_prb_dynpwm_pipeline_mutant`
in 1727.39 s (`verdict=RED refusals=0 fail=PRB-FAIL-IMPROVEMENT=56`, `PRB-MUTANT PASS`). Those runtimes
are longer than the pre-fix 1557.65 s / 1554.34 s because both re-runs ran concurrently on this host.

Leg 3 does not use the CMake binaries at all. It stages its own include roots and compiles every arm
itself with the compiler CMake uses (`--compiler`), so that the pristine arms get an **empty** include
root — their compile line therefore differs from the control arms' rather than silently sharing it —
and so that the `degen-dispatch` arm, which is not a CMake target, is built at all.

**Arm C, the `degen-dispatch` premise arm, and its shas.** It stages its mutation as a shadow include
root, not a source edit — `PRB-SHADOW mutation=degen-dispatch path=…/inc_degen_dispatch/lunar24/core/vco.h
anchor_hits=1`, mutated root sha `02d0b9a0e199220c80327ce9f105bb7c0802506b50cc2e3569beca7a03a9fdc4`.
Its two manifests are `181dd75c7f3c6afb98f9ecc18eb920130b0c0ed43c7eae68f8bda05e2b1b8f92`
(`gh19_s3_scenarios.tsv`, the static side) and
`c380467a6e3c6ba7a390baf2955f57306b03e7fecef6378974b385b4c8cdf911`
(`gh19_prb_degen_scenarios.tsv`, the constructed cells) — the second sha is identical in the pipeline
leg and in leg 3's arms, which is what lets the two legs' identity inputs be the same bytes.

The premise bit is a check and not a permanently-firing alarm, and that is shown rather than asserted:
the **pristine** run of the same driver exits **0**, and only the mutated arm exits **64**.

```
MUTANT degen_pristine_exit=0 (must be 0: otherwise the premise bit below is a permanently-firing alarm rather than a check)
MUTANT degen_dispatch_exit=64 stimulus_bit=False premise_bit=True fatal_premise=6 fatal_stimulus=0 measured_src_span=10,10,10,9.99958,9.99958,9.99958
```

So the dispatch mutation moves the six constant-duty cells onto the moving branch, by construction six of
them report `FATAL-PREMISE`, none reports `FATAL-STIMULUS`, and the source span is **measured, not `-`**.

## 6. THE FOUR LEGS

The criteria file enumerates **four** legs (§6 of `acceptance_criteria.tsv`), and none substitutes for
another: **leg 1** asks whether the corrected reference is reachable and bit-aligned on the product's own
output, and whether the gate comes out green end to end; **leg 2** asks whether the gate is capable of
refusing at all, and whether it can tell a wired code from a dead one; **leg 3** asks whether the gate's
verdict actually *discriminates* — a gate that passes everything and a gate that reads its own
declarations both pass leg 1; **leg 4** asks whether this slice disturbed anything that was already
standing, and answers only "unchanged" (§6.8).

Correction, kept visible rather than quietly fixed: an earlier revision of this report said "the three
legs" and described only legs 1–3. The criteria file it was written against has carried four since the
slice was planned. The heading and this paragraph now match the file, because the number of legs is a
claim about what was checked, not a stylistic choice.

### 6.1 Leg 1 — reference reachability and alignment (complete)

The analyzer's alignment proof on the full 152-cell arm, at the pinned recipe, reports

```
  -> 146/146 cells align (unexplained == 0)
```

and the dynamic block's band declaration, verbatim from `evidence/leg1_alignment_report.txt:505`:

```
  BAND-DECL dynamic b1_lo=100.000 b1_hi=5000.000 eff_lo=0.000 eff_rule=decimator_0.1db eff_hi_cycles=0.491875000 edge_L=0.491875000 edge_2L=0.491875000  (eff_hi = 21692 Hz @44100, 47220 Hz @96000)
```

`unexplained == 0` is the load-bearing part: every cell that fails to align must be *named* with a frame
index and a reason, so a zero here means no cell was quietly dropped from the proof rather than that all
cells were perfect.

**Provenance of the arm, because a reachability claim is only as good as the arm it was measured on.**
The arm is `/tmp/gh19-s3-arm`, and it carries its own plan (`gh19_s3_plan.tsv`, `plan_rev 1`, 152 data
rows: 80 static + 6 mixed + 56 dynamic + 8 connect + 2 ab_asymmetry). Cross-checked against the
reachability survey this slice is built on, all **48** moving-duty cells named in
`../lunar24-task121/reference_reachability_48.tsv` are present in that arm — `comm -23` returns 0 — so
leg 1 measures exactly the cells this slice is about, at full coverage (56 dynamic cells declared, 56
emitted).

The one permitted-class failure is `ALIGN-FIRST-EDGE … err=0.066115702`, recorded as a named class rather
than folded into the count; it is reported by the analyzer, not suppressed by this slice.

**The gate end-to-end, at the registered entry.** Leg 1's other half is the CTest entry
`gh19_prb_dynpwm_acceptance`, and it is green at the fixed tree — `evidence/leg1_leg2_registered_entry_green.txt`,
exit 0, 1557.65 s. It checks its own premise before judging:

```
76: PRB-PIPELINE PRECONDITION product_diff=OK no product path differs from main
76: PRB GATE criteria=acceptance_criteria.tsv cells=56 pairs=6 plan_dynamic=56
76: PRB GATE verdict=PASS refusals=0 failures=0
```

`product_diff=OK` is the runtime form of §0's empty production delta: the pipeline re-derives it instead of
trusting the report to have said so.

**The re-pinned threshold is exercised here, not merely written down.** The same run's verdict line is
`PRB REPORT improvement_min=4.7000 dB at vco_a_dynpwm_44100_220_sq_r20_b25_d100 (corrected -30.1600 vs
neutralised -25.4600, pinned 4.0)`. That is the *minimum* over all 56 judged cells and it is the cell that
was below the old 6.0 pin, so this run is the direct evidence that the re-pin of §6.4 blocker 1 changed the
verdict from RED to PASS — with 0.70 dB of margin, not a hairline. The floor axis is separate and is
reported beside it (`report_floor_margin_db=7.2400`, `refused_cells=0` of 56 scanned).

### 6.2 Leg 2 — the gate's own refusal controls (identity and liveness both run)

**The two-direction acceptance for the code wiring (§7) is complete, and it was re-derived.** Both
directions run end-to-end against two builds of the gate. The current record is
`evidence/wiring_two_direction_rev2.txt`, which carries the builds, the isolation assertion, both
directions and its producer verbatim; the earlier record `evidence/wiring_two_direction.txt` is kept
beside it because §6.7 is visible in it.

*Identity, at the pinned criteria* — the shipped gate and the pre-wiring build were run directly, at the
pinned criteria, over the same artifacts. **Both exit 0**, the `diff` of the two transcripts is empty,
and both hash to `df292f312b20461dda146fea17b974799e65431e125270014b3dda5dcb98c7d2` — the wiring cannot
change a byte of this gate's output while the criteria holds the default values.

*The isolation is asserted, not assumed.* The comparison only means anything if the two builds differ by
the wiring and nothing else, so the producer computes the diff and refuses the reading otherwise:
`ISOLATION diff_hunks=1 wired_lines=3 removed_lines=1 wiring_only=True`, printed in the evidence. That
assertion exists because the rev-1 pair **failed** it (§6.7).

*Anti-degeneracy, because two identical transcripts prove nothing if both are the same error.* This
transcript is 32 lines, carries 23 `PRB REPORT` lines, `cells=56 pairs=6 plan_dynamic=56`,
`refused_cells=0` and its own verdict `PRB GATE verdict=PASS refusals=0 failures=0` — a real verdict
table, and the shipped PASS rather than a shared failure. The rev-1 identity was a 36-line RED
transcript (`PRB GATE verdict=RED refusals=0 failures=3`), a real table too but not evidence about a
passing gate; it is superseded. An **earlier attempt in this slice compared four transcripts that were
all one setup error and printed `IDENTITY … True`**; that run is superseded as well, and it stays
recorded because a degenerate identity pass is exactly the failure a reader should expect this section
to have caught.

*The same claim, reached a second way.* The control runner's whole suite except the liveness control,
run against both builds, also produces byte-identical transcripts
(`0278117bc5028576cae8fba4ba53a823a27fc1cd6632cf8c11c7507ca15ae135`, 19 lines, 14 controls,
`ok=14 bad=0`, `verdict=ALL-HIT`). A bare gate invocation and the control machinery agree.

*Liveness, end-to-end* — through the real control machinery
(`gh19_prb_dynpwm_gate_negcontrol.py --only criteria_code_key_is_read`), which renames `code_cellset` to
the sentinel `LIVENESS-SENTINEL-7Q4Z` and drops the `degen_pair` rows to force a CELLSET refusal. The
shipped gate reports `HIT` (`exit=4 expected=4 code=PRB-REFUSE-LIVENESS-SENTINEL-7Q4Z`,
`NEGCTL-PRB verdict=ALL-HIT`); the pre-wiring build reports `MISS`, having printed its own baked-in
`PRB-REFUSE-CELLSET`, with the sentinel `ABSENT FROM detail-line,counts-line`. The pre-wiring build is
the negative control that shows this control *can* fail. Both transcripts reproduced **byte-identically**
at the rev-2 pair (`5e92e916f7e49c9d` shipped, `3b2987c15918142a` pre-wiring), so the vocabulary
completion that invalidated rev-1's identity left this direction untouched — a small independent check
on that later change. Per-code, **all sixteen** `code_*` rows are live on both surfaces — the probe was
re-run at the shipped gate and the sixteen-row criteria, and it reports `ALL 16 LIVE ON BOTH SURFACES:
True` (`evidence/wire_per_key_16.txt`, §7.2). The nine reached through a variable are additionally
*reachable*, which is what this sentinel control exercises end to end; the seven that appear only as
bare literals cannot be renamed by the criteria at all, so a runtime probe can show their map entry is
live but not that a run arrives there — that half is the code-reading fact of §7.3, and the two
instruments are not substitutes for each other.

*The liveness direction was first obtained through a sandbox criteria, and here is the record.* The
control runner executes `pristine` first and declares the suite INVALID unless the unmutated artifacts
PASS; at the criteria as it then stood they did **not** (§6.4, blocker 1), so the control had to be run
against a copy differing from the pinned file on **exactly one line** (`improvement_min_db` 6.0 → 4.0,
both files then 395 lines), sha `210e339bcd05ec2acd205d20343330c3d45525ff677770d115bcb14825e4a17d`. The
wiring's mechanism does not depend on that number — the map is keyed by the criterion's *key name* and the
control works by renaming a code's *value* — and with blocker 1 resolved the sandbox is no longer needed:
the liveness control now runs at the pinned criteria, inside the full suite, in the same `ok=15 bad=0` run
recorded next.

**The other controls.** With `pristine` passing, the suite now reaches **all 15** of them:

```
NEGCTL-PRB ok=15 bad=0 controls=15
NEGCTL-PRB verdict=ALL-HIT
```

That is the whole difference from the blocked state, so it is stated plainly: an earlier revision of this
section said "All fifteen controls hit" was false, and it was — the suite then completed 5 of 15, died
preparing the sixth, and never reached its own summary line (§6.4, blocker 3). The summary and all fifteen
`HIT` lines are in `evidence/negcontrol_full_after.txt` (§12); the blocked transcript is kept beside it as
`evidence/negcontrol_suite_abort.txt`.

### 6.3 Leg 3 — mutant discrimination (both orientations, at the registered entry)

The design is a two-orientation control over the *same two reports*: the acceptance orientation
(`cand=probe_A neut=probe_B`) must pass, and then the mutant orientation (`cand=probe_B neut=probe_A`)
must be rejected *by name* as `PRB-FAIL-IMPROVEMENT`, with no refusal and exactly that one code. Both
orientations now hold, in one run of the registered CTest entry
(`evidence/leg3_registered_entry_green.txt`, exit 0, 1554.34 s):

```
77: MUTANT orientation=acceptance cand=probe_A neut=probe_B exit=0 verdict=PASS refusals=0 fail=-
77: MUTANT orientation=mutant     cand=probe_B neut=probe_A exit=1 verdict=RED refusals=0 fail=PRB-FAIL-IMPROVEMENT=56
77: PRB-MUTANT PASS: the corrected arm passed in the acceptance orientation; the neutralised arm was
   rejected BY NAME as PRB-FAIL-IMPROVEMENT in the mutant orientation, over the SAME two reports and
   without a refusal and without a crash. …
```

The mutant orientation's rejection is *exactly* that one code over 56 cells, with `refusals=0` — the
runner prints the counts line `PRB COUNT FAIL PRB-FAIL-IMPROVEMENT n=56` and no other code, so the
discrimination is named rather than inferred from a bare exit status. The acceptance orientation returning
`verdict=PASS` is what makes the mutant orientation informative: before the re-pin of §6.4 blocker 1 the
acceptance orientation RED, and the runner **refused to certify** the leg rather than reporting a red
mutant as a catch. That refusal was the guard working, and it is kept in this report's history rather than
deleted — the blocked log is `evidence/leg3_mutant_full.txt`.

**This run is the registered entry, not a hand-invocation** — the distinction §6.5 defect 6 was about, and
the reason the leg has a transcript here at all. Line 12 of the evidence names the exact command, and
`--degen-probe` is absent from it.

**The degen arm rides along, and it is PREMISE evidence rather than identity evidence.** The same run
reports `MUTANT degen_pristine_exit=0` (the premise bit is a check, not a permanently-firing alarm),
`degen_dispatch_exit=64 stimulus_bit=False premise_bit=True fatal_premise=6 fatal_stimulus=0`, and
`measured_src_span=10,10,10,9.99958,9.99958,9.99958`. The two `PRB-DEGEN` blocks printed around it are
the separation: on the pristine arm the six `const_duty` cells read `duty=0` and the six `moving_duty`
cells read `duty≈0.499`, while the degen-dispatch arm pushes all twelve to `duty≈0.499` — the mutation
measurably moves what it claims to move. This is a **verdict about the premise being live**, and it is
**not** a verdict about the identity; the identity's falsifiability control is the one-ulp tamper in leg 2
(`degen_identity_one_ulp` → `PRB-FAIL-DEGEN-IDENTITY`, §6.2). The two are kept apart deliberately; the
runner's own closing paragraph says so in the transcript.

**Cross-leg consistency, checked rather than assumed.** The plan and both degen manifests are byte-equal
across the two legs: plan `621b0c758959b206…`, `degen_dyn` `c380467a6e3c6ba7…`, `degen_static`
`181dd75c7f3c6afb…`. The two *report* files these legs produce are **not** byte-equal, and the reason is
in the analyzer's own source rather than in the numbers: a report's first and last lines carry `--label`
(`gh19_s3_pulse_analyze.py:832`, `:1560`), the pipeline labels its arms `prb_cand`/`prb_neut` while the
mutant runner labels them `probe_A`/`probe_B`, and no other run-varying text enters a report — a retained
report contains **zero** absolute paths (`grep -c` over `evidence/leg1_alignment_report.txt` returns 0),
which is why differently-labelled arms cannot hash alike. Stated as what it is: the label difference is
established by source and by the absence of paths; the two files were not diffed byte-for-byte because the
runners remove their temp trees on exit, and no claim here rests on that un-diffed pair. What the legs
*symmetrically* report over the same plan is `improvement_min` against the same pinned threshold —
`4.7000 dB` at `vco_a_dynpwm_44100_220_sq_r20_b25_d100` in leg 1 (§6.1), above the pin, with leg 3's
acceptance orientation passing under that same pin.

### 6.4 The first three defects, and their resolution

The three below are the defects that **blocked the slice**: each one stopped a leg from issuing a
verdict. None is a product defect; all three are in this slice's own new artifacts; nothing here is
waived. Full record of the blocked run: `evidence/acceptance_orientation_blocker.txt`. The resolutions
are at the end of this subsection, and the three *further* defects that only became reachable once these
were fixed are §6.5 — a suite that dies early hides the tail of its own checks.

**Blocker 1 — the pinned improvement threshold sits above the pristine arm's own minimum.** Four
independent instruments agree on the same three cells: the pipeline (fresh arms it renders itself),
leg 3's acceptance orientation, the control suite's `pristine` control, and the gate run directly. All
report `refusals=0 failures=3`, all three cells `44100 Hz / square / ratio 20`. The table's `pinned`
column is the **historical** pin — the value in force when these four instruments reported, and the value
that made them agree on a failure:

| cell | corrected | neutralised | improvement | pinned |
|---|---|---|---|---|
| `vco_a_dynpwm_44100_220_sq_r20_b50_d50` | −30.89 dB | −25.10 dB | 5.79 dB | 6.0 |
| `vco_a_dynpwm_44100_220_sq_r20_b25_d100` | −30.16 dB | −25.46 dB | 4.70 dB | 6.0 |
| `vco_b_dynpwm_44100_220_sq_r20_b50_d50` | −30.89 dB | −25.10 dB | 5.79 dB | 6.0 |

The whole distribution — not only the part under the line — is in
`evidence/improvement_distribution.txt`, computed from the two analyzer reports using the gate's own
definition (`d = neut.res_db − cand.res_db`, `acceptance.py:818`) and calibrated against the gate: it
reproduces the gate's printed `improvement_min=4.7000` at the same cell and all three named failures.

```
all 56 judged dynamic cells: min 4.70  max 12.61 dB, 3 below the pin
r2  n=22: min 6.52  max 12.61     every r2 cell clears the pin
r20 n=34: min 4.70  max  9.33     the three sub-pin cells live here
tri n=30: min 6.52  max  9.20     no tri cell is below the pin
sq  n=26: min 4.70  max 12.61
```

This is a criteria problem, and the criteria says so itself (`:138-147`, `:304`): `improvement_min_db`
is `borrowed_from_s3_not_derived_here`, and the criterion's stated requirement is only that the value
**separate** the corrected arm from the neutralised one — *"any threshold strictly between the two
separates them"*. Separation holds for anything in (0, 4.70]; the borrowed 6.0 is a magnitude from
another slice's cell mix, and this slice's own full-coverage measurement refutes its transfer. The
quantity class matches; the magnitude does not.

It is newly visible because PR-B takes dynamic coverage from the analyzer's default 8 cells to all 56:
the three cells did not change, they entered the judged set for the first time.

**Blocker 2 — the gate records a refusal for a missing criterion and then indexes it anyway.**
`gh19_prb_dynpwm_acceptance.py:573`, in `check_floor_and_shape`:

```
if margin < crit["floor_margin_min_db"]:
KeyError: 'floor_margin_min_db'
```

The gate has already emitted `PRB-REFUSE-CRITERIA criterion/numeric keys missing=floor_margin_min_db`,
so the refusal is *recorded and not acted on*; the run dies with a traceback and Python exits **1** —
the gate's RED code, not its REFUSE code — with no `PRB GATE verdict=` line, no COUNT lines and no
REPORT lines ever printed (`evidence/gate_crash_dropped_criterion.txt`).

Consequence: the control `criteria_drop_criterion` *must* hit and cannot; the full suite reports
`exit=1 expected=4 … MISS (CRASHED)`. This is **independent of blocker 1** and survives its resolution:
the pipeline runs the control suite with `check=True`, so a MISS there propagates and the pipeline exits
non-zero. (For the record: this pipeline leg never reached that step — it stopped at step 4 on blocker 1
and its transcript contains no `NEGCTL` lines at all. The control-suite evidence in §6.4 comes from
running that suite directly.)

**Blocker 3 — the control suite's own sandbox rolls back the file but not its in-memory text cache, so
the controls are not single-change controls.** `tools/gh19_prb_dynpwm_gate_negcontrol.py`'s `Sandbox`
keeps `self.text` (the loaded inputs) and `self.undo` (the original bytes per touched path). `text_file()`
writes **both**; `restore()` rewrites **only the file** and clears `undo` — it never puts `self.text[key]`
back. Every later mutation therefore reads the previous mutation's text:

```
:113  def restore(self):
:114      problems = []
:115      for p, before in self.undo.items():
:116          with open(p, "wb") as fh: fh.write(before)   # the FILE is restored ...
:123      self.undo = {}                                   # ... self.text is never rolled back
```

The gate reads the file, so each gate run *is* the intended single change; it is the *mutations* that
compound. The mutations' own header says so: `# Mutations. Each takes the sandbox and the resolved arm
paths and changes EXACTLY ONE thing.` (`:170`). **The gate's own output carries the proof**: the
single-change control `criteria_unknown_key` — which only appends one invented row — prints *three*
accumulated refusals at once, two of them belonging to controls 1 and 2:

```
| PRB-REFUSE-CRITERIA …:394: unknown criterion key 'improvement_min_db_invented'
| PRB-REFUSE-CRITERIA criterion/numeric keys missing=floor_margin_min_db unexpected=-
| PRB-REFUSE-CRITERIA criterion/string keys missing=negcontrol_degen_identity_tamper unexpected=-
```

`floor_margin_min_db` was dropped by control 1; `negcontrol_degen_identity_tamper` by control 2. By
control 5 (`criteria_code_key_is_read`) the accumulated text no longer carries the six `degen_pair` rows,
so its own guard fires and the suite dies **preparing** that control:

```
control setup: the criteria do not carry six degen_pair rows
```

So a full suite run completes **5 of 15 controls** — `pristine` (PASS) plus four mutation controls, all
four `MISS (CRASHED)` on blocker 2 — and exits non-zero by `SystemExit`, not by a verdict. Two
consequences worth naming: the suite's summary line is never reached, so the suite reports nothing about
its own incompleteness; and `--only` masks this defect, because a single-control run has no earlier
mutation to compound (which is exactly how the §6.2 liveness direction was obtained).

**Resolution — all three fixed in this slice; none waived, none excepted.**

1. **Blocker 1, fixed by re-pinning on measurement rather than preference.** `improvement_min_db` is now
   **4.0** with provenance `derived_from_prb_survey`. This is not "lower it until it passes": the
   criterion requires *separation*, 4.0 lies inside the interval (0, 4.70] that separation admits, and it
   was chosen as a magnitude this slice's own full-coverage measurement supports rather than one imported
   from another slice's cell mix. The distribution behind it is filed as
   `evidence/improvement_distribution.txt`, so the claim can be checked against the numbers rather than
   against this sentence. The director's ruling authorised the move; the measurement is what says by how
   much.
2. **Blocker 2, fixed by refusing where the refusal is recorded** (§3.1). The control that exists for
   exactly this case now hits: `--only criteria_drop_criterion` → `exit=4 expected=4
   code=PRB-REFUSE-CRITERIA HIT`; before, a `KeyError` traceback and `exit=1`.
3. **Blocker 3, fixed by rolling back the cache with the file** — the sandbox snapshots the pre-mutation
   text, and `restore()` re-reads every touched key from the restored file and asserts it equals that
   snapshot, naming the first diverging line. Two properties of that check are load-bearing, because its
   first version was itself wrong: it compares against a **pre-mutation snapshot** rather than against the
   restored file's text (the cache legitimately holds the *mutated* text until it is rolled back, and a
   check that fires on the correct state is worse than no check — it gets switched off), and it re-reads
   through the same reader instead of re-encoding a string, so universal-newline translation on a Windows
   runner cannot manufacture a mismatch.

**A residual limit of that check, named rather than fixed.** The rollback is verified by *re-reading the
file*, so a second process mutating the same path between the restore write and that read makes the suite
report its own restore as failed. That is not hypothetical: it happened while this slice's legs were
re-run, when the hand-invocation control (R1) ran concurrently with the registered entry `#76` against the
same worktree. The file the control suite mutates is the **worktree's** criteria file — `Sandbox.write()`
writes `self.paths[key]`, and the registered entry passes the relative path
`report/gh19-prb-dynpwm/acceptance_criteria.tsv` — so two pipeline legs running at once are genuinely
racing for one file. R1's two error strings corroborate each other:

```
CONTROL criteria_drop_criterion SETUP FAILED: restoration did not take for
  report/gh19-prb-dynpwm/acceptance_criteria.tsv (the text cache was not rolled back:
  one text is a prefix of the other (want 30714 bytes, got 30030))
control setup: the criteria do not carry six degen_pair rows          # stderr
```

The second line is exactly the transient text of a sibling suite running `criteria_drop_degen_pair`.
**CI cannot hit this**: both ctest invocations in `.github/workflows/ci.yml` (`:76` and `:128`) run without
`-j`, so the two `slow` entries are serial there. The resolution is the cheap one — re-run the same argv
with no sibling process. That run exits **0**, `NEGCTL-PRB ok=15 bad=0 controls=15`, `verdict=ALL-HIT`,
with **zero** `SETUP FAILED` lines, and it reproduces both report digests (`3efee15b…`, `07a18188…`)
(`evidence/hand_invocation_repeatability_windowsfix.txt`). So the earlier exit 1 is reproduced as a
concurrency artifact rather than argued away; it is a limit of running two legs at once against one
worktree, not a measurement difference, and not something CI can reach.

What matters is not "three fixes landed" but that **the suite now runs to the end and says so**:
`NEGCTL-PRB ok=15 bad=0 controls=15`, `NEGCTL-PRB verdict=ALL-HIT`, with a printed summary line the
blocked suite never reached.

**And the three fixes are confirmed by re-running the legs, not only by the direct runs that found them.**
Every number in this subsection was obtained by running the artifact *by hand* against a hand-built pair
of arms; the confirmation is that the two **registered CTest entries** now pass end to end at the fixed
tree — leg 1+2 (`#76`, `verdict=PASS refusals=0 failures=0`, `ok=15 bad=0 controls=15`, `skipped=(none)`)
and leg 3 (`#77`, `PRB-MUTANT PASS`). Full transcripts: `evidence/leg1_leg2_registered_entry_green.txt`
and `evidence/leg3_registered_entry_green.txt` (§12). Those two runs are also what makes §6.5 defect 6
visible as fixed rather than merely argued.

### 6.5 The three further defects, which only fixing the first three made reachable

These were found *because* blocker 3 was fixed. While the suite died at control 5, controls 6–15 never
ran; their defects were not concealed by anything subtle — they simply had no opportunity to appear. The
general shape is worth carrying out of this slice: **a suite that stops early is stale evidence about its
own tail.**

**Defect 4 — the fence's `emitted` count was reconciled against nothing.** `check_fence` compared
`emitted` to `declared`, both read off the same fence line, so a data row deleted from the matrix between
the fences reconciled clean — while the function's own docstring claimed the fence was reconciled against
what was actually read. Fixed per §3.2; aim confirmed by `report_cell_row_dropped` → `MATRIX-SHAPE`.

**Defect 5 — a negative control aimed at a surface the gate does not read.** `report_column_dropped`
took the first line matching the matrix header and deleted a column from it. The report carries **three**
matrix blocks of that shape; the gate parses only the **dynamic** one. The control edited the static
block, the gate never looked at the edited line, the gate exited 0, and the control reported `MISS`.
Fixed by locating the containing block first and the header inside it — the same locate-then-target shape
the sibling controls already use.

Why this one is worth more than its fix: it surfaced loudly only because it expected a **refusal**. Had it
been a no-change control — the other common shape, and the one whose entire value is "this change is
inert" — the same mis-aim would have **passed vacuously** and the suite would have reported `ALL-HIT`
while testing nothing. Mis-aim is self-revealing in refusal-expecting controls and silent in
zero-disturbance ones, which is the opposite of where a reader's suspicion usually sits.

**Defect 6 — the registered leg-3 CTest entry could not run at all.** `add_test(gh19_prb_dynpwm_pipeline_mutant …)`
passed `--degen-probe <binary>`, which `run_gh19_prb_dynpwm_mutants.py` does not accept. Run as
registered, the entry died in **0.14 s** with `error: unrecognized arguments: --degen-probe …`, so leg 3
had never once run in the form CI runs it: the leg-3 evidence in the previous revision of this report came
from invoking the same runner by hand with the arguments I believed were registered. Fixed by removing the
argument — and the removal is the isolation rather than an omission, because the runner compiles all three
arms itself, from the same sources with the same compiler and the same flags, differing only in which
shadow include root is first; handing it a prebuilt binary would put a second, unstated difference on the
very compile line the comparison is about. The reasoning is recorded beside the entry in `CMakeLists.txt`
so a later reader does not helpfully add it back.

The lesson is not "add `--degen-probe`". It is that **a hand-invocation is not a stand-in for the
registered entry**: the two differ in exactly the place a mistake lives, and only one of them is what the
gate actually runs.

### 6.7 The seventh defect, found by checking a claim rather than by running anything

Defects 1–6 each announced themselves: a leg failed, a suite died, a control missed, an entry exited in
0.14 s. This one did not. It was found by reading §11's own sentence — *"the pre-wiring control copy
… differs from the shipped gate by exactly the three-line map-population hunk"* — and testing it instead
of trusting it, which took one `diff`:

```
8 hunks, not 1.  gate_prewiring.py sha 10a6405707ea9a23…  vs  shipped gate sha 0c09ee537773a415…
added 60 lines, removed 12
```

**What had happened.** The 17:16–17:19 two-direction acceptance paired a pre-wiring copy derived from the
gate **as it stood at 16:51** with the gate **as it stood at 17:18**. Both artifacts then moved: the
director's ruling that the code vocabulary must be complete added seven more `code_*` suffixes and
rewrote the wiring's comment (the gate's mtime is 19:27), and the criteria gained those seven rows. The
copy was never re-derived, so it silently stopped being "the shipped gate minus the wiring" and became
"an older gate minus the wiring". The identity it had established (`f88a49d9…`) was real but was about
the superseded pair.

**Why this is the most instructive of the seven.** It is not a wrong number in a file; every individual
fact in §11 was true when written. It is a *stale comparison partner*, and a stale comparison partner is
invisible by construction — cross-validation against a copy is exactly the check designed to look
trustworthy, and it is exactly the check that silently degrades when one side moves. The suite's own
`pristine` control cannot catch it, because `pristine` verifies the *shipped* build only. Nothing in the
machinery was going to report this; only re-reading the claim against the tree did.

**Fixed** by re-deriving the copy from the current shipped gate: one exact-string removal of the wiring
loop, yielding `548c41b3ed78470e…`, whose diff against the shipped gate is **one hunk, 3 added / 1
removed**, asserted by the producer (`ISOLATION wiring_only=True`) rather than asserted by me. Both
directions were then re-run at the current criteria (`0e11bb9a…`), and the results are **stronger than
rev 1's**: the identity now holds between two builds that both exit **0** with `PRB GATE verdict=PASS
refusals=0 failures=0` (`df292f312b20461d…`), where rev 1's shared transcript was a 36-line RED from
blocker 1. The liveness pair reproduced byte-identically. Full record:
`evidence/wiring_two_direction_rev2.txt`; the standalone transcript:
`evidence/wiring_identity_pinned_rev2.txt`. The rev-1 files are kept, not replaced, because they are
where the defect is visible.

Two facts that keep this from being worse than it is, both checked rather than assumed: the wiring's
identity claim does not depend on the vocabulary rows (the seven added names are never reached through a
variable — §7.3 — and the liveness transcript, which does exercise the wired path, is byte-identical
before and after), and the criteria the liveness control is exercised against in-suite is the current
pinned criteria, so the *in-suite* liveness control was never stale. Only the hand-run pre-wiring
comparison was.

**The rule this adds to the ones above:** a comparison against a second build is evidence only if the
difference between the two builds is re-derived at the moment of the comparison. Pin the pair, or the
pair drifts and the identity quietly becomes a statement about the past.

### 6.8 Leg 4 — zero-disturbance, reported and not re-adjudicated

Leg 4 re-runs two surfaces that were already standing before this slice, exactly as they are; this slice's
claim about them is only "unchanged", and it fixes neither:

* **`gh19_s3_pulse_acceptance`** — S3's existing 72-cell gate, CTest entry `#72`, re-run as registered
  through the same pipeline entry, which imports **this slice's modified analyzer**. It **passes**:
  `ACCEPT-GATE verdict=PASS refusals=0 failures=0`, all **22** controls `RED-HIT` (`ok=22 bad=0`), in
  **236.28 s**. Full transcript: `evidence/leg4_s3_acceptance.txt`. This is the zero-disturbance claim
  for the one product-adjacent file this slice touches (`tools/gh19_s3_pulse_analyze.py`, `+21/-3`): the
  gate that was standing before still reaches the same verdict, with no refusal and no failure, run by
  the entry CI already runs. It corroborates rather than replaces §8's byte-identity A/B, which proves
  the analyzer's own outputs unchanged; this shows the next consumer up agreeing.
* **`python3 tools/check_registry_complete.py --require-full`** — the coverage gate CI runs as its own
  job, re-run as CI runs it. It exits **1**, and that is the standing state, not something this slice
  introduced: the gap is **12 parameters, itemised as 8 Root-A non-scalar (structural — must NOT be
  flattened into scalar parameters) and 4 selector-toggle with no evidenced value domain (must stay a gap
  until the value domain is evidenced)**. That red is **known and by-design, and no exception has been
  granted for it**; this PR neither claims one nor re-adjudicates the surface. The full output is quoted
  verbatim in §12.

## 7. THE CODE-WIRING CHANGE AND ITS TWO-DIRECTION ACCEPTANCE

The criteria's `code_*` rows were, until 2026-09-25, **declared and never read**: renaming one changed
the gate's output not at all, while the criteria file's own line 3 promised that the gate READS it.
The wiring is two lines and one rule:

```python
suffix = self.codes.get(suffix, suffix)      # first line of both refuse() and fail()
...
for key, value in sval.items():              # after the prefix assignments, in parse_criteria
    if key.startswith("code_"):
        g.codes[key[len("code_"):].replace("_", "-").upper()] = value
```

Mapped **at record time**, not at print time, because the recorded suffix is what the per-code detail
line prints *and* what the counts line counts — the property the counts line exists to provide. The
derivation is a rule over the key name, not a second key→suffix table that could drift from the call
sites. An **empty map is the identity** (`get(s, s)`), so on the shipped criteria, where every value
equals its derived suffix, the wiring cannot change a byte.

### 7.1 The two directions, and why one is not enough

| direction | what it proves | instrument |
| --- | --- | --- |
| identity | the wiring changed nothing else | full control suite under both builds, `--skip criteria_code_key_is_read`, transcripts must be byte-identical |
| liveness | the criteria is actually consulted | the same suite's sentinel control: a renamed code must print the criteria's string |

The director's ruling on the second, quoted from `#Lunar24:2feabffe` message `e9341380`, verbatim apart
from the leading item number: *"「输出不变」只证必要不证充分，死读同样输出不变；(b) 活性向（沙箱哨兵→必须印出哨兵，明细行与 counts 行都变）才是把死读排除掉的判别器。两向都过才算接线成立，照此验收。"*

### 7.2 Per-key liveness, all sixteen rows

Each row renamed to a sentinel in a copy of the criteria, parsed by the gate's own parser, recorded
through the gate's own `refuse()`, both surfaces read back —
`report/gh19-prb-dynpwm/evidence/wire_per_key_16.txt`, re-run over every row the criteria now carries:

```
criteria code_* rows: 16
key                      value              detail counts default
code_floor_missing       FLOOR-MISSING      True   True   False  OK
code_floor_unresolved    FLOOR-UNRESOLVED   True   True   False  OK
code_trace               TRACE              True   True   False  OK
code_band                BAND               True   True   False  OK
code_cellset             CELLSET            True   True   False  OK
code_missing_arm         MISSING-ARM        True   True   False  OK
code_improvement         IMPROVEMENT        True   True   False  OK
code_degen_identity      DEGEN-IDENTITY     True   True   False  OK
code_degen_nonvacuous    DEGEN-NONVACUOUS   True   True   False  OK
code_criteria            CRITERIA           True   True   False  OK
code_plan                PLAN               True   True   False  OK
code_matrix_shape        MATRIX-SHAPE       True   True   False  OK
code_matrix_exit         MATRIX-EXIT        True   True   False  OK
code_scenarios           SCENARIOS          True   True   False  OK
code_trace_shape         TRACE-SHAPE        True   True   False  OK
code_window              WINDOW             True   True   False  OK
ALL 16 LIVE ON BOTH SURFACES: True
```

The earlier revision of this section reported the **nine** rows that revision carried
(`evidence/wire_per_key.txt`, scoped there and in §12). Sixteen is the stronger fact and is the one that
belongs here: the map is consulted at the record site for **every** row, and for every row the recorded
string reaches both surfaces while the criteria' default reaches neither.

What this does **not** give is reachability. Seven of the sixteen reach their call sites only as bare
literals, which the criteria cannot rename, so a runtime probe cannot show a real run arriving at them;
that is the code-reading fact §7.3 establishes, and the two instruments are not substitutes. The nine
that are reached through a variable are the ones a real run can be *made* to exercise, which is what
§6.2's sentinel control does.

`code_missing_arm` needed this check: its call sites record through a local variable
(`ma = "MISSING-ARM"`) rather than a string literal, so a text search for literals reports **zero**
call sites for it and would have read as a dead row. The map is keyed by the suffix derived from the
key name, which is exactly the value that variable holds, so the row is live.

### 7.3 The vocabulary is the criteria's for all sixteen codes it can print

This was reported as a limitation first, and the director ruled on it: **completeness of the vocabulary
outranks stability of the criteria's sha.** So the criteria now carries a `code_*` row for every suffix
the gate can emit, all sixteen — the nine that were already named, plus `CRITERIA`, `PLAN`,
`MATRIX-SHAPE`, `MATRIX-EXIT`, `SCENARIOS`, `TRACE-SHAPE` and `WINDOW`.

The reason the original nine were the wrong nine is worth keeping: they were the codes the gate reaches
through a *variable* — a call site that reads a name out of the map — while the other seven are emitted
as bare literals. The wiring followed the variables, so the vocabulary ended up covering exactly the
half that was already dynamic and leaving the hard-coded half unnamed. A reader of the criteria could
not rename `code_criteria`, and could not even see that `CRITERIA` was part of the vocabulary. "The
criteria decides the code vocabulary" was therefore half true, which is the shape of claim this slice
removed twice elsewhere.

Verified the only way it can be: the criteria's own sha changed, and the gate's output did not. §6.2's
identity and liveness pair is re-run against the sixteen-row criteria and both directions hold — the
sentinel still reaches both surfaces, and the transcript at the pinned criteria is still byte-identical
to the pre-wiring build's. The re-run is the one recorded in `evidence/wiring_two_direction_rev2.txt`
(§6.7): the vocabulary change is what invalidated the rev-1 pair, so the re-run *is* the proof for the
sixteen-row criteria rather than a repetition of an older one.

### 7.4 The criteria is read-only, as its own header promises

The gate has exactly ONE write site, the optional `--out` transcript (line 915). `args.criteria` is
read at 615 and 943 and appears elsewhere only as a basename in a log line, so line 3's promise —
*"This file is the EXPECTATION. tools/gh19_prb_dynpwm_acceptance.py READS it and NEVER writes it."* —
is now mechanically true.

### 7.5 The end-to-end liveness run

Run, and reported in full in §6.2: the shipped gate prints the criteria's sentinel on both the detail
line and the counts line (`HIT`, `NEGCTL-PRB verdict=ALL-HIT`), while the pre-wiring build misses and
prints its own literal. The per-code counterpart is `evidence/wire_per_key.txt`.

The rev-1 run needed a sandbox criteria differing from the pinned file on one line, for the reason given
in §6.2 and §6.4. The rev-2 run does not: it uses the pinned criteria, copied and asserted byte-identical
before use (`CRITERIA src_sha256=0e11bb9a… copy_sha256=0e11bb9a…`). The sandbox was a workaround for a
blocker that no longer exists, and it is recorded rather than quietly dropped because a reader comparing
the two revisions should see why one of them sandboxes and the other does not.

## 8. R1's INSTRUMENT, AND THE BYTE-IDENTITY OF EXISTING INVOCATIONS

R1 is "taps proportional to L": L=8 → 2001 taps, L=16 → 4001, ratio `(4001-1)/(2001-1) = 2.000`. The
analyser already scanned a second reference level at `--taps`; PR-B separates the tap count of the 2L
reference into `--taps2` so the two levels can be scaled together, which is the only way the movement
between them attributes to **resolution alone**.

The flag's default preserves every existing invocation. The claim is not left to inspection: the same
arm is analysed twice — once by `main`'s analyser, once by this branch's — with `--taps2` **absent**,
and the two transcripts must be byte-identical.

Run, and it holds — byte-for-byte, at two flag sets (`evidence/analyzer_ab.txt`):

| flags | main | branch | compare |
|---|---|---|---|
| `--taps 65 --dynamic-cells 8 --conv-cells 3` | exit 3 | exit 3 | IDENTICAL `0d7f8e7f…` (20974 B) |
| `--taps 2001 --dynamic-cells 56 --conv-cells 56` | exit 0 | exit 0 | IDENTICAL `4bd69113…` (26897 B) |

The two shas differ **between** flag sets, as they must — a 65-tap 8-cell survey is a different matrix
from a 2001-tap 56-cell one; the comparison is main-versus-branch *within* a flag set and never across.
`exit=3` at the cheap flags is the analyzer's own status for a short-tap survey, identical on both
revisions, and is neither the gate's RED (1) nor its REFUSE (4).

Measured: with `--taps 2001 --taps2 4001`, `edge_L = edge_2L = 0.491875000`, which is the pinned
`edge_eff_expected` in the criteria exactly.

The *before* number is not something this slice computed to suit itself. S3's merged baseline already
records it, verbatim, at `report/gh19-s3-pulse-product/base_report.txt:359`:

```
  BAND-DECL dynamic b1_lo=100.000 b1_hi=5000.000 eff_lo=0.000 eff_rule=decimator_0.1db eff_hi_cycles=0.483750000 edge_L=0.491875000 edge_2L=0.483750000  (eff_hi = 21333 Hz @44100, 46440 Hz @96000)
```

That one line is the argument. At equal tap counts the two reference levels **disagree**: the L=8 level
reads `edge_L = 0.491875`, the L=16 level reads `edge_2L = 0.483750`. The bound is the narrower of the
two, so `eff_hi_cycles = 0.483750` — pulled in by the level that was starved of taps, not by the
decimator. With taps ∝ L, leg 1 records `edge_L = edge_2L = eff_hi_cycles = 0.491875000`: the two levels
now agree, and the bound lands on the value the criteria pins.

Per the criteria's own words (`:121-123`), that is the whole of R1's value — it **isolates the
variable**; it is not a source of accuracy, and "more taps" must not be read as "more precise". The
criterion carries the measurement (−0.25 … −0.38 dB, mean −0.30) precisely so that a later reader cannot
upgrade the move into an accuracy claim.

The refusal that would follow from the old recipe is an arithmetic consequence of comparing 0.483750
against the 0.491875 bound. It is **not** an executed refusal and this report does not claim to have run
one.

**One defect, found in this slice's own pinned file.** The R1 rationale cites the analyzer's same-tap-count
construction as `gh19_s3_pulse_analyze.py:1433-1434`. Checked against `main`'s file, the correct range is
**1432-1433** — `h = design_decimator(args.taps, …)` and `h2 = design_decimator(args.taps, args.oversample
* 2, …)`. The cited pair starts one line late, so it shows the 2L construction plus an unrelated line and
omits the L construction: half the evidence the sentence rests on. It is the only file-and-line citation
in the criteria (every other reference is bare), and the one other line reference, `:346-359`, was checked
and is sound. The fix is **comment-only** — `_rows()` drops `#` lines at `gh19_prb_dynpwm_acceptance.py:201`,
so no criterion, threshold or verdict is affected — but it still changes the criteria's sha, and that sha
is the pinned expectation recorded in the gate's own output. So it is **reported and not silently edited**;
correcting it is a call for the director.

## 9. WHAT IS NOT CLAIMED

* **No MET claim.** GH #19 and GH #15 remain open. Nothing is merged, nothing is published, and no
  GitHub issue is closed.
* **The `--require-full` gate's twelve named gaps are NOT covered by this slice, and no exception is
  being claimed for them.** They are known by-design and remain **not approved** for this PR. A
  by-design gap needs its own exception, granted per PR by the director; the two that exist are
  @bearbone `3e4c14c6` for PR #45 and `39040e74` for PR #46. Neither covers this change. Any red seen
  on this branch that is one of the twelve is therefore written up as *known by-design, not approved,
  needs its own approval for this PR* — never as a general approval.
* **Not claimed: that the product's moving-duty cells are now band-correct.** This slice pins and
  measures reference convergence. It changes no DSP (§0).
* **Not claimed: that the criteria's vocabulary is the whole vocabulary.** Nine of the sixteen codes
  the gate can print are renamable through the criteria; seven are not (§7.3).
* **Not claimed: that the control suite covers every code.** It exercises nine distinct codes across
  its fifteen controls; the census is in `evidence/suite_code_coverage.txt`.
* **Not claimed: MSVC behaviour from a macOS result.** The local runs are clang on darwin. The
  authoritative surfaces for the Windows path are the four-platform `build-and-test` job and the
  `probe-gate` job, reported separately.
* **Not claimed: that the analyzer's cheap recipe passes.** At `--taps 65 --dynamic-cells 8` the
  analyzer's own self-check reports `exit_code=3` and it exits 3 — on `main`'s analyzer and this
  branch's alike. It is a pre-existing property of that recipe, recorded here so that the A/B §8 is not
  misread as "both runs succeeded".
* **Not claimed: re-derivation of the historical `baseline_arm/`.** That artifact carries no traces, so
  the pipeline renders its own arms; nothing here re-derives the archived numbers.

## 10. COST

**Measured.** One analyser run at the pinned recipe (`--taps 2001 --taps2 4001 --dynamic-cells 56
--conv-cells 56`) is **749.21 s real** (702.45 user, 20.46 sys), single-threaded, measured alone on this
host. The cheap recipe (`--taps 65 --dynamic-cells 8 --conv-cells 3`) is 149.42 s on `main`'s analyser and
146.68 s on this branch's, measured in the same A/B.

**Counted.** The two long legs invoke the pinned recipe **four** times — the pipeline leg twice
(candidate + neutral) and leg 3 twice (probe_A + probe_B) — and the analyzer A/B of §8 invokes it twice
more (once per analyser build) plus the two cheap runs. At 749 s each, the pinned-recipe part is ≈ 50 min of
analyser time across the slice, and the two leg runs that carry two analyses each take ≈ 25 min wall
apiece.

Correction, kept visible: an earlier revision of this paragraph enumerated "leg 1 once, the pipeline twice,
leg 3 twice" under a total of four. That list sums to five and double-counts leg 1, which *is* the pipeline
entry. The total of four was right and the enumeration was wrong; the enumeration is fixed rather than the
total, because the total is what the measured 749 s and the job-increment estimate both rest on.

**The number the director asked to see before merge.** The `probe-gate` job's increment from this slice
is the two new slow entries, `gh19_prb_dynpwm_acceptance` (which itself runs the pipeline: two pinned
analyses plus the refusal controls) and `gh19_prb_dynpwm_pipeline_mutant` (two more). That is **four**
pinned analyses inside one job, so the expected increment is **≈ 25-30 min**, not the ~12.5 min of a
single run. It is an estimate from the measured per-run time and the entry count, not a measured job
duration: the job's real time is only knowable from a CI run, and none has been run on this branch yet.

The pre-existing `probe-gate` job is already long-tailed (observed 53 min to 3 h 40 m, commonly over an
hour), so a 25-30 min increment sits inside the range the job already spans; it is recorded here because
a reviewer budgeting for that job should see it rather than discover it.

**Correction to the paragraph above, from the A/B's own numbers.** The A/B's "pinned" pair is
`--taps 2001 --dynamic-cells 56 --conv-cells 56` **without** `--taps2`, so it is *not* the criteria's
pinned recipe and does not cost 749 s a side: measured **559.04 s** on `main`'s analyser and **565.92 s**
on this branch's (1124.96 s together). The 749 s figure belongs only to runs that also pass
`--taps2 4001`, which doubles the reference work. Recorded because the sentence above, read carelessly,
would overstate the A/B by ~370 s.

**Wall clock, measured.** The two long legs ran concurrently on this host: the pipeline leg spans
**≈ 26 min** (first arm artifact 16:48 → its neutral report 17:14:46) and leg 3 **≈ 27 min**
(16:48:29 → 17:15:40). Both are consistent with the ≈ 25 min predicted above from two analyses per leg,
so the estimate is a measurement now rather than an inference — for these legs, on this host.

**Wall clock, re-measured at the fixed tree.** The post-fix re-runs were also run concurrently, and they
are the numbers a reviewer should budget from because they are the ones at the tree under review:
`gh19_prb_dynpwm_acceptance` **1557.65 s** and `gh19_prb_dynpwm_pipeline_mutant` **1554.34 s** — 25.96 min
and 25.91 min, within 16 s of each other and both inside the ≈ 25-30 min band. The re-runs are marginally
cheaper than the blocked runs despite doing strictly more work (the control suite now completes all 15
controls instead of stopping at 5), because the blocked leg was spending its time in the same two pinned
analyses. Reproduced: the wall clock is not a one-off, it is two independent pairs agreeing to within
2 %.

**Still an estimate.** The `probe-gate` job's increment is still not a measured job duration: no CI run
has been made on this branch, and the job's own overhead (checkout, CMake configure, four platform
builds) is not in any number above. It remains ≈ 25-30 min, and it remains an estimate.

## 11. FILES

| path | sha256 (first 16) | lines | new? |
| --- | --- | --- | --- |
| `tools/gh19_prb_dynpwm_acceptance.py` | `0c09ee537773a415` | 1010 | new |
| `tools/gh19_prb_dynpwm_gate_negcontrol.py` | `0dd52ac6360822c1` | 683 | new |
| `tools/run_gh19_prb_dynpwm_pipeline.py` | `74722094783db0e2` | 271 | new |
| `tools/run_gh19_prb_dynpwm_mutants.py` | `cde170119abc2813` | 396 | new |
| `tools/stage_gh19_prb_shadow.py` | `f838c8cd4b4e34b1` | 304 | new |
| `tools/gh19_prb_degen_probe.cpp` | `c7e806dd0b890ddf` | 683 | new |
| `report/gh19-prb-dynpwm/acceptance_criteria.tsv` | `0e11bb9af6a657b1` | 431 | new |
| `tools/gh19_s3_pulse_analyze.py` | `36226a259020a44c` | +21/-3 | modified |
| `CMakeLists.txt` | — | +114/-0 | modified |

The pre-wiring control copies of the gate, used only as instruments and not as deliverables, are two, and
the sentence that used to stand here is corrected rather than deleted because it was the defect:

* `10a6405707ea9a23…` — the **rev-1** copy, derived from the gate at 16:51 and used for the 17:16–17:19
  acceptance. It **no longer** differs from the shipped gate by the wiring alone: measured, it differs by
  **8 hunks** (60 added / 12 removed), because the gate moved afterwards and the copy did not. This is
  §6.7. An earlier revision of this report asserted here that it "differs from the shipped gate by
  exactly the three-line map-population hunk" — true when written, false once the gate moved, and false
  in a way nothing in the machinery would have reported.
* `548c41b3ed78470e…` — the **rev-2** copy, re-derived from the shipped gate as it stands now by one
  exact-string removal of the wiring loop, and used for the current acceptance. Its diff against the
  shipped gate is **one hunk, 3 added / 1 removed**, and the producer asserts it (`ISOLATION
  wiring_only=True`) before comparing anything.

Both are `/tmp` scratch instruments recorded by sha; the rev-2 one is reproduced verbatim inside
`evidence/wiring_two_direction_rev2.txt`, so the assertion can be re-checked from this repo and that file.

New CTest entries, all three `slow`: `gh19_prb_degen_probe`, `gh19_prb_dynpwm_acceptance` (the pipeline,
with `--negcontrol`, so the gate's own refusal controls run on every CI run) and
`gh19_prb_dynpwm_pipeline_mutant` (leg 3).

**Why these are not the first numbers published for this table.** Three of the files above moved after the
first revision of this report, because the defects of §6.4–§6.5 were fixed in them: the gate gained the
early refusal return and `check_fence`'s parsed-vs-emitted reconciliation; the control runner gained the
sandbox's pre-mutation text snapshot and the re-aimed column-drop control; the criteria gained the re-pin
and the seven newly named `code_*` rows. `CMakeLists.txt` moved for one further reason (§6.5, defect 6):
its registered leg-3 entry passed an argument the runner does not accept, so that entry could not run at
all; the argument is gone, and the comment beside the entry records why re-adding it would break the
comparison's isolation rather than fix anything. An earlier revision of this section read "it cannot go
green as shipped" — true of the tree it described, false of this one.

**Boundaries, verified at the time of writing.** `git diff --name-only main -- core host tests/probes`
is **empty**: no product path differs from `main`. The complete diff against `main` is two files,
`CMakeLists.txt` (`+114/-0`) and `tools/gh19_s3_pulse_analyze.py` (`+21/-3`); everything else in this slice
is a new untracked file. `build-prb/` is untracked and is **not** committed, and the local build tree is
not deleted. Nothing has been committed yet — the branch head is `c422197e9362`, equal to `main`'s tip, so
the whole of this slice is still uncommitted work in the worktree, and no PR exists.

## 12. EVIDENCE FILES

All under `report/gh19-prb-dynpwm/evidence/`. Shas are sha256, first 16 hex.

| file | sha256 | bytes | what it establishes |
| --- | --- | --- | --- |
| `leg1_alignment_report.txt` | `2acd027d1ad19e29` | 64908 | leg 1: 152 cells, `146/146 align (unexplained == 0)`, the dynamic BAND-DECL with `edge_L = edge_2L = 0.491875000` |
| `suite_code_coverage.txt` | `640e73a712aca0c1` | 2315 | the suite's 15 controls and the code each must produce. **Produced against the 9-row criteria revision**, so its "codes the gate can print that NO criteria row names" line is superseded by `code_vocabulary_16.txt` (§7.3); the control→code table does not depend on the criteria's row count and still stands |
| `wire_per_key.txt` | `de7800a9f2f54f75` | 739 | the **nine** variable-reached `code_*` rows live on both surfaces, default absent. **Produced against the 9-row criteria revision** — superseded for liveness by `wire_per_key_16.txt`, which runs the same probe over all sixteen; kept as the revision this section first reported |
| `wire_per_key_16.txt` | `8d67002e5535bb74` | 5808 | §7.2's per-key liveness probe re-run over every row the criteria now carries: `ALL 16 LIVE ON BOTH SURFACES: True`, with its producer and the gate/criteria shas it ran against |
| `wire_unit.txt` | `3b1858729078443d` | 855 | the unit 2×2: wired gate puts the sentinel on both surfaces and the default on neither; the pre-wiring copy the reverse (§7.1) |
| `code_vocabulary_16.txt` | `18176f9f4a580ac9` | 2346 | §7.3's complete-vocabulary proof together with its producer: 16 `code_*` rows, 16 derived map entries, the map an identity on this tree, 15 literal call sites all named, `MISSING-ARM` named without a literal, `STR_CRITERIA` complete. Re-checked at the shipped gate after that gate moved by one edit — `code_vocabulary_16_recheck.txt` |
| `code_vocabulary_16_recheck.txt` | `8268b91bbb3a1f80` | 1176 | the re-check: its producer reads the gate source at run time, so it was re-run against the shipped gate rather than assumed still valid; output identical to the recorded block (§6.7's rule applied to itself) |
| `instruments.txt` | `75f5345806a0f898` | 9834 | the verbatim producers of the four files above, plus leg 1's exact command |

`instruments.txt` exists because an output is only re-checkable if its producer is on the record too. The
producers were scratch files under `/tmp` during the run; the file reproduces each one verbatim and each
embedded copy was verified byte-identical to the file that actually ran (all three: `verbatim=True`). It
also records leg 1's exact invocation and the arm's plan census. A reader wanting to re-derive any output
above needs this repo and that file, not this report's prose.

Two of the instruments were corrected after their first run — they passed the literal `5` to the gate's
`finish()`, which is not this gate's REFUSE code — and their outputs were re-verified byte-identical
afterwards (`diff` empty), so the correction is cosmetic. It is recorded because the alternative, shipping
an instrument that states a wrong exit code, is the same defect §3 had to fix.

**Added when the legs were run** (all sha256, first 16 hex):

| file | sha256 | bytes | what it establishes |
| --- | --- | --- | --- |
| `b2_only_after.txt` | `0ab72f16e63e83bb` | 434 | the same control re-run after the blocker-2 fix: `criteria_drop_criterion` → `exit=4 expected=4 code=PRB-REFUSE-CRITERIA HIT`, `ok=1 bad=0` (§3.1) |
| `negcontrol_full_after.txt` | `5068107065db675b` | 1745 | the full suite after all seven fixes: `ok=15 bad=0 controls=15`, `verdict=ALL-HIT` — the line the blocked suite never reached (§6.2) |
| `improvement_distribution.txt` | `da6d8a7cbaf6bb40` | 2776 | the 56-cell improvement distribution (min 4.70, median 7.485) behind the 4.0 re-pin, cross-checked against the gate's own `improvement_min` and its three named failures, plus an addendum recording that regenerating it changes exactly one line (§2) |
| `leg4_requirefull.txt` | `698eb52104a2fa0c` | 4971 | leg 4: `check_registry_complete.py --require-full` run as CI runs it — exit 1, the standing 12-parameter gap (8 Root-A non-scalar + 4 selector-toggle), reported and not re-adjudicated (§6.8) |
| `leg4_s3_acceptance.txt` | `0e5743874074e2f8` | 8410 | leg 4: S3's existing 72-cell gate, CTest entry `#72`, re-run as registered at this tree — `verdict=PASS refusals=0 failures=0`, `ok=22 bad=0 controls=22`, 236.28 s (§6.8) |
| `acceptance_orientation_blocker.txt` | `0652ea027b82e314` | 9347 | the three blockers: the pin above the pristine arm's minimum, the gate's crash on a dropped criterion, and the sandbox's un-rolled-back text cache (§6.4) |
| `wiring_two_direction.txt` | `e311bb9676883c6f` | 5088 | the ④ wiring's two-direction acceptance, **rev 1** — run against a pair that has since drifted apart, and the evidence §6.7's defect is visible in. Kept, not replaced |
| `wiring_two_direction_rev2.txt` | `8be1a5e4a3443171` | 15121 | the ④ wiring's two-direction acceptance, **rev 2**, at the current gate and criteria: the builds, the isolation assertion (`wiring_only=True`), the direct identity transcript, both liveness transcripts, the second-form suite identity, and the producer verbatim (§6.2, §6.7) |
| `wiring_identity_pinned_rev2.txt` | `df292f312b20461d` | 2501 | the rev-2 identity transcript alone: 32 lines, `PRB GATE verdict=PASS refusals=0 failures=0`, shared byte-for-byte by both builds (§6.2) |
| `wiring_identity_pinned.txt` | `f88a49d9682c07fa` | 3000 | the **rev-1** identity transcript: what the two builds shared at 17:18, before the gate moved. Superseded by `wiring_identity_pinned_rev2.txt` (§6.7) |
| `wiring_liveness_shipped.txt` | `5e92e916f7e49c9d` | 441 | the shipped gate: sentinel `HIT` on both surfaces, `ALL-HIT` |
| `wiring_liveness_prewiring.txt` | `3b2987c15918142a` | 801 | the pre-wiring build: sentinel `MISS`, its own literal fired — the negative control |
| `negcontrol_suite_abort.txt` | `fd57b40ee28b9a43` | 2625 | the **blocked** suite: `pristine` PASS, four `MISS (CRASHED)` on blocker 2, then dying preparing control 5 on blocker 3 (§6.4) |
| `gate_crash_dropped_criterion.txt` | `76115f9a3e4c906e` | 2634 | the `KeyError` traceback and the truncated transcript, captured with colour off |
| `analyzer_ab.txt` | `9d16c2b27a70a7de` | 2013 | §8's byte-identity A/B at both flag sets, with the costs |
| `pipeline_leg_full.txt` | `ecf17bda4796230c` | 6026 | the pipeline leg's complete log, unedited — the **blocked** run, kept as the record of blocker 1 |
| `leg3_mutant_full.txt` | `dead2e3fb9ffc885` | 12948 | leg 3's complete log, unedited, including the failure that stops it — the **blocked** run, and the hand-invocation that §6.5 defect 6 is about |
| `leg1_leg2_registered_entry_green.txt` | `15a8b3547ec42e8c` | 10437 | legs 1+2 at the fixed tree, **as the registered CTest entry `#76`**: `product_diff=OK`, `verdict=PASS refusals=0 failures=0`, all 15 controls `ok=15 bad=0 controls=15 verdict=ALL-HIT`, `skipped=(none)`, `improvement_min=4.7000` against the re-pinned 4.0, 1557.65 s (§6.1, §6.2) |
| `leg3_registered_entry_green.txt` | `760c9722dd801a15` | 12971 | leg 3 at the fixed tree, **as the registered CTest entry `#77`**: acceptance orientation `exit=0 verdict=PASS`, mutant orientation `exit=1 verdict=RED refusals=0 fail=PRB-FAIL-IMPROVEMENT=56`, `PRB-MUTANT PASS`; the degen arm's `degen_pristine_exit=0` / `exit=64 premise_bit=True fatal_premise=6 fatal_stimulus=0`, 1554.34 s (§6.3) |

**Added for the Windows build fix** (§5.1). These are new files rather than overwrites of the two
pre-fix transcripts above, because the pre-fix rows are the comparison partner §5.1's digest table cites:

| file | sha256 | bytes | what it establishes |
| --- | --- | --- | --- |
| `stage_gh19_prb_shadow_windowsfix.txt` | `2f04c67be18650c8` | 375 | the stager's output after the fix, verbatim: `tree_sha256` unchanged (`08fde1f6…`), `mutated_sha256=afbc1a1f…`, `dead_body_removed_bytes=982` (§5, §5.1) |
| `leg1_leg2_registered_entry_green_windowsfix.txt` | `917e8e9c9820510c` | 10451 | legs 1+2 re-run at the fixed tree through registered entry `#76`: exit 0, `verdict=PASS refusals=0 failures=0`, `ok=15 bad=0 controls=15 verdict=ALL-HIT`, `skipped=(none)`, and `cand`/`neut` digests identical to the pre-fix row above, 1733.33 s (§5.1) |
| `leg3_registered_entry_green_windowsfix.txt` | `7d973dbd20177f03` | 13056 | leg 3 re-run at the fixed tree through registered entry `#77`: exit 0, `PRB-MUTANT PASS`, mutant orientation `verdict=RED refusals=0 fail=PRB-FAIL-IMPROVEMENT=56`, and `probe_A`/`probe_B` digests identical to the pre-fix row above, 1727.39 s (§5.1) |
| `hand_invocation_repeatability_windowsfix.txt` | `b52c76ec099dcbe5` | 7305 | the hand invocation re-run **alone** — the same argv as the concurrent one, no sibling process: `R1b_RC=0`, `PRB GATE verdict=PASS refusals=0 failures=0`, `NEGCTL-PRB ok=15 bad=0 controls=15 verdict=ALL-HIT`, **0** `SETUP FAILED` lines, and both digests identical to the pre-fix row above (`3efee15b…`, `07a18188…`). This is the discriminator for §6.4's residual limit: the earlier exit 1 does not survive removing the sibling process (§6.4) |

Correction, kept visible rather than quietly fixed. Every row of these tables was reconciled mechanically
against the files it names — sha256 and byte count read from the files, compared to the cells — and that
surfaced two defects in **this report's own tables**. First, the two rows for
`leg1_leg2_registered_entry_green.txt` and `leg3_registered_entry_green.txt` carried **line counts** (100
and 108) in the column headed *bytes*; the cells now carry the byte counts, and the other 25 evidence rows
reconciled unchanged. Second, §11's row for `tools/stage_gh19_prb_shadow.py` carried the sha and line
count of that file **before** §5.1's fix (`9e17c5bc…`, 167 lines); it now carries the shipped file's
(`f838c8cd…`, 304 lines), the other eight rows of §11 reconciling unchanged. The reconciliation is stated
because a table of shas is exactly the kind of artifact a reader trusts instead of checking, which is the
property §6.7 was about.
**Re-run after the last evidence row was added: 38 rows checked, 0 mismatched, 0 skipped** — 31 of them
§12's evidence rows (name, sha256 prefix and byte count all read from the file, including the new
`hand_invocation_repeatability_windowsfix.txt`) and 7 of §11's, the remaining two §11 rows being
excluded because their third column is a diffstat (`+21/-3`) or a dash rather than a count. Two further
facts the same run settles: every §12 row's third column is genuinely **bytes**, not lines, in a table
whose predecessor had the two confused; and the whole check is mechanical — it reads the files, not
this report.

The blocked-run transcripts are kept rather than replaced: they are the evidence for §6.4 and §6.5, and a
reader should be able to see the defect and the fix rather than take the fix on trust. For the same reason
the green entries are new files rather than overwrites of the blocked ones — each pair is the before and
the after of one defect, and a review that only sees the after cannot tell a fixed defect from one that
was never there.

The two green entries have **different shas from each other's arms by construction**, and §6.3 records why
rather than leaving it to be noticed: the analyzer writes its `--label` into a report's first and last
lines, and the two runners label the same corrected arm differently.

The transcripts are kept whole on purpose: the digest and the one-line description beside each are my
summary, and a reviewer should be able to check that summary against the run rather than against another
summary.

