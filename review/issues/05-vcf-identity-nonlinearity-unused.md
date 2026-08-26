## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

The persisted unit identity/calibration contract does not affect the VCF path, and the VCF has no input-level nonlinearity.

- `PolivoksFilter::tick_()` is a linear Chamberlin state-variable update (`polivoks_vcf.h:149-160`). It has no input-driven nonlinear stage or per-unit parameterization.
- `DeviceStateV1` persists `identitySeed`, `calibration.vcfLeftTrim`, and `calibration.vcfRightTrim`, but repository-wide consumers outside serialization/tests are absent. `PolivoksFilter` and `SignalPath` accept none of them.
- Left/right delay state is independent, but left/right calibration and nonlinear identity state are not modeled.

The chosen Chamberlin core's measured “resonance does not lose lows” behavior is already documented and is not disputed here. The missing requirements are the separate ones in `design/07-core-contract.md:150-158`: one persistent unit identity, input-level-driven VCF nonlinearity, and independent left/right calibration/nonlinear state through the VCF→distortion→gain path.

## Required correction

Define a versioned mapping from `UnitIdentitySeed`/calibration to the VCF and downstream level-dependent path; consume it in the production runtime; add input-level sweep tests that distinguish linear from nonlinear behavior and left/right identity/calibration tests that fail if either channel ignores its persisted values. Unknown analog constants may remain provisional and clearly labelled.

## Classification

Implementation defect; high severity; P3 identity/fidelity contract not satisfied.
