#pragma once

#include <cstddef>
#include <vector>

// Render-side frame history shared by all renderers of a module. Each stored frame costs
// width x height x 4 bytes: 1440 x 1080 is about 6.2 MB, or 373 MB for 60 frames;
// a 4:3 Display at 960 x 720 costs about 2.8 MB per frame.
// Push, read and clear run on renderer (UI) threads under a mutex per slot.
// Allocate, release and setCapacity run on the construction/UI thread.
// Nothing here is called from Module::process().
constexpr int kFrameRingSlotCount = 64;
constexpr int kFrameRingMaxCapacity = 600;

struct FrameRingFrame {
	std::vector<unsigned char> rgba;
	// Optional float32 copy of the picture, in the same 0..255 units, four floats a pixel. Empty unless the frame
	// was pushed with frameRingPushFloat; only Feedback's "fade trails fully to black" mode writes or reads it.
	// It costs width x height x 16 bytes (4.7 MiB at 640 x 480, 23.7 MiB at 1440 x 1080) on top of rgba.
	std::vector<float> rgbaF;
	int width = 0;
	int height = 0;
	int frameCounter = 0;
};

// Full precision stores a Frame Delay's pictures as float32, 16 bytes a pixel. A module's ring holds `frames + 1`
// pictures, and the float store is used only while all of them together stay within this budget per module; above it
// the delay stores bytes (the float picture rounded half up). 256 MiB is ten 1440 x 1080 pictures (frames up to 9) or
// every one of 31 pictures at 640 x 480 (152 MiB); a 30 frame delay at 1440 x 1080 would need 747 MB, so it stores bytes.
constexpr size_t kFrameDelayFloatBudgetBytes = static_cast<size_t>(256) * 1024u * 1024u;
inline bool frameDelayFloatStoreFits(int frames, int width, int height) {
	if (frames < 1 || width < 1 || height < 1) return true;
	return static_cast<size_t>(frames + 1) * static_cast<size_t>(width) * static_cast<size_t>(height) * 16u <= kFrameDelayFloatBudgetBytes;
}
// The renderer notes here that a slot is storing bytes only because the float budget would be passed, so the module
// can say so (tooltip, menu). Cleared when the slot is released.
void frameRingSetByteFallback(int slotId, bool fallback);
bool frameRingByteFallback(int slotId);

// Returns 0 when all slots are in use.
int frameRingAllocateSlot();
void frameRingReleaseSlot(int slotId);
// Clamps capacity to 1..600; shrinking retains the newest frames that fit.
void frameRingSetCapacity(int slotId, int frames);
// First writer for a counter wins, including its render size. Reuses entry storage.
bool frameRingPush(int slotId, int frameCounter, const std::vector<unsigned char>& rgba, int width, int height);
// As frameRingPush, and the entry also keeps rgbaF (width x height x 4 floats, 0..255). frameRingPush on an entry
// frees its float plane, so a module that stops using it gives the memory back at its next store.
bool frameRingPushFloat(int slotId, int frameCounter, const std::vector<unsigned char>& rgba, const std::vector<float>& rgbaF, int width, int height);
// A float-only entry: rgbaF (width x height x 4 floats, 0..255) with an empty byte plane, so the entry costs
// width x height x 16 bytes, not x 20. Full precision uses it for Frame Delay; a reader that wants bytes rounds the
// floats half up. Same first-writer-wins rule as frameRingPush.
bool frameRingPushFloatOnly(int slotId, int frameCounter, const std::vector<float>& rgbaF, int width, int height);
bool frameRingIsStored(int slotId, int frameCounter);
// Age 0 is newest. Copies into out under the slot mutex.
bool frameRingRead(int slotId, int age, FrameRingFrame& out);
int frameRingCount(int slotId);
// The newest stored picture that is at least minAge ticks older than frameCounter, copied under the slot mutex.
// Age is frameCounter minus the stored counter, taken modulo the counter's wrap (2^30), so it survives the wrap;
// a stored counter that would be in the future (age beyond half the wrap) is ignored. False when none qualifies.
bool frameRingReadAtLeastAge(int slotId, int frameCounter, int minAge, FrameRingFrame& out);
// The Frame Delay's read. As frameRingReadAtLeastAge, with one difference: once the slot has given a picture this way
// since its last clear, a read that finds none old enough gives the oldest stored picture instead of failing. So a
// delay that is made longer shows its oldest picture (a short freeze) until one is old enough, where it used to go
// black for as many ticks as it was lengthened by. Before the first picture (the start, a clear, a new slot) it
// fails exactly as frameRingReadAtLeastAge does.
bool frameRingReadDelayed(int slotId, int frameCounter, int minAge, FrameRingFrame& out);
// Each clear counter is handled once; a new push can follow the clear immediately.
void frameRingClear(int slotId, int clearCounter);
