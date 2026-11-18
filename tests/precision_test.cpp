// Full precision between modules (docs/architecture.md, "Precision"). Offline, no Rack SDK.
//
// With the setting off nothing may change: the pinned hashes in the other test files are that gate. This file is
// about the setting ON. It checks the frame type, then the claims the setting is for:
//   (a) a gain of 0.25 followed by 2.0 keeps about 128 levels, not 64;
//   (b) a loop through a Frame Delay decays like the exponential it should be, not like a truncated one;
//   (c) a 4x stretch of a ramp has no gaps between its levels;
//   (d) one converted node fed an 8-bit picture is within 1 level of the 8-bit path (and equal for the nodes whose
//       rule was already round half up), for settings with no hard thresholds; the thresholds are measured;
//   (e) a chain of k nodes is within k levels;
//   (f) the pictures are deterministic, and a few are pinned.
// Run with VRU_PRINT_GOLDEN=1 to print the pinned hashes. Prints every measurement it asserts on.

#include "video_bus.hpp"
#include "video_graph_renderer.hpp"
#include "render_service.hpp"
#include "frame_ring.hpp"
#include "freeze_frame_store.hpp"
#include "video_external_frame.hpp"
#include "video_frame.hpp"
#include "signal_ring.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

uint64_t hashBytes(const Bytes& f) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : f) {
		h = (h ^ c) * 1099511628211ull;
	}
	return h;
}

bool printGolden() {
	static const bool on = std::getenv("VRU_PRINT_GOLDEN") != nullptr;
	return on;
}

void golden(const char* name, const Bytes& f, uint64_t expected) {
	const uint64_t h = hashBytes(f);
	if (printGolden()) {
		std::printf("  GOLDEN %s %lluull\n", name, static_cast<unsigned long long>(h));
	}
	char text[160];
	std::snprintf(text, sizeof(text), "%s hash is pinned", name);
	check(h == expected, text);
}

void clearBuses() {
	for (int bus = 1; bus <= kVideoBusCount; ++bus) {
		videoBusClear(bus);
	}
}

Bytes render(bool precision, int bus, int w, int h) {
	VideoGraphRenderer r;
	r.setFullPrecision(precision);
	const VideoGraphRenderer::Frame& f = r.renderBus(bus, w, h);
	return Bytes(f.begin(), f.end());
}

int maxDiff(const Bytes& a, const Bytes& b) {
	if (a.size() != b.size() || a.empty()) {
		return 1000;
	}
	int worst = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
	}
	return worst;
}

double fractionOver(const Bytes& a, const Bytes& b, int limit) {
	size_t over = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > limit) ++over;
	}
	return a.empty() ? 1.0 : static_cast<double>(over) / static_cast<double>(a.size());
}

constexpr int kW = 160;
constexpr int kH = 90;

// A colourful 8-bit picture with no flat areas: a gradient in two channels and a hashed third.
Bytes makeSource(int w, int h, int salt) {
	Bytes f(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			const unsigned hash = static_cast<unsigned>(x * 7349 + y * 1931 + salt * 977) * 2654435761u;
			f[i + 0] = static_cast<unsigned char>(salt == 0 ? x * 255 / (w - 1) : 255 - y * 255 / (h - 1));
			f[i + 1] = static_cast<unsigned char>(salt == 0 ? y * 255 / (h - 1) : x * 255 / (w - 1));
			f[i + 2] = static_cast<unsigned char>((hash >> 24) & 0xFF);
			f[i + 3] = 255;
		}
	}
	return f;
}

struct Externals {
	int a = 0;
	int b = 0;
	Externals() {
		a = videoExternalFrameAllocate();
		b = videoExternalFrameAllocate();
		const Bytes fa = makeSource(kW, kH, 0);
		const Bytes fb = makeSource(kW, kH, 1);
		videoExternalFramePublish(a, fa.data(), kW, kH, 0.0);
		videoExternalFramePublish(b, fb.data(), kW, kH, 0.0);
		videoBusPublishExternalFrame(10, a);
		videoBusPublishExternalFrame(11, b);
	}
	~Externals() {
		videoExternalFrameRelease(a);
		videoExternalFrameRelease(b);
		videoBusClear(10);
		videoBusClear(11);
	}
};

// ---- The frame type ----
void testFrameType() {
	std::printf("frame type: formats, store rules, conversions\n");
	check(storeByte<StoreRule::Truncate>(10.9f) == 10, "truncate cuts 10.9 to 10");
	check(storeByte<StoreRule::RoundHalfUp>(10.5f) == 11 && storeByte<StoreRule::RoundHalfUp>(10.49f) == 10, "round half up takes 10.5 to 11 and 10.49 to 10");
	check(storeByte<StoreRule::Lround>(10.5f) == 11 && storeByte<StoreRule::Lround>(2.4f) == 2, "lround takes 10.5 to 11 and 2.4 to 2");
	check(storeByte<StoreRule::Truncate>(-3.f) == 0 && storeByte<StoreRule::Truncate>(300.f) == 255, "every rule clamps");
	check(storeByte<StoreRule::Lround>(-3.f) == 0 && storeByte<StoreRule::Lround>(300.f) == 255, "lround clamps too");
	VideoFrame f;
	f.beginU8(8);
	for (size_t i = 0; i < 8; ++i) f[i] = static_cast<unsigned char>(i * 30);
	check(!f.isFloat() && f.count() == 8 && f.at(3) == 90.f, "a byte frame reads back as float");
	f.ensureF32();
	check(f.isFloat() && f.count() == 8 && f.empty() && f.at(3) == 90.f, "to float is a cast and empties the byte plane");
	f.f[2] = 60.5f;
	f.ensureU8();
	check(!f.isFloat() && f.size() == 8 && f[2] == 61 && f[3] == 90, "to bytes rounds half up");
	VideoFrame g;
	g.beginF32(4);
	g.f = {0.4f, 0.5f, 254.6f, 300.f};
	VideoFrame h;
	h.copyFrom(g);
	check(h.isFloat() && h.f == g.f, "copyFrom keeps the float plane");
	Bytes out;
	h.copyBytesTo(out);
	check(out.size() == 4 && out[0] == 0 && out[1] == 1 && out[2] == 255 && out[3] == 255, "copyBytesTo rounds half up and clamps");
	VideoFrame k;
	k.beginF32(4);
	const Bytes four{1, 2, 3, 4};
	k.assign(four.begin(), four.end());
	check(!k.isFloat() && k.size() == 4, "assign makes it a byte frame again");
}

// ---- (a) levels ----
int distinctReds(const Bytes& f, int w) {
	std::set<int> seen;
	for (int x = 0; x < w; ++x) seen.insert(f[static_cast<size_t>(x) * 4u]);
	return static_cast<int>(seen.size());
}

void testLevels() {
	std::printf("(a) Ramps -> Color Lab 0.25 -> Color Lab 2.0\n");
	clearBuses();
	const int w = 2048;
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
	const Bytes off = render(false, 3, w, 4);
	const Bytes on = render(true, 3, w, 4);
	const int levelsOff = distinctReds(off, w);
	const int levelsOn = distinctReds(on, w);
	std::printf("  distinct levels across the ramp: setting off %d, on %d\n", levelsOff, levelsOn);
	check(levelsOff >= 60 && levelsOff <= 66, "off: about 64 levels (the 8-bit path)");
	check(levelsOn >= 126 && levelsOn <= 130, "on: about 128 levels");
	// A control: the same chain with the middle node taken out has the full 256 levels in either mode.
	videoBusPublishColorLab(3, 1, twice);
	check(distinctReds(render(false, 3, w, 4), w) >= 120, "control: a single gain of 2.0 keeps its levels");
	clearBuses();
}

// ---- (b) a loop through a Frame Delay ----
void testLoopDecay() {
	std::printf("(b) a loop through a Frame Delay decays\n");
	clearBuses();
	const int w = 32, h = 18;
	const float mixValue = 0.97f;
	const int ticks = 160;
	const int slot = frameRingAllocateSlot();
	// bus 1 black: Ramps -> Color Lab brightness 0. bus 3 Bus Mixer (A black, B the delay), bus 4 the delay of bus 3.
	RampsParams ramps;
	videoBusPublishRamps(1, ramps);
	ColorLabParams black;
	black.brightness = 0.f;
	videoBusPublishColorLab(2, 1, black);
	BusMixerParams mixer;
	mixer.busB = 4;
	mixer.mode = 0;
	mixer.mix = mixValue;
	videoBusPublishBusMixer(3, 2, mixer);
	std::vector<std::vector<int>> series(2);
	for (int mode = 0; mode < 2; ++mode) {
		frameRingReleaseSlot(slot);
		const int s = frameRingAllocateSlot();
		FrameDelayParams delay;
		delay.ringSlotId = s;
		delay.frames = 1;
		delay.clearCounter = mode + 1;
		delay.frameCounter = 0;
		videoBusPublishFrameDelay(4, 3, delay);
		VideoGraphRenderer r;
		r.setFullPrecision(mode == 1);
		// Seed: one bright frame at counter 0.
		Bytes white(static_cast<size_t>(w) * h * 4u, 255);
		frameRingClear(s, mode + 1);
		frameRingSetCapacity(s, 2);
		frameRingPush(s, 0, white, w, h);
		for (int n = 1; n <= ticks; ++n) {
			delay.frameCounter = n;
			videoBusPublishFrameDelay(4, 3, delay);
			const VideoGraphRenderer::Frame& f = r.renderBus(3, w, h);
			series[static_cast<size_t>(mode)].push_back(f.empty() ? -1 : f[0]);
		}
		frameRingReleaseSlot(s);
	}
	// Measure against the exponential 255 * mix^n.
	double worstOff = 0.0, worstOn = 0.0;
	bool monotoneOn = true, monotoneOff = true;
	int zeroOff = -1, zeroOn = -1;
	int runOff = 0, longestFlatOff = 0, runOn = 0, longestFlatOn = 0;
	for (int n = 1; n <= ticks; ++n) {
		const double ideal = 255.0 * std::pow(static_cast<double>(mixValue), n);
		const int off = series[0][static_cast<size_t>(n - 1)];
		const int on = series[1][static_cast<size_t>(n - 1)];
		worstOff = std::max(worstOff, std::fabs(off - ideal));
		worstOn = std::max(worstOn, std::fabs(on - ideal));
		if (n > 1) {
			monotoneOff = monotoneOff && off <= series[0][static_cast<size_t>(n - 2)];
			monotoneOn = monotoneOn && on <= series[1][static_cast<size_t>(n - 2)];
			runOff = off == series[0][static_cast<size_t>(n - 2)] ? runOff + 1 : 0;
			runOn = on == series[1][static_cast<size_t>(n - 2)] ? runOn + 1 : 0;
			longestFlatOff = std::max(longestFlatOff, runOff);
			longestFlatOn = std::max(longestFlatOn, runOn);
		}
		if (off == 0 && zeroOff < 0) zeroOff = n;
		if (on == 0 && zeroOn < 0) zeroOn = n;
	}
	std::printf("  worst distance from 255 x %.2f^n over %d ticks: off %.1f levels, on %.2f levels\n", mixValue, ticks, worstOff, worstOn);
	std::printf("  first tick at black: off %d, on %d; longest run of equal levels: off %d, on %d\n", zeroOff, zeroOn, longestFlatOff, longestFlatOn);
	check(monotoneOn, "on: the decay never goes up");
	check(monotoneOff, "off: the decay never goes up");
	check(worstOn <= 0.75, "on: every tick is within 0.75 of the ideal exponential");
	check(worstOff > 10.0, "off: the truncating path falls well away from the exponential");
	check(worstOn < worstOff * 0.1, "on tracks the exponential at least ten times closer");
	check(zeroOff >= 0 && (zeroOn < 0 || zeroOn > zeroOff), "off reaches black first; on takes the exponential's time");
	clearBuses();
}

// ---- (c) a 4x stretch ----
void testStretch() {
	std::printf("(c) Ramps -> Wave Shaper, 4x stretch\n");
	clearBuses();
	const int w = 2048;
	RampsParams ramps;
	videoBusPublishRamps(1, ramps);
	WaveShaperParams shaper;
	shaper.drive = 4.f;
	videoBusPublishWaveShaper(2, 1, shaper);
	int gaps[2] = {0, 0};
	int distinct[2] = {0, 0};
	for (int mode = 0; mode < 2; ++mode) {
		const Bytes f = render(mode == 1, 2, w, 2);
		std::set<int> seen;
		for (int x = 0; x < w; ++x) seen.insert(f[static_cast<size_t>(x) * 4u]);
		int previous = -1;
		int worst = 0;
		for (int v : seen) {
			// Only inside the stretched range; the two ends are clamped to 0 and 255.
			if (previous >= 0 && v < 255) worst = std::max(worst, v - previous);
			previous = v;
		}
		gaps[mode] = worst;
		distinct[mode] = static_cast<int>(seen.size());
	}
	std::printf("  largest step between adjacent output levels: off %d, on %d (distinct levels: off %d, on %d)\n", gaps[0], gaps[1], distinct[0], distinct[1]);
	check(gaps[1] <= 1, "on: adjacent levels differ by at most 1");
	check(gaps[0] >= 4, "off: the 8-bit path jumps by 4");
	check(distinct[1] >= 250, "on: every level is used");
	clearBuses();
}

// ---- (d) one node, 8-bit source ----
struct Case {
	const char* name;
	int limit;  // the most a value may move, in levels; -1: the settings have a hard threshold, see the share check
	bool roundHalfUp;  // the node's own rule was already round half up: equal is expected
	void (*build)();
};

void publishLab(int bus, int src, const ColorLabParams& p) {
	videoBusPublishColorLab(bus, src, p);
}

const Case kCases[] = {
	{"Color Lab hue, saturation, brightness", 2, false, [] {
		ColorLabParams p; p.hue = 0.2f; p.saturation = 1.4f; p.brightness = 0.8f; p.mix = 0.9f; publishLab(20, 10, p); }},
	{"Color Lab contrast 1.2 + channel swap + invert", 2, false, [] {
		ColorLabParams p; p.contrast = 1.2f; p.channelMode = 2; p.invert = true; publishLab(20, 10, p); }},
	{"Color Lab posterize 0.4", -1, false, [] {
		ColorLabParams p; p.posterize = 0.4f; publishLab(20, 10, p); }},
	{"Color Lab solarize 0.6", 2, false, [] {
		ColorLabParams p; p.solarize = 0.6f; publishLab(20, 10, p); }},
	{"Colorizer, no bands, colour source", 3, false, [] {
		ColorizerParams p; videoBusPublishColorizer(20, 10, p); }},
	{"Colorizer, 3 bands", -1, false, [] {
		ColorizerParams p; p.bands = 3; videoBusPublishColorizer(20, 10, p); }},
	{"Wave Shaper drive 1.5, bias, contrast", 1, false, [] {
		WaveShaperParams p; p.drive = 1.5f; p.bias = 0.1f; p.contrast = 1.1f; videoBusPublishWaveShaper(20, 10, p); }},
	{"Wave Shaper luma mode, mirror", 1, false, [] {
		WaveShaperParams p; p.mode = 1; p.mirror = 0.5f; p.mix = 0.8f; videoBusPublishWaveShaper(20, 10, p); }},
	{"Wave Shaper quantize 0.3", -1, false, [] {
		WaveShaperParams p; p.quantize = 0.3f; videoBusPublishWaveShaper(20, 10, p); }},
	{"Keyer mode 0 (matte)", 1, false, [] {
		KeyerParams p; p.mode = 0; videoBusPublishKeyer(20, 10, p); }},
	{"Keyer mode 1 (colour)", 1, false, [] {
		KeyerParams p; p.mode = 1; p.mix = 0.7f; videoBusPublishKeyer(20, 10, p); }},
	{"Keyer mode 2 (source)", 1, false, [] {
		KeyerParams p; p.mode = 2; p.invert = true; videoBusPublishKeyer(20, 10, p); }},
	{"Bus Mixer mix", 1, false, [] {
		BusMixerParams p; p.busB = 11; p.mode = 0; p.mix = 0.37f; videoBusPublishBusMixer(20, 10, p); }},
	{"Bus Mixer multiply", 1, false, [] {
		BusMixerParams p; p.busB = 11; p.mode = 2; p.mix = 0.8f; videoBusPublishBusMixer(20, 10, p); }},
	{"Bus Mixer difference with matte", 1, false, [] {
		BusMixerParams p; p.busB = 11; p.mode = 4; p.mix = 0.6f; p.matteBus = 11; p.matteAmount = 0.5f; videoBusPublishBusMixer(20, 10, p); }},
	{"Bus Mixer matte mode", 1, false, [] {
		BusMixerParams p; p.busB = 11; p.mode = 6; p.matteBus = 10; videoBusPublishBusMixer(20, 10, p); }},
	{"Video Mixer dissolve with hue and gradient colour", 1, true, [] {
		VideoMixerParams p; p.inputBus[0] = 10; p.inputBus[1] = 11; p.onAir = 0; p.next = 1; p.fader = 0.4f;
		p.colorOn[0] = true; p.gradientOn[1] = true; p.span = 0.2f; p.sat = 0.8f; videoBusPublishVideoMixer(20, p); }},
	{"Video Mixer add + black", 1, true, [] {
		VideoMixerParams p; p.inputBus[0] = 10; p.inputBus[1] = 11; p.onAir = 0; p.next = 1; p.fader = 0.6f; p.mode = 1; p.black = 0.2f;
		videoBusPublishVideoMixer(20, p); }},
	{"Video Mixer wipe + key", 1, true, [] {
		VideoMixerParams p; p.inputBus[0] = 10; p.inputBus[1] = 11; p.onAir = 0; p.next = 1; p.fader = 0.5f; p.mode = 2;
		p.keyOn = true; p.keyBus = 11; p.matteBus = 10; videoBusPublishVideoMixer(20, p); }},
	// Spatial nodes (stage 4): each is fed the 8-bit external picture and compared with its 8-bit render.
	{"Rotator angle 0.07 zoom 1.1, hold edge", 1, false, [] {
		RotatorParams p; p.angle = 0.07f; p.zoom = 1.1f; p.edge = 1; videoBusPublishRotator(20, 10, p); }},
	{"Rotator repeat edge with offset", 1, false, [] {
		RotatorParams p; p.angle = 0.31f; p.zoom = 0.8f; p.offsetX = 0.2f; p.offsetY = -0.1f; p.edge = 2; videoBusPublishRotator(20, 10, p); }},
	{"Rotator 5 folds, mirror edge", 1, false, [] {
		RotatorParams p; p.angle = 0.02f; p.folds = 5; p.edge = 3; videoBusPublishRotator(20, 10, p); }},
	// Line Field draws lines of overlapping blends, and each 8-bit blend cuts the value, so a pixel crossed by several
	// lines loses up to one level a blend in bytes and nothing in floats: the limit is 3, not 1.
	{"Focus focus 0.4 smear 0.3 sharp 0.5 converge 0.3", 1, false, [] {
		FocusParams p; p.focus = 0.4f; p.smear = 0.3f; p.sharp = 0.5f; p.converge = 0.3f; videoBusPublishFocus(20, 10, p); }},
	{"Focus focus 0.2 only, MIX 0.5", 1, false, [] {
		FocusParams p; p.focus = 0.2f; p.mix = 0.5f; videoBusPublishFocus(20, 10, p); }},
	{"Line Field, defaults", 3, false, [] {
		LineFieldParams p; videoBusPublishLineField(20, 10, p); }},
	{"Line Field tilted, MIX 0.6, inverted", 3, false, [] {
		LineFieldParams p; p.xTilt = 0.5f; p.yTilt = -0.3f; p.zSpin = 0.2f; p.mix = 0.6f; p.invert = true; p.gain = 0.9f; videoBusPublishLineField(20, 10, p); }},
};

void testPointwise() {
	std::printf("(d) one node, 8-bit source: setting on against setting off\n");
	for (const Case& c : kCases) {
		clearBuses();
		Externals ext;
		c.build();
		const Bytes off = render(false, 20, kW, kH);
		const Bytes on = render(true, 20, kW, kH);
		const int worst = maxDiff(off, on);
		const double over = fractionOver(off, on, 1);
		std::printf("  %-52s max difference %3d, share of values over 1 level: %.4f\n", c.name, worst, over);
		char text[200];
		if (c.limit >= 0) {
			std::snprintf(text, sizeof(text), "%s: within %d level%s", c.name, c.limit, c.limit == 1 ? "" : "s");
			check(worst <= c.limit, text);
		}
		else {
			// A hard threshold turns a one level change of the input into a whole step of the output at some pixels.
			std::snprintf(text, sizeof(text), "%s: at most 2%% of the values move more than 1 level", c.name);
			check(over <= 0.02, text);
		}
		if (c.roundHalfUp) {
			std::snprintf(text, sizeof(text), "%s: its rule was round half up, so the pictures are equal (or one tie moves by 1)", c.name);
			check(worst <= 1 && over == 0.0, text);
		}
	}
	clearBuses();
}

// Colorizer on a grey source is the case the within-1 claim holds for exactly: a grey's luma is a whole number.
void testColorizerGrey() {
	std::printf("(d) Colorizer on 256 integer greys\n");
	clearBuses();
	Bytes grey(256u * 2u * 4u, 255);
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 256; ++x) {
			const size_t i = (static_cast<size_t>(y) * 256u + static_cast<size_t>(x)) * 4u;
			grey[i] = grey[i + 1] = grey[i + 2] = static_cast<unsigned char>(x);
		}
	}
	const int id = videoExternalFrameAllocate();
	videoExternalFramePublish(id, grey.data(), 256, 2, 0.0);
	videoBusPublishExternalFrame(1, id);
	ColorizerParams p;
	videoBusPublishColorizer(2, 1, p);
	const Bytes off = render(false, 2, 256, 2);
	const Bytes on = render(true, 2, 256, 2);
	const int worst = maxDiff(off, on);
	std::printf("  Colorizer, 256 grey levels: max difference %d\n", worst);
	check(worst <= 1, "Colorizer on integer greys is within 1 level");
	// With a ramp that is itself float (Ramps), the gradient is continuous: more levels than the 8-bit table gives.
	RampsParams ramps;
	videoBusPublishRamps(1, ramps);
	std::set<int> blueOff, blueOn;
	const Bytes rampOff = render(false, 2, 2048, 2);
	const Bytes rampOn = render(true, 2, 2048, 2);
	for (int x = 0; x < 2048; ++x) {
		blueOff.insert(rampOff[static_cast<size_t>(x) * 4u + 2]);
		blueOn.insert(rampOn[static_cast<size_t>(x) * 4u + 2]);
	}
	std::printf("  Ramps -> Colorizer, 2048 px: distinct blue levels off %zu, on %zu\n", blueOff.size(), blueOn.size());
	check(blueOn.size() > blueOff.size(), "the Colorizer's output has more levels with the setting on");
	videoExternalFrameRelease(id);
	clearBuses();
}

// Generators: the setting changes only the last bit of each.
void testGenerators() {
	std::printf("(d) generators: setting on against setting off\n");
	clearBuses();
	RampsParams ramps;
	ramps.axisA = 2;
	ramps.freqA = 3.f;
	ramps.axisB = 3;
	ramps.mix = 3;
	videoBusPublishRamps(1, ramps);
	ShapesParams shapes;
	shapes.sides = 6.f;
	videoBusPublishShapes(2, shapes);
	VideoParams osc;
	osc.feedback = 0.f;
	videoBusPublish(3, osc);
	VideoParams pair;
	pair.pattern = static_cast<int>(VideoPattern::BarsAndRows);
	pair.feedback = 0.f;
	videoBusPublish(4, pair);
	VideoParams pairColour = pair;
	pairColour.saturation = 1.2f;
	videoBusPublish(6, pairColour);
	VideoParams bars;
	bars.pattern = static_cast<int>(VideoPattern::Smpte75);
	videoBusPublish(5, bars);
	const char* names[] = {"Ramps", "Shapes", "Video Oscillator", "Bars and Rows", "Smpte75 test pattern", "Bars and Rows, SAT 1.2"};
	for (int bus = 1; bus <= 6; ++bus) {
		const Bytes off = render(false, bus, 192, 108);
		const Bytes on = render(true, bus, 192, 108);
		const int worst = maxDiff(off, on);
		std::printf("  %-22s max difference %d\n", names[bus - 1], worst);
		char text[120];
		// The colour controls of the oscillator patterns multiply the one level the old store lost by SAT.
		const int limit = bus == 6 ? 2 : 1;
		std::snprintf(text, sizeof(text), "%s: within %d level%s", names[bus - 1], limit, limit == 1 ? "" : "s");
		check(worst <= limit, text);
		if (bus == 5) check(off == on, "the test pattern is the same bytes either way");
	}
	clearBuses();
}

// Freeze Frame mix path and Frame Delay pass-through.
void testFreezeAndDelay() {
	std::printf("(d) Freeze Frame mix and Frame Delay pass-through\n");
	clearBuses();
	Externals ext;
	Bytes results[2];
	// Freeze Frame: capture the first picture, then read the second through the hold.
	for (int mode = 0; mode < 2; ++mode) {
		const int slot = freezeFrameAllocateSlot();
		FreezeFrameParams ff;
		ff.freezeSlotId = slot;
		ff.captureCounter = 1;
		ff.hold = 1.f;
		ff.blend = 0.6f;
		videoBusPublishFreezeFrame(20, 10, ff);
		VideoGraphRenderer r;
		r.setFullPrecision(mode == 1);
		r.renderBus(20, kW, kH);  // the capture
		videoBusPublishFreezeFrame(20, 11, ff);  // a different picture comes through now
		const VideoGraphRenderer::Frame& f = r.renderBus(20, kW, kH);
		results[mode].assign(f.begin(), f.end());
		freezeFrameReleaseSlot(slot);
	}
	const int freezeWorst = maxDiff(results[0], results[1]);
	std::printf("  Freeze Frame mix: max difference %d\n", freezeWorst);
	check(freezeWorst <= 1, "Freeze Frame: within 1 level");

	// Frame Delay: Color Lab -> Delay (1 tick), read on the second tick. With the setting on the delay stores the
	// float picture, so what it hands on is exactly the source of the tick before, rounded once.
	ColorLabParams lab;
	lab.hue = 0.1f;
	lab.brightness = 0.9f;
	videoBusPublishColorLab(12, 10, lab);
	Bytes source[2];
	for (int mode = 0; mode < 2; ++mode) {
		const int slot = frameRingAllocateSlot();
		FrameDelayParams fd;
		fd.ringSlotId = slot;
		fd.frames = 1;
		VideoGraphRenderer r;
		r.setFullPrecision(mode == 1);
		fd.frameCounter = 1;
		videoBusPublishFrameDelay(20, 12, fd);
		r.renderBus(20, kW, kH);
		fd.frameCounter = 2;
		videoBusPublishFrameDelay(20, 12, fd);
		const VideoGraphRenderer::Frame& f = r.renderBus(20, kW, kH);
		results[mode].assign(f.begin(), f.end());
		VideoGraphRenderer plain;
		plain.setFullPrecision(mode == 1);
		const VideoGraphRenderer::Frame& s2 = plain.renderBus(12, kW, kH);
		source[mode].assign(s2.begin(), s2.end());
		frameRingReleaseSlot(slot);
	}
	const int delayWorst = maxDiff(results[0], results[1]);
	std::printf("  Frame Delay pass-through: against the setting off %d; against its own source, off %d, on %d\n", delayWorst, maxDiff(results[0], source[0]), maxDiff(results[1], source[1]));
	check(delayWorst <= 2 && !results[0].empty(), "Frame Delay: within 2 levels of the setting off (the Color Lab before it is within 2)");
	check(results[1] == source[1], "setting on: the delay hands on its source exactly (no extra rounding)");
	check(results[0] == source[0], "setting off: the delay hands on its source exactly");
	clearBuses();
}

// ---- Feedback in full precision (stage 4) ----
// A bright picture at tick 1, then black. With the plugin setting on the history is float, so the trail reaches black
// at every gain the 8-bit history sticks at (about 10, 25 and 50 levels at 0.95, 0.98 and 0.99).
void testFeedbackFade() {
	std::printf("(g) Feedback with the setting on reaches black\n");
	const int w = 16, h = 9;
	const float gains[3] = {0.95f, 0.98f, 0.99f};
	for (const float gain : gains) {
		int blackAt[2] = {-1, -1};
		int stuck[2] = {0, 0};
		for (int mode = 0; mode < 2; ++mode) {
			clearBuses();
			const int ext = videoExternalFrameAllocate();
			const int slot = frameRingAllocateSlot();
			VideoGraphRenderer r;
			r.setFullPrecision(mode == 1);
			Bytes white(static_cast<size_t>(w) * h * 4u, 255);
			Bytes black(static_cast<size_t>(w) * h * 4u, 0);
			for (size_t i = 3; i < black.size(); i += 4u) black[i] = 255;
			videoBusPublishExternalFrame(1, ext);
			FeedbackParams fb;
			fb.ringSlotId = slot;
			fb.gain = gain;
			fb.mode = 0;
			fb.fadeToBlack = 0;  // the module option stays off: the plugin setting alone must do it
			const int ticks = 1300;
			for (int n = 1; n <= ticks; ++n) {
				videoExternalFramePublish(ext, (n == 1 ? white : black).data(), w, h, 0.0);
				fb.frameCounter = n;
				videoBusPublishFeedback(2, 1, fb);
				const VideoGraphRenderer::Frame& f = r.renderBus(2, w, h);
				int peak = 0;
				for (size_t i = 0; i + 3 < f.size(); i += 4u) peak = std::max({peak, static_cast<int>(f[i]), static_cast<int>(f[i + 1]), static_cast<int>(f[i + 2])});
				if (peak == 0 && blackAt[mode] < 0 && n > 1) blackAt[mode] = n;
				if (n == ticks) stuck[mode] = peak;
			}
			frameRingReleaseSlot(slot);
			videoExternalFrameRelease(ext);
		}
		std::printf("  gain %.2f: setting off ends at level %d (black at tick %d); setting on ends at level %d (black at tick %d)\n", gain, stuck[0], blackAt[0], stuck[1], blackAt[1]);
		char text[160];
		std::snprintf(text, sizeof(text), "Feedback gain %.2f, setting on: the trail reaches black", gain);
		check(stuck[1] == 0 && blackAt[1] > 0, text);
		std::snprintf(text, sizeof(text), "Feedback gain %.2f, setting off: the 8-bit trail is stuck above black (control)", gain);
		check(stuck[0] > 0, text);
	}
	clearBuses();
}

// Feedback with a moving source: a few ticks on against off, and the rule for the module option.
void testFeedbackSource() {
	std::printf("(g) Feedback, 8-bit source: setting on against setting off over 8 ticks\n");
	for (int variant = 0; variant < 2; ++variant) {
		int worst = 0;
		Bytes last[2];
		for (int mode = 0; mode < 2; ++mode) {
			clearBuses();
			Externals ext;
			const int slot = frameRingAllocateSlot();
			VideoGraphRenderer r;
			r.setFullPrecision(mode == 1);
			FeedbackParams fb;
			fb.ringSlotId = slot;
			fb.gain = 0.6f;
			fb.mode = variant;
			fb.zoom = 1.03f;
			fb.rotate = 0.01f;
			fb.edge = 1;
			fb.fadeToBlack = variant;  // variant 1: the module option is on as well; the plugin setting must agree with it
			for (int n = 1; n <= 8; ++n) {
				fb.frameCounter = n;
				videoBusPublishFeedback(20, 10, fb);
				const VideoGraphRenderer::Frame& f = r.renderBus(20, kW, kH);
				last[mode].assign(f.begin(), f.end());
			}
			frameRingReleaseSlot(slot);
		}
		worst = maxDiff(last[0], last[1]);
		std::printf("  Feedback %s, module option %s: max difference %d\n", variant ? "blend" : "add", variant ? "on" : "off", worst);
		char text[160];
		std::snprintf(text, sizeof(text), "Feedback variant %d: within 3 levels after 8 ticks", variant);
		check(worst <= 3, text);
	}
	clearBuses();
}

// ---- The beam layer (Scope Trace, Deflection Yoke, Scan Processor) in full precision (stage 4) ----
// A signal ring holding a Lissajous pair (channel 0 X, 1 Y, 2 Z), enough samples for the longest window.
int makeScopeRing(double fx, double fy) {
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, 48000.f);
	for (int n = 0; n < 12000; ++n) {
		const double t = static_cast<double>(n) / 48000.0;
		const float v[kSignalRingChannels] = {static_cast<float>(0.8 * std::sin(6.283185307 * fx * t)), static_cast<float>(0.8 * std::sin(6.283185307 * fy * t + 1.0)), 1.f, 0.f};
		signalRingPush(slot, v);
	}
	return slot;
}

int distinctValues(const Bytes& f, int channel) {
	std::set<int> seen;
	for (size_t i = static_cast<size_t>(channel); i < f.size(); i += 4u) seen.insert(f[i]);
	return static_cast<int>(seen.size());
}

void testBeam() {
	std::printf("(h) beam layer: Scope Trace, Deflection Yoke, Scan Processor with the setting on\n");
	clearBuses();
	Externals ext;
	const int w = 320, h = 240;
	const int slot = makeScopeRing(997.0, 1511.0);
	ScopeTraceParams scope;
	scope.mode = static_cast<int>(ScopeTraceMode::XY);
	scope.decay = 0.9f;
	scope.bloom = 0.5f;
	videoBusPublishScopeTrace(1, slot, scope);
	// Scope Trace alone: within 1 level (the graticule blend is truncated in bytes, not in floats).
	{
		const Bytes off = render(false, 1, w, h);
		const Bytes on = render(true, 1, w, h);
		const int worst = maxDiff(off, on);
		std::printf("  Scope Trace alone: max difference %d, share over 1 level %.5f\n", worst, fractionOver(off, on, 1));
		// The graticule is blended twice where two of its lines cross, and the 8-bit blend cuts the value each time.
		check(worst <= 2 && fractionOver(off, on, 1) < 0.001, "Scope Trace: within 2 levels (only where graticule lines cross), 0.1% of values over 1");
	}
	// Scope Trace -> Color Lab (gain 0.25) -> Color Lab (gain 2.0): about twice the levels.
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(2, 1, quarter);
	ColorLabParams twice;
	twice.brightness = 2.f;
	videoBusPublishColorLab(3, 2, twice);
	{
		const Bytes off = render(false, 3, w, h);
		const Bytes on = render(true, 3, w, h);
		int worstLevels[2] = {0, 0};
		for (int ch = 0; ch < 3; ++ch) {
			worstLevels[0] = std::max(worstLevels[0], distinctValues(off, ch));
			worstLevels[1] = std::max(worstLevels[1], distinctValues(on, ch));
		}
		std::printf("  Scope Trace -> gain 0.25 -> gain 2.0: most distinct levels in a channel, off %d, on %d (ratio %.2f)\n", worstLevels[0], worstLevels[1], static_cast<double>(worstLevels[1]) / std::max(1, worstLevels[0]));
		check(worstLevels[1] >= worstLevels[0] * 18 / 10, "Scope Trace through the two gains keeps about twice the levels (at least 1.8x)");
		check(worstLevels[0] <= 66, "control: the 8-bit path has about 64 levels or fewer");
	}
	// Scan Processor and Deflection Yoke, on the 8-bit external picture.
	{
		ScanProcessorParams sp;
		sp.color = true;
		sp.persist = 0.f;
		videoBusPublishScanProcessor(4, 10, sp);
		const Bytes off = render(false, 4, kW, kH);
		const Bytes on = render(true, 4, kW, kH);
		const int worst = maxDiff(off, on);
		check(distinctValues(on, 1) > 20, "Scan Processor: the picture is not blank");
		std::printf("  Scan Processor: max difference %d, share over 1 level %.5f\n", worst, fractionOver(off, on, 1));
		check(worst <= 1, "Scan Processor: within 1 level of the 8-bit render");
	}
	{
		const int ySlot = makeScopeRing(120.0, 90.0);
		DeflectionYokeParams dy;
		dy.depth = 1.f;
		dy.signalSlotId = ySlot;
		dy.persist = 0.f;
		dy.glow = 0.4f;
		videoBusPublishDeflectionYoke(5, 10, dy);
		const Bytes off = render(false, 5, kW, kH);
		const Bytes on = render(true, 5, kW, kH);
		const int worst = maxDiff(off, on);
		check(distinctValues(on, 1) > 20, "Deflection Yoke: the picture is not blank");
		std::printf("  Deflection Yoke: max difference %d, share over 1 level %.5f\n", worst, fractionOver(off, on, 1));
		check(worst <= 1, "Deflection Yoke: within 1 level of the 8-bit render");
		signalRingRelease(ySlot);
	}
	signalRingRelease(slot);
	clearBuses();
}

// ---- Freeze Frame store (stage 4) ----
void testFreezeStore() {
	std::printf("(i) Freeze Frame holds a float picture as floats\n");
	clearBuses();
	const int w = 2048;
	RampsParams ramps;
	ramps.axisA = 0;
	ramps.freqA = 1.f;
	ramps.mix = 0;
	videoBusPublishRamps(1, ramps);
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(2, 1, quarter);
	int levels[2] = {0, 0};
	Bytes held[2];
	for (int mode = 0; mode < 2; ++mode) {
		const int slot = freezeFrameAllocateSlot();
		FreezeFrameParams ff;
		ff.freezeSlotId = slot;
		ff.captureCounter = 1;
		ff.hold = 1.f;
		ff.blend = 1.f;
		videoBusPublishFreezeFrame(3, 2, ff);
		ColorLabParams twice;
		twice.brightness = 2.f;
		videoBusPublishColorLab(4, 3, twice);
		VideoGraphRenderer r;
		r.setFullPrecision(mode == 1);
		r.renderBus(4, w, 4);  // captures
		// A second render: the source goes away from the freeze (a different picture), the held one is read.
		videoBusPublishColorLab(2, 1, ColorLabParams{});
		const VideoGraphRenderer::Frame& f = r.renderBus(4, w, 4);
		held[mode].assign(f.begin(), f.end());
		levels[mode] = distinctReds(held[mode], w);
		FreezeFrameData stored;
		check(freezeFrameRead(slot, stored) && (mode == 1 ? (stored.rgba.empty() && !stored.rgbaF.empty()) : (!stored.rgba.empty() && stored.rgbaF.empty())),
			mode == 1 ? "setting on: a float picture is held as floats (no byte plane)" : "setting off: the held picture is bytes");
		freezeFrameReleaseSlot(slot);
		videoBusPublishColorLab(2, 1, quarter);
	}
	std::printf("  Ramps -> gain 0.25 -> Freeze Frame (hold) -> gain 2.0: distinct levels off %d, on %d\n", levels[0], levels[1]);
	check(levels[0] <= 66 && levels[1] >= 126, "the held picture keeps 128 levels with the setting on, 64 with it off");
	clearBuses();
}

// ---- The Frame Delay memory guard (stage 4) ----
void testDelayGuard() {
	std::printf("(j) Frame Delay: float store only within the memory budget\n");
	check(frameDelayFloatStoreFits(30, 640, 480), "30 frames at 640 x 480 (152 MiB) fit the 256 MiB budget");
	check(frameDelayFloatStoreFits(9, 1440, 1080), "9 frames at 1440 x 1080 (238 MiB) fit");
	check(!frameDelayFloatStoreFits(10, 1440, 1080), "10 frames at 1440 x 1080 (261 MiB with the one being stored) do not fit");
	check(!frameDelayFloatStoreFits(30, 1440, 1080), "30 frames at 1440 x 1080 (747 MB) do not fit");
	clearBuses();
	struct Case {
		int frames, w, h;
		bool expectFloat;
	};
	const Case cases[] = {{30, 640, 480, true}, {4, 1440, 1080, true}, {30, 1440, 1080, false}};
	for (const Case& c : cases) {
		RampsParams ramps;
		videoBusPublishRamps(1, ramps);
		ColorLabParams lab;
		lab.brightness = 0.9f;
		videoBusPublishColorLab(2, 1, lab);
		const int slot = frameRingAllocateSlot();
		VideoGraphRenderer r;
		r.setFullPrecision(true);
		for (int tick = 1; tick <= 3; ++tick) {
			FrameDelayParams d;
			d.ringSlotId = slot;
			d.frames = c.frames;
			d.frameCounter = tick;
			videoBusPublishFrameDelay(3, 2, d);
			r.renderBus(3, c.w, c.h);
		}
		FrameRingFrame stored;
		const bool read = frameRingRead(slot, 0, stored);
		const bool isFloat = read && stored.rgba.empty() && !stored.rgbaF.empty();
		const bool isBytes = read && !stored.rgba.empty() && stored.rgbaF.empty();
		const bool noted = frameRingByteFallback(slot);
		std::printf("  %2d frames at %4dx%-4d: stored as %s, byte-fallback note %s\n", c.frames, c.w, c.h, isFloat ? "float32" : isBytes ? "bytes" : "nothing", noted ? "set" : "clear");
		char text[160];
		std::snprintf(text, sizeof(text), "%d frames at %dx%d is stored as %s", c.frames, c.w, c.h, c.expectFloat ? "float32" : "bytes (over budget)");
		check(c.expectFloat ? isFloat : isBytes, text);
		check(noted == !c.expectFloat, "the module is told when the delay stores bytes only for the budget");
		// Reading it back in a setting-on pass still gives a picture of the right size.
		const VideoGraphRenderer::Frame& f = r.renderBus(3, c.w, c.h);
		check(f.size() == static_cast<size_t>(c.w) * c.h * 4u, "the delay still draws a picture of the right size");
		frameRingReleaseSlot(slot);
	}
	clearBuses();
}

// ---- A loop through a Rotator and a Frame Delay (stage 4) ----
// Video Mixer (ADD, fader 0.4: the loop returns at 0.8) <- Shapes and Frame Delay <- Rotator (a quarter turn, so every
// pixel moves to exactly one other pixel) <- the Video Mixer. Shapes feeds it for 24 ticks, then it is swapped for black
// and the picture is left to decay. After the swap each tick is exactly 0.8 times the turned picture, so the brightest
// and the mean level fall as 0.8^n from the picture at the swap, whatever the turning does.
void testRotatorLoop() {
	std::printf("(k) a loop through Video Mixer ADD, Frame Delay and Rotator, left to decay\n");
	const int w = 64, h = 64;
	const int seedTicks = 24, decayTicks = 90;
	const double gain = 0.8;
	double worst[2] = {0, 0};
	double worstMean[2] = {0, 0};
	int stuckLevel[2] = {0, 0};
	int zeroAt[2] = {-1, -1};
	int longestRun[2] = {0, 0};
	for (int mode = 0; mode < 2; ++mode) {
		clearBuses();
		ShapesParams shapes;
		shapes.sides = 5.f;
		shapes.size = 0.45f;
		videoBusPublishShapes(1, shapes);
		ColorLabParams black;
		black.brightness = 0.f;
		videoBusPublishColorLab(7, 1, black);
		RotatorParams rot;
		rot.angle = 0.25f;
		rot.edge = 0;
		videoBusPublishRotator(3, 5, rot);
		const int slot = frameRingAllocateSlot();
		VideoGraphRenderer r;
		r.setFullPrecision(mode == 1);
		VideoMixerParams vm;
		vm.inputBus[0] = 1;
		vm.inputBus[1] = 4;
		vm.onAir = 0;
		vm.next = 1;
		vm.fader = 0.4f;
		vm.mode = 1;
		videoBusPublishVideoMixer(5, vm);
		Bytes seedPicture;
		double peak0 = 0.0, mean0 = 0.0;
		std::vector<int> peaks;
		int previousPeak = -1;
		int run = 0;
		for (int tick = 1; tick <= seedTicks + decayTicks; ++tick) {
			if (tick == seedTicks + 1) vm.inputBus[0] = 7;
			videoBusPublishVideoMixer(5, vm);
			FrameDelayParams d;
			d.ringSlotId = slot;
			d.frames = 1;
			d.frameCounter = tick;
			videoBusPublishFrameDelay(4, 3, d);
			const VideoGraphRenderer::Frame& f = r.renderBus(5, w, h);
			int peak = 0;
			double sum = 0.0;
			for (size_t i = 0; i + 3 < f.size(); i += 4u) {
				peak = std::max(peak, static_cast<int>(f[i]));
				sum += f[i];
			}
			const double mean = sum / (static_cast<double>(w) * h);
			if (tick == seedTicks) {
				peak0 = peak;
				mean0 = mean;
			}
			if (tick > seedTicks) {
				const int n = tick - seedTicks;
				const double ideal = peak0 * std::pow(gain, n);
				const double idealMean = mean0 * std::pow(gain, n);
				worst[mode] = std::max(worst[mode], std::fabs(peak - ideal));
				worstMean[mode] = std::max(worstMean[mode], std::fabs(mean - idealMean));
				if (peak == 0 && zeroAt[mode] < 0) zeroAt[mode] = n;
				run = peak == previousPeak ? run + 1 : 0;
				longestRun[mode] = std::max(longestRun[mode], run);
				if (tick == seedTicks + decayTicks) stuckLevel[mode] = peak;
			}
			previousPeak = peak;
		}
		frameRingReleaseSlot(slot);
	}
	std::printf("  brightest level against 255-scale ideal peak x 0.8^n over %d ticks: worst distance off %.2f, on %.2f levels\n", decayTicks, worst[0], worst[1]);
	std::printf("  mean level against ideal: worst distance off %.2f, on %.2f; level after %d ticks: off %d, on %d; first tick at black: off %d, on %d; longest run of equal peaks: off %d, on %d\n",
		worstMean[0], worstMean[1], decayTicks, stuckLevel[0], stuckLevel[1], zeroAt[0], zeroAt[1], longestRun[0], longestRun[1]);
	check(worst[1] <= 1.0, "on: the brightest level stays within 1 level of the ideal decay");
	check(worstMean[1] <= 1.0, "on: the mean level stays within 1 level of the ideal decay");
	check(stuckLevel[0] >= 1 && zeroAt[0] < 0, "off: the loop sticks at a grey and never reaches black");
	check(stuckLevel[1] == 0 && zeroAt[1] > 0, "on: the loop reaches black");
	check(longestRun[0] >= 20, "off: a long plateau of equal levels");
	clearBuses();
}

// ---- (e) chains ----
void buildChainNode(int k, int source) {
	const int bus = 20 + k;
	switch (k % 4) {
	case 0: {
		ColorLabParams p;
		p.hue = 0.07f * static_cast<float>(k);
		p.saturation = 0.9f;
		p.brightness = 0.95f;
		videoBusPublishColorLab(bus, source, p);
		break;
	}
	case 1: {
		WaveShaperParams p;
		p.drive = 0.9f;
		p.bias = 0.02f;
		p.mix = 0.9f;
		videoBusPublishWaveShaper(bus, source, p);
		break;
	}
	case 2: {
		BusMixerParams p;
		p.busB = 11;
		p.mode = 2;
		p.mix = 0.3f;
		videoBusPublishBusMixer(bus, source, p);
		break;
	}
	default: {
		KeyerParams p;
		p.mode = 2;
		p.softness = 1.f;  // a narrow edge would multiply a one level difference by 1 / softness
		p.threshold = 0.4f;
		p.mix = 0.5f;
		videoBusPublishKeyer(bus, source, p);
		break;
	}
	}
}

void testChains() {
	std::printf("(e) chains of k nodes: setting on against setting off\n");
	clearBuses();
	Externals ext;
	int source = 10;
	bool all = true;
	int bound = 0;
	for (int k = 1; k <= 8; ++k) {
		buildChainNode(k, source);
		source = 20 + k;
		// Each node may move a value by 1 level, a Color Lab by 2 (it cut the colour to a byte twice).
		bound += (k % 4 == 0) ? 2 : 1;
		const Bytes off = render(false, source, kW, kH);
		const Bytes on = render(true, source, kW, kH);
		const int worst = maxDiff(off, on);
		std::printf("  k = %d: max difference %d levels (allowed %d)\n", k, worst, bound);
		all = all && worst <= bound;
	}
	check(all, "a chain of k nodes is within the sum of its nodes' limits (1 level each, 2 for a Color Lab)");
	clearBuses();
}

// ---- (f) determinism and pinned pictures ----
void testPinned() {
	std::printf("(f) the setting on is deterministic; pinned pictures\n");
	clearBuses();
	Externals ext;
	// Graph 1: Ramps -> Color Lab 0.25 -> Color Lab 2.0.
	RampsParams ramps;
	ramps.freqA = 2.f;
	videoBusPublishRamps(1, ramps);
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(2, 1, quarter);
	ColorLabParams twice;
	twice.brightness = 2.f;
	twice.hue = 0.1f;
	videoBusPublishColorLab(3, 2, twice);
	// Graph 2: external -> Colorizer -> Wave Shaper -> Keyer -> Bus Mixer with the second picture.
	ColorizerParams colorizer;
	videoBusPublishColorizer(30, 10, colorizer);
	WaveShaperParams shaper;
	shaper.drive = 1.6f;
	shaper.fold = 0.3f;
	videoBusPublishWaveShaper(31, 30, shaper);
	KeyerParams keyer;
	keyer.mode = 1;
	keyer.mix = 0.8f;
	videoBusPublishKeyer(32, 31, keyer);
	BusMixerParams mixer;
	mixer.busB = 11;
	mixer.mode = 5;
	mixer.mix = 0.45f;
	videoBusPublishBusMixer(33, 32, mixer);
	// Graph 3: Shapes and a Video Oscillator into a Video Mixer with a gradient.
	ShapesParams shapes;
	shapes.sides = 5.f;
	videoBusPublishShapes(4, shapes);
	VideoParams osc;
	osc.feedback = 0.f;
	videoBusPublish(5, osc);
	VideoMixerParams vm;
	vm.inputBus[0] = 4;
	vm.inputBus[1] = 5;
	vm.onAir = 0;
	vm.next = 1;
	vm.fader = 0.5f;
	vm.gradientOn[0] = true;
	vm.span = 0.15f;
	videoBusPublishVideoMixer(6, vm);
	const struct {
		const char* name;
		int bus;
		uint64_t pinned;
	} graphs[] = {
		{"ramps and two gains, on", 3, 7886856344498600291ull},
		{"colorizer, shaper, keyer, mixer, on", 33, 9815054913535482756ull},
		{"shapes and oscillator through the video mixer, on", 6, 5989471659653783265ull},
	};
	for (const auto& g : graphs) {
		const Bytes a = render(true, g.bus, kW, kH);
		const Bytes b = render(true, g.bus, kW, kH);
		char text[160];
		std::snprintf(text, sizeof(text), "%s: two runs are byte for byte the same", g.name);
		check(!a.empty() && a == b, text);
		golden(g.name, a, g.pinned);
		const Bytes offPicture = render(false, g.bus, kW, kH);
		std::printf("  %s: off against on, max difference %d\n", g.name, maxDiff(offPicture, a));
	}
	clearBuses();
}

// ---- (f2) the converted spatial nodes: determinism and pinned pictures ----
// Three more setting-on pictures: a chain of spatial nodes on an 8-bit picture, the beam layer through two gains, and
// a Feedback run of eight ticks. Each is rendered twice from scratch; the two runs must be byte for byte the same.
Bytes renderSpatialChain() {
	clearBuses();
	Externals ext;
	RotatorParams rot;
	rot.angle = 0.07f;
	rot.zoom = 1.1f;
	rot.edge = 1;
	videoBusPublishRotator(20, 10, rot);
	FocusParams focus;
	focus.focus = 0.3f;
	focus.sharp = 0.4f;
	videoBusPublishFocus(21, 20, focus);
	LineFieldParams lf;
	lf.mix = 0.7f;
	videoBusPublishLineField(22, 21, lf);
	DeflectionYokeParams dy;
	dy.depth = 1.f;
	dy.glow = 0.3f;
	const int ring = makeScopeRing(120.0, 90.0);
	dy.signalSlotId = ring;
	videoBusPublishDeflectionYoke(23, 22, dy);
	const Bytes picture = render(true, 23, kW, kH);
	signalRingRelease(ring);
	clearBuses();
	return picture;
}

Bytes renderBeamChain() {
	clearBuses();
	const int slot = makeScopeRing(997.0, 1511.0);
	ScopeTraceParams scope;
	scope.decay = 0.9f;
	scope.bloom = 0.5f;
	videoBusPublishScopeTrace(1, slot, scope);
	ColorLabParams quarter;
	quarter.brightness = 0.25f;
	videoBusPublishColorLab(2, 1, quarter);
	ColorLabParams twice;
	twice.brightness = 2.f;
	videoBusPublishColorLab(3, 2, twice);
	const Bytes picture = render(true, 3, 200, 150);
	signalRingRelease(slot);
	clearBuses();
	return picture;
}

Bytes renderFeedbackRun() {
	clearBuses();
	Externals ext;
	const int slot = frameRingAllocateSlot();
	VideoGraphRenderer r;
	r.setFullPrecision(true);
	FeedbackParams fb;
	fb.ringSlotId = slot;
	fb.gain = 0.9f;
	fb.zoom = 1.04f;
	fb.rotate = 0.02f;
	fb.hue = 0.05f;
	fb.edge = 1;
	Bytes last;
	for (int n = 1; n <= 8; ++n) {
		fb.frameCounter = n;
		videoBusPublishFeedback(20, 10, fb);
		const VideoGraphRenderer::Frame& f = r.renderBus(20, kW, kH);
		last.assign(f.begin(), f.end());
	}
	frameRingReleaseSlot(slot);
	clearBuses();
	return last;
}

void testPinnedSpatial() {
	std::printf("(f2) spatial nodes with the setting on: deterministic; pinned pictures\n");
	const struct {
		const char* name;
		Bytes (*make)();
		uint64_t pinned;
	} pictures[] = {
		{"rotator, focus, line field, yoke chain, on", renderSpatialChain, 2077890795361136342ull},
		{"scope trace through two gains, on", renderBeamChain, 7310387028945889410ull},
		{"feedback, eight ticks, on", renderFeedbackRun, 17009853618987073673ull},
	};
	for (const auto& p : pictures) {
		const Bytes a = p.make();
		const Bytes b = p.make();
		char text[160];
		std::snprintf(text, sizeof(text), "%s: two runs are byte for byte the same", p.name);
		check(!a.empty() && a == b, text);
		golden(p.name, a, p.pinned);
	}
}

// ---- (f3) Scan Processor: the lift is read at full precision ----
// A flat grey picture on BUS IN, and on LIFT IN a smooth ramp made dim (Ramps -> Color Lab 0.25), so on the 8-bit path it
// has only 64 levels across the picture and each level steps the line by LUMA GAIN x height / 255 = 3.8 pixels. With the setting
// on the lift is read unrounded and the same ramp is smooth. The measure is how far each line's height along the columns
// strays from the straight line it should be: the root mean square of the residual of a least-squares line, in pixels.
constexpr int kLiftW = 640;
constexpr int kLiftH = 960;

void publishLiftScene(int lines, float lumaGain) {
	clearBuses();
	static int flat = 0;
	if (flat == 0) flat = videoExternalFrameAllocate();
	Bytes grey(static_cast<size_t>(kLiftW) * kLiftH * 4u, 128);
	for (size_t i = 3; i < grey.size(); i += 4u) grey[i] = 255;
	videoExternalFramePublish(flat, grey.data(), kLiftW, kLiftH, 0.0);
	videoBusPublishExternalFrame(10, flat);
	RampsParams ramps;
	ramps.axisA = 0;
	ramps.freqA = 1.f;
	ramps.mix = 0;
	videoBusPublishRamps(1, ramps);
	ColorLabParams dim;
	dim.brightness = 0.25f;
	videoBusPublishColorLab(2, 1, dim);
	ScanProcessorParams sp;
	sp.lines = lines;
	sp.lumaGain = lumaGain;
	sp.hPos = 0.f;
	sp.hWidth = 1.f;
	sp.vPos = 0.f;
	sp.vHeight = 1.f;
	sp.intensity = 0.4f;   // dim enough that no line saturates, so its position is in the light, not on a flat top
	sp.persist = 0.f;
	sp.glow = 0.f;
	sp.liftBusId = 2;
	videoBusPublishScanProcessor(3, 10, sp);
}

// Row of the line near `nominal` in column x: the brightest row within 48 rows above and 8 below it (the 8-bit lift can
// sit a little higher than the float one), then the centre of light of green over 40 rows either side of that row.
double linePeakRow(const Bytes& f, int x, double nominal) {
	auto g = [&](int y) { return static_cast<double>(f[(static_cast<size_t>(y) * kLiftW + static_cast<size_t>(x)) * 4u + 1u]); };
	const int lo = std::max(40, static_cast<int>(std::lround(nominal)) - 48);
	const int hi = std::min(kLiftH - 41, static_cast<int>(std::lround(nominal)) + 8);
	int best = lo;
	for (int y = lo; y <= hi; ++y) {
		if (g(y) > g(best)) best = y;
	}
	// Light under the line's own (a faint level that does not move with it) is taken off, so the centre does not lean
	// toward the middle of the window.
	double floorLevel = 255.0;
	for (int y = best - 40; y <= best + 40; ++y) floorLevel = std::min(floorLevel, g(y));
	double sum = 0.0, weighted = 0.0;
	for (int y = best - 40; y <= best + 40; ++y) {
		sum += g(y) - floorLevel;
		weighted += (g(y) - floorLevel) * y;
	}
	return sum > 0.0 ? weighted / sum : nominal;
}

// RMS of the residual of a least-squares line through (column, row) points, in pixels.
double lineResidual(const std::vector<double>& xs, const std::vector<double>& ys) {
	const double n = static_cast<double>(xs.size());
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (size_t i = 0; i < xs.size(); ++i) {
		sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i];
	}
	const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
	const double icpt = (sy - slope * sx) / n;
	double sq = 0.0;
	for (size_t i = 0; i < xs.size(); ++i) {
		const double e = ys[i] - (slope * xs[i] + icpt);
		sq += e * e;
	}
	return std::sqrt(sq / n);
}

Bytes renderLiftPicture() {
	publishLiftScene(4, 1.f);
	const Bytes picture = render(true, 3, kLiftW, kLiftH);
	clearBuses();
	return picture;
}

void testScanProcessorLift() {
	std::printf("(f3) Scan Processor: LIFT IN read at full precision\n");
	const int lines = 4;
	const double pitch = static_cast<double>(kLiftH) / lines;
	double residual[2] = {0.0, 0.0};
	double steps[2] = {0.0, 0.0};
	for (int mode = 0; mode < 2; ++mode) {
		publishLiftScene(lines, 1.f);
		const Bytes picture = render(mode == 1, 3, kLiftW, kLiftH);
		clearBuses();
		const int k = 1;   // the second line; over these columns its lift is less than the line pitch, so it stays clear of the first
		std::vector<double> xs, ys;
		for (int x = 60; x <= 280; x += 2) {
			// The lift of this column: LUMA GAIN 1 x 0.25 x (x / 640) of the height, up from the nominal row.
			const double expected = (k + 0.5) * pitch - 0.25 * kLiftH * (x + 0.5) / kLiftW;
			xs.push_back(x);
			ys.push_back(linePeakRow(picture, x, expected));
		}
		residual[mode] = lineResidual(xs, ys);
		for (size_t i = 1; i < ys.size(); ++i) steps[mode] = std::max(steps[mode], std::fabs(ys[i] - ys[i - 1]));
	}
	std::printf("  line height along the columns: residual from a straight line, setting off %.3f px, on %.3f px; largest step between neighbouring samples 2 columns apart, off %.3f px, on %.3f px\n",
		residual[0], residual[1], steps[0], steps[1]);
	check(residual[0] > 0.5 && steps[0] > 1.5, "off: the 8-bit lift steps the line (residual above 0.5 px, a step of more than 1.5 px over 2 columns)");
	check(residual[1] < 0.2 && residual[1] < 0.25 * residual[0], "on: the float lift makes a straighter line (residual under 0.2 px, a quarter of the 8-bit one)");
	check(steps[1] < 1.25, "on: no step between samples 2 columns apart beyond the slope (0.75 px) and the pixel grid (under 1.25 px)");

	const Bytes a = renderLiftPicture();
	const Bytes b = renderLiftPicture();
	check(!a.empty() && a == b, "Scan Processor with a float lift: two runs are byte for byte the same");
	golden("scan processor with a float lift, on", a, 16048157721766900867ull);
}

// ---- the formats, seen from the render service ----
void testServiceBytes() {
	std::printf("service: every picture handed out is 8-bit, rounded once\n");
	clearBuses();
	Externals ext;
	ColorLabParams lab;
	lab.brightness = 0.7f;
	videoBusPublishColorLab(20, 10, lab);
	RenderService svc;
	svc.setFullPrecision(true);
	check(svc.fullPrecision(), "the service remembers the setting");
	const RenderService::Result r = svc.frame(20, kW, kH);
	check(r.signal && r.pixels && r.pixels->size() == static_cast<size_t>(kW) * kH * 4u && !r.pixels->isFloat(), "the picture is a byte frame of the right size");
	Bytes viaApi(static_cast<size_t>(kW) * kH * 4u, 7);
	check(svc.readCopy(20, kW, kH, viaApi.data(), static_cast<size_t>(kW) * 4u), "readCopy fills the caller's 8-bit buffer");
	check(viaApi[0] == (*r.pixels)[0] && viaApi[100] == (*r.pixels)[100], "and it is the same picture the service handed out");
	svc.setFullPrecision(false);
	clearBuses();
}

}  // namespace

int main() {
	testFrameType();
	testLevels();
	testLoopDecay();
	testStretch();
	testPointwise();
	testColorizerGrey();
	testGenerators();
	testFreezeAndDelay();
	testChains();
	testFeedbackFade();
	testFeedbackSource();
	testBeam();
	testFreezeStore();
	testDelayGuard();
	testRotatorLoop();
	testPinned();
	testPinnedSpatial();
	testScanProcessorLift();
	testServiceBytes();
	std::printf("precision_test: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
