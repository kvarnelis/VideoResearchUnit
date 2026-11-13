// Offline picture check for the six numbered demo patches in patches/.
//
// For each demo this rebuilds the video graph with the renderer the Display uses, and drives every
// LFO-modulated destination to the ends of its swing (and the middle), the way the patch's cables do:
// the knob value from the patch, plus the cable's volts (after the 8vert, VCA or depth that sits on it)
// run through the same volts-to-knob rule the module uses. The volts at each end are the ones that
// scripts/check-demo-ranges.py prints for the patch; keep the two in step when a patch changes.
//
// Every render must have a lit picture (not black) whose lit centre stays inside the middle 80% of the
// frame. The old Muybridge settings (full-swing LFOs straight into LUMA GAIN and INTENSITY) are run as a
// control and must FAIL the same test, so the test is known to catch the fault it was written for.
//
// Muybridge needs a clip, so a stand-in picture is used: a light grey ground with a dark horse-sized blob,
// like the clip's frames. The Deflection Yoke (DeflectionYoke) in the Feedback Tunnel demo has no modulated
// destination and is left out of that chain.
// No Rack needed. Build and run with `make test`.

#include "cv_param.hpp"
#include "frame_ring.hpp"
#include "rotator_math.hpp"
#include "shapes_math.hpp"
#include "signal_ring.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Frame = std::vector<unsigned char>;
constexpr int kW = 192;
constexpr int kH = 108;
constexpr double kPi = 3.14159265358979323846;

int failures = 0;
int checks = 0;

struct Stats {
	double meanLuma = 0.0;
	double litFraction = 0.0;
	double cx = 0.5;  // luma-weighted centre, 0..1 across and down
	double cy = 0.5;
	int edgeLit = 0;  // lit pixels in the outer 2-pixel ring (picture cut off by the frame)
};

Stats measure(const Frame& f) {
	Stats s;
	if (f.size() < static_cast<size_t>(kW) * kH * 4u) {
		s.cx = s.cy = -1.0;
		return s;
	}
	double sum = 0.0, sx = 0.0, sy = 0.0;
	int lit = 0;
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			const double l = (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
			sum += l;
			sx += l * (x + 0.5) / kW;
			sy += l * (y + 0.5) / kH;
			lit += l > 0.08 ? 1 : 0;
			if (l > 0.3 && (x < 2 || y < 2 || x >= kW - 2 || y >= kH - 2)) ++s.edgeLit;
		}
	}
	s.meanLuma = sum / (kW * kH);
	s.litFraction = static_cast<double>(lit) / (kW * kH);
	if (sum > 1e-9) {
		s.cx = sx / sum;
		s.cy = sy / sum;
	} else {
		s.cx = s.cy = -1.0;
	}
	return s;
}

// The pass criterion: lit (not black) and centred.
bool good(const Stats& s) {
	return s.litFraction >= 0.02 && s.cx >= 0.1 && s.cx <= 0.9 && s.cy >= 0.1 && s.cy <= 0.9;
}

void verdict(bool ok, const std::string& what, const Stats& s) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s (lit %.1f%%, mean %.3f, centre %.2f, %.2f)\n", what.c_str(), s.litFraction * 100.0, s.meanLuma, s.cx, s.cy);
	}
}

std::string label(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string label(const char* format, ...) {
	char buffer[256];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	return buffer;
}

// Five ways to push up to eight destinations at once: all low, all middle, all high, and two alternating
// mixes, so a destination is seen against both ends of its neighbours.
constexpr int kPhases = 5;
float phaseU(int phase, int dest) {
	switch (phase) {
		case 0: return -1.f;
		case 1: return 0.f;
		case 2: return 1.f;
		case 3: return (dest % 2 == 0) ? 1.f : -1.f;
		default: return (dest % 2 == 0) ? -1.f : 1.f;
	}
}
// Volts for u = -1, 0, +1 across a cable's swing lo..hi.
float volts(float u, float lo, float hi) {
	return lo + (u + 1.f) * 0.5f * (hi - lo);
}

struct Worst {
	double minLit = 1e9;
	int maxEdge = 0;
	double minCx = 1e9, maxCx = -1e9, minCy = 1e9, maxCy = -1e9;
	void add(const Stats& s) {
		minLit = std::min(minLit, s.litFraction);
		maxEdge = std::max(maxEdge, s.edgeLit);
		minCx = std::min(minCx, s.cx);
		maxCx = std::max(maxCx, s.cx);
		minCy = std::min(minCy, s.cy);
		maxCy = std::max(maxCy, s.cy);
	}
	void print(const char* name) const {
		std::printf("  %-28s lit >= %4.1f%%, centre x %.2f..%.2f, y %.2f..%.2f, edge pixels <= %d\n", name, minLit * 100.0, minCx, maxCx, minCy, maxCy, maxEdge);
	}
};

void publishOsc(int bus, float freq, float hue, float feedback, float shape) {
	VideoParams p;
	p.phase = 0.3f;
	p.frequency = freq;
	p.hue = hue;
	p.feedback = feedback;
	p.shape = shape;
	videoBusPublish(bus, p);
}

shapes_math::ShapesControls shapesControls(float sides, float round, float point, float size, float turn, float repeat, float copies, float ring, float melt, float spread, float matte) {
	shapes_math::ShapesControls c;
	c.sides = sides;
	c.round = round;
	c.point = point;
	c.size = size;
	c.turn = turn;
	c.repeat = repeat;
	c.copies = copies;
	c.ring = ring;
	c.melt = melt;
	c.spread = spread;
	c.matte = matte;
	return c;
}

// The Scan Processor's params the way ScanProcessor.cpp builds them: LUMA GAIN +-5 V = +-1, INTENSITY 0.3 per volt.
ScanProcessorParams scanProcessorParams(float lumaKnob, float lumaVolts, float hPosKnob, float hPosVolts, float vPosKnob, float vPosVolts,
	float vHeight, int lines, float intensityKnob, float intensityVolts, bool colour, float zInt, float persist, float glow) {
	ScanProcessorParams r;
	r.lumaGain = std::clamp(lumaKnob + lumaVolts / 5.f, -1.f, 1.f);
	r.hPos = std::clamp(hPosKnob + hPosVolts / 5.f, -1.f, 1.f);
	r.vPos = std::clamp(vPosKnob + vPosVolts / 5.f, -1.f, 1.f);
	r.vHeight = vHeight;
	r.lines = lines;
	r.intensity = std::clamp(intensityKnob + intensityVolts * 0.3f, 0.f, 3.f);
	r.color = colour;
	r.zIntensity = zInt;
	r.persist = persist;
	r.glow = glow;
	return r;
}

Frame renderSteady(VideoGraphRenderer& r, int bus, int passes) {
	Frame f;
	for (int i = 0; i < passes; ++i) {
		f = r.renderBus(bus, kW, kH);
	}
	return f;
}

// ---- 1. Scope Trace Lissajous: a 3:2 pair of sines, ROTATION turns through a whole circle, DWELL swells ----
void demoLissajous() {
	std::printf("demo 1: Scope Trace Lissajous\n");
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, 48000.f);
	for (int n = 0; n < 9600; ++n) {
		const double t = n / 48000.0;
		const float v[kSignalRingChannels] = {static_cast<float>(std::sin(2.0 * kPi * 130.81 * t)), static_cast<float>(std::sin(2.0 * kPi * 196.0 * t)), 1.f};
		signalRingPush(slot, v);
	}
	Worst w;
	// ROTATION 0..2 (one full turn) across a saw cycle; sample the circle, with DWELL at both ends of 0.3..0.8.
	const float turns[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f, 1.25f, 1.5f, 1.75f};
	for (float turnValue : turns) {
		for (float dwell : {0.3f, 0.8f}) {
			VideoGraphRenderer r;
			ScopeTraceParams p;
			p.scaleX = p.scaleY = 0.85f;
			p.timebase = 0.32f;
			p.thickness = 0.42f;
			p.decay = 0.72f;
			p.hue = 0.32f;
			p.intensity = 1.25f;
			p.dwell = dwell;
			p.rotation = rasterlab_cv::wrapTurn(turnValue);
			videoBusPublishScopeTrace(1, slot, p);
			const Stats s = measure(renderSteady(r, 1, 4));
			w.add(s);
			verdict(good(s), label("demo 1 rotation %.2f dwell %.1f", turnValue, dwell), s);
		}
	}
	w.print("Scope Trace");
	signalRingRelease(slot);
	videoBusClear(1);
}

// ---- 2. Video Oscillator Moire: two oscillators mixed in Difference mode, Colorizer ----
void demoMoire() {
	std::printf("demo 2: Video Oscillator Moire\n");
	Worst w;
	for (int phase = 0; phase < kPhases; ++phase) {
		VideoGraphRenderer r;
		// 0 FREQ A +-1.25 V, 1 FREQ B +-1.25 V, 2 Colorizer OFFSET +-5 V, 3 HUE A +-5 V.
		const float freqA = std::clamp(3.f + 0.4f * volts(phaseU(phase, 0), -1.25f, 1.25f), 0.05f, 24.f);
		const float freqB = std::clamp(5.f + 0.4f * volts(phaseU(phase, 1), -1.25f, 1.25f), 0.05f, 24.f);
		const float hueA = std::clamp(0.5f + 0.1f * volts(phaseU(phase, 3), -5.f, 5.f), 0.f, 1.f);
		publishOsc(1, freqA, hueA, 0.3f, 0.5f);
		publishOsc(2, freqB, 0.05f, 0.2f, 0.25f);
		BusMixerParams m;
		m.busB = 2;
		m.mix = 0.5f;
		m.mode = 4;  // Difference
		m.matteBus = 0;
		videoBusPublishBusMixer(3, 1, m);
		ColorizerParams c;
		const float offset = 0.1f * volts(phaseU(phase, 2), -5.f, 5.f);
		c.offset = offset - std::floor(offset);
		videoBusPublishColorizer(4, 3, c);
		const Stats s = measure(renderSteady(r, 4, 2));
		w.add(s);
		verdict(good(s), label("demo 2 phase %d", phase), s);
	}
	w.print("Moire");
	for (int b = 1; b <= 4; ++b) videoBusClear(b);
}

// ---- 3. Feedback Tunnel: a star fed back on itself. SIZE and POINT breathe, ZOOM and ROTATE sway ----
void demoTunnel() {
	std::printf("demo 3: Feedback Tunnel\n");
	Worst w;
	for (int phase = 0; phase < kPhases; ++phase) {
		VideoGraphRenderer r;
		// 0 SIZE 0..1.5 V, 1 POINT 0..2 V, 2 ROTATE +-0.3 V, 3 ZOOM +-1.5 V.
		shapes_math::ShapesControls c = shapesControls(5.f, 0.2f, 0.35f, 0.3f, 0.f, 1.f, 1.f, 0.4f, 0.f, 0.2f, 1.f);
		c.sizeCv = volts(phaseU(phase, 0), 0.f, 1.5f);
		c.pointCv = volts(phaseU(phase, 1), 0.f, 2.f);
		videoBusPublishShapes(1, shapes_math::shapesParams(c));
		FeedbackParams f;
		f.ringSlotId = frameRingAllocateSlot();
		frameRingSetCapacity(f.ringSlotId, 1);
		f.rotate = std::clamp(0.01f + 0.1f * volts(phaseU(phase, 2), -0.3f, 0.3f), -0.5f, 0.5f);
		f.zoom = std::exp2(std::clamp(-0.1f + 0.1f * volts(phaseU(phase, 3), -1.5f, 1.5f), -1.f, 1.f));
		f.hue = 0.025f;
		f.gain = 0.85f;
		Frame last;
		for (int n = 1; n <= 40; ++n) {
			f.frameCounter = n;
			videoBusPublishFeedback(2, 1, f);
			last = r.renderBus(2, kW, kH);
		}
		const Stats s = measure(last);
		w.add(s);
		verdict(good(s), label("demo 3 phase %d", phase), s);
		frameRingReleaseSlot(f.ringSlotId);
	}
	w.print("Feedback Tunnel");
	for (int b = 1; b <= 2; ++b) videoBusClear(b);
}

// ---- 4. Slow Scan Processor: oscillator + soft shape through the Scan Processor ----
void demoSlowScan() {
	std::printf("demo 4: Slow Scan Processor\n");
	Worst w;
	for (int phase = 0; phase < kPhases; ++phase) {
		VideoGraphRenderer r;
		// 0 FREQ +-2.5 V (8vert x0.5 of +-5 V), 1 HUE +-5 V, 2 LUMA +-0.75 V (x0.15), 3 SIZE +-1 V (x0.2),
		// 4 V POS +-0.4 V, 5 H POS +-0.3 V.
		const float freq = std::clamp(2.5f + 0.4f * volts(phaseU(phase, 0), -2.5f, 2.5f), 0.05f, 24.f);
		const float hue = std::clamp(0.5f + 0.1f * volts(phaseU(phase, 1), -5.f, 5.f), 0.f, 1.f);
		publishOsc(1, freq, hue, 0.35f, 0.5f);
		shapes_math::ShapesControls c = shapesControls(6.f, 0.6f, 0.f, 0.4f, 0.f, 1.f, 1.f, 0.4f, 0.f, 0.8f, 0.f);
		c.sizeCv = volts(phaseU(phase, 3), -1.f, 1.f);
		videoBusPublishShapes(2, shapes_math::shapesParams(c));
		BusMixerParams m;
		m.busB = 2;
		m.mix = 0.5f;
		m.mode = 0;
		m.matteBus = 0;
		videoBusPublishBusMixer(3, 1, m);
		const ScanProcessorParams scanProc = scanProcessorParams(0.3f, volts(phaseU(phase, 2), -0.75f, 0.75f), 0.f, volts(phaseU(phase, 5), -0.3f, 0.3f),
			0.1f, volts(phaseU(phase, 4), -0.4f, 0.4f), 0.8f, 256, 0.8f, 0.f, true, 0.5f, 0.5f, 0.4f);
		videoBusPublishScanProcessor(4, 3, scanProc);
		const Stats s = measure(renderSteady(r, 4, 6));
		w.add(s);
		verdict(good(s), label("demo 4 phase %d", phase), s);
	}
	w.print("Slow Scan Processor");
	for (int b = 1; b <= 4; ++b) videoBusClear(b);
}

// ---- 5. Octoscillator Garden: shapes, kaleidoscope, colour; every cable from one Octoscillator ----
void demoGarden() {
	std::printf("demo 5: Octoscillator Garden\n");
	Worst w;
	for (int phase = 0; phase < kPhases; ++phase) {
		VideoGraphRenderer r;
		// Volts are depth x LEVEL x 5 V: 1.5 V on most outputs, 0.75 V on oscillator 3 (SIZE, depth 0.25).
		// 0 SIDES, 1 MELT, 2 SIZE (+-0.75 V), 3 TURN, 4 ANGLE, 5 ZOOM, 6 colour OFFSET.
		shapes_math::ShapesControls c = shapesControls(4.f, 0.9f, 0.f, 0.3f, 0.f, 1.f, 6.f, 0.5f, 0.55f, 0.5f, 0.f);
		c.sidesCv = volts(phaseU(phase, 0), -1.5f, 1.5f);
		c.meltCv = volts(phaseU(phase, 1), -1.5f, 1.5f);
		c.sizeCv = volts(phaseU(phase, 2), -0.75f, 0.75f);
		c.turnCv = volts(phaseU(phase, 3), -1.5f, 1.5f);
		videoBusPublishShapes(1, shapes_math::shapesParams(c));
		RotatorParams ro;
		ro.angle = rotatorAngle(0.f, volts(phaseU(phase, 4), -1.5f, 1.5f), 0.0, 0.0);
		ro.zoom = std::exp2(3.f * std::clamp(0.12f + 0.1f * volts(phaseU(phase, 5), -1.5f, 1.5f), -1.f, 1.f));
		ro.edge = 3;
		ro.folds = 6;
		videoBusPublishRotator(2, 1, ro);
		ColorizerParams co;
		const float offset = 0.1f * volts(phaseU(phase, 6), -1.5f, 1.5f);
		co.offset = offset - std::floor(offset);
		videoBusPublishColorizer(3, 2, co);
		const Stats s = measure(renderSteady(r, 3, 2));
		w.add(s);
		verdict(good(s), label("demo 5 phase %d", phase), s);
	}
	w.print("Octoscillator Garden");
	for (int b = 1; b <= 3; ++b) videoBusClear(b);
}

// A stand-in for a Muybridge frame: light grey ground, dark horse-sized blob left of centre.
Frame standIn(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4u);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const double dx = (x + 0.5) / w - 0.42, dy = (y + 0.5) / h - 0.52;
			const bool horse = (dx * dx) / (0.26 * 0.26) + (dy * dy) / (0.2 * 0.2) < 1.0;
			const unsigned char v = horse ? 45 : 205;
			unsigned char* p = &f[(static_cast<size_t>(y) * w + x) * 4u];
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	return f;
}

Stats muybridgeRender(float lumaVolts, float intensityVolts, float lumaKnob, float vPosKnob, float intensityKnob) {
	VideoGraphRenderer r;
	const Frame pic = standIn(300, 200);
	const int id = videoExternalFrameAllocate();
	videoExternalFramePublish(id, pic.data(), 300, 200, 0.0);
	videoBusPublishExternalFrame(1, id);
	const ScanProcessorParams scanProc = scanProcessorParams(lumaKnob, lumaVolts, 0.f, 0.f, vPosKnob, 0.f, 1.f, 256, intensityKnob, intensityVolts, false, 0.6f, 0.4f, 0.25f);
	videoBusPublishScanProcessor(2, 1, scanProc);
	const Stats s = measure(renderSteady(r, 2, 6));
	videoBusClear(1);
	videoBusClear(2);
	videoExternalFrameRelease(id);
	return s;
}

// ---- 6. Muybridge on the Scan Processor ----
void demoMuybridge() {
	std::printf("demo 6: Muybridge on the Scan Processor\n");
	Worst w;
	// LUMA GAIN CV +-0.75 V (8vert x0.15 of +-5 V), INTENSITY CV +-1.35 V (x0.27).
	for (float lu : {-1.f, 0.f, 1.f}) {
		for (float iu : {-1.f, 0.f, 1.f}) {
			const Stats s = muybridgeRender(volts(lu, -0.75f, 0.75f), volts(iu, -1.35f, 1.35f), 0.3f, 0.1f, 1.2f);
			w.add(s);
			verdict(good(s), label("demo 6 luma %+.0f intensity %+.0f", lu, iu), s);
		}
	}
	w.print("Muybridge");
	// Control: the old patch, LFOs straight into the jacks at full swing (+-5 V, LUMA knob 0.5, no V POS).
	// It must fail on at least one of: lifted off the top at +5 V, dark at -5 V.
	std::printf("  control (old patch, must fail):\n");
	int failedControls = 0;
	for (float u : {-1.f, 0.f, 1.f}) {
		const Stats lift = muybridgeRender(u * 5.f, 0.f, 0.5f, 0.f, 1.2f);
		const Stats dark = muybridgeRender(0.f, u * 5.f, 0.5f, 0.f, 1.2f);
		std::printf("    LFO %+.0f V: luma-gain run lit %.1f%% centre y %.2f, intensity run lit %.1f%% mean %.3f\n", u * 5.f,
			lift.litFraction * 100.0, lift.cy, dark.litFraction * 100.0, dark.meanLuma);
		failedControls += good(lift) ? 0 : 1;
		failedControls += good(dark) ? 0 : 1;
	}
	++checks;
	if (failedControls == 0) {
		++failures;
		std::printf("  FAIL: the old Muybridge settings passed the picture test, so the test cannot see the fault\n");
	}
}

}  // namespace

int main() {
	demoLissajous();
	demoMoire();
	demoTunnel();
	demoSlowScan();
	demoGarden();
	demoMuybridge();
	std::printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
