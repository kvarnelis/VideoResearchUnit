# VRU Video Bus

How another VCV Rack plugin can read pictures from, or send pictures into, the Video Research Unit (VRU)
video bus. Written for developers of other plugins.

## What you can do

VRU modules pass pictures to each other over BUS cables. Since VRU 2.12 a plugin you write can join in,
through a small public C interface. Your module can read any bus as an RGBA picture (to draw it, save it,
send it to a device), and it can act as a source: you hand VRU an RGBA picture each time you have a new
one, and every VRU processor and sink (Display, Syphon Out, Colorizer, Keyer, Video Mixer and the rest)
can take your cable. You do not link against VRU. You find it at run time, so your plugin still loads
when VRU is absent. The interface is two headers you copy into your plugin: `include/vru_bus.h` and
`include/vru_bus_client.hpp`. Both ship in the VRU package next to this file, and both are MIT licensed.

## Quick start

Copy `vru_bus.h` and `vru_bus_client.hpp` into your plugin's source and include the client header. It
pulls in `vru_bus.h` and `<rack.hpp>`.

```cpp
#include "vru_bus_client.hpp"

const VruBusApiV1* vru = vruBusConnect();   // null when VRU is not installed, or too old
```

`vruBusConnect()` does `rack::plugin::getPlugin("VideoResearchUnit")`, then `dlsym(handle, "vruBusGetApi")`
(`GetProcAddress` on Windows), calls it with version 1, and checks the table's `version` and
`structSize`. It returns null if any step fails. Call it when you need it and test for null each time. Do
not keep the pointer longer than VRU might stay loaded.

The table (version 1):

```c
int  busIdFromVoltage(float voltage);          // 1..64, else 0. Same rule as VRU's own modules.
int  isActive(int busId);                      // 1 if a module publishes a picture on that bus.
int  readFrame(int busId, int width, int height, uint8_t* rgba, size_t strideBytes);
int  claimSource(void);                        // a bus id for your own picture source, 0 if none free
int  publishFrame(int busId, const uint8_t* rgba, int width, int height, size_t strideBytes);
void releaseSource(int busId);
```

Pictures are RGBA, 8 bits per channel, top row first. Width and height are each 1 to
`VRU_BUS_MAX_DIMENSION` (4096). `strideBytes` is the distance between rows and must be at least
`width * 4`. The status codes for `readFrame` and `publishFrame` are:

| Code | Value | Meaning |
| --- | --- | --- |
| `VRU_BUS_OK` | 0 | Success. |
| `VRU_BUS_ERR_BAD_ARGUMENT` | -1 | Null pointer, size outside 1 to 4096, or stride below `width * 4`. |
| `VRU_BUS_ERR_BAD_ID` | -2 | The id is not 1 to 64, or (for `publishFrame`) not a source you claimed. |
| `VRU_BUS_ERR_NO_SIGNAL` | -3 | Nothing is published on that bus, or its chain is a cycle or too deep to draw. |

On any error `readFrame` leaves your buffer untouched.

### A reader module (shows a bus as a picture)

The module copies the cable voltage on the audio thread. The widget reads the picture on the UI thread.

```cpp
struct MyReader : rack::engine::Module {
    enum InputIds { BUS_INPUT, NUM_INPUTS };
    std::atomic<float> busVoltage {0.f};
    MyReader() { config(0, NUM_INPUTS, 0, 0); }
    void process(const ProcessArgs&) override {           // audio thread: just copy the voltage
        busVoltage.store(inputs[BUS_INPUT].getVoltage());
    }
};

struct MyReaderWidget : rack::widget::Widget {
    MyReader* module = nullptr;
    std::vector<uint8_t> pixels;
    void draw(const DrawArgs& args) override {            // UI thread
        const VruBusApiV1* vru = vruBusConnect();
        if (!vru || !module) return;                      // draw "VRU not loaded"
        int id = vru->busIdFromVoltage(module->busVoltage.load());
        if (id == 0 || !vru->isActive(id)) return;        // draw "no signal"
        const int w = 640, h = 480;
        pixels.resize(size_t(w) * h * 4);
        if (vru->readFrame(id, w, h, pixels.data(), size_t(w) * 4) == VRU_BUS_OK) {
            // upload `pixels` as a texture and draw it
        }
    }
};
```

### A source module (gives VRU a picture)

The module claims an id when it is created, puts that id on its output as a voltage, and publishes
pictures from the UI thread or a worker thread of its own. It never publishes from `process()`.

```cpp
struct MySource : rack::engine::Module {
    enum OutputIds { BUS_OUTPUT, NUM_OUTPUTS };
    int busId = 0;                                      // claimed on the UI thread
    MySource() {
        config(0, 0, NUM_OUTPUTS, 0);
        if (const VruBusApiV1* vru = vruBusConnect())
            busId = vru->claimSource();                 // 0 if VRU is absent or full
    }
    ~MySource() {
        if (busId) if (const VruBusApiV1* vru = vruBusConnect()) vru->releaseSource(busId);
    }
    void process(const ProcessArgs&) override {
        outputs[BUS_OUTPUT].setVoltage(float(busId));   // like VRU's own sources; 0 V = no bus
    }
    // Called from your UI thread or your own worker thread, not from process():
    void publish(const uint8_t* rgba, int w, int h) {
        if (const VruBusApiV1* vru = vruBusConnect()) if (busId) vru->publishFrame(busId, rgba, w, h, size_t(w) * 4);
    }
};
```

The claimed id carries no picture until your first `publishFrame`, so `isActive` is 0 until then. VRU
scales your picture to the size each sink renders.

## Thread rules

| Call | Thread |
| --- | --- |
| `busIdFromVoltage`, `isActive` | Any thread, lock free, cheap. Fine on the audio thread. |
| `claimSource`, `releaseSource` | UI or module construction and destruction. May allocate and lock. Never on the audio thread. |
| `publishFrame` | Any thread except the audio thread. It allocates (first call, and whenever the size changes), copies the whole picture and takes a short lock that the renderer also takes. One thread at a time per source id. Do not call it for an id while another thread calls `releaseSource` for it. |
| `readFrame` | The UI thread (as Display does, once per draw) or a worker thread. Not the audio thread. Calls are serialised inside VRU, so a second caller waits. |

`readFrame` is a full CPU render of the whole chain behind that bus at the size you ask for. Ask for the
size you will show, not more. The API keeps its own renderer, so stateful modules (feedback, beam
trails) keep a trail for API readers that is separate from Display's. A read of a bus nobody else is
drawing still renders.

## Versioning, and what VRU promises to keep stable

- `vruBusGetApi(1)` returns the version 1 table. Any other number returns null, so a client built for a
  newer version fails cleanly against an older VRU (and `vruBusConnect()` returns null).
- The table starts with `uint32_t version; uint32_t structSize;`. Future versions only append function
  pointers and raise `structSize`. A version 1 client keeps working against a newer VRU as long as VRU
  still serves version 1, which VRU intends to do for as long as 2.x patches load. Check `structSize`
  before using any member beyond the size you were compiled against.
- Function behaviour, status codes and the constants in `vru_bus.h` do not change within a version.
- The plugin slug is `VideoResearchUnit`. From 2.0.0 the
  slug and every module slug stay. The display name "Video Research Unit" and the name "VRU Video Bus"
  can change. The slug cannot.
- The cable convention stays: integer voltage, 1 to 64, 0 means none.
- Param, input and output ids of VRU modules are append-only, and the saved key `busId` (with the old
  `outputBusVoltage` and `busVoltage` still read) stays.

What VRU does not promise:

- That an id stays the same between sessions. Ids are claimed when modules are created. A saved id is
  honoured on load only if it is free; otherwise the module takes another. Cables carry live ids, so a
  patch still works, but your plugin must not store an id.
- A version field on the cable. There is none. The version lives in the C interface.
- Anything on the private list below.

## What stays private

The node layout and node type numbers, the parameter blocks, the slot count as an implementation detail
(`VRU_BUS_ID_MAX` is 64 today and is the only number you may rely on, as a ceiling), the renderer, the
table's locking, and every C++ symbol. Do not rely on plugin symbols being visible to each other by
name; the lookup goes through the plugin handle on purpose. The only node an outside plugin can create
is the "external picture" source (internally the `ExternalFrame` node that Syphon In and Video Player
already use). You cannot publish a Deflection Yoke or a Colorizer node from outside; you take part by
supplying or reading pictures.

## How the cable works

A BUS cable carries one number: a small integer, as a voltage. 3 V means "bus 3". It is a name for a
slot, not a picture and not a signal you can listen to. The picture does not travel down the cable.

- One channel, monophonic. VRU reads channel 0 only.
- Id N is N volts. Ids run 1 to 64. Put `float(busId)` on your output, with no slew, offset or scaling.
- `busIdFromVoltage` rounds to the nearest integer and returns that id if it is 1 to 64, otherwise 0.
  0 means "no bus". So 0 V, negative voltages, NaN and anything that rounds above 64 all mean no bus. A
  voltage of 3.2 V reads as bus 3.
- There is no version field, no magic number and no checksum on the cable.

A receiver cannot tell a valid bus signal from the cable alone: any value that rounds to 1 to 64 looks
like a bus id. So convert the voltage to an id, then call `isActive(id)` before you draw. An unrelated
cable, say a 1 V envelope, reads as bus 1. If a VRU module owns that slot you see its picture, and if
not you see no signal. Nothing is damaged; a wrong picture is the only effect. A BUS cable plugged into
a normal audio or CV input is just a steady DC voltage of 1 to 64 V. Cables above 10 V are only
meaningful inside Rack, so do not route a BUS cable to an audio or CV output module such as an audio
interface.

A module that does not need pictures can still carry a bus cable: a splitter, multiple or switch works
on the plain voltage and needs nothing from VRU. Keep the voltage exact and monophonic.

Unpatched inputs follow VRU's convention: an unpatched processor input reads bus 1 (a legacy default,
kept on purpose). A THRU output passes the input voltage along when the input is patched and sends 0 V
when it is not. A reader of your own can simply treat an unpatched input as no signal.

## Timing and cost

- **No fixed frame rate on the bus.** The node is updated once per audio sample, but only the latest
  value matters. A picture is rendered when a sink draws: Display on each UI draw, Syphon Out on each
  draw of its hidden OpenGL widget. That follows Rack's UI refresh. A sink that is not drawn (scrolled
  off screen) does not render.
- **Resolution is chosen by the sink**, not the source. Display renders 4:3 with 480 to 1080 rows (it
  follows the on-screen size and the display pixel ratio) and at most 2048 columns. Syphon Out renders
  1024 x 768 or 1440 x 1080. Every node in the chain renders at the sink's size.
- **Sample rate.** The audio sample rate does not set the video rate. Some modules (Deflection Yoke,
  Scan Processor, Scope Trace) also record audio-rate signals into a separate ring buffer
  (`signal_ring`) so the renderer can sample deflection per pixel. That ring is internal to VRU.
- **CPU.** Rendering is on the CPU, per pixel, partly on several threads. A generator keeps state
  between renders and advances once per rendered frame. Measured costs are in `docs/architecture.md`;
  for example, Shapes takes under a few milliseconds for typical scenes at 960 x 540, and the beam
  benchmark cases stay under one 60 fps frame (16.7 ms) at default settings on the author's machine.
  Cost grows with chain length and pixel count.
- **GPU.** The renderer does not use the GPU. The only GPU work is uploading the finished RGBA buffer as
  a texture, which Display and Syphon Out do themselves.
- **Memory.** Grows with the depth of the chain in use, not with the number of modules. Maximum graph
  depth is 16; a deeper chain, or any cycle, is not drawn (Display shows CYCLE).
- **Audio thread.** VRU's own modules publish their processor nodes without allocating or taking locks.
  That is internal to VRU. The public `publishFrame` is not audio-thread safe (see the thread rules).


For your plugin: reading costs one render at the size you ask for, per call. Publishing costs one copy
per `publishFrame` and the memory for the latest picture, even if nobody reads it, so publish at the rate
you have new pictures, not per audio block. The API is tested from a separate program that loads VRU's
library with `dlopen`, but not inside a running Rack with a second plugin.

## Licence

`include/vru_bus.h` and `include/vru_bus_client.hpp` are MIT licensed (`include/LICENSE-MIT`). The rest
of VRU, including the implementation behind the interface, is GPL-3.0-or-later. You may copy the two
headers into your plugin under any licence, closed-source and commercial included, keeping the MIT
notice. Because you talk to VRU only through its plugin handle and the C table, using the interface does
not make your plugin GPL.

## Checklist: a module that is "VRU Video Bus compatible"

Cable level (every module that carries a BUS cable):

- [ ] Treat a BUS cable as monophonic and pass it on unchanged (no slew, no scaling, no offset).
- [ ] If you offer a THRU, send 0 V when the input is unpatched, as VRU does.
- [ ] Never add or mix two tokens. The sum of two ids is a third id.
- [ ] Do not send a BUS cable to audio or CV outputs.
- [ ] Do not store a bus id in your patch data.
- [ ] Name the jacks "Video bus" and say in the tooltip that they carry a VRU token.

Picture level (a module that produces or consumes pictures):

- [ ] Get the interface with `vruBusConnect()` and handle null (VRU absent or too old).
- [ ] Never call `publishFrame` or `readFrame` from the audio thread.
- [ ] Claim a source id when the module is created, release it when it is deleted.
- [ ] Check `isActive` and the status code before you show a picture.
- [ ] Keep the `vru_bus.h` and `vru_bus_client.hpp` MIT notices when you copy them.

## For VRU maintainers

Everything below is internal to VRU and is not part of the public interface.

### How the table works

Each VRU module that has a video output owns one numbered slot in a table inside the plugin (64 slots,
process-wide). Every sample, the module writes a small description of itself into its slot: its type,
its settings, and the number of the slot it reads from. That description is a graph node. Nothing is
drawn on the audio thread. A sink (Display, Syphon Out) reads the slot number from its input cable and
walks the graph backwards from that slot, through each node's source slot, down to a generator. It
renders on the CPU, on the UI thread, into an RGBA buffer, and shows or sends that buffer. So the "bus"
is a table of node descriptions plus a cable that names a row in it. The table is ordinary memory inside
the VRU dylib.

The receiver check in VRU's own code is `videoBusIdFromVoltage()` followed by `videoBusIsActive(id)`
and, if it needs the contents, `videoBusReadNode`. A slot is empty if no module owns it or its owner
has published nothing (or cleared it). Unpatched defaults inside VRU: Bus Mixer's B and MAT inputs
treat unpatched as absent. Display's input A falls back to bus 1 when unpatched; B and C fall back to
nothing.

### The node in the table

Each slot holds one `VideoBusNode`: a tagged structure with a `type` (`VideoBusNodeType`, values 0 to
17, where 0 is Empty), a `sourceBusId`, and one parameter block per module type. The writer replaces
the whole node on each publish. Readers get a consistent copy through a seqlock. They retry if the
writer was mid-update, and give up after 256 tries, in which case that read counts as an empty bus.

The node is a C++ struct of floats and ints, copied bytewise. Its layout is whatever the compiler makes
of `src/video_bus.hpp` and `src/video_engine.hpp`. It is not a serialised format.

Node types and how they connect:

| Kind | Types | Source slot |
| --- | --- | --- |
| Generator | Generator (Video Oscillator), Ramps, Shapes, ScopeTrace, ExternalFrame (Syphon In, Video Player, outside sources) | none |
| One-input processor | DeflectionYoke, LineField, ColorLab, Keyer, WaveShaper, FreezeFrame, ScanProcessor, Colorizer, Feedback, Rotator | `sourceBusId` |
| Multi-input | BusMixer (A in `sourceBusId`; B and matte in its params), VideoMixer (seven buses in its params) | see params |

### Sketches against the real internals


These show how VRU's own modules publish processor nodes. They compile only inside the VRU source tree
and are not a supported plug-in interface.

```cpp
VideoBusOwner videoBus;                                    // claims an id in the constructor
void process(const ProcessArgs& args) override {
    videoBusPublish(videoBus.id(), generatorParams);       // replaces the node, no allocation
    outputs[BUS_OUTPUT].setVoltage(videoBus.voltage());
}
```

A processor reads its input id with `videoBusIdFromVoltage` (unpatched means bus 1), calls
`videoBusPublishColorizer(out, in, params)` if `videoBusIsActive(in) && in != out`, else
`videoBusClear(out)`. A new kind of node means a new `VideoBusNodeType`, a parameter struct, a publish
function and a branch in `VideoGraphRenderer::renderNodeBody`. That is a change to VRU itself.

### Where this lives in the source

Paths are relative to the repository root.

- Bus id as voltage, ids 1 to 64, 0 for none: `src/video_bus.hpp:7-16`, `src/video_bus.cpp:203-209`
  (`videoBusIdFromVoltage`), `src/video_bus.hpp:18` (`kVideoBusCount`).
- Node types and node structure: `src/video_bus.hpp:20-64`.
- Trivially copyable requirement and seqlock: `src/video_bus.cpp:13-53` (writer),
  `src/video_bus.cpp:150-173` (reader, 256 attempts).
- The table is file-local static storage: `src/video_bus.cpp:29-30`.
- Publish functions: `src/video_bus.hpp:66-79,121-127`, `src/video_bus.cpp:63-148,225-271`.
- Occupancy check: `src/video_bus.cpp:184-189`.
- Id allocation, release and `VideoBusOwner`: `src/video_bus.cpp:191-223`, `src/video_bus.hpp:100-119`.
- Saving and loading ids in patches (`busId`, legacy keys): `src/plugin.hpp:15-27`.
- Unpatched input defaults and THRU: `src/video_bus.hpp:15-16,95-98`, `src/display_select.hpp:14-16`.
- Source example (Video Oscillator): `src/VideoOsc.cpp:79,160-161,165-183`.
- Processor example (Colorizer): `src/Colorizer.cpp:15,37-52,58-61`.
- ExternalFrame publishers: `src/SyphonIn.cpp:58`, `src/VideoPlayer.cpp:141`; renderer branch
  `src/video_graph_renderer.cpp:106`.
- Sink example (Display): `src/Display.cpp:245-262` (input to bus id), `src/Display.cpp:596-605` (render
  size), `src/Display.cpp:630-670` (render and texture upload on the UI thread), size limits
  `src/Display.cpp:88-93`.
- Syphon Out sizes and render call: `src/SyphonOut.cpp:20-24,137`.
- Public interface: `include/vru_bus.h` (the contract), `include/vru_bus_client.hpp` (lookup helper),
  `src/vru_bus_api.cpp` (implementation, the one exported symbol). Tests: `tests/vru_bus_api_test.cpp`
  (in-process, calls the table), `tests/vru_bus_dylib_test.cpp` (separate program that `dlopen`s a VRU
  library and uses only the header; `make test` runs it on a test library built from the same sources
  and on `plugin.dylib`).
- Renderer: `src/video_graph_renderer.hpp:20-55` (depth 16, five scratch slots per depth, cycle
  handling), `src/video_graph_renderer.cpp:84-100` (generator advances once per pass).
- Design notes: `docs/architecture.md`, "Video bus plan", "Video bus model", "Threading model".
- Performance figures: `docs/architecture.md`, "Shapes" and the benchmark tables.
- Slug and version: `Makefile:3-4`, `plugin.json`.
