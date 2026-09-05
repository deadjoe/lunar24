# spike / — disposable P1 technology slices

This directory holds **use-once-and-discard** P1 verification artifacts. Its job is
to prove (or refute) that an entire class of platform/framework code works **before**
any of it becomes a product extension.

Everything here may be deleted when P1 concludes. Nothing under `core/` or
`generated/` may ever reference `spike/` — the one-directional boundary is enforced
by the `check_spike_clean.py` gate (a hit in `core/` or `generated/` FAILS the build).

## What belongs here

- P1 framework feasibility probes (a real iPlug2 standalone bootstrap, device
  enumeration, host channel-open behaviour, etc.).
- Throwaway experiments and their results, recorded so a reviewer can re-derive
  the verdict without re-running.

## What does NOT belong here

- Any code the shipped product will depend on. That lives in `core/` / `generated/`
  (framework-free) and is never allowed to import from this directory.
- Anything that must survive a "delete `spike/` and ship" step.

## Discipline

- Each probe is one self-contained directory or file; note what it proved and, if
  it proved a negative, exactly what the fallback would be.
- The framework choice is still live. If slice ① fails on macOS, the P1 plan
  already requires falling back to miniaudio + RtMidi + SDL3 at the exit condition
  in `design/06-master-plan.md` — do not let framework risk reach the DSP body.
