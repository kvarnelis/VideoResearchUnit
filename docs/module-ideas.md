# Candidate modules

Drafted 2026-11-09 as an ideas list, not a plan. Since then Colorizer, Ramps
and Feedback have been built and Slit Scan and Tape Loop planned; nothing else
in it is committed to. It was made from the code as of commit fa0fd91 and a
survey of video-art and television techniques; sources are at the end.

## What is missing

VRU already has picture sources, two scan processors, colour tools and a
monitor. What it lacks is memory (no module can look at an earlier frame
except Freeze Frame), false colour (no module maps brightness to arbitrary
colours), and the two bridges only a Rack video synth can offer: Rack signals
drawn straight onto the raster, and the picture read back as control voltage.

The ten modules below cover that. Three of them (Feedback, Slit Scan, Tape
Loop) share one piece of plumbing, a frame ring, so they should be planned
together.

## How the list was made

A candidate had to pass five tests:

1. It looks striking on the 4:3 CRT display.
2. It is a technique with a lineage in video art or television engineering,
   emulated as a technique, not a copy of a hardware product line.
3. It exploits Rack being a modulation environment: CV in, CV out, or
   audio-rate signals sampled at scan time.
4. It fits the architecture as it is: a CPU per-pixel node in the video graph,
   the Freeze Frame store pattern for state, the signal ring, or the beam
   layer. Shader rendering is a separate roadmap item and nothing here waits
   for it.
5. It is not already covered. Ruled out for that reason: chroma key (Keyer),
   strobe and hold (Freeze Frame), Lissajous and scope traces (Scope Trace),
   audio-rate raster bends (Deflection Yoke), luma relief (Scan Processor, Line
   Field), posterize and solarize (Color Lab), level quantizing and mirroring
   (Wave Shaper).

What Rack already has: a check of the VCV library found only scopes (XY scopes,
Fundamental's Scope and Viz, oscilloscope-music utilities) and no frame-based video module. The check covered the first page of the
"Visual" tag only, so treat it as a weak negative. Nothing below duplicates
those.

Size is a rough build estimate: S is a day or two, M is about a week, L is
more than that.

## The shortlist

### 1. Feedback

What it does. A processor whose output is blended back into its own input one
frame later, after a transform: zoom, rotate, offset, flip, hue rotation, gain
and decay, all under CV. Zoom in and the picture tunnels; rotate and it
spirals; decay below one and it fades into trails. A luma key on the returned
frame, so only the bright parts recur, gives the steerable repeats of modern
feedback apps. Add a Wave Shaper in the loop and it stays alive on its own.

Lineage. A camera pointed at its own monitor, the first video-synth technique
there was, used in television titles from the early 1960s and by video artists
through the late 1960s and 1970s. Every later video synth has a feedback path.

Why. It is the technique people expect from a video synth, it is already on the
roadmap, and it turns every other module into an animator.

Needs. A one-frame store at render size, written after the node renders (the
Freeze Frame store pattern), plus an inverse-mapped bilinear sampler with
clamp, wrap and black edge modes (shared with DVE below). The renderer's cycle
rule, which returns the last good frame, is not a substitute: it is one frame
late with no transform and only works through a Display.

Size. M.

### 2. Slit Scan

What it does. Each scanline, or each column, or each brightness band, is read
from a different frame in a short history. A CV ramp sets the depth and
direction of the time offset. In "by luma" mode the picture's own brightness
is the delay map, so bright areas lag dark ones. Motion smears into sculpture;
a still camera subject turns into a flowing surface.

Lineage. The slit-scan rigs of 1960s film titles and effects, and the
time-displacement films of the 1980s, which do it live from gradient maps.

Why. It is one of the most beautiful things a frame memory can do, it is rare
in live tools, and the Video Player and Syphon In already supply moving
pictures to feed it.

Needs. The frame ring (below). Read-only once the ring exists.

Size. M, or S once the ring exists.

### 3. Colorizer

What it does. False colour. Brightness is split into bands, two to eight, and
each band is given its own colour from CV-controllable hue, saturation and
level; or, in gradient mode, brightness runs through three or four colour
stops. A grayscale ramp becomes stripes of arbitrary colour; a face becomes a
poster.

Lineage. The colorizers of the analog video synthesizers of the late 1960s and
1970s. The signature look of early video art.

Why. Color Lab shifts and posterizes the colours a picture already has; the
Keyer paints one colour. Nothing in VRU assigns arbitrary colours to
brightness, and that is the step that makes a video synth look like one.

Needs. Nothing new: a per-pixel lookup from luma.

Size. S.

### 4. Composite In, with Scan Sync

What it does. Composite In is a generator with three audio-rate inputs that
become red, green and blue (or one brightness input ahead of a Colorizer) at
each pixel's scan time, as if Rack's oscillators were wired straight to the
monitor's guns. A signal locked to the field rate draws stationary horizontal
bands; a slow oscillator sweeps colour fields; a VCO tuned to the line rate or
its low multiples draws soft vertical bars; anything in between draws
diagonals and moire. Scan Sync is the small utility that makes locking
possible: a frame pulse, a field ramp, divided clocks, a line-rate pulse for
hard sync, and a pitch voltage so a Rack VCO tunes to the line rate exactly.

Lineage. The analog video synthesizers of about 1970, which built pictures
entirely from sync-locked oscillators with no camera, feeding the colorizer
directly. Every analog video synth locks its oscillators to horizontal and vertical
sync.

Why. This is the most Rack-native idea on the list: it makes every oscillator,
sequencer and envelope in Rack a video source. The signal ring already samples
Rack signals at scan time for the Deflection Yoke, so the hard part is done.

Needs. The signal ring (exists). Caveat: at 48 kHz a scanline is three
samples, so horizontal detail is coarse and vertical bars are soft; 96 or 192
kHz helps, and the module should say so on its panel or in its manual.

Size. S for Composite In, S for Scan Sync.

### 5. Light Meter

What it does. The reverse bridge: a module that reads a bus and emits CV.
Average brightness, red, green and blue means, a probe at a CV-positioned
point or region, a motion amount from frame difference, the brightness
centroid as X and Y, and a gate when brightness crosses a threshold or the
scene cuts. The picture then plays the sound patch: a Video Player clip drives
a filter, a feedback loop's brightness opens a VCA.

Lineage. Video as control voltage runs through the Scan Processor and early
machine vision works of the 1970s; the photocell on the
screen is the oldest trick in the book. None of the Eurorack video makers
checked offers a picture-to-CV module.

Why. Rack is a modulation environment. Today the modulation flows one way, from
sound to picture. This closes the loop, and no hardware video synth does it as
naturally as a Rack plugin can.

Needs. The module renders its own small resolution of the graph each frame on
the UI side (as Syphon Out does) and publishes the numbers through atomics;
process() only smooths them. A frame of latency is fine. Motion amount wants
one previous frame.

Size. S to M.

### 6. Hold

What it does. The television's own failures as an instrument. Vertical roll
with the blanking bar visible, horizontal tear and skew from a CV-set row
downward, per-line jitter, field offset flicker, weak-signal noise, all with
rate and depth under CV and a trigger for a single tear.

Lineage. The horizontal-hold and vertical-hold knobs on every set until the
1980s; manipulated televisions of the 1960s and the horizontal
drift works of the early 1970s, where a sync error became the material.

Why. It is cheap, it is instantly legible on the CRT display, and it is the
glitch vocabulary everyone reaches for in live video.

Needs. Per-row offsets and a little noise. Nothing new.

Size. S.

### 7. Tape Loop

What it does. A video sampler. The module keeps the last several seconds of its
input in the frame ring. A CV position scrubs it, speed runs from reverse to
double, loop points can be set, a trigger stutters, and a hold freezes. It is
Freeze Frame with a memory, or a videotape you can play with a sequencer.

Lineage. The first frame buffers of the late 1970s, which let a held frame be
worked on; the videotape loop.

Why. Sequencers and clocks are Rack's strength, and nothing in VRU lets
them cut up time yet. With the Video Player it makes a remix instrument.

Needs. The frame ring.

Size. M.

### 8. DVE and Displace

What it does. Two front ends on one warp engine. DVE is the clean geometric
transform the Deflection Yoke deliberately is not: position, scale, rotation,
aspect, flip, perspective tilt, plus tile, mirror and kaleidoscope folds, with
clamp, wrap and black edges, all under CV. Displace takes a second bus as a
map: bus B's brightness (or hue) pushes bus A's pixels in X and Y by CV
amounts. Ramps as the map give smooth warps; a Video Oscillator as the map gives
liquid; the picture as its own map gives relief.

Lineage. Analog deflection transforms of the late 1960s; the digital video
effects of 1980s television, the "flying picture"; and the modulate functions
of live-coding video, which are the displacement idea in another form. Displace is the two-dimensional
generalisation of what the Scan Processor and the Deflection Yoke do along one
axis.

Why. The DVE is the module Feedback needs anyway, so it comes almost free;
Displace is cheap and is the single most alive-looking processor a two-input
video graph can have. A kaleidoscope fold on a Scan Processor relief is worth
the module on its own.

Needs. The inverse-mapped sampler shared with Feedback; a two-input node, as
Bus Mixer already is.

Size. S to M for each front end.

### 9. Ramps

What it does. The analog synth's basic building block: horizontal, vertical,
radial and angular ramps, each with phase, scale and a shaper (triangle fold,
sine, soft step), and two layers combined by sum or difference. Fed into the
Wave Shaper and then the Colorizer, this is the classic ramps-to-shapes-to-
colour signal path, with CV on every stage. It is also the natural map source
for Displace and the wipe source for the Bus Mixer's matte input.

Lineage. The function generators of early analog video synthesizers, the ramp inputs of the Scan Processor; the first thing every hardware
video synth offers.

Why. The Video Oscillator's oscillator mode bakes one recipe (a rotated sine carrier,
a folded triangle and rings, mixed by SHAPE) into a single pattern, and its
grayscale ramp is a test signal. Ramps unbundles that recipe into patchable
parts. With Ramps, Wave Shaper and Colorizer, VRU has the full analog
signal path.

Needs. Nothing new.

Size. S.

### 10. Outline

What it does. A differentiator and comparator with a small
convolution set: edge magnitude as white lines on black with CV width and
gain, hard slicing at a CV threshold, blur, sharpen, emboss. Into the Keyer it
gives moving outlines; into Feedback it gives etched trails.

Lineage. The differentiator and comparator of early analog image processors
(early 1970s), and outlined images made on the Scan Processor.

Why. Edge images are the other half of the early-video look, and nothing in
VRU touches neighbouring pixels yet.

Needs. 3 x 3 and separable kernels on the CPU path. Fine at preview sizes.

Size. S.

## Bench

Good ideas that did not make the ten.

- Pixel Sort: pixels sorted by brightness along rows or columns within a CV
  threshold window, the streaked look of 2010s glitch art (from about
  2010). Cheap on a line buffer; threshold and
  direction modulate well. The strongest bench entry.
- Compare and Matte Logic: a matte where bus A is brighter than bus B, and
  AND, OR and XOR of two mattes, for the Bus Mixer's matte input. The two-input comparator of early image processors; the logic modules of Eurorack video.
- Pixel Lab: spatial resolution reduction, retro palettes, ordered dither and
  halftone, block mosaic. Lineage: early digital frame buffers. Wave Shaper quantizes levels, not space.
- Tape Wear: VHS chroma bleed, dropouts, head-switching band, tracking wobble,
  ghosting. Popular, cheap, and a natural companion to Hold.
- Convergence: independent red, green and blue offsets, the misconverged CRT.
  Very cheap and very TV.
- Wipe: a matte generator for the Bus Mixer's matte input: horizontal,
  vertical, iris, clock, diagonal and box wipes with a soft edge and CV
  position. Lineage: the broadcast production switcher. Ramps covers part of
  it.
- Scanline Osc: the picture as sound. A CV picks a scanline and it loops as an
  audio wavetable at a pitch, or the whole frame plays as a raw signal. The
  mirror image of Composite In.
- Vector Pen: parametric shapes and rotating wireframes drawn on the beam
  layer with audio-rate X, Y and Z. Scope Trace already covers Lissajous.
- Lab Scopes: a waveform monitor and vectorscope on the beam layer. On brand
  for a lab, useful when tuning the Colorizer, not a creative module.
- Textures: analog snow with density and tuning, plasma, cellular automata,
  reaction-diffusion. Evolving fields under CV.
- Character Generator: 1980s titles with CV scroll and reveal.
- Datamosh: one clip's motion transplanted onto another. Striking, but it needs motion estimation. L.

## Shared plumbing

- Frame ring. A per-module store of N frames at render size, written once per
  pass by the first renderer (as the Freeze Frame store does), read by index.
  Feedback needs one frame, Slit Scan and Tape Loop need a few seconds, the
  Light Meter's motion output needs one. At the default Display size a
  32-frame ring is tens of megabytes; make N a menu choice.
- Inverse-mapped sampler. Bilinear lookup through an inverse transform with
  clamp, wrap and black edges. Feedback, DVE and Displace.
- Bus readback to CV. Render a small graph on the UI side, publish numbers
  through atomics. Light Meter, later Scanline Osc.
- The signal ring and the beam layer exist already and carry Composite In,
  Scan Sync and Vector Pen.

## Suggested order

Colorizer and Ramps first: they are small and they complete the analog signal
path. Then Composite In and Scan Sync, because they are small and unique to
Rack. Then the frame ring with Feedback and Slit Scan together. Then Light
Meter, Hold, DVE and Displace, Tape Loop and Outline in whatever order the
patches ask for.

## Rotation, shapes and mixing

On 2026-11-11 the author asked three things: whether anything here rotates a
picture all the way round, whether we could have a shape that can be placed
and sized, and whether we could have a video mixer. Each time the request was
that it not be a copy.

The short answers. Nothing here turns a whole picture all the way round.
Nothing draws a shape that can be placed and sized. Nothing mixes more than
two pictures. The three entries below fill those gaps from television's own
history.

The names are plain working titles, his to replace, and the catalogue
numbers assume this build order.

What exists today:

- Rotation. The Deflection Yoke's S coil turns the whole picture about its
  centre, up to about 29 degrees either way. Feedback rotates only the copy
  inside its loop. Scope Trace and Line Field turn only their own drawings.
  The Scan Processor has a 90 degree switch.
- Shapes. The Video Oscillator has one baked pattern recipe and Ramps has
  radial and angular ramps. Neither can place a shape, size it or change its
  outline.
- Mixing. The Bus Mixer blends two buses with an optional matte. The Keyer
  cuts one key. The Display picks one of three inputs.

### VRU–20 Rotator

This is the DVE front end of shortlist item 8, made specific.

What it does. Turns, zooms and slides any picture.

- ANGLE sets where the picture sits. SPIN sets how fast it keeps turning, in
  either direction, down to stopped.
- ZOOM runs from a postage stamp to a close-up. X and Y slide the picture.
  PIVOT X and PIVOT Y move the point it turns about.
- EDGE picks what shows past the picture's border: black, the edge held, the
  picture repeated, or the picture mirrored.
- FOLDS mirrors the picture into 2 to 12 wedges about the pivot, the
  kaleidoscope of item 8. At 1 it is off.
- NOTCH is a trigger input. Each trigger clicks the picture round by one
  STEP (1/16, 1/12, 1/8, 1/6 or 1/4 of a turn), taking GLIDE to get there, so
  a clock turns it like an indexing table. RESET returns spin and notches to
  zero.
- The ANGLE input is scaled so that 10 V is one full turn. A 0 to 10 V
  sawtooth spins the picture with no seam.

A Rotator after Ramps gives a ramp at any angle, which is also a wipe at any
angle for the Video Mixer below.

Lineage. The rostrum camera and the animation stand's rotating table; the
digital video effects units of the 1980s, which offer rotation about X, Y and Z.

Needs. The inverse-mapped sampler Feedback already uses. One input bus. The
spin angle is accumulated on the audio thread, so its rate is exact.

Later, if wanted. Perspective tilt about X and Y, the "flying picture".

Size. S.

### VRU–21 Shapes

What it does. A source that draws one closed shape and lets voltages move it.

- SIDES runs from 3 to 12 without steps, by blending each polygon into the
  next. ROUND softens the corners until the shape is a circle. POINT pulls
  the middle of each side inward until the polygon is a star.
- SIZE, X, Y, ASPECT and TURN place it.
- REPEAT tiles it across the screen in a grid.
- COPIES puts up to eight of it in a ring, and MELT lets neighbours fuse like
  drops.

The output is not a silhouette. It is a grey picture in which brightness is
the distance from the shape's edge: mid grey on the edge, brighter inside,
darker outside, with SPREAD setting how fast the grey falls away. That one
picture gives a hard matte through the Keyer, a soft matte with the Keyer's
softness, an outline or concentric rings through the Wave Shaper's fold, and
a halo through the Colorizer. A MATTE switch gives the plain white on black
shape for when that is all that is wanted.

Shapes needs nothing patched in. It is set by number of sides, roundness and
star points. It turns, repeats, and melts copies together. It puts out a
distance picture, not a silhouette, as a BUS like every other source here.

Lineage. The pattern generator of a broadcast production switcher, with its
circle, box, diamond and star wipes and its positioner; the iris of early
film; signed distance fields from computer graphics. It absorbs the iris and
box wipes of the Wipe entry on the bench.

Needs. Nothing new. A source like Ramps.

Size. S to M.

### VRU–22 Video Mixer

What it does. The place where pictures meet, take their colors and are laid
over one another.

Revised 2026-11-11. After the first pitch the author asked for a colorizer built
into the mixer, and perhaps a keyer. Both are in below. The request was for the two
functions. How each works here is a proposal, open to change.

Mixing.

- Four picture inputs, A to D. One is on air and one is next.
- FADER, a knob with CV, moves from the picture on air to the next one. When
  it reaches the far end the two swap, as on a studio T-bar, so a triangle
  LFO goes back and forth between them.
- CUT swaps them at once on a trigger. AUTO runs the fader by itself over
  TIME.
- NEXT picks the next input by button, by trigger (step to the following
  input) or by voltage. With ADVANCE on, every finished transition steps
  NEXT, so one clock walks through all four pictures.
- MODE picks the transition: DISSOLVE, ADD or WIPE. A dissolve is a
  crossfade. In ADD the next picture fades up over the one on air, which
  then fades out, so halfway both are at full strength and their colors add
  where they overlap.
- WIPE sweeps a threshold through the greys of the picture in PATTERN, so
  any picture is a wipe. Ramps gives the straight and diagonal wipes, Shapes
  gives the iris and the star, a Video Oscillator gives blinds, and a camera
  or Feedback picture gives organic ones. SOFT sets the wipe's edge.
- With nothing in PATTERN, a wipe uses the next picture's own greys. The
  next picture then arrives brightest parts first, keyed in over the old
  one.
- BLACK, a knob with CV, fades everything to black.
- Four TALLY outputs each put out a gate while their input is on air, so
  sound can follow the cut.

Color.

- Each input has a HUE knob and a COLOR button. With COLOR on, the input is
  read as a grey picture and its brightness takes that hue: black stays
  black and white becomes the full color. With COLOR off, the picture passes
  in its own colors.
- The four hues start a quarter turn apart round the color wheel.
- SAT, with CV, sets the strength of all the colors at once. At zero every
  colored input is plain grey.
- SHIFT is one CV input that moves all four hues round the wheel together.
  10 V is once round, as on the Rotator's ANGLE.

Why the color belongs inside. Ramps and Shapes draw in grey, and so does
every matte. Colored at the mixer, four grey pictures make one picture in
four colors with no Colorizer in front of each, and the hues are set against
each other in one place.

Key.

- A KEY input lays a fifth picture over the mix, and the mixer cuts the hole
  for it. KEY THRESH, with CV, sets the brightness at which the key opens,
  and KEY SOFT sets how gradual its edge is. They are the same THRESH and
  SOFT as on the Keyer.
- With nothing in MATTE, the KEY picture's own brightness cuts the hole. A
  picture in MATTE cuts it instead, so one picture gives the shape and
  another fills it.
- FILL picks what shows in the hole: the KEY picture itself, or a flat color
  from the key's own HUE knob.
- INV turns the matte inside out. KEY ON, a button with a gate input,
  switches the layer on and off at the next frame.
- The key comes after the transition and before BLACK, where a studio mixer
  has its downstream keyer. Cuts and wipes go on underneath it.

What stays in the other modules. The Colorizer keeps the four-stop gradient
and the bands: patch one before an input for false color and leave that
input's COLOR off. The Keyer still cuts a key anywhere in a chain and puts
its matte out as a picture. The Bus Mixer keeps Multiply, Screen, Difference
and Max. The Video Mixer replaces none of them.

The plan is the television studio's: a picture on air and one waiting that swap, a lever between
them, a keyer after the mix, and a fade to black last of all.

Four is also the input count of small video mixers. If the shared count
bothers him, three or six would do as well.

Lineage. A machine and a habit of editing software.

The studio vision mixer, also called the production switcher. Wikipedia's
article gives the plan used here: a program bus and a preset bus that can
exchange their sources, the transition lever or T-bar, dissolves and pattern
wipes, a key bus with a fill and a source that cuts it, a downstream keyer
after the mix, and fade to black as the last stage. Tally lights are the
studio's too.

The color plan of the early multi-camera video synthesizers: each black and
white camera is given a color role, one as red, others as green or blue, and
all of them go through a matrix into one color encoder. Cameras aimed at
nearly the same subject give color pictures that overlap. That is the idea of
the Color section and of ADD: one color to each grey picture, then the
pictures laid together. Here the color role is a hue knob. VRU–01 Deflection
Yoke is already modelled on early video-art manuals, as docs/architecture.md
records. The mixer would bring that way with color into the series beside it.

The gradient wipe of editing software. The entry absorbs the rest of the
Wipe entry on the bench.

Needs. A node that reads up to seven buses; the Bus Mixer reads three today.
No more than five are wanted at once: on air, next, pattern, key and matte.
The color is one multiply for each colored input. The key and the wipe are
both the Keyer's matte line, with the fader as the wipe's threshold. All of
it fits the mixer's one pass over the frame. Cut and take triggers are read
on the audio thread and applied at the next frame, as a studio mixer cuts in
the vertical interval.

Panel. By a first count: 12 knobs, 12 buttons and switches, and 22 jacks, 46
in all. The Deflection Yoke's code declares about 56 knobs, switches and
jacks for its 20 HP panel, and the Keyer's declares 13 for 10 HP. Between
those two densities this panel is 20 to 36 HP wide, and 24 is a fair first
guess. No panel has been drawn.

Later, if wanted. A SUM mode with a level for each input, all four on screen
at once in their colors, which is that plan outright. A BORDER in
the key's flat color along the wipe's edge. A hue CV for each input.

Size. M to L. It splits into two steps if wanted: the mixing and the color
first, then the key.

### Order

Rotator first: it is the smallest, its sampler exists, and it works on every
picture already in the rack. Shapes second. Video Mixer third, because its
wipes are at their best once Shapes exists.

## Patterns for the Video Oscillator

On 2026-11-11 the author asked whether the Video Oscillator could draw the
kind of patterns that a pair of oscillators makes against the scan.

The short answer. Yes. Two oscillators, one for the horizontal parts of a
picture and one for the vertical, each giving a sine, a triangle and a square,
with sync inputs and a knob that mixes the two: that method is as old as
video synthesis and belongs to no maker. The Video Oscillator does not work
that way today, and it can. Ramps already draws the still half of the
family. The entry below is our own version of the half that moves.

He did not repeat the condition he set for the three modules above, that
nothing be a copy. It is kept here anyway.

Every control name below is a working title, his to replace.

What exists today:

- VRU–05 Video Oscillator. Its PATTERN knob has six positions. Five are test
  cards: three sets of color bars, a grey scale and a grid. The sixth,
  Oscillator, is one fixed recipe: sine stripes on an axis that turns slowly
  by itself, crossfaded by SHAPE into triangle stripes the other way, with
  faint rings from the centre. Nothing in it can be locked, nothing bends
  anything else, and every control is read once a frame.
- VRU–06 Ramps. Two layers, each a ramp across the screen, down it, outward
  from the centre or round it, with FREQ up to 16 cycles, PHASE, a WAVE that
  runs from ramp through triangle and sine to a soft step, and a MIX of A,
  A + B, A - B or A x B. So bars, rows, rings, spokes, plaids and checks
  exist now. They are grey, they stand still unless something is patched
  into PHASE, and they stop at 16 cycles. Ramps into a Colorizer shows this
  half of the family today, with no build.

What neither does:

- Lock and slip. On a monitor, an oscillator in step with the scan draws a
  pattern that stands still. Out of step, its bars lean and travel and its
  rows roll. That motion costs nothing and is most of the life in these
  pictures.
- Bending. One oscillator pushing another turns straight bars into zigzags,
  waves and moire.
- Fine pitch. Ramps stops at 16 cycles. The Oscillator recipe's FREQ knob
  runs to 8, and a voltage takes it to 24.
- Sound inside the frame. The generators read their inputs once a frame, so
  an audio signal patched into one does nothing useful. The Deflection Yoke
  reads its inputs at each pixel's scan time. The generators do not.

### VRU–05 Video Oscillator: two oscillators against the scan

What it does. Adds a pair of oscillators, A and B, that run against the
television scan. The scan is the series' own, the one the Deflection Yoke
uses: 15,734 lines a second and 59.94 fields.

Each oscillator has four controls.

- FREQ, with CV, sets how many cycles it draws, from less than one to the
  finest the picture can hold.
- RANGE, a switch, picks what FREQ counts. On BARS it counts cycles across
  one line, so the oscillator draws upright bars. On ROWS it counts cycles
  down one field, so it draws rows.
- WAVE runs from sine through triangle and ramp to square: soft bars,
  gradients, hard-edged blocks.
- LOCK, a knob with CV, is the hold. Fully up, the oscillator is clamped to
  the scan and its pattern stands still at any FREQ. Fully down it runs
  free: bars lean and travel and rows roll, by an amount set by how far FREQ
  sits from a whole number. In between it behaves like a television with its
  hold knob near the edge of its range. Close to a whole number the pattern
  catches and stands. Further off it tears into slanted bands and slips.

Between the two:

- BEND, with CV, lets B push A's bars sideways, so the shape of B becomes
  the shape of every bar.
  - B a ramp, one row tall: the bars lean, and BEND sets the angle.
  - B a triangle: zigzags and chevrons. B's FREQ sets how many.
  - B a sine: waves.
  - B a square: bands of bars stepped sideways from one another. With
    square bars and BEND at half a bar, that is a checkerboard.
  - B with its LOCK down: the bend rolls down through the bars.
  - B on BARS near A's rate: bars that bunch and spread.
- MIX, a switch as on Ramps: A, A + B, A - B, A x B. Bars with rows give
  weaves, plaids and checks. Sine with sine gives a grid of soft dots, and
  triangle with triangle a grid of diamonds. Two oscillators on the same
  range at close rates give moire.

Color and trail:

- HUE colors A. SPREAD, a new knob, sets how far round the color wheel B's
  color sits from A's. At zero both share one hue. Turned up, a weave comes
  out in two colors and a third where they cross.
- SAT, BRI, CON and FB stay as they are.

Sound:

- Two new inputs, IN A and IN B, push each oscillator's bars sideways. They
  are read at each pixel's scan time, not once a frame. A voice, a drum or
  an audio oscillator patched there draws its waveform down the bars. A tone
  of 440 Hz puts about seven waves down the screen, and they drift, because
  440 is not a whole multiple of the field rate.
- The BEND CV is read the same way.

What stays. The old Oscillator picture and the five test cards keep their
places on PATTERN, so a saved patch looks the same. The pair is a seventh
position, working title BARS AND ROWS. In it the present FREQ and SHAPE
knobs and their jacks become A's FREQ and WAVE.

What stays in the other modules. Ramps keeps the still, grey geometry for
wipes and mattes, and the rings and spokes, which this pair does not draw.
Composite In, entry 4 on the shortlist, keeps its own job: sound as the
picture itself, one signal to each color. IN A and IN B do only part of
that, sound bending a pattern, and they do it without that entry's blur,
because the bars come from the oscillator and only the bend comes from the
sound. Hold, entry 6, acts on any picture. The LOCK knob borrows its idea
for these two oscillators alone.

Dots, diamonds and weaves turn up in the mix when the waves are sine,
triangle or square, as they do from any two such oscillators. Shapes that can
be placed and sized belong to VRU–21 Shapes. Color is in HUE and SPREAD; the
series has a Colorizer, a Keyer and a Color Lab as modules of their own.
Reading sound at scan time is new only among our own generators.

Lineage. The one already given under Composite In and Hold. Analog video
synthesizers lock their oscillators to horizontal and vertical sync, from
the analog video synthesizers of about 1970 on. A free oscillator against a scan is also what a television
shows with its hold knobs turned off: a picture that rolls or tears. LOCK is
those knobs handed to the oscillators.

Needs.

- A drawing routine beside the present one in `src/video_engine.cpp`. Each
  pixel's value is worked out from its place in the scan, so the bars are as
  sharp as the picture at any pitch. The limit noted under Composite In,
  three audio samples to a scanline, touches only IN A, IN B and the BEND
  CV, where it does no harm: a sound signal changes little within one line.
- The signal ring, which exists. The Video Oscillator would publish one as
  the Deflection Yoke does. A ring carries four signals, which fits IN A,
  IN B, the BEND CV and one spare.
- Care with the count of lines. Each drawn row must take its scanline's
  timing whole, or locked bars tilt. And a field is 262.5 lines long, so a
  free oscillator at an odd whole number of bars would jump half a bar on
  every field. A real set does not do that, because its line timing runs
  straight on from field to field. The routine should count the same way.
- Near the finest pitch the picture can hold, the bars should fade to an
  even grey and not break into false patterns.
- LOCK between its ends is the simplest model of a hold circuit: at the
  start of each line, or of each field on ROWS, the oscillator is pulled
  part of the way back into step. Its feel has to be tuned by eye.
- New parameters go at the end of the list so saved patches load.
- A wider panel. See Panel.

Panel. By a first count: 14 knobs, 3 switches, 10 input jacks and the bus
output, 28 in all against 13 today, with the preview screen kept. That is 16
to 20 HP against 12 today. No panel has been drawn. Rack 2.5.2 places each
module of a saved patch with `setModulePosForce`, which moves the others in
its row aside, so a wider Video Oscillator pushes its neighbours along the
row and overlaps nothing. That was read in Rack's source and not tried in
his Rack.

Later, if wanted.

- A sound output for each oscillator. On ROWS an oscillator runs at its row
  count times 59.94 Hz, a bass note at one row and a high one at a hundred,
  so the picture and the tone would be one signal.
- FREQ at one volt to the octave, so a keyboard doubles and halves the bars.
- A SNOW position on PATTERN: television static. Nothing in the series draws
  noise yet.
- A third oscillator.
- The frame pulse and field ramp outputs of Scan Sync, so Rack's own
  oscillators can be tuned to the scan.

Where it falls. It needs none of the three modules above, and none of them
needs it.

Size. M. The pair with LOCK only at its two ends is a smaller first step.
The hold in between, and the scan-time inputs, can follow.

## Naming

Keep to plain television and video-art vocabulary, as the current panels do
(Deflection Yoke, Color Lab, Freeze Frame). Avoid any hardware maker's module
names; AGENTS.md says why.

2026-11-11: the plugin was renamed Video Research Unit (VRU) and its modules numbered
VRU–01 to VRU–19 (table in AGENTS.md). Scan Wiggler is now Scan Processor (VRU–02).

## Sources checked

- https://en.wikipedia.org/wiki/Video_synthesizer
- https://en.wikipedia.org/wiki/Video_feedback
- https://en.wikipedia.org/wiki/Slit-scan_photography
- https://library.vcvrack.com (Rack visual plugins)
- For the Video Mixer's color and key, read 2026-11-11:
  https://en.wikipedia.org/wiki/Vision_mixer (program and preset buses, the
  transition lever, the key bus, fill and source, the downstream keyer, fade
  to black).
- For the Video Oscillator's patterns, read 2026-11-11:
  https://raw.githubusercontent.com/VCVRack/Rack/v2.5.2/src/app/RackWidget.cpp
  (how Rack places the modules of a saved patch).

Not verified: exact dates of the early digital video effects, which rest on
search summaries of the pages listed, not on a full reading.

For the Video Mixer's color and key: the date of the early multi-camera
synthesizers is given inconsistently by the sources read. Tally lights and
cutting in the vertical interval are general studio knowledge; no page was
read for them. The panel width is a guess from control counts; no layout was
drawn.

For the Video Oscillator's patterns: the lean, drift and half-bar figures come
from the scan's own numbers in `src/crt_beam.hpp`, and the dots and diamonds
from arithmetic, not from a running picture. Rack's handling of a wider panel
was read in its source at tag v2.5.2 and not tried. The panel width is a guess
from control counts; no layout was drawn. Ramps and the Colorizer are
installed but had not been run in his Rack when this was written.
