#include "video_external_frame.hpp"
#include "video_slot_allocator.hpp"

#include <array>
#include <cstddef>
#include <mutex>

namespace {
std::array<ExternalVideoFrame, kExternalFrameCount> frames;
std::array<bool, kExternalFrameCount> active {};
std::mutex frameMutex;
VideoSlotAllocator<kExternalFrameCount> frameIds;

bool validFrameId(int frameId) {
	return frameId >= 1 && frameId <= kExternalFrameCount;
}
}

int videoExternalFrameAllocate() {
	return frameIds.claim(0);
}

void videoExternalFrameRelease(int frameId) {
	if (!validFrameId(frameId)) {
		return;
	}
	videoExternalFrameClear(frameId);
	frameIds.release(frameId);
}

void videoExternalFramePublish(int frameId, const unsigned char* rgba, int width, int height, double timestampSeconds) {
	if (!validFrameId(frameId) || !rgba || width <= 0 || height <= 0) {
		return;
	}

	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	std::lock_guard<std::mutex> lock(frameMutex);
	ExternalVideoFrame& frame = frames[static_cast<size_t>(frameId - 1)];
	frame.rgba.assign(rgba, rgba + byteCount);
	frame.width = width;
	frame.height = height;
	frame.timestampSeconds = timestampSeconds;
	active[static_cast<size_t>(frameId - 1)] = true;
}

bool videoExternalFrameRead(int frameId, ExternalVideoFrame& frame) {
	if (!validFrameId(frameId)) {
		return false;
	}

	std::lock_guard<std::mutex> lock(frameMutex);
	if (!active[static_cast<size_t>(frameId - 1)]) {
		return false;
	}

	frame = frames[static_cast<size_t>(frameId - 1)];
	return !frame.rgba.empty() && frame.width > 0 && frame.height > 0;
}

void videoExternalFrameClear(int frameId) {
	if (!validFrameId(frameId)) {
		return;
	}

	std::lock_guard<std::mutex> lock(frameMutex);
	active[static_cast<size_t>(frameId - 1)] = false;
	frames[static_cast<size_t>(frameId - 1)] = ExternalVideoFrame {};
}
