# Video Research Unit

Video Research Unit (VRU) is a video synthesizer you patch in VCV Rack 2. It is one plugin with 24
modules, numbered VRU–01 to VRU–24. Rack's module browser lists them under the brand
"Video Research Unit". Sources make a picture. Processors bend, color and mix it. Display or Syphon
Out shows it. CV from any Rack module can drive the CV inputs.

VRU is in active development. It has been tested only on Apple silicon Macs.

## The catalogue

Every module carries a catalogue number. The panel header shows the series name, a rule, the number
and the title. Display's header is a single line (series name at the left of the picture, DISPLAY in the
middle, the number at the right), so its picture keeps its full size. Rack's module browser lists the
same names, for example "VRU–01 Deflection Yoke".

```
VIDEO RESEARCH UNIT
────────────────────────
VRU–01  DEFLECTION YOKE
VRU–02  SCAN PROCESSOR
VRU–03  SCOPE TRACE
VRU–04  LINE FIELD
VRU–05  VIDEO OSCILLATOR
VRU–06  RAMPS
VRU–07  VIDEO PLAYER
VRU–08  SYPHON IN
VRU–09  COLOR LAB
VRU–10  COLORIZER
VRU–11  WAVE SHAPER
VRU–12  KEYER
VRU–13  BUS MIXER
VRU–14  FEEDBACK
VRU–15  FREEZE FRAME
VRU–16  DISPLAY
VRU–17  SYPHON OUT
VRU–18  DUAL LFO CHAOS
VRU–19  1V CV OUT
VRU–20  ROTATOR
VRU–21  SHAPES
VRU–22  VIDEO MIXER
VRU–23  OCTOSCILLATOR
VRU–24  FOCUS
```

The sections below group the modules by what they do and call them by name.

## The modules

Modules with a BUS output put a picture on a numbered video bus. Later sections explain buses.

### Sources

- **Video Oscillator**: a video oscillator and test-pattern generator. The PATTERN knob picks
  oscillator, SMPTE-style 75% bars, EBU-style 75% bars, EBU-style 100% bars, grayscale ramp,
  grid or Bars and Rows. Bars and Rows runs two oscillators, A and B, against the television
  scan: each can draw bars across the picture or rows down it, locked to the scan or free
  running, with WAVE, BEND (B bends A), MIX and SPREAD. FREQ A, WAVE A, FREQ B, BEND, HUE and FB
  take CV. Each pattern has a factory preset in the right-click Preset menu.
- **Ramps**: two grayscale layers. Each layer runs along a horizontal, vertical, radial or angular
  axis, with CV for frequency, phase and wave. The wave morphs from ramp through triangle and sine
  to soft square. The layers combine as A, A + B, A - B or A x B.
- **Shapes**: draws one closed shape and lets voltages move it. SIDES runs from 3 to 12 without
  steps, by blending each polygon into the next. ROUND softens the corners until the shape is a
  circle. POINT pulls the middle of each side inward until the polygon is a star. SIZE, X, Y, ASPECT
  and TURN place it. REPEAT tiles it across the screen in a grid. COPIES puts up to eight of it in a
  ring, RING sets the radius of that ring, and MELT lets neighbours fuse like drops.
  - The output is not a silhouette. It is a grey picture in which brightness is the distance from the
    shape's edge: mid grey on the edge, brighter inside, darker outside, with SPREAD setting how fast
    the grey falls away.
  - That one picture gives a hard matte through the Keyer, a soft matte with the Keyer's softness, an
    outline or concentric rings through the Wave Shaper's fold, and a halo through the Colorizer.
  - The MATTE switch gives the plain white on black shape for when that is all that is wanted.
  - SIDES, ROUND, POINT, SIZE, X, Y, ASPECT, TURN, MELT and SPREAD take CV. REPEAT, COPIES and RING
    are knobs only. Shapes has no bus input.
  - Presets: Soft Hexagon and Melting Ring.
- **Scope Trace**: an oscilloscope and Lissajous source, 20 HP wide. Patch audio or CV into X IN and
  Y IN. It has four modes: XY Lissajous, Oscilloscope, Dual trace and Polar.
  - X and Y scale, X and Y position, rotation, hue, timebase and brightness each have a CV jack with
    an attenuverter. The brightness jack is labeled Z. So do DWELL, INTENS, DECAY, BEAM, BLOOM and the
    trigger level (the last jack, labeled LEVEL, adds volts to the level knob). The six lower pairs
    are each labeled with the control they move.
  - ROTATE turns all the way around. 10 V of CV is one full turn, so a rising ramp or saw spins the
    picture, and the knob sets the starting angle.
  - SWEEP is a ramp over the scope's own window. SYNC is a pulse at the start of each sweep.
  - BEAM sets the width of the beam, a round soft spot. DWELL sets how much brighter the trace is
    where the beam moves slowly. At 0 the line is even. At 1 a stopped beam burns a dot, as on a CRT.
  - Presets: XY Lissajous, Audio Scope, Dual Trace, Vector Bloom and Polar Drift.
- **Video Player**: plays a video file or a YouTube link and puts the picture on its bus and the
  clip's own sound on the L and R outputs (+-5 V at full scale; a mono clip feeds both, and a clip with
  no sound track is silent). Press OPEN FILE, or right-click the module, to choose an mp4, m4v or mov
  file. Jacks cover play/pause, STOP, LOOP and SPEED, which runs from 0.25x to 2x at 1 V per doubling.
  The sound follows SPEED like tape, so 2x is faster and higher, and it is silent while paused or
  stopped. END gives a pulse at the end of each clip.
  - For YouTube, right-click the module and paste a link. VRU runs yt-dlp to download one
    H.264 mp4 file into a cache folder in Rack's user folder and plays that file. Downloads are
    reused, and the menu can open the folder.
  - Install yt-dlp yourself, for example with `brew install yt-dlp`. YouTube often offers a
    single-file mp4 only up to 360p, so expect low resolution. That is untested on a real video.
- **Syphon In**: brings in a picture from any Mac app that publishes a Syphon server. Choose the
  server in the right-click menu, which lists the running servers and None.

### Demo patches and demo clip

The demo patches are not inside the plugin. They are ordinary patch files in a separate download on
the release page, `VideoResearchUnit-demos-<version>.zip` (in this repository they are in `patches/`).
Unzip it and open any patch with File > Open in VCV Rack. The demos use only VRU and the free VCV
Fundamental and Core modules.

The demo `6 - Muybridge on the Scan Processor.vcv` needs a video. The same zip has a `clip` folder
with Eadweard Muybridge's "The Horse in Motion" (1878, public domain) as an MP4, plus its credits.
Right-click the Video Player, choose "Open video file...", and pick the MP4. Any other video works
too. The original GIF is on Wikimedia Commons:
https://commons.wikimedia.org/wiki/File:Muybridge_race_horse_animated.gif

### Processors

Processors read a picture on BUS IN and put a changed picture on BUS.

- **Deflection Yoke**: bends the raster the way a CRT's deflection coils do. Three coil oscillators,
  H, V and S, each have amount, rate, phase and wave. The coil inputs run at audio rate. It also has
  zoom, curl, twist, flips, edge, lines, spot, persistence and glow.
- **Scan Processor**: lifts each scanline by the picture's luma, so the picture becomes a
  vector-monitor relief. Deflection, blank and Z inputs run at audio rate. It has two BUS outputs,
  BUS 1 and BUS 2, that carry the same bus.
- **Line Field**: draws the picture as a heightfield of scanlines. LINES, DEPTH, GAIN, X TILT,
  Y TILT, Z SPIN and PERSP shape the view.
- **Color Lab**: hue, saturation, brightness, contrast, posterize, solarize, channel swap and invert.
- **Colorizer**: maps brightness to color through a four-stop gradient. It has hue CV, band
  quantizing and offset.
- **Keyer**: cuts a matte from the picture's luma. MODE sends out the Matte, a Color or a Fill.
- **Wave Shaper**: drives, biases, folds, mirrors and quantizes the RGB or luma values.
- **Freeze Frame**: holds a frame. A TRIG pulse captures one, and STROBE recaptures up to 12 times a
  second. HOLD and BLEND set how much of the frozen frame replaces the live picture. CLEAR drops it.
- **Feedback**: feeds the previous frame back into the picture, with zoom, rotation, shift and a hue
  step on every pass. It has ADD and BLEND modes, a Clamp, Wrap or Black edge, a luma key and CLEAR.
- **Rotator**: turns, zooms and slides the picture about a pivot you can move. ANGLE is one turn over
  10 V, so a 0 to 10 V saw spins it with no seam. SPIN turns it continuously, and a NOTCH trigger clicks it
  round by 1/16, 1/12, 1/8, 1/6 or 1/4 turn, with GLIDE to ease the move. FOLDS cuts the picture into 2 to 12
  wedges for a kaleidoscope. EDGE is Black, Hold, Repeat or Mirror. RESET zeroes the spin and the notches.
  There is no perspective tilt.
- **Focus**: softens, smears, sharpens and misconverges a picture the way an old picture tube did.
  FOCUS softens in every direction, up to a blur about 6% of the picture width. SMEAR softens along
  each line only, mostly trailing to the right, up to about 15% of the width, and leaves the lines
  apart. SHARP crisps the edges with a thin bright and dark rim; zero is off. CONVERGE slides red and
  blue apart sideways in opposite directions. MIX blends with the original. Each knob has a CV jack
  and a small CV AMT trimmer. The order is CONVERGE, then FOCUS and SMEAR, then SHARP, then MIX.
  With every effect at zero it costs nothing.

### Mixing

- **Bus Mixer**: blends BUS A and BUS B, with an optional MATTE BUS. The modes are Mix, Add,
  Multiply, Screen, Difference, Max and Matte.
- **Video Mixer**: the place where pictures meet, take their colors and are laid over one another.
  It is 30 HP wide. Four pictures come in on A to D. One is live and one is next. FADER moves from
  the live picture to the next one, and at the far end the two swap, as on a studio T-bar, so a
  triangle LFO on FADER goes back and forth between them. CUT swaps them at once on a trigger. AUTO
  runs the fader by itself over TIME, 0.02 s to 20 s. NEXT picks the next input by button, by TRIG
  (a trigger that steps to the following input) or by NEXT CV, where 0 to 10 V picks A to D in four
  equal bands. With ADVANCE on, every finished transition steps NEXT, so one clock walks through
  all four pictures. Each input has a light, red while it is live and green while it is next, and
  a LIVE output below, a gate while that input is live, so sound can follow the picture. BLACK,
  with CV, fades everything to black, last of all.
  - MODE picks the transition: DISSOLVE, ADD or WIPE. A dissolve is a crossfade. In ADD the next
    picture fades up over the live one, which then fades out, so halfway both are at full
    strength and their colors add where they overlap. WIPE sweeps a threshold through the greys of
    the picture in PATTERN, so any picture is a wipe. A Ramps layer gives straight and diagonal
    wipes, a Video Oscillator gives blinds, a Shapes picture gives an iris in the shape you set,
    and a camera or Feedback picture gives organic ones. SOFT sets the wipe's edge. With nothing
    in PATTERN, a wipe uses the next picture's own greys. The next picture then arrives
    brightest parts first, keyed in over the old one.
  - Color: each input has a HUE knob and a COLOR button. With COLOR on, the input is read as a grey
    picture and its brightness takes that hue: black stays black and white becomes the full color.
    With COLOR off, the picture passes in its own colors. The four hues start a quarter turn apart
    round the color wheel. SAT, with CV, sets the strength of all the colors at once. At zero every
    colored input is plain grey. SHIFT is one CV input that moves all four hues round the wheel
    together. 10 V is once round, as on the Rotator's ANGLE.
  - Key: a KEY input lays a picture over the mix, and the mixer cuts the hole for it. THRESH, with
    CV, sets the brightness at which the key opens, and the key's SOFT sets how gradual its edge is.
    They are the same THRESH and SOFT as on the Keyer. With nothing in MATTE, the KEY picture's own
    brightness cuts the hole. A picture in MATTE cuts it instead, so one picture gives the shape and
    another fills it. FILL picks what shows in the hole: the KEY picture itself, or a flat color from
    the key's own KEY HUE knob. INV turns the matte inside out. KEY ON, a button with a GATE input,
    switches the layer on and off at the next frame. The key comes after the transition and before
    BLACK, where a studio mixer has its downstream keyer, so cuts and wipes go on underneath it.
  - An input with nothing patched is absent. The Video Mixer never falls back to bus 1. NEXT steps
    only to inputs that have a picture. If a CUT or an AUTO take happens while the FADER knob is away
    from the end it started at, the new picture shows whole and stays so until the knob comes
    back to that end, instead of jumping to a part-way mix.
  - The Colorizer keeps its four-stop gradient and its bands: patch one before an input for false
    color and leave that input's COLOR off. The Keyer still cuts a key anywhere in a chain and puts
    its matte out as a picture. The Bus Mixer keeps Multiply, Screen, Difference and Max. The Video
    Mixer replaces none of them.
  - Not built: a SUM mode with a level for each input, a BORDER along the wipe's edge, and a hue CV
    for each input.
  - Presets: Four Colors and Wipe Clock.

### Outputs

- **Display**: shows a bus inside Rack. It has three video inputs, A, B and C.
  - The SEL button, or the right-click Input menu, picks which input is on screen. THRU repeats the
    selected input's voltage, so you can keep patching from it.
  - L and R are audio inputs for REC only. Patch a cable into L, R or both before you press REC and
    the movie gets a stereo sound track (AAC): ±5 V is full level, anything louder is clipped. One
    cable alone is heard on both sides. Nothing patched means no sound track, as before. The sound
    sets the movie's clock, so picture and sound stay together. If you change Rack's audio sample rate
    during a recording, the recording stops and the file is kept up to that point. A paused Rack
    pauses the movie. Syphon carries pictures only, so for sound alongside Syphon send Rack's audio
    out through your audio interface or a loopback device.
  - SHOT saves a PNG. REC records an H.264 .mov, and a second press stops it. Files go to
    `~/Pictures/Video Research Unit` unless you pick another folder in the right-click menu. REC
    needs macOS.
  - The right-click Panel width menu offers 12, 16, 20 and 24 HP, or full height, the default.
- **Syphon Out**: publishes a bus as a Syphon server that other Mac apps can pick up. Frames are
  4:3, either 1024 x 768 or 1440 x 1080, chosen in the right-click menu. The server name is
  `VRU – <label or module id>`.

### CV utilities

- **Octoscillator**: eight chaotic CV channels with three outputs each (X, Y and Z), 24 in all.
  SPEED changes their overall pace; SPREAD distributes their rates. RELATE moves from separate
  trajectories to a shared movement. Each channel has RATE, DEPTH and OFFSET. LEVEL adjusts all
  depths; RANGE selects +/-5 V, 0–5 V or +/-1 V before offset. The outputs are ordinary Rack CV:
  patch them to position, rotation, colour, keying and mixer controls. They carry no video bus.
  Start with small DEPTH settings. Very fast CV works spatially only on destinations with scan-time
  capture (such as Deflection Yoke); frame-sampled controls can alias.
- **Dual LFO Chaos**: slow cyclic and chaotic modulation. The outputs are A, B, X, Y, S&H and CLK.
- **1V CV Out**: scales Rack CV into 0 to 1 V, plus or minus 1 V, 0 to 5 V or plus or minus 5 V,
  for DC-coupled, Expert Sleepers-style hardware outputs. It has eight lanes, each with scale,
  offset, slew and invert, plus a calibration output and a safe mute. It only scales CV.

## How a patch fits together

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

To tell video jacks from CV jacks:

- Video jacks are labeled BUS, BUS IN, BUS A, BUS B and MATTE BUS. Display has inputs A, B and C
  and output THRU. Scan Processor has outputs BUS 1 and BUS 2. Video Mixer has the VIDEO IN inputs
  A, B, C and D, then PATTERN, KEY and MATTE, and its output BUS. Every other jack is CV or audio.
  The Video Mixer's LIVE outputs are gates.
- Black plates mark outputs, video and CV alike. Inputs sit on the bare panel, except Scope Trace's
  X IN and Y IN, which sit in outlined plates.
- Processors, Bus Mixer and Syphon Out have a green SIG light. It is lit while the input bus has a
  picture, so a dark light means the input points at an empty bus. Video Mixer's is lit while at least
  one of A to D has a picture. Scope Trace has one too. It is
  dim until X IN or Y IN is patched.

## Why the modules work together

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

Nothing draws the picture until something needs to show it. Display and Syphon Out do that. Each
reads the recipe for its bus, follows the recipes back to the source, and draws the whole chain in
one pass. Here is a small chain:

```
bus 3   Video Oscillator  makes a grid
bus 4   Deflection Yoke   bends bus 3
bus 5   Colorizer         colors bus 4
```

The cable into Display A carries 5 V. Display finds bus 5, 4 and 3, and draws all three.

A few modules also keep a frame in memory. Freeze Frame holds one, Feedback keeps the previous one,
and Video Player and Syphon In keep frames that arrive from outside Rack.

### Why VRU is one plugin

The bus table lives in the memory of one plugin. Rack loads every plugin on its own, and each keeps
its own data. There is no supported way for one plugin to read another plugin's table.

A module from another plugin can see the voltage on a VRU video cable. It cannot see the
table, so it cannot find the picture. All it gets is a steady voltage.

That is why every module that makes or changes a picture ships in this one plugin. They share one
table. Split them across two plugins and each plugin would build its own table. A cable between them
would carry a bus number that the other side could not look up.

The cable still does a cable's job. It routes, and Rack saves it with the patch.

## Patching rules

- Video jacks connect VRU to VRU. Other plugins see only a steady voltage.
- Any change to a video cable's voltage changes which bus it points at. An attenuator, a VCA, an
  offset, a slew limiter and a mixer all do that. Half of 3 V is 1.5 V, which points somewhere else.
- One output can feed many inputs. CV jacks are ordinary, so any Rack module can drive them. The
  VRU CV outputs work with any module too: Octoscillator, Dual LFO Chaos, 1V CV Out, and SWEEP and SYNC on
  Scope Trace.
- A patch can hold 64 modules that own a bus. That is every module except Display, Syphon Out,
  Octoscillator, Dual LFO Chaos and 1V CV Out. The 65th gets no bus and never retries. It still runs and sends 0 V,
  but its picture cannot be shown. Delete a module to free a bus, then delete and re-add the 65th.
- A processor with nothing patched on its input shows bus 1. Display A and Syphon Out do the same.
  Bus 1 is usually the first video module you added, so a forgotten cable can hide behind a picture
  that looks right. Bus Mixer is an exception: an empty BUS B or MATTE BUS means absent. So is Video
  Mixer, for every picture input. Display B and C show NO VIDEO until patched.
- A chain can run 17 modules, one source and 16 processors. A longer chain, or a cable that loops
  back on itself, holds the last good picture, and Display writes CYCLE over it. To feed a picture
  back on purpose, use Feedback. It keeps its own memory of the last frame and needs no loop cable.

## Install

You need a `.vcvplugin` file. Building from source makes one, as described below.

1. Quit Rack.
2. Copy `VideoResearchUnit-<version>-mac-arm64.vcvplugin` into `plugins-mac-arm64` in Rack's user folder.
   On a Mac that is `~/Library/Application Support/Rack2`. Help > Open user folder opens it.
3. Start Rack. It unpacks the file. The modules appear in the module browser under
   "Video Research Unit", named VRU–01 to VRU–22.

The plugin's id is `VideoResearchUnit`. It is in the package file name and the plugin folder.

Requirements: VCV Rack 2, an Apple silicon Mac and macOS 12 or later. Windows, Linux and Intel Macs
are untested.

Syphon In, Syphon Out, the Video Player decoder and Display REC use macOS frameworks. In builds
without them they are stand-ins that do nothing, and the Syphon modules report NO SYPHON. Syphon
works inside Rack, both in a loop from Syphon Out to Syphon In and in the system server list. It
has not yet been checked with OBS or Resolume.

## Old patches

Version 2.1.0 added the Focus module, gives the Video Player L and R audio outputs carrying the clip's own sound, stops the Display flashing NO VIDEO now and then while the Video Player
loops, and makes the Display's REC light tooltip show how long the recording has run. It also gave two modules new internal names: Wobbulator is now DeflectionYoke and RuttEtra is now ScanProcessor.
Panels, names and every parameter are unchanged, but a patch saved with 2.0.0 that uses the Deflection Yoke or the Scan Processor will not find them until it is migrated.
Run `python3 scripts/migrate-slug.py --dry-run` to see what it would change, then run it without `--dry-run` while Rack is closed. It backs up every file
it touches to `~/VRU-migration-backups/` first, with a restore script. Patches that use neither module are unaffected.

Three recent changes to Scope Trace affect patches saved before them.

- DWELL did not exist. A patch saved before DWELL loads it at 0, the even trace that patch was made
  with. A new Scope Trace starts at DWELL 0.5.
- ROTATE wraps around instead of stopping at plus or minus 180 degrees. A patch whose rotation knob plus
  CV stayed inside the range draws the same. A patch that drove it past the end now keeps turning.
- BEAM is now a true Gaussian width, so an old patch draws wider, softer lines. Where strokes
  overlap, under a long DECAY and with BLOOM, it can also look brighter overall.

`docs/scope-trace-audit.md`, section 8, has the numbers. `docs/architecture.md` lists every
behavior change under "Behaviour changes for old patches".

## Building from source

You need the Rack SDK that matches your Rack version and the Xcode command line tools.

```sh
./scripts/download-rack-sdk.sh 2.6.6     # use the version that matches your Rack
export RACK_DIR="$PWD/Rack-SDK"
make
make dist                                # writes dist/VideoResearchUnit-<version>-mac-arm64.vcvplugin
make install                             # builds, then copies the plugin into Rack's plugins folder
make test                                # bus and Video Player tests, Display capture smoke test, Syphon round trip
make bench                               # render timing benchmarks
```

Syphon is linked by default on Apple silicon Macs. `Frameworks/RLSyphon.framework` is a copy of
Syphon-Framework with every symbol renamed to an `RLSyphon` prefix, so it cannot clash with another
Syphon copy in Rack. `scripts/build-syphon-framework.sh` rebuilds it from the pinned commit and
needs python3. To build without Syphon, run `RASTERLAB_SYPHON=0 make`.

If a build fails with a tapi error such as "unknown architecture", try setting
`DEVELOPER_DIR=/Library/Developer/CommandLineTools`.

## For developers

- Rack calls `process()` at audio rate. Keep that path to reading CV, updating atomics and writing
  ordinary voltages. Drawing belongs on the UI side.
- The bus table has 64 nodes, in `src/video_bus.hpp` and `src/video_bus.cpp`. Each owner publishes
  its node, plain data, from `process()`. `src/video_graph_renderer.cpp` resolves a bus by walking
  source nodes back to a source, up to 16 levels deep, and returns the last good frame on a cycle.
- Display renders in `draw()`. Syphon Out renders in `drawFramebuffer()` and publishes the frame as
  an OpenGL texture. Syphon In and Video Player write incoming frames into external frame slots.

Where to read more:

- `docs/architecture.md`: how the bus model, the renderer and each module work, and why.
- `docs/syphon.md`: notes on the Syphon bridge and the renamed framework.
- `docs/VIDEO-BUS.md`: how the VRU Video Bus works and what other plugin developers can and cannot do with it. Ships inside the .vcvplugin.
- `docs/module-ideas.md`: candidate modules.
- `AGENTS.md`: a short guide for contributors and coding agents.

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
