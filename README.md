# Video Research Unit

Video Research Unit (VRU) is a video synthesizer you patch in VCV Rack 2. It is one plugin with 27
modules, numbered VRU–01 to VRU–27. Rack's module browser lists them under the brand
"Video Research Unit". Sources make a picture. Processors bend, color and mix it. Display or Syphon
Out shows it. CV from any Rack module can drive the CV inputs, and one module, Video Probe, turns a
picture back into CV.

This is version 2.3.0. It has been tested only on Apple silicon Macs.

## New in 2.3.0

- Two new modules: [Video Probe](#vru26-video-probe) (VRU–26) reads a picture and gives back
  voltages, and [Frame Delay](#vru27-frame-delay) (VRU–27) shows a picture a few frames late, which
  makes it legal to cable a loop.
- A picture can now modulate another module: [FM IN, MOD IN and LIFT IN](#a-picture-as-a-control).
- Feedback has a "Fade trails fully to black" option. See [Feedback](#vru14-feedback).
- Three plugin-wide settings in Display's right-click menu:
  [Keep video running when hidden, Render video on its own thread and Full precision between
  modules](#display-settings).
- The Octoscillator has five presets, and a picture that feeds several modules is drawn once.
- New [demo patches](#demo-patches) and two test patches.
- For developers: version 2 of the [public bus API](#for-developers) and the offline tool
  `vru-render-patch`.

## Install

You need a `.vcvplugin` file. Building from source makes one, as described in
[Building from source](#building-from-source).

1. Quit Rack.
2. Copy `VideoResearchUnit-<version>-mac-arm64.vcvplugin` into `plugins-mac-arm64` in Rack's user folder.
   On a Mac that is `~/Library/Application Support/Rack2`. Help > Open user folder opens it.
3. Start Rack. It unpacks the file. The modules appear in the module browser under
   "Video Research Unit", named VRU–01 to VRU–27.

The plugin's id is `VideoResearchUnit`. It is in the package file name and the plugin folder.

Requirements: VCV Rack 2, an Apple silicon Mac and macOS 12 or later. Windows, Linux and Intel Macs
are untested.

Syphon In, Syphon Out, the Video Player decoder and Display REC use macOS frameworks. In builds
without them they are stand-ins that do nothing, and the Syphon modules report NO SYPHON. Syphon
works inside Rack, both in a loop from Syphon Out to Syphon In and in the system server list. It
has not yet been checked with OBS or Resolume.

## A first patch

Sources feed processors. Processors feed Display or Syphon Out. A small patch looks like this:

```
Video Oscillator BUS   -> Deflection Yoke BUS IN
Deflection Yoke BUS    -> Colorizer BUS IN
Colorizer BUS          -> Display A
Dual LFO Chaos A       -> Video Oscillator FREQ
Dual LFO Chaos B       -> Colorizer CV 1
```

The first three cables carry video. The last two are ordinary CV. They move the oscillator's
frequency and the hue of the first gradient stop.

The [demo patches](#demo-patches) are a faster way in: open one and turn the knobs its notes name.

## How video cables work

A Rack cable is made for sound. It carries one number per audio sample, on at most 16 channels. At
48 kHz that is 768,000 numbers a second. One 1024 x 768 color frame is about 2.4 million numbers,
so a cable cannot carry even one picture a second. Video needs thirty or sixty every second.

So VRU does not send pictures down cables. It sends a bus number.

Every module that makes or changes a picture owns one bus, numbered 1 to 64. Its BUS output
carries that number as a voltage. Bus 3 is 3 V. The module at the other end reads the voltage and
learns which bus to build on.

Inside the plugin sits one shared table with a slot for each bus. Each slot holds a recipe: what
kind of module owns the bus, how its knobs are set right now, and which bus its picture comes
from. Every owner rewrites its recipe on every audio sample, so CV moves the picture smoothly.

Nothing draws the picture until something needs to show it. Display, Syphon Out and Video Probe do
that. Each reads the recipe for its bus, follows the recipes back to the source, and draws the whole
chain in one pass. Here is a small chain:

```
bus 3   Video Oscillator  makes a grid
bus 4   Deflection Yoke   bends bus 3
bus 5   Colorizer         colors bus 4
```

The cable into Display A carries 5 V. Display finds bus 5, 4 and 3, and draws all three. A picture
that feeds several modules is drawn once per pass, not once for each module that reads it.

A few modules also keep pictures in memory. Freeze Frame holds one, Feedback keeps the previous one,
Frame Delay keeps up to 31, and Video Player and Syphon In keep frames that arrive from outside Rack.

### Why VRU is one plugin

The bus table lives in the memory of one plugin. Rack loads every plugin on its own, and each keeps
its own data. There is no supported way for one plugin to read another plugin's table.

A module from another plugin can see the voltage on a VRU video cable. It cannot see the
table, so it cannot find the picture. All it gets is a steady voltage.

That is why every module that makes or changes a picture ships in this one plugin. They share one
table. Split them across two plugins and each plugin would build its own table. A cable between them
would carry a bus number that the other side could not look up.

The cable still does a cable's job. It routes, and Rack saves it with the patch. Developers of other
plugins can still read and write pictures through the [public bus API](#for-developers).

### Telling video jacks from CV jacks

- Video jacks are labeled BUS, BUS IN, BUS A, BUS B and MATTE BUS. Display has inputs A, B and C
  and output THRU. Scan Processor has outputs BUS 1 and BUS 2, and a second video input, LIFT IN.
  Video Mixer has the VIDEO IN inputs A, B, C and D, then PATTERN, KEY and MATTE, and its output BUS.
  Video Oscillator has FM IN, and Ramps, Keyer and Color Lab have MOD IN. Every other jack is CV or
  audio. The Video Mixer's LIVE outputs are gates.
- Black plates mark outputs, video and CV alike. Inputs sit on the bare panel, except Scope Trace's
  X IN and Y IN, which sit in outlined plates.
- Processors, Bus Mixer and Syphon Out have a green SIG light. It is lit while the input bus has a
  picture, so a dark light means the input points at an empty bus. Video Mixer's is lit while at least
  one of A to D has a picture. Scope Trace has one too. It is dim until X IN or Y IN is patched.

### Patching rules

- Video jacks connect VRU to VRU. Other plugins see only a steady voltage.
- Any change to a video cable's voltage changes which bus it points at. An attenuator, a VCA, an
  offset, a slew limiter and a mixer all do that. Half of 3 V is 1.5 V, which points somewhere else.
- One output can feed many inputs. CV jacks are ordinary, so any Rack module can drive them. The
  VRU CV outputs work with any module too: Octoscillator, Vector Osc, Dual LFO Chaos, 1V CV Out, Video
  Probe, and SWEEP and SYNC on Scope Trace.
- A patch can hold 64 modules that own a bus. That is every module except Display, Syphon Out, Video
  Probe, Octoscillator, Vector Osc, Dual LFO Chaos and 1V CV Out. The 65th gets no bus and never
  retries. It still runs and sends 0 V, but its picture cannot be shown. Delete a module to free a bus,
  then delete and re-add the 65th.
- A processor with nothing patched on its input shows bus 1. Display A, Video Probe and Syphon Out do
  the same. Bus 1 is usually the first video module you added, so a forgotten cable can hide behind a
  picture that looks right. Bus Mixer is an exception: an empty BUS B or MATTE BUS means absent. So is
  Video Mixer, for every picture input. Display B and C show NO VIDEO until patched. An empty FM IN,
  MOD IN or LIFT IN means no modulation.
- A chain can run 17 modules, one source and 16 processors. A longer chain, or a cable that loops
  back on itself, holds the last good picture, and Display writes CYCLE over it. To feed a picture
  back on purpose, use Feedback, which keeps its own memory of the last frame and needs no loop cable,
  or put a Frame Delay in the loop. A loop that goes through a Frame Delay is not a cycle, and the
  delay does not count toward the 16 levels.

### A picture as a control

Four modules take a second picture and let its brightness, at each pixel, move one of their controls
at that pixel. Each has one video input and an AMT knob. AMT runs from -1 to 1. Zero is the default,
and a negative amount reverses the effect. With nothing patched, or AMT at zero, the module makes the
same picture as without the input.

| Module | Input | What the picture moves |
| --- | --- | --- |
| Video Oscillator (VRU–05) | FM IN, with FM AMT | The frequency of oscillator A. |
| Ramps (VRU–06) | MOD IN | The phase of layer A, up to one cycle either way at full AMT. |
| Keyer (VRU–12) | MOD IN | THRESH, so the key opens at a different brightness in different places. |
| Color Lab (VRU–09) | MOD IN | HUE, up to half a turn either way at full AMT. |

The Scan Processor's LIFT IN is a related input: a second picture whose brightness lifts the lines,
while BUS IN still sets their brightness and color.

The picture goes through the usual draw, so it counts toward the 16 levels and a loop through it shows
CYCLE. A picture that is the module's own bus, or a bus with no picture, does nothing. The Video
Oscillator's test cards ignore FM IN.

## The modules

Every module carries a catalogue number. The panel header shows the series name, a rule, the number
and the title. Display's header is a single line (series name at the left of the picture, DISPLAY in the
middle, the number at the right), so its picture keeps its full size. Rack's module browser lists the
same names, for example "VRU–01 Deflection Yoke".

The modules are grouped by what they do, in the order a picture travels. Each entry names the controls
and jacks as they are printed on the panel. Modules with a BUS output put a picture on a numbered
video bus.

| Group | Modules |
| --- | --- |
| [Sources](#sources) | VRU–05 Video Oscillator, VRU–06 Ramps, VRU–21 Shapes, VRU–03 Scope Trace, VRU–07 Video Player |
| [Processors](#processors) | VRU–01 Deflection Yoke, VRU–02 Scan Processor, VRU–04 Line Field, VRU–20 Rotator, VRU–24 Focus, VRU–09 Color Lab, VRU–10 Colorizer, VRU–11 Wave Shaper |
| [Mixers and keys](#mixers-and-keys) | VRU–12 Keyer, VRU–13 Bus Mixer, VRU–22 Video Mixer |
| [Time and memory](#time-and-memory) | VRU–14 Feedback, VRU–15 Freeze Frame, VRU–27 Frame Delay |
| [Outputs and bridges](#outputs-and-bridges) | VRU–16 Display, VRU–08 Syphon In, VRU–17 Syphon Out, VRU–26 Video Probe, VRU–19 1V CV Out |
| [Modulation](#modulation) | VRU–18 Dual LFO Chaos, VRU–23 Octoscillator, VRU–25 Vector Osc |

### Sources

#### VRU–05 Video Oscillator

A video oscillator and test-pattern generator. The PATTERN knob picks oscillator, SMPTE-style 75%
bars, EBU-style 75% bars, EBU-style 100% bars, grayscale ramp, grid or Bars and Rows. Each pattern has a
factory preset in the right-click Preset menu.

- Bars and Rows runs two oscillators, A and B, against the television scan. Each can draw bars across
  the picture or rows down it, locked to the scan or free running, with WAVE, BEND (B bends A), MIX and
  SPREAD.
- LOCK is a knob. All the way up the pattern stands still, all the way down it runs free, and in
  between it catches near a whole number of cycles and tears into slanted bands further off.
- IN A and IN B push the bars of each oscillator sideways, and the BEND CV does the same to the bend,
  all read at each point of the scan, so an audio signal draws its waveform down the bars.
- FREQ A, WAVE A, FREQ B, BEND, HUE, FB and both LOCKs take CV.
- FM IN takes a second picture that bends oscillator A. In Bars and Rows this is true frequency
  modulation: bright parts of that picture speed A up and dark parts slow it down, so A's bars bunch
  together where the picture is bright and spread apart where it is dark. FM AMT sets how far (zero is
  none, negative reverses it). In the plain Oscillator pattern the picture shifts the phase of the
  carrier. The test cards ignore it. Demo patch 8, One Oscillator Bends Another, is the starter.

#### VRU–06 Ramps

Two grayscale layers. Each layer runs along a horizontal, vertical, radial or angular axis, with CV
for frequency, phase and wave. The wave blends from ramp through triangle and sine to soft square.
The layers combine as A, A + B, A - B or A x B. MOD IN takes a second picture whose brightness shifts
the phase of layer A at each pixel; AMT sets how far.

#### VRU–21 Shapes

Draws one closed shape and lets voltages move it. It has no bus input.

- SIDES runs from 3 to 12 without steps, by blending each polygon into the next. ROUND softens the
  corners until the shape is a circle. POINT pulls the middle of each side inward until the polygon is
  a star. SIZE, X, Y, ASPECT and TURN place it.
- REPEAT tiles it across the screen in a grid. COPIES puts up to eight of it in a ring, RING sets the
  radius of that ring, and MELT lets neighbours fuse like drops.
- SIDES, ROUND, POINT, SIZE, X, Y, ASPECT, TURN, MELT and SPREAD take CV. REPEAT, COPIES and RING are
  knobs only.
- The output is not a silhouette. It is a grey picture in which brightness is the distance from the
  shape's edge: mid grey on the edge, brighter inside, darker outside, with SPREAD setting how fast
  the grey falls away. That one picture gives a hard matte through the Keyer, a soft matte with the
  Keyer's softness, an outline or concentric rings through the Wave Shaper's fold, and a halo through
  the Colorizer. The MATTE switch gives the plain white on black shape for when that is all that is
  wanted.
- Presets: Soft Hexagon and Melting Ring.

#### VRU–03 Scope Trace

An oscilloscope and Lissajous source, 20 HP wide. Patch audio or CV into X IN and Y IN. It has four
modes: XY Lissajous, Oscilloscope, Dual trace and Polar.

- X and Y scale, X and Y position, rotation, hue, timebase and brightness each have a CV jack with an
  attenuverter. The brightness jack is labeled Z. So do DWELL, INTENS, DECAY, BEAM, BLOOM and the
  trigger level (the last jack, labeled LEVEL, adds volts to the level knob). The six lower pairs are
  each labeled with the control they move.
- ROTATE turns all the way around. 10 V of CV is one full turn, so a rising ramp or saw spins the
  picture, and the knob sets the starting angle.
- SWEEP is a ramp over the scope's own window. SYNC is a pulse at the start of each sweep.
- BEAM sets the width of the beam, a round soft spot. DWELL sets how much brighter the trace is where
  the beam moves slowly. At 0 the line is even. At 1 a stopped beam burns a dot, as on a CRT.
- Presets: XY Lissajous, Audio Scope, Dual Trace, Vector Bloom and Polar Drift.
- Vector Osc is made to feed X IN and Y IN.

#### VRU–07 Video Player

Plays a video file or a YouTube link and puts the picture on its bus and the clip's own sound on the L
and R outputs (±5 V at full scale; a mono clip feeds both, and a clip with no sound track is silent).
Press OPEN FILE, or right-click the module, to choose an mp4, m4v or mov file.

- Jacks cover play/pause, STOP, LOOP and SPEED, which runs from 0.25x to 2x at 1 V per doubling. The
  sound follows SPEED like tape, so 2x is faster and higher, and it is silent while paused or stopped.
  END gives a pulse at the end of each clip.
- For YouTube, right-click the module and paste a link. VRU runs yt-dlp to download one H.264 mp4 file
  into a cache folder in Rack's user folder and plays that file. Downloads are reused, and the menu
  can open the folder.
- Install yt-dlp yourself, for example with `brew install yt-dlp`. YouTube often offers a single-file
  mp4 only up to 360p, so expect low resolution. That is untested on a real video.

### Processors

Processors read a picture on BUS IN and put a changed picture on BUS.

#### VRU–01 Deflection Yoke

Bends the raster the way a CRT's deflection coils do. Three coil oscillators, H, V and S, each have
amount, rate, phase and wave. The coil inputs run at audio rate. It also has zoom, curl, twist, flips,
edge, lines, spot, persistence and glow.

#### VRU–02 Scan Processor

Lifts each scanline by the picture's luma, so the picture becomes a vector-monitor relief. Deflection,
blank and Z inputs run at audio rate. It has two BUS outputs, BUS 1 and BUS 2, that carry the same bus.
LIFT IN takes a second picture: its brightness lifts the lines while BUS IN still sets how bright they
are and what color. That keeps an outline round while the texture changes. With nothing in LIFT IN,
BUS IN lifts itself.

#### VRU–04 Line Field

Draws the picture as a heightfield of scanlines. LINES, DEPTH, GAIN, X TILT, Y TILT, Z SPIN and PERSP
shape the view.

#### VRU–20 Rotator

Turns, zooms and slides the picture about a pivot you can move.

- ANGLE is one turn over 10 V, so a 0 to 10 V saw spins it with no seam.
- SPIN turns it continuously. A NOTCH trigger clicks it round by 1/16, 1/12, 1/8, 1/6 or 1/4 turn,
  with GLIDE to ease the move.
- FOLDS cuts the picture into 2 to 12 wedges for a kaleidoscope.
- EDGE is Black, Hold, Repeat or Mirror. RESET zeroes the spin and the notches.
- There is no perspective tilt.

#### VRU–24 Focus

Softens, smears, sharpens and misconverges a picture the way an old picture tube did.

- FOCUS softens in every direction, up to a blur about 6% of the picture width.
- SMEAR softens along each line only, mostly trailing to the right, up to about 15% of the width, and
  leaves the lines apart.
- SHARP crisps the edges with a thin bright and dark rim; zero is off.
- CONVERGE slides red and blue apart sideways in opposite directions.
- MIX blends with the original.
- Each knob has a CV jack and a small CV AMT trimmer. The order is CONVERGE, then FOCUS and SMEAR, then
  SHARP, then MIX. With every effect at zero it costs nothing.

#### VRU–09 Color Lab

Hue, saturation, brightness, contrast, posterize, solarize, channel swap and invert. MOD IN takes a
second picture whose brightness shifts the hue at each pixel; AMT sets how far.

#### VRU–10 Colorizer

Maps brightness to color through a four-stop gradient. It has hue CV, band quantizing and offset.

#### VRU–11 Wave Shaper

Drives, biases, folds, mirrors and quantizes the RGB or luma values.

### Mixers and keys

#### VRU–12 Keyer

Cuts a matte from the picture's luma. MODE sends out the Matte, a Color or a Fill. MOD IN takes a
second picture whose brightness moves the threshold at each pixel, so the key opens at a different
brightness in different places; AMT sets how far. It still cuts a key anywhere in a chain and puts its
matte out as a picture.

#### VRU–13 Bus Mixer

Blends BUS A and BUS B, with an optional MATTE BUS. The modes are Mix, Add, Multiply, Screen,
Difference, Max and Matte.

#### VRU–22 Video Mixer

The place where pictures meet, take their colors and are laid over one another. It is 30 HP wide. Four
pictures come in on A to D. One is live and one is next.

- FADER moves from the live picture to the next one, and at the far end the two swap, as on a T-bar
  fader, so a triangle LFO on FADER goes back and forth between them.
- CUT swaps them at once on a trigger. AUTO runs the fader by itself over TIME, 0.02 s to 20 s.
- NEXT picks the next input by button, by TRIG (a trigger that steps to the following input) or by
  NEXT CV, where 0 to 10 V picks A to D in four equal bands. With ADVANCE on, every finished transition
  steps NEXT, so one clock walks through all four pictures.
- Each input has a light, red while it is live and green while it is next, and a LIVE output below, a
  gate while that input is live, so sound can follow the picture.
- BLACK, with CV, fades everything to black, last of all.
- An input with nothing patched is absent. The Video Mixer never falls back to bus 1. NEXT steps only
  to inputs that have a picture. If a CUT or an AUTO take happens while the FADER knob is away from the
  end it started at, the new picture shows whole and stays so until the knob comes back to that end,
  instead of jumping to a part-way mix.
- Presets: Four Colors, Wipe Clock and Gradient Colors.

The transition. MODE picks DISSOLVE, ADD or WIPE.

- A dissolve is a crossfade. In ADD the next picture fades up over the live one, which then fades out,
  so halfway both are at full strength and their colors add where they overlap.
- WIPE sweeps a threshold through the greys of the picture in PATTERN, so any picture is a wipe. A
  Ramps layer gives straight and diagonal wipes, a Video Oscillator gives blinds, a Shapes picture
  gives an iris in the shape you set, and a camera or Feedback picture gives organic ones. SOFT sets
  the wipe's edge. With nothing in PATTERN, a wipe uses the next picture's own greys. The next picture
  then arrives brightest parts first, keyed in over the old one.
- The ALL button (lit while on) shows every patched picture at once. Each input has a LEVEL knob (0 to 100%,
  100% at first); a picture is coloured as usual, scaled by its LEVEL and added to the others, and the sum
  stops at white. A LEVEL of 0 takes a picture out. The right-click menu item "ALL mode averages instead of
  adds" divides by the number of patched pictures instead. While ALL is on, MODE, FADER, CUT, AUTO, NEXT and
  ADVANCE do nothing to the picture. LEVEL does nothing while ALL is off. In ALL each patched input's light
  is red, and every LIVE output is a gate.

Color. Each input has a HUE knob and a COLOR button.

- With COLOR on, the input is read as a grey picture and its brightness takes that hue: black stays
  black and white becomes the full color. With COLOR off, the picture passes in its own colors. The four
  hues start a quarter turn apart round the color wheel.
- SAT, with CV, sets the strength of all the colors at once. At zero every colored input is plain grey.
- SHIFT is one CV input that moves all four hues round the wheel together. 10 V is once round, as on the
  Rotator's ANGLE.
- Click COLOR to step an input through Off, Hue and Gradient. Its light is dark, white or amber. In
  Gradient, the brightness runs through four colors: black, the input's hue, a hue SPAN further round
  the wheel, and white at the top. SPAN, with CV, is one knob for all four inputs. At zero a Gradient
  input is one hue from black up to white. SAT and SHIFT work on it as they do on Hue.
- The Colorizer keeps its four-stop gradient and its bands: patch one before an input for false color
  and leave that input's COLOR off.

Key. A KEY input lays a picture over the mix, and the mixer cuts the hole for it.

- THRESH, with CV, sets the brightness at which the key opens, and the key's SOFT sets how gradual its
  edge is. They are the same THRESH and SOFT as on the Keyer.
- With nothing in MATTE, the KEY picture's own brightness cuts the hole. A picture in MATTE cuts it
  instead, so one picture gives the shape and another fills it.
- FILL picks what shows in the hole: the KEY picture itself, or a flat color from the key's own KEY HUE
  knob. INV turns the matte inside out.
- KEY ON, a button with a GATE input, switches the layer on and off at the next frame.
- The key comes after the transition and before BLACK, where a hardware video mixer has its last keyer,
  so cuts and wipes go on underneath it.

### Time and memory

#### VRU–14 Feedback

Feeds the previous frame back into the picture, with zoom, rotation, shift and a hue step on every
pass. It has ADD and BLEND modes, a Clamp, Wrap or Black edge, a luma key and CLEAR.

Its right-click menu has "Fade trails fully to black". With it on, a trail dies away to pure black.
With it off, a trail stops fading at a dim grey (about grey 10 at gain 0.95, 25 at 0.98, 50 at 0.99)
and stays there. A new Feedback has it on. A patch saved before the option existed loads with it off
and looks exactly as it did.

#### VRU–15 Freeze Frame

Holds a frame. A TRIG pulse captures one, and STROBE recaptures up to 12 times a second. HOLD and BLEND
set how much of the frozen frame replaces the live picture. CLEAR drops it.

#### VRU–27 Frame Delay

Shows its input picture a few frames later, 1 to 30 of them (a frame is 1/60 of a second). It is 7 HP
wide, with FRAMES, a CV jack and a CV AMT trimmer. Its presets are Short Echo, Slow Trail and Half
Second.

- It is what lets you cable a loop. A picture that goes round a loop with no delay in it is an error
  (Display writes CYCLE), but a loop that goes through a Frame Delay is fine: the delay shows the
  picture from a few frames back, so each picture is built from the last one. Put a Video Mixer, a
  Rotator or any other chain in the loop and the picture feeds on itself. Demo patch 7, Loop Through A
  Delay, is the starter.
- Until the delay has a picture that old it shows black, so a loop starts from nothing. If its source
  stops, the delay stops showing a picture (NO VIDEO), as Feedback does. Resetting the module clears
  what it holds.
- The delay is counted in 60ths of a second of audio time, not in screen frames. If Rack draws slower
  than 60 a second, the delay is still the same length in time, so some frames are never shown. Every
  Display, every picture size and the Syphon Out see the same delayed picture.
- Each Frame Delay keeps up to 31 pictures in memory (about 190 MB at 1440 x 1080 with FRAMES at 30).

### Outputs and bridges

#### VRU–16 Display

Shows a bus inside Rack. It has three video inputs, A, B and C.

- The SEL button, or the right-click Input menu, picks which input is on screen. THRU repeats the
  selected input's voltage, so you can keep patching from it.
- SHOT saves a PNG. REC records an H.264 .mov, and a second press stops it. Files go to
  `~/Pictures/Video Research Unit` unless you pick another folder in the right-click menu. REC needs
  macOS.
- L and R are audio inputs for REC only. Patch a cable into L, R or both before you press REC and the
  movie gets a stereo sound track (AAC): ±5 V is full level, anything louder is clipped. One cable alone
  is heard on both sides. Nothing patched means no sound track. The sound sets the movie's clock, so
  picture and sound stay together. If you change Rack's audio sample rate during a recording, the
  recording stops and the file is kept up to that point. A paused Rack pauses the movie. Syphon carries
  pictures only, so for sound alongside Syphon send Rack's audio out through your audio interface or a
  loopback device.
- The right-click Panel width menu offers 12, 16, 20 and 24 HP, or full height, the default.
- The right-click menu also holds three plugin-wide settings. See [Display settings](#display-settings).

#### VRU–08 Syphon In

Brings in a picture from any Mac app that publishes a Syphon server. Choose the server in the
right-click menu, which lists the running servers and None.

#### VRU–17 Syphon Out

Publishes a bus as a Syphon server that other Mac apps can pick up. Frames are 4:3, either 1024 x 768
or 1440 x 1080, chosen in the right-click menu. The server name is `VRU – <label or module id>`.

#### VRU–26 Video Probe

Turns a picture into voltages. It is the one module that works from picture to sound instead of sound
to picture. It is 20 HP wide. It has four presets: Photocell, Wide Average, Audio Scan and Bright Spot
Gate.

The read point. BUS IN takes any video bus. X and Y move a read point over the picture: -5 V is the
left (or bottom) edge, +5 V the right (or top) edge, and anything beyond stays at the edge. The X and Y
knobs set where the point rests, and the X and Y jacks add to it (each jack has a small AMT knob that
scales or flips it). SIZE reads a square patch instead of a point and gives its average, up to the
picture's full height. SMOOTH slows LUMA, R, G and B (none at the bottom, about a second at the top).

The outputs.

- LUMA (brightness), R, G and B give what is under the point, 0 to 10 V, at every audio sample. Swept
  slowly it is a photocell moved across the screen. Swept at audio rate by an oscillator, an LFO or Scope
  Trace's SWEEP output, the picture becomes a waveform you can modulate with or listen to.
- AVG, MOTION and GATE describe the picture and change once per picture. AVG is the average brightness
  of the whole picture. MOTION is how much it changed since the last picture (10 V is an average change
  of 12.5% of full brightness). GATE is 10 V while the patch under the read point is brighter than LEVEL
  (with a small margin so it does not chatter).
- FRAME is a 10 V pulse, 1 ms long, each time a new picture arrives. It can clock a sample and hold or
  a sequencer to the picture. LUMA, R, G and B change only at or after the pulse.

The panel shows the picture with the read point and patch drawn on it. SIG is lit while pictures
arrive.

The rule from picture to sound: the output is the picture's brightness along the path that X and Y
trace, and the picture underneath refreshes about 60 times a second, one picture late. Sweeping X at a
frequency f across a picture of N vertical bars gives a tone at N times f.

Limits.

- The picture is read at 320 x 240 and is redrawn with the screen, which is 60 times a second unless
  you changed Rack's frame rate. The read point moves at audio rate, but the picture under it only
  changes at the picture rate.
- The outputs hold their last value when the bus has no picture (SIG goes dark and the preview says NO
  VIDEO), when the module is bypassed, and when "Keep video running when hidden" is off and the module
  is scrolled out of the Rack window. With that setting on, the default, the outputs keep updating while
  the module is scrolled out of view, at 60 pictures a second, or 30 when nothing at all is on screen.
  Whether they keep updating when Rack's window is minimised depends on whether Rack keeps stepping its
  modules then. The plugin cannot make Rack do that, and this has not been checked.

Two ways to start.

- Control a sound from a film: Video Player BUS into Video Probe BUS IN, an LFO into X, and LUMA to a
  filter's cutoff. The brightness along a line through the film now moves the filter.
- Hear a picture: Scope Trace SWEEP into X, a slow LFO into Y, and LUMA to an audio output. LUMA is 0 to
  10 V, so put a DC blocker or a mixer with an offset control before loud speakers. Each sweep of the
  screen plays one line of the picture, and the slow LFO moves that line up and down.

Demo patch `VRU-2.3.0-test.vcv` has a Video Probe row that uses LUMA to set the pitch of a second
oscillator's bars.

#### VRU–19 1V CV Out

Scales Rack CV into 0 to 1 V, plus or minus 1 V, 0 to 5 V or plus or minus 5 V, for DC-coupled
hardware outputs. It has eight lanes, each with scale, offset, slew and invert, plus a calibration
output and a safe mute. It only scales CV.

### Modulation

None of these modules carries a video bus. Their outputs are ordinary Rack CV, so patch them to
anything.

#### VRU–18 Dual LFO Chaos

Slow cyclic and chaotic modulation. The outputs are A, B, X, Y, S&H and CLK.

#### VRU–23 Octoscillator

Eight chaotic CV channels with three outputs each (X, Y and Z), 24 in all.

- SPEED changes their overall pace; SPREAD distributes their rates. RELATE moves from separate
  trajectories to a shared movement.
- Each channel has RATE, DEPTH and OFFSET. LEVEL adjusts all depths; RANGE selects ±5 V, 0–5 V or ±1 V
  before offset.
- Patch the outputs to position, rotation, colour, keying and mixer controls. Start with small DEPTH
  settings.
- Very fast CV works spatially only on destinations with scan-time capture (such as Deflection Yoke);
  frame-sampled controls can alias.
- Presets: Slow Together, Eight Own Drifts, Fast Jitter, Staggered Doubling and Gentle Fine Mod.

#### VRU–25 Vector Osc

A pair of oscillators made to feed Scope Trace's X IN and Y IN, so the XY picture is a rich figure that
moves by itself. It is 12 HP wide.

- FREQ sets X, with a V/OCT jack.
- RATIO sets how fast Y runs compared with X and snaps to whole and simple ratios (1:4 up to 9:1); FINE
  moves it up to a semitone off the chosen ratio so the figure turns slowly, and the RATIO jack steps
  along the list (1 V per step).
- PHASE turns Y against X: at 1:1 with a sine wave, a quarter turn (the default) draws a circle.
- VOICES (1 to 8) stacks that many copies of each side, slightly detuned and averaged, and DETUNE sets
  how far they spread (zero is one clean line). The figure becomes a bundle of traces that slowly
  breathes.
- WAVE runs each voice from sine through triangle and saw to square; Y WAVE makes Y's shape differ from
  X's. LEVEL scales X and Y.
- DETUNE, WAVE and PHASE have CV jacks, and a rising edge on SYNC restarts every voice, so a saved patch
  always starts from the same figure.
- The outputs are X and Y (±5 V) and Z (0 to 10 V brightness: bright where the trace moves slowly, dim
  where it moves fast; patch it to Scope Trace's Z CV jack).

## Patching ideas

- **A loop.** Put a Frame Delay in a loop with a Video Mixer and a Rotator, as in demo patch 7. Each
  picture is built from the last, turned and shrunk a little, so a shape leaves a spiralling trail. A
  Feedback module does something similar without a loop cable.
- **One picture bends another.** Send one Video Oscillator into FM IN on a second, as in demo patch 8. Or
  send a Shapes or Ramps picture into the MOD IN of a Color Lab, a Keyer or Ramps, or into the LIFT IN
  of a Scan Processor.
- **Picture to sound, and back.** Video Probe turns any picture into voltages. Use LUMA to drive a filter
  or a pitch, or sweep X at audio rate to hear the picture. Send the same voltages back into a
  Video Oscillator's FREQ or into a Deflection Yoke, and the sound bends the picture that made it.
- **Sound to picture.** Audio into Scope Trace's X IN and Y IN draws it. Audio into a Deflection Yoke
  coil, a Scan Processor or the Video Oscillator's IN A and IN B bends a picture at audio rate. See
  [Latency](#latency-sound-bending-a-picture).
- **Shapes as a matte.** A Shapes picture through a Keyer, a Wave Shaper fold or a Colorizer gives a
  matte, an outline or a halo. As the PATTERN of a Video Mixer wipe it gives an iris.
- **Slow movement.** Octoscillator and Dual LFO Chaos outputs on Rotator, Shapes, Colorizer and Video
  Mixer controls make a picture that never repeats, as in demo patch 5.

### Latency: sound bending a picture

Sound that bends a picture is not instant. Deflection Yoke, Scan Processor and the Video Oscillator's IN
A, IN B and BEND read the sound over a window one field long (16.7 ms at the default 240 lines), so a
sample shows up on a Display between 0.04 ms and about 33 ms after it was made (about 17 ms typically),
before the monitor's own delay. With "Render video on its own thread" on, add one more tick: about 50 ms
worst and 25 ms typical before the screen. Scope Trace draws the newest samples and is a frame behind
at most (up to 17 ms). The derivation is in `docs/architecture.md`, "Latency: from a sample to the
screen".

## Display settings

The right-click menu of any Display holds three settings. Each applies to the whole plugin, not to one
Display.

- **Keep video running when hidden** (on by default). Every picture keeps advancing while no Display is
  on screen. Beam trails, the Video Oscillator and Feedback carry on, so a Display you scroll back to
  shows no jump. SHOT and REC also work while the Display is scrolled out of view, and so does Video
  Probe. If no Display, Syphon Out or Video Probe is on screen at all, the pictures are made 30 times a
  second instead of 60. When it is off, a picture is made only for a module that is on screen.
- **Render video on its own thread** (off by default). The pictures are made on a thread of their own,
  60 a second at a steady pace, so a slow video graph does not stall Rack's interface and a slow
  interface does not stall the video. The Display shows the newest finished picture, so it is up to one
  picture late. SHOT, REC and Video Probe take their pictures from that thread. Turn it off again at
  any time to go back to drawing on Rack's interface thread.
- **Full precision between modules** (off by default). Modules normally pass each other pictures with
  256 levels per color. With this on, Color Lab, Colorizer, Wave Shaper, Keyer, Bus Mixer, Video Mixer,
  Freeze Frame, Frame Delay, Ramps, Shapes, the Video Oscillator, Rotator, Feedback, Deflection Yoke,
  Line Field, Focus, Scope Trace and Scan Processor hand each other pictures with fine steps and round to
  8 bits only where a picture leaves the graph. A gain of 0.25 followed by 2.0 then keeps its levels,
  and a loop through a Frame Delay, a Rotator or a Feedback fades smoothly to black. Pictures from Video
  Player and Syphon In are 8-bit to begin with. The cost is memory: it uses about four times as much
  while it is on. A Frame Delay stores 8-bit pictures instead once its float store would pass 256 MB,
  and its tooltip says so.

## Demo patches

The numbered demo patches are not inside the plugin. They are ordinary patch files in a separate
download on the release page, `VideoResearchUnit-demos-<version>.zip` (in this repository they are in
`patches/`). Unzip it and open any patch with File > Open in VCV Rack. The demos use only VRU and the
free VCV Fundamental and Core modules. Each patch has a Notes module that says what to try.

| Patch | What it shows |
| --- | --- |
| `1 - Scope Trace Lissajous.vcv` | Two oscillators a fifth apart draw a Lissajous figure on Scope Trace, and LFOs detune and turn it. |
| `2 - Video Oscillator Moire.vcv` | Two Video Oscillators mixed in Difference mode in the Bus Mixer make moiré stripes, painted by a Colorizer. |
| `3 - Feedback Tunnel.vcv` | A Shapes star fed back into itself through Feedback makes a glowing tunnel, bent by a Deflection Yoke. |
| `4 - Slow Scan Processor.vcv` | A slow patch for leaving running: a Scan Processor draws a Video Oscillator and a Shapes picture as lines, moved by very slow LFOs. |
| `5 - Octoscillator Garden.vcv` | An Octoscillator moves a Shapes ring, a Rotator's kaleidoscope folds and a Colorizer, so nothing repeats. |
| `6 - Muybridge on the Scan Processor.vcv` | A film clip drawn as lines by a Scan Processor. It needs a video (see below). |
| `7 - Loop Through A Delay.vcv` | A Video Mixer, Rotator and Frame Delay in a loop leave a spiralling trail behind a moving shape. |
| `8 - One Oscillator Bends Another.vcv` | One Video Oscillator sends its picture into FM IN of a second, so the second one's bars bunch and spread. |

`Folded-Aurora.vcv` and `Scan-Landscape.vcv` are two older patches in `patches/` that are not in the
download. Two test patches, `VRU-2.2.0-test.vcv` and `VRU-2.3.0-test.vcv`, hold one row for each
feature added in those versions: Vector Osc, the Video Oscillator hold knobs and the Video Mixer gradient
for 2.2.0, and Video Probe, a Frame Delay loop, FM IN, LIFT IN and Feedback's fade option for 2.3.0.
Each row ends in its own Display.

The demo `6 - Muybridge on the Scan Processor.vcv` needs a video. The same zip has a `clip` folder
with Eadweard Muybridge's "The Horse in Motion" (1878, public domain) as an MP4, plus its credits.
Right-click the Video Player, choose "Open video file...", and pick the MP4. Any other video works
too. The original GIF is on Wikimedia Commons:
https://commons.wikimedia.org/wiki/File:Muybridge_race_horse_animated.gif

## Old patches

Patches saved by earlier versions load in version 2.3.0. The new inputs and options are added so that
an old patch makes the same picture as before: Feedback's "Fade trails fully to black" loads off, and
FM IN, MOD IN, LIFT IN and the settings that default to off change nothing until you use them.

Version 2.1.0 gave two modules new internal names: Wobbulator is now DeflectionYoke and RuttEtra is now
ScanProcessor. Panels, names and every parameter are unchanged, but a patch saved with 2.0.0 that uses the
Deflection Yoke or the Scan Processor will not find them until it is migrated. Run
`python3 scripts/migrate-slug.py --dry-run` to see what it would change, then run it without `--dry-run`
while Rack is closed. It backs up every file it touches to `~/VRU-migration-backups/` first, with a restore
script. Patches that use neither module are unaffected.

Three changes to Scope Trace affect patches saved before them.

- DWELL did not exist. A patch saved before DWELL loads it at 0, the even trace that patch was made
  with. A new Scope Trace starts at DWELL 0.5.
- ROTATE wraps around instead of stopping at plus or minus 180 degrees. A patch whose rotation knob plus
  CV stayed inside the range draws the same. A patch that drove it past the end now keeps turning.
- BEAM is now a true Gaussian width, so an old patch draws wider, softer lines. Where strokes
  overlap, under a long DECAY and with BLOOM, it can also look brighter overall.

`docs/scope-trace-audit.md`, section 8, has the numbers. `docs/architecture.md` lists every
behavior change under "Behaviour changes for old patches".

## Known limits

- VRU has been tested only on Apple silicon Macs. Windows, Linux and Intel Macs are untested. VRU is in
  active development.
- Syphon works inside Rack but has not been checked with OBS or Resolume. The Syphon Out server label
  has no panel control yet.
- Pictures refresh about 60 times a second, with Rack's interface. Video Probe reads a 320 x 240
  picture, one picture late. Sound that bends a picture is late by the amounts under
  [Latency](#latency-sound-bending-a-picture).
- Pictures between modules are 8-bit unless "Full precision between modules" is on. Pictures from
  Video Player and Syphon In, and Video Probe's read, are 8-bit either way.
- A patch has 64 buses and a chain runs 17 modules, one source and 16 processors.
- YouTube playback needs yt-dlp, usually gives low resolution and is untested on a real video. REC
  needs macOS.
- The Video Mixer has no SUM mode with a level for each input, no BORDER along the wipe's edge and no
  hue CV for each input. The Rotator has no perspective tilt.
- Fast CV works spatially only on destinations that read signals at each point of the scan (such as
  Deflection Yoke). Frame-sampled controls can alias.

## Building from source

You need the Rack SDK that matches your Rack version and the Xcode command line tools.

```sh
./scripts/download-rack-sdk.sh 2.6.6     # use the version that matches your Rack
export RACK_DIR="$PWD/Rack-SDK"
make
make dist                                # writes dist/VideoResearchUnit-<version>-mac-arm64.vcvplugin
make install                             # builds, then copies the plugin into Rack's plugins folder
make test                                # unit tests, bus API tests, demo patch render check, Display capture and Syphon round trip
make tsan                                # thread-sanitizer builds of the bus API service and render pool tests
make bench                               # render timing benchmarks
```

Syphon is linked by default on Apple silicon Macs. `Frameworks/RLSyphon.framework` is a copy of
Syphon-Framework with every symbol renamed to an `RLSyphon` prefix, so it cannot clash with another
Syphon copy in Rack. `scripts/build-syphon-framework.sh` rebuilds it from the pinned commit and
needs python3. To build without Syphon, run `RASTERLAB_SYPHON=0 make`.

If a build fails with a tapi error such as "unknown architecture", try setting
`DEVELOPER_DIR=/Library/Developer/CommandLineTools`.

`make release-extras` builds the demo download from `patches/`.

## For developers

How it is built:

- Rack calls `process()` at audio rate. Keep that path to reading CV, updating atomics and writing
  ordinary voltages. Drawing belongs on the UI side.
- The bus table has 64 nodes, in `src/video_bus.hpp` and `src/video_bus.cpp`. Each owner publishes
  its node, plain data, from `process()`. `src/video_graph_renderer.cpp` resolves a bus by walking
  source nodes back to a source, up to 16 levels deep, and returns the last good frame on a cycle.
  Frame Delay is the one node it treats as a leaf, which is why a loop through it is legal.
- Display, Syphon Out and Video Probe ask the render service for pictures; a module that is not drawn
  registers from `step()` and the service renders for it. Syphon Out uploads the frame as an OpenGL
  texture in `drawFramebuffer()`, only when drawn. Syphon In and Video Player write incoming frames into
  external frame slots.

### The public bus API

Another plugin can read and write pictures on the VRU bus through two headers, `include/vru_bus.h` and
`include/vru_bus_client.hpp`. Version 1 has an 8-bit read and a way to publish a source's pictures.
Version 2 adds to it without changing it:

- a float read (`readFrameF32`), with pictures from 0 to 1;
- a tick (`readFrameEx`, `readFrameF32Ex`), which says whether a picture is new since your last read;
- the native size of a source (`getNativeSize`).

A client built against version 1 keeps working, and a client built for version 2 falls back to version
1 when VRU is older. `docs/VIDEO-BUS.md` describes both versions and ships inside the .vcvplugin. The
headers are under the MIT license (see [License](#license)).

### Seeing a patch without Rack

`build/vru-render-patch` renders what each Display in a `.vcv` patch would show. Build it with
`make build/vru-render-patch` (macOS, needs `RACK_DIR`), then run:

```sh
export DYLD_LIBRARY_PATH="$RACK_DIR"   # libRack.dylib is found there
build/vru-render-patch PATCH.vcv --out DIR [--seconds 2] [--frames 4] [--size 640x480] [--seed 1] [--fps 60] [--thread-mode]
```

It reads plain JSON patches and zstd tar patches (the second needs `zstd` and `tar` on the PATH). It
builds the real VRU module classes from `src/*.cpp`, loads each module's saved params and data, connects
the cables and runs every `process()` at 48 kHz, so it cannot drift from the plugin. The plugin's render
service (the one the Display uses) then draws the selected input of every Display, `--fps` times a
second, and the tool writes `--frames` PNGs at evenly spaced times to
`DIR/display-<module id>-frame-<n>-<ms>ms.png`. It also writes `DIR/report.txt`, which says for every
module whether it ran for real, was a stand-in, or was unsupported. Read it before you trust a picture.
The same patch, options and seed give byte-identical PNGs. With `--thread-mode` the service's
thread-mode tick is driven synchronously; the PNGs are identical to the default mode.

Its limits:

- Every VRU module is real except these. Video Probe runs but its outputs stay 0 V, because its sampler
  is fed by the widget, and this tool builds no widgets. Video Player and Syphon In have no source
  offline, so they draw a labelled color-bar test card. Syphon Out is not built, because it has no
  output a Display could show.
- Other plugins cannot run. Fundamental LFO, VCO, Noise, VCA and 8vert have stand-ins, listed in the
  report. LFO and VCO read their saved FREQ (and FINE) and make plain waves; their CV inputs are
  ignored, except the VCO's V/OCT. The port orders of the VCA and 8vert stand-ins and the output order of
  Noise are from memory of Fundamental, not from its source. Anything else outputs 0 V and is reported as
  unsupported. Core modules are ignored.
- Cables carry one channel. Poly cables are not modelled.
- Time is simulated: a Feedback or phosphor picture depends on `--fps`. The default is 60, which is about
  what Rack's UI draws, and it is slow at large sizes. A Display still renders at its own size, not the
  size on screen.

### Where to read more

- `docs/architecture.md`: how the bus model, the renderer and each module work, and why.
- `docs/VIDEO-BUS.md`: how the VRU Video Bus works and what other plugin developers can and cannot do
  with it, including "Version 2".
- `docs/syphon.md`: notes on the Syphon bridge and the renamed framework.
- `docs/module-ideas.md`: candidate modules.
- `docs/scope-trace-audit.md`: the Scope Trace audit.

Open items:

- Check Syphon Out and Syphon In with OBS and Resolume.
- Add a panel control for the Syphon Out server label. The patch field `serverLabel` is read today.
- Move more processors, Feedback included, from CPU frames to shader-native texture rendering.
- Add CV attenuverters and CV inputs for more color controls.

## License

GPL-3.0-or-later. See `LICENSE`. The bundled Syphon framework keeps its own license in
`Frameworks/RLSyphon-LICENSE.txt`.

The two public bus headers, `include/vru_bus.h` and `include/vru_bus_client.hpp`, are under the MIT
license (`include/LICENSE-MIT`) so that other plugin developers, including closed-source and commercial
ones, can copy them into their plugins. Everything else in this repository is GPL-3.0-or-later.
