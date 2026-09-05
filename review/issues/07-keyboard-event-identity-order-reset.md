## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

The canonical input pipeline cannot preserve multi-touch note state, and its event order is incompatible with `KeyboardBehaviour`.

- `PerformanceInput`/`ControlEvent` carry no note or touch identity; `note_off` emits only `gate_off` (`input_state_machine.h:39-62,111-127`). Consequently `ArpSeq` explicitly uses a provisional LIFO stack (`arp_sequencer.h:40-50,233-280`): releasing a non-last plate removes the wrong held note and cannot restore the correct remaining pitch.
- A note-on emits pitch/pressure/gate at one timestamp, but the canonical phase order delivers pitch/pressure before gate-on (`control_event.h:59-75`). `KeyboardBehaviour` sets `nextPitchIsNewNote_` only on gate-on and consumes it on a later pitch (`keyboard_behaviour.h:417-440`). A first legato note at 1 V therefore renders at 0 V after the canonical sort; the flag leaks into the next note.
- `KeyboardBehaviour::tick()` reads `portamento_.current()` instead of advancing `portamento_.tick()` (`keyboard_behaviour.h:449-456`), so a configured glide never moves. A 0→2 V glide remains 0 V after 48,000 ticks.
- `reset` is ignored (`keyboard_behaviour.h:417-445`), so the documented all-gates-off failsafe leaves `gate()==true`. A partial note release also disables pressure/vibrato unconditionally even while `activeNotes_` remains nonzero (`:426-430`).
- `translate()` emits a partial note-on when output capacity is 1 or 2 (`input_state_machine.h:95-121`), allowing a gate-only or gate+pitch admission instead of an atomic note transaction.

The P4 tests do not connect `InputStateMachine → EventTimebase phase ordering → ArpSeq/KeyboardBehaviour`; they manually deliver gate-on before pitch and test `PortamentoGlide` separately. P4 is nevertheless marked MET (`design/00-status.md:76-83`).

## Required correction

Carry stable note/touch identity through note-on/off and held-note state; define an atomic note transaction compatible with the fixed same-sample phase order; make reset clear all note/gate/modulation state; and advance portamento per sample. Add end-to-end tests for first note, overlapping notes released out of order, partial release, reset, glide progression, arp/seq held-note identity, and insufficient output capacity.

## Classification

Implementation defect; high severity; P4 exit gate regression.
