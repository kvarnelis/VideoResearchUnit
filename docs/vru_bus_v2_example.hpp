// A small client of the VRU video bus, version 2 with a fall back to version 1. MIT licensed, like the headers.
// tests/vru_bus_client_test.cpp includes and runs this file on every `make test`, so it cannot go stale.
//
// Call it from the UI thread or a worker thread of your own, never from process().
#pragma once

#include "vru_bus_client.hpp"

#include <cstdint>
#include <vector>

// Reads bus `busId` at width x height into `rgba` as floats in 0..1 (4 per pixel, tight rows). Uses the float
// read when VRU serves version 2, else reads bytes with version 1 and divides by 255 itself. `lastTick` is the tick
// of the previous call for this bus and size (start at 0): with version 2 a call that finds nothing new returns
// false and leaves `rgba` alone, so the caller skips its work. Returns true when `rgba` holds a new picture.
// `scratch` holds the bytes of the version 1 path; both vectors are resized only when the size changes.
inline bool vruExampleReadFloat(const VruBusClient& vru, int busId, int width, int height, std::vector<float>& rgba, std::vector<uint8_t>& scratch, uint64_t& lastTick) {
	if (!vru.connected()) {
		return false;
	}
	const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (rgba.size() != count) {
		rgba.resize(count);
	}
	if (vru.hasTickRead()) {
		VruBusReadInfo info = {sizeof(VruBusReadInfo), 0, lastTick, 0, 0, 0};
		const int status = vru.v2->readFrameF32Ex(busId, width, height, rgba.data(), static_cast<size_t>(width) * 4u, &info);
		if (status != VRU_BUS_OK) {
			return false;  // VRU_BUS_UNCHANGED: nothing new. A negative status: no picture.
		}
		lastTick = info.tick;
		return true;
	}
	if (scratch.size() != count) {
		scratch.resize(count);
	}
	if (vru.v1->readFrame(busId, width, height, scratch.data(), static_cast<size_t>(width) * 4u) != VRU_BUS_OK) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		rgba[i] = static_cast<float>(scratch[i]) / 255.f;
	}
	return true;
}
