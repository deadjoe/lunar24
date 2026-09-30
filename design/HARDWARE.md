# The Solar 42N, as the manual describes it

Facts from the official manual (v15) and panel drawing that the engine and panel follow.
Page numbers are manual pages. Anything not listed here is a guess and marked `// tuned by ear`.

## Signal flow (p.6 block scheme)
```
DRONE 1-6 + EXT AUDIO + PREAMP + VCO A/B
  → 10-channel PAN/VOL mixer (L/R)
  → dual 12 dB LP/BP filter (L/R) → distortion (L/R)
  → dual effector (L/R) → WET OUT L/R
VCO A → DRY A, VCO B → DRY B (bypass the whole chain)
```
Mixer order: DRONE 1, 2, 3, EXT.AUDIO, VCO A, VCO B, PREAMP, DRONE 4, 5, 6.

## Modules
- **Classic drones 1/2/4/5**: 5 negistor oscillators each (MOD / TUNE / MUTE per osc), VOLT,
  GATE/HOLD, ATT, RLS, CV.
- **New drones 3/6** ("Papa Srapa" noise synth): LFO (rate, mod, fm/am, divider, PITCH),
  hi/low, NOISE, S&H with clock, GATE/HOLD, ATT/RLS, env out.
- **VCO A/B**: morphing waveform, cv amt, oct+3, low, sub, tune, pwm, pw, 1v/oct, sync.
  Each has an ADSR envelope (hold, A D S R) driving its VCA.
- **Dual VCF** (p.21): Polivoks-style double 12 dB filter, LP/BP per side, link. Raising
  resonance does not lose low end. CV L is normalled to CV R (CV L drives both sides when
  nothing is plugged into CV R). Distortion comes after the filter: GAIN = amount,
  DIST = dry/wet.
- **Dual effector** (p.22-24): 13 cartridges x 3 programs, X/Y/Z per program, BLEND,
  MASTER VOLUME, cv x/y/z. Each side can run a different program; flipping the 1-2-3
  switch reloads the program (the tail is cut).
- **Modulation**: LFO A/B (wave, rate, x1/x6/x10), joystick X/Y, 5-step sequencer
  (pulser, clock, 3/4/5 stages, 5 steps with gates), preamp + envelope follower.
- **Touch-plate keyboard** (p.13-20): 12 pressure plates, single/twin/split, microtonal
  tuning and quantiser, portamento, vibrato, arpeggiator, 16-step sequencer, clock,
  4 presets (A-D), calibration. DRONE VOICES keys 1-6 gate the drones.
- No MIDI or USB on the hardware; the app adds MIDI as another way to play the keyboard.

## Output ranges (p.4 spec table)
| Output | Range | Output | Range |
|---|---|---|---|
| DRY | max 1 V | WET | max 2 V |
| VCO | -5..+5 V | EG | 0..8 V |
| LFOs | 0..+10 V (unipolar) | Joystick | -10..+10 V |
| S&H | -5..+5 V | "Voice 3, 5 modulator" | 0..+12 V |
| 5-step CV / gate | 0..+5 V / 0..+10 V | Env follower CV / gate | 0..+10 V / 0..+8 V |
| Pulser | -10..+10 V | "Env voices" | -10..+10 V |

Voice numbers in the manual's table are inconsistent (it lists voices 1,2,3,6,7,8); kept
as printed. The engine works in volts, so these relative strengths carry over. The LFOs are unipolar
on purpose: as FM they push pitch one way only.

## Product boundary
- Only what the hardware has: no extra sound sources, modulation matrix, macros, plugin
  formats or multi-patch library. The keyboard's own 4 presets are the only presets; the
  app restores the last machine state on launch (like knobs and cables staying put).
- Cables: any output to any input, one cable per jack; plugging overrides a normalled
  connection, unplugging restores it.
- Four logical outputs: WET L/R + DRY A + DRY B. A stereo device gets WET L/R.
- Tech: C++17, CMake, iPlug2 standalone app (macOS/Windows) with IGraphics UI; the synth
  core (`core/`) never includes framework or platform headers.
