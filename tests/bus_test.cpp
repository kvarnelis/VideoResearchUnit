// Offline checks for the video bus, renderer, Freeze Frame, Bus Mixer and slot allocators.
// No Rack needed. Build and run with `make test`. Optional argv[1]: directory for proof PNGs.

#include "crt_beam.hpp"
#include "cv_param.hpp"
#include "display_select.hpp"
#include "expo_hz.hpp"
#include "lorenz.hpp"
#include "freeze_frame_store.hpp"
#include "frame_ring.hpp"
#include "inverse_map.hpp"
#include "scope_sweep.hpp"
#include "scope_timebase.hpp"
#include "signal_ring.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"
#include "video_mixer_math.hpp"
#include "video_slot_allocator.hpp"
#include "rotator_math.hpp"
#include "deflection_yoke_math.hpp"

#include <algorithm>
#include <array>
#include <functional>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// Test-only door into VideoGraphRenderer (it is declared a friend there, so this must live at global
// scope, outside the anonymous namespace below). It exposes the Scope Trace window reader, so the
// trigger level can be checked on the samples themselves instead of through pixels, and the Scope Trace
// stroke, so the beam energy can be counted before the tone map.
struct VideoGraphRendererTestAccess {
	static bool readScopeWindow(VideoGraphRenderer& r, int scopeTraceSlotId, const ScopeTraceParams& params, SignalWindow& out) {
		return r.readScopeWindow(scopeTraceSlotId, params, out);
	}

	// Strokes `window` into `energy` the way a Scope Trace render does, with no ring, decay, glow or tone map.
	static void depositScopeTrace(VideoGraphRenderer& r, crt::EnergyField& energy, int width, int height, const ScopeTraceParams& params, const SignalWindow& window) {
		r.depositScopeTrace(energy, width, height, params, window);
	}

	// The phosphor energy the node on `bus` holds after its last render (before the tone map).
	static const crt::EnergyField& beamEnergy(VideoGraphRenderer& r, int bus) {
		return r.beam[static_cast<size_t>(bus)].energy;
	}

	// The linear RGB (0..1) the renderer gives trace A at `hue`.
	static void traceColor(float hue, float rgb[3]) {
		unsigned char r = 0, g = 0, b = 0;
		VideoGraphRenderer::hsvToRgb(VideoGraphRenderer::fract(hue), 0.92f, 1.f, r, g, b);
		rgb[0] = static_cast<float>(r) / 255.f;
		rgb[1] = static_cast<float>(g) / 255.f;
		rgb[2] = static_cast<float>(b) / 255.f;
	}
};

namespace {

using Frame = std::vector<unsigned char>;
constexpr int kW = 256;
constexpr int kH = 144;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

// Largest per-channel difference, ignoring alpha.
int maxDiff(const Frame& a, const Frame& b) {
	if (a.size() != b.size() || a.empty()) {
		return 255;
	}
	int worst = 0;
	for (size_t i = 0; i < a.size(); i += 4) {
		for (int c = 0; c < 3; ++c) {
			worst = std::max(worst, std::abs(int(a[i + c]) - int(b[i + c])));
		}
	}
	return worst;
}

// Mean per-channel difference, ignoring alpha.
double meanDiff(const Frame& a, const Frame& b) {
	if (a.size() != b.size() || a.empty()) {
		return 255.0;
	}
	double total = 0.0;
	size_t count = 0;
	for (size_t i = 0; i < a.size(); i += 4) {
		for (int c = 0; c < 3; ++c) {
			total += std::abs(int(a[i + c]) - int(b[i + c]));
			++count;
		}
	}
	return total / static_cast<double>(count);
}

float lumaAt(const Frame& f, int x, int y) {
	const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
	return (f[i] * 0.2126f + f[i + 1] * 0.7152f + f[i + 2] * 0.0722f) / 255.f;
}

void publishOsc(int bus, float phase, int pattern = 0) {
	VideoParams p;
	p.phase = phase;
	p.pattern = pattern;
	p.feedback = 0.f;
	videoBusPublish(bus, p);
}

void publishColor(int bus, int source, bool invert, float hue = 0.f) {
	ColorLabParams c;
	c.invert = invert;
	c.hue = hue;
	videoBusPublishColorLab(bus, source, c);
}

Frame render(VideoGraphRenderer& r, int bus) {
	return r.renderBus(bus, kW, kH);
}

// ---- minimal PNG writer (stored deflate blocks, no compression) ----
uint32_t crc32(const unsigned char* data, size_t len, uint32_t crc = 0) {
	crc = ~crc;
	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

void putBE32(std::vector<unsigned char>& out, uint32_t v) {
	out.push_back(static_cast<unsigned char>(v >> 24));
	out.push_back(static_cast<unsigned char>(v >> 16));
	out.push_back(static_cast<unsigned char>(v >> 8));
	out.push_back(static_cast<unsigned char>(v));
}

void pngChunk(std::vector<unsigned char>& file, const char* type, const std::vector<unsigned char>& data) {
	putBE32(file, static_cast<uint32_t>(data.size()));
	std::vector<unsigned char> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	file.insert(file.end(), body.begin(), body.end());
	putBE32(file, crc32(body.data(), body.size()));
}

bool writePng(const std::string& path, int width, int height, const Frame& rgba) {
	std::vector<unsigned char> raw;
	raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 4 + 1));
	for (int y = 0; y < height; ++y) {
		raw.push_back(0);  // filter: none
		const size_t row = static_cast<size_t>(y) * width * 4;
		raw.insert(raw.end(), rgba.begin() + static_cast<long>(row), rgba.begin() + static_cast<long>(row + static_cast<size_t>(width) * 4));
	}

	std::vector<unsigned char> z = {0x78, 0x01};
	size_t pos = 0;
	do {
		const size_t n = std::min<size_t>(65535, raw.size() - pos);
		const bool last = pos + n >= raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back(static_cast<unsigned char>(n & 0xFF));
		z.push_back(static_cast<unsigned char>(n >> 8));
		z.push_back(static_cast<unsigned char>(~n & 0xFF));
		z.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
		z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
		pos += n;
	} while (pos < raw.size());
	uint32_t a = 1, b = 0;
	for (unsigned char c : raw) {
		a = (a + c) % 65521u;
		b = (b + a) % 65521u;
	}
	putBE32(z, (b << 16) | a);

	std::vector<unsigned char> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::vector<unsigned char> ihdr;
	putBE32(ihdr, static_cast<uint32_t>(width));
	putBE32(ihdr, static_cast<uint32_t>(height));
	ihdr.push_back(8);  // bit depth
	ihdr.push_back(6);  // RGBA
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);
	pngChunk(file, "IHDR", ihdr);
	pngChunk(file, "IDAT", z);
	pngChunk(file, "IEND", {});

	std::ofstream out(path, std::ios::binary);
	if (!out) {
		return false;
	}
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
	return static_cast<bool>(out);
}

// Places panels left to right with a 4 px white gutter.
Frame tile(const std::vector<Frame>& panels, int& outWidth) {
	const int gutter = 4;
	outWidth = static_cast<int>(panels.size()) * kW + (static_cast<int>(panels.size()) - 1) * gutter;
	Frame out(static_cast<size_t>(outWidth) * kH * 4u, 255);
	for (size_t p = 0; p < panels.size(); ++p) {
		const int x0 = static_cast<int>(p) * (kW + gutter);
		for (int y = 0; y < kH; ++y) {
			for (int x = 0; x < kW; ++x) {
				const size_t src = (static_cast<size_t>(y) * kW + x) * 4u;
				const size_t dst = (static_cast<size_t>(y) * outWidth + (x0 + x)) * 4u;
				for (int c = 0; c < 4; ++c) {
					out[dst + c] = panels[p].empty() ? 0 : panels[p][src + c];
				}
			}
		}
	}
	return out;
}

std::string gProofDir = "build/bus_test_proofs";

void saveProof(const std::string& name, const std::vector<Frame>& panels) {
	int w = 0;
	const Frame image = tile(panels, w);
	const std::string path = gProofDir + "/" + name;
	if (!writePng(path, w, kH, image)) {
		std::printf("  could not write %s\n", path.c_str());
		check(false, "proof image written");
	}
	else {
		std::printf("  proof: %s\n", path.c_str());
	}
}

// ---- tests ----

void testFreezeLatch() {
	std::printf("freeze latch\n");
	const int slot = freezeFrameAllocateSlot();
	check(slot != 0, "freeze slot allocated");

	FreezeFrameParams fp;
	fp.freezeSlotId = slot;
	fp.hold = 1.f;
	fp.blend = 1.f;
	fp.captureCounter = 0;

	VideoGraphRenderer r;
	publishOsc(1, 0.10f);
	videoBusPublishFreezeFrame(2, 1, fp);

	// No capture yet: pass live through.
	const Frame live0 = render(r, 1);
	const Frame passthrough = render(r, 2);
	check(maxDiff(passthrough, live0) <= 1, "no capture yet passes live through");

	// Trigger 1: captures the current source.
	fp.captureCounter = 1;
	videoBusPublishFreezeFrame(2, 1, fp);
	const Frame captured = render(r, 2);
	check(maxDiff(captured, live0) <= 1, "capture equals the live source at capture time");

	// Source moves; the frozen output must not move with it.
	publishOsc(1, 0.37f);
	const Frame live1 = render(r, 1);
	const Frame frozen1 = render(r, 2);
	check(maxDiff(live1, live0) > 20, "source moved (test sanity)");
	check(maxDiff(frozen1, captured) == 0, "frame N+1 equals frame N while source moves");
	check(maxDiff(frozen1, live1) > 20, "frozen output differs from the moved live source");

	publishOsc(1, 0.61f);
	const Frame frozen2 = render(r, 2);
	check(maxDiff(frozen2, captured) == 0, "frame N+2 still equals frame N");

	// Hold at 0: live again.
	fp.hold = 0.f;
	videoBusPublishFreezeFrame(2, 1, fp);
	const Frame live2 = render(r, 1);
	check(maxDiff(render(r, 2), live2) <= 1, "hold 0 shows live");

	// Hold back up; trigger 2 captures the new source.
	fp.hold = 1.f;
	fp.captureCounter = 2;
	videoBusPublishFreezeFrame(2, 1, fp);
	const Frame recaptured = render(r, 2);
	check(maxDiff(recaptured, live2) <= 1, "trigger 2 captures the current source");
	check(maxDiff(recaptured, captured) > 20, "trigger 2 replaced the old capture");

	// Half hold mixes live and frozen.
	publishOsc(1, 0.85f);
	const Frame live3 = render(r, 1);
	fp.hold = 0.5f;
	videoBusPublishFreezeFrame(2, 1, fp);
	const Frame half = render(r, 2);
	const double expectedHalfA = meanDiff(half, live3);
	const double expectedHalfB = meanDiff(half, recaptured);
	check(expectedHalfA > 1.0 && expectedHalfB > 1.0, "half hold is neither fully live nor fully frozen");
	fp.hold = 1.f;

	// CLEAR empties the slot: live shows, and the cleared capture is not re-taken.
	fp.clear = true;
	fp.captureCounter = 2;
	videoBusPublishFreezeFrame(2, 1, fp);
	check(maxDiff(render(r, 2), live3) <= 1, "clear shows live");
	fp.clear = false;
	publishOsc(1, 0.92f);
	const Frame live4 = render(r, 1);
	check(maxDiff(render(r, 2), live4) <= 1, "after clear, same counter does not re-capture");

	// A new trigger after clear captures again.
	fp.captureCounter = 3;
	videoBusPublishFreezeFrame(2, 1, fp);
	const Frame afterClear = render(r, 2);
	check(maxDiff(afterClear, live4) <= 1, "new trigger after clear captures");

	// Strobe-style repeated captures: each new counter re-latches.
	publishOsc(1, 0.05f);
	fp.captureCounter = 4;
	videoBusPublishFreezeFrame(2, 1, fp);
	check(maxDiff(render(r, 2), render(r, 1)) <= 1, "counter 4 captures");

	videoBusClear(2);
	freezeFrameReleaseSlot(slot);
	saveProof("freeze_proof.png", {live0, captured, live1, frozen1, live2, recaptured});
}

void testSeriesSameType() {
	std::printf("same-type processors in series\n");
	publishOsc(1, 0.20f);
	VideoGraphRenderer r;

	const Frame live = render(r, 1);
	publishColor(2, 1, true);
	const Frame invertOnce = render(r, 2);
	publishColor(3, 2, true);
	const Frame invertTwice = render(r, 3);

	check(maxDiff(invertOnce, live) > 100, "one invert changes the image");
	check(maxDiff(invertTwice, live) <= 3, "two inverts in series return the source");

	// Hue shifts do not cancel: two 0.25 shifts equal one 0.5 shift.
	publishColor(5, 1, false, 0.25f);
	publishColor(6, 5, false, 0.25f);
	publishColor(7, 1, false, 0.5f);
	const Frame hueOnce = render(r, 5);
	const Frame hueTwice = render(r, 6);
	const Frame hueSingle = render(r, 7);
	check(maxDiff(hueOnce, hueSingle) > 10, "one 0.25 hue shift differs from 0.5");
	check(maxDiff(hueTwice, hueSingle) <= 4, "two 0.25 hue shifts in series equal one 0.5 shift");

	saveProof("series_proof.png", {live, invertOnce, invertTwice, hueOnce, hueTwice});
}

void testBusMixer() {
	std::printf("bus mixer\n");
	publishOsc(1, 0.20f);
	VideoGraphRenderer r;

	// A and B are both processed chains. Each must keep its own image.
	publishColor(2, 1, true);       // A: one invert
	publishColor(5, 1, false, 0.25f);
	publishColor(6, 5, false, 0.25f);  // B: two hue shifts
	const Frame a = render(r, 2);
	const Frame b = render(r, 6);

	BusMixerParams m;
	m.busB = 6;
	m.matteBus = 0;
	m.mode = 0;
	m.mix = 0.f;
	videoBusPublishBusMixer(8, 2, m);
	const Frame mixA = render(r, 8);
	check(maxDiff(mixA, a) <= 1, "mix 0 outputs processed A (A not overwritten by nested B render)");

	m.mix = 1.f;
	videoBusPublishBusMixer(8, 2, m);
	const Frame mixB = render(r, 8);
	check(maxDiff(mixB, b) <= 1, "mix 1 outputs processed B");

	// Empty B (unpatched 0, or an empty bus) outputs A alone.
	m.mix = 0.5f;
	m.busB = 0;
	videoBusPublishBusMixer(8, 2, m);
	check(maxDiff(render(r, 8), a) <= 1, "unpatched B outputs A alone");
	m.busB = 50;
	videoBusPublishBusMixer(8, 2, m);
	check(maxDiff(render(r, 8), a) <= 1, "empty B bus outputs A alone");

	// Matte mode keys on the MAT BUS luma. A = colour bars, B = inverted bars.
	publishOsc(20, 0.f, 1);       // SMPTE bars
	publishColor(21, 20, true);   // inverted bars
	publishOsc(22, 0.f, 4);       // grayscale ramp as matte
	m.busB = 21;
	m.matteBus = 22;
	m.mode = 6;
	m.mix = 0.5f;
	m.matteAmount = 0.f;
	m.invertMatte = false;
	videoBusPublishBusMixer(8, 20, m);
	const Frame matteOut = render(r, 8);
	const Frame barsA = render(r, 20);
	const Frame barsB = render(r, 21);
	const Frame ramp = render(r, 22);

	// Find the dark and bright ends of the ramp.
	double darkDiffA = 0.0, brightDiffB = 0.0;
	int darkCount = 0, brightCount = 0;
	double rampMin = 1.0, rampMax = 0.0;
	for (int y = 0; y < kH; y += 8) {
		for (int x = 0; x < kW; ++x) {
			const float l = lumaAt(ramp, x, y);
			rampMin = std::min<double>(rampMin, l);
			rampMax = std::max<double>(rampMax, l);
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			if (l < 0.02f) {
				for (int c = 0; c < 3; ++c) darkDiffA += std::abs(int(matteOut[i + c]) - int(barsA[i + c]));
				++darkCount;
			}
			if (l > 0.98f) {
				for (int c = 0; c < 3; ++c) brightDiffB += std::abs(int(matteOut[i + c]) - int(barsB[i + c]));
				++brightCount;
			}
		}
	}
	check(rampMax - rampMin > 0.8, "grayscale matte ramp spans dark to bright (test sanity)");
	check(darkCount > 0 && brightCount > 0, "matte has dark and bright columns");
	check(darkCount > 0 && darkDiffA / (darkCount * 3.0) <= 2.0, "matte mode: dark matte shows A");
	check(brightCount > 0 && brightDiffB / (brightCount * 3.0) <= 2.0, "matte mode: bright matte shows B");

	// Matte mode with no MAT BUS falls back to the MIX knob (0.5 here).
	m.matteBus = 0;
	videoBusPublishBusMixer(8, 20, m);
	const Frame fallback = render(r, 8);
	double fallbackError = 0.0;
	for (size_t i = 0; i < fallback.size(); i += 4) {
		for (int c = 0; c < 3; ++c) {
			const double avg = (barsA[i + c] + barsB[i + c]) * 0.5;
			fallbackError += std::abs(avg - fallback[i + c]);
		}
	}
	check(fallbackError / (fallback.size() / 4.0 * 3.0) <= 2.0, "matte mode without MAT BUS uses MIX knob");

	videoBusClear(8);
	saveProof("bus_mixer_proof.png", {a, b, mixA, mixB, ramp, matteOut});
}

void testCycle() {
	std::printf("cycles and depth\n");
	VideoGraphRenderer r;
	publishOsc(1, 0.30f);
	publishColor(2, 1, false, 0.1f);
	const Frame good = render(r, 2);
	check(!r.hasCycle() && maxDiff(good, render(r, 2)) == 0, "acyclic render is clean");

	// Bus 1 now reads bus 2, which reads bus 1.
	publishColor(1, 2, false, 0.0f);
	const Frame looped = render(r, 2);
	check(r.hasCycle(), "cycle is reported");
	check(!looped.empty() && maxDiff(looped, good) == 0, "cycle shows the last good frame");

	// Break the loop: rendering is clean again.
	publishOsc(1, 0.30f);
	const Frame fixed = render(r, 2);
	check(!r.hasCycle() && maxDiff(fixed, good) == 0, "after breaking the loop, output is clean again");

	// Self-loop through a single module.
	publishColor(3, 3, false);
	render(r, 3);
	check(r.hasCycle(), "self loop is reported");

	// A chain deeper than the renderer limit is reported, not drawn blank silently.
	publishOsc(40, 0.4f);
	for (int bus = 41; bus <= 66 && bus <= kVideoBusCount; ++bus) {
		publishColor(bus, bus - 1, false);
	}
	const Frame deep = render(r, kVideoBusCount);
	check(r.hasCycle() && deep.empty(), "too deep graph reports a cycle and draws nothing without a good frame");
	const Frame shallow = render(r, 45);
	check(!r.hasCycle() && !shallow.empty(), "chain of 5 processors renders");

	for (int bus = 1; bus <= kVideoBusCount; ++bus) {
		videoBusClear(bus);
	}
}

void testSeqlock() {
	std::printf("seqlock snapshot\n");
	const int bus = 40;
	constexpr int kWrites = 1000000;
	std::atomic<bool> done {false};
	std::thread writer([&]() {
		for (int v = 0; v < kWrites; ++v) {
			LineFieldParams p;
			p.depth = p.xTilt = p.yTilt = p.zSpin = p.perspective = p.lineDensity = p.gain = p.mix = static_cast<float>(v);
			p.invert = (v & 1) != 0;
			videoBusPublishLineField(bus, v, p);
			if (v % 16 == 0) {
				// Give the reader a chance to overlap with a write.
				std::this_thread::yield();
			}
		}
		done.store(true);
	});

	long reads = 0;
	long torn = 0;
	long wrongType = 0;
	long misses = 0;
	while (!done.load()) {
		VideoBusNode node;
		if (!videoBusReadNode(bus, node)) {
			++misses;
			continue;
		}
		if (node.type != VideoBusNodeType::LineField) {
			++wrongType;
			continue;
		}
		++reads;
		const int v = node.sourceBusId;
		const LineFieldParams& p = node.lineField;
		const float f = static_cast<float>(v);
		const bool consistent = p.depth == f && p.xTilt == f && p.yTilt == f && p.zSpin == f && p.perspective == f &&
			p.lineDensity == f && p.gain == f && p.mix == f && p.invert == ((v & 1) != 0);
		if (!consistent) {
			++torn;
		}
	}
	writer.join();
	std::printf("  reads %ld, misses %ld, torn %ld, wrong type %ld\n", reads, misses, torn, wrongType);
	check(reads > 1000, "reader got many snapshots while the writer was running");
	check(torn == 0, "every snapshot is internally consistent");
	check(wrongType == 0, "no snapshot mixes node types");
	videoBusClear(bus);
}

// The Display shows NO VIDEO when the renderer has no signal. A bus read that the writer keeps interrupting
// (the audio thread publishing every sample) says nothing about whether the bus is empty, so the renderer
// holds its last picture for that frame. Here the writer republishes the same live picture as fast as it
// can; the renderer must never lose the signal. After a real clear it must lose it at once.
void testNoFlashUnderWriterContention() {
	std::printf("no NO VIDEO flash while the writer hammers the bus\n");
	const int bus = 41;
	const int frameId = videoExternalFrameAllocate();
	const std::vector<unsigned char> px(4u * 8u * 8u, 200);
	videoExternalFramePublish(frameId, px.data(), 8, 8, 0.0);
	videoBusPublishExternalFrame(bus, frameId);
	VideoGraphRenderer renderer;
	renderer.renderBus(bus, 8, 8);
	check(renderer.hasSignal(), "the first picture has a signal");

	std::atomic<bool> done {false};
	std::thread writer([&]() {
		while (!done.load()) {
			videoBusPublishExternalFrame(bus, frameId);
		}
	});
	long frames = 0;
	long lost = 0;
	for (int i = 0; i < 200000; ++i) {
		renderer.renderBus(bus, 8, 8);
		++frames;
		if (!renderer.hasSignal()) {
			++lost;
		}
	}
	done.store(true);
	writer.join();
	std::printf("  %ld renders, %ld without signal\n", frames, lost);
	check(lost == 0, "the renderer never loses the signal while the bus is only being rewritten");

	videoBusClear(bus);
	renderer.renderBus(bus, 8, 8);
	check(!renderer.hasSignal(), "a real clear still shows no signal at once");
	videoExternalFrameRelease(frameId);
}

void testSlotAllocation() {
	std::printf("slot allocation past 8 instances\n");

	// Freeze slots: 20 distinct live slots, each keeps its own frame.
	std::vector<int> freezeSlots;
	for (int i = 0; i < 20; ++i) {
		freezeSlots.push_back(freezeFrameAllocateSlot());
	}
	std::set<int> uniqueFreeze(freezeSlots.begin(), freezeSlots.end());
	check(uniqueFreeze.size() == 20 && !uniqueFreeze.count(0), "20 freeze slots are distinct");
	for (size_t i = 0; i < freezeSlots.size(); ++i) {
		const unsigned char value = static_cast<unsigned char>(i + 1);
		const Frame px = {value, value, value, 255};
		freezeFrameCaptureIfNew(freezeSlots[i], 1, px, 1, 1);
	}
	bool freezeIsolated = true;
	for (size_t i = 0; i < freezeSlots.size(); ++i) {
		FreezeFrameData data;
		if (!freezeFrameRead(freezeSlots[i], data) || data.rgba[0] != static_cast<unsigned char>(i + 1)) {
			freezeIsolated = false;
		}
	}
	check(freezeIsolated, "freeze slots do not share data");
	// Releasing one slot must not clear another.
	freezeFrameReleaseSlot(freezeSlots[3]);
	FreezeFrameData survivor;
	check(freezeFrameRead(freezeSlots[4], survivor) && survivor.rgba[0] == 5, "releasing one freeze slot leaves others intact");
	FreezeFrameData released;
	check(!freezeFrameRead(freezeSlots[3], released), "released freeze slot is empty");
	const int reused = freezeFrameAllocateSlot();
	check(reused == freezeSlots[3], "released freeze slot id is reused only after release");
	for (int s : freezeSlots) {
		if (s != freezeSlots[3]) freezeFrameReleaseSlot(s);
	}
	freezeFrameReleaseSlot(reused);

	// Signal ring slots (Scope Trace and DeflectionYoke): each ring keeps its own samples.
	std::vector<int> scopeSlots;
	for (int i = 0; i < 20; ++i) {
		scopeSlots.push_back(signalRingAllocate());
	}
	std::set<int> uniqueScope(scopeSlots.begin(), scopeSlots.end());
	check(uniqueScope.size() == 20 && !uniqueScope.count(0), "20 signal ring slots are distinct");
	for (size_t i = 0; i < scopeSlots.size(); ++i) {
		const float v = static_cast<float>(i + 1) / 100.f;
		const float values[kSignalRingChannels] = {v, v, 1.f};
		signalRingPush(scopeSlots[i], values);
		signalRingPush(scopeSlots[i], values);
	}
	bool scopeIsolated = true;
	for (size_t i = 0; i < scopeSlots.size(); ++i) {
		SignalWindow window;
		const float expected = static_cast<float>(i + 1) / 100.f;
		if (!signalRingReadLatest(scopeSlots[i], 2, window) || std::fabs(window.sampleAt(0, 0) - expected) > 1e-6f) {
			scopeIsolated = false;
		}
	}
	check(scopeIsolated, "signal ring slots do not share samples");
	for (int s : scopeSlots) {
		signalRingRelease(s);
	}

	// External frames: distinct ids keep distinct pixels.
	std::vector<int> frameIds;
	for (int i = 0; i < 20; ++i) {
		frameIds.push_back(videoExternalFrameAllocate());
	}
	std::set<int> uniqueFrames(frameIds.begin(), frameIds.end());
	check(uniqueFrames.size() == 20 && !uniqueFrames.count(0), "20 external frame ids are distinct");
	for (size_t i = 0; i < frameIds.size(); ++i) {
		const unsigned char px[4] = {static_cast<unsigned char>(i + 1), 0, 0, 255};
		videoExternalFramePublish(frameIds[i], px, 1, 1, 0.0);
	}
	bool framesIsolated = true;
	for (size_t i = 0; i < frameIds.size(); ++i) {
		ExternalVideoFrame frame;
		if (!videoExternalFrameRead(frameIds[i], frame) || frame.rgba[0] != static_cast<unsigned char>(i + 1)) {
			framesIsolated = false;
		}
	}
	check(framesIsolated, "external frame ids do not share pixels");
	for (int id : frameIds) {
		videoExternalFrameRelease(id);
	}
}

void testDisplayInputs() {
	std::printf("Display input selection, fallback and JSON clamp\n");
	check(displayNextInput(0) == 1, "Display cycles A to B");
	check(displayNextInput(1) == 2, "Display cycles B to C");
	check(displayNextInput(2) == 0, "Display cycles C to A");
	check(displayNextInput(-1) == 0 && displayNextInput(3) == 0 && displayNextInput(999) == 0,
		"Display cycling recovers from invalid selections");
	check(displayInputBusId(0, false, 47.f) == 1, "unpatched A uses bus 1 despite stale voltage");
	check(displayInputBusId(1, false, 47.f) == 0, "unpatched B means no video");
	check(displayInputBusId(2, false, 47.f) == 0, "unpatched C means no video");
	for (int i = 0; i < 3; ++i) {
		check(displayInputBusId(i, true, 7.4f) == 7, "patched Display input decodes its bus token");
		check(displayInputBusId(i, true, 64.f) == 64, "patched Display input accepts the highest bus");
		check(displayInputBusId(i, true, 0.f) == 0, "patched Display input with zero voltage means no bus");
		check(displayInputBusId(i, true, 65.f) == 0, "patched Display input rejects an out-of-range bus");
		check(displayClampInput(i) == i, "JSON clamp preserves valid selections");
	}
	check(displayClampInput(-1) == 0 && displayClampInput(3) == 0 && displayClampInput(999) == 0,
		"JSON clamp resets invalid selections to A");
}

void testThruVoltage() {
	std::printf("THRU passes the BUS IN token and is 0 V when unpatched\n");
	check(videoThruVoltage(true, 3.f) == 3.f, "patched THRU repeats the bus id voltage");
	check(videoThruVoltage(true, 64.f) == 64.f, "patched THRU repeats the top bus id");
	check(videoThruVoltage(false, 3.f) == 0.f, "unpatched THRU is 0 V, even if a stale voltage is passed");
	check(videoBusIdFromVoltage(videoThruVoltage(true, 7.f)) == 7, "THRU output decodes to the same bus");
	check(videoBusIdFromVoltage(videoThruVoltage(false, 7.f)) == 0, "unpatched THRU means no bus");
	std::printf("\n");
}

void testBusIdAllocation() {
	std::printf("bus id allocation and voltage encoding\n");
	for (int bus = 1; bus <= kVideoBusCount; ++bus) {
		videoBusClear(bus);
	}

	check(videoBusIdFromVoltage(2.0f) == 2, "voltage 2 is bus 2");
	check(videoBusIdFromVoltage(2.4f) == 2 && videoBusIdFromVoltage(1.6f) == 2, "voltage rounds to the nearest bus");
	check(videoBusIdFromVoltage(0.f) == 0, "0 V is no bus");
	check(videoBusIdFromVoltage(kVideoBusCount + 1.f) == 0, "voltage above the bus count is no bus");
	check(videoBusIdFromVoltage(std::nanf("")) == 0, "NaN is no bus");

	{
		std::vector<std::unique_ptr<VideoBusOwner>> owners;
		std::set<int> ids;
		for (int i = 0; i < kVideoBusCount; ++i) {
			owners.push_back(std::make_unique<VideoBusOwner>());
			ids.insert(owners.back()->id());
		}
		check(ids.size() == static_cast<size_t>(kVideoBusCount) && !ids.count(0), "64 owners get 64 distinct bus ids");
		check(owners.back()->voltage() == static_cast<float>(owners.back()->id()), "owner voltage encodes its id");

		VideoBusOwner overflow;
		check(overflow.id() == 0, "65th owner gets no bus");
		check(videoBusIdFromVoltage(overflow.voltage()) == 0, "no-bus owner produces no bus voltage");

		// Releasing an owner frees exactly its id, which the next claim receives.
		const int freedA = owners[10]->id();
		owners[10].reset();
		VideoBusOwner loaded;
		check(loaded.id() == freedA, "released bus id is reused");

		// Patch load with a free preferred id: the owner moves to it and releases the old id.
		const int freedB = owners[11]->id();
		owners[11].reset();
		loaded.claimPreferred(freedB);
		check(loaded.id() == freedB, "claimPreferred takes a free saved id");
		VideoBusOwner next;
		check(next.id() == freedA, "id left behind by claimPreferred is free again");

		// Patch load with a saved id that another live module holds: no sharing.
		const int taken = owners[0]->id();
		loaded.claimPreferred(taken);
		check(loaded.id() != taken && loaded.id() != 0, "two modules loaded with the same saved id do not share a bus");
		check(owners[0]->id() == taken, "the module that already held the id keeps it");
	}

	// After owners are gone, all ids are free again.
	VideoBusOwner fresh;
	check(fresh.id() == 1, "all bus ids are released after owners are destroyed");
}

// ---- Bars and Rows (pattern 6) ----

// Parameters for a Bars and Rows picture with the colour out of the way: hue 0 and no spread, so the red
// channel of a pixel is the oscillator level itself.
VideoParams pairBase() {
	VideoParams p;
	p.pattern = static_cast<int>(VideoPattern::BarsAndRows);
	p.feedback = 0.f;
	p.hue = 0.f;
	p.spread = 0.f;
	p.mix = 0;
	return p;
}

Frame pairRender(int w, int h, const VideoParams& p) {
	PreviewVideoEngine engine;
	return engine.render(w, h, p);
}

// The oscillator level at a pixel, 0..1 (the red channel of a pairBase picture).
float pairLevel(const Frame& f, int w, int x, int y) {
	return f[(static_cast<size_t>(y) * w + x) * 4u] / 255.f;
}

double fractOf(double v) {
	return v - std::floor(v);
}

// Records the snapshots a module would publish at the start of each field, running the scan clock at
// `sampleRate`. Returns one VideoParams per field boundary crossed.
std::vector<VideoParams> pairFields(const VideoParams& base, double sampleRate, int fields) {
	PairClock clock;
	std::vector<VideoParams> out;
	const double dt = 1.0 / sampleRate;
	bool odd = clock.fieldOdd;
	for (long n = 0; n < static_cast<long>(sampleRate * 0.2) && static_cast<int>(out.size()) < fields; ++n) {
		clock.advance(dt, base.cyclesA, base.barsA, base.cyclesB, base.barsB);
		if (clock.fieldOdd != odd) {
			odd = clock.fieldOdd;
			VideoParams p = base;
			p.latchedA = clock.latchedA;
			p.latchedB = clock.latchedB;
			p.fieldOdd = clock.fieldOdd;
			out.push_back(p);
		}
	}
	return out;
}

void testPairLockedBars() {
	std::printf("pair: locked bars\n");
	VideoParams p = pairBase();
	p.cyclesA = 4.f;
	p.shape = 1.f;  // square, high on the second half of each cycle
	const int w = 256;
	const int h = 144;
	const Frame f = pairRender(w, h, p);
	int wrong = 0;
	for (int x = 0; x < w; ++x) {
		const double ph = fractOf(4.0 * (x + 0.5) / w);
		if (ph > 0.1 && ph < 0.4 && pairLevel(f, w, x, 70) > 0.05f) {
			++wrong;
		}
		if (ph > 0.6 && ph < 0.9 && pairLevel(f, w, x, 70) < 0.95f) {
			++wrong;
		}
	}
	check(wrong == 0, "locked bars: square wave low then high in every cycle");
	int rowsDiffer = 0;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			rowsDiffer += f[(static_cast<size_t>(y) * w + x) * 4u] != f[static_cast<size_t>(x) * 4u];
		}
	}
	check(rowsDiffer == 0, "locked bars: every row identical");
	// Four whole cycles across the width, counted from rising edges.
	int rising = 0;
	for (int x = 1; x < w; ++x) {
		rising += pairLevel(f, w, x - 1, 0) < 0.5f && pairLevel(f, w, x, 0) >= 0.5f;
	}
	check(rising == 4, "locked bars: 4 cycles across the picture");
}

void testPairLockedRows() {
	std::printf("pair: locked rows\n");
	VideoParams p = pairBase();
	p.cyclesA = 4.f;
	p.barsA = false;
	p.shape = 0.f;  // sine
	const int w = 64;
	const int h = 240;
	const Frame f = pairRender(w, h, p);
	int crossings = 0;
	float worst = 0.f;
	for (int y = 0; y < h; ++y) {
		const double u = (31 + 0.5) / w;
		const double expect = 0.5 - 0.5 * std::cos(6.283185307179586 * (4.0 / 262.5) * (y + u));
		worst = std::max(worst, std::abs(pairLevel(f, w, 31, y) - static_cast<float>(expect)));
		if (y > 0) {
			crossings += (pairLevel(f, w, 31, y - 1) < 0.5f) != (pairLevel(f, w, 31, y) < 0.5f);
		}
	}
	check(worst < 0.01f, "locked rows: level follows 4 cycles per 262.5 lines");
	check(crossings == 7, "locked rows: about 3.66 cycles down the picture (7 mid-level crossings)");
	// Flat along a line to within the small drift of one line.
	check(std::abs(pairLevel(f, w, 0, 100) - pairLevel(f, w, w - 1, 100)) < 0.02f, "locked rows: nearly flat along a line");
	saveProof("pair_rows.png", {pairRender(kW, kH, p)});
}

void testPairCheckerboard() {
	std::printf("pair: checkerboard\n");
	VideoParams p = pairBase();
	p.cyclesA = 4.f;
	p.shape = 1.f;
	p.cyclesB = 4.f;
	p.barsB = false;
	p.waveB = 1.f;
	p.bend = 0.5f;
	const int w = 256;
	const int h = 240;
	const Frame f = pairRender(w, h, p);
	int checked = 0;
	int wrong = 0;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const double u = (x + 0.5) / w;
			const double pb = fractOf(4.0 / 262.5 * (y + u));
			if (std::abs(pb - 0.5) < 0.06 || pb < 0.06 || pb > 0.94) {
				continue;
			}
			const double b = pb > 0.5 ? 1.0 : 0.0;
			const double pa = fractOf(4.0 * u + 0.5 * b);
			if (std::abs(pa - 0.5) < 0.06 || pa < 0.06 || pa > 0.94) {
				continue;
			}
			const double a = pa > 0.5 ? 1.0 : 0.0;
			++checked;
			wrong += std::abs(pairLevel(f, w, x, y) - static_cast<float>(a)) > 0.02f;
		}
	}
	check(checked > 20000, "checkerboard: enough pixels away from edges to judge");
	check(wrong == 0, "checkerboard: A square flipped by half a cycle wherever B square is high");
	saveProof("pair_checker.png", {pairRender(kW, kH, p)});
}

void testPairMixes() {
	std::printf("pair: mixes\n");
	VideoParams p = pairBase();
	p.cyclesA = 2.f;
	p.shape = 1.f;
	p.cyclesB = 4.f;
	p.barsB = true;
	p.waveB = 1.f;
	const int w = 256;
	const int h = 4;
	int seen[4] = {0, 0, 0, 0};
	for (int mix = 0; mix < 4; ++mix) {
		p.mix = mix;
		const Frame f = pairRender(w, h, p);
		int wrong = 0;
		for (int x = 0; x < w; ++x) {
			const double u = (x + 0.5) / w;
			const double pa = fractOf(2.0 * u);
			const double pb = fractOf(4.0 * u);
			if (std::abs(pa - 0.5) < 0.06 || pa < 0.06 || pa > 0.94 || std::abs(pb - 0.5) < 0.06 || pb < 0.06 || pb > 0.94) {
				continue;
			}
			const double a = pa > 0.5 ? 1.0 : 0.0;
			const double b = pb > 0.5 ? 1.0 : 0.0;
			double expect = a;
			if (mix == 1) {
				expect = 0.5 * (a + b);
			}
			else if (mix == 2) {
				expect = 0.5 + 0.5 * (a - b);
			}
			else if (mix == 3) {
				expect = a * b;
			}
			++seen[mix];
			wrong += std::abs(pairLevel(f, w, x, 1) * 255.0 - expect * 255.0) > 2.0;
		}
		static const char* names[4] = {"mix A shows A", "mix A+B is 0.5(a+b)", "mix A-B is 0.5+0.5(a-b)", "mix AxB is a*b"};
		check(seen[mix] > 100 && wrong == 0, names[mix]);
	}
}

void testPairFreeRunning() {
	std::printf("pair: free running\n");
	const double rates[2] = {48000.0, 44100.0};
	for (double rate : rates) {
		const int w = 128;
		const int h = 240;
		auto fieldsOf = [&](float cycles, bool bars) {
			VideoParams p = pairBase();
			p.shape = 0.f;
			p.cyclesA = cycles;
			p.barsA = bars;
			p.lockA = false;
			std::vector<Frame> frames;
			for (const VideoParams& field : pairFields(p, rate, 7)) {
				frames.push_back(pairRender(w, h, field));
			}
			return frames;
		};
		auto sameAcross = [&](const std::vector<Frame>& frames) {
			int worst = 0;
			for (size_t i = 1; i < frames.size(); ++i) {
				worst = std::max(worst, maxDiff(frames[i], frames[0]));
			}
			return worst;
		};
		char label[160];

		const std::vector<Frame> bars7 = fieldsOf(7.f, true);
		std::snprintf(label, sizeof label, "free bars at 7 hold still across %zu fields at %.0f Hz", bars7.size(), rate);
		check(bars7.size() >= 6 && sameAcross(bars7) <= 2, label);

		const std::vector<Frame> bars8 = fieldsOf(8.f, true);
		std::snprintf(label, sizeof label, "free bars at 8 hold still across %zu fields at %.0f Hz", bars8.size(), rate);
		check(bars8.size() >= 6 && sameAcross(bars8) <= 2, label);

		const std::vector<Frame> rows5 = fieldsOf(5.f, false);
		std::snprintf(label, sizeof label, "free rows at 5 hold still across %zu fields at %.0f Hz", rows5.size(), rate);
		check(rows5.size() >= 6 && sameAcross(rows5) <= 2, label);

		const std::vector<Frame> rows41 = fieldsOf(4.1f, false);
		std::snprintf(label, sizeof label, "free rows at 4.1 move from field to field at %.0f Hz", rate);
		check(rows41.size() >= 6 && maxDiff(rows41[1], rows41[0]) > 20 && maxDiff(rows41[5], rows41[1]) > 20, label);

		// Free bars at 8.25: each scanline starts a quarter cycle further on.
		const std::vector<Frame> skew = fieldsOf(8.25f, true);
		bool skewOk = skew.size() >= 6;
		for (size_t i = 0; skewOk && i < skew.size(); ++i) {
			const Frame& f = skew[i];
			// Four lines make a whole 33 cycles; two lines make a half, which turns a sine upside down.
			int offFour = 0;
			int offTwo = 0;
			int offOne = 0;
			for (int x = 0; x < w; ++x) {
				const float v0 = pairLevel(f, w, x, 100);
				offFour += std::abs(pairLevel(f, w, x, 104) - v0) > 0.02f;
				offTwo += std::abs(pairLevel(f, w, x, 102) - (1.f - v0)) > 0.02f;
				offOne += std::abs(pairLevel(f, w, x, 101) - v0) > 0.1f;
			}
			skewOk = skewOk && offFour == 0 && offTwo == 0 && offOne > w / 4;
		}
		std::snprintf(label, sizeof label, "free bars at 8.25 shift a quarter cycle per scanline at %.0f Hz", rate);
		check(skewOk, label);
		if (rate == 48000.0) {
			VideoParams p = pairBase();
			p.shape = 0.f;
			p.cyclesA = 8.25f;
			p.lockA = false;
			const std::vector<VideoParams> fields = pairFields(p, rate, 2);
			saveProof("pair_free_skew.png", {pairRender(kW, kH, fields[0]), pairRender(kW, kH, fields[1])});
		}
	}
	// The clock flips field parity at 59.94 per second.
	PairClock clock;
	int flips = 0;
	bool odd = clock.fieldOdd;
	const long samples = 48000;
	for (long n = 0; n < samples; ++n) {
		clock.advance(1.0 / 48000.0, 8.0, true, 4.0, false);
		if (clock.fieldOdd != odd) {
			odd = clock.fieldOdd;
			++flips;
		}
	}
	const double expectFields = 15734.264 / 262.5;
	check(std::abs(flips - expectFields) <= 1.0, "scan clock: about 59.94 fields in one second");
}

void testPairFinePitch() {
	std::printf("pair: fine pitch\n");
	for (float wave : {0.f, 1.f}) {
		VideoParams p = pairBase();
		p.cyclesA = 400.f;
		p.shape = wave;
		const int w = 640;
		const Frame f = pairRender(w, 4, p);
		int worst = 0;
		for (int x = 0; x < w; ++x) {
			worst = std::max(worst, std::abs(static_cast<int>(f[static_cast<size_t>(x) * 4u]) - 127));
		}
		check(worst <= 2, wave == 0.f ? "400 cycles on 640 px: flat grey (sine)" : "400 cycles on 640 px: flat grey (square)");
	}
	VideoParams p = pairBase();
	p.cyclesA = 100.f;
	p.shape = 0.f;
	const Frame f = pairRender(640, 4, p);
	int lo = 255;
	int hi = 0;
	for (int x = 0; x < 640; ++x) {
		lo = std::min(lo, static_cast<int>(f[static_cast<size_t>(x) * 4u]));
		hi = std::max(hi, static_cast<int>(f[static_cast<size_t>(x) * 4u]));
	}
	check(lo < 30 && hi > 225, "100 cycles on 640 px keeps full swing");
}

void testPairWave() {
	std::printf("pair: wave\n");
	const double tiny = 1e-5;
	bool inRange = true;
	for (int i = 0; i <= 100; ++i) {
		for (int j = 0; j < 200; ++j) {
			const float v = pairWaveValue(j / 200.0, i / 100.f, tiny);
			inRange = inRange && v >= 0.f && v <= 1.f;
		}
	}
	check(inRange, "wave stays within 0..1 for every WAVE and phase");

	bool sineOk = true;
	bool triOk = true;
	bool rampOk = true;
	for (int j = 1; j < 200; ++j) {
		const double ph = j / 200.0;
		sineOk = sineOk && std::abs(pairWaveValue(ph, 0.f, tiny) - (0.5 - 0.5 * std::cos(6.283185307179586 * ph))) < 1e-4;
		triOk = triOk && std::abs(pairWaveValue(ph, 1.f / 3.f, tiny) - (1.0 - std::abs(2.0 * ph - 1.0))) < 1e-3;
		rampOk = rampOk && std::abs(pairWaveValue(ph, 2.f / 3.f, tiny) - ph) < 1e-3;
	}
	check(sineOk, "WAVE 0 is a sine");
	check(triOk, "WAVE 1/3 is a triangle");
	check(rampOk, "WAVE 2/3 is a ramp");
	check(pairWaveValue(0.25, 1.f, tiny) < 0.001f && pairWaveValue(0.75, 1.f, tiny) > 0.999f, "WAVE 1 is a square, high on the second half");

	// Continuity in WAVE at the joins, and in phase away from the wrap edge.
	double worstJoin = 0.0;
	for (int j = 0; j < 200; ++j) {
		const double ph = (j + 0.5) / 200.0;
		for (double join : {1.0 / 3.0, 2.0 / 3.0}) {
			worstJoin = std::max(worstJoin, std::abs(static_cast<double>(pairWaveValue(ph, static_cast<float>(join - 1e-4), 1e-3)) - pairWaveValue(ph, static_cast<float>(join + 1e-4), 1e-3)));
		}
	}
	check(worstJoin < 0.01, "wave is continuous across the joins at 1/3 and 2/3");
	double worstStep = 0.0;
	for (int i = 0; i <= 90; ++i) {
		for (int j = 0; j < 980; ++j) {
			worstStep = std::max(worstStep, std::abs(static_cast<double>(pairWaveValue((j + 1) * 1e-3, i / 100.f, 0.01)) - pairWaveValue(j * 1e-3, i / 100.f, 0.01)));
		}
	}
	check(worstStep < 0.05, "wave has no jumps along the phase up to WAVE 0.9");

	// No edge narrower than one footprint, even at full square.
	const double foot = 0.02;
	double firstAbove = -1.0;
	double lastBelow = -1.0;
	for (int j = 0; j <= 20000; ++j) {
		const double ph = 0.4 + 0.2 * j / 20000.0;
		const float v = pairWaveValue(ph, 1.f, foot);
		if (v < 0.05f) {
			lastBelow = ph;
		}
		if (v > 0.95f && firstAbove < 0.0) {
			firstAbove = ph;
		}
	}
	check(firstAbove - lastBelow >= 0.85 * foot, "square edge takes at least one footprint");
	// The wrap edge of a ramp too.
	double fallStart = -1.0;
	for (int j = 0; j <= 20000; ++j) {
		const double ph = 0.9 + 0.1 * j / 20000.0;
		if (pairWaveValue(ph, 2.f / 3.f, foot) < 0.95f * 0.98f && fallStart < 0.0 && ph > 0.95) {
			fallStart = ph;
		}
	}
	check(fallStart >= 0.0 && fallStart < 1.0 && (1.0 - fallStart) >= 0.5 * foot, "ramp wrap edge takes about one footprint");

	// The knob curve in the module: the A and B knobs share it. Cycles stay in 0.25..512.
	check(kPairMinCycles == 0.25 && kPairMaxCycles == 512.0, "cycles are limited to 0.25..512");
}

void testPairClockContinuity() {
	std::printf("pair: clock continuity\n");
	PairClock c;
	const double dt = 1.0 / 48000.0;
	for (int n = 0; n < 3000; ++n) {
		c.advance(dt, 4.0, true, 3.0, false);
	}
	const double before = c.phaseA;
	const double beforeB = c.phaseB;
	const double lineBefore = c.fieldLine;
	c.advance(dt, 8.0, true, 3.0, false);
	const double step = 8.0 * 15734.264 * dt;
	check(std::abs(fractOf(c.phaseA - before - step)) < 1e-9 || std::abs(fractOf(c.phaseA - before - step) - 1.0) < 1e-9,
	      "changing A's cycles moves the phase on by one step, no jump");
	check(std::abs(c.phaseB - beforeB - 3.0 / 262.5 * 15734.264 * dt) < 1e-9, "B keeps its own phase when A changes");
	check(std::abs(c.fieldLine - lineBefore - 15734.264 * dt) < 1e-9, "field position is unaffected by cycles");
	// Over a field boundary the latch lands on the phase the boundary had, whatever changed before it.
	PairClock d;
	for (int n = 0; n < 5000; ++n) {
		d.advance(dt, n < 2500 ? 4.0 : 6.5, true, 3.0, false);
	}
	check(d.latchedA >= 0.0 && d.latchedA < 1.0 && d.latchedB >= 0.0 && d.latchedB < 1.0, "latched phases stay in 0..1");
}

void testPairProofs() {
	std::printf("pair: proofs\n");
	VideoParams bars = pairBase();
	bars.cyclesA = 8.f;
	bars.shape = 0.f;
	bars.hue = 0.58f;
	bars.saturation = 0.f;
	VideoParams rows = bars;
	rows.barsA = false;
	rows.cyclesA = 6.f;
	saveProof("pair_bars_rows.png", {pairRender(kW, kH, bars), pairRender(kW, kH, rows)});

	VideoParams plaid = pairBase();
	plaid.mix = 1;
	plaid.cyclesA = 10.f;
	plaid.cyclesB = 12.f;
	plaid.barsB = false;
	plaid.shape = 1.f / 3.f;
	plaid.waveB = 1.f / 3.f;
	plaid.hue = 0.1f;
	plaid.spread = 0.5f;
	VideoParams weave = plaid;
	weave.mix = 3;
	weave.shape = 0.f;
	weave.waveB = 0.f;
	saveProof("pair_plaid.png", {pairRender(kW, kH, plaid), pairRender(kW, kH, weave)});

	VideoParams zig = pairBase();
	zig.cyclesA = 6.f;
	zig.shape = 1.f / 3.f;
	zig.cyclesB = 3.f;
	zig.barsB = false;
	zig.waveB = 1.f / 3.f;
	zig.bend = 1.5f;
	zig.hue = 0.6f;
	zig.saturation = 0.f;
	saveProof("pair_zigzag.png", {pairRender(kW, kH, zig)});

	VideoParams colour = pairBase();
	colour.mix = 2;
	colour.cyclesA = 5.f;
	colour.cyclesB = 9.f;
	colour.barsB = false;
	colour.shape = 0.f;
	colour.waveB = 2.f / 3.f;
	colour.hue = 0.55f;
	colour.spread = 0.7f;
	VideoParams colour2 = colour;
	colour2.mix = 1;
	colour2.bend = 0.8f;
	saveProof("pair_colour.png", {pairRender(kW, kH, colour), pairRender(kW, kH, colour2)});
}

}  // namespace

// ---- scan-time signal ring ----
void testSignalRingFrames() {
	std::printf("signal ring: frame windows and interpolation\n");
	constexpr float sr = 48000.f;
	const int S = 800;  // 48000 / 60
	auto ringPtr = std::make_unique<SignalRing>();
	SignalRing& ring = *ringPtr;
	ring.setSampleRate(sr);
	SignalWindow w;
	check(!ring.readFrame(w), "no frame before any samples");

	// A 1 kHz sine on channel 0, a ramp on channel 1, and its negation on channel 2.
	const double pi = 3.14159265358979323846;
	const int total = S * 5 + 37;  // a partial frame at the end
	for (int n = 0; n < total; ++n) {
		const float v = static_cast<float>(std::sin(2.0 * pi * 1000.0 * n / sr));
		const float values[kSignalRingChannels] = {v, static_cast<float>(n), -v};
		ring.push(values);
	}

	check(ring.readFrame(w), "readFrame after several frames");
	check(w.framePeriodSamples == S && w.length == S + 2, "frame window size is S + 2");

	// Newest complete boundary: (total - 2) / S = 5, so the window is frame 4, starting at 3200.
	const int start = 4 * S;
	double worst = 0.0;
	for (int i = 0; i < S; i += 7) {
		const float t = static_cast<float>(i) / static_cast<float>(S) * static_cast<float>(kRasterFramePeriod);
		const float expected = static_cast<float>(std::sin(2.0 * pi * 1000.0 * (start + i) / sr));
		worst = std::max(worst, static_cast<double>(std::fabs(w.valueAtScanTime(0, t) - expected)));
	}
	check(worst < 0.01, "scan-time value of a 1 kHz sine matches the sample at that time");

	// Linear interpolation: channel 1 is a ramp, so any fractional position reads exactly.
	const float ramp = w.sampleAt(1, 10.25);
	check(std::fabs(ramp - (start + 10.25)) < 1e-3f, "interpolation between samples is linear");

	// Channel 2 is the negation of channel 0 everywhere in the window.
	bool mirrored = true;
	for (int i = 0; i < w.length; ++i) {
		if (std::fabs(w.sampleAt(2, i) + w.sampleAt(0, i)) > 1e-6f) {
			mirrored = false;
		}
	}
	check(mirrored, "all channels of a window come from the same sample times");

	// Latest-N reader returns the newest samples.
	SignalWindow latest;
	check(ring.readLatest(100, latest) && latest.length == 100, "readLatest returns the requested count");
	check(std::fabs(latest.sampleAt(1, 99) - (total - 1)) < 1e-3f, "readLatest ends at the newest sample");

	// Clear ignores history; new samples are read again.
	ring.clear();
	check(!ring.readFrame(w), "no frame right after clear");
	check(!ring.readLatest(10, latest), "no latest samples right after clear");
	const float fresh[kSignalRingChannels] = {0.5f, 0.f, -0.5f};
	for (int n = 0; n < 4; ++n) {
		ring.push(fresh);
	}
	check(ring.readLatest(4, latest) && std::fabs(latest.sampleAt(0, 0) - 0.5f) < 1e-6f, "samples after clear are read");
}

void testSignalRingConcurrent() {
	std::printf("signal ring: concurrent writer and reader\n");
	auto ring = std::make_unique<SignalRing>();
	ring->setSampleRate(48000.f);
	std::atomic<bool> done {false};
	std::thread writer([&] {
		for (int n = 0; n < 2000000; ++n) {
			const float v = std::sin(static_cast<float>(n) * 0.01f);
			const float values[kSignalRingChannels] = {v, static_cast<float>(n), -v};
			ring->push(values);
		}
		done.store(true, std::memory_order_release);
	});

	int reads = 0;
	int badWindows = 0;
	SignalWindow w;
	while (!done.load(std::memory_order_acquire)) {
		if (ring->readFrame(w)) {
			++reads;
			for (int i = 0; i < w.length; ++i) {
				if (std::fabs(w.sampleAt(0, i) + w.sampleAt(2, i)) > 1e-6f) {
					++badWindows;
					break;
				}
			}
		}
	}
	writer.join();
	check(reads > 0, "reader obtained windows while the writer ran");
	check(badWindows == 0, "no torn windows seen by the reader");
}

void testExpHz() {
	std::printf("exponential Hz knobs\n");
	check(std::fabs(expHzFromNormalized(0.f, 0.1f, 8000.f) - 0.1f) < 1e-6f, "knob 0 is the minimum");
	check(std::fabs(expHzFromNormalized(1.f, 0.1f, 8000.f) - 8000.f) < 1e-1f, "knob 1 is the maximum");
	const float mid = expHzFromNormalized(0.5f, 0.1f, 8000.f);
	check(std::fabs(mid - 28.28f) < 0.05f, "half a turn is the geometric mean (about 28 Hz)");
	const float back = normalizedFromExpHz(expHzFromNormalized(0.37f, 0.01f, 20.f), 0.01f, 20.f);
	check(std::fabs(back - 0.37f) < 1e-4f, "Hz to knob and back is the identity");
}

// ---- DeflectionYoke on the scan-time signal ----
// Scanlines per field in these tests (the module default). Ripples are counted over the visible
// part of the field: the beam is off during the flyback.
constexpr int kWobLines = 240;
double visibleSeconds(int lines) {
	return static_cast<double>(lines) * crt::lineSeconds();
}

// Source frames and proofs are 448 x 640, a portrait test raster (not the 4:3 Display screen).
// The raster is one nominal frame (1/60 s) tall.
constexpr int kWobW = 448;
constexpr int kWobH = 640;
constexpr float kWobRate = 48000.f;
constexpr int kWobSamplesPerFrame = 800;  // 48000 / 60

// Vertical white line on black, or a grid, at kWobW x kWobH.
Frame makeLineSource(int w, int h, int column) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const unsigned char v = x == column ? 255 : 0;
			f[i] = f[i + 1] = f[i + 2] = v;
			f[i + 3] = 255;
		}
	}
	return f;
}

Frame makeGridSource(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const bool line = (x % 40 == 0) || (y % 40 == 0);
			const unsigned char v = line ? 235 : 40;
			f[i] = v;
			f[i + 1] = static_cast<unsigned char>(line ? 235 : 40);
			f[i + 2] = static_cast<unsigned char>(line ? 80 : 90);
			f[i + 3] = 255;
		}
	}
	return f;
}

// Pushes `frames` raster frames of samples. gen(n, out) fills the channels for sample n.
int pushScanSignal(int slot, int frames, const std::function<void(int, float*)>& gen) {
	signalRingSetSampleRate(slot, kWobRate);
	const int total = kWobSamplesPerFrame * frames + 35;
	for (int n = 0; n < total; ++n) {
		float values[kSignalRingChannels] = {0.f, 0.f, 0.f};
		gen(n, values);
		signalRingPush(slot, values);
	}
	return total;
}

Frame renderWobble(VideoGraphRenderer& r, const Frame& source, int slot, const DeflectionYokeParams& params) {
	constexpr int srcBus = 40;
	constexpr int outBus = 41;
	const int frameId = videoExternalFrameAllocate();
	videoExternalFramePublish(frameId, source.data(), kWobW, kWobH, 0.0);
	videoBusPublishExternalFrame(srcBus, frameId);
	DeflectionYokeParams p = params;
	p.signalSlotId = slot;
	videoBusPublishDeflectionYoke(outBus, srcBus, p);
	Frame out = r.renderBus(outBus, kWobW, kWobH);
	videoBusClear(outBus);
	videoBusClear(srcBus);
	videoExternalFrameRelease(frameId);
	return out;
}

// Luma-weighted centre column of the bright line in each row (NaN where the row is dark).
std::vector<double> lineCentroids(const Frame& f, int w, int h) {
	std::vector<double> c(static_cast<size_t>(h), std::nan(""));
	for (int y = 0; y < h; ++y) {
		double sum = 0.0;
		double weighted = 0.0;
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			const double luma = (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
			sum += luma;
			weighted += luma * x;
		}
		if (sum > 0.0) {
			c[static_cast<size_t>(y)] = weighted / sum;
		}
	}
	return c;
}

// Ripples = full cycles of the row-by-row line position. Counted with a Schmitt trigger at 30 % of
// the amplitude, so noise near a turning point is not counted twice.
int countRipples(const std::vector<double>& c) {
	std::vector<double> valid;
	for (double v : c) {
		if (!std::isnan(v)) {
			valid.push_back(v);
		}
	}
	if (valid.size() < 3) {
		return 0;
	}
	const auto [lo, hi] = std::minmax_element(valid.begin(), valid.end());
	const double mid = 0.5 * (*lo + *hi);
	const double band = 0.3 * 0.5 * (*hi - *lo);
	int cycles = 0;
	int state = 0;  // -1 below the band, +1 above it
	for (double v : valid) {
		const double s = v - mid;
		if (s > band) {
			if (state == -1) {
				++cycles;
			}
			state = 1;
		}
		else if (s < -band) {
			state = -1;
		}
	}
	return cycles;
}

void saveWobbleProof(const std::string& name, const Frame& f) {
	const std::string path = gProofDir + "/" + name;
	if (writePng(path, kWobW, kWobH, f)) {
		std::printf("  proof: %s\n", path.c_str());
	}
	else {
		std::printf("  could not write %s\n", path.c_str());
		check(false, "wobble proof image written");
	}
}

void testDeflectionYokeRipples() {
	std::printf("deflection yoke: ripples per frame from audio-rate deflection\n");
	const Frame line = makeLineSource(kWobW, kWobH, kWobW / 2);
	const int slot = signalRingAllocate();
	VideoGraphRenderer renderer;
	const double pi = 3.14159265358979323846;

	DeflectionYokeParams p;
	p.depth = 1.f;

	const double freqs[] = {200.0, 1000.0, 5000.0};
	for (double f : freqs) {
		// X deflection at full scale (0.25 of the frame width), Y and zoom unused.
		const int frames = 6;
		pushScanSignal(slot, frames, [&](int n, float* v) {
			v[kDeflectionYokeDeflectX] = static_cast<float>(0.25 * std::sin(2.0 * pi * f * n / kWobRate));
			v[kDeflectionYokeDeflectY] = 0.f;
			v[kDeflectionYokeZoom] = 0.f;
		});
		const Frame out = renderWobble(renderer, line, slot, p);
		const std::vector<double> c = lineCentroids(out, kWobW, kWobH);
		const int ripples = countRipples(c);
		// The beam is off during the flyback, so the picture shows the visible lines only.
		const double expected = f * visibleSeconds(kWobLines);
		std::printf("  X at %.0f Hz: %d ripples in one frame (expected %.2f)\n", f, ripples, expected);
		check(std::fabs(ripples - expected) <= 1.5, "ripple count matches frequency times frame period");
		char name[64];
		std::snprintf(name, sizeof(name), "wobble_x_%.0fhz.png", f);
		saveWobbleProof(name, out);

		// The same deflection on the grid: continuous wavy lines at each frequency.
		const Frame gridOut = renderWobble(renderer, makeGridSource(kWobW, kWobH), slot, p);
		std::snprintf(name, sizeof(name), "wobble_grid_x_%.0fhz.png", f);
		saveWobbleProof(name, gridOut);
	}

	// Quadrature: X = sin, Y = cos at 200 Hz on the grid. Verify the ring pair and the rendered warp.
	const Frame grid = makeGridSource(kWobW, kWobH);
	pushScanSignal(slot, 6, [&](int n, float* v) {
		const double ph = 2.0 * pi * 200.0 * n / kWobRate;
		v[kDeflectionYokeDeflectX] = static_cast<float>(0.25 * std::sin(ph));
		v[kDeflectionYokeDeflectY] = static_cast<float>(0.25 * std::cos(ph));
		v[kDeflectionYokeZoom] = 0.f;
	});
	SignalWindow w;
	check(signalRingReadFrame(slot, w), "quadrature: a frame window is readable");
	double crossSum = 0.0;
	double energyX = 0.0;
	double energyY = 0.0;
	for (int i = 0; i < w.length; ++i) {
		const double dx = w.sampleAt(kDeflectionYokeDeflectX, i);
		const double dy = w.sampleAt(kDeflectionYokeDeflectY, i);
		crossSum += dx * dy;
		energyX += dx * dx;
		energyY += dy * dy;
	}
	const double correlation = crossSum / std::sqrt(energyX * energyY);
	std::printf("  X/Y correlation %.4f (quadrature)\n", correlation);
	check(std::fabs(correlation) < 0.05, "X and Y deflection are in quadrature");

	const Frame warp = renderWobble(renderer, grid, slot, p);
	saveWobbleProof("wobble_xy_quadrature_200hz.png", warp);
	check(meanDiff(warp, grid) > 1.0, "quadrature warp changes the grid");
	check(maxDiff(warp, grid) > 0, "quadrature warp differs from the source");

	// Zoom: a vertical line at 3/4 of the width lands at 0.5 + zoom * 0.25 of the width. Each scanline
	// carries that cell, so each scanline's centroid gives the zoom at that line's scan time.
	const int zoomSlot = signalRingAllocate();
	const int frames = 6;
	const double zoomHz = 30.0;
	const double zoomDepth = 0.3;
	const int total = pushScanSignal(zoomSlot, frames, [&](int n, float* v) {
		v[kDeflectionYokeDeflectX] = 0.f;
		v[kDeflectionYokeDeflectY] = 0.f;
		v[kDeflectionYokeZoom] = static_cast<float>(zoomDepth * std::sin(2.0 * pi * zoomHz * n / kWobRate));
	});
	const int fieldSamples = static_cast<int>(std::lround(kWobRate * crt::fieldSeconds(kWobLines)));
	const int boundary = (total - 2) / fieldSamples;
	const int windowStart = (boundary - 1) * fieldSamples;
	const double samplesPerLine = static_cast<double>(fieldSamples) / (kWobLines + crt::kBlankingLines);
	const Frame zoomLine = makeLineSource(kWobW, kWobH, kWobW * 3 / 4);
	const Frame zoomed = renderWobble(renderer, zoomLine, zoomSlot, p);
	const double lineNorm = static_cast<double>(kWobW * 3 / 4) / (kWobW - 1);
	double worstZoomError = 0.0;
	int zoomLines = 0;
	for (int k = 0; k < kWobLines; ++k) {
		// The cell at column 3/4 of line k is painted at (k + 0.75) line times into the field.
		const double sample = windowStart + (k + 0.75) * samplesPerLine;
		const double analytic = 1.0 + zoomDepth * std::sin(2.0 * pi * zoomHz * sample / kWobRate);
		// Zoom also moves the lines up and down, so find this line's row from its predicted landing.
		const double gy = (k + 0.5) / kWobLines;
		const int predicted = static_cast<int>(std::lround((0.5 + analytic * (gy - 0.5)) * kWobH));
		int row = -1;
		double bestLight = 0.0;
		for (int r = std::max(0, predicted - 2); r <= std::min(kWobH - 1, predicted + 2); ++r) {
			double light = 0.0;
			for (int x = 0; x < kWobW; ++x) {
				light += zoomed[(static_cast<size_t>(r) * kWobW + x) * 4u];
			}
			if (light > bestLight) {
				bestLight = light;
				row = r;
			}
		}
		if (row < 0) {
			continue;
		}
		double sum = 0.0;
		double weighted = 0.0;
		for (int x = 0; x < kWobW; ++x) {
			const double v = zoomed[(static_cast<size_t>(row) * kWobW + x) * 4u];
			sum += v;
			weighted += v * x;
		}
		const double xNorm = weighted / sum / (kWobW - 1);
		const double measured = (xNorm - 0.5) / (lineNorm - 0.5);
		worstZoomError = std::max(worstZoomError, std::fabs(measured - analytic));
		++zoomLines;
	}
	std::printf("  zoom: %d scanlines, worst error %.4f\n", zoomLines, worstZoomError);
	check(zoomLines > kWobLines * 9 / 10, "zoom scanlines are visible");
	check(worstZoomError < 0.02, "zoom at each scanline matches the zoom signal at its scan time");
	saveWobbleProof("wobble_zoom_30hz.png", zoomed);
	signalRingRelease(zoomSlot);

	// No signal yet: the source passes through unchanged.
	const int emptySlot = signalRingAllocate();
	const Frame pass = renderWobble(renderer, grid, emptySlot, p);
	check(maxDiff(pass, grid) == 0, "no raster signal passes the source through");
	signalRingRelease(emptySlot);
}

// Grid lines dim (120) and one vertical line at `column` bright (235), on black (40 between lines).
Frame makeGridWithOneLine(int w, int h, int column) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const bool grid = (x % 40 == 0) || (y % 40 == 0);
			const unsigned char v = x == column ? 235 : (grid ? 120 : 40);
			f[i] = f[i + 1] = f[i + 2] = v;
			f[i + 3] = 255;
		}
	}
	return f;
}

// Connected components of pixels whose red channel is at least 200 (8-connected).
int brightComponents(const Frame& f, int w, int h) {
	std::vector<char> seen(static_cast<size_t>(w) * h, 0);
	int components = 0;
	std::vector<int> stack;
	for (int start = 0; start < w * h; ++start) {
		if (seen[static_cast<size_t>(start)] || f[static_cast<size_t>(start) * 4u] < 200) {
			continue;
		}
		++components;
		stack.assign(1, start);
		seen[static_cast<size_t>(start)] = 1;
		while (!stack.empty()) {
			const int p = stack.back();
			stack.pop_back();
			const int py = p / w;
			const int px = p % w;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					const int nx = px + dx;
					const int ny = py + dy;
					if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
						continue;
					}
					const int q = ny * w + nx;
					if (!seen[static_cast<size_t>(q)] && f[static_cast<size_t>(q) * 4u] >= 200) {
						seen[static_cast<size_t>(q)] = 1;
						stack.push_back(q);
					}
				}
			}
		}
	}
	return components;
}

// White bar `x0 .. x1 - 1` on black, at kWobW x kWobH.
Frame makeBarSource(int w, int h, int x0, int x1) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const unsigned char v = (x >= x0 && x < x1) ? 255 : 0;
			f[i] = f[i + 1] = f[i + 2] = v;
			f[i + 3] = 255;
		}
	}
	return f;
}

// Number of runs of lit pixels (red >= threshold) along one row.
int litRuns(const Frame& f, int w, int y, unsigned char threshold) {
	int runs = 0;
	bool inRun = false;
	for (int x = 0; x < w; ++x) {
		const bool lit = f[(static_cast<size_t>(y) * w + x) * 4u] >= threshold;
		if (lit && !inRun) {
			++runs;
		}
		inRun = lit;
	}
	return runs;
}

// A displaced bar is one continuous run along every scanline: the beam paints a scanline as one
// stroke, so no speckle appears inside it. The dark gaps between scanlines are expected, because
// the spot covers only part of each line pitch. Checked at 1 kHz and 5 kHz.
void testDeflectionYokeNoGaps() {
	std::printf("deflection yoke: each scanline is one continuous stroke, with gaps between lines\n");
	const double pi = 3.14159265358979323846;
	const Frame bar = makeBarSource(kWobW, kWobH, 196, 204);
	const int slot = signalRingAllocate();
	VideoGraphRenderer renderer;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	const double pitch = static_cast<double>(kWobH) / kWobLines;
	for (double f : {1000.0, 5000.0}) {
		pushScanSignal(slot, 6, [&](int n, float* v) {
			v[kDeflectionYokeDeflectX] = static_cast<float>(0.25 * std::sin(2.0 * pi * f * n / kWobRate));
			v[kDeflectionYokeDeflectY] = 0.f;
			v[kDeflectionYokeZoom] = 0.f;
		});
		const Frame out = renderWobble(renderer, bar, slot, p);
		int oneRun = 0;
		int empty = 0;
		double centre = 0.0;
		double gap = 0.0;
		for (int k = 0; k < kWobLines; ++k) {
			const int row = static_cast<int>(std::lround((k + 0.5) * pitch));
			const int runs = litRuns(out, kWobW, row, 40);
			oneRun += runs == 1 ? 1 : 0;
			empty += runs == 0 ? 1 : 0;
			const int boundary = static_cast<int>(std::lround((k + 1.0) * pitch));
			if (boundary < kWobH) {
				for (int x = 0; x < kWobW; ++x) {
					centre += out[(static_cast<size_t>(row) * kWobW + x) * 4u];
					gap += out[(static_cast<size_t>(boundary) * kWobW + x) * 4u];
				}
			}
		}
		std::printf("  X at %.0f Hz: %d of %d scanlines are one run, %d empty, gap/centre light %.2f\n", f, oneRun, kWobLines, empty, gap / std::max(centre, 1.0));
		check(oneRun >= kWobLines * 95 / 100, "each scanline is one continuous run (no speckle within a stroke)");
		check(gap < 0.5 * centre, "dark gaps lie between the scanlines");
		char name[64];
		std::snprintf(name, sizeof(name), "wobble_line_connected_%.0fhz.png", f);
		saveWobbleProof(name, out);
	}
	signalRingRelease(slot);
}

// Number of pixels in one column that are not black, counted over every row.
bool columnIsBlack(const Frame& f, int w, int h, int x) {
	for (int y = 0; y < h; ++y) {
		const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
		if (f[i] != 0 || f[i + 1] != 0 || f[i + 2] != 0) {
			return false;
		}
	}
	return true;
}

// A DC shift of a quarter width moves the picture right, so the left quarter of the screen has no
// source. Clamp fills it with the edge colour, wrap tiles the picture into it, and black leaves it dark.
void testDeflectionYokeEdges() {
	std::printf("deflection yoke: edge modes\n");
	const Frame gray = [] {
		Frame f(static_cast<size_t>(kWobW) * kWobH * 4u, 128);
		for (size_t i = 3; i < f.size(); i += 4) {
			f[i] = 255;
		}
		return f;
	}();
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 6, [&](int, float* v) {
		v[kDeflectionYokeDeflectX] = 0.25f;
		v[kDeflectionYokeDeflectY] = 0.f;
		v[kDeflectionYokeZoom] = 0.f;
	});
	VideoGraphRenderer renderer;
	DeflectionYokeParams p;
	p.depth = 1.f;

	auto blackColumns = [](const Frame& f) {
		int count = 0;
		for (int x = 0; x < kWobW; ++x) {
			count += columnIsBlack(f, kWobW, kWobH, x) ? 1 : 0;
		}
		return count;
	};

	p.edge = static_cast<int>(DeflectionYokeEdge::Clamp);
	const Frame clamped = renderWobble(renderer, gray, slot, p);
	check(blackColumns(clamped) == 0, "clamp edge leaves no black columns");

	p.edge = static_cast<int>(DeflectionYokeEdge::Wrap);
	const Frame wrapped = renderWobble(renderer, gray, slot, p);
	check(blackColumns(wrapped) == 0, "wrap edge leaves no black columns");

	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	const Frame blacked = renderWobble(renderer, gray, slot, p);
	std::printf("  black edge: %d black columns of %d\n", blackColumns(blacked), kWobW);
	check(blackColumns(blacked) >= kWobW / 5, "black edge leaves the uncovered quarter dark");
	saveWobbleProof("wobble_edge_black.png", blacked);
	saveWobbleProof("wobble_edge_clamp.png", clamped);
	saveWobbleProof("wobble_edge_wrap.png", wrapped);
	signalRingRelease(slot);
}


// ---- DeflectionYoke rework: rates, waveforms, S coil, sizes (src/deflection_yoke_math.hpp) ----
namespace wb = deflection_yoke;

bool within(double a, double b, double tol) {
	return std::fabs(a - b) <= tol;
}

// A white square of side 2 * half, centred on the pixel boundary (cx, cy), on black.
Frame makeDotSource(int w, int h, int cx, int cy, int half) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const bool inside = x >= cx - half && x < cx + half && y >= cy - half && y < cy + half;
			const unsigned char v = inside ? 255 : 0;
			f[i] = f[i + 1] = f[i + 2] = v;
			f[i + 3] = 255;
		}
	}
	return f;
}

Frame makeFlatSource(int w, int h, unsigned char level) {
	Frame f(static_cast<size_t>(w) * h * 4u, level);
	for (size_t i = 3; i < f.size(); i += 4) {
		f[i] = 255;
	}
	return f;
}

// Red on the left (or top) half, blue on the right (or bottom) half.
Frame makeSplitSource(int w, int h, bool leftRight) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const bool first = leftRight ? x < w / 2 : y < h / 2;
			f[i] = first ? 255 : 0;
			f[i + 1] = 0;
			f[i + 2] = first ? 0 : 255;
			f[i + 3] = 255;
		}
	}
	return f;
}

// Luma-weighted centroid of a frame, in pixel-index units, and its total light.
struct Blob {
	double x = 0.0;
	double y = 0.0;
	double sum = 0.0;
};

Blob blobOf(const Frame& f, int w, int h) {
	Blob b;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			const double luma = (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
			b.sum += luma;
			b.x += luma * x;
			b.y += luma * y;
		}
	}
	if (b.sum > 0.0) {
		b.x /= b.sum;
		b.y /= b.sum;
	}
	else {
		b.x = b.y = std::nan("");
	}
	return b;
}

// Mean luma (0..1) in [x0, x1) x [y0, y1).
double blockMean(const Frame& f, int w, int x0, int y0, int x1, int y1) {
	double total = 0.0;
	for (int y = y0; y < y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			total += (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
		}
	}
	return total / static_cast<double>((x1 - x0) * (y1 - y0));
}

// Largest value of one colour channel (0 red, 1 green, 2 blue) in [x0, x1) x [y0, y1).
int peakChannel(const Frame& f, int w, int channel, int x0, int y0, int x1, int y1) {
	int peak = 0;
	for (int y = y0; y < y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			peak = std::max(peak, static_cast<int>(f[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u + static_cast<size_t>(channel)]));
		}
	}
	return peak;
}

// First and last row (or column) that holds a pixel whose brightest channel exceeds `threshold`. -1 if none.
struct Extent {
	int first = -1;
	int last = -1;
};

Extent litRows(const Frame& f, int w, int h, int threshold) {
	Extent e;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			if (std::max({f[i], f[i + 1], f[i + 2]}) > threshold) {
				if (e.first < 0) {
					e.first = y;
				}
				e.last = y;
				break;
			}
		}
	}
	return e;
}

Extent litColumns(const Frame& f, int w, int h, int threshold) {
	Extent e;
	for (int x = 0; x < w; ++x) {
		for (int y = 0; y < h; ++y) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			if (std::max({f[i], f[i + 1], f[i + 2]}) > threshold) {
				if (e.first < 0) {
					e.first = x;
				}
				e.last = x;
				break;
			}
		}
	}
	return e;
}

// Pushes `count` samples numbered first .. first + count - 1. gen(n, out) fills the four channels.
void pushScanRange(int slot, int first, int count, const std::function<void(int, float*)>& gen) {
	signalRingSetSampleRate(slot, kWobRate);
	for (int n = first; n < first + count; ++n) {
		float values[kSignalRingChannels] = {0.f, 0.f, 0.f, 0.f};
		gen(n, values);
		signalRingPush(slot, values);
	}
}

// Six frames of a constant signal on a fresh slot, then one render. dx and dy are in raster widths and
// heights, zoom is the zoom term, s is the S coil signal (angle = s * kSCoilRadians).
Frame renderConstant(VideoGraphRenderer& r, const Frame& source, const DeflectionYokeParams& params, float dx, float dy, float zoom, float s) {
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 6, [=](int, float* v) {
		v[kDeflectionYokeDeflectX] = dx;
		v[kDeflectionYokeDeflectY] = dy;
		v[kDeflectionYokeZoom] = zoom;
		v[kDeflectionYokeSCoil] = s;
	});
	const Frame out = renderWobble(r, source, slot, params);
	signalRingRelease(slot);
	return out;
}

// One render, or `frames` renders in a row, on a renderer made for the call (no state from earlier renders).
Frame renderFresh(const Frame& source, int slot, const DeflectionYokeParams& params, int frames = 1) {
	VideoGraphRenderer r;
	Frame out;
	for (int i = 0; i < frames; ++i) {
		out = renderWobble(r, source, slot, params);
	}
	return out;
}

// Least-squares fit of p(t) = offset + a sin(omega t) + b cos(omega t). The phase is in cycles, the
// model being amplitude * sin(omega t + 2 pi phase) + offset.
struct SineFit {
	double phase = 0.0;
	double amplitude = 0.0;
	double offset = 0.0;
};

SineFit fitSine(const std::vector<double>& t, const std::vector<double>& p, double omega) {
	const double pi = 3.14159265358979323846;
	double m[3][4] = {};
	for (size_t i = 0; i < t.size(); ++i) {
		const double basis[3] = {1.0, std::sin(omega * t[i]), std::cos(omega * t[i])};
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				m[r][c] += basis[r] * basis[c];
			}
			m[r][3] += basis[r] * p[i];
		}
	}
	for (int col = 0; col < 3; ++col) {
		int pivot = col;
		for (int r = col + 1; r < 3; ++r) {
			if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) {
				pivot = r;
			}
		}
		for (int c = 0; c < 4; ++c) {
			std::swap(m[col][c], m[pivot][c]);
		}
		for (int r = col + 1; r < 3; ++r) {
			const double k = m[r][col] / m[col][col];
			for (int c = col; c < 4; ++c) {
				m[r][c] -= k * m[col][c];
			}
		}
	}
	double x[3] = {};
	for (int r = 2; r >= 0; --r) {
		double acc = m[r][3];
		for (int c = r + 1; c < 3; ++c) {
			acc -= m[r][c] * x[c];
		}
		x[r] = acc / m[r][r];
	}
	SineFit fit;
	fit.offset = x[0];
	fit.amplitude = std::hypot(x[1], x[2]);
	fit.phase = std::atan2(x[2], x[1]) / (2.0 * pi);
	return fit;
}

// RATE knob, waveforms and CV scalings: plain functions of numbers that the module and the renderer share.
void testDeflectionYokeMath() {
	std::printf("deflection yoke: RATE knob, waveforms and CV scalings\n");

	// RATE knob: 0.1 Hz at 0, 8 kHz at 1 (where it stopped before the rework), 20 kHz at the new top.
	check(within(wb::rateHzFromKnob(0.f), 0.1, 1e-6), "RATE knob at 0 is 0.1 Hz");
	check(within(wb::rateHzFromKnob(1.f), 8000.0, 0.1), "RATE knob at 1 is 8 kHz, where it stopped before");
	check(within(wb::rateHzFromKnob(wb::kRateTop), 20000.0, 5.0), "the new top of the RATE knob is 20 kHz");
	bool sameAsShared = true;
	for (int i = 0; i <= 100; ++i) {
		const float v = static_cast<float>(i) / 100.f;
		if (wb::rateHzFromKnob(v) != expHzFromNormalized(v, 0.1f, 8000.f)) {
			sameAsShared = false;
		}
	}
	check(sameAsShared, "over 0..1 the RATE knob plays exactly the Hz a saved patch always had");
	check(wb::rateHzFromKnob(5.f) == wb::rateHzFromKnob(wb::kRateTop), "RATE knob above the top stays at the top");
	check(wb::rateHzFromKnob(-1.f) == wb::rateHzFromKnob(0.f), "RATE knob below 0 stays at 0");
	check(within(wb::rateKnobFromHz(wb::rateHzFromKnob(0.37f)), 0.37, 1e-4), "Hz to knob and back is the identity");
	check(within(wb::rateKnobFromHz(60.f), 0.5666123, 1e-4), "60 Hz sits at 0.5666 on the knob");
	check(within(wb::rateKnobFromHz(1e9f), wb::kRateTop, 1e-5), "a huge Hz maps to the top of the knob");
	check(within(wb::oscillatorHz(0.f, 48000.f), 0.001, 1e-9), "the oscillator never runs below 1 mHz");
	check(within(wb::oscillatorHz(30000.f, 48000.f), 21600.0, 1e-3), "the oscillator stops at 0.45 of the sample rate");
	check(within(wb::oscillatorHz(440.f, 48000.f), 440.0, 1e-3), "a normal rate passes through");
	check(within(wb::oscillatorHz(30000.f, 192000.f), 30000.0, 1e-3), "a higher sample rate allows a higher rate");

	// Waveforms at the quarter points.
	const double quarter[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
	const double sine[5] = {0.0, 1.0, 0.0, -1.0, 0.0};
	const double triangle[5] = {0.0, 1.0, 0.0, -1.0, 0.0};
	const double square[5] = {1.0, 1.0, -1.0, -1.0, 1.0};
	bool shapesRight = true;
	for (int i = 0; i < 5; ++i) {
		shapesRight = shapesRight && within(wb::wave(wb::kWaveSine, quarter[i]), sine[i], 1e-5);
		shapesRight = shapesRight && within(wb::wave(wb::kWaveTriangle, quarter[i]), triangle[i], 1e-6);
		shapesRight = shapesRight && within(wb::wave(wb::kWaveSquare, quarter[i]), square[i], 1e-6);
		shapesRight = shapesRight && within(wb::wave(wb::kWaveDc, quarter[i]), 1.0, 1e-6);
	}
	check(shapesRight, "sine, triangle, square and DC have the right values at the quarter points");
	check(within(wb::wave(wb::kWaveTriangle, -0.25), -1.0, 1e-6), "a triangle at a negative phase wraps");
	check(within(wb::wave(wb::kWaveTriangle, 1000000.25), 1.0, 1e-6), "a triangle at a huge phase still reads the right point");
	check(within(wb::wave(wb::kWaveSine, 3.25), 1.0, 1e-4), "a sine at phase 3.25 is at its peak");
	check(within(wb::wave(7, 0.25), 1.0, 1e-5) && within(wb::wave(-1, 0.25), 1.0, 1e-5), "an unknown shape reads as a sine");
	bool bounded = true;
	for (int shape = 0; shape < 4; ++shape) {
		for (double ph = -3.0; ph <= 3.0; ph += 0.0137) {
			bounded = bounded && std::fabs(wb::wave(shape, ph)) <= 1.0000001;
		}
	}
	check(bounded, "every waveform stays within +/-1");

	// Control voltage scalings.
	check(within(wb::octaveRatio(1.f), 2.0, 1e-6) && within(wb::octaveRatio(-1.f), 0.5, 1e-6), "1 V/oct doubles and halves");
	check(within(wb::octaveRatio(11.f), 1024.0, 1e-3), "the V/OCT input is limited to +/-10 V");
	check(within(wb::fineRatio(0.f, 0.f), 1.0, 1e-6), "FINE at 0 is no change");
	check(within(wb::fineRatio(1.f, 0.f), std::pow(2.0, 1.0 / 24.0), 1e-6), "FINE at 1 is 50 cents");
	check(within(wb::fineRatio(0.f, 5.f), std::pow(2.0, 1.0 / 24.0), 1e-6), "5 V on FINE is the full 50 cents");
	check(within(wb::fineRatio(1.f, 5.f), std::pow(2.0, 1.0 / 24.0), 1e-6), "FINE and its CV together stop at 50 cents");
	check(within(wb::amountWithCv(0.5f, 5.f), 1.0, 1e-6) && within(wb::amountWithCv(0.f, -2.5f), -0.5, 1e-6), "AMOUNT CV is 0.2 per volt and stops at +/-1");
	check(within(wb::depthWithCv(0.5f, 5.f), 1.0, 1e-6) && within(wb::depthWithCv(0.5f, -5.f), 0.0, 1e-6), "DEPTH CV is 0.1 per volt and stays within 0..1");
	check(within(wb::phaseWithCv(0.1f, 5.f), 1.1, 1e-6), "5 V on a PHASE input is one cycle");
	check(within(wb::shapeWithCv(0.5f, 5.f), 1.5, 1e-6), "CURL and TWIST CV add 0.2 per volt");
	check(within(wb::shapeWithCv(0.5f, 100.f), 3.0, 1e-6) && within(wb::shapeWithCv(-0.5f, -100.f), -3.0, 1e-6), "CURL and TWIST stop at +/-3");
	check(within(wb::rangeWithCv(0.5f, 5.f, 0.1f, 0.6f), 0.6, 1e-6), "a range CV stops at the top of the range");
	check(within(wb::rangeWithCv(0.2f, 1.f, 0.1f, 0.6f), 0.3, 1e-6), "+/-5 V sweeps a range knob's whole range");
	check(within(wb::rangeWithCv(0.2f, -5.f, 0.1f, 0.6f), 0.1, 1e-6), "a range CV stops at the bottom of the range");
	check(within(wb::rangeWithCv(0.2f, 0.f, 0.1f, 0.6f), 0.2, 1e-6), "a range CV at 0 V leaves the knob alone");
	check(wb::linesWithCv(240.f, 5.f, 120, 600) == 480, "LINES CV: 5 V is one octave up");
	check(wb::linesWithCv(240.f, -5.f, 120, 600) == 120, "LINES CV: -5 V is one octave down");
	check(wb::linesWithCv(240.f, 10.f, 120, 600) == 600, "LINES CV stops at the maximum");
	check(wb::linesWithCv(240.f, 0.f, 120, 600) == 240, "LINES CV at 0 V leaves the knob alone");
	check(wb::linesWithCv(200.f, 2.5f, 120, 600) == 283, "LINES CV rounds to a whole line");
	check(within(wb::coilCutoffWithCv(1000.f, 1.f, 48000.f), 2000.0, 1e-3), "COIL cutoff follows 1 V/oct");
	check(within(wb::coilCutoffWithCv(1000.f, 10.f, 48000.f), 21600.0, 1e-3), "COIL cutoff stops at 0.45 of the sample rate");
	check(within(wb::coilCutoffWithCv(5.f, 0.f, 48000.f), 10.0, 1e-3), "COIL cutoff stops at 10 Hz");
	check(wb::flipWithGate(false, 2.f) && !wb::flipWithGate(true, 2.f), "a high gate inverts the FLIP switch");
	check(wb::flipWithGate(true, 0.5f) && !wb::flipWithGate(false, 1.f), "a gate at or below 1 V does nothing");
}

// The ring carries a fourth channel (the S coil), and the window it hands the renderer is aligned to the
// global sample count, so a signal keeps its phase against the raster from one field to the next.
void testSignalRingFourthChannel() {
	std::printf("signal ring: the fourth channel and window alignment\n");
	check(kSignalRingChannels == 4 && kDeflectionYokeSCoil == 3, "the ring carries four channels and the S coil is the fourth");

	{
		auto ringPtr = std::make_unique<SignalRing>();
		SignalRing& ring = *ringPtr;
		ring.setSampleRate(48000.f);
		for (int n = 0; n < 4000; ++n) {
			const float values[kSignalRingChannels] = {static_cast<float>(n), -static_cast<float>(n), 0.5f, 0.001f * static_cast<float>(n)};
			ring.push(values);
		}
		SignalWindow w;
		check(ring.readFrame(w, 1.0 / 60.0) && w.channels == 4 && w.framePeriodSamples == 800, "a 60 Hz window has four channels and 800 samples per frame");
		check(within(w.sampleAt(0, 7), 2407.0, 1e-3), "the window starts on a frame boundary (sample 2400)");
		check(within(w.sampleAt(3, 10.25), 2.41025, 1e-3), "the fourth channel is read back with the same alignment");
		check(within(w.sampleAt(2, 400.0), 0.5, 1e-6), "the other channels are untouched by the fourth");
		SignalWindow latest;
		check(ring.readLatest(100, latest) && within(latest.sampleAt(3, 99), 3.999, 1e-3), "readLatest returns the fourth channel too");
	}

	{
		// A field window is S = round(rate * fieldSeconds(240)) = 801 samples and is aligned to multiples of S
		// in the ring's own sample count. Samples pushed after the newest boundary do not move it.
		auto ringPtr = std::make_unique<SignalRing>();
		SignalRing& ring = *ringPtr;
		ring.setSampleRate(48000.f);
		const double period = crt::fieldSeconds(kWobLines);
		const int S = static_cast<int>(std::lround(48000.0 * period));
		check(S == 801, "a 240-line field is 801 samples at 48 kHz");
		auto pushUpTo = [&](int& pushed, int target) {
			while (pushed < target) {
				const float values[kSignalRingChannels] = {static_cast<float>(pushed), 0.f, 0.f, 0.f};
				ring.push(values);
				++pushed;
			}
		};
		int pushed = 0;
		pushUpTo(pushed, 6 * S + 35);
		SignalWindow w;
		check(ring.readFrame(w, period) && w.framePeriodSamples == S, "a field window is read");
		check(within(w.sampleAt(0, 0), 5.0 * S, 1e-3), "the window starts on the newest field boundary");
		pushUpTo(pushed, 6 * S + 135);
		check(ring.readFrame(w, period) && within(w.sampleAt(0, 0), 5.0 * S, 1e-3), "more samples within the field leave the window where it was");
		pushUpTo(pushed, 7 * S + 135);
		check(ring.readFrame(w, period) && within(w.sampleAt(0, 0), 6.0 * S, 1e-3), "one more field of samples moves the window by exactly S");
	}
}

// The S coil turns the raster's total deflection about the screen centre by (signal x kSCoilRadians).
// A white square is rendered at three places for three angles, and its centroid has to land where a
// rotation of its resting position puts it. A positive angle turns the picture clockwise on screen.
void testDeflectionYokeSCoil() {
	std::printf("deflection yoke: the S coil rotates the raster about the screen centre\n");
	const double cx = kWobW / 2.0;
	const double cy = kWobH / 2.0;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	const int offsets[3][2] = {{100, 0}, {0, 100}, {-80, -120}};
	const double angles[3] = {0.5, -0.5, 1.2};
	double worstPos = 0.0;
	double worstLight = 0.0;
	for (const auto& off : offsets) {
		VideoGraphRenderer renderer;
		const Frame dot = makeDotSource(kWobW, kWobH, kWobW / 2 + off[0], kWobH / 2 + off[1], 12);
		const Blob rest = blobOf(renderConstant(renderer, dot, p, 0.f, 0.f, 0.f, 0.f), kWobW, kWobH);
		std::printf("  dot at offset %d, %d:", off[0], off[1]);
		for (double angle : angles) {
			const Frame out = renderConstant(renderer, dot, p, 0.f, 0.f, 0.f, static_cast<float>(angle / wb::kSCoilRadians));
			const Blob got = blobOf(out, kWobW, kWobH);
			const double rx = rest.x - cx;
			const double ry = rest.y - cy;
			const double ex = cx + rx * std::cos(angle) - ry * std::sin(angle);
			const double ey = cy + rx * std::sin(angle) + ry * std::cos(angle);
			worstPos = std::max(worstPos, std::hypot(got.x - ex, got.y - ey));
			worstLight = std::max(worstLight, std::fabs(got.sum / rest.sum - 1.0));
			std::printf("  %+.1f rad: error %.2f px, light %+.1f %%", angle, std::hypot(got.x - ex, got.y - ey), 100.0 * (got.sum / rest.sum - 1.0));
		}
		std::printf("\n");
	}
	std::printf("  worst centroid error %.2f px, worst change in total light %.1f %%\n", worstPos, 100.0 * worstLight);
	check(worstPos < 1.5, "a dot lands where a rotation about the screen centre puts it");
	check(worstLight < 0.15, "a turned raster carries the same light");

	// Stills: a fixed turn each way, and a 60 Hz sine that turns every scanline by a different amount.
	{
		const Frame grid = makeGridSource(kWobW, kWobH);
		VideoGraphRenderer renderer;
		DeflectionYokeParams g;
		g.depth = 1.f;
		saveWobbleProof("wobble_s_dc_pos.png", renderConstant(renderer, grid, g, 0.f, 0.f, 0.f, 0.5f));
		saveWobbleProof("wobble_s_dc_neg.png", renderConstant(renderer, grid, g, 0.f, 0.f, 0.f, -0.5f));
		const double pi = 3.14159265358979323846;
		const int slot = signalRingAllocate();
		pushScanSignal(slot, 6, [&](int n, float* v) {
			v[kDeflectionYokeSCoil] = static_cast<float>(std::sin(2.0 * pi * 60.0 * n / kWobRate));
		});
		saveWobbleProof("wobble_s_curve_60hz.png", renderWobble(renderer, grid, slot, g));
		signalRingRelease(slot);
	}

	// A signal too small to move a pixel draws the frame without the coil, and the step to a drawn
	// rotation is not a jump.
	{
		const Frame grid = makeGridSource(kWobW, kWobH);
		VideoGraphRenderer renderer;
		DeflectionYokeParams g;
		g.depth = 1.f;
		auto at = [&](float s) { return renderConstant(renderer, grid, g, 0.f, 0.f, 0.f, s); };
		const Frame off = at(0.f);
		check(at(1e-6f) == off && at(5e-6f) == off, "an S coil signal under 1e-5 draws exactly the frame without it");
		const Frame below = at(9e-6f);
		const Frame above = at(1.1e-5f);
		const Frame f50 = at(5e-5f);
		const Frame f500 = at(5e-4f);
		const Frame f5000 = at(5e-3f);
		std::printf("  tiny S: 1.1e-5 vs none max %d; 9e-6 vs 1.1e-5 max %d; mean diff 5e-5 %.4f, 5e-4 %.4f, 5e-3 %.4f\n", maxDiff(off, above), maxDiff(below, above), meanDiff(off, f50), meanDiff(off, f500), meanDiff(off, f5000));
		check(maxDiff(below, above) <= 3, "stepping over the S coil threshold changes the picture by almost nothing");
		check(meanDiff(off, f50) <= meanDiff(off, f500) && meanDiff(off, f500) <= meanDiff(off, f5000), "a larger S signal turns the picture more");
	}
}

// SIZE shrinks the normal scan about the screen centre. The coils (their own yokes) keep full strength.
void testDeflectionYokeSize() {
	std::printf("deflection yoke: H SIZE and V SIZE shrink the normal scan, not the coils\n");
	const double cx = kWobW / 2.0;
	const double cy = kWobH / 2.0;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	VideoGraphRenderer renderer;
	const Frame dot = makeDotSource(kWobW, kWobH, kWobW / 2 + 120, kWobH / 2 + 100, 12);
	const Blob rest = blobOf(renderConstant(renderer, dot, p, 0.f, 0.f, 0.f, 0.f), kWobW, kWobH);
	auto at = [&](const Frame& source, float h, float v, float dx, float dy) {
		DeflectionYokeParams q = p;
		q.hSize = h;
		q.vSize = v;
		return blobOf(renderConstant(renderer, source, q, dx, dy, 0.f, 0.f), kWobW, kWobH);
	};
	double worst = 0.0;
	{
		const Blob b = at(dot, 0.5f, 1.f, 0.f, 0.f);
		worst = std::max(worst, std::hypot(b.x - (cx + 0.5 * (rest.x - cx)), b.y - rest.y));
	}
	{
		const Blob b = at(dot, 1.f, 0.25f, 0.f, 0.f);
		worst = std::max(worst, std::hypot(b.x - rest.x, b.y - (cy + 0.25 * (rest.y - cy))));
	}
	{
		const Blob b = at(dot, 0.5f, 0.25f, 0.f, 0.f);
		worst = std::max(worst, std::hypot(b.x - (cx + 0.5 * (rest.x - cx)), b.y - (cy + 0.25 * (rest.y - cy))));
	}
	std::printf("  worst centroid error %.2f px (dot rests at %.1f, %.1f)\n", worst, rest.x, rest.y);
	check(worst < 1.5, "SIZE pulls the picture toward the screen centre by that fraction, on each axis");

	// The coils keep full strength. A dot at the centre moves by the coil deflection whatever the SIZE.
	{
		const Frame centre = makeDotSource(kWobW, kWobH, kWobW / 2, kWobH / 2, 12);
		const Blob home = blobOf(renderConstant(renderer, centre, p, 0.f, 0.f, 0.f, 0.f), kWobW, kWobH);
		const double wantX = 0.1 * kWobW;
		const double wantY = 0.05 * kWobH;
		double worstCoil = 0.0;
		for (float size : {1.f, 0.5f}) {
			const Blob b = at(centre, size, size, 0.1f, 0.05f);
			worstCoil = std::max(worstCoil, std::hypot(b.x - home.x - wantX, b.y - home.y - wantY));
		}
		const Blob sum = at(dot, 0.5f, 1.f, 0.1f, 0.f);
		const double both = std::hypot(sum.x - (cx + 0.5 * (rest.x - cx) + wantX), sum.y - rest.y);
		std::printf("  coil at SIZE 1 and 0.5: worst error %.2f px; SIZE and coil together %.2f px\n", worstCoil, both);
		check(worstCoil < 1.5, "the coils move the picture by the same amount at any SIZE");
		check(both < 1.5, "the scaled scan and the coil deflection add");
	}
}

// SIZE 0 on one axis folds the raster onto a line, on both onto a point. The light is kept (the
// collapsed raster is not a few dim pixels) and the colours stay where they were on the line.
void testDeflectionYokeCollapse() {
	std::printf("deflection yoke: SIZE 0 collapses the raster onto a line or a point\n");
	const int cx = kWobW / 2;
	const int cy = kWobH / 2;
	const int lit = 13;  // 5 % of full scale
	DeflectionYokeParams p;
	p.depth = 1.f;
	VideoGraphRenderer renderer;

	{
		// V SIZE 0: every scanline lies on the centre row. The source is red on the left, blue on the right.
		p.hSize = 1.f;
		p.vSize = 0.f;
		const Frame out = renderConstant(renderer, makeSplitSource(kWobW, kWobH, true), p, 0.f, 0.f, 0.f, 0.f);
		const Extent rows = litRows(out, kWobW, kWobH, lit);
		const Extent cols = litColumns(out, kWobW, kWobH, lit);
		const int redLeft = peakChannel(out, kWobW, 0, kWobW / 4 - 2, cy - 4, kWobW / 4 + 2, cy + 5);
		const int blueLeft = peakChannel(out, kWobW, 2, kWobW / 4 - 2, cy - 4, kWobW / 4 + 2, cy + 5);
		const int redRight = peakChannel(out, kWobW, 0, 3 * kWobW / 4 - 2, cy - 4, 3 * kWobW / 4 + 2, cy + 5);
		const int blueRight = peakChannel(out, kWobW, 2, 3 * kWobW / 4 - 2, cy - 4, 3 * kWobW / 4 + 2, cy + 5);
		std::printf("  V SIZE 0: lit rows %d..%d, lit columns %d..%d; left R %d B %d, right R %d B %d\n", rows.first, rows.last, cols.first, cols.last, redLeft, blueLeft, redRight, blueRight);
		check(rows.first >= cy - 6 && rows.last <= cy + 6 && rows.first >= 0, "V SIZE 0 lights a narrow band across the middle");
		check(cols.first <= 2 && cols.last >= kWobW - 3, "the line reaches both sides of the screen");
		check(redLeft > 200 && blueLeft < 40 && blueRight > 200 && redRight < 40, "the line keeps the colours of the picture, red left and blue right");
		saveWobbleProof("wobble_collapse_v.png", out);
	}
	{
		// H SIZE 0: every cell of a scanline lies on the centre column. Red on top, blue below.
		p.hSize = 0.f;
		p.vSize = 1.f;
		const Frame out = renderConstant(renderer, makeSplitSource(kWobW, kWobH, false), p, 0.f, 0.f, 0.f, 0.f);
		const Extent rows = litRows(out, kWobW, kWobH, lit);
		const Extent cols = litColumns(out, kWobW, kWobH, lit);
		const int redTop = peakChannel(out, kWobW, 0, cx - 4, kWobH / 4 - 3, cx + 5, kWobH / 4 + 4);
		const int blueTop = peakChannel(out, kWobW, 2, cx - 4, kWobH / 4 - 3, cx + 5, kWobH / 4 + 4);
		const int redBottom = peakChannel(out, kWobW, 0, cx - 4, 3 * kWobH / 4 - 3, cx + 5, 3 * kWobH / 4 + 4);
		const int blueBottom = peakChannel(out, kWobW, 2, cx - 4, 3 * kWobH / 4 - 3, cx + 5, 3 * kWobH / 4 + 4);
		std::printf("  H SIZE 0: lit columns %d..%d, lit rows %d..%d; top R %d B %d, bottom R %d B %d\n", cols.first, cols.last, rows.first, rows.last, redTop, blueTop, redBottom, blueBottom);
		check(cols.first >= cx - 6 && cols.last <= cx + 6 && cols.first >= 0, "H SIZE 0 lights a narrow band down the middle");
		check(rows.first <= 4 && rows.last >= kWobH - 5, "the line reaches the top and the bottom of the screen");
		check(redTop > 200 && blueTop < 40 && blueBottom > 200 && redBottom < 40, "the line keeps the colours of the picture, red on top and blue below");
		saveWobbleProof("wobble_collapse_h.png", out);
	}
	{
		// Both 0: one bright point at the centre.
		p.hSize = 0.f;
		p.vSize = 0.f;
		const Frame out = renderConstant(renderer, makeFlatSource(kWobW, kWobH, 128), p, 0.f, 0.f, 0.f, 0.f);
		const Blob b = blobOf(out, kWobW, kWobH);
		double peak = 0.0;
		double outside = 0.0;
		int bright = 0;
		for (int y = 0; y < kWobH; ++y) {
			for (int x = 0; x < kWobW; ++x) {
				const size_t i = (static_cast<size_t>(y) * kWobW + static_cast<size_t>(x)) * 4u;
				const double luma = (out[i] * 0.2126 + out[i + 1] * 0.7152 + out[i + 2] * 0.0722) / 255.0;
				if (std::hypot(x - cx, y - cy) <= 10.0) {
					peak = std::max(peak, luma);
					bright += luma > 0.5 ? 1 : 0;
				} else {
					outside = std::max(outside, luma);
				}
			}
		}
		std::printf("  both 0: centroid %.2f, %.2f; peak luma %.2f, %d pixels above 0.5; brightest pixel beyond 10 px %.3f\n", b.x, b.y, peak, bright, outside);
		check(std::fabs(b.x - cx) < 2.0 && std::fabs(b.y - cy) < 2.0, "both SIZEs at 0 draw a point at the screen centre");
		check(peak >= 0.9, "the point is bright, not a few dim pixels");
		check(outside < 0.05, "nothing is lit away from the point");
		saveWobbleProof("wobble_collapse_point.png", out);
	}
	{
		// Near 1 the picture changes smoothly: the centre of a flat picture keeps its brightness.
		const Frame flat = makeFlatSource(kWobW, kWobH, 128);
		p.vSize = 1.f;
		p.hSize = 1.f;
		const double full = blockMean(renderConstant(renderer, flat, p, 0.f, 0.f, 0.f, 0.f), kWobW, cx - 8, cy - 8, cx + 8, cy + 8);
		double step[3] = {};
		const float sizes[3] = {0.999f, 0.98f, 0.9f};
		for (int i = 0; i < 3; ++i) {
			p.hSize = p.vSize = sizes[i];
			step[i] = std::fabs(blockMean(renderConstant(renderer, flat, p, 0.f, 0.f, 0.f, 0.f), kWobW, cx - 8, cy - 8, cx + 8, cy + 8) - full);
		}
		std::printf("  centre brightness %.3f at SIZE 1; change at 0.999, 0.98, 0.9: %.4f, %.4f, %.4f\n", full, step[0], step[1], step[2]);
		check(step[1] < 0.03, "a SIZE of 0.98 keeps the brightness of the middle of the picture");
		check(step[2] < 0.10, "a SIZE of 0.9 keeps the brightness of the middle of the picture");
	}
}

// Row-by-row centre of the bright line, but only on rows that hold a stroke: a row must carry at least
// `keep` of the light of the brightest row, so the faint tails between strokes do not add noise.
std::vector<double> strongCentroids(const Frame& f, int w, int h, double keep) {
	std::vector<double> c = lineCentroids(f, w, h);
	std::vector<double> sums(static_cast<size_t>(h), 0.0);
	double top = 0.0;
	for (int y = 0; y < h; ++y) {
		double sum = 0.0;
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			sum += (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
		}
		sums[static_cast<size_t>(y)] = sum;
		top = std::max(top, sum);
	}
	for (int y = 0; y < h; ++y) {
		if (!(sums[static_cast<size_t>(y)] >= keep * top)) {
			c[static_cast<size_t>(y)] = std::nan("");
		}
	}
	return c;
}

// A flat picture shows any gap in the coverage of a turned raster as a dark patch. With the Clamp edge the
// corners of the screen have to be painted by the padded strokes however far the raster turns.
void testDeflectionYokeClampRotation() {
	std::printf("deflection yoke: a turned raster still fills the screen on the Clamp edge\n");
	const int cx = kWobW / 2;
	const int cy = kWobH / 2;
	const Frame flat = makeFlatSource(kWobW, kWobH, 200);
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Clamp);
	double worstCorner = 1e9;
	double worstBlock = 1e9;
	for (double angle : {-1.2, -0.8, -0.3, 0.3, 0.8, 1.2}) {
		VideoGraphRenderer renderer;
		const Frame out = renderConstant(renderer, flat, p, 0.f, 0.f, 0.f, static_cast<float>(angle / wb::kSCoilRadians));
		const double centre = blockMean(out, kWobW, cx - 8, cy - 8, cx + 8, cy + 8);
		double corner = 1e9;
		corner = std::min(corner, blockMean(out, kWobW, 0, 0, 16, 16));
		corner = std::min(corner, blockMean(out, kWobW, kWobW - 16, 0, kWobW, 16));
		corner = std::min(corner, blockMean(out, kWobW, 0, kWobH - 16, 16, kWobH));
		corner = std::min(corner, blockMean(out, kWobW, kWobW - 16, kWobH - 16, kWobW, kWobH));
		double block = 1e9;
		int worstX = 0;
		int worstY = 0;
		for (int y = 0; y + 32 <= kWobH; y += 32) {
			for (int x = 0; x + 32 <= kWobW; x += 32) {
				const double m = blockMean(out, kWobW, x, y, x + 32, y + 32);
				if (m < block) {
					block = m;
					worstX = x;
					worstY = y;
				}
			}
		}
		std::printf("  %+.1f rad: centre luma %.3f; darkest 16 px corner %.2f of it; darkest 32 px block %.2f of it (at %d, %d)\n", angle, centre, corner / centre, block / centre, worstX, worstY);
		worstCorner = std::min(worstCorner, corner / centre);
		worstBlock = std::min(worstBlock, block / centre);
	}
	check(worstCorner >= 0.9, "the corners of a turned screen are painted, not left dark");
	check(worstBlock >= 0.9, "no 32 px patch of a turned screen is left dark");

	const Frame grid = makeGridSource(kWobW, kWobH);
	VideoGraphRenderer renderer;
	DeflectionYokeParams g = p;
	saveWobbleProof("wobble_clamp_rot_pos.png", renderConstant(renderer, grid, g, 0.f, 0.f, 0.f, static_cast<float>(1.2 / wb::kSCoilRadians)));
	saveWobbleProof("wobble_clamp_rot_neg.png", renderConstant(renderer, grid, g, 0.f, 0.f, 0.f, static_cast<float>(-1.2 / wb::kSCoilRadians)));
}

// LINES changes how many scanlines make a field, and with it how many ripples an audio-rate signal shows.
void testDeflectionYokeLines() {
	std::printf("deflection yoke: LINES sets the scanline count and the field it spans\n");
	const double pi = 3.14159265358979323846;
	const Frame line = makeLineSource(kWobW, kWobH, kWobW / 2);
	const int lineCounts[] = {120, 240, 600};
	const double freqs[] = {200.0, 1000.0};
	for (int lines : lineCounts) {
		for (double f : freqs) {
			const int slot = signalRingAllocate();
			pushScanSignal(slot, 6, [&](int n, float* v) {
				v[kDeflectionYokeDeflectX] = static_cast<float>(0.25 * std::sin(2.0 * pi * f * n / kWobRate));
			});
			DeflectionYokeParams p;
			p.depth = 1.f;
			p.lines = lines;
			VideoGraphRenderer renderer;
			const Frame out = renderWobble(renderer, line, slot, p);
			const int ripples = countRipples(lineCentroids(out, kWobW, kWobH));
			const double expected = f * visibleSeconds(lines);
			std::printf("  %d lines, X at %.0f Hz: %d ripples (expected %.2f)\n", lines, f, ripples, expected);
			check(std::fabs(ripples - expected) <= 1.5, "ripple count follows frequency times the visible field time");
			if (f == 1000.0) {
				char name[64];
				std::snprintf(name, sizeof(name), "wobble_lines_%d_1khz.png", lines);
				saveWobbleProof(name, out);
			}
			signalRingRelease(slot);
		}
	}

	// LINES changes from one frame to the next on a live ring: every frame is a real picture, the line
	// lands where the constant deflection puts it, and coming back to a count draws the same picture.
	{
		const int slot = signalRingAllocate();
		pushScanSignal(slot, 6, [](int, float* v) {
			v[kDeflectionYokeDeflectX] = 0.1f;
			v[kDeflectionYokeDeflectY] = 0.05f;
		});
		VideoGraphRenderer renderer;
		DeflectionYokeParams p;
		p.depth = 1.f;
		const int sequence[] = {240, 480, 120, 240};
		Frame first;
		double worstError = 0.0;
		double dimmest = 1e9;
		for (int i = 0; i < 4; ++i) {
			p.lines = sequence[i];
			const Frame out = renderWobble(renderer, line, slot, p);
			const Blob b = blobOf(out, kWobW, kWobH);
			std::printf("  LINES %d: centroid x %.2f (expected %.2f), light %.1f\n", p.lines, b.x, kWobW / 2.0 + 0.1 * kWobW, b.sum);
			worstError = std::max(worstError, std::fabs(b.x - (kWobW / 2.0 + 0.1 * kWobW)));
			dimmest = std::min(dimmest, b.sum);
			if (i == 0) {
				first = out;
			}
			else if (i == 3) {
				check(out == first, "going back to 240 lines draws exactly the first picture again");
			}
		}
		check(dimmest > 20.0, "every LINES change draws a lit picture");
		check(worstError < 1.5, "the line lands where the deflection puts it at every line count");
		signalRingRelease(slot);
	}
}

// The window the renderer reads is aligned to the field length S, so a signal keeps a fixed phase against
// the picture from one field to the next when its frequency is rate / S, and creeps at any other.
void testDeflectionYokeFieldRate() {
	std::printf("deflection yoke: the signal keeps its phase against the field\n");
	const double pi = 3.14159265358979323846;
	const int S = static_cast<int>(std::lround(kWobRate * crt::fieldSeconds(kWobLines)));
	const double pitch = static_cast<double>(kWobH) / kWobLines;
	const double samplesPerLine = static_cast<double>(S) / (kWobLines + crt::kBlankingLines);
	const Frame line = makeLineSource(kWobW, kWobH, kWobW / 2);
	DeflectionYokeParams p;
	p.depth = 1.f;

	const double hold = static_cast<double>(kWobRate) / S;
	const double cases[2] = {hold, 60.0};
	for (int c = 0; c < 2; ++c) {
		const double hz = cases[c];
		const double omega = 2.0 * pi * hz / kWobRate;
		const int slot = signalRingAllocate();
		VideoGraphRenderer renderer;
		auto gen = [&](int n, float* v) {
			v[kDeflectionYokeDeflectX] = static_cast<float>(0.25 * std::sin(2.0 * pi * hz * n / kWobRate));
		};
		int pushed = 6 * S + 35;
		pushScanRange(slot, 0, pushed, gen);
		double phases[5] = {};
		double worstAmplitude = 0.0;
		int smallestStep = 1 << 30;
		int largestStep = 0;
		Frame previous;
		for (int field = 0; field < 5; ++field) {
			const Frame out = renderWobble(renderer, line, slot, p);
			const std::vector<double> centres = strongCentroids(out, kWobW, kWobH, 0.5);
			std::vector<double> t;
			std::vector<double> v;
			for (int r = 0; r < kWobH; ++r) {
				if (std::isnan(centres[static_cast<size_t>(r)])) {
					continue;
				}
				const double k = std::floor((r + 0.5) / pitch);
				t.push_back((k + 0.5) * samplesPerLine);
				v.push_back(centres[static_cast<size_t>(r)]);
			}
			const SineFit fit = fitSine(t, v, omega);
			phases[field] = fit.phase;
			worstAmplitude = std::max(worstAmplitude, std::fabs(fit.amplitude - 0.25 * kWobW));
			if (field > 0) {
				smallestStep = std::min(smallestStep, maxDiff(previous, out));
				largestStep = std::max(largestStep, maxDiff(previous, out));
			}
			previous = out;
			pushScanRange(slot, pushed, S, gen);
			pushed += S;
		}
		double drift = 0.0;
		for (int field = 1; field < 5; ++field) {
			double d = phases[field] - phases[field - 1];
			d -= std::round(d);
			drift += d / 4.0;
		}
		double expected = hz * S / kWobRate;
		expected -= std::round(expected);
		std::printf("  %.3f Hz: phase moves %.5f cycles per field (expected %.5f), amplitude off by %.2f px, change between fields %d..%d\n", hz, drift, expected, worstAmplitude, smallestStep, largestStep);
		check(worstAmplitude < 4.0, "the fitted ripple has the amplitude that was put in");
		check(std::fabs(drift - expected) < 0.0004, "the phase moves per field by frequency x S / rate, modulo one cycle");
		if (c == 0) {
			check(largestStep <= 2, "a signal at rate / S holds the picture still from field to field");
		}
		else {
			check(smallestStep > 100, "a 60 Hz signal moves the picture a little each field");
		}
		signalRingRelease(slot);
	}

	// Samples pushed after the newest field boundary do not move the picture. It moves when S more samples
	// have gone by since the boundary, and not before.
	{
		const int slot = signalRingAllocate();
		VideoGraphRenderer renderer;
		auto gen = [&](int n, float* v) {
			v[kDeflectionYokeDeflectX] = static_cast<float>(0.25 * std::sin(2.0 * pi * 60.0 * n / kWobRate));
		};
		const double period = crt::fieldSeconds(kWobLines);
		auto windowData = [&]() {
			SignalWindow w;
			return signalRingReadFrame(slot, w, period) ? w.data : std::vector<float>();
		};
		int pushed = 6 * S + 35;
		pushScanRange(slot, 0, pushed, gen);
		const std::vector<float> before = windowData();
		std::vector<float> after;
		int crossing = -1;
		for (int i = 1; i <= S + 5 && crossing < 0; ++i) {
			pushScanRange(slot, pushed, 1, gen);
			++pushed;
			const std::vector<float> now = windowData();
			if (now != before) {
				crossing = i;
				after = now;
			}
		}
		check(crossing > 0 && crossing <= S, "the field window moves within one field of new samples");
		const Frame a = renderWobble(renderer, line, slot, p);
		pushScanRange(slot, pushed, 100, gen);
		pushed += 100;
		const Frame b = renderWobble(renderer, line, slot, p);
		check(windowData() == after && a == b, "100 more samples do not move the picture");
		pushScanRange(slot, pushed, S - 101, gen);
		pushed += S - 101;
		check(windowData() == after, "the window holds until S samples have passed the boundary");
		pushScanRange(slot, pushed, 1, gen);
		pushed += 1;
		const Frame cFrame = renderWobble(renderer, line, slot, p);
		check(windowData() != after && cFrame != a, "the S-th sample after the boundary moves the picture");
		std::printf("  window crossed %d samples after the first read, held for %d more, moved on the S-th\n", crossing, S - 1);
		signalRingRelease(slot);
	}
}

// Parameters and signals that cannot be right (NaN, infinity, out of range) are tamed to the nearest
// sane value, so the picture is the one that value draws. The worst signals render in bounded time.
void testDeflectionYokeExtremes() {
	std::printf("deflection yoke: nonsense parameters and signals are tamed\n");
	const double pi = 3.14159265358979323846;
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const Frame grid = makeGridSource(kWobW, kWobH);
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 12, [&](int n, float* v) {
		v[kDeflectionYokeDeflectX] = static_cast<float>(0.2 * std::sin(2.0 * pi * 300.0 * n / kWobRate));
		v[kDeflectionYokeDeflectY] = static_cast<float>(0.1 * std::cos(2.0 * pi * 300.0 * n / kWobRate));
		v[kDeflectionYokeZoom] = static_cast<float>(0.1 * std::sin(2.0 * pi * 30.0 * n / kWobRate));
		v[kDeflectionYokeSCoil] = static_cast<float>(0.15 * std::sin(2.0 * pi * 45.0 * n / kWobRate));
	});

	DeflectionYokeParams base;
	base.depth = 0.8f;
	base.twist = 0.5f;
	base.lineCurl = 0.3f;
	auto with = [&](auto&& edit) {
		DeflectionYokeParams q = base;
		edit(q);
		return q;
	};
	auto same = [&](const char* what, const DeflectionYokeParams& bad, const DeflectionYokeParams& good, int frames = 1) {
		check(renderFresh(grid, slot, bad, frames) == renderFresh(grid, slot, good, frames), what);
	};
	same("depth NaN draws as depth 0", with([&](DeflectionYokeParams& q) { q.depth = nan; }), with([](DeflectionYokeParams& q) { q.depth = 0.f; }));
	same("depth 7 draws as depth 1", with([](DeflectionYokeParams& q) { q.depth = 7.f; }), with([](DeflectionYokeParams& q) { q.depth = 1.f; }));
	same("twist +Inf draws as twist 0", with([&](DeflectionYokeParams& q) { q.twist = inf; }), with([](DeflectionYokeParams& q) { q.twist = 0.f; }));
	same("twist -Inf draws as twist 0", with([&](DeflectionYokeParams& q) { q.twist = -inf; }), with([](DeflectionYokeParams& q) { q.twist = 0.f; }));
	same("twist 1e30 draws as twist 3", with([](DeflectionYokeParams& q) { q.twist = 1e30f; }), with([](DeflectionYokeParams& q) { q.twist = 3.f; }));
	same("line curl NaN draws as curl 0", with([&](DeflectionYokeParams& q) { q.lineCurl = nan; }), with([](DeflectionYokeParams& q) { q.lineCurl = 0.f; }));
	same("line curl -1e30 draws as curl -3", with([](DeflectionYokeParams& q) { q.lineCurl = -1e30f; }), with([](DeflectionYokeParams& q) { q.lineCurl = -3.f; }));
	same("H SIZE NaN draws as 1", with([&](DeflectionYokeParams& q) { q.hSize = nan; }), with([](DeflectionYokeParams& q) { q.hSize = 1.f; }));
	same("V SIZE +Inf draws as 1", with([&](DeflectionYokeParams& q) { q.vSize = inf; }), with([](DeflectionYokeParams& q) { q.vSize = 1.f; }));
	same("H SIZE -4 draws as 0", with([](DeflectionYokeParams& q) { q.hSize = -4.f; }), with([](DeflectionYokeParams& q) { q.hSize = 0.f; }));
	same("V SIZE 9 draws as 1", with([](DeflectionYokeParams& q) { q.vSize = 9.f; }), with([](DeflectionYokeParams& q) { q.vSize = 1.f; }));
	same("spot NaN draws as 0.2", with([&](DeflectionYokeParams& q) { q.spot = nan; }), with([](DeflectionYokeParams& q) { q.spot = 0.2f; }));
	same("spot 0 draws as 0.05", with([](DeflectionYokeParams& q) { q.spot = 0.f; }), with([](DeflectionYokeParams& q) { q.spot = 0.05f; }));
	same("spot 100 draws as 2", with([](DeflectionYokeParams& q) { q.spot = 100.f; }), with([](DeflectionYokeParams& q) { q.spot = 2.f; }));
	same("persistence NaN draws as 0", with([&](DeflectionYokeParams& q) { q.persist = nan; }), with([](DeflectionYokeParams& q) { q.persist = 0.f; }), 2);
	same("persistence 5 draws as 1", with([](DeflectionYokeParams& q) { q.persist = 5.f; }), with([](DeflectionYokeParams& q) { q.persist = 1.f; }), 2);
	same("glow NaN draws as 0", with([&](DeflectionYokeParams& q) { q.glow = nan; }), with([](DeflectionYokeParams& q) { q.glow = 0.f; }));
	same("glow 99 draws as 2", with([](DeflectionYokeParams& q) { q.glow = 99.f; }), with([](DeflectionYokeParams& q) { q.glow = 2.f; }));
	same("LINES -7 draws as 120", with([](DeflectionYokeParams& q) { q.lines = -7; }), with([](DeflectionYokeParams& q) { q.lines = 120; }));
	same("LINES 99999 draws as 600", with([](DeflectionYokeParams& q) { q.lines = 99999; }), with([](DeflectionYokeParams& q) { q.lines = 600; }));
	same("edge 99 draws as Black", with([](DeflectionYokeParams& q) { q.edge = 99; }), with([](DeflectionYokeParams& q) { q.edge = static_cast<int>(DeflectionYokeEdge::Black); }));
	same("edge -5 draws as Clamp", with([](DeflectionYokeParams& q) { q.edge = -5; }), with([](DeflectionYokeParams& q) { q.edge = static_cast<int>(DeflectionYokeEdge::Clamp); }));

	// A signal slot that does not exist passes the source through.
	{
		VideoGraphRenderer renderer;
		for (int bad : {0, -1, 1000}) {
			check(maxDiff(renderWobble(renderer, grid, bad, base), grid) == 0, "a missing signal ring passes the source through");
		}
	}
	signalRingRelease(slot);

	// The same for the signal itself.
	{
		auto constSame = [&](const char* what, float ax, float ay, float az, float as, float bx, float by, float bz, float bs) {
			VideoGraphRenderer first;
			VideoGraphRenderer second;
			const Frame a = renderConstant(first, grid, base, ax, ay, az, as);
			const Frame b = renderConstant(second, grid, base, bx, by, bz, bs);
			check(a == b, what);
		};
		constSame("an all-NaN signal draws as silence", nan, nan, nan, nan, 0.f, 0.f, 0.f, 0.f);
		constSame("an infinite deflection draws as none", inf, -inf, inf, -inf, 0.f, 0.f, 0.f, 0.f);
		constSame("a huge signal is limited to 8", 1e30f, -1e30f, 1e30f, 1e30f, 8.f, -8.f, 8.f, 8.f);
	}

	// Time. The worst signal the renderer can be given: every channel flipping between +8 and -8 on every
	// sample, with the smallest raster, the most scanlines and the strongest curl and twist.
	{
		using Clock = std::chrono::steady_clock;
		const int nastySlot = signalRingAllocate();
		pushScanSignal(nastySlot, 12, [](int n, float* v) {
			for (int c = 0; c < kSignalRingChannels; ++c) {
				v[c] = (n & 1) != 0 ? 8.f : -8.f;
			}
		});
		const int reachableSlot = signalRingAllocate();
		pushScanSignal(reachableSlot, 12, [](int n, float* v) {
			const float s = (n & 1) != 0 ? 1.f : -1.f;
			v[kDeflectionYokeDeflectX] = s;
			v[kDeflectionYokeDeflectY] = -s;
			v[kDeflectionYokeZoom] = 0.5f * s;
			v[kDeflectionYokeSCoil] = 2.f * s;
		});
		DeflectionYokeParams p;
		p.depth = 1.f;
		p.twist = 3.f;
		p.lineCurl = 3.f;
		p.hSize = 0.5f;
		p.vSize = 0.5f;
		p.lines = 600;
		constexpr double kLimitSeconds = 15.0;
		double worst = 0.0;
		const char* const edgeNames[3] = {"Clamp", "Wrap", "Black"};
		for (int pass = 0; pass < 2; ++pass) {
			const int useSlot = pass == 0 ? nastySlot : reachableSlot;
			for (int edge = 0; edge < 3; ++edge) {
				p.edge = edge;
				// The limit is on wall-clock time and the renderer runs on up to 8 threads, so a machine busy with
				// other work (several builds at once) can push one frame over it on its own: a Wrap frame measured
				// 30 s in a load average of 37 and 1.6 s a minute later. A frame over the limit is timed again, up
				// to twice more, and the fastest run counts. A frame that really is unbounded stays over the limit.
				Frame out;
				double seconds = 0.0;
				int attempts = 0;
				do {
					VideoGraphRenderer renderer;
					const auto t0 = Clock::now();
					Frame frame = renderWobble(renderer, grid, useSlot, p);
					const double took = std::chrono::duration<double>(Clock::now() - t0).count();
					if (attempts == 0 || took < seconds) {
						seconds = took;
						out = std::move(frame);
					}
					++attempts;
				} while (seconds >= kLimitSeconds && attempts < 3);
				worst = std::max(worst, seconds);
				std::printf("  %s signal, %s edge, 600 lines: %.3f s per frame", pass == 0 ? "+/-8 on every channel" : "module worst case", edgeNames[edge], seconds);
				if (attempts > 1) {
					std::printf(" (fastest of %d runs)", attempts);
				}
				std::printf("\n");
				if (pass == 0 && edge == 1) {
					saveWobbleProof("wobble_extreme_wrap.png", out);
				}
			}
		}
		// Measured on the development Mac, otherwise idle: 0.6 to 1.6 s (wrap), 0.2 to 0.6 s (clamp, black), 0.01 to
		// 0.04 s for the module's own worst case.
		check(worst < kLimitSeconds, "the worst signals render in bounded time");
		signalRingRelease(nastySlot);
		signalRingRelease(reachableSlot);
	}
}

// ---- CRT beam layer (src/crt_beam.hpp) and the DeflectionYoke's beam physics ----

// Sum over all channels and the largest value of a field.
void fieldTotals(const crt::EnergyField& f, double& sum, float& peak) {
	sum = 0.0;
	peak = 0.f;
	const size_t n = static_cast<size_t>(f.width()) * static_cast<size_t>(f.height()) * 3u;
	for (size_t i = 0; i < n; ++i) {
		sum += f.data()[i];
		peak = std::max(peak, f.data()[i]);
	}
}

// Share of a field's adjacent-pixel steps that land on the busiest phase of a 4-pixel grid, for the
// first channel of a w x h x 3 field. 0.25 is an even spread. 1.00 means every step is on the grid.
double glowGridShare(const float* field, int w, int h, bool horizontal) {
	double phase[4] = {0.0, 0.0, 0.0, 0.0};
	const size_t rowFloats = static_cast<size_t>(w) * 3u;
	for (int y = 0; y < h - (horizontal ? 0 : 1); ++y) {
		for (int x = 0; x < w - (horizontal ? 1 : 0); ++x) {
			const size_t here = static_cast<size_t>(y) * rowFloats + static_cast<size_t>(x) * 3u;
			const size_t next = horizontal ? here + 3u : here + rowFloats;
			phase[(horizontal ? x : y) % 4] += std::fabs(field[next] - field[here]);
		}
	}
	const double total = phase[0] + phase[1] + phase[2] + phase[3];
	return total > 0.0 ? *std::max_element(phase, phase + 4) / total : 0.0;
}

// The glow is blurred at quarter size and has to come back up smoothly, or it lays a block grid over
// the picture. 837 x 628 is the Display's render size for the default panel at 2x.
void testBeamGlow() {
	std::printf("crt beam: glow scales up with no 4-pixel grid, and keeps its energy\n");
	const crt::BeamSpot spot(0.6f);
	const float white[3] = {1.f, 1.f, 1.f};
	const int w = 837;
	const int h = 628;
	crt::EnergyField strokes;
	strokes.resize(w, h);
	crt::depositPath(strokes, spot, 120.f, 90.f, 700.f, 510.f, white, 40.f, 0, h);
	crt::depositPath(strokes, spot, 700.f, 110.f, 150.f, 470.f, white, 40.f, 0, h);
	crt::depositPath(strokes, spot, 90.f, 317.f, 760.f, 303.f, white, 40.f, 0, h);
	crt::Glow glow;
	const float amount = 0.4f;
	glow.compute(strokes, amount);
	const double shareX = glowGridShare(glow.data(), w, h, true);
	const double shareY = glowGridShare(glow.data(), w, h, false);
	std::printf("  steps on the busiest phase of a 4-pixel grid: columns %.3f, rows %.3f (0.25 even, 1.00 blocks)\n", shareX, shareY);
	check(shareX < 0.30 && shareY < 0.30, "glow steps do not line up on a 4-pixel grid");

	double energyIn = 0.0;
	float peakIn = 0.f;
	fieldTotals(strokes, energyIn, peakIn);
	double energyOut = 0.0;
	for (size_t i = 0; i < static_cast<size_t>(w) * h * 3u; ++i) {
		energyOut += glow.data()[i];
	}
	const double ratio = energyOut / (energyIn * amount * 4.0);
	std::printf("  glow energy out / (energy in x amount x 4): %.4f\n", ratio);
	check(std::fabs(ratio - 1.0) < 0.01, "glow keeps its total energy, set by amount alone");

	// One bright point: its halo has to fade out, not end in a square edge.
	crt::EnergyField point;
	point.resize(w, h);
	const size_t pointRow = static_cast<size_t>(h / 2) * static_cast<size_t>(w);
	point.data()[(pointRow + static_cast<size_t>(w / 2)) * 3u] = 1000.f;
	glow.compute(point, 1.f);
	float pointPeak = 0.f;
	float pointStep = 0.f;
	for (int x = 1; x < w; ++x) {
		const float here = glow.data()[(pointRow + static_cast<size_t>(x)) * 3u];
		const float before = glow.data()[(pointRow + static_cast<size_t>(x) - 1u) * 3u];
		pointPeak = std::max(pointPeak, here);
		pointStep = std::max(pointStep, std::fabs(here - before));
	}
	std::printf("  point halo: peak %.3f, largest step between neighbouring pixels %.3f\n", pointPeak, pointStep);
	check(pointStep < 0.1f * pointPeak, "a point's glow fades out with no hard edge");

	// A flat field at a size that is not a multiple of 4 leaves partial cells on the far edges. They
	// must not read darker than the rest.
	crt::EnergyField flat;
	flat.resize(838, 627);
	const size_t flatCount = static_cast<size_t>(flat.width()) * flat.height() * 3u;
	std::fill(flat.data(), flat.data() + flatCount, 0.5f);
	glow.compute(flat, amount);
	float lo = glow.data()[0];
	float hi = glow.data()[0];
	for (size_t i = 0; i < flatCount; ++i) {
		lo = std::min(lo, glow.data()[i]);
		hi = std::max(hi, glow.data()[i]);
	}
	std::printf("  flat field at 838 x 627: glow from %.5f to %.5f (expected %.5f)\n", lo, hi, 0.5f * amount * 4.f);
	check(hi - lo < 1e-3f && std::fabs(lo - 0.5f * amount * 4.f) < 1e-3f, "a flat field glows evenly up to the last row and column");

	glow.compute(flat, 0.f);
	check(glow.data() == nullptr, "amount 0 gives no glow field");
}

// Energy law of the beam: a path deposits its full energy whatever its length, and the energy per
// pixel falls as the path gets longer. So intensity per area is proportional to 1 / speed.
void testBeamEnergyLaw() {
	std::printf("crt beam: energy is conserved, and per-pixel energy falls with path length\n");
	const crt::BeamSpot spot(0.6f);
	const float white[3] = {1.f, 1.f, 1.f};
	crt::EnergyField shortPath;
	shortPath.resize(200, 60);
	crt::depositPath(shortPath, spot, 40.f, 30.f, 60.f, 30.f, white, 1.f, 0, 60);  // 20 px
	crt::EnergyField longPath;
	longPath.resize(200, 60);
	crt::depositPath(longPath, spot, 20.f, 30.f, 180.f, 30.f, white, 1.f, 0, 60);  // 160 px
	double shortSum = 0.0;
	double longSum = 0.0;
	float shortPeak = 0.f;
	float longPeak = 0.f;
	fieldTotals(shortPath, shortSum, shortPeak);
	fieldTotals(longPath, longSum, longPeak);
	std::printf("  energy in: 20 px path %.4f, 160 px path %.4f; peak %.4f and %.4f\n", shortSum / 3.0, longSum / 3.0, shortPeak, longPeak);
	check(std::fabs(shortSum / 3.0 - 1.0) < 1e-3 && std::fabs(longSum / 3.0 - 1.0) < 1e-3, "a path deposits its full energy, whatever its length");
	const double ratio = shortPeak / longPeak;
	std::printf("  peak ratio short / long: %.2f (length ratio 8)\n", ratio);
	check(ratio > 6.0 && ratio < 10.0, "per-pixel energy falls in proportion to path length (1 / speed)");

	// Overlapping passes add: a second identical stroke doubles the energy on the same pixels.
	crt::EnergyField twice;
	twice.resize(200, 60);
	crt::depositPath(twice, spot, 40.f, 30.f, 60.f, 30.f, white, 1.f, 0, 60);
	crt::depositPath(twice, spot, 40.f, 30.f, 60.f, 30.f, white, 1.f, 0, 60);
	double twiceSum = 0.0;
	float twicePeak = 0.f;
	fieldTotals(twice, twiceSum, twicePeak);
	check(std::fabs(twiceSum - 2.0 * shortSum) < 1e-3 && std::fabs(twicePeak - 2.f * shortPeak) < 1e-4, "overlapping strokes add their energy");
	crt::EnergyField disjoint;
	disjoint.resize(200, 60);
	crt::depositPath(disjoint, spot, 40.f, 30.f, 60.f, 30.f, white, 1.f, 10, 20);  // rows 10..19 only
	double disjointSum = 0.0;
	float disjointPeak = 0.f;
	fieldTotals(disjoint, disjointSum, disjointPeak);
	check(disjointSum == 0.0, "rows outside [rowLo, rowHi) are not written (threads can split rows)");
}

// NTSC-like timing: the line rate is fixed and the field is the visible lines plus the flyback.
void testBeamTiming() {
	std::printf("crt beam: NTSC-like timing\n");
	const double fieldRate = 1.0 / crt::fieldSeconds(240);
	std::printf("  240 lines: field %.3f Hz, %.1f lines per field incl. flyback\n", fieldRate, crt::fieldSeconds(240) * crt::kLineRateHz);
	check(std::fabs(fieldRate - 59.94) < 0.01, "240 visible lines give the NTSC field rate, 59.94 Hz");
	check(std::fabs(crt::fieldSeconds(240) * crt::kLineRateHz - 262.5) < 1e-6, "a field is 262.5 lines, including the flyback");
	check(crt::fieldSeconds(600) > crt::fieldSeconds(120), "more visible lines take a longer field");
}

// LINES sets the scanline count: a white raster shows one bright line per scanline.
void testBeamScanlineCount() {
	std::printf("deflection yoke: LINES sets the number of scanlines\n");
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 6, [](int, float* v) {
		v[kDeflectionYokeDeflectX] = 0.f;
		v[kDeflectionYokeDeflectY] = 0.f;
		v[kDeflectionYokeZoom] = 0.f;
	});
	const Frame white(static_cast<size_t>(kWobW) * kWobH * 4u, 255);
	VideoGraphRenderer renderer;
	for (int lines : {120, 240}) {
		DeflectionYokeParams p;
		p.depth = 1.f;
		p.edge = static_cast<int>(DeflectionYokeEdge::Black);
		p.lines = lines;
		const Frame out = renderWobble(renderer, white, slot, p);
		std::vector<double> profile(kWobH, 0.0);
		double peak = 0.0;
		for (int y = 0; y < kWobH; ++y) {
			double sum = 0.0;
			for (int x = 0; x < kWobW; ++x) {
				sum += out[(static_cast<size_t>(y) * kWobW + x) * 4u];
			}
			profile[static_cast<size_t>(y)] = sum / kWobW;
			peak = std::max(peak, profile[static_cast<size_t>(y)]);
		}
		int maxima = 0;
		for (int y = 1; y + 1 < kWobH; ++y) {
			const double v = profile[static_cast<size_t>(y)];
			if (v > 0.5 * peak && v > profile[static_cast<size_t>(y) - 1] && v >= profile[static_cast<size_t>(y) + 1]) {
				++maxima;
			}
		}
		std::printf("  %d lines: %d bright scanlines counted\n", lines, maxima);
		check(std::abs(maxima - lines) <= 2, "one bright scanline per LINES value");
	}
	signalRingRelease(slot);
}

// Turnarounds are brighter than fast passes. A 5 kHz swing of half the width folds the beam back on
// itself inside each scanline: where the stroke's landing speed falls toward zero, each pixel gets
// more energy (energy per pixel is proportional to 1 / speed). Every scanline of a field is folded at
// a different phase, so the test looks at all of them: on each stroke the turnaround must beat the
// fastest tenth of the stroke by a clear factor. Phase-independent, because the field is one raster.
void testBeamTurnaroundBrightness() {
	std::printf("deflection yoke: a folded stroke is brightest at its turnaround\n");
	const double pi = 3.14159265358979323846;
	const Frame gray(static_cast<size_t>(kWobW) * kWobH * 4u, 90);
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 6, [&](int n, float* v) {
		v[kDeflectionYokeDeflectX] = static_cast<float>(0.5 * std::sin(2.0 * pi * 5000.0 * n / kWobRate));
		v[kDeflectionYokeDeflectY] = 0.f;
		v[kDeflectionYokeZoom] = 0.f;
	});
	VideoGraphRenderer renderer;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	const Frame out = renderWobble(renderer, gray, slot, p);
	const double pitch = static_cast<double>(kWobH) / kWobLines;
	std::vector<double> ratios;
	for (int k = 0; k < kWobLines; ++k) {
		const int row = static_cast<int>(std::lround((k + 0.5) * pitch));
		std::vector<double> profile;
		for (int x = 0; x < kWobW; ++x) {
			double sum = 0.0;
			for (int y = row - 1; y <= row + 1; ++y) {
				sum += out[(static_cast<size_t>(y) * kWobW + x) * 4u];
			}
			if (sum > 0.0) {
				profile.push_back(sum / 3.0);
			}
		}
		if (profile.size() < 40) {
			continue;  // a stroke that barely reaches the screen
		}
		std::sort(profile.begin(), profile.end());
		ratios.push_back(profile.back() / std::max(profile[profile.size() / 10], 1.0));
	}
	std::sort(ratios.begin(), ratios.end());
	const double median = ratios.empty() ? 0.0 : ratios[ratios.size() / 2];
	const double top = ratios.empty() ? 0.0 : ratios.back();
	std::printf("  %zu strokes: turnaround / fastest-tenth median %.2f, best %.2f\n", ratios.size(), median, top);
	check(ratios.size() > kWobLines / 2, "most scanlines show a stroke");
	check(median > 1.8, "a turnaround is clearly brighter than the fastest part of its stroke");
	signalRingRelease(slot);
}

// Two strokes on the same pixels add. Persistence and repeated passes build up light the same way.
void testBeamOverlapAdds() {
	std::printf("crt beam: overlapping passes add\n");
	const crt::BeamSpot spot(0.f);
	const float white[3] = {1.f, 1.f, 1.f};
	crt::EnergyField one;
	one.resize(64, 16);
	crt::depositPath(one, spot, 8.f, 8.f, 56.f, 8.f, white, 8.f, 0, 16);
	crt::EnergyField two;
	two.resize(64, 16);
	crt::depositPath(two, spot, 8.f, 8.f, 56.f, 8.f, white, 8.f, 0, 16);
	crt::depositPath(two, spot, 8.f, 8.f, 56.f, 8.f, white, 8.f, 0, 16);
	const size_t centre = (8u * 64u + 32u) * 3u;
	const float e1 = one.data()[centre];
	const float e2 = two.data()[centre];
	const unsigned char lit1 = static_cast<unsigned char>(std::lround(255.f * (1.f - std::exp(-1.4f * e1))));
	const unsigned char lit2 = static_cast<unsigned char>(std::lround(255.f * (1.f - std::exp(-1.4f * e2))));
	std::printf("  one pass %.3f, two passes %.3f: tone-mapped %d and %d\n", e1, e2, lit1, lit2);
	check(std::fabs(e2 - 2.f * e1) < 1e-4f, "two passes on one pixel carry twice the energy");
	check(lit2 > lit1 + 10, "the tone curve shows the added light");
}

// Phosphor: after the source goes dark, the energy decays. One frame leaves some light, and each
// further frame leaves less.
void testBeamPhosphorDecay() {
	std::printf("deflection yoke: phosphor persistence decays across frames\n");
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 6, [](int, float* v) {
		v[kDeflectionYokeDeflectX] = 0.f;
		v[kDeflectionYokeDeflectY] = 0.f;
		v[kDeflectionYokeZoom] = 0.f;
	});
	const Frame white(static_cast<size_t>(kWobW) * kWobH * 4u, 255);
	const Frame black = [] {
		Frame f(static_cast<size_t>(kWobW) * kWobH * 4u, 0);
		for (size_t i = 3; i < f.size(); i += 4) {
			f[i] = 255;
		}
		return f;
	}();
	VideoGraphRenderer renderer;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	p.persist = 0.8f;
	renderWobble(renderer, white, slot, p);
	double lit[6] = {};
	for (int i = 1; i <= 5; ++i) {
		const Frame out = renderWobble(renderer, black, slot, p);
		double sum = 0.0;
		for (size_t j = 0; j < out.size(); j += 4) {
			sum += out[j];
		}
		lit[i] = sum;
	}
	std::printf("  light after 1, 2 and 5 dark frames: %.0f, %.0f, %.0f\n", lit[1], lit[2], lit[5]);
	check(lit[1] > 0.0, "one frame after the source goes dark leaves light");
	check(lit[2] < lit[1] && lit[5] < 0.5 * lit[1], "less light remains after more frames");
	signalRingRelease(slot);
}

// Zoom near zero collapses the raster to a point. A white source at zoom 0.01 lights only a small
// patch at the centre. At zoom 1 it fills the screen.
void testBeamZoomCollapse() {
	std::printf("deflection yoke: zoom near 0 collapses the raster toward a point\n");
	const int slot = signalRingAllocate();
	const Frame white(static_cast<size_t>(kWobW) * kWobH * 4u, 255);
	VideoGraphRenderer renderer;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	auto litBox = [&](float zoomChannel) {
		pushScanSignal(slot, 6, [&](int, float* v) {
			v[kDeflectionYokeDeflectX] = 0.f;
			v[kDeflectionYokeDeflectY] = 0.f;
			v[kDeflectionYokeZoom] = zoomChannel;
		});
		const Frame out = renderWobble(renderer, white, slot, p);
		int x0 = kWobW, x1 = -1, y0 = kWobH, y1 = -1;
		for (int y = 0; y < kWobH; ++y) {
			for (int x = 0; x < kWobW; ++x) {
				if (out[(static_cast<size_t>(y) * kWobW + x) * 4u] > 40) {
					x0 = std::min(x0, x);
					x1 = std::max(x1, x);
					y0 = std::min(y0, y);
					y1 = std::max(y1, y);
				}
			}
		}
		return std::array<int, 4>{x0, x1, y0, y1};
	};
	const auto collapsed = litBox(-0.99f);  // zoom 0.01, held at the 0.02 floor
	const auto full = litBox(0.f);          // zoom 1
	const int boxW = collapsed[1] - collapsed[0];
	const int boxH = collapsed[3] - collapsed[2];
	std::printf("  zoom 0.02: lit box %d x %d px; zoom 1: %d x %d px\n", boxW + 1, boxH + 1, full[1] - full[0] + 1, full[3] - full[2] + 1);
	check(boxW < kWobW / 10 && boxH < kWobH / 10, "near-zero zoom collapses the raster to a small patch");
	check(full[1] - full[0] > kWobW * 9 / 10 && full[3] - full[2] > kWobH * 9 / 10, "zoom 1 fills the screen");
	saveWobbleProof("wobble_zoom_collapse.png", renderWobble(renderer, white, slot, p));
	signalRingRelease(slot);
}

// Coil: the yoke's inductance lowers the deflection of high tones. A 5 kHz tone deflects less than
// a 500 Hz tone of the same amplitude, both in the filter and on the picture.
void testBeamCoil() {
	std::printf("deflection yoke: coil response lowers high-frequency deflection\n");
	const double pi = 3.14159265358979323846;
	auto steadyGain = [&](double hz) {
		crt::CoilFilter coil;
		coil.setCutoff(3000.f, 48000.f);
		float peak = 0.f;
		for (int n = 0; n < 48000; ++n) {
			const float y = coil.process(static_cast<float>(std::sin(2.0 * pi * hz * n / 48000.0)));
			if (n >= 24000) {
				peak = std::max(peak, std::fabs(y));
			}
		}
		return static_cast<double>(peak);
	};
	const double gLow = steadyGain(500.0);
	const double gHigh = steadyGain(5000.0);
	std::printf("  filter gain at 500 Hz %.3f, at 5 kHz %.3f (cutoff 3 kHz)\n", gLow, gHigh);
	check(gLow > 0.95 && gHigh < 0.6 * gLow, "a 5 kHz tone is attenuated by the coil, a 500 Hz tone is not");

	const Frame line = makeLineSource(kWobW, kWobH, kWobW / 2);
	const int slot = signalRingAllocate();
	VideoGraphRenderer renderer;
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	auto swing = [&](double hz) {
		crt::CoilFilter coil;
		coil.setCutoff(3000.f, kWobRate);
		pushScanSignal(slot, 6, [&](int n, float* v) {
			const float x = coil.process(static_cast<float>(0.25 * std::sin(2.0 * pi * hz * n / kWobRate)));
			v[kDeflectionYokeDeflectX] = x;
			v[kDeflectionYokeDeflectY] = 0.f;
			v[kDeflectionYokeZoom] = 0.f;
		});
		const std::vector<double> c = lineCentroids(renderWobble(renderer, line, slot, p), kWobW, kWobH);
		double lo = 1e9;
		double hi = -1e9;
		for (double v : c) {
			if (!std::isnan(v)) {
				lo = std::min(lo, v);
				hi = std::max(hi, v);
			}
		}
		return 0.5 * (hi - lo);
	};
	const double amp500 = swing(500.0);
	const double amp5k = swing(5000.0);
	std::printf("  picture swing: 500 Hz %.1f px, 5 kHz %.1f px\n", amp500, amp5k);
	check(amp5k < 0.7 * amp500, "on the picture, a 5 kHz swing is smaller than a 500 Hz swing of the same amplitude");
	signalRingRelease(slot);
}

// Soft camera-like test image: a radial falloff with a bright disc, a dark horizon bar, a warm
// rectangle and a few hard edges. Portrait, like the other DeflectionYoke proofs.
Frame makeCameraSource(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.f - 1.f;
			const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.f - 1.f;
			const float r = std::sqrt(u * u + 0.8f * v * v);
			float light = std::clamp(0.75f - 0.55f * r * r, 0.05f, 1.f);
			// Dark horizon bar and a soft bright disc.
			if (std::fabs(v - 0.15f) < 0.05f) {
				light *= 0.25f;
			}
			const float dx = u + 0.35f;
			const float dy = v + 0.3f;
			const float disc = std::clamp((0.24f - std::sqrt(dx * dx + dy * dy)) * 18.f, 0.f, 1.f);
			light = light * (1.f - disc) + disc;
			// Warm rectangle on the right and a hard vertical edge.
			const bool warm = u > 0.25f && u < 0.8f && v > 0.35f && v < 0.8f;
			const float r0 = warm ? 1.f : light;
			const float g0 = warm ? 0.62f : light * 0.9f;
			const float b0 = warm ? 0.3f : light * 0.8f;
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			f[i + 0] = static_cast<unsigned char>(std::clamp(r0 * 255.f, 0.f, 255.f));
			f[i + 1] = static_cast<unsigned char>(std::clamp(g0 * 255.f, 0.f, 255.f));
			f[i + 2] = static_cast<unsigned char>(std::clamp(b0 * 255.f, 0.f, 255.f));
			f[i + 3] = 255;
		}
	}
	return f;
}

// Module-default beam for the proofs: 240 lines, spot 0.20, persistence 0.35, glow 0.4, black edge.
DeflectionYokeParams proofParams() {
	DeflectionYokeParams p;
	p.depth = 1.f;
	p.edge = static_cast<int>(DeflectionYokeEdge::Black);
	p.lines = 240;
	p.spot = 0.20f;
	p.persist = 0.35f;
	p.glow = 0.4f;
	return p;
}

// Renders one proof frame: X at xHz (amplitude xAmp), Y in quadrature at yHz (amplitude yAmp), and a
// constant zoom channel. Amplitudes are in the ring's units (0.25 = a quarter of the width).
Frame beamProof(VideoGraphRenderer& renderer, const Frame& source, double xHz, double xAmp, double yHz, double yAmp, float zoomChannel, const DeflectionYokeParams& p) {
	const double pi = 3.14159265358979323846;
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 6, [&](int n, float* v) {
		v[kDeflectionYokeDeflectX] = static_cast<float>(xAmp * std::sin(2.0 * pi * xHz * n / kWobRate));
		v[kDeflectionYokeDeflectY] = static_cast<float>(yAmp * std::cos(2.0 * pi * yHz * n / kWobRate));
		v[kDeflectionYokeZoom] = zoomChannel;
	});
	const Frame out = renderWobble(renderer, source, slot, p);
	signalRingRelease(slot);
	return out;
}

// Proof images (no checks): the grid and the camera-like image at 200 Hz, 1 kHz and 5 kHz, quadrature,
// zoom collapse and persistence. Written to the proof directory.
void testBeamProofs() {
	std::printf("beam proofs: scanlines on a tube\n");
	const Frame grid = makeGridSource(kWobW, kWobH);
	const Frame camera = makeCameraSource(kWobW, kWobH);
	VideoGraphRenderer renderer;
	const DeflectionYokeParams p = proofParams();
	struct Case {
		const char* name;
		double xHz, xAmp, yHz, yAmp;
		float zoom;
	};
	const Case cases[] = {
		{"200hz", 200.0, 0.25, 0.0, 0.0, 0.f},
		{"1khz", 1000.0, 0.25, 0.0, 0.0, 0.f},
		{"5khz", 5000.0, 0.25, 0.0, 0.0, 0.f},
		{"quadrature_1khz", 1000.0, 0.12, 1000.0, 0.12, 0.f},
		{"zoom_collapse", 0.0, 0.0, 0.0, 0.0, -0.9f},
	};
	for (const Case& c : cases) {
		const Frame g = beamProof(renderer, grid, c.xHz, c.xAmp, c.yHz, c.yAmp, c.zoom, p);
		const Frame q = beamProof(renderer, camera, c.xHz, c.xAmp, c.yHz, c.yAmp, c.zoom, p);
		char name[96];
		std::snprintf(name, sizeof(name), "beam_grid_%s.png", c.name);
		saveWobbleProof(name, g);
		std::snprintf(name, sizeof(name), "beam_camera_%s.png", c.name);
		saveWobbleProof(name, q);
	}
	// Persistence: the camera image, then the source goes dark. Frames 0, 1 and 4 after the change, at
	// persistence 0 and 0.8 side by side.
	const Frame dark(static_cast<size_t>(kWobW) * kWobH * 4u, 0);
	std::vector<Frame> row;
	for (double persist : {0.0, 0.8}) {
		VideoGraphRenderer trail;
		DeflectionYokeParams pp = p;
		pp.persist = static_cast<float>(persist);
		beamProof(trail, camera, 1000.0, 0.12, 0.0, 0.0, 0.f, pp);
		row.push_back(beamProof(trail, dark, 1000.0, 0.12, 0.0, 0.0, 0.f, pp));
		for (int i = 0; i < 3; ++i) {
			beamProof(trail, dark, 1000.0, 0.12, 0.0, 0.0, 0.f, pp);
		}
		row.push_back(beamProof(trail, dark, 1000.0, 0.12, 0.0, 0.0, 0.f, pp));
	}
	saveProof("beam_persistence.png", row);
}

// ---- Video Oscillator grid at display sizes ----
// Grid lines are at least two pixels wide at any render height, so they survive resampling.
void testGridLineWidth() {
	std::printf("video osc: grid line width\n");
	for (int h : {144, 480, 720}) {
		const int w = h * 7 / 10;
		PreviewVideoEngine engine;
		VideoParams params;
		params.pattern = static_cast<int>(VideoPattern::Grid);
		const Frame& f = engine.render(w, h, params);
		const int row = h / 3;
		auto luma = [&](int x) {
			const size_t i = (static_cast<size_t>(row) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			return static_cast<int>(f[i]) + f[i + 1] + f[i + 2];
		};
		int background = 765;
		for (int x = 1; x < w - 1; ++x) {
			background = std::min(background, luma(x));
		}
		const int major = static_cast<int>(std::lround(static_cast<double>(w) / 8.0));
		int lit = 0;
		for (int x = major - 3; x <= major + 3; ++x) {
			if (x > 0 && x < w - 1 && luma(x) > background + 60) {
				++lit;
			}
		}
		std::printf("  %d x %d: %d lit pixels across a major line\n", w, h, lit);
		check(lit >= 2, "a major grid line is at least two pixels wide");
	}
}

// ---- Scope Trace ----
void testScopeCaptureAndFallback() {
	std::printf("scope trace: full-rate capture and fallback frequency\n");
	const double pi = 3.14159265358979323846;
	// Fallback: the phase advances at scopeFallbackHz(TIME). Count cycles over one second.
	for (float tb : {0.f, 0.35f, 1.f}) {
		double phase = 0.0;
		int crossings = 0;
		double previous = 0.0;
		const float sampleTime = 1.f / 48000.f;
		for (int n = 0; n < 48000; ++n) {
			phase = advanceScopeFallbackPhase(phase, tb, sampleTime);
			const double value = std::sin(2.0 * pi * phase);
			if (n > 0 && previous < 0.0 && value >= 0.0) {
				++crossings;
			}
			previous = value;
		}
		const double hz = scopeFallbackHz(tb);
		std::printf("  TIME %.2f: window %.1f ms, fallback %.1f Hz, %d cycles per second\n", tb, scopeWindowSeconds(tb) * 1000.0, hz, crossings);
		check(std::abs(crossings - hz) <= 1.0, "fallback oscillator runs at the Hz it reports");
	}
	// The default window is 10^(-2.3) s = 5.012 ms, so two cycles in it are 399.05 Hz, not a round 400.
	check(std::abs(scopeFallbackHz(0.35f) - 399.05) < 0.05, "default TIME gives a 399.05 Hz fallback (two cycles in 5.01 ms)");

	// Full-rate capture: a 5 kHz tone sampled at 48 kHz keeps its frequency in the ring. A
	// decimated capture would alias it. 10 ms should hold 50 cycles.
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, 48000.f);
	for (int n = 0; n < 48000; ++n) {
		const float v = static_cast<float>(std::sin(2.0 * pi * 5000.0 * n / 48000.0));
		const float values[kSignalRingChannels] = {v, 0.f, 1.f};
		signalRingPush(slot, values);
	}
	SignalWindow window;
	check(signalRingReadSeconds(slot, 0.01, window) && window.length == 480, "10 ms window is read in samples");
	int cycles = 0;
	for (int i = 1; i < window.length; ++i) {
		if (window.sampleAt(0, i - 1) < 0.f && window.sampleAt(0, i) >= 0.f) {
			++cycles;
		}
	}
	std::printf("  5 kHz tone, 10 ms window at full rate: %d cycles (expected 50)\n", cycles);
	check(std::abs(cycles - 50) <= 1, "full-rate capture keeps a 5 kHz tone at 5 kHz");
	signalRingRelease(slot);
}

void testScopePersistenceAndBloom() {
	std::printf("scope trace: persistence and bloom\n");
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, 48000.f);
	const int bus = 50;
	const double pi = 3.14159265358979323846;
	// A 1 kHz sine fills the window, then a flat line replaces it.
	for (int n = 0; n < 4800; ++n) {
		const float values[kSignalRingChannels] = {static_cast<float>(0.8 * std::sin(2.0 * pi * 1000.0 * n / 48000.0)), 0.f, 1.f};
		signalRingPush(slot, values);
	}

	auto publish = [&](float decay, float bloom) {
		ScopeTraceParams p;
		p.mode = static_cast<int>(ScopeTraceMode::Oscilloscope);
		p.timebase = 0.35f;
		p.decay = decay;
		p.bloom = bloom;
		p.intensity = 1.2f;
		videoBusPublishScopeTrace(bus, slot, p);
	};
	// Luma outside the flat trace row, summed over the frame: how much of the old sine is left.
	auto leftover = [](const Frame& f) {
		double sum = 0.0;
		for (int y = 0; y < kH; ++y) {
			if (std::abs(y - kH / 2) < 6) {
				continue;
			}
			for (int x = 0; x < kW; ++x) {
				sum += lumaAt(f, x, y);
			}
		}
		return sum;
	};

	VideoGraphRenderer longTail;
	publish(1.f, 0.f);
	render(longTail, bus);  // draws the sine
	for (int n = 0; n < 4800; ++n) {
		const float values[kSignalRingChannels] = {0.f, 0.f, 1.f};
		signalRingPush(slot, values);
	}
	Frame held;
	for (int i = 0; i < 4; ++i) {
		held = render(longTail, bus);  // the flat trace is drawn; the sine decays over four renders
	}
	const double longLeft = leftover(held);

	VideoGraphRenderer shortTail;
	publish(0.f, 0.f);
	for (int n = 0; n < 4800; ++n) {
		const float values[kSignalRingChannels] = {static_cast<float>(0.8 * std::sin(2.0 * pi * 1000.0 * n / 48000.0)), 0.f, 1.f};
		signalRingPush(slot, values);
	}
	render(shortTail, bus);
	for (int n = 0; n < 4800; ++n) {
		const float values[kSignalRingChannels] = {0.f, 0.f, 1.f};
		signalRingPush(slot, values);
	}
	for (int i = 0; i < 4; ++i) {
		held = render(shortTail, bus);
	}
	const double shortLeft = leftover(held);
	std::printf("  leftover trail after 4 frames: decay 1.0 = %.1f, decay 0.0 = %.1f\n", longLeft, shortLeft);
	check(longLeft > 3.0 * shortLeft && longLeft > 0.0, "high persistence keeps the trail across frames");

	// Bloom: with bloom on, pixels a few rows off the flat trace get light; without it they stay dark.
	VideoGraphRenderer plain;
	VideoGraphRenderer glow;
	publish(0.65f, 0.f);
	const Frame noBloom = render(plain, bus);
	publish(0.65f, 1.5f);
	const Frame withBloom = render(glow, bus);
	double offPlain = 0.0;
	double offGlow = 0.0;
	for (int y = kH / 2 + 8; y <= kH / 2 + 12; ++y) {
		for (int x = 0; x < kW; ++x) {
			offPlain += lumaAt(noBloom, x, y);
			offGlow += lumaAt(withBloom, x, y);
		}
	}
	std::printf("  light 8-12 rows off the trace: no bloom %.1f, bloom %.1f\n", offPlain, offGlow);
	check(offGlow > 2.0 * offPlain, "bloom spreads light away from the trace");
	saveProof("scope_persistence_bloom_proof.png", {noBloom, withBloom});
	signalRingRelease(slot);
}

// ---- Scope Trace: trigger, trigger stamps, per-mode fallback ----
void testScopeTriggerFinder() {
	std::printf("scope trace: trigger finder\n");
	// 400 Hz at 48 kHz is 120 samples per cycle. Window 240 (2 cycles), buffer 480. The sine
	// passes +hysteresis (0.05) between samples 121 (0.042) and 122 (0.084), so the latest edge
	// that leaves room for the window starts at 122.
	std::vector<float> sine(480);
	for (int n = 0; n < 480; ++n) {
		sine[static_cast<size_t>(n)] = static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979323846 * n / 120.0));
	}
	const int p = scopeFindTrigger(sine.data(), 480, 240, kScopeTriggerHysteresis);
	std::printf("  400 Hz sine, 480 samples, 240-sample window: trigger at %d\n", p);
	check(p == 122, "trigger is the latest rising edge that fits the window");
	check(p >= 1 && sine[static_cast<size_t>(p) - 1] < kScopeTriggerHysteresis && sine[static_cast<size_t>(p)] >= kScopeTriggerHysteresis, "trigger sits on the rising edge through +hysteresis");

	std::vector<float> dc(480, 0.3f);
	check(scopeFindTrigger(dc.data(), 480, 240, kScopeTriggerHysteresis) == -1, "a signal that never goes below -hysteresis has no trigger (auto free-run)");
	std::vector<float> chatter(480);
	for (int n = 0; n < 480; ++n) {
		chatter[static_cast<size_t>(n)] = 0.02f * static_cast<float>(std::sin(0.9 * n));
	}
	check(scopeFindTrigger(chatter.data(), 480, 240, kScopeTriggerHysteresis) == -1, "noise around zero, inside the hysteresis band, does not fire the trigger");
	check(scopeFindTrigger(sine.data(), 200, 240, kScopeTriggerHysteresis) == -1, "a buffer shorter than the window has no trigger");
}

void testScopeTrigStamps() {
	std::printf("scope trace: trigger stamps\n");
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, 48000.f);
	for (int n = 0; n < 1000; ++n) {
		const float values[kSignalRingChannels] = {static_cast<float>(n), 0.f, 1.f};
		signalRingPush(slot, values);
		if (n == 300) {
			signalRingMarkTrigger(slot);  // stamps sample 300
		}
	}
	SignalWindow window;
	check(signalRingReadTriggered(slot, 200, window) && window.length == 200, "window of 200 samples starts at a trigger");
	check(window.length > 0 && window.sampleAt(0, 0) == 300.0 && window.sampleAt(0, 199) == 499.0, "triggered window starts at the stamped sample");
	check(!signalRingReadTriggered(slot, 800, window), "no window while the stamp has too few samples after it");
	for (int n = 1000; n < 1800; ++n) {
		const float values[kSignalRingChannels] = {static_cast<float>(n), 0.f, 1.f};
		signalRingPush(slot, values);
	}
	check(signalRingReadTriggered(slot, 800, window) && window.sampleAt(0, 0) == 300.0, "the stamp becomes usable once its window is complete");
	signalRingRelease(slot);
}

// Pushes `count` samples of a sine (X) from sample index `from`, at 48 kHz.
void pushSine(int slot, int from, int count, double freqHz, float amplitude, float offset) {
	const double pi = 3.14159265358979323846;
	for (int n = from; n < from + count; ++n) {
		const float values[kSignalRingChannels] = {offset + static_cast<float>(amplitude * std::sin(2.0 * pi * freqHz * n / 48000.0)), 0.f, 1.f};
		signalRingPush(slot, values);
	}
}

ScopeTraceParams scopeDefaults(ScopeTraceMode mode) {
	ScopeTraceParams p;
	p.mode = static_cast<int>(mode);
	p.timebase = 0.35f;
	p.decay = 0.f;  // phosphor retains 0.5 per frame: 12 warm-up frames settle it
	p.thickness = 0.4f;
	p.intensity = 1.2f;
	return p;
}

void testScopeTriggeredFramesLock() {
	std::printf("scope trace: triggered frames line up\n");
	const int bus = 51;
	// Frame N and N+1: 800 new samples per frame (60 Hz at 48 kHz), so the raw window moves 80
	// samples of a 120-sample cycle each frame. A triggered window does not move.
	// trigStamps: the TRIG path. The ring is stamped at every cycle start (sample 0 mod 120), as a
	// TRIG cable on the same 400 Hz would do, and the scope is told a TRIG cable is patched.
	auto framePair = [&](float amplitude, float offset, Frame& a, Frame& b, bool trigStamps = false) {
		const int slot = signalRingAllocate();
		signalRingSetSampleRate(slot, 48000.f);
		int from = 0;
		auto push = [&](int count) {
			for (int n = from; n < from + count; ++n) {
				const double pi = 3.14159265358979323846;
				const float values[kSignalRingChannels] = {offset + static_cast<float>(amplitude * std::sin(2.0 * pi * 400.0 * n / 48000.0)), 0.f, 1.f};
				signalRingPush(slot, values);
				if (trigStamps && n % 120 == 0) {
					signalRingMarkTrigger(slot);
				}
			}
			from += count;
		};
		push(4800);
		ScopeTraceParams p = scopeDefaults(ScopeTraceMode::Oscilloscope);
		p.triggerPatched = trigStamps;
		videoBusPublishScopeTrace(bus, slot, p);
		VideoGraphRenderer r;
		for (int i = 0; i < 12; ++i) {
			push(800);
			r.renderBus(bus, kW, kH);
		}
		push(800);
		a = render(r, bus);
		push(800);
		b = render(r, bus);
		signalRingRelease(slot);
	};

	Frame triggeredA, triggeredB;
	framePair(0.8f, 0.f, triggeredA, triggeredB);
	// Pixels whose brightness differs by more than 16 between the two frames.
	auto movedPixels = [](const Frame& a, const Frame& b) {
		int moved = 0;
		for (size_t i = 0; i < a.size(); i += 4) {
			int d = 0;
			for (int c = 0; c < 3; ++c) {
				d = std::max(d, std::abs(int(a[i + c]) - int(b[i + c])));
			}
			moved += d > 16 ? 1 : 0;
		}
		return moved;
	};
	const int triggeredMoved = movedPixels(triggeredA, triggeredB);
	std::printf("  periodic input: frame N vs N+1, %d pixels moved, max diff %d\n", triggeredMoved, maxDiff(triggeredA, triggeredB));
	check(triggeredMoved == 0 && maxDiff(triggeredA, triggeredB) <= 2, "triggered oscilloscope: frame N and N+1 match for a periodic input");

	// The TRIG cable path: stamps at the cycle starts line the frames up the same way.
	Frame stampA, stampB;
	framePair(0.8f, 0.f, stampA, stampB, true);
	std::printf("  TRIG stamps: frame N vs N+1, %d pixels moved\n", movedPixels(stampA, stampB));
	check(movedPixels(stampA, stampB) == 0 && maxDiff(stampA, stampB) <= 2, "TRIG-stamped oscilloscope: frame N and N+1 match");

	// Contrast: a sine that never dips below the trigger band (DC offset 0.6) has no trigger, so
	// it free-runs, and the window walks from frame to frame.
	Frame freeA, freeB;
	framePair(0.3f, 0.6f, freeA, freeB);
	const int freeMoved = movedPixels(freeA, freeB);
	std::printf("  no trigger (free-run): frame N vs N+1, %d pixels moved\n", freeMoved);
	check(freeMoved > 50, "without a trigger the same input walks from frame to frame");
}

void testScopeModesDiffer() {
	std::printf("scope trace: modes draw different pictures\n");
	// Each mode gets the module's fallback signal for that mode, warmed up for 12 frames.
	auto modeFrame = [&](ScopeTraceMode mode, int bus) {
		const int slot = signalRingAllocate();
		signalRingSetSampleRate(slot, 48000.f);
		double phase = 0.0;
		auto push800 = [&]() {
			for (int n = 0; n < 800; ++n) {
				phase = advanceScopeFallbackPhase(phase, 0.35f, 1.f / 48000.f);
				float x = 0.f, y = 0.f;
				scopeFallbackSignal(static_cast<int>(mode), phase, x, y);
				const float values[kSignalRingChannels] = {x, y, 1.f};
				signalRingPush(slot, values);
			}
		};
		ScopeTraceParams p = scopeDefaults(mode);
		p.decay = 0.65f;
		p.timebase = 0.35f;
		videoBusPublishScopeTrace(bus, slot, p);
		VideoGraphRenderer r;
		Frame f;
		for (int i = 0; i < 12; ++i) {
			push800();
			f = render(r, bus);
		}
		signalRingRelease(slot);
		return f;
	};
	const Frame xy = modeFrame(ScopeTraceMode::XY, 52);
	const Frame osc = modeFrame(ScopeTraceMode::Oscilloscope, 53);
	const Frame dual = modeFrame(ScopeTraceMode::DualTrace, 54);
	const Frame polar = modeFrame(ScopeTraceMode::Polar, 55);
	const std::vector<Frame> modes = {xy, osc, dual, polar};
	const char* names[4] = {"XY", "Oscilloscope", "Dual", "Polar"};
	for (int i = 0; i < 4; ++i) {
		int lit = 0;
		for (int y = 0; y < kH; ++y) {
			for (int x = 0; x < kW; ++x) {
				lit += lumaAt(modes[static_cast<size_t>(i)], x, y) > 0.1f ? 1 : 0;
			}
		}
		std::printf("  %-13s %d lit pixels\n", names[i], lit);
		check(lit > 50, "each mode draws a visible trace");
	}
	for (int i = 0; i < 4; ++i) {
		for (int j = i + 1; j < 4; ++j) {
			const double d = meanDiff(modes[static_cast<size_t>(i)], modes[static_cast<size_t>(j)]);
			std::printf("  %s vs %s: mean diff %.2f\n", names[i], names[j], d);
			check(d > 1.0, "every pair of modes draws a measurably different image");
		}
	}
	saveProof("scope_modes_proof.png", modes);
}

// ---- Scope Trace: CV maths, sweep generator, trigger level, RESET ----
void testCvParamMaths() {
	std::printf("cv_param: modulated, wrapped hue and Z maths\n");
	using namespace rasterlab_cv;
	const float bipolar = 1.f / 5.f;    // per volt, for a -1..+1 control driven by +-5 V
	const float unipolar = 1.f / 10.f;  // per volt, for a 0..1 control driven by 0..10 V
	auto isNear = [](float a, float b) {
		return std::abs(a - b) < 1e-5f;
	};

	// modulated(): knob + amount * volts * perVolt, clamped to the control's range.
	bool unpatchedExact = true;
	for (float amount : {-1.f, -0.5f, 0.f, 0.5f, 1.f}) {
		unpatchedExact = unpatchedExact && modulated(0.85f, 0.f, amount, bipolar, 0.f, 2.f) == 0.85f;
	}
	check(unpatchedExact, "modulated: 0 V (an unpatched jack) adds nothing at any attenuverter setting");
	check(isNear(modulated(0.f, 2.5f, 1.f, bipolar, -1.f, 1.f), 0.5f), "modulated: +2.5 V at full amount moves a +-1 control by 0.5");
	check(isNear(modulated(0.f, 5.f, 0.5f, bipolar, -1.f, 1.f), 0.5f), "modulated: half amount halves the CV");
	check(isNear(modulated(0.f, 5.f, -1.f, bipolar, -1.f, 1.f), -1.f), "modulated: a negative amount inverts the CV");
	check(modulated(0.3f, 10.f, 0.f, bipolar, -1.f, 1.f) == 0.3f, "modulated: amount 0 ignores the CV");
	check(isNear(modulated(0.35f, 5.f, 1.f, unipolar, 0.f, 1.f), 0.85f), "modulated: TIME moves 0.1 per volt");
	check(modulated(1.9f, 5.f, 1.f, bipolar, 0.f, 2.f) == 2.f, "modulated: scale clamps at 2");
	check(modulated(0.1f, -5.f, 1.f, bipolar, 0.f, 2.f) == 0.f, "modulated: scale clamps at 0");
	check(modulated(0.9f, 10.f, 1.f, bipolar, -1.f, 1.f) == 1.f && modulated(-0.9f, -10.f, 1.f, bipolar, -1.f, 1.f) == -1.f, "modulated: position and rotation clamp at +-1");
	check(modulated(0.35f, 10.f, 1.f, unipolar, 0.f, 1.f) == 1.f && modulated(0.35f, -5.f, 1.f, unipolar, 0.f, 1.f) == 0.f, "modulated: TIME clamps at 0 and 1");
	bool inRange = true;
	for (float volts = -60.f; volts <= 60.f; volts += 0.37f) {
		for (float amount : {-1.f, 0.5f, 1.f}) {
			const float v = modulated(0.5f, volts, amount, bipolar, -1.f, 1.f);
			inRange = inRange && v >= -1.f && v <= 1.f;
		}
	}
	check(inRange, "modulated: no CV voltage pushes a control outside its range");

	// modulatedWrapped(): the same sum, wrapped round the 0..1 hue wheel instead of clamped.
	check(modulatedWrapped(0.3f, 0.f, 1.f, unipolar) == 0.3f, "wrapped: hue with no CV is the knob");
	check(isNear(modulatedWrapped(0.9f, 2.f, 1.f, unipolar), 0.1f), "wrapped: hue wraps past 1");
	check(isNear(modulatedWrapped(0.1f, -3.f, 1.f, unipolar), 0.8f) && isNear(modulatedWrapped(0.f, -1.f, 1.f, unipolar), 0.9f), "wrapped: negative hue wraps round from the top");
	check(isNear(modulatedWrapped(0.f, -25.f, 1.f, unipolar), 0.5f), "wrapped: hue wraps more than once");
	check(isNear(modulatedWrapped(0.25f, 10.f, 1.f, unipolar), 0.25f), "wrapped: 10 V at 0.1 per volt is once round the wheel");
	bool wrappedInRange = true;
	for (float volts = -60.f; volts <= 60.f; volts += 0.37f) {
		for (float knob : {0.f, 0.32f, 1.f}) {
			for (float amount : {-1.f, 0.5f, 1.f}) {
				const float h = modulatedWrapped(knob, volts, amount, unipolar);
				wrappedInRange = wrappedInRange && h >= 0.f && h < 1.f;
			}
		}
	}
	check(wrappedInRange, "wrapped: hue always lands in [0, 1)");
	const float justBelowZero = modulatedWrapped(0.f, -1e-8f, 1.f, unipolar);
	check(justBelowZero >= 0.f && justBelowZero < 1.f, "wrapped: a hair below zero never rounds up to 1.0");

	// zModulation(): the Z (beam brightness) factor.
	check(zModulation(0.f, 1.f, false) == 1.f && zModulation(8.f, -1.f, false) == 1.f && zModulation(-3.f, 0.4f, false) == 1.f, "Z: unpatched is always full brightness");
	bool classicZ = true;
	for (float volts = -12.f; volts <= 12.f; volts += 0.25f) {
		classicZ = classicZ && zModulation(volts, 1.f, true) == std::clamp(volts / 5.f, 0.f, 2.f);
	}
	check(classicZ, "Z: patched at amount +1 is exactly the old clamp(V / 5, 0, 2)");
	check(zModulation(0.f, 1.f, true) == 0.f && zModulation(5.f, 1.f, true) == 1.f && zModulation(10.f, 1.f, true) == 2.f, "Z: at +1, 0 V blanks, 5 V is full, 10 V is double");
	check(zModulation(-10.f, 0.f, true) == 1.f && zModulation(0.f, 0.f, true) == 1.f && zModulation(10.f, 0.f, true) == 1.f, "Z: at amount 0 the CV is ignored and the beam stays full");
	check(zModulation(0.f, -1.f, true) == 1.f && zModulation(5.f, -1.f, true) == 0.f && zModulation(-5.f, -1.f, true) == 2.f, "Z: at amount -1 it inverts (+5 V blanks, -5 V doubles)");
	check(isNear(zModulation(0.f, 0.5f, true), 0.5f) && isNear(zModulation(5.f, 0.5f, true), 1.f), "Z: at amount +0.5 the floor lifts to half bright");
}

void testScopeSweep() {
	std::printf("scope sweep: ramp, auto-start, holdoff and restart\n");
	const double dt = static_cast<double>(1.f / 48000.f);  // what the module passes (args.sampleTime)
	const double window = 0.005;                           // 5 ms is 240 samples at 48 kHz
	const int N = 240;

	// No trigger: the sweep starts by itself once a window has passed, like a scope on AUTO.
	{
		ScopeSweep sweep;
		std::vector<int> starts;
		std::vector<float> ramp;
		for (int n = 0; n < 7 * N; ++n) {
			if (sweep.process(dt, window, false)) {
				starts.push_back(n);
			}
			ramp.push_back(sweep.rampVolts());
		}
		check(starts.size() >= 3, "sweep: it keeps running with no trigger");
		if (starts.size() >= 3) {
			std::printf("  no trigger: sweeps start at samples %d, %d, %d (window is %d samples)\n", starts[0], starts[1], starts[2], N);
			check(std::abs(starts[0] - N) <= 1, "sweep: with no trigger the first sweep starts after one window");
			check(starts[1] - starts[0] == 2 * N && starts[2] - starts[1] == 2 * N, "sweep: with no trigger it repeats every two windows (one sweeping, one waiting)");
			const size_t s = static_cast<size_t>(starts[0]);
			bool shapeOk = ramp[s] == -5.f;
			float last = ramp[s];
			for (int j = 1; j < N; ++j) {
				const float v = ramp[s + static_cast<size_t>(j)];
				shapeOk = shapeOk && v > last && std::abs(v - (-5.f + 10.f * static_cast<float>(j) / static_cast<float>(N))) < 1e-3f;
				last = v;
			}
			check(shapeOk, "sweep: the ramp climbs in equal steps from -5 V across one window");
			check(last > 4.9f && last < 5.f, "sweep: the last step of the sweep is just under +5 V");
			bool waiting = true;
			for (int j = N; j < 2 * N; ++j) {
				waiting = waiting && ramp[s + static_cast<size_t>(j)] == -5.f;
			}
			check(waiting, "sweep: after the sweep the ramp waits at -5 V");
		}
	}

	// A trigger every 3 ms (144 samples) against a 5 ms window: edges that arrive while a sweep
	// runs are ignored, and the next sweep starts on the first edge after it ended.
	{
		ScopeSweep sweep;
		std::vector<int> starts;
		for (int n = 0; n < 2000; ++n) {
			const bool edge = n >= 100 && (n - 100) % 144 == 0;
			if (sweep.process(dt, window, edge)) {
				starts.push_back(n);
			}
		}
		const std::vector<int> expected = {100, 388, 676, 964, 1252, 1540, 1828};
		std::printf("  3 ms triggers: %zu sweeps, first at %d, second at %d\n", starts.size(), starts.empty() ? -1 : starts[0], starts.size() < 2 ? -1 : starts[1]);
		check(starts == expected, "sweep: triggers inside a sweep are ignored; the next sweep starts on the first edge after the end");
	}

	// A trigger on the very sample the sweep ends starts the next sweep on that sample.
	{
		ScopeSweep sweep;
		check(sweep.process(dt, window, true), "sweep: a trigger starts a sweep");
		for (int n = 1; n < N; ++n) {
			sweep.process(dt, window, false);
		}
		const bool again = sweep.process(dt, window, true);
		check(again && sweep.running && sweep.rampVolts() == -5.f, "sweep: a trigger on the end sample starts the next sweep at once");
	}

	// restart() (the RESET jack) begins a new sweep on the next sample, mid-sweep or idle.
	{
		ScopeSweep sweep;
		sweep.process(dt, window, true);
		for (int n = 0; n < 50; ++n) {
			sweep.process(dt, window, false);
		}
		const float mid = sweep.rampVolts();
		sweep.restart();
		const bool started = sweep.process(dt, window, false);
		check(mid > -4.f && started && sweep.rampVolts() == -5.f, "sweep: restart() begins a new sweep on the next sample, mid-sweep too");
		const bool again = sweep.process(dt, window, false);
		check(!again && sweep.rampVolts() > -5.f, "sweep: restart() is one shot, the sweep then climbs from the bottom");
		ScopeSweep idle;
		idle.restart();
		check(idle.process(dt, window, false) && idle.running, "sweep: restart() from idle starts a sweep too");
	}

	// A window that is not a positive number stops the sweep.
	{
		ScopeSweep sweep;
		sweep.process(dt, window, true);
		check(!sweep.process(dt, 0.0, true) && !sweep.running && sweep.rampVolts() == -5.f, "sweep: a zero window stops the sweep");
		sweep.process(dt, window, true);
		check(!sweep.process(dt, std::nan(""), true) && !sweep.running, "sweep: a NaN window stops the sweep");
	}
}

void testScopeTriggerLevel() {
	std::printf("scope trace: trigger level\n");
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, 48000.f);
	// 400 Hz at 48 kHz is 120 samples a cycle at +-0.8, so the sine climbs 0.042 per sample at most.
	pushSine(slot, 0, 4800, 400.0, 0.8f, 0.f);
	const float step = 0.8f * 2.f * 3.14159265f / 120.f;

	// Level 0 is the old zero-crossing trigger, bit for bit. Reference: the finder as it was before
	// the level existed.
	auto original = [](const float* x, int length, int window, float h) {
		const int lastStart = length - window;
		if (x == nullptr || window < 2 || lastStart < 0) {
			return -1;
		}
		int trigger = -1;
		bool armed = false;
		for (int i = 0; i <= lastStart; ++i) {
			if (x[i] <= -h) {
				armed = true;
			}
			else if (armed && x[i] >= h) {
				trigger = i;
				armed = false;
			}
		}
		return trigger;
	};
	int sameAsOriginal = 0;
	int signals = 0;
	std::vector<float> sig(960);
	for (double freq : {97.0, 400.0, 1234.0}) {
		for (float amp : {0.3f, 0.8f}) {
			for (float offset : {-0.2f, 0.f, 0.3f}) {
				for (int n = 0; n < 960; ++n) {
					sig[static_cast<size_t>(n)] = offset + static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 * freq * n / 48000.0));
				}
				++signals;
				const int old = original(sig.data(), 960, 240, kScopeTriggerHysteresis);
				const bool same = scopeFindTrigger(sig.data(), 960, 240, kScopeTriggerHysteresis) == old && scopeFindTrigger(sig.data(), 960, 240, kScopeTriggerHysteresis, 0.f) == old;
				sameAsOriginal += same ? 1 : 0;
			}
		}
	}
	std::printf("  level 0 vs the original finder: %d of %d signals give the same start\n", sameAsOriginal, signals);
	check(sameAsOriginal == signals, "level 0 picks exactly the start the original zero-crossing trigger picked");

	// The window the renderer reads for the Oscilloscope with no TRIG cable. Its first sample is
	// where the trigger fired: the first sample at or above level + hysteresis.
	VideoGraphRenderer probe;
	auto windowStart = [&](float level, float& first, float& next) {
		ScopeTraceParams p = scopeDefaults(ScopeTraceMode::Oscilloscope);
		p.triggerPatched = false;
		p.triggerLevel = level;
		SignalWindow w;
		if (!VideoGraphRendererTestAccess::readScopeWindow(probe, slot, p, w) || w.length < 2) {
			return false;
		}
		first = w.sampleAt(0, 0);
		next = w.sampleAt(0, 1);
		return true;
	};
	float first0 = 0.f, next0 = 0.f;
	check(windowStart(0.f, first0, next0), "the renderer reads a window at level 0");
	for (float level : {0.f, 0.25f, 0.5f, -0.5f}) {
		float first = 0.f, next = 0.f;
		const bool ok = windowStart(level, first, next);
		std::printf("  level %+.2f: window starts at %.4f (fires at %.2f, one step is %.4f)\n", level, first, level + kScopeTriggerHysteresis, step);
		check(ok && first >= level + kScopeTriggerHysteresis - 1e-6f && first <= level + kScopeTriggerHysteresis + step + 1e-3f, "the window starts within a sample of the trigger level");
		check(ok && next > first, "the window starts on a rising edge");
	}
	float first5 = 0.f, next5 = 0.f;
	windowStart(0.5f, first5, next5);
	check(first5 - first0 > 0.4f, "raising the level moves the start further up the edge");

	// A level the signal never reaches cannot fire: free-run, the newest samples, as before.
	const float expectedFree = 0.f + static_cast<float>(0.8f * std::sin(2.0 * 3.14159265358979323846 * 400.0 * (4800 - 241) / 48000.0));
	for (float level : {1.f, -1.f}) {
		float first = 0.f, next = 0.f;
		const bool ok = windowStart(level, first, next);
		check(ok && std::abs(first - expectedFree) < 1e-5f, "a level above the peak (or below the trough) free-runs on the newest samples");
	}
	signalRingRelease(slot);

	// The same through the pixels: level 0.5 draws the same sine from a later point, and a render
	// repeated at the same level is identical.
	const int slot2 = signalRingAllocate();
	signalRingSetSampleRate(slot2, 48000.f);
	pushSine(slot2, 0, 4800, 400.0, 0.8f, 0.f);
	auto frameAt = [&](int bus, float level) {
		ScopeTraceParams p = scopeDefaults(ScopeTraceMode::Oscilloscope);
		p.triggerLevel = level;
		videoBusPublishScopeTrace(bus, slot2, p);
		VideoGraphRenderer r;
		Frame f;
		for (int i = 0; i < 12; ++i) {
			f = render(r, bus);
		}
		return f;
	};
	const Frame atZero = frameAt(56, 0.f);
	const Frame atHalf = frameAt(57, 0.5f);
	const Frame atHalfAgain = frameAt(57, 0.5f);
	const double moved = meanDiff(atZero, atHalf);
	std::printf("  frame at level 0 vs 0.5: mean diff %.2f; same level twice: max diff %d\n", moved, maxDiff(atHalf, atHalfAgain));
	check(!atZero.empty() && !atHalf.empty() && moved > 0.3, "moving the trigger level changes the drawn trace");
	check(maxDiff(atHalf, atHalfAgain) == 0, "the same level draws the same frame");
	saveProof("scope_trigger_level_proof.png", {atZero, atHalf});
	signalRingRelease(slot2);
}

void testScopeClearSerial() {
	std::printf("scope trace: RESET clears the phosphor\n");
	// A bright trace builds up in a long-persistence node, then the signal goes dark (Z = 0 draws
	// nothing). With the serial unchanged the trail stays and fades slowly. With a new serial the node
	// is wiped before the next window is drawn, so the frame is back to the bare graticule: exactly
	// what a node that never saw a trace shows for the same dark signal.
	// The graticule is about 0.13 in luma at its brightest (where the centre lines cross), so a lit
	// pixel here is one above 0.3, which only phosphor reaches.
	auto litPixels = [](const Frame& f) {
		int lit = 0;
		for (int y = 0; y < kH; ++y) {
			for (int x = 0; x < kW; ++x) {
				lit += lumaAt(f, x, y) > 0.3f ? 1 : 0;
			}
		}
		return lit;
	};
	auto run = [&](int bus, bool bump, int& litBefore, Frame& after) {
		const int slot = signalRingAllocate();
		signalRingSetSampleRate(slot, 48000.f);
		pushSine(slot, 0, 4800, 400.0, 0.8f, 0.f);
		ScopeTraceParams p = scopeDefaults(ScopeTraceMode::Oscilloscope);
		p.decay = 1.f;  // each render keeps 98% of the trail
		videoBusPublishScopeTrace(bus, slot, p);
		VideoGraphRenderer r;
		Frame f;
		for (int i = 0; i < 12; ++i) {
			f = render(r, bus);
		}
		litBefore = f.empty() ? 0 : litPixels(f);
		for (int n = 0; n < 4800; ++n) {
			const float quiet[kSignalRingChannels] = {0.f, 0.f, 0.f};
			signalRingPush(slot, quiet);
		}
		if (bump) {
			p.clearSerial += 1;
		}
		videoBusPublishScopeTrace(bus, slot, p);
		after = render(r, bus);
		signalRingRelease(slot);
	};

	int litBeforeKept = 0, litBeforeWiped = 0;
	Frame kept, wiped;
	run(58, false, litBeforeKept, kept);
	run(59, true, litBeforeWiped, wiped);

	// Reference: a node that never saw a trace, fed the same dark signal.
	Frame blank;
	{
		const int slot = signalRingAllocate();
		signalRingSetSampleRate(slot, 48000.f);
		for (int n = 0; n < 4800; ++n) {
			const float quiet[kSignalRingChannels] = {0.f, 0.f, 0.f};
			signalRingPush(slot, quiet);
		}
		ScopeTraceParams p = scopeDefaults(ScopeTraceMode::Oscilloscope);
		p.decay = 1.f;
		videoBusPublishScopeTrace(60, slot, p);
		VideoGraphRenderer r;
		blank = render(r, 60);
		signalRingRelease(slot);
	}

	const int litKept = kept.empty() ? 0 : litPixels(kept);
	const int litWiped = wiped.empty() ? -1 : litPixels(wiped);
	const int litBlank = blank.empty() ? -1 : litPixels(blank);
	std::printf("  lit pixels: before %d, after a quiet render with the serial unchanged %d, with a new serial %d, never-lit node %d\n", litBeforeKept, litKept, litWiped, litBlank);
	std::printf("  max channel difference from the never-lit node: serial unchanged %d, new serial %d\n", maxDiff(kept, blank), maxDiff(wiped, blank));
	check(litBlank == 0, "the bare graticule has no pixel above the lit threshold");
	check(litBeforeKept > 100 && litBeforeWiped > 100, "a bright trace is on screen before the signal goes dark");
	check(litKept > litBeforeKept / 2, "with the serial unchanged the trail is still there after a quiet render");
	check(!wiped.empty() && litWiped == 0, "with a new serial the next render has no lit pixel: RESET wiped the phosphor");
	check(maxDiff(wiped, blank) == 0, "with a new serial the next render is exactly the bare graticule");
	check(maxDiff(kept, blank) > 100, "with the serial unchanged the old trail still differs from the bare graticule");
	saveProof("scope_reset_clears_proof.png", {kept, wiped});
}

// ---- Scope Trace beam: DWELL, a true BEAM width, and asking for a ring slot again ----

// These tests stroke straight into an EnergyField (no ring, decay, glow or tone map) and read the energy
// back in "units": one unit is the energy one pixel of path gets at DWELL 0 (measuredUnit). The canvas is
// 640 x 400. That is under the 480-row reference height, so the beam width is not scaled, and with scale 1
// a signal of 2 px / (W - 1) - 1 lands the beam on pixel px.
constexpr int kBeamW = 640;
constexpr int kBeamH = 400;

// The fixed gain that replaced the old (0.6 + 0.8 x BEAM) factor on the energy. testScopeBeamBrightness
// says why it is 2.6.
constexpr double kExpectedBeamGain = 2.6;

// A polyline in screen pixels.
struct PixelPath {
	std::vector<float> x;
	std::vector<float> y;

	void add(float px, float py) {
		x.push_back(px);
		y.push_back(py);
	}
	int size() const {
		return static_cast<int>(x.size());
	}
	// Length of the segment from point i to point i + 1.
	double segment(int i) const {
		const size_t a = static_cast<size_t>(i);
		return std::hypot(static_cast<double>(x[a + 1] - x[a]), static_cast<double>(y[a + 1] - y[a]));
	}
	double length() const {
		double total = 0.0;
		for (int i = 0; i + 1 < size(); ++i) {
			total += segment(i);
		}
		return total;
	}
};

// The XY-mode window that puts `path` on the pixels of a w x h canvas when the scale is 1. Channel-major:
// X, Y, Z. Z is the brightness (1 draws at full intensity).
SignalWindow pixelWindow(const PixelPath& path, int w, int h, float z = 1.f) {
	const int n = path.size();
	SignalWindow window;
	window.channels = kSignalRingChannels;
	window.length = n;
	window.sampleRate = 48000.f;
	window.data.assign(static_cast<size_t>(kSignalRingChannels) * static_cast<size_t>(n), 0.f);
	for (int i = 0; i < n; ++i) {
		const size_t k = static_cast<size_t>(i);
		window.data[k] = 2.f * path.x[k] / static_cast<float>(w - 1) - 1.f;
		window.data[static_cast<size_t>(n) + k] = 1.f - 2.f * path.y[k] / static_cast<float>(h - 1);
		window.data[2u * static_cast<size_t>(n) + k] = z;
	}
	return window;
}

// XY mode at scale 1 and intensity 1, with the given DWELL and BEAM.
ScopeTraceParams beamParams(float dwell, float beam = 0.4f) {
	ScopeTraceParams p;
	p.mode = static_cast<int>(ScopeTraceMode::XY);
	p.scaleX = 1.f;
	p.scaleY = 1.f;
	p.intensity = 1.f;
	p.thickness = beam;
	p.dwell = dwell;
	return p;
}

crt::EnergyField drawWindow(const SignalWindow& window, const ScopeTraceParams& p, int w = kBeamW, int h = kBeamH) {
	static VideoGraphRenderer renderer;
	crt::EnergyField field;
	field.resize(w, h);
	VideoGraphRendererTestAccess::depositScopeTrace(renderer, field, w, h, p, window);
	return field;
}

crt::EnergyField drawPath(const PixelPath& path, const ScopeTraceParams& p, int w = kBeamW, int h = kBeamH) {
	return drawWindow(pixelWindow(path, w, h), p, w, h);
}

// Sum of the three channels at a pixel (0 outside the field).
double pixelEnergy(const crt::EnergyField& f, int x, int y) {
	if (x < 0 || y < 0 || x >= f.width() || y >= f.height()) {
		return 0.0;
	}
	const float* p = f.data() + (static_cast<size_t>(y) * static_cast<size_t>(f.width()) + static_cast<size_t>(x)) * 3u;
	return static_cast<double>(p[0]) + static_cast<double>(p[1]) + static_cast<double>(p[2]);
}

// Energy in columns [x0, x1) and rows [y0, y1), clamped to the field.
double regionEnergy(const crt::EnergyField& f, int x0, int x1, int y0, int y1) {
	double sum = 0.0;
	for (int y = std::max(y0, 0); y < std::min(y1, f.height()); ++y) {
		for (int x = std::max(x0, 0); x < std::min(x1, f.width()); ++x) {
			sum += pixelEnergy(f, x, y);
		}
	}
	return sum;
}

double wholeEnergy(const crt::EnergyField& f) {
	return regionEnergy(f, 0, f.width(), 0, f.height());
}

// The brightest pixel (all channels added) in the region, and where it is.
double peakEnergy(const crt::EnergyField& f, int x0, int x1, int y0, int y1, int* atX = nullptr, int* atY = nullptr) {
	double best = 0.0;
	for (int y = std::max(y0, 0); y < std::min(y1, f.height()); ++y) {
		for (int x = std::max(x0, 0); x < std::min(x1, f.width()); ++x) {
			const double e = pixelEnergy(f, x, y);
			if (e > best) {
				best = e;
				if (atX != nullptr) {
					*atX = x;
				}
				if (atY != nullptr) {
					*atY = y;
				}
			}
		}
	}
	return best;
}

// True when every value is finite and none is negative.
bool fieldIsFinite(const crt::EnergyField& f) {
	const size_t n = static_cast<size_t>(f.width()) * static_cast<size_t>(f.height()) * 3u;
	for (size_t i = 0; i < n; ++i) {
		if (!std::isfinite(f.data()[i]) || f.data()[i] < 0.f) {
			return false;
		}
	}
	return true;
}

// Variance, across a straight line from (x0, y0) to (x1, y1), of the energy in the slab of the line between
// fractions t0 and t1 of its length. Pixels more than 14 px from the line are left out. Distances are taken
// from the energy-weighted mean, so a beam a fraction of a pixel off the line still reads its width.
double lineVariance(const crt::EnergyField& f, double x0, double y0, double x1, double y1, double t0, double t1) {
	const double length = std::hypot(x1 - x0, y1 - y0);
	const double ux = (x1 - x0) / length;
	const double uy = (y1 - y0) / length;
	double sumW = 0.0;
	double sumD = 0.0;
	double sumDD = 0.0;
	for (int y = 0; y < f.height(); ++y) {
		for (int x = 0; x < f.width(); ++x) {
			const double rx = static_cast<double>(x) - x0;
			const double ry = static_cast<double>(y) - y0;
			const double along = (rx * ux + ry * uy) / length;
			const double across = -rx * uy + ry * ux;
			if (along < t0 || along > t1 || std::fabs(across) > 14.0) {
				continue;
			}
			const double w = pixelEnergy(f, x, y);
			sumW += w;
			sumD += w * across;
			sumDD += w * across * across;
		}
	}
	if (!(sumW > 0.0)) {
		return 0.0;
	}
	const double mean = sumD / sumW;
	return sumDD / sumW - mean * mean;
}

struct LineStats {
	double spread = 0.0;  // px: standard deviation across the line
	double peak = 0.0;    // the brightest pixel, all channels added
	double total = 0.0;   // energy in the whole field
};

// Draws one straight stroke at four sub-pixel shifts and averages. A spot of 0.5 px sigma is sampled by a
// pixel grid, so its measured width and peak depend on where the line falls between pixel centres. The
// average over (0, 0), (0.5, 0), (0, 0.5) and (0.5, 0.5) does not. The spread is the square root of the
// averaged variance.
LineStats measureLine(float x0, float y0, float x1, float y1, float beam, float dwell = 0.f, int w = kBeamW, int h = kBeamH) {
	const float shifts[4][2] = {{0.f, 0.f}, {0.5f, 0.f}, {0.f, 0.5f}, {0.5f, 0.5f}};
	LineStats stats;
	double variance = 0.0;
	for (const auto& shift : shifts) {
		PixelPath line;
		line.add(x0 + shift[0], y0 + shift[1]);
		line.add(x1 + shift[0], y1 + shift[1]);
		const crt::EnergyField f = drawPath(line, beamParams(dwell, beam), w, h);
		variance += lineVariance(f, x0 + shift[0], y0 + shift[1], x1 + shift[0], y1 + shift[1], 0.2, 0.8) / 4.0;
		stats.peak += peakEnergy(f, 0, w, 0, h) / 4.0;
		stats.total += wholeEnergy(f) / 4.0;
	}
	stats.spread = std::sqrt(variance);
	return stats;
}

// Energy one pixel of path gets at DWELL 0, all channels added, worked out from the law:
// 0.25 x INTENSITY x Z x gain x (r + g + b of the trace colour).
double evenUnit(const ScopeTraceParams& p) {
	float rgb[3];
	VideoGraphRendererTestAccess::traceColor(p.hue, rgb);
	return 0.25 * static_cast<double>(p.intensity) * kExpectedBeamGain * (static_cast<double>(rgb[0]) + static_cast<double>(rgb[1]) + static_cast<double>(rgb[2]));
}

// The same number, measured: 400 segments of 1 px at DWELL 0, well inside the canvas.
double measuredUnit(const ScopeTraceParams& p) {
	PixelPath line;
	for (int i = 0; i <= 400; ++i) {
		line.add(120.f + static_cast<float>(i), 200.f);
	}
	ScopeTraceParams q = p;
	q.dwell = 0.f;
	return wholeEnergy(drawPath(line, q)) / 400.0;
}

// What the DWELL law adds up to for a polyline of finite segments, in units: each segment gets
// min((mean / max(len, 0.25)) ^ dwell, 16) per pixel of path and is charged as at least 0.5 px, where
// mean is the mean segment length (at least 0.5 px).
double dwellOracle(const PixelPath& path, double dwell) {
	const int segments = path.size() - 1;
	if (segments < 1) {
		return 0.0;
	}
	double sum = 0.0;
	for (int i = 0; i < segments; ++i) {
		sum += path.segment(i);
	}
	const double mean = std::max(sum / segments, 0.5);
	double total = 0.0;
	for (int i = 0; i < segments; ++i) {
		const double len = path.segment(i);
		const double boost = std::min(std::pow(mean / std::max(len, 0.25), dwell), 16.0);
		total += boost * std::max(len, 0.5);
	}
	return total;
}

void testBeamRoundSpot() {
	std::printf("crt beam: the round spot Scope Trace strokes with\n");
	const float white[3] = {1.f, 1.f, 1.f};

	// A spot built from a sigma alone is still the Row spot, so the DeflectionYoke and Scan Processor draw what they drew.
	const crt::BeamSpot row(0.6f);
	check(row.shape() == crt::BeamSpot::Shape::Row, "a spot built with a sigma only is a Row spot");
	check(row.taps() == 5 && row.first() == -2 && row.reach() == 2, "a 0.6 px Row spot keeps its five taps and its 2 px reach");

	// Sigma 0 is the same bilinear splat in either shape.
	const crt::BeamSpot rowZero(0.f);
	const crt::BeamSpot roundZero(0.f, crt::BeamSpot::Shape::Round);
	bool zeroSame = rowZero.taps() == roundZero.taps() && rowZero.first() == roundZero.first() && rowZero.reach() == roundZero.reach();
	for (int phase = 0; phase <= crt::BeamSpot::kPhases && zeroSame; ++phase) {
		for (int t = 0; t < rowZero.taps(); ++t) {
			zeroSame = zeroSame && std::fabs(rowZero.weights(phase)[t] - roundZero.weights(phase)[t]) < 1e-7f;
		}
	}
	check(zeroSame, "a Round spot of sigma 0 has the weights of a Row spot of sigma 0");

	// A sigma too big for the tables is clamped, not overrun.
	const crt::BeamSpot huge(100.f, crt::BeamSpot::Shape::Round);
	check(huge.taps() == crt::BeamSpot::kMaxTaps && huge.reach() == crt::BeamSpot::kMaxRoundRadius && huge.first() == -crt::BeamSpot::kMaxRoundRadius, "a huge Round sigma is clamped to 41 taps");

	// Every deposit keeps its energy, whatever the width.
	bool kept = true;
	for (const float sigma : {0.5f, 1.5f, 3.f, 8.f}) {
		crt::EnergyField f;
		f.resize(100, 100);
		crt::depositPath(f, crt::BeamSpot(sigma, crt::BeamSpot::Shape::Round), 50.3f, 50.7f, 50.3f, 50.7f, white, 2.f, 0, 100);
		double sum = 0.0;
		float peak = 0.f;
		fieldTotals(f, sum, peak);
		kept = kept && std::fabs(sum - 6.0) < 6.0e-4;
	}
	check(kept, "a Round splat adds all of its energy at sigma 0.5, 1.5, 3 and 8");

	// The splat is as wide going up as going across, centred on the point, with a variance near sigma squared
	// (a little under: the table is cut at 2.67 sigma).
	{
		crt::EnergyField f;
		f.resize(100, 100);
		crt::depositPath(f, crt::BeamSpot(3.f, crt::BeamSpot::Shape::Round), 50.3f, 50.7f, 50.3f, 50.7f, white, 1.f, 0, 100);
		double total = 0.0, mx = 0.0, my = 0.0;
		for (int y = 0; y < 100; ++y) {
			for (int x = 0; x < 100; ++x) {
				const double e = f.data()[(static_cast<size_t>(y) * 100u + static_cast<size_t>(x)) * 3u];
				total += e;
				mx += e * x;
				my += e * y;
			}
		}
		mx /= total;
		my /= total;
		double vx = 0.0, vy = 0.0;
		for (int y = 0; y < 100; ++y) {
			for (int x = 0; x < 100; ++x) {
				const double e = f.data()[(static_cast<size_t>(y) * 100u + static_cast<size_t>(x)) * 3u];
				vx += e * (x - mx) * (x - mx);
				vy += e * (y - my) * (y - my);
			}
		}
		vx /= total;
		vy /= total;
		std::printf("  round splat at sigma 3: centre (%.3f, %.3f), variance x %.3f y %.3f (sigma squared 9)\n", mx, my, vx, vy);
		check(std::fabs(mx - 50.3) < 0.05 && std::fabs(my - 50.7) < 0.05, "a Round splat is centred on its point, to a 1/32 px phase");
		check(std::fabs(vx / vy - 1.0) < 0.02, "a Round splat is as wide along x as along y");
		check(vx / 9.0 > 0.90 && vx / 9.0 < 1.0, "a Round splat has a variance just under sigma squared");
	}

	// Edges: a deposit off the field adds nothing, and one on the edge keeps the half that is on the field.
	{
		crt::EnergyField f;
		f.resize(100, 100);
		const crt::BeamSpot spot(1.5f, crt::BeamSpot::Shape::Round);
		crt::depositPath(f, spot, -100.f, -100.f, -100.f, -100.f, white, 1.f, 0, 100);
		crt::depositPath(f, spot, 500.f, 50.f, 500.f, 50.f, white, 1.f, 0, 100);
		check(wholeEnergy(f) == 0.0, "a Round deposit far off the field leaves it empty");
		crt::depositPath(f, spot, -0.5f, 50.f, -0.5f, 50.f, white, 1.f, 0, 100);
		const double onEdge = wholeEnergy(f) / 3.0;
		check(std::fabs(onEdge - 0.5) < 0.02, "a Round deposit centred half a pixel off the left edge keeps half its energy");
	}

	// Row bands: a thread given rows [45, 50) writes nothing else, so the renderer can split rows across threads.
	{
		crt::EnergyField f;
		f.resize(100, 100);
		crt::depositPath(f, crt::BeamSpot(3.f, crt::BeamSpot::Shape::Round), 20.f, 50.f, 80.f, 50.f, white, 60.f, 45, 50);
		check(regionEnergy(f, 0, 100, 0, 45) == 0.0 && regionEnergy(f, 0, 100, 50, 100) == 0.0, "a Round deposit writes no row outside the band it was given");
		check(regionEnergy(f, 0, 100, 45, 50) > 1.0, "and writes the band");
	}

	// A line of Round splats two sigma apart keeps all its energy and is even to within 3 percent.
	{
		crt::EnergyField f;
		f.resize(100, 100);
		crt::depositPath(f, crt::BeamSpot(3.f, crt::BeamSpot::Shape::Round), 20.f, 50.f, 80.f, 50.f, white, 60.f, 0, 100);
		double lo = 1e30, hi = 0.0;
		for (int x = 30; x < 70; ++x) {
			double column = 0.0;
			for (int y = 0; y < 100; ++y) {
				column += f.data()[(static_cast<size_t>(y) * 100u + static_cast<size_t>(x)) * 3u];
			}
			lo = std::min(lo, column);
			hi = std::max(hi, column);
		}
		const double ripple = (hi - lo) / (hi + lo);
		std::printf("  round line at sigma 3: ripple %.2f%%, energy %.3f of 180\n", ripple * 100.0, wholeEnergy(f));
		check(std::fabs(wholeEnergy(f) - 180.0) < 0.02, "a Round line keeps its energy");
		check(ripple < 0.03, "a Round line is even along its length (under 3 percent ripple)");
	}
}

void testSignalRingRetry() {
	std::printf("signal ring slots: a module that found the pool full can ask again without waiting\n");
	std::vector<int> held;
	for (;;) {
		const int id = signalRingAllocate();
		if (id == 0) {
			break;
		}
		held.push_back(id);
	}
	check(!held.empty(), "ring slots could be claimed");
	check(signalRingAllocate() == 0, "with every slot claimed, allocate gives 0");
	check(signalRingTryAllocate() == 0, "with every slot claimed, tryAllocate gives 0 too");
	if (!held.empty()) {
		const int freed = held.back();
		held.pop_back();
		signalRingRelease(freed);
		const int again = signalRingTryAllocate();
		check(again == freed, "once a slot is released, tryAllocate hands it out");
		check(signalRingTryAllocate() == 0, "and hands it out once");
		if (again != 0) {
			held.push_back(again);
		}
	}
	for (const int id : held) {
		signalRingRelease(id);
	}
	const int fresh = signalRingTryAllocate();
	check(fresh != 0, "after everything is released, tryAllocate finds a slot");
	signalRingRelease(fresh);

	// The allocator the pool is built on.
	VideoSlotAllocator<3> three;
	const int a = three.claim();
	const int b = three.tryClaim();
	const int c = three.claim(0);
	check(a == 1 && b == 2 && c == 3, "claim and tryClaim hand out the lowest free id");
	check(three.claim() == 0 && three.tryClaim() == 0, "a full allocator gives 0 from both");
	three.release(b);
	check(three.tryClaim() == b, "tryClaim finds a released id");
	check(three.tryClaim() == 0, "and the allocator is full again");
}

// A slot that was used and released comes back like a new one. readFrame() cuts its windows at multiples of
// S counted from the ring's first sample, so a ring that kept its old write count would cut them somewhere
// else, and every test (and module) that claims a slot would depend on who had it before.
void testSignalRingSlotReuse() {
	std::printf("signal ring slots: a released slot comes back like a new one\n");
	constexpr float sr = 48000.f;
	const int S = 800;  // 48000 / 60
	const int total = S * 3 + 41;  // a partial frame at the end

	// Channel 1 is the sample number, so the window position shows in the values.
	const auto fill = [&](int slot, int count) {
		signalRingSetSampleRate(slot, sr);
		for (int n = 0; n < count; ++n) {
			const float values[kSignalRingChannels] = {0.f, static_cast<float>(n), 0.f};
			signalRingPush(slot, values);
		}
	};

	// The first owner writes a few frames and a bit over, stamps a trigger, and goes away.
	const int first = signalRingAllocate();
	check(first != 0, "a slot is free");
	fill(first, S * 5 + 123);
	signalRingMarkTrigger(first);
	signalRingRelease(first);

	const int second = signalRingAllocate();
	check(second == first, "the lowest free slot is handed out again");
	SignalWindow w;
	check(signalRingRate(second) == 0.f, "the new owner does not inherit the old sample rate");
	check(!signalRingReadFrame(second, w) && !signalRingReadLatest(second, 10, w), "and finds the ring empty");

	fill(second, total);
	check(signalRingReadFrame(second, w) && w.framePeriodSamples == S, "a frame can be read once it has written one");
	// The newest complete boundary is (total - 2) / S = 3, so the window is frame 2, starting at sample 2 * S.
	check(w.valid() && std::fabs(w.sampleAt(1, 0) - static_cast<float>(2 * S)) < 1e-3f,
		"frames are cut at multiples of S counted from the new owner's first sample");
	check(!signalRingReadTriggered(second, 100, w), "and no trigger stamp is left from the old owner");
	signalRingRelease(second);
}

// `got` is within `tol` of `want`, relative to `want`.
bool relNear(double got, double want, double tol) {
	return std::fabs(got - want) <= tol * std::fabs(want);
}

// Points along row `y` from x = `from` to x = `to`, `step` apart.
void addRow(PixelPath& path, float from, float to, float step, float y) {
	const int count = static_cast<int>(std::lround((to - from) / step));
	for (int i = 0; i <= count; ++i) {
		path.add(from + step * static_cast<float>(i), y);
	}
}

// Mean energy per column, in units, over columns [x0, x1) and rows [y0, y1): the energy each pixel of a
// horizontal path gets.
double columnLevel(const crt::EnergyField& f, int x0, int x1, int y0, int y1, double unit) {
	return regionEnergy(f, x0, x1, y0, y1) / static_cast<double>(x1 - x0) / unit;
}

void testScopeDwell() {
	std::printf("scope trace dwell: the beam burns brighter where it moves slowly\n");
	const ScopeTraceParams plain = beamParams(0.f);
	const double unit = measuredUnit(plain);
	std::printf("  one pixel of path at DWELL 0: %.4f energy (0.25 x INTENSITY x gain %.1f x colour gives %.4f)\n", unit, kExpectedBeamGain, evenUnit(plain));
	check(relNear(unit, evenUnit(plain), 0.005), "a pixel of path at DWELL 0 gets 0.25 x INTENSITY x Z x 2.6 x the trace colour");

	// One path, three speeds, all along row 200: 1 px steps, then 5 px steps, then 20 px steps. The 5 px join
	// in front of the second stretch and the 20 px join in front of the third are strokes of the same length
	// as the stretch they lead into. 100 strokes, 240 px, so the mean stroke is 2.4 px, and the law gives a
	// pixel of path (2.4 / stroke) ^ DWELL units: 2.4 on the 1 px stretch at DWELL 1, 0.48 on the 5 px
	// stretch and 0.12 on the 20 px stretch.
	PixelPath blocks;
	addRow(blocks, 40.f, 120.f, 1.f, 200.f);
	addRow(blocks, 125.f, 200.f, 5.f, 200.f);
	addRow(blocks, 220.f, 280.f, 20.f, 200.f);
	const double meanStroke = blocks.length() / static_cast<double>(blocks.size() - 1);
	check(blocks.size() == 101 && relNear(blocks.length(), 240.0, 1e-6) && relNear(meanStroke, 2.4, 1e-6), "the test path is 100 strokes, 240 px, mean 2.4 px");

	struct Reading {
		double a = 0.0;      // units per pixel of path on the 1 px stretch (columns 46..113)
		double b = 0.0;      // on the 5 px stretch (columns 128..191)
		double c = 0.0;      // on the 20 px stretch (columns 206..273)
		double total = 0.0;  // the whole path, in units
	};
	const float dwells[5] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
	Reading seen[5];
	bool levelsRight = true;
	bool totalsRight = true;
	for (int i = 0; i < 5; ++i) {
		const crt::EnergyField f = drawPath(blocks, beamParams(dwells[i]));
		seen[i].a = columnLevel(f, 46, 114, 190, 211, unit);
		seen[i].b = columnLevel(f, 128, 192, 190, 211, unit);
		seen[i].c = columnLevel(f, 206, 274, 190, 211, unit);
		seen[i].total = wholeEnergy(f) / unit;
		const double d = static_cast<double>(dwells[i]);
		const double wantA = std::pow(meanStroke / 1.0, d);
		const double wantB = std::pow(meanStroke / 5.0, d);
		const double wantC = std::pow(meanStroke / 20.0, d);
		const double oracle = dwellOracle(blocks, d);
		std::printf("  DWELL %.2f: units per pixel of path: 1 px steps %.3f (law %.3f), 5 px steps %.3f (%.3f), 20 px steps %.3f (%.3f); whole path %.2f units (law %.2f)\n", d, seen[i].a, wantA, seen[i].b, wantB, seen[i].c, wantC, seen[i].total, oracle);
		levelsRight = levelsRight && fieldIsFinite(f) && relNear(seen[i].a, wantA, 0.03) && relNear(seen[i].b, wantB, 0.03) && relNear(seen[i].c, wantC, 0.03);
		totalsRight = totalsRight && relNear(seen[i].total, oracle, 0.005);
	}
	check(levelsRight, "at every DWELL a pixel of path gets (mean stroke / its own stroke) ^ DWELL units, to 3 percent");
	check(totalsRight, "at every DWELL the whole path adds up to what the law predicts, to 0.5 percent");
	check(relNear(seen[0].a, 1.0, 0.03) && relNear(seen[0].b, 1.0, 0.03) && relNear(seen[0].c, 1.0, 0.03), "DWELL 0 gives every pixel of path the same energy, whatever the stroke length");
	check(relNear(seen[0].total, 240.0, 0.005) && relNear(seen[4].total, 240.0, 0.005), "DWELL 0 and DWELL 1 both spend 1 unit per pixel of path over the whole path (240 units)");
	bool aRises = true;
	bool cFalls = true;
	for (int i = 0; i + 1 < 5; ++i) {
		aRises = aRises && seen[i + 1].a > seen[i].a;
		cFalls = cFalls && seen[i + 1].c < seen[i].c;
	}
	check(aRises, "turning DWELL up brightens the slow stretch at every step");
	check(cFalls, "turning DWELL up dims the fast stretch at every step");

	// The cap. This path starts with 0.5 px steps, then runs three 360 px strokes, so its mean stroke is 15.8 px
	// and the raw boost for a 0.5 px step is 31.7. The cap holds it to 16.
	PixelPath crawl;
	addRow(crawl, 40.f, 80.f, 0.5f, 200.f);
	crawl.add(100.f, 20.f);
	crawl.add(100.f, 380.f);
	crawl.add(130.f, 380.f);
	crawl.add(130.f, 20.f);
	crawl.add(160.f, 20.f);
	crawl.add(160.f, 380.f);
	const double crawlMean = crawl.length() / static_cast<double>(crawl.size() - 1);
	const double rawBoost = crawlMean / 0.5;
	const double crawl0 = columnLevel(drawPath(crawl, beamParams(0.f)), 46, 74, 190, 211, unit);
	const double crawlHalf = columnLevel(drawPath(crawl, beamParams(0.5f)), 46, 74, 190, 211, unit);
	const double crawl1 = columnLevel(drawPath(crawl, beamParams(1.f)), 46, 74, 190, 211, unit);
	std::printf("  0.5 px steps against a %.1f px mean stroke (raw boost %.1f): %.3f units per pixel of path at DWELL 0, %.3f at 0.5, %.3f at 1\n", crawlMean, rawBoost, crawl0, crawlHalf, crawl1);
	check(rawBoost > 16.0 && relNear(crawl0, 1.0, 0.03), "the cap test path is slow enough to need the cap");
	check(relNear(crawl1 / crawl0, 16.0, 0.03), "the boost is capped at 16 times the DWELL 0 energy");
	check(relNear(crawlHalf / crawl0, std::sqrt(rawBoost), 0.03), "below the cap, DWELL 0.5 boosts by the square root of the speed ratio");

	// A beam that stops still burns a dot. A stroke of length 0 is charged as 0.5 px of path, so it gets half
	// a unit at DWELL 0. Its mean stroke is 0, floored to 0.5 px, and its own length is floored to 0.25 px
	// for the boost, so at DWELL 1 the dot is twice as bright: one unit per stroke.
	for (const int points : {2, 100}) {
		PixelPath stopped;
		for (int i = 0; i < points; ++i) {
			stopped.add(300.5f, 150.25f);
		}
		const double strokes = static_cast<double>(points - 1);
		const crt::EnergyField f0 = drawPath(stopped, beamParams(0.f));
		const crt::EnergyField f1 = drawPath(stopped, beamParams(1.f));
		const double e0 = wholeEnergy(f0) / unit;
		const double e1 = wholeEnergy(f1) / unit;
		int px = -1, py = -1;
		peakEnergy(f1, 0, kBeamW, 0, kBeamH, &px, &py);
		std::printf("  a beam stopped for %d strokes: %.2f units at DWELL 0, %.2f at DWELL 1, brightest pixel (%d, %d)\n", points - 1, e0, e1, px, py);
		check(fieldIsFinite(f0) && fieldIsFinite(f1), "a stopped beam leaves a finite field");
		check(relNear(e0, 0.5 * strokes, 0.005), "a stopped beam burns half a unit per stroke at DWELL 0");
		check(relNear(e1, 1.0 * strokes, 0.005), "a stopped beam burns one unit per stroke at DWELL 1");
		check(std::fabs(px - 300.5) <= 1.0 && std::fabs(py - 150.25) <= 1.0, "and the dot is where the beam stopped");
	}

	// Bad samples. A NaN or an Inf drops the strokes on either side of it. It must not reach the mean stroke that
	// sets the brightness of everything else, and the dropped strokes must not be counted in it either.
	{
		PixelPath line;
		addRow(line, 100.f, 300.f, 1.f, 200.f);
		const SignalWindow clean = pixelWindow(line, kBeamW, kBeamH);
		SignalWindow dirty = clean;
		const size_t n = static_cast<size_t>(dirty.length);
		dirty.data[60] = std::numeric_limits<float>::quiet_NaN();
		dirty.data[n + 60] = std::numeric_limits<float>::quiet_NaN();
		dirty.data[n + 140] = std::numeric_limits<float>::infinity();
		for (const float dwell : {0.f, 1.f}) {
			const crt::EnergyField a = drawWindow(clean, beamParams(dwell));
			const crt::EnergyField b = drawWindow(dirty, beamParams(dwell));
			const double ea = regionEnergy(a, 104, 150, 0, kBeamH) + regionEnergy(a, 250, 292, 0, kBeamH);
			const double eb = regionEnergy(b, 104, 150, 0, kBeamH) + regionEnergy(b, 250, 292, 0, kBeamH);
			std::printf("  NaN and Inf samples, DWELL %.0f: clean line %.3f units, line with gaps %.3f in the same columns\n", static_cast<double>(dwell), ea / unit, eb / unit);
			check(fieldIsFinite(b), "a NaN or an Inf in the window leaves a finite field");
			check(ea > 0.0 && relNear(eb, ea, 0.005), "the rest of the line is as bright with bad samples in the window as without");
			check(wholeEnergy(b) < wholeEnergy(a), "and the strokes next to the bad samples are dropped");
		}
	}

	// Speeding up. The same circle drawn with 400 steps and with 100 steps, a beam four times faster. A law that
	// charged by time, or that did not divide by the mean, would make the fast circle a quarter as bright.
	const double pi = 3.14159265358979323846;
	auto circle = [&](int steps) {
		PixelPath c;
		for (int i = 0; i <= steps; ++i) {
			const double t = 2.0 * pi * static_cast<double>(i) / static_cast<double>(steps);
			c.add(static_cast<float>(320.0 + 100.0 * std::cos(t)), static_cast<float>(200.0 + 100.0 * std::sin(t)));
		}
		return c;
	};
	{
		const PixelPath slowCircle = circle(400);
		const PixelPath fastCircle = circle(100);
		const double slow0 = wholeEnergy(drawPath(slowCircle, beamParams(0.f))) / unit;
		const double slow1 = wholeEnergy(drawPath(slowCircle, beamParams(1.f))) / unit;
		const double fast0 = wholeEnergy(drawPath(fastCircle, beamParams(0.f))) / unit;
		const double fast1 = wholeEnergy(drawPath(fastCircle, beamParams(1.f))) / unit;
		std::printf("  a circle in 400 steps (%.2f px each): %.1f units at DWELL 0, %.1f at DWELL 1; in 100 steps (%.2f px each): %.1f and %.1f\n", slowCircle.segment(0), slow0, slow1, fastCircle.segment(0), fast0, fast1);
		check(relNear(fast1, slow1, 0.01), "at DWELL 1 a circle drawn four times as fast has the same total energy");
		check(relNear(slow1, slow0, 0.01) && relNear(fast1, fast0, 0.01), "for steps of one size DWELL changes nothing: the mean is the stroke");
	}

	// A Lissajous traced once, or four times over, in the same 1000 samples: four times the speed. Its speed
	// varies along the path, so this checks the whole law (the 0.25 px and 0.5 px floors included) against
	// the oracle. The 1 cycle figure has strokes under 0.5 px that the floors brighten, so the two figures
	// are compared loosely. A faster figure in a fixed window covers more path and so has more energy in
	// all; what stays put is the energy per pixel of path.
	{
		auto lissajous = [&](int cycles) {
			PixelPath p;
			for (int i = 0; i < 1000; ++i) {
				const double t = 2.0 * pi * static_cast<double>(cycles) * static_cast<double>(i) / 999.0;
				p.add(static_cast<float>(320.0 + 150.0 * std::sin(3.0 * t)), static_cast<float>(200.0 + 90.0 * std::sin(2.0 * t + 0.6)));
			}
			return p;
		};
		double perPixel[2] = {0.0, 0.0};
		const int cycleCounts[2] = {1, 4};
		for (int k = 0; k < 2; ++k) {
			const PixelPath figure = lissajous(cycleCounts[k]);
			const double length = figure.length();
			const double total0 = wholeEnergy(drawPath(figure, beamParams(0.f))) / unit;
			const double total1 = wholeEnergy(drawPath(figure, beamParams(1.f))) / unit;
			const double oracle = dwellOracle(figure, 1.0);
			perPixel[k] = total1 / length;
			std::printf("  Lissajous, %d cycle(s): path %.0f px, mean stroke %.2f px; %.0f units at DWELL 0, %.0f at DWELL 1 (law %.0f); %.3f units per pixel of path at DWELL 1\n", cycleCounts[k], length, length / (figure.size() - 1), total0, total1, oracle, perPixel[k]);
			check(relNear(total1, oracle, 0.005), "a Lissajous at DWELL 1 adds up to what the law predicts, to 0.5 percent");
		}
		check(relNear(perPixel[1], perPixel[0], 0.10), "a figure four times as fast burns about the same energy per pixel of path");
	}

	// Dual Trace has two traces, so two means. A flat trace B must not get brighter because trace A zig-zags
	// in 84 px jumps: the rows below 220 are trace B alone.
	{
		const int n = 400;
		auto dual = [&](bool zigzag) {
			SignalWindow w;
			w.channels = kSignalRingChannels;
			w.length = n;
			w.sampleRate = 48000.f;
			w.data.assign(static_cast<size_t>(kSignalRingChannels) * static_cast<size_t>(n), 0.f);
			for (int i = 0; i < n; ++i) {
				w.data[static_cast<size_t>(i)] = zigzag ? ((i % 2 == 0) ? 0.5f : -0.5f) : 0.f;
				w.data[2u * static_cast<size_t>(n) + static_cast<size_t>(i)] = 1.f;
			}
			return w;
		};
		ScopeTraceParams p = beamParams(1.f);
		p.mode = static_cast<int>(ScopeTraceMode::DualTrace);
		const crt::EnergyField flat = drawWindow(dual(false), p);
		const crt::EnergyField zig = drawWindow(dual(true), p);
		const double bFlat = regionEnergy(flat, 0, kBeamW, 220, kBeamH);
		const double bZig = regionEnergy(zig, 0, kBeamW, 220, kBeamH);
		const double aFlat = regionEnergy(flat, 0, kBeamW, 0, 220);
		const double aZig = regionEnergy(zig, 0, kBeamW, 0, 220);
		std::printf("  Dual Trace at DWELL 1, trace A flat or zig-zag: trace B %.1f and %.1f; trace A %.1f and %.1f\n", bFlat, bZig, aFlat, aZig);
		check(bFlat > 0.0 && aFlat > 0.0 && aZig > 0.0, "both traces draw");
		check(relNear(bZig, bFlat, 0.02), "trace B is as bright whether trace A is flat or zig-zags (each trace has its own mean stroke)");
	}
}

void testScopeBeamWidth() {
	std::printf("scope trace beam width: BEAM sets how wide the beam is, not how much energy it carries\n");
	auto closeTo = [](float a, float b) {
		return std::fabs(a - b) < 1e-5f;
	};

	// The mapping: sigma = 0.5 + 2.5 x BEAM px on a 480-row frame, scaled up with the height above that.
	check(closeTo(scopeBeamSigmaPx(0.f, 480), 0.5f) && closeTo(scopeBeamSigmaPx(0.4f, 480), 1.5f) && closeTo(scopeBeamSigmaPx(1.f, 480), 3.f), "BEAM 0, 0.4 and 1 give a sigma of 0.5, 1.5 and 3 px on a 480-row frame");
	check(closeTo(scopeBeamSigmaPx(0.f, 1080), 1.125f) && closeTo(scopeBeamSigmaPx(0.4f, 1080), 3.375f) && closeTo(scopeBeamSigmaPx(1.f, 1080), 6.75f), "on a 1080-row frame the sigma is 2.25 times as big, the same share of the picture");
	check(closeTo(scopeBeamSigmaPx(0.4f, 144), 1.5f) && closeTo(scopeBeamSigmaPx(0.4f, 400), 1.5f), "a frame under 480 rows keeps the 480-row sigma");
	check(closeTo(scopeBeamSigmaPx(-1.f, 480), 0.5f) && closeTo(scopeBeamSigmaPx(5.f, 480), 3.f), "BEAM is clamped to 0..1");
	check(closeTo(scopeBeamSigmaPx(1.f, 4000), kScopeBeamMaxSigmaPx) && closeTo(kScopeBeamMaxSigmaPx, 8.f), "the sigma stops at 8 px on a very tall frame");
	check(closeTo(scopeBeamScale(1080), 2.25f) && closeTo(scopeBeamScale(480), 1.f) && closeTo(scopeBeamScale(144), 1.f), "the beam scale is the height over 480, at least 1");

	// Measured through depositScopeTrace: one straight stroke, DWELL 0, at BEAM 0, 0.4 and 1. The spot is
	// round, so the line is as wide going across the canvas as going down it, and as wide on a diagonal.
	const double unit = measuredUnit(beamParams(0.f));
	struct Probe {
		const char* name;
		float x0, y0, x1, y1;
	};
	const Probe probes[3] = {
		{"horizontal", 100.f, 200.f, 540.f, 200.f},
		{"vertical", 320.f, 20.f, 320.f, 380.f},
		{"diagonal", 150.f, 40.f, 470.f, 360.f},
	};
	const float beams[3] = {0.f, 0.4f, 1.f};
	const double nominal[3] = {0.5, 1.5, 3.0};
	LineStats stats[3][3];
	for (int l = 0; l < 3; ++l) {
		const double length = std::hypot(static_cast<double>(probes[l].x1 - probes[l].x0), static_cast<double>(probes[l].y1 - probes[l].y0));
		for (int b = 0; b < 3; ++b) {
			stats[l][b] = measureLine(probes[l].x0, probes[l].y0, probes[l].x1, probes[l].y1, beams[b]);
			std::printf("  %-10s BEAM %.1f: spread %.3f px (sigma %.1f, ratio %.3f), peak %.3f units, energy %.2f units for %.1f px\n", probes[l].name, static_cast<double>(beams[b]), stats[l][b].spread, nominal[b], stats[l][b].spread / nominal[b], stats[l][b].peak / unit, stats[l][b].total / unit, length);
		}
	}
	bool spreadsRight = true;
	bool ratioRight = true;
	bool energyRight = true;
	bool peaksFall = true;
	bool roundRight = true;
	for (int l = 0; l < 3; ++l) {
		const double length = std::hypot(static_cast<double>(probes[l].x1 - probes[l].x0), static_cast<double>(probes[l].y1 - probes[l].y0));
		for (int b = 0; b < 3; ++b) {
			spreadsRight = spreadsRight && relNear(stats[l][b].spread, nominal[b], 0.08);
			energyRight = energyRight && relNear(stats[l][b].total / unit, length, 0.01);
		}
		const double ratio = stats[l][2].spread / stats[l][0].spread;
		ratioRight = ratioRight && ratio >= 5.0 && ratio <= 7.0;
		peaksFall = peaksFall && stats[l][0].peak > stats[l][1].peak && stats[l][1].peak > stats[l][2].peak;
	}
	for (int b = 0; b < 3; ++b) {
		roundRight = roundRight && relNear(stats[1][b].spread, stats[0][b].spread, 0.05);
	}
	check(spreadsRight, "the beam is BEAM's sigma wide, to 8 percent (the table is cut at 2.67 sigma, which costs 3 percent)");
	check(ratioRight, "BEAM 1 draws a line 5 to 7 times as wide as BEAM 0");
	check(energyRight, "BEAM changes the width and not the energy: every line carries 1 unit per pixel of its length, to 1 percent");
	check(peaksFall, "the wider the beam, the dimmer its brightest pixel");
	check(roundRight, "a vertical line is as wide as a horizontal one (the spot is round)");

	// The same beam on a 1080-row frame is 2.25 times as wide, and carries 2.25 times the energy, so its peak
	// is the same as on the short frame: the trace looks the same at every size.
	{
		const LineStats shortFrame = measureLine(60.f, 200.f, 240.f, 200.f, 0.4f, 0.f, 300, 400);
		const LineStats tallFrame = measureLine(60.f, 540.f, 240.f, 540.f, 0.4f, 0.f, 300, 1080);
		std::printf("  400 rows -> 1080 rows at BEAM 0.4: spread %.3f -> %.3f px (x%.3f), peak %.3f -> %.3f units (x%.3f), energy x%.3f\n", shortFrame.spread, tallFrame.spread, tallFrame.spread / shortFrame.spread, shortFrame.peak / unit, tallFrame.peak / unit, tallFrame.peak / shortFrame.peak, tallFrame.total / shortFrame.total);
		check(relNear(tallFrame.spread / shortFrame.spread, 2.25, 0.03), "on a 1080-row frame the line is 2.25 times as wide");
		check(relNear(tallFrame.peak / shortFrame.peak, 1.0, 0.03), "and its peak is as bright as on the short frame");
		check(relNear(tallFrame.total / shortFrame.total, 2.25, 0.005), "because it carries 2.25 times the energy");
	}
}

// The Scope Trace stroke as it was before the Beam rework: a two-tap linear spot, and 0.25 x INTENSITY x Z x
// (0.6 + 0.8 x BEAM) energy per pixel of path.
crt::EnergyField drawOldLaw(float x0, float y0, float x1, float y1, float beam, const ScopeTraceParams& p, int w, int h) {
	crt::EnergyField field;
	field.resize(w, h);
	float rgb[3];
	VideoGraphRendererTestAccess::traceColor(p.hue, rgb);
	const double length = std::hypot(static_cast<double>(x1 - x0), static_cast<double>(y1 - y0));
	const float total = static_cast<float>(0.25 * static_cast<double>(p.intensity) * (0.6 + 0.8 * static_cast<double>(beam)) * length);
	crt::depositPath(field, crt::BeamSpot(0.f), x0, y0, x1, y1, rgb, total, 0, h);
	return field;
}

// measureLine for the old stroke: the same four sub-pixel shifts, and the same spread, peak and total.
LineStats measureOldLine(float x0, float y0, float x1, float y1, float beam, int w = kBeamW, int h = kBeamH) {
	const float shifts[4][2] = {{0.f, 0.f}, {0.5f, 0.f}, {0.f, 0.5f}, {0.5f, 0.5f}};
	LineStats stats;
	double variance = 0.0;
	for (const auto& shift : shifts) {
		const float ax = x0 + shift[0], ay = y0 + shift[1], bx = x1 + shift[0], by = y1 + shift[1];
		const crt::EnergyField f = drawOldLaw(ax, ay, bx, by, beam, beamParams(0.f, beam), w, h);
		variance += lineVariance(f, ax, ay, bx, by, 0.2, 0.8) / 4.0;
		stats.peak += peakEnergy(f, 0, w, 0, h) / 4.0;
		stats.total += wholeEnergy(f) / 4.0;
	}
	stats.spread = std::sqrt(variance);
	return stats;
}

void testScopeBeamBrightness() {
	std::printf("scope trace beam brightness: the default beam's horizontal and vertical lines keep their brightest pixel\n");
	// BEAM used to scale the energy by (0.6 + 0.8 x BEAM) and the spot was always two taps wide. Now BEAM sets
	// the sigma and the energy carries one fixed gain, 2.6, chosen so that a horizontal or vertical line at the
	// default BEAM (0.4) has about the brightest pixel it had before; a diagonal line ends up at 0.81 of it. The
	// new spot is wider, so it carries more energy to get there.
	// A line's brightest pixel depends on where the line falls between pixel centres, so each reading is the
	// mean of 16 sub-pixel positions of the line.
	struct Probe {
		const char* name;
		float x0, y0, x1, y1;
		bool shiftX;  // slide the line sideways (else down) through the 16 positions
	};
	const Probe probes[3] = {
		{"horizontal", 50.f, 200.f, 550.f, 200.f, false},
		{"vertical", 300.f, 30.f, 300.f, 370.f, true},
		{"diagonal", 100.f, 30.f, 440.f, 370.f, true},
	};
	auto meanPeak = [&](const Probe& pr, float beam, bool oldLaw) {
		double sum = 0.0;
		for (int i = 0; i < 16; ++i) {
			const float fr = static_cast<float>(i) / 16.f;
			const float dx = pr.shiftX ? fr : 0.f;
			const float dy = pr.shiftX ? 0.f : fr;
			const float x0 = pr.x0 + dx, y0 = pr.y0 + dy, x1 = pr.x1 + dx, y1 = pr.y1 + dy;
			crt::EnergyField f;
			if (oldLaw) {
				f = drawOldLaw(x0, y0, x1, y1, beam, beamParams(0.f, beam), kBeamW, kBeamH);
			} else {
				PixelPath line;
				line.add(x0, y0);
				line.add(x1, y1);
				f = drawPath(line, beamParams(0.f, beam));
			}
			const int mx = static_cast<int>(0.5f * (x0 + x1));
			const int my = static_cast<int>(0.5f * (y0 + y1));
			sum += peakEnergy(f, mx - 30, mx + 30, my - 30, my + 30);
		}
		return sum / 16.0;
	};
	double thin[3], same[3], wide[3];
	for (int l = 0; l < 3; ++l) {
		const double before = meanPeak(probes[l], 0.4f, true);
		thin[l] = meanPeak(probes[l], 0.f, false) / before;
		same[l] = meanPeak(probes[l], 0.4f, false) / before;
		wide[l] = meanPeak(probes[l], 1.f, false) / before;
		std::printf("  %-10s brightest pixel against the old BEAM 0.4 line: BEAM 0 x%.3f, BEAM 0.4 x%.3f, BEAM 1 x%.3f\n", probes[l].name, thin[l], same[l], wide[l]);
	}
	check(same[0] > 0.92 && same[0] < 1.08, "the default beam's horizontal line has the brightest pixel it had before, to 8 percent");
	check(same[1] > 0.92 && same[1] < 1.08, "and its vertical line too");
	check(same[2] > 0.70 && same[2] < 0.95, "its diagonal line is a little dimmer than before (the old two-tap spot lit diagonals more)");
	bool ordered = true;
	for (int l = 0; l < 3; ++l) {
		ordered = ordered && thin[l] > same[l] && same[l] > wide[l];
	}
	check(ordered, "with a fixed gain a thinner beam is brighter at its peak and a wider one dimmer");
	check(thin[0] > 2.0 && thin[0] < 3.2 && wide[0] > 0.4 && wide[0] < 0.65, "the horizontal line at BEAM 0 is about 2.6 times and at BEAM 1 about half the old default's peak");

	// An old patch keeps the BEAM it was saved with and loads DWELL 0 (ScopeTrace::paramsFromJson), so it meets
	// the new beam at the same BEAM, not at the old default. The old spot was two taps wide whatever BEAM said and
	// BEAM only scaled the energy; now BEAM is the sigma and the energy is fixed. A line therefore gets wider,
	// carries more light per pixel of path, and its brightest pixel goes up or down with the BEAM it was saved at.
	{
		const float beams[3] = {0.f, 0.4f, 1.f};
		double peaks[3][3], widthNow[3], widthThen[3], light[3];
		for (int b = 0; b < 3; ++b) {
			for (int l = 0; l < 3; ++l) {
				peaks[b][l] = meanPeak(probes[l], beams[b], false) / meanPeak(probes[l], beams[b], true);
			}
			const LineStats now = measureLine(50.f, 200.f, 550.f, 200.f, beams[b]);
			const LineStats then = measureOldLine(50.f, 200.f, 550.f, 200.f, beams[b]);
			widthNow[b] = now.spread;
			widthThen[b] = then.spread;
			light[b] = now.total / then.total;
			std::printf("  old patch at its own BEAM %.1f, new beam against old: spread %.2f px against %.2f px, light per pixel of path x%.2f, brightest pixel x%.3f horizontal, x%.3f vertical, x%.3f diagonal\n", static_cast<double>(beams[b]), widthNow[b], widthThen[b], light[b], peaks[b][0], peaks[b][1], peaks[b][2]);
		}
		bool lightRight = true;
		for (int b = 0; b < 3; ++b) {
			lightRight = lightRight && relNear(light[b], kExpectedBeamGain / (0.6 + 0.8 * static_cast<double>(beams[b])), 0.01);
		}
		check(lightRight, "at its own BEAM an old patch's line carries 2.6 / (0.6 + 0.8 x BEAM) times the old light per pixel of path (4.3, 2.8 and 1.9 times at BEAM 0, 0.4 and 1), to 1 percent");
		check(widthThen[0] > 0.3 && widthThen[0] < 0.45 && relNear(widthThen[1], widthThen[0], 0.01) && relNear(widthThen[2], widthThen[0], 0.01), "the old line was about 0.4 px wide at every BEAM");
		check(widthNow[0] > 0.4 && widthNow[0] < 0.6 && widthNow[1] > 1.3 && widthNow[1] < 1.7 && widthNow[2] > 2.7 && widthNow[2] < 3.3, "the new line is about 0.5, 1.5 and 3 px wide at BEAM 0, 0.4 and 1");
		check(peaks[0][0] > 3.2 && peaks[0][0] < 4.6 && peaks[0][1] > 3.2 && peaks[0][1] < 4.6, "a horizontal or vertical line saved at BEAM 0 has a brightest pixel about 3.9 times as bright");
		check(peaks[1][0] > 0.98 && peaks[1][0] < 1.03 && peaks[1][1] > 0.98 && peaks[1][1] < 1.03, "saved at the default BEAM 0.4 it has the brightest pixel it had (horizontal and vertical lines, within 3 percent)");
		check(peaks[1][2] > 0.70 && peaks[1][2] < 0.95, "and a diagonal at the default BEAM is a little dimmer");
		check(peaks[2][0] > 0.25 && peaks[2][0] < 0.45 && peaks[2][1] > 0.25 && peaks[2][1] < 0.45, "saved at BEAM 1 it has a brightest pixel about a third as bright, though it carries 1.9 times the light");
	}

	// The gain itself, from the energy of one pixel of path: 0.25 x INTENSITY x Z x 2.6.
	check(relNear(measuredUnit(beamParams(0.f)), evenUnit(beamParams(0.f)), 0.005), "a pixel of path carries 0.25 x INTENSITY x Z x 2.6 x the trace colour");
	check(relNear(measuredUnit(beamParams(0.f, 0.f)), measuredUnit(beamParams(0.f, 1.f)), 0.005), "and BEAM does not change what a pixel of path carries");
}

void testScopeBeamPipeline() {
	std::printf("scope trace beam through the renderer: DWELL and BEAM change the picture\n");
	// A triangle the beam crosses in 30 samples a side and rests on each corner for 11 samples, 120 samples
	// to the lap, fed through a real ring. 960 samples (8 laps) arrive before each render, so every render
	// sees the same 241-sample window.
	const float cornerX[3] = {0.f, -0.7f, 0.7f};
	const float cornerY[3] = {0.7f, -0.5f, -0.5f};
	struct Run {
		Frame frame;
		crt::EnergyField energy;
	};
	auto run = [&](int bus, float dwell, float beam) {
		const int slot = signalRingAllocate();
		signalRingSetSampleRate(slot, 48000.f);
		int from = 0;
		auto push = [&](int count) {
			for (int n = from; n < from + count; ++n) {
				const int phase = n % 120;
				const int leg = phase / 40;
				const int k = phase % 40;
				const int next = (leg + 1) % 3;
				const float f = k <= 10 ? 0.f : static_cast<float>(k - 10) / 30.f;
				const float values[kSignalRingChannels] = {cornerX[leg] + (cornerX[next] - cornerX[leg]) * f, cornerY[leg] + (cornerY[next] - cornerY[leg]) * f, 1.f, 0.f};
				signalRingPush(slot, values);
			}
			from += count;
		};
		push(4800);
		ScopeTraceParams p = scopeDefaults(ScopeTraceMode::XY);
		p.dwell = dwell;
		p.thickness = beam;
		videoBusPublishScopeTrace(bus, slot, p);
		VideoGraphRenderer r;
		Run out;
		for (int i = 0; i < 4; ++i) {
			push(960);
			out.frame = render(r, bus);
		}
		out.energy = VideoGraphRendererTestAccess::beamEnergy(r, bus);
		signalRingRelease(slot);
		return out;
	};

	// On a 256 x 144 frame at scale 0.85 the corners are at about (127.5, 29), (51.6, 101.9) and (203.4, 101.9).
	// The left side's midpoint is about (89.5, 65.4).
	auto corners = [](const crt::EnergyField& e) {
		return regionEnergy(e, 123, 132, 25, 34) + regionEnergy(e, 47, 56, 97, 106) + regionEnergy(e, 199, 208, 97, 106);
	};
	auto side = [](const crt::EnergyField& e) {
		return regionEnergy(e, 87, 92, 63, 68);
	};

	const Run dwell0 = run(61, 0.f, 0.4f);
	const Run dwellHalf = run(61, 0.5f, 0.4f);
	const Run dwell1 = run(61, 1.f, 0.4f);
	const double c0 = corners(dwell0.energy), cHalf = corners(dwellHalf.energy), c1 = corners(dwell1.energy);
	const double s0 = side(dwell0.energy), sHalf = side(dwellHalf.energy), s1 = side(dwell1.energy);
	std::printf("  triangle, raw energy: corners %.2f / %.2f / %.2f, side midpoint %.3f / %.3f / %.3f at DWELL 0 / 0.5 / 1; frames differ by %d (0 vs 1)\n", c0, cHalf, c1, s0, sHalf, s1, maxDiff(dwell0.frame, dwell1.frame));
	check(!dwell0.frame.empty() && !dwell1.frame.empty() && fieldIsFinite(dwell1.energy), "the triangle renders at every DWELL");
	check(c0 > 0.0 && c0 < cHalf && cHalf < c1, "turning DWELL up lights the corners, where the beam rests, more and more");
	check(s0 > 0.0 && s1 <= s0, "and the sides, where it moves, no brighter");
	check(maxDiff(dwell0.frame, dwell1.frame) > 8, "so the DWELL 0 and DWELL 1 pictures differ");

	// BEAM through the renderer: the bottom side of the triangle is horizontal at row 101.9, and its cross
	// section is read from the raw energy, which has no graticule in it.
	auto cross = [](const crt::EnergyField& e, double& spread, double& peak) {
		double sw = 0.0, sy = 0.0, syy = 0.0;
		for (int y = 90; y < 114; ++y) {
			const double w = regionEnergy(e, 100, 140, y, y + 1);
			sw += w;
			sy += w * y;
			syy += w * y * y;
		}
		const double mean = sw > 0.0 ? sy / sw : 0.0;
		spread = sw > 0.0 ? std::sqrt(std::max(0.0, syy / sw - mean * mean)) : 0.0;
		peak = peakEnergy(e, 100, 140, 90, 114);
	};
	const Run beam0 = run(62, 0.f, 0.f);
	const Run beamMid = run(62, 0.f, 0.4f);
	const Run beam1 = run(62, 0.f, 1.f);
	double spread0 = 0.0, spreadMid = 0.0, spread1 = 0.0, peak0 = 0.0, peakMid = 0.0, peak1 = 0.0;
	cross(beam0.energy, spread0, peak0);
	cross(beamMid.energy, spreadMid, peakMid);
	cross(beam1.energy, spread1, peak1);
	std::printf("  triangle base, BEAM 0 / 0.4 / 1: spread %.2f / %.2f / %.2f px (sigma 0.5 / 1.5 / 3), peak %.3f / %.3f / %.3f; frames differ by %d (0 vs 1)\n", spread0, spreadMid, spread1, peak0, peakMid, peak1, maxDiff(beam0.frame, beam1.frame));
	check(spread0 > 0.4 && spread0 < 0.6 && relNear(spreadMid, 1.5, 0.15) && relNear(spread1, 3.0, 0.15), "through the renderer BEAM 0, 0.4 and 1 draw a line 0.5, 1.5 and 3 px wide (sigma)");
	check(spread0 < spreadMid && spreadMid < spread1, "turning BEAM up widens the line");
	check(peak0 > peakMid && peakMid > peak1, "and dims its peak, as the energy is the same");
	check(maxDiff(beam0.frame, beam1.frame) > 8, "so the BEAM 0 and BEAM 1 pictures differ");
	saveProof("scope_dwell_proof.png", {dwell0.frame, dwellHalf.frame, dwell1.frame});
	saveProof("scope_beam_width_proof.png", {beam0.frame, beamMid.frame, beam1.frame});
}

// ---- Dual LFO Chaos: Lorenz attractor ----
void testLorenz() {
	std::printf("dual LFO chaos: Lorenz attractor\n");
	LorenzState s;
	const double h = 0.005;  // inside the module's step limit (0.01 time units)
	for (int i = 0; i < 20000; ++i) {
		lorenz::step(s, h);  // let the transient die down
	}

	// Bounded: over 2e6 steps (10 000 time units) the state stays on the attractor.
	double maxX = 0.0, maxY = 0.0, minZ = 1e9, maxZ = -1e9;
	int overScale = 0;
	std::vector<double> xs;
	xs.reserve(200000);
	for (int i = 0; i < 2000000; ++i) {
		lorenz::step(s, h);
		maxX = std::max(maxX, std::fabs(s.x));
		maxY = std::max(maxY, std::fabs(s.y));
		minZ = std::min(minZ, s.z);
		maxZ = std::max(maxZ, s.z);
		if (std::fabs(s.x) > 22.0) {
			++overScale;
		}
		if (i % 50 == 0) {
			xs.push_back(s.x);  // one sample every 0.25 time units
		}
	}
	std::printf("  extent over 10000 time units: |x| <= %.2f, |y| <= %.2f, z in [%.2f, %.2f]\n", maxX, maxY, minZ, maxZ);
	check(maxX < 30.0 && maxY < 35.0 && minZ > 0.0 && maxZ < 55.0, "Lorenz state stays bounded");
	check(overScale < 2000000 / 20, "outputs scaled by 22 are inside +/-1 for most of the time");

	// Not periodic: autocorrelation of the sampled x is small at every lag, so no repeating cycle.
	const size_t n = xs.size();
	double mean = 0.0;
	for (double v : xs) mean += v;
	mean /= static_cast<double>(n);
	double var = 0.0;
	for (double v : xs) var += (v - mean) * (v - mean);
	double worst = 0.0;
	int worstLag = 0;
	for (int lag = 4; lag <= 400; ++lag) {
		double c = 0.0;
		for (size_t i = 0; i + static_cast<size_t>(lag) < n; ++i) {
			c += (xs[i] - mean) * (xs[i + static_cast<size_t>(lag)] - mean);
		}
		const double r = c / var;
		if (std::fabs(r) > worst) {
			worst = std::fabs(r);
			worstLag = lag;
		}
	}
	std::printf("  largest autocorrelation at lags 1..100 time units: %.3f (lag %d samples)\n", worst, worstLag);
	check(worst < 0.5, "no repeating cycle: autocorrelation stays small");

	// Sensitive dependence: 1e-9 difference grows to order 1 within 30 time units.
	LorenzState a = {1.0, 1.0, 20.0};
	LorenzState b = {1.0 + 1e-9, 1.0, 20.0};
	for (int i = 0; i < 6000; ++i) {
		lorenz::step(a, h);
		lorenz::step(b, h);
	}
	const double separation = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
	std::printf("  two trajectories 1e-9 apart: separation after 30 time units %.3f\n", separation);
	check(separation > 1.0, "trajectories diverge (chaotic, not a fixed cycle)");
}

// ---- Scan Processor: luma-lifted scanlines ----
// Test raster: 448 x 640, portrait, as the DeflectionYoke proofs. Measurements use persistence 0 and no glow,
// so each render is one clean frame. Lines are read from one screen column with sub-pixel peak fits.
constexpr int kRutW = 448;
constexpr int kRutH = 640;
constexpr int kRutSrcBus = 42;
constexpr int kRutOutBus = 43;
constexpr int kRutGenBus = 44;

Frame flatFrame(int w, int h, unsigned char v) {
	return Frame(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, v);
}

// Luma in 0..1 of one pixel.
double lum01(const Frame& f, int w, int x, int y) {
	const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
	return (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
}

// Sub-pixel row positions of the luma peaks along column x (parabolic fit over three rows).
std::vector<double> columnPeaks(const Frame& f, int w, int h, int x, double minLuma) {
	std::vector<double> prof(static_cast<size_t>(h));
	for (int y = 0; y < h; ++y) {
		prof[static_cast<size_t>(y)] = lum01(f, w, x, y);
	}
	std::vector<double> peaks;
	for (int y = 1; y + 1 < h; ++y) {
		const double a = prof[static_cast<size_t>(y) - 1];
		const double b = prof[static_cast<size_t>(y)];
		const double c = prof[static_cast<size_t>(y) + 1];
		if (b >= minLuma && b >= a && b > c) {
			const double denom = a - 2.0 * b + c;
			const double off = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
			peaks.push_back(y + off);
		}
	}
	return peaks;
}

// Mean luma of one row.
double rowMean(const Frame& f, int w, int y) {
	double sum = 0.0;
	for (int x = 0; x < w; ++x) {
		sum += lum01(f, w, x, y);
	}
	return sum / w;
}

ScanProcessorParams measureParams(int lines) {
	ScanProcessorParams p;
	p.lines = lines;
	p.lumaGain = 0.f;
	p.hPos = 0.f;
	p.hWidth = 1.f;
	p.vPos = 0.f;
	p.vHeight = 1.f;
	p.intensity = 1.f;
	p.persist = 0.f;
	p.glow = 0.f;
	return p;
}

// Renders a Scan Processor node over the source bus srcBus (already published) at w x h.
Frame renderScanProcessorNode(VideoGraphRenderer& r, int srcBus, int slot, ScanProcessorParams p, int w, int h) {
	p.signalSlotId = slot;
	videoBusPublishScanProcessor(kRutOutBus, srcBus, p);
	Frame out = r.renderBus(kRutOutBus, w, h);
	videoBusClear(kRutOutBus);
	return out;
}

// Renders a Scan Processor node over a source frame.
Frame renderScanProcessorFrame(VideoGraphRenderer& r, const Frame& source, int slot, const ScanProcessorParams& p, int w, int h) {
	const int frameId = videoExternalFrameAllocate();
	videoExternalFramePublish(frameId, source.data(), w, h, 0.0);
	videoBusPublishExternalFrame(kRutSrcBus, frameId);
	Frame out = renderScanProcessorNode(r, kRutSrcBus, slot, p, w, h);
	videoBusClear(kRutSrcBus);
	videoExternalFrameRelease(frameId);
	return out;
}

void saveScanProcessorProof(const std::string& name, const Frame& f, int w = kRutW, int h = kRutH) {
	const std::string path = gProofDir + "/" + name;
	if (writePng(path, w, h, f)) {
		std::printf("  proof: %s\n", path.c_str());
	}
	else {
		std::printf("  could not write %s\n", path.c_str());
		check(false, "scanProc proof image written");
	}
}

// 1. A flat grey source gives evenly spaced, straight lines.
void testScanProcessorFlatLines() {
	std::printf("scan processor: flat grey gives even straight lines\n");
	const Frame grey = flatFrame(kRutW, kRutH, 128);
	ScanProcessorParams p = measureParams(128);
	p.lumaGain = 0.1f;   // lifts every line by 0.1 x 0.5 of the height...
	p.vPos = 0.05f;      // ...and this offset puts them back at their nominal rows
	VideoGraphRenderer r;
	const int slot = signalRingAllocate();
	const Frame out = renderScanProcessorFrame(r, grey, slot, p, kRutW, kRutH);
	const std::vector<double> a = columnPeaks(out, kRutW, kRutH, 100, 0.2);
	const std::vector<double> b = columnPeaks(out, kRutW, kRutH, 300, 0.2);
	const double pitch = static_cast<double>(kRutH) / 128.0;
	double worst = 0.0;
	double worstStraight = 0.0;
	for (size_t i = 0; i < std::min(a.size(), b.size()) && i < 128; ++i) {
		worst = std::max(worst, std::fabs(a[i] - (static_cast<double>(i) + 0.5) * pitch));
		worstStraight = std::max(worstStraight, std::fabs(a[i] - b[i]));
	}
	double spacing = (a.empty() || a.size() < 2) ? 0.0 : (a.back() - a.front()) / static_cast<double>(a.size() - 1);
	std::printf("  %zu lines, mean spacing %.3f px (expected %.3f), worst offset from nominal %.3f px, straightness %.3f px\n",
		a.size(), spacing, pitch, worst, worstStraight);
	check(a.size() == 128 && b.size() == 128, "flat grey shows all 128 lines");
	check(std::fabs(spacing - pitch) < 0.05, "lines are evenly spaced at the line pitch");
	check(worst < 0.3, "each line sits on its nominal row");
	check(worstStraight < 0.05, "lines are straight across the picture");
	signalRingRelease(slot);
}

// 2. A bright square lifts the lines inside it by LUMA GAIN x luma.
void testScanProcessorLift() {
	std::printf("scan processor: a bright square lifts the lines by LUMA GAIN x luma\n");
	const int W = kRutW;
	const int H = kRutH;
	Frame square = flatFrame(W, H, 0);
	for (int y = H / 4; y < 3 * H / 4; ++y) {
		for (int x = W / 4; x < 3 * W / 4; ++x) {
			const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
			square[i] = square[i + 1] = square[i + 2] = 255;
		}
	}
	const float gain = 0.05f;
	ScanProcessorParams p = measureParams(32);
	p.lumaGain = gain;
	VideoGraphRenderer r;
	const int slot = signalRingAllocate();
	const Frame out = renderScanProcessorFrame(r, square, slot, p, W, H);
	const std::vector<double> peaks = columnPeaks(out, W, H, W / 2, 0.05);
	const double pitch = static_cast<double>(H) / 32.0;
	const double expected = gain * H * 1.0;  // luma 1 inside the square
	double sum = 0.0;
	int n = 0;
	int missing = 0;
	for (int k = 0; k < 32; ++k) {
		const double y0 = (k + 0.5) * pitch;
		if (y0 < H / 4 + 2 * pitch || y0 > 3 * H / 4 - 2 * pitch) {
			continue;  // skip lines near the square's edges, where neighbours cross
		}
		double best = 1e9;
		for (double pk : peaks) {
			if (std::fabs(pk - (y0 - expected)) < std::fabs(best)) {
				best = pk - (y0 - expected);
			}
		}
		if (std::fabs(best) > 0.5 * pitch) {
			++missing;
			continue;
		}
		sum += expected - best;  // the peak sits `best` px from where a full lift would put it
		++n;
	}
	const double meanLift = n > 0 ? sum / n : 0.0;
	std::printf("  %d interior lines, mean lift %.2f px (expected %.2f px), %d unmatched\n", n, meanLift, expected, missing);
	check(n >= 6 && missing == 0, "every interior line is found inside the square");
	check(std::fabs(meanLift - expected) < 0.1 * expected, "lift inside the square is LUMA GAIN x luma within 10 %");
	signalRingRelease(slot);
}

// 3. Where lines converge, brightness adds.
void testScanProcessorBunching() {
	std::printf("scan processor: converging lines add brightness\n");
	const int W = kRutW;
	const int H = kRutH;
	// Luma ramps from 0 at L = 0.3 to 1 at L = 0.8 (rows 192 to 512). With gain 0.4 the lines there
	// land 0.2 of a height apart per unit line, so about one pixel apart: a bunch.
	Frame ramp = flatFrame(W, H, 0);
	for (int y = 0; y < H; ++y) {
		const double L = (static_cast<double>(y) + 0.5) / H;
		const double luma = std::clamp((L - 0.3) * 2.0, 0.0, 1.0);
		for (int x = 0; x < W; ++x) {
			const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
			ramp[i] = ramp[i + 1] = ramp[i + 2] = static_cast<unsigned char>(std::lround(luma * 255.0));
		}
	}
	ScanProcessorParams p = measureParams(128);
	p.lumaGain = 0.4f;
	p.intensity = 0.15f;  // low, so a single line is far from saturation
	VideoGraphRenderer r;
	const int slot = signalRingAllocate();
	const Frame out = renderScanProcessorFrame(r, ramp, slot, p, W, H);
	double bunched = 0.0;  // rows 200..250 land in the bunch
	for (int y = 200; y <= 250; ++y) {
		bunched = std::max(bunched, rowMean(out, W, y));
	}
	double spread = 0.0;  // rows 280..330 hold ordinary 5 px spacing
	for (int y = 280; y <= 330; ++y) {
		spread = std::max(spread, rowMean(out, W, y));
	}
	const double ratio = spread > 0.0 ? bunched / spread : 0.0;
	std::printf("  peak row brightness: bunched %.3f, spread %.3f, ratio %.2f\n", bunched, spread, ratio);
	check(ratio > 1.8, "a bunch of lines is brighter than the same energy spread over the line pitch");
	signalRingRelease(slot);
}

// 4. A negative H WIDTH mirrors the picture (about H POS).
void testScanProcessorMirror() {
	std::printf("scan processor: negative H WIDTH mirrors the picture\n");
	const int W = kRutW;
	const int H = kRutH;
	Frame bar = flatFrame(W, H, 0);
	for (int y = 0; y < H; ++y) {
		for (int x = W / 10; x < 3 * W / 10; ++x) {
			const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
			bar[i] = bar[i + 1] = bar[i + 2] = 255;
		}
	}
	const Frame dark = flatFrame(W, H, 0);
	auto centroidOfChange = [&](float hPos, float hWidth) {
		ScanProcessorParams p = measureParams(128);
		p.lumaGain = 0.05f;
		p.hPos = hPos;
		p.hWidth = hWidth;
		VideoGraphRenderer r1;
		VideoGraphRenderer r2;
		const int slot = signalRingAllocate();
		const Frame lit = renderScanProcessorFrame(r1, bar, slot, p, W, H);
		const Frame base = renderScanProcessorFrame(r2, dark, slot, p, W, H);
		signalRingRelease(slot);
		double sum = 0.0;
		double weighted = 0.0;
		for (int x = 0; x < W; ++x) {
			double column = 0.0;
			for (int y = 0; y < H; ++y) {
				column += std::fabs(lum01(lit, W, x, y) - lum01(base, W, x, y));
			}
			sum += column;
			weighted += column * x;
		}
		return sum > 0.0 ? weighted / sum : -1.0;
	};
	const double plus = centroidOfChange(0.f, 1.f);
	const double minus = centroidOfChange(1.f, -1.f);
	std::printf("  change centred at column %.1f (H WIDTH +1), %.1f (H WIDTH -1, H POS 1); mirror of the first is %.1f\n",
		plus, minus, static_cast<double>(W) - plus);
	check(plus > 0.0 && minus > 0.0, "both mirror settings show the bar");
	check(std::fabs(minus - (static_cast<double>(W) - plus)) < 0.02 * W, "H WIDTH -1 puts the bar at the mirror position");
}

// 5. ROTATE 90 swaps the axes. On a square raster, the rotated picture is the transpose of the plain one.
void testScanProcessorRotate() {
	std::printf("scan processor: ROTATE 90 swaps the axes\n");
	const int S = 384;
	const Frame src = makeCameraSource(S, S);
	ScanProcessorParams p = measureParams(64);
	p.lumaGain = 0.15f;
	p.intensity = 0.6f;
	p.persist = 0.35f;
	p.glow = 0.4f;
	VideoGraphRenderer plainR;
	VideoGraphRenderer turnR;
	const int slot = signalRingAllocate();
	const Frame plain = renderScanProcessorFrame(plainR, src, slot, p, S, S);
	p.rotate = true;
	const Frame turned = renderScanProcessorFrame(turnR, src, slot, p, S, S);
	int worst = 0;
	for (int y = 0; y < S; ++y) {
		for (int x = 0; x < S; ++x) {
			const size_t a = (static_cast<size_t>(y) * S + x) * 4u;
			const size_t b = (static_cast<size_t>(x) * S + y) * 4u;
			for (int c = 0; c < 3; ++c) {
				worst = std::max(worst, std::abs(int(turned[a + c]) - int(plain[b + c])));
			}
		}
	}
	std::printf("  worst channel difference between ROTATE 90 and the transpose of the plain picture: %d\n", worst);
	check(worst <= 1, "rotated picture is the transpose of the plain one");
	check(meanDiff(plain, turned) > 5.0, "ROTATE 90 changes the picture");
	signalRingRelease(slot);
}

// 6. BLANK high gives no energy; with BLANK low the same picture shows.
void testScanProcessorBlank() {
	std::printf("scan processor: BLANK high gives no energy\n");
	const Frame grey = flatFrame(kRutW, kRutH, 160);
	ScanProcessorParams p = measureParams(128);
	p.lumaGain = 0.1f;
	const int slot = signalRingAllocate();
	pushScanSignal(slot, 4, [](int, float* v) {
		v[kScanProcessorDeflectX] = 0.f;
		v[kScanProcessorDeflectY] = 0.f;
		v[kScanProcessorBlank] = 1.f;
	});
	VideoGraphRenderer blankR;
	const Frame blanked = renderScanProcessorFrame(blankR, grey, slot, p, kRutW, kRutH);
	int peak = 0;
	for (size_t i = 0; i < blanked.size(); i += 4) {
		peak = std::max({peak, int(blanked[i]), int(blanked[i + 1]), int(blanked[i + 2])});
	}
	pushScanSignal(slot, 4, [](int, float* v) {
		v[kScanProcessorDeflectX] = 0.f;
		v[kScanProcessorDeflectY] = 0.f;
		v[kScanProcessorBlank] = 0.f;
	});
	VideoGraphRenderer openR;
	const Frame open = renderScanProcessorFrame(openR, grey, slot, p, kRutW, kRutH);
	int openPeak = 0;
	for (size_t i = 0; i < open.size(); i += 4) {
		openPeak = std::max(openPeak, int(open[i + 1]));
	}
	std::printf("  brightest channel: blanked %d, unblanked %d\n", peak, openPeak);
	check(peak == 0, "BLANK high leaves the screen dark");
	check(openPeak > 100, "BLANK low shows the lines");
	signalRingRelease(slot);
}

// 6b. Beam Z scales the beam energy: -1 (-5 V) turns it off, 0 leaves it alone, +1 (+5 V) doubles it.
// Energy is linear in INTENSITY, so +1 at intensity 0.5 must match 0 at intensity 1.0. The picture brightness
// is not linear (the tone map is 1 - exp(-kE)), so the brightness ratio is only checked loosely.
void testScanProcessorZ() {
	std::printf("scan processor: beam Z -1 turns the beam off, +1 doubles the beam energy\n");
	const Frame grey = flatFrame(kRutW, kRutH, 160);
	ScanProcessorParams p = measureParams(128);
	p.lumaGain = 0.1f;
	const int slot = signalRingAllocate();
	auto render = [&](float z, float intensity) {
		pushScanSignal(slot, 4, [z](int, float* v) {
			v[kScanProcessorDeflectX] = 0.f;
			v[kScanProcessorDeflectY] = 0.f;
			v[kScanProcessorBlank] = 0.f;
			v[kScanProcessorZ] = z;
		});
		ScanProcessorParams q = p;
		q.intensity = intensity;
		VideoGraphRenderer r;
		return renderScanProcessorFrame(r, grey, slot, q, kRutW, kRutH);
	};
	auto meanGreen = [](const Frame& f) {
		double sum = 0.0;
		for (size_t i = 0; i < f.size(); i += 4) {
			sum += f[i + 1];
		}
		return sum / static_cast<double>(f.size() / 4);
	};
	auto brightest = [](const Frame& f) {
		int best = 0;
		for (size_t i = 0; i < f.size(); i += 4) {
			best = std::max({best, int(f[i]), int(f[i + 1]), int(f[i + 2])});
		}
		return best;
	};
	const Frame off = render(-1.f, 0.5f);
	const Frame base = render(0.f, 0.5f);
	const Frame doubled = render(1.f, 0.5f);
	const Frame twiceIntensity = render(0.f, 1.f);
	const double baseMean = meanGreen(base);
	const double doubledMean = meanGreen(doubled);
	const double twiceMean = meanGreen(twiceIntensity);
	std::printf("  mean green: off %.2f, unchanged %.2f, Z +1 %.2f, intensity doubled %.2f (brightness ratio %.2f)\n",
		meanGreen(off), baseMean, doubledMean, twiceMean, doubledMean / baseMean);
	check(brightest(off) == 0, "beam Z at -5 V leaves the screen dark");
	check(baseMean > 5.0, "beam Z at 0 V shows the lines");
	check(std::fabs(doubledMean - twiceMean) < 0.5, "beam Z at +5 V gives the same beam energy as doubling INTENSITY");
	check(doubledMean > 1.3 * baseMean, "beam Z at +5 V brightens the picture");
	signalRingRelease(slot);
}

// 7. Audio on DEFLECT Y at f gives the expected ripple count per field: f x visible time.
void testScanProcessorDeflectRipples() {
	std::printf("scan processor: DEFLECT Y at f gives f x visible time ripples per field\n");
	const Frame grey = flatFrame(kRutW, kRutH, 0);
	const int lines = 128;
	ScanProcessorParams p = measureParams(lines);
	// Scale the raster to 90 % and move it down, so no line runs off the edge when it is deflected.
	p.vHeight = 0.9f;
	p.vPos = 0.05f;
	const double pi = 3.14159265358979323846;
	VideoGraphRenderer baseR;
	const int zeroSlot = signalRingAllocate();
	const Frame base = renderScanProcessorFrame(baseR, grey, zeroSlot, p, kRutW, kRutH);
	const std::vector<double> nominal = columnPeaks(base, kRutW, kRutH, kRutW / 2, 0.05);
	signalRingRelease(zeroSlot);
	check(nominal.size() == static_cast<size_t>(lines), "undeflected lines are all found");

	for (double f : {1000.0, 2000.0}) {
		const int slot = signalRingAllocate();
		// 0.0035 in ramp units is 0.07 V at the module (0.5 x V / 5). With V HEIGHT 0.9 that is about 2 px.
		constexpr double kAmp = 0.0035;
		const double expectedAmp = kAmp * 0.9 * kRutH;
		pushScanSignal(slot, 4, [&](int n, float* v) {
			v[kScanProcessorDeflectX] = 0.f;
			v[kScanProcessorDeflectY] = static_cast<float>(kAmp * std::sin(2.0 * pi * f * n / kWobRate));
			v[kScanProcessorBlank] = 0.f;
		});
		VideoGraphRenderer r;
		const Frame out = renderScanProcessorFrame(r, grey, slot, p, kRutW, kRutH);
		signalRingRelease(slot);
		const std::vector<double> peaks = columnPeaks(out, kRutW, kRutH, kRutW / 2, 0.05);
		std::vector<double> dev;
		double amp = 0.0;
		if (peaks.size() == static_cast<size_t>(lines)) {
			for (int k = 0; k < lines; ++k) {
				dev.push_back(peaks[static_cast<size_t>(k)] - nominal[static_cast<size_t>(k)]);
				amp = std::max(amp, std::fabs(dev.back()));
			}
		}
		const double expected = f * visibleSeconds(lines);
		const int ripples = countRipples(dev);
		std::printf("  %.0f Hz: %zu lines found, %d ripples counted, expected %.2f; peak displacement %.2f px (%.2f expected)\n", f, peaks.size(), ripples, expected, amp, expectedAmp);
		check(peaks.size() == static_cast<size_t>(lines), "deflected lines are all found");
		check(std::fabs(ripples - expected) <= 1.0, "ripple count matches f x visible time");
		check(std::fabs(amp - expectedAmp) < 0.2 * expectedAmp, "DEFLECT Y amplitude gives the expected displacement");
	}
}

// Proof images (no checks): the test source, the picture from it, a bright shape, Video Oscillator patterns,
// ROTATE 90 and a deflect-Y sine. Written to the proof directory.
Frame faceSource(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.f - 1.f;
			const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.f - 1.f;
			const float rr = std::sqrt(u * u + v * v);
			float light = 0.18f + 0.22f * std::clamp(1.f - rr, 0.f, 1.f);  // soft radial falloff
			// Head: a soft oval.
			const float oval = std::sqrt((u * u) / 0.42f + (v * v) / 0.66f);
			const float head = std::clamp((1.f - oval) * 6.f, 0.f, 1.f);
			light = light * (1.f - head) + (0.80f + 0.12f * (1.f - oval)) * head;
			// Eyes and mouth: soft dark marks.
			const float eyeL = std::exp(-((u + 0.2f) * (u + 0.2f) + (v + 0.12f) * (v + 0.12f)) / 0.0045f);
			const float eyeR = std::exp(-((u - 0.2f) * (u - 0.2f) + (v + 0.12f) * (v + 0.12f)) / 0.0045f);
			const float mouthY = v - (0.36f + 0.22f * u * u);
			const float mouth = std::exp(-(mouthY * mouthY) / 0.0018f) * std::clamp(1.f - std::fabs(u) * 1.7f, 0.f, 1.f);
			light *= (1.f - 0.85f * std::max(eyeL, eyeR)) * (1.f - 0.6f * mouth);
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const unsigned char g = static_cast<unsigned char>(std::clamp(light * 255.f, 0.f, 255.f));
			f[i] = g;
			f[i + 1] = g;
			f[i + 2] = g;
			f[i + 3] = 255;
		}
	}
	return f;
}

Frame brightShapeSource(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.f - 1.f;
			const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.f - 1.f;
			const float disc = std::clamp((0.5f - std::sqrt(u * u + (v + 0.1f) * (v + 0.1f) * 1.6f)) * 14.f, 0.f, 1.f);
			const float box = std::clamp((0.72f - std::max(std::fabs(u - 0.0f), std::fabs(v + 0.75f))) * 14.f, 0.f, 1.f);
			const float light = std::max(0.04f, std::max(disc, 0.9f * box));
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const unsigned char g = static_cast<unsigned char>(std::clamp(light * 255.f, 0.f, 255.f));
			f[i] = g;
			f[i + 1] = g;
			f[i + 2] = g;
			f[i + 3] = 255;
		}
	}
	return f;
}

void testScanProcessorProofs() {
	std::printf("scan processor proofs: relief on a tube\n");
	const Frame face = faceSource(kRutW, kRutH);
	const Frame shape = brightShapeSource(kRutW, kRutH);
	saveScanProcessorProof("scan_processor_source_face.png", face);

	ScanProcessorParams p;  // module defaults: 128 lines, luma gain 0.2, persistence 0.35, glow 0.4
	{
		VideoGraphRenderer r;
		const int slot = signalRingAllocate();
		saveScanProcessorProof("scan_processor_face.png", renderScanProcessorFrame(r, face, slot, p, kRutW, kRutH));
		signalRingRelease(slot);
	}
	{
		VideoGraphRenderer r;
		const int slot = signalRingAllocate();
		saveScanProcessorProof("scan_processor_bright_shape.png", renderScanProcessorFrame(r, shape, slot, p, kRutW, kRutH));
		signalRingRelease(slot);
	}
	{
		// Video Oscillator SMPTE bars and an oscillator pattern, through the bus.
		VideoGraphRenderer r;
		const int slot = signalRingAllocate();
		publishOsc(kRutGenBus, 0.f, static_cast<int>(VideoPattern::Smpte75));
		saveScanProcessorProof("scan_processor_smpte.png", renderScanProcessorNode(r, kRutGenBus, slot, p, kRutW, kRutH));
		videoBusClear(kRutGenBus);
		signalRingRelease(slot);
	}
	{
		VideoGraphRenderer r;
		const int slot = signalRingAllocate();
		publishOsc(kRutGenBus, 0.2f, static_cast<int>(VideoPattern::Oscillator));
		saveScanProcessorProof("scan_processor_osc_pattern.png", renderScanProcessorNode(r, kRutGenBus, slot, p, kRutW, kRutH));
		videoBusClear(kRutGenBus);
		signalRingRelease(slot);
	}
	{
		VideoGraphRenderer r;
		const int slot = signalRingAllocate();
		ScanProcessorParams q = p;
		q.rotate = true;
		saveScanProcessorProof("scan_processor_rotate90_face.png", renderScanProcessorFrame(r, face, slot, q, kRutW, kRutH));
		signalRingRelease(slot);
	}
	{
		// Deflect Y: a 300 Hz sine, 0.04 in ramp units (about 26 px at this raster; lines stay in order).
		const double pi = 3.14159265358979323846;
		VideoGraphRenderer r;
		const int slot = signalRingAllocate();
		pushScanSignal(slot, 4, [&](int n, float* v) {
			v[kScanProcessorDeflectX] = 0.f;
			v[kScanProcessorDeflectY] = static_cast<float>(0.04 * std::sin(2.0 * pi * 300.0 * n / kWobRate));
			v[kScanProcessorBlank] = 0.f;
		});
		saveScanProcessorProof("scan_processor_deflect_y_sine.png", renderScanProcessorFrame(r, shape, slot, p, kRutW, kRutH));
		signalRingRelease(slot);
	}
}

void testRampsMonotonic() {
	std::printf("ramps: axes, monotonicity, grey output and subtraction\n");
	VideoGraphRenderer r;
	RampsParams p;
	std::vector<Frame> axes;
	for (int axis = 0; axis < 4; ++axis) {
		p.axisA = axis;
		videoBusPublishRamps(1, p);
		axes.push_back(render(r, 1));
	}
	bool horizontal = true, vertical = true, grey = true;
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			if (x > 0 && lumaAt(axes[0], x, y) < lumaAt(axes[0], x - 1, y)) horizontal = false;
			if (y > 0 && lumaAt(axes[1], x, y) < lumaAt(axes[1], x, y - 1)) vertical = false;
		}
	}
	for (const Frame& frame : axes) {
		for (size_t i = 0; i < frame.size(); i += 4) {
			if (frame[i] != frame[i + 1] || frame[i] != frame[i + 2] || frame[i + 3] != 255) grey = false;
		}
	}
	check(horizontal, "horizontal ramp never decreases along any row");
	check(vertical, "vertical ramp never decreases down any column");
	check(lumaAt(axes[0], 0, 0) < 0.01f && lumaAt(axes[0], kW - 1, 0) > 0.98f, "horizontal ramp spans near black to near white");
	check(lumaAt(axes[1], 0, 0) < 0.01f && lumaAt(axes[1], 0, kH - 1) > 0.98f, "vertical ramp spans near black to near white");
	check(lumaAt(axes[2], kW / 2, kH / 2) < 0.05f && lumaAt(axes[2], kW / 2, 0) > 0.9f, "radial ramp rises from centre to nearer edge");
	check(grey, "all four axes produce opaque grey pixels");
	p.axisA = p.axisB = 0;
	p.mix = 2;
	videoBusPublishRamps(1, p);
	const Frame difference = render(r, 1);
	bool neutral = true;
	for (size_t i = 0; i < difference.size(); i += 4) {
		if (std::abs(int(difference[i]) - 128) > 1) neutral = false;
	}
	check(neutral, "identical layers in A-B give 128 within one byte");
	saveProof("ramps_axes.png", axes);
	videoBusClear(1);
}

void testColorizerStops() {
	std::printf("colorizer: stops, bands, offset and alpha\n");
	VideoGraphRenderer r;
	videoBusPublishRamps(1, RampsParams{});
	ColorizerParams p;
	p.hue[0] = 0.f; p.hue[1] = 1.f / 3.f; p.hue[2] = 2.f / 3.f; p.hue[3] = 0.f;
	for (float& level : p.level) level = 1.f;
	p.saturation = 1.f;
	videoBusPublishColorizer(2, 1, p);
	const Frame smooth = render(r, 2);
	auto primaryColumn = [&](int x, int channel) {
		for (int y = 0; y < kH; ++y) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			if (smooth[i + channel] <= 200 || smooth[i + (channel + 1) % 3] >= 40 || smooth[i + (channel + 2) % 3] >= 40) return false;
		}
		return true;
	};
	check(primaryColumn(0, 0), "leftmost column is red");
	check(primaryColumn(kW / 3, 1), "one-third column is green");
	check(primaryColumn(2 * kW / 3, 2), "two-thirds column is blue");
	p.bands = 2;
	videoBusPublishColorizer(2, 1, p);
	const Frame banded = render(r, 2);
	std::set<std::array<unsigned char, 3>> colours;
	for (int x = 0; x < kW; ++x) colours.insert({banded[x * 4], banded[x * 4 + 1], banded[x * 4 + 2]});
	check(colours.size() == 2, "two bands give exactly two distinct colours in a row");
	p.bands = 0;
	p.offset = 0.5f;
	videoBusPublishColorizer(2, 1, p);
	const Frame offset = render(r, 2);
	bool shifted = true;
	for (int y = 0; y < kH; ++y) {
		for (int c = 0; c < 3; ++c) {
			const size_t a = static_cast<size_t>(y) * kW * 4u + c;
			const size_t b = (static_cast<size_t>(y) * kW + kW / 2) * 4u + c;
			if (std::abs(int(offset[a]) - int(smooth[b])) > 4) shifted = false;
		}
	}
	check(shifted, "half-turn offset moves the middle colour to the left edge within luma-table precision");
	saveProof("colorizer_stops.png", {smooth, banded, offset});

	// A processor must keep the incoming matte byte even when it recolours every pixel.
	Frame source = render(r, 1);
	for (size_t i = 3; i < source.size(); i += 4) source[i] = static_cast<unsigned char>((i / 4) % 256);
	const int frameId = videoExternalFrameAllocate();
	videoExternalFramePublish(frameId, source.data(), kW, kH, 0.0);
	videoBusPublishExternalFrame(1, frameId);
	const Frame alpha = render(r, 2);
	bool alphaPassed = true;
	for (size_t i = 3; i < alpha.size(); i += 4) if (alpha[i] != source[i]) alphaPassed = false;
	check(alphaPassed, "Colorizer preserves every source alpha byte");
	videoBusClear(1);
	videoBusClear(2);
	videoExternalFrameRelease(frameId);
}

// The 256-entry table runs from grey 0 at the bottom stop to grey 255 at the top stop. White used to
// wrap round to the bottom stop at OFFSET 0, which cut a dark hole in any white plateau.
void testColorizerTop() {
	std::printf("colorizer: white keeps the top of the palette, OFFSET still wraps\n");
	VideoGraphRenderer r;
	// Column x of the source is grey x, so one row of the output is the whole table.
	Frame ramp(static_cast<size_t>(kW) * kH * 4u);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
			ramp[i] = ramp[i + 1] = ramp[i + 2] = static_cast<unsigned char>(x);
			ramp[i + 3] = 255;
		}
	}
	const int frameId = videoExternalFrameAllocate();
	videoExternalFramePublish(frameId, ramp.data(), kW, kH, 0.0);
	videoBusPublishExternalFrame(1, frameId);
	check(maxDiff(render(r, 1), ramp) == 0, "grey ramp source arrives unchanged");

	// Red only, brightness rising 0.25, 0.5, 0.75, 1 over the four stops: red = 255 x (0.25 + 0.75 x luma).
	ColorizerParams p;
	for (float& hue : p.hue) hue = 0.f;
	p.level[0] = 0.25f;
	p.level[1] = 0.5f;
	p.level[2] = 0.75f;
	p.level[3] = 1.f;
	p.saturation = 1.f;
	auto expectedRed = [](double luma) { return static_cast<int>(std::floor(255.0 * (0.25 + 0.75 * luma))); };
	auto redAt = [](const Frame& f, int grey) { return static_cast<int>(f[static_cast<size_t>(grey) * 4u]); };
	// Within one step of the formula: the renderer works in float and truncates.
	auto followsFormula = [&](const Frame& f, int first, int last, double offset) {
		for (int grey = first; grey <= last; ++grey) {
			double luma = grey / 255.0 + offset;
			if (luma > 1.0) luma -= 1.0;
			if (std::abs(redAt(f, grey) - expectedRed(luma)) > 1) return false;
		}
		return true;
	};
	auto redOnlyAndOpaque = [](const Frame& f) {
		for (size_t i = 0; i < f.size(); i += 4) {
			if (f[i + 1] != 0 || f[i + 2] != 0 || f[i + 3] != 255) return false;
		}
		return true;
	};

	p.offset = 0.f;
	videoBusPublishColorizer(2, 1, p);
	const Frame plain = render(r, 2);
	check(redAt(plain, 255) == 255, "OFFSET 0: white takes the palette's top colour");
	check(std::abs(redAt(plain, 0) - expectedRed(0.0)) <= 1, "OFFSET 0: black keeps the palette's bottom colour");
	check(followsFormula(plain, 0, 254, 0.0), "OFFSET 0: greys 0 to 254 keep their colours");
	bool rises = true;
	for (int grey = 1; grey < 256; ++grey) {
		if (redAt(plain, grey) < redAt(plain, grey - 1)) rises = false;
	}
	check(rises, "OFFSET 0: no grey is darker than the one below it, so no hole at white");
	check(redOnlyAndOpaque(plain), "OFFSET 0: only red, and every pixel opaque");

	// A half turn still wraps: the seam moves between grey 127 (just under the top) and grey 128 (just over the bottom).
	p.offset = 0.5f;
	videoBusPublishColorizer(2, 1, p);
	const Frame halfTurn = render(r, 2);
	check(redAt(halfTurn, 127) >= 250, "OFFSET 0.5: grey 127 is just under the top of the palette");
	check(redAt(halfTurn, 128) <= 66, "OFFSET 0.5: grey 128 wraps to just over the bottom of the palette");
	check(followsFormula(halfTurn, 0, 255, 0.5), "OFFSET 0.5: every grey is shifted half a turn, wrapping past the top");

	// Bands: white is in the top band, not one band past it and not back in the bottom one.
	p.offset = 0.f;
	p.bands = 4;
	videoBusPublishColorizer(2, 1, p);
	const Frame fourBands = render(r, 2);
	check(redAt(fourBands, 255) == redAt(fourBands, 192) && redAt(fourBands, 255) == redAt(fourBands, 254),
		"4 bands: white shares the top band with greys 192 to 254");
	check(std::abs(redAt(fourBands, 255) - expectedRed(3.5 / 4.0)) <= 1, "4 bands: white takes the colour at the centre of the top band");
	check(redAt(fourBands, 255) != redAt(fourBands, 0), "4 bands: white and black are in different bands");
	p.bands = 8;
	videoBusPublishColorizer(2, 1, p);
	const Frame eightBands = render(r, 2);
	check(redAt(eightBands, 255) == redAt(eightBands, 224) && std::abs(redAt(eightBands, 255) - expectedRed(7.5 / 8.0)) <= 1,
		"8 bands: white stays inside the last band, its colour is not pushed past the palette");

	videoBusClear(1);
	videoBusClear(2);
	videoExternalFrameRelease(frameId);
}

void testFrameRing() {
	std::printf("Frame ring\n");
	const int slot = frameRingAllocateSlot();
	check(slot > 0, "frame ring slot allocated");
	frameRingSetCapacity(slot, 3);
	for (int n = 1; n <= 4; ++n) {
		check(frameRingPush(slot, n, Frame(8*4*4, static_cast<unsigned char>(n*10)), 8, 4), "frame ring pushes a new counter");
	}
	check(frameRingCount(slot) == 3, "frame ring respects capacity");
	FrameRingFrame f;
	for (int age = 0; age < 3; ++age) {
		check(frameRingRead(slot, age, f) && f.frameCounter == 4-age && f.rgba[0] == (4-age)*10, "frame ring age ordering");
	}
	check(!frameRingRead(slot, 3, f), "frame ring rejects missing age");
	check(!frameRingPush(slot, 4, Frame(8*4*4, 99), 8, 4), "frame ring duplicate counter rejected");
	check(frameRingRead(slot, 0, f) && f.rgba[0] == 40, "duplicate leaves frame intact");
	check(frameRingIsStored(slot, 4) && !frameRingIsStored(slot, 3), "frame ring newest counter guard");
	check(frameRingPush(slot, 5, Frame(16*8*4, 50), 16, 8), "frame ring accepts changed size");
	check(frameRingRead(slot, 0, f) && f.width == 16 && f.height == 8 && f.rgba.size() == 16*8*4, "frame ring stores entry size");
	frameRingClear(slot, 1);
	check(frameRingCount(slot) == 0 && !frameRingRead(slot, 0, f), "frame ring clear empties history");
	frameRingPush(slot, 5, Frame(8*4*4, 60), 8, 4);
	frameRingClear(slot, 1);
	check(frameRingRead(slot, 0, f) && f.rgba[0] == 60, "clear counter handled only once");
	frameRingPush(slot, 6, Frame(8*4*4, 70), 8, 4);
	frameRingSetCapacity(slot, 1);
	check(frameRingCount(slot) == 1 && frameRingRead(slot, 0, f) && f.frameCounter == 6, "shrinking ring keeps newest");
	frameRingSetCapacity(slot, 4);
	frameRingPush(slot, 7, Frame(8*4*4, 80), 8, 4);
	check(frameRingRead(slot, 1, f) && f.frameCounter == 6, "growing ring preserves history");
	frameRingReleaseSlot(slot);
	check(frameRingCount(slot) == 0, "release drops history");
}

void testInverseMap() {
	float x = 0.f, y = 0.f;
	auto m = makeInverseMap(9, 9, 1.f, 0.f, 0.f, 0.f, InverseMapEdge::Clamp);
	check(inverseMapSource(m, 9, 9, 2.f, 3.f, x, y) && x == 2.f && y == 3.f, "inverse map identity");
	m = makeInverseMap(9, 9, 2.f, 0.f, 0.f, 0.f, InverseMapEdge::Clamp);
	inverseMapSource(m, 9, 9, 8.f, 4.f, x, y);
	check(std::abs(x-6.f) < 0.001f && y == 4.f, "inverse map zoom about centre");
	m = makeInverseMap(9, 9, 1.f, 0.25f, 0.f, 0.f, InverseMapEdge::Clamp);
	inverseMapSource(m, 9, 9, 4.f, 2.f, x, y);
	check(std::abs(x-6.f) < 0.001f && std::abs(y-4.f) < 0.001f, "inverse map positive rotation is counter-clockwise on screen");
	m = makeInverseMap(9, 9, 1.f, 0.f, 0.5f, 0.f, InverseMapEdge::Black);
	check(!inverseMapSource(m, 9, 9, 0.f, 4.f, x, y), "inverse map black rejects outside");
	m.edge = static_cast<int>(InverseMapEdge::Wrap);
	check(inverseMapSource(m, 9, 9, 0.f, 4.f, x, y) && std::abs(x-4.5f) < 0.001f, "inverse map wrap is positive");
}

void testFeedbackAccumulates() {
	std::printf("Feedback accumulation\n");
	publishOsc(1, 0.f, 4);
	VideoGraphRenderer r;
	const Frame source = render(r, 1);
	FeedbackParams p;
	p.ringSlotId = frameRingAllocateSlot();
	frameRingSetCapacity(p.ringSlotId, 1);
	p.gain = 1.f;
	p.frameCounter = 1;
	videoBusPublishFeedback(2, 1, p);
	const Frame pass1 = render(r, 2);
	check(maxDiff(source, pass1) <= 1, "feedback first pass equals source");
	p.frameCounter = 2;
	videoBusPublishFeedback(2, 1, p);
	const Frame pass2 = render(r, 2);
	check(lumaAt(pass2, kW/2, kH/2) > lumaAt(pass1, kW/2, kH/2), "feedback adds previous output");
	check(maxDiff(pass2, render(r, 2)) == 0, "feedback same tick does not advance");
	Frame pass5, pass12;
	for (int n = 3; n <= 12; ++n) {
		p.frameCounter = n;
		videoBusPublishFeedback(2, 1, p);
		const Frame next = render(r, 2);
		if (n == 5) pass5 = next;
		if (n == 12) pass12 = next;
	}
	check(lumaAt(pass12, kW/2, kH/2) > 0.98f, "feedback accumulates to white");
	p.clearCounter = 1;
	p.gain = 0.f;
	p.frameCounter = 13;
	videoBusPublishFeedback(2, 1, p);
	const Frame pass13 = render(r, 2);
	check(maxDiff(source, pass13) <= 1, "feedback clear restores source");
	p.zoom = 2.f;
	p.edge = static_cast<int>(InverseMapEdge::Black);
	p.gain = 1.f;
	for (int n = 14; n <= 15; ++n) {
		p.frameCounter = n;
		videoBusPublishFeedback(2, 1, p);
		check(maxDiff(pass13, render(r, 2)) > 10, "feedback magnified previous output changes frame");
	}
	saveProof("feedback.png", {pass1, pass2, pass5, pass12});
	frameRingReleaseSlot(p.ringSlotId);
	videoBusClear(2);
	videoBusClear(1);
}

void testFeedbackTwoRenderers() {
	std::printf("Feedback shared renderers\n");
	publishOsc(1, 0.f, 4);
	FeedbackParams p;
	p.ringSlotId = frameRingAllocateSlot();
	p.frameCounter = 1;
	p.gain = 0.6f;
	VideoGraphRenderer a, b;
	videoBusPublishFeedback(2, 1, p);
	render(a, 2);
	p.frameCounter = 2;
	videoBusPublishFeedback(2, 1, p);
	const Frame first = render(a, 2);
	check(maxDiff(first, render(b, 2)) == 0, "two renderers see identical feedback step");
	const Frame small = b.renderBus(2, 128, 72);
	auto mean = [](const Frame& f) {
		double sum = 0.;
		for (size_t i = 0; i < f.size(); i += 4) sum += (f[i]*0.2126+f[i+1]*0.7152+f[i+2]*0.0722)/255.;
		return f.empty() ? 0. : sum / (f.size()/4);
	};
	check(small.size() == 128*72*4 && std::abs(mean(small)-mean(first)) < 0.02, "different-size renderer resamples without another feedback step");
	FrameRingFrame stored;
	check(frameRingRead(p.ringSlotId, 0, stored) && stored.width == kW && stored.height == kH, "first renderer chooses stored size");
	p.mode = 1;
	p.gain = 0.f;
	p.frameCounter = 3;
	videoBusPublishFeedback(2, 1, p);
	const Frame source = render(a, 1);
	check(maxDiff(source, render(a, 2)) <= 1, "blend at zero gain equals source");
	frameRingReleaseSlot(p.ringSlotId);
	videoBusClear(2);
	videoBusClear(1);
}


void testFeedbackControls() {
	publishOsc(1, 0.f, 4);
	FeedbackParams p;
	p.ringSlotId = frameRingAllocateSlot();
	p.mode = 1;
	p.gain = 1.2f;
	p.frameCounter = 1;
	Frame red(kW*kH*4, 0);
	for (size_t i = 0; i < red.size(); i += 4) red[i] = red[i+3] = 255;
	VideoGraphRenderer r;
	auto seeded = [&]() {
		frameRingPush(p.ringSlotId, 0, red, kW, kH);
		videoBusPublishFeedback(2, 1, p);
		return render(r, 2);
	};
	check(maxDiff(seeded(), red) == 0, "blend caps gain at one");
	p.hue = 0.25f;
	check(maxDiff(seeded(), red) > 100, "feedback hue changes coloured history");
	p.hue = 0.f;
	p.key = 0.8f;
	const Frame keyed = seeded();
	check(lumaAt(keyed, kW/2, kH/2) == 0.f, "feedback key removes dark history");
	p.key = 0.f;
	p.offsetX = 0.5f;
	p.edge = static_cast<int>(InverseMapEdge::Black);
	const Frame blackEdge = seeded();
	check(lumaAt(blackEdge, 0, kH/2) == 0.f && lumaAt(blackEdge, kW-1, kH/2) > 0.2f, "feedback black edge clips transformed history");
	p.offsetX = 0.5f / kW;
	p.edge = static_cast<int>(InverseMapEdge::Wrap);
	check(maxDiff(seeded(), red) == 0, "feedback wrap interpolates across seam without black");
	p.edge = static_cast<int>(InverseMapEdge::Clamp);
	check(maxDiff(seeded(), red) == 0, "feedback clamp extends edge colour");
	frameRingReleaseSlot(p.ringSlotId);
	videoBusClear(2);
	videoBusClear(1);
}

// ---- Video Mixer (VRU-22) ----

Frame vmSolid(int r, int g, int b) {
	Frame f(static_cast<size_t>(kW) * kH * 4u);
	for (size_t i = 0; i < f.size(); i += 4) {
		f[i] = static_cast<unsigned char>(r);
		f[i + 1] = static_cast<unsigned char>(g);
		f[i + 2] = static_cast<unsigned char>(b);
		f[i + 3] = 255;
	}
	return f;
}

// A grey picture: fn(u, v) is the level, 0..1, at u = x / (kW - 1) and v = y / (kH - 1).
template <class Fn>
Frame vmGrey(Fn fn) {
	Frame f(static_cast<size_t>(kW) * kH * 4u);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
			const double level = std::clamp(fn(static_cast<double>(x) / (kW - 1), static_cast<double>(y) / (kH - 1)), 0.0, 1.0);
			const auto g = static_cast<unsigned char>(std::lround(level * 255.0));
			f[i] = f[i + 1] = f[i + 2] = g;
			f[i + 3] = 255;
		}
	}
	return f;
}

// Dark on the left, bright on the right: a straight wipe, or a plain grey picture to colour.
Frame vmRamp() {
	return vmGrey([](double u, double) { return u; });
}

// Bright at the top left, dark at the bottom right: a diagonal wipe.
Frame vmDiagonal() {
	return vmGrey([](double u, double v) { return 1.0 - (u + v) * 0.5; });
}

// Bright in the middle, dark in the corners: an iris.
Frame vmIris() {
	return vmGrey([](double u, double v) {
		const double dx = u * 2.0 - 1.0;
		const double dy = v * 2.0 - 1.0;
		return 1.0 - std::sqrt((dx * dx + dy * dy) * 0.5);
	});
}

// A white block on black, columns 64 to 191 and rows 36 to 107: a matte with hard edges.
Frame vmRect() {
	return vmGrey([](double u, double v) { return (u >= 0.25 && u < 0.75 && v >= 0.25 && v < 0.75) ? 1.0 : 0.0; });
}

// Rings of grey about the centre, so a coloured picture has more than one edge to show.
Frame vmRings() {
	return vmGrey([](double u, double v) {
		const double dx = (u * 2.0 - 1.0) * 1.78;
		const double dy = v * 2.0 - 1.0;
		return 0.5 + 0.5 * std::cos(std::sqrt(dx * dx + dy * dy) * 14.0);
	});
}

// A coloured picture that differs everywhere. R rises with x, G with y and B is a 16 px checker. Each variant (0 to 5)
// reads the three the other way round, and from 3 up in negative, so no two cards share a pixel.
Frame vmCard(int variant) {
	Frame f(static_cast<size_t>(kW) * kH * 4u);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
			const double base[3] = {0.1 + 0.8 * x / (kW - 1), 0.1 + 0.7 * y / (kH - 1), (((x / 16) + (y / 16)) & 1) ? 0.85 : 0.15};
			for (int c = 0; c < 3; ++c) {
				double v = base[(c + variant) % 3];
				if (variant >= 3) {
					v = 1.0 - v;
				}
				f[i + static_cast<size_t>(c)] = static_cast<unsigned char>(std::lround(v * 255.0));
			}
			f[i + 3] = 255;
		}
	}
	return f;
}

// A frame whose level at byte i, channel c, is fn(i, c) in 0..255, rounded.
template <class Fn>
Frame vmExpect(Fn fn) {
	Frame f(static_cast<size_t>(kW) * kH * 4u, 255);
	for (size_t i = 0; i < f.size(); i += 4) {
		for (int c = 0; c < 3; ++c) {
			f[i + static_cast<size_t>(c)] = static_cast<unsigned char>(std::lround(std::clamp(fn(i, c), 0.0, 255.0)));
		}
	}
	return f;
}

bool vmOpaque(const Frame& f) {
	if (f.empty()) {
		return false;
	}
	for (size_t i = 3; i < f.size(); i += 4) {
		if (f[i] != 255) {
			return false;
		}
	}
	return true;
}

int vmPx(const Frame& f, int x, int y, int channel) {
	return f[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u + static_cast<size_t>(channel)];
}

// check() with a printf-style message saying which case it was.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void vmCheckf(bool ok, const char* format, ...) {
	char text[256];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof text, format, args);
	va_end(args);
	check(ok, text);
}

double vmGreyOf(const Frame& f, size_t i) {
	return (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
}

// Hue h (turns, any value) at full saturation and brightness, as red, green and blue in 0..1. Written out here on its
// own, not borrowed from the renderer.
void vmHue(double h, double rgb[3]) {
	h -= std::floor(h);
	const double s = h * 6.0;
	const int sector = static_cast<int>(s);
	const double f = s - sector;
	switch (sector % 6) {
	case 0: rgb[0] = 1.0; rgb[1] = f; rgb[2] = 0.0; break;
	case 1: rgb[0] = 1.0 - f; rgb[1] = 1.0; rgb[2] = 0.0; break;
	case 2: rgb[0] = 0.0; rgb[1] = 1.0; rgb[2] = f; break;
	case 3: rgb[0] = 0.0; rgb[1] = 1.0 - f; rgb[2] = 1.0; break;
	case 4: rgb[0] = f; rgb[1] = 0.0; rgb[2] = 1.0; break;
	default: rgb[0] = 1.0; rgb[1] = 0.0; rgb[2] = 1.0 - f; break;
	}
}

// The multiplier COLOR gives input `index`: white pulled toward its hue by SAT. False when COLOR is off.
bool vmTint(const VideoMixerParams& p, int index, double tint[3]) {
	tint[0] = tint[1] = tint[2] = 1.0;
	if (!p.colorOn[index]) {
		return false;
	}
	double hue[3];
	vmHue(p.hue[index], hue);
	const double sat = std::clamp<double>(p.sat, 0.0, 1.0);
	for (int c = 0; c < 3; ++c) {
		tint[c] = 1.0 + (hue[c] - 1.0) * sat;
	}
	return true;
}

// The key's flat colour, hue KEY HUE at 0.9 saturation, full brightness.
void vmFlat(const VideoMixerParams& p, double rgb[3]) {
	double hue[3];
	vmHue(p.keyHue, hue);
	for (int c = 0; c < 3; ++c) {
		rgb[c] = 1.0 - 0.9 * (1.0 - hue[c]);
	}
}

// An independent reading of the Video Mixer maths in doubles, written from the spec and not from the renderer, for
// the pictures the renderer was handed (null: nothing in that role).
Frame vmReference(const Frame* air, const Frame* next, const Frame* pattern, const Frame* key, const Frame* matte, const VideoMixerParams& p) {
	const double fader = std::clamp<double>(p.fader, 0.0, 1.0);
	const double soft = std::clamp<double>(p.soft, 0.001, 1.0);
	const double black = std::clamp<double>(p.black, 0.0, 1.0);
	const double keySoft = std::clamp<double>(p.keySoft, 0.001, 1.0);
	double airTint[3], nextTint[3], flat[3];
	const bool airColor = vmTint(p, p.onAir, airTint);
	const bool nextColor = vmTint(p, p.next, nextTint);
	vmFlat(p, flat);
	const double edge = (1.0 + soft * 0.5) - fader * (1.0 + soft);
	const Frame* matteSource = matte != nullptr ? matte : key;
	const bool flatFill = p.keyFill == 1;
	const bool keyActive = p.keyOn && matteSource != nullptr && (flatFill || key != nullptr);

	return vmExpect([&](size_t i, int c) {
		auto load = [&](const Frame* pic, bool colored, const double* tint) {
			if (pic == nullptr) {
				return 0.0;
			}
			return colored ? vmGreyOf(*pic, i) * tint[c] : (*pic)[i + static_cast<size_t>(c)] / 255.0;
		};
		const double a = load(air, airColor, airTint);
		const double n = load(next, nextColor, nextTint);
		double v;
		if (fader <= 0.0) {
			v = a;
		}
		else if (fader >= 1.0) {
			v = n;
		}
		else if (p.mode == 0) {
			v = a * (1.0 - fader) + n * fader;
		}
		else if (p.mode == 1) {
			v = std::min(1.0, a * std::min(1.0, 2.0 * (1.0 - fader)) + n * std::min(1.0, 2.0 * fader));
		}
		else {
			const double l = pattern != nullptr ? vmGreyOf(*pattern, i) : (next != nullptr ? vmGreyOf(*next, i) : 0.0);
			const double m = std::clamp((l - edge) / soft + 0.5, 0.0, 1.0);
			v = a * (1.0 - m) + n * m;
		}
		if (keyActive) {
			double m = std::clamp((vmGreyOf(*matteSource, i) - p.keyThresh + keySoft * 0.5) / keySoft, 0.0, 1.0);
			if (p.keyInvert) {
				m = 1.0 - m;
			}
			const double fill = flatFill ? flat[c] : (*key)[i + static_cast<size_t>(c)] / 255.0;
			v = v * (1.0 - m) + fill * m;
		}
		return v * (1.0 - black) * 255.0;
	});
}

// Pictures on buses 1 to 7 (the four inputs, then PATTERN, KEY and MATTE as a test needs them) and the mixer on bus 9.
struct VmRig {
	static constexpr int kOut = 9;
	VideoGraphRenderer r;
	int frameId[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	Frame pic[8];

	void set(int bus, const Frame& f) {
		if (frameId[bus] == 0) {
			frameId[bus] = videoExternalFrameAllocate();
		}
		pic[bus] = f;
		videoExternalFramePublish(frameId[bus], f.data(), kW, kH, 0.0);
		videoBusPublishExternalFrame(bus, frameId[bus]);
	}

	// The picture on `bus` as the rig set it, or null for 0 and for a bus it did not set.
	const Frame* of(int bus) const {
		return (bus >= 1 && bus < 8 && frameId[bus] != 0) ? &pic[bus] : nullptr;
	}

	Frame run(const VideoMixerParams& p) {
		videoBusPublishVideoMixer(kOut, p);
		return render(r, kOut);
	}

	// What the maths says the mixer should draw for `p` from the pictures set on the buses.
	Frame expected(const VideoMixerParams& p) const {
		return vmReference(of(p.inputBus[p.onAir]), of(p.inputBus[p.next]), of(p.patternBus), of(p.keyBus), of(p.matteBus), p);
	}

	~VmRig() {
		videoBusClear(kOut);
		for (int bus = 1; bus < 8; ++bus) {
			if (frameId[bus] != 0) {
				videoBusClear(bus);
				videoExternalFrameRelease(frameId[bus]);
			}
		}
	}
};

// Four inputs on buses 1 to 4, A live, B next, dissolve, everything else off.
VideoMixerParams vmParams() {
	VideoMixerParams p;
	for (int i = 0; i < 4; ++i) {
		p.inputBus[i] = i + 1;
	}
	return p;
}

VideoMixerState::Inputs vmIn(float lever = 0.f) {
	VideoMixerState::Inputs in;
	in.lever = lever;
	return in;
}

void testVideoMixerState() {
	std::printf("Video Mixer state\n");
	const double dt = 0.001;

	// TIME and the NEXT voltage.
	check(std::abs(videoMixerTakeSeconds(0.f) - 0.02f) < 1e-6f, "TIME at the bottom is 20 ms");
	check(std::abs(videoMixerTakeSeconds(1.f) - 20.f) < 1e-3f, "TIME at the top is 20 s");
	check(std::abs(videoMixerTakeSeconds(0.5f) - 0.02f * std::sqrt(1000.f)) < 1e-4f, "TIME in the middle is the geometric mean, 0.63 s");
	check(videoMixerTakeSeconds(-1.f) == videoMixerTakeSeconds(0.f) && videoMixerTakeSeconds(2.f) == videoMixerTakeSeconds(1.f), "TIME stays inside its range");
	check(videoMixerInputFromVolts(0.f) == 0 && videoMixerInputFromVolts(2.49f) == 0, "0 to 2.5 V picks A");
	check(videoMixerInputFromVolts(2.5f) == 1 && videoMixerInputFromVolts(4.99f) == 1, "2.5 to 5 V picks B");
	check(videoMixerInputFromVolts(5.f) == 2 && videoMixerInputFromVolts(7.49f) == 2, "5 to 7.5 V picks C");
	check(videoMixerInputFromVolts(7.5f) == 3 && videoMixerInputFromVolts(10.f) == 3, "7.5 to 10 V picks D");
	check(videoMixerInputFromVolts(-4.f) == 0 && videoMixerInputFromVolts(14.f) == 3, "outside 0 to 10 V it stops at A and D");

	// The lever is a T-bar.
	VideoMixerState s;
	s.step(vmIn(0.f), dt);
	check(s.onAir == 0 && s.next == 1 && s.mix == 0.f, "a fresh mixer has A live, B next and nothing mixed");
	s.step(vmIn(0.4f), dt);
	check(std::abs(s.mix - 0.4f) < 1e-6f && s.onAir == 0, "the lever sets how far the next picture is up");
	s.step(vmIn(1.f), dt);
	check(s.onAir == 1 && s.next == 0 && s.mix == 0.f && s.flipped, "at the far end the two pictures swap, as on a T-bar");
	s.step(vmIn(1.f), dt);
	check(s.mix == 0.f && s.onAir == 1, "after the swap the lever left at that end shows only the new picture");
	s.step(vmIn(0.75f), dt);
	check(std::abs(s.mix - 0.25f) < 1e-6f && s.onAir == 1, "coming back, the lever brings the old picture up from the other end");
	s.step(vmIn(0.f), dt);
	check(s.onAir == 0 && s.next == 1 && !s.flipped && s.mix == 0.f, "back at the first end the two swap again");

	// A triangle LFO on the lever goes back and forth: one swap at each end.
	VideoMixerState t;
	int swaps = 0;
	int last = t.onAir;
	for (int i = 0; i < 4000; ++i) {
		const double phase = (i % 1000) / 1000.0;
		t.step(vmIn(static_cast<float>(phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0)), dt);
		if (t.onAir != last) {
			++swaps;
			last = t.onAir;
		}
	}
	check(swaps == 8 && t.onAir == 0, "a triangle on the lever swaps once at every peak and every trough");

	// A lever that stops short does not swap, and one that shakes at the end swaps once.
	VideoMixerState shy;
	shy.step(vmIn(0.99f), dt);
	shy.step(vmIn(0.f), dt);
	check(shy.onAir == 0 && !shy.flipped, "a lever that stops short of the end does not swap");
	VideoMixerState shaky;
	for (float lever : {0.996f, 0.99f, 0.997f, 0.98f, 0.999f}) {
		shaky.step(vmIn(lever), dt);
	}
	check(shaky.onAir == 1 && shaky.flipped, "a lever shaking at the end swaps once, not on every shake");

	// CUT.
	VideoMixerState c;
	VideoMixerState::Inputs cut = vmIn(0.f);
	cut.cut = true;
	c.step(cut, dt);
	check(c.onAir == 1 && c.next == 0 && c.mix == 0.f, "CUT makes the next picture live at once and the old one next");
	c.step(vmIn(0.f), dt);
	check(c.onAir == 1 && c.mix == 0.f, "and the new picture stays live");
	c.step(cut, dt);
	check(c.onAir == 0 && c.next == 1, "a second CUT goes back");
	VideoMixerState part;
	part.step(vmIn(0.5f), dt);
	check(std::abs(part.mix - 0.5f) < 1e-6f, "the lever is half way (test sanity)");
	VideoMixerState::Inputs partCut = vmIn(0.5f);
	partCut.cut = true;
	part.step(partCut, dt);
	check(part.onAir == 1 && part.mix == 0.f, "a CUT with the lever part-way shows the new picture whole, not a jump to half");
	part.step(vmIn(0.5f), dt);
	check(part.mix == 0.f && part.onAir == 1, "and it stays whole while the lever stays away from home");
	part.step(vmIn(0.01f), dt);
	part.step(vmIn(0.3f), dt);
	check(std::abs(part.mix - 0.3f) < 1e-6f, "once the lever has been home again it moves the picture again");

	// AUTO.
	VideoMixerState a;
	VideoMixerState::Inputs go = vmIn(0.f);
	go.take = true;
	go.takeSeconds = 1.f;
	a.step(go, dt);
	check(a.taking && a.mix > 0.f && a.mix < 0.01f, "AUTO starts a take");
	bool rising = true;
	float before = a.mix;
	for (int i = 0; i < 499; ++i) {
		a.step(vmIn(0.f), dt);
		if (a.mix <= before) {
			rising = false;
		}
		before = a.mix;
	}
	check(rising, "a take only goes up");
	check(std::abs(a.mix - 0.5f) < 0.002f, "half the TIME in, the take is half way");
	a.step(go, dt);
	check(a.mix > 0.5f && a.mix < 0.503f, "AUTO again during a take does not start it over");
	int ticks = 501;
	while (a.taking && ticks < 5000) {
		a.step(vmIn(0.f), dt);
		++ticks;
	}
	check(ticks >= 995 && ticks <= 1005, "the take lasts TIME");
	check(a.onAir == 1 && a.next == 0 && a.mix == 0.f && !a.taking, "at the end of a take the next picture is live");

	VideoMixerState cutIn;
	cutIn.step(go, dt);
	for (int i = 0; i < 100; ++i) {
		cutIn.step(vmIn(0.f), dt);
	}
	cutIn.step(cut, dt);
	check(cutIn.onAir == 1 && !cutIn.taking && cutIn.mix == 0.f, "CUT during a take ends it on the new picture");

	VideoMixerState fromLever;
	fromLever.step(vmIn(0.3f), dt);
	VideoMixerState::Inputs goFrom = go;
	goFrom.lever = 0.3f;
	fromLever.step(goFrom, dt);
	check(fromLever.mix > 0.3f && fromLever.mix < 0.31f, "AUTO takes up from where the lever stands");
	for (int i = 0; i < 2000 && fromLever.taking; ++i) {
		fromLever.step(vmIn(0.3f), dt);
	}
	check(fromLever.onAir == 1 && fromLever.mix == 0.f, "and ends on the new picture, shown whole while the lever is away from home");
	fromLever.step(vmIn(0.f), dt);
	fromLever.step(vmIn(0.3f), dt);
	check(std::abs(fromLever.mix - 0.3f) < 1e-6f, "the lever works again after it has been home");

	// NEXT by button or trigger, by voltage; unpatched inputs are skipped.
	VideoMixerState n;
	VideoMixerState::Inputs stepIn = vmIn(0.f);
	stepIn.step = true;
	n.step(stepIn, dt);
	check(n.next == 2 && n.onAir == 0, "a NEXT trigger steps to the following input");
	n.step(stepIn, dt);
	check(n.next == 3, "and on");
	n.step(stepIn, dt);
	check(n.next == 1, "and round, skipping the live picture");
	VideoMixerState u;
	VideoMixerState::Inputs partial = vmIn(0.f);
	partial.step = true;
	partial.patched = 0x5u;   // A and C
	u.step(partial, dt);
	check(u.next == 2, "an input with nothing patched is skipped");
	u.step(partial, dt);
	check(u.next == 2, "with only A live and C patched, NEXT stays on C");
	VideoMixerState idle;
	VideoMixerState::Inputs partialIdle = vmIn(0.f);
	partialIdle.patched = 0x5u;
	idle.step(partialIdle, dt);
	check(idle.next == 2, "a fresh mixer whose B is empty makes C next on its own");
	partialIdle.patched = 0x1u;
	VideoMixerState alone;
	alone.step(partialIdle, dt);
	check(alone.onAir == 0 && alone.next == 1, "with one picture patched there is nothing else to make next");
	VideoMixerState midMix;
	VideoMixerState::Inputs half = vmIn(0.5f);
	half.patched = 0x7u;   // A, B and C
	midMix.step(half, dt);
	check(midMix.next == 1 && std::abs(midMix.mix - 0.5f) < 1e-6f, "B is next and half way up (test sanity)");
	half.patched = 0x5u;   // B's cable comes out
	midMix.step(half, dt);
	check(midMix.next == 1 && std::abs(midMix.mix - 0.5f) < 1e-6f, "pulling the next cable mid-mix does not change what is coming up");
	midMix.step(vmIn(0.f), dt);
	VideoMixerState::Inputs rest = vmIn(0.f);
	rest.patched = 0x5u;
	midMix.step(rest, dt);
	check(midMix.next == 2, "once the lever is home the empty input is swapped for the next patched one");

	VideoMixerState v;
	VideoMixerState::Inputs volts = vmIn(0.f);
	volts.select = 3;
	v.step(volts, dt);
	check(v.next == 3, "the NEXT voltage picks the input");
	volts.select = 2;
	v.step(volts, dt);
	check(v.next == 2, "and follows when it changes");
	volts.step = true;
	volts.select = 1;
	v.step(volts, dt);
	check(v.next == 1, "with a voltage patched a trigger does not step");

	// ADVANCE: a clock walks the four inputs.
	VideoMixerState w;
	VideoMixerState::Inputs walk = vmIn(0.f);
	walk.cut = true;
	walk.advance = true;
	const int walkAir[5] = {1, 2, 3, 0, 1};
	bool walked = true;
	for (int i = 0; i < 5; ++i) {
		w.step(walk, dt);
		walked = walked && w.onAir == walkAir[i] && w.next == (walkAir[i] + 1) % 4;
	}
	check(walked, "with ADVANCE each CUT makes the next picture live and the following one next, round all four");
	VideoMixerState skip;
	walk.patched = 0xDu;   // A, C and D; B is empty
	int seen[4] = {0, 0, 0, 0};
	for (int i = 0; i < 6; ++i) {
		skip.step(walk, dt);
		seen[skip.onAir] += 1;
	}
	check(seen[1] == 0 && seen[0] == 2 && seen[2] == 2 && seen[3] == 2, "ADVANCE skips an input with nothing patched");
	VideoMixerState trade;
	VideoMixerState::Inputs plain = vmIn(0.f);
	plain.cut = true;
	trade.step(plain, dt);
	trade.step(plain, dt);
	trade.step(plain, dt);
	check(trade.onAir == 1 && trade.next == 0, "without ADVANCE two pictures just trade places");
	VideoMixerState lfo;
	VideoMixerState::Inputs peak = vmIn(1.f);
	peak.advance = true;
	lfo.step(peak, dt);
	check(lfo.onAir == 1 && lfo.next == 2, "with ADVANCE a lever that reaches the far end sets up the following input");
	VideoMixerState::Inputs trough = vmIn(0.f);
	trough.advance = true;
	lfo.step(trough, dt);
	check(lfo.onAir == 2 && lfo.next == 3 && !lfo.flipped, "and coming back to the first end it walks on again, so a triangle on FADER visits every input");

	VideoMixerState autoWalk;
	VideoMixerState::Inputs goWalk = vmIn(0.f);
	goWalk.take = true;
	goWalk.advance = true;
	goWalk.takeSeconds = 0.05f;
	VideoMixerState::Inputs holdWalk = vmIn(0.f);
	holdWalk.advance = true;
	holdWalk.takeSeconds = 0.05f;
	for (int i = 0; i < 4; ++i) {
		autoWalk.step(goWalk, dt);
		for (int k = 0; k < 80; ++k) {
			autoWalk.step(holdWalk, dt);
		}
	}
	check(autoWalk.onAir == 0 && autoWalk.next == 1 && !autoWalk.taking, "AUTO with ADVANCE walks round the inputs, one take each");

	// RESET and a lever outside its range.
	VideoMixerState bad;
	bad.step(vmIn(7.f), dt);
	check(bad.onAir == 1, "a lever above the range counts as the far end");
	bad.reset();
	check(bad.onAir == 0 && bad.next == 1 && !bad.flipped && bad.armed && !bad.taking, "reset puts the mixer back to A live, B next");
}

// A smooth coloured picture, for the proofs and for tests that want structure in every channel. Variant 0: hue sweeps
// left to right, darker toward the bottom. 1: rings of hue about the centre, bright in the middle. 2: diagonal bands.
// 3: hue down the picture, brighter to the right.
Frame vmScene(int variant) {
	Frame f(static_cast<size_t>(kW) * kH * 4u);
	const double pi = 3.14159265358979323846;
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const double u = static_cast<double>(x) / (kW - 1);
			const double v = static_cast<double>(y) / (kH - 1);
			const double dx = (u * 2.0 - 1.0) * 1.78;
			const double dy = v * 2.0 - 1.0;
			const double r = std::min(1.0, std::sqrt(dx * dx + dy * dy) / 1.6);
			double hue = 0.0;
			double sat = 0.85;
			double val = 1.0;
			switch (variant) {
			case 0:
				hue = 0.02 + 0.7 * u;
				val = 1.0 - 0.6 * v;
				break;
			case 1:
				hue = 0.55 + 0.9 * r;
				sat = 0.8;
				val = 1.0 - 0.75 * r;
				break;
			case 2: {
				const double band = 3.0 * (u * 0.6 + v);
				hue = band - std::floor(band);
				sat = 0.9;
				val = 0.7 + 0.3 * std::cos(2.0 * pi * 4.0 * (u * 0.6 + v));
				break;
			}
			default:
				hue = 0.35 + 0.5 * v;
				sat = 0.8;
				val = 0.25 + 0.75 * u;
				break;
			}
			double rgb[3];
			vmHue(hue, rgb);
			const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
			for (int c = 0; c < 3; ++c) {
				f[i + static_cast<size_t>(c)] = static_cast<unsigned char>(std::lround(255.0 * std::clamp(val * (1.0 - sat * (1.0 - rgb[c])), 0.0, 1.0)));
			}
			f[i + 3] = 255;
		}
	}
	return f;
}

// A white disc on black.
Frame vmDisc() {
	return vmGrey([](double u, double v) {
		const double dx = (u - 0.5) * 1.78;
		const double dy = v - 0.5;
		return std::sqrt(dx * dx + dy * dy) < 0.3 ? 1.0 : 0.0;
	});
}

// A soft blob: white in the middle, falling to black toward the edges.
Frame vmBlob() {
	return vmGrey([](double u, double v) {
		const double dx = (u - 0.5) * 1.78;
		const double dy = v - 0.5;
		return 1.0 - std::sqrt(dx * dx + dy * dy) / 0.55;
	});
}

// Saves rows of panels as one picture with a white 4 px gutter between panels and between rows.
void vmSaveGrid(const std::string& name, const std::vector<std::vector<Frame>>& rows) {
	const int gutter = 4;
	size_t columns = 0;
	for (const std::vector<Frame>& row : rows) {
		columns = std::max(columns, row.size());
	}
	const int width = static_cast<int>(columns) * kW + (static_cast<int>(columns) - 1) * gutter;
	const int height = static_cast<int>(rows.size()) * kH + (static_cast<int>(rows.size()) - 1) * gutter;
	Frame image(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255);
	for (size_t row = 0; row < rows.size(); ++row) {
		for (size_t col = 0; col < rows[row].size(); ++col) {
			const Frame& panel = rows[row][col];
			if (panel.size() != static_cast<size_t>(kW) * kH * 4u) {
				continue;
			}
			const size_t x0 = col * static_cast<size_t>(kW + gutter);
			const size_t y0 = row * static_cast<size_t>(kH + gutter);
			for (int y = 0; y < kH; ++y) {
				std::memcpy(&image[((y0 + static_cast<size_t>(y)) * static_cast<size_t>(width) + x0) * 4u], &panel[static_cast<size_t>(y) * kW * 4u], static_cast<size_t>(kW) * 4u);
			}
		}
	}
	const std::string path = gProofDir + "/" + name;
	if (!writePng(path, width, height, image)) {
		std::printf("  could not write %s\n", path.c_str());
		check(false, "proof image written");
	}
	else {
		std::printf("  proof: %s\n", path.c_str());
	}
}

// The share of a row that is brighter than half, for a picture of black and white.
double vmCoverage(const Frame& f, int row) {
	int count = 0;
	for (int x = 0; x < kW; ++x) {
		if (vmPx(f, x, row, 0) > 127) {
			++count;
		}
	}
	return static_cast<double>(count) / kW;
}

bool vmAll(const Frame& f, int r, int g, int b) {
	for (size_t i = 0; i < f.size(); i += 4) {
		if (f[i] != r || f[i + 1] != g || f[i + 2] != b || f[i + 3] != 255) {
			return false;
		}
	}
	return !f.empty();
}

void testVideoMixerEnds() {
	std::printf("Video Mixer ends and inputs\n");
	VmRig rig;
	Frame cards[4];
	for (int i = 0; i < 4; ++i) {
		cards[i] = vmCard(i);
		rig.set(i + 1, cards[i]);
	}
	const char* modeName[3] = {"dissolve", "add", "wipe"};
	for (int mode = 0; mode < 3; ++mode) {
		bool airOk = true;
		bool nextOk = true;
		bool opaque = true;
		for (int air = 0; air < 4; ++air) {
			for (int step = 1; step < 4; ++step) {
				VideoMixerParams p = vmParams();
				p.mode = mode;
				p.onAir = air;
				p.next = (air + step) % 4;
				p.fader = 0.f;
				Frame f = rig.run(p);
				airOk = airOk && maxDiff(f, cards[air]) == 0;
				opaque = opaque && vmOpaque(f);
				p.fader = 1.f;
				f = rig.run(p);
				nextOk = nextOk && maxDiff(f, cards[p.next]) == 0;
				opaque = opaque && vmOpaque(f);
			}
		}
		vmCheckf(airOk, "%s: with the fader at 0 the live picture passes through untouched, for every pair of inputs", modeName[mode]);
		vmCheckf(nextOk, "%s: with the fader at 1 the next picture passes through untouched, for every pair of inputs", modeName[mode]);
		vmCheckf(opaque, "%s: the output is opaque", modeName[mode]);
	}

	VideoMixerParams p = vmParams();
	p.fader = -0.5f;
	check(maxDiff(rig.run(p), cards[0]) == 0, "a fader below 0 counts as 0");
	p.fader = 3.f;
	check(maxDiff(rig.run(p), cards[1]) == 0, "a fader above 1 counts as 1");

	// The same input live and next: any fader position shows that picture (add excepted, it adds the picture to itself).
	for (int mode : {0, 2}) {
		p.mode = mode;
		p.onAir = 2;
		p.next = 2;
		p.fader = 0.37f;
		vmCheckf(maxDiff(rig.run(p), cards[2]) <= 1, "%s: one input live and next stays that picture", modeName[mode]);
	}

	// An input with nothing in it is black.
	VideoMixerParams gap = vmParams();
	gap.inputBus[0] = 0;
	gap.fader = 0.f;
	check(vmAll(rig.run(gap), 0, 0, 0), "an unpatched live input is black");
	gap.fader = 1.f;
	check(maxDiff(rig.run(gap), cards[1]) == 0, "the next picture still arrives when the live one is missing");
	gap.fader = 0.5f;
	const Frame half = rig.run(gap);
	const Frame expectHalf = vmExpect([&](size_t i, int c) { return cards[1][i + static_cast<size_t>(c)] * 0.5; });
	check(maxDiff(half, expectHalf) <= 1, "a dissolve from a missing picture is the next picture at half strength");
	gap.inputBus[1] = 0;
	check(vmAll(rig.run(gap), 0, 0, 0), "a mixer with nothing patched draws black");
	VideoMixerParams dead = vmParams();
	dead.inputBus[1] = 8;   // a bus nothing publishes on
	dead.fader = 1.f;
	check(vmAll(rig.run(dead), 0, 0, 0), "an input on a bus with nothing on it is black");
}

void testVideoMixerDissolveAndAdd() {
	std::printf("Video Mixer dissolve and add\n");
	VmRig rig;
	rig.set(1, vmSolid(200, 100, 0));
	rig.set(2, vmSolid(0, 100, 200));
	VideoMixerParams p = vmParams();

	p.mode = 0;
	bool crossFade = true;
	for (float f : {0.f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.f}) {
		p.fader = f;
		const Frame out = rig.run(p);
		const double r = 200.0 * (1.0 - f);
		const double b = 200.0 * f;
		const bool ok = std::abs(vmPx(out, 100, 70, 0) - r) <= 1.0 && std::abs(vmPx(out, 100, 70, 1) - 100.0) <= 1.0 && std::abs(vmPx(out, 100, 70, 2) - b) <= 1.0 && vmOpaque(out);
		vmCheckf(ok, "dissolve at %.2f is the cross-fade, got %d %d %d", f, vmPx(out, 100, 70, 0), vmPx(out, 100, 70, 1), vmPx(out, 100, 70, 2));
		crossFade = crossFade && ok;
	}
	check(crossFade, "dissolve: every stop is a straight cross-fade");

	p.mode = 1;
	p.fader = 0.5f;
	Frame out = rig.run(p);
	vmCheckf(std::abs(vmPx(out, 100, 70, 0) - 200) <= 1 && std::abs(vmPx(out, 100, 70, 1) - 200) <= 1 && std::abs(vmPx(out, 100, 70, 2) - 200) <= 1,
			 "add at halfway has both pictures at full strength and their colours add, got %d %d %d", vmPx(out, 100, 70, 0), vmPx(out, 100, 70, 1), vmPx(out, 100, 70, 2));
	p.fader = 0.25f;
	out = rig.run(p);
	vmCheckf(std::abs(vmPx(out, 100, 70, 0) - 200) <= 1 && std::abs(vmPx(out, 100, 70, 1) - 150) <= 1 && std::abs(vmPx(out, 100, 70, 2) - 100) <= 1,
			 "add at a quarter has the old picture full and the new at half, got %d %d %d", vmPx(out, 100, 70, 0), vmPx(out, 100, 70, 1), vmPx(out, 100, 70, 2));
	p.fader = 0.75f;
	out = rig.run(p);
	vmCheckf(std::abs(vmPx(out, 100, 70, 0) - 100) <= 1 && std::abs(vmPx(out, 100, 70, 1) - 150) <= 1 && std::abs(vmPx(out, 100, 70, 2) - 200) <= 1,
			 "add at three quarters has the old picture at half and the new full, got %d %d %d", vmPx(out, 100, 70, 0), vmPx(out, 100, 70, 1), vmPx(out, 100, 70, 2));

	// Bright pictures add past white and stop there.
	rig.set(1, vmSolid(200, 200, 200));
	rig.set(2, vmSolid(200, 200, 200));
	p.fader = 0.5f;
	check(vmAll(rig.run(p), 255, 255, 255), "add stops at white");
	// Add is brighter than dissolve for the same two pictures at the same fader.
	rig.set(1, vmCard(0));
	rig.set(2, vmCard(1));
	p.fader = 0.5f;
	const Frame added = rig.run(p);
	p.mode = 0;
	const Frame dissolved = rig.run(p);
	double addedSum = 0.0;
	double dissolvedSum = 0.0;
	for (size_t i = 0; i < added.size(); i += 4) {
		addedSum += added[i] + added[i + 1] + added[i + 2];
		dissolvedSum += dissolved[i] + dissolved[i + 1] + dissolved[i + 2];
	}
	check(addedSum > dissolvedSum * 1.4, "halfway through an add the picture is much brighter than halfway through a dissolve");
}

void testVideoMixerWipe() {
	std::printf("Video Mixer wipe\n");
	VmRig rig;
	rig.set(1, vmSolid(0, 0, 0));
	rig.set(2, vmSolid(255, 255, 255));
	rig.set(5, vmRamp());   // PATTERN: dark on the left, bright on the right
	VideoMixerParams p = vmParams();
	p.mode = 2;
	p.patternBus = 5;
	p.soft = 0.001f;

	// The new picture arrives brightest parts first, and the share of it that has arrived follows the fader.
	bool coverageOk = true;
	for (int k = 1; k <= 9; ++k) {
		p.fader = static_cast<float>(k) / 10.f;
		const double covered = vmCoverage(rig.run(p), 70);
		const bool ok = std::abs(covered - p.fader) <= 0.02;
		vmCheckf(ok, "wipe with a ramp PATTERN at %.1f covers %.3f", p.fader, covered);
		coverageOk = coverageOk && ok;
	}
	check(coverageOk, "a hard wipe covers as much of the picture as the fader says");
	p.fader = 0.2f;
	Frame out = rig.run(p);
	check(vmPx(out, 250, 70, 0) == 255 && vmPx(out, 215, 70, 0) == 255 && vmPx(out, 190, 70, 0) == 0 && vmPx(out, 10, 70, 0) == 0, "the bright end of the pattern is covered first");

	// Soft edge: SOFT is the width of the transition in grey levels, centred on the threshold.
	p.soft = 0.25f;
	p.fader = 0.5f;
	out = rig.run(p);
	int band = 0;
	for (int x = 0; x < kW; ++x) {
		const int level = vmPx(out, x, 70, 0);
		if (level > 0 && level < 255) {
			++band;
		}
	}
	vmCheckf(std::abs(band - static_cast<int>(0.25 * (kW - 1))) <= 3, "SOFT 0.25 on a ramp makes an edge about 64 px wide, got %d", band);
	vmCheckf(std::abs(vmPx(out, 128, 70, 0) - 128) <= 3, "and the middle of the edge is half way, got %d", vmPx(out, 128, 70, 0));
	bool narrower = true;
	int lastBand = 1 << 20;
	for (float soft : {0.6f, 0.3f, 0.1f, 0.02f}) {
		p.soft = soft;
		const Frame f = rig.run(p);
		int width = 0;
		for (int x = 0; x < kW; ++x) {
			const int level = vmPx(f, x, 70, 0);
			if (level > 0 && level < 255) {
				++width;
			}
		}
		narrower = narrower && width < lastBand;
		lastBand = width;
	}
	check(narrower, "a smaller SOFT makes a narrower edge");

	// Every pixel only ever moves toward the new picture as the fader rises, for a diagonal pattern as well.
	p.soft = 0.2f;
	for (int shape = 0; shape < 3; ++shape) {
		rig.set(5, shape == 0 ? vmRamp() : (shape == 1 ? vmDiagonal() : vmBlob()));
		std::vector<Frame> steps;
		for (int k = 0; k <= 20; ++k) {
			p.fader = static_cast<float>(k) / 20.f;
			steps.push_back(rig.run(p));
		}
		bool monotone = true;
		for (size_t k = 1; k < steps.size(); ++k) {
			for (size_t i = 0; i < steps[k].size(); i += 4) {
				if (steps[k][i] < steps[k - 1][i]) {
					monotone = false;
				}
			}
		}
		vmCheckf(monotone, "pattern %d: no pixel ever goes back toward the old picture as the fader rises", shape);
		vmCheckf(vmAll(steps.front(), 0, 0, 0) && vmAll(steps.back(), 255, 255, 255), "pattern %d: the wipe starts on the old picture and ends on the new", shape);
	}
	rig.set(5, vmDiagonal());
	p.soft = 0.001f;
	p.fader = 0.2f;
	out = rig.run(p);
	check(vmPx(out, 5, 5, 0) == 255 && vmPx(out, 250, 138, 0) == 0, "a diagonal pattern arrives from its bright corner");
	rig.set(5, vmBlob());
	out = rig.run(p);
	check(vmPx(out, 128, 72, 0) == 255 && vmPx(out, 4, 4, 0) == 0, "an iris pattern arrives from the centre");

	// With nothing in PATTERN the wipe uses the next picture's own greys, brightest parts first, whatever COLOR does.
	rig.set(1, vmSolid(255, 0, 0));
	rig.set(2, vmRamp());
	p.patternBus = 0;
	p.soft = 0.001f;
	p.fader = 0.5f;
	out = rig.run(p);
	check(vmPx(out, 40, 70, 0) == 255 && vmPx(out, 40, 70, 1) == 0 && vmPx(out, 230, 70, 0) == 230 && vmPx(out, 230, 70, 1) == 230, "with no PATTERN the next picture keys itself in over the old one, brightest parts first");
	int firstPlain = -1;
	for (int x = 0; x < kW && firstPlain < 0; ++x) {
		if (vmPx(out, x, 70, 1) != 0) {
			firstPlain = x;
		}
	}
	p.colorOn[1] = true;
	p.hue[1] = 0.6667f;
	p.sat = 1.f;
	out = rig.run(p);
	int firstTinted = -1;
	for (int x = 0; x < kW && firstTinted < 0; ++x) {
		if (vmPx(out, x, 70, 2) != 0) {
			firstTinted = x;
		}
	}
	vmCheckf(firstPlain > 0 && firstPlain == firstTinted, "the wipe's shape comes from the picture's own greys, not its colour (edge at %d plain, %d coloured)", firstPlain, firstTinted);
	vmCheckf(vmPx(out, 230, 70, 0) == 0 && vmPx(out, 230, 70, 1) == 0 && vmPx(out, 230, 70, 2) == 230, "and the new picture arrives in its COLOR (%d %d %d)", vmPx(out, 230, 70, 0), vmPx(out, 230, 70, 1), vmPx(out, 230, 70, 2));

	// With no PATTERN and no next picture the pattern is black: the old picture stays, then the last stretch of
	// the fader fades it away.
	VideoMixerParams alone = vmParams();
	alone.mode = 2;
	alone.inputBus[1] = 0;
	rig.set(1, vmCard(3));
	alone.fader = 0.5f;
	check(maxDiff(rig.run(alone), vmCard(3)) == 0, "a wipe to a missing picture leaves the old one alone to begin with");
	alone.fader = 1.f;
	check(vmAll(rig.run(alone), 0, 0, 0), "and ends on black");
	double lastSum = 1e18;
	bool fading = true;
	for (int k = 0; k <= 10; ++k) {
		alone.fader = 0.8f + 0.02f * static_cast<float>(k);
		const Frame f = rig.run(alone);
		double sum = 0.0;
		for (size_t i = 0; i < f.size(); i += 4) {
			sum += f[i] + f[i + 1] + f[i + 2];
		}
		fading = fading && sum <= lastSum;
		lastSum = sum;
	}
	check(fading, "the fade over the last stretch only gets darker");
}

void testVideoMixerColor() {
	std::printf("Video Mixer colour\n");
	VmRig rig;
	const Frame ramp = vmRamp();
	const Frame card = vmCard(2);
	rig.set(1, ramp);
	rig.set(2, card);
	rig.set(3, vmSolid(0, 0, 0));
	rig.set(4, vmSolid(255, 255, 255));
	VideoMixerParams p = vmParams();
	p.fader = 0.f;
	const int x = 200;
	const int y = 70;

	check(maxDiff(rig.run(p), ramp) == 0, "COLOR off leaves the picture alone");

	p.colorOn[0] = true;
	p.hue[0] = 0.f;
	p.sat = 1.f;
	Frame f = rig.run(p);
	vmCheckf(vmPx(f, x, y, 0) == 200 && vmPx(f, x, y, 1) == 0 && vmPx(f, x, y, 2) == 0, "hue 0 at full SAT turns grey 200 into red 200, got %d %d %d", vmPx(f, x, y, 0), vmPx(f, x, y, 1), vmPx(f, x, y, 2));
	p.hue[0] = 0.5f;
	f = rig.run(p);
	vmCheckf(vmPx(f, x, y, 0) == 0 && vmPx(f, x, y, 1) == 200 && vmPx(f, x, y, 2) == 200, "hue 0.5 gives cyan, got %d %d %d", vmPx(f, x, y, 0), vmPx(f, x, y, 1), vmPx(f, x, y, 2));
	p.hue[0] = 0.f;
	p.sat = 0.5f;
	f = rig.run(p);
	vmCheckf(std::abs(vmPx(f, x, y, 0) - 200) <= 1 && std::abs(vmPx(f, x, y, 1) - 100) <= 1 && std::abs(vmPx(f, x, y, 2) - 100) <= 1, "SAT 0.5 pulls red half way to grey, got %d %d %d", vmPx(f, x, y, 0), vmPx(f, x, y, 1), vmPx(f, x, y, 2));
	p.sat = 0.f;
	f = rig.run(p);
	vmCheckf(vmPx(f, x, y, 0) == 200 && vmPx(f, x, y, 1) == 200 && vmPx(f, x, y, 2) == 200, "SAT 0 is plain grey whatever the hue, got %d %d %d", vmPx(f, x, y, 0), vmPx(f, x, y, 1), vmPx(f, x, y, 2));

	// One hue per picture: a coloured card has only its brightness left, so at full SAT a red card is red and nothing else.
	VideoMixerParams cp = vmParams();
	cp.onAir = 1;
	cp.next = 0;
	cp.fader = 0.f;
	cp.colorOn[1] = true;
	cp.hue[1] = 0.f;
	cp.sat = 1.f;
	f = rig.run(cp);
	bool redOnly = true;
	for (size_t i = 0; i < f.size(); i += 4) {
		redOnly = redOnly && f[i + 1] == 0 && f[i + 2] == 0;
	}
	check(redOnly, "a coloured picture keeps one hue: at hue 0 and full SAT there is no green or blue anywhere");
	cp.sat = 0.f;
	f = rig.run(cp);
	const Frame cardGrey = vmExpect([&](size_t i, int) { return vmGreyOf(card, i) * 255.0; });
	check(maxDiff(f, cardGrey) <= 1, "at SAT 0 a coloured input is the plain grey of the picture");
	cp.colorOn[1] = false;
	check(maxDiff(rig.run(cp), card) == 0, "with COLOR off the same picture comes through in its own colours");

	// Black stays black, white becomes the full tint.
	VideoMixerParams bw = vmParams();
	bw.onAir = 2;
	bw.next = 3;
	bw.colorOn[2] = bw.colorOn[3] = true;
	bw.hue[2] = bw.hue[3] = 0.25f;
	bw.sat = 1.f;
	bw.fader = 0.f;
	check(vmAll(rig.run(bw), 0, 0, 0), "black stays black in any hue");
	bw.fader = 1.f;
	f = rig.run(bw);
	double tint[3];
	vmTint(bw, 3, tint);
	vmCheckf(std::abs(vmPx(f, 5, 5, 0) - 255.0 * tint[0]) <= 1 && std::abs(vmPx(f, 5, 5, 1) - 255.0 * tint[1]) <= 1 && std::abs(vmPx(f, 5, 5, 2) - 255.0 * tint[2]) <= 1, "white becomes the full hue, got %d %d %d", vmPx(f, 5, 5, 0), vmPx(f, 5, 5, 1), vmPx(f, 5, 5, 2));

	// The colour belongs to the input, not to the place it is in: B live and A next, with only A coloured.
	VideoMixerParams swap = vmParams();
	swap.onAir = 1;
	swap.next = 0;
	swap.colorOn[0] = true;
	swap.hue[0] = 0.f;
	swap.sat = 1.f;
	swap.fader = 0.f;
	check(maxDiff(rig.run(swap), card) == 0, "B live shows its own colours although A has a hue");
	swap.fader = 1.f;
	f = rig.run(swap);
	vmCheckf(vmPx(f, x, y, 0) == 200 && vmPx(f, x, y, 1) == 0, "and A, now next, arrives in its hue, got %d %d", vmPx(f, x, y, 0), vmPx(f, x, y, 1));

	// Each input has its own hue, and two complementary hues in a dissolve make grey.
	rig.set(3, ramp);
	VideoMixerParams two = vmParams();
	two.inputBus[1] = 3;
	two.colorOn[0] = two.colorOn[1] = true;
	two.hue[0] = 0.f;
	two.hue[1] = 0.5f;
	two.sat = 1.f;
	two.fader = 0.5f;
	f = rig.run(two);
	vmCheckf(std::abs(vmPx(f, x, y, 0) - 100) <= 1 && std::abs(vmPx(f, x, y, 1) - 100) <= 1 && std::abs(vmPx(f, x, y, 2) - 100) <= 1, "red and cyan versions of one grey, dissolved half and half, are grey again, got %d %d %d", vmPx(f, x, y, 0), vmPx(f, x, y, 1), vmPx(f, x, y, 2));

	// SHIFT turns every hue: 10 V is once round, 5 V half a turn.
	check(std::abs(videoMixerShiftedHue(0.25f, 0.f) - 0.25f) < 1e-6f, "no SHIFT leaves a hue alone");
	check(std::abs(videoMixerShiftedHue(0.25f, 10.f) - 1.25f) < 1e-6f, "10 V of SHIFT adds a whole turn");
	check(std::abs(videoMixerShiftedHue(0.f, 5.f) - 0.5f) < 1e-6f, "5 V of SHIFT adds half a turn");
	check(std::abs(videoMixerShiftedHue(0.5f, -5.f) - 0.f) < 1e-6f, "-5 V of SHIFT turns back half a turn");
	VideoMixerParams a = vmParams();
	a.colorOn[0] = true;
	a.hue[0] = 0.25f;
	a.fader = 0.f;
	rig.set(1, card);
	const Frame plainHue = rig.run(a);
	a.hue[0] = videoMixerShiftedHue(0.25f, 10.f);
	check(maxDiff(rig.run(a), plainHue) == 0, "a hue shifted by 10 V is the same colour");
	a.hue[0] = videoMixerShiftedHue(0.f, 5.f);
	const Frame halfTurn = rig.run(a);
	a.hue[0] = 0.5f;
	check(maxDiff(rig.run(a), halfTurn) == 0, "0 plus 5 V is hue 0.5");
	a.hue[0] = videoMixerShiftedHue(0.f, 0.f);
	check(maxDiff(rig.run(a), halfTurn) > 10, "and not hue 0");
}

void testVideoMixerKey() {
	std::printf("Video Mixer key\n");
	VmRig rig;
	const Frame keyPic = vmScene(2);
	const Frame rect = vmRect();
	rig.set(1, vmSolid(40, 90, 160));
	rig.set(2, vmSolid(200, 160, 40));
	rig.set(6, keyPic);
	rig.set(7, rect);
	VideoMixerParams p = vmParams();
	p.fader = 0.5f;   // a dissolve half way: (120, 125, 100)
	p.keyBus = 6;
	p.matteBus = 7;
	const int inX = 128;
	const int inY = 72;
	const int outX = 20;
	const int outY = 20;

	Frame f = rig.run(p);
	vmCheckf(vmPx(f, outX, outY, 0) == 120 && vmPx(f, inX, inY, 0) == 120, "(sanity) KEY ON off: the mix, got %d %d", vmPx(f, outX, outY, 0), vmPx(f, inX, inY, 0));
	const Frame base = f;

	p.keyOn = true;
	f = rig.run(p);
	bool inside = true;
	bool outside = true;
	for (int yy = 40; yy < 104; ++yy) {
		for (int xx = 68; xx < 188; ++xx) {
			for (int c = 0; c < 3; ++c) {
				inside = inside && vmPx(f, xx, yy, c) == vmPx(keyPic, xx, yy, c);
			}
		}
	}
	for (int yy = 0; yy < kH; ++yy) {
		for (int xx = 0; xx < kW; ++xx) {
			if (xx < 56 || xx >= 200 || yy < 28 || yy >= 116) {
				for (int c = 0; c < 3; ++c) {
					outside = outside && vmPx(f, xx, yy, c) == vmPx(base, xx, yy, c);
				}
			}
		}
	}
	check(inside, "KEY with a MATTE: inside the matte the KEY picture replaces the mix");
	check(outside, "KEY with a MATTE: outside the matte the mix is untouched");
	check(vmOpaque(f), "the keyed picture is opaque");

	p.keyInvert = true;
	f = rig.run(p);
	check(vmPx(f, inX, inY, 0) == 120 && vmPx(f, outX, outY, 0) == vmPx(keyPic, outX, outY, 0) && vmPx(f, outX, outY, 1) == vmPx(keyPic, outX, outY, 1), "INV swaps inside and outside");
	p.keyInvert = false;

	p.keyOn = false;
	check(maxDiff(rig.run(p), base) == 0, "with KEY ON off, KEY and MATTE do nothing");
	p.keyOn = true;

	// Flat fill: the colour at KEY HUE where the matte is, no KEY picture needed.
	p.keyFill = 1;
	p.keyBus = 0;
	p.keyHue = 0.6f;
	f = rig.run(p);
	double flat[3];
	vmFlat(p, flat);
	vmCheckf(std::abs(vmPx(f, inX, inY, 0) - 255.0 * flat[0]) <= 1 && std::abs(vmPx(f, inX, inY, 1) - 255.0 * flat[1]) <= 1 && std::abs(vmPx(f, inX, inY, 2) - 255.0 * flat[2]) <= 1,
			 "FILL on flat paints KEY HUE inside the matte, got %d %d %d", vmPx(f, inX, inY, 0), vmPx(f, inX, inY, 1), vmPx(f, inX, inY, 2));
	check(vmPx(f, outX, outY, 0) == 120, "and leaves the outside alone");
	p.keyHue = 0.f;
	const Frame red = rig.run(p);
	p.keyHue = 0.33f;
	const Frame green = rig.run(p);
	check(vmPx(red, inX, inY, 0) > 240 && vmPx(red, inX, inY, 1) < 60 && vmPx(green, inX, inY, 1) > 240 && vmPx(green, inX, inY, 0) < 120, "KEY HUE chooses the colour");

	// A picture fill with no KEY picture, or any key with no matte at all, does nothing.
	p.keyFill = 0;
	p.keyBus = 0;
	check(maxDiff(rig.run(p), base) == 0, "a picture fill with nothing in KEY does nothing");
	p.keyFill = 1;
	p.matteBus = 0;
	check(maxDiff(rig.run(p), base) == 0, "a flat fill with nothing in KEY or MATTE does nothing");

	// Without a MATTE input the KEY picture's own greys are the matte.
	rig.set(6, vmRamp());
	p.keyFill = 0;
	p.keyBus = 6;
	p.matteBus = 0;
	p.keyThresh = 0.5f;
	p.keySoft = 0.12f;
	f = rig.run(p);
	vmCheckf(vmPx(f, 240, 70, 0) == 240 && vmPx(f, 240, 70, 2) == 240 && vmPx(f, 20, 70, 0) == 120, "with no MATTE the KEY picture's bright parts are kept and its dark parts are cut away, got %d %d", vmPx(f, 240, 70, 0), vmPx(f, 20, 70, 0));

	// THRESH sets how much of a ramp is kept, SOFT how wide the edge is.
	p.keyFill = 1;
	p.keyBus = 0;
	p.matteBus = 6;
	p.keySoft = 0.001f;
	bool thresholds = true;
	for (float t : {0.25f, 0.5f, 0.75f}) {
		p.keyThresh = t;
		f = rig.run(p);
		int kept = 0;
		for (int xx = 0; xx < kW; ++xx) {
			if (vmPx(f, xx, 70, 0) != 120) {
				++kept;
			}
		}
		const bool ok = std::abs(static_cast<double>(kept) / kW - (1.0 - t)) <= 0.02;
		vmCheckf(ok, "THRESH %.2f keeps the brighter %.0f%% of a ramp, kept %.1f%%", t, (1.0 - t) * 100.0, 100.0 * kept / kW);
		thresholds = thresholds && ok;
	}
	check(thresholds, "THRESH works as a level");
	p.keyThresh = 0.5f;
	p.keySoft = 0.25f;
	f = rig.run(p);
	int soft = 0;
	for (int xx = 0; xx < kW; ++xx) {
		const int g = vmPx(f, xx, 70, 1);
		if (g != 125 && g != vmPx(f, 255, 70, 1)) {
			++soft;
		}
	}
	vmCheckf(std::abs(soft - 64) <= 4, "key SOFT 0.25 makes an edge about 64 px wide, got %d", soft);

	// Order: the key goes over the transition, and BLACK fades the whole of it, key included.
	p.keyThresh = 0.5f;
	p.keySoft = 0.12f;
	p.matteBus = 7;
	p.black = 0.5f;
	p.keyHue = 0.6f;
	f = rig.run(p);
	vmFlat(p, flat);
	vmCheckf(std::abs(vmPx(f, inX, inY, 0) - 127.5 * flat[0]) <= 1 && std::abs(vmPx(f, inX, inY, 2) - 127.5 * flat[2]) <= 1 && std::abs(vmPx(f, outX, outY, 0) - 60) <= 1,
			 "BLACK fades the keyed layer along with the mix, got %d %d / %d", vmPx(f, inX, inY, 0), vmPx(f, inX, inY, 2), vmPx(f, outX, outY, 0));
	p.black = 1.f;
	check(vmAll(rig.run(p), 0, 0, 0), "BLACK at full is black, key and all");
	p.black = 0.f;

	// The key sits over a transition in progress, with nothing of the transition showing through inside the matte.
	p.keyFill = 0;
	p.keyBus = 6;
	rig.set(6, keyPic);
	p.fader = 0.3f;
	const Frame early = rig.run(p);
	p.fader = 0.8f;
	const Frame late = rig.run(p);
	bool same = true;
	for (int yy = 40; yy < 104; ++yy) {
		for (int xx = 68; xx < 188; ++xx) {
			same = same && vmPx(early, xx, yy, 1) == vmPx(late, xx, yy, 1);
		}
	}
	check(same && vmPx(early, outX, outY, 0) != vmPx(late, outX, outY, 0), "the keyed area stays put while the transition runs outside it");
}

void testVideoMixerGraph() {
	std::printf("Video Mixer in the graph\n");
	{
		// The node and the bus: what goes in comes out.
		VideoMixerParams p = vmParams();
		p.onAir = 2;
		p.next = 3;
		p.patternBus = 5;
		p.keyBus = 6;
		p.matteBus = 7;
		p.fader = 0.625f;
		p.mode = 2;
		p.soft = 0.5f;
		p.black = 0.25f;
		p.colorOn[1] = true;
		p.hue[1] = 0.125f;
		p.sat = 0.75f;
		p.keyOn = true;
		p.keyThresh = 0.375f;
		p.keySoft = 0.0625f;
		p.keyHue = 0.875f;
		p.keyFill = 1;
		p.keyInvert = true;
		videoBusPublishVideoMixer(11, p);
		VideoBusNode node;
		const bool read = videoBusReadNode(11, node);
		const VideoMixerParams& q = node.videoMixer;
		check(read && node.type == VideoBusNodeType::VideoMixer, "a published mixer reads back as a Video Mixer node");
		check(q.inputBus[0] == 1 && q.inputBus[3] == 4 && q.onAir == 2 && q.next == 3 && q.patternBus == 5 && q.keyBus == 6 && q.matteBus == 7, "its inputs, selection and side buses survive the bus");
		check(q.fader == 0.625f && q.mode == 2 && q.soft == 0.5f && q.black == 0.25f && q.sat == 0.75f && q.colorOn[1] && !q.colorOn[0] && q.hue[1] == 0.125f, "its transition and colour settings survive the bus");
		check(q.keyOn && q.keyThresh == 0.375f && q.keySoft == 0.0625f && q.keyHue == 0.875f && q.keyFill == 1 && q.keyInvert, "its key settings survive the bus");
		videoBusClear(11);
		check(!videoBusReadNode(11, node), "a cleared mixer is gone from the bus");
	}
	{
		// Roles a frame does not read are not rendered, so they cannot make a cycle; roles it does read can.
		VmRig rig;
		rig.set(1, vmCard(0));
		rig.set(2, vmCard(1));
		VideoMixerParams p = vmParams();
		p.fader = 0.5f;
		const Frame good = rig.run(p);
		check(!rig.r.hasCycle() && !good.empty(), "(sanity) a plain mixer renders clean");

		VideoMixerParams loop = p;
		loop.inputBus[0] = VmRig::kOut;
		const Frame looped = rig.run(loop);
		check(rig.r.hasCycle(), "a mixer that reads its own output is reported as a cycle");
		check(maxDiff(looped, good) == 0, "and shows the last good frame");
		check(!rig.r.hasCycle() || maxDiff(rig.run(p), good) == 0, "(sanity) fixing the patch brings it back");
		check(!rig.r.hasCycle(), "after fixing it the cycle report clears");

		VideoMixerParams key = p;
		key.keyBus = VmRig::kOut;
		key.matteBus = VmRig::kOut;
		rig.run(key);
		check(!rig.r.hasCycle(), "KEY and MATTE are not read while KEY ON is off");
		key.keyOn = true;
		rig.run(key);
		check(rig.r.hasCycle(), "they are read once KEY ON is on");

		VideoMixerParams pat = p;
		pat.patternBus = VmRig::kOut;
		rig.run(pat);
		check(!rig.r.hasCycle(), "PATTERN is not read in a dissolve");
		pat.mode = 2;
		rig.run(pat);
		check(rig.r.hasCycle(), "PATTERN is read mid-wipe");
		pat.fader = 0.f;
		rig.run(pat);
		check(!rig.r.hasCycle(), "PATTERN is not read with the fader at the old picture");
		pat.fader = 1.f;
		rig.run(pat);
		check(!rig.r.hasCycle(), "PATTERN is not read with the fader at the new picture");

		VideoMixerParams ends = p;
		ends.inputBus[0] = VmRig::kOut;
		ends.fader = 1.f;
		rig.run(ends);
		check(!rig.r.hasCycle(), "the live picture is not read once the new one has fully arrived");
		ends = p;
		ends.inputBus[1] = VmRig::kOut;
		ends.fader = 0.f;
		rig.run(ends);
		check(!rig.r.hasCycle(), "the next picture is not read while the fader is at the old one");
		ends.fader = 0.01f;
		rig.run(ends);
		check(rig.r.hasCycle(), "but it is the moment the fader leaves");
	}
	{
		// A mixer into a mixer.
		VmRig rig;
		const Frame c0 = vmCard(0);
		const Frame c1 = vmCard(1);
		const Frame c2 = vmCard(2);
		rig.set(1, c0);
		rig.set(2, c1);
		rig.set(3, c2);
		VideoMixerParams first = vmParams();
		first.fader = 0.5f;
		videoBusPublishVideoMixer(10, first);
		VideoMixerParams second = vmParams();
		second.inputBus[0] = 10;
		second.inputBus[1] = 3;
		second.fader = 0.25f;
		const Frame nested = rig.run(second);
		const Frame inner = vmReference(&c0, &c1, nullptr, nullptr, nullptr, first);
		const Frame expect = vmReference(&inner, &c2, nullptr, nullptr, nullptr, second);
		vmCheckf(maxDiff(nested, expect) <= 1, "a mixer's output works as another mixer's input (max difference %d)", maxDiff(nested, expect));
		check(!rig.r.hasCycle(), "and is not a cycle");
		videoBusClear(10);
	}
	{
		// One picture in two roles is the same as two copies of it.
		VmRig rig;
		rig.set(1, vmScene(0));
		rig.set(2, vmScene(1));
		rig.set(5, vmScene(1));
		VideoMixerParams p = vmParams();
		p.mode = 2;
		p.fader = 0.4f;
		p.soft = 0.3f;
		p.patternBus = 2;
		const Frame shared = rig.run(p);
		p.patternBus = 5;
		const Frame copies = rig.run(p);
		p.patternBus = 0;
		const Frame bare = rig.run(p);
		check(maxDiff(shared, copies) == 0, "the next picture as its own PATTERN gives what a copy of it does");
		check(maxDiff(shared, bare) == 0, "and what leaving PATTERN empty does");
		rig.set(6, vmScene(2));
		rig.set(7, vmScene(2));
		p.keyOn = true;
		p.keyFill = 0;
		p.keyBus = 6;
		p.matteBus = 6;
		p.keyThresh = 0.4f;
		const Frame together = rig.run(p);
		p.matteBus = 7;
		const Frame apart = rig.run(p);
		p.matteBus = 0;
		const Frame fromKey = rig.run(p);
		check(maxDiff(together, apart) == 0, "one picture as KEY and MATTE gives what two copies do");
		check(maxDiff(together, fromKey) == 0, "and what leaving MATTE empty does");
		p.inputBus[0] = 2;
		p.onAir = 0;
		p.next = 1;
		p.fader = 0.5f;
		p.mode = 0;
		p.keyOn = false;
		check(maxDiff(rig.run(p), vmReference(rig.of(2), rig.of(2), nullptr, nullptr, nullptr, p)) <= 1, "the same bus live and next renders once and mixes with itself");
	}
	{
		// Real nodes on the bus: oscillator bars, a hue-turned copy and a grey ramp pattern, all drawn by the renderer.
		VideoGraphRenderer r;
		publishOsc(1, 0.3f, 1);
		publishColor(2, 1, true, 0.3f);
		publishOsc(3, 0.3f, 4);
		const Frame bars = render(r, 1);
		const Frame turned = render(r, 2);
		const Frame ramp = render(r, 3);
		VideoMixerParams p = vmParams();
		p.inputBus[2] = 3;
		p.mode = 2;
		p.fader = 0.45f;
		p.patternBus = 3;
		p.soft = 0.2f;
		videoBusPublishVideoMixer(9, p);
		const Frame mixed = render(r, 9);
		const Frame expect = vmReference(&bars, &turned, &ramp, nullptr, nullptr, p);
		vmCheckf(maxDiff(mixed, expect) <= 1, "a mixer over oscillator and colour nodes mixes what those nodes draw (max difference %d)", maxDiff(mixed, expect));
		check(!r.hasCycle() && meanDiff(mixed, bars) > 5.0 && meanDiff(mixed, turned) > 5.0, "and is neither picture alone");
		videoBusClear(9);
		for (int bus = 1; bus <= 3; ++bus) {
			videoBusClear(bus);
		}
	}
	{
		// Another size: the pictures are resampled to it and the mixer draws at it.
		VmRig rig;
		rig.set(1, vmScene(0));
		rig.set(2, vmScene(1));
		VideoMixerParams p = vmParams();
		p.fader = 0.5f;
		videoBusPublishVideoMixer(VmRig::kOut, p);
		const Frame small = rig.r.renderBus(VmRig::kOut, 128, 72);
		bool opaque = small.size() == 128u * 72u * 4u;
		double sum = 0.0;
		for (size_t i = 3; i < small.size(); i += 4) {
			opaque = opaque && small[i] == 255;
			sum += small[i - 3] + small[i - 2] + small[i - 1];
		}
		check(opaque && sum > 1000.0, "the mixer renders at another size");
	}
}

struct VmRandom {
	uint32_t state;
	explicit VmRandom(uint32_t seed) : state(seed) {}
	float next() {
		state = state * 1664525u + 1013904223u;
		return static_cast<float>((state >> 8) & 0xFFFFFFu) / 16777216.f;
	}
	int below(int n) { return std::min(n - 1, static_cast<int>(next() * static_cast<float>(n))); }
	bool coin(float chance = 0.5f) { return next() < chance; }
};

std::string vmDescribe(const VideoMixerParams& p) {
	char text[400];
	std::snprintf(text, sizeof text,
				  "in %d/%d/%d/%d live %d next %d pat %d key %d matte %d fader %.3f mode %d soft %.3f black %.2f color %d%d%d%d hue %.2f/%.2f/%.2f/%.2f sat %.2f keyOn %d thresh %.2f ksoft %.3f khue %.2f fill %d inv %d",
				  p.inputBus[0], p.inputBus[1], p.inputBus[2], p.inputBus[3], p.onAir, p.next, p.patternBus, p.keyBus, p.matteBus, static_cast<double>(p.fader), p.mode,
				  static_cast<double>(p.soft), static_cast<double>(p.black), p.colorOn[0], p.colorOn[1], p.colorOn[2], p.colorOn[3], static_cast<double>(p.hue[0]), static_cast<double>(p.hue[1]),
				  static_cast<double>(p.hue[2]), static_cast<double>(p.hue[3]), static_cast<double>(p.sat), p.keyOn, static_cast<double>(p.keyThresh), static_cast<double>(p.keySoft),
				  static_cast<double>(p.keyHue), p.keyFill, p.keyInvert);
	return text;
}

// Many random settings against the independent reading of the maths. The pictures are coloured and busy, so a
// channel mixed up, a role swapped or a stage out of order shows at once.
void testVideoMixerReference() {
	std::printf("Video Mixer against the reference maths\n");
	VmRig rig;
	rig.set(1, vmCard(0));
	rig.set(2, vmCard(1));
	rig.set(3, vmScene(0));
	rig.set(4, vmScene(1));
	rig.set(5, vmRings());
	rig.set(6, vmScene(2));
	rig.set(7, vmBlob());
	VmRandom rnd(20261111u);
	int worstDiff = 0;
	int failures = 0;
	const int cases = 600;
	int modeCount[3] = {0, 0, 0};
	int keyed = 0;
	int colored = 0;
	for (int n = 0; n < cases; ++n) {
		VideoMixerParams p;
		for (int i = 0; i < 4; ++i) {
			p.inputBus[i] = rnd.coin(0.9f) ? i + 1 : (rnd.coin(0.3f) ? 8 : 0);
		}
		p.onAir = rnd.below(4);
		p.next = rnd.coin(0.1f) ? p.onAir : rnd.below(4);
		p.mode = rnd.below(3);
		const int faderKind = rnd.below(8);
		p.fader = faderKind == 0 ? 0.f : (faderKind == 1 ? 1.f : rnd.next());
		p.soft = 0.001f + rnd.next() * rnd.next();
		p.black = rnd.coin(0.6f) ? 0.f : rnd.next();
		for (int i = 0; i < 4; ++i) {
			p.colorOn[i] = rnd.coin(0.4f);
			p.hue[i] = rnd.next() * 2.f - 0.5f;
		}
		p.sat = rnd.coin(0.2f) ? 1.f : rnd.next();
		p.patternBus = rnd.coin(0.35f) ? 0 : (rnd.coin(0.7f) ? 5 : p.inputBus[p.next]);
		p.keyOn = rnd.coin(0.5f);
		p.keyBus = rnd.coin(0.25f) ? 0 : (rnd.coin(0.8f) ? 6 : 1 + rnd.below(4));
		p.matteBus = rnd.coin(0.4f) ? 0 : (rnd.coin(0.2f) ? p.keyBus : 7);
		p.keyThresh = 0.1f + 0.8f * rnd.next();
		p.keySoft = 0.001f + 0.5f * rnd.next() * rnd.next();
		p.keyHue = rnd.next();
		p.keyFill = rnd.coin(0.4f) ? 1 : 0;
		p.keyInvert = rnd.coin(0.3f);
		const Frame out = rig.run(p);
		const Frame expect = rig.expected(p);
		const int diff = maxDiff(out, expect);
		worstDiff = std::max(worstDiff, diff);
		++modeCount[p.mode];
		keyed += p.keyOn ? 1 : 0;
		colored += (p.colorOn[p.onAir] || p.colorOn[p.next]) ? 1 : 0;
		if (diff > 1 || !vmOpaque(out)) {
			if (failures < 6) {
				std::printf("  case %d differs by %d: %s\n", n, diff, vmDescribe(p).c_str());
			}
			++failures;
		}
	}
	std::printf("  %d random settings (%d dissolve, %d add, %d wipe; %d keyed, %d coloured), worst difference %d level(s)\n", cases, modeCount[0], modeCount[1], modeCount[2], keyed, colored, worstDiff);
	vmCheckf(failures == 0, "every random setting matches the reference to within one level (%d did not)", failures);
	check(modeCount[0] > 100 && modeCount[1] > 100 && modeCount[2] > 100 && keyed > 150 && colored > 150, "the random settings cover every mode, the key and colour");
}

void testVideoMixerProofs() {
	std::printf("Video Mixer proofs\n");
	VmRig rig;
	const Frame sceneA = vmScene(0);
	const Frame sceneB = vmScene(1);
	rig.set(1, sceneA);
	rig.set(2, sceneB);
	VideoMixerParams p = vmParams();

	// Dissolve, add and a wipe with nothing in PATTERN (the new picture's own greys, brightest first).
	std::vector<std::vector<Frame>> rows;
	for (int mode = 0; mode < 3; ++mode) {
		std::vector<Frame> row;
		row.push_back(sceneA);
		for (float f : {0.25f, 0.5f, 0.75f}) {
			p.mode = mode;
			p.fader = f;
			row.push_back(rig.run(p));
		}
		row.push_back(sceneB);
		rows.push_back(row);
	}
	vmSaveGrid("video_mixer_transitions.png", rows);

	// Wipes with a PATTERN: a ramp, a diagonal, a blob and a soft edge. The first panel is the pattern.
	rows.clear();
	const Frame patterns[4] = {vmRamp(), vmDiagonal(), vmBlob(), vmBlob()};
	const float softs[4] = {0.15f, 0.15f, 0.15f, 0.6f};
	for (int k = 0; k < 4; ++k) {
		rig.set(5, patterns[k]);
		p.mode = 2;
		p.patternBus = 5;
		p.soft = softs[k];
		std::vector<Frame> row;
		row.push_back(patterns[k]);
		for (float f : {0.2f, 0.4f, 0.6f, 0.8f}) {
			p.fader = f;
			row.push_back(rig.run(p));
		}
		rows.push_back(row);
	}
	vmSaveGrid("video_mixer_wipe_patterns.png", rows);
	p.patternBus = 0;
	p.soft = 0.15f;

	// A pattern drawn by another module on the bus: the oscillator's grid, shown first, then used as the wipe's edge.
	{
		rows.clear();
		publishOsc(5, 0.3f, 5);
		const Frame grid = render(rig.r, 5);
		std::vector<Frame> row;
		row.push_back(grid);
		for (float f : {0.2f, 0.4f, 0.6f, 0.8f}) {
			p.fader = f;
			p.patternBus = 5;
			row.push_back(rig.run(p));
		}
		rows.push_back(row);
		vmSaveGrid("video_mixer_wipe_oscillator.png", rows);
		videoBusClear(5);
		p.patternBus = 0;
	}

	// Colour: one grey picture through each input's hue, then SAT, then colour in a dissolve, an add and a wipe.
	rows.clear();
	const Frame grey = vmRings();
	for (int i = 0; i < 4; ++i) {
		rig.set(i + 1, grey);
	}
	{
		std::vector<Frame> row;
		row.push_back(grey);
		for (int i = 0; i < 4; ++i) {
			VideoMixerParams c = vmParams();
			c.onAir = i;
			c.next = (i + 1) % 4;
			c.fader = 0.f;
			for (int k = 0; k < 4; ++k) {
				c.colorOn[k] = true;
			}
			row.push_back(rig.run(c));
		}
		rows.push_back(row);
	}
	{
		std::vector<Frame> row;
		for (float sat : {1.f, 0.75f, 0.5f, 0.25f, 0.f}) {
			VideoMixerParams c = vmParams();
			c.fader = 0.f;
			c.colorOn[0] = true;
			c.hue[0] = 0.62f;
			c.sat = sat;
			row.push_back(rig.run(c));
		}
		rows.push_back(row);
	}
	rig.set(1, vmRings());
	rig.set(2, vmDiagonal());
	{
		std::vector<Frame> row;
		VideoMixerParams c = vmParams();
		c.colorOn[0] = c.colorOn[1] = true;
		c.hue[0] = 0.02f;
		c.hue[1] = 0.55f;
		c.sat = 1.f;
		c.fader = 0.f;
		row.push_back(rig.run(c));
		for (int mode = 0; mode < 3; ++mode) {
			c.mode = mode;
			c.fader = 0.5f;
			row.push_back(rig.run(c));
		}
		c.fader = 1.f;
		row.push_back(rig.run(c));
		rows.push_back(row);
	}
	vmSaveGrid("video_mixer_color.png", rows);

	// The key over a half-way dissolve: with a MATTE, without one, flat fills, and BLACK last.
	rig.set(1, sceneA);
	rig.set(2, sceneB);
	const Frame keyPic = vmScene(3);
	const Frame disc = vmDisc();
	const Frame blob = vmBlob();
	rig.set(6, keyPic);
	rig.set(7, disc);
	rows.clear();
	VideoMixerParams k = vmParams();
	k.fader = 0.5f;
	k.keyBus = 6;
	k.matteBus = 7;
	k.keyThresh = 0.5f;
	k.keySoft = 0.12f;
	{
		std::vector<Frame> row;
		row.push_back(rig.run(k));
		row.push_back(keyPic);
		row.push_back(disc);
		k.keyOn = true;
		row.push_back(rig.run(k));
		k.keyInvert = true;
		row.push_back(rig.run(k));
		k.keyInvert = false;
		rows.push_back(row);
	}
	{
		// No MATTE: the KEY picture cuts itself out by its own greys, at three thresholds, and with a wide soft edge.
		std::vector<Frame> row;
		k.matteBus = 0;
		row.push_back(rig.run(k));
		k.keyThresh = 0.3f;
		row.push_back(rig.run(k));
		k.keyThresh = 0.7f;
		row.push_back(rig.run(k));
		k.keyThresh = 0.5f;
		k.keySoft = 0.6f;
		row.push_back(rig.run(k));
		rig.set(7, blob);
		k.matteBus = 7;
		k.keyThresh = 0.4f;
		k.keySoft = 0.3f;
		row.push_back(rig.run(k));
		rows.push_back(row);
	}
	{
		// A flat fill at four KEY HUEs, then one with INV.
		std::vector<Frame> row;
		k.keyFill = 1;
		k.keySoft = 0.12f;
		k.keyThresh = 0.5f;
		rig.set(7, disc);
		for (float hue : {0.f, 0.15f, 0.55f, 0.8f}) {
			k.keyHue = hue;
			row.push_back(rig.run(k));
		}
		k.keyInvert = true;
		row.push_back(rig.run(k));
		k.keyInvert = false;
		rows.push_back(row);
	}
	{
		// BLACK comes last: it fades the whole picture, key included.
		std::vector<Frame> row;
		k.keyFill = 0;
		for (float black : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
			k.black = black;
			row.push_back(rig.run(k));
		}
		rows.push_back(row);
	}
	vmSaveGrid("video_mixer_key.png", rows);
}

// ---- Rotator (VRU-20) ----

// A card that is different everywhere: R rises with x, G with y, B is a 16 px checker, and a white
// block marks the top left corner so every turn and mirror can be seen. R and G start at 40, so Hold
// and Repeat show up against Black.
Frame rotatorCard() {
	Frame f(static_cast<size_t>(kW) * kH * 4u);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			float r = 40.f + x * 200.f / (kW - 1), g = 40.f + y * 150.f / (kH - 1);
			float b = (((x / 16) + (y / 16)) & 1) ? 200.f : 20.f;
			if (x < 48 && y < 36) r = g = b = 255.f;
			f[i] = static_cast<unsigned char>(std::lround(r));
			f[i + 1] = static_cast<unsigned char>(std::lround(g));
			f[i + 2] = static_cast<unsigned char>(std::lround(b));
			f[i + 3] = 255;
		}
	}
	return f;
}

// Publishes the card on bus 1 and a Rotator over it on bus 2.
struct RotatorRig {
	int frameId = videoExternalFrameAllocate();
	Frame card = rotatorCard();
	VideoGraphRenderer r;
	RotatorRig() {
		videoExternalFramePublish(frameId, card.data(), kW, kH, 0.0);
		videoBusPublishExternalFrame(1, frameId);
	}
	~RotatorRig() {
		videoBusClear(2);
		videoBusClear(1);
		videoExternalFrameRelease(frameId);
	}
	Frame run(const RotatorParams& p) {
		videoBusPublishRotator(2, 1, p);
		return render(r, 2);
	}
	static int px(const Frame& f, int x, int y, int ch) { return f[(static_cast<size_t>(y) * kW + x) * 4u + ch]; }
};

void testRotatorIdentityAndTurns() {
	std::printf("Rotator identity and exact turns\n");
	RotatorRig rig;
	check(maxDiff(rig.card, render(rig.r, 1)) == 0, "rotator rig source is the card");
	RotatorParams p;
	check(maxDiff(rig.run(p), rig.card) == 0, "rotator defaults pass the picture through byte for byte");
	p.pivotX = 0.3f; p.pivotY = -0.2f; p.edge = 3;
	check(maxDiff(rig.run(p), rig.card) == 0, "pivot and edge change nothing while the picture is not moved");
	p = RotatorParams{};
	p.angle = 0.5f;
	const Frame half = rig.run(p);
	int bad = 0;
	for (int y = 0; y < kH; ++y)
		for (int x = 0; x < kW; ++x)
			for (int ch = 0; ch < 3; ++ch)
				if (rig.px(half, x, y, ch) != rig.px(rig.card, kW - 1 - x, kH - 1 - y, ch)) ++bad;
	check(bad == 0, "180 degrees is an exact point reflection about the centre");
	// A quarter turn counter-clockwise: out(x, y) = card(199 - y, x - 56) where that lies inside the card.
	p.angle = 0.25f;
	const Frame quarter = rig.run(p);
	bad = 0;
	int inside = 0, outsideBlack = 0, outside = 0;
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const int sx = 199 - y, sy = x - 56;
			if (sx >= 0 && sx < kW && sy >= 0 && sy < kH) {
				++inside;
				for (int ch = 0; ch < 3; ++ch)
					if (rig.px(quarter, x, y, ch) != rig.px(rig.card, sx, sy, ch)) ++bad;
			}
			else {
				++outside;
				if (rig.px(quarter, x, y, 0) + rig.px(quarter, x, y, 1) + rig.px(quarter, x, y, 2) == 0) ++outsideBlack;
			}
		}
	}
	check(inside == 144 * 144 && bad == 0, "90 degrees is an exact quarter turn (counter-clockwise)");
	check(outside > 0 && outsideBlack == outside, "the corners a quarter turn uncovers are black");
	// -90 degrees turns the other way: out(x, y) = card(y + 56, 199 - x).
	p.angle = -0.25f;
	const Frame back = rig.run(p);
	bad = 0;
	for (int y = 0; y < kH; ++y)
		for (int x = 0; x < kW; ++x) {
			const int sx = y + 56, sy = 199 - x;
			if (sx < kW && sy >= 0 && sy < kH)
				for (int ch = 0; ch < 3; ++ch)
					if (rig.px(back, x, y, ch) != rig.px(rig.card, sx, sy, ch)) ++bad;
		}
	check(bad == 0, "-90 degrees turns clockwise");
	p.angle = 1.f;
	check(maxDiff(rig.run(p), rig.card) == 0, "a whole turn is the identity");
	// A half turn about PIVOT X = 0.25 turns about x = 191.5: pixel x takes the source at 383 - x.
	p = RotatorParams{};
	p.angle = 0.5f;
	p.pivotX = 0.25f;
	const Frame pivoted = rig.run(p);
	check(std::abs(rig.px(pivoted, 200, 72, 0) - rig.px(rig.card, 183, 71, 0)) <= 1
		&& std::abs(rig.px(pivoted, 200, 72, 1) - rig.px(rig.card, 183, 71, 1)) <= 1, "a half turn about PIVOT X reflects through that point");
	RotatorParams q;
	q.angle = 30.f / 360.f;
	const Frame thirty = rig.run(q);
	check(maxDiff(thirty, rig.card) > 30, "30 degrees changes the picture");
	q.edge = 3;
	const Frame thirtyMirror = rig.run(q);
	RotatorParams z;
	z.angle = -20.f / 360.f; z.zoom = 0.6f; z.offsetX = 0.12f; z.edge = 2;
	const Frame stamp = rig.run(z);
	saveProof("rotator_turns.png", {rig.card, thirty, thirtyMirror, stamp});
}

void testRotatorZoomXY() {
	std::printf("Rotator zoom, X and Y\n");
	RotatorRig rig;
	RotatorParams p;
	p.zoom = 2.f;
	const Frame big = rig.run(p);
	bool ok = true;
	for (int x = 30; x < kW - 30; x += 7) {
		const float sx = (x - 127.5f) / 2.f + 127.5f;
		const float want = 40.f + sx * 200.f / (kW - 1);
		if (std::abs(rig.px(big, x, 100, 0) - want) > 2.f) ok = false;
	}
	check(ok, "zoom 2 magnifies about the centre");
	p.zoom = 0.5f;
	const Frame small = rig.run(p);
	check(rig.px(small, 5, 5, 0) + rig.px(small, 5, 5, 1) + rig.px(small, 5, 5, 2) == 0, "zoom 0.5 leaves a black border");
	check(std::abs(rig.px(small, 128, 72, 1) - (40.f + 72.5f * 150.f / 143.f)) <= 2.f, "zoom 0.5 shrinks the picture about the centre");
	// X slides the picture right by a fraction of the width, Y down by a fraction of the height.
	p = RotatorParams{};
	p.offsetX = 0.25f; p.offsetY = 0.25f;
	const Frame moved = rig.run(p);
	check(rig.px(moved, 100, 100, 0) == rig.px(rig.card, 100 - 64, 100 - 36, 0) && rig.px(moved, 100, 100, 1) == rig.px(rig.card, 100 - 64, 100 - 36, 1),
		"X and Y slide the picture right and down");
	check(rig.px(moved, 10, 10, 0) == 0, "the strip the slide uncovers is black");
}

void testRotatorEdges() {
	std::printf("Rotator edge modes\n");
	RotatorRig rig;
	RotatorParams p;
	p.zoom = 0.5f;
	// At x = 5 the source position is 2 * (5 - 127.5) + 127.5 = -117.5; at y = 72 it is 72.5.
	const float wantG = 40.f + 72.5f * 150.f / 143.f;
	p.edge = 0;
	const Frame black = rig.run(p);
	check(rig.px(black, 5, 72, 0) == 0 && rig.px(black, 5, 72, 1) == 0, "edge black shows black past the border");
	check(rig.px(black, 5, 72, 3) == 255, "edge black keeps the output opaque");
	p.edge = 1;
	const Frame hold = rig.run(p);
	check(std::abs(rig.px(hold, 5, 72, 0) - 40) <= 1 && std::abs(rig.px(hold, 5, 72, 1) - wantG) <= 2.f, "edge hold repeats the border pixel");
	p.edge = 2;
	const Frame repeat = rig.run(p);
	check(std::abs(rig.px(repeat, 5, 72, 0) - (40.f + 138.5f * 200.f / 255.f)) <= 2.f && std::abs(rig.px(repeat, 5, 72, 1) - wantG) <= 2.f,
		"edge repeat tiles the picture");
	p.edge = 3;
	const Frame mirror = rig.run(p);
	check(std::abs(rig.px(mirror, 5, 72, 0) - (40.f + 117.5f * 200.f / 255.f)) <= 2.f && std::abs(rig.px(mirror, 5, 72, 1) - wantG) <= 2.f,
		"edge mirror reflects the picture");
	// Mirror is continuous across the border, repeat is not.
	check(std::abs(rig.px(mirror, 63, 72, 0) - rig.px(mirror, 64, 72, 0)) <= 4, "mirror has no seam at the border");
	check(std::abs(rig.px(repeat, 63, 72, 0) - rig.px(repeat, 64, 72, 0)) > 60, "repeat has a seam at the border");
	saveProof("rotator_edges.png", {black, hold, repeat, mirror});
}

void testRotatorFolds() {
	std::printf("Rotator folds\n");
	RotatorRig rig;
	RotatorParams p;
	p.folds = 1;
	check(maxDiff(rig.run(p), rig.card) == 0, "folds 1 is off");
	// Two wedges: the picture is symmetric left to right and top to bottom about the pivot.
	p.folds = 2;
	Frame f = rig.run(p);
	int bad = 0, n = 0;
	for (int y = 4; y < kH - 4; ++y)
		for (int x = 4; x < kW - 4; ++x) {
			++n;
			if (std::abs(rig.px(f, x, y, 0) - rig.px(f, kW - 1 - x, y, 0)) > 4 || std::abs(rig.px(f, x, y, 1) - rig.px(f, x, kH - 1 - y, 1)) > 4) ++bad;
		}
	check(bad < n / 50, "two folds mirror the picture about both axes through the pivot");
	// Four wedges repeat every quarter turn: out(x, y) = out(199 - y, x - 56).
	p.folds = 4;
	f = rig.run(p);
	bad = 0; n = 0;
	for (int y = 20; y < kH - 20; ++y)
		for (int x = 76; x < 180; ++x) {
			const int sx = 199 - y, sy = x - 56;
			++n;
			if (std::abs(rig.px(f, x, y, 0) - rig.px(f, sx, sy, 0)) > 6 || std::abs(rig.px(f, x, y, 1) - rig.px(f, sx, sy, 1)) > 6) ++bad;
		}
	check(n > 5000 && bad < n / 20, "four folds repeat every quarter turn");
	// Six wedges: turning by 60 degrees about the centre gives the same picture.
	p.folds = 6;
	f = rig.run(p);
	bad = 0; n = 0;
	const float c60 = std::cos(3.14159265f / 3.f), s60 = std::sin(3.14159265f / 3.f);
	for (int y = 10; y < kH - 10; y += 2)
		for (int x = 60; x < 196; x += 2) {
			const float dx = x - 127.5f, dy = y - 71.5f;
			const float rx = 127.5f + c60 * dx - s60 * dy, ry = 71.5f + s60 * dx + c60 * dy;
			const int ix = static_cast<int>(std::lround(rx)), iy = static_cast<int>(std::lround(ry));
			if (ix < 0 || ix >= kW || iy < 0 || iy >= kH) continue;
			++n;
			if (std::abs(rig.px(f, x, y, 0) - rig.px(f, ix, iy, 0)) > 8 || std::abs(rig.px(f, x, y, 1) - rig.px(f, ix, iy, 1)) > 8) ++bad;
		}
	check(n > 2000 && bad < n / 8, "six folds repeat every sixty degrees");
	const Frame six = f;
	// The angle turns the picture inside fixed wedges.
	RotatorParams q;
	q.folds = 6;
	q.angle = 0.1f;
	check(maxDiff(rig.run(q), six) > 20, "the angle turns the picture inside the wedges");
	q = RotatorParams{};
	q.folds = 12; q.angle = 0.03f; q.zoom = 1.4f; q.edge = 3;
	const Frame twelve = rig.run(q);
	saveProof("rotator_kaleidoscope.png", {rig.card, six, twelve});
}

void testRotatorNotchSpinReset() {
	std::printf("Rotator notch, spin and reset\n");
	RotatorState s;
	// Spin is accumulated per sample, so the rate is exact whatever the sample rate.
	for (int i = 0; i < 48000; ++i) s.advance(1.0 / 48000.0, 0.5f, 0.125f, 0.f, false, false);
	check(std::abs(s.spin - 0.5) < 1e-6, "SPIN 0.5 is half a turn per second");
	RotatorState s44;
	for (int i = 0; i < 44100; ++i) s44.advance(1.0 / 44100.0, 0.5f, 0.125f, 0.f, false, false);
	check(std::abs(s44.spin - s.spin) < 1e-6, "spin does not depend on the sample rate");
	RotatorState rev;
	for (int i = 0; i < 1000; ++i) rev.advance(0.001, -0.5f, 0.125f, 0.f, false, false);
	check(std::abs(rev.spin - 0.5) < 1e-6, "negative SPIN turns the other way (wrapped to 0.5)");
	RotatorState stopped;
	for (int i = 0; i < 1000; ++i) stopped.advance(0.001, 0.f, 0.125f, 0.f, false, false);
	check(stopped.spin == 0.0, "SPIN at the centre stops");
	// Notch with no glide: each trigger clicks one step.
	const float steps[] = {1.f / 16, 1.f / 12, 1.f / 8, 1.f / 6, 1.f / 4};
	for (int k = 0; k < 5; ++k) check(rotatorStepTurns(k) == steps[k], "STEP list is 1/16, 1/12, 1/8, 1/6, 1/4");
	RotatorState n;
	n.advance(0.001, 0.f, 0.125f, 0.f, true, false);
	check(std::abs(n.notchPos - 0.125) < 1e-9, "one trigger clicks round by one step");
	n.advance(0.001, 0.f, 0.125f, 0.f, false, false);
	check(std::abs(n.notchPos - 0.125) < 1e-9, "the picture rests between triggers");
	for (int i = 0; i < 3; ++i) n.advance(0.001, 0.f, 0.125f, 0.f, true, false);
	check(std::abs(n.notchPos - 0.5) < 1e-9, "four 1/8 clicks make half a turn");
	// Glide: constant speed, arriving after GLIDE seconds.
	RotatorState g;
	g.advance(0.001, 0.f, 0.25f, 0.5f, true, false);
	check(g.notchPos < 0.01, "with glide the picture has barely moved after one tick");
	for (int i = 0; i < 249; ++i) g.advance(0.001, 0.f, 0.25f, 0.5f, false, false);
	check(std::abs(g.notchPos - 0.125) < 0.003, "halfway through the glide it is halfway round");
	for (int i = 0; i < 400; ++i) g.advance(0.001, 0.f, 0.25f, 0.5f, false, false);
	check(std::abs(g.notchPos - 0.25) < 1e-9, "after the glide it has arrived exactly");
	// A fast clock queues steps and the glide chases the target.
	RotatorState chase;
	chase.advance(0.001, 0.f, 0.25f, 0.5f, true, false);
	chase.advance(0.001, 0.f, 0.25f, 0.5f, true, false);
	check(std::abs(chase.notchTarget - 0.5) < 1e-9 && chase.notchPos < 0.01, "a second trigger moves the target on");
	for (int i = 0; i < 1100; ++i) chase.advance(0.001, 0.f, 0.25f, 0.5f, false, false);
	check(std::abs(chase.notchPos - 0.5) < 1e-9, "the glide catches up with the target");
	// Reset.
	RotatorState rs;
	for (int i = 0; i < 100; ++i) rs.advance(0.001, 0.7f, 0.125f, 0.2f, i == 10, false);
	check(rs.spin != 0.0 && rs.notchTarget != 0.0, "spin and notches have built up");
	rs.advance(0.001, 0.7f, 0.125f, 0.2f, false, true);
	check(rs.spin == 0.0 && rs.notchPos == 0.0 && rs.notchTarget == 0.0, "RESET returns spin and notches to zero");
	// Long runs stay small.
	RotatorState longRun;
	for (int i = 0; i < 2000; ++i) longRun.advance(0.001, 0.f, 0.25f, 0.f, true, false);
	check(longRun.notchTarget < 70.0 && std::abs(rotatorWrapTurns(longRun.notchPos)) < 1e-6, "notch bookkeeping stays small and whole turns vanish");
	// And the picture clicks to the same place a manual angle would.
	RotatorRig rig;
	RotatorParams a, b;
	a.angle = rotatorAngle(0.f, 0.f, 0.0, 0.25);
	b.angle = 0.25f;
	check(maxDiff(rig.run(a), rig.run(b)) == 0, "a notch total of a quarter turn renders as 90 degrees");
}

void testRotatorSawSeam() {
	std::printf("Rotator ANGLE CV saw\n");
	check(rotatorAngle(0.f, 0.f, 0.0, 0.0) == 0.f && rotatorAngle(0.f, 10.f, 0.0, 0.0) == 0.f, "0 V and 10 V are the same angle");
	check(std::abs(rotatorAngle(0.f, 2.5f, 0.0, 0.0) - 0.25f) < 1e-6f, "2.5 V is a quarter turn");
	check(std::abs(rotatorAngle(0.f, -2.5f, 0.0, 0.0) + 0.25f) < 1e-6f, "-2.5 V is a quarter turn the other way");
	float worst = 0.f;
	float previous = rotatorAngle(0.f, 0.f, 0.0, 0.0);
	for (int i = 1; i <= 2000; ++i) {
		const float volts = 10.f * i / 2000.f;
		const float angle = rotatorAngle(0.f, volts, 0.0, 0.0);
		worst = std::max(worst, std::abs(rotatorWrapTurns(angle - previous) - 0.0005f));
		previous = angle;
	}
	check(worst < 1e-5f, "a 0 to 10 V saw advances by equal angles with no jump, including across the wrap");
	check(std::abs(rotatorAngle(0.f, 9.99f, 0.0, 0.0) - -0.001f) < 1e-5f, "just under 10 V is just under a whole turn");
	check(std::abs(rotatorAngle(0.25f, 5.f, 0.0, 0.0) - -0.25f) < 1e-6f, "knob and CV add and wrap");
	check(std::abs(rotatorAngle(0.f, 0.f, 0.75, 0.5) - 0.25f) < 1e-6f, "spin and notch add and wrap");
	RotatorRig rig;
	RotatorParams a, b;
	a.angle = rotatorAngle(0.f, 0.f, 0.0, 0.0);
	b.angle = rotatorAngle(0.f, 10.f, 0.0, 0.0);
	check(maxDiff(rig.run(a), rig.run(b)) == 0, "the picture at 0 V and at 10 V is byte-identical");
}


int main(int argc, char** argv) {
	if (argc > 1) {
		gProofDir = argv[1];
	}
	mkdir("build", 0755);
	mkdir(gProofDir.c_str(), 0755);

	testFreezeLatch();
	testSeriesSameType();
	testBusMixer();
	testCycle();
	testSeqlock();
	testNoFlashUnderWriterContention();
	testSlotAllocation();
	testSignalRingRetry();
	testSignalRingSlotReuse();
	testBusIdAllocation();
	testThruVoltage();
	testRampsMonotonic();
	testColorizerStops();
	testColorizerTop();
	testFrameRing();
	testInverseMap();
	testFeedbackAccumulates();
	testFeedbackTwoRenderers();
	testFeedbackControls();
	testVideoMixerState();
	testVideoMixerEnds();
	testVideoMixerDissolveAndAdd();
	testVideoMixerWipe();
	testVideoMixerColor();
	testVideoMixerKey();
	testVideoMixerGraph();
	testVideoMixerReference();
	testVideoMixerProofs();
	testRotatorIdentityAndTurns();
	testRotatorZoomXY();
	testRotatorEdges();
	testRotatorFolds();
	testRotatorNotchSpinReset();
	testRotatorSawSeam();
	testDisplayInputs();
	testSignalRingFrames();
	testSignalRingConcurrent();
	testExpHz();
	testDeflectionYokeRipples();
	testDeflectionYokeNoGaps();
	testDeflectionYokeEdges();
	testDeflectionYokeMath();
	testSignalRingFourthChannel();
	testDeflectionYokeSCoil();
	testDeflectionYokeSize();
	testDeflectionYokeCollapse();
	testDeflectionYokeClampRotation();
	testDeflectionYokeLines();
	testDeflectionYokeFieldRate();
	testDeflectionYokeExtremes();
	testBeamEnergyLaw();
	testBeamTiming();
	testBeamScanlineCount();
	testBeamTurnaroundBrightness();
	testBeamOverlapAdds();
	testBeamPhosphorDecay();
	testBeamZoomCollapse();
	testBeamCoil();
	testBeamGlow();
	testBeamProofs();
	testGridLineWidth();
	testScopeCaptureAndFallback();
	testScopePersistenceAndBloom();
	testScopeTriggerFinder();
	testScopeTrigStamps();
	testScopeTriggeredFramesLock();
	testScopeModesDiffer();
	testCvParamMaths();
	testScopeSweep();
	testScopeTriggerLevel();
	testScopeClearSerial();
	testBeamRoundSpot();
	testScopeDwell();
	testScopeBeamWidth();
	testScopeBeamBrightness();
	testScopeBeamPipeline();
	testLorenz();
	testScanProcessorFlatLines();
	testScanProcessorLift();
	testScanProcessorBunching();
	testScanProcessorMirror();
	testScanProcessorRotate();
	testScanProcessorBlank();
	testScanProcessorZ();
	testScanProcessorDeflectRipples();
	testScanProcessorProofs();
	testPairLockedBars();
	testPairLockedRows();
	testPairCheckerboard();
	testPairMixes();
	testPairFreeRunning();
	testPairFinePitch();
	testPairWave();
	testPairClockContinuity();
	testPairProofs();

	std::printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
