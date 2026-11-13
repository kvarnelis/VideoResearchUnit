# Scope Trace audit

Date: 2026-11-09. Basis: shared checkout on main at 3646126. Line numbers
refer to `src/ScopeTrace.cpp` unless a file is named; "renderer" is
`src/video_graph_renderer.cpp`. Nothing here is implemented yet.

Gist: Scope Trace is a 12 HP display with twelve knobs, six jacks, one CV input,
four thin modes and a Z axis that can only be driven from a cable. The two
changes that pay off most are a beam-speed brightness law (the code promises one
and does not deliver it) and a hands-on Z knob. After that: CV with
attenuverters on rotation, position, scale and hue, a trigger level, sweep and
sync outputs, two more modes, and a 20 HP panel to hold it all. X/Y/Z outputs
belong on a separate vector oscillator, not on the scope.

## 1. What it is today

Panel: 12 HP (60.96 mm), 12 knobs, 6 input jacks, 1 bus output, 1 light
(:205-263). The code and the panel agree.

| Control | Range, default | What it does |
|---|---|---|
| MODE (snap) | 0-3, 0 | XY Lissajous, Oscilloscope, Dual trace, Polar (:85) |
| X SCALE, Y SCALE | 0-2, 0.85 | Axis gain before rotation (renderer :479-480) |
| X POS, Y POS | -1..1, 0 | Axis offset after rotation (renderer :483-484) |
| TIME | 0-1, 0.35 | Window 10^(-3 + 2t) s, so 1-100 ms; 5.01 ms at default (scope_timebase.hpp:9-12) |
| BEAM "thickness" | 0-1, 0.4 | Energy gain 0.6 + 0.8t (renderer :482 at 4f7e0f7). Not a width; see F3 |
| DECAY | 0-1, 0.65 | Phosphor retain 0.5 + 0.48d per Display draw (renderer :379) |
| HUE | 0-1, 0.32 | Trace A hue; trace B is fixed at hue + 0.33 (renderer :464-465) |
| INTENSITY | 0-2, 1.2 | Energy multiplier (renderer :459) |
| ROTATE | -1..1, 0 | Rotation, times pi (renderer :461) |
| BLOOM | 0-2, 0 | Glow gain (renderer :392, crt_beam.cpp:232) |

| Jack | Scaling | Unpatched |
|---|---|---|
| X / A | clamp(V/5, -1.5, 1.5) (:14-16, :128) | Fallback tone, two cycles per window (:122-128, scope_timebase.hpp:61-77) |
| Y / B | same (:129) | same |
| Z | clamp(V/5, 0, 2) (:130) | 1.0 (full) |
| TIME CV | adds 0.1 per volt to TIME, clamped 0-1 (:116) | no effect |
| RESET | Schmitt 0.1/2 V; moves the ring origin and zeroes the fallback phase (:111-114) | no effect |
| TRIG | Schmitt 0.1/2 V; rising edge stamps the newest sample (:135-138) | time modes zero-cross on A, else free-run (renderer :423-447) |

Output: BUS (video bus token voltage, :157). Light: SIG, full when X or Y is
patched (:158-160).

Modes (video_engine.hpp:98-103; renderer :513-533):

| Mode | Window | Mapping |
|---|---|---|
| XY Lissajous | newest W samples | x = A, y = B |
| Oscilloscope | trigger stamp, else zero-cross on A, else free-run | x = time, y = A; B unused |
| Dual trace | as Oscilloscope | A: y = 0.42 A + 0.38; B: y = 0.42 B - 0.38; B hue = hue + 0.33 |
| Polar | newest W samples | angle = A x pi; radius = clamp(0.5 B + 0.5, 0, 1.25) |

W = window seconds x sample rate, capped at 16384 (renderer :420-421); strokes
are decimated to at most 4096 points (renderer :454, :466-467), which starts
above TIME 0.965 at 48 kHz.

Brightness. Each segment between consecutive samples deposits

    total = 0.25 x INTENSITY x zmid x (0.6 + 0.8 BEAM) x length_px     (renderer :490-494)

and `crt::depositPath` spreads `total` evenly along that length
(crt_beam.cpp:133-147). Energy per pixel is therefore constant along the trace.
At defaults with Z unpatched: 0.276 per pixel, one pass from black reaches 36%
(tone curve 1 - exp(-1.6 E)), a pixel crossed every draw settles near 91%.

## 2. Findings

F1. The beam-speed law is documented but not implemented. crt_beam.hpp:9-12 and
docs/architecture.md:273-276 describe fixed-energy strokes so that fast segments
dim and slow ones brighten; renderer :450-451 repeats the promise directly above
`depositScopeTrace`. But `stroke` makes `total` proportional to `len`, which
cancels the law exactly. Consequences: Lissajous figures are flat, dwell points
do not bloom, and a stationary beam (len 0) deposits nothing at all
(crt_beam.cpp:134) where a real CRT would burn a dot.

F2. Z has no hands-on control. There is no bias, attenuator or polarity: 0 V
blanks, 5 V is unity, 10 V doubles, unpatched is full. Real scopes let you set
the beam level and most Tektronix and Sencore units blank on *positive* Z, which
this module cannot do. The only non-CV brightness controls are INTENSITY (global)
and BLOOM (glow).

F3. BEAM is a second intensity knob. It scales energy (0.6 + 0.8t) while the
spot is hard-coded to the two-tap linear splat (`crt::BeamSpot spot(0.f)`,
renderer :456). `BeamSpot` already supports a Gaussian width (crt_beam.hpp:74-84);
Scope Trace never uses it. Two knobs do one job and no knob does width.

F4. CV coverage is one jack. Only TIME has CV, at a fixed 0.1 per volt with no
attenuator. Rotation, position, scale, hue and persistence are knob-only. Those
are exactly the parameters comparable tools put under CV (hue, scale,
afterglow, rotation).

F5. Trigger has no level or slope. Auto zero-cross on A is right; the TRIG jack
is edge-only. XY and Polar windows ignore the trigger, so a figure whose period
does not divide the window shows a seam that crawls around it.

F6. Modes are thin. Oscilloscope ignores B. Dual trace has a fixed split and a
fixed second hue. Polar is XY in polar coordinates (angle from A), not the
circular time sweep that gives radar and clock traces. There is no 3D.

F7. No signal outputs. The module already runs a window-rate phase for the
fallback tone (:79, :122) but exposes neither a sweep ramp nor a sync pulse.

F8. RESET does not clear the screen. It moves the ring origin and zeroes the
fallback phase (:111-114); the phosphor keeps decaying from wherever it was.

F9. Housekeeping. `signalRingAllocate` is unchecked (:83) and slot 0 is ignored
silently (signal_ring.cpp:171-173, :188-192), so an exhausted 64-slot pool gives
a silent module. Stale comments: ":119 no decimation" vs the 4096-point stride;
renderer :414 "S = W or less if the ring is short" vs `min(W, 16384 - W)`
(:431); :215 says labels sit at 42.5 mm, `labelOffset` gives 43.7
(panel_style.hpp:89-91); signal_ring.hpp:22 cites a `DeflectionYokeSignal` the
header does not define; architecture.md:416-417 says 400 Hz, the code gives
399.05 Hz. In `presets/ScopeTrace`, presets 00, 01, 02 and 04 hold BLOOM 0; 03 Vector Bloom holds BLOOM 0.9.

F10. The panel is full. Four knob rows and a five-jack row at 11.4 mm pitch on
12 HP leave no room for any of the above.

## 3. Controlling Z without a cable

Today: INTENSITY and BLOOM, nothing else. Proposed, in order of payoff:

Z1. DWELL knob (0-1). Blends from today's uniform brightness (0) to the physical
1/speed law (1). Spec for `stroke`, with `meanLen` the mean segment length over
the current window (floor 0.5 px) so overall brightness does not change when the
figure speeds up, only its distribution along the trace:

    perPx = 0.25 x INTENSITY x zmid x gain x pow(meanLen / max(len, 0.25), DWELL)
    perPx = min(perPx, 16 x 0.25 x INTENSITY x zmid x gain)   // dwell cap
    total = perPx x max(len, 0.5)                              // a stopped beam still burns a dot

This is the one Z control that cannot come from a cable, because it is derived
from the trace's own motion. It also makes the comment at renderer :450 true.

Z2. Z knob: a bipolar attenuverter with built-in bias, next to the Z jack.

    zmod = clamp(1 - max(a, 0) + a x V/5, 0, 2),  a in [-1, 1], default +1
    unpatched: zmod = 1

At +1 this is exactly today's behaviour (0 V blanks, 5 V unity, 10 V doubles).
At -1 it is a Tektronix Z axis (0 V full, +5 V blanks). At 0 the cable is
ignored. Continuous in between, no polarity switch needed.

Z3. Context menu, no panel space: Z response (linear / gate at 1 V / Z drives
hue instead of brightness); auto-Z source when the jack is empty (off / radius
from centre / |A| / sweep position).

Z4. In the proposed XYZ mode (section 6) the Z jack becomes depth, read bipolar,
and brightness comes from depth fog.

## 4. CV inputs to add

Every new jack gets a small attenuverter directly above it. Keep the plugin's
scaling: bipolar signals V/5, unipolar CVs 0.1 per volt, attenuverter -1..1.

| Jack | Formula | Why |
|---|---|---|
| ROT | ROTATE + a x V/5 | spinning figures; the single most-wanted CV |
| X POS, Y POS | POS + a x V/5 | orbiting and panning; two LFOs give Spirograph motion |
| X SCL, Y SCL | SCALE + a x V/5 | breathing, squash and stretch |
| HUE | HUE + a x V/10, wrapped | colour cycling separate from Z |
| TIME | existing jack, gains an attenuverter | today's fixed 0.1/V cannot be tamed |
| Z | existing jack, gains the Z2 knob | |

Not added: INTENSITY CV (Z already is that, at audio rate), BLOOM and BEAM CV
(low value), DECAY CV (defer; menu or expander later), MODE CV (menu option to
step modes on a trigger, if wanted).

Introduce one shared helper for knob + jack + attenuverter (a small
`cv_param.hpp`) and use it here. Every sibling module open-codes its 0.1/V
today (VideoOsc :66-77, ColorLab :33-41, LineField :55-73, DeflectionYoke
:119-181); they can migrate later.

## 5. Outputs

Add two, both derived from a window-rate ramp run in `process()` (the fallback
phase already does this at two cycles per window):

- SWEEP: -5..+5 V ramp over one window, reset by TRIG or by the zero-cross
  trigger. Runs in every mode. Patched into another module's X it gives a sweep
  locked to the scope; into Deflection Yoke or Scan Processor it gives sync.
- SYNC: 10 V, 1 ms pulse at each ramp wrap or trigger.

Using the same phase in the renderer to place the drawn window (instead of
"newest W samples") also removes the crawling seam in F5.

X/Y/Z outputs: not on this module. A display's outputs are its sweep and its
trigger. Generated shapes belong on a separate Vector Osc (sine/cos pairs with
frequency ratio, phase, harmonic and quadrature outs, plus a Z out), which is
how oscilloscope-music practice is organised and keeps Scope Trace a sink.
A "thru" of the transformed X/Y (after scale, rotate, offset) is cheap but low
value; leave it out unless panel space is free.

## 6. Modes

Keep the four, deepen each, add two:

- XY Lissajous: DWELL and the Z knob do most of the work. When TRIG is patched,
  start the window at the trigger stamp (as Oscilloscope does) so the figure's
  seam stops crawling.
- Oscilloscope: TRIG LEVEL knob (-5..+5 V) feeds both the zero-cross detector on
  A and the TRIG jack's Schmitt; slope (rising/falling) and auto/normal in the
  menu. Draw B as a dim second trace when patched (menu).
- Dual trace: SPREAD in the menu (0 = overlaid, 1 = today's split); B hue offset
  in the menu (default 0.33).
- Polar: unchanged (angle from A, radius from B).
- Radar (new): angle = 2 pi x sweep phase, radius = clamp(0.5 + 0.5 A, 0, 1.25).
  The circular time sweep of PPI radar and clock displays; B unused.
- XYZ (new): point (A, B, Z) with Z read bipolar (V/5); yaw = ROTATE x pi, pitch
  = TILT x pi (new knob with CV); orthographic projection; brightness =
  INTENSITY x (1 - 0.5 x depth) so far edges fade. Rotating a Lissajous pair
  with a third signal on Z gives true wireframes.

Menu extras: graticule overlay (off / dim / bright), RESET also clears phosphor
(default on).

## 7. Knobs and panel

New knobs: DWELL, Z, TRIG LVL, TILT (4) and eight attenuverters (X SCL, Y SCL,
ROT, X POS, Y POS, TIME, HUE, Z). BEAM becomes a true width: `BeamSpot(sigma)`
with sigma = 0.5 + 2.5 x BEAM px, and its energy gain folds into INTENSITY.
Totals: 23 knobs, 12 input jacks, 3 outputs. That is a 20 HP panel (101.6 mm).

Sketch, mm from the top, eight columns at 12.7 mm pitch (four at 25.4 for the
large knobs):

| y | Row |
|---|---|
| 30 | MODE (snap, readout below), X SCALE, Y SCALE, ROTATE (large) |
| 50 | X POS, Y POS, TIME, DWELL, INTENSITY, HUE, DECAY, BEAM (small) |
| 68 | attenuverters: X SCL, Y SCL, ROT, X POS, Y POS, TIME, HUE, Z (trimpots) |
| 80 | CV jacks under each attenuverter |
| 96 | TRIG LVL, TILT, BLOOM (small) then X, Y, TRIG, RESET jacks |
| 112 | SWEEP, SYNC, BUS (plated) and the SIG light |

Alternative if the 12 HP panel must stay: an 8 HP "Scope CV" expander carrying
the attenuverters and CV jacks over `rightExpander`. More to patch, and it
leaves DWELL, Z, TRIG LVL and TILT homeless; the 20 HP panel is the better
trade.

## 8. Compatibility

Append all new ParamId, InputId and OutputId values after the existing ones so
saved patches keep their knob values. Rack does not push neighbours when a
module widens. The widening from 12 to 20 HP shipped in 4fde1ef, already in the
base, and phase 2 does not change the width, so a patch saved at 12 HP overlaps
by 8 HP until moved by hand. Re-save the five presets: 00, 01, 02 and 04 hold
BLOOM 0, and 03 Vector Bloom holds BLOOM 0.9. Update the Scope Trace sections of
`docs/architecture.md` and `README.md`, and fix the stale comments in
F9 in the same change.

Phase 2 (the beam) adds two differences for a patch saved before it (written
2026-11-10, after the review of the phase 2 work):

- DWELL loads at 0 in an old patch. Rack restores only the param ids that the
  saved file contains, and an old file has no id 21, so a plain load would give
  DWELL its knob default of 0.5 and draw slow stretches brighter than the patch
  was made with. `ScopeTrace::paramsFromJson` sets DWELL to 0 first and then lets
  Rack restore the file, so an old patch keeps the even trace it was saved with,
  and a DWELL the file does hold still wins. A new module and Initialize do not
  come through that path and keep 0.5 (DWELL is on by default). The five
  presets store their own DWELL (0.6, 0.35, 0.35, 0.6, 0.5). `tests/bus_test.cpp`
  has no check for the override, because it builds without the Rack SDK and
  cannot construct the module.
- BEAM now means a true Gaussian width, so an old patch draws differently at the
  BEAM it was saved with. BEAM used to scale the energy of a thin two-tap spot by
  `0.6 + 0.8 x BEAM`. It now sets a round spot (sigma `0.5 + 2.5 x BEAM` px at
  480 rows or fewer) and the energy gain is 2.6 at 480 rows or fewer, times
  `max(1, height / 480)` above that (renderer :508; 5.85 at 1080 rows). At 480 rows
  or fewer, per pixel of path the new line carries `2.6 / (0.6 + 0.8 x BEAM)` times
  the old light: 4.3, 2.8 and 1.9 times at BEAM 0, 0.4 and 1. Most of that extra light goes into width, not into the
  peak (`bus_test` measures a horizontal line for the width, and the mean over 16
  sub-pixel positions of the brightest pixel for the peak):

  | Saved BEAM | Width across the line (old, new) | Light per pixel of path | Brightest pixel, new over old: horizontal, vertical, diagonal |
  |---|---|---|---|
  | 0 | 0.35 px, 0.50 px | x4.33 | x3.94, x3.94, x3.47 |
  | 0.4 (default) | 0.35 px, 1.48 px | x2.83 | x1.01, x1.00, x0.81 |
  | 1 | 0.35 px, 2.94 px | x1.86 | x0.34, x0.34, x0.27 |

  The width is the standard deviation across the line at 400 rows, the test raster
  (the same at any height up to 480; the old spot was 0.35 px at every BEAM). A
  taller frame makes the new spot wider in proportion and raises the gain with it.
  At BEAM 0.4, going from 400 to 1080 rows (the `bus_test` size pair) the spread
  grows x2.23 and the energy x2.25, but the brightest pixel only x1.03. In words: at
  BEAM 0 the spot is 1.4 times as wide as the old one (0.50 px against 0.35 px), so
  the extra light lands in the peak and the line is about 3.9 times as bright at its centre (the tone map
  flattens the top of that). At the default BEAM the centre of a horizontal or
  vertical line is where it was, a diagonal is 0.81 of it, and the line is about
  4.2 times wider. At BEAM 1 the line is about 8.4 times wider and its centre is about
  a third as bright, although it carries 1.9 times the light. The total light is
  what adds up where strokes overlap, under a long DECAY and in BLOOM, so a busy
  old patch can read brighter overall at the default BEAM even though no single
  line is brighter at its centre.

## 9. Suggested order of work

1. Panel and plumbing (mechanical): 20 HP layout, new enums, `cv_param.hpp`
   helper, CV + attenuverter maths, TRIG LEVEL wired into both detectors, SWEEP
   and SYNC ramp in `process()`, RESET-clears-phosphor flag through
   `ScopeTraceParams`, presets re-saved, docs updated, `bus_test` extended.
   Status: implemented and merged 2026-11-09, not yet seen in Rack; DWELL and
   TILT panel slots reserved.
2. Beam (DSP): DWELL law in `depositScopeTrace`, Z knob formula, BEAM as
   Gaussian width, allocate check, stale comments fixed.
   Status: implemented 2026-11-09, fixed, re-checked and merged 2026-11-10, not
   yet seen in Rack. Done: DWELL (param 21),
   BEAM as a round Gaussian width, the Scope Trace allocation retry, and the
   stale comments. Left alone: the Z knob formula.
   Fix-up 2026-11-10, after review: old patches load DWELL at 0 (section 8);
   the BEAM wording was corrected; the bench covers BEAM 0, 0.4 and 1 at
   640 x 480, 1440 x 1080 and 1920 x 1080; a recycled signal ring slot kept its
   write count, so a test that ran after another saw a shifted frame grid (the
   Scan Processor DEFLECT Y proof changed with test order), and `SignalRing::reset()`
   now runs when a slot is released; and the cost against the code before this
   phase was measured (`docs/architecture.md`, the bench paragraph). At the
   defaults and 1440 x 1080 the bench build (-O2) has the stroke at about 3.1
   to 3.2 times the old one and the whole frame at 1.6 to 1.7 times. Built with
   the plugin's own flags, XY at that size has the stroke at 4.3 times and the
   whole frame at 1.85 times (6.66 to 12.32 ms). No measured default case is
   over the 16.7 ms frame budget, old or new.
3. Modes: trigger-synced XY/Polar windows from the shared phase, Radar, XYZ with
   TILT and fog, Dual trace spread and hue in the menu, graticule.
4. New module: Vector Osc with X/Y/Z outputs.

Note for implementers: another work session had `src/Display.cpp`,
`docs/architecture.md`, `AGENTS.md` and `tests/bus_test.cpp`
modified in this checkout (plus an untracked `src/display_select.hpp`). Land
after that work or rebase on it; do not touch Display.cpp for Scope Trace.
