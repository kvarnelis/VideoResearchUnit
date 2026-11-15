// Version 2 of the public C interface (include/vru_bus.h): the table layout against version 1, the float read, the
// tick, the native size and the bad-argument rules, in both service modes (UI-driven and render thread). No Rack.
// The version 1 behaviour is pinned by the other API tests, which this change leaves as they were.

#include "vru_bus.h"

#include "render_service.hpp"
#include "video_bus.hpp"
#include "video_external_frame.hpp"

#include <atomic>
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

static std::atomic<uint64_t> gTick{1};
static uint64_t tickSource() {
	return gTick.load();
}

typedef std::vector<uint8_t> Bytes;

static Bytes makePicture(int w, int h, int salt) {
	Bytes px(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < px.size(); ++i) {
		px[i] = static_cast<uint8_t>((i * 37 + static_cast<size_t>(salt) * 101 + (i >> 3)) & 0xFF);
	}
	return px;
}

static const VruBusApiV1* v1 = nullptr;
static const VruBusApiV2* v2 = nullptr;

static void layout() {
	std::printf("v2: table layout\n");
	CHECK(v1->version == 1u);
	CHECK(v1->structSize == sizeof(VruBusApiV1));
	CHECK(v2->v1.version == 2u);
	CHECK(v2->v1.structSize == sizeof(VruBusApiV2));
	CHECK(sizeof(VruBusApiV2) > sizeof(VruBusApiV1));
	CHECK(offsetof(VruBusApiV2, v1) == 0);
	// The pointers of the version 1 part, in order, are the version 1 table's.
	CHECK(v2->v1.busIdFromVoltage == v1->busIdFromVoltage);
	CHECK(v2->v1.isActive == v1->isActive);
	CHECK(v2->v1.readFrame == v1->readFrame);
	CHECK(v2->v1.claimSource == v1->claimSource);
	CHECK(v2->v1.publishFrame == v1->publishFrame);
	CHECK(v2->v1.releaseSource == v1->releaseSource);
	const size_t header = 2 * sizeof(uint32_t);
	CHECK(std::memcmp(reinterpret_cast<const char*>(v1) + header, reinterpret_cast<const char*>(&v2->v1) + header, sizeof(VruBusApiV1) - header) == 0);
	CHECK(v2->readFrameF32 && v2->readFrameEx && v2->readFrameF32Ex && v2->getNativeSize);
	// The same table every time; unknown versions are still refused.
	CHECK(vruBusGetApi(1) == v1);
	CHECK(vruBusGetApi(2) == &v2->v1);
	CHECK(vruBusGetApi(0) == nullptr);
	CHECK(vruBusGetApi(3) == nullptr);
	CHECK(vruBusGetApi(99) == nullptr);
}

// A source of a known size on a claimed bus.
struct Source {
	int id = 0;
	Bytes px;
	int w = 0;
	int h = 0;
	void make(int width, int height, int salt) {
		w = width;
		h = height;
		id = v1->claimSource();
		px = makePicture(w, h, salt);
		v1->publishFrame(id, px.data(), w, h, static_cast<size_t>(w) * 4);
	}
	void release() {
		v1->releaseSource(id);
	}
};

static void floatRead(bool threadMode, bool precision) {
	std::printf("v2: float read equals the byte read / 255 (render thread %s, precision %s)\n", threadMode ? "on" : "off", precision ? "on" : "off");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setThreadAutoStart(true);
	svc.setTickSource(tickSource);
	svc.setFullPrecision(precision);
	svc.setThreadMode(threadMode);
	Source src;
	src.make(16, 12, 3);
	const int w = 16, h = 12;
	++gTick;
	Bytes bytes(static_cast<size_t>(w) * h * 4, 0x11);
	CHECK(v1->readFrame(src.id, w, h, bytes.data(), static_cast<size_t>(w) * 4) == VRU_BUS_OK);
	CHECK(bytes == src.px);
	std::vector<float> f(static_cast<size_t>(w) * h * 4, -7.f);
	CHECK(v2->readFrameF32(src.id, w, h, f.data(), static_cast<size_t>(w) * 4) == VRU_BUS_OK);
	bool exact = true;
	for (size_t i = 0; i < f.size(); ++i) {
		exact = exact && f[i] == static_cast<float>(bytes[i]) / 255.f;
	}
	CHECK(exact);
	// A stride wider than a row: the padding is not touched, the rows are right.
	const size_t stride = static_cast<size_t>(w) * 4 + 5;
	std::vector<float> g(stride * h, -7.f);
	CHECK(v2->readFrameF32(src.id, w, h, g.data(), stride) == VRU_BUS_OK);
	bool strided = true;
	for (int y = 0; y < h; ++y) {
		for (size_t x = 0; x < static_cast<size_t>(w) * 4; ++x) {
			strided = strided && g[static_cast<size_t>(y) * stride + x] == static_cast<float>(src.px[static_cast<size_t>(y) * w * 4 + x]) / 255.f;
		}
		for (size_t x = static_cast<size_t>(w) * 4; x < stride && y + 1 < h; ++x) {
			strided = strided && g[static_cast<size_t>(y) * stride + x] == -7.f;
		}
	}
	CHECK(strided);
	// The full range: 0 and 255 map to 0.0 and 1.0.
	Bytes edge = {0, 255, 1, 254};
	v1->publishFrame(src.id, edge.data(), 1, 1, 4);
	++gTick;
	float one[4] = {-1, -1, -1, -1};
	CHECK(v2->readFrameF32(src.id, 1, 1, one, 4) == VRU_BUS_OK);
	CHECK(one[0] == 0.f && one[1] == 1.f && one[2] == 1.f / 255.f && one[3] == 254.f / 255.f);
	src.release();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	svc.setTickSource(nullptr);
	svc.clear();
}

static void floatErrors() {
	std::printf("v2: errors leave the buffer alone\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setTickSource(tickSource);
	Source src;
	src.make(8, 8, 1);
	std::vector<float> f(8 * 8 * 4 + 64, 0.5f);
	const std::vector<float> keep = f;
	CHECK(v2->readFrameF32(src.id, 8, 8, nullptr, 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32(src.id, 0, 8, f.data(), 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32(src.id, 8, 0, f.data(), 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32(src.id, -1, 8, f.data(), 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32(src.id, VRU_BUS_MAX_DIMENSION + 1, 8, f.data(), 4u * (VRU_BUS_MAX_DIMENSION + 1)) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32(src.id, 8, VRU_BUS_MAX_DIMENSION + 1, f.data(), 32) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32(src.id, 8, 8, f.data(), 31) == VRU_BUS_ERR_BAD_ARGUMENT);  // stride below width * 4 floats
	CHECK(v2->readFrameF32(src.id, 8, 8, f.data(), 8) == VRU_BUS_ERR_BAD_ARGUMENT);   // a byte stride used by mistake
	CHECK(v2->readFrameF32(0, 8, 8, f.data(), 32) == VRU_BUS_ERR_BAD_ID);
	CHECK(v2->readFrameF32(65, 8, 8, f.data(), 32) == VRU_BUS_ERR_BAD_ID);
	CHECK(v2->readFrameF32(-3, 8, 8, f.data(), 32) == VRU_BUS_ERR_BAD_ID);
	CHECK(v2->readFrameF32(60, 8, 8, f.data(), 32) == VRU_BUS_ERR_NO_SIGNAL);  // nothing on bus 60
	CHECK(f == keep);

	// The tick reads: a missing or too small info struct, and the same argument rules.
	VruBusReadInfo info = {sizeof(VruBusReadInfo), 0, 0, 99, 1, 0};
	Bytes b(8 * 8 * 4, 0x66);
	const Bytes bkeep = b;
	CHECK(v2->readFrameEx(src.id, 8, 8, b.data(), 32, nullptr) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, nullptr) == VRU_BUS_ERR_BAD_ARGUMENT);
	VruBusReadInfo small = {4, 0, 0, 0, 0, 0};
	CHECK(v2->readFrameEx(src.id, 8, 8, b.data(), 32, &small) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameEx(src.id, 8, 8, nullptr, 32, &info) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(info.tick == 0 && info.isNew == 0);  // an error clears the outputs
	CHECK(v2->readFrameEx(src.id, 8, 8, b.data(), 31, &info) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameEx(src.id, 8, 8, b.data(), 32, &info) == VRU_BUS_OK);
	info.sinceTick = 0;
	CHECK(v2->readFrameF32Ex(src.id, 0, 8, f.data(), 32, &info) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 31, &info) == VRU_BUS_ERR_BAD_ARGUMENT);
	CHECK(v2->readFrameF32Ex(0, 8, 8, f.data(), 32, &info) == VRU_BUS_ERR_BAD_ID);
	CHECK(v2->readFrameF32Ex(60, 8, 8, f.data(), 32, &info) == VRU_BUS_ERR_NO_SIGNAL);
	CHECK(info.tick == 0);
	CHECK(f == keep);
	(void)bkeep;
	src.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

static void tickUiDriven() {
	std::printf("v2: the tick, UI-driven\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setTickSource(tickSource);
	Source src;
	src.make(8, 8, 5);
	++gTick;
	std::vector<float> f(8 * 8 * 4, 0.f);
	VruBusReadInfo first = {sizeof(VruBusReadInfo), 0, 0, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, &first) == VRU_BUS_OK);
	CHECK(first.tick != 0 && first.isNew == 1);
	const uint64_t renders = svc.renderTotal();

	// No service tick in between: not new, nothing drawn, buffer untouched.
	std::vector<float> g(8 * 8 * 4, -2.f);
	VruBusReadInfo second = {sizeof(VruBusReadInfo), 0, first.tick, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, g.data(), 32, &second) == VRU_BUS_UNCHANGED);
	CHECK(second.tick == first.tick && second.isNew == 0);
	CHECK(g[0] == -2.f);
	CHECK(svc.renderTotal() == renders);
	Bytes b(8 * 8 * 4, 0x44);
	VruBusReadInfo third = {sizeof(VruBusReadInfo), 0, first.tick, 0, 0, 0};
	CHECK(v2->readFrameEx(src.id, 8, 8, b.data(), 32, &third) == VRU_BUS_UNCHANGED && b[0] == 0x44 && third.isNew == 0);
	// COPY_ALWAYS copies, reports not new, and does not draw again.
	VruBusReadInfo forced = {sizeof(VruBusReadInfo), VRU_BUS_READ_COPY_ALWAYS, first.tick, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, g.data(), 32, &forced) == VRU_BUS_OK);
	CHECK(forced.tick == first.tick && forced.isNew == 0 && g == f);
	CHECK(svc.renderTotal() == renders);
	// A bus that went away reports no signal, not "unchanged".
	v1->releaseSource(src.id);
	VruBusReadInfo gone = {sizeof(VruBusReadInfo), 0, first.tick, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, g.data(), 32, &gone) == VRU_BUS_ERR_NO_SIGNAL);
	src.make(8, 8, 6);

	// After a service tick: new, and the serial did not go down.
	++gTick;
	VruBusReadInfo after = {sizeof(VruBusReadInfo), 0, first.tick, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, g.data(), 32, &after) == VRU_BUS_OK);
	CHECK(after.isNew == 1 && after.tick > first.tick);
	uint64_t last = after.tick;
	bool monotonic = true;
	for (int n = 0; n < 50; ++n) {
		if (n % 3 == 0) ++gTick;
		VruBusReadInfo info = {sizeof(VruBusReadInfo), VRU_BUS_READ_COPY_ALWAYS, 0, 0, 0, 0};
		CHECK(v2->readFrameF32Ex(src.id, 8, 8, g.data(), 32, &info) == VRU_BUS_OK);
		monotonic = monotonic && info.tick >= last;
		last = info.tick;
	}
	CHECK(monotonic);
	src.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

static void tickThreaded() {
	std::printf("v2: the tick, render thread (driven by hand)\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setThreadAutoStart(false);
	svc.setThreadMode(true);
	Source src;
	src.make(8, 8, 7);
	std::vector<float> f(8 * 8 * 4, -2.f);
	VruBusReadInfo info = {sizeof(VruBusReadInfo), 0, 0, 0, 0, 0};
	// Nothing rendered yet: the read registers and reports no signal.
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, &info) == VRU_BUS_ERR_NO_SIGNAL);
	svc.runTickForTest();
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, &info) == VRU_BUS_OK);
	CHECK(info.isNew == 1 && info.tick != 0);
	const uint64_t t1 = info.tick;
	Bytes px = makePicture(8, 8, 7);
	CHECK(f[5] == static_cast<float>(px[5]) / 255.f);
	f.assign(f.size(), -2.f);
	VruBusReadInfo again = {sizeof(VruBusReadInfo), 0, t1, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, &again) == VRU_BUS_UNCHANGED);
	CHECK(again.tick == t1 && again.isNew == 0 && f[0] == -2.f);
	VruBusReadInfo forced = {sizeof(VruBusReadInfo), VRU_BUS_READ_COPY_ALWAYS, t1, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, &forced) == VRU_BUS_OK && forced.isNew == 0 && f[5] == static_cast<float>(px[5]) / 255.f);
	svc.runTickForTest();
	VruBusReadInfo next = {sizeof(VruBusReadInfo), 0, t1, 0, 0, 0};
	CHECK(v2->readFrameF32Ex(src.id, 8, 8, f.data(), 32, &next) == VRU_BUS_OK);
	CHECK(next.isNew == 1 && next.tick > t1);
	src.release();
	svc.setThreadMode(false);
	svc.setThreadAutoStart(true);
	svc.clear();
}

static void nativeSize() {
	std::printf("v2: the native size\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	Source src;
	src.make(37, 23, 2);
	VruBusNativeSize size = {sizeof(VruBusNativeSize), -1, -1, -1};
	CHECK(v2->getNativeSize(src.id, &size) == VRU_BUS_OK);
	CHECK(size.width == 37 && size.height == 23 && size.reserved == 0);
	// A new picture of another size is reported at once.
	Bytes bigger = makePicture(50, 10, 4);
	v1->publishFrame(src.id, bigger.data(), 50, 10, 200);
	CHECK(v2->getNativeSize(src.id, &size) == VRU_BUS_OK && size.width == 50 && size.height == 10);

	// A claimed bus that has not published yet has no picture.
	const int idle = v1->claimSource();
	size = {sizeof(VruBusNativeSize), -1, -1, -1};
	CHECK(v2->getNativeSize(idle, &size) == VRU_BUS_ERR_NO_SIGNAL);
	CHECK(size.width == 0 && size.height == 0);
	v1->releaseSource(idle);

	// A generator has no native size, and neither does a processor on a source.
	const int gen = 50;
	VideoParams generator;
	videoBusPublish(gen, generator);
	size = {sizeof(VruBusNativeSize), -1, -1, -1};
	CHECK(v2->getNativeSize(gen, &size) == VRU_BUS_NO_NATIVE_SIZE);
	CHECK(size.width == 0 && size.height == 0);
	RampsParams ramps;
	videoBusPublishRamps(51, ramps);
	CHECK(v2->getNativeSize(51, &size) == VRU_BUS_NO_NATIVE_SIZE);
	ColorLabParams lab;
	videoBusPublishColorLab(52, src.id, lab);
	CHECK(v2->getNativeSize(52, &size) == VRU_BUS_NO_NATIVE_SIZE);
	CHECK(VRU_BUS_NO_NATIVE_SIZE >= 0);

	// Errors.
	CHECK(v2->getNativeSize(61, &size) == VRU_BUS_ERR_NO_SIGNAL);
	CHECK(v2->getNativeSize(0, &size) == VRU_BUS_ERR_BAD_ID);
	CHECK(v2->getNativeSize(65, &size) == VRU_BUS_ERR_BAD_ID);
	CHECK(v2->getNativeSize(src.id, nullptr) == VRU_BUS_ERR_BAD_ARGUMENT);
	VruBusNativeSize tiny = {4, 7, 7, 7};
	CHECK(v2->getNativeSize(src.id, &tiny) == VRU_BUS_ERR_BAD_ARGUMENT && tiny.width == 7);
	src.release();
	videoBusClear(gen);
	videoBusClear(51);
	videoBusClear(52);
	svc.clear();
}

int main() {
	v1 = vruBusGetApi(VRU_BUS_API_VERSION);
	v2 = reinterpret_cast<const VruBusApiV2*>(vruBusGetApi(VRU_BUS_API_VERSION_2));
	CHECK(v1 && v2);
	if (!v1 || !v2) {
		return 1;
	}
	layout();
	floatRead(false, false);
	floatRead(true, false);
	floatRead(false, true);
	floatRead(true, true);
	floatErrors();
	tickUiDriven();
	tickThreaded();
	nativeSize();
	std::printf("vru_bus_api_v2_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
