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

## Current Modules

- `VRU–01 Deflection Yoke`: consumes a video bus token, publishes raster-bent output on its own allocated video bus. Deflection is audio-rate and sampled at each pixel's scan time (docs/architecture.md, "Scan-time signal model").
- `VRU–02 Scan Processor`: consumes a video bus token, publishes luma-lifted scanline output on the shared CRT beam layer (see docs/architecture.md, "Scan Processor"). Deflection, blank and beam Z are audio-rate and sampled at each point's scan time.
- `VRU–03 Scope Trace`: captures audio/CV at full rate into a signal ring (see docs/architecture.md, "Scan-time signal model") and publishes oscilloscope/Lissajous with phosphor persistence on its own allocated video bus. It is 20 HP, with CV jacks and attenuverters for scale, position, rotation, hue, timebase and Z, a trigger level in volts, and two outputs besides the bus: SWEEP (a -5 to +5 V ramp over one window) and SYNC (a 10 V pulse at each sweep start, 1 ms, or half a window if that is shorter). Param, input and output ids are append-only (see docs/architecture.md, "Scope Trace controls and CV").
- `VRU–04 Line Field`: consumes a video bus token, publishes scanline-heightfield output
  on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–05 Video Oscillator`: publishes generated video/test-pattern state on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–06 Ramps`: publishes two-layer grayscale shape state on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–07 Video Player`: plays a local video file or YouTube source and publishes its frames on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–08 Syphon In`: receives frames through the optional macOS Syphon bridge and publishes an external frame on its own allocated video bus.
- `VRU–09 Color Lab`: consumes a video bus token and publishes color-mutated output on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–10 Colorizer`: consumes a video bus token and publishes a four-stop luma-to-colour gradient on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–11 Wave Shaper`: consumes a video bus token and publishes folded/quantized output on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–12 Keyer`: consumes a video bus token and publishes luma matte/color/fill output on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–13 Bus Mixer`: consumes two video bus tokens plus optional matte bus and publishes mixed output on its own allocated video bus (see docs/architecture.md, "Video bus model").
- `VRU–14 Feedback`: consumes a video bus token and publishes feedback output on its own allocated video bus; frame copy and the feedback step happen in the renderer, not `process()`.
- `VRU–15 Freeze Frame`: consumes a video bus token and publishes held/strobed frame output on its own allocated video bus (see docs/architecture.md, "Video bus model"); frame copy happens in the renderer, not `process()`.
- `VRU–16 Display`: selects one of three video bus inputs (A, B, C) with SEL or the Input submenu, renders its resolved graph, and repeats its token on THRU. L and R are audio inputs for REC only: with a cable in either, the recorded `.mov` gets a stereo AAC track (docs/architecture.md, "Audio for REC"). The selected input light is green for an active bus and red for no video. Unpatched A reads bus 1; unpatched B/C means no video.
- `VRU–17 Syphon Out`: resolves a video bus, uploads it to an OpenGL texture, and publishes through the optional macOS Syphon bridge.
- `VRU–18 Dual LFO Chaos`: CV-only modulation utility; it does not publish a video bus.
- `VRU–19 1V CV Out`: scales ordinary Rack CV into `0-1V`, `+/-1V`, `0-5V`, or `+/-5V` video-synth ranges for routing through VCV Audio to DC-coupled Expert Sleepers-style outputs. It does not generate video-rate RGB.
- `VRU–20 Rotator`: consumes a video bus token and publishes a rotated, zoomed, folded picture on its own allocated bus; spin and notch are accumulated in `process()` (`rotator_math.hpp`), the pixel work is in the renderer.
- `VRU–21 Shapes`: a source with no bus input; it draws one closed shape (SIDES 3 to 12 blended without steps, ROUND, POINT, SIZE, X, Y, ASPECT, TURN, REPEAT tiles, COPIES ring, MELT) as a grey picture of distance from the edge (mid grey on the edge, brighter inside, darker outside, SPREAD sets the ramp), or white on black with MATTE, on its own allocated bus. `process()` only publishes the controls (`shapes_math.hpp`); the per-pixel distance field is `shapes_field.hpp`, called from the renderer. Tests are in `tests/shapes_test.cpp` (`make test` runs them).
- `VRU–22 Video Mixer`: takes up to four pictures (A to D) plus optional PATTERN, KEY and MATTE buses and publishes one picture on its own allocated bus. Which input is live, which is next and how far the lever has come up is decided in `process()` (`video_mixer_math.hpp`, `VideoMixerState`); the tint, dissolve/add/wipe and key maths is in the renderer (`renderVideoMixerFrame`). Its 30 HP panel is laid out in docs/architecture.md, "Video Mixer".

- `VRU–23 Octoscillator`: CV-only eight-channel Lorenz motion source with 24 mono XYZ outputs, shared/local relationship blend, local rate/depth/offset and global speed/spread/level/range. No video bus.
- `VRU–24 Focus`: one bus in, one bus out. FOCUS (all-direction soften, up to 6% of the width), SMEAR (horizontal, mostly trailing, up to 15%), SHARP (unsharp mask), CONVERGE (red and blue slid apart, up to 3%) and MIX, each with a CV jack and a CV AMT trimmer. The knobs ride the bus as `FocusParams` (node type `Focus`); the pixel work is `renderFocusFrame` in the renderer using the running-sum filters in `src/focus_filter.hpp`. Zero effect swaps the source through. Tested by `tests/focus_test.cpp`.

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
- processor nodes from `Line Field`, `Color Lab`, `Colorizer`, `Keyer`, `Bus Mixer`, `Wave Shaper`, `Freeze Frame`, `Feedback`, `Rotator`, and `Video Mixer`
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

## Release State

- Source version: `2.1.0` (24 modules, VRU–01 to VRU–24). It adds Focus, gives the Video Player L and R audio outputs, makes the Video Player and Display hold the last picture, and renames two internal module slugs (Wobbulator to DeflectionYoke, RuttEtra to ScanProcessor; `scripts/migrate-slug.py` migrates patches saved with 2.0.0). The first public release was `2.0.0` (23 modules). Rack 2 requires a plugin's major version to be 2.
- Build with `DEVELOPER_DIR=/Library/Developer/CommandLineTools` if the selected Xcode toolchain fails.

## Next Good Moves

1. Live-check Syphon Out and In in Rack and OBS or Resolume (`Frameworks/RLSyphon.framework` is vendored and prefixed; see `docs/syphon.md`).
2. Add a UI for the Syphon Out server label.
3. Move Deflection Yoke/Line Field/new processors from CPU frames toward shader-native texture processors.
4. Add CV attenuverters and CV inputs for more color controls.
5. Move the Feedback processor toward shader-native rendering.

