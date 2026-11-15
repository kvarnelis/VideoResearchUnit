// Stage 2 of the render service: the public API (readFrame) draws through the service. Checks the tick rule
// (src/render_service.hpp), sizes, and one thread reading through the API while another drives the service as a
// Display would. Build with -fsanitize=thread (make tsan) for the race check. No Rack needed.

#include "vru_bus.h"

#include "frame_ring.hpp"
#include "freeze_frame_store.hpp"
#include "render_service.hpp"
#include "signal_ring.hpp"
#include "video_external_frame.hpp"
#include "video_bus.hpp"
#include "video_graph_renderer.hpp"
#include "vru_bus_api_rig.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
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

static const VruBusApiV1* api = nullptr;
static const VruBusApiV2* api2 = nullptr;
static const int kW = 96;
static const int kH = 72;
typedef std::vector<unsigned char> Bytes;

static bool read(int bus, int w, int h, Bytes& out) {
	out.assign(static_cast<size_t>(w) * h * 4, 0x33);
	return api->readFrame(bus, w, h, out.data(), static_cast<size_t>(w) * 4) == VRU_BUS_OK;
}

static void tickRule() {
	std::printf("api service: the tick rule\n");
	RenderService& svc = renderService();
	svc.clear();
	svc.setTickSource(tickSource);
	BeamRig rig;
	rig.build(1);
	Bytes a, b, c;

	// A client alone: two reads in one tick are two passes, and they match a private renderer call for call.
	VideoGraphRenderer priv;
	CHECK(read(4, kW, kH, a));
	const VideoGraphRenderer::Frame& p1 = priv.renderBus(4, kW, kH);
	CHECK(Bytes(p1.begin(), p1.end()) == a);
	CHECK(read(4, kW, kH, b));
	const VideoGraphRenderer::Frame& p2 = priv.renderBus(4, kW, kH);
	CHECK(Bytes(p2.begin(), p2.end()) == b);
	CHECK(a != b);  // the trails advanced

	// A Display of the same size in this tick: the client gets its picture, and nothing advances.
	++gTick;
	rig.nextTick();
	const RenderService::Result d = svc.frame(4, kW, kH);
	CHECK(d.signal);
	const Bytes shown(d.pixels->begin(), d.pixels->end());
	CHECK(read(4, kW, kH, a) && a == shown);
	CHECK(read(4, kW, kH, b) && b == shown);
	CHECK(svc.busRenderCount(4, kW, kH) == 1);
	// Another bus of that size in the same tick is drawn in the same pass, and the Display then sees it unchanged.
	CHECK(read(3, kW, kH, c));
	const RenderService::Result d3 = svc.frame(3, kW, kH);
	CHECK(Bytes(d3.pixels->begin(), d3.pixels->end()) == c);

	// A Display of another size does not make the client share: its own size still advances per call.
	++gTick;
	rig.nextTick();
	(void)svc.frame(4, 64, 48);
	CHECK(read(4, kW, kH, a));
	CHECK(read(4, kW, kH, b));
	CHECK(a != b);

	// The client reads first, the Display joins that pass in the same tick and gets the same picture.
	++gTick;
	rig.nextTick();
	CHECK(read(4, kW, kH, a));
	const RenderService::Result late = svc.frame(4, kW, kH);
	CHECK(Bytes(late.pixels->begin(), late.pixels->end()) == a);
	CHECK(read(4, kW, kH, b) && b == a);  // the Display now holds this tick for the size

	// A bad read leaves the buffer alone.
	CHECK(api->readFrame(30, kW, kH, nullptr, 4) == VRU_BUS_ERR_BAD_ARGUMENT);
	std::vector<uint8_t> keep(kW * kH * 4, 0x77);
	CHECK(api->readFrame(30, kW, kH, keep.data(), kW * 4) == VRU_BUS_ERR_NO_SIGNAL && keep[0] == 0x77);

	rig.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

static void sizes() {
	std::printf("api service: odd sizes\n");
	RenderService& svc = renderService();
	svc.clear();
	svc.setTickSource(tickSource);
	BeamRig rig;
	rig.build(1);

	// The UI uses three sizes every tick; a client sweeps many odd sizes between ticks.
	const int ui[3][2] = {{96, 72}, {64, 48}, {32, 24}};
	for (int round = 0; round < 3; ++round) {
		++gTick;
		for (const auto& s : ui) {
			(void)svc.frame(4, s[0], s[1]);
		}
		Bytes out;
		for (int n = 0; n < 40; ++n) {
			CHECK(read(4, 20 + n, 15 + n, out));
			CHECK(svc.sizeCount() <= RenderService::kMaxSizes);
		}
		for (const auto& s : ui) {
			CHECK(svc.hasSize(s[0], s[1]));
		}
	}
	// With no tick change at all, a sweep still stays inside the cap.
	Bytes out;
	for (int n = 0; n < 60; ++n) {
		CHECK(read(4, 40 + n, 30, out));
	}
	CHECK(svc.sizeCount() <= RenderService::kMaxSizes);
	for (const auto& s : ui) {
		CHECK(svc.hasSize(s[0], s[1]));
	}
	// A client's own size keeps its trails while the UI ticks on (the idle rule counts API use).
	CHECK(read(4, 50, 40, out));
	for (int t = 0; t < 30; ++t) {
		++gTick;
		(void)svc.frame(4, 96, 72);
	}
	CHECK(svc.hasSize(50, 40));
	rig.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

// The version 2 reads, rotated by n: 8-bit, float, the 8-bit read with the tick (twice, the second with the first's tick),
// and the float read with the tick. True when the read gave a picture or reported "unchanged"; false is an error.
// A float picture is checked for the unit (0..1) and for being a whole number of 8-bit levels' worth.
static bool readV2(int n, int bus, int w, int h, Bytes& out, std::vector<float>& fout) {
	const size_t stride = static_cast<size_t>(w) * 4;
	switch (n % 4) {
	case 0:
		return read(bus, w, h, out);
	case 1: {
		fout.assign(stride * h, -1.f);
		const int status = api2->readFrameF32(bus, w, h, fout.data(), stride);
		if (status != VRU_BUS_OK) {
			return false;
		}
		for (float v : fout) {
			if (!(v >= 0.f && v <= 1.f)) {
				return false;
			}
		}
		return true;
	}
	case 2: {
		out.assign(stride * h, 0x33);
		VruBusReadInfo info = {sizeof(VruBusReadInfo), 0, 0, 0, 0, 0};
		if (api2->readFrameEx(bus, w, h, out.data(), stride, &info) != VRU_BUS_OK || info.tick == 0 || !info.isNew) {
			return false;
		}
		VruBusReadInfo again = {sizeof(VruBusReadInfo), 0, info.tick, 0, 0, 0};
		const int status = api2->readFrameEx(bus, w, h, out.data(), stride, &again);
		return (status == VRU_BUS_OK || status == VRU_BUS_UNCHANGED) && again.tick >= info.tick;
	}
	default: {
		fout.assign(stride * h, -1.f);
		VruBusReadInfo info = {sizeof(VruBusReadInfo), VRU_BUS_READ_COPY_ALWAYS, 0, 0, 0, 0};
		const int status = api2->readFrameF32Ex(bus, w, h, fout.data(), stride, &info);
		return status == VRU_BUS_OK && info.tick != 0;
	}
	}
}

// One thread reads through the API, another ticks and requests like a Display and a Probe, the main thread
// moves the signal. Every API read must succeed and every Display frame must have a picture.
static void stress() {
	std::printf("api service: one thread on the API, one driving the service\n");
	RenderService& svc = renderService();
	svc.clear();
	svc.setTickSource(tickSource);
	BeamRig rig;
	rig.build(1);
	const int iterations = 2500;
	std::atomic<int> apiOk{0}, apiBad{0}, uiOk{0}, uiBad{0};
	uint64_t sink = 0;
	std::thread client([&] {
		Bytes out;
		std::vector<float> fout;
		for (int n = 0; n < iterations; ++n) {
			const int w = (n % 3 == 0) ? 64 : 48;
			const int h = (n % 3 == 0) ? 48 : 36;
			if (readV2(n, 4 - (n % 2), w, h, out, fout)) {
				++apiOk;
			}
			else {
				++apiBad;
			}
		}
	});
	std::thread display([&] {
		for (int n = 0; n < iterations; ++n) {
			++gTick;
			const RenderService::Result r = svc.frame(4, 64, 48);
			if (r.signal && !r.pixels->empty()) {
				++uiOk;
				// Read the pixels the way a Display does, outside the service's mutex. No API read re-renders this
				// size until this thread moves the tick on, because the size is used by the UI in this tick.
				uint64_t sum = 0;
				for (unsigned char c : *r.pixels) {
					sum += c;
				}
				sink += sum;
			}
			else {
				++uiBad;
			}
			if (n % 7 == 0) {
				(void)svc.frame(4, 48, 36);
			}
		}
	});
	client.join();
	display.join();
	CHECK(sink != 0);
	CHECK(apiOk == iterations && apiBad == 0);
	CHECK(uiOk == iterations && uiBad == 0);
	CHECK(svc.sizeCount() <= RenderService::kMaxSizes);
	rig.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

// Stage 4: the render thread at full rate while a UI thread, an API thread and an audio-side thread all play. The
// UI thread touches, draws, lets registrations come and go, changes sizes and flips both plugin settings; the API
// thread reads at several sizes; the audio side publishes bus nodes, pushes signal-ring samples and makes and
// destroys module stores (the Feedback ring, Freeze Frame store, Scope Trace ring, external frame) that the
// renderer reads. Run under ThreadSanitizer by `make tsan`. A watchdog turns a deadlock into a failure.
static void threadStress() {
	std::printf("api service: the render thread under a UI, an API and an audio-side thread\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setThreadMode(true);
	BeamRig rig;
	rig.build(1);
	std::atomic<bool> stop{false};
	std::atomic<bool> finished{false};
	std::thread watchdog([&] {
		for (int i = 0; i < 900 && !finished.load(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!finished.load()) {
			std::fprintf(stderr, "WATCHDOG: the render thread stress test did not finish in 90 s\n");
			std::abort();
		}
	});
	std::atomic<long> uiFrames{0}, apiOk{0}, apiTried{0}, churn{0}, clientIter{0};
	std::atomic<bool> sameSizeOk{true};

	std::thread ui([&] {
		int owners[4] = {0, 0, 0, 0};
		int n = 0;
		while (!stop.load()) {
			++n;
			++gTick;
			const int w = (n / 50) % 2 ? 64 : 96;
			const int h = (n / 50) % 2 ? 48 : 72;
			for (int k = 0; k < 4; ++k) {
				const RenderService::Taken t = svc.touch(&owners[k], 4 - (k % 2), k < 2 ? w : 48, k < 2 ? h : 36);
				if (t.fresh && t.signal && !t.pixels->empty()) {
					volatile unsigned char sink = t.pixels->back();
					(void)sink;
				}
			}
			const RenderService::Result r = svc.frame(&owners[0], 4, w, h);
			if (r.signal && !r.pixels->empty()) {
				++uiFrames;
				volatile unsigned char sink = r.pixels->front();
				(void)sink;
			}
			if (n % 17 == 0) {
				svc.forget(&owners[1 + (n / 17) % 3]);
			}
			if (n % 97 == 0) {
				svc.setHiddenRendering((n / 97) % 2 == 0);
			}
			if (n % 150 == 0) {
				// Off for one round in three: the UI-driven path takes over. In that mode (stage 3, unchanged) an API
				// read of the same size as a UI consumer may re-render a picture the consumer still holds, so the
				// API client is kept off the UI's sizes while the mode is off.
				const bool wantOn = (n / 150) % 3 != 0;
				if (!wantOn) {
					sameSizeOk.store(false);
					const long seen = clientIter.load();
					while (clientIter.load() < seen + 2 && !stop.load()) {
						std::this_thread::yield();
					}
				}
				svc.setThreadMode(wantOn);
				if (wantOn) {
					sameSizeOk.store(true);
				}
			}
			if (n % 29 == 0) {
				(void)svc.hasSize(64, 48);
				(void)svc.registrationCount();
			}
			std::this_thread::sleep_for(std::chrono::microseconds(800));
		}
	});
	std::thread client([&] {
		Bytes out;
		std::vector<float> fout;
		int n = 0;
		while (!stop.load()) {
			clientIter.fetch_add(1);
			// The UI's sizes are 96x72, 64x48 and 48x36. In thread mode the client reads those too.
			const bool same = sameSizeOk.load();
			const int w = same ? 40 + 8 * (n % 4) : 44 + 8 * (n % 4);
			const int h = same ? 30 + 6 * (n % 4) : 33 + 6 * (n % 4);
			++apiTried;
			if (readV2(n, 4 - (n % 2), w, h, out, fout)) {
				++apiOk;
			}
			++n;
			std::this_thread::sleep_for(std::chrono::microseconds(400));
		}
	});
	std::thread audio([&] {
		int n = 0;
		while (!stop.load()) {
			++n;
			rig.nextTick();
			// Module stores come and go while the renderer may be reading them.
			const int ring = frameRingAllocateSlot();
			const int freeze = freezeFrameAllocateSlot();
			const int scope = signalRingAllocate();
			const int ext = videoExternalFrameAllocate();
			signalRingSetSampleRate(scope, 48000.f);
			FeedbackParams fb;
			fb.ringSlotId = ring;
			fb.frameCounter = n;
			fb.gain = 0.6f;
			videoBusPublishFeedback(10, 1, fb);
			FreezeFrameParams ff;
			ff.freezeSlotId = freeze;
			ff.captureCounter = n;
			ff.hold = 0.5f;
			videoBusPublishFreezeFrame(11, 10, ff);
			ScopeTraceParams st;
			videoBusPublishScopeTrace(12, scope, st);
			std::vector<unsigned char> px(16 * 16 * 4, static_cast<unsigned char>(n));
			videoExternalFramePublish(ext, px.data(), 16, 16, 0.0);
			videoBusPublishExternalFrame(13, ext);
			VideoParams v;
			v.phase = 0.01f * static_cast<float>(n % 100);
			videoBusPublish(5, v);
			for (int k = 0; k < 200; ++k) {
				const float s[kSignalRingChannels] = {0.5f, -0.5f, 1.f, 0.f};
				signalRingPush(scope, s);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			// The consumers ask for these in the meantime; then the "modules" are deleted.
			videoBusClear(10);
			videoBusClear(11);
			videoBusClear(12);
			videoBusClear(13);
			frameRingReleaseSlot(ring);
			freezeFrameReleaseSlot(freeze);
			signalRingRelease(scope);
			videoExternalFrameRelease(ext);
			++churn;
		}
	});
	// A second UI-like thread asks for the churning buses so the renderer actually reads those stores.
	std::thread churnReader([&] {
		int owner = 0;
		int n = 0;
		while (!stop.load()) {
			svc.touch(&owner, 10 + (n % 4), 48, 36);
			(void)svc.frame(&owner, 10 + (n % 4), 48, 36);
			++n;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	std::this_thread::sleep_for(std::chrono::seconds(6));
	stop.store(true);
	ui.join();
	client.join();
	audio.join();
	churnReader.join();
	CHECK(uiFrames > 100);
	CHECK(churn > 50);
	CHECK(apiTried > 100);
	svc.setThreadMode(false);
	CHECK(!svc.threadRunning());
	std::printf("  ui frames %ld, api reads %ld of %ld, module churn %ld, render thread ticks %llu\n", uiFrames.load(), apiOk.load(), apiTried.load(), churn.load(),
		static_cast<unsigned long long>(svc.threadTickCount()));
	finished.store(true);
	watchdog.join();
	rig.release();
	svc.setHiddenRendering(true);
	svc.setTickSource(nullptr);
	svc.clear();
}

// 200 start and stop cycles of the render thread (also under ThreadSanitizer), ended three different ways, with a
// reader on the API during the run. Leaks show up in the thread count, deadlocks in the watchdog.
static void startStopCycles() {
	std::printf("api service: 200 render thread start and stop cycles\n");
	std::atomic<bool> finished{false};
	std::thread watchdog([&] {
		for (int i = 0; i < 1200 && !finished.load(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!finished.load()) {
			std::fprintf(stderr, "WATCHDOG: start and stop cycles did not finish in 120 s\n");
			std::abort();
		}
	});
	BeamRig rig;
	rig.build(1);
	int clean = 0;
	for (int cycle = 0; cycle < 200; ++cycle) {
		RenderService svc;
		svc.setThreadMode(true);
		int owner = 0;
		bool got = false;
		for (int tries = 0; tries < 5000 && !got; ++tries) {
			const RenderService::Taken t = svc.touch(&owner, 4, 48, 36);
			got = t.fresh && t.signal;
			if (!got) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		CHECK(got && svc.threadRunning());
		switch (cycle % 3) {
		case 0: svc.setThreadMode(false); CHECK(!svc.threadRunning()); break;
		case 1: svc.stopThread(); CHECK(!svc.threadRunning()); break;
		default: break;  // the destructor stops it, here at the end of the scope
		}
		++clean;
	}
	CHECK(clean == 200);
	finished.store(true);
	watchdog.join();
	rig.release();
}

// ---- The UI-driven mode: a picture handed to a consumer stays valid and unchanged while it holds it ----
// A UI consumer (Display, Syphon Out, Video Probe, SHOT and REC) gets a picture from frame() or touch() and keeps
// reading it (an upload, a summed-area table) after the service lock is gone. A foreign thread's readCopy() at the
// same size, in a later tick, renders the next pass; it must not write into the picture the consumer holds.
static uint64_t hashPixels(const RenderService::Frame& f) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : f) {
		h = (h ^ c) * 1099511628211ull;
	}
	return h;
}

// Single thread, exact: hold a picture, let the API start a newer tick and render it, and the held one is unchanged.
static void heldPictureSurvivesNewTick() {
	std::printf("api service: a held picture survives the next tick (UI-driven mode)\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	BeamRig rig;
	rig.build(1);
	Bytes a;

	// frame(): the picture a draw() uploads.
	++gTick;
	rig.nextTick();
	const RenderService::Result held = svc.frame(4, kW, kH);
	CHECK(held.signal && !held.pixels->empty());
	const Bytes before(held.pixels->begin(), held.pixels->end());
	const uint64_t h0 = hashPixels(*held.pixels);
	for (int n = 0; n < 3; ++n) {
		++gTick;
		rig.nextTick();
		CHECK(read(4, kW, kH, a));  // the API's own tick: a new pass at the same size
		CHECK(a != before);         // the picture really moved on
	}
	CHECK(hashPixels(*held.pixels) == h0);
	CHECK(Bytes(held.pixels->begin(), held.pixels->end()) == before);

	// touch(): the picture a step() feeds to SHOT, REC and the Probe.
	int owner = 0;
	++gTick;
	rig.nextTick();
	const RenderService::Taken first = svc.touch(&owner, 4, kW, kH);
	(void)first;
	++gTick;
	rig.nextTick();
	// The API's first call in the new tick closes the old one and renders the registered bus (a hidden render).
	CHECK(read(4, kW, kH, a));
	const RenderService::Taken taken = svc.touch(&owner, 4, kW, kH);
	CHECK(taken.fresh && taken.signal);
	const Bytes tbefore(taken.pixels->begin(), taken.pixels->end());
	for (int n = 0; n < 3; ++n) {
		++gTick;
		rig.nextTick();
		CHECK(read(4, kW, kH, a));
	}
	CHECK(Bytes(taken.pixels->begin(), taken.pixels->end()) == tbefore);

	// A size the consumer holds is evicted by an API client that sweeps sizes: the held picture must stay readable.
	++gTick;
	rig.nextTick();
	const RenderService::Result keep = svc.frame(4, 40, 30);
	CHECK(keep.signal);
	const Bytes kbefore(keep.pixels->begin(), keep.pixels->end());
	for (int n = 0; n < 12; ++n) {
		++gTick;
		rig.nextTick();
		CHECK(read(4, 20 + 2 * n, 16, a));
	}
	CHECK(!svc.hasSize(40, 30));  // it did go
	CHECK(Bytes(keep.pixels->begin(), keep.pixels->end()) == kbefore);

	rig.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

// Two threads: one is a UI consumer that reads every byte of what frame() or touch() gave it, twice, with a pause
// between, and checks both reads agree; the other hammers readCopy() at the same bus and size, bumping the tick so
// every call is a pass of its own. Run in both modes. Under ThreadSanitizer a write into a held picture is a report.
static void heldPictureStress(bool threadMode) {
	std::printf("api service: a UI consumer holds pictures while the API reads (%s)\n", threadMode ? "render thread" : "UI-driven");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	svc.setThreadMode(threadMode);
	BeamRig rig;
	rig.build(1);
	std::atomic<bool> stop{false};
	std::atomic<long> apiReads{0}, uiHolds{0}, torn{0};
	std::thread api([&] {
		Bytes out;
		while (!stop.load()) {
			++gTick;
			if (read(4, kW, kH, out)) {
				++apiReads;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(5));  // let the UI thread take the lock too
		}
	});
	std::thread ui([&] {
		int owner = 0;
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
		long n = 0;
		while (std::chrono::steady_clock::now() < end || uiHolds.load() < 200) {
			++n;
			const RenderService::Frame* pixels = nullptr;
			RenderService::Result r;
			RenderService::Taken t;
			if (n % 2) {
				r = svc.frame(&owner, 4, kW, kH);
				if (r.signal) {
					pixels = r.pixels;
				}
			}
			else {
				t = svc.touch(&owner, 4, kW, kH);
				if (t.fresh && t.signal) {
					pixels = t.pixels;
				}
			}
			if (pixels && !pixels->empty()) {
				const uint64_t h1 = hashPixels(*pixels);
				std::this_thread::sleep_for(std::chrono::microseconds(300));
				const uint64_t h2 = hashPixels(*pixels);
				++uiHolds;
				if (h1 != h2) {
					++torn;
				}
			}
			if (n % 3 == 0) {
				std::this_thread::sleep_for(std::chrono::microseconds(30));
			}
		}
	});
	ui.join();
	stop.store(true);
	api.join();
	std::printf("  %ld held pictures, %ld API reads, %ld changed under the consumer\n", uiHolds.load(), apiReads.load(), torn.load());
	CHECK(uiHolds.load() >= 200);
	CHECK(apiReads.load() > 0);
	CHECK(torn.load() == 0);
	rig.release();
	svc.setThreadMode(false);
	svc.setTickSource(nullptr);
	svc.clear();
}

// Steady state: pictures circulate through a few pooled buffers, so frames do not allocate. Counted by the number
// of distinct pixel addresses seen over many ticks, with a consumer that still holds the last tick's picture while
// it takes the next, and an API read in every other tick.
static void bufferPoolSteadyState() {
	std::printf("api service: pictures reuse pooled buffers\n");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	BeamRig rig;
	rig.build(1);
	std::set<const unsigned char*> seen;
	RenderService::Result previous;
	Bytes a;
	for (int n = 0; n < 300; ++n) {
		++gTick;
		rig.nextTick();
		const RenderService::Result r = svc.frame(4, kW, kH);
		CHECK(r.signal);
		seen.insert(r.pixels->data());
		if (n % 2 == 0) {
			CHECK(read(4, kW, kH, a));
		}
		previous = r;
	}
	std::printf("  %zu distinct buffers over 300 ticks\n", seen.size());
	CHECK(seen.size() <= 6);
	rig.release();
	svc.setTickSource(nullptr);
	svc.clear();
}

// Full precision between modules, flipped while the render thread, a UI thread and an API reader are all drawing.
// The graph has every kind of picture the setting changes: Ramps through two Color Labs (float generators and nodes),
// and a Bus Mixer loop through a Frame Delay (the float ring). Every picture handed out must be a byte frame of the
// right size whose ramp is still a ramp, whichever way the setting was when it was drawn.
static void precisionFlipStress(bool threadMode) {
	std::printf("api service: the precision setting flipped under load (render thread %s)\n", threadMode ? "on" : "off");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
	RampsParams ramps;
	videoBusPublishRamps(40, ramps);
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(41, 40, quarter);
	ColorLabParams twice;
	twice.brightness = 2.f;
	videoBusPublishColorLab(42, 41, twice);
	const int ringSlot = frameRingAllocateSlot();
	BusMixerParams mixer;
	mixer.busB = 44;
	mixer.mix = 0.9f;
	videoBusPublishBusMixer(43, 42, mixer);
	FrameDelayParams delay;
	delay.ringSlotId = ringSlot;
	delay.frames = 2;
	videoBusPublishFrameDelay(44, 43, delay);
	svc.setThreadMode(threadMode);

	std::atomic<bool> stop{false};
	std::atomic<long> uiPictures{0}, apiPictures{0}, flips{0}, bad{0};
	auto rampIsRamp = [&](const unsigned char* px, int w) {
		// Ramps -> 0.25 -> 2.0 is a ramp from 0 up to about half scale, whichever way the setting was.
		return px[3] == 255 && px[0] <= 3 && px[(w - 1) * 4] >= 100 && px[(w - 1) * 4] <= 140;
	};
	std::thread flipper([&] {
		bool on = false;
		while (!stop.load()) {
			on = !on;
			svc.setFullPrecision(on);
			flips.fetch_add(1);
			std::this_thread::sleep_for(std::chrono::microseconds(700));
		}
	});
	std::thread ui([&] {
		int owner = 0;
		int n = 0;
		while (!stop.load()) {
			++n;
			++gTick;
			delay.frameCounter = n;
			videoBusPublishFrameDelay(44, 43, delay);
			const RenderService::Result a = svc.frame(&owner, 42, 64, 4);
			if (a.signal && a.pixels && !a.pixels->empty()) {
				if (a.pixels->isFloat() || a.pixels->size() != 64u * 4u * 4u || !rampIsRamp(a.pixels->data(), 64)) ++bad;
				++uiPictures;
			}
			const RenderService::Result b = svc.frame(&owner, 43, 64, 4);
			if (b.signal && b.pixels && !b.pixels->empty()) {
				if (b.pixels->isFloat() || b.pixels->size() != 64u * 4u * 4u || (*b.pixels)[3] != 255) ++bad;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(600));
		}
	});
	std::thread client([&] {
		Bytes out;
		while (!stop.load()) {
			if (read(42, 64, 4, out)) {
				if (out.size() != 64u * 4u * 4u || !rampIsRamp(out.data(), 64)) ++bad;
				++apiPictures;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});
	std::atomic<long> floatPictures{0}, unchanged{0};
	std::thread floatClient([&] {
		std::vector<float> fout;
		uint64_t lastTick = 0;
		while (!stop.load()) {
			fout.assign(64u * 4u * 4u, -1.f);
			VruBusReadInfo info = {sizeof(VruBusReadInfo), 0, lastTick, 0, 0, 0};
			const int status = api2->readFrameF32Ex(42, 64, 4, fout.data(), 64u * 4u, &info);
			if (status == VRU_BUS_OK) {
				// The same ramp, in the 0..1 unit: red from about 0 up to about half scale, alpha 1.
				if (fout[3] != 1.f || fout[0] > 3.f / 255.f || fout[63 * 4] < 100.f / 255.f || fout[63 * 4] > 140.f / 255.f) ++bad;
				if (info.tick < lastTick) ++bad;
				lastTick = info.tick;
				++floatPictures;
			}
			else if (status == VRU_BUS_UNCHANGED) {
				++unchanged;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	stop.store(true);
	flipper.join();
	ui.join();
	floatClient.join();
	client.join();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	std::printf("  %ld flips, %ld UI pictures, %ld API pictures, %ld bad\n", flips.load(), uiPictures.load(), apiPictures.load(), bad.load());
	CHECK(flips.load() > 50);
	CHECK(uiPictures.load() > 20);
	CHECK(apiPictures.load() > 20);
	CHECK(floatPictures.load() > 20);
	std::printf("  %ld float pictures, %ld unchanged\n", floatPictures.load(), unchanged.load());
	CHECK(bad.load() == 0);
	frameRingReleaseSlot(ringSlot);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
	svc.setTickSource(nullptr);
	svc.clear();
}

// Float reads at full precision with the setting held on, in both service modes, while a UI thread draws the same bus, a
// byte client reads it and the plane's ask lapses and returns. Ramps -> Color Lab 0.25 -> Color Lab 2.0 at 512 x 4: as bytes
// about 128 levels, as a float read at full precision many more. Every float read must be byte-derived or full precision
// (never anything else), the two must both occur, and every byte read must be the 8-bit picture. Run under
// ThreadSanitizer by `make tsan`.
static void floatPlaneStress(bool threadMode) {
	std::printf("api service: float reads at full precision (render thread %s)\n", threadMode ? "on" : "off");
	RenderService& svc = renderService();
	svc.setThreadMode(false);
	svc.setFullPrecision(true);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
	RampsParams ramps;
	videoBusPublishRamps(40, ramps);
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(41, 40, quarter);
	ColorLabParams twice;
	twice.brightness = 2.f;
	videoBusPublishColorLab(42, 41, twice);
	svc.setThreadMode(threadMode);
	const int w = 512, h = 4;

	std::atomic<bool> stop{false};
	std::atomic<long> uiPictures{0}, bytePictures{0}, bad{0}, floatFull{0}, floatBytes{0}, held{0};
	std::thread ui([&] {
		int owner = 0;
		while (!stop.load()) {
			++gTick;
			const RenderService::Result a = svc.frame(&owner, 42, w, h);
			if (a.signal && a.pixels && !a.pixels->empty()) {
				// The picture a consumer gets is always the byte picture; a float plane, when there is one, matches it.
				if (a.pixels->isFloat() || a.pixels->size() != static_cast<size_t>(w) * h * 4u) ++bad;
				const std::vector<float>& plane = a.pixels->precise;
				if (!plane.empty()) {
					if (plane.size() != a.pixels->size()) ++bad;
					for (size_t i = 0; i < plane.size(); i += 97) {
						if (std::fabs(plane[i] - static_cast<float>((*a.pixels)[i])) > 0.5f + 1e-3f) ++bad;
					}
					++held;
				}
				++uiPictures;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(600));
		}
	});
	std::thread byteClient([&] {
		Bytes out;
		while (!stop.load()) {
			if (read(42, w, h, out)) {
				std::set<int> seen;
				for (int x = 0; x < w; ++x) seen.insert(out[static_cast<size_t>(x) * 4u]);
				if (seen.size() > 130u) ++bad;
				++bytePictures;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});
	std::thread floatClient([&] {
		std::vector<float> fout;
		int n = 0;
		while (!stop.load()) {
			++n;
			// Every so often stop asking for a while, so the ask lapses and comes back.
			if ((n / 400) % 2 == 1) {
				std::this_thread::sleep_for(std::chrono::microseconds(500));
				continue;
			}
			fout.assign(static_cast<size_t>(w) * h * 4u, -1.f);
			const int status = api2->readFrameF32(42, w, h, fout.data(), static_cast<size_t>(w) * 4u);
			if (status == VRU_BUS_OK) {
				std::set<float> seen;
				for (int x = 0; x < w; ++x) seen.insert(fout[static_cast<size_t>(x) * 4u]);
				if (fout[3] != 1.f || fout[0] > 3.f / 255.f || fout[(w - 1) * 4] < 100.f / 255.f || fout[(w - 1) * 4] > 140.f / 255.f) ++bad;
				if (seen.size() > 130u) ++floatFull; else ++floatBytes;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	stop.store(true);
	ui.join();
	byteClient.join();
	floatClient.join();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	std::printf("  %ld UI pictures (%ld with a plane), %ld byte reads, %ld float reads at full precision, %ld byte-derived, %ld bad\n", uiPictures.load(), held.load(), bytePictures.load(), floatFull.load(), floatBytes.load(), bad.load());
	CHECK(uiPictures.load() > 20);
	CHECK(bytePictures.load() > 20);
	CHECK(floatFull.load() > 20);
	CHECK(held.load() > 5);
	CHECK(bad.load() == 0);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
	svc.setTickSource(nullptr);
	svc.clear();
}

int main() {
	api = vruBusGetApi(VRU_BUS_API_VERSION);
	CHECK(api != nullptr);
	api2 = reinterpret_cast<const VruBusApiV2*>(vruBusGetApi(VRU_BUS_API_VERSION_2));
	CHECK(api2 != nullptr);
	if (!api || !api2) {
		return 1;
	}
	tickRule();
	sizes();
	stress();
	heldPictureSurvivesNewTick();
	bufferPoolSteadyState();
	heldPictureStress(false);
	heldPictureStress(true);
	threadStress();
	startStopCycles();
	precisionFlipStress(false);
	precisionFlipStress(true);
	floatPlaneStress(false);
	floatPlaneStress(true);
	std::printf("vru_bus_api_service_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
