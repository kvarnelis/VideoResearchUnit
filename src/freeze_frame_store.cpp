#include "freeze_frame_store.hpp"
#include "video_slot_allocator.hpp"

#include <array>
#include <mutex>

namespace {
struct FreezeSlot {
	FreezeFrameData frame;
	bool active = false;
	int seenCounter = 0;
};

std::array<FreezeSlot, kFreezeFrameSlotCount> slots;
std::mutex frameMutex;
VideoSlotAllocator<kFreezeFrameSlotCount> slotIds;

bool validSlotId(int slotId) {
	return slotId >= 1 && slotId <= kFreezeFrameSlotCount;
}
}

int freezeFrameAllocateSlot() {
	return slotIds.claim(0);
}

void freezeFrameReleaseSlot(int slotId) {
	if (!validSlotId(slotId)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		slots[static_cast<size_t>(slotId - 1)] = FreezeSlot {};
	}
	slotIds.release(slotId);
}

bool freezeFrameCaptureIfNew(int slotId, int captureCounter, const std::vector<unsigned char>& rgba, int width, int height) {
	if (!validSlotId(slotId) || captureCounter == 0 || rgba.empty() || width <= 0 || height <= 0) {
		return false;
	}
	std::lock_guard<std::mutex> lock(frameMutex);
	FreezeSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	if (captureCounter == slot.seenCounter) {
		return false;
	}
	slot.seenCounter = captureCounter;
	slot.frame.rgba.assign(rgba.begin(), rgba.end());
	slot.frame.width = width;
	slot.frame.height = height;
	slot.frame.captureCounter = captureCounter;
	slot.active = true;
	return true;
}

void freezeFrameClear(int slotId, int captureCounter) {
	if (!validSlotId(slotId)) {
		return;
	}
	std::lock_guard<std::mutex> lock(frameMutex);
	FreezeSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	slot.active = false;
	slot.frame.rgba.clear();
	slot.frame.width = 0;
	slot.frame.height = 0;
	slot.seenCounter = captureCounter;
}

bool freezeFrameRead(int slotId, FreezeFrameData& frame) {
	if (!validSlotId(slotId)) {
		return false;
	}
	std::lock_guard<std::mutex> lock(frameMutex);
	const FreezeSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	if (!slot.active) {
		return false;
	}
	frame = slot.frame;
	return !frame.rgba.empty();
}
