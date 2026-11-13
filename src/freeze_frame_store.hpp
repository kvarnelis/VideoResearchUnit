#pragma once
#include <vector>

// Frame store for Freeze Frame modules. Slots are claimed per module instance and released
// in its destructor, so no two live modules share a slot.
constexpr int kFreezeFrameSlotCount = 64;

struct FreezeFrameData {
	std::vector<unsigned char> rgba;
	int width = 0;
	int height = 0;
	int captureCounter = -1;
};

// Returns 0 when every slot is in use.
int freezeFrameAllocateSlot();
// Empties the slot and makes its id available again.
void freezeFrameReleaseSlot(int slotId);
// Latch: stores rgba only when captureCounter is non-zero and differs from the last counter
// this slot handled. Repeated renders of the same counter do not re-capture.
bool freezeFrameCaptureIfNew(int slotId, int captureCounter, const std::vector<unsigned char>& rgba, int width, int height);
// Empties the slot and records captureCounter as handled, so the current capture is not taken again.
void freezeFrameClear(int slotId, int captureCounter);
// Copies the held frame. False when the slot holds nothing.
bool freezeFrameRead(int slotId, FreezeFrameData& frame);
