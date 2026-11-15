#pragma once
#include <vector>

// Frame store for Freeze Frame modules. Slots are claimed per module instance and released
// in its destructor, so no two live modules share a slot.
constexpr int kFreezeFrameSlotCount = 64;

struct FreezeFrameData {
	std::vector<unsigned char> rgba;
	// A picture captured in full precision is held as float32 (0..255 units, width x height x 16 bytes: 4.7 MiB at
	// 640 x 480, 23.7 MiB at 1440 x 1080) with rgba empty. A slot holds one or the other, never both.
	std::vector<float> rgbaF;
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
// As freezeFrameCaptureIfNew, for a float picture (full precision); the slot then holds floats and no bytes.
bool freezeFrameCaptureFloatIfNew(int slotId, int captureCounter, const std::vector<float>& rgbaF, int width, int height);
// True when freezeFrameCaptureIfNew() would take a picture for this counter (so a caller whose picture needs
// converting first can skip the conversion on every other frame).
bool freezeFrameWouldCapture(int slotId, int captureCounter);
// Empties the slot and records captureCounter as handled, so the current capture is not taken again.
void freezeFrameClear(int slotId, int captureCounter);
// Copies the held frame (the byte plane or the float plane, whichever was captured). False when the slot holds nothing.
bool freezeFrameRead(int slotId, FreezeFrameData& frame);
