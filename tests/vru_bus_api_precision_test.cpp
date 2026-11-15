// The float read of the public API (include/vru_bus.h, readFrameF32 / readFrameF32Ex) with the plugin setting "Full
// precision between modules". The rule under test is written in src/render_service.hpp ("The float plane"):
//   - precision off: a float read is exactly byte / 255;
//   - precision on, and a float reader has asked for this bus and size: the read carries the picture before rounding,
//     and the byte read of the same picture is that picture rounded half up;
//   - the first float read after a cold start is full precision only if it is the read that renders the picture; a read
//     that joins a picture already finished without a plane (drawn by a UI consumer earlier in the tick, or published by
//     the render thread before the ask) is byte-derived, and the next picture has the plane;
//   - a reader that stops asking stops costing memory after kFloatKeepTicks ticks; a byte reader never costs any.
// Both service modes. The graph is the one of tests/precision_test.cpp: Ramps -> Color Lab 0.25 -> Color Lab 2.0, which
// has 63 levels as bytes with the setting off and about 128 with it on. A float read at full precision is not limited to
// levels: its 2048 pixels of ramp are all (nearly) different values. No Rack.

#include "vru_bus.h"

#include "render_service.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
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

static std::atomic<uint64_t> gTick{1};
static uint64_t tickSource() {
	return gTick.load();
}

static const VruBusApiV1* v1 = nullptr;
static const VruBusApiV2* v2 = nullptr;

static const int kW = 2048;
static const int kH = 4;
static const int kBus = 3;

static void buildGraph() {
	for (int bus = 1; bus <= kVideoBusCount; ++bus) {
		videoBusClear(bus);
	}
	RampsParams ramps;
	ramps.axisA = 0;
	ramps.freqA = 1.f;
	ramps.mix = 0;
	videoBusPublishRamps(1, ramps);
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(2, 1, quarter);
	ColorLabParams twice;
	twice.brightness = 2.f;
	videoBusPublishColorLab(3, 2, twice);
}

static void setup(bool threadMode, bool precision) {
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	svc.setHiddenRendering(true);
	svc.clear();
	svc.setThreadAutoStart(!threadMode);  // thread mode: ticks are driven by runTickForTest()
	svc.setTickSource(tickSource);
	svc.setFullPrecision(precision);
	svc.setThreadMode(threadMode);
	buildGraph();
}

static void teardown() {
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	svc.setThreadAutoStart(true);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) {
		videoBusClear(bus);
	}
	svc.setTickSource(nullptr);
	svc.clear();
}

static std::vector<uint8_t> readBytes(int* status = nullptr) {
	std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4, 0x55);
	const int s = v1->readFrame(kBus, kW, kH, px.data(), static_cast<size_t>(kW) * 4);
	if (status) {
		*status = s;
	}
	return px;
}

static std::vector<float> readFloats(int* status = nullptr) {
	std::vector<float> px(static_cast<size_t>(kW) * kH * 4, -9.f);
	const int s = v2->readFrameF32(kBus, kW, kH, px.data(), static_cast<size_t>(kW) * 4);
	if (status) {
		*status = s;
	}
	return px;
}

static int levelsOfBytes(const std::vector<uint8_t>& px) {
	std::set<int> seen;
	for (int x = 0; x < kW; ++x) {
		seen.insert(px[static_cast<size_t>(x) * 4u]);
	}
	return static_cast<int>(seen.size());
}

static int levelsOfFloats(const std::vector<float>& px) {
	std::set<float> seen;
	for (int x = 0; x < kW; ++x) {
		seen.insert(px[static_cast<size_t>(x) * 4u]);
	}
	return static_cast<int>(seen.size());
}

// Every float is exactly byte / 255.
static bool isByteDerived(const std::vector<float>& f, const std::vector<uint8_t>& b) {
	for (size_t i = 0; i < b.size(); ++i) {
		if (f[i] != static_cast<float>(b[i]) / 255.f) {
			return false;
		}
	}
	return true;
}

// The byte is the float rounded half up (0.001 level of slack for the divide by 255).
static bool bytesAreRoundedFloats(const std::vector<float>& f, const std::vector<uint8_t>& b) {
	for (size_t i = 0; i < b.size(); ++i) {
		const double level = static_cast<double>(f[i]) * 255.0;
		if (std::fabs(level - static_cast<double>(b[i])) > 0.5 + 1e-3) {
			return false;
		}
	}
	return true;
}

// Precision off: the float read stays exactly byte / 255, in both modes.
static void precisionOff(bool threadMode) {
	std::printf("float read, precision off (render thread %s)\n", threadMode ? "on" : "off");
	setup(threadMode, false);
	RenderService& svc = renderService();
	for (int round = 0; round < 3; ++round) {
		++gTick;
		int status = 0;
		std::vector<float> f = readFloats(&status);
		if (threadMode) {
			if (status != VRU_BUS_OK) {
				svc.runTickForTest();
				f = readFloats(&status);
			}
		}
		CHECK(status == VRU_BUS_OK);
		const std::vector<uint8_t> b = readBytes();
		CHECK(isByteDerived(f, b));
		CHECK(levelsOfFloats(f) >= 60 && levelsOfFloats(f) <= 66);  // 63: the 8-bit path
		CHECK(svc.floatPlaneBytes() == 0);
		if (threadMode) {
			svc.runTickForTest();
		}
	}
	teardown();
}

// UI-driven mode, the read renders the picture itself: full precision at once.
static void uiCold() {
	std::printf("float read, precision on, UI-driven, the read renders the picture\n");
	setup(false, true);
	RenderService& svc = renderService();
	++gTick;
	int status = 0;
	const std::vector<float> f = readFloats(&status);
	CHECK(status == VRU_BUS_OK);
	const int levelsF = levelsOfFloats(f);
	const std::vector<uint8_t> b = readBytes();
	const int levelsB = levelsOfBytes(b);
	std::printf("  distinct levels: float read %d, byte read %d\n", levelsF, levelsB);
	CHECK(levelsB >= 126 && levelsB <= 130);
	CHECK(levelsF >= 126);  // 2048 pixels of a ramp to about half scale: every pixel is its own value
	CHECK(bytesAreRoundedFloats(f, b));
	CHECK(!isByteDerived(f, b));
	CHECK(svc.floatPlaneBytes() > 0);
	teardown();
}

// UI-driven mode, a UI consumer drew the bus first in every tick (Display): the first float read joins that picture and is
// byte-derived; the next tick's picture has the plane.
static void uiJoin() {
	std::printf("float read, precision on, UI-driven, joining a picture a UI consumer drew\n");
	setup(false, true);
	RenderService& svc = renderService();
	int owner = 0;
	++gTick;
	{
		const RenderService::Result shown = svc.frame(&owner, kBus, kW, kH);
		CHECK(shown.signal);
		int status = 0;
		const std::vector<float> f = readFloats(&status);
		CHECK(status == VRU_BUS_OK);
		const std::vector<uint8_t> b = readBytes();
		CHECK(isByteDerived(f, b));  // cold start: byte-derived
		CHECK(levelsOfFloats(f) <= 130);
		// The UI consumer's own picture is the bytes it always got.
		CHECK(shown.pixels->size() == b.size());
		bool same = true;
		for (size_t i = 0; i < b.size(); ++i) {
			same = same && (*shown.pixels)[i] == b[i];
		}
		CHECK(same);
	}
	std::vector<uint8_t> shownBytes;
	for (int tick = 0; tick < 3; ++tick) {
		++gTick;
		const RenderService::Result shown = svc.frame(&owner, kBus, kW, kH);
		shownBytes.assign(shown.pixels->begin(), shown.pixels->end());
		int status = 0;
		const std::vector<float> f = readFloats(&status);
		const std::vector<uint8_t> b = readBytes();
		CHECK(status == VRU_BUS_OK);
		CHECK(levelsOfFloats(f) >= 1000);
		CHECK(levelsOfBytes(b) <= 130);
		CHECK(bytesAreRoundedFloats(f, b));
		CHECK(shownBytes == b);  // the Display got the same rounded bytes
	}
	teardown();
}

// A float reader that stops asking stops costing memory; a byte reader never costs any.
static void lapse(bool threadMode) {
	std::printf("float plane memory lapses (render thread %s)\n", threadMode ? "on" : "off");
	setup(threadMode, true);
	RenderService& svc = renderService();
	int owner = 0;
	// Bytes only: no float plane memory at all.
	for (int n = 0; n < 5; ++n) {
		++gTick;
		if (threadMode) {
			(void)readBytes();
			svc.runTickForTest();
		}
		else {
			(void)svc.frame(&owner, kBus, kW, kH);
			(void)readBytes();
		}
	}
	CHECK(svc.floatPlaneBytes() == 0);
	// A float reader: the plane is kept.
	for (int n = 0; n < 4; ++n) {
		++gTick;
		if (threadMode) {
			(void)readFloats();
			svc.runTickForTest();
		}
		else {
			(void)svc.frame(&owner, kBus, kW, kH);
			(void)readFloats();
		}
	}
	const size_t held = svc.floatPlaneBytes();
	std::printf("  with a float reader: %zu bytes of float plane\n", held);
	CHECK(held >= static_cast<size_t>(kW) * kH * 4u * sizeof(float));
	// It stops. The byte consumer goes on. After kFloatKeepTicks (plus a few for the pool's spares) the memory is gone.
	int ticks = 0;
	for (; ticks < static_cast<int>(RenderService::kFloatKeepTicks) + 8; ++ticks) {
		++gTick;
		if (threadMode) {
			(void)readBytes();
			svc.runTickForTest();
		}
		else {
			(void)svc.frame(&owner, kBus, kW, kH);
		}
		if (ticks == static_cast<int>(RenderService::kFloatKeepTicks) - 20) {
			CHECK(svc.floatPlaneBytes() > 0);  // not yet: the ask has not lapsed
		}
	}
	std::printf("  %d ticks after the last float read: %zu bytes of float plane\n", ticks, svc.floatPlaneBytes());
	CHECK(svc.floatPlaneBytes() == 0);
	// And a float read afterwards is byte-derived at first, then full again.
	++gTick;
	int status = 0;
	std::vector<float> f = readFloats(&status);
	if (threadMode && status != VRU_BUS_OK) {
		svc.runTickForTest();
		f = readFloats(&status);
	}
	CHECK(status == VRU_BUS_OK);
	for (int n = 0; n < 3; ++n) {
		++gTick;
		if (threadMode) {
			svc.runTickForTest();
		}
		else {
			(void)svc.frame(&owner, kBus, kW, kH);
		}
		f = readFloats(&status);
	}
	CHECK(levelsOfFloats(f) >= 1000);
	teardown();
}

// Thread mode: the thread publishes a picture without a plane, then a float read asks, then the next tick has the plane.
static void threadCold() {
	std::printf("float read, precision on, render thread, cold start\n");
	setup(true, true);
	RenderService& svc = renderService();
	// A byte read registers the bus; the first tick publishes a picture with no plane.
	CHECK(readBytes().size() != 0);
	svc.runTickForTest();
	int status = 0;
	std::vector<uint8_t> b = readBytes(&status);
	CHECK(status == VRU_BUS_OK);
	CHECK(levelsOfBytes(b) >= 60 && levelsOfBytes(b) <= 130);
	std::vector<float> f = readFloats(&status);
	CHECK(status == VRU_BUS_OK);
	CHECK(isByteDerived(f, b));  // the picture was finished before the ask
	CHECK(levelsOfFloats(f) <= 130);
	svc.runTickForTest();  // the next picture has its plane
	f = readFloats(&status);
	b = readBytes();
	CHECK(status == VRU_BUS_OK);
	std::printf("  distinct levels: float read %d, byte read %d\n", levelsOfFloats(f), levelsOfBytes(b));
	CHECK(levelsOfFloats(f) >= 1000);
	CHECK(levelsOfBytes(b) <= 130);
	CHECK(bytesAreRoundedFloats(f, b));
	// The Ex read and copyAlways give the same plane.
	VruBusReadInfo info = {sizeof(VruBusReadInfo), VRU_BUS_READ_COPY_ALWAYS, 0, 0, 0, 0};
	std::vector<float> g(static_cast<size_t>(kW) * kH * 4, -9.f);
	CHECK(v2->readFrameF32Ex(kBus, kW, kH, g.data(), static_cast<size_t>(kW) * 4, &info) == VRU_BUS_OK);
	CHECK(levelsOfFloats(g) >= 1000);
	teardown();
}

// A picture held by a consumer keeps its float plane unchanged while new ones are made.
static void heldPicture() {
	std::printf("a held picture and its float plane stay unchanged\n");
	setup(false, true);
	RenderService& svc = renderService();
	int owner = 0;
	std::vector<float> keep;
	RenderService::Result held;
	for (int n = 0; n < 6; ++n) {
		++gTick;
		held = svc.frame(&owner, kBus, kW, kH);
		(void)readFloats();
		if (n == 3) {
			break;
		}
	}
	CHECK(held.pixels && held.pixels->precise.size() == held.pixels->size());
	keep = held.pixels->precise;
	const std::vector<uint8_t> keepBytes(held.pixels->begin(), held.pixels->end());
	for (int n = 0; n < 40; ++n) {
		++gTick;
		(void)svc.frame(&owner, kBus, kW, kH);
		(void)readFloats();
	}
	CHECK(held.pixels->precise == keep);
	CHECK(std::vector<uint8_t>(held.pixels->begin(), held.pixels->end()) == keepBytes);
	teardown();
}

int main() {
	v1 = vruBusGetApi(VRU_BUS_API_VERSION);
	v2 = reinterpret_cast<const VruBusApiV2*>(vruBusGetApi(VRU_BUS_API_VERSION_2));
	CHECK(v1 && v2);
	if (!v1 || !v2) {
		return 1;
	}
	precisionOff(false);
	precisionOff(true);
	uiCold();
	uiJoin();
	threadCold();
	lapse(false);
	lapse(true);
	heldPicture();
	std::printf("vru_bus_api_precision_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
