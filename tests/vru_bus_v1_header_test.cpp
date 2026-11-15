// A version 1 client built against the FROZEN version 1 header (tests/data/vru_bus_v1.h, copied from git at
// eff1729, before version 2 existed) talks to the current plugin code. It must see the same table as ever.
// Built with -Itests/data and without -Iinclude, so the include below is the frozen file.

#include "vru_bus_v1.h"

#include <cstdint>
#include <cstdio>
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

int main() {
	CHECK(VRU_BUS_API_VERSION == 1u);
	CHECK(vruBusGetApi(0) == nullptr);
	CHECK(vruBusGetApi(99) == nullptr);
	const VruBusApiV1* api = vruBusGetApi(VRU_BUS_API_VERSION);
	CHECK(api != nullptr);
	if (!api) {
		return 1;
	}
	CHECK(api->version == 1u);
	CHECK(api->structSize == sizeof(VruBusApiV1));
	CHECK(api->structSize == 8 + 6 * sizeof(void*));
	CHECK(api->busIdFromVoltage(3.2f) == 3 && api->busIdFromVoltage(0.f) == 0 && api->busIdFromVoltage(65.f) == 0);

	const int id = api->claimSource();
	CHECK(id >= 1);
	std::vector<uint8_t> px(5 * 3 * 4);
	for (size_t i = 0; i < px.size(); ++i) {
		px[i] = static_cast<uint8_t>(i * 7 + 3);
	}
	CHECK(api->publishFrame(id, px.data(), 5, 3, 20) == VRU_BUS_OK);
	CHECK(api->isActive(id) == 1);
	std::vector<uint8_t> out(5 * 3 * 4, 0);
	CHECK(api->readFrame(id, 5, 3, out.data(), 20) == VRU_BUS_OK);
	CHECK(out == px);
	std::vector<uint8_t> keep(8, 0x5A);
	CHECK(api->readFrame(id, 0, 3, keep.data(), 20) == VRU_BUS_ERR_BAD_ARGUMENT && keep[0] == 0x5A);
	CHECK(api->readFrame(0, 5, 3, out.data(), 20) == VRU_BUS_ERR_BAD_ID);
	CHECK(api->readFrame(60, 5, 3, out.data(), 20) == VRU_BUS_ERR_NO_SIGNAL);
	api->releaseSource(id);
	CHECK(api->isActive(id) == 0);

	std::printf("vru_bus_v1_header_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
