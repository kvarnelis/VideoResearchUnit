# Video Research Unit: Contributor and Agent Guide

## Project

Video Research Unit (VRU) is a VCV Rack 2 plugin for CV-controlled video
synthesis and raster processing. It was called RasterLab before 2.8.0.

Treat this as a Rack-native visual instrument, not a software clone of any
hardware video synth product line. Avoid names and panel language taken from
commercial video synth makers, module replicas, and marketing claims.

## Naming

The plugin is "Video Research Unit" (VRU). Every module has a catalogue number. The Rack browser
shows `VRU–01 Deflection Yoke`. The panel header shows `VIDEO RESEARCH UNIT`, a rule, `VRU–01`
and the title; Display is the one exception, with a one-line header so its picture keeps its full
size. The dash is an en dash (U+2013), never a hyphen. In C++ build it as
`std::string("VRU\xe2\x80\x93") + number` (`catalogueNumber()` in `src/panel_style.hpp`), because a
hex escape swallows the digit that follows it.

The plugin slug is `VideoResearchUnit`. Module slugs, class and file names, the param, input and
output ids and the `rasterlab_panel` namespace are as in the source.

| No. | Name | Slug |
| --- | --- | --- |
| VRU–01 | Deflection Yoke | `DeflectionYoke` |
| VRU–02 | Scan Processor | `ScanProcessor` |
| VRU–03 | Scope Trace | `ScopeTrace` |
| VRU–04 | Line Field | `LineField` |
| VRU–05 | Video Oscillator | `VideoOsc` |
| VRU–06 | Ramps | `Ramps` |
| VRU–07 | Video Player | `VideoPlayer` |
| VRU–08 | Syphon In | `SyphonIn` |
| VRU–09 | Color Lab | `ColorLab` |
| VRU–10 | Colorizer | `Colorizer` |
| VRU–11 | Wave Shaper | `WaveShaper` |
| VRU–12 | Keyer | `Keyer` |
| VRU–13 | Bus Mixer | `BusMixer` |
| VRU–14 | Feedback | `Feedback` |
| VRU–15 | Freeze Frame | `FreezeFrame` |
| VRU–16 | Display | `Display` |
| VRU–17 | Syphon Out | `SyphonOut` |
| VRU–18 | Dual LFO Chaos | `DualLfoChaos` |
| VRU–19 | 1V CV Out | `OneVoltCvOut` |
| VRU–20 | Rotator | `Rotator` |
| VRU–21 | Shapes | `Shapes` |
| VRU–22 | Video Mixer | `VideoMixer` |
| VRU–23 | Octoscillator | `Octoscillator` |
| VRU–24 | Focus | `Focus` |
| VRU–25 | Vector Osc | `VectorOsc` |
| VRU–26 | Video Probe | `VideoProbe` |
| VRU–27 | Frame Delay | `FrameDelay` |
| VRU–28 | Scan Sync | `ScanSync` |
| VRU–29 | Raster | `Raster` |

## Current Modules

- `VRU–01 Deflection Yoke`: consumes a video bus token, publishes raster-bent output on its own allocated video bus. Deflection is audio-rate and sampled at each pixel's scan time (docs/architecture.md, "Scan-time signal model").
- `VRU–02 Scan Processor`: consumes a video bus token, publishes luma-lifted scanline output on the shared CRT beam layer (see docs/architecture.md, "Scan Processor"). Deflection, blank and beam Z are audio-rate and sampled at each point's scan time. LIFT IN (the last input id, `ScanProcessorParams::liftBusId`) is an optional second picture whose luma lifts the lines; the brightness and colour still come from BUS IN, and with no cable BUS IN lifts itself, pixel for pixel as before.
- `VRU–03 Scope Trace`: captures audio/CV at full rate into a signal ring (see docs/architecture.md, "Scan-time signal model") and publishes oscilloscope/Lissajous with phosphor persistence on its own allocated video bus. It is 20 HP, with CV jacks and attenuverters for scale, position, rotation, hue, timebase and Z, a trigger level in volts, and two outputs besides the bus: SWEEP (a -5 to +5 V ramp over one window) and SYNC (a 10 V pulse at each sweep start, 1 ms, or half a window if that is shorter). Param, input and output ids are append-only (see docs/architecture.md, "Scope Trace controls and CV").
- `VRU–04 Line Field`: consumes a video bus token, publishes scanline-heightfield output
  on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–05 Video Oscillator`: publishes generated video/test-pattern state on its own allocated video bus (see docs/architecture.md, "Video bus model"). In Bars and Rows, LOCK A and LOCK B are 0 to 1 holds (0 free, 1 locked to the scan; the old switch ids), and IN A, IN B and the BEND CV are read at each pixel's scan time from the module's own signal ring (docs/architecture.md, "Bars and Rows"). FM IN (the last input id, `VideoOsc::FM_INPUT`) and FM AMT (the last param id, `FM_AMOUNT_PARAM`, -1 to 1, default 0) take a second picture that frequency-modulates oscillator A (`VideoParams::fmBusId`, `fmAmount`; docs/architecture.md, "Picture as modulation"). Unpatched or at zero, the picture is byte for byte as before. Since 2.4.0 it also has oscillator C (`FREQ_C_PARAM`, `WAVE_C_PARAM`, `RANGE_C_PARAM`, `LOCK_C_PARAM`, `LEVEL_C_PARAM`, the params after `FM_AMOUNT_PARAM`; `FREQ_C_INPUT`, `LOCK_C_INPUT`, `IN_C_INPUT`, `VOCT_C_INPUT`; LEVEL C defaults to 0, which leaves the picture byte for byte as before), V/OCT inputs for A, B and C (`VOCT_A_INPUT`, `VOCT_B_INPUT` after `FM_INPUT`), sound outputs SOUND A, B and C (`AUDIO_A_OUTPUT` to `AUDIO_C_OUTPUT`, `src/video_osc_audio.hpp`; silent above Nyquist, never folded) and a Snow position on PATTERN (7). The panel is 29 HP, laid out as data in `src/video_osc_layout.hpp` (three identical oscillator rows A, B, C; a jack table with columns A, B, C); `scripts/check-video-osc-panel.py` checks it for overlaps and draws `docs/video-osc-panel-layout.png` (`make test` runs the check). The last param id is now `LEVEL_C_PARAM`, the last input id `VOCT_C_INPUT` (docs/architecture.md, "2.4.0 additions to the Video Oscillator").
- `VRU–06 Ramps`: publishes two-layer grayscale shape state on its own allocated video bus (see docs/architecture.md, "Video bus model"). MOD IN (`Ramps::MOD_INPUT`, the last input id) and AMT (`MOD_AMOUNT_PARAM`, the last param id, -1 to 1, default 0) take a picture whose brightness offsets layer A's phase at each pixel (`RampsParams::modBusId`, `modAmount`; "Picture as modulation").
- `VRU–07 Video Player`: plays a local video file or YouTube source and publishes its frames on its own allocated video bus (see docs/architecture.md, "Video bus model"). Since 2.1.0 it also has L and R audio outputs carrying the clip's own soundtrack (docs/architecture.md, "Video Player").
- `VRU–08 Syphon In`: receives frames through the optional macOS Syphon bridge and publishes an external frame on its own allocated video bus.
- `VRU–09 Color Lab`: consumes a video bus token and publishes color-mutated output on its own allocated video bus (see docs/architecture.md, "Video bus model"). MOD IN (`ColorLab::MOD_INPUT`, the last input id) and AMT (`MOD_AMOUNT_PARAM`, the last param id, -1 to 1, default 0) take a picture whose brightness shifts the hue at each pixel (`ColorLabParams::modBusId`, `modAmount`; "Picture as modulation").
- `VRU–10 Colorizer`: consumes a video bus token and publishes a four-stop luma-to-colour gradient on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–11 Wave Shaper`: consumes a video bus token and publishes folded/quantized output on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–12 Keyer`: consumes a video bus token and publishes luma matte/color/fill output on its own allocated video bus (see docs/architecture.md, "Video bus model"). MOD IN (`Keyer::MOD_INPUT`, the last input id) and AMT (`MOD_AMOUNT_PARAM`, the last param id, -1 to 1, default 0) take a picture whose brightness moves the threshold at each pixel (`KeyerParams::modBusId`, `modAmount`; "Picture as modulation").
- `VRU–13 Bus Mixer`: consumes two video bus tokens plus optional matte bus and publishes mixed output on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–14 Feedback`: consumes a video bus token and publishes feedback output on its own allocated video bus; frame copy and the feedback step happen in the renderer, not `process()`. Its history is the 8-bit picture in the frame ring, which rounds each pass and so leaves a trail stuck at a grey (about 10, 25, 50 at gain 0.95, 0.98, 0.99). The right-click option "Fade trails fully to black" (`FeedbackParams::fadeToBlack`, saved as `fadeTrailsToBlack`) keeps a float32 copy beside it in the ring (`FrameRingFrame::rgbaF`) so trails reach black. New module: on. Saved data without the key, or no data at all (`Feedback::fromJson`): off, byte for byte the old picture. Initialize: on. With the plugin setting "Full precision between modules" on, the history is a float-only ring entry for that render whatever this option says (docs/architecture.md, "Precision"). Tests: `tests/feedback_fade_test.cpp`, `tests/feedback_module_test.cpp`.
- `VRU–15 Freeze Frame`: consumes a video bus token and publishes held/strobed frame output on its own allocated video bus (see docs/architecture.md, "Video bus model"); frame copy happens in the renderer, not `process()`.
- `VRU–16 Display`: selects one of three video bus inputs (A, B, C) with SEL or the Input submenu, renders its resolved graph, and repeats its token on THRU. L and R are audio inputs for REC only: with a cable in either, the recorded `.mov` gets a stereo AAC track (docs/architecture.md, "Audio for REC"). The selected input light is green for an active bus and red for no video. Unpatched A reads bus 1; unpatched B/C means no video. Since 2.1.0 it holds its last picture when a bus read is interrupted instead of flashing NO VIDEO, and its SHOT and REC light tooltips say what happened (REC shows elapsed recording time). Its right-click menu has the plugin-wide check item "Keep video running when hidden" (default on, stored in Rack's settings as `keepRunningHidden`): the render service then keeps every registered picture advancing, and SHOT and REC take their pictures from the service, while the Display is scrolled out of view; off, behaviour is as in 2.2.0 (docs/architecture.md, "Render service"). It also has the plugin-wide check item "Render video on its own thread" (default off, stored as `renderOnOwnThread`): the render service then draws all registered pictures on a thread of its own at 60 a second, and Display, Syphon Out and Video Probe only take the newest finished picture. `vru-render-patch --thread-mode` drives that tick synchronously. It also has the plugin-wide check item "Full precision between modules" (default off, stored as `fullPrecision`): the generators, the pointwise nodes, Frame Delay, Freeze Frame, Rotator, Feedback, Deflection Yoke, Line Field, Focus and the beam layer's output then pass float pictures (`VideoFrame`, `src/video_frame.hpp`) and a picture is rounded to 8 bits once per root where it leaves the graph; the Scan Processor's picture and its LIFT IN are read unrounded with the setting on (as bytes with it off), so a smooth float ramp lifts the lines smoothly; external frames and Video Probe stay 8-bit, and a Frame Delay stores 8-bit pictures above a 256 MiB float budget; with the setting off every picture is byte for byte as before (docs/architecture.md, "Precision"; `vru-render-patch --precision`; `tests/precision_test.cpp`).
- `VRU–17 Syphon Out`: resolves a video bus, uploads it to an OpenGL texture, and publishes through the optional macOS Syphon bridge.
- `VRU–18 Dual LFO Chaos`: CV-only modulation utility; it does not publish a video bus.
- `VRU–19 1V CV Out`: scales ordinary Rack CV into `0-1V`, `+/-1V`, `0-5V`, or `+/-5V` video-synth ranges for routing through VCV Audio to DC-coupled Expert Sleepers-style outputs. It does not generate video-rate RGB.
- `VRU–20 Rotator`: consumes a video bus token and publishes a rotated, zoomed, folded picture on its own allocated bus; spin and notch are accumulated in `process()` (`rotator_math.hpp`), the pixel work is in the renderer.
- `VRU–21 Shapes`: a source with no bus input; it draws one closed shape (SIDES 3 to 12 blended without steps, ROUND, POINT, SIZE, X, Y, ASPECT, TURN, REPEAT tiles, COPIES ring, MELT) as a grey picture of distance from the edge (mid grey on the edge, brighter inside, darker outside, SPREAD sets the ramp), or white on black with MATTE, on its own allocated bus. `process()` only publishes the controls (`shapes_math.hpp`); the per-pixel distance field is `shapes_field.hpp`, called from the renderer. Tests are in `tests/shapes_test.cpp` (`make test` runs them).
- `VRU–22 Video Mixer`: takes up to four pictures (A to D) plus optional PATTERN, KEY and MATTE buses and publishes one picture on its own allocated bus. Which input is live, which is next and how far the lever has come up is decided in `process()` (`video_mixer_math.hpp`, `VideoMixerState`); the tint, dissolve/add/wipe and key maths is in the renderer (`renderVideoMixerFrame`). Each input's COLOR is Off, Hue or Gradient (param 0, 1, 2); Gradient builds a four-stop table with the Colorizer's `gradientTable` and one shared SPAN knob. Its 30 HP panel is laid out in docs/architecture.md, "Video Mixer". The ALL latch (param 29, light last) overrides MODE: while on, the mode is 3, every patched picture at its LEVEL (params 25 to 28) is summed and clamped, or averaged with the saved `allAverages` menu item; the renderer handles it at the top of `renderVideoMixerNode` and in `renderVideoMixerFrame`, and MODE, FADER and NEXT do nothing there.

- `VRU–23 Octoscillator`: CV-only eight-channel Lorenz motion source with 24 mono XYZ outputs, shared/local relationship blend, local rate/depth/offset and global speed/spread/level/range. No video bus.
- `VRU–24 Focus`: one bus in, one bus out. FOCUS (all-direction soften, up to 6% of the width), SMEAR (horizontal, mostly trailing, up to 15%), SHARP (unsharp mask), CONVERGE (red and blue slid apart, up to 3%) and MIX, each with a CV jack and a CV AMT trimmer. The knobs ride the bus as `FocusParams` (node type `Focus`); the pixel work is `renderFocusFrame` in the renderer using the running-sum filters in `src/focus_filter.hpp`. Zero effect swaps the source through. Tested by `tests/focus_test.cpp`.

- `VRU–25 Vector Osc`: CV and audio source for Scope Trace's X IN and Y IN; no video bus. 12 HP. FREQ (+ V/OCT), RATIO (snap list 1:4 to 9:1, plus a RATIO step jack), FINE, PHASE, VOICES (1 to 8), DETUNE, WAVE (sine, triangle, saw, square), Y WAVE (offset of Y's shape), LEVEL; DETUNE, WAVE and PHASE have CV jacks; SYNC restarts the phases. Outputs X, Y (+/-5 V) and Z (0 to 10 V, beam brightness following inverse beam speed). The maths is `src/vector_osc_math.hpp`; tested by `tests/vector_osc_test.cpp`.

- `VRU–26 Video Probe`: picture to CV, the reverse of every other module. BUS IN, X and Y jacks with knobs and AMT trimmers (+/-5 V spans the picture), SIZE, SMOOTH, LEVEL. Outputs LUMA, R, G, B (0 to 10 V, at the read point, per audio sample, bilinear or box-averaged), then AVG, MOTION and GATE (per picture), and FRAME (the last output id: a 10 V, 1 ms pulse each time a new picture reaches `process()`; LUMA, R, G and B change at or after it). 20 HP. The widget gets the bus at 320 x 240 from the render service and builds summed-area tables (`src/video_probe_math.hpp`); a lock-free triple buffer hands them to `process()`. The widget registers from `step()`, which Rack calls every UI frame whether or not the widget is drawn, so the outputs keep updating when the module is scrolled out of view (60 pictures a second, or 30 when nothing at all is drawn). They hold their last values when the bus has no picture, when the module is bypassed, and when the plugin setting "Keep video running when hidden" is off (then only `draw()` feeds `process()`, as in 2.2.0). Whether they update with Rack's window minimised depends on whether Rack keeps calling `step()`; this is unchecked. Tested by `tests/video_probe_test.cpp`; four presets in `presets/VideoProbe/`. Since 2.4.0 it also has SCAN (`SCAN_PARAM`, switch, default 0) and LINES (`LINES_PARAM`, 16 to 600, default 240), the last two param ids: with SCAN on, an axis whose jack has no cable follows the video clock, so with X and Y both open the picture is read as a raster, one cell per audio sample (`video_probe::sampleScan`, `src/raster_scan.hpp`, docs/architecture.md, "Raster rule"); SCAN off is byte for byte the old read. This is the scanner half of the picture <-> signal pair; Raster (VRU–29) is the other half. SLOW is param 9 (appended); Scan Sync's SLOW is param 1. Panel from `src/video_probe_layout.hpp`, checked by `scripts/check-panel.py`.

- `VRU–27 Frame Delay`: one bus in, one bus out, the input as it was FRAMES ticks ago (1 to 30; a tick is 1/60 s of audio time, the counter Feedback uses). It is the one node the renderer treats as a leaf, so a patch loop that goes through it is legal and does not count toward the 16 levels; a loop with no delay is still a cycle. The renderer draws its source separately, after the root, and stores it (docs/architecture.md, "Frame Delay"). 7 HP: FRAMES (snap knob), CV AMT trimmer, CV jack, BUS IN, SIG, BUS. Black until a picture that old is stored; the bus clears when the source goes (NO VIDEO downstream, as Feedback). Module reset clears the ring. `src/FrameDelay.cpp`; tests in `tests/frame_delay_test.cpp`; three presets in `presets/FrameDelay/`; demo patch `7 - Loop Through A Delay.vcv`.

- `VRU–28 Scan Sync`: no video bus. Pulses and ramps from the video clock (`src/video_clock.hpp`, docs/architecture.md, "Video clock"): TOP (10 V, 1 ms at the top of each picture), DOWN (0 to 10 V down the picture, held through the flyback), LINE (a pulse at each line start) and ACROSS (0 to 10 V across each line), with a LINES knob (120 to 600) that must match the scan module being driven, and a SLOW knob (param 1, x1 to x128, default x1 = sample-identical to before) that makes a picture K fields long (`vclock::Clock(rate, lines, K)`; TOP once per slow picture, DOWN over all of it, LINE and ACROSS on the slow line). The line outputs are coarse (a line is about three samples). No reset input, on purpose (see "Video clock", RESET). The outputs are a pure function of `args.frame` (`src/scan_sync_math.hpp`). The clock also owns the Deflection Yoke's and Scan Processor's field timing: both push with `signalRingPushAt(slot, samples, args.frame)` and the renderer reads `signalRingReadField()`, a field of fractional length, so a tone at a whole multiple of 59.94006 Hz stands still at any sample rate. The Video Oscillator's scan inputs, Snow tick and field latch use it too (`PairClock::advanceAt`); its SOUND outputs stay free-running. Tests: `tests/video_clock_test.cpp`, `tests/scan_sync_module_test.cpp`; `make tsan` covers the ring.
- `VRU–29 Raster`: signals to a picture, the way back from Video Probe's SCAN. No bus input; BUS out. 10 HP: LINES (16 to 600, snap), FILL (HOLD or JOIN), inputs BRIGHT, X, Y, R, G, B (ids in that order), output BUS. Each audio sample is one place in the picture from the video clock (`src/raster_scan.hpp`, docs/architecture.md, "Raster rule"); `process()` pushes R, G, B (or X, Y and grey when X or Y has a cable) as volts / 10 into its signal ring with `signalRingPushAt` and publishes a `RasterParams` node (`VideoBusNodeType::Raster`); the renderer reads the newest field (`signalRingReadField`) and `raster_scan::render` paints it. About 3 samples a line at 48 kHz at SLOW x1; SLOW (param 2, x1 to x128, `vclock::Clock(rate, lines, K)`) makes a picture K fields long and paints it a slice at a time (docs/architecture.md, "Raster rule"). Panel from `src/raster_layout.hpp`; `scripts/check-panel.py` checks it (`make test`) and draws `docs/raster-panel-layout.png`. Tests: `tests/raster_scan_test.cpp` (`make test`; `make tsan` runs its thread part). Demo patch `9 - A Picture Through A Signal.vcv`.

Each video module owns a bus id (1-64) allocated per instance; cables carry the id as voltage. Unpatched processor inputs read bus 1.

Patch test:

```text
Video Oscillator BUS -> Deflection Yoke BUS IN
Deflection Yoke BUS OUT -> Line Field BUS IN
Line Field BUS OUT -> Display A
Scope Trace BUS -> Display A
Any BUS -> Syphon Out BUS IN
```

## Architecture

Rack cables carry normal voltages only. VRU uses those voltages as video
bus tokens.

The plugin-global video bus currently stores small graph nodes:

- generator nodes from `Video Oscillator`, `Ramps`, and `Shapes`
- processor nodes from `Deflection Yoke`
- processor nodes from `Line Field`, `Color Lab`, `Colorizer`, `Keyer`, `Bus Mixer`, `Wave Shaper`, `Freeze Frame`, `Feedback`, `Frame Delay`, `Rotator`, `Focus`, and `Video Mixer`
- external frame nodes from `Syphon In`
- oscilloscope/Lissajous source nodes from `Scope Trace`

`Display` resolves the graph with `VideoGraphRenderer` and renders a CPU-backed
preview frame. `Syphon Out` uses `OpenGlVideoTexture` to upload that resolved
frame into an OpenGL texture before publishing through the bridge.

Keep `Module::process()` lightweight. It should read CV, update atomics/bus
state, emit Rack voltages, and avoid allocations, locks, video decoding, Syphon
IO, or GPU work.

## Build

Use a Rack SDK matching the installed VCV Rack 2 version.

Development so far used Rack SDK `2.6.6`:

```sh
https://vcvrack.com/downloads/Rack-SDK-2.6.6-mac-arm64.zip
```

Expected local setup:

```sh
export RACK_DIR=/path/to/Rack-SDK
make
make dist
```

The local workspace may also keep `Rack-SDK/` in the project root. The SDK,
`build/`, `dist/`, and `plugin.dylib` are intentionally ignored by git.

## Seeing a patch without Rack

Agents cannot look at Rack, so a patch they write is easy to judge blind. `build/vru-render-patch` renders
what each Display in a `.vcv` patch would show. Build it with `make build/vru-render-patch` (macOS, needs
`RACK_DIR`), then run:

```sh
export DYLD_LIBRARY_PATH="$RACK_DIR"   # libRack.dylib is found there
build/vru-render-patch PATCH.vcv --out DIR [--seconds 2] [--frames 4] [--size 640x480] [--seed 1] [--fps 60] [--thread-mode]
```

It reads plain JSON patches and zstd tar patches (the second needs `zstd` and `tar` on the PATH). It builds
the real VRU module classes from `src/*.cpp`, loads each module's saved params and data, connects the
cables and runs every `process()` at 48 kHz, so it cannot drift from the plugin. The plugin's render service (the one the Display uses)
then draws the selected input of every Display, `--fps` times a second, and the tool writes `--frames` PNGs at
evenly spaced times to `DIR/display-<module id>-frame-<n>-<ms>ms.png`. It also writes `DIR/report.txt`, the
same text it prints. The report says, for every module, whether it ran for real, was a stand-in, or was
unsupported. Read it before you trust a picture. The same patch, options and seed give byte-identical PNGs. With `--thread-mode` the service's thread-mode tick is driven synchronously (each Display registers, one tick renders, the Display reads); the PNGs are identical to the default mode.
`tests/render_patch_test.py` checks this and runs in `make test`.

Limits:

- Real: every VRU module except the four below. Video Probe runs but its outputs stay 0 V, because its
  sampler is fed by the widget's `step()`, and this tool builds no widgets.
- Video Player and Syphon In have no source offline. They draw a labelled colour-bar test card.
  Syphon Out is not built; it has no output a Display could show.
- Other plugins cannot run. Fundamental LFO, VCO, Noise, VCA and 8vert have stand-ins, listed in the report.
  LFO and VCO read their saved FREQ (and FINE) and make plain waves; their CV inputs are ignored, except the
  VCO's V/OCT. Noise makes new seeded gaussian noise at every sample (white, pink, red, violet, blue,
  approximate gray and black), at an assumed level. The VCA and 8vert port orders and Noise output order
  are from memory of Fundamental, not from its source. Anything else outputs 0 V and is reported as
  unsupported. Core modules are ignored.
- Cables carry one channel. Poly cables are not modelled.
- Time is simulated: a Feedback or phosphor picture depends on `--fps`. The default is 60, which is about
  what Rack's UI draws, and it is slow at large sizes. A Display still renders at its own size, not the
  size on screen.

## Release State

- Source version: `2.4.0`. It adds Scan Sync (VRU–28) and Raster (VRU–29, 29 modules in all), and Video Probe's SCAN and LINES (a picture read as a raster and painted back, docs/architecture.md, "Raster rule") and a video clock that gives the Deflection Yoke, Scan Processor and Video Oscillator scan inputs one shared exact field (docs/architecture.md, "Video clock"). It also adds a third oscillator, V/OCT inputs, sound outputs and a Snow pattern to the Video Oscillator, and widens its panel to 29 HP. Version 2.3.0 added these. Video Probe (VRU–26) turns a picture into CV, the first module to do that. It has a FRAME output, a 10 V, 1 ms pulse for each new picture. The Scan Processor has a LIFT IN bus input. The Octoscillator has five presets. Frame Delay (VRU–27) holds a picture for a number of frames, and a picture can loop back through it. A picture that feeds several modules is drawn once per pass instead of once per module. Display, Syphon Out and Video Probe take their pictures from one shared render service, with two Display menu settings: "Keep video running when hidden" (on by default) and "Render video on its own thread" (off by default). Feedback has a "Fade trails fully to black" setting. The Display menu setting "Full precision between modules" (off by default) passes pictures between modules as floating point and rounds to 8 bits once, where a picture leaves the graph. Three modules take a picture as a modulation input: FM IN on the Video Oscillator, and MOD IN on Ramps, Keyer and Color Lab. The public bus API has version 2, with a float read, a tick, and the native size of a source. The offline tool vru-render-patch draws what a Display in a patch would show, without Rack. Version 2.2.0 added Vector Osc, gives the Video Oscillator LOCK hold knobs with CV and IN A and IN B inputs, and gives the Video Mixer a Gradient colour state with a SPAN knob. Version 2.1.0 added Focus, gave the Video Player L and R audio outputs, makes the Video Player and Display hold the last picture instead of flashing NO VIDEO, and makes the Display's REC light tooltip show elapsed recording time. Version 2.1.0 added Focus, gave the Video Player L and R audio outputs, made the Video Player and Display hold the last picture, and renamed two internal module slugs (Wobbulator to DeflectionYoke, RuttEtra to ScanProcessor; `scripts/migrate-slug.py` migrates patches saved with 2.0.0). The first public release was `2.0.0` (23 modules). Rack 2 requires a plugin's major version to be 2.
- Build with `DEVELOPER_DIR=/Library/Developer/CommandLineTools` if the selected Xcode toolchain fails.

- Public bus API: `include/vru_bus.h` (MIT) only grows. Version 2 is version 1 plus appended entries (`src/vru_bus_api.cpp`); `vruBusGetApi(1)` must stay bit for bit as it was, checked by `tests/vru_bus_v1_header_test.cpp` against the frozen `tests/data/vru_bus_v1.h`. See `docs/VIDEO-BUS.md`, "Version 2".

## Next Good Moves

1. Live-check Syphon Out and In in Rack and OBS or Resolume (`Frameworks/RLSyphon.framework` is vendored and prefixed; see `docs/syphon.md`).
2. Add a UI for the Syphon Out server label.
3. Move Deflection Yoke/Line Field/new processors from CPU frames toward shader-native texture processors.
4. Add CV attenuverters and CV inputs for more color controls.
5. Move the Feedback processor toward shader-native rendering.

