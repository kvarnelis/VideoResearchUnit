#include "frame_ring.hpp"
#include "video_slot_allocator.hpp"

#include <algorithm>
#include <array>
#include <mutex>

namespace {
struct RingSlot {
	std::mutex mutex;
	std::vector<FrameRingFrame> entries {1};
	int head = 0;
	int count = 0;
	int clearCounter = 0;
};
std::array<RingSlot, kFrameRingSlotCount> slots;
VideoSlotAllocator<kFrameRingSlotCount> slotIds;

bool validSlotId(int id) {
	return id >= 1 && id <= kFrameRingSlotCount;
}
}

int frameRingAllocateSlot() {
	return slotIds.claim(0);
}

void frameRingReleaseSlot(int slotId) {
	if (!validSlotId(slotId)) return;
	{
		RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
		std::lock_guard<std::mutex> lock(slot.mutex);
		slot.entries = std::vector<FrameRingFrame>(1);
		slot.head = slot.count = slot.clearCounter = 0;
	}
	slotIds.release(slotId);
}

void frameRingSetCapacity(int slotId, int frames) {
	if (!validSlotId(slotId)) return;
	RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	std::lock_guard<std::mutex> lock(slot.mutex);
	frames = std::clamp(frames, 1, kFrameRingMaxCapacity);
	const int oldCapacity = static_cast<int>(slot.entries.size());
	if (frames == oldCapacity) return;
	const int keep = std::min(slot.count, frames);
	std::vector<FrameRingFrame> entries(static_cast<size_t>(frames));
	// Move oldest to newest so the next push can continue at the next entry.
	for (int i = 0; i < keep; ++i) {
		const int index = (slot.head - (keep - 1 - i) + oldCapacity) % oldCapacity;
		entries[static_cast<size_t>(i)] = std::move(slot.entries[static_cast<size_t>(index)]);
	}
	slot.entries.swap(entries);
	slot.head = keep > 0 ? keep - 1 : 0;
	slot.count = keep;
}

bool frameRingPush(int slotId, int frameCounter, const std::vector<unsigned char>& rgba, int width, int height) {
	if (!validSlotId(slotId) || width <= 0 || height <= 0 ||
		rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) return false;
	RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	std::lock_guard<std::mutex> lock(slot.mutex);
	if (slot.count && slot.entries[static_cast<size_t>(slot.head)].frameCounter == frameCounter) return false;
	const int capacity = static_cast<int>(slot.entries.size());
	if (slot.count) slot.head = (slot.head + 1) % capacity;
	FrameRingFrame& entry = slot.entries[static_cast<size_t>(slot.head)];
	entry.rgba.assign(rgba.begin(), rgba.end());
	entry.width = width;
	entry.height = height;
	entry.frameCounter = frameCounter;
	slot.count = std::min(slot.count + 1, capacity);
	return true;
}

bool frameRingIsStored(int slotId, int frameCounter) {
	if (!validSlotId(slotId)) return false;
	RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	std::lock_guard<std::mutex> lock(slot.mutex);
	return slot.count && slot.entries[static_cast<size_t>(slot.head)].frameCounter == frameCounter;
}

bool frameRingRead(int slotId, int age, FrameRingFrame& out) {
	if (!validSlotId(slotId) || age < 0) return false;
	RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	std::lock_guard<std::mutex> lock(slot.mutex);
	if (age >= slot.count) return false;
	const int capacity = static_cast<int>(slot.entries.size());
	out = slot.entries[static_cast<size_t>((slot.head - age + capacity) % capacity)];
	return true;
}

int frameRingCount(int slotId) {
	if (!validSlotId(slotId)) return 0;
	RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	std::lock_guard<std::mutex> lock(slot.mutex);
	return slot.count;
}

void frameRingClear(int slotId, int clearCounter) {
	if (!validSlotId(slotId)) return;
	RingSlot& slot = slots[static_cast<size_t>(slotId - 1)];
	std::lock_guard<std::mutex> lock(slot.mutex);
	if (slot.clearCounter == clearCounter) return;
	// Keep vector capacity for the next pass, but make all frames unavailable to readers.
	for (FrameRingFrame& entry : slot.entries) entry.rgba.clear();
	slot.count = 0;
	slot.head = 0;
	slot.clearCounter = clearCounter;
}
