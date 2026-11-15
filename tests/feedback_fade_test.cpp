// Feedback (VRU-14): characterization of the picture it makes today, and of the grey trail that never fades.
//
// Feedback stores its output as 8-bit and mixes the next picture with that stored copy. With a gain g below 1 and
// a black input, a stored level n comes back as round(g * n), which is n again whenever n <= 0.5 / (1 - g). So the
// trail stops falling at that grey and stays there for ever. The first half of this file pins exactly that, and pins
// the pictures of the Blend and Add modes at the edge and transform settings the other tests do not hash.
// Run with VRU_PRINT_GOLDEN=1 to print the hashes. Needs no Rack SDK.

#include "video_bus.hpp"
#include "video_graph_renderer.hpp"
#include "frame_ring.hpp"
#include "video_external_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using Frame = std::vector<unsigned char>;
constexpr int kW = 160;
constexpr int kH = 90;
// Ticks to black with the option on (see testFadeReachesBlack).
// The 8-bit picture is black at BLACK, the float history is exactly 0 at ZERO, counted from the bright frame at
// tick 1 (255 x g^n falls under half a level, then under 1/1024 of a level).
constexpr int BLACK_095 = 123, ZERO_095 = 245, BLACK_098 = 310, ZERO_098 = 619, BLACK_099 = 622, ZERO_099 = 1243;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

uint64_t hashFrame(const Frame& f) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : f) h = (h ^ c) * 1099511628211ull;
	return h;
}

bool printGolden() {
	static const bool on = std::getenv("VRU_PRINT_GOLDEN") != nullptr;
	return on;
}

// name -> expected hash. Filled from a VRU_PRINT_GOLDEN=1 run on the unmodified renderer.
const std::map<std::string, uint64_t>& expectedHashes() {
	static const std::map<std::string, uint64_t> hashes = {
		{"stuck g0.95 t1", 16155179336049496707ull},
		{"stuck g0.95 t2", 15553354203164381947ull},
		{"stuck g0.95 t3", 4860200784026950875ull},
		{"stuck g0.95 t10", 10678127791516383011ull},
		{"stuck g0.95 t400", 11825966286538699811ull},
		{"stuck g0.98 t1", 16155179336049496707ull},
		{"stuck g0.98 t2", 10828683071135890075ull},
		{"stuck g0.98 t3", 8675495937557811203ull},
		{"stuck g0.98 t10", 9412638985153650003ull},
		{"stuck g0.98 t400", 17456904271042106515ull},
		{"stuck g0.99 t1", 16155179336049496707ull},
		{"stuck g0.99 t2", 15825206015411272307ull},
		{"stuck g0.99 t3", 18273140372356005763ull},
		{"stuck g0.99 t10", 320785655156097555ull},
		{"stuck g0.99 t400", 14728232099822008339ull},
		{"add clamp zoom t3", 17127424215159846902ull},
		{"add clamp zoom t6", 2650282003463113529ull},
		{"add wrap rotate t3", 6001658443320044335ull},
		{"add wrap rotate t6", 14886022249948174363ull},
		{"add black offset t3", 16569854406458519205ull},
		{"add black offset t6", 15854575062202845813ull},
		{"add key hue t3", 2947680427759235059ull},
		{"add key hue t6", 16058599587420187963ull},
		{"add over gain wrap t3", 7364110439218138486ull},
		{"add over gain wrap t6", 13773012088908219236ull},
		{"blend clamp hue t3", 3415226270958603335ull},
		{"blend clamp hue t6", 2691904791774836610ull},
		{"blend wrap rotate zoom key t3", 2029607206902900857ull},
		{"blend wrap rotate zoom key t6", 18262224498916625494ull},
		{"blend black zoom t3", 2866605592728332176ull},
		{"blend black zoom t6", 1139186045537709999ull},
		{"blend black offset rotate t3", 4967542583163677336ull},
		{"blend black offset rotate t6", 15794427511538048223ull},
		{"blend clamp offset t3", 680525203940445057ull},
		{"blend clamp offset t6", 10452788869265502084ull},
	};
	return hashes;
}

void golden(const std::string& name, const Frame& f) {
	const uint64_t h = hashFrame(f);
	if (printGolden()) std::printf("  GOLDEN %s %lluull\n", name.c_str(), static_cast<unsigned long long>(h));
	const auto it = expectedHashes().find(name);
	check(it != expectedHashes().end() && it->second == h, (name + " hash is pinned").c_str());
}

void clearBuses() {
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
}

// The input picture: a picture published from outside, so a test chooses every level exactly.
struct Source {
	int frameId = 0;
	Source() { frameId = videoExternalFrameAllocate(); }
	~Source() {
		videoExternalFrameRelease(frameId);
		videoBusClear(1);
	}
	void publish(const Frame& f, int w = kW, int h = kH) {
		videoExternalFramePublish(frameId, f.data(), w, h, 0.0);
		videoBusPublishExternalFrame(1, frameId);
	}
};

Frame solid(unsigned char level, int w = kW, int h = kH) {
	Frame f(static_cast<size_t>(w) * h * 4, level);
	for (size_t i = 3; i < f.size(); i += 4) f[i] = 255;
	return f;
}

// A horizontal grey ramp, 0 at the left edge to 255 at the right.
Frame ramp(int w = kW, int h = kH) {
	Frame f(static_cast<size_t>(w) * h * 4, 255);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const unsigned char v = static_cast<unsigned char>(x * 255 / (w - 1));
			const size_t i = (static_cast<size_t>(y) * w + x) * 4;
			f[i] = f[i + 1] = f[i + 2] = v;
		}
	}
	return f;
}

int maxLevel(const Frame& f) {
	int m = 0;
	for (size_t i = 0; i < f.size(); i += 4) m = std::max({m, static_cast<int>(f[i]), static_cast<int>(f[i + 1]), static_cast<int>(f[i + 2])});
	return m;
}

// ---- The stuck trail ----
// One bright frame (the ramp) at tick 1, then black input. Blend mode and Add mode give the same picture here
// (the input is black, so both are gain x the previous picture). Returns the highest level at each tick.
struct Trail {
	std::vector<int> peak;      // peak[t] is the highest level of the picture at tick t + 1
	std::vector<Frame> frames;  // the pictures at the ticks in keep
};

Trail runTrail(float gain, int ticks, int mode, const std::vector<int>& keep, bool fade) {
	Trail t;
	Source src;
	const int slot = frameRingAllocateSlot();
	frameRingSetCapacity(slot, 1);
	VideoGraphRenderer r;
	FeedbackParams p;
	p.ringSlotId = slot;
	p.gain = gain;
	p.mode = mode;
	p.fadeToBlack = fade ? 1 : 0;
	for (int tick = 1; tick <= ticks; ++tick) {
		src.publish(tick == 1 ? ramp() : solid(0));
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		const Frame out = r.renderBus(2, kW, kH);
		t.peak.push_back(maxLevel(out));
		if (std::find(keep.begin(), keep.end(), tick) != keep.end()) t.frames.push_back(out);
	}
	frameRingReleaseSlot(slot);
	videoBusClear(2);
	return t;
}

void testStuckTrail() {
	std::printf("feedback today: the trail sticks at a grey and never reaches black\n");
	clearBuses();
	struct Case { float gain; int stuck; const char* name; };
	// The stuck grey is the largest n with round(g * n) == n, about 0.5 / (1 - g). The numbers below are what the
	// code really does (0.95 gives 10, not 9, because 0.95 x 10 sits on the rounding tie).
	const Case cases[] = {{0.95f, 10, "g0.95"}, {0.98f, 25, "g0.98"}, {0.99f, 50, "g0.99"}};
	for (const Case& c : cases) {
		const std::vector<int> keep = {1, 2, 3, 10, 400};
		const Trail t = runTrail(c.gain, 800, 0, keep, false);
		if (printGolden()) std::printf("  STUCK %s peak at 400 = %d, at 800 = %d\n", c.name, t.peak[399], t.peak[799]);
		check(t.peak[0] == 255, "tick 1 is the bright ramp");
		check(t.peak[399] == c.stuck && t.peak[799] == c.stuck, (std::string(c.name) + " the peak level sticks at the pinned grey").c_str());
		bool nonIncreasing = true;
		for (size_t i = 1; i < t.peak.size(); ++i) nonIncreasing = nonIncreasing && t.peak[i] <= t.peak[i - 1];
		check(nonIncreasing, (std::string(c.name) + " the peak never rises").c_str());
		const char* tags[] = {"t1", "t2", "t3", "t10", "t400"};
		for (size_t i = 0; i < t.frames.size(); ++i) golden(std::string("stuck ") + c.name + " " + tags[i], t.frames[i]);
		// Blend mode at the same gain makes the same black-input trail.
		const Trail b = runTrail(c.gain, 400, 1, {400}, false);
		check(b.peak[399] == c.stuck, (std::string(c.name) + " blend mode sticks at the same grey").c_str());
	}
}

// ---- Normal graphs, every blend and transform setting ----
struct Setting {
	const char* name;
	int mode;
	int edge;
	float gain, zoom, rotate, x, y, hue, key;
};

void testSettings() {
	std::printf("feedback today: pictures at each blend, edge and transform setting\n");
	clearBuses();
	const Setting settings[] = {
		{"add clamp zoom", 0, 0, 0.8f, 1.03f, 0.f, 0.f, 0.f, 0.f, 0.f},
		{"add wrap rotate", 0, 1, 0.7f, 1.f, 0.02f, 0.f, 0.f, 0.f, 0.f},
		{"add black offset", 0, 2, 0.9f, 1.f, 0.f, 0.03f, -0.02f, 0.f, 0.f},
		{"add key hue", 0, 0, 0.5f, 1.f, 0.f, 0.f, 0.f, -0.2f, 0.5f},
		{"add over gain wrap", 0, 1, 1.2f, 1.02f, -0.04f, 0.01f, 0.f, 0.f, 0.f},
		{"blend clamp hue", 1, 0, 0.8f, 1.f, 0.f, 0.f, 0.f, 0.1f, 0.f},
		{"blend wrap rotate zoom key", 1, 1, 0.9f, 0.97f, 0.03f, 0.f, 0.f, 0.f, 0.3f},
		{"blend black zoom", 1, 2, 1.f, 1.1f, 0.f, 0.f, 0.f, 0.f, 0.f},
		{"blend black offset rotate", 1, 2, 0.95f, 1.f, -0.02f, -0.03f, 0.02f, 0.05f, 0.f},
		{"blend clamp offset", 1, 0, 0.6f, 0.9f, 0.f, 0.02f, 0.01f, 0.f, 0.2f},
	};
	for (const Setting& s : settings) {
		Source src;
		const int slot = frameRingAllocateSlot();
		frameRingSetCapacity(slot, 1);
		VideoGraphRenderer r;
		FeedbackParams p;
		p.ringSlotId = slot;
		p.mode = s.mode;
		p.edge = s.edge;
		p.gain = s.gain;
		p.zoom = s.zoom;
		p.rotate = s.rotate;
		p.offsetX = s.x;
		p.offsetY = s.y;
		p.hue = s.hue;
		p.key = s.key;
		Frame three, six;
		// A coloured shape in the ramp's left half, so hue and edge settings have something to move.
		Frame in = ramp();
		for (int y = 0; y < kH; ++y) {
			for (int x = 0; x < kW / 2; ++x) {
				const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
				in[i + 1] = static_cast<unsigned char>(in[i + 1] / 2);
				in[i + 2] = static_cast<unsigned char>(y * 255 / (kH - 1));
			}
		}
		for (int tick = 1; tick <= 6; ++tick) {
			src.publish(in);
			p.frameCounter = tick;
			videoBusPublishFeedback(2, 1, p);
			const Frame out = r.renderBus(2, kW, kH);
			if (tick == 3) three = out;
			if (tick == 6) six = out;
		}
		golden(std::string(s.name) + " t3", three);
		golden(std::string(s.name) + " t6", six);
		frameRingReleaseSlot(slot);
		videoBusClear(2);
	}
}


// ---- The fade option: float history ----

float maxFloat(const FrameRingFrame& f) {
	// The colour channels only: alpha is always 255.
	float m = 0.f;
	for (size_t i = 0; i < f.rgbaF.size(); ++i) {
		if (i % 4 != 3) m = std::max(m, f.rgbaF[i]);
	}
	return m;
}

int maxDiff(const Frame& a, const Frame& b) {
	int m = 0;
	if (a.size() != b.size()) return 255;
	for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
	return m;
}

// With the option on, the trail falls to exact black, never rises, and never rests at a level on the way.
void testFadeReachesBlack() {
	std::printf("feedback fade: the trail reaches exactly black\n");
	clearBuses();
	struct Case { float gain; int mode; const char* name; int blackTick; int zeroTick; };
	// blackTick: the first tick whose 8-bit picture is all 0. zeroTick: the first tick whose float history is all 0
	// (it falls below 1/1024 of a level and snaps). Both are counted from the bright frame at tick 1.
	const Case cases[] = {
		{0.95f, 0, "g0.95 add", BLACK_095, ZERO_095},
		{0.98f, 0, "g0.98 add", BLACK_098, ZERO_098},
		{0.99f, 0, "g0.99 add", BLACK_099, ZERO_099},
		{0.95f, 1, "g0.95 blend", BLACK_095, ZERO_095},
		{0.99f, 1, "g0.99 blend", BLACK_099, ZERO_099},
	};
	for (const Case& c : cases) {
		Source src;
		const int slot = frameRingAllocateSlot();
		frameRingSetCapacity(slot, 1);
		VideoGraphRenderer r;
		FeedbackParams p;
		p.ringSlotId = slot;
		p.gain = c.gain;
		p.mode = c.mode;
		p.fadeToBlack = 1;
		int blackTick = 0, zeroTick = 0;
		float prevPeak = 1e9f;
		bool planeOk = true, stuck = false, noRise = true;
		for (int tick = 1; tick <= 3000; ++tick) {
			src.publish(tick == 1 ? ramp() : solid(0));
			p.frameCounter = tick;
			videoBusPublishFeedback(2, 1, p);
			const Frame out = r.renderBus(2, kW, kH);
			FrameRingFrame stored;
			frameRingRead(slot, 0, stored);
			const float peak = maxFloat(stored);
			if (stored.rgbaF.size() != stored.rgba.size()) planeOk = false;
			if (peak > prevPeak) noRise = false;
			if (peak == prevPeak && peak > 0.f) stuck = true;
			prevPeak = peak;
			if (!blackTick && maxLevel(out) == 0) blackTick = tick;
			if (!zeroTick && peak == 0.f) zeroTick = tick;
			if (zeroTick && tick >= zeroTick + 20) {
				check(maxLevel(out) == 0, "it stays black");
				break;
			}
		}
		if (printGolden()) std::printf("  FADE %s black at tick %d, float zero at tick %d\n", c.name, blackTick, zeroTick);
		const std::string n = c.name;
		check(planeOk, (n + ": the float plane is stored beside the 8-bit one").c_str());
		check(noRise, (n + ": the trail never rises").c_str());
		check(!stuck, (n + ": the trail never rests at a level").c_str());
		check(blackTick > 0 && zeroTick >= blackTick, (n + ": the 8-bit picture is black no later than the float history").c_str());
		check(blackTick == c.blackTick && zeroTick == c.zeroTick, (n + ": ticks to black are the pinned numbers").c_str());
		frameRingReleaseSlot(slot);
		videoBusClear(2);
	}
}

// For the first few ticks after the bright frame the two modes differ by at most one level.
void testFadeCloseToOff() {
	std::printf("feedback fade: close to the 8-bit picture at first\n");
	clearBuses();
	for (int mode = 0; mode <= 1; ++mode) {
		for (float gain : {0.95f, 0.98f, 0.99f}) {
			Source src;
			const int a = frameRingAllocateSlot();
			const int b = frameRingAllocateSlot();
			frameRingSetCapacity(a, 1);
			frameRingSetCapacity(b, 1);
			VideoGraphRenderer ra, rb;
			FeedbackParams pa, pb;
			pa.ringSlotId = a;
			pb.ringSlotId = b;
			pa.gain = pb.gain = gain;
			pa.mode = pb.mode = mode;
			pb.fadeToBlack = 1;
			int worst = 0;
			// The 8-bit path rounds at every tick, so its error grows; it reaches 2 at the fifth tick (measured).
			for (int tick = 1; tick <= 4; ++tick) {
				src.publish(tick == 1 ? ramp() : solid(0));
				pa.frameCounter = pb.frameCounter = tick;
				videoBusPublishFeedback(2, 1, pa);
				const Frame off = ra.renderBus(2, kW, kH);
				videoBusPublishFeedback(2, 1, pb);
				const Frame on = rb.renderBus(2, kW, kH);
				worst = std::max(worst, maxDiff(off, on));
				if (printGolden()) std::printf("  CLOSE mode %d gain %.2f tick %d difference %d\n", mode, gain, tick, maxDiff(off, on));
			}
			if (printGolden()) std::printf("  CLOSE mode %d gain %.2f worst difference over 4 ticks: %d\n", mode, gain, worst);
			check(worst <= 1, "on and off differ by at most one level for the first four ticks");
			frameRingReleaseSlot(a);
			frameRingReleaseSlot(b);
			videoBusClear(2);
		}
	}
}

// A picture of a new size reads the float history resampled, so the trail still fades to black and does not
// drop back to the 8-bit picture and stick.
void testFadeResampled() {
	std::printf("feedback fade: a different size resamples the float history\n");
	clearBuses();
	Source src;
	const int slot = frameRingAllocateSlot();
	frameRingSetCapacity(slot, 1);
	VideoGraphRenderer big, small;
	FeedbackParams p;
	p.ringSlotId = slot;
	p.gain = 0.98f;
	p.fadeToBlack = 1;
	for (int tick = 1; tick <= 4; ++tick) {
		src.publish(tick == 1 ? ramp() : solid(0));
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		big.renderBus(2, kW, kH);
	}
	// The small renderer is the first to draw tick 5, so it takes the step and fixes the stored size.
	const int sw = 80, sh = 45;
	int zeroTick = 0;
	bool sizeOk = true, floatOk = true;
	for (int tick = 5; tick <= 3000; ++tick) {
		src.publish(solid(0, sw, sh), sw, sh);
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		const Frame out = small.renderBus(2, sw, sh);
		FrameRingFrame stored;
		frameRingRead(slot, 0, stored);
		sizeOk = sizeOk && out.size() == static_cast<size_t>(sw) * sh * 4 && stored.width == sw && stored.height == sh;
		floatOk = floatOk && stored.rgbaF.size() == stored.rgba.size();
		if (maxFloat(stored) == 0.f) {
			zeroTick = tick;
			break;
		}
	}
	check(sizeOk, "the picture and the stored frame take the new size");
	check(floatOk, "the stored float plane has the new size too");
	check(zeroTick > 0, "after the size change the trail still reaches exactly black");
	if (printGolden()) std::printf("  RESAMPLED fade to black at tick %d\n", zeroTick);
	// The same change with the option off sticks at the 8-bit grey (0.98 -> 25).
	const int slotOff = frameRingAllocateSlot();
	frameRingSetCapacity(slotOff, 1);
	VideoGraphRenderer big2, small2;
	p.ringSlotId = slotOff;
	p.fadeToBlack = 0;
	for (int tick = 1; tick <= 4; ++tick) {
		src.publish(tick == 1 ? ramp() : solid(0));
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		big2.renderBus(2, kW, kH);
	}
	Frame last;
	for (int tick = 5; tick <= 800; ++tick) {
		src.publish(solid(0, sw, sh), sw, sh);
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		last = small2.renderBus(2, sw, sh);
	}
	check(maxLevel(last) > 0, "with the option off the resized trail still sticks (as today)");
	frameRingReleaseSlot(slot);
	frameRingReleaseSlot(slotOff);
	videoBusClear(2);
}

// A second consumer at another size shares the one store per tick, and reads the same picture resampled.
void testFadeTwoRenderers() {
	std::printf("feedback fade: two renderers at different sizes, one store per tick\n");
	clearBuses();
	Source src;
	const int slot = frameRingAllocateSlot();
	frameRingSetCapacity(slot, 8);
	VideoGraphRenderer a, b;
	FeedbackParams p;
	p.ringSlotId = slot;
	p.gain = 0.9f;
	p.fadeToBlack = 1;
	bool oneStore = true, sameSize = true, ownSize = true;
	for (int tick = 1; tick <= 6; ++tick) {
		src.publish(tick == 1 ? ramp() : solid(0));
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		const Frame big = a.renderBus(2, kW, kH);
		const Frame small = b.renderBus(2, 80, 45);
		oneStore = oneStore && frameRingCount(slot) == tick;
		FrameRingFrame stored;
		frameRingRead(slot, 0, stored);
		sameSize = sameSize && stored.width == kW && stored.height == kH && stored.rgbaF.size() == stored.rgba.size() && stored.frameCounter == tick;
		ownSize = ownSize && small.size() == 80u * 45u * 4u && big.size() == static_cast<size_t>(kW) * kH * 4;
	}
	check(ownSize, "both consumers get their own size");
	check(oneStore, "one frame is stored per tick however many consumers draw it");
	check(sameSize, "the first renderer in a tick fixes the stored size and its float plane");
	// The small renderer going first on a later tick fixes the size at 80 x 45, and the other resamples up.
	src.publish(solid(0));
	p.frameCounter = 7;
	videoBusPublishFeedback(2, 1, p);
	b.renderBus(2, 80, 45);
	const Frame bigAfter = a.renderBus(2, kW, kH);
	FrameRingFrame stored;
	frameRingRead(slot, 0, stored);
	check(stored.width == 80 && stored.height == 45 && frameRingCount(slot) == 7, "the first renderer of tick 7 fixed the stored size");
	check(bigAfter.size() == static_cast<size_t>(kW) * kH * 4, "the second renderer reads it resampled");
	frameRingReleaseSlot(slot);
	videoBusClear(2);
}

// One extra float frame per module, only with the option on, given back on switching off and on removal.
void testFadeMemory() {
	std::printf("feedback fade: memory\n");
	clearBuses();
	const int sizes[2][2] = {{640, 480}, {1440, 1080}};
	for (const auto& size : sizes) {
		const int w = size[0], h = size[1];
		Source src;
		const int slot = frameRingAllocateSlot();
		frameRingSetCapacity(slot, 1);
		VideoGraphRenderer r;
		FeedbackParams p;
		p.ringSlotId = slot;
		FrameRingFrame stored;
		// Off: no float plane.
		src.publish(solid(200, w, h), w, h);
		p.frameCounter = 1;
		videoBusPublishFeedback(2, 1, p);
		r.renderBus(2, w, h);
		check(frameRingRead(slot, 0, stored) && stored.rgbaF.empty(), "off: no float plane");
		// On: one float frame, w x h x 16 bytes, beside the 8-bit one.
		p.fadeToBlack = 1;
		p.frameCounter = 2;
		videoBusPublishFeedback(2, 1, p);
		r.renderBus(2, w, h);
		check(frameRingRead(slot, 0, stored) && stored.rgbaF.size() * sizeof(float) == static_cast<size_t>(w) * h * 16, "on: one float frame of w x h x 16 bytes");
		if (printGolden()) std::printf("  MEMORY %d x %d: 8-bit %zu bytes, float %zu bytes\n", w, h, stored.rgba.size(), stored.rgbaF.size() * sizeof(float));
		// Switched off again: the next store frees it.
		p.fadeToBlack = 0;
		p.frameCounter = 3;
		videoBusPublishFeedback(2, 1, p);
		r.renderBus(2, w, h);
		check(frameRingRead(slot, 0, stored) && stored.rgbaF.empty(), "switched off: the float plane is freed at the next store");
		// On again, then CLEAR frees it too.
		p.fadeToBlack = 1;
		p.frameCounter = 4;
		videoBusPublishFeedback(2, 1, p);
		r.renderBus(2, w, h);
		check(frameRingRead(slot, 0, stored) && !stored.rgbaF.empty(), "on again: the float plane is back");
		frameRingClear(slot, 7);
		check(!frameRingRead(slot, 0, stored), "clear: nothing is stored");
		frameRingReleaseSlot(slot);
		videoBusClear(2);
	}
}

// Switching the option on mid-trail starts from the 8-bit picture and then fades.
void testFadeSwitchOnMidTrail() {
	std::printf("feedback fade: switching on in the middle of a trail\n");
	clearBuses();
	Source src;
	const int slot = frameRingAllocateSlot();
	frameRingSetCapacity(slot, 1);
	VideoGraphRenderer r;
	FeedbackParams p;
	p.ringSlotId = slot;
	p.gain = 0.98f;
	Frame out;
	for (int tick = 1; tick <= 400; ++tick) {
		src.publish(tick == 1 ? ramp() : solid(0));
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		out = r.renderBus(2, kW, kH);
	}
	check(maxLevel(out) == 25, "the trail is stuck at 25 with the option off");
	p.fadeToBlack = 1;
	int zeroTick = 0;
	for (int tick = 401; tick <= 4000 && !zeroTick; ++tick) {
		src.publish(solid(0));
		p.frameCounter = tick;
		videoBusPublishFeedback(2, 1, p);
		out = r.renderBus(2, kW, kH);
		FrameRingFrame stored;
		frameRingRead(slot, 0, stored);
		if (maxFloat(stored) == 0.f) zeroTick = tick;
	}
	check(zeroTick > 401 && maxLevel(out) == 0, "switched on, the stuck trail fades to black");
	if (printGolden()) std::printf("  MIDTRAIL stuck at 25, black at tick %d\n", zeroTick);
	frameRingReleaseSlot(slot);
	videoBusClear(2);
}

}  // namespace

int main() {
	testStuckTrail();
	testSettings();
	testFadeReachesBlack();
	testFadeCloseToOff();
	testFadeResampled();
	testFadeTwoRenderers();
	testFadeMemory();
	testFadeSwitchOnMidTrail();
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
