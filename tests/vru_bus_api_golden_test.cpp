// Characterization test for the public API's readFrame (include/vru_bus.h). It pins, with hashes of the RGBA
// the client receives, what readFrame returns for: a generator, a processor chain, beam trails (Scope Trace
// and Deflection Yoke), a Feedback graph, one bus at two sizes, and a read made while a Display-size render
// of the same bus goes through the render service. No Rack needed. Run with `make test`.
//
// The alongside-display hashes changed in stage 2 of the render service, by design: a read in a tick where a
// Display-size render of that size already happened now gets that tick's shared picture and advances nothing.
// Every other hash is the one pinned before the change.
//
// Pass any argument to print the hashes instead of checking them.

#include "vru_bus.h"

#include "frame_ring.hpp"
#include "render_service.hpp"
#include "video_bus.hpp"
#include "vru_bus_api_rig.hpp"

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

static uint64_t gTick = 1;
static uint64_t tickSource() {
	return gTick;
}

static const VruBusApiV1* api = nullptr;
static const int kW = 160;
static const int kH = 120;

static uint64_t hashBytes(const std::vector<uint8_t>& px, uint64_t h) {
	for (uint8_t c : px) {
		h = (h ^ c) * 1099511628211ull;
	}
	return h;
}

// What the client received: the status and the pixels, hashed together.
static uint64_t readHash(int bus, int w, int h) {
	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0x33);
	const int status = api->readFrame(bus, w, h, px.data(), static_cast<size_t>(w) * 4);
	return hashBytes(px, 1469598103934665603ull ^ static_cast<uint64_t>(status + 10));
}

static void publishGen(int bus, float phase, float feedback) {
	VideoParams v;
	v.phase = phase;
	v.pattern = 1;
	v.feedback = feedback;
	videoBusPublish(bus, v);
}

static std::vector<uint64_t> generator() {
	publishGen(1, 0.1f, 0.5f);
	std::vector<uint64_t> out;
	for (int n = 0; n < 4; ++n) {
		out.push_back(readHash(1, kW, kH));
	}
	videoBusClear(1);
	return out;
}

static std::vector<uint64_t> chain() {
	publishGen(11, 0.2f, 0.4f);
	ColorLabParams cl;
	cl.hue = 0.2f;
	videoBusPublishColorLab(12, 11, cl);
	RotatorParams ro;
	ro.angle = 0.1f;
	videoBusPublishRotator(13, 12, ro);
	std::vector<uint64_t> out;
	for (int n = 0; n < 3; ++n) {
		out.push_back(readHash(13, kW, kH));
	}
	for (int k = 11; k <= 13; ++k) {
		videoBusClear(k);
	}
	return out;
}

static std::vector<uint64_t> beams() {
	BeamRig rig;
	rig.build(21);
	std::vector<uint64_t> out;
	for (int n = 0; n < 3; ++n) {  // two reads per signal step, so the beams advance per call
		out.push_back(readHash(24, kW, kH));
		out.push_back(readHash(24, kW, kH));
		rig.nextTick();
	}
	out.push_back(readHash(22, kW, kH));  // the Scope Trace bus alone
	out.push_back(readHash(23, kW, kH));  // the Deflection Yoke bus alone
	rig.release();
	return out;
}

static std::vector<uint64_t> feedback() {
	publishGen(31, 0.0f, 0.f);
	RotatorParams ro;
	ro.angle = 0.05f;
	videoBusPublishRotator(32, 31, ro);
	FeedbackParams fb;
	fb.ringSlotId = frameRingAllocateSlot();
	fb.gain = 0.7f;
	BusMixerParams m;
	m.busB = 33;
	m.mode = 1;
	m.mix = 0.5f;
	videoBusPublishBusMixer(34, 33, m);
	std::vector<uint64_t> out;
	for (int n = 1; n <= 5; ++n) {
		fb.frameCounter = n;
		videoBusPublishFeedback(33, 32, fb);
		out.push_back(readHash(34, kW, kH));
		if (n == 2) {
			out.push_back(readHash(34, kW, kH));  // a second read with no new frame counter
		}
	}
	frameRingReleaseSlot(fb.ringSlotId);
	for (int k = 31; k <= 34; ++k) {
		videoBusClear(k);
	}
	return out;
}

static std::vector<uint64_t> twoSizes() {
	publishGen(41, 0.3f, 0.5f);
	std::vector<uint64_t> out;
	for (int n = 0; n < 4; ++n) {
		out.push_back(readHash(41, kW, kH));
		out.push_back(readHash(41, 96, 54));
	}
	videoBusClear(41);
	return out;
}

// A Display-size render of the same bus goes through the render service before the API reads, one tick each.
static std::vector<uint64_t> alongsideDisplay() {
	BeamRig rig;
	rig.build(51);
	RenderService& svc = renderService();
	svc.clear();
	svc.setTickSource(tickSource);
	std::vector<uint64_t> out;
	for (int n = 0; n < 4; ++n) {
		++gTick;
		const RenderService::Result d = svc.frame(54, kW, kH);  // the Display draws first this tick
		CHECK(d.signal);
		out.push_back(readHash(54, kW, kH));  // then a client reads the same bus at the same size
		out.push_back(readHash(54, kW, kH));  // and again, in the same tick
		rig.nextTick();
	}
	svc.setTickSource(nullptr);
	svc.clear();
	rig.release();
	return out;
}

static const char* const kNames[] = {"generator", "chain", "beams", "feedback", "two sizes", "alongside display"};
typedef std::vector<uint64_t> (*Scenario)();
static const Scenario kScenarios[] = {generator, chain, beams, feedback, twoSizes, alongsideDisplay};

// GOLDEN-BEGIN
static const std::vector<std::vector<uint64_t>> kGolden = {
	{0x7991904239ca3064ull, 0x7991904239ca3064ull, 0x7991904239ca3064ull, 0x7991904239ca3064ull, },  // generator
	{0x45ba9372ef347897ull, 0x45ba9372ef347897ull, 0x45ba9372ef347897ull, },  // chain
	{0xb553cd31447b4258ull, 0x5b429a78266ad901ull, 0xd064ff9621f0e011ull, 0x279bc66a09208093ull, 0x29b8a4906d39329full, 0x46b67a9c96c62903ull, 0x5abf83d9f8594cd7ull, 0xbbeefd23469aefe7ull, },  // beams (the Yoke frames changed with the video clock; the Scope Trace frame, 7th of 8, did not)
	{0x6cfc2cb8d05229c6ull, 0xfacf25bc9dc6c86bull, 0xfacf25bc9dc6c86bull, 0xaba2ea4fa022b711ull, 0x170e088b11b4129full, 0x007b0c649821028aull, },  // feedback
	{0x7991904239ca3064ull, 0xfc962eb139fed25cull, 0x7991904239ca3064ull, 0xfc962eb139fed25cull, 0x7991904239ca3064ull, 0xfc962eb139fed25cull, 0x7991904239ca3064ull, 0xfc962eb139fed25cull, },  // two sizes
	{0xb553cd31447b4258ull, 0xb553cd31447b4258ull, 0x417fa2ac63a7f6f8ull, 0x417fa2ac63a7f6f8ull, 0x0b4d8ff825dea768ull, 0x0b4d8ff825dea768ull, 0x0de4bc950ac8963cull, 0x0de4bc950ac8963cull, },  // alongside display (changed in stage 2: the API shares the Display's picture for the tick, so the pair is equal; the Yoke frames changed again when the scan modules moved onto the video clock, and the Scope Trace frame did not)
};
// GOLDEN-END

int main(int argc, char**) {
	api = vruBusGetApi(VRU_BUS_API_VERSION);
	CHECK(api != nullptr);
	if (!api) {
		return 1;
	}
	const bool print = argc > 1;
	for (size_t s = 0; s < sizeof(kScenarios) / sizeof(kScenarios[0]); ++s) {
		const std::vector<uint64_t> got = kScenarios[s]();
		if (print) {
			std::printf("\t{");
			for (uint64_t h : got) {
				std::printf("0x%016llxull, ", static_cast<unsigned long long>(h));
			}
			std::printf("},  // %s\n", kNames[s]);
			continue;
		}
		const bool same = s < kGolden.size() && kGolden[s] == got;
		if (!same) {
			std::printf("  differs: %s\n", kNames[s]);
		}
		CHECK(same);
	}
	if (!print) {
		std::printf("vru_bus_api_golden_test: %d checks, %d failures\n", checks, failures);
	}
	return failures == 0 ? 0 : 1;
}
