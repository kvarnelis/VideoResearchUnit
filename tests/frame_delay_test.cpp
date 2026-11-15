// Frame Delay (VRU-27): a loop in the patch is legal when it goes through a delay.
//
// The first half of this file pins what the renderer does today, before the Frame Delay exists: a loop
// that meets a bus while it is still being drawn, or a graph deeper than 16 levels, is a cycle, and the
// caller gets the last good picture. Those pins stay in force after the Frame Delay is added (a loop with
// no delay is still an error). Run with VRU_PRINT_GOLDEN=1 to print the hashes.
// Needs no Rack SDK.

#include "video_bus.hpp"
#include "video_graph_renderer.hpp"
#include "render_service.hpp"
#include "frame_ring.hpp"
#include "video_external_frame.hpp"

#include <algorithm>
#include <cmath>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Frame = std::vector<unsigned char>;
constexpr int kW = 160;
constexpr int kH = 90;

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
	for (unsigned char c : f) {
		h = (h ^ c) * 1099511628211ull;
	}
	return h;
}

bool printGolden() {
	static const bool on = std::getenv("VRU_PRINT_GOLDEN") != nullptr;
	return on;
}

void golden(const char* name, const Frame& f, uint64_t expected) {
	const uint64_t h = hashFrame(f);
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

void publishShapes(int bus) {
	ShapesParams s;
	s.sides = 5.f;
	s.size = 0.28f;
	s.x = -0.12f;
	s.y = 0.05f;
	videoBusPublishShapes(bus, s);
}

void publishRotator(int bus, int source) {
	RotatorParams r;
	r.angle = 0.04f;
	r.zoom = 1.04f;
	videoBusPublishRotator(bus, source, r);
}

void publishMixer(int bus, int a, int b) {
	BusMixerParams m;
	m.busB = b;
	m.mode = 1;
	m.mix = 0.8f;
	videoBusPublishBusMixer(bus, a, m);
}

void publishColor(int bus, int source) {
	ColorLabParams c;
	c.hue = 0.05f;
	videoBusPublishColorLab(bus, source, c);
}

// ---- Today: a two-node loop is a cycle and shows the last good picture ----
void testPinnedDirectLoop() {
	std::printf("pinned: a loop with no delay is a cycle\n");
	clearBuses();
	publishShapes(1);
	publishRotator(6, 1);       // not a loop yet: the Rotator reads the shape
	publishMixer(5, 1, 6);      // the mixer reads the shape and the Rotator
	VideoGraphRenderer r;
	const Frame good = r.renderBus(5, kW, kH);
	check(!good.empty() && !r.hasCycle(), "the acyclic graph renders");
	golden("acyclic mixer", good, 17676925611340756621ull);

	publishRotator(6, 5);       // now the Rotator reads the mixer: 5 -> 6 -> 5
	const Frame held = r.renderBus(5, kW, kH);
	check(r.hasCycle(), "the loop is reported as a cycle");
	check(!held.empty() && hashFrame(held) == hashFrame(good), "the cycle shows the last good picture, unchanged");
	check(r.hasSignal(), "the held picture counts as a signal");

	VideoGraphRenderer fresh;
	const Frame none = fresh.renderBus(5, kW, kH);
	check(fresh.hasCycle() && none.empty(), "a fresh renderer has no last good picture to show");

	// Entered from the other side of the loop it is the same error.
	VideoGraphRenderer other;
	check(other.renderBus(6, kW, kH).empty() && other.hasCycle(), "the loop is a cycle from the Rotator's side too");
	clearBuses();
}

// ---- Today: a chain 17 levels deep is too deep ----
void publishChain(int firstBus, int count, int sourceBus) {
	int source = sourceBus;
	for (int i = 0; i < count; ++i) {
		publishColor(firstBus + i, source);
		source = firstBus + i;
	}
}

void testPinnedDepth() {
	std::printf("pinned: a chain 17 levels deep is a cycle\n");
	clearBuses();
	publishShapes(10);
	publishChain(11, 16, 10);   // buses 11 to 26: the picture is 16 levels below the root
	VideoGraphRenderer r;
	const Frame good = r.renderBus(26, kW, kH);
	check(!good.empty() && !r.hasCycle(), "16 processors above the source render");
	golden("16 level chain", good, 96956861521169311ull);

	publishColor(9, 10);
	publishColor(11, 9);        // one more level: 17 processors above the source
	const Frame held = r.renderBus(26, kW, kH);
	check(r.hasCycle(), "17 levels is reported as a cycle");
	check(hashFrame(held) == hashFrame(good), "too deep shows the last good picture, unchanged");

	VideoGraphRenderer fresh;
	check(fresh.renderBus(26, kW, kH).empty() && fresh.hasCycle(), "too deep with no last good picture draws nothing");
	clearBuses();
}

// ---- The Frame Delay ----
// The rig's graph:  bus 1 Shapes; bus 4 Frame Delay of bus 6; bus 5 Bus Mixer (A bus 1, B bus 4); bus 6 Rotator of
// bus 5. The loop 5 -> 4 (delay) ... 6 -> 5 is cabled, and a Display would read bus 5. A tick is one count of the
// delay's frameCounter, which the module advances 60 times a second of audio time; the tests set it by hand.
constexpr int kShape = 1;
constexpr int kDelayBus = 4;
constexpr int kMixerBus = 5;
constexpr int kRotBus = 6;

void publishLoopMixer(int bus, int a, int b) {
	BusMixerParams m;
	m.busB = b;
	m.mode = 0;
	m.mix = 0.5f;
	videoBusPublishBusMixer(bus, a, m);
}

void publishLoopRotator(int bus, int source, float angle = 0.03f) {
	RotatorParams r;
	r.angle = angle;
	r.zoom = 1.06f;
	videoBusPublishRotator(bus, source, r);
}

FrameDelayParams delayParams(int slot, int counter, int frames, int clear = 0) {
	FrameDelayParams p;
	p.ringSlotId = slot;
	p.frameCounter = counter;
	p.clearCounter = clear;
	p.frames = frames;
	return p;
}

Frame blackFrame(int w = kW, int h = kH) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (size_t i = 3; i < f.size(); i += 4) f[i] = 255;
	return f;
}

double meanLuma(const Frame& f) {
	double sum = 0.0;
	for (size_t i = 0; i + 3 < f.size(); i += 4) sum += (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
	return f.empty() ? 0.0 : sum / static_cast<double>(f.size() / 4);
}

// The same loop with the delay replaced by a picture published from outside (an external frame), so the answer for
// tick n is worked out with no delay in the graph at all: source = rotate(mix(shape, previous source)).
struct Reference {
	int frameId = 0;
	Reference() {
		frameId = videoExternalFrameAllocate();
	}
	~Reference() {
		videoExternalFrameRelease(frameId);
		videoBusClear(10);
		videoBusClear(11);
		videoBusClear(12);
	}
	// The source the loop should hold at the end of a tick whose delay output was `delayed`.
	Frame next(const Frame& delayed) {
		videoExternalFramePublish(frameId, delayed.data(), kW, kH, 0.0);
		videoBusPublishExternalFrame(10, frameId);
		publishLoopMixer(11, kShape, 10);
		publishLoopRotator(12, 11);
		VideoGraphRenderer r;
		return r.renderBus(12, kW, kH);
	}
};

// (a) The loop renders with no cycle, evolves, and each tick's delay output is the last tick's source.
void testLoopThroughDelay() {
	std::printf("frame delay: a loop through a delay is not a cycle and evolves\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	publishShapes(kShape);
	publishLoopMixer(kMixerBus, kShape, kDelayBus);
	publishLoopRotator(kRotBus, kMixerBus);
	VideoGraphRenderer display;
	VideoGraphRenderer reader;
	Reference ref;
	Frame previousSource = blackFrame();
	Frame firstMixer;
	bool noCycle = true, delayMatches = true, sourceMatches = true, evolves = true;
	uint64_t lastMixerHash = 0;
	for (int n = 1; n <= 6; ++n) {
		videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, n, 1));
		const Frame shown = display.renderBus(kMixerBus, kW, kH);
		noCycle = noCycle && !display.hasCycle() && !shown.empty();
		const Frame delayOut = reader.renderBus(kDelayBus, kW, kH);
		delayMatches = delayMatches && hashFrame(delayOut) == hashFrame(previousSource);
		const Frame expectedSource = ref.next(previousSource);
		FrameRingFrame stored;
		sourceMatches = sourceMatches && frameRingRead(slot, 0, stored) && stored.frameCounter == n && hashFrame(stored.rgba) == hashFrame(expectedSource);
		evolves = evolves && hashFrame(shown) != lastMixerHash;
		lastMixerHash = hashFrame(shown);
		if (n == 1) firstMixer = shown;
		previousSource = expectedSource;
	}
	check(noCycle, "the loop renders on every tick with no cycle");
	check(delayMatches, "tick n's delay output is byte for byte tick n-1's source");
	check(sourceMatches, "the stored picture is the source worked out with no delay in the graph");
	check(evolves, "the picture changes every tick");
	check(meanLuma(display.renderBus(kMixerBus, kW, kH)) > meanLuma(firstMixer), "the trail builds up over the ticks");
	frameRingReleaseSlot(slot);
	clearBuses();
}

// (b) Take the delay out of the same loop and it is the pinned cycle again.
void testLoopWithoutDelayIsStillACycle() {
	std::printf("frame delay: the same loop with the delay taken out is still a cycle\n");
	clearBuses();
	publishShapes(kShape);
	publishLoopRotator(kRotBus, kShape);
	publishLoopMixer(kMixerBus, kShape, kRotBus);
	VideoGraphRenderer r;
	const Frame good = r.renderBus(kMixerBus, kW, kH);
	check(!good.empty() && !r.hasCycle(), "before the loop is closed it renders");
	publishLoopRotator(kRotBus, kMixerBus);  // the Rotator reads the mixer, which reads the Rotator
	const Frame held = r.renderBus(kMixerBus, kW, kH);
	check(r.hasCycle() && hashFrame(held) == hashFrame(good), "closed with no delay it reports a cycle and shows the last good picture");
	VideoGraphRenderer fresh;
	check(fresh.renderBus(kMixerBus, kW, kH).empty() && fresh.hasCycle(), "and with no last good picture it draws nothing");

	// A delay elsewhere in the patch does not excuse a zero-delay loop.
	const int slot = frameRingAllocateSlot();
	publishLoopMixer(8, kShape, 9);
	publishLoopRotator(9, 8);
	videoBusPublishFrameDelay(kDelayBus, kShape, delayParams(slot, 1, 1));
	VideoGraphRenderer other;
	check(other.renderBus(8, kW, kH).empty() && other.hasCycle(), "a delay on another bus changes nothing about a loop without one");
	frameRingReleaseSlot(slot);
	clearBuses();
}

// (c) FRAMES = k gives the source as it was k ticks ago; before that, black.
void testDelayLength() {
	std::printf("frame delay: FRAMES picks how many ticks old the picture is\n");
	clearBuses();
	const int lengths[] = {1, 2, 5, 12, kFrameDelayMaxFrames};
	for (int k : lengths) {
		const int slot = frameRingAllocateSlot();
		publishShapes(kShape);
		VideoGraphRenderer r;
		VideoGraphRenderer ref;
		std::vector<Frame> sourceAt(1 + 40);
		bool right = true, blackFirst = true;
		for (int n = 1; n <= 36; ++n) {
			publishLoopRotator(7, kShape, 0.004f * static_cast<float>(n));
			sourceAt[static_cast<size_t>(n)] = ref.renderBus(7, kW, kH);
			videoBusPublishFrameDelay(kDelayBus, 7, delayParams(slot, n, k));
			const Frame out = r.renderBus(kDelayBus, kW, kH);
			if (n <= k) blackFirst = blackFirst && hashFrame(out) == hashFrame(blackFrame());
			else right = right && hashFrame(out) == hashFrame(sourceAt[static_cast<size_t>(n - k)]);
		}
		char text[120];
		std::snprintf(text, sizeof(text), "FRAMES %d gives the source as it was %d ticks ago", k, k);
		check(right, text);
		std::snprintf(text, sizeof(text), "FRAMES %d is black until a picture that old exists", k);
		check(blackFirst, text);
		frameRingReleaseSlot(slot);
	}
	clearBuses();
}

// A tick the delay is read at twice, a counter that jumps (a slow screen) and one that goes round the wrap.
void testDelayCounters() {
	std::printf("frame delay: repeated, skipped and wrapped counts\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	publishShapes(kShape);
	VideoGraphRenderer r;
	VideoGraphRenderer ref;
	auto sourceFor = [&](int n) {
		publishLoopRotator(7, kShape, 0.004f * static_cast<float>(n));
		return ref.renderBus(7, kW, kH);
	};
	auto readAt = [&](int counter, int sourceTag, int frames) {
		publishLoopRotator(7, kShape, 0.004f * static_cast<float>(sourceTag));
		videoBusPublishFrameDelay(kDelayBus, 7, delayParams(slot, counter, frames));
		return r.renderBus(kDelayBus, kW, kH);
	};
	readAt(10, 10, 2);
	const Frame again = readAt(10, 99, 2);  // same tick, a source that has moved on: nothing more is stored
	FrameRingFrame head;
	check(frameRingCount(slot) == 1 && frameRingRead(slot, 0, head) && hashFrame(head.rgba) == hashFrame(sourceFor(10)),
		"a second read in the same tick stores nothing and keeps the first picture");
	check(hashFrame(again) == hashFrame(blackFrame()), "and the delay output is unchanged");
	readAt(11, 11, 2);
	// The screen skipped counts 12 and 13. At 14 the pictures are 11 (3 old) and 10 (4 old): the newest old enough is 11.
	const Frame skipped = readAt(14, 14, 2);
	check(hashFrame(skipped) == hashFrame(sourceFor(11)), "after skipped counts the newest picture that is old enough is used");
	// Round the wrap: 2^30 - 1, 2^30, 1, 2, 3 are consecutive ticks (the module wraps to 1, as Feedback does).
	frameRingClear(slot, 1);
	const int counts[] = {(1 << 30) - 1, 1 << 30, 1, 2, 3};
	Frame out;
	for (int i = 0; i < 5; ++i) {
		publishLoopRotator(7, kShape, 0.004f * static_cast<float>(100 + i));
		videoBusPublishFrameDelay(kDelayBus, 7, delayParams(slot, counts[i], 2, 1));
		out = r.renderBus(kDelayBus, kW, kH);
	}
	check(hashFrame(out) == hashFrame(sourceFor(102)), "the delay survives the counter wrapping round");
	frameRingReleaseSlot(slot);
	clearBuses();
}

uint64_t gTestTick = 0;
uint64_t testTickSource() {
	return gTestTick;
}

// (d) One store per tick, whoever reads and however many sizes there are.
void testOneStorePerTick() {
	std::printf("frame delay: one store per tick for every reader\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	publishShapes(kShape);
	publishLoopMixer(kMixerBus, kShape, kDelayBus);
	publishLoopRotator(kRotBus, kMixerBus);
	ColorLabParams c;
	videoBusPublishColorLab(8, kMixerBus, c);  // a second consumer of the same loop
	RenderService svc;
	svc.setTickSource(testTickSource);
	svc.setHiddenRendering(false);
	std::vector<unsigned char> api(static_cast<size_t>(kW) * kH * 4u);
	bool oneEach = true, headRight = true, firstSizeStored = true, sameBothSizes = true, noCycle = true;
	for (int n = 1; n <= 8; ++n) {
		gTestTick = static_cast<uint64_t>(n);
		videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, n, 30));
		const auto a = svc.frame(kMixerBus, kW, kH);   // two readers at one size
		const auto b = svc.frame(8, kW, kH);
		const auto c2 = svc.frame(kMixerBus, kW / 2, kH / 2);  // a reader at another size
		const auto d = svc.frame(kDelayBus, kW / 2, kH / 2);
		noCycle = noCycle && a.signal && !a.cycle && b.signal && !b.cycle && c2.signal && !c2.cycle && d.signal;
		const bool apiOk = svc.readCopy(kMixerBus, kW, kH, api.data(), static_cast<size_t>(kW) * 4u);  // an API read in the same tick
		noCycle = noCycle && apiOk;
		FrameRingFrame head;
		oneEach = oneEach && frameRingCount(slot) == n;
		headRight = headRight && frameRingRead(slot, 0, head) && head.frameCounter == n;
		firstSizeStored = firstSizeStored && head.width == kW && head.height == kH;
		// What the delay shows at the second size is the first size's picture, resampled.
		const auto full = svc.frame(kDelayBus, kW, kH);
		sameBothSizes = sameBothSizes && std::fabs(meanLuma(*full.pixels) - meanLuma(*d.pixels)) < 0.02;
	}
	check(noCycle, "every reader gets a picture and none reports a cycle");
	check(oneEach, "after n ticks the delay has stored exactly n pictures, with readers at two sizes and an API read");
	check(headRight, "the newest stored picture belongs to the tick just drawn");
	check(firstSizeStored, "the size stored is the size of the first renderer to draw that tick");
	check(sameBothSizes, "a reader at another size sees the same delayed picture, resampled");
	frameRingReleaseSlot(slot);
	clearBuses();
}

// Two renderers of the same size, drawn one after the other with no service.
void testTwoPrivateRenderers() {
	std::printf("frame delay: two private renderers store once\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	publishShapes(kShape);
	publishLoopMixer(kMixerBus, kShape, kDelayBus);
	publishLoopRotator(kRotBus, kMixerBus);
	VideoGraphRenderer a, b;
	bool same = true;
	for (int n = 1; n <= 5; ++n) {
		videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, n, 8));
		const Frame fa = a.renderBus(kMixerBus, kW, kH);
		const Frame fb = b.renderBus(kMixerBus, kW, kH);
		same = same && hashFrame(fa) == hashFrame(fb) && frameRingCount(slot) == n;
	}
	check(same, "two renderers see the same loop picture and the delay holds one picture per tick");
	frameRingReleaseSlot(slot);
	clearBuses();
}

// (e) A chain of two delays adds their lengths.
void testTwoDelaysInSeries() {
	std::printf("frame delay: two delays in a row add up\n");
	clearBuses();
	const int slotA = frameRingAllocateSlot();
	const int slotB = frameRingAllocateSlot();
	publishShapes(kShape);
	VideoGraphRenderer r;
	VideoGraphRenderer ref;
	std::vector<Frame> sourceAt(20);
	bool right = true;
	for (int n = 1; n <= 14; ++n) {
		publishLoopRotator(7, kShape, 0.004f * static_cast<float>(n));
		sourceAt[static_cast<size_t>(n)] = ref.renderBus(7, kW, kH);
		videoBusPublishFrameDelay(kDelayBus, 7, delayParams(slotA, n, 1));
		videoBusPublishFrameDelay(8, kDelayBus, delayParams(slotB, n, 2));
		const Frame out = r.renderBus(8, kW, kH);
		const Frame expected = n > 3 ? sourceAt[static_cast<size_t>(n - 3)] : blackFrame();
		right = right && hashFrame(out) == hashFrame(expected);
	}
	check(right, "a delay of 1 into a delay of 2 shows the source 3 ticks ago");
	frameRingReleaseSlot(slotA);
	frameRingReleaseSlot(slotB);
	clearBuses();
}

// (f) The root reaches only the delay; the source is drawn after it, from the pass.
void testDeferredRoot() {
	std::printf("frame delay: a source reached only through the delay is still drawn\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	publishShapes(kShape);
	publishLoopMixer(kMixerBus, kShape, kDelayBus);
	publishLoopRotator(kRotBus, kMixerBus);
	ColorLabParams c;
	videoBusPublishColorLab(9, kDelayBus, c);  // the only thing a Display would read: the delay, through a Color Lab
	VideoGraphRenderer r;
	Reference ref;
	Frame previousSource = blackFrame();
	bool advances = true, matches = true;
	uint64_t last = 0;
	for (int n = 1; n <= 6; ++n) {
		videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, n, 1));
		const Frame shown = r.renderBus(9, kW, kH);
		advances = advances && !shown.empty() && !r.hasCycle() && frameRingCount(slot) == std::min(n, 2);
		if (n > 1) advances = advances && hashFrame(shown) != last;
		last = hashFrame(shown);
		FrameRingFrame stored;
		const Frame expected = ref.next(previousSource);
		matches = matches && frameRingRead(slot, 0, stored) && hashFrame(stored.rgba) == hashFrame(expected);
		previousSource = expected;
	}
	check(advances, "the loop advances on every tick although the root never reaches the source");
	check(matches, "and what it stores is what the loop should hold");

	// The root drawn in a service pass with the delay's own source as the other root: drawn once.
	RenderService svc;
	svc.setTickSource(testTickSource);
	gTestTick = 1000;
	videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, 7, 1));
	svc.frame(9, kW, kH);
	svc.frame(kRotBus, kW, kH);
	check(svc.busRenderCount(kRotBus, kW, kH) == 1 && svc.busRenderCount(kMixerBus, kW, kH) == 1, "the source and the mixer each draw once in the pass");
	frameRingReleaseSlot(slot);
	clearBuses();
}

// (g) Depth: the delay costs no level, and the limit still stands.
void testDepth() {
	std::printf("frame delay: the depth limit with a loop through a delay\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	// Bus 1 shape; bus 20 mixer (A shape, B delay); buses 21.. a chain of Color Labs on the mixer; the Rotator on top
	// feeds the delay. Counted from the Rotator: the shape is `chain + 2` levels down.
	auto build = [&](int chain) {
		clearBuses();
		publishShapes(kShape);
		publishLoopMixer(20, kShape, kDelayBus);
		int source = 20;
		for (int i = 0; i < chain; ++i) {
			publishColor(21 + i, source);
			source = 21 + i;
		}
		publishLoopRotator(40, source);
		return 40;
	};
	int root = build(13);  // Rotator 0, 13 Color Labs, the mixer 14, the shape 15
	VideoGraphRenderer r;
	bool fine = true, advances = true;
	uint64_t last = 0;
	for (int n = 1; n <= 5; ++n) {
		videoBusPublishFrameDelay(kDelayBus, root, delayParams(slot, n, 1));
		const Frame shown = r.renderBus(root, kW, kH);
		fine = fine && !shown.empty() && !r.hasCycle();
		advances = advances && hashFrame(shown) != last;
		last = hashFrame(shown);
	}
	check(fine, "a loop through a delay inside a 15 level chain does not trip the depth limit");
	check(advances, "and it advances on every tick");
	root = build(15);  // the shape would be 17 levels down
	VideoGraphRenderer deep;
	videoBusPublishFrameDelay(kDelayBus, root, delayParams(slot, 9, 1));
	check(deep.renderBus(root, kW, kH).empty() && deep.hasCycle(), "17 levels is still too deep with a delay in the loop");
	frameRingReleaseSlot(slot);
	clearBuses();
}

// (h) A reset clears the stored pictures.
void testReset() {
	std::printf("frame delay: reset clears the stored pictures\n");
	clearBuses();
	const int slot = frameRingAllocateSlot();
	publishShapes(kShape);
	publishLoopMixer(kMixerBus, kShape, kDelayBus);
	publishLoopRotator(kRotBus, kMixerBus);
	VideoGraphRenderer r;
	for (int n = 1; n <= 5; ++n) {
		videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, n, 8));
		r.renderBus(kMixerBus, kW, kH);
	}
	check(frameRingCount(slot) == 5, "five pictures are stored before the reset");
	videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot, 6, 1, 1));  // the reset count changed
	VideoGraphRenderer reader;
	check(hashFrame(reader.renderBus(kDelayBus, kW, kH)) == hashFrame(blackFrame()), "after a reset the delay shows black");
	check(frameRingCount(slot) == 1, "and holds only the picture stored since");
	frameRingReleaseSlot(slot);

	// The loop starts again from black: the first picture after a reset is the first picture of a fresh loop.
	const int slot2 = frameRingAllocateSlot();
	videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot2, 1, 1));
	VideoGraphRenderer fresh;
	const Frame freshFirst = fresh.renderBus(kMixerBus, kW, kH);
	frameRingReleaseSlot(slot2);
	const int slot3 = frameRingAllocateSlot();
	for (int n = 1; n <= 3; ++n) {
		videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot3, 20 + n, 1));
		r.renderBus(kMixerBus, kW, kH);
	}
	videoBusPublishFrameDelay(kDelayBus, kRotBus, delayParams(slot3, 30, 1, 7));
	VideoGraphRenderer afterReset;
	check(hashFrame(afterReset.renderBus(kMixerBus, kW, kH)) == hashFrame(freshFirst), "after a reset the loop's first picture is the first picture of a fresh loop");
	frameRingReleaseSlot(slot3);
	clearBuses();
}

// The ring's age lookup on its own.
void testRingAge() {
	std::printf("frame ring: newest picture at least so old\n");
	const int slot = frameRingAllocateSlot();
	frameRingSetCapacity(slot, 6);
	for (int n = 1; n <= 4; ++n) {
		Frame f(4 * 4, static_cast<unsigned char>(n));
		frameRingPush(slot, n * 2, f, 2, 2);  // counters 2, 4, 6, 8
	}
	FrameRingFrame out;
	check(frameRingReadAtLeastAge(slot, 8, 1, out) && out.frameCounter == 6, "age 1 at counter 8 gives the picture of 6");
	check(frameRingReadAtLeastAge(slot, 8, 0, out) && out.frameCounter == 8, "age 0 gives the newest");
	check(frameRingReadAtLeastAge(slot, 9, 3, out) && out.frameCounter == 6, "age 3 at counter 9 gives the picture of 6");
	check(!frameRingReadAtLeastAge(slot, 8, 7, out), "nothing is old enough");
	check(!frameRingReadAtLeastAge(slot, 1, 1, out), "a picture from the future is not used");
	frameRingReleaseSlot(slot);
}

}  // namespace

int main() {
	testPinnedDirectLoop();
	testPinnedDepth();
	testRingAge();
	testLoopThroughDelay();
	testLoopWithoutDelayIsStillACycle();
	testDelayLength();
	testDelayCounters();
	testOneStorePerTick();
	testTwoPrivateRenderers();
	testTwoDelaysInSeries();
	testDeferredRoot();
	testDepth();
	testReset();
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
