#pragma once

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
	int width = 0;
	int height = 0;
	int frameCounter = 0;
};

// Returns 0 when all slots are in use.
int frameRingAllocateSlot();
void frameRingReleaseSlot(int slotId);
// Clamps capacity to 1..600; shrinking retains the newest frames that fit.
void frameRingSetCapacity(int slotId, int frames);
// First writer for a counter wins, including its render size. Reuses entry storage.
bool frameRingPush(int slotId, int frameCounter, const std::vector<unsigned char>& rgba, int width, int height);
bool frameRingIsStored(int slotId, int frameCounter);
// Age 0 is newest. Copies into out under the slot mutex.
bool frameRingRead(int slotId, int age, FrameRingFrame& out);
int frameRingCount(int slotId);
// Each clear counter is handled once; a new push can follow the clear immediately.
void frameRingClear(int slotId, int clearCounter);
