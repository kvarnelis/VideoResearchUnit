// The client header (include/vru_bus_client.hpp) and the example client (docs/vru_bus_v2_example.hpp), run against
// the real table in this process, and against a stand-in vruBusGetApi that only knows version 1, as an older VRU does.
// Needs the Rack SDK headers because the client header includes <rack.hpp>; nothing from Rack is called.

#include "vru_bus_client.hpp"
#include "vru_bus_v2_example.hpp"

#include <cstdint>
#include <cstdio>
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

// An older VRU: knows version 1 only. It serves the real version 1 table, so the reads work.
static const VruBusApiV1* oldVru(uint32_t requestedVersion) {
	return requestedVersion == 1u ? vruBusGetApi(1u) : nullptr;
}

// A plugin that answers version 2 with a table that is too small.
static VruBusApiV1 shortTable;
static const VruBusApiV1* brokenVru(uint32_t requestedVersion) {
	shortTable = *vruBusGetApi(1u);
	shortTable.version = requestedVersion;
	return requestedVersion <= 2u ? &shortTable : nullptr;
}

static const VruBusApiV1* noVru(uint32_t) {
	return nullptr;
}

int main() {
	// Newest: version 2 is chosen, and its v1 member is the table the version 1 entries come from.
	const VruBusClient now = vruBusConnectFrom(vruBusGetApi);
	CHECK(now.connected() && now.hasVersion2() && now.hasFloatRead() && now.hasTickRead() && now.hasNativeSize());
	CHECK(now.v1 == &now.v2->v1 && now.v1->version == 2u);

	// Older plugin: falls back to version 1, no float read, and says so.
	const VruBusClient old = vruBusConnectFrom(oldVru);
	CHECK(old.connected() && !old.hasVersion2() && !old.hasFloatRead() && !old.hasTickRead() && !old.hasNativeSize());
	CHECK(old.v1 && old.v1->version == 1u && old.v2 == nullptr);

	// Nothing installed, or a table too small to trust.
	const VruBusClient none = vruBusConnectFrom(noVru);
	CHECK(!none.connected() && !none.hasFloatRead());
	CHECK(!vruBusConnectFrom(nullptr).connected());
	const VruBusClient broken = vruBusConnectFrom(brokenVru);
	CHECK(broken.connected() && !broken.hasVersion2());  // too small for version 2, so it keeps the version 1 table it asked for second

	// The example, on both paths, gives the same picture.
	const int id = now.v1->claimSource();
	std::vector<uint8_t> px(6 * 4 * 4);
	for (size_t i = 0; i < px.size(); ++i) {
		px[i] = static_cast<uint8_t>(i * 11 + 5);
	}
	CHECK(now.v1->publishFrame(id, px.data(), 6, 4, 24) == VRU_BUS_OK);
	std::vector<float> a, b;
	std::vector<uint8_t> scratch;
	uint64_t tickA = 0, tickB = 0;
	CHECK(vruExampleReadFloat(now, id, 6, 4, a, scratch, tickA));
	CHECK(vruExampleReadFloat(old, id, 6, 4, b, scratch, tickB));
	CHECK(a.size() == 96 && a == b);
	bool exact = true;
	for (size_t i = 0; i < a.size(); ++i) {
		exact = exact && a[i] == static_cast<float>(px[i]) / 255.f;
	}
	CHECK(exact);
	CHECK(tickA != 0 && tickB == 0);
	CHECK(!vruExampleReadFloat(none, id, 6, 4, a, scratch, tickA));
	CHECK(!vruExampleReadFloat(now, 0, 6, 4, a, scratch, tickA));
	// Version 2 skips when nothing new: with no tick source every read is a tick, so the next read is new.
	CHECK(vruExampleReadFloat(now, id, 6, 4, a, scratch, tickA));
	now.v1->releaseSource(id);

	std::printf("vru_bus_client_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
