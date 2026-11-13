# Frame ring: Slit Scan and Tape Loop plan

Both modules store pictures in the frame ring that Feedback already uses. Each
module instance owns one ring slot sized for its job, and the renderer, not
`process()`, does every frame copy.

## 1. The frame ring as built

### Storage and API

`src/frame_ring.hpp` and `src/frame_ring.cpp` provide 64 independently owned
slots, numbered 1..64, with a maximum capacity of 600 frames per slot.
A new slot has capacity 1. `FrameRingFrame` holds RGBA bytes, width, height
and the integer counter of the tick that stored it.

- `frameRingAllocateSlot()` claims a slot and returns its id, or 0 if all 64
  are owned. It does not allocate pixel buffers.
- `frameRingReleaseSlot(slotId)` drops the frames, restores capacity 1, resets
  head, count and clear counter, and releases ownership.
- `frameRingSetCapacity(slotId, frames)` clamps `frames` to 1..600; shrinking
  retains the newest frames that fit, and growing keeps existing history.
- `frameRingPush(slotId, frameCounter, rgba, width, height)` copies one complete
  RGBA frame into the next entry, making it age 0. Dimensions must be positive
  and the vector must contain exactly `width * height * 4` bytes. It returns
  false for invalid input or when the newest entry already has that counter;
  otherwise it returns true. Entry vectors reuse their allocated storage.
- `frameRingIsStored(slotId, frameCounter)` returns whether the newest entry
  has that counter; it does not search older entries.
- `frameRingRead(slotId, age, out)` copies the complete entry, including its
  dimensions and counter, into `out`; age 0 is newest. It returns false for
  negative or unavailable ages and does not clamp to the oldest frame.
- `frameRingCount(slotId)` returns the number of available entries, or 0 for
  an invalid slot.
- `frameRingClear(slotId, clearCounter)` empties history once for each changed
  clear counter, retaining pixel-vector allocations for reuse. The initial
  clear counter is 0, so clearing with 0 initially does nothing. A new push
  can follow immediately, even with a counter used before the clear.

Invalid slot ids make void operations no-ops and boolean operations false.
Allocation and release use the shared slot allocator. Each slot has its own
mutex around capacity changes, push, read, count, counter checks and clear;
unrelated modules do not contend on one pixel-store lock.

### Counters, sizes and threads

The first writer of the current counter wins, including its picture size.
Two Displays, or a Display and a Syphon Out, therefore cannot normally append
that tick twice. The guard compares only against the newest entry: it is not
a monotonic-counter check or a record of every counter ever seen, and a stale
counter arriving after a newer one can still append. Tests should expose this
limit before future concurrent rendering relies on stronger ordering.

Feedback stores its assembled output in its capacity-1 ring. It checks for an
already stored tick before computing, then reads that output; if another
renderer wins during computation, it reads the winner after its push fails.
The renderer calls `resampleInto()` when dimensions differ. The ring itself
neither resamples nor stores a shared assembled-output cache for other effects.

Each entry carries its own dimensions. Changing Display size leaves existing
frames intact; later pushes can have a new size, and readers resample old
entries as needed. Nothing is dropped because of a size change, although normal
capacity rollover and shrinking still remove old history.

Renderer threads push, read and clear; today these are UI draw paths, including
the Syphon widgets. The construction/UI thread allocates, releases and sets
capacity. `process()` never calls any ring API, including count queries. It
publishes small parameter snapshots through the existing video bus.

Ring pixels, slot ids and tick counters are not saved in Rack patch JSON.
Feedback starts empty on load. Both proposed modules will save buffer settings
and ordinary controls, but start with empty rings and reset transport state.
If slot allocation fails, pass live input through and show unavailable history.

### Display size and memory

Display follows the panel's on-screen height times zoom and the larger of
pixelRatio and windowRatio. Height is clamped to 480..1080 and width is rounded
to four thirds of height. Although `kMaxRenderWidth` is 2048, the height clamp
currently limits actual Display output to 1440 x 1080. The 2048 x 1536 column
below is a requested planning ceiling, not a size the current Display reaches.

RGBA uses four bytes per pixel: 640 x 480 is about 1.2 MB per frame;
960 x 720 about 2.8 MB; 1440 x 1080 about 6.2 MB; 2048 x 1536 about 12.6 MB.
The table multiplies exact byte counts before rounding, using decimal
MB (1,000,000 bytes) and GB (1,000 MB); each cell is MB / GB.

| Frames | 640 x 480 | 960 x 720 | 1440 x 1080 | 2048 x 1536 |
|---|---|---|---|---|
| 1 | 1.2 / 0.001 | 2.8 / 0.003 | 6.2 / 0.006 | 12.6 / 0.013 |
| 15 | 18.4 / 0.018 | 41.5 / 0.041 | 93.3 / 0.093 | 188.7 / 0.189 |
| 60 | 73.7 / 0.074 | 165.9 / 0.166 | 373.2 / 0.373 | 755.0 / 0.755 |
| 150 | 184.3 / 0.184 | 414.7 / 0.415 | 933.1 / 0.933 | 1887.4 / 1.887 |
| 300 | 368.6 / 0.369 | 829.4 / 0.829 | 1866.2 / 1.866 | 3774.9 / 3.775 |
| 600 | 737.3 / 0.737 | 1658.9 / 1.659 | 3732.5 / 3.732 | 7549.7 / 7.550 |

For a laptop patch with other applications open, the 300- and 600-frame cells
at 1440 x 1080 and the 150-, 300- and 600-frame cells at 2048 x 1536 are
unreasonable defaults for one module. Even 600 at 960 x 720 is expensive.
Several instances multiply these costs. This is why buffer length is a module
setting rather than a fixed number. Figures exclude renderer scratch, the
assembled-output cache proposed below, vector overhead and retained allocations;
a ring whose entries once held larger pictures can retain more memory.

## 2. Slit Scan

Slit Scan assembles an output picture from many stored input frames, taking
each output row or column from a different age so time runs along one axis of
the picture. LUMA BAND uses brightness to distribute time across the picture
instead, making bright and dark regions reveal different moments.

### Controls and mapping

- MODE is a three-position snap knob: ROWS, COLUMNS, LUMA BAND.
- DIRECTION is a two-position switch that flips which edge is newest; a gate
  at DIRECTION overrides the switch while patched (high means flipped).
- DEPTH knob plus CV sets the oldest requested age, clamped to
  `0..capacity - 1` frames. DEPTH 0 passes the live input through, while the
  renderer continues recording history.
- Include SHAPE knob plus CV in the first version: a bipolar -1..1 control
  gives `k = 2^(2 * shape)`, from 0.25 to 4, with linear mapping at 0.
  It makes time bunch at either edge without adding another mode.

For ROWS, `t = y / (H - 1)`; for COLUMNS, `t = x / (W - 1)`.
Use `t = 0` for a one-pixel axis. DIRECTION replaces `t` with `1 - t`.
The linear rule is `age = round(depth * t)`; SHAPE changes it to
`age = round(depth * t^k)`.

For LUMA BAND, sample normalized luma from the newest stored source frame at
that pixel: `luma = 0.2126 R + 0.7152 G + 0.0722 B` with RGB in 0..1.
The linear rule is `age = round(depth * luma)`, optionally inverted by
DIRECTION using `1 - luma`. Apply the same exponent for SHAPE. Normally bright
areas are older; inversion makes them newer. Sample the chosen frame at the
same normalized picture coordinate, accounting for its stored dimensions.

Clamp every requested age to `count - 1` when history is short; the existing
read API does not do this. With no frames, pass live input through. The effect
fills in over the first `depth` successfully rendered recording ticks, nominally
`depth` frames after a patch starts, rather than reading unavailable memory.

### Tick and multi-frame reads

As in Feedback, `process()` advances a frame counter at 60 Hz from sample time,
with the same counter rollover convention. The renderer pushes the source under
the counter guard, then reads ages. Unlike Feedback, this ring holds source
history, not the assembled output.

A naive ROWS render of height H touches up to `min(H, depth + 1)` distinct
frames (including age 0). Repeating `frameRingRead()` per line copies an entire
frame under the mutex for every row; even grouping rows by age still copies up
to `depth + 1` whole frames. COLUMNS and LUMA BAND need scattered pixels too.

Recommend a scoped snapshot API that locks the slot once and supplies const
entry pointers, dimensions, counters and count for one assembly operation.
Pointers must never escape the scope, and no push, clear, resize or recursive
source render may happen while the view is held. Render the source first,
push it, then take the view. Sample directly into a preallocated output, release
the lock immediately, and do downstream rendering afterward.

`frameRingReadRows(slot, age, firstRow, rowCount, dst)` would reduce copies for
ROWS, but needs additional dimension/resampling rules and separate column and
pixel APIs. A snapshot handles all three modes and Tape Loop's read in one
consistent view. Measure lock duration; holding it throughout assembly still
blocks another renderer of the same module.

The push guard alone cannot make two differently sized Slit Scan assemblies
identical: rounding row ages and computing luma after resampling can differ.
Add one shared assembled-output cache per ring slot, outside the history entries,
with first-writer publication keyed by tick. The winner assembles at the stored
newest frame's size; later renderers copy/resample that same completed picture.
Serialize cache publication and ensure losers use the winner. Latch controls
for the tick too, so same-tick CV changes do not make the two views disagree.

### Capacity and panel

The right-click menu offers `Buffer: 15 / 30 / 60 / 120 / 300 frames`, default
60. Capacity changes occur on the UI thread and DEPTH's displayed range follows
capacity; keep its stored control normalized so a menu change needs no audio
thread allocation. Save the selected capacity. Shrinking retains newest history.

The history required for depth D is `D + 1` frames. The chosen capacity reserves
that many entry records and, after filling, can retain a full capacity of pixels
even at DEPTH 0. At default capacity 60, maximum depth is 59; see the 60-frame
row above. Add roughly one frame for the shared output cache plus scratch.

Proposed panel: 12 HP, flat light-grey artwork, the standard four-line header (series line,
rule, catalogue number, uppercase title; `addPanelHeader`), medium main knobs, plain input jacks and black plated BUS output, following
`src/panel_style.hpp` without reducing the shared label sizes.

- Row 1: MODE snap knob and DIRECTION switch, with a mode readout below MODE.
- Row 2: DEPTH and SHAPE knobs.
- Row 3: DEPTH CV and SHAPE CV jacks.
- Row 4: DIRECTION gate and a history-fill light.
- Bottom row: BUS IN, green SIG light and BUS output.

Use the shared grid with wider column pitch where words require it; measure
labels, toggle words and plate clearance before fixing coordinates.

### Tests and live check

Add tests to `tests/bus_test.cpp` using small frames and explicit counters.

- Move the oscillator pattern by changing `phase` between pushes; retain each
  expected source frame. At depth N with linear SHAPE, the top ROWS row equals
  the newest frame's top row, and the bottom equals the frame pushed N ticks
  ago. DIRECTION reverses those ages. Check COLUMNS endpoints as well.
- Use a two-tone newest frame and distinguishable historical pictures: LUMA
  BAND selects two expected ages, with inversion reversing them.
- Check SHAPE endpoints, DEPTH 0 bypass, empty history and oldest-frame
  clamping during fill; test capacity shrink and mixed stored dimensions.
- Render twice at the same tick, changing source phase between calls: count
  advances once and a second renderer at another size gets a resampled copy
  of the same assembled output, not a fresh assembly at its own dimensions.

In Rack, use moving Video Oscillator input, then Deflection Yoke upstream; check both
axes, luma mapping, direction gate, depth CV, menu changes, fill on load and two
Displays of different widths. Measure UI response at 60 and 300 frames.

## 3. Tape Loop

Tape Loop records the recent input into a buffer covering several seconds and
plays that history under position, speed and loop control, like a tape machine.
HOLD stops recording while playback continues through the frozen buffer; this
is the tape loop proper.

### Length and memory

Offer 1, 2, 4, 8 and 15 seconds, default 4. At 60 recorded frames per second,
these ask for 60, 120, 240, 480 and 900 frames. The built ring caps at 600, so
900 does not fit. Recommend keeping that maximum and storing every other tick
for the 15-second setting: 450 frames at 30 Hz. Label it `15 s (30 fps)`.
Silently clamping 900 to 600 would give only 10 seconds at 60 Hz.

These are nominal durations: the renderer must actually visit each recording
tick. Use a recording bucket counter for the 30 Hz setting so an odd tick can
record a bucket whose even tick was never drawn. A single stored frame covers
one bucket; no missed pictures are synthesized.

Memory is derived from the same exact byte counts as the first table (MB / GB):

| Setting / stored frames | 640 x 480 | 960 x 720 | 1440 x 1080 | 2048 x 1536 |
|---|---|---|---|---|
| 1 s / 60 | 73.7 / 0.074 | 165.9 / 0.166 | 373.2 / 0.373 | 755.0 / 0.755 |
| 2 s / 120 | 147.5 / 0.147 | 331.8 / 0.332 | 746.5 / 0.746 | 1509.9 / 1.510 |
| 4 s / 240 | 294.9 / 0.295 | 663.6 / 0.664 | 1493.0 / 1.493 | 3019.9 / 3.020 |
| 8 s / 480 | 589.8 / 0.590 | 1327.1 / 1.327 | 2986.0 / 2.986 | 6039.8 / 6.040 |
| 15 s / 450 at 30 Hz | 553.0 / 0.553 | 1244.2 / 1.244 | 2799.4 / 2.799 | 5662.3 / 5.662 |

The default alone costs about 1.5 GB at the actual largest Display size.
Show estimated memory in the length menu; the long high-resolution settings
are deliberate choices, not sensible laptop defaults. Changing length retains
the newest entries that fit but resets the transport and recording bucket;
changing sampling rate should clear history so entry ages keep one time scale.

### Controls

- POSITION knob plus CV scrubs from 0 (newest) to 1 (oldest). A patched
  0..10 V CV sets position directly, clamped, overriding autonomous playback;
  unpatched, moving the knob seeks once and SPEED then resumes from there.
- SPEED knob plus CV spans -2..+2: 1 is normal, 0 stopped, negative reverse,
  2 double speed. CV adds 0.2 speed units per volt, clamped to that range.
- LOOP START and LOOP LENGTH knobs plus CV select a region of configured
  capacity, in frames after conversion from normalized controls; start is
  0..capacity-1 and length is at least 1, trimmed at the oldest boundary.
- LOOP switch enables wrapping within that region; off clamps at the ends.
- STUTTER trigger input and momentary button jump to LOOP START on an edge.
- HOLD gate input and latching button stop recording when either is on;
  playback continues. Releasing the gate leaves the button's latch intact.
- DRY/WET knob mixes live input at 0 with playback at 1, default 1.

### Two clocks and playback arithmetic

`process()` owns the fractional playhead in frames of age, the 60 Hz tick
accumulator and counters, knob-change tracking, button and trigger latches,
and loop arithmetic. These are plain scalar floats, doubles and integers,
with small trigger state; no pixel vectors, allocation or ring locks.
It publishes the playhead as a float and controls as a coherent video-bus
snapshot. The renderer owns all frames, pushes once per eligible recording
counter under the guard, and never advances playback locally.

The age coordinate needs an explicit sign convention. In a frozen buffer,
forward chronology runs from larger ages toward age 0. Recommend
`playhead -= speed` per 60 Hz tick while held; while recording, add 1 for the
new frame shifting the age origin, giving `playhead += 1 - speed`. Thus normal
playback tracks a fixed delay during recording, stopped playback follows the
same stored picture as its age grows, and reverse moves toward older content.
This is a clarification of the brief's unsigned phrase “moves by SPEED frames
per tick”; using `playhead += speed` would make positive speed run backward in
a frozen age-indexed buffer. The speed contribution advances by k frames after
k ticks at SPEED 1, with sign and recording-origin compensation tested separately.

For the 15-second setting, scale both movement terms by one half per tick so
normal playback remains normal at 30 stored frames per second. This necessarily
changes the literal one-stored-frame-per-tick rule for that setting.

For loop start S and integer length L, wrap the continuous playhead with
`S + positiveModulo(playhead - S, L)`. Quantize its offset within the loop
before the read, wrapping the rounded offset too: START 10, LENGTH 5 must read
only ages 10..14 even when a fractional playhead is close to 15. Without LOOP,
clamp to the configured range. POSITION CV has priority over free stepping;
STUTTER wins for its trigger tick and POSITION resumes on the next tick.

The renderer reads `age = round(playhead)` subject to that loop rule and clamps
to actual `count - 1` during fill. Do not query count from `process()`; it uses
configured capacity, while the renderer handles incomplete history. No frames
means live passthrough. Defer blending adjacent ages: nearest-frame playback
is cheaper and preserves crisp stutters; fractional speed intentionally repeats
or skips frames. HOLD leaves ages fixed, so the transport loops in frozen history.

Reuse the shared completed-output cache proposed for Slit Scan, keyed by the
60 Hz playback tick, separately from the recording bucket. Cache the final mix,
including the first renderer's dry frame, for identical results across renderers.
HOLD and 30 Hz recording must not freeze this output cache: playback still
changes at 60 Hz. Latch the published transport and controls per output tick.

### Panel

Proposed panel: 18 HP, using the same shared text and panel vocabulary.
Two rows of three knobs fit the control count without shrinking labels.

- Row 1: POSITION, SPEED and DRY/WET knobs.
- Row 2: LOOP START, LOOP LENGTH knobs and LOOP switch, with LOOP light.
- Row 3: POSITION CV, SPEED CV, START CV and LENGTH CV inputs.
- Row 4: STUTTER input and button, HOLD gate and latching button.
- Bottom row: BUS IN, REC light, SIG light and black plated BUS output.

REC is lit while recording is enabled, LOOP while wrapping is enabled, and
HOLD's button is lit for the effective held state. The REC light signals intent;
like Feedback, an off-screen module does not collect frames. Use two-line
labels where needed, and check every text and control clearance before layout.

### Tests and live check

Add tests to `tests/bus_test.cpp` with small frames whose bytes carry a counter
(use fewer than 256 pushes or encode a wider counter in separate channels).

- POSITION 0 reads newest and POSITION 1 oldest, including partial fill.
- Put transport arithmetic in a pure `src/tape_loop_transport.hpp` helper
  called by the module. Test k ticks at SPEED 1, separately checking the k-frame
  speed contribution, frozen-buffer sign and recording-origin compensation;
  also test 0, -1, 2 and fractional speed, seeking and trigger precedence.
- LOOP START 10, LENGTH 5 keeps all read ages in 10..14 in either direction,
  including rounding near the wrap. Test minimum length and end clamping.
- HOLD prevents pushes: count stops rising in a partially filled ring, and
  newest counter and pixels remain unchanged even when full (count alone
  cannot prove this). Playback must continue and loop inside those frames.
- Two renderers use one recording push and the same completed playback frame,
  with the second size resampled; include HOLD and a playback-only 30 Hz tick.
- Test recording buckets, skipped ticks, menu shrink, dry/wet endpoints, empty
  load, and allocation failure. Run the helper through published params too,
  so a correct pure function cannot hide incorrect bus wiring.

In Rack check scrub CV overriding SPEED, forward and reverse under HOLD,
stutter, loop boundaries, gate/button combinations, dry/wet, 15-second cadence,
save/load empty state and two Displays plus Syphon Out. Observe memory and UI
response at the default length before trying longer buffers.

## 4. Shared work and order

### Ring additions

Add the scoped snapshot view first, exposing entries by age with their sizes,
count, capacity and newest counter under one lock. Add
`frameRingNewestCounter(slot)` with an explicit empty result, and
`frameRingCapacity(slot)` for UI diagnostics and tests; neither is audio-thread
safe. Keep exact-age read semantics and put oldest-age clamping in the module
renderer, where empty-history policy belongs.

Add the per-slot completed-output cache and atomic first-writer publication
operation for source-history modules; clear/release/capacity and recording-rate
changes must invalidate it. Keep cache locking separate in purpose from the
snapshot lifetime: do not reacquire the same mutex recursively. Use a documented
lock order if two mutexes are necessary. Test simultaneous publication, including
the loser returning the winning picture, rather than only serial render calls.

Feedback can remain on its existing one-frame copy API: it does not benefit
from a multi-age view, and its ring already stores completed output. Preserve
its behavior and run its regressions with every shared change. Stronger stale
counter rejection, if added, must handle counter rollover and clear explicitly.

### Build order

1. Ring additions and tests first: extend `src/frame_ring.hpp/.cpp` with the
   scoped view, queries and completed-output cache; append age, mixed-size,
   lock-lifetime, cache, shrink, clear and counter tests to `tests/bus_test.cpp`.
   Document the additions in `docs/architecture.md`. Run `make test`, then
   build/package at the project's existing output paths. In Rack confirm
   Feedback still accumulates, clears and shares a tick across Displays and
   Syphon Out; its live behavior is not yet confirmed by this plan.
2. Slit Scan second: add `src/SlitScan.cpp`, `res/SlitScan.svg` and, if needed,
   a small pure mapping header. Add `SlitScanParams` in `src/video_engine.hpp`;
   append the enum value, `VideoBusNode` field and publish declaration in
   `src/video_bus.hpp`, plus atomic payload load/store and publish function in
   `src/video_bus.cpp`. Add the renderer declaration, scratch and dispatch case
   in `src/video_graph_renderer.hpp/.cpp`. Append model declaration/registration
   in `src/plugin.hpp/.cpp` and the module entry in `plugin.json`. Add the tests
   above and update architecture, module inventory docs. Run
   `make test` and `make dist`, then the Slit Scan Rack checks above. It comes
   first because it has fewer controls and exercises multi-frame reads.
3. Tape Loop third: add `src/TapeLoop.cpp`, `src/tape_loop_transport.hpp` and
   `res/TapeLoop.svg`. Append `TapeLoopParams`, node enum/field, atomic payload
   handling and `videoBusPublishTapeLoop()` in the same engine/bus files;
   append renderer declaration, scratch and case in the renderer files, model
   registration in `src/plugin.hpp/.cpp`, and the entry in `plugin.json`.
   Add transport and renderer tests to `tests/bus_test.cpp`, update architecture,
   module inventory docs, run `make test` and `make dist`, then the
   Tape Loop live checks above.

Existing ids stay unchanged, and `VideoBusNode` stays trivially copyable.
Both modules own their video bus and ring independently, default unpatched
BUS IN to bus 1, and reject direct self-input like Feedback. The steps above
are future implementation work; this change writes only the plan and its
docs pointer.

### Risks

- Large Display sizes multiply memory by buffer length and module count;
  retained allocations and output caches add to the table's pixel costs.
- Whole-frame copies under a mutex can stall the UI thread. Snapshot reads
  avoid repeated copies but still hold a lock during assembly; benchmark that
  hold time and avoid nested rendering while locked.
- The 60 Hz sample-time tick and Rack UI draws are separate clocks. A tick
  drawn twice records once and reuses completed output; a tick never drawn
  records nothing. Slit Scan ages count stored pictures, so its time axis
  becomes uneven and covers more real time when draws are missed. Tape Loop's
  playhead keeps stepping while recording misses pictures, so playback can
  jump or clamp during fill and its advertised seconds are approximate.
  If neither Display nor Syphon Out draws the node, no recording occurs.
- Recording-origin compensation in Tape Loop is nominal when draws are
  skipped: the module cannot know actual pushes through a mutex query.
  A future lock-free scalar receipt from the renderer could correct it;
  do not claim exact stopped-frame retention during missed recording ticks.
- Several differently sized Displays can require resampling every frame;
  first-writer size can alternate across ticks, preserving history but changing
  detail. Shared output caching makes the assembled picture consistent, not
  free to copy or resample.
- Rings are empty after patch load. Pass live input while filling and show
  fill state; a saved HOLD state with an empty buffer cannot restore a loop.

### Open questions

- Should 15 seconds raise the built 600-frame maximum to 900; recommend keeping 600 and offering an explicitly labelled 30 fps setting with 450 frames.
- Should the planning ceiling become an actual 2048 x 1536 Display option despite the current 1080-row clamp; recommend keeping the existing limit for these modules.
- Should Slit Scan include SHAPE at launch; recommend including it with a linear default and the scoped snapshot implementation.
- Should Tape Loop positive speed follow forward chronology toward newer pictures or add positive age literally; recommend forward chronology with recording-origin compensation as specified above.
- Should Tape Loop default to four seconds despite its roughly 1.5 GB history at 1440 x 1080; recommend retaining the requested default with a visible menu memory estimate and testing it on the target machine before release.
