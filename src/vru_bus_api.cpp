// Public C interface to the video bus. See include/vru_bus.h and docs/VIDEO-BUS.md.
//
// An external source is an ExternalFrame node (the same node Syphon In and Video Player publish): VRU
// copies the caller's pixels into its external frame store and the renderer samples and scales them
// like any generator. readFrame asks the plugin's render service (src/render_service.hpp), the one the Display uses.

#include "vru_bus.h"

#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "render_service.hpp"

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

	// The render service draws it and copies it into the caller's buffer while holding its own mutex, so a
	// read from a foreign thread is safe beside the UI thread's Display draws (src/render_service.hpp).
	return renderService().readCopy(busId, width, height, rgba, strideBytes) ? VRU_BUS_OK : VRU_BUS_ERR_NO_SIGNAL;
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

bool validInfo(const VruBusReadInfo* info) {
	return info && info->structSize >= sizeof(VruBusReadInfo);
}

// Shared by the 8-bit and the float reads of version 2. `info` may be null (the plain float read).
int readV2(int busId, int width, int height, uint8_t* bytes, size_t strideBytes, float* floats, size_t strideFloats, VruBusReadInfo* info) {
	if (info) {
		info->tick = 0;
		info->isNew = 0;
		info->reserved = 0;
	}
	const size_t stride = bytes ? strideBytes : strideFloats;
	if ((!bytes && !floats) || !validDimensions(width, height) || stride < static_cast<size_t>(width) * 4u) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	if (busId < 1 || busId > kVideoBusCount) {
		return VRU_BUS_ERR_BAD_ID;
	}
	RenderService::ReadRequest request;
	request.busId = busId;
	request.width = width;
	request.height = height;
	request.bytes = bytes;
	request.strideBytes = strideBytes;
	request.floats = floats;
	request.strideFloats = strideFloats;
	if (info) {
		request.sinceTick = info->sinceTick;
		request.copyAlways = (info->flags & VRU_BUS_READ_COPY_ALWAYS) != 0;
	}
	const RenderService::ReadOutcome outcome = renderService().read(request);
	if (outcome.status == RenderService::ReadStatus::BadArgument) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	if (outcome.status == RenderService::ReadStatus::NoSignal) {
		return VRU_BUS_ERR_NO_SIGNAL;
	}
	if (info) {
		info->tick = outcome.tick;
		info->isNew = outcome.isNew ? 1 : 0;
	}
	return outcome.status == RenderService::ReadStatus::Unchanged ? VRU_BUS_UNCHANGED : VRU_BUS_OK;
}

int apiReadFrameF32(int busId, int width, int height, float* rgba, size_t strideFloats) {
	return readV2(busId, width, height, nullptr, 0, rgba, strideFloats, nullptr);
}

int apiReadFrameEx(int busId, int width, int height, uint8_t* rgba, size_t strideBytes, VruBusReadInfo* info) {
	if (!validInfo(info)) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	return readV2(busId, width, height, rgba, strideBytes, nullptr, 0, info);
}

int apiReadFrameF32Ex(int busId, int width, int height, float* rgba, size_t strideFloats, VruBusReadInfo* info) {
	if (!validInfo(info)) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	return readV2(busId, width, height, nullptr, 0, rgba, strideFloats, info);
}

int apiGetNativeSize(int busId, VruBusNativeSize* out) {
	if (!out || out->structSize < sizeof(VruBusNativeSize)) {
		return VRU_BUS_ERR_BAD_ARGUMENT;
	}
	out->width = 0;
	out->height = 0;
	out->reserved = 0;
	if (busId < 1 || busId > kVideoBusCount) {
		return VRU_BUS_ERR_BAD_ID;
	}
	VideoBusNode node;
	if (!videoBusReadNode(busId, node) || node.type == VideoBusNodeType::Empty) {
		return VRU_BUS_ERR_NO_SIGNAL;
	}
	if (node.type != VideoBusNodeType::ExternalFrame) {
		return VRU_BUS_NO_NATIVE_SIZE;
	}
	int width = 0;
	int height = 0;
	if (!videoExternalFrameSize(node.externalFrameId, width, height)) {
		return VRU_BUS_ERR_NO_SIGNAL;
	}
	out->width = width;
	out->height = height;
	return VRU_BUS_OK;
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

// Version 2: the version 1 entries in the same order, then the new ones. Its first member carries version 2.
const VruBusApiV2 apiV2 = {
	{
		VRU_BUS_API_VERSION_2,
		static_cast<uint32_t>(sizeof(VruBusApiV2)),
		apiBusIdFromVoltage,
		apiIsActive,
		apiReadFrame,
		apiClaimSource,
		apiPublishFrame,
		apiReleaseSource,
	},
	apiReadFrameF32,
	apiReadFrameEx,
	apiReadFrameF32Ex,
	apiGetNativeSize,
};

}  // namespace

// The declared return type stays const VruBusApiV1* as in version 1. A version 2 request returns the address of a
// VruBusApiV2, whose first member is a VruBusApiV1, so the pointer is a valid VruBusApiV1* and casts to VruBusApiV2*.
VRU_BUS_EXPORT const VruBusApiV1* vruBusGetApi(uint32_t requestedVersion) {
	if (requestedVersion == VRU_BUS_API_VERSION_2) {
		return &apiV2.v1;
	}
	return requestedVersion == VRU_BUS_API_VERSION ? &apiV1 : nullptr;
}
