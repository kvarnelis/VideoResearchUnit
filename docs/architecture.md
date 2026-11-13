# Architecture

## Names

The plugin is Video Research Unit (VRU), version 2.0.0, with 23 modules. It was called RasterLab before
2.8.0. Every module has a catalogue number, VRU–01 to VRU–22, and Rack's module browser shows
`VRU–01 Deflection Yoke`. The table is in `AGENTS.md`. The plugin slug is `VideoResearchUnit`.

This document names modules by their Rack names. The code names are `DeflectionYoke` for VRU–01 Deflection
Yoke and `ScanProcessor` for VRU–02 Scan Processor.

## Rack boundary

Rack patch cables carry voltages. VRU keeps video frames outside Rack's
native cable payload and treats Rack as:

- the module UI host,
- the CV/audio modulation source,
- the patch persistence layer,
- the transport/clock context.

## Threading model

The audio engine calls `Module::process()` at sample rate. It must not run video
decoding, Syphon IO, GPU command submission, allocation-heavy work, or locks that
can block for a visible amount of time.

The intended production model is:

1. `process()` samples CV into atomics or a lock-free state buffer.
2. A render thread consumes the latest state at 30/60 fps.
3. The render thread owns video textures and framebuffers.
4. The Rack UI preview samples a downscaled texture or shared preview frame.

The prototype currently renders a display frame in the UI draw path. `Syphon In`
and `Syphon Out` also do their frame copy/upload work from tiny Rack
`OpenGlWidget` children so the audio thread remains limited to bus-token state.

## Syphon plan

The macOS implementation should use Syphon for inter-app texture sharing.

- `Syphon In`: receives external server frames into an external-frame store and
  publishes a bus token.
- `Syphon Out`: resolves a bus, renders it at 4:3 (1024 x 768 or 1440 x 1080),
  uploads it to an OpenGL texture, and publishes it through the Syphon bridge.
  The server is named `VRU – <module id or label>`.
- Bridge: `src/syphon_bridge_mac.mm` talks to a vendored, prefixed copy of
  Syphon-Framework (`Frameworks/RLSyphon.framework`). The plugin links it through
  `@rpath` with an `@loader_path/Frameworks` rpath. See `docs/syphon.md`.
- Camera capture should not be a v1 dependency inside Rack because macOS camera
  permission strings live in the host app bundle, not in a Rack plugin.

## Video bus plan

For multi-module video patching, a video module output publishes a small graph
node into a plugin-global registry and emits a normal Rack voltage token.

A downstream video module input reads the token and resolves the corresponding
node in the registry. `Video Oscillator`, `Ramps` and `Shapes` publish generator nodes. `Deflection Yoke`, `Scan Processor`, `Line Field`, `Color Lab`, `Colorizer`,
`Keyer`, `Wave Shaper`, `Freeze Frame`, `Feedback`, and `Rotator` publish processor nodes that point to
their source bus and store processor state. `Bus Mixer` publishes a processor
node that points to bus A plus secondary bus tokens for bus B and an optional
matte bus. `Video Mixer` publishes a processor node whose params hold seven buses (the four pictures,
pattern, key and matte) and has no source bus of its own. `Freeze Frame` uses a render-side frame
store keyed by a module-owned slot; `process()` only publishes the latest capture counter and controls.

This is intentionally a private protocol. It allows Rack-looking patches, but
generic Rack modules should not be expected to process those token voltages.

## 1V CV bridge

`1V CV Out` is a voltage-scaling utility, not a video-bus processor. It emits
ordinary Rack CV in one-volt video-synth ranges so users can route signals through
VCV Rack's Audio module to DC-coupled hardware outputs such as Expert Sleepers
interfaces. It must not be described as full video-rate RGB output; audio/CV
interfaces do not provide the MHz bandwidth expected by patchable video.

## Display panel

- The screen is a 4:3 landscape picture, like a classic Sony Trinitron. It has small rounded corners and a thin dark bezel (1 mm). The picture is 141.7 x 106.3 mm at full height (it was 144 x 108 mm before the panel text grew; see "Panel text and layout").
- Layout: a thin title strip above (one line; see "Panel text and layout"), the screen centred in the band between the title and the bottom strip, and a bottom strip that holds, at one pitch `p` centred on the panel, `[SHOT] A B C SEL THRU [L R] [REC]` (L and R are the audio jacks, see "Audio for REC"). The letters and their 2 mm lights sit below the jacks on one centreline at 125.8 mm, 0.4 mm under the THRU label centre (125.4 mm) so they clear the jacks; SEL, L and R use that same line. The bottom strip jacks sit at 119.8 mm so the THRU plate (jack + 7.6 mm) stays inside the panel. The width decides what fits on the strip:
  - 24 and 30 HP: all nine on the strip row. `p = (bezel half-width - 12.82) / 4` (12.82 = the 4.45 mm label offset plus the 8.37 mm SHOT label), which puts the SHOT and REC labels' outer edges on the bezel: 11.53 mm at 24 HP, 14.76 mm at 30 HP. A is the first jack after SHOT and SEL is on the panel's centre line.
  - 16 and 20 HP: `A B C SEL THRU L R` on the strip row, `p = (bezel half-width - 4.6) / 3`, which puts the THRU plate's outer edge (the R jack's, on the right) near the bezel: 11.35 mm at 16 HP, 14.73 mm at 20 HP. SHOT and REC move to the row above (y 108.6). (At 20 HP they used to share the strip row; nine items there would be 9.2 mm apart, closer than a jack is wide.)
  - 12 HP: only `A B C SEL THRU` fit the strip (`p = (bezel half-width - 4.6) / 2 = 11.94 mm`). SHOT and REC are on the row above and L and R on a third row above that (y 99.0, x 7 mm either side of the centre, the letter on the outer side of each jack), where the small picture leaves room.
- Panel width is chosen from the context menu and saved as `panelHp` in patch JSON: 12, 16, 20, 24 or 30 HP. The default is 30 HP, the narrowest whole HP at which the full-height 4:3 screen fits with at least 3 mm side margins (152.4 mm). On narrower panels the screen is width-limited and centred vertically, so there is equal space above and below. Saved widths that are no longer offered (for example 32 or 48) load at the nearest choice.
- The render size is 4:3. Rows = on-screen height in device pixels (zoom times the larger of pixelRatio and windowRatio), clamped to 480..1080; width = rows x 4/3, capped at 2048. At 2x on the default panel that is 837 x 628 (screen 106.3 mm tall at 75 px/in).
- Ports: A, B and C take video bus tokens. L and R take audio for REC only. THRU repeats the selected input voltage, so a Display can sit mid-chain. When the selected input is unpatched, THRU outputs 0 V. The checks are `testDisplayInputs()` and `testThruVoltage()` in `tests/bus_test.cpp`.
- Existing param, port and light IDs are unchanged. A is input 0, the former `VIDEO_BUS_INPUT`, now `BUS_A_INPUT`. `BUS_B_INPUT` and `BUS_C_INPUT`, `SEL_PARAM`, and the consecutive green/red pairs `SEL_A_LIGHT` / `SEL_A_LIGHT_RED`, `SEL_B_LIGHT` / `SEL_B_LIGHT_RED`, `SEL_C_LIGHT` / `SEL_C_LIGHT_RED` are appended before their respective `*_LEN`. `SIGNAL_LIGHT` keeps its retired ID, with no widget or updates. `AUDIO_L_INPUT` and `AUDIO_R_INPUT` were appended after `BUS_C_INPUT` (ids 3 and 4), so cables in saved patches keep their ids. Their tooltips read "Audio left" and "Audio right, record only".
- SEL cycles A, B, C, A on the audio thread, so THRU follows in the same step. The selected input light is green when its bus is active and red when there is no video; unselected lights are off. Unpatched A still reads bus 1; unpatched B or C reads no bus and shows NO VIDEO. Patch JSON saves `selectedInput` as 0, 1 or 2; missing or invalid values load as A. The Input submenu beside Panel width also selects A, B or C. The pure rules live in `src/display_select.hpp`.
- The widget box is sized in Rack grid units: `Vec(hp * RACK_GRID_WIDTH, RACK_GRID_HEIGHT)`. Never size a ModuleWidget from millimetres. Rack's `RackWidget::addModule` throws unless the height is exactly 380 px, and the uncaught throw aborts Rack. `mm2px(128.5 mm)` is 379.43 px, and that crashed Rack on every Display added from the module browser (2026-11-07). Loading a patch skips the check, so saved patches hid the bug.

### Display capture: SHOT and REC

- SHOT saves the frame the Display shows as a PNG. REC starts a recording on the first press and ends it on the second (H.264 in a `.mov`, with an AAC audio track if the L or R jack is patched, see "Audio for REC").
- **No dialog on any button press, ever.** Names are automatic: `vru-YYYY-MM-DD_HH-MM-SS.png` / `.mov` (local time), with `-2`, `-3`, ... added when the name exists. Files are created exclusively, so nothing is overwritten.
- Folder: `~/Pictures/Video Research Unit` by default (`~/Pictures/RasterLab` before 2.8.0; files saved there stay where they are), created on the first save. It is deliberately not Desktop, Documents or Downloads, where macOS asks for permission. It is also used when the chosen folder no longer exists. The right-click menu is the only place a folder picker opens ("Choose folder...", `osdialog_file(OSDIALOG_OPEN_DIR, ...)` from the menu action on the UI thread); it also has "Show folder" and "Use default folder". The chosen folder is saved per module (`captureFolder` in `dataToJson`) and as the default for new Displays (`settingsToJson` / `settingsFromJson` in `src/plugin.cpp`), so it lasts across patches and restarts.
- Failures show no dialog: a `WARN` line in the Rack log, and the SHOT light flashes red (REC: its light flickers red and the badge reads "REC FAILED" for 4 s) instead of the normal blink.
- Indicator: the REC light breathes while recording, and a small "REC" badge sits at the top left of the screen. The badge is drawn with NanoVG after the picture has been read back, so it is never in a screenshot or a movie.
- Button positions (mm from the panel's left edge). Each light is centred on its button. The SHOT label is right-aligned 4.45 mm left of the SHOT button and the REC label is left-aligned 4.45 mm right of the REC button, both in `LABEL_MM` text. Below 24 HP there is no room beside the jacks, so the pair sits in its own row above the strip (y 108.6); from 24 HP it sits on the strip row (y 119.8), one each side of the A / B / C / SEL / THRU / L / R group, four pitches from the panel centre with each label's outer edge on the bezel. Jack and button x, with the audio jacks:

  | HP | SHOT x | REC x | SHOT/REC y | A x | SEL x | THRU x | L x | R x | L/R y |
  |---|---|---|---|---|---|---|---|---|---|
  | 12 | 23.48 | 37.48 | 108.6 | 6.60 | 42.42 | 54.36 | 23.48 | 37.48 | 99.0 |
  | 16 | 33.64 | 47.64 | 108.6 | 6.60 | 40.64 | 51.99 | 63.33 | 74.68 | 119.8 |
  | 20 | 43.80 | 57.80 | 108.6 | 6.60 | 50.80 | 65.53 | 80.27 | 95.00 | 119.8 |
  | 24 | 14.82 | 107.10 | 119.8 | 26.36 | 60.96 | 72.49 | 84.03 | 95.56 | 119.8 |
  | 30 | 17.17 | 135.23 | 119.8 | 31.93 | 76.20 | 90.96 | 105.71 | 120.47 | 119.8 |

  These come from the layout code in `applyPanelHp` and have not been checked by eye in Rack (2026-11-12, no Rack session was used for this change). B and C are at A + p and A + 2p. The panel height is still exactly `RACK_GRID_HEIGHT`; the HP choices are unchanged.

### Audio for REC

- Two jacks, L (`AUDIO_L_INPUT`) and R (`AUDIO_R_INPUT`), feed REC and nothing else. They do not change the picture, THRU or Syphon.
- A recording gets a stereo audio track if a cable is in L or R **at the moment the first frame is taken**. Both patched: L and R. Only L: L on both channels. Only R: R on both. Neither: no audio track, the movie is the same as before (`routeAudio()` in `src/audio_ring.hpp`). A cable pulled during a recording gives silence on its channel(s) (the mapping is applied per sample, so L alone then becomes L on both only if it is the one still patched). A cable added after the recording started is ignored for that movie.
- Level: Rack's +-5 V is +-1.0 full scale (`voltsToSample`, volts x 0.2). Beyond that the sample is clipped hard at +-1.0 (no soft clip). A NaN becomes silence. A polyphonic cable is summed.
- Format: AAC, 192 kbit/s stereo, in the QuickTime `.mov`, the format the container and every player handle cleanly. The sample rate is the engine rate when the recording starts (8 to 96 kHz as it is). AAC stops at 96 kHz, so a faster engine is written at 48 kHz (44.1 kHz for 44.1 kHz multiples) and AVFoundation converts the rate. If this Mac refused AAC, the track would be 16-bit PCM instead (`audioSettingsFor` in `src/display_recorder_mac.mm`); it has not been needed.
- Real time: `process()` never locks, allocates or does I/O. Each call stores which jacks are patched and the engine rate in atomics and, only while a recording with sound runs, pushes one stereo frame into `display_recorder::AudioRing` (`src/audio_ring.hpp`), a single-producer single-consumer ring of 2^17 frames (2.7 s at 48 kHz, 1 MB) built once, on the UI thread, when the first recording with sound starts and kept until the module is deleted. A full ring drops the incoming frame and counts it; it never waits. The recorder's worker thread is the one consumer. `claim()` hands the ring to a new session and discards what is queued; a session that was replaced reads nothing, and its pop is a compare-and-swap, so a late reader cannot move the tail back.
- Sync: the audio sample count is the master clock. The worker throws away anything queued before the encoder opened (the timeline starts there), then counts every frame it takes from the ring. Picture number n is written when that count reaches n / 30 s and carries that time as its stamp, so sound and picture are always on the same timeline and cannot drift apart over a long recording. Audio is written contiguously (each chunk starts where the last ended). The movie is never shorter than its audio. The remaining error is the audio thread's lead over the screen, up to one audio buffer.
  - Overflow (the worker stalled for more than 2.7 s): the lost frames are replaced by silence of the same length, placed after what was queued, and counted in `SessionStats::audioDropped`.
  - Engine paused (or the Display bypassed): no samples arrive, so the clock stands still and the movie pauses with it; pictures are not written. When the engine runs again the movie goes on from the same point.
  - Engine sample rate changed during the recording (Rack's audio settings): the recording **stops** at that point. The file is closed and complete, the Display logs a warning, and REC lights off. Pressing REC again starts a new movie at the new rate.
  - A recording without audio keeps the old wall-clock timeline.
- Syphon carries pictures only; it has no audio channel and Syphon Out is unchanged. To get sound with a Syphon picture, send Rack's audio out through the audio interface or a loopback device and capture that in the receiving app.
- Tests (`tests/display_capture_smoke.mm`): the voltage scaling and clipping, routing, the ring (order, overflow count, ownership); then real movies: a 440 Hz sine on L and a 1000 Hz sine at half the level on R for 2 s at 48 kHz, read back with AVAssetReader as one two-channel AAC track whose duration is within one picture of the audio and whose channels differ as fed; the same at 44.1 kHz and at 192 kHz; a pause in the middle; a sample rate change in the middle; and a recording with nothing patched, which has no audio track.

- Threading: for capture, `process()` only counts SHOT and REC presses. The UI thread acts on them and reads the picture back in `draw()`, right after the renderer returns its CPU RGBA buffer; the only work there is one memory copy. PNG encoding and the file write run on `display_capture::ShotWriter`'s thread (at most 3 queued; a press beyond that gets the red cue). A recording is a `display_recorder::Session` with its own worker thread, fed through a small mailbox (newest frame wins), so the UI thread never waits for the encoder or the disk. A bypassed Display ignores the buttons because Rack does not call its `process()`. A Display that is not being drawn (off screen) cannot capture; the press fails with the red cue after 1 s.
- PNG: `stb_image_write.h` (v0.92) from the Rack SDK's `dep/include`, included only in `src/display_png.cpp`, inside an anonymous namespace, with its extern declarations switched off. Only `stbi_write_png_to_mem()` is used, under a mutex; the file is written by our own code.
- Video: `src/display_recorder_mac.mm` uses AVFoundation (`AVAssetWriter` plus a pixel buffer adaptor): H.264 (High profile, BT.709) in a QuickTime `.mov`. Other platforms build `src/display_recorder_stub.cpp`, where REC reports "unavailable". Frames go on a constant 30 fps timeline that starts when the encoder opens: the newest frame at each tick, or the previous one again when the picture is static. If the encoder falls behind, the missed ticks are skipped and the earlier frame stays on screen longer; nothing blocks. The size is fixed by the first frame (rounded down to even). A later frame of another size is scaled to it (cropped when exactly one pixel larger).
- The file is closed on the second REC press, when the module is deleted, when the patch closes and when Rack quits (`~Display()` waits up to 15 s). Recording state is never saved in a patch, so loading one never starts a recording. A Rack crash during a recording leaves an unplayable file. The recorded movie is not affected by the Syphon and screen paths.
- `tests/display_capture_smoke.mm` (`make test`, macOS) writes a real PNG and a real `.mov` and reads both back with ImageIO and AVAssetReader.

## Panel text and layout

Decision (2026-11-08): panel text was too small to read, so every size in `src/panel_style.hpp` went up
about 1.8x, to match VCV Fundamental's Scope (capitals about 2.0 mm tall). The face is still Rack's
Nunito-Bold. Sizes are NanoVG sizes in mm, and capital height is 0.561 x size.

- Module title 4.9 (cap 2.75), series line VIDEO RESEARCH UNIT 2.8 (1.57), catalogue number VRU–NN 3.4 (1.91),
  control and jack labels 3.8 (2.13), toggle position words 3.2 (1.80), output plate text 3.4 (1.91),
  Syphon readout headline 4.7. Before, title, brand and labels were 2.7, 1.7 and 2.1. The brand line
  that the series line replaced was 3.1 (1.74); see "Panel header" below.
- `nvgFontSize` is the font's line height, 1.255 x the em, so a text is (advance sum in em) x size / 1.255
  wide. Everything is measured for Nunito-Bold, so panel text, the Syphon readouts and the placeholder
  text on Video Oscillator and Display call `useLabelFont` first (they used NanoVG's default face before).
  `fitToWidth` cuts a readout line with "..." where it would run past its screen.
- A label is centred above its control. `labelOffset(r)` is the control radius + 1.4 mm clear + half the
  capital height, with r = `R_KNOB` 4.8, `R_SMALL` 3.84, `R_TRIM` 3.03, `R_SWITCH` 3.5, `R_JACK` 4.0 or
  `R_BUTTON` 3.05. A row of mixed controls gives every label the largest radius, so the row shares one
  baseline. A text containing `\n` stacks lines 3.4 mm apart, the last one nearest the control.
- Rows use `rowX(W, n, k, pitch)` with a pitch wider than 10.16 mm where labels need it: neighbours fit
  when (w1 + w2) / 2 + 1.2 mm <= pitch. Full words are used where they fit, abbreviations only in dense rows.
- `addToggleLabels` puts a word at each lever position (a CKSS at 1 is lever up, so the first word names
  1): beside the lever (side +1 or -1).
- Plates. An output sits on a filled black plate with a reversed label (`addPlatedOutput`). A module's
  main signal inputs may sit in an outlined plate instead, label inside (`addPlatedInput`; Scope Trace
  X IN and Y IN): the same 9.2 x 11.8 mm footprint and label position, no fill, a 0.35 mm dark outline
  (`PLATE_OUTLINE_MM`) and dark label text at the same size. The outline is stroked on a path inset by
  half its width, so its outer edge is where an output plate's edge is. Filled plates stay outputs only.
- Every label box was computed from the Nunito-Bold advance widths. None overlaps another text or a knob,
  jack, switch, light, plate, screen or panel edge (clearances: text to text 1.2 mm, to a control 0.7 mm,
  to the edge 0.9 mm). No panel got wider, no height changed, and the param, input, output and light IDs
  and config calls are as before, except that `ScanProcessor` later gained a second bus output, appended after the first,
  and four CV inputs (INTENSITY, H POS, V POS, Z), appended after BLANK.
- Display: the screen is 141.7 x 106.3 mm (was 144 x 108) and the bottom strip is at y 119.8 mm (was
  121.5), so the larger strip labels and the THRU plate fit. 30 HP is still the default width.
- Deflection Yoke (2026-11-09): the panel went from 12 HP (60.96 mm) to 20 HP (101.6 mm) and is still 128.5 mm
  tall. `res/DeflectionYoke.svg` (`width="101.6mm" height="128.5mm"`, viewBox 101.6 x 128.5) and
  `DeflectionYokeWidget` (`W = 101.6`) agree on both numbers, and the widget takes its size from the SVG as it
  did before, so the height Rack sees is unchanged. A patch saved with the old panel keeps the module's
  position, so the wider module can overlap its right-hand neighbours until it is moved.
  Layout: a hairline at x 50.8 mm splits the panel. The left block has one column per coil (H, V, S) and an
  ALL column, with rows for amount, rate, wave, and phase (the ALL column holds DEPTH, FINE, PHASE, and
  RESET). The right block has four titled bands (RASTER, SHAPE, TUBE, VIDEO) of four columns each. Every old
  control is still on the panel, each knob has its CV jack directly below it, and only the jacks that take
  something other than plain CV carry a label (V/OCT, IN, RESET, BUS IN); tooltips name the rest. Clearances
  from the Nunito-Bold advance widths: text to text 1.30 mm, text to a control 1.10 mm, text to the edge
  2.52 mm.
- Video Mixer (2026-11-11). 30 HP, 46 controls, three titled zones and an OUTPUT strip; the layout and the
  clearance check are described under "Video Mixer" in the video bus section. Its zone titles are
  `VideoMixerZoneTitle` widgets, not SVG text, and its labels use the shared helpers, so the label rules
  above hold. Its horizontal clearances were checked on the rendered panel, not from advance widths.
- Panel header (2026-11-11, the Video Research Unit rename). Every panel except Display opens with four
  lines, drawn in code by `addPanelHeader` in `src/panel_style.hpp` (centre y, then size in mm): the series
  line VIDEO RESEARCH UNIT at 3.8 (2.8, colour 90 90 88), a rule at 6.0 (0.15 mm stroke, colour 27 27 27,
  from `ruleInsetMm` in on each side, 4 mm by default), the catalogue number VRU–NN at 8.45 (3.4) and the
  module title at 12.3 (4.9; Dual LFO Chaos 4.5). The dash is an en dash (U+2013). In C++ `catalogueNumber()`
  builds it, because a hex escape swallows the digit that follows it. The hairline rule at y 15.4 stays in
  each `res/*.svg`. The series line is 25.6 mm wide, so it fits the narrowest panel (7 HP, 35.56 mm).
  Nothing else on those panels starts above about y 18.
- Display header (2026-11-11). Display keeps its whole picture (141.7 x 106.3 mm, top at y 8.0), so it has a
  one-line header and no rule: DISPLAY centred at y 4.6 (4.9 mm), VIDEO RESEARCH UNIT (2.8 mm, colour
  90 90 88) with its left edge on the bezel's left edge at y 5.18, and VRU–16 (3.4 mm) with its right edge
  on the bezel's right edge at y 5.02. The three share one baseline. The series line is hidden when its right
  end would come within 3 mm of the title's left end, which happens at 12 HP and not from 16 HP up. The
  number always shows. Measured Nunito-Bold widths: series line 25.63 mm (a named constant in
  `src/Display.cpp`; remeasure it if the series size changes), DISPLAY 16.7 mm, VRU–16 10.33 mm. Both side
  texts are placed from the screen's real edges, again whenever the width changes.

## Video bus model

This section describes the bus as it is after the stage 1 correctness pass. Code:
`src/video_bus.{hpp,cpp}`, `src/video_graph_renderer.{hpp,cpp}`,
`src/video_slot_allocator.hpp`, the slot stores, and the modules that publish.

### Per-instance bus ids

- Each module instance with a video output owns one bus id in `1..64`
  (`kVideoBusCount`). `VideoBusOwner` claims it in the constructor and releases
  it in the destructor (which also clears the bus).
- The lowest free id is taken first. Bus 1 is therefore normally the first
  video module created in a patch.
- The output cable carries the id as its voltage (id 3 = 3 V). Consumers convert
  voltage back with `videoBusIdFromVoltage()`. Anything that is not an integer
  id from 1 to 64 (0 V, NaN, above 64) means "no bus".
- The id is saved as `busId` in patch JSON. On load, `loadVideoBusId()` claims
  the saved id if it is free. If another live module already holds it, the
  module takes a fresh free id. Two modules loaded with the same saved id never
  share a bus. Cables carry live ids, so a load-time reassignment never breaks
  a cable.
- Old patches store the id as `outputBusVoltage` (or `busVoltage` for Video Oscillator).
  That value is read as a preference when `busId` is absent. Old graphs keep
  working because the cables carry the live ids.
- Above 10 V, a bus voltage is only meaningful inside Rack. Video cables must
  not go to audio or CV outputs.

### Unpatched inputs

- An unpatched processor input reads bus 1 (legacy default). This is kept on
  purpose. Whichever module holds bus 1 is the default source.
- An unpatched Bus Mixer B or MAT BUS input means "absent", not bus 1.

### Snapshot publishing

- The audio thread publishes a whole `VideoBusNode` per bus with a seqlock. The
  writer makes the sequence odd, writes the payload, then makes it even again.
  Payload words are relaxed atomics, so the reader has no data race.
- Readers (UI thread) copy the node and retry if the sequence changed or was
  odd. A reader that keeps losing gives up for that read and treats the bus as
  empty for that frame.
- Each bus has one writer: the module that owns its id. `process()` only calls
  `videoBusPublish*()` or `videoBusClear()`. Those functions do not allocate.
- `VideoBusNode` must stay trivially copyable (checked by `static_assert`).

### Renderer recursion

- `VideoGraphRenderer::renderBus()` walks the graph from the requested bus down
  to its generators. Each node writes its result into the output frame that its
  caller passed in.
- Each depth has its own scratch frames. A single-input processor renders its
  source into `scratch(depth, 0)`. A Bus Mixer uses slots 0 (A), 1 (B) and 2
  (matte). A Video Mixer uses slots 0 (live), 1 (next), 2 (pattern), 3 (key) and 4 (matte), so
  `kSlotsPerDepth` is 5 (it was 3) and the pool is `(kMaxDepth + 2) * 5` frames. A nested render
  never overwrites a frame that a parent still needs.
- Results are swapped, not copied. The only copies are the generator and
  external frame copies, plus the freeze read.
- A generator reached twice in one pass (for example, A and B both from one
  Video Oscillator) is rendered once per pass and reused. Its feedback state therefore
  advances once per frame.
- Memory grows with the depth of the chain in use, not with the number of
  modules.

### Cycles and depth

- A bus that is already on the current walk, or a walk deeper than 16 levels,
  is a cycle. The renderer does not draw it.
- `renderBus()` then returns the last good frame for the same bus and size and
  sets `hasCycle()`. Display draws a `CYCLE` label. If there is no good frame,
  the result is empty.
- The modules also refuse to publish a processor whose input is its own output.

### Freeze Frame

- Each Freeze Frame owns a slot in the freeze store, allocated in the
  constructor and released in the destructor.
- Captures are latched on `captureCounter`. A frame is stored only when the
  counter is non-zero and differs from the last counter the slot handled. The
  module bumps the counter on a trigger edge and on each strobe period.
- CLEAR (while held) empties the slot and records the current counter as
  handled, so the cleared capture is not taken again. A new trigger captures
  again.
- Output: `hold * blend` crossfades live (0) into the frozen frame (1). With no
  capture yet, or hold 0, the live frame passes through.
- `captureCounter` is not saved in patch JSON. A loaded patch starts with no
  capture.
- Limitation: the store is shared by all renderers (Display and Syphon Out). The
  frame is stored at the size of the renderer that captured first. Another
  renderer at a different size resamples it. Moving captures to the
  full-resolution producer is future work.

### Frame ring

`src/frame_ring.{hpp,cpp}` provides shared render-side history for a module.
`frameRingAllocateSlot()` claims one of 64 slots (0 means none available);
`frameRingReleaseSlot()` drops its frames and releases it. `frameRingSetCapacity()`
clamps capacity to 1..600 and retains the newest frames that fit when shrinking.
`frameRingPush()` stores a new counter at age 0; `frameRingRead()` copies a frame
at age 0 (newest), 1 (previous), and so on. `frameRingCount()` reports the count.
`frameRingIsStored()` checks the newest counter. `frameRingClear()` handles each
clear counter once, emptying history until the next push.

The frame-counter guard makes the first writer for a tick win. Each entry keeps
its own width and height; another renderer resamples that stored picture rather
than choosing a new size for the same tick. Later ticks may be stored at another
size. Entries reuse their pixel vectors, and each slot has its own mutex so
unrelated modules do not wait on one shared lock.

Each stored frame costs width x height x 4 bytes: about 6.2 MB at 1440 x 1080,
about 373 MB for 60 frames at that size, and about 2.8 MB at 960 x 720.
Push/read/clear run on renderer (UI) threads under the slot mutex. Allocation,
release and capacity changes run on the construction/UI thread. None of this
API runs in `process()`.

### Rotator

VRU–20 Rotator is a single-input processor (node type `Rotator`, params in `RotatorParams`). The
renderer renders the source bus into a scratch frame, then inverse-maps it with `src/inverse_map.hpp`:
zoom 1/8..8 (2^(3v)), angle in turns, X/Y slide as frame fractions, and a pivot that moves the map
centre. The sampler gained a Mirror edge (triangle fold) beside Black, Clamp and Wrap; the module's EDGE
switch maps Black, Hold, Repeat, Mirror onto them. FOLDS (1 = off, 2..12) reflects each pixel into one
wedge about the fold centre before sampling. At defaults the renderer swaps the source into the output,
so the picture is byte-identical. Quarter turns snap cos and sin to exact values, so 90 and 180 degrees
are exact pixel moves. There is no perspective tilt.

`src/rotator_math.hpp` holds the audio-thread maths: `RotatorState` accumulates SPIN (2v|v| turns/s) and
NOTCH steps (1/16, 1/12, 1/8, 1/6, 1/4 turn, constant-speed GLIDE toward a target, RESET zeroes both) per
sample, and `rotatorAngle` adds knob, CV (10 V = one turn), spin and notch, then wraps in double to
[-0.5, 0.5) so a 0 to 10 V saw has no seam.

### Feedback

Feedback owns a frame ring with capacity 1. Each pass inverse-maps the previous
output through zoom (0.5..2, default 1), rotation (-0.5..0.5 turns), X/Y offsets
(-0.5..0.5 frame fractions), and Clamp, Wrap or Black edges (default Clamp).
The reusable header `src/inverse_map.hpp` computes the source position. Positive
rotation is counter-clockwise on screen. Hue rotates RGB per pass (-0.5..0.5
turns, default 0); the renderer builds its hue matrix once per frame.

ADD clamps input + gain x feedback. BLEND crossfades input and feedback with
gain capped at 1. Gain ranges 0..1.2, default 0.9; ADD is the default mode.
KEY (0..1, default 0) smoothly removes feedback below its luma threshold, with
a 0.1 transition below that threshold. At 0 it keeps everything. With no
previous frame, either mode starts from the input. CLEAR, a rising trigger or
button press, increments a counter; each event clears once and lights red for
0.2 seconds. All seven knobs have CV inputs adding voltage x 0.1 to the knob
value (ZOOM uses an exponent -1..1 and displays its factor).

`process()` publishes controls and a 60 Hz counter based on elapsed sample time;
it allocates no frames and takes no ring locks. Only rendered ticks produce a
step; skipped ticks are not replayed. Two renderers at the same tick see the
same stored step, with the second resampling if its size differs. Frame copy
and feedback computation run in the renderer. The slot and counters are not
saved in patch JSON; a loaded patch starts with empty history.

### Bus Mixer

- A = input bus, B = bus B, optional MAT BUS.
- B absent (unpatched, or its bus is empty): the output is A alone.
- Mix, Add, Multiply, Screen, Difference, Max: B blends into A by MIX, or by
  the MAT BUS luma when MATTE is above zero.
- Matte (6): the MAT BUS luma keys A (dark) into B (bright). INV MAT inverts
  the key. With no MAT BUS, Matte falls back to the MIX knob.

### Video Mixer

VRU–22 Video Mixer is a studio vision mixer in one module: four pictures, one live and one next, a
lever between them, a key layer over the result, and a fade to black last. Code: `src/VideoMixer.cpp`
(module and panel), `src/video_mixer_math.hpp` (the control logic, with no Rack types so the tests can
drive it), `VideoMixerParams` in `src/video_engine.hpp`, and `renderVideoMixerNode` and
`renderVideoMixerFrame` in `src/video_graph_renderer.cpp`. The node type is
`VideoBusNodeType::VideoMixer` (17), published by `videoBusPublishVideoMixer(busId, params)`; its
`sourceBusId` is 0 because all seven sources are in the params. Slug `VideoMixer`, 24 params, 17 inputs,
5 outputs and 19 lights. With no picture patched the module publishes nothing (`videoBusClear`).

- Pictures read. The node reads up to seven buses (A to D, PATTERN, KEY, MATTE) and draws at most five at
  once, in five roles that own scratch slots 0 to 4: live, next, pattern, key and matte. A role is
  rendered only when the frame uses it: live while the fader is below 1, next while it is above 0,
  pattern only part-way through a WIPE, key and matte only while the key layer is on. One bus in two roles
  (the next picture as its own pattern, one picture as both key and matte) is rendered once and shared. A
  cycle in any role makes the mixer's bus a cycle. An input that is unpatched, has nothing behind its
  token, or carries this mixer's own bus reads as black: it is absent, never bus 1. An empty MATTE means
  no matte, so the key reads its own picture.
- Control (`VideoMixerState`, run per sample on the audio thread; the renderer sees the result at its
  next frame, as a studio mixer cuts in the vertical interval). FADER plus its CV is a T-bar. Its position
  is how far the next picture has come up, counted from the end it last left, and `flipped` says which
  end is home. At the far end (within 0.005) the two pictures swap and home moves to that end, so a
  triangle LFO on FADER goes back and forth. CUT makes next live at once. AUTO runs the mix from where it
  stands to 1 over TIME, `0.02 * 1000^knob` seconds (0.02 s to 20 s; the default knob 0.57 is about a
  second). When a transition ends, next is the picture after the new one if ADVANCE is on and one is
  patched; otherwise it is the picture that was live, so two pictures trade places. A CUT or an AUTO
  take while the lever is away from home (more than 0.02) disarms it: the new picture stays whole until
  the lever is home again, so it never jumps to a part-way mix. NEXT steps next to the following patched
  input and skips the live one. The NEXT CV picks outright (0 to 10 V in four 2.5 V bands, A to D), and
  overrides stepping and the idle tidy. At rest, next is moved off an unpatched or live input whenever
  another is patched, never in mid-take. CUT, AUTO and NEXT are Schmitt triggers (0.1 V, 2 V) or their
  buttons, each processed every sample. KEY ON is its button or a gate of 1 V or more. The LIVE outputs
  send a gate (10 V) while their input is live, so sound can follow the picture; the lights are red for
  live and green for next. The patch saves `onAir` (the live input), `next`, `flipped` and `armed`, not a
  take in progress.
- CV laws. FADER, SAT, THRESH and BLACK add 0.1 per volt to the knob and clamp to 0..1. SHIFT adds 0.1
  turn per volt to every HUE (10 V is once round, as on the Rotator's ANGLE); the renderer wraps the hue.
  SOFT, KEY SOFT, TIME and KEY HUE have no CV jack.
- Picture maths (`renderVideoMixerFrame`), per pixel and in this order: tint, transition, key, black. The
  alpha is 255. Luma is Rec. 709 (0.2126, 0.7152, 0.0722).
  - Tint. With COLOR on, an input is read as its luma times `1 + (hue - 1) * SAT`, where `hue` is
    `hsv(HUE + SHIFT, 1, 1)` per channel. Black stays black, white becomes the full hue, and at SAT 0 the
    input is plain grey. With COLOR off the picture passes in its own colors and SAT leaves it alone.
  - Transition. With f the lever, `a` the live pixel and `n` the next one, f = 0 gives `a` and f = 1
    gives `n`, byte for byte in every mode. DISSOLVE is `a(1-f) + n f`. ADD is
    `min(1, a min(1, 2(1-f)) + n min(1, 2f))`, so both are at full strength at f = 0.5 and their colors
    add where they overlap. WIPE: `edge = (1 + SOFT/2) - (1 + SOFT) f`, `m = clamp((l - edge) / SOFT +
    0.5, 0, 1)` with `l` the PATTERN picture's luma, and the output is `a(1-m) + n m`. SOFT is clamped to
    0.001..1. With nothing in PATTERN, `l` is the next picture's own grey before it is tinted, or 0 when
    next is absent.
  - Key. The matte is MATTE's luma when MATTE is patched, else the KEY picture's. `m = clamp((luma -
    THRESH + KEY SOFT/2) / KEY SOFT, 0, 1)`, turned to `1 - m` by INV, with KEY SOFT clamped to
    0.001..1. The output is `v(1-m) + fill m`, where `fill` is the KEY picture, or with FILL on FLAT
    the flat color `hsv(KEY HUE, 0.9, 1)`. The layer does nothing when there is no matte source, or when
    a picture fill has no KEY picture. A flat fill with only MATTE patched works. The key sits after the
    transition and before BLACK, where a studio mixer has its downstream keyer, so cuts and wipes go on
    underneath it.
  - Black. `v(1 - BLACK)`, last of all.
- Wipe edge cases. With no PATTERN a wipe sweeps the whole grey range of the next picture, so a dim next
  picture shows nothing until the edge reaches its levels. With next absent and no PATTERN, `l` is 0: the
  old picture holds until f passes 1/(1 + SOFT) (about 0.87 at the default SOFT 0.15) and then fades to
  black.
- Panel. 30 HP (152.4 x 128.5 mm). The entry guessed 24 HP before any panel was drawn; its count of 46
  controls (12 knobs, 12 buttons and switches, 22 jacks) holds exactly and needs the width. Eleven columns
  12.7 mm apart, with 3 mm more after the fourth and the eighth, make three zones, each with a title and a
  hairline: VIDEO IN, TRANSITION and KEY LAYER, with OUTPUT along the foot. VIDEO IN: for each of A to D a
  jack with its status light beside it, a HUE knob and a COLOR latch; below a rule, SAT, SAT CV and SHIFT;
  below another, NEXT, its trigger jack and NEXT CV. TRANSITION: MODE (WIPE up, ADD in the middle,
  DISSOLVE down), PATTERN and SOFT along the top; the large FADER knob in the middle with CUT and AUTO,
  each with a trigger jack under it, to its right; FADER CV, ADVANCE and TIME along the foot. KEY LAYER:
  KEY, MATTE and KEY ON; THRESH, SOFT and GATE; THRESH CV, INV and KEY HUE; the FILL switch (KEY up, FLAT
  down); and under a rule BLACK and its CV. OUTPUT: four plated LIVE outputs, the SIG light (lit while
  any of A to D has a picture) and the plated BUS output. The zone titles are `VideoMixerZoneTitle`
  widgets that measure their word when they draw, so the hairline starts a fixed gap after it. No two
  labels touch; the closest pair, PATTERN and SOFT, is about 2 mm apart. That was read off the rendered
  panel (the module-browser picture), not computed from the font's advance widths as the older panels were.
- Not built, from the entry's "Later, if wanted": a SUM mode with a level for each input, a BORDER along
  the wipe's edge in the key's flat color, and a hue CV for each input.
  keys.
- Lineage. The television studio's vision mixer: a program bus and a preset bus that exchange their
  sources, the transition lever or T-bar, dissolves and pattern wipes, a key bus with a fill and a source
  that cuts it, a downstream keyer after the mix, fade to black as the last stage, and lights that show
  which source is live. The color plan of the early multi-camera video synthesizers: one color role to
  each camera, here one hue knob to each of four pictures. The gradient wipe of editing software.

### Colorizer

- Maps Rec. 709 luma through four hue and level stops at 0, 1/3, 2/3 and 1.
- A 256-entry table is built per frame. Hue takes the shortest path around the
  colour wheel; levels interpolate linearly and saturation is global.
- OFFSET shifts luma before lookup; only what the shift pushes past 1 wraps back to the bottom, so pure
  white (grey 255, luma exactly 1) at OFFSET 0 takes the top stop and not the bottom one. BANDS selects
  smooth or 2..8 band centres, and white sits in the top band. The source alpha passes through, and pixel
  work splits into row ranges.

### Ramps

- Two grayscale layers use horizontal, vertical, radial or angular coordinates.
  Each has frequency (0.25..16 cycles), wrapping phase and a wave morph from
  ramp through triangle and sine to soft square.
- MIX selects A, the average of A and B, centred A minus B, or A times B.
- Parameters are sampled per frame. The bus and 128 x 84 panel preview both
  call `renderRamps()`; it produces opaque grayscale pixels without render state.

### Bars and Rows

- Video Oscillator PATTERN 6 (VideoPattern 6). Two oscillators, A and B, run against a television scan.
  Each pixel row is one whole scanline (`floor((y + 0.5) * 240 / height)`, 0..239) at the 15734.264 Hz
  line rate of 262.5-line fields (`crt_beam.hpp`); `u = (x + 0.5) / width`. Rendered on the CPU per pixel
  by `renderPair()`.
- RANGE BARS counts cycles per line (`n`); ROWS counts cycles per field (`n / 262.5` per line).
  FREQ reads `0.5 x 2^(knob + 0.4 per volt)` cycles, clamped to 0.25..512; A uses FREQUENCY, B FREQ_B.
- LOCK locked starts every BARS line at phase 0 and runs ROWS on down the field (`cpl * (line + u)`).
  Free starts from the phase latched at the top of the field. On BARS in an odd field the start is
  half a line later (interlace). A free oscillator at a whole number of cycles per field holds still.
- `PairClock` (no Rack dependency) is advanced once per audio sample in `process()` in every pattern,
  in doubles. At each field boundary it latches both phases (taken back to the exact boundary) and
  flips `fieldOdd`; the latched phases and parity travel in the published `VideoParams`.
- WAVE 0..1: sine, triangle at 1/3, ramp at 2/3, square at 1. No edge is narrower than one pixel's
  phase footprint, and the level fades to mid grey as the footprint passes 0.3 to 0.5 of a cycle.
- BEND adds `bend x b` cycles to A's phase (b is B's level); BEND CV is 0.4 cycles per volt.
  MIX (A, A+B, A-B, AxB) and SPREAD (hue moves by SPREAD x B's share) give the colour.
- Parameters were added at the end of the lists, so older patches load unchanged. Patterns 0 to 5
  render the same pixels as before. Tests: `testPair*` in `tests/bus_test.cpp`.

### Shapes

- VRU–21 Shapes is a generator (node type `Shapes` = 16, params in `ShapesParams`). It has no bus input.
  `process()` turns the knobs and CV into `ShapesParams` with `shapes_math::shapesParams()` (no Rack
  types, so the tests call the same function) and publishes them with `videoBusPublishShapes()`. The
  renderer calls `renderShapes()`, which draws on the CPU per pixel on several threads and keeps no state.
  Control ranges and CV scales are in the header comment of `src/shapes_math.hpp`.
- The output is a distance picture: `v = clamp(0.5 - d / (2 L), 0, 1)` with `d` the signed distance to the
  edge (negative inside) and pixel value `lround(255 v)`. Mid grey is the edge, brighter is inside. `L`
  is SPREAD (`0.004 x 500^k` half picture heights, never under one pixel). MATTE sets `L` to half a pixel,
  which gives white on black with a one pixel edge. World units: y up, 1.0 is half the picture height.
- `src/shapes_field.hpp` is the kernel (header only, no Rack types). Polygons, stars and rounded shapes are
  exact distances, found by folding the plane into one wedge of the shape. POINT pulls each side's middle in to
  0.38 of the corner radius. A fractional SIDES value blends the distances of the two neighbouring polygons
  linearly, so the edge morphs smoothly but is not an exact distance between whole numbers of sides.
  ASPECT other than 1 stretches the shape (ROUND is applied first, then the stretch), and a stretched shape is
  measured against its outline as a polyline, with round corners as chords within about 0.1 px of the curve.
- COPIES places the copies on a ring of radius RING that turns with TURN. REPEAT tiles the whole composition on
  a lattice (a window of at most 7 x 7 cells, plus an early exit deep inside a shape). MELT joins every copy
  and cell with a soft union `d = -k ln(sum exp(-d_i / k))`, `k = 0.28 x MELT` composition units (one
  composition unit is half a cell when REPEAT is on). It does not depend on the order shapes arrive in, so
  the ring stays even and the lattice has no seams. Shapes too far away to matter are skipped.
- SPREAD is in picture units, so the halos of a fine grid overlap and the picture rises towards white. SIZE 0
  draws a point (a cone of distance). The distance inside a stretched shape has creases at the medial axis.
- A white plateau inside a Shapes picture goes through a Colorizer like any other grey: at OFFSET 0 pure
  white takes the palette's top stop, so the plateau keeps the top colour and the gradient runs unbroken
  from the outside colour to it.
- Cost at 960 x 540 on this machine: a rounded hexagon 0.7 ms; REPEAT 8 with COPIES 8, fractional SIDES,
  ASPECT 2, ROUND and MELT together 1 to 4 ms (about 100 ms without the table). A scene of at least 30000
  pixels that would take more than about 150 ns a pixel the long way is tabulated instead, afresh each frame
  (`buildFieldTable()` in `src/shapes_field.hpp`). With REPEAT on the field is measured over one lattice
  cell; with REPEAT off it is measured over a square around the frame, and the squares wholly outside the
  frame are left alone. Parts that are all black or all white are read back as such, busy parts from a flat
  blend of four corner values or, where that is not close enough, a curved surface through nine stored values
  (a quadratic each way). Either is trusted only after it matches the field at 25 points to within a quarter
  of a grey level; the flat blend is preferred because it is quicker to read back. Only the rest is worked out
  per pixel. The table differs
  from the long way by at most one grey level on every scene tried (`testTableAgainstOldKernel`,
  `testBusyScenesAgainstOldKernel` and `testFrameTableAgainstOldKernel` in `tests/shapes_test.cpp` compare
  several hundred scenes pixel by pixel; the 25-point check is sampled, not proved) and does not depend on the
  thread count. A thin SPREAD (0.12) with MELT at its maximum, and a busy scene with REPEAT off, take about 5
  to 10 ms (30 to 120 ms before the curved surfaces and the frame table). The distance to a stretched shape
  stops its search for the nearest edge sooner than it did (`angleBound2()` in `src/shapes_field.hpp`: a
  lower bound from the shape's largest radius), which gives the same bytes. Scenes that stay under the
  150 ns threshold are never tabulated: REPEAT 3 with COPIES 8 and SPREAD 0.12 on a five-sided shape takes
  about 10 ms at 960 x 540 and 35 ms at 1440 x 1080.

### Slot allocation

- Bus ids, freeze slots, frame ring slots, signal ring slots and external frame ids all come from
  `VideoSlotAllocator<N>`. A slot is held until released and is never shared
  by two live owners. Claim and release happen on the UI or construction
  thread only. The one call the audio thread may make is `tryClaim()`, which
  never waits: it returns 0 at once if every slot is taken or another thread holds the lock.
  Scope Trace uses it (through `signalRingTryAllocate()`) to retry about once a second when its
  constructor found every signal ring slot taken; it logs one warning when that first happens.
- Limits: 64 bus ids, 64 freeze slots, 64 signal rings, 64 external frames, 64 frame rings.

### Public C interface

`include/vru_bus.h` and `src/vru_bus_api.cpp` expose the bus to other plugins as a versioned table of
C functions behind one exported symbol, `vruBusGetApi` (see `docs/VIDEO-BUS.md`, section 3). Outside
sources use the existing `ExternalFrame` node: `claimSource` takes a bus id and an external frame id,
`publishFrame` copies the pixels into the external frame store (mutex, allocation: not for the audio
thread) and publishes the node once, and the renderer samples it like Syphon In. `readFrame` renders
with one `VideoGraphRenderer` owned by the API, behind a mutex, so API readers have their own
trail and generator state. Nothing in the node table or the renderer changed for existing modules.
Tests: `tests/vru_bus_api_test.cpp` and `tests/vru_bus_dylib_test.cpp` (both run by `make test`).

### Tests

- `make test` builds `build/bus_test` from `tests/bus_test.cpp` plus the engine sources and
  runs it (it needs no Rack SDK), then runs the Syphon round-trip on Apple silicon. The bus
  tests cover the freeze latch, same-type chains, Bus Mixer A/B and Matte, empty B, cycles and
  depth, seqlock consistency, slot allocation past 8, bus id voltage mapping, the signal ring,
  the CRT beam layer, the Deflection Yoke scanline beam, the Scope Trace fallback, trigger and persistence,
  the Scope Trace CV maths, trigger level, sweep generator, RESET wipe, DWELL and BEAM, the signal
  ring retry, the Lorenz attractor, and Scan Processor. The Deflection Yoke checks are ripple counts, one continuous stroke
  per scanline with gaps between lines, edge modes, LINES, turnaround brightness, persistence, zoom
  collapse, coil response and zoom at each scan time. The Deflection Yoke rework (2026-11-09) added
  `testDeflectionYokeMath` (the RATE knob and its Hz mapping, the four waveforms, and every CV scaling in
  `src/deflection_yoke_math.hpp`), `testSignalRingFourthChannel` (the S channel, and that the window the renderer
  reads is aligned to the sample count), `testDeflectionYokeSCoil` (a square lands where the rotation of its
  resting place puts it, in the right direction), `testDeflectionYokeSize` (SIZE scales the normal scan about the
  centre and the coil deflection still adds), `testDeflectionYokeCollapse` (a line and a point keep their light, a
  line keeps its colours), `testDeflectionYokeClampRotation` (no dark corner under the Clamp edge however far the
  raster turns), `testDeflectionYokeLines`, `testDeflectionYokeFieldRate` (a signal at the field rate keeps its phase
  from field to field and creeps at any other rate) and `testDeflectionYokeExtremes` (NaN, infinity and
  out-of-range values draw what the nearest sane value draws, and the worst Wrap frame renders in bounded
  time). The Scan Processor checks are the flat-grey lines,
  the lift under a bright square, bunching, the H WIDTH mirror, ROTATE 90, BLANK, and DEFLECT Y ripples.
  The Scope Trace CV and sweep checks are: the CV functions (0 V adds nothing, the attenuverter scales
  and inverts, every clamp, the hue wrap including a hair below zero, the Z cases); the sweep (the
  no-trigger period of two windows, the ramp steps and last value, holdoff, restart, a zero or NaN
  window); the trigger level (level 0 picks the same start as the original finder on 18 signals, the
  window starts within a sample of the level, a level beyond the signal free-runs, a pixel proof that
  moving the level moves the trace); and the RESET wipe (the trail survives a quiet render, and a new
  `clearSerial` leaves exactly the bare graticule).
  The Scope Trace beam checks (phase 2) are: the Round spot (it adds all its energy, is centred to
  1/32 px, is as wide along x as along y, keeps half its energy half a pixel off the edge, and writes
  nothing outside the row band it is given); the DWELL law (DWELL 0 gives every pixel of path the same
  energy, DWELL d gives a pixel `(mean stroke / its stroke)^d` units to 3 percent, the whole path adds up to
  what the law predicts, the boost stops at 16 times, a stopped beam burns a dot, NaN and Inf samples
  are dropped, a Lissajous four times as fast burns the same energy per pixel of path, and each trace of
  Dual Trace has its own mean); BEAM as a width (the sigma mapping at 480, 1080 and 144 rows and the 8 px
  cap, the measured width of a drawn line, a round spot, energy that does not depend on BEAM, and the
  1080-row frame 2.25 times as wide with the same peak); the calibration of the default brightness (the
  old law and the new law each draw a line, and the mean over 16 sub-pixel positions of its brightest
  pixel is compared at BEAM 0, 0.4 and 1: at BEAM 0.4 a horizontal or vertical line is within 3 percent of
  the old one and a diagonal is 0.81 of it); what an old patch meets at the BEAM it was saved with (the
  new line carries `2.6 / (0.6 + 0.8 x BEAM)` times the old light to 1 percent, is about 0.5, 1.5 and 3 px
  wide against the old 0.35 px, and has its brightest pixel at about 3.9, 1.0 and 0.34 times the old one
  for a horizontal or vertical line); and a triangle that rests on its corners, fed through a real ring
  and rendered at several DWELL and BEAM values. The slot checks cover `signalRingTryAllocate` and
  `VideoSlotAllocator::tryClaim` (with every slot claimed they give 0, and they find a slot as soon as
  one is released) and `testSignalRingSlotReuse`: a released ring comes back like a new one (no old sample
  rate, no old trigger stamps, and frames cut at multiples of S from the new owner's first sample, so a
  test that claims a slot no longer depends on which test held it before). `ScopeTrace::paramsFromJson`
  (DWELL 0 for a patch that has no DWELL) is not covered: the test build has no Rack SDK, so it cannot
  construct the module.
- `testRampsMonotonic` checks both linear axes, radial growth, grayscale output and
  identical-layer subtraction; `testColorizerStops` checks primary stops, exact band
  count, offset and alpha passthrough; `testColorizerTop` checks that white keeps the top
  stop at OFFSET 0, with and without bands, that greys 0 to 254 keep their colours, and that a
  half-turn OFFSET still wraps. Proofs are `ramps_axes.png` and `colorizer_stops.png`.
- Frame ring tests cover ages, capacity, size changes, duplicate counters and clear latching.
  Feedback tests cover accumulation, same-tick reuse, CLEAR, zoom and two renderers at different
  sizes. Inverse-map tests check identity, zoom, rotation, Black and Wrap. `feedback.png` shows
  passes 1, 2, 5 and 12.
- Rotator tests (`testRotator*`) cover byte-identical defaults, exact 90, 180 and whole turns, a pivoted half
  turn, zoom, X/Y, each edge mode, fold symmetry, notch stepping with glide, spin, RESET and the seamless
  10 V saw. Proofs are `rotator_turns.png`, `rotator_edges.png` and `rotator_kaleidoscope.png`.
- `make test` also builds and runs `build/shapes_test` from `tests/shapes_test.cpp`, right after the bus
  test (no Rack SDK; it links the same engine sources). It covers bus publishing, the control map and CV
  scales, the grey picture and MATTE, identical pictures however many threads draw, whole and fractional
  SIDES, ROUND, POINT, SIZE, X, Y, ASPECT and TURN, REPEAT, COPIES and MELT, seams between cells, the culled
  kernel against a brute-force sum, distances against references that share none of its code, smoothness,
  extreme inputs, speed, the presets and registration, and the chain into the Keyer, Wave Shaper and
  Colorizer. Proofs are `shapes_sides.png`, `shapes_fractional_sides.png`, `shapes_round.png`,
  `shapes_point.png`, `shapes_cv.png`, `shapes_placement.png`, `shapes_spread.png`, `shapes_repeat.png`,
  `shapes_copies.png`, `shapes_melt.png`, `shapes_chain.png` and `shapes_presets.png`.
- Video Mixer tests (`testVideoMixer*`, nine functions). State covers the lever and its flip at the far
  end, CUT, AUTO and the TIME law, NEXT stepping and the NEXT CV bands, ADVANCE and its fallback, pickup
  after a cut, the idle tidy and reset. Ends checks that every pair of inputs passes through untouched at
  fader 0 and 1 in all three modes, out-of-range faders, and absent inputs. Dissolve and add, wipe, colour
  and key each have a test of their own, down to the SOFT and no-PATTERN cases and a missing key or
  matte. The graph test publishes the node through the bus and renders it with real generators behind
  the inputs: which roles are read at which fader position, one bus in two roles, a cycle in any role and
  a second frame size. The reference test runs 600 random settings against an independent reading of the
  maths and requires every pixel within one level. Proofs are `video_mixer_transitions.png`,
  `video_mixer_wipe_patterns.png`, `video_mixer_wipe_oscillator.png`, `video_mixer_color.png` and
  `video_mixer_key.png`.
- Proof PNGs go to `build/bus_test_proofs`, or to the directory given to `make test TEST_PROOF_DIR=`.
  The beam proofs are the grid and a camera-like image at 200 Hz, 1 kHz and 5 kHz, quadrature,
  zoom collapse, and a persistence strip. The Deflection Yoke rework adds `wobble_s_*.png` (the S coil at
  fixed angles and at 60 Hz), `wobble_collapse_*.png`, `wobble_clamp_rot_pos.png` and `_neg.png`, and
  `wobble_extreme_wrap.png`. The Scan Processor proofs are `scan_processor_*.png`: the source, the
  relief from it, a bright shape, Video Oscillator SMPTE bars and an oscillator pattern, ROTATE 90, and a
  DEFLECT Y sine. The Scope Trace proofs include `scope_trigger_level_proof.png` (the same signal at
  two trigger levels), `scope_reset_clears_proof.png` (a trail kept, and a trail wiped by RESET),
  `scope_dwell_proof.png` (a triangle that rests on its corners, at DWELL 0, 0.5 and 1) and
  `scope_beam_width_proof.png` (the same trace at BEAM 0, 0.4 and 1).
- `make bench` builds `build/deflection_yoke_bench` from `tests/deflection_yoke_bench.cpp`. It times one
  Deflection Yoke frame at 448 x 640, 576 x 720 and 600 x 1080, for 200 Hz, 1 kHz and 5 kHz, with X-only
  and X+Y deflection. It uses the module defaults (black edge, PERSIST 0.35, GLOW 0.4) and it times only
  the neutral path (no S rotation, full SIZE), which is the path that ran before the rework. Pass
  `clamp` as the second argument to time the clamp edge. It then builds and runs
  `build/scan_processor_bench` from `tests/scan_processor_bench.cpp`, which times one Scan Processor frame at
  448 x 640 and 576 x 720 for 128, 256, 525 and 1050 lines, with X and Y deflection at 1 kHz. Last it
  builds and runs `build/scope_trace_bench` from `tests/scope_trace_bench.cpp`, which times
  `depositScopeTrace` (the beam stroke alone, before the glow and the tone map) over the longest TIME
  window (100 ms: 4800 samples at 48 kHz, drawn as 2400 points) at BEAM 0, 0.4 (the default) and 1, in
  Oscilloscope, XY and Dual Trace modes, at 640 x 480, 1440 x 1080 and 1920 x 1080, with DWELL at its
  default. 1440 x 1080 is the real worst case: the Display draws at most 1080 lines and Syphon Out
  offers 1440 x 1080. 1920 x 1080 is a stress case beyond it. The signal is a worst case too, a
  full-scale 1 kHz sine on X and a 1.51 kHz sine on Y. The bench prints the median of 15 runs, and a
  last column times a whole `renderBus()` of the same trace so the stroke can be read against the
  frame. It builds at -O2 with the flags in the Makefile; the plugin itself builds at -O3 with
  -funsafe-math-optimizations, so a plugin build can time differently.

  Medians in ms for BEAM 0 / 0.4 / 1 from one `make bench` run on 2026-11-10, with other jobs running
  on the machine (expect a spread of about 10 percent between runs):

  | Size | Mode | Stroke alone | Whole frame |
  |---|---|---|---|
  | 640 x 480 | Oscilloscope | 1.26 / 0.81 / 1.09 | 2.21 / 1.80 / 2.05 |
  | 640 x 480 | XY | 2.64 / 1.66 / 2.19 | 3.68 / 2.66 / 3.22 |
  | 640 x 480 | Dual Trace | 1.37 / 0.90 / 1.23 | 2.40 / 1.95 / 2.23 |
  | 1440 x 1080 | Oscilloscope | 2.07 / 2.81 / 4.43 | 6.93 / 7.80 / 9.30 |
  | 1440 x 1080 | XY | 4.83 / 6.92 / 9.93 | 10.35 / 11.79 / 15.67 |
  | 1440 x 1080 | Dual Trace | 2.27 / 3.06 / 4.86 | 7.34 / 8.04 / 9.85 |
  | 1920 x 1080 | Oscilloscope | 2.19 / 2.83 / 4.51 | 7.96 / 9.36 / 11.11 |
  | 1920 x 1080 | XY | 6.69 / 8.22 / 13.11 | 13.26 / 15.39 / 20.15 |
  | 1920 x 1080 | Dual Trace | 2.31 / 3.09 / 4.91 | 8.99 / 9.80 / 11.49 |

  At the default BEAM no case is over one 60 fps frame (16.7 ms); the slowest is XY at 1920 x 1080 with
  15.4 ms. Over the budget: XY at 1920 x 1080 and BEAM 1 (20.2 ms). XY at 1440 x 1080 and BEAM 1 is
  just under it (15.7 ms). At 480 rows BEAM 0 costs more than BEAM 0.4, because the small spot (sigma
  0.5 px) is stamped every pixel of path and the default spot (sigma 1.5 px) every 3 px, and a stamp
  has a fixed cost. At 1080 rows the cost rises with BEAM, as the splat grows to 19 x 19 pixels at the
  default (sigma 3.4 px) and 35 x 35 at BEAM 1. The stroke runs on one thread (`depositScopeTrace`
  passes the whole height to `depositPath`), unlike the Deflection Yoke and Scan Processor strokes, which split
  rows across threads.

  Cost against the Scope Trace as it was before the beam work (commit 4f7e0f7), each build at its own
  defaults (BEAM 0.4, and DWELL 0.5 in the new one), the same signal, both built at -O2, the median
  over 12 interleaved rounds of the median of 15 runs, with other jobs running on the machine. Old to
  new, in ms:

  | Size | Mode | Stroke | Whole frame |
  |---|---|---|---|
  | 1440 x 1080 | Oscilloscope | 0.89 to 2.74 (x3.1) | 4.46 to 7.58 (x1.7) |
  | 1440 x 1080 | XY | 1.87 to 6.05 (x3.2) | 6.77 to 10.98 (x1.6) |
  | 1440 x 1080 | Dual Trace | 0.97 to 3.00 (x3.1) | 4.50 to 7.52 (x1.7) |
  | 640 x 480 | Oscilloscope | 0.40 to 0.81 (x2.0) | 1.24 to 1.77 (x1.4) |
  | 640 x 480 | XY | 0.80 to 1.64 (x2.0) | 1.80 to 2.61 (x1.4) |
  | 640 x 480 | Dual Trace | 0.46 to 0.91 (x2.0) | 1.29 to 1.82 (x1.4) |

  Neither build is over the 16.7 ms budget at its defaults. A second comparison built both with the
  plugin's own flags (-O3 -funsafe-math-optimizations) and timed XY at 1440 x 1080 and BEAM 0.4 alone,
  over 12 paired rounds. There the gap is wider: the stroke went from 1.71 to 7.37 ms (x4.3) and the
  whole frame from 6.66 to 12.32 ms (x1.85), still under the budget.

  The extra cost is the wider spot: the old
  one was a thin two-tap line stamped every pixel of path, and the new default is a 19 x 19 splat
  every 6.75 px at 1080 rows. A patch saved before DWELL loads DWELL at 0 and costs the same as a
  new one. The splat loop was tried in two other forms that give the same output to the last bit
  (zero-padded weight rows with no scalar tail, and a flat form with every weight stored three
  times). Both timed slower than the plain loop, which the compiler already vectorises (the stroke 9
  to 17 percent and 22 to 38 percent slower), so the plain loop stays. The largest lever left is to
  split the stroke across row bands as the Deflection Yoke does. It should not change the output, because
  each pixel would still take its stamps in the same order, but it changes how the stroke is drawn. It
  has not been made or tried.

## CRT beam layer

Code: `src/crt_beam.{hpp,cpp}`. The beam layer is the shared CRT physics. Scope Trace, the
Deflection Yoke and Scan Processor use it.
A module publishes a node that carries its parameters, and the renderer does the beam work.

### Model

- **Energy field.** `crt::EnergyField` holds linear RGB energy as float, one value per channel
  per pixel. It is additive. Each beam node on a bus owns one field in each renderer, so Display
  and Syphon Out keep separate trails.
- **Path deposit.** `crt::depositPath(field, spot, x0, y0, x1, y1, rgb, energy, rowLo, rowHi)`
  spreads a path's total energy evenly along its length, one sample per pixel of travel (a Round
  spot of sigma s samples every `max(1, 2 s)` pixels, which still overlaps into a smooth line). The
  caller chooses `energy`, so the caller chooses the brightness law. The Deflection Yoke and Scan Processor
  give every scanline the same energy: a fast stroke covers more pixels, so each pixel gets less,
  and a slow stroke, or one that folds back, gets more. Intensity per screen area is then
  proportional to 1 / speed. Scope Trace gives each segment energy in proportion to its length,
  scaled by the DWELL law (see Scope Trace below), so its trace is even by default and brightens where
  the beam lingers as DWELL goes up. A zero-length path splats once, so a stopped beam burns a
  dot. Strokes that overlap add. `rowLo` and `rowHi` limit writes to a band of rows, so threads
  split a field by rows with no shared writes.
- **Spot.** `crt::BeamSpot(sigmaPx, shape)` has two shapes. Both have normalised weights, so a
  deposit always adds its full energy.
  - `Shape::Row` (the default, used by the Deflection Yoke and Scan Processor) is a Gaussian across the
    stroke (in rows) and linear along it (two columns). It suits near-horizontal scanlines, and it
    leaves a vertical stroke the same width whatever sigma is.
  - `Shape::Round` (used by Scope Trace, which asks for it; the default shape is `Row`) is the same
    Gaussian along both axes, a separable kernel of radius `ceil(2.5 sigma)`, clamped to 1..20 pixels
    (up to 41 taps). A stroke is as wide going up or along a diagonal as going across.
  - A sigma of 0 gives a plain bilinear splat in either shape.
- **Persistence.** `EnergyField::decay(retain)` multiplies the field by `retain` once per render.
  The renderer then deposits the new strokes on top.
- **Glow.** `crt::Glow::compute(energy, amount)` averages the field into a 4x downsample, runs two
  box-blur passes of radius 2 on each axis (a triangular kernel, so a halo has no hard outer edge),
  and reads the result back bilinearly between cell centres, so the glow shows no 4-pixel grid.
  Scope Trace BLOOM, Deflection Yoke GLOW and Scan Processor all use it.
- **Tone map.** `crt::toneMap(energy, glow, k, rgba)` writes `1 - exp(-k (E + G))` as RGBA8 with
  alpha 255. It uses a lookup table, so it costs one lookup per channel.
- **Coil.** `crt::CoilFilter` is a one-pole lowpass (`y += a (x - y)`), with
  `a = 1 - exp(-2 pi fc / fs)`. It models yoke inductance: high tones deflect less. Each channel
  needs its own instance.

### Scan timing (NTSC-like)

- Line rate is fixed at `crt::kLineRateHz` = 15,734.264 Hz.
- A field is the visible lines (LINES) plus `crt::kBlankingLines` = 22.5 lines of vertical blanking
  (flyback). The beam is off during the flyback. With 240 lines a field is 262.5 lines, which is
  59.94 Hz. Other counts follow the line rate: 120 lines gives 118.3 Hz, and 600 lines gives 25.3 Hz.
  The field rate follows LINES. The line rate does not change.
- Timing does not depend on the render height. Scanline k starts at `(k + x / W) * line time`
  within the field, for source column x. The signal ring is read one field long, so its window
  is the field the scan runs on.
- The Deflection Yoke RATE knobs reach 20 kHz (they reached 8 kHz before 2026-11-09). A tone at half the
  line rate (7,867 Hz) sets up a stationary pattern on alternate lines, as on a real set, and a tone at
  the full line rate (15,734 Hz) puts one cycle on every scanline. The ring holds the module's samples,
  so a 48 kHz session has about 6.1 samples per cycle at half the line rate and about 3.05 at the line
  rate (2.8 at 44.1 kHz, double that at 96 kHz); at the line rate part of the picture is a sampling beat,
  about 20 lines long.
- `crt::fieldSeconds(lines)` and `crt::lineSeconds()` give these periods.

### Renderer pattern

A beam node follows the same steps each render:

1. Resize its field if the render size changed, and clear it.
2. Decay the field by the persistence factor.
3. Deposit the strokes. Split the work across threads by row bands.
4. `Glow::compute` on the field, then `toneMap` into the output frame.
5. Keep the composed frame, so a node reached twice in one pass shows the same frame.

Scope Trace and the Deflection Yoke both run this pattern. Their state is `VideoGraphRenderer::beam`,
indexed by bus id.

## Scan-time signal model

This section describes the model after the stage 2 pass. Code:
`src/signal_ring.{hpp,cpp}`, `src/scope_timebase.hpp`, `src/expo_hz.hpp`,
`src/hz_param.hpp`, `src/lorenz.hpp`, and the Deflection Yoke and Scope Trace renderer
paths in `src/video_graph_renderer.cpp`.

### Why

A renderer that reads a control once per redraw (about 60 Hz) cannot respond to
audio-rate signals. A raster is drawn over time, so a real raster display puts each
scanline, and each point on it, where the deflection is at the moment that point
is scanned. Modules that need this publish a signal ring, and the renderer reads
the ring at each pixel's scan time.

### Signal ring

- One ring per module instance, claimed with `signalRingAllocate()` in the
  constructor and released with `signalRingRelease()` in the destructor. Storage
  is 4 channels by 32768 samples (power of two), with relaxed atomic floats.
  That is 64 slots x 4 x 32768 x 4 bytes = 32 MiB of static memory (it was 3 channels and 24 MiB
  before 2026-11-09). The Deflection Yoke uses all four (H, V, zoom, S coil: `kDeflectionYokeDeflectX`,
  `kDeflectionYokeDeflectY`, `kDeflectionYokeZoom`, `kDeflectionYokeSCoil`, the `DeflectionYokeSignalChannel` enum
  in `video_engine.hpp`). The ScanProcessor module (shown as Scan Processor) uses all four too
  (`kScanProcessorDeflectX`, `kScanProcessorDeflectY`, `kScanProcessorBlank`, `kScanProcessorZ`, the
  `ScanProcessorSignalChannel` enum); the other writers fill the first three and leave the fourth at 0.
- Single writer. `process()` calls `signalRingSetSampleRate()` and
  `signalRingPush()` once per sample. No allocation, no lock.
- `signalRingClear()` (writer side) hides earlier samples without stopping the
  ring. Scope Trace reset uses it.
- Readers (UI thread):
  - `signalRingReadFrame(slot, out, periodSeconds)`: the newest complete frame of
    that period (the 60 Hz raster by default, or a field from
    `crt::fieldSeconds()`). The window is the frame that ends on the newest boundary
    of the sample clock, plus one sample of interpolation margin (S + 2 samples).
    Frames therefore stay phase-locked to the sample clock. The boundary is counted on
    the ring's absolute write counter, which is never reset (a released slot keeps
    counting, and `signalRingClear()` only moves the ring's origin). So a signal whose
    rate is a whole multiple of 1 / period keeps the same phase against the picture from
    field to field, and any other rate creeps through the picture at its beat.
  - `signalRingReadLatest()` and `signalRingReadSeconds()`: the newest N samples
    or seconds, for traces.
- Overrun check: a reader returns false if the writer wrapped onto its window
  during the copy. It tries again on its next call.
- Values come back by linear interpolation (`SignalWindow::valueAtScanTime`,
  `sampleAt`). `valueAtScanTime` maps time by the window's own period.

### Deflection Yoke

The module is shown in Rack as "VRU–01 Deflection Yoke". Its slug, class, files and preset folder are `DeflectionYoke`.

Code: `src/DeflectionYoke.cpp` (module, panel, signal side), `src/deflection_yoke_math.hpp` (header-only math with no
Rack types: the RATE mapping, the waveforms, every CV scaling and the S coil strength), `DeflectionYokeParams`
and `DeflectionYokeSignalChannel` in `src/video_engine.hpp`, and `renderDeflectionYokeFrame` in
`src/video_graph_renderer.cpp` (beam side).

Design basis (rework of 2026-11-09). The module is modelled on the behaviour of extra coils on a monitor's
deflection yoke, driven by
oscillators, that move the scanned picture: a horizontal coil, a vertical coil and a third, hand-wound "S
curve" coil. The oscillators give sine, triangle and square waves from about 10 Hz to 30 kHz, and 30, 60 and
90 Hz give the most symmetrical pictures. Enough level folds the picture over on itself, DC through a coil
shifts the raster, and the normal scan can be collapsed to a line or a point so that the coils draw alone:
60 Hz on both gives a circle, 120 Hz against 60 Hz in phase gives a bow tie or a figure 8, and 30 Hz on a
collapsed horizontal scan gives two sine shapes half a cycle apart. The module follows that behaviour. It is
not a copy of any unit.

Inference. The published descriptions do not say how the S coil acts on the picture. The module rotates the whole
deflection about the screen centre by an angle proportional to the S signal, so a straight line bends into
an S as the angle changes down the picture. That mechanism, and its strength (`kSCoilRadians`, 0.5 rad per
unit, chosen by eye from a sweep of 0.25 to 1.0), are this project's inference. No source states them.

Controls and ids. Params and inputs are the `ParamId` and `InputId` enums in `DeflectionYoke.cpp`. Ids are
saved in patches, so the nine params and 22 inputs the rework added come after the old ones (params 19 to
27, inputs 7 to 28) and nothing moved. The `X_` and `Y_` names are the H and V coils. Params: an amount per
coil (X_BEND, Y_BEND, S_BEND, bipolar), DEPTH, a RATE per coil (RATE is H, then Y_RATE and S_RATE), FINE,
PHASE, Y_PHASE, S_PHASE, a WAVE per coil (X_WAVE, Y_WAVE, S_WAVE: sine, triangle, square, DC), H_SIZE,
V_SIZE, ZOOM, Z_RATE, LINE_CURL, TWIST, FLIP_H, FLIP_V, EDGE, LINES, SPOT, COIL, PERSIST and GLOW. Inputs:
the video bus token; an audio-rate input for each coil and for zoom, which replaces that oscillator; RESET;
and a CV jack for every other control except the WAVE and EDGE switches and the ZOOM amount. Output: the
video bus token, with a SIG light. A fresh module works at once: DEPTH 0.5, H amount 0.75, all three rates
at 60 Hz, everything else neutral (S amount 0, both SIZE at 1, phases 0, waves sine, ZOOM 0, EDGE black).
Before the rework a fresh module had DEPTH 0, H amount 0 and rates of 28.3 Hz, which drew nothing. A saved
patch keeps its own values either way.

Signal side (audio rate):

- Four oscillators run in `process()`: one per coil (H, V, S) and one for zoom. A coil's RATE knob stores
  `v` and plays 0.1 x 80000^v Hz, so v = 1 is 8 kHz (where the knob used to stop, so every saved patch keeps
  its exact Hz) and the knob's end, v = 1.0812, is 20 kHz. The mapping is the unclamped
  `deflection_yoke::rateHzFromKnob`; the shared `expHzFromNormalized` clamps to 0..1 and is left alone. One FINE
  ratio (+/-50 cents) scales the H, V and S rates together, so circles stay circles. Each rate has a 1 V/oct
  input (clamped to +/-10 V). An oscillator never runs below 1 mHz or above 0.45 x the sample rate. The zoom
  oscillator is a sine, Z RATE 0.01 to 20 Hz (default 0.5 Hz).
- WAVE picks sine, triangle, square or DC per coil. DC is a constant +1: through a coil it shifts the raster,
  and through the S coil it turns the picture a fixed angle. PHASE offsets all four oscillators together;
  V PHASE and S PHASE offset their own (5 V on a phase jack is one cycle). V runs a quarter cycle ahead of
  H on its own (H is a sine, V a cosine), so equal rates draw a circle; V PHASE of -0.25 cancels that lead
  and puts the two in phase. RESET clears all four phases.
- A cable on a coil input (H, V, S or zoom) replaces that oscillator: +/-5 V is +/-1, limited to +/-2.
- Level. DEPTH is the master level of the three coils. The H and V channels carry
  `DEPTH x AMOUNT x source x 0.5` in picture widths and heights, so full scale moves the picture by half its
  width (or height) and it can leave the screen completely. The S channel carries `DEPTH x AMOUNT x source`
  as a plain strength, and the renderer multiplies it by `kSCoilRadians`. The zoom channel carries
  `2 x ZOOM x signal`. The renderer uses zoom = 1 + value, clamped to 0.02 to 4, so zoom near 0 collapses the
  raster toward a point and zoom above 1 overscans. Each sample the module pushes these four values (H, V,
  zoom, S) to its signal ring, one channel each (`DeflectionYokeSignalChannel`).
- Yoke lowpass. The H, V and S channels each pass through a COIL lowpass (one `crt::CoilFilter` per coil, one
  shared cutoff of 500 Hz to 8 kHz, default 3 kHz, with a 1 V/oct COIL input, limited to 10 Hz up to
  0.45 x the sample rate). The gain is 1 / sqrt(1 + (f / fc)^2), so high tones deflect less: at the default
  cutoff about 0.19 at the line rate. Zoom is not filtered.
- CV scalings are in `deflection_yoke_math.hpp`: an amount jack adds +/-5 V for +/-1; DEPTH adds 0.1 per volt;
  a phase jack adds one cycle per 5 V; CURL and TWIST add +/-1 for +/-5 V (the total is limited to +/-3);
  H SIZE, V SIZE, SPOT, PERSIST and GLOW sweep the knob's whole range over +/-5 V; LINES moves one octave per
  5 V, rounded to whole lines; FLIP H and V invert their switch while the gate is above 1 V. An unpatched
  jack reads 0 V, and a voltage that is not finite reads 0.
- Cost. Per sample: four oscillator phases, the wave select, the 1 V/oct on each rate, the amounts, DEPTH, the
  phases, the three coil filters and the ring push. At default settings there is no `pow()` per sample (the
  version before had about five); a cable on FINE or on a RATE input costs one. The slow controls (knob rates
  in Hz, FINE, COIL, LINES, SPOT, PERSIST, GLOW, CURL, TWIST, SIZE, FLIP, EDGE) and the bus publish run every
  32 samples (`kControlDivision`, about 0.7 ms at 48 kHz), and at once after a load or a sample rate change.

Beam side (per render):

- **Scanlines.** The beam paints LINES scanlines per field, top to bottom. Scanline k is a
  left-to-right sweep of the source width. Its colour is the average of the source rows it covers.
  Flips are applied here. With more lines than rows, each line takes one row.
- **Signals.** The renderer reads one field window of the four channels (H, V, zoom, S) and runs each value
  through `sanitiseSignal` (a value that is not finite counts as 0, and anything beyond +/-8 is cut,
  `kWobbleSignalLimit`), so no cable or setting can put a NaN or an infinity into the arithmetic.
- **Landing.** Each cell corner lands where deflection, zoom and the static twist and curl put it
  at that corner's scan time. Each cell's energy is `kWobbleGain x pitch x colour`, where pitch is
  the render height divided by LINES. That energy is spread along the cell's landed path with
  `depositPath`. Pitch is the scanline spacing in pixels, so the average brightness of a full
  white source stays the same whatever LINES, the spot size and the render size are.
- **Two corner paths.** The corner loop (`landRows`) is instantiated twice. A patch that uses neither the S
  coil nor a SIZE below 1 runs the neutral one, which holds only the original arithmetic,
  `px = fw (0.5 + zoom (gx - 0.5 + dx))`, so its frames are the same bytes as before the rework (checked
  against the previous build at the default settings and at a saved-patch set, X 0.85, DEPTH 0.55, RATE 0.35).
  The general one does the new work in this order, all in pixels so a circle stays round: the normal scan,
  scaled by SIZE, plus the H and V coils (`ox = fw ((gx - 0.5) hSize + dx)`, `oy = fh ((gy - 0.5) vSize + dy)`);
  then the S coil rotates (ox, oy) about the centre by `S x kSCoilRadians`, where a positive angle turns the
  picture clockwise on screen; then zoom scales the result about the centre. A window whose S signal stays
  under `kSCoilMinSignal` (1e-5) is drawn as if the coil were off.
- **Collapse brightness.** `depositPath` gives every cell the same energy whatever its path, so a raster
  squeezed onto a line or a point would stack all its light there and go solid white. While a SIZE is below 1,
  a grid of 4 px cells (`kCollapseCellPx`) marks every cell a stroke passes through (a long stroke is sampled
  along its length, at most 32 samples per path, `kCollapseMaxSteps`), and the gain is multiplied by
  `1 + (coverage - 1) x collapse`, where coverage is the marked share of the screen and
  `collapse = 1 - min(H SIZE, V SIZE)`. That gives no compensation at 1, full compensation at 0, and
  a smooth blend between. A collapsed line then reads bright and keeps its colours; a point is a white-hot
  dot and loses its colour. (`kCollapseBoost` is a trim left at 1, so it adds nothing.)
- **Spot.** The Gaussian sigma across the line is SPOT x pitch, with a 0.3 px minimum. The
  default SPOT of 0.20 gives visible lines with dark gaps. Smaller spots make brighter lines
  and darker gaps, as a sharper focus does.
- **Speed.** A cell that lands on a slow stretch of the path deposits its energy onto fewer
  pixels, so it reads brighter. Where the deflection folds the beam back, the energy piles up.
  A cell that moves across many pixels reads dimmer, as a fast beam does.
- **Persistence and glow.** Each render decays the field by `0.9 x PERSIST`, then deposits. GLOW
  is the blur-add amount. The tone map is `1 - exp(-1.6 E)`.
- **Threads.** Corners are computed per scanline in parallel (each thread keeps its own occupancy grid and
  merges it under a lock). Deposits run in parallel over row bands. No two threads write one pixel.

Edge modes apply to where the beam lands:

- **Black.** Cells that land off the screen deposit nothing. Cells outside the source are not drawn.
- **Clamp.** The source is extended with its edge colour. The beam keeps going, so extra cells
  beyond the first and last column, and extra scanlines beyond the first and last line, carry the
  edge colour. Their count covers the largest excursion of the picture (costs more, see the bench). With
  the S coil it also covers the rotation's reach, `half diagonal x (sin t + 1 - cos t) x largest zoom` with
  `t = min(pi/2, largest S x kSCoilRadians)`, so a turned raster shows no dark corner. On an axis whose SIZE is
  below 1 there is no extension: a smaller raster shows its own edge, so Clamp jumps from the extended picture
  to the smaller raster as SIZE passes exactly 1.
- **Wrap.** Each path is drawn again at each whole-screen offset that reaches the band, so the
  picture tiles across the screen. A stroke whose box misses the band at that repeat is skipped before
  `depositPath` walks it; without that, the worst Wrap frame (every cell thrown far off the screen) took
  94 s in a test, and now takes under 3 s. The collapse grid wraps too.

Ripples: a sine on one axis gives `f x visible time` ripples per field, where visible time is
LINES x line time (the flyback is dark). At 240 lines that is `f x 15.25 ms`. Measured on a vertical line
with a 0.25 sine on the H signal: 200 Hz gives 3.1, 1 kHz gives 15.3, 5 kHz gives 76.3 (expected 3.05,
15.25, 76.27). Near the line rate the count follows the distance from the line rate, not the frequency:
each scanline meets a tone `d` Hz away a little further along its cycle, so the wave slides down the
picture `d x 15.25 ms` times per field. Measured: 130 Hz below and 130 Hz above the line rate give 2.0
each (expected 1.98), 16 kHz gives 4.1 (4.05), 14 kHz gives 26.4 (26.45). At exactly the line rate
(15,734.26 Hz) every scanline meets the same part of the wave, so the tone itself makes no ripple down
the picture, and 240 per field is not what shows. What shows is the beat of the sampled ring (see the
timing notes): about 12 per field (measured 12.3), at about a sixth of the tone's full swing (18 px
against 112 px in the test).

Folding. The normal scan crosses the picture at about 15,700 picture widths per second (one width per line
time). The H coil folds the picture back where its own speed passes that. Its speed is at most
`pi x COIL x DEPTH x AMOUNT` widths per second (the lowpass caps it), so the H coil can fold only when
COIL x DEPTH x AMOUNT is above about 5 kHz. At the default 3 kHz COIL it never folds (its speed tops out at
0.6 of the scan speed); with COIL at 8 kHz, DEPTH 1 and AMOUNT 1, a tone folds from about 6.4 kHz up, and 1.4
times the scan speed is reached at the line rate. The V coil folds at 60 Hz and 240 lines once
`DEPTH x AMOUNT x 0.5` passes about 0.17 picture heights, because the scan crosses a height in 15.25 ms;
preset 06 sets it to 0.5. The S coil folds at one sine cycle per field once `DEPTH x AMOUNT x kSCoilRadians` passes about 0.26 rad, so the
default DEPTH of 0.5 with a full S amount (0.25 rad) sits just under it (preset 07), and DEPTH or the S
amount past that is where the fold-over starts.

Timing notes.

- The field is 262.5 lines (59.94 Hz) at 240 lines, but the ring window holds a whole number of samples (801
  at 48 kHz, so windows come at 59.93 Hz). A 60 Hz coil is therefore not locked to the picture: it creeps
  through it, one roll in about 13 s at 48 kHz. A coil at a whole multiple of the window rate holds still (a
  window starts on a multiple of its length on the ring's sample count; see "Signal ring"), and FINE can trim
  a drifting coil until it does.
- 30 Hz is one cycle in two fields, so alternate fields are displaced in opposite directions. With
  persistence both stay visible: two images half a cycle apart (preset 08). How they look depends on the
  PERSIST setting and the frame cadence of the Display.
- Near the line rate the ring is the limit: at 48 kHz a line lasts 3.05 samples, not a whole number, so the
  sample grid slips against the start of each line by 0.05 sample and the pattern repeats about every 20 lines
  (12.3 per field in the ripple measurement). A tone at the line rate has only 3.05 samples per cycle, so the
  picture shows this sampling beat as well as the tone. This is the "scallops" look and is expected.

Presets. `presets/DeflectionYoke/` holds twelve `.vcvm` files. Each lists all 28 params and pins the four
oscillator phases to 0, so a load always starts the same. Rates are stored as knob values. 00 Neutral
changes nothing. 00 to 02 are the earlier presets retuned to the 60 Hz home rate (02 also lowers V to 0.5,
so its 60 Hz sweep stays under the fold point). 03 to 05 keep their stored values, with V RATE set equal to
RATE because the old files never stored it. 06 to 11 set up the manuals' recipes: 06 Fold Over (V coil at
60 Hz, DEPTH and V amount full), 07 S Curve (S coil at 60 Hz), 08 Two Images (H coil at 30 Hz, more
persistence), 09 Dancing Line (H collapsed, H coil at 243 Hz), 10 Circle (both collapsed, 60 Hz, H amount
0.75 and V amount 1.0 so the 4:3 picture draws a round circle) and 11 Bow Tie (V at 120 Hz in phase with H,
both collapsed).

Known limits.

- A rotated stroke keeps its Gaussian across rows and its linear spread along x, so a turned line loses a
  little light (about 3% at 0.5 rad, 10 to 12% at 1.2 rad).
- Clamp shows the smaller raster when a SIZE is below 1 (it jumps at exactly 1), and a point loses its colour.
- Near the line rate the ring's sample spacing, not the model, limits the picture (see the timing notes).
- Two Images depends on PERSIST and on the frame cadence of the Display.
- The S coil mechanism and its strength are inference (see above).

Render size: the Display renders at its on-screen size (see "Display panel"). Test proofs use
448 x 640, a portrait test raster.

### Scope Trace

- Captures every sample into its ring at full rate (the ring is not decimated, so nothing
  aliases). TIME sets the displayed window in seconds:
  `10^(-3 + 2 * TIME)`, from 1 ms to 100 ms, where TIME is the knob plus its CV (see "Scope Trace
  controls and CV"). The TIME knob shows the window in ms. The renderer does thin a long window when
  it draws: at most 4096 points, one in every `stride` samples (a 100 ms window at 48 kHz is 4800
  samples, so stride 2).
- The fallback tone (used for an unpatched X or Y) advances once per sample at
  `scopeFallbackHz(TIME)`, which is two cycles per window (399.05 Hz at the default
  TIME, whose window is 5.012 ms). The Hz is the frequency the oscillator actually runs at. Its shape depends on
  the mode (`scopeFallbackSignal`): XY and Polar get the Lissajous pair, Oscilloscope a
  sine, Dual a sine plus a triangle at the same rate.
- Trigger (Oscilloscope and Dual only; XY and Polar free-run on the newest window). The
  window starts at a rising edge of X through the trigger level (TRIG LVL, 0 V by default),
  the latest one that leaves room for the window in the newest 2 x window samples. It is a
  Schmitt trigger centred on the level: armed 0.05 below it and fired 0.05 above it, in
  channel units (volts / 5), so 0.25 V either side (see `scopeFindTrigger`). A periodic input
  therefore draws the same picture each frame. If there is no edge in range (auto, which
  includes a level the signal never reaches), the window free-runs. The optional TRIG input
  stamps the ring on its rising edge through the level (`signalRingMarkTrigger`); with a cable
  patched, the window starts at the newest stamp that has a full window after it.
- The mode name is drawn on the panel under the MODE knob (`ScopeModeReadout`). MODE is a
  snap switch with four positions (0 to 3); the param, the knob range and the seqlock
  payload all carry it unchanged.
- Persistence: the trace is drawn as strokes into the node's CRT energy field (`depositPath`). Each
  render decays the field by `0.5 + 0.48 * DECAY`, then tone-maps it with k = 1.6. The graticule goes
  on top, not into the field.
- BEAM is the width of the beam and nothing more. The spot is `crt::BeamSpot::Shape::Round` with
  `sigma = 0.5 + 2.5 x BEAM` px at 480 rows: 0.5 px at BEAM 0, 1.5 px at the default 0.4, 3 px at
  BEAM 1. The energy gain is `kScopeBeamGain` = 2.6 at 480 rows or fewer, times
  `max(1, height / 480)` above that, so a wider beam spreads the same light over more
  pixels and its peak is lower. (Before this, BEAM scaled the energy by
  `0.6 + 0.8 x BEAM` and the spot was a thin two-tap linear one.) The 2.6 was chosen so that a
  horizontal or vertical line at the default BEAM 0.4 has about the brightest pixel the old gain gave it,
  and that is all it holds. `tests/bus_test.cpp` draws the same line with the old law and the new,
  slides it through 16 sub-pixel positions and takes the mean of the brightest pixel. At BEAM 0.4 that
  is 1.013 times the old one for a horizontal line and 1.003 for a vertical line, and a diagonal is 0.81
  (the old two-tap spot lit diagonals more). Away from 0.4 the peak is not held. Against the old
  default line (the old BEAM 0.4 patch, horizontal line), BEAM 0 peaks about 2.6 times as bright and
  BEAM 1 about half as bright (2.567 and 0.511 in `tests/bus_test.cpp`). Against each old patch at its
  own BEAM, the horizontal line's brightest pixel is 3.9 times the old one at BEAM 0 and 0.34 at BEAM 1
  (3.936 and 0.336 in the same test).
  The default beam carries about 2.8 times the old light in total, because it is wider. An old patch
  meets the new beam at the BEAM it was saved with, not at 0.4; those figures are in "Behaviour changes
  for old patches" below.
- Frame height: the sigma and the gain both scale by `max(1, height / 480)`, with the sigma capped at
  8 px (`scopeBeamScale` and `scopeBeamSigmaPx` in `src/video_engine.hpp`), so the beam keeps the same
  width as a fraction of the picture at any size from 480 rows up (a 1080-row frame uses a sigma of
  1.125 to 6.75 px). A spot k times wider spreads each pixel of path over k times the rows, so the gain
  goes up k times to keep the peak where it was. A frame under 480 rows keeps the 480-row size, so
  the beam is not thinner than 0.5 px.
- DWELL (beam dwell, 0 to 1, default 0.5) says how a stroke's energy is shared along the trace. For a
  stroke of `len` pixels, with `base = 0.25 x INTENSITY x Z x gain`:
  `perPx = min(base x pow(meanLen / max(len, 0.25), DWELL), 16 x base)`, and the stroke deposits
  `perPx x max(len, 0.5)`. `meanLen` is the mean length of the window's drawable segments (at least
  0.5 px), taken for each trace in Dual Trace. At DWELL 0 every pixel of path gets `base`, so the
  trace is even. At DWELL 1 every stroke gets the same energy whatever its length, as a CRT beam at
  constant current does: a slow stretch glows and a fast one is dim. A slow stretch can outshine the
  even level by 16 times at most. Because the law is relative to the window's own mean, running the
  whole figure faster leaves its average brightness per pixel where it was, and only the spread along
  the trace changes. The 0.5 px floor on the stroke length makes a stopped beam burn a dot, which
  energy in proportion to length never drew. The Deflection Yoke and Scan Processor do not use this law.
- BLOOM: the shared `crt::Glow`, added before the tone map.
- A node reached twice in one pass reuses its frame, so the trail advances once
  per pass.

### Scope Trace controls and CV

Saved patches and presets address params, inputs and outputs by number, so ids are only ever
appended; `ScopeTrace.cpp` pins them with `static_assert`s. Params 0 to 11, inputs 0 to 5 and the
VIDEO_BUS output keep the ids, ranges and defaults they always had (the BEAM tooltip now reads
"Beam width"). The new ids are params 12 to 21, inputs 6 to 11 and outputs 1 and 2 (the light is
unchanged). A later change appended params 22 to 27 (the attenuverters for DWELL, INTENSITY, DECAY,
BEAM, BLOOM and trigger level) and inputs 12 to 17 (their CV jacks), in that order; a patch saved
before them loads with the new attenuverters at +100 % and the new jacks unpatched, so it draws as it did.

| Control | Param (range, default) | CV input | Attenuverter (label, param) | Value used |
|---|---|---|---|---|
| MODE | 0 (snap 0 to 3, 0) | none | none | XY Lissajous, Oscilloscope, Dual trace, Polar |
| X SCALE | 1 (0 to 2, 0.85) | X_SCALE, 6 | X SCL, 12 | `clamp(knob + att x V/5, 0, 2)` |
| Y SCALE | 2 (0 to 2, 0.85) | Y_SCALE, 7 | Y SCL, 13 | `clamp(knob + att x V/5, 0, 2)` |
| X POS | 3 (-1 to 1, 0) | X_POS, 9 | X POS, 15 | `clamp(knob + att x V/5, -1, 1)` |
| Y POS | 4 (-1 to 1, 0) | Y_POS, 10 | Y POS, 16 | `clamp(knob + att x V/5, -1, 1)` |
| TIME | 5 (0 to 1, 0.35) | TIME, 3 | TIME, 17 | `clamp(knob + att x V/10, 0, 1)`, then the window |
| BEAM | 6 (0 to 1, 0.4) | BEAM, 15 | BEAM, 25 | beam width: `sigma = 0.5 + 2.5 x clamp(knob + att x V/10, 0, 1)` px at 480 rows |
| DECAY | 7 (0 to 1, 0.65) | DECAY, 14 | DECAY, 24 | persistence: `clamp(knob + att x V/10, 0, 1)` |
| HUE | 8 (0 to 1, 0.32) | HUE, 11 | HUE, 18 | `knob + att x V/10`, wrapped into [0, 1) |
| INTENSITY | 9 (0 to 2, 1.2) | INTENSITY, 13 | INTENS, 23 | beam intensity: `clamp(knob + att x V/5, 0, 2)` |
| ROTATE | 10 (-1 to 1, 0) | ROTATION, 8 | ROT, 14 | `knob + att x V/5`, wrapped into [-1, 1) (see below) |
| BLOOM | 11 (0 to 2, 0) | BLOOM, 16 | BLOOM, 26 | glow: `clamp(knob + att x V/5, 0, 2)` |
| (brightness) | none | Z, 2 | Z, 19 | unpatched 1; patched `clamp(1 - max(att, 0) + att x V/5, 0, 2)` |
| TRIG LVL | 20 (-5 to +5 V, 0) | TRIG_LEVEL, 17 | LEVEL, 27 | `clamp(knob + att x V, -5, 5)` volts, see below |
| DWELL | 21 (0 to 1, 0.5) | DWELL, 12 | DWELL, 22 | `clamp(knob + att x V/10, 0, 1)`: how much brighter the beam is where it moves slowly, see the DWELL bullet above |

CV maths lives in `src/cv_param.hpp` (`rasterlab_cv`); `process()` and the tests call the same
functions. Each attenuverter runs from -100 % to +100 % and defaults to +100 %. An unpatched jack reads
0 V, so at the defaults a patch with no CV cable draws exactly what it drew before. Scales, positions,
rotation, INTENSITY and BLOOM move 1/5 of a unit per volt (as on the Deflection Yoke), so +-5 V adds +-1
(INTENSITY and BLOOM run 0 to 2, so 10 V sweeps their whole knob). TIME, HUE, DWELL, DECAY and BEAM
move 1/10 per volt, so 0 to 10 V sweeps the whole knob. The trigger level is already in volts, so its
CV adds volts one for one and the sum stays within +-5 V. There is no smoothing: every jack is read
once per sample, as before, and the sum is a multiply and an add. The TIME jack already worked this way
(knob + V x 0.1), so +100 % reproduces it. HUE wraps instead of clamping, because a colour wheel has
no end: a negative sum comes round from the top, and a knob at 1.0 wraps to 0, which is the same colour.

ROTATE wraps instead of clamping (`rasterlab_cv::modulatedTurn`, `wrapTurn`). The knob runs -1 to +1,
which is -180 to +180 degrees, one full turn being 2 units, so 10 V of CV travel (at +100 %) is one
full turn. Past either end the angle comes round again: +1 and -1 are the same angle, so the picture
does not jump. A steadily rising CV, such as a ramp or saw, spins the picture continuously, and the
knob sets the starting angle. The wrap is floor-based, so negative and large sums work, and a NaN or
infinite sum gives 0. The renderer wraps the same way (`depositScopeTrace`), so the value it receives
is safe even from a caller that did not. Only Scope Trace's rotation changed; `ScopeTraceParams`
is the only user of that field, and the Display, Feedback and Deflection Yoke code paths do not share it.

The Z jack keeps its meaning at +100 %: 0 V blanks, 5 V is full brightness, 10 V is double (bit-identical
to the old `clamp(V/5, 0, 2)`). A smaller positive setting lifts the floor (at 50 %, 0 V is half
bright; 5 V is full at any positive setting), and 0 % leaves the beam full. A negative setting inverts,
fully at -100 % (0 V is full and 5 V blanks); at -50 %, 5 V leaves the beam half bright. With nothing
patched into Z the beam is always full.

### Scope Trace trigger level, SWEEP, SYNC and RESET

- Trigger level. TRIG LVL (-5 to +5 V, default 0) is the level L. With a cable in TRIG, the module
  runs a Schmitt trigger on that jack with thresholds L and L + 10 mV; each rising crossing stamps the
  ring. At the default level a 0/10 V gate still fires on every gate. (The old thresholds were
  0.1 V and 2 V.) With TRIG unpatched, the renderer finds the crossing in X itself:
  `ScopeTraceParams::triggerLevel` carries L in channel units (volts / 5) to `scopeFindTrigger`,
  which is bit-identical to the old finder at L = 0.
- SWEEP output: a ramp from -5 V up to just under +5 V over one window, then a snap back to -5 V,
  where it waits. The window is the TIME window, so the ramp follows TIME and its CV. It runs in
  every mode, XY and Polar included. Counting is in whole samples: at 5 ms and 48 kHz the sweep is
  240 samples, rising 10 V / 240 per sample, so the last running sample is about 4.958 V.
- SYNC output: a 10 V pulse of 1 ms on the sample where each sweep starts. It is never longer than
  half a window, so at the shortest timebase two sweeps in a row stay two pulses.
- What starts a sweep (`ScopeSweep` in `src/scope_sweep.hpp`: pure maths, one call per sample). A
  trigger edge starts it, and triggers that arrive while a sweep runs are ignored (holdoff). With no
  trigger it starts itself after one idle window, like a scope's AUTO mode, so the period is two
  windows: one ramp, one wait. The trigger source is the TRIG jack if patched, otherwise a second
  Schmitt trigger in the module on the X signal (the fallback tone when X is unpatched) with the
  same L and L + 10 mV band. That trigger paces the sweep only. The drawn window comes from the
  renderer's own finder, so on a periodic input the two agree but they are not sample-locked.
- RESET (rising edge, thresholds 0.1 V and 2 V as before) clears the ring, restarts the fallback
  tone at phase 0, and restarts the sweep on the next sample, even in the middle of one. It now also
  wipes the phosphor: the module counts RESET edges in `clearSerial`, which travels in
  `ScopeTraceParams`, and the renderer clears the node's energy field when the number it sees is
  not the one it last drew with. It is a counter rather than a flag so a RESET between two frames
  cannot be lost. Only a change of the serial clears, so steady state never clears. The renderer
  keeps the last serial per bus, so a replacement module that starts on a bus an earlier Scope Trace
  used (it publishes serial 0 against the stored count) clears the trail once, when it first
  publishes. `clearSerial` is not saved, and loading a patch does not bump it.

### Scope Trace panel

The panel is 20 HP (101.6 mm; it was 12 HP). Small controls and jacks sit on eight columns 12.7 mm
apart, columns 0 to 7 from the left. The four large knobs sit on four columns 25.4 mm apart, each
centred over a pair of the eight columns. Rows are centre y in mm from the top of the panel.

| Row | y | Contents, left to right |
|---|---|---|
| Large knobs | 30 | MODE (the mode name is drawn under it), X SCALE, Y SCALE, ROTATE |
| Small knobs | 50 | X POS, Y POS, TIME, DWELL, INTENSITY (labelled INTENS), HUE, DECAY, BEAM |
| Attenuverters | 68 | X SCL, Y SCL, ROT, X POS, Y POS, TIME, HUE, Z |
| CV jacks | 80 | one under each attenuverter, each labelled CV |
| Small knobs | 97.5 | TRIG LVL (column 0), BLOOM (1) |
| Attenuverters, second set | 94 | columns 2 to 7: DWELL, INTENS, DECAY, BEAM, BLOOM, LEVEL |
| CV jacks, second set | 106 | one under each, labelled CV |
| Signal jacks | 119 | X IN, Y IN, TRIG, RESET, SIG light, SWEEP, SYNC, BUS |

TILT (beam tilt) is a later phase of `docs/scope-trace-audit.md`. It has no param yet, and when it
arrives it takes the next free id (28 and up) and a free spot. The six new CV pairs took the free
columns 2 to 7 below the TRIG LVL and BLOOM row, and the signal jack row moved from 112 to 119 mm
(the panel stays 20 HP wide and exactly 128.5 mm tall). The LEVEL pair modulates the trigger level
and the BLOOM pair modulates BLOOM; their knobs stay in columns 0 and 1. The sketch of this layout is
at `docs/scope-trace-cv-panel.png`. DWELL took
column 3 of the second small-knob row, the slot that was kept for it. The trigger-level row is at
97.5 mm, not 96, so the two-line TRIG LVL label clears the CV jacks above it. The CV jacks sit under
their attenuverters, not under the knobs they modulate; each attenuverter's label names its
control. The INTENSITY knob is labelled INTENS (its tooltip still says Intensity). The full word is
15.8 mm wide, and beside the 10.6 mm DWELL label on a 12.7 mm pitch the two would overlap by
0.5 mm; INTENS is 11.2 mm wide and clears it. SWEEP, SYNC and BUS sit on black output plates
(SWEEP's is wider to fit its label), the same as the other modules.

The two main signal inputs, X IN and Y IN, sit in outlined plates: the output plates' size (9.2 x
11.8 mm), unfilled, with a 0.35 mm dark outline and the label in dark text inside, below the jack.
They used to be plain jacks labelled X/A and Y/B above the jack, directly under the X SCL and Y SCL CV
jacks in the same two columns, and signals were patched into those by mistake. A plate with its
label below the jack is the one thing in the column that reads as the signal input. Their
tooltips read "X in / trace A" and "Y in / trace B" (A and B are the two traces of Dual Trace). TRIG
and RESET stay plain jacks with the label above, and the SIG light and the three output plates are as
before. The two plates are 3.5 mm apart, X IN's is 1.75 mm from the left edge of the panel (BUS's is
1.75 mm from the right), and both stand 6.5 mm below the TRIG LVL and BLOOM knobs above them. Only the
drawing, the label text and the two tooltips changed: the input ids, and every other id, are as they
were.

### Dual LFO Chaos

- CHAOS selects the speed of a Lorenz attractor (sigma 10, rho 28, beta 8/3),
  stepped with RK4 once per audio sample. Speed runs from 0.5 to 32 time units
  per second on a log scale. The X and Y outputs are the attractor's x and y,
  scaled to +/-1 before the output range.
- CLK is a 1 ms gate.
- RATE A and RATE B are exponential (0.01 to 20 Hz). The RATE inputs are 1 V/oct.

### Behaviour changes for old patches

- Deflection Yoke (2026-11-08 rebuild): the picture is now drawn as scanlines (LINES, default 240), with dark gaps
  between lines and a 59.94 Hz field with dark flyback. Deflection is twice as large (full scale is
  half the width), and zoom runs from 0.02 to 4 (was 0.05 to 2, and the CV term is doubled). X and Y
  pass through the COIL lowpass (default 3 kHz), so high tones deflect less. Clamp extends the source
  with its edge colour instead of pinning the corners. New params (LINES, SPOT, COIL, PERSIST, GLOW)
  are appended after EDGE. Old patches keep their values for old params, and get the defaults for
  new ones (PERSIST 0.35, GLOW 0.4), so they gain a trail and glow. The old sine-pattern LFO is
  gone, RATE is an exponential audio-rate oscillator, and X/Y BEND inputs carry deflection signals.
  A patch with bend at 0 and depth at 0 looks the same, apart from scanlines and glow.
- Deflection Yoke (2026-11-09 rework): the module gained an S coil, a WAVE per coil, H and V SIZE and a CV jack
  for nearly every control (see "Deflection Yoke"). New params (ids 19 to 27) and inputs (ids 7 to 28) are
  appended and default to neutral, so a saved patch loads with the S coil off, both SIZE at 1 and sine
  waves, and renders the same frames as before. That was checked byte for byte, feeding the renderer one
  fixed signal, with the new controls at their neutral settings and with a saved-patch set (X 0.85,
  DEPTH 0.55, RATE 0.35), built at -O2 and with the plugin's flags. Through the module itself (driven
  offline, with no Rack running), the signals that reach the ring are bit for bit those of the old code
  when the compiler follows strict floating-point rules. With the plugin's `-funsafe-math-optimizations`
  the neutral case is still identical, but a saved patch, a busy one and one with audio-rate cables differ
  in about a quarter of their samples by one or two float ulps (at most 6e-8 on a signal peaking at 0.43),
  because the compiler groups the sums of the old and new code differently. That is rounding, not a change
  in the algorithm. The panel is wider:
  20 HP (101.6 mm) instead of 12 HP (60.96 mm), at the same 128.5 mm height. A patch saved with the 12 HP
  panel can have the module overlap its right-hand neighbours until it is moved. Every old control kept its
  id and is still on the panel, under new labels: X DEFL and Y DEFL are the AMT knobs under H COIL and
  V COIL, X RATE and Y RATE the RATE knobs under them, and the audio-rate deflection inputs are their IN
  jacks. The RATE knobs now reach 20 kHz (they stopped at 8 kHz). A saved knob value keeps its exact Hz,
  but its pointer sits at a different angle, because the same travel now covers a longer range. A fresh
  module starts at DEPTH 0.5, H amount 0.75 and 60 Hz on all three coils (it was DEPTH 0, amount 0 and
  28.3 Hz, which drew the plain picture); a saved patch keeps its own values. RESET clears four phases (it
  cleared three), and the module data gains "sPhase". The six presets were rewritten to store every param,
  and six new ones were added. The signal ring has a fourth channel for the S coil, so its static memory
  grows from 24 to 32 MiB.
- Dual LFO Chaos: RATE knobs read as exponential Hz. A saved RATE value from the
  old linear 0 to 12 Hz scale is read as a knob position, so it will run at a
  different rate. The CHAOS output looks different. The saved chaos state is
  dropped unless it is on the attractor.
- Scope Trace: the strokes now use the shared CRT beam layer, sampled at 1 px of beam travel (was
  0.5 px) and with the shared glow, so the look is close to the old one. TIME now means the window length (1 to 100 ms), not a decimation
  factor, so the old display does not line up with the new one. The old
  fallback advanced about 34 times slower than its stated 0.18 to 2.9 Hz. It now
  runs at 399.05 Hz at the default TIME (two cycles per window of 5.012 ms).
  The CV and sweep phase appends params 12 to 20 (eight attenuverters and TRIG LVL), inputs 6 to 11
  and the SWEEP and SYNC outputs after the old ids. An old patch loads the defaults (attenuverters
  +100 %, TRIG LVL 0 V, new jacks unpatched), which draw today's picture. Three things differ.
  The panel went from 12 to 20 HP in 4fde1ef, which is already in the base, and phase 2
  does not change the width. Rack does not push neighbouring modules aside, so in an old
  12 HP patch the Scope Trace overlaps whatever sat to its right by 8 HP until one of them
  is moved. The TRIG
  jack thresholds changed from 0.1 V and 2 V to the trigger level L and L + 10 mV (L = 0 V by
  default): a 0/10 V gate still fires on every gate, but a pulse whose low level stays between 0 V and
  0.1 V no longer re-arms, a pulse peaking between 10 mV and 2 V now fires, and a bipolar signal
  fires on every rising crossing of the level. RESET now also wipes the phosphor trail, which is
  visible only at the moment of a RESET.
- Scope Trace (phase 2, the beam): the module appends one param, DWELL (id 21, default 0.5), and BEAM
  now sets the width of the beam instead of scaling its energy. A new module starts at DWELL 0.5, but an
  old patch loads DWELL 0: Rack restores only the param ids a saved file contains, and an old file has no
  id 21, so `ScopeTrace::paramsFromJson` sets DWELL to 0 before the restore (a DWELL the file does hold
  still wins, and Initialize does not come through there and gives 0.5). An old patch therefore keeps
  its even trace and its own BEAM. `tests/bus_test.cpp` cannot check that override, because the test
  build has no Rack SDK and cannot construct the module. The five presets store their own DWELL (0.6,
  0.35, 0.35, 0.6, 0.5) and kept their BEAM. BEAM now means a true Gaussian width, so an old patch draws differently at the
  BEAM it was saved with. The spot is a round Gaussian with a sigma from 0.5 px (BEAM 0) to 3 px
  (BEAM 1) at 480 rows, where the old spot was always two taps wide (0.35 px), and the gain is 2.6 (times
  `max(1, height / 480)` above 480 rows) where the old one was `0.6 + 0.8 x BEAM`. Per pixel of path the new line carries 4.3,
  2.8 and 1.9 times the old light at BEAM 0, 0.4 and 1 (`2.6 / (0.6 + 0.8 x BEAM)`). Most of that is
  width, not peak. Measured on a line and averaged over 16 sub-pixel positions, the brightest pixel is
  3.9 times the old one at BEAM 0 (the spot is 1.4 times as wide as before, 0.50 px against 0.35 px, so the light lands in the
  peak), 1.01 times for a horizontal line and 1.00 for a vertical one at BEAM 0.4 (a diagonal is 0.81;
  the line is about 4.2 times wider) and 0.34 times at BEAM 1 (about 8.4 times wider, and 1.9 times the
  light). The total light is what adds up where strokes overlap, under a long DECAY and in BLOOM, so a
  busy old patch can read brighter overall at the default BEAM although no single line is brighter at
  its centre. A thinner BEAM now draws a brighter line and a wider one a dimmer line, where BEAM used to
  scale the whole trace. At DWELL above 0 (a new module, the presets) the trace is brighter where the
  beam moves slowly (the turns of a sine, the corners of a Lissajous) and dimmer where it moves fast;
  DWELL 0 is the even trace of the old code. A stopped beam, such as a held or repeated sample, now
  burns a dot at any DWELL where it drew nothing before. The INTENSITY knob's panel label is now
  INTENS, to make room for DWELL. If no signal ring slot is free, the module now warns once in the Rack
  log and tries again about once a second; it used to draw nothing, without a word.

## Scan Processor

The module appears in Rack as VRU–02 Scan Processor; the slug, file and type names are `ScanProcessor`.

Code: `src/ScanProcessor.cpp` (module, panel, signal side), `ScanProcessorParams` and
`ScanProcessorSignalChannel` in `src/video_engine.hpp`, the node in `src/video_bus.{hpp,cpp}`, and
`renderScanProcessorFrame` in `src/video_graph_renderer.cpp`. The module is a scan processor of the
kind built in the early 1970s: the picture's brightness is summed
into the vertical deflection of each scanline, so bright areas lift the raster lines and the lines
bunch and spread. It is built on the shared CRT beam layer. Line Field is unchanged.

### Model

Each scanline is one continuous beam stroke across the source. For a point at ramp `r` along line
`k` (line ramp `L = (k + 0.5) / N`), in screen units (0 to 1 spans the picture):

- `x = H POS + H WIDTH (r + DEFLECT X)`
- `y = V POS + V HEIGHT (L + DEFLECT Y) - LUMA GAIN x luma(r, L)`

Luma is the source averaged over the rows of the line's band (an area average), bilinear in x.
When there are more lines than source rows, it is bilinear in y instead, so there are no steps.
One segment per scan column joins neighbouring points, and each segment is deposited with
`crt::depositPath`. Overlapping lines add light, and a steep lift is dimmer, as in the beam model.

- **Scan space.** The beam is drawn with lines as rows and the sweep along columns, so the spot is
  Gaussian across the line (sigma 0.22 x line pitch, at least 0.3 px). ROTATE 90 transposes that
  scan into the output at the end, so the spot stays right on vertical lines too.
- **Energy.** Each segment deposits `0.8 x pitch x INTENSITY x Z factor`, with the same calibration
  as the Deflection Yoke, so brightness is the same whatever the line count. The Z factor is
  `(1 + Z-INTENSITY x (2 luma - 1)) x max(0, 1 + beam Z)`. Beam Z is the Z input, read at audio rate:
  -5 V turns the beam off, 0 V leaves it alone, +5 V doubles it. The monochrome beam is a green/white
  phosphor. COLOR takes the beam colour from the source at each point instead.
- **Scan timing.** DEFLECT X, DEFLECT Y, BLANK and beam Z are read from the signal ring at each point's
  scan time, `(k + r) x line time`, in a field of the chosen line count (`crt::fieldSeconds`).
  Deflection is sampled per point, so the lines follow audio-rate signals.
- **Blank.** A point with BLANK above 0.5 deposits no energy, so the segment on either side of it
  is dark.
- **Persistence and glow.** The shared phosphor, as the Deflection Yoke: the field decays by
  `0.9 x PERSIST` each render, then `crt::Glow` and `crt::toneMap` (k = 1.6).
- **Threads.** Points are computed per line in parallel (each thread has its own row buffer).
  The deposit splits the scan rows into bands, so no two threads write one pixel.

### Controls and IDs

Params (ids are the `ParamId` enum in `ScanProcessor.cpp`): LUMA GAIN (bipolar, -1..1, default
0.2 screen heights per unit luma), H POS (-1..1), H WIDTH (-2..2, default 1; negative mirrors about
H POS), V POS (-1..1), V HEIGHT (0..2, default 1; 0 collapses the lines to one thin line),
LINES (switch 128 / 256 / 525 / 1050, default 128), INTENSITY (0..3, default 0.6), ROTATE (switch),
COLOR (switch: phosphor / source), Z-INTENSITY (0..1, default 0), PERSIST (0..1, default 0.35),
GLOW (0..2, default 0.25).

Inputs: BUS IN (unpatched reads bus 1), X and Y (audio-rate deflection; +/-5 V full scale moves the picture
half its width, through a fixed 3 kHz `crt::CoilFilter`), LUMA (the luma gain CV, +/-5 V = +/-1 on LUMA GAIN),
INTENSITY (CV added to the knob, 10 V = full knob travel), H POS and V POS (CV added to the knobs, +/-5 V =
+/-1 screen width or height), Z (audio-rate beam brightness: -5 V off, 0 V unchanged, +5 V double; not
filtered), BLANK (beam off while above 1 V). Outputs: BUS 1 and BUS 2 (on black plates, the same video bus
token), with a SIG light.

Panel (13 HP, 66.04 mm, titled SCAN WIGGLER): four columns 15.24 mm apart and six rows. The knobs and
switches fill the top three rows. Each switch carries only its name, with no position words. The CV jacks
fill rows four and five, each H POS, INTENSITY and V POS jack under the knob it adds to, and the bus jacks
and SIG light are in row six. The panel label for Z-INTENSITY is Z-INT.

- Row 1, scan, y 29 (large knobs): LUMA GAIN, LINES, INTENSITY, and the ROTATE switch (up turns the picture 90 degrees).
- Row 2, picture position, y 46: H WIDTH (large knob), H POS (small knob), V HEIGHT (large knob), V POS (small knob).
- Row 3, beam, y 63 (small knobs): Z-INT, the COLOR switch (up takes the colour from the source, down from the phosphor), PERSIST, GLOW.
- Row 4, CV inputs, y 80: LUMA (luma gain CV), H POS, INTENSITY, V POS.
- Row 5, audio-rate CV, y 98: X and Y (audio-rate deflection), Z (beam brightness), BLANK.
- Row 6, bus, y 115: BUS IN, BUS 1 and BUS 2 (both carry the same token, on black plates), and SIG.

The module pushes four channels to its ring each sample: DEFLECT X, DEFLECT Y (both filtered and
scaled to ramp units, 0.5 at full scale), BLANK (1 or 0) and beam Z (the Z input in units of 5 V, clamped to
-1..2, unfiltered; 0 when unpatched). `process()` does no drawing.

### Tests and benchmark

- `make test` checks: flat grey gives 128 evenly spaced, straight lines at the line pitch; a bright
  square lifts the interior lines by LUMA GAIN x luma (32 px measured against 32 px expected); a
  ramp that converges lines gives a brighter bunch than the same energy spread over the pitch (2.4x
  measured); a negative H WIDTH mirrors the bar; ROTATE 90 on a square raster equals the transpose
  of the plain picture; BLANK high gives a black screen; beam Z at -5 V gives a black screen, and +5 V
  at INTENSITY 0.5 gives the same beam energy as 0 V at INTENSITY 1; DEFLECT Y at 1 kHz and 2 kHz gives
  8 and 16 ripples per field, the expected f x visible time (8.14 and 16.27), with the expected
  displacement.
- `make bench`: a 448 x 640 frame at 256 lines takes about 3.2 ms (median, X and Y deflection, on the
  development Mac). The 1050-line case at 576 x 720 takes about 7.5 ms.

## Video Player

Code: `src/VideoPlayer.cpp` (module and 12 HP panel), `src/video_player_engine.{hpp,cpp}` (worker
thread, transport, yt-dlp, loop), `src/video_player_core.{hpp,cpp}` (pure helpers: link and path
classification, YouTube ID, cache path, yt-dlp lookup and arguments, the transport state machine,
status messages, saved settings), `src/video_player_decoder.hpp` (the decoder interface),
`src/video_player_decoder_mac.mm` (AVAssetReader) and `src/video_player_decoder_stub.cpp` (every
other platform: a status message, no crash). The module plays a local file or a YouTube link and puts
the picture on the video bus. Audio is out of scope.

Decisions:

- **Threads.** One worker thread per module opens files, runs yt-dlp, decodes and keeps the playback
  clock. `process()` only calls the engine's lock-free setters (play toggle, stop, loop, speed) and
  getters (`hasFrame`, `isPlaying`, `endCount`), so the audio thread never waits on a file, a child
  process or a lock. The UI thread reads a copy of the state (`snapshot()`) once per drawn frame.
- **Pixels.** Frames go into an external frame slot (`video_external_frame.hpp`), and `process()`
  publishes only the slot id on the bus, as Syphon In does. With no frame the bus is cleared, so a
  removed video does not leave a stale picture. Frames are scaled down to 960 px on the long side,
  never up. If all 64 slots are taken the module still runs, without a picture.
- **Decoder.** AVAssetReader, reached through `FrameSource`, so `make test` runs the whole engine
  against a fake decoder. The decoder creates no thread. Decoding runs on the module's worker thread
  (`engine.cpp:167`) through `decodeSome()`, called from `step()` (`engine.cpp:379`). The track
  lookup, `loadTracksWithMediaType`, waits on a dispatch semaphore
  (`video_player_decoder_mac.mm:240, 246`), so that wait always happens on the worker, never the
  audio thread. With `RASTERLAB_SYPHON=1`, the default on Apple silicon, the build targets macOS
  12.0 or later (`Makefile:23` and `Makefile:102`). MKV and WebM are refused by file extension with a plain message,
  before any decoding.
- **Transport.** Play toggles (button or rising edge). Stop returns to the first frame and shows it.
  Play after the end restarts from the first frame. With LOOP off the clip stops at the end and holds
  the last frame. With LOOP on it wraps: the decoder rewinds while 8 decoded frames are still waiting
  and each pass has its own timeline offset, so the wrap shows no pause. END fires once per end of
  clip, including each wrap. LOOP is on when the button is lit or the gate is high. SPEED is
  0.25x to 2x on an exponential knob with 1 V per doubling.
- **Seek.** Click or drag the thin progress line in the readout to move around the clip. A 10 px
  strip over the line takes a plain left press (no modifier keys) while a clip is loaded; every other
  press falls through, so the module still drags and the menu still opens. The pointer position
  becomes a clip time along the line, and a drag keeps seeking. `Engine::requestSeek` keeps only the
  newest target, in a flag next to the load request but separate from it: a fast drag skips the
  targets the worker never reached, and a seek never cancels a download. The worker moves the
  decoder (`FrameSource::seek`; AVAssetReader cannot reposition, so the Mac decoder starts a new
  reader with a `timeRange` and drops the frames before the target, among them the first sample
  AVFoundation hands over, which is stamped with the start of the range), then shows the first frame
  at once. That is the first frame at or after the target, up to one frame late (33 ms at 29.97 fps,
  measured on a real file). Playing keeps playing from there. Paused stays Paused with the new frame.
  Ready and Ended become Paused, so Play continues from the sought spot instead of rewinding. A seek
  never fires END and is dropped when a new video starts loading. The target is clamped to the clip,
  about two frames short of the end. When the decoder finds no frame there, the worker steps back and
  retries, and in the end falls back to the first frame; only a real decoder error becomes Error.
  Positions stay relative to the clip start, also when the file's first frame is not at 0. The
  decision: a drag on the line, not a new control. It needs no new panel element or SVG change, and
  the position is not saved in the patch.
- **YouTube.** yt-dlp is found in `/opt/homebrew/bin`, then `/usr/local/bin`, then PATH. The format
  is `best[ext=mp4][vcodec^=avc1][height<=1080]` with `--no-playlist`: one H.264 file that
  AVFoundation plays and no ffmpeg merge. It is saved as `<video ID>.mp4` in
  `asset::user("VideoResearchUnit-video-cache")` and reused on later loads. Nothing is installed. A missing
  yt-dlp, a failed download and
  "YouTube has no single-file MP4 (H.264) version of this video, so it cannot be played here. Try another video."
  each give a plain message. YouTube often offers a single-file MP4 only up to 360p (not tested
  against a real video), because higher resolutions come as separate video and audio files that need
  ffmpeg to join.
- **Saved state.** The patch stores the bus id and `source` (none, file or youtube), `filePath` and
  `youtubeUrl`. Loading a patch reopens the source and never starts playback. The source and its
  path are saved when a load is requested (`engine.cpp:191-192`), so a missing file keeps its path.
  The file check runs only in `loadSource` (`engine.cpp:466`), for a queued load (`engine.cpp:306`).
  It gives a persistent error, `File not found: <path>`. Only a new load (`engine.cpp:426`) or a
  clear (`engine.cpp:303`) removes it. Reset removes the video.
- **Tests.** `tests/video_player_test.cpp` covers the core helpers. `tests/video_player_engine_test.cpp`
  runs the engine with a fake decoder and fake yt-dlp scripts, so nothing needs yt-dlp installed:
  play, pause, stop, loop and end-of-clip, the wrap timing, speed, a missing file, MKV and WebM,
  URL against file path, finding yt-dlp (missing, search order), download, cache reuse and failures,
  stopping yt-dlp when the module is deleted, clearing and replacing a source, no free frame slot,
  and saving and restoring state. The seek tests cover a seek while playing, paused, from Ready and
  after the end, a flood of targets (only the first and the newest reach the decoder), a target at
  and past the end, a decoder that fails or finds no frame, no video, a load or a download in
  progress, a loop wrap after a seek, no END pulse, and a clip whose first frame is not at 0.
  Dragging on the real panel is not covered; it needs a running Rack.

### Octoscillator (VRU–23)

Added in 2.12.0. CV-only: no video bus, texture, frame or signal-ring allocation. Eight
independent Lorenz trajectories (sigma 10, rho 28, beta 8/3) expose X, Y and centered Z, giving
24 mono Rack voltage outputs. A ninth trajectory supplies common movement. RELATE is a convex
blend of each local coordinate with the corresponding common coordinate, not nonlinear coupling
or a calibrated statistical correlation coefficient. At RELATE 1 the raw coordinate outputs match
across channels; local depth/offset can still differ. Local states keep advancing at all settings.

`src/octoscillator_math.hpp` owns fixed-size state and pure voltage maths. RK4 advances at Rack's
audio rate, subdividing time steps to at most 0.01 Lorenz time units. SPEED is an exponential time
multiplier (1/256 to 128), not an exact periodic frequency. SPREAD places eight local rates over
a four-octave span at maximum; each RATE adds +/-2 octaves, with final speed bounded to
1/1024–128. At the fastest setting the output has rapid irregular movement in the tens-of-Hz
region; it is not a line-locked or pixel-clock generator. Inputs: SPEED adds 1 V/oct, RELATE
adds 0.1/V, RESET restarts all nine trajectories at distinct settled seeds. Controls update every
32 audio samples; integration and all 24 outputs update every audio sample.

X and Y use tanh scaling at 22 and 28; Z subtracts 25 before tanh scaling at 22, so all three are
bipolar and bounded smoothly, without clipping plateaus. Per-channel DEPTH (default 0.5) multiplies
master LEVEL. RANGE selects +/-5 V, 0–5 V (midpoint 2.5 V) or +/-1 V; per-channel OFFSET then adds
-5 to +5 V. A final +/-10 V guard bounds emitted CV. Zero depth in unipolar mode leaves 2.5 V plus
offset, rather than 0 V. One bipolar activity indicator per channel shows the raw X movement.

Panel is 36 HP: a global control/input row, then eight channel strips, each RATE/DEPTH/OFFSET
above X/Y/Z output plates. Tooltip names include channel numbers. There are no hidden output
channel banks or polyphonic-only outputs.

Patch JSON saves all nine XYZ states under `trajectories`. Invalid shapes, nonnumeric, nonfinite
and out-of-bounds states are ignored. Engine recovery also replaces invalid integration state
with the corresponding seed. RESET restarts the trajectory, and does not reset knob settings;
Rack's module reset also restores parameters. Saving state preserves a trajectory; exact reproduction
requires the same controls, modulation and sample rate. No random reseeding occurs on patch load.

Video use: patch directly into other modules' CV inputs, never BUS inputs. For example X1 to
Shapes TURN, Y2 to Color Lab HUE and Z3 to Video Mixer controls. Shapes and many colour parameters
are sampled per frame, so faster movement can alias; Deflection Yoke and Scan Processor capture
selected controls at scan time. Ordinary CV compatibility does not promise every destination
responds within the raster. Existing module IDs/slugs are unchanged.

`make test` includes `build/octoscillator_test`: integration versus a finer-step reference, all
coordinates at 8/44.1/48/96 kHz, bounds and invalid inputs, independent seeds, exact RELATE endpoints
and midpoint, rate laws, output ranges/offsets, deterministic state restoration and reset.
