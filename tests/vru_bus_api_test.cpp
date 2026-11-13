// In-process test of the public C interface (include/vru_bus.h): claim, publish a known pattern, read
// it back through the renderer, scaling, a processor chained on an external source, release, bad
// arguments and version mismatch. No Rack needed. Run with `make test`.

#include "vru_bus.h"

#include "video_bus.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" const VruBusApiV1* vruBusGetApi(uint32_t requestedVersion);

static int failures = 0;
static int checks = 0;
#define CHECK(cond) \
	do { \
		++checks; \
		if (!(cond)) { \
			++failures; \
			std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
		} \
	} while (0)

static std::vector<uint8_t> pattern(int w, int h, size_t stride) {
	std::vector<uint8_t> px(stride * h, 0xEE);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			uint8_t* p = &px[y * stride + x * 4];
			p[0] = static_cast<uint8_t>(x * 16);
			p[1] = static_cast<uint8_t>(y * 16);
			p[2] = static_cast<uint8_t>((x + y) * 8);
			p[3] = 255;
		}
	}
	return px;
}

int main() {
	CHECK(vruBusGetApi(0) == nullptr);
	CHECK(vruBusGetApi(2) == nullptr);
	CHECK(vruBusGetApi(0xFFFFFFFFu) == nullptr);
	const VruBusApiV1* api = vruBusGetApi(VRU_BUS_API_VERSION);
	CHECK(api != nullptr);
	if (!api) {
		return 1;
	}
	CHECK(api->version == 1u);
	CHECK(api->structSize == sizeof(VruBusApiV1));

	// Voltage rule matches the internal one.
	CHECK(api->busIdFromVoltage(3.2f) == 3);
	CHECK(api->busIdFromVoltage(0.f) == 0);
	CHECK(api->busIdFromVoltage(-2.f) == 0);
	CHECK(api->busIdFromVoltage(65.f) == 0);
	CHECK(api->busIdFromVoltage(64.f) == 64);
	CHECK(api->busIdFromVoltage(std::nanf("")) == 0);

	// Claim: empty until the first frame.
	const int id = api->claimSource();
	CHECK(id >= 1 && id <= VRU_BUS_ID_MAX);
	CHECK(api->isActive(id) == 0);
	std::vector<uint8_t> out(8 * 8 * 4, 0x55);
	CHECK(api->readFrame(id, 8, 8, out.data(), 32) == VRU_BUS_ERR_NO_SIGNAL);
	CHECK(out[0] == 0x55);

	// Publish an 8x8 pattern with padded stride; read it back at the same size, exact pixels.
	const size_t padded = 8 * 4 + 12;
	std::vector<uint8_t> src = pattern(8, 8, padded);
	CHECK(api->publishFrame(id, src.data(), 8, 8, padded) == VRU_BUS_OK);
	CHECK(api->isActive(id) == 1);
	std::vector<uint8_t> back(8 * 8 * 4, 0);
	CHECK(api->readFrame(id, 8, 8, back.data(), 32) == VRU_BUS_OK);
	bool same = true;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 32; ++x) {
			same = same && back[y * 32 + x] == src[y * padded + x];
		}
	}
	CHECK(same);

	// Read with a padded destination: padding bytes stay untouched.
	std::vector<uint8_t> wide(8 * 64, 0x77);
	CHECK(api->readFrame(id, 8, 8, wide.data(), 64) == VRU_BUS_OK);
	CHECK(wide[32] == 0x77 && wide[63] == 0x77);
	CHECK(wide[0] == src[0] && wide[7 * 64 + 4] == src[7 * padded + 4]);

	// Scaled read: a flat colour picture sampled at another size stays that colour.
	std::vector<uint8_t> flat(4 * 4 * 4);
	for (size_t i = 0; i < flat.size(); i += 4) {
		flat[i] = 200;
		flat[i + 1] = 100;
		flat[i + 2] = 50;
		flat[i + 3] = 255;
	}
	CHECK(api->publishFrame(id, flat.data(), 4, 4, 16) == VRU_BUS_OK);
	std::vector<uint8_t> big(16 * 12 * 4, 0);
	CHECK(api->readFrame(id, 16, 12, big.data(), 64) == VRU_BUS_OK);
	bool flatOk = true;
	for (size_t i = 0; i < big.size(); i += 4) {
		flatOk = flatOk && std::abs(big[i] - 200) <= 1 && std::abs(big[i + 1] - 100) <= 1 && std::abs(big[i + 2] - 50) <= 1;
	}
	CHECK(flatOk);

	// A VRU processor reads the external source through the graph.
	const int proc = videoBusAllocate(0);
	CHECK(proc != 0);
	ColorLabParams lab;
	videoBusPublishColorLab(proc, id, lab);
	std::vector<uint8_t> chained(16 * 12 * 4, 0);
	CHECK(api->readFrame(proc, 16, 12, chained.data(), 64) == VRU_BUS_OK);
	videoBusRelease(proc);

	// Bad arguments.
	CHECK(api->readFrame(id, 8, 8, nullptr, 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->readFrame(id, 0, 8, back.data(), 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->readFrame(id, 8, 8, back.data(), 31) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->readFrame(id, VRU_BUS_MAX_DIMENSION + 1, 8, back.data(), 1 << 20) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->readFrame(0, 8, 8, back.data(), 32) == VRU_BUS_ERR_BAD_ID);
	CHECK(api->readFrame(65, 8, 8, back.data(), 32) == VRU_BUS_ERR_BAD_ID);
	CHECK(api->readFrame(-1, 8, 8, back.data(), 32) == VRU_BUS_ERR_BAD_ID);
	CHECK(api->publishFrame(id, nullptr, 4, 4, 16) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->publishFrame(id, flat.data(), 4, 4, 15) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->publishFrame(id, flat.data(), -4, 4, 16) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(api->publishFrame(0, flat.data(), 4, 4, 16) == VRU_BUS_ERR_BAD_ID);
	CHECK(api->publishFrame(100, flat.data(), 4, 4, 16) == VRU_BUS_ERR_BAD_ID);
	// An id that is not a claimed source (another module's bus) cannot be written or released.
	const int other = videoBusAllocate(0);
	CHECK(api->publishFrame(other, flat.data(), 4, 4, 16) == VRU_BUS_ERR_BAD_ID);
	videoBusPublishExternalFrame(other, 0);
	api->releaseSource(other);
	CHECK(videoBusIsActive(other) == true);
	videoBusRelease(other);

	// Release: the bus goes empty, publishing again fails, a double release is harmless.
	api->releaseSource(id);
	CHECK(api->isActive(id) == 0);
	CHECK(api->readFrame(id, 8, 8, back.data(), 32) == VRU_BUS_ERR_NO_SIGNAL);
	CHECK(api->publishFrame(id, flat.data(), 4, 4, 16) == VRU_BUS_ERR_BAD_ID);
	api->releaseSource(id);

	// Claim until the table is full.
	std::vector<int> ids;
	for (int i = 0; i < VRU_BUS_ID_MAX + 2; ++i) {
		const int c = api->claimSource();
		if (c) {
			ids.push_back(c);
		}
	}
	CHECK(static_cast<int>(ids.size()) == VRU_BUS_ID_MAX);
	CHECK(api->claimSource() == 0);
	for (int c : ids) {
		api->releaseSource(c);
	}
	const int again = api->claimSource();
	CHECK(again != 0);
	api->releaseSource(again);

	std::printf("vru_bus_api_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
