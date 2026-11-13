// Cross-dylib test: loads a VRU library with dlopen and talks to it only through vru_bus.h and the
// one exported symbol, as an outside plugin would. Does not link any VRU code.
// Usage: vru_bus_dylib_test <path to dylib>

#include "vru_bus.h"

#include <dlfcn.h>
#include <cstdio>
#include <vector>

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

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: %s <dylib>\n", argv[0]);
		return 2;
	}
	void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		std::printf("dlopen failed: %s\n", dlerror());
		return 2;
	}
	void* symbol = dlsym(handle, VRU_BUS_GET_API_SYMBOL);
	CHECK(symbol != nullptr);
	if (!symbol) {
		return 1;
	}
	VruBusGetApiFn getApi = reinterpret_cast<VruBusGetApiFn>(symbol);
	CHECK(getApi(99) == nullptr);
	const VruBusApiV1* api = getApi(VRU_BUS_API_VERSION);
	CHECK(api != nullptr);
	if (!api) {
		return 1;
	}
	CHECK(api->version == VRU_BUS_API_VERSION);
	CHECK(api->structSize >= sizeof(VruBusApiV1));

	const int id = api->claimSource();
	CHECK(id >= 1);
	std::vector<uint8_t> px(4 * 4 * 4);
	for (size_t i = 0; i < px.size(); i += 4) {
		px[i] = static_cast<uint8_t>(i);
		px[i + 1] = 9;
		px[i + 2] = 99;
		px[i + 3] = 255;
	}
	CHECK(api->publishFrame(id, px.data(), 4, 4, 16) == VRU_BUS_OK);
	CHECK(api->isActive(id) == 1);
	CHECK(api->busIdFromVoltage(static_cast<float>(id)) == id);
	std::vector<uint8_t> out(4 * 4 * 4, 0);
	CHECK(api->readFrame(id, 4, 4, out.data(), 16) == VRU_BUS_OK);
	CHECK(out == px);
	api->releaseSource(id);
	CHECK(api->isActive(id) == 0);

	std::printf("vru_bus_dylib_test (%s): %d checks, %d failures\n", argv[1], checks, failures);
	dlclose(handle);
	return failures == 0 ? 0 : 1;
}
