// Public C interface to the video bus. See include/vru_bus.h and docs/VIDEO-BUS.md.
//
// An external source is an ExternalFrame node (the same node Syphon In and Video Player publish): VRU
// copies the caller's pixels into its external frame store and the renderer samples and scales them
// like any generator. readFrame renders with the same VideoGraphRenderer the Display uses.

#include "vru_bus.h"

#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#define VRU_BUS_EXPORT extern "C" __declspec(dllexport)
#else
#define VRU_BUS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct Source {
	bool claimed = false;
	bool nodePublished = false;
	int frameId = 0;
	std::vector<unsigned char> packed;  // repacked rows when the caller's stride is not tight
};

std::mutex sourceMutex;
std::array<Source, kVideoBusCount + 1> sources;

std::mutex rendererMutex;

bool validDimensions(int width, int height) {
	return width >= 1 && height >= 1 && width <= VRU_BUS_MAX_DIMENSION && height <= VRU_BUS_MAX_DIMENSION;
}

int apiBusIdFromVoltage(float voltage) {
	return videoBusIdFromVoltage(voltage);
}

int apiIsActive(int busId) {
	return videoBusIsActive(busId) ? 1 : 0;
}

int apiReadFrame(int busId, int width, int height, uint8_t* rgba, size_t strideBytes) {
	if (!rgba || !validDimensions(width, height) || strideBytes < static_cast<size_t>(width) * 4u) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	if (busId < 1 || busId > kVideoBusCount) {
		return VRU_BUS_ERR_BAD_ID;
	}

	std::lock_guard<std::mutex> lock(rendererMutex);
	// One renderer for API callers, like one per Display. Created on first use and kept for the life
	// of the process, so its generator and trail state persists between calls.
	static VideoGraphRenderer* renderer = new VideoGraphRenderer();
	const std::vector<unsigned char>& frame = renderer->renderBus(busId, width, height);
	const size_t rowBytes = static_cast<size_t>(width) * 4u;
	if (!renderer->hasSignal() || renderer->hasCycle() || frame.size() < rowBytes * static_cast<size_t>(height)) {
		return VRU_BUS_ERR_NO_SIGNAL;
	}
	for (int y = 0; y < height; ++y) {
		std::memcpy(rgba + static_cast<size_t>(y) * strideBytes, frame.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
	}
	return VRU_BUS_OK;
}

int apiClaimSource() {
	std::lock_guard<std::mutex> lock(sourceMutex);
	const int frameId = videoExternalFrameAllocate();
	if (frameId == 0) {
		return 0;
	}
	const int busId = videoBusAllocate(0);
	if (busId == 0) {
		videoExternalFrameRelease(frameId);
		return 0;
	}
	Source& source = sources[static_cast<size_t>(busId)];
	source.claimed = true;
	source.nodePublished = false;
	source.frameId = frameId;
	return busId;
}

int apiPublishFrame(int busId, const uint8_t* rgba, int width, int height, size_t strideBytes) {
	if (!rgba || !validDimensions(width, height) || strideBytes < static_cast<size_t>(width) * 4u) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	if (busId < 1 || busId > kVideoBusCount) {
		return VRU_BUS_ERR_BAD_ID;
	}

	std::lock_guard<std::mutex> lock(sourceMutex);
	Source& source = sources[static_cast<size_t>(busId)];
	if (!source.claimed) {
		return VRU_BUS_ERR_BAD_ID;
	}

	const size_t rowBytes = static_cast<size_t>(width) * 4u;
	const unsigned char* pixels = rgba;
	if (strideBytes != rowBytes) {
		source.packed.resize(rowBytes * static_cast<size_t>(height));
		for (int y = 0; y < height; ++y) {
			std::memcpy(source.packed.data() + static_cast<size_t>(y) * rowBytes, rgba + static_cast<size_t>(y) * strideBytes, rowBytes);
		}
		pixels = source.packed.data();
	}
	videoExternalFramePublish(source.frameId, pixels, width, height, 0.0);
	if (!source.nodePublished) {
		videoBusPublishExternalFrame(busId, source.frameId);
		source.nodePublished = true;
	}
	return VRU_BUS_OK;
}

void apiReleaseSource(int busId) {
	if (busId < 1 || busId > kVideoBusCount) {
		return;
	}
	std::lock_guard<std::mutex> lock(sourceMutex);
	Source& source = sources[static_cast<size_t>(busId)];
	if (!source.claimed) {
		return;
	}
	videoBusRelease(busId);
	videoExternalFrameRelease(source.frameId);
	source = Source {};
}

const VruBusApiV1 apiV1 = {
	VRU_BUS_API_VERSION,
	static_cast<uint32_t>(sizeof(VruBusApiV1)),
	apiBusIdFromVoltage,
	apiIsActive,
	apiReadFrame,
	apiClaimSource,
	apiPublishFrame,
	apiReleaseSource,
};

}  // namespace

VRU_BUS_EXPORT const VruBusApiV1* vruBusGetApi(uint32_t requestedVersion) {
	return requestedVersion == VRU_BUS_API_VERSION ? &apiV1 : nullptr;
}
